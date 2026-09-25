// (c) Eric Korff de Gidts
// SPDX-License-Identifier: MIT
// HidHideClientDlg.h
#pragma once
#include "IDropTarget.h"
#include "BlacklistDlg.h"
#include "WhitelistDlg.h"
#include "ProfilesCoordinator.h"
#include "ProfilesEnforcementAdapter.h"
#include "ProfilesDeviceSource.h"
#include "ConfigurationChannel.h"
#include "EditorService.h"

extern UINT const WM_HIDHIDE_SHOW_MANAGER;

struct ProfilesAcceptanceContext
{
    std::filesystem::path repositoryRoot;
    HidHide::Profiles::IEnforcement& enforcement;
    IProfilesDeviceSource& devices;
    CProfilesCoordinator::ProcessSource processes;
};

class CHidHideClientDlg : public CDialogEx, public HidHide::IDropTarget
{
public:

    CHidHideClientDlg() noexcept = delete;
    CHidHideClientDlg(_In_ CHidHideClientDlg const& rhs) = delete;
    CHidHideClientDlg(_In_ CHidHideClientDlg&& rhs) noexcept = delete;
    CHidHideClientDlg& operator=(_In_ CHidHideClientDlg const& rhs) = delete;
    CHidHideClientDlg& operator=(_In_ CHidHideClientDlg&& rhs) = delete;

    explicit CHidHideClientDlg(_In_opt_ CWnd* pParent, _In_ bool startHidden = false);
    CHidHideClientDlg(_In_opt_ CWnd* pParent, ProfilesAcceptanceContext& acceptance);
    virtual ~CHidHideClientDlg() = default;

    // Allow child dialogs access to the shared filter driver proxy
    HidHide::FilterDriverProxy& FilterDriverProxy() noexcept;
    HidHide::DeviceInstancePaths Baseline();
    void EditBaseline(HidHide::DeviceInstancePaths const& displayed, HidHide::DeviceInstancePaths const& requested);
    void SetEnabled(bool displayed, bool requested);
    bool EffectiveHidingEnabled() const;
    bool AcceptanceDeviceBurstCoalesced();
    void MarkEditorPrelaunched() { m_EditorPrelaunched = true; }

private:

    // Handler for drop target events
    class CDropTarget : public COleDropTarget
    {
    public:

        CDropTarget(_In_ CDropTarget const& rhs) = delete;
        CDropTarget(_In_ CDropTarget&& rhs) noexcept = delete;
        CDropTarget& operator=(_In_ CDropTarget const& rhs) = delete;
        CDropTarget& operator=(_In_ CDropTarget&& rhs) = delete;

        CDropTarget() noexcept : m_IDropTarget{} {};
        virtual ~CDropTarget() {};

        // Called when the cursor first enters the window
        DROPEFFECT OnDragEnter(_In_ CWnd* pWnd, _In_ COleDataObject* pDataObject, _In_ DWORD dwKeyState, _In_ CPoint point) override
        {
            return ((nullptr == m_IDropTarget) ? DROPEFFECT_NONE : m_IDropTarget->OnDragEnter(pWnd, pDataObject, dwKeyState, point));
        }

        // Called repeatedly when the cursor is dragged over the window
        DROPEFFECT OnDragOver(_In_ CWnd* pWnd, _In_ COleDataObject* pDataObject, _In_ DWORD dwKeyState, _In_ CPoint point) override
        {
            return ((nullptr == m_IDropTarget) ? DROPEFFECT_NONE : m_IDropTarget->OnDragOver(pWnd, pDataObject, dwKeyState, point));
        }

        // Called when data is dropped into the window, initial handler
        DROPEFFECT OnDropEx(_In_ CWnd* pWnd, _In_ COleDataObject* pDataObject, _In_ DROPEFFECT dropDefault, _In_ DROPEFFECT dropList, _In_ CPoint point) override
        {
            return ((nullptr == m_IDropTarget) ? DROPEFFECT_NONE : m_IDropTarget->OnDropEx(pWnd, pDataObject, dropDefault, dropList, point));
        }

