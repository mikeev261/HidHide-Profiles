// (c) Eric Korff de Gidts
// SPDX-License-Identifier: MIT
// HidHideClientDlg.cpp
#include "stdafx.h"
#include "HidHideClient.h"
#include "HidHideClientDlg.h"
#include "FilterDriverProxy.h"
#include "Utils.h"
#include "Logging.h"
#include "ManagerActivation.h"
#include <winver.h>
#pragma comment(lib, "version.lib")

UINT const WM_HIDHIDE_SHOW_MANAGER{ ::RegisterWindowMessageW(L"HidHide.AppProfiles.ShowManager") };
UINT const WM_TASKBAR_CREATED{ ::RegisterWindowMessageW(L"TaskbarCreated") };

namespace
{
    constexpr UINT WM_TRAY_ICON{ WM_APP + 1 };
    constexpr UINT WM_HIDE_AFTER_START{ WM_APP + 2 };
    constexpr UINT WM_DEVICES_CHANGED{ WM_APP + 3 };
    constexpr UINT WM_PROFILE_CHANGED{ WM_APP + 4 };
    constexpr UINT WM_CHANNEL_STATE_CHANGED{ WM_APP + 7 };
    constexpr UINT WM_CHANNEL_REQUEST{ WM_APP + 8 };
    struct ChannelDispatch
    {
        explicit ChannelDispatch(std::vector<std::uint8_t> value) : request(std::move(value)), completed(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
        { if (!completed) throw std::system_error(::GetLastError(), std::system_category(), "Create channel dispatch event"); }
        ~ChannelDispatch() { ::CloseHandle(completed); }
        void Release() { if (references.fetch_sub(1) == 1) delete this; }
        bool editor{};
        std::vector<std::uint8_t> request, response; std::exception_ptr failure; HANDLE completed{};
        std::atomic_uint references{ 2 }; std::atomic_bool cancelled{};
    };
    DWORD CALLBACK DevicesChanged(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA, DWORD)
    {
        // The main window unregisters synchronously in OnDestroy, before HWND reuse.
        // Never access MFC objects or the driver on Configuration Manager's thread.
        if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
            ::PostMessageW(static_cast<HWND>(context), WM_DEVICES_CHANGED, 0, 0);
        return ERROR_SUCCESS;
    }
    constexpr UINT TRAY_COMMAND_SHOW{ 1 };
    constexpr UINT TRAY_COMMAND_EXIT{ 2 };
    constexpr UINT ABOUT_COMMAND{ 0x0010 };

    std::wstring InstalledDriverVersion()
    {
        wchar_t system[MAX_PATH]{};
        auto length = ::GetSystemDirectoryW(system, MAX_PATH);
        if (!length || length >= MAX_PATH) return L"Unavailable";
        auto path = std::wstring(system) + L"\\drivers\\HidHide.sys";
        DWORD unused{};
        auto size = ::GetFileVersionInfoSizeW(path.c_str(), &unused);
        if (!size || size > 1024 * 1024) return L"Not installed or version unavailable";
        std::vector<BYTE> bytes(size);
        if (!::GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) return L"Unavailable";
        VS_FIXEDFILEINFO* info{}; UINT count{};
        if (!::VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &count) ||
            count < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xfeef04bd) return L"Unavailable";
        std::wostringstream version;
        version << HIWORD(info->dwFileVersionMS) << L'.' << LOWORD(info->dwFileVersionMS)
            << L'.' << HIWORD(info->dwFileVersionLS) << L'.' << LOWORD(info->dwFileVersionLS);
        return version.str();
    }
}

