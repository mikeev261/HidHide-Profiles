// (c) Eric Korff de Gidts
// SPDX-License-Identifier: MIT
// HidHideClient.cpp
#include "stdafx.h"
#include "HidHideClient.h"
#include "HidHideClientDlg.h"
#include "FilterDriverProxy.h"
#include "Utils.h"
#include "Logging.h"
#include "ConfigurationChannel.h"
#include "ManagerActivation.h"
#include "ProfileRepository.h"
#include "ProfileRecovery.h"
#include "EditorService.h"
#include "ApplicationDiscovery.h"
#include <future>

CHidHideClientApp theApp;

BEGIN_MESSAGE_MAP(CHidHideClientApp, CWinApp)
    ON_COMMAND(ID_HELP, &CWinApp::OnHelp)
END_MESSAGE_MAP()

namespace
{
    class AcceptanceEnforcement : public HidHide::Profiles::IEnforcement
    {
    public:
        enum class ObservationMode { Normal, KnownConflict, Unknown };
        HidHide::Profiles::EnforcementResult Observe() override
        {
            if (m_ObservationMode == ObservationMode::KnownConflict) return { false, true, m_State, L"Injected known conflict", true };
            if (m_ObservationMode == ObservationMode::Unknown) return { false, false, {}, L"Injected unknown read failure" };
            return { true, true, m_State, {} };
        }
        HidHide::Profiles::EnforcementResult Reconcile(HidHide::Profiles::DesiredEnforcement const& desired) override
        {
            if (m_FailReconcile) return { false, false, {}, L"Injected deterministic enforcement failure" };
            m_State = desired; ++m_Writes; return { true, true, m_State, {} };
        }
        HidHide::Profiles::EnforcementResult RestoreBaseline() override { m_State = m_Baseline; return { true, true, m_State, {} }; }
        HidHide::Profiles::EnforcementResult AdoptCurrentAsBaseline() override { ++m_Adoptions; m_RecoveryEvidence.clear(); m_Baseline = m_State; return { true, true, m_State, {} }; }
        std::uint64_t Writes() const noexcept { return m_Writes; }
        std::uint64_t Adoptions() const noexcept { return m_Adoptions; }
        std::vector<std::uint8_t> RecoveryEvidence() const { return m_RecoveryEvidence; }
        void FailReconcile(bool value) noexcept { m_FailReconcile = value; }
        void SetObservationMode(ObservationMode value) noexcept { m_ObservationMode = value; }
        void SetAllowedApplications(std::set<std::filesystem::path> value) { m_State.allowedApplications = std::move(value); }
    private:
        HidHide::Profiles::DesiredEnforcement m_State, m_Baseline;
        std::uint64_t m_Writes{};
        std::uint64_t m_Adoptions{};
        bool m_FailReconcile{};
        ObservationMode m_ObservationMode{ ObservationMode::Normal };
        std::vector<std::uint8_t> m_RecoveryEvidence{ 0x52, 0x45, 0x43, 0x56 };
    };

    class AcceptanceDevices final : public IProfilesDeviceSource
    {
    public:
        std::vector<ProfilesDeviceItem> Enumerate() override
        {
            return { { L"HID\\VID_1234&PID_0001\\CONNECTED", L"Connected wheel — individual HID interface", m_Connected.load() },
                { L"HID\\VID_1234&PID_0002\\REMEMBERED", L"Disconnected pedals — individual HID interface", false } };
        }
        void Refresh() override { ++m_Refreshes; }
        std::uint64_t RefreshCount() const override { return m_Refreshes.load(); }
    private:
        std::atomic_bool m_Connected{ true };
        std::atomic_uint64_t m_Refreshes{};
    };

    bool IsIsolatedRestartRoot(std::filesystem::path const& root)
    {
        wchar_t temporary[MAX_PATH]{}; if (!::GetTempPathW(MAX_PATH, temporary)) return false;
        auto absolute = std::filesystem::absolute(root).lexically_normal().native(); auto prefix = std::filesystem::path(temporary).lexically_normal().native();
        if (absolute.size() <= prefix.size() || _wcsnicmp(absolute.c_str(), prefix.c_str(), prefix.size()) != 0) return false;
        return absolute.find(L"HidHide-Profiles-Restart-Test-") != std::wstring::npos;
    }

    ProfilesDriverTransport IsolatedProfileTestTransport(ProfilesDriverTransport transport)
    {
        auto suffix = std::to_wstring(::GetCurrentProcessId());
        transport.admissionName = L"Local\\HidHide.ProfilesTest.Admission." + suffix;
        transport.barrierName = L"Local\\HidHide.ProfilesTest.Maintenance." + suffix;
        return transport;
    }

