// SPDX-License-Identifier: MIT
#pragma once

#include "ProfileJson.h"
#include <Windows.h>
#include <bcrypt.h>
#include <ShlObj.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <mutex>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace HidHide::Profiles
{
    class RepositoryConflict : public std::runtime_error { public: using std::runtime_error::runtime_error; };

    struct RepositoryIssue { std::filesystem::path file; std::wstring message; };
    struct SavedVersion { std::uint64_t revision{}; std::wstring sha256; };
    struct LoadResult
    {
        Snapshot snapshot;
        std::vector<RepositoryIssue> issues;
        std::map<std::wstring, SavedVersion> profileVersions;
        SavedVersion settingsVersion;
    };
    constexpr std::size_t MaxRepositoryIssues{ 64 };

    inline std::wstring NewStableId()
    {
        GUID guid{}; if (FAILED(::CoCreateGuid(&guid))) throw std::runtime_error("Could not create profile id");
        wchar_t text[39]{}; if (::StringFromGUID2(guid, text, 39) != 39) throw std::runtime_error("Could not format profile id");
        std::wstring id(text + 1, text + 37);
        std::transform(id.begin(), id.end(), id.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return id;
    }

    inline std::string ReadBytes(std::filesystem::path const& path)
    {
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::system_error(::GetLastError(), std::system_category(), "Open repository file");
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > static_cast<LONGLONG>(Json::MaxBytes))
        { auto e = ::GetLastError(); ::CloseHandle(file); throw std::system_error(e ? e : ERROR_FILE_TOO_LARGE, std::system_category(), "Repository file size"); }
        std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0'); DWORD read{};
        bool ok = bytes.empty() || (::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size());
        auto error = ok ? ERROR_SUCCESS : ::GetLastError(); ::CloseHandle(file);
        if (!ok) throw std::system_error(error, std::system_category(), "Read repository file"); return bytes;
    }

    inline std::wstring Sha256(std::string const& bytes)
    {
        BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{}; DWORD objectSize{}, received{}; std::vector<UCHAR> object; UCHAR digest[32]{};
        auto Check = [](NTSTATUS status) { if (status < 0) throw std::runtime_error("SHA-256 operation failed"); };
        Check(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        try
        {
            Check(::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &received, 0));
            object.resize(objectSize); Check(::BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0));
            if (!bytes.empty()) Check(::BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0));
            Check(::BCryptFinishHash(hash, digest, sizeof(digest), 0)); ::BCryptDestroyHash(hash); ::BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        catch (...) { if (hash) ::BCryptDestroyHash(hash); if (algorithm) ::BCryptCloseAlgorithmProvider(algorithm, 0); throw; }
        std::wostringstream out; out << std::hex << std::setfill(L'0'); for (auto byte : digest) out << std::setw(2) << static_cast<unsigned>(byte); return out.str();
    }

    // Serializes one repository across threads, processes and sessions of the
    // same user. The lock is an exclusive "<repository>.writer.lock" file next to
    // the repository directory, in the user's own profile: unlike a predictable
    // Global object name, other users cannot create or deny it. Exclusivity is the
    // open handle, so the lease is freed if a holder dies. The empty file itself
    // is created once and then left in place: the coordinator watches the
    // repository's parent directory recursively, and creating or deleting any
    // file there on each read would trigger a reload that reads again, forever.
    // Opening an existing file raises no change notification.
    // Nested leases on one thread are reentrant, like the mutex this replaces.
    class WriterLease
    {
        static std::map<std::wstring, std::pair<HANDLE, unsigned>>& Held()
        { thread_local std::map<std::wstring, std::pair<HANDLE, unsigned>> held; return held; }
        std::wstring m_Key;
    public:
        explicit WriterLease(std::filesystem::path const& root)
        {
            auto canonical = std::filesystem::absolute(root).lexically_normal();
            if (!canonical.has_filename()) canonical = canonical.parent_path();
            m_Key = canonical.native(); std::transform(m_Key.begin(), m_Key.end(), m_Key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            auto& held = Held();
            if (auto it = held.find(m_Key); it != held.end()) { ++it->second.second; return; }
            // The lock lives beside the repository. Taking a lease creates no
            // directory: an absent catalog, or an absent parent that someone is
            // about to put back, is a state the coordinator reports and must
            // not disturb. Writers create the repository before they lease it.
            auto path = canonical.parent_path() / (canonical.filename().native() + L".writer.lock"); auto deadline = ::GetTickCount64() + 10000;
            HANDLE file{};
            while ((file = ::CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_ALWAYS,
                FILE_ATTRIBUTE_HIDDEN, nullptr)) == INVALID_HANDLE_VALUE)
            {
                // A sharing violation means another writer holds the lease. The
                // file is never deleted, so any other failure is not transient.
                auto error = ::GetLastError();
                // Without a parent directory there is no repository to serialize
                // against. This lease is empty and is not recorded as held, so a
                // writer nested under it still takes the real lock.
                if (error == ERROR_PATH_NOT_FOUND) { m_Key.clear(); return; }
                if (error != ERROR_SHARING_VIOLATION || ::GetTickCount64() >= deadline)
                    throw std::system_error(error == ERROR_SHARING_VIOLATION ? ERROR_BUSY : error, std::system_category(), "Acquire repository writer lease");
                ::Sleep(15);
            }
            try { held.emplace(m_Key, std::make_pair(file, 1u)); } catch (...) { ::CloseHandle(file); throw; }
        }
        ~WriterLease()
        {
            auto& held = Held(); auto it = held.find(m_Key);
            if (it != held.end() && --it->second.second == 0) { ::CloseHandle(it->second.first); held.erase(it); }
        }
        WriterLease(WriterLease const&) = delete; WriterLease& operator=(WriterLease const&) = delete;
    };

    constexpr std::size_t MaxTransactionItems{ MaxProfiles * 2 + 1 };

    class RepositoryView
    {
    public:
        explicit RepositoryView(std::filesystem::path root) : m_Root(std::filesystem::absolute(std::move(root)).lexically_normal()) {}
        std::filesystem::path const& Root() const { return m_Root; }
        LoadResult Load() const
        {
            WriterLease lease(m_Root);
            if (TransactionPendingLocked()) throw std::runtime_error("Profile repository recovery is required");
            return LoadLocked(false);
        }
        LoadResult Inspect() const
        {
            WriterLease lease(m_Root);
            if (!std::filesystem::exists(m_Root / L"settings.json")) return UninitializedInspection(L"Profile catalog is not initialized; activation is blocked and existing evidence was preserved");
            auto result = LoadLocked(false);
            if (TransactionPendingLocked())
                AddIssue(result, m_Root / L".transaction.intent", L"A repository transaction requires recovery; activation is blocked and all evidence was preserved");
            return result;
        }
        SavedVersion Version(std::wstring const& id) const
        {
            if (!IsStableId(id)) throw std::invalid_argument("Profile id is not a canonical GUID");
            WriterLease lease(m_Root); return VersionLocked(id);
        }
        SavedVersion SettingsVersion() const
        {
            WriterLease lease(m_Root); return SettingsVersionLocked();
        }
        // True only for a root that holds no catalog file at all, as opposed to
        // an incomplete or corrupt catalog whose files are recovery evidence.
        bool CatalogUninitialized() const
        {
            WriterLease lease(m_Root); return !std::filesystem::exists(m_Root / L"settings.json") && !HasCatalogEvidenceLocked();
        }
    protected:
        // Names the commit protocol generates for staging files:
        // ".<transaction>.<ordinal>" with .tmp, .bak or .restore, and the
        // unpublished ".new" record files. They can never name a catalog file.
        static bool IsStagingOrphanName(std::wstring const& name)
        {
            if (name == L".transaction.intent.new" || name == L".transaction.committed.new") return true;
            if (name.size() < 43 || name[0] != L'.' || name[37] != L'.') return false;
            auto extension = std::filesystem::path(name).extension().native();
            auto ordinal = name.substr(38, name.size() - 38 - extension.size());
            return IsStableId(name.substr(1, 36)) && !ordinal.empty() && std::all_of(ordinal.begin(), ordinal.end(), ::iswdigit)
                && (extension == L".tmp" || extension == L".bak" || extension == L".restore");
        }
        bool HasCatalogEvidenceLocked() const
        {
            // Reads never create the repository directory; an absent one holds no evidence.
            // Staging files orphaned by a failed first save are not evidence
            // either: the next writer sweeps them, and seeding must stay possible.
            std::error_code ec; if (!std::filesystem::exists(m_Root, ec) && !ec) return false;
            for (auto const& entry : std::filesystem::directory_iterator(m_Root, ec))
                if (entry.is_regular_file() && !IsStagingOrphanName(entry.path().filename().native())) return true;
            if (ec) throw std::system_error(ec, "Inspect profile repository before initialization");
            return false;
        }
        struct IntentItem { std::wstring fileName, tempName, backupName, sha256, oldSha256; bool existed{}, remove{}; };
        struct ParsedIntent { std::wstring transaction; std::vector<IntentItem> items; };
        static bool IsDigest(std::wstring const& value, bool emptyAllowed = false)
        {
            if (emptyAllowed && value.empty()) return true;
            if (value.size() != 64) return false;
            return std::all_of(value.begin(), value.end(), [](wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'); });
        }
        static bool IsTargetName(std::wstring const& name)
        {
            if (name == L"settings.json") return true;
            return name.size() == 41 && name.substr(36) == L".json" && IsStableId(name.substr(0, 36));
        }
        static ParsedIntent ParseIntent(std::string const& bytes)
        {
            auto text = Json::FromUtf8(bytes); std::wistringstream in(text); std::wstring line;
            if (!std::getline(in, line) || line.rfind(L"HIDHIDE-PROFILES-TRANSACTION-1\t", 0) != 0) throw std::runtime_error("Invalid repository transaction record");
            ParsedIntent result; result.transaction = line.substr(31); if (!IsStableId(result.transaction)) throw std::runtime_error("Invalid repository transaction id");
            std::set<std::wstring> targets;
            while (std::getline(in, line))
            {
                if (line.empty()) continue; std::vector<std::wstring> fields; std::size_t start{};
                for (std::size_t i{}; i <= line.size(); ++i) if (i == line.size() || line[i] == L'\t') { fields.emplace_back(line.substr(start, i - start)); start = i + 1; }
                auto index = result.items.size(); auto expectedTemp = L"." + result.transaction + L"." + std::to_wstring(index) + L".tmp"; auto expectedBackup = L"." + result.transaction + L"." + std::to_wstring(index) + L".bak";
                if (fields.size() != 7 || (fields[0] != L"0" && fields[0] != L"1") || (fields[1] != L"0" && fields[1] != L"1")
                    || !IsTargetName(fields[2]) || fields[3] != expectedTemp || fields[4] != expectedBackup || !IsDigest(fields[5], fields[1] == L"1")
                    || !IsDigest(fields[6], fields[0] == L"0")) throw std::runtime_error("Invalid repository transaction item");
                result.items.push_back({ fields[2], fields[3], fields[4], fields[5], fields[6], fields[0] == L"1", fields[1] == L"1" });
                if (!targets.emplace(fields[2]).second) throw std::runtime_error("Duplicate repository transaction target");
                if (result.items.size() > MaxTransactionItems) throw std::runtime_error("Repository transaction is too large");
            }
            if (result.items.empty()) throw std::runtime_error("Empty repository transaction"); return result;
        }
        // An intent blocks loading unless its matching commit marker is durable
        // and every target verifies: then only staging cleanup remains (for
        // example a locked backup), which the next writer finishes.
        bool TransactionPendingLocked() const
        {
            auto intentPath = m_Root / L".transaction.intent"; auto committedPath = m_Root / L".transaction.committed";
            if (!std::filesystem::exists(intentPath)) return false;
            try
            {
                auto parsed = ParseIntent(ReadBytes(intentPath));
                if (!std::filesystem::exists(committedPath) || Json::FromUtf8(ReadBytes(committedPath)) != parsed.transaction) return true;
                for (auto const& item : parsed.items)
                {
                    auto target = m_Root / item.fileName;
                    if (item.remove ? std::filesystem::exists(target) : (!std::filesystem::exists(target) || Sha256(ReadBytes(target)) != item.sha256)) return true;
                }
                return false;
            }
            catch (...) { return true; }
        }
        LoadResult UninitializedInspection(std::wstring message) const
        {
            LoadResult result; Profile profile; profile.id = L"00000000-0000-0000-0000-000000000001"; profile.name = L"Unsaved recovery view";
            profile.kind = Kind::Global; profile.enabled = true; result.snapshot.profiles.emplace(profile.id, profile); result.snapshot.settings.selectedGlobalId = profile.id;
            AddIssue(result, m_Root, std::move(message)); return result;
        }
        static void AddIssue(LoadResult& result, std::filesystem::path const& file, std::wstring message)
        {
            if (result.issues.size() < MaxRepositoryIssues - 1) result.issues.push_back({ file, std::move(message) });
            else if (result.issues.size() == MaxRepositoryIssues - 1)
                result.issues.push_back({ result.snapshot.settings.selectedGlobalId.empty() ? file.parent_path() : file,
                    L"Additional repository diagnostics were omitted to keep startup bounded" });
        }
        LoadResult LoadLocked(bool requireValid = true) const
        {
            LoadResult result;
            auto settingsPath = m_Root / L"settings.json";
            if (!std::filesystem::exists(settingsPath)) throw std::runtime_error("Profile repository is not initialized");
            auto settingsBytes = ReadBytes(settingsPath); bool settingsParsed{ true };
            try { result.snapshot.settings = ParseSettings(settingsBytes); }
            catch (std::exception const& error)
            {
                if (requireValid) throw;
                settingsParsed = false; AddIssue(result, settingsPath, std::wstring(error.what(), error.what() + strlen(error.what())));
            }
            auto MixDigest = [](std::uint64_t value, std::wstring const& digest)
            {
                for (auto c : digest) { value ^= static_cast<std::uint16_t>(c); value *= 1099511628211ull; }
                return value;
            };
            auto settingsDigest = Sha256(settingsBytes);
            if (settingsParsed) result.settingsVersion = { result.snapshot.settings.revision, settingsDigest };
            std::vector<std::pair<std::wstring, std::wstring>> manifest{ { L"settings.json", settingsDigest } };
            std::error_code ec; std::size_t candidates{};
            for (auto const& entry : std::filesystem::directory_iterator(m_Root, ec))
            {
                if (ec) break; if (!entry.is_regular_file() || entry.path().extension() != L".json" || entry.path().filename() == L"settings.json") continue;
                if (++candidates > MaxProfiles)
                {
                    AddIssue(result, m_Root, L"Repository contains more than the supported number of profile files; remaining files were preserved and not parsed");
                    break;
                }
                try
                {
                    auto bytes = ReadBytes(entry.path());
                    auto profile = ParseProfile(bytes);
                    if (entry.path().stem().native() != profile.id) throw std::runtime_error("Filename does not match profile id");
                    auto id = profile.id; auto revision = profile.revision;
                    if (!result.snapshot.profiles.emplace(id, std::move(profile)).second) throw std::runtime_error("Duplicate profile id");
                    auto digest = Sha256(bytes);
                    result.profileVersions.emplace(id, SavedVersion{ revision, digest });
                    manifest.emplace_back(entry.path().filename().native(), std::move(digest));
                }
                catch (std::exception const& e) { AddIssue(result, entry.path(), std::wstring(e.what(), e.what() + strlen(e.what()))); }
            }
            if (ec) throw std::system_error(ec, "Enumerate profile repository");
            if (!settingsParsed)
            {
                auto fallback = std::find_if(result.snapshot.profiles.begin(), result.snapshot.profiles.end(), [](auto const& item) { return item.second.kind == Kind::Global && item.second.enabled; });
                if (fallback != result.snapshot.profiles.end()) result.snapshot.settings.selectedGlobalId = fallback->first;
            }
            try { Validate(result.snapshot); }
            catch (std::exception const& error)
            {
                if (requireValid) throw;
                AddIssue(result, settingsPath, std::wstring(error.what(), error.what() + strlen(error.what())));
            }
            std::sort(manifest.begin(), manifest.end());
            std::uint64_t generation{ 1469598103934665603ull };
            for (auto const& [name, digest] : manifest) { generation = MixDigest(generation, name); generation = MixDigest(generation, digest); }
            result.snapshot.generation = generation; return result;
        }
        SavedVersion VersionLocked(std::wstring const& id) const
        {
            auto bytes = ReadBytes(m_Root / (id + L".json")); auto profile = ParseProfile(bytes); return { profile.revision, Sha256(bytes) };
        }
        SavedVersion SettingsVersionLocked() const
        {
            auto bytes = ReadBytes(m_Root / L"settings.json"); auto settings = ParseSettings(bytes); return { settings.revision, Sha256(bytes) };
        }
        LoadResult LoadValidLocked() const
        {
            auto loaded = LoadLocked(); if (!loaded.issues.empty()) throw RepositoryConflict("Profile repository contains invalid or unsupported files; normal mutation is blocked"); return loaded;
        }
        std::filesystem::path m_Root;
    };

    // *PartiallyWritten fire halfway through writing a transaction record, so a
    // failure there leaves exactly what a crash during that write would.
    enum class CommitBoundary { TempFlushed, IntentFlushed, TargetReplaced, SetVerified, CommitMarked, RecoveryTargetRestored, RecoveryVerified, Completed,
        IntentPartiallyWritten, CommitMarkPartiallyWritten };

    class ProfileRepository : public RepositoryView
    {
    public:
        using FailureHook = std::function<void(CommitBoundary, std::size_t)>;
        explicit ProfileRepository(std::filesystem::path root, FailureHook failureHook = {})
            : RepositoryView(std::move(root)), m_FailureHook(std::move(failureHook)) {}

        LoadResult OpenOrCreate(std::set<std::filesystem::path> initialAllowedApplications = {})
        { return OpenOrCreateObserved(std::move(initialAllowedApplications)); }
        LoadResult OpenOrCreateObserved(std::optional<std::set<std::filesystem::path>> initialAllowedApplications)
        {
            EnsureDirectory(); WriterLease lease(m_Root);
            try { RecoverLocked(); }
            catch (std::exception const& error)
            {
                if (!std::filesystem::exists(m_Root / L"settings.json"))
                    return UninitializedInspection(L"Repository recovery is blocked: " + std::wstring(error.what(), error.what() + strlen(error.what())));
                auto result = LoadLocked(false);
                AddIssue(result, m_Root / L".transaction.intent", L"Repository recovery is blocked: " + std::wstring(error.what(), error.what() + strlen(error.what())));
                return result;
            }
            if (!std::filesystem::exists(m_Root / L"settings.json"))
            {
                if (HasCatalogEvidenceLocked()) return UninitializedInspection(L"Profile repository is incomplete: settings.json is missing; existing files were preserved");
                if (!initialAllowedApplications) return UninitializedInspection(L"Fresh profile repository initialization requires a successful, conflict-free observed Allowed-app list; no catalog was written");
                Profile profile; profile.id = NewStableId(); profile.revision = 1; profile.name = L"Default"; profile.kind = Kind::Global;
                Settings settings; settings.revision = 1; settings.selectedGlobalId = profile.id; settings.allowedApplications = std::move(*initialAllowedApplications);
                Snapshot candidate; candidate.profiles.emplace(profile.id, profile); candidate.settings = settings; Validate(candidate);
                CommitLocked({ { profile.id + L".json", SerializeProfile(profile), false }, { L"settings.json", SerializeSettings(settings), false } });
            }
            return LoadLocked(false);
        }

        SavedVersion Apply(Profile draft, std::optional<SavedVersion> expected, std::optional<Settings> changedSettings = std::nullopt,
            std::optional<SavedVersion> expectedSettings = std::nullopt)
        {
            if (!IsStableId(draft.id)) throw std::invalid_argument("Profile id is not a canonical GUID");
            EnsureDirectory(); WriterLease lease(m_Root); RecoverLocked();
            auto current = LoadValidLocked(); auto it = current.snapshot.profiles.find(draft.id); auto target = m_Root / (draft.id + L".json");
            if (expected)
            {
                if (it == current.snapshot.profiles.end()) throw RepositoryConflict("Profile was deleted outside this window");
                auto actual = VersionLocked(draft.id); if (actual.revision != expected->revision || actual.sha256 != expected->sha256) throw RepositoryConflict("Profile changed outside this window");
                if (expected->revision == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Profile revision is exhausted");
                draft.revision = expected->revision + 1;
            }
            else
            {
                if (it != current.snapshot.profiles.end() || std::filesystem::exists(target)) throw RepositoryConflict("Profile id or target file already exists"); draft.revision = 1;
            }
            std::vector<Change> changes{ { draft.id + L".json", SerializeProfile(draft), false } };
            if (changedSettings)
            {
                auto actualSettings = SettingsVersionLocked();
                if (!expectedSettings || actualSettings.revision != expectedSettings->revision || actualSettings.sha256 != expectedSettings->sha256)
                    throw RepositoryConflict("Settings changed outside this window");
                if (expectedSettings->revision == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Settings revision is exhausted");
                changedSettings->revision = expectedSettings->revision + 1; changes.push_back({ L"settings.json", SerializeSettings(*changedSettings), false });
            }
            auto candidate = current.snapshot; candidate.profiles[draft.id] = draft; if (changedSettings) candidate.settings = *changedSettings; Validate(candidate);
            CommitLocked(changes); return VersionLocked(draft.id);
        }

        void Delete(std::wstring const& id, SavedVersion expected, Settings changedSettings, SavedVersion expectedSettings)
        {
            if (!IsStableId(id)) throw std::invalid_argument("Profile id is not a canonical GUID");
            WriterLease lease(m_Root); RecoverLocked(); auto loaded = LoadValidLocked(); auto it = loaded.snapshot.profiles.find(id);
            if (it == loaded.snapshot.profiles.end()) throw RepositoryConflict("Profile was already deleted");
            auto actual = VersionLocked(id); if (actual.revision != expected.revision || actual.sha256 != expected.sha256) throw RepositoryConflict("Profile changed outside this window");
            if (it->second.kind == Kind::Global)
            {
                auto globals = std::count_if(loaded.snapshot.profiles.begin(), loaded.snapshot.profiles.end(), [](auto const& item) { return item.second.kind == Kind::Global; });
                if (globals <= 1 || changedSettings.selectedGlobalId == id) throw std::invalid_argument("Cannot delete the last or selected Global profile");
            }
            auto actualSettings = SettingsVersionLocked();
            if (actualSettings.revision != expectedSettings.revision || actualSettings.sha256 != expectedSettings.sha256)
                throw RepositoryConflict("Settings changed outside this window");
            if (expectedSettings.revision == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Settings revision is exhausted");
            changedSettings.revision = expectedSettings.revision + 1;
            auto candidate = loaded.snapshot; candidate.profiles.erase(id); candidate.settings = changedSettings; Validate(candidate);
            CommitLocked({ { id + L".json", {}, true }, { L"settings.json", SerializeSettings(changedSettings), false } });
        }

        SavedVersion ApplySettings(Settings draft, SavedVersion expected)
        {
            EnsureDirectory(); WriterLease lease(m_Root); RecoverLocked();
            auto actual = SettingsVersionLocked(); if (actual.revision != expected.revision || actual.sha256 != expected.sha256) throw RepositoryConflict("Settings changed outside this window");
            if (expected.revision == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Settings revision is exhausted");
            auto candidate = LoadValidLocked().snapshot; draft.revision = expected.revision + 1; candidate.settings = draft; Validate(candidate);
            CommitLocked({ { L"settings.json", SerializeSettings(draft), false } }); return SettingsVersionLocked();
        }

        void ExportProfile(std::wstring const& id, std::filesystem::path const& destination) const
        {
            if (!IsStableId(id)) throw std::invalid_argument("Profile id is not a canonical GUID");
            WriterLease lease(m_Root);
            if (std::filesystem::exists(m_Root / L".transaction.intent")) throw std::runtime_error("Profile repository recovery is required");
            EnsureExternalDestination(destination);
            auto source = m_Root / (id + L".json"); auto bytes = ReadBytes(source); ParseProfile(bytes);
            WriteExternalDurable(destination, bytes);
        }

        void Backup(std::filesystem::path const& destination) const
        {
            WriterLease lease(m_Root);
            if (std::filesystem::exists(m_Root / L".transaction.intent")) throw std::runtime_error("Profile repository recovery is required");
            EnsureExternalDestination(destination);
            if (std::filesystem::exists(destination)) throw std::runtime_error("Backup destination already exists");
            auto snapshot = LoadLocked();
            if (!snapshot.issues.empty()) throw std::runtime_error("Backup refused because the repository contains invalid files; raw evidence was preserved in place");
            std::error_code ec; std::filesystem::create_directories(destination, ec); if (ec) throw std::system_error(ec, "Create backup directory");
            for (auto const& [id, profile] : snapshot.snapshot.profiles)
            {
                (void)profile; auto bytes = ReadBytes(m_Root / (id + L".json")); WriteDurable(destination / (id + L".json"), bytes);
            }
            auto settings = ReadBytes(m_Root / L"settings.json"); WriteDurable(destination / L"settings.json", settings);
            std::wostringstream manifest; manifest << L"{\n  \"schemaVersion\": 1,\n  \"format\": \"HidHide Profiles portable backup\",\n  \"profileCount\": " << snapshot.snapshot.profiles.size()
                << L",\n  \"excludedRuntimeState\": [\"process state\", \"device connection state\", \"observed driver state\"]\n}\n";
            WriteDurable(destination / L"manifest.json", Json::ToUtf8(manifest.str()));
        }

        LoadResult RestoreBackup(std::filesystem::path const& source)
        {
            EnsureDirectory(); WriterLease lease(m_Root); RecoverLocked();
            EnsureExternalDestination(source);
            auto attributes = ::GetFileAttributesW(source.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                throw std::invalid_argument("Backup source must be a real directory outside the profile repository");

            auto manifestValue = Json::Parser(Json::FromUtf8(ReadBytes(source / L"manifest.json"))).Parse();
            auto const& manifest = Json::AsObject(manifestValue);
            Json::ExactMembers(manifest, { L"schemaVersion", L"format", L"profileCount", L"excludedRuntimeState" });
            if (Json::AsUnsigned(Json::Required(manifest, L"schemaVersion")) != 1
                || Json::AsString(Json::Required(manifest, L"format")) != L"HidHide Profiles portable backup")
                throw std::runtime_error("Unsupported profile backup manifest");
            auto expectedCount = Json::AsUnsigned(Json::Required(manifest, L"profileCount"));
            auto const& exclusions = Json::AsArray(Json::Required(manifest, L"excludedRuntimeState"));
            if (exclusions.size() != 3 || Json::AsString(exclusions[0]) != L"process state"
                || Json::AsString(exclusions[1]) != L"device connection state" || Json::AsString(exclusions[2]) != L"observed driver state")
                throw std::runtime_error("Invalid profile backup manifest");

            Snapshot candidate; std::map<std::wstring, std::string> profileBytes; std::size_t sourceProfileCount{};
            auto settingsBytes = ReadBytes(source / L"settings.json"); candidate.settings = ParseSettings(settingsBytes);
            std::error_code ec;
            for (auto const& entry : std::filesystem::directory_iterator(source, ec))
            {
                if (ec) break;
                auto name = entry.path().filename().native();
                if (name == L"settings.json" || name == L"manifest.json") continue;
                if (!entry.is_regular_file() || entry.is_symlink() || entry.path().extension() != L".json" || !IsTargetName(name))
                    throw std::runtime_error("Backup contains an unexpected or unsafe entry");
                if (++sourceProfileCount > MaxProfiles) throw std::runtime_error("Backup contains more than the supported number of profile files");
                auto bytes = ReadBytes(entry.path()); auto profile = ParseProfile(bytes);
                if (profile.id + L".json" != name || !candidate.profiles.emplace(profile.id, profile).second)
                    throw std::runtime_error("Backup profile identity is invalid or duplicated");
                profileBytes.emplace(profile.id, std::move(bytes));
            }
            if (ec) throw std::system_error(ec, "Enumerate profile backup");
            if (candidate.profiles.size() != expectedCount) throw std::runtime_error("Backup profile count does not match its manifest");
            Validate(candidate);

            std::vector<Change> changes;
            for (auto const& [id, bytes] : profileBytes) changes.push_back({ id + L".json", bytes, false });
            changes.push_back({ L"settings.json", settingsBytes, false });
            std::size_t currentProfileCount{};
            for (auto const& entry : std::filesystem::directory_iterator(m_Root, ec))
            {
                if (ec) break; auto name = entry.path().filename().native();
                if (!entry.is_regular_file() || name == L"settings.json" || entry.path().extension() != L".json") continue;
                if (!IsTargetName(name)) throw std::runtime_error("Repository contains an unsafe JSON filename; restore preserved it for manual recovery");
                if (++currentProfileCount > MaxProfiles) throw std::runtime_error("Repository contains too many profile files for one safe restore transaction");
                auto id = entry.path().stem().native(); if (!candidate.profiles.count(id)) changes.push_back({ name, {}, true });
            }
            if (ec) throw std::system_error(ec, "Enumerate repository before restore");
            CommitLocked(changes); return m_CleanupPending ? LoadCommittedAfterMutationLocked() : LoadLocked(false);
        }

        void Recover()
        {
            EnsureDirectory(); WriterLease lease(m_Root); RecoverLocked();
        }
        bool CleanupPending() const noexcept { return m_CleanupPending; }
        LoadResult LoadCommittedAfterMutation() const
        {
            WriterLease lease(m_Root);
            if (!m_CleanupPending) throw std::logic_error("No committed cleanup is pending");
            return LoadCommittedAfterMutationLocked();
        }

    private:
        struct Change { std::wstring fileName; std::string bytes; bool remove{}; };
        LoadResult LoadCommittedAfterMutationLocked() const
        {
            if (!m_CleanupPending) throw std::logic_error("No committed cleanup is pending");
            return LoadLocked(false);
        }
        void Fire(CommitBoundary boundary, std::size_t index)
        {
            if (!m_FailureHook) return;
            try { m_FailureHook(boundary, index); } catch (...) { m_InjectedCrash = true; throw; }
        }
        void EnsureExternalDestination(std::filesystem::path const& destination) const
        {
            if (destination.empty()) throw std::invalid_argument("Destination is empty");
            std::error_code ec;
            auto canonicalRoot = std::filesystem::weakly_canonical(m_Root, ec); if (ec) throw std::system_error(ec, "Resolve repository root");
            auto absoluteDestination = std::filesystem::absolute(destination, ec); if (ec) throw std::system_error(ec, "Resolve destination");
            auto parent = absoluteDestination.parent_path(); if (parent.empty()) parent = std::filesystem::current_path(ec);
            auto canonicalParent = std::filesystem::weakly_canonical(parent, ec); if (ec) throw std::system_error(ec, "Resolve destination parent");
            auto canonicalDestination = std::filesystem::exists(absoluteDestination)
                ? std::filesystem::weakly_canonical(absoluteDestination, ec)
                : (canonicalParent / absoluteDestination.filename()).lexically_normal();
            if (ec) throw std::system_error(ec, "Resolve destination");
            auto rootText = canonicalRoot.native(), destinationText = canonicalDestination.native();
            std::transform(rootText.begin(), rootText.end(), rootText.begin(), ::towlower);
            std::transform(destinationText.begin(), destinationText.end(), destinationText.begin(), ::towlower);
            auto underRoot = destinationText == rootText || (destinationText.size() > rootText.size()
                && destinationText.compare(0, rootText.size(), rootText) == 0
                && (destinationText[rootText.size()] == L'\\' || destinationText[rootText.size()] == L'/'));
            if (underRoot) throw std::invalid_argument("Export and backup destinations must be outside the profile repository");
        }
        void WriteExternalDurable(std::filesystem::path const& destination, std::string const& bytes) const
        {
            struct Identity { DWORD volume{}, high{}, low{}; };
            std::vector<Identity> protectedFiles; std::error_code ec;
            for (auto const& entry : std::filesystem::directory_iterator(m_Root, ec))
            {
                if (!entry.is_regular_file()) continue;
                HANDLE file = ::CreateFileW(entry.path().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                if (file == INVALID_HANDLE_VALUE) throw std::system_error(::GetLastError(), std::system_category(), "Inspect repository file identity");
                BY_HANDLE_FILE_INFORMATION info{}; auto ok = ::GetFileInformationByHandle(file, &info); auto error = ::GetLastError(); ::CloseHandle(file);
                if (!ok) throw std::system_error(error, std::system_category(), "Read repository file identity");
                protectedFiles.push_back({ info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow });
            }
            if (ec) throw std::system_error(ec, "Enumerate repository identities");
            HANDLE file = ::CreateFileW(destination.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
            if (file == INVALID_HANDLE_VALUE) throw std::system_error(::GetLastError(), std::system_category(), "Open export destination");
            BY_HANDLE_FILE_INFORMATION info{}; bool ok = !!::GetFileInformationByHandle(file, &info);
            if (ok)
            {
                auto alias = std::any_of(protectedFiles.begin(), protectedFiles.end(), [&](auto const& protectedFile)
                { return protectedFile.volume == info.dwVolumeSerialNumber && protectedFile.high == info.nFileIndexHigh && protectedFile.low == info.nFileIndexLow; });
                if (alias) { ::CloseHandle(file); throw std::invalid_argument("Export destination aliases a profile repository file"); }
                LARGE_INTEGER zero{}; DWORD written{};
                ok = !!::SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) && !!::SetEndOfFile(file)
                    && (bytes.empty() || (::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size()))
                    && !!::FlushFileBuffers(file);
            }
            auto error = ok ? ERROR_SUCCESS : ::GetLastError(); ::CloseHandle(file); if (!ok) throw std::system_error(error, std::system_category(), "Write export destination");
        }
        void EnsureDirectory()
        {
            std::error_code ec; std::filesystem::create_directories(m_Root, ec); if (ec) throw std::system_error(ec, "Create profile repository");
            auto attributes = ::GetFileAttributesW(m_Root.c_str()); if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) || attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                throw std::runtime_error("Profile repository root must be a real directory");
        }
        static void WriteDurable(std::filesystem::path const& path, std::string const& bytes, bool createNew = true, std::function<void()> const& partiallyWritten = {})
        {
            HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE | GENERIC_READ, 0, nullptr, createNew ? CREATE_NEW : CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
            if (file == INVALID_HANDLE_VALUE) throw std::system_error(::GetLastError(), std::system_category(), "Create repository file");
            auto write = [&](char const* data, std::size_t size)
            { DWORD written{}; return size == 0 || (::WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size); };
            auto half = partiallyWritten ? bytes.size() / 2 : 0; bool ok = write(bytes.data(), half);
            if (ok && partiallyWritten) try { partiallyWritten(); } catch (...) { ::CloseHandle(file); throw; }
            ok = ok && write(bytes.data() + half, bytes.size() - half) && ::FlushFileBuffers(file);
            auto error = ok ? ERROR_SUCCESS : ::GetLastError(); ::CloseHandle(file); if (!ok) throw std::system_error(error, std::system_category(), "Flush repository file");
        }
        // Transaction records become visible only when complete: a crash while
        // writing leaves at most the ".new" staging file, never a torn record.
        static std::filesystem::path RecordStaging(std::filesystem::path path) { path += L".new"; return path; }
        void WriteRecordDurable(std::filesystem::path const& path, std::string const& bytes, CommitBoundary partiallyWritten)
        {
            auto staging = RecordStaging(path);
            DeleteChecked(staging, "Remove stale repository transaction record");
            WriteDurable(staging, bytes, true, [&] { Fire(partiallyWritten, 0); });
            if (!::MoveFileExW(staging.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
                throw std::system_error(::GetLastError(), std::system_category(), "Publish repository transaction record");
        }
        static void DeleteChecked(std::filesystem::path const& path, char const* operation)
        {
            if (::DeleteFileW(path.c_str())) return;
            auto error = ::GetLastError(); if (error == ERROR_FILE_NOT_FOUND) return;
            throw std::system_error(error, std::system_category(), operation);
        }
        static std::string IntentBytes(std::wstring const& transaction, std::vector<IntentItem> const& items)
        {
            std::wostringstream out; out << L"HIDHIDE-PROFILES-TRANSACTION-1\t" << transaction << L'\n';
            for (auto const& item : items) out << (item.existed ? 1 : 0) << L'\t' << (item.remove ? 1 : 0) << L'\t' << item.fileName << L'\t' << item.tempName << L'\t' << item.backupName << L'\t' << item.sha256 << L'\t' << item.oldSha256 << L'\n';
            return Json::ToUtf8(out.str());
        }
        void CommitLocked(std::vector<Change> const& changes)
        {
            if (changes.empty() || changes.size() > MaxTransactionItems) throw std::invalid_argument("Repository transaction is empty or too large");
            m_CleanupPending = false; m_InjectedCrash = false;
            struct VerificationHandles
            {
                std::vector<HANDLE> values;
                ~VerificationHandles() { for (auto value : values) if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
            } verification;
            auto transaction = NewStableId(); std::vector<IntentItem> items; items.reserve(changes.size());
            for (std::size_t i{}; i < changes.size(); ++i)
            {
                auto const& change = changes[i];
                if (change.fileName.empty() || change.fileName.find_first_of(L"\\/:") != std::wstring::npos) throw std::invalid_argument("Unsafe repository filename");
                if (!IsTargetName(change.fileName)) throw std::invalid_argument("Unsafe repository target filename");
                auto existed = std::filesystem::exists(m_Root / change.fileName);
                auto oldDigest = existed ? Sha256(ReadBytes(m_Root / change.fileName)) : L"";
                IntentItem item{ change.fileName, L"." + transaction + L"." + std::to_wstring(i) + L".tmp", L"." + transaction + L"." + std::to_wstring(i) + L".bak", change.remove ? L"" : Sha256(change.bytes), oldDigest, existed, change.remove };
                if (!change.remove) { WriteDurable(m_Root / item.tempName, change.bytes); Fire(CommitBoundary::TempFlushed, i); }
                if (item.existed)
                {
                    if (!::CopyFileW((m_Root / item.fileName).c_str(), (m_Root / item.backupName).c_str(), TRUE)) throw std::system_error(::GetLastError(), std::system_category(), "Create last-good repository file");
                    HANDLE backup = ::CreateFileW((m_Root / item.backupName).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, nullptr);
                    if (backup == INVALID_HANDLE_VALUE || !::FlushFileBuffers(backup)) { auto e = ::GetLastError(); if (backup != INVALID_HANDLE_VALUE) ::CloseHandle(backup); throw std::system_error(e, std::system_category(), "Flush last-good repository file"); }
                    ::CloseHandle(backup);
                }
                items.push_back(std::move(item));
            }
            auto intentPath = m_Root / L".transaction.intent"; auto committedPath = m_Root / L".transaction.committed";
            // Until the commit marker is durable the old set is authoritative.
            // An ordinary failure (for example a sharing violation) rolls back
            // in this process, so the repository is not left blocked until the
            // next start. Injected crashes keep their evidence for recovery.
            struct RollbackOnFailure
            {
                ProfileRepository& repository; VerificationHandles& verification; int exceptions{ std::uncaught_exceptions() }; bool armed{ true };
                ~RollbackOnFailure()
                {
                    if (!armed || std::uncaught_exceptions() <= exceptions || std::exchange(repository.m_InjectedCrash, false)) return;
                    for (auto value : verification.values) ::CloseHandle(value);
                    verification.values.clear();
                    try { repository.RecoverLocked(); } catch (...) {} // Evidence stays for the next writer or start.
                }
            } rollback{ *this, verification };
            WriteRecordDurable(intentPath, IntentBytes(transaction, items), CommitBoundary::IntentPartiallyWritten); Fire(CommitBoundary::IntentFlushed, 0);
            for (std::size_t i{}; i < items.size(); ++i)
            {
                auto const& item = items[i]; auto target = m_Root / item.fileName;
                // Revalidate the exact old bytes immediately before replacement.
                // The named lease excludes every participating writer; this check
                // also refuses a non-participating editor change made while the
                // transaction was being staged.
                auto currentlyExists = std::filesystem::exists(target);
                if (currentlyExists != item.existed || (currentlyExists && Sha256(ReadBytes(target)) != item.oldSha256))
                    throw RepositoryConflict("Repository target changed while the transaction was staged");
                BOOL ok{};
                if (item.remove) ok = ::DeleteFileW(target.c_str());
                else ok = ::MoveFileExW((m_Root / item.tempName).c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                if (!ok) throw std::system_error(::GetLastError(), std::system_category(), "Replace repository file");
                if (!item.remove)
                {
                    auto reopened = ReadBytes(target); if (Sha256(reopened) != item.sha256) throw std::runtime_error("Committed repository file failed digest verification");
                    if (item.fileName == L"settings.json") ParseSettings(reopened);
                    else
                    {
                        auto parsed = ParseProfile(reopened);
                        if (parsed.id + L".json" != item.fileName) throw std::runtime_error("Committed profile identity verification failed");
                    }
                    // Keep a no-write/no-delete sharing handle through the
                    // commit point. A non-participating editor therefore
                    // cannot replace an already-verified early target while a
                    // later target is still being committed.
                    auto held = ::CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (held == INVALID_HANDLE_VALUE) throw std::system_error(::GetLastError(), std::system_category(), "Protect verified repository file");
                    verification.values.push_back(held);
                }
                Fire(CommitBoundary::TargetReplaced, i);
            }
            // Revalidate the complete set immediately before the durable
            // commit point. Removed targets have no handle to hold, so their
            // required absence is checked here as a single set with all new
            // file digests.
            for (auto const& item : items)
            {
                auto target = m_Root / item.fileName;
                if (item.remove ? std::filesystem::exists(target) : (!std::filesystem::exists(target) || Sha256(ReadBytes(target)) != item.sha256))
                    throw RepositoryConflict("Repository target changed before transaction commit");
            }
            Fire(CommitBoundary::SetVerified, 0);
            WriteRecordDurable(committedPath, Json::ToUtf8(transaction), CommitBoundary::CommitMarkPartiallyWritten); rollback.armed = false;
            // A deleted path cannot be protected by an open sharing handle.
            // Recheck the entire set after the durable linearization marker so
            // a recreation racing SetVerified is classified as incomplete
            // committed evidence rather than a successful deletion.
            for (auto const& item : items)
            {
                auto target = m_Root / item.fileName;
                if (item.remove ? std::filesystem::exists(target) : (!std::filesystem::exists(target) || Sha256(ReadBytes(target)) != item.sha256))
                    throw RepositoryConflict("Repository target changed at transaction commit; recovery evidence was preserved");
            }
            try
            {
                Fire(CommitBoundary::CommitMarked, 0);
                for (auto const& item : items) { DeleteChecked(m_Root / item.tempName, "Remove committed repository temporary file"); DeleteChecked(m_Root / item.backupName, "Remove committed repository backup"); }
                // Intent must disappear before the marker. If marker deletion is
                // interrupted, startup recognizes it as finalized cleanup only;
                // the reverse order could misclassify the committed set as old.
                DeleteChecked(intentPath, "Remove repository transaction intent"); DeleteChecked(committedPath, "Remove repository commit marker"); Fire(CommitBoundary::Completed, 0);
            }
            catch (...)
            {
                // The commit marker is the durable point of no return. Cleanup
                // is recoverable startup work and must never turn a committed
                // save into a false "not saved" result.
                m_CleanupPending = true;
            }
        }
        void RecoverLocked()
        {
            auto intentPath = m_Root / L".transaction.intent"; auto committedPath = m_Root / L".transaction.committed";
            if (!std::filesystem::exists(intentPath))
            {
                if (std::filesystem::exists(committedPath))
                {
                    auto marker = Json::FromUtf8(ReadBytes(committedPath)); if (!IsStableId(marker)) throw std::runtime_error("Invalid orphaned repository commit marker");
                    DeleteChecked(committedPath, "Remove finalized repository commit marker");
                }
                // A crash before the intent becomes durable can leave only
                // transaction-scoped staging files. Their strict generated
                // names cannot designate catalog files and are safe to remove.
                std::error_code ec;
                for (auto const& entry : std::filesystem::directory_iterator(m_Root, ec))
                {
                    auto name = entry.path().filename().native();
                    // The two ".new" record files are removed, checked, just below.
                    if (!entry.is_regular_file() || name.size() < 43 || !IsStagingOrphanName(name)) continue;
                    std::filesystem::remove(entry.path(), ec);
                    if (ec) throw std::system_error(ec, "Remove orphaned repository staging file");
                }
                if (ec) throw std::system_error(ec, "Enumerate repository staging files");
                DeleteChecked(RecordStaging(intentPath), "Remove unpublished repository transaction intent");
                DeleteChecked(RecordStaging(committedPath), "Remove unpublished repository commit marker");
                return;
            }
            auto parsed = ParseIntent(ReadBytes(intentPath)); auto const& items = parsed.items; bool committed = std::filesystem::exists(committedPath);
            if (committed)
            {
                if (Json::FromUtf8(ReadBytes(committedPath)) != parsed.transaction) throw std::runtime_error("Repository commit marker does not match intent");
                for (auto const& item : items)
                {
                    auto target = m_Root / item.fileName;
                    if (item.remove ? std::filesystem::exists(target) : (!std::filesystem::exists(target) || Sha256(ReadBytes(target)) != item.sha256))
                        throw std::runtime_error("Committed repository transaction is incomplete");
                }
            }
            else
            {
                // Preflight every rollback artifact before changing any target.
                // Recovery must never restore item 1 and only then discover
                // that item 2 has no trustworthy old-byte evidence.
                for (auto const& item : items)
                {
                    if (item.existed)
                    {
                        auto backup = m_Root / item.backupName;
                        if (!std::filesystem::exists(backup) || Sha256(ReadBytes(backup)) != item.oldSha256) throw std::runtime_error("Repository rollback file is missing or corrupt");
                    }
                    auto target = m_Root / item.fileName; auto exists = std::filesystem::exists(target);
                    std::wstring digest; if (exists) digest = Sha256(ReadBytes(target));
                    // A pre-commit crash can leave each target at exactly its
                    // recorded old or staged-new state (or absent for a
                    // completed delete/new file not yet installed). Anything
                    // else is an external edit and must stop the entire
                    // rollback before even one target or evidence file moves.
                    bool recognized{};
                    if (item.existed)
                        recognized = item.remove ? (!exists || digest == item.oldSha256)
                            : (exists && (digest == item.oldSha256 || digest == item.sha256));
                    else
                        recognized = !exists || (!item.remove && digest == item.sha256);
                    if (!recognized) throw RepositoryConflict("Repository target changed after the interrupted transaction; recovery preserved every file");
                }
                for (std::size_t index{}; index < items.size(); ++index)
                {
                    auto const& item = items[index]; auto target = m_Root / item.fileName; auto backup = m_Root / item.backupName;
                    // A target never replaced already holds its old bytes and
                    // needs no restore (it may still be locked by a reader).
                    if (item.existed && std::filesystem::exists(target) && Sha256(ReadBytes(target)) == item.oldSha256)
                    { Fire(CommitBoundary::RecoveryTargetRestored, index); continue; }
                    if (item.existed)
                    {
                        // Preserve the only trusted backup across every retry.
                        // A crash at any following boundary can restart this
                        // copy-and-replace operation idempotently.
                        auto restore = m_Root / (L"." + parsed.transaction + L"." + std::to_wstring(index) + L".restore");
                        DeleteChecked(restore, "Remove stale recovery staging file");
                        if (!::CopyFileW(backup.c_str(), restore.c_str(), TRUE)) throw std::system_error(::GetLastError(), std::system_category(), "Stage repository rollback file");
                        HANDLE staged = ::CreateFileW(restore.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, nullptr);
                        if (staged == INVALID_HANDLE_VALUE || !::FlushFileBuffers(staged)) { auto error = ::GetLastError(); if (staged != INVALID_HANDLE_VALUE) ::CloseHandle(staged); throw std::system_error(error, std::system_category(), "Flush repository rollback file"); }
                        ::CloseHandle(staged);
                        if (!::MoveFileExW(restore.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::system_error(::GetLastError(), std::system_category(), "Restore repository transaction");
                    }
                    else DeleteChecked(target, "Remove newly created repository target during rollback");
                    Fire(CommitBoundary::RecoveryTargetRestored, index);
                }
                for (auto const& item : items)
                {
                    auto target = m_Root / item.fileName;
                    if (item.existed ? (!std::filesystem::exists(target) || Sha256(ReadBytes(target)) != item.oldSha256) : std::filesystem::exists(target))
                        throw std::runtime_error("Recovered repository set failed old-byte verification");
                }
                Fire(CommitBoundary::RecoveryVerified, 0);
            }
            // Every target is verified, so the intent has done its work. Remove
            // it before the staging files: if a later delete fails or the
            // process dies, only orphans remain for the sweep above. Deleting a
            // backup first would leave an intent that no recovery could satisfy.
            DeleteChecked(intentPath, "Remove recovered repository intent");
            for (std::size_t index{}; index < items.size(); ++index)
            {
                auto const& item = items[index];
                DeleteChecked(m_Root / item.tempName, "Remove recovered repository temporary file");
                DeleteChecked(m_Root / (L"." + parsed.transaction + L"." + std::to_wstring(index) + L".restore"), "Remove recovery staging file");
                DeleteChecked(m_Root / item.backupName, "Remove recovered repository backup");
            }
            DeleteChecked(committedPath, "Remove recovered repository commit marker");
            DeleteChecked(RecordStaging(committedPath), "Remove unpublished repository commit marker");
        }
        FailureHook m_FailureHook;
        bool m_CleanupPending{}, m_InjectedCrash{};
    };

    inline std::filesystem::path DefaultRepositoryRoot()
    {
        PWSTR local{}; auto result = ::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local);
        if (FAILED(result) || !local) throw std::runtime_error("Local AppData known folder is unavailable");
        std::filesystem::path root(local); ::CoTaskMemFree(local); return root / L"HidHide Profiles" / L"Profiles";
    }
}
