// SPDX-License-Identifier: MIT
#include "stdafx.h"
#include "ProfilesEnforcementAdapter.h"
#include "ConfigurationChannel.h"
#include "Maintenance.h"
#include "ProfileRecovery.h"
#include "Volume.h"
#include "Utils.h"

namespace
{
    std::optional<std::vector<std::uint8_t>> LoadRecoveryRecord()
    {
        HKEY key{}; auto opened = ::RegOpenKeyExW(HKEY_CURRENT_USER, HidHide::ProfileRecovery::RuntimeKey, 0, KEY_QUERY_VALUE, &key);
        if (opened == ERROR_FILE_NOT_FOUND) return std::nullopt;
        if (opened != ERROR_SUCCESS) throw std::system_error(opened, std::system_category(), "Open driver recovery record");
        DWORD type{}, size{}; auto error = ::RegQueryValueExW(key, HidHide::ProfileRecovery::OwnedValue, nullptr, &type, nullptr, &size);
        if (error == ERROR_FILE_NOT_FOUND) { ::RegCloseKey(key); return std::nullopt; }
        if (error != ERROR_SUCCESS || type != REG_BINARY || size > HidHide::Protocol::MaxBytes)
        { ::RegCloseKey(key); throw std::runtime_error("Driver recovery record is invalid"); }
        std::vector<std::uint8_t> bytes(size); error = ::RegQueryValueExW(key, HidHide::ProfileRecovery::OwnedValue, nullptr, &type, bytes.data(), &size); ::RegCloseKey(key);
        if (error != ERROR_SUCCESS) throw std::system_error(error, std::system_category(), "Read driver recovery record"); bytes.resize(size); return bytes;
    }

