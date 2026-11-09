// SPDX-License-Identifier: MIT
#pragma once

#include "ProfileDomain.h"
#include <Windows.h>

namespace HidHide::Profiles
{
    // Own only exact enabled application lifetimes. Exited process objects are
    // released on every scan, even while some other candidate is inaccessible.
    class ProcessLifetimeCache
    {
        using Key = std::pair<DWORD, std::uint64_t>;
        std::set<std::filesystem::path> m_Paths;
        std::map<Key, std::pair<HANDLE, ProcessObservation>> m_Live;
        // Disjoint interval union, indexed by start: adding one exit touches
        // only its overlapping neighbours, never the entire retained history.
        std::map<std::filesystem::path, std::map<std::uint64_t, std::uint64_t>> m_Exited;
    public:
        ProcessLifetimeCache() = default;
        ProcessLifetimeCache(ProcessLifetimeCache const&) = delete;
        ProcessLifetimeCache& operator=(ProcessLifetimeCache const&) = delete;
        ~ProcessLifetimeCache() { Clear(); }
        void Clear()
        {
            for (auto const& [key, retained] : m_Live) { (void)key; ::CloseHandle(retained.first); }
            m_Live.clear(); m_Exited.clear();
        }
        void Retain(Snapshot const& snapshot)
        {
            m_Paths.clear();
            for (auto const& [id, profile] : snapshot.profiles)
                if (profile.enabled && profile.kind == Kind::Application) m_Paths.insert(NormalizeExecutable(profile.executable));
            for (auto it = m_Live.begin(); it != m_Live.end();)
                if (!Relevant(it->second.second)) { ::CloseHandle(it->second.first); it = m_Live.erase(it); } else ++it;
            for (auto it = m_Exited.begin(); it != m_Exited.end();)
                if (!m_Paths.count(it->first)) it = m_Exited.erase(it); else ++it;
        }
        bool Relevant(ProcessObservation const& observation) const
        {
            return observation.processId && observation.lifetimeIdentity && observation.pathAccessible
                && !observation.verifiedPath.empty() && m_Paths.count(NormalizeObservedExecutable(observation.verifiedPath));
        }
        bool Contains(DWORD pid, std::uint64_t lifetime) const { return m_Live.count({pid, lifetime}) != 0; }
        // Takes ownership whether accepted, redundant, or irrelevant.
        void Remember(HANDLE handle, ProcessObservation const& observation)
        {
            if (!Relevant(observation) || !m_Live.emplace(Key{observation.processId, observation.lifetimeIdentity}, std::make_pair(handle, observation)).second)
                ::CloseHandle(handle);
        }
        void RememberExit(ProcessObservation const& observation)
        {
            if (!Relevant(observation) || !observation.exitedAt) return;
            auto& intervals = m_Exited[NormalizeObservedExecutable(observation.verifiedPath)];
            auto start = observation.lifetimeIdentity, end = observation.exitedAt;
            auto it = intervals.lower_bound(start);
            if (it != intervals.begin()) { auto previous = std::prev(it); if (previous->second >= start) it = previous; }
            while (it != intervals.end() && it->first <= end)
            {
                start = (std::min)(start, it->first); end = (std::max)(end, it->second); it = intervals.erase(it);
            }
            intervals.emplace(start, end);
        }
        bool Collect(std::vector<ProcessObservation>& processes)
        {
            bool complete = true;
            for (auto it = m_Live.begin(); it != m_Live.end();)
            {
                auto observation = it->second.second;
                auto state = ::WaitForSingleObject(it->second.first, 0);
                if (state != WAIT_OBJECT_0 && state != WAIT_TIMEOUT) { complete = false; ++it; continue; }
                if (state == WAIT_OBJECT_0)
                {
                    FILETIME created{}, exited{}, kernel{}, user{};
                    if (!::GetProcessTimes(it->second.first, &created, &exited, &kernel, &user)) { complete = false; ++it; continue; }
                    ULARGE_INTEGER time{}; time.LowPart = exited.dwLowDateTime; time.HighPart = exited.dwHighDateTime;
                    if (!time.QuadPart) { complete = false; ++it; continue; }
                    observation.exitedAt = time.QuadPart;
                    RememberExit(observation);
                    ::CloseHandle(it->second.first); it = m_Live.erase(it);
                }
                else ++it;
                auto found = std::find_if(processes.begin(), processes.end(), [&](auto const& p)
                    { return p.processId == observation.processId && p.lifetimeIdentity == observation.lifetimeIdentity; });
                if (found == processes.end()) processes.push_back(observation); else *found = observation;
            }
            return complete;
        }
        // Only materialize history when a complete discovery can consume it.
        // PID zero distinguishes compact interval evidence from an identity.
        void AppendExited(std::vector<ProcessObservation>& processes) const
        {
            for (auto const& [path, intervals] : m_Exited)
                for (auto const& [start, end] : intervals) processes.push_back({0, start, path.filename().native(), path, true, end});
        }
        void ConsumeExited() { m_Exited.clear(); }
        std::size_t HandleCount() const { return m_Live.size(); }
        std::size_t IntervalCount() const
        { std::size_t count{}; for (auto const& [path, intervals] : m_Exited) { (void)path; count += intervals.size(); } return count; }
    };
}