    bool ExerciseProductionEnforcementAdapter()
    {
        ProfilesDriverTransport defaults;
        if (defaults.admissionName != HidHide::Maintenance::AdmissionName
            || defaults.barrierName != HidHide::Maintenance::BarrierName)
            throw std::runtime_error("production enforcement transport has invalid maintenance names");

        HidHide::Configuration state; std::uint64_t commits{};
        CProfilesEnforcementAdapter adapter(IsolatedProfileTestTransport(ProfilesDriverTransport{
            [&] { return state; },
            [&](auto const& expected, auto const& desired)
            {
                if (HidHide::DriverState(state) != expected) throw std::runtime_error("test transport CAS mismatch");
                HidHide::SetDriverState(state, desired); ++commits;
            }, false }));
        auto observed = adapter.Observe(); if (!observed.success || !observed.observedKnown || commits) return false;
        HidHide::Profiles::DesiredEnforcement desired; desired.hidingEnabled = true; desired.hiddenDevices.emplace(L"HID\\EXACT\\ONE");
        auto applied = adapter.Reconcile(desired); if (!applied.success || !applied.observedKnown || !(applied.observed == desired) || commits != 1) return false;
        auto deduplicated = adapter.Reconcile(desired); if (!deduplicated.success || commits != 1) return false;
        auto restored = adapter.RestoreBaseline(); if (!restored.success || !restored.observedKnown || commits != 2 || state.active || !state.blacklist.empty()) return false;
        state.blacklist.emplace(L"HID\\EXTERNAL\\CHANGE"); auto conflict = adapter.Observe();
        if (conflict.success || !conflict.conflict || !conflict.observedKnown) return false;

        HidHide::Configuration mismatchState; CProfilesEnforcementAdapter mismatch(IsolatedProfileTestTransport(ProfilesDriverTransport{
            [&] { return mismatchState; }, [](auto const&, auto const&) {}, false }));
        auto mismatchResult = mismatch.Reconcile(desired); if (mismatchResult.success || !mismatchResult.observedKnown) return false;
        HidHide::Configuration partialState; bool failPartialOnce{ true }; CProfilesEnforcementAdapter partial(IsolatedProfileTestTransport(ProfilesDriverTransport{
            [&] { return partialState; }, [&](auto const&, auto const& requested) { HidHide::SetDriverState(partialState, requested); if (std::exchange(failPartialOnce, false)) throw std::runtime_error("partial IOCTL"); }, false }));
        auto partialResult = partial.Reconcile(desired);
        if (partialResult.success || !partialResult.observedKnown || !partialResult.conflict || !(partialResult.observed == desired)) return false;
        auto partialRestored = partial.RestoreBaseline();
        if (!partialRestored.success || !partialRestored.observedKnown || partialState.active || !partialState.blacklist.empty()) return false;

        for (bool structurallyValidButMismatched : { false, true })
        {
            HidHide::Configuration blockedState; std::vector<std::uint8_t> stored;
            if (structurallyValidButMismatched)
            {
                HidHide::DriverConfiguration unrelated; unrelated.active = true; unrelated.blacklist.emplace(L"HID\\UNRELATED");
                stored = HidHide::ProfileRecovery::SerializeV2(HidHide::Channel::CurrentSid(), { unrelated, unrelated, unrelated });
            }
            else stored = { 0xff, 0x00, 0x7f };
            auto original = stored; unsigned clears{}, blockedWrites{}; bool failClearOnce{ true };
            CProfilesEnforcementAdapter blocked(IsolatedProfileTestTransport(ProfilesDriverTransport{
                [&] { return blockedState; }, [&](auto const&, auto const&) { ++blockedWrites; }, true,
                [&]() -> std::optional<std::vector<std::uint8_t>> { return stored; }, [&](auto const& bytes) { stored = bytes; },
                [&] { if (std::exchange(failClearOnce, false)) throw std::runtime_error("injected clear failure"); ++clears; stored.clear(); } }));
            auto blockedObservation = blocked.Observe(); if (blockedObservation.success || !blockedObservation.conflict || stored != original || clears) return false;
            auto blockedRestore = blocked.RestoreBaseline(); if (blockedRestore.success || !blockedRestore.conflict || stored != original || clears) return false;
            try { (void)blocked.PrepareMaintenance(); return false; } catch (std::runtime_error const&) {}
            if (stored != original || clears) return false;
            auto failedAdopt = blocked.AdoptCurrentAsBaseline(); if (failedAdopt.success || !failedAdopt.conflict || stored != original || clears || blockedWrites) return false;
            if (blocked.Reconcile(desired).success || blockedWrites) return false;
            auto adopted = blocked.AdoptCurrentAsBaseline(); if (!adopted.success || clears != 1 || !stored.empty() || blockedWrites) return false;
        }
        {
            HidHide::Configuration saveFailureState; std::vector<std::uint8_t> stored; unsigned saveFailureCommits{}, clears{};
            CProfilesEnforcementAdapter saveFailure(IsolatedProfileTestTransport(ProfilesDriverTransport{
                [&] { return saveFailureState; }, [&](auto const&, auto const&) { ++saveFailureCommits; }, true,
                [&]() -> std::optional<std::vector<std::uint8_t>> { return stored.empty() ? std::nullopt : std::optional(stored); },
                [&](auto const&) { stored = { 0xde, 0xad, 0x00, 0xff }; throw std::runtime_error("injected partial recovery-store write"); },
                [&] { ++clears; stored.clear(); } }));
            auto failed = saveFailure.Reconcile(desired);
            if (failed.success || !failed.conflict || saveFailureCommits || stored.empty()) throw std::runtime_error("save-failure recovery latch did not engage");
            if (saveFailure.Observe().success || saveFailure.Reconcile(desired).success || saveFailure.RestoreBaseline().success || saveFailureCommits) throw std::runtime_error("save-failure recovery latch allowed a later operation");
            try { (void)saveFailure.PrepareMaintenance(); return false; } catch (std::runtime_error const&) {}
            if (stored.empty() || clears || saveFailureCommits) throw std::runtime_error("save-failure evidence was not preserved");
            auto adopted = saveFailure.AdoptCurrentAsBaseline(); if (!adopted.success || !stored.empty() || clears != 1 || saveFailureCommits) throw std::runtime_error("save-failure explicit adoption did not clear safely");
        }
        {
            HidHide::Configuration opaqueState; opaqueState.whitelist.emplace(L"\\Device\\DefinitelyMissingVolume\\opaque.exe");
            CProfilesEnforcementAdapter opaque(IsolatedProfileTestTransport(ProfilesDriverTransport{ [&] { return opaqueState; }, [](auto const&, auto const&) {}, false }));
            auto observation = opaque.Observe(); if (observation.success || observation.observedKnown) throw std::runtime_error("opaque Allowed-app observation was called verified");
            auto reconcile = opaque.Reconcile({}); if (reconcile.success || reconcile.observedKnown) throw std::runtime_error("opaque Allowed-app reconciliation was called verified");
        }
        {
            HidHide::Configuration legacyState; bool legacyPending{ true }; unsigned writes{}, clears{};
            std::map<std::wstring, std::vector<std::uint8_t>> evidence{
                { HidHide::ProfileRecovery::LegacyValue, { 1, 2, 3 } }, { L"ForeignValue", { 9 } } };
            CProfilesEnforcementAdapter legacyBlocked(IsolatedProfileTestTransport(ProfilesDriverTransport{
                [&] { return legacyState; }, [&](auto const&, auto const&) { ++writes; }, true,
                []() -> std::optional<std::vector<std::uint8_t>> { return std::nullopt; }, [](auto const&) {}, [] {},
                [&] { return legacyPending; }, [&]
                {
                    ++clears; HidHide::ProfileRecovery::ClearKnownRecoveryAfterExplicitAdoption(
                        [&](wchar_t const* name) { evidence.erase(name); }); legacyPending = false;
                } }));
            if (legacyBlocked.Observe().success || legacyBlocked.Reconcile(desired).success || legacyBlocked.RestoreBaseline().success || writes || clears)
                throw std::runtime_error("legacy recovery evidence did not block the profiles runtime");
            auto adopted = legacyBlocked.AdoptCurrentAsBaseline();
            if (!adopted.success || writes || clears != 1 || evidence.count(HidHide::ProfileRecovery::LegacyValue) || !evidence.count(L"ForeignValue"))
                throw std::runtime_error("explicit adoption did not clear only known recovery evidence");
        }
        {
            HidHide::Configuration opaqueBlockedState; opaqueBlockedState.whitelist.emplace(L"\\Device\\DefinitelyMissingVolume\\opaque.exe");
            bool legacyPending{ true }; unsigned clears{}, writes{}; auto evidence = std::vector<std::uint8_t>{ 4, 5, 6 };
            CProfilesEnforcementAdapter opaqueBlocked(IsolatedProfileTestTransport(ProfilesDriverTransport{
                [&] { return opaqueBlockedState; }, [&](auto const&, auto const&) { ++writes; }, true,
                []() -> std::optional<std::vector<std::uint8_t>> { return std::nullopt; }, [](auto const&) {}, [] {},
                [&] { return legacyPending; }, [&] { ++clears; evidence.clear(); legacyPending = false; } }));
            auto adopted = opaqueBlocked.AdoptCurrentAsBaseline();
            if (adopted.success || !adopted.conflict || clears || writes || evidence != std::vector<std::uint8_t>({ 4, 5, 6 }))
                throw std::runtime_error("opaque blocked adoption destroyed recovery evidence");
            if (opaqueBlocked.Observe().success || opaqueBlocked.Reconcile(desired).success || opaqueBlocked.RestoreBaseline().success || clears || writes)
                throw std::runtime_error("failed opaque adoption unblocked enforcement");
        }
        return true;
    }

