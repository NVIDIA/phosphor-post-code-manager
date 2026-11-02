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
#include "postcode_notify.hpp"

#include <sdbusplus/bus.hpp>

#include <iostream>

int main(int argc, char* argv[])
{
    return prebootdiag::notifyPostCode(
        argc, argv,
        [](const std::string& state, const std::string& payload) {
            auto bus = sdbusplus::bus::new_default();
            auto method = bus.new_method_call(
                "com.nvidia.PreBootDiag", "/com/nvidia/prebootdiag",
                "com.nvidia.PreBootDiag.App", "Notify");
            method.append(state, payload);
            bus.call(method);
        },
        std::cerr);
}
