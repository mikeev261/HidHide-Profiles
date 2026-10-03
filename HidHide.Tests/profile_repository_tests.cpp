// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "ProfileRepository.h"
#include "ProfileApplicationService.h"
#include "ProfileRecovery.h"
#include "ProfileProcessLifetime.h"
#include "FilterDriverProxy.h"
#include <future>
#include <iostream>

using namespace HidHide::Profiles;

namespace
{
    struct IndependentProfileDocument
    {
        std::string id, executable; std::uint64_t revision{};
        std::vector<std::pair<std::string, std::string>> rules;
    };

    class IndependentJsonReader
    {
    public:
        explicit IndependentJsonReader(std::string const& bytes) : m_Bytes(bytes) {}
        IndependentProfileDocument Profile()
        {
            IndependentProfileDocument result; std::set<std::string> members; ObjectBegin();
            while (!ObjectEnd())
            {
                auto key = String(); if (!members.emplace(key).second) throw std::runtime_error("duplicate JSON member"); Colon();
                if (key == "id") result.id = String();
                else if (key == "revision") result.revision = Unsigned();
                else if (key == "executablePath") result.executable = String();
                else if (key == "deviceRules") result.rules = Rules();
                else Skip();
                SeparatorOrEnd('}');
            }
            Space(); if (m_Pos != m_Bytes.size() || result.id.empty()) throw std::runtime_error("invalid independent profile JSON");
            return result;
        }
    private:
        void Space() { while (m_Pos < m_Bytes.size() && (m_Bytes[m_Pos] == ' ' || m_Bytes[m_Pos] == '\r' || m_Bytes[m_Pos] == '\n' || m_Bytes[m_Pos] == '\t')) ++m_Pos; }
        char Peek() { Space(); if (m_Pos >= m_Bytes.size()) throw std::runtime_error("unexpected JSON end"); return m_Bytes[m_Pos]; }
        void Take(char expected) { Space(); if (m_Pos >= m_Bytes.size() || m_Bytes[m_Pos++] != expected) throw std::runtime_error("unexpected JSON token"); }
        void ObjectBegin() { Take('{'); m_First = true; }
        bool ObjectEnd() { Space(); if (Peek() == '}') { ++m_Pos; return true; } if (!m_First) Take(','); m_First = false; return false; }
        void Colon() { Take(':'); }
        void SeparatorOrEnd(char) { /* consumed by ObjectEnd/array loop */ }
        std::string String()
        {
            Take('"'); std::string result;
            while (m_Pos < m_Bytes.size())
            {
                auto c = m_Bytes[m_Pos++]; if (c == '"') return result;
                if (static_cast<unsigned char>(c) < 0x20) throw std::runtime_error("invalid JSON string");
                if (c != '\\') { result.push_back(c); continue; }
                if (m_Pos >= m_Bytes.size()) throw std::runtime_error("invalid JSON escape"); auto escaped = m_Bytes[m_Pos++];
                if (escaped == '"' || escaped == '\\' || escaped == '/') result.push_back(escaped);
                else if (escaped == 'b') result.push_back('\b'); else if (escaped == 'f') result.push_back('\f'); else if (escaped == 'n') result.push_back('\n'); else if (escaped == 'r') result.push_back('\r'); else if (escaped == 't') result.push_back('\t');
                else if (escaped == 'u') { for (int i{}; i < 4; ++i) { if (m_Pos >= m_Bytes.size() || !std::isxdigit(static_cast<unsigned char>(m_Bytes[m_Pos++]))) throw std::runtime_error("invalid unicode escape"); } result.push_back('?'); }
                else throw std::runtime_error("invalid JSON escape");
            }
            throw std::runtime_error("unterminated JSON string");
        }
        std::uint64_t Unsigned()
        {
            Space(); auto start = m_Pos; while (m_Pos < m_Bytes.size() && m_Bytes[m_Pos] >= '0' && m_Bytes[m_Pos] <= '9') ++m_Pos;
            if (start == m_Pos) throw std::runtime_error("expected unsigned JSON number"); return std::stoull(m_Bytes.substr(start, m_Pos - start));
        }
        void Literal(char const* text)
        {
            Space(); auto length = std::strlen(text); if (m_Bytes.compare(m_Pos, length, text) != 0) throw std::runtime_error("invalid JSON literal"); m_Pos += length;
        }
        void Skip()
        {
            auto c = Peek(); if (c == '"') { (void)String(); return; }
            if (c == '{') { Take('{'); bool first = true; while (Peek() != '}') { if (!first) Take(','); first = false; (void)String(); Colon(); Skip(); } Take('}'); return; }
            if (c == '[') { Take('['); bool first = true; while (Peek() != ']') { if (!first) Take(','); first = false; Skip(); } Take(']'); return; }
            if (c >= '0' && c <= '9') { (void)Unsigned(); return; }
            if (c == '-') { ++m_Pos; (void)Unsigned(); return; }
            if (c == 't') { Literal("true"); return; } if (c == 'f') { Literal("false"); return; } if (c == 'n') { Literal("null"); return; }
            throw std::runtime_error("unsupported JSON value");
        }
        std::vector<std::pair<std::string, std::string>> Rules()
        {
            std::vector<std::pair<std::string, std::string>> result; Take('['); bool first = true;
            while (Peek() != ']')
            {
                if (!first) Take(','); first = false; Take('{'); bool memberFirst = true; std::string identity, visibility; std::set<std::string> members;
                while (Peek() != '}')
                {
                    if (!memberFirst) Take(','); memberFirst = false; auto key = String(); if (!members.emplace(key).second) throw std::runtime_error("duplicate rule member"); Colon();
                    if (key == "identity") identity = String(); else if (key == "visibility") visibility = String(); else Skip();
                }
                Take('}'); if (identity.empty() || visibility.empty()) throw std::runtime_error("incomplete rule"); result.emplace_back(identity, visibility);
            }
            Take(']'); return result;
        }
        std::string const& m_Bytes; std::size_t m_Pos{}; bool m_First{};
    };

    Profile App(std::wstring id, std::wstring name, int priority, std::wstring path, std::vector<DeviceRule> rules = {})
    {
        return { std::move(id), 1, std::move(name), Kind::Application, true, priority, NormalizeExecutable(path), std::move(rules) };
    }
    Profile Global(std::wstring id, std::wstring name = L"Default")
    {
        return { std::move(id), 1, std::move(name), Kind::Global, true, 0, {}, {} };
    }
    std::filesystem::path TempRoot()
    {
        wchar_t root[MAX_PATH]{}; if (!::GetTempPathW(MAX_PATH, root)) throw std::runtime_error("temp path");
        return std::filesystem::path(root) / (L"HidHide-Profiles-Test-" + NewStableId());
    }
    struct TempDirectory
    {
        std::filesystem::path path{ TempRoot() };
        ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    };
    std::filesystem::path ClientExecutable()
    {
        wchar_t module[32768]{}; auto length = ::GetModuleFileNameW(nullptr, module, static_cast<DWORD>(std::size(module)));
        if (!length || length >= std::size(module)) throw std::runtime_error("test executable path");
        return std::filesystem::path(std::wstring(module, length)).parent_path() / L"HidHideClient.exe";
    }
    struct ChildProcess
    {
        PROCESS_INFORMATION process{};
        ChildProcess() = default; ChildProcess(ChildProcess const&) = delete; ChildProcess& operator=(ChildProcess const&) = delete;
        ChildProcess(ChildProcess&& other) noexcept : process(other.process) { other.process = {}; }
        ChildProcess& operator=(ChildProcess&& other) noexcept
        {
            if (this != &other) { process = other.process; other.process = {}; } return *this;
        }
        ~ChildProcess() { if (process.hProcess) { ::TerminateProcess(process.hProcess, 99); ::WaitForSingleObject(process.hProcess, 5000); ::CloseHandle(process.hProcess); } if (process.hThread) ::CloseHandle(process.hThread); }
        void TerminateAndVerify()
        {
            ASSERT_TRUE(::TerminateProcess(process.hProcess, 0)); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(process.hProcess, 5000));
            DWORD code{ STILL_ACTIVE }; ASSERT_TRUE(::GetExitCodeProcess(process.hProcess, &code)); ASSERT_NE(STILL_ACTIVE, code);
            ::CloseHandle(process.hProcess); process.hProcess = nullptr; ::CloseHandle(process.hThread); process.hThread = nullptr;
        }
        void WaitAndVerifyExit()
        {
            ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(process.hProcess, 10000)); DWORD code{ STILL_ACTIVE };
            ASSERT_TRUE(::GetExitCodeProcess(process.hProcess, &code)); ASSERT_NE(STILL_ACTIVE, code);
            ::CloseHandle(process.hProcess); process.hProcess = nullptr; ::CloseHandle(process.hThread); process.hThread = nullptr;
        }
    };
    ChildProcess StartRestartWorker(wchar_t const* phase, std::filesystem::path const& root, std::wstring const& ready,
        std::wstring const& command, std::wstring const& completed)
    {
        auto executable = ClientExecutable(); std::wstring line = L"\"" + executable.native() + L"\" --profile-restart-test " + phase + L" \"" + root.native() + L"\" " + ready + L" " + command + L" " + completed;
        std::vector<wchar_t> mutableLine(line.begin(), line.end()); mutableLine.push_back(0); STARTUPINFOW startup{}; startup.cb = sizeof(startup); ChildProcess child;
        if (!::CreateProcessW(executable.c_str(), mutableLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &child.process))
            throw std::system_error(::GetLastError(), std::system_category(), "launch production client");
        return child;
    }
    std::map<std::wstring, std::string> RepositoryBytes(std::filesystem::path const& root)
    {
        std::map<std::wstring, std::string> result;
        for (auto const& entry : std::filesystem::directory_iterator(root)) if (entry.is_regular_file() && entry.path().extension() == L".json") result.emplace(entry.path().filename().native(), ReadBytes(entry.path()));
        return result;
    }
    std::map<std::wstring, std::string> AllRepositoryFileBytes(std::filesystem::path const& root)
    {
        std::map<std::wstring, std::string> result;
        for (auto const& entry : std::filesystem::directory_iterator(root)) if (entry.is_regular_file()) result.emplace(entry.path().filename().native(), ReadBytes(entry.path()));
        return result;
    }
    HWND FindProcessWindow(DWORD process)
    {
        struct State { DWORD process; HWND window{}; } state{ process };
        ::EnumWindows([](HWND window, LPARAM parameter)
        {
            auto& state = *reinterpret_cast<State*>(parameter); DWORD owner{}; ::GetWindowThreadProcessId(window, &owner);
            if (owner == state.process && ::IsWindowVisible(window)) { state.window = window; return FALSE; } return TRUE;
        }, reinterpret_cast<LPARAM>(&state));
        return state.window;
    }

}

TEST(ProfilePolicy, RestartCreationTimeExitFallbackManualAndPause)
{
    auto globalId = L"00000000-0000-0000-0000-000000000001";
    Snapshot snapshot; snapshot.settings = { 1, globalId, Mode::Automatic, false, true, {} };
    snapshot.profiles.emplace(globalId, Global(globalId));
    auto low = App(L"00000000-0000-0000-0000-000000000010", L"Low", 4, L"C:\\Games\\F1_25.exe");
    auto tiedFirst = App(L"00000000-0000-0000-0000-000000000002", L"Tie first", 8, L"D:\\Games\\LMU.exe");
    auto tiedSecond = App(L"00000000-0000-0000-0000-000000000003", L"Tie second", 8, L"E:\\Games\\AC.exe");
    snapshot.profiles.emplace(low.id, low); snapshot.profiles.emplace(tiedFirst.id, tiedFirst); snapshot.profiles.emplace(tiedSecond.id, tiedSecond);
    std::vector<ProcessObservation> processes{
        { 1, 101, L"F1_25.exe", L"C:\\Games\\F1_25.exe", true },
        { 2, 102, L"LMU.exe", L"D:\\Games\\LMU.exe", true },
        { 3, 103, L"AC.exe", L"E:\\Games\\AC.exe", true } };
    EXPECT_EQ(tiedSecond.id, SelectWinner(snapshot, processes).profileId);
    processes.erase(processes.begin() + 1); EXPECT_EQ(tiedSecond.id, SelectWinner(snapshot, processes).profileId);
    processes.clear(); EXPECT_EQ(globalId, SelectWinner(snapshot, processes).profileId);
    snapshot.settings.mode = Mode::UseGlobal; EXPECT_EQ(SelectionReason::ManualGlobal, SelectWinner(snapshot, processes).reason);
    snapshot.settings.paused = true; EXPECT_EQ(SelectionReason::Paused, SelectWinner(snapshot, processes).reason);
}

TEST(ProfilePolicy, InaccessibleWrongPathAndMultipleInstancesNeverInventWinner)
{
    auto globalId = L"00000000-0000-0000-0000-000000000001"; auto appId = L"00000000-0000-0000-0000-000000000002";
    Snapshot snapshot; snapshot.settings = { 1, globalId, Mode::Automatic, false, true, {} };
    snapshot.profiles.emplace(globalId, Global(globalId)); snapshot.profiles.emplace(appId, App(appId, L"F1", 1, L"C:\\Games\\F1_25.exe"));
    EXPECT_EQ(SelectionReason::DetectionUncertain, SelectWinner(snapshot, { { 5, 1, L"F1_25.exe", {}, false } }).reason);
    EXPECT_EQ(globalId, SelectWinner(snapshot, { { 5, 1, L"F1_25.exe", L"D:\\Other\\F1_25.exe", true } }).profileId);
    EXPECT_EQ(appId, SelectWinner(snapshot, { { 5, 1, L"F1_25.exe", L"D:\\Other\\F1_25.exe", true }, { 6, 2, L"F1_25.exe", L"C:\\Games\\F1_25.exe", true } }).profileId);
}

