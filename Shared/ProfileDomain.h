// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <Windows.h>

namespace HidHide::Profiles
{
    constexpr std::uint32_t SchemaVersion{ 1 };
    constexpr std::size_t MaxProfiles{ 512 };
    constexpr std::size_t MaxRulesPerProfile{ 4096 };
    constexpr std::size_t MaxTextCharacters{ 32768 };
    constexpr std::int32_t MinPriority{ -100000 };
    constexpr std::int32_t MaxPriority{ 100000 };

    enum class Kind { Application, Global };
    enum class Visibility { Hidden, Visible };
    enum class Mode { Automatic, UseGlobal };

    struct DeviceRule
    {
        std::wstring identity;
        std::wstring friendlyName;
        Visibility visibility{ Visibility::Visible };
        bool operator==(DeviceRule const& other) const
        {
            return identity == other.identity && friendlyName == other.friendlyName && visibility == other.visibility;
        }
    };

    struct Profile
    {
        std::wstring id;
        std::uint64_t revision{};
        std::wstring name;
        Kind kind{ Kind::Global };
        bool enabled{ true };
        std::int32_t priority{};
        std::filesystem::path executable;
        std::vector<DeviceRule> rules;

        bool operator==(Profile const& other) const
        {
            return id == other.id && revision == other.revision && name == other.name && kind == other.kind
                && enabled == other.enabled && priority == other.priority && executable == other.executable
                && rules == other.rules;
        }
    };

    struct Settings
    {
        std::uint64_t revision{};
        std::wstring selectedGlobalId;
        Mode mode{ Mode::Automatic };
        bool paused{};
        bool startWithWindows{ true };
        std::set<std::filesystem::path> allowedApplications;
        bool operator==(Settings const& other) const
        {
            return revision == other.revision && selectedGlobalId == other.selectedGlobalId && mode == other.mode
                && paused == other.paused && startWithWindows == other.startWithWindows
                && allowedApplications == other.allowedApplications;
        }
    };

    struct Snapshot
    {
        std::uint64_t generation{};
        std::map<std::wstring, Profile> profiles;
        Settings settings;
    };

    inline bool IsStableId(std::wstring const& id)
    {
        // Repository filenames use the canonical lower-case GUID form without braces.
        if (id.size() != 36 || id[8] != L'-' || id[13] != L'-' || id[18] != L'-' || id[23] != L'-') return false;
        for (std::size_t i{}; i < id.size(); ++i)
        {
            if (i == 8 || i == 13 || i == 18 || i == 23) continue;
            if (!((id[i] >= L'0' && id[i] <= L'9') || (id[i] >= L'a' && id[i] <= L'f'))) return false;
        }
        return true;
    }

