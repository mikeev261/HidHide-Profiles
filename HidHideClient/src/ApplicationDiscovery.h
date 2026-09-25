// SPDX-License-Identifier: MIT
#pragma once
#include <Windows.h>
#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <cstdint>

namespace HidHide::Applications
{
    inline std::wstring Lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return value;
    }
    inline std::wstring Clean(std::wstring value)
    {
        std::wstring result; bool space = false;
        for (std::size_t i = 0; i < value.size(); ++i)
        {
            auto c = value[i];
            if (std::iswspace(c) || c == L'_' || c == L'\0') { space = !result.empty(); continue; }
            if (std::iswcntrl(c)) continue;
            auto pair = c >= 0xd800 && c <= 0xdbff;
            if (pair && (i + 1 == value.size() || value[i + 1] < 0xdc00 || value[i + 1] > 0xdfff)) continue;
            if (c >= 0xdc00 && c <= 0xdfff) continue;
            if (result.size() + (space ? 1 : 0) + (pair ? 2 : 1) > 256) break;
            if (space) result += L' ';
            result += c; if (pair) result += value[++i]; space = false;
        }
        return result;
    }
    inline bool Meaningful(std::wstring const& text)
    {
        auto value = Lower(Clean(text));
        if (value.empty()) return false;
        if (value == L"todo" || value.find(L"todo:") == 0 || value.find_first_of(L"<>") != std::wstring::npos || value == L"placeholder" || value == L"your product name") return false;
        // Windows components share this product branding. Prefer their actual
        // description, preserving useful Microsoft product names and Unicode text.
        auto brand = value;
        brand.erase(std::remove_if(brand.begin(), brand.end(), [](wchar_t c) { return c == L'\u00ae' || c == L'\u2122'; }), brand.end());
        brand = Clean(brand);
        for (auto generic : { L"microsoft windows operating system", L"windows operating system", L"microsoft windows", L"windows" })
            if (brand == generic) return false;
        for (auto generic : { L"application", L"app", L"game", L"launcher", L"helper", L"bootstrap", L"bootstrapper", L"bootstrappackagedgame", L"unreal engine", L"ue4", L"ue5", L"ue4game", L"ue5game", L"unity", L"unity player", L"unityplayer", L"my game", L"mygame", L"productname", L"product name", L"filedescription", L"file description", L"unknown", L"n/a", L"default", L"defaultproduct", L"default product", L"shipping", L"win64", L"win32", L"x64", L"x86", L"binaries", L"bin", L"engine" })
            if (value == generic) return false;
        return value.find(L"unrealengine") == std::wstring::npos && value.find(L"unreal engine") == std::wstring::npos;
    }
    struct Metadata { std::wstring product, description; };
    inline bool TitleDirectory(std::wstring const& candidate)
    {
        auto value = Lower(candidate);
        for (auto container : { L"games", L"game", L"program files", L"program files (x86)", L"windows", L"system32", L"syswow64", L"users", L"desktop", L"downloads", L"documents", L"steam", L"steamapps", L"common", L"epic games", L"gog games", L"apps", L"applications", L"tools", L"temp" })
            if (value == container) return false;
        return Meaningful(candidate);
    }
    inline std::wstring SuggestName(std::filesystem::path const& file, Metadata const& metadata)
    {
        if (Meaningful(metadata.product)) return Clean(metadata.product);
        if (Meaningful(metadata.description)) return Clean(metadata.description);
        // Only infer a title from recognized binary layouts, never arbitrary ancestors.
        auto parent = file.parent_path(); auto leaf = Lower(parent.filename().native());
        if (leaf == L"win64" || leaf == L"win32" || leaf == L"x64" || leaf == L"x86") parent = parent.parent_path();
        auto layout = Lower(parent.filename().native());
        if (layout == L"binaries" || layout == L"bin")
        {
            auto candidate = Clean(parent.parent_path().filename().native());
            if (TitleDirectory(candidate)) return candidate;
        }
        auto stem = Clean(file.stem().native()); auto lower = Lower(stem);
        for (auto suffix : { L"-win64-shipping", L"-win32-shipping", L"-shipping" })
        {
            auto length = std::wcslen(suffix);
            if (lower.size() > length && lower.compare(lower.size() - length, length, suffix) == 0) { stem.resize(stem.size() - length); break; }
        }
        return Clean(stem);
    }
    inline std::wstring PathKey(std::wstring const& path) { return Lower(std::filesystem::path(path).lexically_normal().native()); }
    struct PathLess
    {
        bool operator()(std::wstring const& a, std::wstring const& b) const
        {
            return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
        }
    };
    struct Process { std::uint32_t pid{}; std::uint64_t created{}; std::wstring path; bool visible{}; };
    inline void AddVisibleWindowProcess(HWND window, std::set<DWORD>& processes)
    {
        // A top-level application window can be visible before it has a caption.
        if (!::IsWindowVisible(window)) return;
        DWORD pid{};
        ::GetWindowThreadProcessId(window, &pid);
        if (pid) processes.insert(pid);
    }
    struct Application { Process process; std::wstring name; unsigned instances{1}; };
    using Query = std::function<std::optional<Process>(std::uint32_t)>;
    // Process creation time sent back by the editor as decimal text.
    inline std::optional<std::uint64_t> ParseCreationTime(std::wstring const& text)
    {
        if (text.empty() || text.size() > 20 || text.find_first_not_of(L"0123456789") != std::wstring::npos) return std::nullopt;
        std::uint64_t value{};
        for (auto c : text)
        {
            auto digit = static_cast<std::uint64_t>(c - L'0');
            if (value > (UINT64_MAX - digit) / 10) return std::nullopt;
            value = value * 10 + digit;
        }
        return value;
    }
    inline bool SameProcess(Process const& selected, Query const& query)
    {
        auto current = query(selected.pid);
        if (!current || !selected.created || current->created != selected.created || current->path.empty()) return false;
        auto a = PathKey(current->path), b = PathKey(selected.path);
        return !PathLess{}(a,b) && !PathLess{}(b,a);
    }
    inline std::vector<Application> Group(std::vector<Process> const& processes, std::function<Metadata(std::wstring const&)> const& metadata)
    {
        std::map<std::wstring, Application, PathLess> groups;
        for (auto const& process : processes)
        {
            if (process.path.empty() || !process.created || Lower(std::filesystem::path(process.path).extension().native()) != L".exe") continue;
            auto key = PathKey(process.path); auto found = groups.find(key);
            if (found != groups.end()) { ++found->second.instances; found->second.process.visible |= process.visible; continue; }
            if (groups.size() >= 512) continue;
            groups.emplace(key, Application{process, SuggestName(process.path, metadata(process.path)), 1});
        }
        std::vector<Application> result;
        for (auto& item : groups) result.push_back(std::move(item.second));
        std::sort(result.begin(), result.end(), [](auto const& a, auto const& b) {
            if (a.process.visible != b.process.visible) return a.process.visible > b.process.visible;
            if (Lower(a.name) != Lower(b.name)) return Lower(a.name) < Lower(b.name);
            return PathKey(a.process.path) < PathKey(b.process.path);
        });
        return result;
    }
    bool RunHelper();
}
