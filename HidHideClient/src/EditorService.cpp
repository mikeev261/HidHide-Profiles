// SPDX-License-Identifier: MIT
#include "stdafx.h"
#include "EditorService.h"
#include "ConfigurationChannel.h"

namespace HidHide::Editor
{
    namespace
    {
        using namespace Profiles;
        using namespace Profiles::Json;

        std::wstring Encode(Value const& value)
        {
            if (std::holds_alternative<std::nullptr_t>(value.data)) return L"null";
            if (auto p = std::get_if<bool>(&value.data)) return *p ? L"true" : L"false";
            if (auto p = std::get_if<std::uint64_t>(&value.data)) return std::to_wstring(*p);
            if (auto p = std::get_if<std::int64_t>(&value.data)) return std::to_wstring(*p);
            if (auto p = std::get_if<std::wstring>(&value.data)) return Escape(*p);
            std::wstring result; bool first = true;
            if (auto p = std::get_if<Array>(&value.data))
            {
                result = L"["; for (auto const& item : *p) { if (!first) result += L","; first = false; result += Encode(item); } return result + L"]";
            }
            result = L"{"; for (auto const& [key, item] : AsObject(value))
            { if (!first) result += L","; first = false; result += Escape(key) + L":" + Encode(item); } return result + L"}";
        }
        Value Decode(std::string const& bytes) { return Parser(FromUtf8(bytes)).Parse(); }
        Value Text(std::wstring text) { return Value{ std::move(text) }; }
        Value Version(SavedVersion const& version)
        { return Value{ Object{ { L"revision", Value{ version.revision } }, { L"hash", Text(version.sha256) } } }; }
        SavedVersion Expected(Value const& value)
        {
            auto const& object = AsObject(value); ExactMembers(object, { L"revision", L"hash" });
            auto hash = AsString(Required(object, L"hash"));
            if (hash.size() != 64 || !std::all_of(hash.begin(), hash.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); }))
                throw std::invalid_argument("Saved revision hash is invalid");
            return { AsUnsigned(Required(object, L"revision")), hash };
        }
        Value Outcome(CProfilesCoordinator::ApplyOutcome const& result)
        { return Value{ Object{ {L"ok", Value{true}}, {L"saved", Value{result.saved}}, {L"applied", Value{result.applied}}, {L"message", Text(result.message)} } }; }
        std::vector<std::uint8_t> Bytes(Value const& value)
        {
            auto bytes = ToUtf8(Encode(value));
            if (bytes.size() > Protocol::MaxBytes) throw std::runtime_error("Editor response exceeds the transport limit; reduce the profile catalog before retrying");
            return { bytes.begin(), bytes.end() };
        }
        std::vector<std::uint8_t> Failure(char const* text)
        { return Bytes(Value{Object{{L"ok", Value{false}}, {L"error", Text(std::wstring(text, text + strlen(text)))}}}); }
    }

    void RequireOrdinaryUser()
    {
        HANDLE token{}; if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) throw std::runtime_error("Cannot inspect editor process identity");
        auto handle = Channel::Own(token); TOKEN_ELEVATION elevation{}; DWORD length{};
        if (!::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &length) || elevation.TokenIsElevated)
            throw std::runtime_error("Open HidHide Profiles normally, not as administrator");
    }

    std::wstring FixturePipeName(std::filesystem::path const& root)
    {
        std::vector<wchar_t> temporary(32768); auto size = ::GetTempPathW(static_cast<DWORD>(temporary.size()), temporary.data());
        if (!size || size >= temporary.size()) throw std::runtime_error("Cannot locate isolated fixture storage");
        auto path = std::filesystem::absolute(root).lexically_normal();
        auto parent = std::filesystem::path(std::wstring(temporary.data(), size)).lexically_normal();
        // Only one newly allocated direct TEMP child is admitted, never an
        // arbitrary profile directory or a redirected/reparse fixture root.
        if (parent.filename().empty()) parent = parent.parent_path();
        auto name = path.filename().native();
        if (_wcsicmp(path.parent_path().c_str(), parent.c_str()) != 0 || name.find(L"HidHide-Profiles-Restart-Test-") != 0
            || !std::all_of(name.begin(), name.end(), [](wchar_t c) { return std::iswalnum(c) || c == L'-'; }))
            throw std::invalid_argument("Editor fixture requires its own direct temporary test directory");
        auto attributes = ::GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(attributes & FILE_ATTRIBUTE_DIRECTORY)))
            throw std::invalid_argument("Editor fixture root must be an ordinary directory");
        auto identity = path.native(); std::transform(identity.begin(), identity.end(), identity.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return L"\\\\.\\pipe\\HidHide.Profiles.Editor.Test." + Profiles::Sha256(Profiles::Json::ToUtf8(identity));
    }

    bool RunRequestBridge()
    {
        auto production = __argc == 2 && _wcsicmp(__wargv[1], L"--editor-request") == 0;
        auto fixture = __argc == 3 && _wcsicmp(__wargv[1], L"--editor-fixture-request") == 0;
        auto session = (__argc == 2 && _wcsicmp(__wargv[1], L"--editor-session") == 0)
            || (__argc == 3 && _wcsicmp(__wargv[1], L"--editor-fixture-session") == 0);
        if (!production && !fixture && !session) return false;
        std::wstring pipeName;
        try
        {
            RequireOrdinaryUser();
            pipeName = __argc == 3 ? FixturePipeName(__wargv[2]) : std::wstring(PipeName);
        }
        catch (std::exception const& error)
        {
            auto response = Failure(error.what()); if (session) response.push_back('\n');
            DWORD written{}; auto output = ::GetStdHandle(STD_OUTPUT_HANDLE);
            if (output && output != INVALID_HANDLE_VALUE) ::WriteFile(output, response.data(), static_cast<DWORD>(response.size()), &written, nullptr);
            return true;
        }
        auto input = ::GetStdHandle(STD_INPUT_HANDLE), output = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (!input || input == INVALID_HANDLE_VALUE || ::GetFileType(input) != FILE_TYPE_PIPE || !output || output == INVALID_HANDLE_VALUE)
            return true;
        auto exchange = [&](std::vector<std::uint8_t> request)
        {
            std::vector<std::uint8_t> response;
            try { if (request.empty()) throw std::runtime_error("Editor request is empty"); response = Channel::Exchange(std::move(request), pipeName.c_str()); }
            catch (std::exception const& error) { response = Failure(error.what()); }
            if (session) response.push_back('\n');
            DWORD written{};
            return ::WriteFile(output, response.data(), static_cast<DWORD>(response.size()), &written, nullptr) && written == response.size();
        };
        std::vector<std::uint8_t> request; std::uint8_t buffer[8192]; DWORD read{};
        while (::ReadFile(input, buffer, sizeof(buffer), &read, nullptr) && read)
        {
            for (DWORD i{}; i < read; ++i)
            {
                if (session && buffer[i] == '\n')
                {
                    if (!exchange(std::move(request))) return true;
                    request.clear(); continue;
                }
                if (request.size() >= Protocol::MaxBytes)
                {
                    auto response = Failure("Editor request is too large"); DWORD written{};
                    if (session) response.push_back('\n');
                    (void)::WriteFile(output, response.data(), static_cast<DWORD>(response.size()), &written, nullptr); return true;
                }
                request.push_back(buffer[i]);
            }
        }
        if (!session) (void)exchange(std::move(request));
        return true;
    }

    void Launch(bool engineLaunching)
    {
        RequireOrdinaryUser();
        std::vector<wchar_t> module(32768); auto length = ::GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (!length || length >= module.size()) throw std::runtime_error("Cannot locate the profiles editor");
        auto path = std::filesystem::path(std::wstring(module.data(), length)).parent_path() / L"Editor" / L"HidHideProfiles.exe";
        if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("The profiles editor is missing. Repair HidHide Profiles using its installer");
        auto command = L"\"" + path.native() + L"\"" + (engineLaunching ? L" --engine-launching" : L"");
        STARTUPINFOW startup{ sizeof(startup) }; PROCESS_INFORMATION process{};
        if (!::CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, path.parent_path().c_str(), &startup, &process))
            throw std::system_error(::GetLastError(), std::system_category(), "Launch profiles editor");
        ::CloseHandle(process.hThread); ::CloseHandle(process.hProcess);
    }

    Profiles::Json::Value Service::Snapshot()
    {
        using namespace Profiles; using namespace Json;
        // Pair each displayed value with the hash of its exact saved bytes while
        // the existing repository lease excludes cooperative writers.
        WriterLease lease(m_Application.Root());
        m_Coordinator.ReloadRepositoryIfChanged(); m_Coordinator.Tick();
        auto const& snapshot = m_Coordinator.Snapshot(); Array profiles, issues, devices;
        for (auto const& issue : m_Coordinator.Issues()) issues.push_back(Value{Object{{L"file", Text(issue.file.native())}, {L"message", Text(issue.message)}}});
        for (auto const& [id, profile] : snapshot.profiles)
        {
            // Repository diagnostics can include a synthetic unsaved recovery
            // view. Keep that view inspectable, with no writable saved version.
            auto display = profile; if (!display.revision) display.revision = 1;
            auto value = Decode(SerializeProfile(display)); auto& object = std::get<Object>(value.data);
            object[L"version"] = Value{nullptr};
            if (m_Coordinator.Issues().empty())
            {
                auto found = m_Coordinator.ProfileVersions().find(id);
                if (found == m_Coordinator.ProfileVersions().end()) throw RepositoryConflict("Profile version is unavailable. Refresh again");
                object[L"version"] = Version(found->second);
            }
            object[L"running"] = Value{m_Coordinator.IsVerifiedRunning(id)};
            object[L"missing"] = Value{m_Coordinator.IsApplicationMissing(id)};
            profiles.push_back(std::move(value));
        }
        Value settingsVersion{nullptr};
        if (m_Coordinator.Issues().empty())
        {
            settingsVersion = Version(m_Coordinator.SettingsVersion());
        }
        auto observed = m_Coordinator.ObserveEnforcement();
        struct IdentityLess
        {
            bool operator()(std::wstring const& left, std::wstring const& right) const
            { return _wcsicmp(left.c_str(), right.c_str()) < 0; }
        };
        std::set<std::wstring, IdentityLess> known;
        auto addDevice = [&](std::wstring id, std::wstring name, bool connected, std::vector<std::wstring> const& identities, std::wstring detail, std::wstring kind, std::vector<ProfileHidUsage> const& usages)
        {
            Array paths; std::size_t hidden{};
            for (auto const& identity : identities)
            {
                paths.push_back(Text(identity)); known.insert(identity);
                if (std::any_of(observed.observed.hiddenDevices.begin(), observed.observed.hiddenDevices.end(), [&](auto const& path) { return _wcsicmp(path.c_str(), identity.c_str()) == 0; })) ++hidden;
            }
            std::wstring current = !observed.observedKnown ? L"Unknown" : !observed.observed.hidingEnabled || !hidden ? L"Visible" : hidden == identities.size() ? L"Hidden" : L"Mixed";
            Array hidUsages;
            for (auto const& usage : usages)
                hidUsages.push_back(Value{Object{{L"known",Value{usage.known}}, {L"page",Value{static_cast<std::uint64_t>(usage.page)}}, {L"usage",Value{static_cast<std::uint64_t>(usage.usage)}}}});
            devices.push_back(Value{Object{{L"id", Text(std::move(id))}, {L"name", Text(std::move(name))}, {L"detail", Text(std::move(detail))},
                {L"connected", Value{connected}}, {L"identities", Value{std::move(paths)}}, {L"current", Text(current)}, {L"kind", Text(std::move(kind))}, {L"hidUsages",Value{std::move(hidUsages)}}}});
        };
        std::wstring deviceError;
        try { for (auto const& device : m_Devices.Enumerate()) addDevice(device.identity, device.friendly, device.connected, device.policyIdentities, device.identity, device.kind, device.hidUsages); }
        catch (std::exception const& error) { deviceError.assign(error.what(), error.what() + strlen(error.what())); }
        for (auto const& [id, profile] : snapshot.profiles)
        {
            (void)id; for (auto const& rule : profile.rules) if (!known.count(rule.identity))
                addDevice(rule.identity, rule.friendlyName.empty() ? rule.identity : rule.friendlyName, false, {rule.identity}, L"Remembered exact path", L"unknown", {});
        }
        auto displaySettings = snapshot.settings; if (!displaySettings.revision) displaySettings.revision = 1;
        if (!IsStableId(displaySettings.selectedGlobalId) && !snapshot.profiles.empty()) displaySettings.selectedGlobalId = snapshot.profiles.begin()->first;
        Array runningOrder; for (auto const& id : m_Coordinator.RunningOrder()) runningOrder.push_back(Text(id));
        return Value{Object{{L"profiles", Value{std::move(profiles)}}, {L"settings", Decode(SerializeSettings(displaySettings))}, {L"settingsVersion", std::move(settingsVersion)},
            {L"activeId", Text(m_Coordinator.EffectiveSelection().profileId)}, {L"verified", Value{m_Coordinator.EffectiveSelectionVerified()}},
            {L"launchedId", Text(m_Coordinator.LaunchUncertain() ? m_Coordinator.LaunchedProfileId() : L"")},
            {L"runningOrder", Value{std::move(runningOrder)}}, {L"manualMaskId", Text(m_Coordinator.ManualMaskId())},
            {L"requestedId", Text(m_Coordinator.RequestedSelection().profileId)},
            {L"status", Text(m_Coordinator.Status())}, {L"conflict", Value{m_Coordinator.HasDriverConflict()}}, {L"repositoryIssues", Value{std::move(issues)}},
            {L"devices", Value{std::move(devices)}}, {L"deviceError", Text(deviceError)}, {L"repositoryPath", Text(m_Application.Root().native())}, {L"engineInstance", Text(m_Instance)},
            {L"adoptionAwaitingSave", Value{m_Coordinator.AdoptionAwaitingSave()}},
            {L"adoptedSettings", m_Coordinator.AdoptionAwaitingSave() && m_AdoptedSettings ? Decode(SerializeSettings(*m_AdoptedSettings)) : Value{nullptr}}}};
    }

    Profiles::Json::Value Service::Execute(Profiles::Json::Object const& request)
    {
        using namespace Profiles; using namespace Json;
        auto command = AsString(Required(request, L"command"));
        if (command == L"snapshot") return Value{Object{{L"ok", Value{true}}, {L"snapshot", Snapshot()}}};
        if (command == L"editor-state")
        {
            auto pid = AsUnsigned(Required(request,L"pid"));
            if (!pid || pid > MAXDWORD) throw std::invalid_argument("Editor process id is invalid");
            auto process = Channel::Own(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid)));
            HANDLE token{}; if (!::OpenProcessToken(process.get(), TOKEN_QUERY, &token)) throw std::runtime_error("Cannot authenticate editor process");
            auto identity = Channel::Own(token); DWORD size{}, session{}, currentSession{}; TOKEN_ELEVATION elevation{};
            if (Channel::TokenSid(token) != Channel::CurrentSid()
                || !::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) || elevation.TokenIsElevated
                || !::ProcessIdToSessionId(static_cast<DWORD>(pid), &session) || !::ProcessIdToSessionId(::GetCurrentProcessId(), &currentSession)
                || !session || session != currentSession || ::WaitForSingleObject(process.get(),0) != WAIT_TIMEOUT)
                throw std::runtime_error("Editor must belong to the same ordinary user and interactive session");
            m_EditorProcess = std::move(process);
            return Value{Object{{L"ok",Value{true}}}};
        }
        auto profileValue = [&](wchar_t const* field) { return ParseProfile(ToUtf8(Encode(Required(request, field)))); };
        auto settingsValue = [&] { return ParseSettings(ToUtf8(Encode(Required(request, L"settings")))); };
        auto expectedSettings = [&] { return Expected(Required(request, L"expectedSettings")); };
        auto publish = [&](ProfileApplicationService::SavedChange change, wchar_t const* action)
        { return Outcome(m_Coordinator.PublishSaved(std::move(change.loaded), change.version, action, change.cleanupPending)); };
        if (command == L"mask" || command == L"automatic")
        {
            WriterLease lease(m_Application.Root());
            m_Coordinator.ReloadRepositoryIfChanged(); m_Coordinator.Tick();
            auto expected = expectedSettings(); auto current = m_Application.SettingsVersion();
            if (expected.revision != current.revision || expected.sha256 != current.sha256)
                throw RepositoryConflict("Saved settings changed; refresh before selecting a mask");
            std::wstring id;
            if (command == L"mask")
            {
                id = AsString(Required(request, L"id"));
                auto profileExpected = Expected(Required(request, L"expected")); auto profileCurrent = m_Application.Version(id);
                if (profileExpected.revision != profileCurrent.revision || profileExpected.sha256 != profileCurrent.sha256)
                    throw RepositoryConflict("Saved profile changed; refresh before selecting its mask");
            }
            return Outcome(m_Coordinator.SelectMask(id));
        }
        if (command == L"launch")
        {
            WriterLease lease(m_Application.Root());
            m_Coordinator.ReloadRepositoryIfChanged(); m_Coordinator.Tick();
            auto id = AsString(Required(request, L"id"));
            auto expected = Expected(Required(request, L"expected"));
            auto current = m_Application.Version(id);
            auto settingsExpected = expectedSettings(); auto settingsCurrent = m_Application.SettingsVersion();
            if (expected.revision != current.revision || expected.sha256 != current.sha256
                || settingsExpected.revision != settingsCurrent.revision || settingsExpected.sha256 != settingsCurrent.sha256)
                throw RepositoryConflict("Saved profile or Allowed apps changed; refresh before launching");
            auto previousSettings = m_Coordinator.Snapshot().settings;
            std::optional<SavedVersion> launchSettingsVersion;
            bool modeSaveAttempted{};
            // The coordinator invokes this only after suspended creation and
            // identity verification, and rolls back only after confirmed abort.
            // Keep both writes on the ordinary CAS application-service path.
            auto changeMode = [&](bool automatic)
            {
                if (previousSettings.mode == Mode::Automatic) return;
                if (!automatic && !launchSettingsVersion)
                {
                    if (modeSaveAttempted)
                    {
                        auto live = m_Application.SettingsVersion();
                        if (live.revision != settingsCurrent.revision || live.sha256 != settingsCurrent.sha256)
                            throw std::runtime_error("The failed mode save has an unknown durable outcome; review saved settings");
                    }
                    return;
                }
                auto settings = previousSettings;
                if (automatic) settings.mode = Mode::Automatic;
                if (automatic) modeSaveAttempted = true;
                auto changed = m_Application.ApplySettings(settings, automatic ? settingsCurrent : *launchSettingsVersion);
                if (automatic) launchSettingsVersion = changed.version;
                (void)m_Coordinator.PublishSaved(std::move(changed.loaded), changed.version,
                    automatic ? L"Automatic mode" : L"Previous launch mode", changed.cleanupPending);
                auto const& published = m_Coordinator.SettingsVersion();
                if (published.revision != changed.version.revision || published.sha256 != changed.version.sha256
                    || m_Coordinator.Snapshot().settings.mode != settings.mode)
                    throw std::runtime_error("Saved launch mode could not be published; review saved settings");
                if (!automatic) launchSettingsVersion.reset();
            };
            auto message = m_Coordinator.LaunchSavedProfile(id, changeMode);
            return Value{Object{{L"ok", Value{true}}, {L"message", Text(message)}}};
        }
        if (m_Coordinator.LaunchUncertain() && (command == L"apply" || command == L"settings" || command == L"delete"
            || command == L"restore" || command == L"adopt" || command == L"abandon-adoption"))
            throw std::runtime_error("Resolve the uncertain application launch before changing profile settings");
        if (command == L"adopt")
        {
            WriterLease lease(m_Application.Root()); auto expected = expectedSettings(); auto live = m_Application.SettingsVersion();
            if (live.revision != expected.revision || live.sha256 != expected.sha256) throw RepositoryConflict("Settings changed during driver conflict confirmation. Refresh before retrying");
            auto allowed = m_Coordinator.AdoptExternalState();
            if (allowed == m_Coordinator.Snapshot().settings.allowedApplications)
                return Outcome(m_Coordinator.CompleteAdoptionWithoutSettingsChange(allowed));
            m_AdoptedSettings = m_Coordinator.Snapshot().settings; m_AdoptedSettings->allowedApplications = std::move(allowed);
            return Value{Object{{L"ok",Value{true}}, {L"settings", Decode(SerializeSettings(*m_AdoptedSettings))}, {L"needsApply",Value{true}},
                {L"message",Text(L"Current baseline accepted. Review the observed Allowed apps, then Apply or Discard the staged settings")}}};
        }
        if (command == L"abandon-adoption") { auto outcome = m_Coordinator.AbandonAdoptionDraft(); m_AdoptedSettings.reset(); return Outcome(outcome); }
        if (command == L"apply")
        {
            auto const& expected = Required(request, L"expected");
            auto version = std::holds_alternative<std::nullptr_t>(expected.data) ? std::nullopt : std::optional<SavedVersion>(Expected(expected));
            return publish(m_Application.Apply(profileValue(L"profile"), version, settingsValue(), expectedSettings()), L"Profile");
        }
        if (command == L"settings") return publish(m_Application.ApplySettings(settingsValue(), expectedSettings()), L"Settings");
        if (command == L"delete") return publish(m_Application.Delete(AsString(Required(request,L"id")), Expected(Required(request,L"expected")), settingsValue(), expectedSettings()), L"Profile deletion");
        if (command == L"retry") return Outcome(m_Coordinator.RetryActivation());
        if (command == L"restore") return publish(m_Application.RestoreBackup(AsString(Required(request,L"path"))), L"Backup restore");
        if (command == L"backup") { m_Application.Backup(AsString(Required(request,L"path"))); return Value{Object{{L"ok",Value{true}}, {L"message",Text(L"Profile backup created")}}}; }
        if (command == L"export") { m_Application.ExportProfile(AsString(Required(request,L"id")), AsString(Required(request,L"path"))); return Value{Object{{L"ok",Value{true}}, {L"message",Text(L"Saved profile exported")}}}; }
        if (command == L"new" || command == L"import")
        {
            Profile profile;
            if (command == L"import") { profile = ParseProfile(ReadBytes(AsString(Required(request,L"path")))); profile.enabled = false; }
            else
            {
                auto kind = AsString(Required(request,L"kind"));
                if (kind != L"global" && kind != L"application") throw std::invalid_argument("Unknown profile kind");
                profile.kind = kind == L"application" ? Kind::Application : Kind::Global;
                profile.name = AsString(Required(request,L"name"));
                if (profile.kind == Kind::Application) profile.executable = NormalizeExecutable(AsString(Required(request,L"executable")));
            }
            profile.id = NewStableId(); profile.revision = 1; // Detached wire draft; Apply assigns the first durable revision.
            return Value{Object{{L"ok",Value{true}}, {L"profile",Decode(SerializeProfile(profile))}}};
        }
        throw std::invalid_argument("Unknown editor command");
    }

    std::vector<std::uint8_t> Service::Handle(std::vector<std::uint8_t> const& request) noexcept
    {
        try
        {
            if (request.empty() || request.size() > Protocol::MaxBytes) throw std::invalid_argument("Invalid editor request size");
            auto value = Decode(std::string(request.begin(), request.end()));
            return Bytes(Execute(AsObject(value)));
        }
        catch (std::exception const& error) { return Failure(error.what()); }
        catch (...) { return Failure("Editor operation failed; refresh before retrying"); }
    }
}
