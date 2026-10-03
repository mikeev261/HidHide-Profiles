// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>

namespace HidHide
{
    struct StartupEntryPlan
    {
        bool setCurrent{};
        bool deleteCurrent{};
        bool deleteLegacy{};
    };

    inline bool OwnedStartupCommand(std::optional<std::wstring> const& value,
        std::wstring const& currentCommand, std::wstring const& legacyCommand)
    {
        // Compare the entire bounded registry value: a C-string comparison alone
        // would accept an owned prefix followed by an embedded NUL and foreign data.
        auto matches = [&](std::wstring const& command) {
            return value && value->find(L'\0') == std::wstring::npos
                && value->size() == command.size()
                && 0 == ::_wcsicmp(value->c_str(), command.c_str());
        };
        return matches(currentCommand) || matches(legacyCommand);
    }

    inline StartupEntryPlan PlanStartupEntry(bool enabled,
        std::optional<std::wstring> const& currentValue, std::optional<std::wstring> const& legacyValue,
        std::wstring const& currentCommand, std::wstring const& legacyCommand)
    {
        bool const currentOwned = OwnedStartupCommand(currentValue, currentCommand, legacyCommand);
        bool const legacyOwned = OwnedStartupCommand(legacyValue, currentCommand, legacyCommand);
        if (!enabled) return { false, currentOwned, legacyOwned };
        // A foreign value using the new product name is not ours to overwrite.
        // Keep the old owned value too: deleting it would silently disable an
        // existing startup preference while the new name is unavailable.
        if (currentValue && !currentOwned) return {};
        return { !currentValue || *currentValue != currentCommand, false, legacyOwned };
    }
}
