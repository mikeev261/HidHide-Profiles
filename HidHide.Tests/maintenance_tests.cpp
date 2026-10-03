// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "Maintenance.h"
#include "MaintenanceSession.h"
#include "DriverRegistrySnapshot.h"
#include <future>
#include <thread>
#pragma comment(lib, "Advapi32.lib")
using namespace HidHide;
TEST(Maintenance, UninstallRemovesOnlyExactOwnedStartupCommands) {
    auto path = L"Software\\HidHide.Tests\\StartupCleanup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HKEY key{};
    ASSERT_EQ(ERROR_SUCCESS, RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr));
    struct Cleanup { HKEY key; std::wstring path; ~Cleanup() { RegCloseKey(key); RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str()); } } cleanup{key,path};
    std::wstring current = L"\"C:\\Program Files\\HidHide\\HidHideClient.exe\" --background";
    std::wstring legacy = L"\"C:\\Program Files\\HidHide App Profiles\\HidHideClient.exe\" --background";
    auto write = [&](PCWSTR name, std::wstring const& value) {
        ASSERT_EQ(ERROR_SUCCESS, RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<BYTE const*>(value.c_str()), static_cast<DWORD>((value.size()+1)*sizeof(wchar_t))));
    };
    auto exists = [&](PCWSTR name) { return RegQueryValueExW(key, name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS; };
    write(L"HidHide Profiles", current);
    write(L"HidHide App Profiles", L"\"C:\\Other\\Utility.exe\"");
    write(L"Other App", current);
    RemoveOwnedStartupValues(key, current, legacy);
    EXPECT_FALSE(exists(L"HidHide Profiles"));
    EXPECT_TRUE(exists(L"HidHide App Profiles"));
    EXPECT_TRUE(exists(L"Other App"));
    write(L"HidHide Profiles", current + L" --foreign-argument");
    write(L"HidHide App Profiles", legacy);
    RemoveOwnedStartupValues(key, current, legacy);
    EXPECT_TRUE(exists(L"HidHide Profiles"));
    EXPECT_FALSE(exists(L"HidHide App Profiles"));
    EXPECT_TRUE(exists(L"Other App"));
}
TEST(Maintenance, StartupCleanupPreservesOversizedAndEmbeddedNullValues) {
    auto path = L"Software\\HidHide.Tests\\StartupBounds-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    HKEY key{};
    ASSERT_EQ(ERROR_SUCCESS, RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr));
    struct Cleanup { HKEY key; std::wstring path; ~Cleanup() { RegCloseKey(key); RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str()); } } cleanup{key,path};
    std::wstring owned = L"\"C:\\Program Files\\HidHide\\HidHideClient.exe\" --background";
    auto write = [&](std::wstring const& value) {
        ASSERT_EQ(ERROR_SUCCESS, RegSetValueExW(key, L"HidHide Profiles", 0, REG_SZ, reinterpret_cast<BYTE const*>(value.c_str()), static_cast<DWORD>((value.size()+1)*sizeof(wchar_t))));
    };
    for (auto const& value : { std::wstring(32768, L'x'), owned + std::wstring(1, L'\0') + L"foreign" }) {
        write(value);
        RemoveOwnedStartupValues(key, owned, L"legacy");
        DWORD bytes{};
        EXPECT_EQ(ERROR_SUCCESS, RegQueryValueExW(key, L"HidHide Profiles", nullptr, nullptr, nullptr, &bytes));
        EXPECT_EQ((value.size()+1)*sizeof(wchar_t), bytes);
        std::vector<wchar_t> preserved(value.size()+1);
        ASSERT_EQ(ERROR_SUCCESS, RegQueryValueExW(key, L"HidHide Profiles", nullptr, nullptr,
            reinterpret_cast<BYTE*>(preserved.data()), &bytes));
        EXPECT_EQ(value, std::wstring(preserved.data(), value.size()));
    }
    write(owned);
    RemoveOwnedStartupValues(key, owned, L"legacy");
    EXPECT_EQ(ERROR_FILE_NOT_FOUND, RegQueryValueExW(key, L"HidHide Profiles", nullptr, nullptr, nullptr, nullptr));
}
TEST(Maintenance, MsiSessionUsesOnlyStrictTransactionIdentities) {
    auto const valid = std::wstring(L"01234567-89ab-cdef-0123-456789abcdef");
    EXPECT_TRUE(ValidMsiTransactionId(valid));
    EXPECT_EQ(L"Global\\HidHide.Profiles.Msi.Ready." + valid, MsiEventName(L"Ready", valid));
    for (auto const& value : { L"", L"{01234567-89ab-cdef-0123-456789abcdef}",
        L"01234567-89ab-cdef-0123-456789abcdeg", L"01234567-89ab-cdef-0123-456789abcdef\\Other" })
    {
        EXPECT_FALSE(ValidMsiTransactionId(value));
        EXPECT_THROW(MsiEventName(L"Ready", value), std::runtime_error);
    }
}
TEST(Maintenance, StoredBaselineListsRejectMalformedRegistryData) {
    using Maintenance::DecodeStoredDriverList;
    EXPECT_TRUE(DecodeStoredDriverList({L'\0'}).empty());
    EXPECT_TRUE(DecodeStoredDriverList({L'\0', L'\0'}).empty());
    EXPECT_EQ(DecodeStoredDriverList({L'B', L'\0', L'A', L'\0', L'\0'}), (std::set<std::wstring>{L"A", L"B"}));
    EXPECT_THROW(DecodeStoredDriverList({}), std::runtime_error);
    EXPECT_THROW(DecodeStoredDriverList({L'A', L'\0'}), std::runtime_error);
    EXPECT_THROW(DecodeStoredDriverList({L'A', L'\0', L'A', L'\0', L'\0'}), std::runtime_error);
    EXPECT_THROW(DecodeStoredDriverList({L'\0', L'A', L'\0', L'\0'}), std::runtime_error);
}
namespace {
struct Names {
    std::wstring root = L"Local\\HidHide.Maintenance.Tests." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    std::wstring admission = root + L".Admission", barrier = root + L".Barrier", owner = root + L".Owner";
};
Protocol::Writer Request(Protocol::Command command) {
    Protocol::Writer w; w.Number(Protocol::Version); w.Number(static_cast<uint32_t>(command)); return w;
}
}
TEST(Maintenance, BarrierExcludesWritersAndSurvivesCreatorHandoff) {
    Names n;
    Channel::Handle retained(nullptr, &CloseHandle);
    {
        Maintenance::Barrier barrier(n.admission.c_str(), n.barrier.c_str());
        EXPECT_TRUE(Maintenance::Active(n.barrier.c_str()));
        EXPECT_THROW(Maintenance::Admission(n.admission.c_str(), n.barrier.c_str()), std::runtime_error);
        EXPECT_THROW(Maintenance::Barrier(n.admission.c_str(), n.barrier.c_str()), std::runtime_error);
        retained = Maintenance::Barrier::Join(n.barrier.c_str());
    }
    EXPECT_TRUE(Maintenance::Active(n.barrier.c_str()));
    retained.reset();
    EXPECT_FALSE(Maintenance::Active(n.barrier.c_str()));
    EXPECT_NO_THROW(Maintenance::Admission(n.admission.c_str(), n.barrier.c_str()));
    EXPECT_THROW(Maintenance::Barrier::Join(n.barrier.c_str()), std::runtime_error);
}
TEST(Maintenance, AdmittedWritePreventsBarrierCreation) {
    Names n;
    Maintenance::Admission admitted(n.admission.c_str(), n.barrier.c_str());
    auto result = std::async(std::launch::async, [&] {
        try { Maintenance::Barrier barrier(n.admission.c_str(), n.barrier.c_str()); return false; }
        catch (std::runtime_error const&) { return true; }
    });
    EXPECT_TRUE(result.get());
}
TEST(Maintenance, RecoveryFailureReleasesBarrierWithoutReadingSnapshot) {
    Names n; bool read = false;
    EXPECT_THROW(Maintenance::Session([] { return Configuration{}; }, [] { throw std::runtime_error("recovery"); },
        [&] { read = true; return Maintenance::Snapshot{}; }, n.admission.c_str(), n.barrier.c_str(), n.owner.c_str()), std::runtime_error);
    EXPECT_FALSE(read);
    EXPECT_FALSE(Maintenance::Active(n.barrier.c_str()));
}
TEST(Maintenance, HandoffReleasesOwnerButRetainsBarrier) {
    Names n; bool prepared = false;
    Maintenance::Session session([&] { prepared = true; return Configuration{}; }, [] {},
        [] { return Maintenance::Snapshot{}; }, n.admission.c_str(), n.barrier.c_str(), n.owner.c_str());
    EXPECT_FALSE(prepared);
    auto acquire = [&] { return std::async(std::launch::async, [&] { Channel::Lease lease(n.owner.c_str()); return lease.Acquired(); }).get(); };
    EXPECT_FALSE(acquire());
    session.Handoff();
    EXPECT_TRUE(acquire());
    EXPECT_TRUE(Maintenance::Active(n.barrier.c_str()));
    EXPECT_THROW(session.Handoff(), std::runtime_error);
}
TEST(Maintenance, OwnerMustReleaseAfterPreparation) {
    Names n; std::promise<void> ready, release; auto released = release.get_future();
    std::thread manager([&] { Channel::Lease owner(n.owner.c_str()); ready.set_value(); released.wait(); });
    ready.get_future().wait();
    bool read = false;
    EXPECT_THROW(Maintenance::Session([] { return Configuration{}; }, [] {},
        [&] { read = true; return Maintenance::Snapshot{}; }, n.admission.c_str(), n.barrier.c_str(), n.owner.c_str(), 20), std::runtime_error);
    EXPECT_FALSE(read);
    EXPECT_FALSE(Maintenance::Active(n.barrier.c_str()));
    release.set_value(); manager.join();
}
TEST(Maintenance, ChangedBaselineRefusesHandoff) {
    Names n; std::promise<void> ready, release; auto released = release.get_future();
    std::thread manager([&] { Channel::Lease owner(n.owner.c_str()); ready.set_value(); released.wait(); });
    ready.get_future().wait();
    EXPECT_THROW(Maintenance::Session([&] { release.set_value(); return Configuration{}; }, [] {},
        [] { Maintenance::Snapshot s; s.driverPresent = true; s.configuration.active = true; return s; },
        n.admission.c_str(), n.barrier.c_str(), n.owner.c_str()), std::runtime_error);
    manager.join();
    EXPECT_FALSE(Maintenance::Active(n.barrier.c_str()));
}
TEST(Maintenance, ConfirmedBaselineAllowsHandoff) {
    Names n; std::promise<void> ready, release; auto released = release.get_future();
    std::thread manager([&] { Channel::Lease owner(n.owner.c_str()); ready.set_value(); released.wait(); });
    ready.get_future().wait();
    EXPECT_NO_THROW(Maintenance::Session([&] { release.set_value(); return Configuration{}; }, [] {},
        [] { Maintenance::Snapshot s; s.driverPresent = true; return s; },
        n.admission.c_str(), n.barrier.c_str(), n.owner.c_str()));
    manager.join();
}
TEST(MaintenanceProtocol, MalformedRequestsNeverPrepare) {
    auto valid = Request(Protocol::Command::PrepareMaintenance).data;
    int calls = 0;
    auto dispatch = [&](auto const& bytes) { return Protocol::Dispatch(bytes, {}, {}, [&] { ++calls; return Configuration{}; }); };
    for (size_t i = 0; i < valid.size(); ++i) {
        std::vector<uint8_t> truncated(valid.begin(), valid.begin() + i);
        EXPECT_THROW(Protocol::ReadReply(dispatch(truncated)), std::runtime_error);
    }
    auto trailing = valid; trailing.push_back(0);
    EXPECT_THROW(Protocol::ReadReply(dispatch(trailing)), std::runtime_error);
    auto wrongVersion = valid; wrongVersion[0] = 2;
    EXPECT_THROW(Protocol::ReadReply(dispatch(wrongVersion)), std::runtime_error);
    auto unknown = valid; unknown[4] = 99;
    EXPECT_THROW(Protocol::ReadReply(dispatch(unknown)), std::runtime_error);
    EXPECT_EQ(0, calls);
    EXPECT_NO_THROW(Protocol::ReadReply(dispatch(valid)));
    EXPECT_EQ(1, calls);
}
TEST(MaintenanceProtocol, RefusalAndUnsupportedOwnerAreErrors) {
    auto request = Request(Protocol::Command::PrepareMaintenance);
    EXPECT_THROW(Protocol::ReadReply(Protocol::Dispatch(request.data, {}, {}, {})), std::runtime_error);
    EXPECT_THROW(Protocol::ReadReply(Protocol::Dispatch(request.data, {}, {}, []() -> Configuration { throw std::runtime_error("conflict"); })), std::runtime_error);
}
TEST(MaintenanceProtocol, ExistingReadAndCommitRoundTrip) {
    Configuration state; state.blacklist.insert(L"TEST\\BASELINE");
    auto read = Request(Protocol::Command::Read);
    EXPECT_TRUE(state == Protocol::ReadReply(Protocol::Dispatch(read.data, [&] { return state; }, {}, {})));
    auto commit = Request(Protocol::Command::Commit); Configuration desired = state; desired.active = true;
    commit.State(state); commit.State(desired); commit.Number(0); bool called = false;
    auto reply = Protocol::Dispatch(commit.data, {}, [&](auto const& before, auto const& after, bool disable) {
        EXPECT_TRUE(before == state); EXPECT_TRUE(after == desired); EXPECT_FALSE(disable); called = true;
    }, {});
    EXPECT_TRUE(called); EXPECT_TRUE(desired == Protocol::ReadReply(reply));
}
TEST(MaintenanceProtocol, ControlAcceptsOnlyFixedVerbs) {
    EXPECT_EQ(Maintenance::Control::Handoff, Maintenance::ParseControl("handoff"));
    EXPECT_EQ(Maintenance::Control::Release, Maintenance::ParseControl("release"));
    for (auto text : {"", "HANDOFF", "handoff x", "release\n", "cmd.exe", "C:\\file"})
        EXPECT_THROW(Maintenance::ParseControl(text), std::runtime_error);
    Protocol::Writer bad; bad.Number(Protocol::Version); bad.Number(2);
    EXPECT_THROW(Protocol::ReadReply(bad.data), std::runtime_error);
}
TEST(Maintenance, StalledControllerOutputIsCancelled) {
    HANDLE read{}, write{};
    ASSERT_TRUE(CreatePipe(&read, &write, nullptr, 1024));
    auto reader = Channel::Own(read); auto writer = Channel::Own(write);
    auto started = GetTickCount64();
    EXPECT_THROW(Maintenance::WriteControllerLine(writer.get(), std::string(128 * 1024, 'x'), 20), std::runtime_error);
    EXPECT_LT(GetTickCount64() - started, 2000ULL);
}
TEST(Maintenance, ControllerOutputAndDisconnectedPipe) {
    HANDLE read{}, write{};
    ASSERT_TRUE(CreatePipe(&read, &write, nullptr, 1024));
    auto reader = Channel::Own(read); auto writer = Channel::Own(write);
    EXPECT_NO_THROW(Maintenance::WriteControllerLine(writer.get(), "HANDED_OFF"));
    char buffer[32]{}; DWORD count{};
    ASSERT_TRUE(ReadFile(reader.get(), buffer, sizeof(buffer), &count, nullptr));
    EXPECT_EQ(std::string("HANDED_OFF\n"), std::string(buffer, count));
    reader.reset();
    EXPECT_THROW(Maintenance::WriteControllerLine(writer.get(), "READY"), std::runtime_error);
}
TEST(Maintenance, OwnerRefusalNeverFallsBackToDirectSnapshot) {
    Names n; std::promise<void> ready, release; auto released = release.get_future();
    std::thread manager([&] { Channel::Lease owner(n.owner.c_str()); ready.set_value(); released.wait(); });
    ready.get_future().wait();
    bool read = false;
    EXPECT_THROW(Maintenance::Session([]() -> Configuration { throw std::runtime_error("conflicting recovery"); }, [] {},
        [&] { read = true; return Maintenance::Snapshot{}; }, n.admission.c_str(), n.barrier.c_str(), n.owner.c_str()), std::runtime_error);
    EXPECT_FALSE(read); EXPECT_FALSE(Maintenance::Active(n.barrier.c_str()));
    release.set_value(); manager.join();
}
TEST(Maintenance, IncompleteDurableMarkerBlocksUntilRemoved) {
    auto path = L"Software\\HidHide.Maintenance.Tests." + std::to_wstring(GetCurrentProcessId());
    EXPECT_FALSE(Maintenance::DurableMarkerExists(HKEY_CURRENT_USER, path.c_str()));
    HKEY key{}; DWORD disposition{};
    ASSERT_EQ(ERROR_SUCCESS, RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, &disposition));
    EXPECT_EQ(REG_CREATED_NEW_KEY, disposition);
    EXPECT_TRUE(Maintenance::DurableMarkerExists(HKEY_CURRENT_USER, path.c_str()));
    RegCloseKey(key);
    EXPECT_EQ(ERROR_SUCCESS, RegDeleteKeyExW(HKEY_CURRENT_USER, path.c_str(), KEY_WOW64_64KEY, 0));
    EXPECT_FALSE(Maintenance::DurableMarkerExists(HKEY_CURRENT_USER, path.c_str()));
}
TEST(Maintenance, DurableMarkerReadErrorsFailClosed) {
    EXPECT_THROW(Maintenance::DurableMarkerExists(reinterpret_cast<HKEY>(static_cast<ULONG_PTR>(1)), L"InvalidHandle"), std::runtime_error);
}
