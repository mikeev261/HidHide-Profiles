// SPDX-License-Identifier: MIT
#include "stdafx.h"
#include "ApplicationDiscovery.h"
#include "ProfileJson.h"
#include <TlHelp32.h>
#include <set>
#pragma comment(lib, "Version.lib")

namespace HidHide::Applications
{
    namespace
    {
        struct Handle { HANDLE value{}; ~Handle() { if (value && value != INVALID_HANDLE_VALUE) ::CloseHandle(value); } };
        bool LocalExecutable(std::wstring const& path)
        {
            return path.size() <= 32767 && path.size() >= 7 && std::iswalpha(path[0]) && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')
                && std::none_of(path.begin(), path.end(), [](wchar_t c) { return std::iswcntrl(c); }) && path.find(L':', 2) == std::wstring::npos
                && Lower(std::filesystem::path(path).extension().native()) == L".exe";
        }
        std::optional<Process> QueryProcess(std::uint32_t pid)
        {
            Handle handle{::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid)};
            if (!handle.value) return {};
            FILETIME created{}, exit{}, kernel{}, user{};
            std::wstring path(32768, L'\0'); DWORD size = static_cast<DWORD>(path.size());
            if (!::GetProcessTimes(handle.value, &created, &exit, &kernel, &user) || !::QueryFullProcessImageNameW(handle.value, 0, path.data(), &size)
                || ::WaitForSingleObject(handle.value, 0) != WAIT_TIMEOUT) return {};
            path.resize(size);
            if (!LocalExecutable(path)) return {};
            return Process{pid, (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime, path, false};
        }
        void RequireFile(std::wstring const& path)
        {
            if (!LocalExecutable(path))
                throw std::runtime_error("Choose a local executable file.");
            auto attributes = ::GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("The executable is no longer available.");
        }
        Metadata ReadMetadata(std::wstring const& path)
        {
            DWORD ignored{}; auto size = ::GetFileVersionInfoSizeW(path.c_str(), &ignored);
            if (!size || size > 1024 * 1024) return {};
            std::vector<BYTE> bytes(size);
            if (!::GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) return {};
            struct Translation { WORD language, codepage; };
            Translation* translations{}; UINT length{};
            if (!::VerQueryValueW(bytes.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length)) return {};
            std::vector<Translation> languages(translations, translations + (std::min)(length / sizeof(Translation), static_cast<size_t>(64)));
            auto preferred = ::GetUserDefaultUILanguage();
            std::stable_sort(languages.begin(), languages.end(), [preferred](auto a, auto b) { return (a.language == preferred) > (b.language == preferred); });
            Metadata result;
            for (auto translation : languages)
            {
                auto read = [&](wchar_t const* field) {
                    wchar_t key[128]{}; swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\%s", translation.language, translation.codepage, field);
                    wchar_t* value{}; UINT count{};
                    if (!::VerQueryValueW(bytes.data(), key, reinterpret_cast<void**>(&value), &count) || !count || count > 4096) return std::wstring{};
                    return Clean(std::wstring(value, count - 1));
                };
                auto product = read(L"ProductName"), description = read(L"FileDescription");
                if (result.product.empty() && Meaningful(product)) result.product = product;
                if (result.description.empty() && Meaningful(description)) result.description = description;
            }
            return result;
        }
        struct Scan { std::vector<Process> processes; unsigned unavailable{}; bool truncated{}; };
        Scan Enumerate()
        {
            std::set<DWORD> visible;
            ::EnumWindows([](HWND window, LPARAM data) -> BOOL {
                AddVisibleWindowProcess(window, *reinterpret_cast<std::set<DWORD>*>(data));
                return TRUE;
            }, reinterpret_cast<LPARAM>(&visible));
            Handle snapshot{::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
            if (snapshot.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not list running applications. Refresh to retry.");
            PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry); Scan result; DWORD session{}; unsigned examined{};
            if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &session)) throw std::runtime_error("Could not determine the current session.");
            if (::Process32FirstW(snapshot.value, &entry)) do {
                if (++examined > 16384) { result.truncated = true; break; }
                DWORD candidateSession{};
                if (!::ProcessIdToSessionId(entry.th32ProcessID, &candidateSession) || candidateSession != session) continue;
                auto process = QueryProcess(entry.th32ProcessID);
                if (process) { process->visible = visible.count(process->pid) != 0; result.processes.push_back(std::move(*process)); }
                else ++result.unavailable;
            } while (::Process32NextW(snapshot.value, &entry));
            std::stable_sort(result.processes.begin(), result.processes.end(), [](auto const& a, auto const& b) { return a.visible > b.visible; });
            return result;
        }
        std::wstring Encode(Application const& app)
        {
            using Profiles::Json::Escape;
            return L"{\"path\":" + Escape(app.process.path) + L",\"name\":" + Escape(app.name) + L",\"pid\":" + std::to_wstring(app.process.pid)
                + L",\"created\":" + Escape(std::to_wstring(app.process.created)) + L",\"visible\":" + (app.process.visible ? L"true" : L"false") + L",\"instances\":" + std::to_wstring(app.instances) + L"}";
        }
    }
    bool RunHelper()
    {
        // Recognize the mode even with bad arguments; never fall through to ownership/driver startup.
        if (__argc < 2 || _wcsicmp(__wargv[1], L"--application-discovery") != 0) return false;
        using namespace Profiles::Json;
        std::wstring response;
        try
        {
            if (__argc != 2) throw std::runtime_error("Invalid application discovery arguments.");
            Handle token; TOKEN_ELEVATION elevation{}; DWORD size{};
            if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token.value) || !::GetTokenInformation(token.value, TokenElevation, &elevation, sizeof(elevation), &size) || elevation.TokenIsElevated)
                throw std::runtime_error("Application discovery requires an ordinary user.");
            auto input = ::GetStdHandle(STD_INPUT_HANDLE);
            if (::GetFileType(input) != FILE_TYPE_PIPE) throw std::runtime_error("Application discovery requires piped input.");
            std::string bytes; char buffer[4096]; DWORD read{};
            while (::ReadFile(input, buffer, sizeof(buffer), &read, nullptr) && read) { bytes.append(buffer, read); if (bytes.size() > 128 * 1024) throw std::runtime_error("Application request is too large."); }
            auto root = Parser(FromUtf8(bytes)).Parse(); auto const& object = AsObject(root); auto command = AsString(Required(object, L"command"));
            if (command == L"list")
            {
                response = L"{\"applications\":["; bool first = true;
                auto scan = Enumerate(); auto deadline = ::GetTickCount64() + 8000; bool metadataPartial{};
                std::set<std::wstring, PathLess> paths;
                for (auto const& process : scan.processes) paths.insert(PathKey(process.path));
                auto applications = Group(scan.processes, [&](auto const& path) {
                    if (::GetTickCount64() >= deadline) { metadataPartial = true; return Metadata{}; }
                    return ReadMetadata(path);
                });
                for (auto const& item : applications) { if (!first) response += L","; first = false; response += Encode(item); }
                response += L"],\"unavailable\":" + std::to_wstring(scan.unavailable) + L",\"truncated\":" + (scan.truncated || paths.size() > 512 ? L"true" : L"false") + L",\"metadataPartial\":" + (metadataPartial ? L"true" : L"false") + L"}";
            }
            else if (command == L"describe" || command == L"validate" || command == L"validate-file")
            {
                auto path = AsString(Required(object, L"path")); RequireFile(path);
                if (command == L"validate")
                {
                    auto pid = AsUnsigned(Required(object, L"pid")); auto created = ParseCreationTime(AsString(Required(object, L"created")));
                    if (!pid || pid > UINT32_MAX || !created
                        || !SameProcess(Process{static_cast<std::uint32_t>(pid), *created, path, false}, QueryProcess))
                        throw std::runtime_error("This application has exited or changed. Refresh and choose it again.");
                }
                response = L"{\"application\":" + Encode(Application{Process{0, 0, path, false}, SuggestName(path, ReadMetadata(path)), 1}) + L"}";
            }
            else throw std::runtime_error("Unknown application discovery request.");
            if (ToUtf8(response).size() > 2 * 1024 * 1024) throw std::runtime_error("Too many applications to display.");
        }
        catch (std::exception const& error) { response = L"{\"error\":" + Escape(FromUtf8(error.what())) + L"}"; }
        auto bytes = ToUtf8(response); DWORD written{};
        ::WriteFile(::GetStdHandle(STD_OUTPUT_HANDLE), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        return true;
    }
}
