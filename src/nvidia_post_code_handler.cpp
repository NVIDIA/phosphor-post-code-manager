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
#include "nvidia_post_code_handler.hpp"

#include <sdbusplus/bus.hpp>

#include <format>
#include <iostream>
#include <map>
#include <string_view>

// NVIDIA TB500-specific post code format definitions
// 32-bit compressed boot progress code format:
// Bits 31:30 - Status Code Type (2 bits): 0x1=Progress, 0x2=Error, 0x3=Debug
// Bits 29:24 - Compressed Class (6 bits): Package identification for TB500
//              SiP range 0x30-0x37 compressed to 0x08-0x0F
//              Lower 3 bits indicate package number (0-7)
// Bits 23:16 - Subclass (8 bits): Firmware identification
//              SiP range 0xC0-0xDF: ROM(0xC0), PSCBL(0xC1), etc.
// Bits 15:0  - Operation (16 bits): Upper 10 bits=instance#, Lower 6
// bits=opcode

// D-Bus logging service constants
constexpr auto loggingService = "xyz.openbmc_project.Logging";
constexpr auto loggingObject = "/xyz/openbmc_project/logging";
constexpr auto loggingInterface = "xyz.openbmc_project.Logging.Create";
constexpr auto errorSeverity = "xyz.openbmc_project.Logging.Entry.Level.Error";
constexpr auto warningSeverity =
    "xyz.openbmc_project.Logging.Entry.Level.Warning";
constexpr auto resolutionKey = "xyz.openbmc_project.Logging.Entry.Resolution";
// Mask definitions for extracting fields
constexpr uint32_t statusCodeTypeMask = 0xC0000000;
constexpr uint32_t statusCodeClassMask = 0x3F000000;
constexpr uint32_t statusCodeSubclassMask = 0x00FF0000;
constexpr uint32_t statusCodeOperationMask = 0x0000FFFF;

// Operation field subdivision (16 bits total)
constexpr uint32_t statusCodeInstanceMask = 0x0000FFC0; // Upper 10 bits
constexpr uint32_t statusCodeOpcodeMask = 0x0000003F;   // Lower 6 bits

// Shift values
constexpr uint32_t statusCodeTypeShift = 30;
constexpr uint32_t statusCodeClassShift = 24;
constexpr uint32_t statusCodeSubclassShift = 16;
constexpr uint32_t statusCodeInstanceShift = 6;

// Status Code Type definitions
constexpr uint8_t statusTypeProgress = 0x01;
constexpr uint8_t statusTypeError = 0x02;
constexpr uint8_t statusTypeDebug = 0x03;

// NVIDIA TB500-specific class definitions (Package identification)
// Uncompressed (original) class values
constexpr uint8_t efiClassPackage0 = 0x30;
constexpr uint8_t efiClassPackage1 = 0x31;
constexpr uint8_t efiClassSipMin = 0x30;
constexpr uint8_t efiClassSipMax = 0x37;

// Compressed class values (after TB500 compression algorithm)
// Compression: CompressedClass = (Class & 0x7) | ((Class & 0xE0) >> 2)
// For 0x30-0x37 range: compressed to 0x08-0x0F
constexpr uint8_t efiClassSipCompressedMin = 0x08; // 0x30 compressed
constexpr uint8_t efiClassSipCompressedMax = 0x0F; // 0x37 compressed

// NVIDIA TB500-specific subclass definitions (Firmware identification)
constexpr uint8_t efiSubclassRom = 0xC0;
constexpr uint8_t efiSubclassPscbl = 0xC1;
constexpr uint8_t efiSubclassSipMin = 0xC0;
constexpr uint8_t efiSubclassSipMax = 0xDF;

// Helper function to convert 4-byte post code to uint32_t
// Assumes big-endian byte order (MSB first)
uint32_t postcodeToUint32(const std::vector<uint8_t>& code)
{
    return (static_cast<uint32_t>(code[0]) << 24) |
           (static_cast<uint32_t>(code[1]) << 16) |
           (static_cast<uint32_t>(code[2]) << 8) |
           (static_cast<uint32_t>(code[3]));
}

// Extract Status Code Type (bits 31:30)
uint8_t extractStatusType(uint32_t postcode)
{
    return static_cast<uint8_t>(
        (postcode & statusCodeTypeMask) >> statusCodeTypeShift);
}