TEST(ProfilePolicy, ProcessPathCacheRejectsPidReuse)
{
    ProcessIdentityCache cache; ProcessObservation old{ 42, 100, L"F1_25.exe", L"C:\\Games\\F1_25.exe", true }; cache.Remember(old);
    ASSERT_TRUE(cache.Find(42, 100)); EXPECT_FALSE(cache.Find(42, 101));
    cache.Retain({ { 42, 101, L"other.exe", L"D:\\other.exe", true } }); EXPECT_FALSE(cache.Find(42, 100));
}

TEST(ProfilePolicy, NegativeApplicationPriorityIsValid)
{
    auto globalId = L"00000000-0000-0000-0000-000000000001"; Snapshot snapshot;
    snapshot.settings = { 1, globalId, Mode::Automatic, false, true, {} }; snapshot.profiles.emplace(globalId, Global(globalId));
    auto app = App(L"00000000-0000-0000-0000-000000000002", L"Negative", -100000, L"C:\\negative.exe"); snapshot.profiles.emplace(app.id, app);
    EXPECT_NO_THROW(Validate(snapshot)); snapshot.profiles.at(app.id).priority = -100001; EXPECT_THROW(Validate(snapshot), std::invalid_argument);
}

TEST(ProfileJson, StrictUnicodeRoundTripAndDisconnectedRule)
{
    auto profile = App(L"00000000-0000-0000-0000-000000000002", L"F1 — レース", 10, L"C:\\Games\\F1_25.exe",
        { { L"HID\\CONNECTED", L"Pedals é", Visibility::Hidden }, { L"HID\\DISCONNECTED", L"Remembered", Visibility::Visible } });
    auto bytes = SerializeProfile(profile); EXPECT_EQ(profile, ParseProfile(bytes));
    EXPECT_THROW(ParseProfile(bytes.substr(0, bytes.size() - 2)), std::runtime_error);
    auto unknown = bytes; auto pos = unknown.find("\"schemaVersion\": 1"); unknown.replace(pos, 18, "\"schemaVersion\": 2"); EXPECT_THROW(ParseProfile(unknown), std::runtime_error);
    auto duplicate = bytes; duplicate.insert(2, "\"name\": \"duplicate\",\n  "); EXPECT_THROW(ParseProfile(duplicate), std::runtime_error);
    EXPECT_THROW(ParseProfile(std::string("{\"x\":\"") + static_cast<char>(0xff) + "\"}"), std::runtime_error);
    auto nul = bytes; auto name = nul.find("F1 "); ASSERT_NE(std::string::npos, name); nul.replace(name, 3, "F1\\u0000");
    EXPECT_THROW(ParseProfile(nul), std::runtime_error);
    std::string nested(66, '['); nested += "0"; nested.append(66, ']'); EXPECT_THROW(Json::Parser(Json::FromUtf8(nested)).Parse(), std::runtime_error);
}

TEST(ProfileJson, CommonSchemaUsesNeutralTriggerFieldsForGlobalProfiles)
{
    auto profile = Global(NewStableId(), L"Common schema Global"); auto bytes = SerializeProfile(profile);
    EXPECT_NE(std::string::npos, bytes.find("\"priority\": 0"));
    EXPECT_NE(std::string::npos, bytes.find("\"executablePath\": \"\""));
    auto parsed = ParseProfile(bytes); EXPECT_EQ(Kind::Global, parsed.kind); EXPECT_EQ(0, parsed.priority); EXPECT_TRUE(parsed.executable.empty());

    auto withoutPriority = bytes; auto priority = withoutPriority.find("  \"priority\": 0,\n"); ASSERT_NE(std::string::npos, priority);
    withoutPriority.erase(priority, std::strlen("  \"priority\": 0,\n")); EXPECT_THROW(ParseProfile(withoutPriority), std::runtime_error);
    auto withoutExecutable = bytes; auto executable = withoutExecutable.find("  \"executablePath\": \"\",\n"); ASSERT_NE(std::string::npos, executable);
    withoutExecutable.erase(executable, std::strlen("  \"executablePath\": \"\",\n")); EXPECT_THROW(ParseProfile(withoutExecutable), std::runtime_error);
}

TEST(ProfileRepository, FreshRepositoryIsAllVisibleAndJsonAuthoritative)
{
    TempDirectory temp; ProfileRepository repository(temp.path); auto loaded = repository.OpenOrCreate({ L"C:\\Feeders\\Gremlin.exe" });
    ASSERT_EQ(1u, loaded.snapshot.profiles.size()); auto const& profile = loaded.snapshot.profiles.begin()->second;
    EXPECT_EQ(Kind::Global, profile.kind); EXPECT_EQ(L"Default", profile.name); EXPECT_TRUE(profile.rules.empty());
    EXPECT_EQ(profile.id, loaded.snapshot.settings.selectedGlobalId); EXPECT_TRUE(std::filesystem::exists(temp.path / L"settings.json"));
    EXPECT_TRUE(loaded.snapshot.settings.allowedApplications.count(NormalizeExecutable(L"C:\\Feeders\\Gremlin.exe")));
}

TEST(ProfileRepository, FreshRepositoryValidatesObservedAllowedAppsBeforeWriting)
{
    TempDirectory temp; std::set<std::filesystem::path> oversized;
    for (std::size_t index{}; index <= MaxRulesPerProfile; ++index) oversized.emplace(L"C:\\Feeders\\reader-" + std::to_wstring(index) + L".exe");
    ProfileRepository repository(temp.path); EXPECT_THROW(repository.OpenOrCreate(std::move(oversized)), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(temp.path / L"settings.json"));
    for (auto const& entry : std::filesystem::directory_iterator(temp.path)) EXPECT_FALSE(entry.path().extension() == L".json");
}

TEST(ProfileRepository, UnknownInitialAllowedAppsCannotCreateCatalog)
{
    TempDirectory temp; ProfileRepository repository(temp.path);
    auto blocked = repository.OpenOrCreateObserved(std::nullopt); EXPECT_FALSE(blocked.issues.empty());
    EXPECT_TRUE(AllRepositoryFileBytes(temp.path).empty());
}

TEST(ProfileRepository, DraftDoesNotWriteApplySurvivesRepositoryReconstruction)
{
    TempDirectory temp; ProfileRepository repository(temp.path); repository.OpenOrCreate();
    auto id = NewStableId(); auto draft = App(id, L"F1 25", 25, L"C:\\Games\\F1_25.exe",
        { { L"HID\\PEDALS", L"Pedals", Visibility::Hidden }, { L"HID\\REMEMBERED", L"Offline", Visibility::Hidden } });
    EXPECT_FALSE(std::filesystem::exists(temp.path / (id + L".json")));
    auto saved = repository.Apply(draft, std::nullopt); EXPECT_EQ(1u, saved.revision);
    RepositoryView fresh(temp.path); auto loaded = fresh.Load(); auto found = loaded.snapshot.profiles.at(id);
    EXPECT_EQ(NormalizeExecutable(L"C:\\Games\\F1_25.exe"), found.executable); ASSERT_EQ(2u, found.rules.size());
    EXPECT_EQ(Visibility::Hidden, found.rules[1].visibility);
}

