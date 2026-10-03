// SPDX-License-Identifier: MIT
#pragma once

#include "FilterDriverProxy.h"
#include "ProfileRepository.h"
#include "ProfileProcessLifetime.h"
#include <condition_variable>
#include <mutex>
#include <thread>

class CProfilesCoordinator
{
public:
    struct ApplyOutcome
    {
        bool saved{};
        bool applied{};
        bool observedKnown{};
        std::wstring message;
        HidHide::Profiles::SavedVersion version;
    };

    using ProcessSource = std::function<std::vector<HidHide::Profiles::ProcessObservation>()>;
    using StartupIntegration = std::function<std::wstring(bool)>;
    using MaintenanceSource = std::function<bool()>;
    CProfilesCoordinator(HidHide::Profiles::IEnforcement& enforcement, std::filesystem::path repositoryRoot,
        bool systemIntegration = true, ProcessSource processSource = {}, StartupIntegration startupIntegration = {},
        MaintenanceSource maintenanceSource = {},
        std::optional<HidHide::Profiles::EnforcementResult> initialObservation = std::nullopt);
    ~CProfilesCoordinator();
    CProfilesCoordinator(CProfilesCoordinator const&) = delete;
    CProfilesCoordinator& operator=(CProfilesCoordinator const&) = delete;

    HidHide::Profiles::Snapshot const& Snapshot() const { return m_Snapshot; }
    std::map<std::wstring, HidHide::Profiles::SavedVersion> const& ProfileVersions() const { return m_ProfileVersions; }
    HidHide::Profiles::SavedVersion const& SettingsVersion() const { return m_SettingsVersion; }
    std::vector<HidHide::Profiles::RepositoryIssue> const& Issues() const { return m_Issues; }
    std::filesystem::path const& RepositoryRoot() const { return m_Repository.Root(); }
    HidHide::Profiles::Selection const& EffectiveSelection() const { return m_Selection; }
    bool EffectiveSelectionVerified() const { return m_EffectiveVerified; }
    std::wstring Status() const { return m_StartupIssue.empty() ? m_Status : m_Status + L" " + m_StartupIssue; }
    bool HasDriverConflict() const { return m_Conflict; }
    bool HasRepositoryDiagnostics() const { return m_RepositoryInvalid; }
    bool IsVerifiedRunning(std::wstring const& id) const { return m_RunningProfiles.count(id) != 0; }
    bool IsApplicationMissing(std::wstring const& id) const { return m_MissingProfiles.count(id) != 0; }
    HidHide::Profiles::EnforcementResult ObserveEnforcement();
    std::uint64_t RepositoryWriteCount() const { return m_RepositoryWrites; }