    // Release-only acceptance seam. It is deliberately limited to an isolated
    // temporary root and named semantic events, and never opens the live driver.
    bool RunEditorAcceptanceHost()
    {
        if (__argc != 3 || _wcsicmp(__wargv[1], L"--profiles-editor-host") != 0) return false;
        std::filesystem::path root;
        try
        {
            HidHide::Editor::RequireOrdinaryUser();
            root = std::filesystem::absolute(__wargv[2]).lexically_normal();
            auto pipeName = HidHide::Editor::FixturePipeName(root);
            if (std::filesystem::exists(root)) throw std::runtime_error("Editor fixture requires a new empty test root");
            std::filesystem::create_directory(root);
            using namespace HidHide::Profiles; using namespace HidHide::Profiles::Json;
            class HostDevices final : public IProfilesDeviceSource
            {
                std::vector<ProfilesDeviceItem> Enumerate() override
                {
                    return {{L"HID\\FIXTURE_WHEEL",L"Fixture steering wheel",true,{L"HID\\FIXTURE_WHEEL"},L"wheel",{{true,1,4},{false,0,0}}},
                        {L"HID\\FIXTURE_PEDALS",L"Fixture pedals",true,{L"HID\\FIXTURE_PEDALS"}},
                        {L"HID\\FIXTURE_VIRTUAL",L"Fixture virtual controller",true,{L"HID\\FIXTURE_VIRTUAL"}}};
                }
            } devices;
            class HostEnforcement final : public AcceptanceEnforcement
            {
                std::filesystem::path m_Root;
                bool m_LaunchWitness{};
            public:
                explicit HostEnforcement(std::filesystem::path root) : m_Root(std::move(root)) {}
                void ArmLaunchWitness()
                {
                    m_LaunchWitness = true;
                    std::error_code ignored; std::filesystem::remove(m_Root/L"launch-verified.flag", ignored);
                    std::filesystem::remove(m_Root/L"launch-readback-started.flag", ignored);
                }
                EnforcementResult Observe() override
                {
                    auto result = AcceptanceEnforcement::Observe();
                    if (m_LaunchWitness && result.success && result.observedKnown
                        && result.observed.hiddenDevices.count(L"HID\\FIXTURE_PEDALS"))
                    {
                        std::ofstream started(m_Root/L"launch-readback-started.flag", std::ios::binary|std::ios::trunc);
                        started << "readback"; started.close();
                        ::Sleep(350); // Child execution must remain suspended through readback.
                        std::ofstream marker(m_Root/L"launch-verified.flag", std::ios::binary|std::ios::trunc);
                        marker << "verified";
                        m_LaunchWitness = false;
                    }
                    return result;
                }
            public:
                EnforcementResult Reconcile(DesiredEnforcement const& desired) override
                {
                    auto current = Observe();
                    if (current.success && current.observedKnown && current.observed == desired) return current;
                    return AcceptanceEnforcement::Reconcile(desired);
                }
            } enforcement(root);
            ProfileApplicationService application(root/L"Profiles"); auto initial = application.OpenOrCreate();
            auto settings = initial.snapshot.settings; settings.startWithWindows = false;
            application.ApplySettings(settings,application.SettingsVersion());
            auto gamePath = NormalizeExecutable(root/L"FixtureGame.exe");
            Profile game; game.id = NewStableId(); game.name=L"Fixture Game"; game.kind=Kind::Application; game.executable=gamePath;
            game.rules.push_back({L"HID\\FIXTURE_PEDALS",L"Fixture pedals",Visibility::Hidden});
            application.Apply(game,std::nullopt);
            std::mutex processMutex; std::vector<ProcessObservation> fixtureProcesses; bool scanUnavailable{};
            CProfilesCoordinator coordinator(enforcement,application.Root(),false,[&]
            {
                std::lock_guard<std::mutex> lock(processMutex);
                if (scanUnavailable) throw std::runtime_error("Injected incomplete scan");
                return fixtureProcesses;
            },{},[] { return false; });
            coordinator.AcceptanceScanNow();
            HidHide::Editor::Service service(application,coordinator,devices);
            HidHide::Channel::Server server(pipeName.c_str(), true);
            {
                std::ofstream ready(root/L"editor-host-ready.json",std::ios::binary|std::ios::trunc);
                ready << "{\"pid\":" << ::GetCurrentProcessId() << ",\"pipe\":" << ToUtf8(Escape(pipeName))
                    << ",\"applicationId\":" << ToUtf8(Escape(game.id)) << ",\"globalId\":" << ToUtf8(Escape(settings.selectedGlobalId)) << "}";
            }
            bool stopping{}; auto deadline = ::GetTickCount64()+30*60*1000; std::uint64_t requests{};
            while (::GetTickCount64()<deadline)
            {
                coordinator.Tick();
                (void)::WaitForSingleObject(server.WakeHandle(), std::min<DWORD>(server.WaitTimeoutMs(), 250));
                server.Pump([&](auto const& bytes)
                {
                    ++requests;
                    try
                    {
                        auto json=Parser(FromUtf8(std::string(bytes.begin(),bytes.end()))).Parse(); auto const& request=AsObject(json);
                        auto command=AsString(Required(request,L"command"));
                        if(command==L"fixture-stop")
                        {
                            stopping=true; std::string result="{\"ok\":true}"; return std::vector<std::uint8_t>(result.begin(),result.end());
                        }
                        if(command==L"fixture-state")
                        {
                            auto result=std::string("{\"ok\":true,\"editorOpen\":")+(service.EditorOpen()?"true":"false")
                                +",\"enginePid\":"+std::to_string(::GetCurrentProcessId())+",\"enforcementWrites\":"+std::to_string(enforcement.Writes())+"}";
                            return std::vector<std::uint8_t>(result.begin(),result.end());
                        }
                        if(command==L"fixture-process")
                        {
                            { std::lock_guard<std::mutex> lock(processMutex);
                              fixtureProcesses = AsBool(Required(request,L"running")) ? std::vector<ProcessObservation>{{60001,1,L"FixtureGame.exe",gamePath,true}} : std::vector<ProcessObservation>{}; }
                            coordinator.AcceptanceScanNow();
                            std::string snapshot="{\"command\":\"snapshot\"}"; return service.Handle({snapshot.begin(),snapshot.end()});
                        }
                        if(command==L"fixture-processes" || command==L"fixture-timeline")
                        {
                            auto set = [&](Json::Object const& sample)
                            {
                                std::vector<ProcessObservation> observations;
                                for (auto const& value : AsArray(Required(sample,L"processes")))
                                {
                                    auto const& item=AsObject(value); auto id=AsString(Required(item,L"id"));
                                    auto const& profile=coordinator.Snapshot().profiles.at(id);
                                    observations.push_back({static_cast<DWORD>(AsUnsigned(Required(item,L"pid"))),
                                        AsUnsigned(Required(item,L"created")),profile.executable.filename().native(),profile.executable,true,
                                        item.count(L"exited") ? AsUnsigned(Required(item,L"exited")) : 0});
                                }
                                std::lock_guard<std::mutex> lock(processMutex); fixtureProcesses=std::move(observations);
                                scanUnavailable=AsBool(Required(sample,L"incomplete"));
                            };
                            if(command==L"fixture-timeline")
                                for(auto const& sample : AsArray(Required(request,L"samples"))) { set(AsObject(sample)); ::Sleep(750); }
                            else { set(request); coordinator.AcceptanceScanNow(); }
                            std::string snapshot="{\"command\":\"snapshot\"}"; return service.Handle({snapshot.begin(),snapshot.end()});
                        }
                        if(command==L"fixture-launch-witness")
                        {
                            enforcement.ArmLaunchWitness(); std::string result="{\"ok\":true}";
                            return std::vector<std::uint8_t>(result.begin(),result.end());
                        }
                        if(command==L"fixture-launch-readback")
                        {
                            enforcement.SetObservationMode(AsBool(Required(request,L"fail"))
                                ? AcceptanceEnforcement::ObservationMode::Unknown : AcceptanceEnforcement::ObservationMode::Normal);
                            std::string result="{\"ok\":true}"; return std::vector<std::uint8_t>(result.begin(),result.end());
                        }
                        if(command==L"fixture-reconcile-failure")
                        {
                            auto fail=AsBool(Required(request,L"fail"));
                            enforcement.FailReconcile(fail);
                            enforcement.SetObservationMode(fail ? AcceptanceEnforcement::ObservationMode::Unknown : AcceptanceEnforcement::ObservationMode::Normal);
                            std::string result="{\"ok\":true}"; return std::vector<std::uint8_t>(result.begin(),result.end());
                        }
                    }
                    catch (...) {} // Production handler returns bounded diagnostics.
                    return service.Handle(bytes);
                });
                if(stopping && !server.Connected()) break;
            }
            coordinator.ExitSafely(); auto restored = enforcement.Observe();
            std::ofstream stopped(root/L"editor-host-stopped.json",std::ios::binary|std::ios::trunc);
            stopped << "{\"clean\":" << (stopping?"true":"false") << ",\"requests\":" << requests
                << ",\"enforcementWrites\":" << enforcement.Writes() << ",\"baselineRestored\":"
                << (restored.observedKnown && restored.observed==DesiredEnforcement{}?"true":"false") << "}";
        }
        catch(std::exception const& error)
        {
            // No diagnostics are written outside an already-validated test root.
            try { if(!root.empty()) { (void)HidHide::Editor::FixturePipeName(root); if(std::filesystem::is_directory(root))
                { std::ofstream failed(root/L"editor-host-error.txt",std::ios::binary|std::ios::trunc); failed<<error.what(); } } } catch(...) {}
        }
        return true;
    }