TEST(ProfileRepository, ExternalReplacementConflictsAndBytesArePreserved)
{
    TempDirectory temp; ProfileRepository repository(temp.path); repository.OpenOrCreate(); auto id = NewStableId();
    auto draft = App(id, L"F1", 1, L"C:\\F1.exe"); auto expected = repository.Apply(draft, std::nullopt);
    auto path = temp.path / (id + L".json"); auto external = ParseProfile(ReadBytes(path)); external.name = L"External"; external.revision++;
    auto bytes = SerializeProfile(external); { std::ofstream out(path, std::ios::binary | std::ios::trunc); out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
    draft.name = L"Draft"; EXPECT_THROW(repository.Apply(draft, expected), RepositoryConflict); EXPECT_EQ(bytes, ReadBytes(path));
}

TEST(ProfileRepository, PartialMultiFileApplyRecoversCompleteOldSet)
{
    TempDirectory temp; ProfileRepository repository(temp.path); auto loaded = repository.OpenOrCreate(); auto globalId = loaded.snapshot.settings.selectedGlobalId;
    auto version = repository.Version(globalId); auto draft = loaded.snapshot.profiles.at(globalId); draft.name = L"Changed";
    auto settings = loaded.snapshot.settings; settings.mode = Mode::UseGlobal;
    ProfileRepository failing(temp.path, [](CommitBoundary boundary, std::size_t index)
    { if (boundary == CommitBoundary::TargetReplaced && index == 0) throw std::runtime_error("injected crash"); });
    EXPECT_THROW(failing.Apply(draft, version, settings), std::runtime_error);
    ProfileRepository recovered(temp.path); recovered.Recover(); auto after = recovered.Load();
    EXPECT_EQ(L"Default", after.snapshot.profiles.at(globalId).name); EXPECT_EQ(Mode::Automatic, after.snapshot.settings.mode);
}

TEST(ProfileRepository, CorruptUnknownAndFilenameMismatchArePreservedAndReported)
{
    TempDirectory temp; ProfileRepository repository(temp.path); repository.OpenOrCreate();
    std::filesystem::path corrupt = temp.path / L"00000000-0000-0000-0000-000000000099.json"; { std::ofstream out(corrupt); out << "{ truncated"; }
    auto loaded = repository.Load(); ASSERT_EQ(1u, loaded.issues.size()); EXPECT_TRUE(std::filesystem::exists(corrupt)); EXPECT_EQ("{ truncated", ReadBytes(corrupt));
    auto before = RepositoryBytes(temp.path); auto settings = loaded.snapshot.settings; settings.paused = true;
    EXPECT_THROW(repository.ApplySettings(settings, repository.SettingsVersion()), RepositoryConflict); EXPECT_EQ(before, RepositoryBytes(temp.path));
}

TEST(ProfileRepository, RepositoryAndRestoreEnumerationAreBoundedAtDeclaredLimits)
{
    auto makeId = [](unsigned value)
    {
        wchar_t id[37]{}; swprintf_s(id, L"30000000-0000-0000-0000-%012x", value); return std::wstring(id);
    };
    TempDirectory temp; ProfileRepository repository(temp.path); auto original = repository.OpenOrCreate();
    for (unsigned index{ 1 }; index <= MaxProfiles + 8; ++index)
    {
        auto profile = App(makeId(index), L"Bounded", 0, L"C:\\Bounded\\Game.exe");
        std::ofstream file(temp.path / (profile.id + L".json"), std::ios::binary); auto bytes = SerializeProfile(profile);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    auto loaded = repository.Load(); EXPECT_LE(loaded.snapshot.profiles.size(), MaxProfiles); EXPECT_LE(loaded.issues.size(), MaxRepositoryIssues);
    EXPECT_TRUE(std::any_of(loaded.issues.begin(), loaded.issues.end(), [](auto const& issue) { return issue.message.find(L"supported number") != std::wstring::npos; }));
    auto overLimitBytes = AllRepositoryFileBytes(temp.path); auto settingsDraft = loaded.snapshot.settings; settingsDraft.paused = true;
    EXPECT_ANY_THROW(repository.ApplySettings(settingsDraft, repository.SettingsVersion())); EXPECT_EQ(overLimitBytes, AllRepositoryFileBytes(temp.path));

    auto backup = std::filesystem::path(temp.path.native() + L"-oversized"); std::filesystem::create_directories(backup);
    auto global = Global(makeId(10000), L"Backup Global"); Snapshot candidate; candidate.profiles.emplace(global.id, global);
    for (unsigned index{ 1 }; index <= MaxProfiles; ++index)
    {
        auto profile = App(makeId(10000 + index), L"Backup", 0, L"C:\\Backup\\Game.exe"); candidate.profiles.emplace(profile.id, profile);
    }
    candidate.settings = { 1, global.id, Mode::Automatic, false, true, {} };
    for (auto const& [id, profile] : candidate.profiles)
    {
        std::ofstream file(backup / (id + L".json"), std::ios::binary); auto bytes = SerializeProfile(profile);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    { std::ofstream file(backup / L"settings.json", std::ios::binary); auto bytes = SerializeSettings(candidate.settings); file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
    std::wostringstream manifest; manifest << L"{\n  \"schemaVersion\": 1,\n  \"format\": \"HidHide Profiles portable backup\",\n  \"profileCount\": " << candidate.profiles.size()
        << L",\n  \"excludedRuntimeState\": [\"process state\", \"device connection state\", \"observed driver state\"]\n}\n";
    { std::ofstream file(backup / L"manifest.json", std::ios::binary); auto bytes = Json::ToUtf8(manifest.str()); file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
    auto before = AllRepositoryFileBytes(temp.path); EXPECT_THROW(repository.RestoreBackup(backup), std::runtime_error); EXPECT_EQ(before, AllRepositoryFileBytes(temp.path));
    std::error_code ignored; std::filesystem::remove_all(backup, ignored);
}

TEST(ProfileRepository, SelectedOrLastGlobalCannotBeDeleted)
{
    TempDirectory temp; ProfileRepository repository(temp.path); auto loaded = repository.OpenOrCreate(); auto id = loaded.snapshot.settings.selectedGlobalId;
    EXPECT_THROW(repository.Delete(id, repository.Version(id), loaded.snapshot.settings, repository.SettingsVersion()), std::invalid_argument);
}

TEST(ProfileRepository, SameRevisionSettingsReplacementConflictsAndIsPreserved)
{
    TempDirectory temp; ProfileRepository repository(temp.path); auto loaded = repository.OpenOrCreate(); auto expected = repository.SettingsVersion();
    auto external = loaded.snapshot.settings; external.allowedApplications.emplace(NormalizeExecutable(L"C:\\External\\reader.exe"));
    auto externalBytes = SerializeSettings(external); auto settingsPath = temp.path / L"settings.json";
    { std::ofstream out(settingsPath, std::ios::binary | std::ios::trunc); out.write(externalBytes.data(), static_cast<std::streamsize>(externalBytes.size())); }
    auto draft = loaded.snapshot.settings; draft.mode = Mode::UseGlobal;
    EXPECT_THROW(repository.ApplySettings(draft, expected), RepositoryConflict); EXPECT_EQ(externalBytes, ReadBytes(settingsPath));
}

TEST(ProfileRepository, RevisionsDeriveFromVerifiedStateAndRefuseOverflow)
{
    TempDirectory temp; ProfileRepository repository(temp.path); auto loaded = repository.OpenOrCreate();
    auto appId = NewStableId(); repository.Apply(App(appId, L"Revision", 1, L"C:\\Games\\revision.exe"), std::nullopt);
    loaded = repository.Load(); auto app = loaded.snapshot.profiles.at(appId); app.name = L"Revision changed";
    auto settings = loaded.snapshot.settings; settings.paused = !settings.paused; settings.revision = 0;
    auto expectedSettings = repository.SettingsVersion();
    repository.Apply(app, repository.Version(appId), settings, expectedSettings);
    EXPECT_EQ(expectedSettings.revision + 1, repository.SettingsVersion().revision);

    loaded = repository.Load(); settings = loaded.snapshot.settings; settings.revision = 999999;
    expectedSettings = repository.SettingsVersion();
    repository.Delete(appId, repository.Version(appId), settings, expectedSettings);
    EXPECT_EQ(expectedSettings.revision + 1, repository.SettingsVersion().revision);

    loaded = repository.Load(); auto globalId = loaded.snapshot.settings.selectedGlobalId; auto global = loaded.snapshot.profiles.at(globalId);
    global.revision = std::numeric_limits<std::uint64_t>::max();
    { auto bytes = SerializeProfile(global); std::ofstream file(temp.path / (globalId + L".json"), std::ios::binary | std::ios::trunc); file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
    auto profileBefore = ReadBytes(temp.path / (globalId + L".json")); auto profileVersion = repository.Version(globalId); global.name = L"must not wrap";
    EXPECT_THROW(repository.Apply(global, profileVersion), std::overflow_error); EXPECT_EQ(profileBefore, ReadBytes(temp.path / (globalId + L".json")));

    settings = repository.Load().snapshot.settings; settings.revision = std::numeric_limits<std::uint64_t>::max();
    { auto bytes = SerializeSettings(settings); std::ofstream file(temp.path / L"settings.json", std::ios::binary | std::ios::trunc); file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
    auto settingsBefore = ReadBytes(temp.path / L"settings.json"); auto settingsVersion = repository.SettingsVersion(); settings.paused = !settings.paused;
    EXPECT_THROW(repository.ApplySettings(settings, settingsVersion), std::overflow_error); EXPECT_EQ(settingsBefore, ReadBytes(temp.path / L"settings.json"));
}

TEST(ProfileRepository, InvalidMergedCandidateNeverChangesBytes)
{
    TempDirectory temp; ProfileRepository repository(temp.path); auto loaded = repository.OpenOrCreate(); auto id = loaded.snapshot.settings.selectedGlobalId;
    auto profilePath = temp.path / (id + L".json"); auto settingsPath = temp.path / L"settings.json";
    auto profileBytes = ReadBytes(profilePath); auto settingsBytes = ReadBytes(settingsPath); auto draft = loaded.snapshot.profiles.at(id); draft.enabled = false;
    EXPECT_THROW(repository.Apply(draft, repository.Version(id)), std::invalid_argument);
    EXPECT_EQ(profileBytes, ReadBytes(profilePath)); EXPECT_EQ(settingsBytes, ReadBytes(settingsPath));
}

TEST(ProfileRepository, MalformedExistingTargetForNewIdConflictsAndIsPreserved)
{
    TempDirectory temp; ProfileRepository repository(temp.path); repository.OpenOrCreate(); auto id = NewStableId(); auto target = temp.path / (id + L".json");
    auto malformed = std::string("{ not profile json"); { std::ofstream out(target, std::ios::binary); out.write(malformed.data(), static_cast<std::streamsize>(malformed.size())); }
    EXPECT_THROW(repository.Apply(App(id, L"New", 0, L"C:\\new.exe"), std::nullopt), RepositoryConflict); EXPECT_EQ(malformed, ReadBytes(target));
}

TEST(ProfileRepository, SameRevisionContentChangesRepositoryGeneration)
{
    TempDirectory temp; ProfileRepository repository(temp.path); auto first = repository.OpenOrCreate(); auto settings = first.snapshot.settings;
    settings.allowedApplications.emplace(NormalizeExecutable(L"C:\\external.exe")); auto bytes = SerializeSettings(settings);
    { std::ofstream out(temp.path / L"settings.json", std::ios::binary | std::ios::trunc); out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
    auto second = repository.Load(); EXPECT_NE(first.snapshot.generation, second.snapshot.generation);
}

TEST(ProfileRepository, RecoveryRejectsCorruptIntentBackupAndCommitMarker)
{
    TempDirectory temp; ProfileRepository repository(temp.path); auto loaded = repository.OpenOrCreate(); auto id = loaded.snapshot.settings.selectedGlobalId;
    auto version = repository.Version(id); auto draft = loaded.snapshot.profiles.at(id); draft.name = L"Changed";
    auto settings = loaded.snapshot.settings; settings.mode = Mode::UseGlobal;
    ProfileRepository failing(temp.path, [](CommitBoundary boundary, std::size_t) { if (boundary == CommitBoundary::TargetReplaced) throw std::runtime_error("crash"); });
    EXPECT_THROW(failing.Apply(draft, version, settings, failing.SettingsVersion()), std::runtime_error);
    auto intent = ReadBytes(temp.path / L".transaction.intent");
    auto duplicate = intent; auto settingsTarget = duplicate.find("settings.json"); ASSERT_NE(std::string::npos, settingsTarget);
    auto profileTarget = Json::ToUtf8(id + L".json"); duplicate.replace(settingsTarget, strlen("settings.json"), profileTarget);
    { std::ofstream out(temp.path / L".transaction.intent", std::ios::binary | std::ios::trunc); out.write(duplicate.data(), static_cast<std::streamsize>(duplicate.size())); }
    EXPECT_THROW(repository.Recover(), std::runtime_error);
    { std::ofstream out(temp.path / L".transaction.intent", std::ios::binary | std::ios::trunc); out.write(intent.data(), static_cast<std::streamsize>(intent.size())); }
    auto backup = std::filesystem::directory_iterator(temp.path); std::filesystem::path backupPath;
    for (auto const& entry : backup) if (entry.path().extension() == L".bak") backupPath = entry.path();
    ASSERT_FALSE(backupPath.empty()); { std::ofstream out(backupPath, std::ios::binary | std::ios::trunc); out << "corrupt"; }
    EXPECT_THROW(repository.Recover(), std::runtime_error);
    { std::ofstream out(temp.path / L".transaction.intent", std::ios::binary | std::ios::trunc); out << "invalid"; }
    EXPECT_THROW(repository.Recover(), std::runtime_error);
    { std::ofstream out(temp.path / L".transaction.intent", std::ios::binary | std::ios::trunc); out.write(intent.data(), static_cast<std::streamsize>(intent.size())); }
    { std::ofstream out(temp.path / L".transaction.committed", std::ios::binary); out << "00000000-0000-0000-0000-000000000000"; }
    EXPECT_THROW(repository.Recover(), std::runtime_error);
}

TEST(ProfileRepository, RecoveryNeverOverwritesExternalTargetChanges)
{
    for (bool create : { false, true })
    {
        SCOPED_TRACE(create ? "new target" : "existing target"); TempDirectory temp; ProfileRepository setup(temp.path); auto loaded = setup.OpenOrCreate();
        auto id = create ? NewStableId() : loaded.snapshot.settings.selectedGlobalId;
        auto draft = create ? App(id, L"New", 1, L"C:\\new.exe") : loaded.snapshot.profiles.at(id); if (!create) draft.name = L"Updated";
        ProfileRepository interrupted(temp.path, [](CommitBoundary boundary, std::size_t index)
        { if (boundary == CommitBoundary::TargetReplaced && index == 0) throw std::runtime_error("crash after target replacement"); });
        EXPECT_THROW(interrupted.Apply(draft, create ? std::nullopt : std::optional(interrupted.Version(id))), std::runtime_error);
        auto target = temp.path / (id + L".json"); auto external = std::string("external editor bytes that match neither transaction digest");
        { std::ofstream out(target, std::ios::binary | std::ios::trunc); out.write(external.data(), static_cast<std::streamsize>(external.size())); }
        auto beforeRecovery = AllRepositoryFileBytes(temp.path); EXPECT_THROW(setup.Recover(), RepositoryConflict);
        EXPECT_EQ(beforeRecovery, AllRepositoryFileBytes(temp.path)); EXPECT_EQ(external, ReadBytes(target));
        EXPECT_TRUE(std::filesystem::exists(temp.path / L".transaction.intent"));
    }
}

TEST(ProfileRepository, CreateUpdateDeleteRecoverAsCompleteOldOrNewAtEveryBoundary)
{
    enum class Operation { Create, Update, Delete };
    struct Injection { CommitBoundary boundary; std::size_t index; };
    std::vector<Injection> createOrUpdate{ { CommitBoundary::TempFlushed, 0 }, { CommitBoundary::TempFlushed, 1 },
        { CommitBoundary::IntentFlushed, 0 }, { CommitBoundary::TargetReplaced, 0 }, { CommitBoundary::TargetReplaced, 1 }, { CommitBoundary::SetVerified, 0 },
        { CommitBoundary::CommitMarked, 0 }, { CommitBoundary::Completed, 0 } };
    std::vector<Injection> deleting{ { CommitBoundary::TempFlushed, 1 }, { CommitBoundary::IntentFlushed, 0 },
        { CommitBoundary::TargetReplaced, 0 }, { CommitBoundary::TargetReplaced, 1 }, { CommitBoundary::SetVerified, 0 }, { CommitBoundary::CommitMarked, 0 }, { CommitBoundary::Completed, 0 } };
    for (auto operation : { Operation::Create, Operation::Update, Operation::Delete })
    {
        auto const& injections = operation == Operation::Delete ? deleting : createOrUpdate;
        for (auto injection : injections)
        {
            SCOPED_TRACE(static_cast<int>(operation));
            SCOPED_TRACE(static_cast<int>(injection.boundary));
            SCOPED_TRACE(injection.index);
            TempDirectory temp; ProfileRepository setup(temp.path); auto initial = setup.OpenOrCreate(); auto id = NewStableId();
            auto base = App(id, L"Before", 1, L"C:\\before.exe");
            if (operation != Operation::Create) setup.Apply(base, std::nullopt);
            auto oldBytes = RepositoryBytes(temp.path); bool fired{};
            ProfileRepository failing(temp.path, [&](CommitBoundary boundary, std::size_t index)
            { if (boundary == injection.boundary && index == injection.index) { fired = true; throw std::runtime_error("injected crash"); } });
            auto settings = failing.Load().snapshot.settings; settings.mode = Mode::UseGlobal;
            bool returned{};
            try
            {
                if (operation == Operation::Create) failing.Apply(base, std::nullopt, settings, failing.SettingsVersion());
                else if (operation == Operation::Update) { auto changed = base; changed.name = L"After"; failing.Apply(changed, failing.Version(id), settings, failing.SettingsVersion()); }
                else failing.Delete(id, failing.Version(id), settings, failing.SettingsVersion());
                returned = true;
            }
            catch (std::runtime_error const&) {}
            ASSERT_TRUE(fired); ProfileRepository recovered(temp.path); recovered.Recover(); auto recoveredBytes = RepositoryBytes(temp.path);
            auto committed = injection.boundary == CommitBoundary::CommitMarked || injection.boundary == CommitBoundary::Completed;
            EXPECT_EQ(committed, returned);
            if (!committed) EXPECT_EQ(oldBytes, recoveredBytes);
            else
            {
                EXPECT_NE(oldBytes, recoveredBytes); auto loaded = recovered.Load(); EXPECT_EQ(Mode::UseGlobal, loaded.snapshot.settings.mode);
                if (operation == Operation::Delete) EXPECT_EQ(loaded.snapshot.profiles.end(), loaded.snapshot.profiles.find(id));
                else EXPECT_NE(loaded.snapshot.profiles.end(), loaded.snapshot.profiles.find(id));
            }
        }
    }
}

TEST(ProfileAcceptance, CurrentEditorServiceContractsRunWithoutAProfileWindow)
{
    TempDirectory temp; temp.path = temp.path.parent_path() / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
    auto executable = ClientExecutable();
    auto line = L"\"" + executable.native() + L"\" --profiles-editor-self-test \"" + temp.path.native() + L"\"";
    STARTUPINFOW startup{sizeof(startup)}; ChildProcess child;
    ASSERT_TRUE(::CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, executable.parent_path().c_str(), &startup, &child.process));
    child.WaitAndVerifyExit();
    auto report = temp.path / L"editor-self-test.txt";
    ASSERT_TRUE(std::filesystem::exists(report));
    auto results = ReadBytes(report);
    EXPECT_NE(std::string::npos, results.find("ALL PASSED")) << results;
    EXPECT_EQ(std::string::npos, results.find("FAILED:")) << results;
}

TEST(ProfileAcceptance, ProfileApplySurvivesFullRestart)
{
    TempDirectory temp; temp.path = std::filesystem::path(temp.path.parent_path()) / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
    auto token = NewStableId(); auto readyName = L"Local\\HidHide.Restart.Ready." + token; auto commandName = L"Local\\HidHide.Restart.Command." + token; auto completedName = L"Local\\HidHide.Restart.Completed." + token;
    HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str()); HANDLE command = ::CreateEventW(nullptr, TRUE, FALSE, commandName.c_str()); HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, completedName.c_str());
    ASSERT_TRUE(ready && command && completed);
    auto first = StartRestartWorker(L"apply", temp.path, readyName, commandName, completedName); auto firstPid = first.process.dwProcessId;
    ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 10000)); auto beforeDraft = RepositoryBytes(temp.path);
    ASSERT_EQ(2u, beforeDraft.size()); ASSERT_TRUE(::ResetEvent(ready)); ASSERT_TRUE(::SetEvent(command));
    ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000)); EXPECT_EQ(beforeDraft, RepositoryBytes(temp.path));
    ASSERT_TRUE(::ResetEvent(completed)); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 10000));
    ASSERT_TRUE(::ResetEvent(ready)); ASSERT_TRUE(::SetEvent(command)); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000));
    ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 0)) << "production service Apply path did not report semantic success";
    std::filesystem::path profilePath; IndependentProfileDocument independent;
    for (auto const& entry : std::filesystem::directory_iterator(temp.path))
    {
        if (!entry.is_regular_file() || entry.path().extension() != L".json" || entry.path().filename() == L"settings.json") continue;
        auto candidate = IndependentJsonReader(ReadBytes(entry.path())).Profile();
        if (!candidate.executable.empty()) { profilePath = entry.path(); independent = std::move(candidate); break; }
    }
    ASSERT_FALSE(profilePath.empty()); EXPECT_EQ(Json::ToUtf8(profilePath.stem().native()), independent.id);
    // Structurally parse the committed UTF-8 with a test-only parser that does
    // not call the production codec.
    EXPECT_TRUE(IsStableId(Json::FromUtf8(independent.id))); EXPECT_EQ(1u, independent.revision);
    EXPECT_EQ(Json::ToUtf8(NormalizeExecutable(temp.path / L"F1_25.exe").native()), independent.executable); ASSERT_EQ(2u, independent.rules.size());
    EXPECT_EQ(std::make_pair(std::string("HID\\VID_1234&PID_0001\\CONNECTED"), std::string("hidden")), independent.rules[0]);
    EXPECT_EQ(std::make_pair(std::string("HID\\VID_1234&PID_0002\\REMEMBERED"), std::string("hidden")), independent.rules[1]);
    auto committed = RepositoryBytes(temp.path); first.TerminateAndVerify();
    ASSERT_TRUE(::ResetEvent(ready)); ASSERT_TRUE(::ResetEvent(command)); ASSERT_TRUE(::ResetEvent(completed));
    auto second = StartRestartWorker(L"reload", temp.path, readyName, commandName, completedName); ASSERT_NE(firstPid, second.process.dwProcessId);
    ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 10000)); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000)); EXPECT_EQ(committed, RepositoryBytes(temp.path));
    ASSERT_TRUE(::SetEvent(command)); second.WaitAndVerifyExit(); EXPECT_EQ(committed, RepositoryBytes(temp.path));
    ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed);
    std::cout << "Profile Apply Survives Full Restart: PASS\n";
}

