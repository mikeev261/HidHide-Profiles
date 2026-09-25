// SPDX-License-Identifier: MIT
#pragma once

#include "FilterDriverProxy.h"
#include "Maintenance.h"
#include "ProfileDomain.h"
#include <functional>

struct ProfilesDriverTransport
{
    std::function<HidHide::Configuration()> read;
    std::function<void(HidHide::DriverConfiguration const&, HidHide::DriverConfiguration const&)> commit;
    bool persistentRecovery{};
    std::function<std::optional<std::vector<std::uint8_t>>()> loadRecovery;
    std::function<void(std::vector<std::uint8_t> const&)> saveRecovery;
    std::function<void()> clearRecovery;
    std::function<bool()> legacyRecoveryPending;
    std::function<void()> clearBlockedRecovery;
    std::wstring admissionName{ HidHide::Maintenance::AdmissionName };
    std::wstring barrierName{ HidHide::Maintenance::BarrierName };
};

class CProfilesEnforcementAdapter final : public HidHide::Profiles::IEnforcement
{
public:
    explicit CProfilesEnforcementAdapter(HidHide::FilterDriverProxy& proxy);
    explicit CProfilesEnforcementAdapter(ProfilesDriverTransport transport);
    HidHide::Profiles::EnforcementResult Observe() override;
    HidHide::Profiles::EnforcementResult Reconcile(HidHide::Profiles::DesiredEnforcement const& desired) override;
    HidHide::Profiles::EnforcementResult RestoreBaseline() override;
    HidHide::Profiles::EnforcementResult AdoptCurrentAsBaseline() override;
    HidHide::Configuration PrepareMaintenance();
    void InstallConfigurationChannel();
    // Handler of the CLI configuration channel (for example --app-reg).
    void CommitExternalRequest(HidHide::Configuration const& expected, HidHide::Configuration const& desired);
private:
    HidHide::Profiles::DesiredEnforcement ToNeutral(HidHide::DriverConfiguration const& state) const;
    HidHide::DriverConfiguration ToDriver(HidHide::Profiles::DesiredEnforcement const& desired) const;
    void Recover();
    void SaveRecovery(HidHide::DriverConfiguration const& before, HidHide::DriverConfiguration const& after);
    void ClearRecovery();
    HidHide::Configuration Read() const;
    void Commit(HidHide::DriverConfiguration const& before, HidHide::DriverConfiguration const& after) const;
    HidHide::Profiles::EnforcementResult Failure(std::wstring message, bool observedKnown = false, bool conflict = false);
    HidHide::Profiles::EnforcementResult FailureAfterAttempt(std::wstring message);

    HidHide::FilterDriverProxy* m_Proxy{};
    ProfilesDriverTransport m_Transport;
    HidHide::DriverConfiguration m_Baseline, m_Expected, m_RecoveryBefore, m_RecoveryAfter; // Before/after: HidHide's last recorded attempt
    bool m_RecoveryPending{}, m_RecoveryBlocked{}, m_Conflict{};
    // Baseline recorded together with m_RecoveryAfter (what HidHide Profiles
    // applied). Set only while that pair is trustworthy evidence.
    std::optional<HidHide::DriverConfiguration> m_RecoveryBaseline;
    std::optional<HidHide::DriverConfiguration> m_ExternalRequest; // last CLI-requested state
};
