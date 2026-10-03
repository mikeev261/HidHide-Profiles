// SPDX-License-Identifier: MIT
#include "stdafx.h"
#include "ProfilesCoordinator.h"
#include "ConfigurationChannel.h"
#include "Logging.h"
#include "Maintenance.h"
#include "ProfileProcessMatch.h"
#include "ProfileRecovery.h"
#include "StartupEntry.h"
#include "Utils.h"
#include "Volume.h"
#include <TlHelp32.h>

namespace
{
    constexpr auto RUN_KEY{ L"Software\\Microsoft\\Windows\\CurrentVersion\\Run" };
    constexpr auto RUN_VALUE{ L"HidHide Profiles" };
    constexpr auto LEGACY_RUN_VALUE{ L"HidHide App Profiles" };
    constexpr auto RUNTIME_KEY{ L"Software\\Nefarius Software Solutions e.U.\\HidHide\\AppProfileRuntime" };
    constexpr auto RECOVERY_V2{ L"TransactionV2" };

    std::wstring Lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); }); return value;
    }
    std::uint64_t ProcessLifetime(HANDLE process)
    {
        FILETIME created{}, exited{}, kernel{}, user{}; if (!::GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
        ULARGE_INTEGER value{}; value.LowPart = created.dwLowDateTime; value.HighPart = created.dwHighDateTime; return value.QuadPart;
    }
    struct FileIdentity
    {
        DWORD volume{}, high{}, low{};
        bool operator==(FileIdentity const& other) const
        { return volume == other.volume && high == other.high && low == other.low; }
    };
    std::optional<FileIdentity> IdentifyExecutable(std::filesystem::path const& path)
    {
        HANDLE file = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            auto error = ::GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return std::nullopt;
            throw std::system_error(error, std::system_category(), "Inspect executable identity");
        }
        BY_HANDLE_FILE_INFORMATION info{}; auto known = ::GetFileInformationByHandle(file, &info); auto error = ::GetLastError();
        ::CloseHandle(file);
        if (!known) throw std::system_error(error, std::system_category(), "Inspect executable identity");
        return FileIdentity{ info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow };
    }
    std::optional<FileIdentity> IdentifyDirectory(std::filesystem::path const& path)
    {
        HANDLE directory = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (directory == INVALID_HANDLE_VALUE) return std::nullopt;
        BY_HANDLE_FILE_INFORMATION info{}; auto known = ::GetFileInformationByHandle(directory, &info);
        ::CloseHandle(directory);
        if (!known || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return std::nullopt;
        return FileIdentity{ info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow };
    }
    struct SuspendedProcess { HANDLE process{}; HANDLE thread{}; DWORD processId{}; };
    class DirectLauncher final
    {
    public:
        static SuspendedProcess Create(std::filesystem::path const& executable)
        {
            auto command = L"\"" + executable.native() + L"\"";
            STARTUPINFOW startup{ sizeof(startup) }; PROCESS_INFORMATION process{};
            if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                nullptr, executable.parent_path().c_str(), &startup, &process))
                throw std::system_error(::GetLastError(), std::system_category(), "Create suspended application");
            return { process.hProcess, process.hThread, process.dwProcessId };
        }
        static std::filesystem::path Path(SuspendedProcess const& process)
        {
            std::vector<wchar_t> path(32768); DWORD size = static_cast<DWORD>(path.size());
            if (!::QueryFullProcessImageNameW(process.process, 0, path.data(), &size) || !size || size >= path.size())
                throw std::runtime_error("Could not verify the launched executable path");
            return std::wstring(path.data(), size);
        }
        static bool Resume(SuspendedProcess const& process)
        { return ::ResumeThread(process.thread) == 1; }
        // Return true only when the process has exited. A failed resume may
        // have started the thread, in which case its profile must remain held.
        static bool Abort(SuspendedProcess const& process) noexcept
        {
            if (::WaitForSingleObject(process.process, 0) == WAIT_OBJECT_0) return true;
            auto prior = ::SuspendThread(process.thread);
            if (prior == static_cast<DWORD>(-1)) return ::WaitForSingleObject(process.process, 0) == WAIT_OBJECT_0;
            if (prior == 0) { (void)::ResumeThread(process.thread); return false; }
            (void)::TerminateProcess(process.process, ERROR_CANCELLED);
            return ::WaitForSingleObject(process.process, 5000) == WAIT_OBJECT_0;
        }
    };
}

CProfilesCoordinator::CProfilesCoordinator(HidHide::Profiles::IEnforcement& enforcement, std::filesystem::path repositoryRoot,
    bool systemIntegration, ProcessSource processSource, StartupIntegration startupIntegration, MaintenanceSource maintenanceSource,
    std::optional<HidHide::Profiles::EnforcementResult> initialObservation)
    : m_Enforcement(enforcement), m_SystemIntegration(systemIntegration), m_ProcessSource(std::move(processSource)),
      m_StartupIntegration(std::move(startupIntegration)), m_MaintenanceSource(std::move(maintenanceSource)),
      m_Repository(std::move(repositoryRoot))
{
    if (!m_MaintenanceSource) m_MaintenanceSource = [] { return HidHide::Maintenance::Active(); };
    auto observed = initialObservation ? std::move(*initialObservation) : m_Enforcement.Observe();
    if (!observed.success || !observed.observedKnown) { m_Conflict = true; m_Status = observed.failure.empty() ? L"Driver state is unknown; activation is blocked" : observed.failure; }
    auto loaded = m_Repository.Inspect(); m_Snapshot = std::move(loaded.snapshot); m_Issues = std::move(loaded.issues);
    m_SettingsVersion = std::move(loaded.settingsVersion); m_ProfileVersions = std::move(loaded.profileVersions);
    bool catalogValid{ m_Issues.empty() }; try { if (!catalogValid) throw std::runtime_error("Profile repository contains invalid files"); HidHide::Profiles::Validate(m_Snapshot); }
    catch (...) { catalogValid = false; m_RepositoryInvalid = true; m_Status = L"Profile repository requires repair; activation is blocked"; }
    if (catalogValid && observed.observedKnown && observed.observed.allowedApplications != m_Snapshot.settings.allowedApplications)
    {
        catalogValid = false; m_Conflict = true; m_Status = L"Global Allowed apps changed outside HidHide Profiles; activation is blocked until reviewed";
    }
    if (catalogValid && m_SystemIntegration)
    {
        try { m_StartupIssue = UpdateAutoStart(m_Snapshot.settings.startWithWindows); }
        catch (std::exception const& error) { m_StartupIssue = L"Startup integration failed: " + std::wstring(error.what(), error.what() + strlen(error.what())); }
    }
    m_WatcherStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr); if (!m_WatcherStop) THROW_WIN32_LAST_ERROR;
    try
    {
        m_Worker = std::thread(&CProfilesCoordinator::WorkerMain, this);
        m_RepositoryWatcher = std::thread(&CProfilesCoordinator::RepositoryWatcherMain, this);
        if (catalogValid) SubmitSnapshot();
    }
    catch (...)
    {
        { std::lock_guard<std::mutex> lock(m_WorkerMutex); m_StopRequested = true; }
        m_WorkerWake.notify_one(); ::SetEvent(m_WatcherStop);
        if (m_Worker.joinable()) m_Worker.join(); if (m_RepositoryWatcher.joinable()) m_RepositoryWatcher.join();
        ::CloseHandle(m_WatcherStop); m_WatcherStop = nullptr; throw;
    }
}