TEST(ProfileAcceptance, InvalidSettingsBlocksServiceWithoutEnforcementOrRepositoryWrites)
{
    TempDirectory temp; temp.path = std::filesystem::path(temp.path.parent_path()) / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
    ProfileRepository repository(temp.path); repository.OpenOrCreate(); auto settings = temp.path / L"settings.json";
    auto corrupt = std::string("{\"schemaVersion\":1,"); HANDLE file = ::CreateFileW(settings.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(INVALID_HANDLE_VALUE, file); DWORD written{}; ASSERT_TRUE(::WriteFile(file, corrupt.data(), static_cast<DWORD>(corrupt.size()), &written, nullptr)); ASSERT_EQ(corrupt.size(), written); ASSERT_TRUE(::FlushFileBuffers(file)); ::CloseHandle(file);
    auto before = RepositoryBytes(temp.path); auto token = NewStableId(); auto readyName = L"Local\\HidHide.Invalid.Ready." + token;
    auto commandName = L"Local\\HidHide.Invalid.Command." + token; auto completedName = L"Local\\HidHide.Invalid.Completed." + token;
    HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str()); HANDLE command = ::CreateEventW(nullptr, TRUE, FALSE, commandName.c_str());
    HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, completedName.c_str()); ASSERT_TRUE(ready && command && completed);
    auto worker = StartRestartWorker(L"invalid", temp.path, readyName, commandName, completedName);
    ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 10000)); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000));
    EXPECT_EQ(nullptr, FindProcessWindow(worker.process.dwProcessId));
    EXPECT_EQ(before, RepositoryBytes(temp.path)); ASSERT_TRUE(::SetEvent(command)); worker.WaitAndVerifyExit(); EXPECT_EQ(before, RepositoryBytes(temp.path));
    ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed);
}

TEST(ProfileAcceptance, UnknownFreshObservationCreatesNoAuthoritativeCatalog)
{
    for (auto phase : { L"unknown-fresh", L"conflict-fresh" })
    {
        SCOPED_TRACE(phase); TempDirectory temp; temp.path = std::filesystem::path(temp.path.parent_path()) / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
        auto token = NewStableId(); auto readyName = L"Local\\HidHide.UnknownFresh.Ready." + token;
        auto commandName = L"Local\\HidHide.UnknownFresh.Command." + token; auto completedName = L"Local\\HidHide.UnknownFresh.Completed." + token;
        HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str()); HANDLE command = ::CreateEventW(nullptr, TRUE, FALSE, commandName.c_str());
        HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, completedName.c_str()); ASSERT_TRUE(ready && command && completed);
        auto worker = StartRestartWorker(phase, temp.path, readyName, commandName, completedName);
        ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 10000)); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000));
        EXPECT_TRUE(AllRepositoryFileBytes(temp.path).empty()); ASSERT_TRUE(::SetEvent(command)); worker.WaitAndVerifyExit();
        EXPECT_TRUE(AllRepositoryFileBytes(temp.path).empty()); ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed);
    }
}

TEST(ProfileAcceptance, HardRecoveryFailuresBlockServiceAndPreserveAllEvidence)
{
    for (int scenario : { 0, 1, 2 })
    {
        SCOPED_TRACE(scenario == 0 ? "corrupt intent" : scenario == 1 ? "external target" : "missing settings with profile evidence");
        TempDirectory temp; temp.path = std::filesystem::path(temp.path.parent_path()) / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
        ProfileRepository setup(temp.path); auto loaded = setup.OpenOrCreate(); auto id = loaded.snapshot.settings.selectedGlobalId;
        if (scenario == 0)
        {
            std::ofstream intent(temp.path / L".transaction.intent", std::ios::binary | std::ios::trunc); intent << "corrupt intent"; intent.close();
        }
        else if (scenario == 1)
        {
            auto changed = loaded.snapshot.profiles.at(id); changed.name = L"transaction replacement";
            ProfileRepository interrupted(temp.path, [](CommitBoundary boundary, std::size_t)
            { if (boundary == CommitBoundary::TargetReplaced) throw std::runtime_error("injected pre-commit stop"); });
            EXPECT_THROW(interrupted.Apply(changed, interrupted.Version(id)), std::runtime_error);
            auto external = changed; external.name = L"external third state"; external.revision = 77;
            std::ofstream target(temp.path / (id + L".json"), std::ios::binary | std::ios::trunc); auto bytes = SerializeProfile(external);
            target.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); target.close();
        }
        else std::filesystem::remove(temp.path / L"settings.json");
        auto before = AllRepositoryFileBytes(temp.path); auto token = NewStableId(); auto readyName = L"Local\\HidHide.HardRecovery.Ready." + token;
        auto commandName = L"Local\\HidHide.HardRecovery.Command." + token; auto completedName = L"Local\\HidHide.HardRecovery.Completed." + token;
        HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str()); HANDLE command = ::CreateEventW(nullptr, TRUE, FALSE, commandName.c_str());
        HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, completedName.c_str()); ASSERT_TRUE(ready && command && completed);
        auto worker = StartRestartWorker(L"invalid", temp.path, readyName, commandName, completedName);
        ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 10000)); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000));
        EXPECT_EQ(before, AllRepositoryFileBytes(temp.path)); ASSERT_TRUE(::SetEvent(command)); worker.WaitAndVerifyExit();
        EXPECT_EQ(before, AllRepositoryFileBytes(temp.path)); ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed);
    }
}

TEST(ProfileAcceptance, AutomaticSnapshotDraftFailureAndShutdownWriteNoJson)
{
    TempDirectory temp; temp.path = std::filesystem::path(temp.path.parent_path()) / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
    ProfileRepository repository(temp.path); repository.OpenOrCreate(); auto before = RepositoryBytes(temp.path); auto token = NewStableId();
    auto readyName = L"Local\\HidHide.Events.Ready." + token; auto commandName = L"Local\\HidHide.Events.Command." + token;
    auto completedName = L"Local\\HidHide.Events.Completed." + token; HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    HANDLE command = ::CreateEventW(nullptr, TRUE, FALSE, commandName.c_str()); HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, completedName.c_str()); ASSERT_TRUE(ready && command && completed);
    auto worker = StartRestartWorker(L"events", temp.path, readyName, commandName, completedName);
    ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 10000)); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000)); EXPECT_EQ(before, RepositoryBytes(temp.path));
    ASSERT_TRUE(::SetEvent(command)); worker.WaitAndVerifyExit(); EXPECT_EQ(before, RepositoryBytes(temp.path));
    ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed);
}

TEST(ProfileEnforcement, ProductionAdapterConformanceCasDedupReadbackAndPartialFailure)
{
    TempDirectory temp; temp.path = std::filesystem::path(temp.path.parent_path()) / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
    auto token = NewStableId(); auto readyName = L"Local\\HidHide.Adapter.Ready." + token; auto commandName = L"Local\\HidHide.Adapter.Command." + token;
    auto completedName = L"Local\\HidHide.Adapter.Completed." + token; HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    HANDLE command = ::CreateEventW(nullptr, TRUE, FALSE, commandName.c_str()); HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, completedName.c_str()); ASSERT_TRUE(ready && command && completed);
    auto worker = StartRestartWorker(L"adapter", temp.path, readyName, commandName, completedName);
    ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 10000)); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000));
    ASSERT_TRUE(::SetEvent(command)); worker.WaitAndVerifyExit(); ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed);
}

TEST(ProfileAcceptance, CoordinatorWatcherAndBoundedRetryNotifications)
{
    for (auto phase : { L"coordinator-late-registration", L"coordinator-parent-replacement", L"coordinator-watcher", L"coordinator-retry", L"coordinator-conflict", L"coordinator-maintenance", L"coordinator-incomplete", L"coordinator-missing" })
    {
        SCOPED_TRACE(Json::ToUtf8(phase));
        TempDirectory temp; temp.path = temp.path.parent_path() / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
        auto token = NewStableId(); auto readyName = L"Local\\HidHide.Coordinator.Ready." + token;
        auto commandName = L"Local\\HidHide.Coordinator.Command." + token; auto completedName = L"Local\\HidHide.Coordinator.Completed." + token;
        HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        HANDLE command = ::CreateEventW(nullptr, TRUE, FALSE, commandName.c_str());
        HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, completedName.c_str()); ASSERT_TRUE(ready && command && completed);
        auto worker = StartRestartWorker(phase, temp.path, readyName, commandName, completedName);
        auto wait = ::WaitForSingleObject(completed, 15000);
        std::string diagnostic;
        if (std::filesystem::exists(temp.path / L".acceptance-error.txt")) diagnostic = ReadBytes(temp.path / L".acceptance-error.txt");
        EXPECT_EQ(WAIT_OBJECT_0, wait) << diagnostic;
        EXPECT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 0)) << diagnostic;
        ::SetEvent(command); worker.WaitAndVerifyExit();
        ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed);
    }
}

TEST(ProfileAcceptance, ValidatedRestoreImmediatelyClearsRepositoryBlockAndApplies)
{
    TempDirectory temp; temp.path = std::filesystem::path(temp.path.parent_path()) / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
    ProfileRepository repository(temp.path); repository.OpenOrCreate(); auto backup = std::filesystem::path(temp.path.native() + L"-backup"); repository.Backup(backup);
    auto settings = temp.path / L"settings.json"; std::ofstream corrupt(settings, std::ios::binary | std::ios::trunc); corrupt << "{"; corrupt.close();
    auto token = NewStableId(); auto readyName = L"Local\\HidHide.Restore.Ready." + token; auto commandName = L"Local\\HidHide.Restore.Command." + token;
    auto completedName = L"Local\\HidHide.Restore.Completed." + token; HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    HANDLE command = ::CreateEventW(nullptr, TRUE, FALSE, commandName.c_str()); HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, completedName.c_str()); ASSERT_TRUE(ready && command && completed);
    auto worker = StartRestartWorker(L"restore", temp.path, readyName, commandName, completedName);
    ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 10000)); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000)); EXPECT_TRUE(repository.Load().issues.empty());
    ASSERT_TRUE(::SetEvent(command)); worker.WaitAndVerifyExit(); ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed);
    std::error_code ignored; std::filesystem::remove_all(backup, ignored);
}