// Extract Class field (bits 29:24) - used for Package identification
uint8_t extractClass(uint32_t postcode)
{
    return static_cast<uint8_t>(
        (postcode & statusCodeClassMask) >> statusCodeClassShift);
}

// Extract Subclass field (bits 23:16) - used for Firmware identification
uint8_t extractSubclass(uint32_t postcode)
{
    return static_cast<uint8_t>(
        (postcode & statusCodeSubclassMask) >> statusCodeSubclassShift);
}

// Extract Instance number from Operation field (upper 10 bits of 16-bit
// operation)
uint16_t extractInstance(uint32_t postcode)
{
    return static_cast<uint16_t>(
        (postcode & statusCodeInstanceMask) >> statusCodeInstanceShift);
}

// Extract Operation code from Operation field (lower 6 bits of 16-bit
// operation)
uint8_t extractOpcode(uint32_t postcode)
{
    return static_cast<uint8_t>(postcode & statusCodeOpcodeMask);
}

// Get firmware name from subclass
std::string_view getFirmwareName(uint8_t subclass)
{
    switch (subclass)
    {
        case efiSubclassRom:
            return "ROM";
        case efiSubclassPscbl:
            return "PSCBL";
        default:
            if (subclass >= efiSubclassSipMin && subclass <= efiSubclassSipMax)
            {
                return "SiP-Range";
            }
            return "Unknown";
    }
}

// Get package/socket number from class field (compressed or uncompressed)
int getPackageNumber(uint8_t classField)
{
    // Check if in compressed SiP range (0x08-0x0F)
    if (classField >= efiClassSipCompressedMin &&
        classField <= efiClassSipCompressedMax)
    {
        // Decompress to get original class value
        // For SiP range 0x30-0x37, decompression: originalClass = (compressed &
        // 0x7) | 0x30 The lower 3 bits give us the package number directly
        return classField & 0x7;
    }
    // Check if in uncompressed SiP range (0x30-0x37)
    else if (classField >= efiClassSipMin && classField <= efiClassSipMax)
    {
        // For uncompressed values, lower 3 bits directly indicate package
        // number
        return classField & 0x7;
    }
    return -1; // Unknown or not in SiP range
}

// NVIDIA-specific logging function (public API)
void logNvidiaPostCode(sdbusplus::bus_t& bus, const std::vector<uint8_t>& code,
                       const std::optional<std::string>& resolution)
{
    // Only process 4-byte NVIDIA TB500 post codes
    if (code.size() != 4)
    {
        return;
    }

    // Convert to 32-bit value
    uint32_t postcodeValue = postcodeToUint32(code);

    // Check status type first - only log error codes
    uint8_t statusType = extractStatusType(postcodeValue);
    if (statusType != statusTypeError)
    {
        return;
    }

    // Decode remaining fields (only for errors)
    uint8_t classField = extractClass(postcodeValue);
    uint8_t subclass = extractSubclass(postcodeValue);
    uint16_t instance = extractInstance(postcodeValue);
    uint8_t opcode = extractOpcode(postcodeValue);
    int packageNum = getPackageNumber(classField);
    std::string_view firmwareName = getFirmwareName(subclass);

    // Build hex code string from the 32-bit value
    std::string hexCode = std::format("0x{:08X}", postcodeValue);

    // Format the log message
    std::string logMsg =
        std::format("{} detected on Package/Socket: {}, Firmware: "
                    "{}, Instance: {}, OpCode: {}",
                    hexCode, packageNum, firmwareName, instance,
                    std::format("{:#04x}", opcode));

    // Build additional data for logging
    std::map<std::string, std::string> additionalData;

    // Add resolution if available from JSON configuration
    if (resolution && !resolution->empty())
    {
        additionalData[resolutionKey] = *resolution;
    }

    try
    {
        auto method = bus.new_method_call(loggingService, loggingObject,
                                          loggingInterface, "Create");
        method.append(logMsg);
        method.append(errorSeverity);
        method.append(additionalData);
        bus.call_noreply(method);
    }
    catch (const std::exception& e)
    {
        // Silently fail - logging errors shouldn't break POST code handling
        std::cerr << "Failed to log NVIDIA POST code error: " << e.what()
                  << std::endl;
    }
}