CProfilesCoordinator::~CProfilesCoordinator() { Stop(); }

void CProfilesCoordinator::SubmitSnapshot()
{
    std::lock_guard<std::mutex> scanLock(m_ScanMutex);
    m_Activations.Update(m_Snapshot, {}, false);
    std::lock_guard<std::mutex> lock(m_WorkerMutex); m_PendingSnapshot = m_Snapshot; ++m_SubmittedRevision; m_WorkerWake.notify_one();
}

CProfilesCoordinator::ScanResult CProfilesCoordinator::Scan(HidHide::Profiles::Snapshot const& snapshot)
{
    std::lock_guard<std::mutex> scanLock(m_ScanMutex);
    ScanResult result;
    // A retired worker snapshot must neither rewrite history nor prune handles
    // belonging to the newly saved catalog.
    { std::lock_guard<std::mutex> lock(m_WorkerMutex);
      if (snapshot.generation != m_PendingSnapshot.generation) { result.complete = false; return result; } }
    m_ObservedProcesses.Retain(snapshot);
    std::map<std::wstring, bool> candidates;
    for (auto const& [id, profile] : snapshot.profiles) if (profile.kind == HidHide::Profiles::Kind::Application && profile.enabled)
        candidates[Lower(profile.executable.filename().native())] = true;
    auto finish = [&]()
    {
        // Owned launch handles provide lifetime identity even if discovery is
        // temporarily late. Successful launches still participate normally.
        for (auto handle : m_OwnedProcesses)
        {
            if (::WaitForSingleObject(handle, 0) != WAIT_TIMEOUT) continue;
            auto pid = ::GetProcessId(handle); auto lifetime = ProcessLifetime(handle);
            if (std::any_of(result.processes.begin(), result.processes.end(), [&](auto const& p) { return p.processId == pid && p.lifetimeIdentity == lifetime; })) continue;
            std::vector<wchar_t> path(32768); DWORD size = static_cast<DWORD>(path.size());
            if (!::QueryFullProcessImageNameW(handle, 0, path.data(), &size) || !lifetime) { result.complete = false; continue; }
            std::filesystem::path verified(std::wstring(path.data(), size));
            result.processes.push_back({ pid, lifetime, verified.filename().native(), verified, true });
            auto key = std::make_pair(pid, lifetime);
            if (m_ObservedProcesses.Relevant(result.processes.back()) && !m_ObservedProcesses.Contains(key.first, key.second))
            {
                HANDLE retained{};
                if (::DuplicateHandle(::GetCurrentProcess(), handle, ::GetCurrentProcess(), &retained, 0, FALSE, DUPLICATE_SAME_ACCESS))
                    m_ObservedProcesses.Remember(retained, result.processes.back());
                else result.complete = false;
            }
        }
        // Never let a stale worker catalog rewrite history after a save.
        std::lock_guard<std::mutex> lock(m_WorkerMutex);
        if (snapshot.generation != m_PendingSnapshot.generation) { result.complete = false; return result; }
        if (!m_ObservedProcesses.Collect(result.processes)) result.complete = false;
        for (auto const& process : result.processes)
            if (!process.pathAccessible || process.verifiedPath.empty() || !process.lifetimeIdentity) result.complete = false;
        auto eligible = snapshot;
        if (!m_ProcessSource) for (auto& [id, profile] : eligible.profiles)
            if (profile.kind == HidHide::Profiles::Kind::Application && profile.enabled
                && ::GetFileAttributesW(profile.executable.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                auto error = ::GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                { profile.enabled = false; result.missingProfiles.insert(id); }
                else result.complete = false;
            }
        m_ObservedProcesses.Retain(eligible);
        if (result.complete) m_ObservedProcesses.AppendExited(result.processes);
        m_Activations.Update(eligible, result.processes, result.complete);
        if (result.complete) m_ObservedProcesses.ConsumeExited();
        m_LastScanComplete = result.complete;
        result.selection = m_Activations.Select(snapshot);
        for (auto const& id : m_Activations.Running()) result.runningProfiles.insert(id);
        return result;
    };
    if (candidates.empty()) return finish();
    if (m_ProcessSource)
    {
        try { result.processes = m_ProcessSource(); }
        catch (...) { result.complete = false; return finish(); }
        return finish();
    }
    HANDLE processes = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (processes == INVALID_HANDLE_VALUE) { result.complete = false; return finish(); }
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (::Process32FirstW(processes, &entry)) do
    {
        if (!candidates.count(Lower(entry.szExeFile))) continue;
        HidHide::Profiles::ProcessObservation observation; observation.processId = entry.th32ProcessID; observation.fileName = entry.szExeFile;
        HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID);
        if (!process) observation.pathAccessible = false;
        else
        {
            observation.lifetimeIdentity = ProcessLifetime(process);
            if (auto cached = m_ProcessCache.Find(observation.processId, observation.lifetimeIdentity)) observation.verifiedPath = *cached;
            else
            {
                std::vector<wchar_t> path(32768); DWORD size = static_cast<DWORD>(path.size());
                if (::QueryFullProcessImageNameW(process, 0, path.data(), &size) && size && size < path.size()) { observation.verifiedPath = std::wstring(path.data(), size); m_ProcessCache.Remember(observation); }
                else observation.pathAccessible = false;
            }
            if (observation.lifetimeIdentity && observation.pathAccessible && !observation.verifiedPath.empty())
            {
                m_ObservedProcesses.Remember(process, observation); process = nullptr;
            }
            if (process) ::CloseHandle(process);
        }
        result.processes.emplace_back(std::move(observation));
    } while (::Process32NextW(processes, &entry));
    if (::GetLastError() != ERROR_NO_MORE_FILES) result.complete = false; ::CloseHandle(processes); m_ProcessCache.Retain(result.processes);
    return finish();
}