TEST(ProfileAcceptance, SavedButEnforcementFailedRemainsDistinctInService)
{
    TempDirectory temp; temp.path = std::filesystem::path(temp.path.parent_path()) / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
    auto token = NewStableId(); auto readyName = L"Local\\HidHide.EnforcementFailure.Ready." + token; auto commandName = L"Local\\HidHide.EnforcementFailure.Command." + token;
    auto completedName = L"Local\\HidHide.EnforcementFailure.Completed." + token; HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    HANDLE command = ::CreateEventW(nullptr, TRUE, FALSE, commandName.c_str()); HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, completedName.c_str()); ASSERT_TRUE(ready && command && completed);
    auto worker = StartRestartWorker(L"enforcement-failure", temp.path, readyName, commandName, completedName);
    ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 10000)); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000));
    ASSERT_TRUE(::SetEvent(command)); worker.WaitAndVerifyExit(); ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed);
}

TEST(ProfileAcceptance, DeviceNotificationBurstUsesBoundedProductionCoalescer)
{
    TempDirectory temp; temp.path = std::filesystem::path(temp.path.parent_path()) / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
    auto token = NewStableId(); auto readyName = L"Local\\HidHide.DeviceCoalescing.Ready." + token; auto commandName = L"Local\\HidHide.DeviceCoalescing.Command." + token;
    auto completedName = L"Local\\HidHide.DeviceCoalescing.Completed." + token; HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    HANDLE command = ::CreateEventW(nullptr, TRUE, FALSE, commandName.c_str()); HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, completedName.c_str()); ASSERT_TRUE(ready && command && completed);
    auto worker = StartRestartWorker(L"device-coalescing", temp.path, readyName, commandName, completedName);
    ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 10000)); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000));
    ASSERT_TRUE(::SetEvent(command)); worker.WaitAndVerifyExit(); ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed);
}

TEST(ProfileAcceptance, KnownConflictAndUnknownReadbackRemainDistinctInService)
{
    for (auto phase : { L"verified-observation", L"known-observation", L"known-observation-allowed-change", L"unknown-observation" })
    {
        SCOPED_TRACE(phase); TempDirectory temp; temp.path = std::filesystem::path(temp.path.parent_path()) / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
        ProfileRepository(temp.path).OpenOrCreate();
        auto token = NewStableId(); auto readyName = L"Local\\HidHide.Observation.Ready." + token; auto commandName = L"Local\\HidHide.Observation.Command." + token;
        auto completedName = L"Local\\HidHide.Observation.Completed." + token; HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        HANDLE command = ::CreateEventW(nullptr, TRUE, FALSE, commandName.c_str()); HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, completedName.c_str()); ASSERT_TRUE(ready && command && completed);
        auto worker = StartRestartWorker(phase, temp.path, readyName, commandName, completedName);
        ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 10000)); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000));
        ASSERT_TRUE(::SetEvent(command)); worker.WaitAndVerifyExit(); ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed);
    }
}

TEST(ProfileAcceptance, LiveProcessSelectionUpdatesWithoutRepositoryWrites)
{
    TempDirectory temp; temp.path = std::filesystem::path(temp.path.parent_path()) / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
    auto token = NewStableId(); auto readyName = L"Local\\HidHide.LiveStatus.Ready." + token; auto commandName = L"Local\\HidHide.LiveStatus.Command." + token;
    auto completedName = L"Local\\HidHide.LiveStatus.Completed." + token; HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    HANDLE command = ::CreateEventW(nullptr, TRUE, FALSE, commandName.c_str()); HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, completedName.c_str()); ASSERT_TRUE(ready && command && completed);
    auto worker = StartRestartWorker(L"live-status", temp.path, readyName, commandName, completedName);
    ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 10000)); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000));
    ASSERT_TRUE(::SetEvent(command)); worker.WaitAndVerifyExit(); ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed);
}

TEST(ProfileAcceptance, AdoptionBoundaryRetryAndGlobalStatusUseCurrentService)
{
    for (auto phase : { L"adoption-race", L"adoption-retry", L"global-status" })
    {
        SCOPED_TRACE(phase); TempDirectory temp; temp.path = std::filesystem::path(temp.path.parent_path()) / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
        auto token = NewStableId(); auto readyName = L"Local\\HidHide.AdoptionStatus.Ready." + token; auto commandName = L"Local\\HidHide.AdoptionStatus.Command." + token;
        auto completedName = L"Local\\HidHide.AdoptionStatus.Completed." + token; HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        HANDLE command = ::CreateEventW(nullptr, TRUE, FALSE, commandName.c_str()); HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, completedName.c_str()); ASSERT_TRUE(ready && command && completed);
        auto worker = StartRestartWorker(phase, temp.path, readyName, commandName, completedName);
        HANDLE initial[]{ ready, completed }; auto first = ::WaitForMultipleObjects(2, initial, FALSE, 10000);
        if (first == WAIT_OBJECT_0 + 1)
        {
            auto diagnostic = temp.path / L".acceptance-error.txt";
            FAIL() << (std::filesystem::exists(diagnostic) ? ReadBytes(diagnostic) : "acceptance worker failed without diagnostics");
        }
        ASSERT_EQ(WAIT_OBJECT_0, first); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000));
        ASSERT_TRUE(::SetEvent(command)); worker.WaitAndVerifyExit(); ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed);
    }
}

TEST(ProfileAcceptance, ChangedAdoptionAndObservedStateUseCurrentService)
{
    for (auto phase : { L"adoption-changed-lifecycle", L"device-conflict-propagation", L"selection-unknown-propagation" })
    {
        SCOPED_TRACE(phase); TempDirectory temp; temp.path = std::filesystem::path(temp.path.parent_path()) / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
        auto token = NewStableId(); auto readyName = L"Local\\HidHide.AdoptionObserve.Ready." + token; auto commandName = L"Local\\HidHide.AdoptionObserve.Command." + token;
        auto completedName = L"Local\\HidHide.AdoptionObserve.Completed." + token; HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        HANDLE command = ::CreateEventW(nullptr, TRUE, FALSE, commandName.c_str()); HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, completedName.c_str()); ASSERT_TRUE(ready && command && completed);
        auto worker = StartRestartWorker(phase, temp.path, readyName, commandName, completedName); HANDLE initial[]{ ready, completed };
        auto first = ::WaitForMultipleObjects(2, initial, FALSE, 10000); if (first == WAIT_OBJECT_0 + 1)
        {
            auto diagnostic = temp.path / L".acceptance-error.txt"; FAIL() << (std::filesystem::exists(diagnostic) ? ReadBytes(diagnostic) : "acceptance worker failed without diagnostics");
        }
        ASSERT_EQ(WAIT_OBJECT_0, first); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000));
        ASSERT_TRUE(::SetEvent(command)); worker.WaitAndVerifyExit(); ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed);
    }
}

TEST(ProfileAcceptance, StartupFailureDoesNotBlockPolicyAndRepositoryDiagnosticsCannotAdoptDriver)
{
    for (auto phase : { L"startup-failure", L"repository-domain" })
    {
        SCOPED_TRACE(phase); TempDirectory temp; temp.path = std::filesystem::path(temp.path.parent_path()) / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
        auto token = NewStableId(); auto readyName = L"Local\\HidHide.Domains.Ready." + token; auto commandName = L"Local\\HidHide.Domains.Command." + token;
        auto completedName = L"Local\\HidHide.Domains.Completed." + token; HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        HANDLE command = ::CreateEventW(nullptr, TRUE, FALSE, commandName.c_str()); HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, completedName.c_str()); ASSERT_TRUE(ready && command && completed);
        auto worker = StartRestartWorker(phase, temp.path, readyName, commandName, completedName);
        ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 10000)); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000));
        ASSERT_TRUE(::SetEvent(command)); worker.WaitAndVerifyExit(); ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed);
    }
}

TEST(ProfileRepository, CrossProcessWriterLeaseSerializesApplyAndPreservesCas)
{
    TempDirectory temp; temp.path = std::filesystem::path(temp.path.parent_path()) / (L"HidHide-Profiles-Restart-Test-" + NewStableId());
    ProfileRepository repository(temp.path); repository.OpenOrCreate(); auto token = NewStableId();
    auto readyName = L"Local\\HidHide.Lease.Ready." + token; auto commandName = L"Local\\HidHide.Lease.Command." + token; auto completedName = L"Local\\HidHide.Lease.Completed." + token;
    HANDLE ready = ::CreateEventW(nullptr, TRUE, FALSE, readyName.c_str()); HANDLE command = ::CreateEventW(nullptr, TRUE, FALSE, commandName.c_str()); HANDLE completed = ::CreateEventW(nullptr, TRUE, FALSE, completedName.c_str()); ASSERT_TRUE(ready && command && completed);
    auto holder = StartRestartWorker(L"hold", temp.path, readyName, commandName, completedName); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(ready, 10000));
    auto id = NewStableId(); auto apply = std::async(std::launch::async, [&] { return repository.Apply(App(id, L"Serialized", 0, L"C:\\serialized.exe"), std::nullopt); });
    EXPECT_EQ(std::future_status::timeout, apply.wait_for(std::chrono::milliseconds(250))); ASSERT_TRUE(::SetEvent(command)); ASSERT_EQ(WAIT_OBJECT_0, ::WaitForSingleObject(completed, 10000));
    EXPECT_EQ(1u, apply.get().revision); holder.TerminateAndVerify(); EXPECT_TRUE(repository.Load().snapshot.profiles.count(id));
    ::CloseHandle(ready); ::CloseHandle(command); ::CloseHandle(completed);
}

TEST(ProfileRepository, ExportBackupCollisionTraversalAndLockedTargetAreSafe)
{
    TempDirectory temp; ProfileRepository repository(temp.path); repository.OpenOrCreate(); auto id = NewStableId();
    auto version = repository.Apply(App(id, L"Export", 0, L"C:\\export.exe"), std::nullopt); auto source = temp.path / (id + L".json");
    auto exported = temp.path.parent_path() / (L"HidHide-export-" + NewStableId() + L".json");
    repository.ExportProfile(id, exported); EXPECT_EQ(ReadBytes(source), ReadBytes(exported));
    EXPECT_THROW(repository.ExportProfile(L"..\\settings", exported), std::invalid_argument);
    EXPECT_THROW(repository.ExportProfile(id, temp.path / L"settings.json"), std::invalid_argument);
    EXPECT_THROW(repository.ExportProfile(id, temp.path / L".transaction.intent"), std::invalid_argument);
    EXPECT_THROW(repository.ExportProfile(id, temp.path / L".." / temp.path.filename() / L"case-alias.json"), std::invalid_argument);
    auto upperRoot = temp.path.native(); std::transform(upperRoot.begin(), upperRoot.end(), upperRoot.begin(), ::towupper);
    EXPECT_THROW(repository.ExportProfile(id, std::filesystem::path(upperRoot) / L"case.json"), std::invalid_argument);
    EXPECT_THROW(repository.Backup(temp.path / L"nested-backup"), std::invalid_argument);
    auto hardlink = temp.path.parent_path() / (L"HidHide-hardlink-" + NewStableId() + L".json"); auto settingsBytes = ReadBytes(temp.path / L"settings.json");
    ASSERT_TRUE(::CreateHardLinkW(hardlink.c_str(), (temp.path / L"settings.json").c_str(), nullptr));
    EXPECT_THROW(repository.ExportProfile(id, hardlink), std::invalid_argument); EXPECT_EQ(settingsBytes, ReadBytes(temp.path / L"settings.json"));
    auto backup = temp.path.parent_path() / (L"HidHide-backup-" + NewStableId()); std::filesystem::create_directories(backup);
    EXPECT_THROW(repository.Backup(backup), std::runtime_error);
    HANDLE locked = ::CreateFileW(source.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); ASSERT_NE(INVALID_HANDLE_VALUE, locked);
    auto changed = App(id, L"Locked change", 0, L"C:\\export.exe"); EXPECT_ANY_THROW(repository.Apply(changed, version)); ::CloseHandle(locked); repository.Recover();
    EXPECT_EQ(L"Export", repository.Load().snapshot.profiles.at(id).name);
    std::error_code ignored; std::filesystem::remove(exported, ignored); std::filesystem::remove(hardlink, ignored); std::filesystem::remove_all(backup, ignored);
}