#pragma warning(push)
#pragma warning(disable: 26454 28213) // Warnings caused by Microsoft MFC macros
BEGIN_MESSAGE_MAP(CHidHideClientDlg, CDialogEx)
    ON_WM_PAINT()
    ON_WM_QUERYDRAGICON()
    ON_WM_WINDOWPOSCHANGING()
    ON_WM_CLOSE()
    ON_WM_DESTROY()
    ON_WM_QUERYENDSESSION()
    ON_WM_ENDSESSION()
    ON_WM_SYSCOMMAND()
    ON_MESSAGE(WM_DEVICES_CHANGED, &CHidHideClientDlg::OnDevicesChanged)
    ON_MESSAGE(WM_PROFILE_CHANGED, &CHidHideClientDlg::OnProfileChanged)
    ON_MESSAGE(WM_CHANNEL_STATE_CHANGED, &CHidHideClientDlg::OnChannelStateChanged)
    ON_MESSAGE(WM_CHANNEL_REQUEST, &CHidHideClientDlg::OnChannelRequest)
    ON_MESSAGE(WM_TRAY_ICON, &CHidHideClientDlg::OnTrayIcon)
    ON_MESSAGE(WM_HIDE_AFTER_START, &CHidHideClientDlg::OnHideAfterStart)
    ON_REGISTERED_MESSAGE(WM_HIDHIDE_SHOW_MANAGER, &CHidHideClientDlg::OnShowManager)
    ON_REGISTERED_MESSAGE(WM_TASKBAR_CREATED, &CHidHideClientDlg::OnTaskbarCreated)
END_MESSAGE_MAP()
#pragma warning(pop)

_Use_decl_annotations_
CHidHideClientDlg::CHidHideClientDlg(CWnd* pParent, bool startHidden)
    : CDialogEx(IDD_DIALOG_APPLICATION, pParent)
    , m_FilterDriverProxy{}
    , m_ProfilesEnforcement{}
    , m_ProfileApplication{}
    , m_ProfilesCoordinator{}
    , m_ProfileDevices{}
    , m_DropTarget{}
    , m_hIcon{}
    , m_TabApplication{}
    , m_BlacklistDlg(*this, nullptr)
    , m_WhitelistDlg(*this, nullptr)
    , m_StartHidden(startHidden)
{
    TRACE_ALWAYS(L"");
    m_hIcon = ::AfxGetApp()->LoadIcon(IDR_DIALOG_APPLICATION);
}

_Use_decl_annotations_
CHidHideClientDlg::CHidHideClientDlg(CWnd* pParent, ProfilesAcceptanceContext& acceptance)
    : CHidHideClientDlg(pParent, false)
{
    m_Acceptance = &acceptance;
}

bool CHidHideClientDlg::AcceptanceDeviceBurstCoalesced()
{
    if (!m_Acceptance) return false; auto before = m_Acceptance->devices.RefreshCount();
    for (int event{}; event < 100; ++event) PostMessageW(WM_DEVICES_CHANGED); MSG message{};
    auto deadline = ::GetTickCount64() + 1000;
    while (::GetTickCount64() < deadline) { while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&message); ::DispatchMessageW(&message); } ::Sleep(10); }
    auto refreshes = m_Acceptance->devices.RefreshCount() - before; return refreshes >= 1 && refreshes <= 2;
}

HidHide::FilterDriverProxy& CHidHideClientDlg::FilterDriverProxy() noexcept
{
    return (*m_FilterDriverProxy.get());
}

bool CHidHideClientDlg::EffectiveHidingEnabled() const
{
    return HidHide::FilterDriverProxy::ReadDriverConfiguration().active;
}

_Use_decl_annotations_
void CHidHideClientDlg::DoDataExchange(CDataExchange* pDX)
{
    TRACE_ALWAYS(L"");
    CDialogEx::DoDataExchange(pDX);
}

