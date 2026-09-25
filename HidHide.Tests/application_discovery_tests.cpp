// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../HidHideClient/src/ApplicationDiscovery.h"
using namespace HidHide::Applications;

TEST(ApplicationDiscovery, MetadataPriorityAndGenericFallback)
{
    EXPECT_EQ(L"Assetto Corsa", SuggestName(L"C:\\Games\\acs.exe", {L"Assetto Corsa", L"AC simulator"}));
    EXPECT_EQ(L"\u65e5\u672c\u8a9e Racing", SuggestName(L"C:\\Games\\game.exe", {L"Unity Player", L"\u65e5\u672c\u8a9e Racing"}));
    EXPECT_EQ(L"Racing Legends", SuggestName(L"C:\\Games\\Racing Legends\\Binaries\\Win64\\game.exe", {L"Unreal Engine", L"Game"}));
    EXPECT_EQ(L"My Simulator", SuggestName(L"C:\\Games\\My_Simulator-Win64-Shipping.exe", {}));
    EXPECT_EQ(L"foo", SuggestName(L"C:\\Games\\bin\\foo.exe", {}));
    EXPECT_EQ(L"foo", SuggestName(L"C:\\Program Files\\bin\\foo.exe", {}));
    EXPECT_EQ(L"foo", SuggestName(L"C:\\Games\\Arbitrary Parent\\foo.exe", {}));
    EXPECT_EQ(L"foo", SuggestName(L"C:\\Games\\Title\\Engine\\Binaries\\Win64\\foo.exe", {}));
    EXPECT_EQ(L"Games", SuggestName(L"C:\\foo.exe", {L"Games", {}}));
    EXPECT_EQ(L"foo", SuggestName(L"C:\\foo.exe", {L"TODO: <Product name>", L"<File description>"}));
}
TEST(ApplicationDiscovery, ExactDefaultProductPlaceholderUsesMeaningfulFallback)
{
    for (auto placeholder : { L"DefaultProduct", L"Default Product", L"  DEFAULTPRODUCT  ", L"Default_Product" })
    {
        EXPECT_EQ(L"Real Racing Title", SuggestName(L"C:\\Games\\game.exe", {placeholder, L"Real Racing Title"}));
        EXPECT_EQ(L"Real Racer", SuggestName(L"C:\\Games\\Real_Racer.exe", {placeholder, L"Game"}));
        EXPECT_EQ(L"Real Racer", SuggestName(L"C:\\Games\\Real_Racer.exe", {L"Unity Player", placeholder}));
    }
    EXPECT_EQ(L"DefaultProduct Racing", SuggestName(L"C:\\Games\\game.exe", {L"DefaultProduct Racing", L"Game"}));
    EXPECT_EQ(L"Default Product Racing", SuggestName(L"C:\\Games\\game.exe", {L"Default Product Racing", L"Game"}));
    EXPECT_EQ(L"Bravely Default", SuggestName(L"C:\\Games\\game.exe", {L"Bravely Default", L"Game"}));
}
TEST(ApplicationDiscovery, BoundedCleanUnicode)
{
    EXPECT_EQ(L"A B", Clean(L"  A_\r\nB  "));
    auto longName = std::wstring(255, L'A') + L"\xd83c\xdfce";
    EXPECT_EQ(std::wstring(255, L'A'), Clean(longName));
    EXPECT_EQ(L"AB", Clean(L"A\xd83c" L"B"));
    EXPECT_LE(Clean(std::wstring(255, L'A') + L" B").size(), 256u);
}
TEST(ApplicationDiscovery, GenericWindowsBrandingUsesApplicationDescription)
{
    EXPECT_EQ(L"Notepad", SuggestName(L"C:\\Windows\\notepad.exe", {L"Microsoft\u00ae Windows\u00ae Operating System", L"Notepad"}));
    EXPECT_EQ(L"Windows Command Processor", SuggestName(L"C:\\Windows\\System32\\cmd.exe", {L"Microsoft Windows Operating System", L"Windows Command Processor"}));
    EXPECT_EQ(L"Windows Explorer", SuggestName(L"C:\\Windows\\explorer.exe", {L"Microsoft\u2122 Windows\u00ae", L"Windows Explorer"}));
    EXPECT_EQ(L"\u30e1\u30e2\u5e33", SuggestName(L"C:\\Windows\\notepad.exe", {L"Microsoft\u00ae Windows\u00ae Operating System", L"\u30e1\u30e2\u5e33"}));
    EXPECT_EQ(L"Microsoft Flight Simulator", SuggestName(L"C:\\Games\\game.exe", {L"Microsoft Flight Simulator", L"Simulator executable"}));
    EXPECT_EQ(L"Microsoft Visual Studio", SuggestName(L"C:\\Apps\\devenv.exe", {L"Microsoft Visual Studio", L"Development environment"}));
    EXPECT_EQ(L"Windows Terminal", SuggestName(L"C:\\Apps\\terminal.exe", {L"Windows Terminal", L"Terminal executable"}));
}
TEST(ApplicationDiscovery, TodoApplicationNamesAreNotPlaceholders)
{
    EXPECT_EQ(L"Todoist", SuggestName(L"C:\\Apps\\app.exe", {L"Todoist", L"Application"}));
    EXPECT_EQ(L"Todo Racing", SuggestName(L"C:\\Games\\game.exe", {L"Todo Racing", L"Game"}));
    EXPECT_EQ(L"Useful description", SuggestName(L"C:\\Apps\\app.exe", {L"TODO", L"Useful description"}));
    EXPECT_EQ(L"Useful description", SuggestName(L"C:\\Apps\\app.exe", {L"TODO: product name", L"Useful description"}));
}
TEST(ApplicationDiscovery, UntitledVisibleNativeWindowCountsAsApplicationWindow)
{
    struct Window { HWND value{}; ~Window() { if (value) ::DestroyWindow(value); } };
    // Off-screen, non-activating and owned by this test; no existing windows are changed.
    Window window{::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP,
        -32000, -32000, 1, 1, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr)};
    ASSERT_NE(nullptr, window.value);
    ASSERT_EQ(0, ::GetWindowTextLengthW(window.value));
    std::set<DWORD> processes;
    AddVisibleWindowProcess(window.value, processes);
    EXPECT_TRUE(processes.empty());
    ::ShowWindow(window.value, SW_SHOWNOACTIVATE);
    ASSERT_TRUE(::IsWindowVisible(window.value));
    AddVisibleWindowProcess(window.value, processes);
    EXPECT_EQ(1u, processes.count(::GetCurrentProcessId()));
    ::ShowWindow(window.value, SW_HIDE);
    processes.clear();
    AddVisibleWindowProcess(window.value, processes);
    EXPECT_TRUE(processes.empty());
}
TEST(ApplicationDiscovery, GroupsCaseVariantsKeepsDistinctDirectoriesAndSortsWindowsFirst)
{
    unsigned reads{};
    auto rows = Group({{1,10,L"C:\\Games\\game.exe",false},{2,20,L"c:\\GAMES\\GAME.exe",true},{3,30,L"D:\\Tools\\game.exe",false},{4,40,L"",true},{5,0,L"C:\\missing.exe",true}},
        [&](auto const&) { ++reads; return Metadata{}; });
    ASSERT_EQ(2u, rows.size()); EXPECT_EQ(2u, reads);
    EXPECT_TRUE(rows[0].process.visible); EXPECT_EQ(2u, rows[0].instances);
    EXPECT_EQ(L"C:\\Games\\game.exe", rows[0].process.path);
    EXPECT_EQ(L"D:\\Tools\\game.exe", rows[1].process.path);
}
TEST(ApplicationDiscovery, SelectionBindsPidLifetimeAndPath)
{
    Process selected{17, 123, L"C:\\Games\\game.exe", true};
    EXPECT_TRUE(SameProcess(selected, [&](auto pid) -> std::optional<Process> { EXPECT_EQ(17u,pid); return Process{17,123,L"c:\\games\\GAME.exe",false}; }));
    EXPECT_FALSE(SameProcess(selected, [](auto) -> std::optional<Process> { return {}; })); // access denied / exited
    EXPECT_FALSE(SameProcess(selected, [](auto) -> std::optional<Process> { return Process{17,124,L"C:\\Games\\game.exe",true}; })); // reused PID
    EXPECT_FALSE(SameProcess(selected, [](auto) -> std::optional<Process> { return Process{17,123,L"D:\\Games\\game.exe",true}; }));
    EXPECT_FALSE(SameProcess(selected, [](auto) -> std::optional<Process> { return Process{17,123,L"",true}; }));
}
TEST(ApplicationDiscovery, EnumerationAndMetadataWorkAreBounded)
{
    std::vector<Process> processes;
    for (unsigned i=1;i<=600;++i) processes.push_back({i,i,L"C:\\Apps\\"+std::to_wstring(i)+L".exe",false});
    unsigned reads{}; auto result=Group(processes,[&](auto const&){++reads;return Metadata{};});
    EXPECT_EQ(512u,result.size()); EXPECT_EQ(512u,reads);
}
TEST(ApplicationDiscovery, UnicodePathsUseWindowsOrdinalCaseComparison)
{
    // Explicit code points keep the witness independent of MSVC's source code page.
    auto rows = Group({{1,1,L"C:\\\u00c9preuve\\racer.exe",false},{2,2,L"c:\\\u00e9preuve\\RACER.EXE",true}}, [](auto const&){return Metadata{};});
    ASSERT_EQ(1u,rows.size()); EXPECT_EQ(2u,rows[0].instances);
    EXPECT_TRUE(SameProcess(Process{1,1,L"C:\\\u00c9preuve\\racer.exe",false},[](auto)->std::optional<Process>{return Process{1,1,L"c:\\\u00e9preuve\\RACER.EXE",true};}));
}

TEST(ApplicationDiscovery, CreationTimeOutsideUint64IsRejectedWithoutThrowing)
{
    // #32: stoull threw a raw out_of_range for 20-digit values above ULLONG_MAX.
    std::optional<std::uint64_t> parsed{ 1 };
    EXPECT_NO_THROW(parsed = ParseCreationTime(L"99999999999999999999")); EXPECT_FALSE(parsed);
    EXPECT_EQ(18446744073709551615ull, ParseCreationTime(L"18446744073709551615").value_or(0));
    EXPECT_FALSE(ParseCreationTime(L"18446744073709551616")); EXPECT_FALSE(ParseCreationTime(L"")); EXPECT_FALSE(ParseCreationTime(L"-1"));
    EXPECT_EQ(133000000000000000ull, ParseCreationTime(L"133000000000000000").value_or(0));
}
