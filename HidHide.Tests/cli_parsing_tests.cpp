// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include "CliParsing.h"

using HidHide::CliParsing::ExtractCommand;
using HidHide::CliParsing::ExtractCommands;
using HidHide::CliParsing::ExtractKeyword;
using HidHide::CliParsing::ExtractString;

TEST(CliParsing, ExtractKeyword_basic)
{
    std::wstring s{ L"help rest" };
    EXPECT_EQ(L"help", ExtractKeyword(s));
    EXPECT_EQ(L"rest", s);
}

TEST(CliParsing, ExtractKeyword_entireWhenNoSpace)
{
    std::wstring s{ L"version" };
    EXPECT_EQ(L"version", ExtractKeyword(s));
    EXPECT_TRUE(s.empty());
}

TEST(CliParsing, ExtractString_quoted)
{
    std::wstring s{ LR"("C:\path\a.exe" tail)" };
    EXPECT_EQ(LR"(C:\path\a.exe)", ExtractString(s));
    EXPECT_EQ(L"tail", s);
}

TEST(CliParsing, ExtractString_quotedAtEnd)
{
    std::wstring s{ L"\"only\"" };
    EXPECT_EQ(L"only", ExtractString(s));
    EXPECT_TRUE(s.empty());
}

TEST(CliParsing, ExtractString_unquoted)
{
    std::wstring s{ LR"(C:\path\file.exe more)" };
    EXPECT_EQ(LR"(C:\path\file.exe)", ExtractString(s));
    EXPECT_EQ(L"more", s);
}

TEST(CliParsing, ExtractString_badQuote_returnsEmpty)
{
    std::wstring s{ L"\"unclosed" };
    EXPECT_TRUE(ExtractString(s).empty());
    EXPECT_EQ(L"\"unclosed", s);
}

TEST(CliParsing, ExtractCommand_oneArg)
{
    std::wstring s{ LR"(dev-hide "HID\VID" )" };
    auto const cmd{ ExtractCommand(s) };
    ASSERT_EQ(2u, cmd.size());
    EXPECT_EQ(L"dev-hide", cmd[0]);
    EXPECT_EQ(LR"(HID\VID)", cmd[1]);
}

TEST(CliParsing, ExtractCommands_chained)
{
    std::wstring s{ L"--cloak-on --cloak-off" };
    auto const cmds{ ExtractCommands(s) };
    ASSERT_EQ(2u, cmds.size());
    ASSERT_EQ(1u, cmds[0].size());
    ASSERT_EQ(1u, cmds[1].size());
    EXPECT_EQ(L"cloak-on", cmds[0][0]);
    EXPECT_EQ(L"cloak-off", cmds[1][0]);
}

TEST(CliParsing, ExtractCommands_syntaxError_returnsEmpty)
{
    std::wstring s{ L"dev-hide \"bad" };
    auto const cmds{ ExtractCommands(s) };
    EXPECT_TRUE(cmds.empty());
}

#include "CliDispatch.h"
#include <stdexcept>

namespace
{
    struct BatchFixture
    {
        bool staged{};
        bool persisted{};
        int handlers{};
        HidHide::CliDispatch::RegisteredCommands commands;
        explicit BatchFixture(bool writeThrough = false)
        {
            auto noArguments = [](HidHide::CliParsing::Args const& args)
            { return args.size() == 1 ? std::wstring{} : L"wrong arguments"; };
            commands[L"cloak-on"] = { {}, {}, [this, writeThrough](auto const&)
            { ++handlers; staged = true; if (writeThrough) persisted = staged; }, noArguments };
            commands[L"cloak-toggle"] = { {}, {}, [this, writeThrough](auto const&)
            { ++handlers; staged = !staged; if (writeThrough) persisted = staged; }, noArguments };
            commands[L"fail"] = { {}, {}, [](auto const&)
            { throw std::runtime_error("handler failed"); }, noArguments };
        }
        std::wstring Run(std::wstring line)
        {
            auto batch = ExtractCommands(line);
            if (!line.empty()) return L"syntax error";
            return HidHide::CliDispatch::ExecuteCommands(batch, commands, L"unknown command");
        }
    };
}

TEST(CliDispatch, LaterUnknownRejectsBeforeStagingOrWriteThrough)
{
    for (bool live : { false, true })
    {
        BatchFixture fixture(live);
        EXPECT_EQ(L"unknown command", fixture.Run(L"--cloak-on --missing"));
        EXPECT_EQ(0, fixture.handlers);
        EXPECT_FALSE(fixture.staged);
        EXPECT_FALSE(fixture.persisted);
    }
}

TEST(CliDispatch, LaterInvalidArgumentsRejectBeforeStagingOrWriteThrough)
{
    for (bool live : { false, true })
    {
        BatchFixture fixture(live);
        EXPECT_EQ(L"wrong arguments", fixture.Run(L"--cloak-on --cloak-toggle extra"));
        EXPECT_EQ(0, fixture.handlers);
        EXPECT_FALSE(fixture.staged);
        EXPECT_FALSE(fixture.persisted);
    }
}

TEST(CliDispatch, ValidBatchRetainsOrderAndStagedCommit)
{
    BatchFixture fixture;
    EXPECT_TRUE(fixture.Run(L"--cloak-on --cloak-toggle --cloak-toggle").empty());
    EXPECT_EQ(3, fixture.handlers);
    EXPECT_TRUE(fixture.staged);
    EXPECT_FALSE(fixture.persisted);
    fixture.persisted = fixture.staged; // Interpreter commits only after input completes.
    EXPECT_TRUE(fixture.persisted);
}

TEST(CliDispatch, RejectedInteractiveLinePreservesEarlierValidLine)
{
    BatchFixture fixture;
    ASSERT_TRUE(fixture.Run(L"--cloak-on").empty());
    EXPECT_EQ(L"unknown command", fixture.Run(L"--cloak-toggle --missing"));
    EXPECT_EQ(1, fixture.handlers);
    EXPECT_TRUE(fixture.staged);
    fixture.persisted = fixture.staged;
    EXPECT_TRUE(fixture.persisted);
}

TEST(CliDispatch, RuntimeHandlerFailurePropagatesWithoutAtomicityPromise)
{
    BatchFixture fixture(true);
    try
    {
        fixture.Run(L"--cloak-on --fail --cloak-toggle");
        FAIL();
    }
    catch (std::runtime_error const& error)
    {
        EXPECT_STREQ("handler failed", error.what());
    }
    EXPECT_EQ(1, fixture.handlers);
    EXPECT_TRUE(fixture.persisted);
}