    bool RunEditorSelfTest()
    {
        if (__argc != 3 || _wcsicmp(__wargv[1], L"--profiles-editor-self-test") != 0) return false;
        auto root = std::filesystem::absolute(__wargv[2]).lexically_normal();
        if (!IsIsolatedRestartRoot(root) || std::filesystem::exists(root)) return true;
        std::filesystem::create_directories(root);
        std::ofstream report(root / L"editor-self-test.txt", std::ios::binary | std::ios::trunc);
        try
        {
            using namespace HidHide::Profiles; using namespace HidHide::Profiles::Json;
            AcceptanceEnforcement enforcement; AcceptanceDevices devices;
            ProfileApplicationService application(root / L"Profiles"); application.OpenOrCreate();
            CProfilesCoordinator coordinator(enforcement, application.Root(), false, [] { return std::vector<ProcessObservation>{}; }, {}, [] { return false; });
            HidHide::Editor::Service service(application, coordinator, devices);
            auto request = [&](std::string const& json, bool expectedOk = true)
            {
                auto bytes = service.Handle({json.begin(),json.end()}); auto value = Parser(FromUtf8(std::string(bytes.begin(),bytes.end()))).Parse();
                if (AsBool(Required(AsObject(value),L"ok")) != expectedOk) throw std::runtime_error("Editor response did not match expected outcome: " + std::string(bytes.begin(),bytes.end()));
                return value;
            };
            auto versionJson = [](SavedVersion const& version)
            { return "{\"revision\":" + std::to_string(version.revision) + ",\"hash\":" + ToUtf8(Escape(version.sha256)) + "}"; };
            request("{\"command\":\"snapshot\"}");
            auto id = coordinator.Snapshot().settings.selectedGlobalId;
            auto profile = coordinator.Snapshot().profiles.at(id); auto before = application.Version(id); auto settings = coordinator.Snapshot().settings; auto settingsVersion = application.SettingsVersion();
            auto settingsHash = Sha256(ReadBytes(application.Root()/L"settings.json"));
            auto newResult = request("{\"command\":\"new\",\"kind\":\"global\",\"name\":\"Detached Global\"}");
            auto newId = AsString(Required(AsObject(Required(AsObject(newResult),L"profile")),L"id"));
            if (std::filesystem::exists(application.Root()/(newId+L".json")) || settingsHash != Sha256(ReadBytes(application.Root()/L"settings.json"))) throw std::runtime_error("New profile persisted before Apply");
            profile.name = L"Editor CAS checked"; profile.rules.push_back({L"HID\\VID_1234&PID_0001\\CONNECTED",L"Fixture wheel",Visibility::Hidden});
            auto apply = "{\"command\":\"apply\",\"profile\":"+SerializeProfile(profile)+",\"expected\":"+versionJson(before)+",\"settings\":"+SerializeSettings(settings)+",\"expectedSettings\":"+versionJson(settingsVersion)+"}";
            auto saved = request(apply);
            if (!AsBool(Required(AsObject(saved),L"saved"))) throw std::runtime_error("Apply did not save");
            auto after = application.Version(id); request(apply,false);
            if (application.Version(id).sha256 != after.sha256) throw std::runtime_error("Stale Apply overwrote the saved profile");
            report << "PASS: detached new, CAS apply and stale rejection\n";
            auto exportPath = root/L"export.json"; request("{\"command\":\"export\",\"id\":"+ToUtf8(Escape(id))+",\"path\":"+ToUtf8(Escape(exportPath.native()))+"}");
            auto imported = request("{\"command\":\"import\",\"path\":"+ToUtf8(Escape(exportPath.native()))+"}");
            auto const& importedProfile = AsObject(Required(AsObject(imported),L"profile"));
            if (AsBool(Required(importedProfile,L"enabled")) || AsString(Required(importedProfile,L"id")) == id || application.Version(id).sha256 != after.sha256) throw std::runtime_error("Import was not a disabled detached copy");
            report << "PASS: saved export and detached disabled import\n";
            enforcement.FailReconcile(true); settings = coordinator.Snapshot().settings; settings.paused = true;
            auto paused = request("{\"command\":\"settings\",\"settings\":"+SerializeSettings(settings)+",\"expectedSettings\":"+versionJson(application.SettingsVersion())+"}");
            if (!AsBool(Required(AsObject(paused),L"saved")) || AsBool(Required(AsObject(paused),L"applied"))) throw std::runtime_error("Save and failed enforcement were conflated");
            auto failedHash = application.SettingsVersion().sha256; enforcement.FailReconcile(false); request("{\"command\":\"retry\"}");
            if (application.SettingsVersion().sha256 != failedHash) throw std::runtime_error("Retry rewrote settings");
            report << "PASS: saved-but-not-applied and zero-write retry\n";
            request("{\"command\":\"unrecognized\"}",false); request("{broken",false);
            auto oversized = service.Handle(std::vector<std::uint8_t>(HidHide::Protocol::MaxBytes+1,'x'));
            if (AsBool(Required(AsObject(Parser(FromUtf8(std::string(oversized.begin(),oversized.end()))).Parse()),L"ok"))) throw std::runtime_error("Oversized request was accepted");
            report << "PASS: malformed, unknown and oversized requests rejected\n";
            enforcement.SetObservationMode(AcceptanceEnforcement::ObservationMode::Unknown);
            auto unknown = request("{\"command\":\"snapshot\"}");
            auto const& snapshot = AsObject(Required(AsObject(unknown),L"snapshot"));
            if (AsBool(Required(snapshot,L"verified"))) throw std::runtime_error("Unknown driver state was presented as verified");
            for (auto const& device : AsArray(Required(snapshot,L"devices"))) if (AsString(Required(AsObject(device),L"current")) != L"Unknown") throw std::runtime_error("Unknown device state was presented as visible");
            report << "PASS: unknown observed state remains unknown\n";
            enforcement.SetObservationMode(AcceptanceEnforcement::ObservationMode::Normal);
            settings = coordinator.Snapshot().settings; settings.paused = false; settings.mode = Mode::UseGlobal;
            request("{\"command\":\"settings\",\"settings\":"+SerializeSettings(settings)+",\"expectedSettings\":"+versionJson(application.SettingsVersion())+"}");
            Profile launchProfile; launchProfile.id=NewStableId(); launchProfile.revision=1; launchProfile.name=L"Adoption launch regression";
            launchProfile.kind=Kind::Application; launchProfile.executable=root/L"AdoptionGame.exe";
            request("{\"command\":\"apply\",\"profile\":"+SerializeProfile(launchProfile)+",\"expected\":null,\"settings\":"
                +SerializeSettings(coordinator.Snapshot().settings)+",\"expectedSettings\":"+versionJson(application.SettingsVersion())+"}");
            enforcement.SetAllowedApplications({root/L"FixtureFeeder.exe"});
            enforcement.SetObservationMode(AcceptanceEnforcement::ObservationMode::KnownConflict); coordinator.ObserveEnforcement();
            auto adoptHash = application.SettingsVersion().sha256;
            auto adoption = request("{\"command\":\"adopt\",\"expectedSettings\":"+versionJson(application.SettingsVersion())+"}");
            if (!AsBool(Required(AsObject(adoption),L"needsApply")) || !coordinator.AdoptionAwaitingSave()
                || application.SettingsVersion().sha256 != adoptHash || enforcement.Adoptions() != 1) throw std::runtime_error("Driver adoption did not stage Allowed apps without JSON writes");
            enforcement.SetObservationMode(AcceptanceEnforcement::ObservationMode::Normal);
            auto adoptWrites = enforcement.Writes();
            request("{\"command\":\"launch\",\"id\":"+ToUtf8(Escape(launchProfile.id))+",\"expected\":"+versionJson(application.Version(launchProfile.id))
                +",\"expectedSettings\":"+versionJson(application.SettingsVersion())+"}",false);
            if (!coordinator.AdoptionAwaitingSave() || application.SettingsVersion().sha256 != adoptHash
                || coordinator.Snapshot().settings.mode != Mode::UseGlobal || enforcement.Writes() != adoptWrites
                || !enforcement.Observe().observed.allowedApplications.count(root/L"FixtureFeeder.exe"))
                throw std::runtime_error("Rejected launch mutated settings or discarded the accepted external baseline");
            report << "PASS: launch through production service preserves pending adoption and Use Global settings\n";
            request("{\"command\":\"abandon-adoption\"}");
            if (coordinator.AdoptionAwaitingSave() || application.SettingsVersion().sha256 != adoptHash) throw std::runtime_error("Adoption discard modified the saved catalog");
            enforcement.SetObservationMode(AcceptanceEnforcement::ObservationMode::Normal);
            report << "PASS: explicit driver adoption and discard preserve JSON\n";
            auto backupPath=root/L"Backup";
            request("{\"command\":\"backup\",\"path\":"+ToUtf8(Escape(backupPath.native()))+"}");
            request("{\"command\":\"restore\",\"path\":"+ToUtf8(Escape(backupPath.native()))+"}");
            if (coordinator.Snapshot().profiles.at(id).name != L"Editor CAS checked") throw std::runtime_error("Backup restore changed the saved profile");
            report << "PASS: backup and restore through the production service\n";
            auto pipeName = L"\\\\.\\pipe\\HidHide.Profiles.Editor.Test." + NewStableId();
            HidHide::Channel::Server pipe(pipeName.c_str(), true);
            auto pending = std::async(std::launch::async, [&]
            {
                std::string query="{\"command\":\"snapshot\"}";
                return HidHide::Channel::Exchange({query.begin(),query.end()},pipeName.c_str());
            });
            auto deadline=::GetTickCount64()+10000;
            while(pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && ::GetTickCount64()<deadline)
            { pipe.Pump([&](auto const& bytes){return service.Handle(bytes);}); ::Sleep(10); }
            auto pipeResponse=pending.get();
            if (!AsBool(Required(AsObject(Parser(FromUtf8(std::string(pipeResponse.begin(),pipeResponse.end()))).Parse()),L"ok"))) throw std::runtime_error("Authenticated editor pipe failed");
            report << "PASS: bounded same-user named-pipe exchange\n";
            {
                std::ofstream malformed(application.Root()/L"settings.json",std::ios::binary|std::ios::trunc); malformed<<"{invalid";
            }
            auto broken = request("{\"command\":\"snapshot\"}");
            auto const& brokenSnapshot=AsObject(Required(AsObject(broken),L"snapshot"));
            if (AsArray(Required(brokenSnapshot,L"repositoryIssues")).empty()
                || !std::holds_alternative<std::nullptr_t>(Required(brokenSnapshot,L"settingsVersion").data)
                || ReadBytes(application.Root()/L"settings.json") != "{invalid") throw std::runtime_error("Repository diagnostics were hidden or rewrote invalid evidence");
            report << "PASS: invalid repository is inspectable, blocked and preserved\nALL PASSED\n";
        }
        catch (std::exception const& error) { report << "FAILED: " << error.what() << '\n'; }
        return true;
    }