BOOL CHidHideClientDlg::OnInitDialog()
{
    TRACE_ALWAYS(L"");
    CDialogEx::OnInitDialog();
    SetWindowTextW(L"HidHide Profiles");
    if (auto menu = GetSystemMenu(FALSE))
    {
        menu->AppendMenuW(MF_SEPARATOR);
        menu->AppendMenuW(MF_STRING, ABOUT_COMMAND, L"About HidHide Profiles...");
    }

    if (m_Acceptance)
    {
        m_ProfileApplication = std::make_unique<HidHide::Profiles::ProfileApplicationService>(m_Acceptance->repositoryRoot);
        auto initial = m_Acceptance->enforcement.Observe();
        m_ProfileApplication->OpenOrCreateObserved(initial.success && initial.observedKnown && !initial.conflict
            ? std::optional<std::set<std::filesystem::path>>(initial.observed.allowedApplications) : std::nullopt);
        m_ProfilesCoordinator = std::make_unique<CProfilesCoordinator>(m_Acceptance->enforcement, m_Acceptance->repositoryRoot,
            false, m_Acceptance->processes, CProfilesCoordinator::StartupIntegration{}, [] { return false; }, initial);
        m_ProfilesCoordinator->SetNotificationWindow(m_hWnd, WM_PROFILE_CHANGED);
        m_DeviceStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr); m_DeviceWake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_DeviceStop || !m_DeviceWake) THROW_WIN32_LAST_ERROR; m_DeviceWorker = std::thread(&CHidHideClientDlg::DeviceWorkerMain, this);
        m_ProfilesCoordinator->Tick();
        return TRUE;
    }

    m_FilterDriverProxy = std::make_unique<HidHide::FilterDriverProxy>(true, true);

    // Register this window as a drop target
    m_DropTarget.Register(this);

    // Set the dialog title and include the version number, as defined via a define from the build environment
    std::wostringstream title;
    title << HidHide::StringTable(IDS_DIALOG_APPLICATION) << L" v" << _L(BldProductVersion);
    SetWindowTextW(title.str().c_str());

    m_ProfilesEnforcement = std::make_unique<CProfilesEnforcementAdapter>(*m_FilterDriverProxy);
    auto repositoryRoot = HidHide::Profiles::DefaultRepositoryRoot(); m_ProfileApplication = std::make_unique<HidHide::Profiles::ProfileApplicationService>(repositoryRoot);
    auto initial = m_ProfilesEnforcement->Observe();
    m_ProfileApplication->OpenOrCreateObserved(initial.success && initial.observedKnown && !initial.conflict
        ? std::optional<std::set<std::filesystem::path>>(initial.observed.allowedApplications) : std::nullopt);
    m_ProfilesCoordinator = std::make_unique<CProfilesCoordinator>(*m_ProfilesEnforcement, repositoryRoot, true,
        CProfilesCoordinator::ProcessSource{}, CProfilesCoordinator::StartupIntegration{}, CProfilesCoordinator::MaintenanceSource{}, initial);
    m_ProfilesEnforcement->InstallConfigurationChannel();
    m_ProfilesCoordinator->SetNotificationWindow(m_hWnd, WM_PROFILE_CHANGED);
    // Production owns no profile controls: the editor is an independent process.
    m_EditorService = std::make_unique<HidHide::Editor::Service>(*m_ProfileApplication, *m_ProfilesCoordinator, m_ProfileDevices);
    m_EditorServer = std::make_unique<HidHide::Channel::Server>(HidHide::Editor::PipeName, true);
    m_FilterDriverProxy->SetMaintenanceHandler([this]
    {
        if (m_Exiting || !IsWindowEnabled()) throw std::runtime_error("Close open HidHide dialogs before running setup");
        if (m_EditorService && m_EditorService->EditorOpen()) throw std::runtime_error("Close the HidHide Profiles editor before running setup. Apply or discard its unsaved changes first");
        if (m_ProfilesCoordinator && m_ProfilesCoordinator->HasLaunchedProcess())
            throw std::runtime_error("Close the directly launched application before running setup maintenance");
        auto confirmed = m_ProfilesEnforcement->PrepareMaintenance(); m_MaintenancePrepared = true; EnableWindow(FALSE); return confirmed;
    });
    m_ConfigurationServer = std::make_unique<HidHide::Channel::Server>();
    m_ProfilesCoordinator->Tick();
    AddTrayIcon();
    m_ChannelStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr); if (!m_ChannelStop) THROW_WIN32_LAST_ERROR;
    m_DeviceStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr); m_DeviceWake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_DeviceStop || !m_DeviceWake) THROW_WIN32_LAST_ERROR;
    PostMessageW(WM_HIDE_AFTER_START);
    CM_NOTIFY_FILTER filter{ sizeof(CM_NOTIFY_FILTER), CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES, CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE, 0, 0 };
    if (auto const result = ::CM_Register_Notification(&filter, m_hWnd, &DevicesChanged, &m_DeviceNotification); result != CR_SUCCESS)
        THROW_CONFIGRET(result);

    if (!::SetPropW(m_hWnd, HidHide::ManagerActivation::WindowProperty, reinterpret_cast<HANDLE>(1)))
        THROW_WIN32_LAST_ERROR;
    try
    {
        m_ChannelWorker = std::thread(&CHidHideClientDlg::ChannelWorkerMain, this);
        m_DeviceWorker = std::thread(&CHidHideClientDlg::DeviceWorkerMain, this); ::SetEvent(m_DeviceWake);
    }
    catch (...)
    {
        ::SetEvent(m_ChannelStop); ::SetEvent(m_DeviceStop);
        if (m_ChannelWorker.joinable()) m_ChannelWorker.join(); if (m_DeviceWorker.joinable()) m_DeviceWorker.join(); throw;
    }
    return (TRUE);
}

