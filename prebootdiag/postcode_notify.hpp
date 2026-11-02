/*
// Copyright (c) 2026 NVIDIA Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/
#pragma once

#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace prebootdiag
{

// Post codes the Vera handlers hand to
// com.nvidia.PreBootDiag.PostCodeCallback@<name>.service.
inline constexpr std::string_view postCodeNames[] = {
    "PSC_FMC_PC_BOOT_MODE_PREBOOT_DIAG",
    "MB2_PC_CCPLEX_PREBOOT_DIAG_ENTRY",
};

inline constexpr auto statePostCodeReceived =
    "com.nvidia.PreBootDiag.App.StateType.PostCodeReceived";

// The App.Notify payload for a known post code name, or nullopt.
std::optional<std::string> postCodePayload(std::string_view name);

// Sends App.Notify(state, payload) to the running prebootdiag service.
using NotifyCall =
    std::function<void(const std::string& state, const std::string& payload)>;

// Runs "prebootdiag-postcode-notify <PostCodeName>": validates the name and
// calls App.Notify with the PostCodeReceived state. Returns the exit code.
int notifyPostCode(int argc, const char* const argv[], const NotifyCall& call,
                   std::ostream& err);

} // namespace prebootdiag