bool CProfilesCoordinator::CanReconcileScan(bool complete) const
{
    // Keep tracking history in every mode. Explicit Global and Pause do not
    // depend on application discovery; normal enforcement guards still apply.
    return complete || m_Snapshot.settings.paused || m_Snapshot.settings.mode == HidHide::Profiles::Mode::UseGlobal;
}

void CProfilesCoordinator::WorkerMain() noexcept
{
    try
    {
        std::unique_lock<std::mutex> lock(m_WorkerMutex); std::uint64_t revision{}; HidHide::Profiles::Snapshot snapshot;
        while (!m_StopRequested)
        {
            auto active = std::any_of(snapshot.profiles.begin(), snapshot.profiles.end(), [](auto const& item) { return item.second.kind == HidHide::Profiles::Kind::Application && item.second.enabled; });
            if (active || m_ReconcileRetryPending) m_WorkerWake.wait_for(lock, std::chrono::milliseconds(500), [&] { return m_StopRequested || revision != m_SubmittedRevision; });
            else m_WorkerWake.wait(lock, [&] { return m_StopRequested || revision != m_SubmittedRevision || m_ReconcileRetryPending; });
            if (m_StopRequested) break;
            if (revision != m_SubmittedRevision) { snapshot = m_PendingSnapshot; revision = m_SubmittedRevision; }
            lock.unlock();
            auto result = Scan(snapshot); result.revision = revision; lock.lock();
            if (m_StopRequested || revision != m_SubmittedRevision) continue;
            bool changed = revision != m_LastPublishedRevision || result.complete != m_LastPublishedComplete
                || !m_LastPublishedSelection || result.selection != *m_LastPublishedSelection
                || result.runningProfiles != m_LastPublishedRunning || result.missingProfiles != m_LastPublishedMissing;
            bool retryDue = m_ReconcileRetryPending && std::chrono::steady_clock::now() >= m_ReconcileRetryAt;
            if (!changed && !retryDue) continue;
            if (changed)
            {
                m_LastPublishedRevision = revision; m_LastPublishedComplete = result.complete; m_LastPublishedSelection = result.selection;
                m_LastPublishedRunning = result.runningProfiles; m_LastPublishedMissing = result.missingProfiles;
                m_Completed = std::move(result); ++m_CompletedSequence;
            }
            auto window = m_NotifyWindow; auto message = m_NotifyMessage; lock.unlock(); if (window && message) ::PostMessageW(window, message, 0, 0); lock.lock();
        }
    }
    catch (...) { std::lock_guard<std::mutex> lock(m_WorkerMutex); m_WorkerFailed = true; }
}

void CProfilesCoordinator::SetNotificationWindow(HWND window, UINT message)
{
    { std::lock_guard<std::mutex> lock(m_WorkerMutex); m_NotifyWindow = window; m_NotifyMessage = message; }
    // Registration may follow catalog creation or a completed worker scan.
    // Reload on the owner thread, even if the watcher consumed changes earlier.
    if (window && message) ::PostMessageW(window, message, 1, 0);
}