void CHidHideClientDlg::AddTrayIcon()
{
    m_LastTrayProfileCount = static_cast<size_t>(-1);
    m_NotifyIcon = {};
    m_NotifyIcon.cbSize = sizeof(m_NotifyIcon);
    m_NotifyIcon.hWnd = m_hWnd;
    m_NotifyIcon.uID = 1;
    m_NotifyIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    m_NotifyIcon.uCallbackMessage = WM_TRAY_ICON;
    m_NotifyIcon.hIcon = m_hIcon;
    wcscpy_s(m_NotifyIcon.szTip, L"HidHide Profiles");
    m_TrayIconAdded = FALSE != ::Shell_NotifyIconW(NIM_ADD, &m_NotifyIcon);
}

void CHidHideClientDlg::RemoveTrayIcon() noexcept
{
    if (nullptr != m_NotifyIcon.hWnd) ::Shell_NotifyIconW(NIM_DELETE, &m_NotifyIcon);
    m_NotifyIcon.hWnd = nullptr; m_TrayIconAdded = false;
}

void CHidHideClientDlg::HideToTray()
{
    ShowWindow(SW_HIDE);
    if (!m_HideNoticeShown)
    {
        m_HideNoticeShown = true;
        m_NotifyIcon.uFlags = NIF_INFO;
        wcscpy_s(m_NotifyIcon.szInfoTitle, L"HidHide Profiles");
        wcscpy_s(m_NotifyIcon.szInfo, L"Profile monitoring is still running. Use the tray icon to reopen or exit.");
        m_NotifyIcon.dwInfoFlags = NIIF_INFO;
        ::Shell_NotifyIconW(NIM_MODIFY, &m_NotifyIcon);
    }
}

void CHidHideClientDlg::ShowFromTray()
{
    if (m_Acceptance) return;
    try { HidHide::Editor::Launch(); }
    catch (std::exception const& error) { ::MessageBoxA(m_hWnd, error.what(), "HidHide Profiles", MB_OK | MB_ICONERROR); }
}

void CHidHideClientDlg::UpdateTrayTooltip()
{
    if (!m_ProfilesCoordinator) return;
    auto const& selection = m_ProfilesCoordinator->EffectiveSelection(); auto const& snapshot = m_ProfilesCoordinator->Snapshot();
    auto selected = snapshot.profiles.find(selection.profileId); std::wstring profileLabel;
    if (selected != snapshot.profiles.end())
    {
        profileLabel = selected->second.name;
        if (!m_ProfilesCoordinator->EffectiveSelectionVerified()) profileLabel += L" (saved selection; current unconfirmed)";
        else if (selection.reason == HidHide::Profiles::SelectionReason::Paused) profileLabel += L" (paused; hiding off)";
        else if (selection.reason == HidHide::Profiles::SelectionReason::ManualApplication) profileLabel += L" (manual mask override)";
        else if (selected->second.kind == HidHide::Profiles::Kind::Application) profileLabel += L" (running application)";
        else if (selection.reason == HidHide::Profiles::SelectionReason::ManualGlobal) profileLabel += L" (manual Use Global)";
        else profileLabel += L" (Automatic fallback)";
    }
    auto const activeProfileCount{ profileLabel.empty() ? 0u : 1u };
    auto const status = m_ProfilesCoordinator->Status();
    if (m_LastTrayProfileCount == activeProfileCount && m_LastTrayProfileLabel == profileLabel && m_LastStatus == status) return;

    std::wostringstream text;
    text << L"HidHide Profiles v" << _L(BldProductVersion);
    if (!profileLabel.empty()) text << L" - " << profileLabel;
    text << L" - " << status;
    SetWindowTextW(L"HidHide Profiles");
    m_NotifyIcon.uFlags = NIF_TIP;
    wcsncpy_s(m_NotifyIcon.szTip, text.str().c_str(), _TRUNCATE);
    ::Shell_NotifyIconW(NIM_MODIFY, &m_NotifyIcon);
    m_LastTrayProfileCount = activeProfileCount;
    m_LastTrayProfileLabel = std::move(profileLabel);
    m_LastStatus = status;
}