TEST(ProfileRepository, PortableBackupRestoreIsValidatedTransactionalAndRepairsSettings)
{
    TempDirectory temp; ProfileRepository repository(temp.path); auto original = repository.OpenOrCreate();
    auto applicationId = NewStableId(); repository.Apply(App(applicationId, L"Backed up", 7, L"C:\\Games\\F1_25.exe"), std::nullopt);
    auto expected = RepositoryBytes(temp.path);
    auto backup = temp.path.parent_path() / (L"HidHide-restore-" + NewStableId()); repository.Backup(backup);

    auto changed = repository.Load(); auto globalId = changed.snapshot.settings.selectedGlobalId;
    auto global = changed.snapshot.profiles.at(globalId); global.name = L"Changed after backup"; repository.Apply(global, repository.Version(globalId));
    auto extraId = NewStableId(); repository.Apply(App(extraId, L"Extra", 99, L"C:\\Games\\Extra.exe"), std::nullopt);
    repository.RestoreBackup(backup);
    EXPECT_EQ(expected, RepositoryBytes(temp.path)); EXPECT_FALSE(repository.Load().snapshot.profiles.count(extraId));

    auto settingsPath = temp.path / L"settings.json"; auto corrupt = std::string("{not-json");
    HANDLE file = ::CreateFileW(settingsPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(INVALID_HANDLE_VALUE, file); DWORD written{}; ASSERT_TRUE(::WriteFile(file, corrupt.data(), static_cast<DWORD>(corrupt.size()), &written, nullptr)); ::CloseHandle(file);
    ASSERT_NO_THROW(repository.RestoreBackup(backup)); EXPECT_EQ(expected, RepositoryBytes(temp.path));

    auto manifestPath = backup / L"manifest.json"; auto manifest = ReadBytes(manifestPath);
    file = ::CreateFileW(manifestPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(INVALID_HANDLE_VALUE, file); auto invalid = std::string("{}\n"); ASSERT_TRUE(::WriteFile(file, invalid.data(), static_cast<DWORD>(invalid.size()), &written, nullptr)); ::CloseHandle(file);
    auto beforeRejectedRestore = RepositoryBytes(temp.path); EXPECT_ANY_THROW(repository.RestoreBackup(backup)); EXPECT_EQ(beforeRejectedRestore, RepositoryBytes(temp.path));
    std::error_code ignored; std::filesystem::remove_all(backup, ignored);
}

TEST(ProfileRepository, MaximumDisjointRestoreIntentRemainsRecoverable)
{
    TempDirectory temp; ProfileRepository setup(temp.path); auto current = setup.OpenOrCreate();
    auto MakeId = [](unsigned family, unsigned value)
    {
        wchar_t text[37]{}; swprintf_s(text, L"%08x-0000-0000-0000-%012x", family, value); return std::wstring(text);
    };
    auto Write = [](std::filesystem::path const& path, std::string const& bytes)
    { std::ofstream out(path, std::ios::binary | std::ios::trunc); if (!out) throw std::runtime_error("create high-count fixture"); out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); };
    for (unsigned i = 1; i < MaxProfiles; ++i)
    {
        auto profile = App(MakeId(1, i), L"Current", 0, L"C:\\Current\\Game.exe"); Write(temp.path / (profile.id + L".json"), SerializeProfile(profile));
    }
    auto backup = temp.path.parent_path() / (L"HidHide-max-restore-" + NewStableId()); std::filesystem::create_directories(backup);
    Snapshot replacement; auto replacementGlobal = Global(MakeId(2, 0), L"Replacement"); replacement.profiles.emplace(replacementGlobal.id, replacementGlobal);
    for (unsigned i = 1; i < MaxProfiles; ++i)
    {
        auto profile = App(MakeId(2, i), L"Replacement", 0, L"C:\\Replacement\\Game.exe"); replacement.profiles.emplace(profile.id, profile);
    }
    replacement.settings = { 1, replacementGlobal.id, Mode::Automatic, false, true, {} }; Validate(replacement);
    for (auto const& [id, profile] : replacement.profiles) Write(backup / (id + L".json"), SerializeProfile(profile));
    Write(backup / L"settings.json", SerializeSettings(replacement.settings));
    std::wostringstream manifest; manifest << L"{\n  \"schemaVersion\": 1,\n  \"format\": \"HidHide Profiles portable backup\",\n  \"profileCount\": " << MaxProfiles
        << L",\n  \"excludedRuntimeState\": [\"process state\", \"device connection state\", \"observed driver state\"]\n}\n";
    Write(backup / L"manifest.json", Json::ToUtf8(manifest.str()));
    ProfileRepository interrupted(temp.path, [](CommitBoundary boundary, std::size_t) { if (boundary == CommitBoundary::IntentFlushed) throw std::runtime_error("crash after maximum intent"); });
    EXPECT_THROW(interrupted.RestoreBackup(backup), std::runtime_error); ASSERT_TRUE(std::filesystem::exists(temp.path / L".transaction.intent"));
    ASSERT_NO_THROW(setup.Recover()); auto recovered = setup.Load(); EXPECT_EQ(MaxProfiles, recovered.snapshot.profiles.size());
    EXPECT_EQ(current.snapshot.settings.selectedGlobalId, recovered.snapshot.settings.selectedGlobalId); EXPECT_FALSE(recovered.snapshot.profiles.count(replacementGlobal.id));
    std::error_code ignored; std::filesystem::remove_all(backup, ignored);
}

TEST(ProfileRepository, LockedRollbackAndCommittedCleanupRetainRecoverableEvidence)
{
    {
        TempDirectory temp; ProfileRepository setup(temp.path); setup.OpenOrCreate(); auto id = NewStableId();
        ProfileRepository failing(temp.path, [](CommitBoundary boundary, std::size_t) { if (boundary == CommitBoundary::TargetReplaced) throw std::runtime_error("crash"); });
        EXPECT_THROW(failing.Apply(App(id, L"New", 0, L"C:\\new.exe"), std::nullopt), std::runtime_error);
        auto target = temp.path / (id + L".json"); HANDLE locked = ::CreateFileW(target.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); ASSERT_NE(INVALID_HANDLE_VALUE, locked);
        EXPECT_THROW(setup.Recover(), std::system_error); EXPECT_TRUE(std::filesystem::exists(temp.path / L".transaction.intent"));
        ::CloseHandle(locked); setup.Recover(); EXPECT_FALSE(std::filesystem::exists(target));
    }
    {
        TempDirectory temp; ProfileRepository setup(temp.path); auto loaded = setup.OpenOrCreate(); auto id = loaded.snapshot.settings.selectedGlobalId; HANDLE locked{};
        ProfileRepository failing(temp.path, [&](CommitBoundary boundary, std::size_t)
        {
            if (boundary != CommitBoundary::CommitMarked) return;
            for (auto const& entry : std::filesystem::directory_iterator(temp.path)) if (entry.path().extension() == L".bak")
            { locked = ::CreateFileW(entry.path().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr); break; }
        });
        auto changed = loaded.snapshot.profiles.at(id); changed.name = L"Committed";
        failing.Apply(changed, failing.Version(id)); ASSERT_NE(INVALID_HANDLE_VALUE, locked); EXPECT_TRUE(failing.CleanupPending());
        EXPECT_TRUE(std::filesystem::exists(temp.path / L".transaction.intent")); EXPECT_TRUE(std::filesystem::exists(temp.path / L".transaction.committed"));
        ::CloseHandle(locked); setup.Recover(); EXPECT_EQ(L"Committed", setup.Load().snapshot.profiles.at(id).name);
    }
}

TEST(ProfileRepository, CommittedCleanupFailureReturnsTruthfulSavedOutcome)
{
    TempDirectory temp; ProfileApplicationService setup(temp.path); auto loaded = setup.OpenOrCreate(); auto id = loaded.snapshot.settings.selectedGlobalId;
    auto changed = loaded.snapshot.profiles.at(id); changed.name = L"Durably saved";
    ProfileApplicationService service(temp.path, [](CommitBoundary boundary, std::size_t)
    { if (boundary == CommitBoundary::CommitMarked) throw std::runtime_error("cleanup interruption"); });
    auto result = service.Apply(changed, service.Version(id));
    EXPECT_TRUE(result.cleanupPending); EXPECT_EQ(L"Durably saved", result.loaded.snapshot.profiles.at(id).name);
    ProfileRepository restarted(temp.path); restarted.Recover(); EXPECT_EQ(L"Durably saved", restarted.Load().snapshot.profiles.at(id).name);
}

TEST(ProfileRepository, RecoveryIsIdempotentAfterEveryRestoredItemAndFullVerification)
{
    for (auto injection : std::vector<std::pair<CommitBoundary, std::size_t>>{
        { CommitBoundary::RecoveryTargetRestored, 0 }, { CommitBoundary::RecoveryTargetRestored, 1 }, { CommitBoundary::RecoveryVerified, 0 } })
    {
        TempDirectory temp; ProfileRepository setup(temp.path); auto loaded = setup.OpenOrCreate(); auto id = loaded.snapshot.settings.selectedGlobalId;
        auto oldBytes = RepositoryBytes(temp.path); auto changed = loaded.snapshot.profiles.at(id); changed.name = L"Interrupted";
        auto settings = loaded.snapshot.settings; settings.mode = Mode::UseGlobal;
        ProfileRepository crashingCommit(temp.path, [](CommitBoundary boundary, std::size_t index)
        { if (boundary == CommitBoundary::TargetReplaced && index == 0) throw std::runtime_error("commit crash"); });
        EXPECT_THROW(crashingCommit.Apply(changed, crashingCommit.Version(id), settings, crashingCommit.SettingsVersion()), std::runtime_error);
        bool fired{}; ProfileRepository crashingRecovery(temp.path, [&](CommitBoundary boundary, std::size_t index)
        { if (boundary == injection.first && index == injection.second) { fired = true; throw std::runtime_error("recovery crash"); } });
        EXPECT_THROW(crashingRecovery.Recover(), std::runtime_error); ASSERT_TRUE(fired);
        ProfileRepository retry(temp.path); retry.Recover(); EXPECT_EQ(oldBytes, RepositoryBytes(temp.path));
    }
}

TEST(ProfileRepository, FinalCommitSetCannotBeExternallyReplacedAfterEarlyVerification)
{
    TempDirectory temp; ProfileRepository setup(temp.path); auto loaded = setup.OpenOrCreate(); auto id = loaded.snapshot.settings.selectedGlobalId;
    auto changed = loaded.snapshot.profiles.at(id); changed.name = L"Protected"; auto settings = loaded.snapshot.settings; settings.mode = Mode::UseGlobal;
    bool replacementBlocked{};
    ProfileRepository protectedCommit(temp.path, [&](CommitBoundary boundary, std::size_t)
    {
        if (boundary != CommitBoundary::SetVerified) return;
        auto target = temp.path / (id + L".json");
        HANDLE external = ::CreateFileW(target.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        replacementBlocked = external == INVALID_HANDLE_VALUE && ::GetLastError() == ERROR_SHARING_VIOLATION;
        if (external != INVALID_HANDLE_VALUE) ::CloseHandle(external);
    });
    protectedCommit.Apply(changed, protectedCommit.Version(id), settings, protectedCommit.SettingsVersion());
    EXPECT_TRUE(replacementBlocked); auto after = setup.Load(); EXPECT_EQ(L"Protected", after.snapshot.profiles.at(id).name); EXPECT_EQ(Mode::UseGlobal, after.snapshot.settings.mode);
}

TEST(ProfileRepository, DeletedTargetRecreationAtCommitCannotReportFalseSuccess)
{
    TempDirectory temp; ProfileRepository setup(temp.path); auto loaded = setup.OpenOrCreate(); auto id = NewStableId();
    auto app = App(id, L"Delete race", 1, L"C:\\Games\\delete-race.exe"); setup.Apply(app, std::nullopt);
    auto target = temp.path / (id + L".json"); auto original = ReadBytes(target); bool recreated{};
    ProfileRepository racingDelete(temp.path, [&](CommitBoundary boundary, std::size_t)
    {
        if (boundary != CommitBoundary::SetVerified) return;
        std::ofstream replacement(target, std::ios::binary | std::ios::trunc);
        replacement.write(original.data(), static_cast<std::streamsize>(original.size())); replacement.close(); recreated = true;
    });
    EXPECT_THROW(racingDelete.Delete(id, racingDelete.Version(id), racingDelete.Load().snapshot.settings,
        racingDelete.SettingsVersion()), RepositoryConflict);
    ASSERT_TRUE(recreated); EXPECT_EQ(original, ReadBytes(target));
    EXPECT_TRUE(std::filesystem::exists(temp.path / L".transaction.intent"));
    EXPECT_TRUE(std::filesystem::exists(temp.path / L".transaction.committed"));
    auto preserved = AllRepositoryFileBytes(temp.path);
    ProfileRepository restart(temp.path); EXPECT_THROW(restart.Recover(), std::runtime_error);
    EXPECT_EQ(preserved, AllRepositoryFileBytes(temp.path));
}

namespace
{
    class FakeEnforcement final : public IEnforcement
    {
    public:
        EnforcementResult Observe() override { ++observations; return observation; }
        EnforcementResult Reconcile(DesiredEnforcement const& desired) override { ++writes; observation = { true, true, desired, {} }; return observation; }
        EnforcementResult RestoreBaseline() override { return observation; }
        EnforcementResult AdoptCurrentAsBaseline() override { return observation; }
        EnforcementResult observation; unsigned observations{}, writes{};
    };
}

TEST(ProfileEnforcement, DedupReobservesAndRepairsExternalChange)
{
    FakeEnforcement fake; DeduplicatingEnforcement adapter(fake); DesiredEnforcement desired{ true, { L"HID\\A" }, { L"C:\\feeder.exe" } };
    ASSERT_TRUE(adapter.Reconcile(desired).success); EXPECT_EQ(1u, fake.writes);
    ASSERT_TRUE(adapter.Reconcile(desired).success); EXPECT_EQ(1u, fake.writes); EXPECT_EQ(1u, fake.observations);
    fake.observation.observed.hiddenDevices.clear(); ASSERT_TRUE(adapter.Reconcile(desired).success); EXPECT_EQ(2u, fake.writes); EXPECT_EQ(2u, fake.observations);
}

TEST(ProfileEnforcement, CoordinatorProxyStartupRefreshDoesNotRequireLegacyCatalog)
{
    unsigned driverOnlyReads{}; HidHide::DriverConfiguration driverOnly; driverOnly.active = true;
    auto runtime = HidHide::FilterDriverProxy::ComposeProfilesRuntimeConfiguration([&] { ++driverOnlyReads; return driverOnly; });
    EXPECT_EQ(1u, driverOnlyReads); EXPECT_TRUE(runtime.active); EXPECT_TRUE(runtime.profiles.empty());
    // The exact production composition function accepts only a driver-state
    // reader; there is no ConfigurationV1 callback that the runtime can reach.
}

TEST(ProfileRecovery, ValueScopedOwnershipPreservesForeignAndLegacyEvidence)
{
    std::map<std::wstring, std::vector<std::uint8_t>> values{
        { HidHide::ProfileRecovery::OwnedValue, { 2 } }, { HidHide::ProfileRecovery::LegacyValue, { 1 } }, { L"ForeignValue", { 9 } } };
    auto exists = [&](wchar_t const* name) { return values.count(name) != 0; };
    ASSERT_TRUE(HidHide::ProfileRecovery::RelevantRecoveryExists(exists));
    HidHide::ProfileRecovery::ClearOwnedRecovery([&](wchar_t const* name) { values.erase(name); });
    EXPECT_FALSE(values.count(HidHide::ProfileRecovery::OwnedValue)); EXPECT_TRUE(values.count(HidHide::ProfileRecovery::LegacyValue)); EXPECT_TRUE(values.count(L"ForeignValue"));
    EXPECT_TRUE(HidHide::ProfileRecovery::RelevantRecoveryExists(exists)); values.erase(HidHide::ProfileRecovery::LegacyValue);
    EXPECT_FALSE(HidHide::ProfileRecovery::RelevantRecoveryExists(exists)); EXPECT_TRUE(values.count(L"ForeignValue")); values.clear();
    EXPECT_FALSE(HidHide::ProfileRecovery::RelevantRecoveryExists(exists));

    values = { { HidHide::ProfileRecovery::OwnedValue, { 2 } }, { HidHide::ProfileRecovery::LegacyValue, { 1 } }, { L"ForeignValue", { 9 } } };
    HidHide::ProfileRecovery::ClearKnownRecoveryAfterExplicitAdoption([&](wchar_t const* name) { values.erase(name); });
    EXPECT_FALSE(HidHide::ProfileRecovery::RelevantRecoveryExists(exists)); EXPECT_TRUE(values.count(L"ForeignValue"));
}

template <typename T, typename = void> struct ExposesLegacyProfileMutation : std::false_type {};
template <typename T> struct ExposesLegacyProfileMutation<T, std::void_t<
    decltype(std::declval<T&>().SetAppProfiles(std::declval<HidHide::AppProfiles const&>())),
    decltype(std::declval<T&>().AppProfileAdd(std::declval<HidHide::FullImageName const&>())),
    decltype(std::declval<T&>().AppProfileDelete(std::declval<HidHide::FullImageName const&>())),
    decltype(std::declval<T&>().AppProfileAddEntry(std::declval<HidHide::FullImageName const&>(), std::declval<HidHide::DeviceInstancePath const&>())),
    decltype(std::declval<T&>().AppProfileDelEntry(std::declval<HidHide::FullImageName const&>(), std::declval<HidHide::DeviceInstancePath const&>()))>> : std::true_type {};

static_assert(!ExposesLegacyProfileMutation<HidHide::FilterDriverProxy>::value,
    "Ordinary UI/CLI code must not expose a ConfigurationV1 profile writer");

TEST(ProfilePresentation, ResidentEngineHasNoRetiredPageAndUsesUnicode)
{
    auto root=std::filesystem::path(__FILE__).parent_path().parent_path();
    for(auto relative:{L"HidHideClient/src/HidHideClientDlg.cpp"})
    {
        auto text=ReadBytes(root/relative);EXPECT_EQ(std::string::npos,text.find("SetTimer("));EXPECT_EQ(std::string::npos,text.find("ON_WM_TIMER("));
    }
    auto project=ReadBytes(root/L"HidHideClient/HidHideClient.vcxproj");EXPECT_EQ(std::string::npos,project.find("ProfilesPage"));EXPECT_EQ(std::string::npos,project.find("ProfilesView"));EXPECT_NE(std::string::npos,project.find("/utf-8"));
    auto resource=ReadBytes(root/L"HidHideClient/HidHideClient.rc");EXPECT_NE(std::string::npos,resource.find("#pragma code_page(65001)"));EXPECT_EQ(std::string::npos,resource.find("#pragma code_page(1252)"));
    auto manifest=ReadBytes(root/L"HidHideClient/Profiles.manifest");EXPECT_NE(std::string::npos,manifest.find("PerMonitorV2"));
}

namespace
{
    struct ActivationFixture
    {
        Snapshot snapshot;
        ActivationHistory history;
        std::wstring global{L"00000000-0000-0000-0000-000000000001"};
        std::wstring a{L"00000000-0000-0000-0000-000000000002"};
        std::wstring b{L"00000000-0000-0000-0000-000000000003"};
        std::wstring c{L"00000000-0000-0000-0000-000000000004"};
        ActivationFixture()
        {
            snapshot.settings = {1, global, Mode::Automatic, false, false, {}};
            snapshot.profiles.emplace(global, Global(global));
            snapshot.profiles.emplace(a, App(a,L"A",100,L"C:\\A.exe"));
            snapshot.profiles.emplace(b, App(b,L"B",2,L"C:\\B.exe"));
            snapshot.profiles.emplace(c, App(c,L"C",1,L"C:\\C.exe"));
        }
        ProcessObservation Process(std::wstring const& id, unsigned pid, std::uint64_t time)
        { auto path = snapshot.profiles.at(id).executable; return {pid,time,path.filename().native(),path,true}; }
        std::wstring Scan(std::vector<ProcessObservation> const& processes, bool complete=true)
        { history.Update(snapshot,processes,complete); return history.Select(snapshot).profileId; }
    };
}
TEST(ActivationHistory, NewestActivationFallsBackThroughAllStillRunningProfiles)
{
    ActivationFixture f; auto a=f.Process(f.a,1,10), b=f.Process(f.b,2,20), c=f.Process(f.c,3,30);
    EXPECT_EQ(f.a,f.Scan({a})); EXPECT_EQ(f.b,f.Scan({a,b})); EXPECT_EQ(f.c,f.Scan({a,b,c}));
    EXPECT_EQ(f.b,f.Scan({a,b})); EXPECT_EQ(f.a,f.Scan({a})); EXPECT_EQ(f.global,f.Scan({}));
}
TEST(ActivationHistory, InstancesEditsAndRefreshDoNotPromoteButNewLifetimeDoes)
{
    ActivationFixture f; auto a=f.Process(f.a,1,10), b=f.Process(f.b,2,20), extra=f.Process(f.a,3,30);
    EXPECT_EQ(f.b,f.Scan({a,b})); EXPECT_EQ(f.b,f.Scan({a,b,extra}));
    f.snapshot.profiles.at(f.a).priority=1000; f.snapshot.profiles.at(f.a).revision++;
    EXPECT_EQ(f.b,f.Scan({a,b,extra})); EXPECT_EQ(f.b,f.Scan({b,extra}));
    auto reused=f.Process(f.a,3,40); EXPECT_EQ(f.a,f.Scan({b,reused}));
}
TEST(ActivationHistory, RestartUsesCreationTimeThenPriorityThenStableId)
{
    ActivationFixture f; auto a=f.Process(f.a,1,10),b=f.Process(f.b,2,20),c=f.Process(f.c,3,20);
    EXPECT_EQ(f.b,f.Scan({c,a,b}));
    f.history=ActivationHistory{}; f.snapshot.profiles.at(f.b).priority=1;
    EXPECT_EQ(f.b,f.Scan({c,b,a}));
    f.history=ActivationHistory{}; a.lifetimeIdentity=21;
    EXPECT_EQ(f.a,f.Scan({b,c,a}));
}
TEST(ActivationHistory, UnsampledInstanceHandoffPreservesOrderAndPinUsingVerifiedExitTime)
{
    ActivationFixture f; auto a=f.Process(f.a,1,10), b=f.Process(f.b,2,20), a2=f.Process(f.a,3,30);
    f.Scan({a,b}); f.history.SetPin(f.a);
    // A2 started before A1 exited, although no snapshot ever contained both live.
    a.exitedAt=40;
    EXPECT_EQ(f.a,f.Scan({a,b,a2})); EXPECT_EQ(f.a,f.history.Pin());
    f.history.SetPin({}); EXPECT_EQ(f.b,f.Scan({b,a2}));
    f.history.SetPin(f.a); a2.exitedAt=50; auto restarted=f.Process(f.a,3,60);
    EXPECT_EQ(f.a,f.Scan({b,a2,restarted})); EXPECT_TRUE(f.history.Pin().empty());
    EXPECT_EQ(f.a,f.history.Running().front());
}
TEST(ActivationHistory, ExitEvidenceSurvivesIncompleteDiscoveryWithoutInventingActivation)
{
    ActivationFixture f; auto a=f.Process(f.a,1,10), b=f.Process(f.b,2,20), a2=f.Process(f.a,3,30);
    f.Scan({a,b}); f.history.SetPin(f.a); a.exitedAt=40;
    EXPECT_EQ(f.a,f.Scan({a,a2},false)); EXPECT_EQ(f.a,f.history.Pin());
    EXPECT_EQ(f.a,f.Scan({a,b,a2})); EXPECT_EQ(f.a,f.history.Pin());
    f.history.SetPin({}); EXPECT_EQ(f.b,f.Scan({b,a2}));
    a2.exitedAt=50; EXPECT_EQ(f.b,f.Scan({b,a2}));
    EXPECT_EQ(std::vector<std::wstring>{f.b},f.history.Running());
}
TEST(ActivationHistory, RetainedIntermediateLifetimeBridgesIncompleteScanHandoff)
{
    ActivationFixture f; auto a=f.Process(f.a,1,10), b=f.Process(f.b,2,20), a2=f.Process(f.a,3,30), a3=f.Process(f.a,4,50);
    f.Scan({a,b}); f.history.SetPin(f.a); a.exitedAt=40;
    f.Scan({a,a2},false); a2.exitedAt=60;
    EXPECT_EQ(f.a,f.Scan({a,a2,a3,b})); EXPECT_EQ(f.a,f.history.Pin());
    f.history.SetPin({}); EXPECT_EQ(f.b,f.Scan({a3,b}));
}
TEST(ActivationHistory, FirstCompleteActivationIncludesExitedOverlappingPredecessor)
{
    ActivationFixture f; auto a1=f.Process(f.a,1,50), c=f.Process(f.c,3,80), a2=f.Process(f.a,2,90);
    EXPECT_EQ(f.global,f.Scan({}));
    EXPECT_EQ(f.global,f.Scan({a1},false));
    a1.exitedAt=100;
    EXPECT_EQ(f.c,f.Scan({a1,a2,c}));
    EXPECT_EQ((std::vector<std::wstring>{f.c,f.a}),f.history.Running());
    EXPECT_EQ(f.a,f.Scan({a2}));
}
TEST(ActivationHistory, NewEpisodeUsesOverlappingPredecessorsWithoutBridgingInactiveGap)
{
    ActivationFixture f;
    auto old=f.Process(f.a,1,10), b=f.Process(f.b,2,40), a1=f.Process(f.a,3,50);
    auto a2=f.Process(f.a,4,70), c=f.Process(f.c,5,80), a3=f.Process(f.a,6,90);
    f.Scan({old}); f.history.SetPin(f.a); old.exitedAt=30;
    EXPECT_EQ(f.a,f.Scan({old,b,a1},false));
    a1.exitedAt=75;
    EXPECT_EQ(f.a,f.Scan({old,b,a1,a2,c},false));
    a2.exitedAt=100;
    // This ordering requires another backward pass to connect A1 to live A3.
    EXPECT_EQ(f.c,f.Scan({old,b,a1,a2,c,a3}));
    EXPECT_TRUE(f.history.Pin().empty());
    EXPECT_EQ((std::vector<std::wstring>{f.c,f.a,f.b}),f.history.Running());
    EXPECT_EQ(f.a,f.Scan({b,a3}));
    EXPECT_EQ(f.b,f.Scan({b}));
}
TEST(EditorSourceContract, TrayManualOverrideLabelPrecedesGenericApplicationLabel)
{
    auto root=std::filesystem::path(__FILE__).parent_path().parent_path();
    auto source=ReadBytes(root/L"HidHideClient/src/HidHideClientDlg.cpp");
    auto manual=source.find("SelectionReason::ManualApplication");
    auto generic=source.find("selected->second.kind == HidHide::Profiles::Kind::Application");
    ASSERT_NE(std::string::npos,manual); ASSERT_NE(std::string::npos,generic); EXPECT_LT(manual,generic);
}
TEST(ActivationHistory, PinTracksBackgroundHistoryAndExpiresOnLastInstanceExit)
{
    ActivationFixture f; auto a=f.Process(f.a,1,10),b=f.Process(f.b,2,20),c=f.Process(f.c,3,30),a2=f.Process(f.a,4,40);
    f.Scan({a,b}); f.history.SetPin(f.a);
    EXPECT_EQ(f.a,f.Scan({a,b,c,a2})); EXPECT_EQ(SelectionReason::ManualApplication,f.history.Select(f.snapshot).reason);
    EXPECT_EQ(f.a,f.Scan({b,c,a2})); f.history.SetPin({}); EXPECT_EQ(f.c,f.Scan({b,c,a2}));
    f.history.SetPin(f.a); EXPECT_EQ(f.c,f.Scan({b,c})); EXPECT_TRUE(f.history.Pin().empty());
    EXPECT_THROW(f.history.SetPin(f.a),std::invalid_argument);
}
TEST(ActivationHistory, IncompleteDiscoveryCannotExitOrPromoteAndCatalogInvalidatesPin)
{
    ActivationFixture f; auto a=f.Process(f.a,1,10),b=f.Process(f.b,2,20);
    f.Scan({a}); f.history.SetPin(f.a);
    EXPECT_EQ(f.a,f.Scan({b},false)); EXPECT_EQ(f.a,f.Scan({},false));
    EXPECT_EQ(f.a,f.Scan({a,b}));
    f.snapshot.profiles.at(f.a).enabled=false;
    EXPECT_EQ(f.b,f.Scan({},false)); EXPECT_TRUE(f.history.Pin().empty());
    f.snapshot.profiles.erase(f.b); EXPECT_EQ(f.global,f.Scan({},false));
}
TEST(ActivationHistory, GlobalAndPauseTrackHistoryClearPinAndPathChangesInvalidate)
{
    ActivationFixture f; auto a=f.Process(f.a,1,10),b=f.Process(f.b,2,20),c=f.Process(f.c,3,30);
    f.Scan({a}); f.history.SetPin(f.a); f.snapshot.settings.mode=Mode::UseGlobal;
    EXPECT_EQ(f.global,f.Scan({a,b})); EXPECT_TRUE(f.history.Pin().empty());
    f.snapshot.settings.mode=Mode::Automatic; EXPECT_EQ(f.b,f.Scan({a,b}));
    f.history.SetPin(f.a); f.snapshot.settings.paused=true; EXPECT_EQ(f.global,f.Scan({a,b,c}));
    f.snapshot.settings.paused=false; EXPECT_EQ(f.c,f.Scan({a,b,c}));
    f.history.SetPin(f.a); f.snapshot.profiles.at(f.a).executable=NormalizeExecutable(L"C:\\Different.exe");
    EXPECT_EQ(f.c,f.Scan({a,b,c})); EXPECT_TRUE(f.history.Pin().empty());
}
TEST(ActivationHistory, ExactPathAndLifetimeAreRequiredAndReusedPidCannotRetainPin)
{
    ActivationFixture f; auto a=f.Process(f.a,1,10); f.Scan({a}); f.history.SetPin(f.a);
    auto recycled=f.Process(f.a,1,30); EXPECT_EQ(f.a,f.Scan({recycled})); EXPECT_TRUE(f.history.Pin().empty());
    recycled.verifiedPath=L"D:\\A.exe"; EXPECT_EQ(f.global,f.Scan({recycled}));
    recycled=f.Process(f.a,1,0); EXPECT_EQ(f.global,f.Scan({recycled}));
}
TEST(ActivationHistory, IncompleteReusedPidDefersPinExpiryUntilCompleteReconciliation)
{
    ActivationFixture f; auto a=f.Process(f.a,1,10); f.Scan({a}); f.history.SetPin(f.a);
    auto reused=f.Process(f.a,1,20); reused.pathAccessible=false; reused.verifiedPath.clear();
    f.Scan({reused},false); EXPECT_EQ(f.a,f.history.Pin());
    reused=f.Process(f.a,1,20);
    EXPECT_EQ(f.a,f.Scan({reused})); EXPECT_TRUE(f.history.Pin().empty());
    EXPECT_EQ(SelectionReason::Application,f.history.Select(f.snapshot).reason);
}

// Spawned only with this exact filter by the real-handle cache stress test.
// No coordinator, device, driver, registry, or repository is touched.
TEST(ProcessLifetimeCache, ChildExitProbe) {}

TEST(ProcessLifetimeCache, IncompleteDiscoveryClosesRealExitedProcessesEveryScan)
{
    ActivationFixture f; ProcessLifetimeCache cache;
    std::vector<wchar_t> module(32768);
    auto size = ::GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    ASSERT_GT(size, 0u); ASSERT_LT(size, module.size());
    std::filesystem::path executable(std::wstring(module.data(), size));
    f.snapshot.profiles.at(f.a).executable = executable; cache.Retain(f.snapshot);
    DWORD handlesBefore{}; ASSERT_TRUE(::GetProcessHandleCount(::GetCurrentProcess(), &handlesBefore));
    for (unsigned i{}; i < 32; ++i)
    {
        auto command = L"\"" + executable.native() + L"\" --gtest_filter=ProcessLifetimeCache.ChildExitProbe";
        STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION child{};
        ASSERT_TRUE(::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child));
        FILETIME created{}, exited{}, kernel{}, user{};
        auto known = ::GetProcessTimes(child.hProcess, &created, &exited, &kernel, &user);
        ULARGE_INTEGER time{}; time.LowPart = created.dwLowDateTime; time.HighPart = created.dwHighDateTime;
        auto process = child.hProcess;
        cache.Remember(process, {child.dwProcessId, time.QuadPart, executable.filename().native(), executable, true});
        auto resumed = ::ResumeThread(child.hThread); ::CloseHandle(child.hThread);
        auto wait = ::WaitForSingleObject(process, 10000);
        if (wait != WAIT_OBJECT_0) { ::TerminateProcess(process, ERROR_CANCELLED); ::WaitForSingleObject(process, 5000); }
        ASSERT_TRUE(known); ASSERT_EQ(1u, resumed); ASSERT_EQ(WAIT_OBJECT_0, wait);
        // A persistently inaccessible candidate keeps discovery incomplete.
        std::vector<ProcessObservation> sample{{99999, 0, executable.filename().native(), {}, false}};
        ASSERT_TRUE(cache.Collect(sample));
        f.history.Update(f.snapshot, sample, false);
        EXPECT_EQ(0u, cache.HandleCount()); EXPECT_LE(sample.size(), 2u);
        EXPECT_EQ(f.global, f.history.Select(f.snapshot).profileId);
    }
    DWORD handlesAfter{}; ASSERT_TRUE(::GetProcessHandleCount(::GetCurrentProcess(), &handlesAfter));
    EXPECT_LE(handlesAfter, handlesBefore + 2);
    EXPECT_GT(cache.IntervalCount(), 0u);
    // Incomplete scans do not enumerate the retained exit intervals again.
    std::vector<ProcessObservation> sample; EXPECT_TRUE(cache.Collect(sample)); EXPECT_TRUE(sample.empty());
    cache.AppendExited(sample); EXPECT_FALSE(sample.empty()); cache.ConsumeExited(); EXPECT_EQ(0u, cache.IntervalCount());
}

TEST(ProcessLifetimeCache, IrrelevantDisabledDeletedAndChangedPathsReleaseHandlesImmediately)
{
    ActivationFixture f; ProcessLifetimeCache cache; cache.Retain(f.snapshot);
    auto remember = [&](ProcessObservation const& observation)
    {
        HANDLE retained{};
        EXPECT_TRUE(::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentProcess(), ::GetCurrentProcess(),
            &retained, 0, FALSE, DUPLICATE_SAME_ACCESS));
        cache.Remember(retained, observation);
    };
    DWORD before{}; ASSERT_TRUE(::GetProcessHandleCount(::GetCurrentProcess(), &before));
    auto irrelevant = f.Process(f.a, 1, 10); irrelevant.verifiedPath = L"D:\\unrelated\\A.exe";
    for (unsigned i{}; i < 4096; ++i) { irrelevant.processId = i + 1; remember(irrelevant); }
    EXPECT_EQ(0u, cache.HandleCount());
    auto a = f.Process(f.a, 1, 10), b = f.Process(f.b, 2, 20), c = f.Process(f.c, 3, 30);
    remember(a); remember(b); remember(c); EXPECT_EQ(3u, cache.HandleCount());
    a.exitedAt = 40; cache.RememberExit(a);
    f.snapshot.profiles.at(f.a).enabled = false; f.snapshot.profiles.erase(f.b);
    f.snapshot.profiles.at(f.c).executable = L"D:\\different\\C.exe";
    cache.Retain(f.snapshot); EXPECT_EQ(0u, cache.HandleCount()); EXPECT_EQ(0u, cache.IntervalCount());
    DWORD after{}; ASSERT_TRUE(::GetProcessHandleCount(::GetCurrentProcess(), &after)); EXPECT_LE(after, before + 2);
}

