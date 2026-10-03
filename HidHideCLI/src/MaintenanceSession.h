// SPDX-License-Identifier: MIT
#pragma once
#include "FilterDriverProxy.h"
#include "StartupEntry.h"
#include <iostream>
#include <vector>

namespace HidHide
{
    inline void RemoveOwnedStartupValues(HKEY run, std::wstring const& currentCommand, std::wstring const& legacyCommand)
    {
        for (auto name : { L"HidHide Profiles", L"HidHide App Profiles" })
        {
            std::vector<wchar_t> value(32768); DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
            if (::RegGetValueW(run, nullptr, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) == ERROR_SUCCESS
                && bytes >= sizeof(wchar_t) && bytes <= value.size() * sizeof(wchar_t) && bytes % sizeof(wchar_t) == 0
                && value[bytes / sizeof(wchar_t) - 1] == L'\0'
                && OwnedStartupCommand(std::wstring(value.data(), bytes / sizeof(wchar_t) - 1), currentCommand, legacyCommand))
            {
                auto removed = ::RegDeleteValueW(run, name);
                if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND)
                    throw std::runtime_error("Could not remove the owned startup command");
            }
        }
    }

    inline void RemoveOwnedStartupAfterUninstall(std::wstring const& transaction)
    {
        // Release is also signalled on failure. Clean startup only after the
        // protected worker confirms this uninstall's successful restart checkpoint.
        HKEY marker{};
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\mikeev261\\HidHide\\Maintenance", 0,
            KEY_QUERY_VALUE | KEY_WOW64_64KEY, &marker) != ERROR_SUCCESS) return;
        auto text = [marker](PCWSTR name)
        {
            wchar_t value[64]{}; DWORD size = sizeof(value);
            return ::RegGetValueW(marker, nullptr, name, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS ? std::wstring(value) : std::wstring();
        };
        DWORD restart{}, size = sizeof(restart);
        bool confirmed = text(L"Transaction") == transaction && text(L"Operation") == L"uninstall"
            && ::RegGetValueW(marker, nullptr, L"RestartRequired", RRF_RT_REG_DWORD, nullptr, &restart, &size) == ERROR_SUCCESS && restart == 1;
        ::RegCloseKey(marker);
        if (!confirmed) return;
        std::vector<wchar_t> image(32768);
        auto length = ::GetModuleFileNameW(nullptr, image.data(), static_cast<DWORD>(image.size()));
        if (!length || length >= image.size()) throw std::runtime_error("Cannot identify the installed startup command");
        auto directory = std::filesystem::path(std::wstring(image.data(), length)).parent_path();
        auto currentCommand = L"\"" + (directory / L"HidHideClient.exe").wstring() + L"\" --background";
        auto legacyCommand = L"\"" + (directory.parent_path() / L"HidHide App Profiles" / L"HidHideClient.exe").wstring() + L"\" --background";
        HKEY run{};
        auto opened = ::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &run);
        if (opened == ERROR_FILE_NOT_FOUND) return;
        if (opened != ERROR_SUCCESS) throw std::runtime_error("Cannot remove the owned startup command");
        try { RemoveOwnedStartupValues(run, currentCommand, legacyCommand); }
        catch (...) { ::RegCloseKey(run); throw; }
        ::RegCloseKey(run);
    }
    inline bool ValidMsiTransactionId(std::wstring const& value)
    {
        if (value.size() != 36) return false;
        for (size_t index = 0; index < value.size(); ++index)
        {
            if (index == 8 || index == 13 || index == 18 || index == 23)
            {
                if (value[index] != L'-') return false;
            }
            else if (!iswxdigit(value[index])) return false;
        }
        return true;
    }

    inline std::wstring MsiEventName(PCWSTR role, std::wstring const& transaction)
    {
        if (!ValidMsiTransactionId(transaction)) throw std::runtime_error("Invalid Windows Installer transaction identity");
        return std::wstring(L"Global\\HidHide.Profiles.Msi.") + role + L"." + transaction;
    }

    // Windows Installer runs this helper as the initiating ordinary user before
    // its elevated execute sequence. It restores the confirmed baseline, hands
    // configuration ownership to the protected MSI action, and retains the
    // barrier until that action has created the administrator-owned durable
    // marker. No profile data or caller-selected path crosses the boundary.
    inline void RunMsiMaintenanceSession(std::wstring const& transaction, bool uninstall = false)
    {
        Maintenance::RequireOrdinaryUser();
        auto session = FilterDriverProxy::BeginMaintenance();
        Channel::Security security(L"D:P(A;;0x00100002;;;AU)(A;;GA;;;SY)(A;;GA;;;BA)");
        auto ready = Channel::Own(::CreateEventExW(&security.attributes, MsiEventName(L"Ready", transaction).c_str(),
            CREATE_EVENT_MANUAL_RESET, SYNCHRONIZE | EVENT_MODIFY_STATE));
        auto release = Channel::Own(::CreateEventExW(&security.attributes, MsiEventName(L"Release", transaction).c_str(),
            CREATE_EVENT_MANUAL_RESET, SYNCHRONIZE | EVENT_MODIFY_STATE));
        if (!ready || !release) throw std::runtime_error("Cannot create Windows Installer maintenance events");
        session->Handoff();
        if (!::SetEvent(ready.get())) throw std::runtime_error("Cannot signal Windows Installer maintenance readiness");
        auto const result = ::WaitForSingleObject(release.get(), 5 * 60 * 1000);
        if (result != WAIT_OBJECT_0) throw std::runtime_error("Windows Installer maintenance preparation timed out");
        if (uninstall) RemoveOwnedStartupAfterUninstall(transaction);
    }

    // An internal setup interface, invoked before elevation by the initiating
    // user's controller. No executable/path/SID arguments or commands are accepted.
    inline void RunMaintenanceSession()
    {
        auto const input = ::GetStdHandle(STD_INPUT_HANDLE);
        if (::GetFileType(input) != FILE_TYPE_PIPE || ::GetFileType(::GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_PIPE)
            throw std::runtime_error("Maintenance sessions require redirected setup-controller input and output pipes");
        auto session = FilterDriverProxy::BeginMaintenance();
        Protocol::Writer writer;
        writer.Number(2); // Setup-only READY protocol, independent of configuration IPC.
        writer.String(Channel::CurrentSid());
        writer.Number(session->Confirmed().driverPresent);
        writer.Number(session->Confirmed().driverPresent || session->Confirmed().storedBaselineAvailable);
        writer.State(session->Confirmed().configuration);
        constexpr char digits[] = "0123456789ABCDEF";
        std::string line = "READY ";
        line.reserve(7 + writer.data.size() * 2);
        for (auto byte : writer.data) { line.push_back(digits[byte >> 4]); line.push_back(digits[byte & 15]); }
        Maintenance::WriteControllerLine(::GetStdHandle(STD_OUTPUT_HANDLE), line);

        // Anonymous pipe reads are synchronous. Peek before reading a single byte
        // so an unresponsive controller cannot make the input wait unbounded.
        auto const deadline = ::GetTickCount64() + 30ULL * 60 * 1000;
        std::string command;
        while (::GetTickCount64() < deadline)
        {
            DWORD available{};
            if (!::PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr))
                throw std::runtime_error("Maintenance controller disconnected before release");
            if (!available) { ::Sleep(10); continue; }
            char character{}; DWORD read{};
            if (!::ReadFile(input, &character, 1, &read, nullptr) || read != 1)
                throw std::runtime_error("Maintenance controller input failed");
            if (character == '\n')
            {
                if (!command.empty() && command.back() == '\r') command.pop_back();
                if (Maintenance::ParseControl(command) == Maintenance::Control::Release) return;
                session->Handoff();
                Maintenance::WriteControllerLine(::GetStdHandle(STD_OUTPUT_HANDLE), "HANDED_OFF");
                command.clear();
            }
            else
            {
                if (command.size() >= 16) throw std::runtime_error("Oversized maintenance control");
                command.push_back(character);
            }
        }
        throw std::runtime_error("Maintenance controller session expired; re-detect before retrying setup");
    }
}
