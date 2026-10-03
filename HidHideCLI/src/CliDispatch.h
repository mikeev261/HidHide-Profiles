// SPDX-License-Identifier: MIT
#pragma once
#include "CliParsing.h"
#include <functional>
#include <map>

namespace HidHide::CliDispatch
{
    struct RegisteredCommandInfo
    {
        std::wstring syntax;
        std::wstring description;
        std::function<void(CliParsing::Args const&)> execute;
        std::function<std::wstring(CliParsing::Args const&)> validate;
    };
    using RegisteredCommands = std::map<std::wstring, RegisteredCommandInfo>;

    // Validation errors reject the entire line before any handler runs.
    // Handler exceptions propagate; execution/driver writes are not transactional.
    inline std::wstring ExecuteCommands(std::vector<CliParsing::Args> const& commands,
        RegisteredCommands const& registered, std::wstring const& unrecognized)
    {
        for (auto const& command : commands)
        {
            auto const info = registered.find(command.at(0));
            if (info == registered.end()) return unrecognized;
            if (auto const error = info->second.validate(command); !error.empty()) return error;

        }
        for (auto const& command : commands)
            registered.at(command.at(0)).execute(command);
        return {};
    }
}