void CHidHideClientDlg::OnPaint()
{
    TRACE_ALWAYS(L"");
    if (IsIconic())
    {
        CPaintDC dc(this); // device context for painting

        SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

        // Center icon in client rectangle
        int cxIcon = GetSystemMetrics(SM_CXICON);
        int cyIcon = GetSystemMetrics(SM_CYICON);
        CRect rect;
        GetClientRect(&rect);
        int x = (rect.Width() - cxIcon + 1) / 2;
        int y = (rect.Height() - cyIcon + 1) / 2;

        // Draw the icon
        dc.DrawIcon(x, y, m_hIcon);
    }
    else
    {
        CDialogEx::OnPaint();
    }
}

HCURSOR CHidHideClientDlg::OnQueryDragIcon()
{
    TRACE_ALWAYS(L"");
    return static_cast<HCURSOR>(m_hIcon);
}

void CHidHideClientDlg::OnWindowPosChanging(WINDOWPOS* position)
{
    // MFC's modal loop attempts to show on idle; suppress that in the resident
    // engine, including launch/maximize messages, without constructing a page.
    position->flags &= ~SWP_SHOWWINDOW; position->flags |= SWP_HIDEWINDOW;
    CDialogEx::OnWindowPosChanging(position);
}

void CHidHideClientDlg::ChannelWorkerMain() noexcept
{
    HANDLE waits[]{ m_ChannelStop, m_ConfigurationServer->WakeHandle(), m_EditorServer->WakeHandle() };
    while (true)
    {
        try
        {
            auto timeout = std::min(m_ConfigurationServer->WaitTimeoutMs(), m_EditorServer->WaitTimeoutMs());
            if (::WaitForMultipleObjects(3, waits, FALSE, timeout) == WAIT_OBJECT_0) return;
            auto dispatchRequest = [this](auto const& request, bool editor)
            {
                auto dispatch = new ChannelDispatch(request); dispatch->editor = editor;
                if (!::PostMessageW(m_hWnd, WM_CHANNEL_REQUEST, 0, reinterpret_cast<LPARAM>(dispatch)))
                { dispatch->Release(); dispatch->Release(); throw std::runtime_error("Configuration request could not reach the owner window"); }
                HANDLE waits[]{ m_ChannelStop, dispatch->completed }; auto wait = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wait != WAIT_OBJECT_0 + 1) { dispatch->cancelled = true; dispatch->Release(); throw std::runtime_error("Configuration request was cancelled during shutdown"); }
                auto failure = dispatch->failure; auto response = std::move(dispatch->response); dispatch->Release();
                if (failure) std::rethrow_exception(failure); return response;
            };
            if (m_ConfigurationServer) m_ConfigurationServer->Pump([&](auto const& request) { return dispatchRequest(request, false); });
            if (m_EditorServer) m_EditorServer->Pump([&](auto const& request) { return dispatchRequest(request, true); });
            if (m_MaintenancePrepared)
            {
                if (!m_ConfigurationServer->Connected()) { ::PostMessageW(m_hWnd, WM_CHANNEL_STATE_CHANGED, 1, 0); return; }
            }
        }
        catch (...) { ::PostMessageW(m_hWnd, WM_CHANNEL_STATE_CHANGED, 0, 0); }
    }
}