    inline std::filesystem::path NormalizeExecutable(std::filesystem::path path)
    {
        if (path.empty()) return {};
        if (path.native().find(L'\0') != std::wstring::npos) throw std::invalid_argument("Application path contains an embedded NUL");
        path = path.lexically_normal();
        if (!path.is_absolute() || path.native().rfind(L"\\\\", 0) == 0 || _wcsicmp(path.extension().c_str(), L".exe") != 0)
            throw std::invalid_argument("Application profile executable must be an absolute local .exe path");
        auto text = path.native();
        // Windows paths are case-insensitive for all of Unicode, not only ASCII.
        if (!text.empty() && ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.c_str(), static_cast<int>(text.size()),
            text.data(), static_cast<int>(text.size()), nullptr, nullptr, 0) != static_cast<int>(text.size()))
            throw std::runtime_error("Application path case normalization failed");
        return text;
    }

    // Observed process images are not profile input. A path no profile could
    // name (network share, device namespace, non-.exe image) is simply no match.
    inline std::filesystem::path NormalizeObservedExecutable(std::filesystem::path const& path) noexcept
    {
        try { return NormalizeExecutable(path); }
        catch (...) { return {}; }
    }

    inline void Validate(Profile const& profile)
    {
        if (!IsStableId(profile.id)) throw std::invalid_argument("Profile id is not a canonical GUID");
        if (!profile.revision) throw std::invalid_argument("Profile revision must be positive");
        if (profile.name.empty() || profile.name.size() > 256 || profile.name.find(L'\0') != std::wstring::npos) throw std::invalid_argument("Profile name is missing or too long");
        if (profile.priority < MinPriority || profile.priority > MaxPriority) throw std::invalid_argument("Profile priority is out of range");
        if (profile.rules.size() > MaxRulesPerProfile) throw std::invalid_argument("Profile has too many device rules");
        if (profile.kind == Kind::Application)
        {
            if (profile.executable.empty() || !profile.executable.is_absolute()) throw std::invalid_argument("Application profile requires an absolute executable path");
        }
        else if (!profile.executable.empty()) throw std::invalid_argument("Global profile cannot have an executable trigger");
        std::vector<std::wstring> identities;
        for (auto const& rule : profile.rules)
        {
            if (rule.identity.empty() || rule.identity.size() > MaxTextCharacters || rule.identity.find(L'\0') != std::wstring::npos) throw std::invalid_argument("Device identity is missing or too long");
            if (rule.friendlyName.size() > 1024 || rule.friendlyName.find(L'\0') != std::wstring::npos) throw std::invalid_argument("Device friendly name is too long");
            if (std::any_of(identities.begin(), identities.end(), [&](auto const& identity) { return _wcsicmp(identity.c_str(), rule.identity.c_str()) == 0; }))
                throw std::invalid_argument("Duplicate device identity in profile");
            identities.emplace_back(rule.identity);
        }
    }

    inline void Validate(Snapshot const& snapshot)
    {
        if (snapshot.profiles.empty() || snapshot.profiles.size() > MaxProfiles) throw std::invalid_argument("Repository must contain a bounded profile set");
        bool selectedGlobal{};
        for (auto const& [id, profile] : snapshot.profiles)
        {
            Validate(profile);
            if (id != profile.id) throw std::invalid_argument("Profile map key does not match id");
            if (profile.kind == Kind::Global && profile.enabled && profile.id == snapshot.settings.selectedGlobalId) selectedGlobal = true;
        }
        if (!selectedGlobal) throw std::invalid_argument("Selected Global profile does not exist");
        if (snapshot.settings.allowedApplications.size() > MaxRulesPerProfile) throw std::invalid_argument("Too many allowed applications");
        for (auto const& application : snapshot.settings.allowedApplications)
            if (application.empty() || !application.is_absolute()) throw std::invalid_argument("Allowed application path is invalid");
    }

    struct ProcessObservation
    {
        std::uint32_t processId{};
        std::uint64_t lifetimeIdentity{};
        std::wstring fileName;
        std::filesystem::path verifiedPath;
        bool pathAccessible{ true };
        // Verified exit FILETIME from a retained handle for this exact lifetime.
        // Exited observations provide continuity evidence, never a live match.
        std::uint64_t exitedAt{};
    };

    // Process path caches must include a creation/lifetime identity. A recycled PID
    // can never inherit a prior path verification.
    class ProcessIdentityCache
    {
    public:
        void Remember(ProcessObservation const& observation)
        {
            if (!observation.lifetimeIdentity || !observation.pathAccessible || observation.verifiedPath.empty()) return;
            m_Entries[observation.processId] = { observation.lifetimeIdentity, observation.verifiedPath };
        }
        std::optional<std::filesystem::path> Find(std::uint32_t processId, std::uint64_t lifetimeIdentity) const
        {
            auto it = m_Entries.find(processId);
            if (it == m_Entries.end() || !lifetimeIdentity || it->second.first != lifetimeIdentity) return std::nullopt;
            return it->second.second;
        }
        void Retain(std::vector<ProcessObservation> const& live)
        {
            for (auto it = m_Entries.begin(); it != m_Entries.end();)
            {
                auto found = std::find_if(live.begin(), live.end(), [&](auto const& item)
                { return item.processId == it->first && item.lifetimeIdentity == it->second.first; });
                if (found == live.end()) it = m_Entries.erase(it); else ++it;
            }
        }
    private:
        std::map<std::uint32_t, std::pair<std::uint64_t, std::filesystem::path>> m_Entries;
    };

    enum class SelectionReason { Application, ManualApplication, GlobalFallback, ManualGlobal, Paused, DetectionUncertain };
    struct Selection
    {
        std::wstring profileId;
        SelectionReason reason{ SelectionReason::GlobalFallback };
        std::wstring executableName;
        bool verified{};
        bool operator==(Selection const& other) const
        {
            return profileId == other.profileId && reason == other.reason && executableName == other.executableName
                && verified == other.verified;
        }
        bool operator!=(Selection const& other) const { return !(*this == other); }
    };

    // Runtime only. Each complete observation advances history even when no UI
    // consumer has drained the worker's latest result yet.
    class ActivationHistory
    {
        struct Entry
        {
            std::filesystem::path path;
            std::set<std::pair<std::uint32_t, std::uint64_t>> instances;
            std::uint64_t order{}, created{};
        };
        std::map<std::wstring, Entry> m_Entries;
        std::uint64_t m_Order{};
        std::wstring m_Pin;
    public:
        void Update(Snapshot const& snapshot, std::vector<ProcessObservation> const& processes, bool complete)
        {
            // Catalog invalidations do not depend on process discovery.
            for (auto it = m_Entries.begin(); it != m_Entries.end();)
            {
                auto p = snapshot.profiles.find(it->first);
                if (p == snapshot.profiles.end() || !p->second.enabled || p->second.kind != Kind::Application
                    || NormalizeExecutable(p->second.executable) != it->second.path) it = m_Entries.erase(it);
                else ++it;
            }
            if (!m_Entries.count(m_Pin)) m_Pin.clear();
            if (snapshot.settings.paused || snapshot.settings.mode == Mode::UseGlobal) m_Pin.clear();
            if (!complete)
            {
                // A recycled PID proves that instance ended, not that the
                // profile stopped: an overlapping successor may still run.
                // Defer pin expiry until a complete scan can reconcile all
                // live instances with the retained exit intervals below.
                return;
            }
            std::map<std::wstring, Entry> next;
            std::vector<std::wstring> activated;
            for (auto const& [id, profile] : snapshot.profiles)
            {
                if (!profile.enabled || profile.kind != Kind::Application) continue;
                Entry entry; entry.path = NormalizeExecutable(profile.executable);
                for (auto const& process : processes)
                {
                    if (!process.processId || !process.lifetimeIdentity || !process.pathAccessible || process.verifiedPath.empty() || process.exitedAt) continue;
                    if (NormalizeObservedExecutable(process.verifiedPath) != entry.path) continue;
                    entry.instances.emplace(process.processId, process.lifetimeIdentity);
                    if (!entry.created || process.lifetimeIdentity < entry.created) entry.created = process.lifetimeIdentity;
                }
                if (entry.instances.empty()) continue;
                // Date this live episode from all retained verified intervals,
                // including predecessors first observed in an incomplete scan.
                // Walk backward only through overlaps: a real inactive gap starts
                // a new activation even if an older entry still exists.
                bool earlier{};
                do
                {
                    earlier = false;
                    for (auto const& process : processes)
                        if (process.lifetimeIdentity && process.lifetimeIdentity < entry.created
                            && process.exitedAt >= entry.created && process.pathAccessible && !process.verifiedPath.empty()
                            && NormalizeObservedExecutable(process.verifiedPath) == entry.path)
                        { entry.created = process.lifetimeIdentity; earlier = true; }
                } while (earlier);
                auto old = m_Entries.find(id);
                // Retained exit evidence may also contain an intermediate
                // instance seen during an incomplete scan. Follow overlapping
                // verified intervals, without bridging a real inactive gap.
                std::uint64_t continuousUntil{};
                if (old != m_Entries.end()) for (auto const& process : processes)
                    if (process.exitedAt && process.pathAccessible && NormalizeObservedExecutable(process.verifiedPath) == entry.path
                        && (process.processId ? old->second.instances.count({process.processId, process.lifetimeIdentity}) != 0
                            : std::any_of(old->second.instances.begin(), old->second.instances.end(), [&](auto const& instance)
                                { return instance.second >= process.lifetimeIdentity && instance.second <= process.exitedAt; })))
                        continuousUntil = (std::max)(continuousUntil, process.exitedAt);
                bool extended{};
                do
                {
                    extended = false;
                    for (auto const& process : processes)
                        if (process.lifetimeIdentity && process.lifetimeIdentity <= continuousUntil && process.exitedAt > continuousUntil
                            && process.pathAccessible && NormalizeObservedExecutable(process.verifiedPath) == entry.path)
                        { continuousUntil = process.exitedAt; extended = true; }
                } while (extended);
                if (old != m_Entries.end() && (std::any_of(entry.instances.begin(), entry.instances.end(),
                    [&](auto const& instance) { return old->second.instances.count(instance) != 0; })
                    || continuousUntil >= entry.created)) entry.order = old->second.order;
                else { activated.push_back(id); if (m_Pin == id) m_Pin.clear(); }
                next.emplace(id, std::move(entry));
            }
            std::sort(activated.begin(), activated.end(), [&](auto const& a, auto const& b)
            {
                if (next.at(a).created != next.at(b).created) return next.at(a).created < next.at(b).created;
                if (snapshot.profiles.at(a).priority != snapshot.profiles.at(b).priority)
                    return snapshot.profiles.at(a).priority < snapshot.profiles.at(b).priority;
                return a > b;
            });
            for (auto const& id : activated) next.at(id).order = ++m_Order;
            m_Entries = std::move(next);
            if (!m_Entries.count(m_Pin)) m_Pin.clear();
        }
        std::vector<std::wstring> Running() const
        {
            std::vector<std::wstring> result;
            for (auto const& [id, entry] : m_Entries) { (void)entry; result.push_back(id); }
            std::sort(result.begin(), result.end(), [&](auto const& a, auto const& b) { return m_Entries.at(a).order > m_Entries.at(b).order; });
            return result;
        }
        std::wstring const& Pin() const { return m_Pin; }
        void SetPin(std::wstring const& id)
        {
            if (!id.empty() && !m_Entries.count(id)) throw std::invalid_argument("The saved application is no longer running");
            m_Pin = id;
        }
        Selection Select(Snapshot const& snapshot) const
        {
            if (snapshot.settings.paused) return { snapshot.settings.selectedGlobalId, SelectionReason::Paused, {}, true };
            if (snapshot.settings.mode == Mode::UseGlobal) return { snapshot.settings.selectedGlobalId, SelectionReason::ManualGlobal, {}, true };
            auto running = Running(); auto id = m_Pin.empty() ? (running.empty() ? std::wstring{} : running.front()) : m_Pin;
            if (id.empty()) return { snapshot.settings.selectedGlobalId, SelectionReason::GlobalFallback, {}, true };
            return { id, m_Pin.empty() ? SelectionReason::Application : SelectionReason::ManualApplication,
                snapshot.profiles.at(id).executable.filename().native(), true };
        }
    };

    inline Selection SelectWinner(Snapshot const& snapshot, std::vector<ProcessObservation> const& processes)
    {
        Validate(snapshot);
        if (snapshot.settings.paused) return { snapshot.settings.selectedGlobalId, SelectionReason::Paused, {}, true };
        if (snapshot.settings.mode == Mode::UseGlobal)
            return { snapshot.settings.selectedGlobalId, SelectionReason::ManualGlobal, {}, true };

        ActivationHistory history; history.Update(snapshot, processes, true);
        auto selection = history.Select(snapshot);
        if (selection.reason == SelectionReason::GlobalFallback && std::any_of(processes.begin(), processes.end(),
            [](auto const& p) { return !p.pathAccessible || p.verifiedPath.empty() || !p.lifetimeIdentity; }))
            selection.reason = SelectionReason::DetectionUncertain;
        return selection;
    }

    inline std::set<std::wstring> HiddenDevices(Profile const& profile)
    {
        std::set<std::wstring> result;
        for (auto const& rule : profile.rules) if (rule.visibility == Visibility::Hidden) result.emplace(rule.identity);
        return result;
    }

    struct DesiredEnforcement
    {
        bool hidingEnabled{};
        std::set<std::wstring> hiddenDevices;
        std::set<std::filesystem::path> allowedApplications;
        bool operator==(DesiredEnforcement const& other) const
        {
            return hidingEnabled == other.hidingEnabled && hiddenDevices == other.hiddenDevices
                && allowedApplications == other.allowedApplications;
        }
    };

    struct EnforcementResult
    {
        bool success{};
        bool observedKnown{};
        DesiredEnforcement observed;
        std::wstring failure;
        bool conflict{};
    };

    class IEnforcement
    {
    public:
        virtual ~IEnforcement() = default;
        virtual EnforcementResult Observe() = 0;
        virtual EnforcementResult Reconcile(DesiredEnforcement const& desired) = 0;
        virtual EnforcementResult RestoreBaseline() = 0;
        virtual EnforcementResult AdoptCurrentAsBaseline() = 0;
    };

    class DeduplicatingEnforcement
    {
    public:
        explicit DeduplicatingEnforcement(IEnforcement& enforcement) : m_Enforcement(enforcement) {}
        EnforcementResult Reconcile(DesiredEnforcement const& desired)
        {
            if (m_LastObserved && *m_LastObserved == desired)
            {
                auto observed = m_Enforcement.Observe();
                if (observed.success && observed.observedKnown && observed.observed == desired) return observed;
                m_LastObserved.reset();
            }
            auto result = m_Enforcement.Reconcile(desired);
            if (result.success && result.observedKnown && result.observed == desired) m_LastObserved = desired;
            return result;
        }
        void Invalidate() { m_LastObserved.reset(); }
    private:
        IEnforcement& m_Enforcement;
        std::optional<DesiredEnforcement> m_LastObserved;
    };
}