    void ExerciseCoordinatorScheduling(std::filesystem::path const& root, std::wstring const& phase)
    {
        using namespace HidHide::Profiles;
        auto require = [](bool condition, char const* message) { if (!condition) throw std::runtime_error(message); };
        class Enforcement final : public IEnforcement
        {
        public:
            EnforcementResult Observe() override { return { true, true, state, {} }; }
            EnforcementResult Reconcile(DesiredEnforcement const& desired) override
            {
                ++attempts;
                if (fail) return { false, false, {}, L"Transient test read failure", conflict };
                if (!(state == desired)) { state = desired; ++writes; }
                return { true, true, state, {} };
            }
            EnforcementResult RestoreBaseline() override { ++restores; return { true, true, state, {} }; }
            EnforcementResult AdoptCurrentAsBaseline() override { throw std::runtime_error("Unexpected baseline adoption"); }
            DesiredEnforcement state;
            unsigned attempts{}, writes{}, restores{};
            bool fail{}, conflict{};
        } enforcement;
        // Only posted coordinator notifications drive Tick after the initial
        // startup call, just as the resident dialog's OnProfileChanged does.
        class Notifications
        {
        public:
            explicit Notifications(CProfilesCoordinator& coordinator) : owner(coordinator)
            {
                window = ::CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
                if (!window) throw std::runtime_error("Could not create coordinator notification fixture");
                owner.SetNotificationWindow(window, WM_APP + 73); owner.Tick();
            }
            ~Notifications() { owner.SetNotificationWindow(nullptr, 0); ::DestroyWindow(window); }
            void Pump()
            {
                MSG message{};
                while (::PeekMessageW(&message, window, WM_APP + 73, WM_APP + 73, PM_REMOVE))
                {
                    if (message.wParam) { ++repositoryEvents; owner.ReloadRepositoryIfChanged(); }
                    owner.Tick();
                }
            }
            bool Await(std::function<bool()> const& condition, DWORD timeout = 4500)
            {
                auto deadline = ::GetTickCount64() + timeout;
                do { Pump(); if (condition()) return true; ::Sleep(10); } while (::GetTickCount64() < deadline);
                Pump(); return condition();
            }
            void For(DWORD duration) { (void)Await([] { return false; }, duration); }
            unsigned repositoryEvents{};
        private:
            CProfilesCoordinator& owner; HWND window{};
        };

        if (phase == L"coordinator-late-registration")
        {
            // Let the watcher arm and consume creation before any HWND exists.
            // A Global-only worker cannot fabricate a later process notification.
            auto catalogRoot = root / L"HidHide Profiles" / L"Profiles";
            std::filesystem::create_directories(catalogRoot.parent_path());
            CProfilesCoordinator coordinator(enforcement, catalogRoot, false, {}, {}, [] { return false; });
            ::Sleep(650);
            ProfileApplicationService application(catalogRoot); application.OpenOrCreate();
            ::Sleep(1250);
            require(coordinator.HasRepositoryDiagnostics() && enforcement.attempts == 0,
                "Late-registration fixture did not start from the absent catalog");
            auto before = ReadBytes(catalogRoot / L"settings.json");
            Notifications notifications(coordinator);
            require(notifications.Await([&] { return !coordinator.HasRepositoryDiagnostics() && coordinator.EffectiveSelectionVerified(); }),
                "Notification registration lost the catalog created before the HWND existed");
            require(ReadBytes(catalogRoot / L"settings.json") == before && coordinator.RepositoryWriteCount() == 0 && enforcement.restores == 0,
                "Late notification registration mutated the catalog or baseline");
            return;
        }
        if (phase == L"coordinator-parent-replacement")
        {
            // The watched parent itself moves; do not rename the shared temp
            // directory used by the other isolated fixtures.
            auto catalogRoot = root / L"HidHide Profiles" / L"Profiles";
            ProfileApplicationService application(catalogRoot); application.OpenOrCreate();
            CProfilesCoordinator coordinator(enforcement, catalogRoot, false, {}, {}, [] { return false; });
            Notifications notifications(coordinator);
            require(notifications.Await([&] { return coordinator.EffectiveSelectionVerified(); }), "Parent fixture failed initial activation");
            notifications.For(650);
            auto originalParent = root / L"original-parent";
            std::filesystem::rename(catalogRoot.parent_path(), originalParent);
            std::filesystem::copy(originalParent, catalogRoot.parent_path(), std::filesystem::copy_options::recursive);
            notifications.For(1250);
            auto settings = RepositoryView(catalogRoot).Load().snapshot.settings; settings.paused = true;
            application.ApplySettings(settings, application.SettingsVersion());
            require(notifications.Await([&] { return coordinator.Snapshot().settings.paused && coordinator.EffectiveSelectionVerified(); }),
                "Watcher missed edits after its watched parent was replaced");
            settings.paused = false; application.ApplySettings(settings, application.SettingsVersion());
            require(notifications.Await([&] { return !coordinator.Snapshot().settings.paused && coordinator.EffectiveSelectionVerified(); }),
                "Watcher failed to remain attached to the replacement parent");
            require(coordinator.RepositoryWriteCount() == 0 && enforcement.restores == 0, "Parent watcher mutated catalog or baseline");
            return;
        }
        if (phase == L"coordinator-watcher")
        {
            // Do not create the repository until the watcher has attempted to
            // attach. This also covers the otherwise dormant Global-only worker.
            // Both catalogs remain inside the parent fixture's cleanup boundary.
            auto catalogRoot = root / L"Profiles";
            std::filesystem::create_directories(root);
            CProfilesCoordinator coordinator(enforcement, catalogRoot, false, {}, {}, [] { return false; });
            Notifications notifications(coordinator); notifications.For(650);
            require(!std::filesystem::exists(catalogRoot), "Coordinator created a repository as a watcher workaround");
            ProfileApplicationService application(catalogRoot); application.OpenOrCreate();
            require(notifications.Await([&] { return !coordinator.HasRepositoryDiagnostics() && coordinator.EffectiveSelectionVerified(); }),
                "Late-created repository was never loaded through watcher notification");
            auto settings = coordinator.Snapshot().settings; settings.paused = true;
            application.ApplySettings(settings, application.SettingsVersion());
            require(notifications.Await([&] { return coordinator.Snapshot().settings.paused && coordinator.EffectiveSelectionVerified(); }),
                "Watcher missed later changes after repository creation");
            auto original = root / L"original-repository";
            std::filesystem::rename(catalogRoot, original);
            notifications.For(650); std::filesystem::create_directories(catalogRoot);
            for (auto const& entry : std::filesystem::directory_iterator(original))
                std::filesystem::copy_file(entry.path(), catalogRoot / entry.path().filename());
            require(notifications.Await([&] { return !coordinator.HasRepositoryDiagnostics(); }), "Watcher did not recover after repository replacement");
            settings.paused = false; application.ApplySettings(settings, application.SettingsVersion());
            require(notifications.Await([&] { return !coordinator.Snapshot().settings.paused && coordinator.EffectiveSelectionVerified(); }),
                "Watcher stayed attached to a removed repository");
            require(coordinator.RepositoryWriteCount() == 0 && enforcement.restores == 0, "Watcher mutated repository or baseline");
            return;
        }

        ProfileApplicationService application(root); auto loaded = application.OpenOrCreate();
        Profile game; game.id = NewStableId(); game.revision = 1; game.name = L"Coordinator fixture";
        game.kind = Kind::Application; game.executable = NormalizeExecutable(root / L"CoordinatorFixture.exe");
        game.rules.push_back({ L"HID\\FIXTURE\\ONLY", L"Fixture", Visibility::Hidden });
        if (phase == L"coordinator-missing" || phase == L"coordinator-incomplete") application.Apply(game, std::nullopt);
        auto settingsBytes = ReadBytes(root / L"settings.json");
        auto globalBytes = ReadBytes(root / (loaded.snapshot.settings.selectedGlobalId + L".json"));
        bool maintenance{}; std::atomic_bool incomplete{ false };
        CProfilesCoordinator::ProcessSource processes;
        if (phase == L"coordinator-incomplete") processes = [&]
        {
            if (incomplete.load()) throw std::runtime_error("Temporarily incomplete process scan");
            return std::vector<ProcessObservation>{{4242, 1, game.executable.filename().native(), game.executable, true}};
        };
        enforcement.fail = phase != L"coordinator-missing";
        enforcement.conflict = phase == L"coordinator-conflict";
        CProfilesCoordinator coordinator(enforcement, root, false, processes, {}, [&] { return maintenance; });
        Notifications notifications(coordinator);
        if (phase == L"coordinator-missing")
        {
            require(notifications.Await([&] { return coordinator.IsApplicationMissing(game.id); }), "Missing executable was not detected");
            { std::ofstream executable(game.executable); executable << "fixture; never executed"; }
            require(notifications.Await([&] { return !coordinator.IsApplicationMissing(game.id); }), "Executable creation left stale missing diagnostics");
            std::filesystem::rename(game.executable, root / L"MovedFixture.exe");
            require(notifications.Await([&] { return coordinator.IsApplicationMissing(game.id); }), "Executable move left stale missing diagnostics");
        }
        else
        {
            require(notifications.Await([&] { return enforcement.attempts == 1; }), "Initial reconciliation was not attempted");
            // Rapid explicit ticks must not bypass the retry delay.
            for (unsigned i{}; i < 100; ++i) coordinator.Tick();
            notifications.For(250);
            require(enforcement.attempts == 1, "Transient failure caused immediate repeated driver calls");
            if (phase == L"coordinator-conflict")
            {
                enforcement.fail = false; notifications.For(1750);
                require(coordinator.HasDriverConflict() && !coordinator.EffectiveSelectionVerified() && enforcement.attempts == 1,
                    "Automatic retry bypassed the conflict/recovery guard");
            }
            else
            {
                if (phase == L"coordinator-incomplete") incomplete = true;
                if (phase == L"coordinator-maintenance") maintenance = true;
                if (maintenance || incomplete.load())
                {
                    enforcement.fail = false; notifications.For(1750);
                    require(enforcement.attempts == 1 && !coordinator.EffectiveSelectionVerified(), "Retry bypassed maintenance or incomplete scan");
                    incomplete = false; maintenance = false;
                }
                else
                {
                    notifications.For(1750);
                    require(enforcement.attempts >= 2 && enforcement.attempts <= 3, "Persistent transient failure lacked bounded scheduled retries");
                    enforcement.fail = false;
                }
                require(notifications.Await([&] { return coordinator.EffectiveSelectionVerified(); }), "Unchanged policy did not recover through scheduled notification");
                auto attempts = enforcement.attempts; auto writes = enforcement.writes;
                notifications.For(1250);
                require(enforcement.attempts == attempts && enforcement.writes == writes && writes == 1, "Successful reconciliation kept retrying or wrote unnecessarily");
                if (phase == L"coordinator-incomplete") require(coordinator.EffectiveSelection().profileId == game.id, "Retry lost newest running profile");
            }
        }
        require(ReadBytes(root / L"settings.json") == settingsBytes && ReadBytes(root / (loaded.snapshot.settings.selectedGlobalId + L".json")) == globalBytes
            && coordinator.RepositoryWriteCount() == 0 && enforcement.restores == 0, "Automatic scheduling wrote catalog or restored baseline");
    }

