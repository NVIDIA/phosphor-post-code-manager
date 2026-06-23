/*
// Copyright (c) 2024 NVIDIA Corporation
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

#include <sdbusplus/bus.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// NVIDIA TB500 POST code logging function
// Decodes and logs error codes with socket, instance, and firmware information.
// Uses the caller's bus (e.g. PostCode service connection); no static bus.
void logNvidiaPostCode(
    sdbusplus::bus_t& bus, const std::vector<uint8_t>& code,
    const std::optional<std::string>& resolution,
    const std::optional<std::string>& description = std::nullopt);
