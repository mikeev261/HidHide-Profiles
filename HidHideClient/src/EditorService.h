// SPDX-License-Identifier: MIT
#pragma once
#include "ProfilesDeviceSource.h"
#include "ProfilesCoordinator.h"
#include "ProfileApplicationService.h"
#include "ConfigurationChannel.h"

namespace HidHide::Editor
{
    inline constexpr auto PipeName = L"\\\\.\\pipe\\HidHide.Profiles.Editor.v1";
    // Called before any coordinator/driver construction. Only forwards a bounded
    // request to the authenticated ordinary-user owner; never opens the driver.
    bool RunRequestBridge();
    void Launch(bool engineLaunching = false);
    void RequireOrdinaryUser();
    std::wstring FixturePipeName(std::filesystem::path const& root);

    class Service
    {
    public:
        Service(Profiles::ProfileApplicationService& application, CProfilesCoordinator& coordinator, IProfilesDeviceSource& devices)
            : m_Application(application), m_Coordinator(coordinator), m_Devices(devices) {}
        std::vector<std::uint8_t> Handle(std::vector<std::uint8_t> const& request) noexcept;
        bool EditorOpen() const { return m_EditorProcess && ::WaitForSingleObject(m_EditorProcess.get(), 0) == WAIT_TIMEOUT; }
    private:
        Profiles::Json::Value Snapshot();
        Profiles::Json::Value Execute(Profiles::Json::Object const& request);
        Profiles::ProfileApplicationService& m_Application;
        CProfilesCoordinator& m_Coordinator;
        IProfilesDeviceSource& m_Devices;
        Channel::Handle m_EditorProcess{nullptr, &::CloseHandle};
        std::optional<Profiles::Settings> m_AdoptedSettings;
        // Lets the editor detect a restarted engine and register again.
        std::wstring const m_Instance{ Profiles::NewStableId() };
    };
}