    ApplyOutcome PublishSaved(HidHide::Profiles::LoadResult loaded, HidHide::Profiles::SavedVersion version, std::wstring action, bool cleanupPending = false);
    ApplyOutcome RetryActivation();
    // Called synchronously while the service retains its repository writer lease.
    // true commits Automatic; false restores the prior mode after confirmed abort.
    std::wstring LaunchSavedProfile(std::wstring const& id, std::function<void(bool)> const& changeMode);
    void ValidateLaunch(std::wstring const& id, DWORD ownPid = 0);
    bool HasLaunchedProcess() const;
    bool LaunchUncertain() const { return m_LaunchUncertain; }
    std::vector<std::wstring> RunningOrder() const;
    std::wstring ManualMaskId() const;
    HidHide::Profiles::Selection RequestedSelection() const;
    ApplyOutcome SelectMask(std::wstring const& id);
    std::wstring const& LaunchedProfileId() const { return m_LaunchedProfileId; }
    void Tick();
    bool AcceptanceScanNow();
    bool ReloadRepositoryIfChanged();
    void SetNotificationWindow(HWND window, UINT message);
    void ExitSafely();
    std::set<std::filesystem::path> AdoptExternalState();
    ApplyOutcome CompleteAdoptionWithoutSettingsChange(std::set<std::filesystem::path> const& observedAllowedApplications);
    ApplyOutcome AbandonAdoptionDraft();
    bool AdoptionAwaitingSave() const { return m_AdoptedNeedsApply; }
    void Stop() noexcept;

private:
    struct ScanResult { std::uint64_t revision{}; bool complete{ true }; std::vector<HidHide::Profiles::ProcessObservation> processes; std::set<std::wstring> runningProfiles, missingProfiles; HidHide::Profiles::Selection selection; };
    ScanResult Scan(HidHide::Profiles::Snapshot const& snapshot);
    bool CanReconcileScan(bool complete) const;
    void WorkerMain() noexcept;
    void RepositoryWatcherMain() noexcept;
    void SubmitSnapshot();
    void ScheduleReconcileRetry(bool pending);
    ApplyOutcome Reconcile(HidHide::Profiles::Selection const& selection, bool saved, HidHide::Profiles::SavedVersion version = {});
    void SetVerifiedSelection(HidHide::Profiles::Selection const& selection);
    static HidHide::Profiles::DesiredEnforcement Desired(HidHide::Profiles::Snapshot const& snapshot, HidHide::Profiles::Selection const& selection);
    std::wstring ConfigureAutoStart(bool enabled) const;
    std::wstring UpdateAutoStart(bool enabled) const;
    HidHide::Profiles::IEnforcement& m_Enforcement;
    bool m_SystemIntegration{ true };
    ProcessSource m_ProcessSource;
    StartupIntegration m_StartupIntegration;
    MaintenanceSource m_MaintenanceSource;
    HidHide::Profiles::RepositoryView m_Repository;
    HidHide::Profiles::Snapshot m_Snapshot;
    std::vector<HidHide::Profiles::RepositoryIssue> m_Issues;
    HidHide::Profiles::Selection m_Selection;
    std::set<std::wstring> m_RunningProfiles, m_MissingProfiles;
    HidHide::Profiles::SavedVersion m_SettingsVersion;
    std::map<std::wstring, HidHide::Profiles::SavedVersion> m_ProfileVersions;
    std::wstring m_Status{ L"Starting" };
    std::wstring m_StartupIssue;
    bool m_Conflict{};
    bool m_RepositoryInvalid{};
    bool m_AdoptedNeedsApply{};
    bool m_EffectiveVerified{};
    std::uint64_t m_RepositoryWrites{};

    std::mutex m_WorkerMutex;
    std::condition_variable m_WorkerWake;
    HidHide::Profiles::Snapshot m_PendingSnapshot;
    std::uint64_t m_SubmittedRevision{};
    std::uint64_t m_CompletedSequence{};
    std::uint64_t m_AppliedSequence{};
    bool m_ReconcileRetryPending{};
    std::chrono::steady_clock::time_point m_ReconcileRetryAt{};
    ScanResult m_Completed;
    std::optional<HidHide::Profiles::Selection> m_LastPublishedSelection;
    bool m_LastPublishedComplete{};
    std::uint64_t m_LastPublishedRevision{};
    std::set<std::wstring> m_LastPublishedRunning, m_LastPublishedMissing;
    HidHide::Profiles::ProcessIdentityCache m_ProcessCache;
    mutable std::mutex m_ScanMutex;
    HidHide::Profiles::ActivationHistory m_Activations;
    bool m_LastScanComplete{};
    std::vector<HANDLE> m_OwnedProcesses;
    HidHide::Profiles::ProcessLifetimeCache m_ObservedProcesses;
    bool m_StopRequested{};
    bool m_WorkerFailed{};
    std::thread m_Worker;
    std::thread m_RepositoryWatcher;
    HANDLE m_WatcherStop{};
    HWND m_NotifyWindow{};
    UINT m_NotifyMessage{};
    HANDLE m_LaunchedProcess{};
    std::wstring m_LaunchedProfileId;
    bool m_LaunchUncertain{};
};