    // Cross-process acceptance exercises the current editor service without constructing
    // retired profile controls. Only the device-coalescer phase needs a hidden HWND.
    bool RunProfileRestartWorker()
    {
        if (__argc < 2 || _wcsicmp(__wargv[1], L"--profile-restart-test") != 0) return false;
        if (__argc != 7) return true; // Never fall through to the installed driver.
        std::filesystem::path root(__wargv[3]); if (!IsIsolatedRestartRoot(root)) return true;
        HANDLE ready = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, __wargv[4]);
        HANDLE command = ::OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, __wargv[5]);
        HANDLE completed = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, __wargv[6]);
        if (!ready || !command || !completed)
        { if (ready) ::CloseHandle(ready); if (command) ::CloseHandle(command); if (completed) ::CloseHandle(completed); return true; }
        try
        {
            using namespace HidHide::Profiles; using namespace HidHide::Profiles::Json;
            auto require = [](bool condition, char const* message) { if (!condition) throw std::runtime_error(message); };
            auto awaitCommand = [&] { require(::WaitForSingleObject(command, 10000) == WAIT_OBJECT_0, "acceptance command timed out"); ::ResetEvent(command); };
            auto finish = [&]
            {
                bool visible{};
                ::EnumWindows([](HWND window, LPARAM context)
                {
                    DWORD owner{}; ::GetWindowThreadProcessId(window, &owner);
                    if (owner == ::GetCurrentProcessId() && ::IsWindowVisible(window)) *reinterpret_cast<bool*>(context) = true;
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&visible));
                require(!visible, "headless acceptance opened a visible window");
                ::SetEvent(ready); ::SetEvent(completed); awaitCommand();
            };
            auto phase = std::wstring(__wargv[2]);
            if (phase == L"hold")
            {
                { WriterLease lease(root); ::SetEvent(ready); awaitCommand(); }
                ::SetEvent(completed); ::Sleep(INFINITE);
            }
            AcceptanceEnforcement enforcement; AcceptanceDevices devices;
            if (phase.rfind(L"coordinator-", 0) == 0)
            {
                ExerciseCoordinatorScheduling(root, phase); finish();
            }
            else if (phase == L"device-coalescing")
            {
                ProfilesAcceptanceContext context{root, enforcement, devices, [] { return std::vector<ProcessObservation>{}; }};
                CHidHideClientDlg dialog(nullptr, context);
                require(dialog.Create(IDD_DIALOG_APPLICATION) != FALSE, "hidden engine window creation failed");
                require(!dialog.IsWindowVisible(), "resident fixture unexpectedly became visible");
                require(dialog.AcceptanceDeviceBurstCoalesced(), "device burst was not bounded and coalesced");
                dialog.DestroyWindow(); finish();
            }
            else
            {
                if (phase == L"unknown-fresh") enforcement.SetObservationMode(AcceptanceEnforcement::ObservationMode::Unknown);
                if (phase == L"conflict-fresh") enforcement.SetObservationMode(AcceptanceEnforcement::ObservationMode::KnownConflict);
                ProfileApplicationService application(root); auto initial = enforcement.Observe();
                application.OpenOrCreateObserved(initial.success && initial.observedKnown && !initial.conflict
                    ? std::optional<std::set<std::filesystem::path>>(initial.observed.allowedApplications) : std::nullopt);
                std::atomic_int running{};
                auto executable = NormalizeExecutable(root/L"F1_25.exe");
                auto processSource = [&] { return running.load() ? std::vector<ProcessObservation>{{4242, 1, L"F1_25.exe", executable, true}} : std::vector<ProcessObservation>{}; };
                auto startup = phase == L"startup-failure" ? CProfilesCoordinator::StartupIntegration([](bool) -> std::wstring { throw std::runtime_error("injected Run-key failure"); }) : CProfilesCoordinator::StartupIntegration{};
                CProfilesCoordinator coordinator(enforcement, root, phase == L"startup-failure", processSource, startup,
                    [phase] { if (phase == L"adapter") throw std::runtime_error("injected maintenance inspection failure"); return false; }, initial);
                if (!coordinator.HasRepositoryDiagnostics()) coordinator.AcceptanceScanNow();
                HidHide::Editor::Service service(application, coordinator, devices);
                auto request = [&](std::string const& json, bool expectedOk = true)
                {
                    auto bytes = service.Handle({json.begin(), json.end()}); auto text = std::string(bytes.begin(), bytes.end());
                    auto result = Parser(FromUtf8(text)).Parse();
                    if (AsBool(Required(AsObject(result), L"ok")) != expectedOk) throw std::runtime_error("Unexpected service response: " + text);
                    return result;
                };
                auto version = [](SavedVersion const& value) { return "{\"revision\":"+std::to_string(value.revision)+",\"hash\":"+ToUtf8(Escape(value.sha256))+"}"; };
                auto settingsRequest = [&](Settings const& settings)
                { return request("{\"command\":\"settings\",\"settings\":"+SerializeSettings(settings)+",\"expectedSettings\":"+version(application.SettingsVersion())+"}"); };
                auto newDraft = [&]
                {
                    { std::ofstream file(executable); file << "isolated non-executable fixture"; }
                    auto result = request("{\"command\":\"new\",\"kind\":\"application\",\"name\":\"F1 25\",\"executable\":"+ToUtf8(Escape(executable.native()))+"}");
                    auto const& wire = AsObject(Required(AsObject(result), L"profile"));
                    Profile profile; profile.id = AsString(Required(wire,L"id")); profile.name = AsString(Required(wire,L"name"));
                    profile.revision = 1; profile.kind = Kind::Application; profile.executable = NormalizeExecutable(AsString(Required(wire,L"executablePath")));
                    profile.priority = -1;
                    for (auto const& device : devices.Enumerate()) profile.rules.push_back({device.identity, device.friendly, Visibility::Hidden});
                    return profile;
                };
                auto apply = [&](Profile const& profile)
                { return request("{\"command\":\"apply\",\"profile\":"+SerializeProfile(profile)+",\"expected\":null,\"settings\":"+SerializeSettings(coordinator.Snapshot().settings)+",\"expectedSettings\":"+version(application.SettingsVersion())+"}"); };
                auto adopt = [&](std::string const& expected, bool ok = true)
                { return request("{\"command\":\"adopt\",\"expectedSettings\":"+expected+"}", ok); };
                if (phase == L"apply")
                {
                    ::SetEvent(ready); awaitCommand(); ::ResetEvent(ready);
                    auto draft = newDraft(); ::SetEvent(completed); ::SetEvent(ready); awaitCommand();
                    auto saved = apply(draft); require(AsBool(Required(AsObject(saved), L"saved")), "Apply failed to save");
                    ::SetEvent(ready); ::SetEvent(completed); ::Sleep(INFINITE); // Parent tests abrupt process loss.
                }
                else if (phase == L"reload")
                {
                    auto const& profiles = coordinator.Snapshot().profiles;
                    auto found = std::find_if(profiles.begin(), profiles.end(), [&](auto const& item) { return item.second.executable == executable; });
                    require(found != profiles.end() && found->second.name == L"F1 25" && found->second.rules.size() == 2, "saved application did not reload");
                    request("{\"command\":\"snapshot\"}");
                }
                else if (phase == L"invalid" || phase == L"unknown-fresh" || phase == L"conflict-fresh")
                {
                    auto result = request("{\"command\":\"snapshot\"}"); auto const& snapshot = AsObject(Required(AsObject(result), L"snapshot"));
                    require(!AsArray(Required(snapshot,L"repositoryIssues")).empty() && coordinator.HasRepositoryDiagnostics(), "invalid repository was not blocked");
                    std::string retryRequest="{\"command\":\"retry\"}";
                    auto retryBytes=service.Handle({retryRequest.begin(),retryRequest.end()});
                    auto retry=Parser(FromUtf8(std::string(retryBytes.begin(),retryBytes.end()))).Parse();
                    require(!AsBool(Required(AsObject(retry),L"ok")) || !AsBool(Required(AsObject(retry),L"applied")), "blocked repository retry reported success");
                    request("{\"command\":\"adopt\",\"expectedSettings\":{\"revision\":1,\"hash\":\"invalid\"}}", false);
                    require(enforcement.Writes() == 0 && enforcement.Adoptions() == 0, "blocked service touched driver state");
                }
                else if (phase == L"adapter")
                {
                    require(ExerciseProductionEnforcementAdapter(), "production adapter conformance failed");
                    require(!coordinator.EffectiveSelectionVerified() && coordinator.Status().find(L"maintenance") != std::wstring::npos, "maintenance failure was not fail-closed");
                }
                else if (phase == L"restore")
                {
                    require(coordinator.HasRepositoryDiagnostics(), "restore fixture was not initially blocked");
                    request("{\"command\":\"restore\",\"path\":"+ToUtf8(Escape(root.native()+L"-backup"))+"}");
                    require(!coordinator.HasRepositoryDiagnostics() && coordinator.EffectiveSelectionVerified() && enforcement.Writes() == 1, "restore did not recover and apply immediately");
                }
                else if (phase == L"events")
                {
                    enforcement.FailReconcile(true); auto draft = newDraft(); (void)draft;
                    coordinator.AcceptanceScanNow(); devices.Refresh(); request("{\"command\":\"snapshot\"}");
                    request("{\"command\":\"retry\"}");
                }
                else if (phase == L"enforcement-failure")
                {
                    enforcement.FailReconcile(true); auto settings = coordinator.Snapshot().settings; settings.paused = true;
                    auto result = settingsRequest(settings);
                    require(AsBool(Required(AsObject(result), L"saved")) && !AsBool(Required(AsObject(result), L"applied")), "save and failed enforcement were conflated");
                    auto hash = application.SettingsVersion().sha256; enforcement.FailReconcile(false); request("{\"command\":\"retry\"}");
                    require(coordinator.EffectiveSelectionVerified() && hash == application.SettingsVersion().sha256, "retry did not verify without writes");
                }
                else if (phase == L"live-status")
                {
                    auto draft = newDraft(); apply(draft); auto hash = application.SettingsVersion().sha256; auto saved = application.Version(draft.id).sha256;
                    running = 1; require(coordinator.RetryActivation().applied && coordinator.IsVerifiedRunning(draft.id)
                        && coordinator.EffectiveSelection().profileId == draft.id, "running application did not activate");
                    running = 0; require(coordinator.RetryActivation().applied && !coordinator.IsVerifiedRunning(draft.id)
                        && coordinator.EffectiveSelection().profileId == coordinator.Snapshot().settings.selectedGlobalId, "exited application did not return to Global");
                    require(hash == application.SettingsVersion().sha256 && saved == application.Version(draft.id).sha256, "process selection wrote repository");
                }
                else if (phase == L"startup-failure")
                {
                    require(coordinator.Status().find(L"Run-key") != std::wstring::npos && coordinator.EffectiveSelectionVerified(), "startup error blocked policy or was hidden");
                }
                else if (phase == L"repository-domain" || phase == L"adoption-race")
                {
                    auto expected = version(application.SettingsVersion()); auto adoptions = enforcement.Adoptions(); auto evidence = enforcement.RecoveryEvidence();
                    enforcement.SetObservationMode(AcceptanceEnforcement::ObservationMode::KnownConflict); coordinator.ObserveEnforcement();
                    { std::ofstream corrupt(root/L"settings.json", std::ios::trunc); corrupt << "{"; }
                    request("{\"command\":\"snapshot\"}"); adopt(expected, false);
                    require(coordinator.HasRepositoryDiagnostics() && enforcement.Adoptions() == adoptions && enforcement.RecoveryEvidence() == evidence
                        && ReadBytes(root/L"settings.json") == "{", "repository diagnostics crossed driver adoption boundary");
                }
                else if (phase == L"adoption-retry")
                {
                    enforcement.SetObservationMode(AcceptanceEnforcement::ObservationMode::KnownConflict); coordinator.ObserveEnforcement(); enforcement.FailReconcile(true);
                    auto hash = application.SettingsVersion().sha256; auto result = adopt(version(application.SettingsVersion()));
                    require(!AsBool(Required(AsObject(result),L"applied")) && !coordinator.AdoptionAwaitingSave(), "failed equal-Allowed adoption left an unsavable draft");
                    enforcement.FailReconcile(false); enforcement.SetObservationMode(AcceptanceEnforcement::ObservationMode::Normal);
                    request("{\"command\":\"retry\"}"); require(coordinator.EffectiveSelectionVerified() && hash == application.SettingsVersion().sha256, "post-adoption retry failed or wrote JSON");
                }
                else if (phase == L"global-status")
                {
                    require(coordinator.EffectiveSelection().reason == SelectionReason::GlobalFallback, "Automatic fallback reason missing");
                    auto settings = coordinator.Snapshot().settings; settings.mode = Mode::UseGlobal; settingsRequest(settings);
                    require(coordinator.EffectiveSelection().reason == SelectionReason::ManualGlobal, "Manual Global reason missing");
                    settings = coordinator.Snapshot().settings; settings.paused = true; settingsRequest(settings);
                    require(coordinator.EffectiveSelection().reason == SelectionReason::Paused && !enforcement.Observe().observed.hidingEnabled, "Paused reason or hiding state incorrect");
                }
                else if (phase == L"adoption-changed-lifecycle")
                {
                    auto hash = application.SettingsVersion().sha256; auto feeder = NormalizeExecutable(root/L"Feeder.exe");
                    { std::ofstream file(feeder); file << "fixture"; }
                    enforcement.SetAllowedApplications({feeder}); enforcement.SetObservationMode(AcceptanceEnforcement::ObservationMode::KnownConflict); coordinator.ObserveEnforcement();
                    auto result = adopt(version(application.SettingsVersion()));
                    require(AsBool(Required(AsObject(result),L"needsApply")) && coordinator.AdoptionAwaitingSave() && hash == application.SettingsVersion().sha256, "changed adoption did not stay detached");
                    auto settings = coordinator.Snapshot().settings; settings.allowedApplications = {feeder};
                    request("{\"command\":\"abandon-adoption\"}");
                    require(!coordinator.AdoptionAwaitingSave() && hash == application.SettingsVersion().sha256, "discard changed repository");
                    enforcement.SetObservationMode(AcceptanceEnforcement::ObservationMode::KnownConflict); coordinator.ObserveEnforcement();
                    adopt(version(application.SettingsVersion())); settingsRequest(settings);
                    require(coordinator.Snapshot().settings.allowedApplications.count(feeder) == 1 && !coordinator.AdoptionAwaitingSave(), "adopted Allowed apps were not committed");
                }
                else if (phase == L"verified-observation" || phase == L"known-observation" || phase == L"known-observation-allowed-change"
                    || phase == L"unknown-observation" || phase == L"device-conflict-propagation" || phase == L"selection-unknown-propagation")
                {
                    bool verified = phase == L"verified-observation";
                    bool unknown = phase == L"unknown-observation" || phase == L"selection-unknown-propagation";
                    if (phase == L"known-observation-allowed-change") enforcement.SetAllowedApplications({NormalizeExecutable(root/L"Feeder.exe")});
                    if (!verified) enforcement.SetObservationMode(unknown ? AcceptanceEnforcement::ObservationMode::Unknown : AcceptanceEnforcement::ObservationMode::KnownConflict);
                    devices.Refresh(); auto result = request("{\"command\":\"snapshot\"}"); auto const& snapshot = AsObject(Required(AsObject(result),L"snapshot"));
                    require(AsBool(Required(snapshot,L"verified")) == verified, "snapshot verification was stale");
                    for (auto const& device : AsArray(Required(snapshot,L"devices")))
                        require((AsString(Required(AsObject(device),L"current")) == L"Unknown") == unknown, "known and unknown device state were conflated");
                    require(coordinator.HasDriverConflict() == (!verified && !unknown), "driver conflict state was stale");
                }
                else throw std::runtime_error("unknown headless acceptance phase");
                finish(); coordinator.Stop();
            }
        }
        catch (std::exception const& error)
        {
            std::ofstream diagnostic(root / L".acceptance-error.txt", std::ios::binary | std::ios::trunc); diagnostic << error.what(); diagnostic.close(); ::SetEvent(completed);
        }
        catch (...) { ::SetEvent(completed); }
        ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed); return true;
    }

    HHOOK s_hHook;

    // Alter message box labels and detach from the window activiation notification
    LRESULT CALLBACK LocalizedMessageBoxCBTProc(_In_ INT code, _In_ WPARAM wParam, _In_ LPARAM lParam)
    {
        // Act during window activitation
        if (HCBT_ACTIVATE == code)
        {
            TRACE_ALWAYS(L"");
            auto dlg{ reinterpret_cast<HWND>(wParam) };

            // Alter labels
            if (nullptr != ::GetDlgItem(dlg, IDOK))     ::SetDlgItemTextW(dlg, IDOK,     HidHide::StringTable(IDS_STATIC_MESSAGEBOX_OK).c_str());
            if (nullptr != ::GetDlgItem(dlg, IDCANCEL)) ::SetDlgItemTextW(dlg, IDCANCEL, HidHide::StringTable(IDS_STATIC_MESSAGEBOX_CANCEL).c_str());
            if (nullptr != ::GetDlgItem(dlg, IDRETRY))  ::SetDlgItemTextW(dlg, IDRETRY,  HidHide::StringTable(IDS_STATIC_MESSAGEBOX_RETRY).c_str());
            if (nullptr != ::GetDlgItem(dlg, IDIGNORE)) ::SetDlgItemTextW(dlg, IDIGNORE, HidHide::StringTable(IDS_STATIC_MESSAGEBOX_IGNORE).c_str());
            if (nullptr != ::GetDlgItem(dlg, IDABORT))  ::SetDlgItemTextW(dlg, IDABORT,  HidHide::StringTable(IDS_STATIC_MESSAGEBOX_ABORT).c_str());
            if (nullptr != ::GetDlgItem(dlg, IDYES))    ::SetDlgItemTextW(dlg, IDYES,    HidHide::StringTable(IDS_STATIC_MESSAGEBOX_YES).c_str());
            if (nullptr != ::GetDlgItem(dlg, IDNO))     ::SetDlgItemTextW(dlg, IDNO,     HidHide::StringTable(IDS_STATIC_MESSAGEBOX_NO).c_str());

            // Fire-once so detach again
            ::UnhookWindowsHookEx(s_hHook);
        }

        // Allow other hooks to act too
        ::CallNextHookEx(s_hHook, code, wParam, lParam);
        return (0);
    }

    // Show message box with localized buttons
    INT WINAPI LocalizedMessageBox(_In_ UINT resourceId, _In_ UINT type)
    {
        TRACE_ALWAYS(L"");

        // Attach hook (fire-once)
        s_hHook = ::SetWindowsHookExW(WH_CBT, &LocalizedMessageBoxCBTProc, 0, ::GetCurrentThreadId());
        return (::MessageBoxExW(::AfxGetApp()->GetMainWnd()->m_hWnd, HidHide::StringTable(resourceId).c_str(), HidHide::StringTable(IDS_DIALOG_APPLICATION).c_str(), type, LANG_USER_DEFAULT));
    }
}