void CProfilesCoordinator::RepositoryWatcherMain() noexcept
{
    try
    {
        while (::WaitForSingleObject(m_WatcherStop, 0) == WAIT_TIMEOUT)
        {
            // Watch an existing ancestor, including directory names, so both a
            // not-yet-created repository and a replaced directory stay visible.
            // Reattach after each change to narrow an initially broader watch.
            auto ancestor = m_Repository.Root().parent_path(); HANDLE changed = INVALID_HANDLE_VALUE;
            std::optional<FileIdentity> identity;
            while (!ancestor.empty())
            {
                identity = IdentifyDirectory(ancestor);
                changed = ::FindFirstChangeNotificationW(ancestor.c_str(), TRUE,
                    FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
                if (changed != INVALID_HANDLE_VALUE) break;
                auto parent = ancestor.parent_path(); if (parent == ancestor) break; ancestor = std::move(parent);
            }
            if (changed == INVALID_HANDLE_VALUE)
            {
                if (::WaitForSingleObject(m_WatcherStop, 1000) != WAIT_TIMEOUT) break;
                continue;
            }
            // Inspect after arming: a creation/write in the reattachment gap is
            // included in this reload, and later writes signal the new handle.
            HWND window{}; UINT message{}; { std::lock_guard<std::mutex> lock(m_WorkerMutex); window = m_NotifyWindow; message = m_NotifyMessage; }
            if (window && message) ::PostMessageW(window, message, 1, 0);
            HANDLE waits[]{ m_WatcherStop, changed };
            DWORD wait{};
            // The notification handle follows a directory object, not its
            // pathname. Renaming/replacing that ancestor need not signal it.
            // Revalidate identity at a bounded cadence without reloading an
            // unchanged catalog or polling the driver during idle periods.
            do
            {
                wait = ::WaitForMultipleObjects(2, waits, FALSE, 1000);
                if (wait != WAIT_TIMEOUT) break;
                auto currentIdentity = IdentifyDirectory(ancestor);
                if (!identity || !currentIdentity || !(*currentIdentity == *identity)) break;
            } while (wait == WAIT_TIMEOUT);
            ::FindCloseChangeNotification(changed);
            if (wait == WAIT_OBJECT_0) break;
            if (wait != WAIT_OBJECT_0 + 1 && wait != WAIT_TIMEOUT && ::WaitForSingleObject(m_WatcherStop, 1000) != WAIT_TIMEOUT) break;
        }
    }
    catch (...) {} // A watcher failure must never escape the resident thread.
}

void CProfilesCoordinator::ScheduleReconcileRetry(bool pending)
{
    std::lock_guard<std::mutex> lock(m_WorkerMutex);
    m_ReconcileRetryPending = pending;
    if (pending) m_ReconcileRetryAt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    m_WorkerWake.notify_one();
}

bool CProfilesCoordinator::ReloadRepositoryIfChanged()
{
    try
    {
        auto loaded = m_Repository.Load(); if (!loaded.issues.empty()) throw std::runtime_error("Profile repository contains invalid or unsupported files");
        if (loaded.snapshot.generation == m_Snapshot.generation && !m_RepositoryInvalid) return false;
        HidHide::Profiles::Validate(loaded.snapshot); m_EffectiveVerified = false; m_RunningProfiles.clear(); m_MissingProfiles.clear();
        m_Snapshot = std::move(loaded.snapshot); m_Issues = std::move(loaded.issues);
        m_SettingsVersion = std::move(loaded.settingsVersion); m_ProfileVersions = std::move(loaded.profileVersions); m_RepositoryInvalid = false;
        m_Status = L"Repository changed outside this window; open drafts require conflict review"; SubmitSnapshot(); return true;
    }
    catch (std::exception const& error)
    {
        m_RepositoryInvalid = true; m_EffectiveVerified = false; m_Status = L"External repository change is invalid; activation is blocked";
        m_Issues.clear(); m_Issues.push_back({ m_Repository.Root(), std::wstring(error.what(), error.what() + strlen(error.what())) }); return true;
    }
}

HidHide::Profiles::DesiredEnforcement CProfilesCoordinator::Desired(HidHide::Profiles::Snapshot const& snapshot, HidHide::Profiles::Selection const& selection)
{
    HidHide::Profiles::DesiredEnforcement desired; desired.allowedApplications = snapshot.settings.allowedApplications; desired.hidingEnabled = !snapshot.settings.paused;
    auto found = snapshot.profiles.find(selection.profileId);
    if (found != snapshot.profiles.end() && !snapshot.settings.paused) desired.hiddenDevices = HidHide::Profiles::HiddenDevices(found->second);
    return desired;
}

CProfilesCoordinator::ApplyOutcome CProfilesCoordinator::Reconcile(HidHide::Profiles::Selection const& selection, bool saved, HidHide::Profiles::SavedVersion version)
{
    // Saving JSON is allowed while recovery/external-driver conflict is
    // unresolved, but no saved=true path may bypass the enforcement block.
    if (m_RepositoryInvalid) { ScheduleReconcileRetry(false); m_EffectiveVerified = false; return { saved, false, false, saved ? L"Saved, but repository validation remains unresolved; nothing was applied." : m_Status, version }; }
    if (m_Conflict) { ScheduleReconcileRetry(false); m_EffectiveVerified = false; return { saved, false, false, saved ? L"Saved. Driver recovery or external-state conflict remains unresolved; nothing was applied." : m_Status, version }; }
    if (m_AdoptedNeedsApply && !saved) { ScheduleReconcileRetry(false); return { false, false, true, L"Current driver settings were accepted. Press Apply after reviewing global Allowed apps.", version }; }
    try
    {
        if (m_MaintenanceSource()) { ScheduleReconcileRetry(true); return { saved, false, false, saved ? L"Saved. Setup maintenance is active; nothing was activated." : L"Setup maintenance is active", version }; }
    }
    catch (...)
    {
        m_EffectiveVerified = false;
        m_Status = L"Setup maintenance state could not be verified; profile activation is blocked";
        ScheduleReconcileRetry(true);
        return { saved, false, false, saved ? L"Saved. Setup maintenance state could not be verified; nothing was activated." : m_Status, version };
    }
    if (!selection.verified)
    {
        m_Status = L"Executable identity could not be verified; last verified device visibility was retained";
        ScheduleReconcileRetry(true);
        return { saved, false, true, saved ? L"Saved. Activation deferred because executable identity is unverified." : m_Status, version };
    }
    auto desired = Desired(m_Snapshot, selection); auto result = m_Enforcement.Reconcile(desired);
    if (result.success && result.observedKnown && result.observed == desired)
    {
        SetVerifiedSelection(selection);
        return { saved, true, true, saved ? L"Saved and applied." : m_Status, version };
    }
    m_Status = result.failure.empty() ? L"Saved settings could not be applied to the driver" : result.failure;
    m_EffectiveVerified = false;
    if (result.conflict) m_Conflict = true;
    ScheduleReconcileRetry(!m_Conflict);
    return { saved, false, result.observedKnown, saved ? (result.observedKnown ? L"Profile saved. Device visibility could not be updated." : L"Profile saved. Device visibility is unknown.") : m_Status, version };
}

void CProfilesCoordinator::SetVerifiedSelection(HidHide::Profiles::Selection const& selection)
{
    ScheduleReconcileRetry(false);
    m_Selection = selection; m_Conflict = false; m_AdoptedNeedsApply = false; m_EffectiveVerified = true;
    m_Status = m_Snapshot.settings.paused ? L"Hiding paused — selected Global retained; all devices are visible"
        : selection.reason == HidHide::Profiles::SelectionReason::ManualApplication ? L"Manual override mask applied and verified"
        : selection.reason == HidHide::Profiles::SelectionReason::Application ? L"Most recently activated running application mask applied and verified"
        : selection.reason == HidHide::Profiles::SelectionReason::ManualGlobal ? L"Manual Global policy applied and verified (Use Global)"
        : selection.reason == HidHide::Profiles::SelectionReason::DetectionUncertain ? L"Automatic Global fallback applied; a same-name executable path could not be verified"
        : L"Automatic Global fallback applied and verified";
}

CProfilesCoordinator::ApplyOutcome CProfilesCoordinator::PublishSaved(HidHide::Profiles::LoadResult loaded,
    HidHide::Profiles::SavedVersion version, std::wstring action, bool cleanupPending)
{
    ++m_RepositoryWrites;
    try
    {
        if (!loaded.issues.empty()) throw std::runtime_error("Saved repository still contains invalid files");
        HidHide::Profiles::Validate(loaded.snapshot);
        auto previousSnapshot = m_Snapshot; auto previousSelection = m_Selection; auto recoveringInvalidRepository = m_RepositoryInvalid;
        // A snapshot change invalidates the relationship between the prior
        // observed driver tuple and the newly requested policy. Only a fresh
        // Observe/Reconcile readback may mark the new snapshot effective.
        m_EffectiveVerified = false; m_RunningProfiles.clear(); m_MissingProfiles.clear();
        m_Snapshot = std::move(loaded.snapshot); m_Issues.clear(); m_RepositoryInvalid = false;
        m_SettingsVersion = std::move(loaded.settingsVersion); m_ProfileVersions = std::move(loaded.profileVersions); SubmitSnapshot();
        // Publication is the durable decision boundary. Once the reviewed
        // draft is saved, activation failures must remain retryable without
        // requiring a fabricated second JSON revision.
        m_AdoptedNeedsApply = false;
        if (m_SystemIntegration)
        {
            try { m_StartupIssue = UpdateAutoStart(m_Snapshot.settings.startWithWindows); }
            catch (std::exception const& error) { m_StartupIssue = L"Startup integration failed: " + std::wstring(error.what(), error.what() + strlen(error.what())); }
        }
        auto Finish = [&](ApplyOutcome outcome) { if (!m_StartupIssue.empty()) outcome.message += L" " + m_StartupIssue; return outcome; };
        auto scan = Scan(m_Snapshot); if (!CanReconcileScan(scan.complete)) return Finish({ true, false, false, L"Saved. Process detection is unavailable; device visibility was retained.", version });
        m_RunningProfiles = scan.runningProfiles; m_MissingProfiles = scan.missingProfiles;
        auto previousDesired = Desired(previousSnapshot, previousSelection);
        auto nextDesired = Desired(m_Snapshot, scan.selection);
        if (!recoveringInvalidRepository && previousDesired == nextDesired)
        {
            auto observation = m_Enforcement.Observe();
            if (observation.success && observation.observedKnown && observation.observed == nextDesired)
            {
                SetVerifiedSelection(scan.selection);
                return Finish({ true, false, true, action + (cleanupPending ? L" saved. Transaction cleanup is pending and will finish safely on restart. Effective device policy was already verified and unchanged." : L" saved. Effective device policy was already verified and unchanged."), version });
            }
            if (observation.conflict) m_Conflict = true;
            m_EffectiveVerified = false;
            m_Status = observation.failure.empty() ? L"Driver observation does not match the saved effective policy" : observation.failure;
            return Finish({ true, false, observation.observedKnown, action + (observation.observedKnown ? L" saved. Effective device policy was not changed because the observed driver state conflicts." : L" saved. Effective device state is unknown; nothing was applied."), version });
        }
        auto outcome = Reconcile(scan.selection, true, version); if (outcome.saved && outcome.applied) outcome.message = action + (cleanupPending ? L" saved and applied. Transaction cleanup is pending and will finish safely on restart." : L" saved and applied."); return Finish(std::move(outcome));
    }
    catch (...) { return { true, false, false, action + L" saved. Follow-up activation could not be completed; device visibility is unknown.", version }; }
}

CProfilesCoordinator::ApplyOutcome CProfilesCoordinator::RetryActivation()
{
    if (m_LaunchUncertain && HasLaunchedProcess()) return { false, false, m_EffectiveVerified, L"A directly launched application holds its verified profile until that process exits.", {} };
    auto scan = Scan(m_Snapshot); if (!CanReconcileScan(scan.complete)) return { false, false, false, L"Retry could not verify running processes; last verified visibility was retained.", {} };
    m_RunningProfiles = scan.runningProfiles; m_MissingProfiles = scan.missingProfiles;
    return Reconcile(scan.selection, false, {});
}

bool CProfilesCoordinator::HasLaunchedProcess() const
{
    if (m_LaunchedProcess && ::WaitForSingleObject(m_LaunchedProcess, 0) != WAIT_OBJECT_0) return true;
    return std::any_of(m_OwnedProcesses.begin(), m_OwnedProcesses.end(), [](HANDLE process) { return ::WaitForSingleObject(process, 0) != WAIT_OBJECT_0; });
}
std::vector<std::wstring> CProfilesCoordinator::RunningOrder() const
{ std::lock_guard<std::mutex> lock(m_ScanMutex); return m_Activations.Running(); }
std::wstring CProfilesCoordinator::ManualMaskId() const
{ std::lock_guard<std::mutex> lock(m_ScanMutex); return m_Activations.Pin(); }
HidHide::Profiles::Selection CProfilesCoordinator::RequestedSelection() const
{ std::lock_guard<std::mutex> lock(m_ScanMutex); return m_Activations.Select(m_Snapshot); }
CProfilesCoordinator::ApplyOutcome CProfilesCoordinator::SelectMask(std::wstring const& id)
{
    if (m_RepositoryInvalid || m_Conflict || m_AdoptedNeedsApply || m_LaunchUncertain)
        throw std::runtime_error("Resolve repository, driver, or launch recovery before choosing a mask");
    if (m_Snapshot.settings.paused || m_Snapshot.settings.mode != HidHide::Profiles::Mode::Automatic)
        throw std::runtime_error("Apply Automatic mode with hiding enabled before choosing an application mask");
    auto scan = Scan(m_Snapshot);
    if (!scan.complete) throw std::runtime_error("Running applications could not be verified; mask selection was retained");
    { std::lock_guard<std::mutex> lock(m_ScanMutex); m_Activations.SetPin(id); scan.selection = m_Activations.Select(m_Snapshot); }
    m_RunningProfiles = scan.runningProfiles;
    auto result = Reconcile(scan.selection, false);
    SubmitSnapshot();
    return result;
}

void CProfilesCoordinator::ValidateLaunch(std::wstring const& id, DWORD ownPid)
{
    if (m_LaunchUncertain && HasLaunchedProcess()) throw std::runtime_error("Resolve the uncertain application launch before launching another profile");
    if (m_RepositoryInvalid || m_Conflict || m_AdoptedNeedsApply || m_Snapshot.settings.paused || !m_Issues.empty())
        throw std::runtime_error("Resolve profile, driver, or paused-hiding state before launching an application");
    try { if (m_MaintenanceSource()) throw std::runtime_error("Setup maintenance is active; application launch is blocked"); }
    catch (std::runtime_error const&) { throw; }
    catch (...) { throw std::runtime_error("Setup maintenance state is unknown; application launch is blocked"); }
    { std::lock_guard<std::mutex> lock(m_WorkerMutex); if (m_WorkerFailed || m_StopRequested) throw std::runtime_error("Profile monitoring is unavailable; application launch is blocked"); }
    auto found = m_Snapshot.profiles.find(id);
    if (found == m_Snapshot.profiles.end() || found->second.kind != HidHide::Profiles::Kind::Application || !found->second.enabled)
        throw std::invalid_argument("Choose a saved, enabled application profile to launch");
    auto executable = HidHide::Profiles::NormalizeExecutable(found->second.executable);
    auto targetIdentity = IdentifyExecutable(executable);
    if (!targetIdentity) throw std::runtime_error("The saved application executable is missing");
    for (auto const& allowed : m_Snapshot.settings.allowedApplications)
    {
        auto allowedIdentity = IdentifyExecutable(allowed);
        if (_wcsicmp(allowed.lexically_normal().c_str(), executable.c_str()) == 0
            || (allowedIdentity && *allowedIdentity == *targetIdentity))
            throw std::runtime_error("This application is in Allowed apps and can read hidden devices; remove that exception before launch");
    }

    // Revalidate the target around suspended creation: an existing process
    // may retain an old physical-device handle.
    auto scanSnapshot = m_Snapshot; scanSnapshot.settings.mode = HidHide::Profiles::Mode::Automatic;
    scanSnapshot.settings.paused = false;
    auto scan = Scan(scanSnapshot);
    if (!scan.complete) throw std::runtime_error("Process discovery is unavailable; application launch is blocked");
    for (auto const& process : scan.processes)
    {
        if (process.exitedAt || process.processId == ownPid || _wcsicmp(process.fileName.c_str(), executable.filename().c_str()) != 0) continue;
        if (!process.pathAccessible || process.verifiedPath.empty())
            throw std::runtime_error("A same-name process cannot be identified; close it before launching this profile");
        if (_wcsicmp(HidHide::Profiles::NormalizeExecutable(process.verifiedPath).c_str(), executable.c_str()) == 0)
            throw std::runtime_error("This application is already running and may hold physical devices; close it before launching through HidHide Profiles");
    }
}

std::wstring CProfilesCoordinator::LaunchSavedProfile(std::wstring const& id, std::function<void(bool)> const& changeMode)
{
    ValidateLaunch(id);
    if (m_LaunchedProcess) { ::CloseHandle(m_LaunchedProcess); m_LaunchedProcess = nullptr; m_LaunchedProfileId.clear(); m_LaunchUncertain = false; }
    auto executable = HidHide::Profiles::NormalizeExecutable(m_Snapshot.profiles.at(id).executable);
    std::wstring launchId = id;
    auto process = DirectLauncher::Create(executable);
    auto close = [&] { if (process.thread) ::CloseHandle(process.thread); if (process.process) ::CloseHandle(process.process); };
    try
    {
        if (!process.process || !process.thread || !process.processId
            || _wcsicmp(HidHide::Profiles::NormalizeExecutable(DirectLauncher::Path(process)).c_str(), executable.c_str()) != 0)
            throw std::runtime_error("The suspended process did not match the saved executable path");
        ValidateLaunch(id, process.processId);
        changeMode(true);
        // Saving can publish a new snapshot or expose an enforcement failure.
        // Recheck every launch gate before applying the suspended child's mask.
        ValidateLaunch(id, process.processId);
        if (m_Snapshot.settings.mode != HidHide::Profiles::Mode::Automatic)
            throw std::runtime_error("Automatic mode could not be saved before launch");
        HidHide::Profiles::Selection selected{ id, HidHide::Profiles::SelectionReason::Application, executable.filename().native(), true };
        auto applied = Reconcile(selected, false);
        if (!applied.applied || !m_EffectiveVerified)
            throw std::runtime_error("The application profile could not be applied and verified before launch");
        auto observed = m_Enforcement.Observe();
        if (!observed.success || !observed.observedKnown || observed.conflict || !(observed.observed == Desired(m_Snapshot, selected)))
        {
            m_EffectiveVerified = false; if (observed.conflict) m_Conflict = true;
            throw std::runtime_error("Driver readback changed before launch; the suspended application was stopped");
        }
        std::wstring successStatus = L"Application mask verified before launch; Automatic selection is active";
        std::wstring successMessage = L"Application launched after its complete profile was verified. Automatic selection is active; a newer application may replace this mask.";
        { std::lock_guard<std::mutex> lock(m_ScanMutex); m_OwnedProcesses.reserve(m_OwnedProcesses.size() + 1); }
        if (!DirectLauncher::Resume(process)) throw std::runtime_error("The verified application could not be resumed");
        { std::lock_guard<std::mutex> lock(m_ScanMutex);
          m_OwnedProcesses.push_back(process.process); process.process = nullptr; m_Activations.SetPin({}); }
        m_LaunchUncertain = false;
        m_Status.swap(successStatus);
        close();
        try { SubmitSnapshot(); (void)RetryActivation(); } catch (...) { m_EffectiveVerified = false; }
        return successMessage;
    }
    catch (...)
    {
        auto failure = std::current_exception();
        if (process.process && !DirectLauncher::Abort(process))
        {
            m_LaunchedProcess = process.process; process.process = nullptr;
            m_LaunchedProfileId.swap(launchId);
            m_LaunchUncertain = true;
            m_EffectiveVerified = false;
            m_Status = L"Application start is uncertain; last policy is held until that process exits";
            close();
            throw std::runtime_error("Application start is uncertain; its profile remains held until the process exits");
        }
        close(); m_LaunchedProfileId.clear(); m_LaunchUncertain = false;
        try { changeMode(false); }
        catch (...)
        {
            // Do not silently reconcile a stale snapshot if the compensating
            // CAS save failed. Preserve diagnostics and require explicit retry.
            (void)ReloadRepositoryIfChanged(); m_EffectiveVerified = false;
            m_Status = L"Application was stopped, but its previous mode could not be restored; review saved settings";
            try { std::rethrow_exception(failure); }
            catch (std::exception const& launchFailure)
            {
                throw std::runtime_error(std::string(launchFailure.what())
                    + ". Application was stopped, but its previous mode could not be restored; review saved settings");
            }
        }
        // The prelaunch policy may have reached the driver. Restore the current
        // automatic/Global selection if it can be verified, or retain evidence.
        try { (void)RetryActivation(); } catch (...) { m_EffectiveVerified = false; }
        std::rethrow_exception(failure);
    }
}

HidHide::Profiles::EnforcementResult CProfilesCoordinator::ObserveEnforcement()
{
    auto result = m_Enforcement.Observe();
    auto verified = result.success && result.observedKnown && !m_RepositoryInvalid && !m_LaunchUncertain
        && result.observed == Desired(m_Snapshot, m_Selection);
    m_EffectiveVerified = verified;
    if (result.conflict) { m_Conflict = true; m_Status = result.failure; }
    else if (!verified && !result.failure.empty()) m_Status = result.failure;
    return result;
}

void CProfilesCoordinator::Tick()
{
    try
    {
        if (m_MaintenanceSource()) { ScheduleReconcileRetry(true); m_EffectiveVerified = false; m_Status = L"Setup maintenance is active; profile monitoring is suspended"; return; }
    }
    catch (...)
    {
        ScheduleReconcileRetry(true); m_EffectiveVerified = false; m_Status = L"Setup maintenance state could not be verified; profile activation is blocked"; return;
    }
    if (m_LaunchedProcess)
    {
        auto wait = ::WaitForSingleObject(m_LaunchedProcess, 0);
        if (wait == WAIT_TIMEOUT) return;
        if (wait == WAIT_FAILED) { m_EffectiveVerified = false; m_Status = L"Launched process state is unknown; last policy was retained"; return; }
        ::CloseHandle(m_LaunchedProcess); m_LaunchedProcess = nullptr; m_LaunchedProfileId.clear(); m_LaunchUncertain = false;
        m_EffectiveVerified = false;
        { std::lock_guard<std::mutex> lock(m_WorkerMutex); m_AppliedSequence = m_CompletedSequence; }
        (void)RetryActivation(); return;
    }
    { std::lock_guard<std::mutex> lock(m_ScanMutex);
      for (auto it = m_OwnedProcesses.begin(); it != m_OwnedProcesses.end();)
        if (::WaitForSingleObject(*it, 0) == WAIT_OBJECT_0) { ::CloseHandle(*it); it = m_OwnedProcesses.erase(it); } else ++it; }
    ScanResult result; std::uint64_t sequence{};
    { std::lock_guard<std::mutex> lock(m_WorkerMutex); if (m_WorkerFailed) { m_Status = L"Process monitoring stopped"; return; }
      if (m_ReconcileRetryPending && std::chrono::steady_clock::now() < m_ReconcileRetryAt) return;
      if (m_AppliedSequence == m_CompletedSequence && !m_ReconcileRetryPending) return;
      result = m_Completed; sequence = m_CompletedSequence; if (result.revision != m_SubmittedRevision || !sequence) return; }
    // Every sample advances history, including samples superseded before Tick.
    { std::lock_guard<std::mutex> lock(m_ScanMutex);
      result.selection = m_Activations.Select(m_Snapshot); result.complete = m_LastScanComplete;
      result.runningProfiles.clear(); for (auto const& id : m_Activations.Running()) result.runningProfiles.insert(id); }
    m_RunningProfiles = result.runningProfiles; m_MissingProfiles = result.missingProfiles;
    if (!CanReconcileScan(result.complete)) { ScheduleReconcileRetry(true); m_EffectiveVerified = false; m_Status = L"Process detection is unavailable; last verified device visibility was retained"; }
    else Reconcile(result.selection, false);
    m_AppliedSequence = sequence;
}

bool CProfilesCoordinator::AcceptanceScanNow()
{
    if (m_LaunchUncertain && HasLaunchedProcess()) return ObserveEnforcement().success && m_EffectiveVerified;
    auto scan = Scan(m_Snapshot); if (!CanReconcileScan(scan.complete)) return false;
    m_RunningProfiles = scan.runningProfiles; m_MissingProfiles = scan.missingProfiles;
    auto outcome = Reconcile(scan.selection, false); return outcome.applied && m_RepositoryWrites == 0;
}

void CProfilesCoordinator::ExitSafely()
{
    auto result = m_Enforcement.RestoreBaseline(); if (!result.success || !result.observedKnown) throw std::runtime_error("Baseline restoration was not confirmed");
}

std::set<std::filesystem::path> CProfilesCoordinator::AdoptExternalState()
{
    if (!m_Conflict) throw std::logic_error("There is no driver conflict to adopt; repository diagnostics require JSON repair or backup restore");
    if (m_RepositoryInvalid) throw std::logic_error("Driver settings cannot be adopted while the JSON repository is invalid; repair or restore the repository first");
    try
    {
        auto current = m_Repository.Load();
        if (!current.issues.empty()) throw std::runtime_error("Profile repository contains invalid files");
        HidHide::Profiles::Validate(current.snapshot);
        if (current.snapshot.generation != m_Snapshot.generation || !(current.snapshot.settings == m_Snapshot.settings)
            || current.snapshot.profiles != m_Snapshot.profiles)
            throw std::runtime_error("Profile repository changed while driver adoption was being confirmed");
    }
    catch (std::exception const& error)
    {
        m_RepositoryInvalid = true; m_EffectiveVerified = false;
        m_Status = L"Repository changed while driver adoption was being confirmed; recovery evidence was preserved";
        m_Issues.clear(); m_Issues.push_back({ m_Repository.Root(), std::wstring(error.what(), error.what() + strlen(error.what())) });
        throw std::logic_error("The JSON repository changed while confirmation was open; current driver settings were not adopted");
    }
    auto result = m_Enforcement.AdoptCurrentAsBaseline(); if (!result.success || !result.observedKnown) throw std::runtime_error("Current driver settings could not be adopted");
    m_Conflict = false; m_Status = L"Current driver settings accepted; review the staged global Allowed apps and press Apply"; m_AdoptedNeedsApply = true;
    return result.observed.allowedApplications;
}

CProfilesCoordinator::ApplyOutcome CProfilesCoordinator::CompleteAdoptionWithoutSettingsChange(
    std::set<std::filesystem::path> const& observedAllowedApplications)
{
    if (!m_AdoptedNeedsApply) throw std::logic_error("No adopted driver state is awaiting resolution");
    if (observedAllowedApplications != m_Snapshot.settings.allowedApplications)
        throw std::logic_error("Changed Allowed apps must be reviewed and saved with Apply");
    // No catalog mutation is needed. Release the adoption gate and reconcile
    // the already-saved policy against the newly accepted baseline now.
    m_AdoptedNeedsApply = false;
    return RetryActivation();
}

CProfilesCoordinator::ApplyOutcome CProfilesCoordinator::AbandonAdoptionDraft()
{
    if (!m_AdoptedNeedsApply) return { false, false, m_EffectiveVerified, L"No adopted driver settings were awaiting a saved decision.", {} };
    m_AdoptedNeedsApply = false;
    auto outcome = RetryActivation();
    if (outcome.applied) outcome.message = L"Adopted Allowed-app changes were discarded; the saved profile policy was reapplied without changing JSON.";
    else outcome.message = L"Adopted Allowed-app changes were discarded. The saved profile policy still needs activation. " + outcome.message;
    return outcome;
}

void CProfilesCoordinator::Stop() noexcept
{
    { std::lock_guard<std::mutex> lock(m_WorkerMutex); if (m_StopRequested) return; m_StopRequested = true; } m_WorkerWake.notify_one(); if (m_WatcherStop) ::SetEvent(m_WatcherStop); if (m_Worker.joinable()) m_Worker.join(); if (m_RepositoryWatcher.joinable()) m_RepositoryWatcher.join(); if (m_WatcherStop) { ::CloseHandle(m_WatcherStop); m_WatcherStop = nullptr; }
    try { ExitSafely(); } catch (...) {}
    for (auto process : m_OwnedProcesses) ::CloseHandle(process); m_OwnedProcesses.clear();
    m_ObservedProcesses.Clear();
    if (m_LaunchedProcess) { ::CloseHandle(m_LaunchedProcess); m_LaunchedProcess = nullptr; m_LaunchedProfileId.clear(); m_LaunchUncertain = false; }
}

std::wstring CProfilesCoordinator::ConfigureAutoStart(bool enabled) const
{
    HKEY key{}; auto status = ::RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr); if (status != ERROR_SUCCESS) THROW_WIN32(status);
    std::vector<wchar_t> module(32768); auto length = ::GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size())); if (!length || length >= module.size()) { auto error = ::GetLastError(); ::RegCloseKey(key); THROW_WIN32(error); }
    std::wstring command = L"\"" + std::wstring(module.data(), length) + L"\" --background";
    auto legacyPath = std::filesystem::path(module.data(), module.data() + length).parent_path().parent_path() / L"HidHide App Profiles" / L"HidHideClient.exe";
    std::wstring legacyCommand = L"\"" + legacyPath.native() + L"\" --background";
    auto ReadValue = [key](wchar_t const* name) -> std::optional<std::wstring>
    {
        DWORD type{}, size{}; auto error = ::RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
        if (error == ERROR_FILE_NOT_FOUND) return std::nullopt;
        if (error != ERROR_SUCCESS || type != REG_SZ || size < sizeof(wchar_t) || size % sizeof(wchar_t) || size > 32768 * sizeof(wchar_t)) return std::wstring{};
        std::vector<wchar_t> value(size / sizeof(wchar_t) + 1); error = ::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &size);
        auto characters = size / sizeof(wchar_t);
        if (error != ERROR_SUCCESS || !characters || value[characters - 1] != L'\0'
            || std::find(value.begin(), value.begin() + characters - 1, L'\0') != value.begin() + characters - 1) return std::wstring{};
        return std::wstring(value.data(), characters - 1);
    };
    auto plan = HidHide::PlanStartupEntry(enabled, ReadValue(RUN_VALUE), ReadValue(LEGACY_RUN_VALUE), command, legacyCommand);
    if (enabled && !plan.setCurrent && ReadValue(RUN_VALUE) != command)
    {
        ::RegCloseKey(key); return L"Startup preference was saved, but a foreign HidHide Profiles startup entry was preserved and requires manual review.";
    }
    auto DeleteOwned = [key, &status](wchar_t const* name)
    {
        if (status != ERROR_SUCCESS) return; auto removed = ::RegDeleteValueW(key, name);
        if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) status = removed;
    };
    if (plan.setCurrent) status = ::RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, reinterpret_cast<BYTE const*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    if (plan.deleteCurrent) DeleteOwned(RUN_VALUE); if (plan.deleteLegacy) DeleteOwned(LEGACY_RUN_VALUE);
    ::RegCloseKey(key); if (status != ERROR_SUCCESS) THROW_WIN32(status); return {};
}

std::wstring CProfilesCoordinator::UpdateAutoStart(bool enabled) const
{
    return m_StartupIntegration ? m_StartupIntegration(enabled) : ConfigureAutoStart(enabled);
}