TEST(ProcessLifetimeCache, OverlappingExitsCompactAndDisjointEpisodesRemainDistinct)
{
    ActivationFixture f; ProcessLifetimeCache cache; cache.Retain(f.snapshot);
    for (unsigned i{}; i < 10000; ++i)
    {
        auto process = f.Process(f.a, i + 1, i + 10); process.exitedAt = i + 20;
        cache.RememberExit(process); EXPECT_EQ(1u, cache.IntervalCount());
    }
    auto separate = f.Process(f.a, 20000, 20000); separate.exitedAt = 20010; cache.RememberExit(separate);
    EXPECT_EQ(2u, cache.IntervalCount());
    std::vector<ProcessObservation> evidence; cache.AppendExited(evidence);
    ASSERT_EQ(2u, evidence.size()); EXPECT_EQ(0u, evidence.front().processId);
    EXPECT_EQ(10u, evidence.front().lifetimeIdentity); EXPECT_EQ(10019u, evidence.front().exitedAt);
}

TEST(ProcessLifetimeCache, CompactedEvidencePreservesHandoffsAndDoesNotBridgeAnInactiveGap)
{
    ActivationFixture f; ProcessLifetimeCache cache; cache.Retain(f.snapshot);
    auto old = f.Process(f.a, 1, 10), b = f.Process(f.b, 2, 20);
    f.Scan({old, b}); f.history.SetPin(f.a);
    auto middle = f.Process(f.a, 3, 30); old.exitedAt = 40; middle.exitedAt = 60;
    cache.RememberExit(old); cache.RememberExit(middle);
    std::vector<ProcessObservation> sample{b, f.Process(f.a, 4, 50)}; cache.AppendExited(sample);
    EXPECT_EQ(f.a, f.Scan(sample)); EXPECT_EQ(f.a, f.history.Pin());
    f.history.SetPin({}); EXPECT_EQ(f.b, f.Scan(sample)); cache.ConsumeExited();
    auto ended = f.Process(f.a, 4, 50); ended.exitedAt = 70; cache.RememberExit(ended);
    auto next = f.Process(f.a, 5, 80); next.exitedAt = 100; cache.RememberExit(next);
    f.history.SetPin(f.a);
    sample = {b, f.Process(f.c, 6, 90), f.Process(f.a, 7, 95)}; cache.AppendExited(sample);
    EXPECT_EQ(f.c, f.Scan(sample)); EXPECT_TRUE(f.history.Pin().empty());
    EXPECT_EQ((std::vector<std::wstring>{f.c, f.a, f.b}), f.history.Running());
}