void CHidHideClientDlg::DeviceWorkerMain() noexcept
{
    HANDLE waits[]{ m_DeviceStop, m_DeviceWake };
    while (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
    {
        // Bound enumeration to one pass per 250 ms window. Device-interface
        // storms are folded together without delaying stop responsiveness.
        auto deadline = ::GetTickCount64() + 250;
        for (;;)
        {
            auto now = ::GetTickCount64(); if (now >= deadline) break;
            auto wait = ::WaitForMultipleObjects(2, waits, FALSE, static_cast<DWORD>(deadline - now));
            if (wait == WAIT_OBJECT_0) return; if (wait == WAIT_TIMEOUT) break;
        }
        try { if (m_Acceptance) m_Acceptance->devices.Refresh(); else m_ProfileDevices.Refresh(); } catch (...) {}
    }
}

void CHidHideClientDlg::OnClose()
{
    HideToTray();
}

void CHidHideClientDlg::OnSysCommand(UINT id, LPARAM parameter)
{
    if ((id & 0xfff0) == ABOUT_COMMAND)
    {
        std::wstring message = L"HidHide Profiles\nProduct version: " + std::wstring(_L(BldProductVersion))
            + L"\nInstalled driver file version: " + InstalledDriverVersion()
            + L"\n\nBased on HidHide by Nefarius Software Solutions and Eric Korff de Gidts."
              L"\nThe Microsoft-signed upstream driver is distributed unchanged."
              L"\nSee the installed Driver\\LICENSE.rtf for upstream license terms.";
        MessageBoxW(message.c_str(), L"About HidHide Profiles", MB_OK | MB_ICONINFORMATION);
        return;
    }
    CDialogEx::OnSysCommand(id, parameter);
}

void CHidHideClientDlg::OnCancel()
{
    if (!m_Exiting)
    {
        OnClose();
        return;
    }
    CDialogEx::OnCancel();
}

void CHidHideClientDlg::OnOK()
{
    OnClose();
}

void CHidHideClientDlg::OnDestroy()
{
    ReleaseEngine();
    CDialogEx::OnDestroy();
}

BOOL CHidHideClientDlg::OnQueryEndSession()
{
    // Never veto and never restore here: the session end may still be cancelled.
    return TRUE;
}

void CHidHideClientDlg::OnEndSession(BOOL ending)
{
    // Logoff, shutdown and Restart Manager terminate the process soon after this
    // returns, without WM_DESTROY. Stop the engine and restore the baseline now,
    // as tray Exit does; if restoration is not confirmed the recovery record is
    // kept so the next start recovers. A cancelled session end changes nothing.
    if (!ending) return;
    auto const blocking{ ::ShutdownBlockReasonCreate(m_hWnd, L"Restoring HidHide device settings") != FALSE };
    ReleaseEngine();
    if (blocking) ::ShutdownBlockReasonDestroy(m_hWnd);
    m_Exiting = true; CDialogEx::OnCancel();
}

// Idempotent: runs from WM_ENDSESSION and again from WM_DESTROY.
void CHidHideClientDlg::ReleaseEngine()
{
    if (!m_Acceptance) ::RemovePropW(m_hWnd, HidHide::ManagerActivation::WindowProperty);
    if (m_DeviceNotification)
    {
        ::CM_Unregister_Notification(m_DeviceNotification);
        m_DeviceNotification = nullptr;
    }
    if (m_ChannelStop) ::SetEvent(m_ChannelStop);
    if (m_DeviceStop) ::SetEvent(m_DeviceStop);
    if (m_ChannelWorker.joinable()) m_ChannelWorker.join();
    if (m_DeviceWorker.joinable()) m_DeviceWorker.join();
    MSG pending{}; while (::PeekMessageW(&pending, m_hWnd, WM_CHANNEL_REQUEST, WM_CHANNEL_REQUEST, PM_REMOVE))
    {
        auto dispatch = reinterpret_cast<ChannelDispatch*>(pending.lParam); dispatch->cancelled = true; dispatch->Release();
    }
    m_ConfigurationServer.reset();
    m_EditorServer.reset(); m_EditorService.reset();
    if (m_ChannelStop) { ::CloseHandle(m_ChannelStop); m_ChannelStop = nullptr; }
    if (m_DeviceStop) { ::CloseHandle(m_DeviceStop); m_DeviceStop = nullptr; }
    if (m_DeviceWake) { ::CloseHandle(m_DeviceWake); m_DeviceWake = nullptr; }
    if (m_ProfilesCoordinator) { m_ProfilesCoordinator->SetNotificationWindow(nullptr, 0); m_ProfilesCoordinator->Stop(); }
    RemoveTrayIcon();
}

_Use_decl_annotations_
LRESULT CHidHideClientDlg::OnTrayIcon(WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);
    auto const message{ static_cast<UINT>(lParam) };
    if ((WM_LBUTTONDBLCLK == message) || (WM_LBUTTONUP == message))
    {
        ShowFromTray();
    }
    else if ((WM_RBUTTONUP == message) || (WM_CONTEXTMENU == message))
    {
        CMenu menu;
        menu.CreatePopupMenu();
        menu.AppendMenuW(MF_STRING, TRAY_COMMAND_SHOW, L"Open HidHide Profiles");
        menu.AppendMenuW(MF_SEPARATOR);
        menu.AppendMenuW(MF_STRING, TRAY_COMMAND_EXIT, L"Exit and restore device settings");

        CPoint point;
        ::GetCursorPos(&point);
        SetForegroundWindow();
        auto const command{ menu.TrackPopupMenu(TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, this) };
        if (TRAY_COMMAND_SHOW == command) ShowFromTray();
        if (TRAY_COMMAND_EXIT == command)
        {
            try { m_ProfilesCoordinator->ExitSafely(); m_Exiting = true; CDialogEx::OnCancel(); }
            catch (std::exception const& error) { ::MessageBoxA(m_hWnd, error.what(), "HidHide restoration was not confirmed", MB_OK | MB_ICONERROR); }
        }
    }
    return 0;
}