        // Define redirection
        void SetRedirectionTarget(IDropTarget& iDropTarget)
        {
            m_IDropTarget = &iDropTarget;
        }

    private:
        IDropTarget* m_IDropTarget;
    };

    // Dialog Data
#ifdef AFX_DESIGN_TIME
    enum { IDD = IDD_DIALOG_APPLICATION };
#endif

    void DoDataExchange(_In_ CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    void OnCancel() override;
    void OnOK() override;

    DECLARE_MESSAGE_MAP()

    // Acquire exclusive access to the filter driver
    std::unique_ptr<HidHide::FilterDriverProxy> m_FilterDriverProxy;
    std::unique_ptr<CProfilesEnforcementAdapter> m_ProfilesEnforcement;
    std::unique_ptr<HidHide::Profiles::ProfileApplicationService> m_ProfileApplication;
    std::unique_ptr<CProfilesCoordinator> m_ProfilesCoordinator;
    CProductionProfilesDeviceSource m_ProfileDevices;
    ProfilesAcceptanceContext* m_Acceptance{};
    std::unique_ptr<HidHide::Channel::Server> m_ConfigurationServer;
    std::unique_ptr<HidHide::Channel::Server> m_EditorServer;
    std::unique_ptr<HidHide::Editor::Service> m_EditorService;
    HANDLE m_ChannelStop{};
    std::thread m_ChannelWorker;
    HANDLE m_DeviceStop{}, m_DeviceWake{};
    std::thread m_DeviceWorker;

    // Drop file support
    CDropTarget m_DropTarget;

    // Controls
    HICON           m_hIcon;
    CTabCtrl        m_TabApplication;
    CBlacklistDlg   m_BlacklistDlg;
    CWhitelistDlg   m_WhitelistDlg;

    HCMNOTIFICATION m_DeviceNotification{};
    NOTIFYICONDATAW m_NotifyIcon{};
    bool m_StartHidden{};
    bool m_EditorPrelaunched{};
    bool m_Exiting{};
    std::atomic_bool m_MaintenancePrepared{};
    bool m_HideNoticeShown{};
    bool m_TrayIconAdded{};
    size_t m_LastTrayProfileCount{ static_cast<size_t>(-1) };
    std::wstring m_LastTrayProfileLabel;
    std::wstring m_LastStatus;

    void AddTrayIcon();
    void RemoveTrayIcon() noexcept;
    void HideToTray();
    void ShowFromTray();
    void UpdateTrayTooltip();
    void ChannelWorkerMain() noexcept;
    void DeviceWorkerMain() noexcept;
    void ReleaseEngine();

    // Events
    afx_msg void OnPaint();
    afx_msg HCURSOR OnQueryDragIcon();
    afx_msg void OnWindowPosChanging(WINDOWPOS* position);
    afx_msg void OnClose();
    afx_msg void OnDestroy();
    afx_msg BOOL OnQueryEndSession();
    afx_msg void OnEndSession(BOOL ending);
    afx_msg void OnSysCommand(UINT id, LPARAM parameter);
    afx_msg LRESULT OnDevicesChanged(WPARAM, LPARAM);
    afx_msg LRESULT OnProfileChanged(WPARAM, LPARAM);
    afx_msg LRESULT OnChannelStateChanged(WPARAM, LPARAM);
    afx_msg LRESULT OnChannelRequest(WPARAM, LPARAM);
    afx_msg LRESULT OnTrayIcon(_In_ WPARAM wParam, _In_ LPARAM lParam);
    afx_msg LRESULT OnHideAfterStart(_In_ WPARAM wParam, _In_ LPARAM lParam);
    afx_msg LRESULT OnShowManager(_In_ WPARAM wParam, _In_ LPARAM lParam);
    afx_msg LRESULT OnTaskbarCreated(_In_ WPARAM wParam, _In_ LPARAM lParam);
};