// Register the ETW logging and tracing providers
NTSTATUS WINAPI LogRegisterProviders() noexcept
{
    try
    {
        EventRegisterNefarius_HidHide_Client();
        EventRegisterNefarius_Drivers_HidHideClient();

        // The define for BldProductVersion is passed from the project file to the source code via a define
        ::LogEvent(ETW(Started), L"%s", _L(BldProductVersion));
        return (STATUS_SUCCESS);
    }
    catch (...)
    {
        DBG_AND_RETURN_NTSTATUS("LogRegisterProviders", STATUS_UNHANDLED_EXCEPTION);
    }
}

// Unregister the ETW logging and tracing providers
NTSTATUS WINAPI LogUnregisterProviders() noexcept
{
    try
    {
        ::LogEvent(ETW(Stopped), L"");
        EventUnregisterNefarius_Drivers_HidHideClient();
        EventUnregisterNefarius_HidHide_Client();
        return (STATUS_SUCCESS);
    }
    catch (...)
    {
        DBG_AND_RETURN_NTSTATUS("LogUnregisterProviders", STATUS_UNHANDLED_EXCEPTION);
    }
}

CHidHideClientApp::CHidHideClientApp() noexcept
{
    ::LogRegisterProviders();
}

CHidHideClientApp::~CHidHideClientApp()
{
    ::LogUnregisterProviders();
}