TEST(ProcessLifetimeCache, RecycledPidDuringIncompleteHandoffPreservesPinUntilReconciliation)
{
    ActivationFixture f; ProcessLifetimeCache cache; cache.Retain(f.snapshot);
    auto old=f.Process(f.a,1,10), b=f.Process(f.b,3,15), successor=f.Process(f.a,2,20);
    auto recycled=f.Process(f.c,1,40);
    f.Scan({old,b}); f.history.SetPin(f.a);
    // Collect reports the exact exit once and retains its compact interval.
    old.exitedAt=30; cache.RememberExit(old);
    std::vector<ProcessObservation> sample{old,b,successor};
    ASSERT_TRUE(cache.Collect(sample)); f.Scan(sample,false);
    EXPECT_EQ(f.a,f.history.Pin()); EXPECT_EQ(1u,cache.IntervalCount());
    // Later incomplete scans no longer include the retired PID's exit.
    sample={b,successor,recycled}; ASSERT_TRUE(cache.Collect(sample));
    ASSERT_EQ(3u,sample.size()); EXPECT_EQ(f.a,f.Scan(sample,false)); EXPECT_EQ(f.a,f.history.Pin());
    cache.AppendExited(sample);
    EXPECT_EQ(f.a,f.Scan(sample)); EXPECT_EQ(f.a,f.history.Pin());
    EXPECT_EQ(SelectionReason::ManualApplication,f.history.Select(f.snapshot).reason);
    EXPECT_EQ((std::vector<std::wstring>{f.c,f.b,f.a}),f.history.Running());
    cache.ConsumeExited(); EXPECT_EQ(0u,cache.IntervalCount());
    f.history.SetPin({}); EXPECT_EQ(f.b,f.Scan({b,successor}));
}

TEST(ProcessLifetimeCache, RecycledPidDuringIncompleteInactiveGapExpiresPinAfterReconciliation)
{
    ActivationFixture f; ProcessLifetimeCache cache; cache.Retain(f.snapshot);
    auto old=f.Process(f.a,1,10), b=f.Process(f.b,3,15), successor=f.Process(f.a,2,35);
    auto recycled=f.Process(f.c,1,40);
    f.Scan({old,b}); f.history.SetPin(f.a);
    old.exitedAt=30; cache.RememberExit(old);
    std::vector<ProcessObservation> sample{old,b,successor};
    ASSERT_TRUE(cache.Collect(sample)); f.Scan(sample,false); EXPECT_EQ(f.a,f.history.Pin());
    sample={b,successor,recycled}; ASSERT_TRUE(cache.Collect(sample));
    ASSERT_EQ(3u,sample.size()); EXPECT_EQ(f.a,f.Scan(sample,false)); EXPECT_EQ(f.a,f.history.Pin());
    cache.AppendExited(sample);
    EXPECT_EQ(f.c,f.Scan(sample)); EXPECT_TRUE(f.history.Pin().empty());
    EXPECT_EQ(SelectionReason::Application,f.history.Select(f.snapshot).reason);
    EXPECT_EQ((std::vector<std::wstring>{f.c,f.a,f.b}),f.history.Running());
    cache.ConsumeExited(); EXPECT_EQ(f.a,f.Scan({b,successor}));
}