    void SaveRecoveryRecord(std::vector<std::uint8_t> const& bytes)
    {
        HKEY key{}; auto error = ::RegCreateKeyExW(HKEY_CURRENT_USER, HidHide::ProfileRecovery::RuntimeKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
        if (error != ERROR_SUCCESS) throw std::system_error(error, std::system_category(), "Create profile recovery key");
        error = ::RegSetValueExW(key, HidHide::ProfileRecovery::OwnedValue, 0, REG_BINARY, bytes.data(), static_cast<DWORD>(bytes.size())); if (error == ERROR_SUCCESS) error = ::RegFlushKey(key); ::RegCloseKey(key);
        if (error != ERROR_SUCCESS) throw std::system_error(error, std::system_category(), "Write profile recovery");
    }

    void ClearRecoveryRecord()
    {
        HKEY key{}; auto error = ::RegOpenKeyExW(HKEY_CURRENT_USER, HidHide::ProfileRecovery::RuntimeKey, 0, KEY_SET_VALUE, &key);
        if (error == ERROR_FILE_NOT_FOUND) return;
        if (error != ERROR_SUCCESS) throw std::system_error(error, std::system_category(), "Open profile recovery key for clear");
        HidHide::ProfileRecovery::ClearOwnedRecovery([&](wchar_t const* value) { error = ::RegDeleteValueW(key, value); });
        ::RegCloseKey(key);
        if (error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND) throw std::system_error(error, std::system_category(), "Clear profile recovery value");
        // Keep the shared key. Deleting it could race with and erase a foreign
        // or legacy value; maintenance admission is value-scoped below.
    }

    bool LegacyRecoveryRecordPending()
    {
        HKEY key{}; auto opened = ::RegOpenKeyExW(HKEY_CURRENT_USER, HidHide::ProfileRecovery::RuntimeKey, 0, KEY_QUERY_VALUE, &key);
        if (opened == ERROR_FILE_NOT_FOUND) return false;
        if (opened != ERROR_SUCCESS) throw std::system_error(opened, std::system_category(), "Open legacy recovery evidence");
        DWORD type{}, size{}; auto error = ::RegQueryValueExW(key, HidHide::ProfileRecovery::LegacyValue, nullptr, &type, nullptr, &size); ::RegCloseKey(key);
        if (error == ERROR_FILE_NOT_FOUND) return false;
        if (error != ERROR_SUCCESS) throw std::system_error(error, std::system_category(), "Inspect legacy recovery evidence");
        return true;
    }

    void ClearBlockedRecoveryRecords()
    {
        HKEY key{}; auto error = ::RegOpenKeyExW(HKEY_CURRENT_USER, HidHide::ProfileRecovery::RuntimeKey, 0, KEY_SET_VALUE, &key);
        if (error == ERROR_FILE_NOT_FOUND) return;
        if (error != ERROR_SUCCESS) throw std::system_error(error, std::system_category(), "Open blocked recovery evidence for clear");
        try
        {
            HidHide::ProfileRecovery::ClearKnownRecoveryAfterExplicitAdoption([&](wchar_t const* value)
            {
                auto erased = ::RegDeleteValueW(key, value);
                if (erased != ERROR_SUCCESS && erased != ERROR_FILE_NOT_FOUND)
                    throw std::system_error(erased, std::system_category(), "Clear known recovery value");
            });
            if (auto flushed = ::RegFlushKey(key); flushed != ERROR_SUCCESS) throw std::system_error(flushed, std::system_category(), "Flush recovery clear");
            ::RegCloseKey(key);
        }
        catch (...) { ::RegCloseKey(key); throw; }
    }

    ProfilesDriverTransport ProductionTransport()
    {
        return ProfilesDriverTransport{
            [] { return HidHide::FilterDriverProxy::ReadDriverOnlyConfiguration(); },
            [](auto const& before, auto const& after) { HidHide::FilterDriverProxy::CommitDriverState(before, after); }, true,
            &LoadRecoveryRecord, &SaveRecoveryRecord, &ClearRecoveryRecord,
            &LegacyRecoveryRecordPending, &ClearBlockedRecoveryRecords };
    }
}

CProfilesEnforcementAdapter::CProfilesEnforcementAdapter(HidHide::FilterDriverProxy& proxy)
    : CProfilesEnforcementAdapter(ProductionTransport())
{
    m_Proxy = &proxy;
}

CProfilesEnforcementAdapter::CProfilesEnforcementAdapter(ProfilesDriverTransport transport) : m_Transport(std::move(transport))
{
    if (m_Transport.admissionName.empty()) m_Transport.admissionName = HidHide::Maintenance::AdmissionName;
    if (m_Transport.barrierName.empty()) m_Transport.barrierName = HidHide::Maintenance::BarrierName;
    if (!m_Transport.read || !m_Transport.commit || (m_Transport.persistentRecovery
        && (!m_Transport.loadRecovery || !m_Transport.saveRecovery || !m_Transport.clearRecovery)))
        throw std::invalid_argument("Enforcement transport or persistent recovery store is incomplete");
    Recover();
}

HidHide::Configuration CProfilesEnforcementAdapter::Read() const { return m_Transport.read(); }
void CProfilesEnforcementAdapter::Commit(HidHide::DriverConfiguration const& before, HidHide::DriverConfiguration const& after) const { m_Transport.commit(before, after); }

HidHide::Profiles::DesiredEnforcement CProfilesEnforcementAdapter::ToNeutral(HidHide::DriverConfiguration const& state) const
{
    HidHide::Profiles::DesiredEnforcement result; result.hidingEnabled = state.active; result.hiddenDevices = state.blacklist;
    for (auto const& image : state.whitelist)
    {
        try
        {
            auto path = HidHide::FullImageNameToFileName(image);
            if (path.empty()) throw std::runtime_error("Driver Allowed-app entry cannot be represented as a local executable path");
            result.allowedApplications.emplace(HidHide::Profiles::NormalizeExecutable(path));
        }
        catch (...) { throw std::runtime_error("Driver Allowed-app state contains an opaque entry; exact observation is unavailable"); }
    }
    return result;
}

HidHide::DriverConfiguration CProfilesEnforcementAdapter::ToDriver(HidHide::Profiles::DesiredEnforcement const& desired) const
{
    auto result = m_Baseline; result.active = desired.hidingEnabled; result.inverse = false; result.blacklist = desired.hiddenDevices; result.whitelist.clear();
    for (auto const& path : desired.allowedApplications)
    {
        auto image = HidHide::FileNameToFullImageName(path); if (image.empty()) throw std::runtime_error("Allowed application is not on a supported local volume"); result.whitelist.emplace(std::move(image));
    }
    for (auto const& image : m_Baseline.whitelist) try { if (HidHide::FullImageNameToFileName(image).empty()) result.whitelist.emplace(image); } catch (...) { result.whitelist.emplace(image); }
    return result;
}

HidHide::Profiles::EnforcementResult CProfilesEnforcementAdapter::Failure(std::wstring message, bool known, bool conflict)
{
    HidHide::Profiles::EnforcementResult result; result.failure = std::move(message); result.observedKnown = known; result.conflict = conflict;
    if (known) try { result.observed = ToNeutral(HidHide::DriverState(Read())); } catch (...) { result.observedKnown = false; }
    return result;
}

HidHide::Profiles::EnforcementResult CProfilesEnforcementAdapter::FailureAfterAttempt(std::wstring message)
{
    HidHide::Profiles::EnforcementResult result; result.failure = std::move(message);
    try
    {
        auto current = HidHide::DriverState(Read()); auto neutral = ToNeutral(current); result.observed = std::move(neutral); result.observedKnown = true;
        if (current != m_Expected) { m_Conflict = true; result.conflict = true; }
        if (m_Conflict || m_RecoveryBlocked) result.conflict = true;
    }
    catch (...) {}
    return result;
}

HidHide::Profiles::EnforcementResult CProfilesEnforcementAdapter::Observe()
{
    if (m_RecoveryBlocked) return Failure(L"Unvalidated driver recovery evidence is preserved; explicit review is required", true, true);
    if (m_Conflict) return Failure(L"Driver recovery or external-state conflict is unresolved", true, true);
    try
    {
        auto current = HidHide::DriverState(Read());
        if (current != m_Expected) { m_Conflict = true; return Failure(L"Driver settings changed outside HidHide Profiles", true, true); }
        return { true, true, ToNeutral(current), {} };
    }
    catch (...) { return Failure(L"Device visibility could not be observed"); }
}

HidHide::Profiles::EnforcementResult CProfilesEnforcementAdapter::Reconcile(HidHide::Profiles::DesiredEnforcement const& desired)
{
    if (m_RecoveryBlocked) return Failure(L"Unvalidated driver recovery evidence is preserved; reconciliation is blocked", true, true);
    if (m_Conflict) return Failure(L"Driver conflict is unresolved", true, true);
    try
    {
        if (HidHide::Maintenance::Active(m_Transport.barrierName.c_str())) return Failure(L"Setup maintenance is active");
    }
    catch (...)
    {
        return Failure(L"Setup maintenance state could not be verified; driver updates are blocked");
    }
    try
    {
        HidHide::Maintenance::Admission admission(m_Transport.admissionName.c_str(), m_Transport.barrierName.c_str()); auto before = HidHide::DriverState(Read());
        if (before != m_Expected) { m_Conflict = true; return Failure(L"Driver settings changed outside HidHide Profiles", true, true); }
        auto requested = ToDriver(desired);
        if (before != requested) { if (requested != m_Baseline || m_RecoveryPending) SaveRecovery(before, requested); Commit(before, requested); }
        auto after = HidHide::DriverState(Read());
        if (after != requested) return Failure(L"Driver read-back did not match requested visibility", true);
        m_Expected = after; if (after == m_Baseline && m_RecoveryPending) ClearRecovery(); return { true, true, ToNeutral(after), {} };
    }
    catch (...) { return FailureAfterAttempt(L"The driver update failed; current device visibility was re-observed where possible"); }
}

HidHide::Profiles::EnforcementResult CProfilesEnforcementAdapter::RestoreBaseline()
{
    if (m_RecoveryBlocked) return Failure(L"Unvalidated driver recovery evidence is preserved; explicit review is required", true, true);
    try
    {
        HidHide::Maintenance::Admission admission(m_Transport.admissionName.c_str(), m_Transport.barrierName.c_str()); auto before = HidHide::DriverState(Read());
        if (before != m_Expected)
        {
            if (m_RecoveryPending && before == m_RecoveryAfter) m_Conflict = false;
            else { m_Conflict = true; return Failure(L"Driver changed; restoration refused", true, true); }
        }
        if (before != m_Baseline) Commit(before, m_Baseline);
        auto after = HidHide::DriverState(Read()); if (after != m_Baseline) return Failure(L"Baseline restoration was not confirmed", true);
        m_Expected = after; if (m_RecoveryPending) ClearRecovery(); return { true, true, ToNeutral(after), {} };
    }
    catch (...) { return FailureAfterAttempt(L"Baseline restoration failed; current device visibility was re-observed where possible"); }
}

HidHide::Profiles::EnforcementResult CProfilesEnforcementAdapter::AdoptCurrentAsBaseline()
{
    try
    {
        HidHide::Maintenance::Admission admission(m_Transport.admissionName.c_str(), m_Transport.barrierName.c_str()); auto adopted = HidHide::DriverState(Read());
        // Translation is part of verification. Never destroy recovery evidence
        // before proving the full observed tuple can be represented faithfully.
        auto neutral = ToNeutral(adopted);
        if (m_RecoveryBlocked && m_Transport.persistentRecovery && m_Transport.clearBlockedRecovery)
            m_Transport.clearBlockedRecovery();
        else if (m_RecoveryPending || m_RecoveryBlocked) ClearRecovery();
        m_RecoveryPending = false; m_RecoveryBlocked = false; m_RecoveryAfter = {};
        m_Baseline = adopted; m_Expected = adopted; m_Conflict = false; m_ExternalRequest.reset(); return { true, true, std::move(neutral), {} };
    }
    catch (...) { if (m_RecoveryBlocked) m_Conflict = true; return Failure(L"Current driver settings could not be adopted", true, m_Conflict || m_RecoveryBlocked); }
}

void CProfilesEnforcementAdapter::Recover()
{
    m_Expected = HidHide::DriverState(Read()); m_Baseline = m_Expected; if (!m_Transport.persistentRecovery) return;
    try
    {
        if (m_Transport.legacyRecoveryPending && m_Transport.legacyRecoveryPending())
            throw std::runtime_error("Legacy driver recovery evidence requires explicit review");
        auto bytes = m_Transport.loadRecovery(); if (!bytes) return; m_RecoveryPending = true;
        auto record = HidHide::ProfileRecovery::Parse(*bytes, HidHide::Channel::CurrentSid()); auto current = m_Expected; m_RecoveryAfter = record.after;
        if (!HidHide::ProfileRecovery::MatchesRecordedDriverState(current, record.baseline, record.before, record.after)) throw std::runtime_error("Driver recovery record does not match observed state");
        HidHide::Maintenance::Admission admission(m_Transport.admissionName.c_str(), m_Transport.barrierName.c_str()); if (current != record.baseline) Commit(current, record.baseline);
        auto confirmed = HidHide::DriverState(Read()); if (confirmed != record.baseline) throw std::runtime_error("Recovered baseline was not confirmed");
        m_Baseline = confirmed; m_Expected = confirmed; ClearRecovery();
    }
    catch (...) { m_RecoveryBlocked = true; m_Conflict = true; }
}

void CProfilesEnforcementAdapter::SaveRecovery(HidHide::DriverConfiguration const& before, HidHide::DriverConfiguration const& after)
{
    m_RecoveryAfter = after;
    if (!m_Transport.persistentRecovery) { m_RecoveryPending = true; return; }
    auto bytes = HidHide::ProfileRecovery::SerializeV2(HidHide::Channel::CurrentSid(), { m_Baseline, before, after });
    try { m_Transport.saveRecovery(bytes); m_RecoveryPending = true; }
    catch (...) { m_RecoveryBlocked = true; m_Conflict = true; throw; }
}

void CProfilesEnforcementAdapter::ClearRecovery() { if (m_Transport.persistentRecovery) m_Transport.clearRecovery(); m_RecoveryPending = false; m_RecoveryBlocked = false; m_RecoveryAfter = {}; }

HidHide::Configuration CProfilesEnforcementAdapter::PrepareMaintenance()
{
    if (m_RecoveryBlocked) throw std::runtime_error("Unvalidated driver recovery evidence is preserved; setup maintenance is refused");
    if (!HidHide::Maintenance::Active(m_Transport.barrierName.c_str())) throw std::runtime_error("A maintenance barrier is required"); auto configuration = Read(); auto observed = HidHide::DriverState(configuration);
    if (observed != m_Expected) throw std::runtime_error("Driver settings changed; maintenance restoration was refused"); if (observed != m_Baseline) Commit(observed, m_Baseline);
    configuration = Read(); if (HidHide::DriverState(configuration) != m_Baseline) throw std::runtime_error("Baseline restoration was not confirmed");
    // Legacy catalog access is isolated to this explicit protected maintenance
    // snapshot so unrelated historical data can be preserved by setup.
    configuration.profiles = HidHide::FilterDriverProxy::ReadLegacyProfileCatalogForMaintenance();
    m_Expected = m_Baseline; if (m_RecoveryPending) ClearRecovery(); return configuration;
}

void CProfilesEnforcementAdapter::InstallConfigurationChannel()
{
    if (!m_Proxy) throw std::logic_error("Configuration channel is unavailable for a deterministic enforcement transport");
    m_Proxy->SetCoordinator([this] { auto current = Read(); current.profiles.clear(); return current; },
        [this](auto const& expected, auto const& desired, bool) { CommitExternalRequest(expected, desired); });
}

void CProfilesEnforcementAdapter::CommitExternalRequest(HidHide::Configuration const& expected, HidHide::Configuration const& desired)
{
    // A conflict raised only by earlier CLI requests does not refuse another
    // one (feeder installers call --app-reg repeatedly), provided the driver
    // still holds exactly what the previous request left.
    if (m_RecoveryBlocked || (m_Conflict && !m_ExternalRequest)) throw std::runtime_error("Driver recovery or external-state conflict is unresolved");
    if (expected.profiles != desired.profiles) throw std::invalid_argument("Legacy profile mutation commands are not supported; use HidHide Profiles");
    HidHide::Maintenance::Admission admission(m_Transport.admissionName.c_str(), m_Transport.barrierName.c_str()); auto live = HidHide::DriverState(Read());
    if (live != HidHide::DriverState(expected) || live != (m_ExternalRequest ? *m_ExternalRequest : m_Expected)) throw std::runtime_error("Driver state changed before CLI commit");
    Commit(live, HidHide::DriverState(desired));
    // This explicit request is external to the authoritative JSON policy.
    // Keep the prior expected tuple so the next reconciliation reports a
    // conflict instead of silently erasing the CLI-requested state.
    m_ExternalRequest = HidHide::DriverState(desired); m_Conflict = true;
}
