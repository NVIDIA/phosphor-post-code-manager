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

#include <sdbusplus/test/sdbus_mock.hpp>

#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::NiceMock;

uint32_t postcodeToUint32(const std::vector<uint8_t>& code);
uint8_t extractStatusType(uint32_t postcode);
uint8_t extractClass(uint32_t postcode);
uint8_t extractSubclass(uint32_t postcode);
uint16_t extractInstance(uint32_t postcode);
uint8_t extractOpcode(uint32_t postcode);
std::string_view getFirmwareName(uint8_t subclass);
int getPackageNumber(uint8_t classField);

class NvidiaPostCodeHandlerTest : public ::testing::Test
{
  protected:
    NvidiaPostCodeHandlerTest() :
        bus_mock(std::make_unique<NiceMock<sdbusplus::SdBusMock>>()),
        bus(sdbusplus::get_mocked_new(bus_mock.get()))
    {}

    std::unique_ptr<NiceMock<sdbusplus::SdBusMock>> bus_mock;
    sdbusplus::bus_t bus;
};

TEST_F(NvidiaPostCodeHandlerTest, PostcodeToUint32)
{
    std::vector<uint8_t> code = {0x12, 0x34, 0x56, 0x78};
    uint32_t result = postcodeToUint32(code);
    EXPECT_EQ(result, 0x12345678);
}

TEST_F(NvidiaPostCodeHandlerTest, ExtractStatusType)
{
    uint32_t postcode = 0xC0000000;
    uint8_t result = extractStatusType(postcode);
    EXPECT_EQ(result, 0x03);
}

TEST_F(NvidiaPostCodeHandlerTest, ExtractClass)
{
    uint32_t postcode = 0x0F000000;
    uint8_t result = extractClass(postcode);
    EXPECT_EQ(result, 0x0F);
}

TEST_F(NvidiaPostCodeHandlerTest, ExtractSubclass)
{
    uint32_t postcode = 0x00C00000;
    uint8_t result = extractSubclass(postcode);
    EXPECT_EQ(result, 0xC0);
}

TEST_F(NvidiaPostCodeHandlerTest, ExtractInstance)
{
    uint32_t postcode = 0x0000FFC0;
    uint16_t result = extractInstance(postcode);
    EXPECT_EQ(result, 0x03FF);
}

TEST_F(NvidiaPostCodeHandlerTest, ExtractOpcode)
{
    uint32_t postcode = 0x0000003F;
    uint8_t result = extractOpcode(postcode);
    EXPECT_EQ(result, 0x3F);
}

TEST_F(NvidiaPostCodeHandlerTest, GetFirmwareNameRom)
{
    std::string_view result = getFirmwareName(0xC0);
    EXPECT_EQ(result, "ROM");
}

TEST_F(NvidiaPostCodeHandlerTest, GetFirmwareNamePscbl)
{
    std::string_view result = getFirmwareName(0xC1);
    EXPECT_EQ(result, "PSCBL");
}

TEST_F(NvidiaPostCodeHandlerTest, GetFirmwareNameSipRange)
{
    std::string_view result = getFirmwareName(0xD0);
    EXPECT_EQ(result, "SiP-Range");
}

TEST_F(NvidiaPostCodeHandlerTest, GetFirmwareNameUnknown)
{
    std::string_view result = getFirmwareName(0x00);
    EXPECT_EQ(result, "Unknown");
}

TEST_F(NvidiaPostCodeHandlerTest, GetPackageNumberCompressed)
{
    int result = getPackageNumber(0x08);
    EXPECT_EQ(result, 0);
}

TEST_F(NvidiaPostCodeHandlerTest, GetPackageNumberUncompressed)
{
    int result = getPackageNumber(0x30);
    EXPECT_EQ(result, 0);
}

TEST_F(NvidiaPostCodeHandlerTest, GetPackageNumberInvalid)
{
    int result = getPackageNumber(0x00);
    EXPECT_EQ(result, -1);
}

TEST_F(NvidiaPostCodeHandlerTest, GetPackageNumberCompressedPackage1)
{
    int result = getPackageNumber(0x09);
    EXPECT_EQ(result, 1);
}

TEST_F(NvidiaPostCodeHandlerTest, GetPackageNumberUncompressedPackage1)
{
    int result = getPackageNumber(0x31);
    EXPECT_EQ(result, 1);
}

TEST_F(NvidiaPostCodeHandlerTest, GetFirmwareNameSipRangeMid)
{
    std::string_view result = getFirmwareName(0xCA);
    EXPECT_EQ(result, "SiP-Range");
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeErrorType)
{
    std::vector<uint8_t> code = {0x80, 0x08, 0xC0, 0x01};
    std::optional<std::string> resolution = "Test resolution";
    EXPECT_NO_THROW(logNvidiaPostCode(code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeProgressType)
{
    std::vector<uint8_t> code = {0x40, 0x08, 0xC0, 0x01};
    std::optional<std::string> resolution;
    logNvidiaPostCode(code, resolution);
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeDebugType)
{
    std::vector<uint8_t> code = {0xC0, 0x08, 0xC0, 0x01};
    std::optional<std::string> resolution;
    logNvidiaPostCode(code, resolution);
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeWrongSize)
{
    std::vector<uint8_t> code = {0x01, 0x02, 0x03};
    std::optional<std::string> resolution;
    logNvidiaPostCode(code, resolution);
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeEmptyResolution)
{
    std::vector<uint8_t> code = {0x80, 0x08, 0xC0, 0x01};
    std::optional<std::string> resolution = "";
    EXPECT_NO_THROW(logNvidiaPostCode(code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeNoResolution)
{
    std::vector<uint8_t> code = {0x80, 0x08, 0xC0, 0x01};
    std::optional<std::string> resolution = std::nullopt;
    EXPECT_NO_THROW(logNvidiaPostCode(code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeException)
{
    std::vector<uint8_t> code = {0x80, 0x08, 0xC0, 0x01};
    std::optional<std::string> resolution = "Test";
    EXPECT_NO_THROW(logNvidiaPostCode(code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeFullError)
{
    std::vector<uint8_t> code = {0x82, 0x0F, 0xC1, 0xFF};
    std::optional<std::string> resolution = "Full resolution";
    EXPECT_NO_THROW(logNvidiaPostCode(code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeWithResolutionNonEmpty)
{
    std::vector<uint8_t> code = {0x80, 0x08, 0xC0, 0x01};
    std::optional<std::string> resolution = "Check firmware version";
    EXPECT_NO_THROW(logNvidiaPostCode(code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeCatchBlockSwallowsException)
{
    std::vector<uint8_t> code = {0x80, 0x30, 0xC0, 0x00};
    std::optional<std::string> resolution = std::nullopt;
    EXPECT_NO_THROW(logNvidiaPostCode(code, resolution));
}