BOOL CHidHideClientApp::InitInstance()
{
    TRACE_ALWAYS(L"");

    // The editor bridge serves one or more requests with no driver or profile
    // ownership. Run before OLE, the dialog, startup integration, or any lease.
    if (HidHide::Applications::RunHelper()) return FALSE;
    if (HidHide::Editor::RunRequestBridge()) return FALSE;

    // Initialize OLE library
    AfxOleInit();

    // Initialize the common controls .dll first
    INITCOMMONCONTROLSEX initCommonControlsEx;
    initCommonControlsEx.dwSize = sizeof(initCommonControlsEx);
    initCommonControlsEx.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&initCommonControlsEx);

    // Initialize the application instance
    CWinApp::InitInstance();

    // Initialize COM services
    AfxEnableControlContainer();

    // The isolated acceptance process uses the same real dialog, page,
    // coordinator, repository view and Apply message map as production.  Only
    // process/device/enforcement adapters and the repository root are replaced.
    if (RunEditorAcceptanceHost()) return FALSE;
    if (RunEditorSelfTest()) return FALSE;
    if (RunProfileRestartWorker()) return FALSE;

    // Create the shell manager, in case the dialog contains any shell tree view or shell list view controls
    std::unique_ptr<CShellManager> const shellManager{ std::make_unique<CShellManager>() };

    // Activate "Windows Native" visual manager for enabling themes in MFC controls
    CMFCVisualManager::SetDefaultManager(RUNTIME_CLASS(CMFCVisualManagerWindows));

    // We can't do anything when the control device isn't present so allow for a retry on failure
    bool startHidden{};
    for (int index = 1; index < __argc; index++)
        startHidden = startHidden || (0 == _wcsicmp(__wargv[index], L"--background"));

    // Keep ownership until the modal dialog and its profile manager are destroyed.
    // A namespace-static lease would be destroyed before the earlier global app
    // object, making any release from the app destructor a use-after-destruction.
    std::unique_ptr<HidHide::Channel::Lease> owner;
    try
    {
        HidHide::Editor::RequireOrdinaryUser();
        HidHide::Maintenance::Admission admission;
        owner = std::make_unique<HidHide::Channel::Lease>();
    }
    catch (std::exception const& error)
    {
        if (!startHidden) ::MessageBoxA(nullptr, error.what(), "HidHide ownership", MB_OK | MB_ICONERROR);
        return FALSE;
    }
    if (!owner->Acquired())
    {
        if (!startHidden && !HidHide::ManagerActivation::ShowExisting(WM_HIDHIDE_SHOW_MANAGER))
            ::MessageBoxW(nullptr, L"The configuration coordinator is already running but could not be opened in this Windows session. It may be starting or owned by another session. Try again shortly or open it from its tray icon.", L"HidHide Profiles", MB_OK | MB_ICONINFORMATION);
        return FALSE;
    }

    CHidHideClientDlg dlg(nullptr, startHidden);
    m_pMainWnd = &dlg;

    // We use exception handling so catch it at top-level and bail out
    try
    {
        // Keep retrying when the device is unavailable
        while (true)
        {
            if (auto const deviceStatus{ HidHide::FilterDriverProxy::DeviceStatus() }; (ERROR_SUCCESS == deviceStatus))
            {
                // Let Electron load its local renderer while the native owner
                // verifies recovery, repository and driver state. It will show
                // Connecting until this dialog creates the authenticated pipe.
                if (!startHidden)
                {
                    try { HidHide::Editor::Launch(true); dlg.MarkEditorPrelaunched(); }
                    catch (...) {} // The regular dialog launch reports a missing editor.
                }
                if (-1 == dlg.DoModal()) THROW_WIN32_LAST_ERROR;
                break;
            }
            else
            {
                TRACE_ALWAYS(L"");
                // Electron owns visible startup diagnostics. A background engine
                // must never leave a hidden retry dialog waiting for interaction.
                if (startHidden) break;
                if (IDRETRY != LocalizedMessageBox(((ERROR_ACCESS_DENIED == deviceStatus) ? IDS_STATIC_MESSAGEBOX_IN_USE : IDS_STATIC_MESSAGEBOX_PRESENT), (MB_RETRYCANCEL | MB_ICONEXCLAMATION)))
                {
                    break;
                }
            }
        }
    }
    catch (std::exception const& error)
    {
        LOGEXC_AND_CONTINUE;
        std::string message = "HidHide Profiles could not start or continue:\n\n";
        message += error.what();
        if (!startHidden) ::MessageBoxA(nullptr, message.c_str(), "HidHide Profiles", MB_OK | MB_ICONERROR);
    }
    catch (...)
    {
        LOGEXC_AND_CONTINUE;
        if (!startHidden) LocalizedMessageBox(IDS_STATIC_MESSAGEBOX_EXCEPTION, (MB_OK | MB_ICONERROR));
    }

    // Don't start the application's message pump as we are done already
    return (FALSE);
}