_Use_decl_annotations_
LRESULT CHidHideClientDlg::OnHideAfterStart(WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);
    if (!m_Acceptance)
    {
        ShowWindow(SW_HIDE);
        if (!m_StartHidden && !std::exchange(m_EditorPrelaunched, false)) ShowFromTray();
    }
    else if (m_StartHidden) ShowWindow(SW_HIDE);
    return 0;
}

_Use_decl_annotations_
LRESULT CHidHideClientDlg::OnShowManager(WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);
    m_StartHidden = false;
    ShowFromTray();
    return HidHide::ManagerActivation::Acknowledged;
}

_Use_decl_annotations_
LRESULT CHidHideClientDlg::OnTaskbarCreated(WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(wParam);
    UNREFERENCED_PARAMETER(lParam);
    AddTrayIcon();
    UpdateTrayTooltip();
    return 0;
}

HidHide::DeviceInstancePaths CHidHideClientDlg::Baseline()
{ return m_FilterDriverProxy->GetBlacklist(); }

void CHidHideClientDlg::EditBaseline(HidHide::DeviceInstancePaths const& displayed, HidHide::DeviceInstancePaths const& requested)
{ m_FilterDriverProxy->SetBlacklist(displayed, requested); }

void CHidHideClientDlg::SetEnabled(bool displayed, bool requested)
{
    UNREFERENCED_PARAMETER(displayed); UNREFERENCED_PARAMETER(requested);
}

LRESULT CHidHideClientDlg::OnDevicesChanged(WPARAM, LPARAM)
{
    if (m_DeviceWake) ::SetEvent(m_DeviceWake);
    return 0;
}

LRESULT CHidHideClientDlg::OnProfileChanged(WPARAM repositoryChange, LPARAM)
{
    if (repositoryChange && m_ProfilesCoordinator) m_ProfilesCoordinator->ReloadRepositoryIfChanged();
    if (m_ProfilesCoordinator) m_ProfilesCoordinator->Tick();
    UpdateTrayTooltip();
    return 0;
}

LRESULT CHidHideClientDlg::OnChannelStateChanged(WPARAM maintenanceComplete, LPARAM)
{
    if (maintenanceComplete) { m_Exiting = true; CDialogEx::OnCancel(); return 0; }
    UpdateTrayTooltip(); return 0;
}

LRESULT CHidHideClientDlg::OnChannelRequest(WPARAM, LPARAM value)
{
    auto& dispatch = *reinterpret_cast<ChannelDispatch*>(value);
    try
    {
        if (m_Exiting || dispatch.cancelled) throw std::runtime_error("HidHide Profiles is closing");
        if (dispatch.editor)
        {
            if (m_MaintenancePrepared || !m_EditorService) throw std::runtime_error("Setup maintenance is in progress");
            dispatch.response = m_EditorService->Handle(dispatch.request);
        }
        else dispatch.response = m_FilterDriverProxy->HandleRequest(dispatch.request);
        // The editor snapshot observes directly and editor mutations reconcile
        // through the coordinator; a second readback here repeated driver I/O.
        if (!dispatch.editor && m_ProfilesCoordinator) (void)m_ProfilesCoordinator->ObserveEnforcement();
        UpdateTrayTooltip();
    }
    catch (...) { dispatch.failure = std::current_exception(); }
    ::SetEvent(dispatch.completed); dispatch.Release(); return TRUE;
}
