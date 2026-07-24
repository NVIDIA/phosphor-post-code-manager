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

#include <array>
#include <cerrno>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::Throw;

uint32_t postcodeToUint32(const std::vector<uint8_t>& code);
uint8_t extractStatusType(uint32_t postcode);
uint8_t extractClass(uint32_t postcode);
uint8_t extractSubclass(uint32_t postcode);
uint16_t extractInstance(uint32_t postcode);
uint8_t extractOpcode(uint32_t postcode);
std::string_view getFirmwareName(uint8_t subclass);
std::optional<int> getPackageNumber(uint8_t classField);
std::optional<std::string_view> getResetReasonName(
    uint8_t subclass, uint8_t statusType, uint8_t opcode, uint16_t instance);

using InstData = std::pair<uint8_t, std::string_view>;
std::optional<std::string_view> findInstanceName(std::span<const InstData> arr,
                                                 uint16_t instance);
std::string_view getSubclassName(uint8_t classField, uint8_t subclass);
std::optional<std::string_view> getOperationName(
    uint8_t classField, uint8_t subclass, uint8_t statusType, uint8_t opcode,
    uint16_t operation);

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
    uint32_t postcode = 0x00001FC0;
    uint16_t result = extractInstance(postcode);
    EXPECT_EQ(result, 0x7F);
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

TEST_F(NvidiaPostCodeHandlerTest, GetPackageNumberSipMin)
{
    auto result = getPackageNumber(0x30);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, 0);
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetPackageNumberSipPackage1)
{
    auto result = getPackageNumber(0x31);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, 1);
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetPackageNumberSipMax)
{
    auto result = getPackageNumber(0x37);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, 7);
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetPackageNumberInvalid)
{
    auto result = getPackageNumber(0x00);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceRomI2cMsgInitFirst)
{
    auto result = getResetReasonName(0xC0, 0x01, 0x01, 0x00);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "SYS_RESET_N");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceRomI2cMsgInitBpmpWdt)
{
    auto result = getResetReasonName(0xC0, 0x01, 0x01, 0x1E);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "BPMP_WDT_POR");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceRomI2cMsgInitLast)
{
    auto result = getResetReasonName(0xC0, 0x01, 0x01, 0x37);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "SC7");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceRomUsbMsgInit)
{
    auto result = getResetReasonName(0xC0, 0x01, 0x03, 0x16);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "PSC_SW");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceRomOutOfRange)
{
    auto result = getResetReasonName(0xC0, 0x01, 0x01, 0x38);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceRomWrongOpcode)
{
    auto result = getResetReasonName(0xC0, 0x01, 0x02, 0x00);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceFmcNvdlinkFirst)
{
    auto result = getResetReasonName(0xC1, 0x01, 0x03, 0x00);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "SYS_RESET_N");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceFmcNvdlinkSc7)
{
    auto result = getResetReasonName(0xC1, 0x01, 0x03, 0x1E);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "SC7");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceFmcNvdlinkL0RstSys)
{
    auto result = getResetReasonName(0xC1, 0x01, 0x03, 0x04);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "L0_RST_REQ_N_SYS");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceFmcOutOfRange)
{
    auto result = getResetReasonName(0xC1, 0x01, 0x03, 0x1F);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceFmcWrongOpcode)
{
    auto result = getResetReasonName(0xC1, 0x01, 0x01, 0x00);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceUnknownSubclass)
{
    auto result = getResetReasonName(0xD0, 0x01, 0x01, 0x00);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, FindInstanceNameDenseFirst)
{
    static constexpr std::array<InstData, 3> dense = {{
        {0x00, "A"},
        {0x01, "B"},
        {0x02, "C"},
    }};
    auto result = findInstanceName(dense, 0x00);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "A");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, FindInstanceNameDenseLast)
{
    static constexpr std::array<InstData, 3> dense = {{
        {0x00, "A"},
        {0x01, "B"},
        {0x02, "C"},
    }};
    auto result = findInstanceName(dense, 0x02);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "C");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, FindInstanceNameDenseOutOfRange)
{
    static constexpr std::array<InstData, 3> dense = {{
        {0x00, "A"},
        {0x01, "B"},
        {0x02, "C"},
    }};
    auto result = findInstanceName(dense, 0x03);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, FindInstanceNameSparseHitAfterGap)
{
    static constexpr std::array<InstData, 4> sparse = {{
        {0x14, "CARVEOUT_PSC_SYSRAM"},
        {0x15, "CARVEOUT_CMET"},
        {0x21, "CARVEOUT_ROOT_SRAM"},
        {0x22, "CARVEOUT_RAS_UEFIMM"},
    }};
    auto result = findInstanceName(sparse, 0x21);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "CARVEOUT_ROOT_SRAM");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, FindInstanceNameSparseGapMisses)
{
    static constexpr std::array<InstData, 4> sparse = {{
        {0x14, "CARVEOUT_PSC_SYSRAM"},
        {0x15, "CARVEOUT_CMET"},
        {0x21, "CARVEOUT_ROOT_SRAM"},
        {0x22, "CARVEOUT_RAS_UEFIMM"},
    }};
    auto result = findInstanceName(sparse, 0x16);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, FindInstanceNameSparseBelowFirst)
{
    static constexpr std::array<InstData, 4> sparse = {{
        {0x14, "CARVEOUT_PSC_SYSRAM"},
        {0x15, "CARVEOUT_CMET"},
        {0x21, "CARVEOUT_ROOT_SRAM"},
        {0x22, "CARVEOUT_RAS_UEFIMM"},
    }};
    auto result = findInstanceName(sparse, 0x00);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, FindInstanceNameEmpty)
{
    std::array<InstData, 0> empty{};
    auto result = findInstanceName(empty, 0x00);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeErrorType)
{
    std::vector<uint8_t> code = {0x80, 0x08, 0xC0, 0x01};
    std::optional<std::string> resolution = "Test resolution";
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeProgressType)
{
    std::vector<uint8_t> code = {0x40, 0x08, 0xC0, 0x01};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeDebugType)
{
    std::vector<uint8_t> code = {0xC0, 0x08, 0xC0, 0x01};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeWrongSize)
{
    std::vector<uint8_t> code = {0x01, 0x02, 0x03};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeEmptyResolution)
{
    std::vector<uint8_t> code = {0x80, 0x08, 0xC0, 0x01};
    std::optional<std::string> resolution = "";
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeNoResolution)
{
    std::vector<uint8_t> code = {0x80, 0x08, 0xC0, 0x01};
    std::optional<std::string> resolution = std::nullopt;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeFullError)
{
    std::vector<uint8_t> code = {0x82, 0x0F, 0xC1, 0xFF};
    std::optional<std::string> resolution = "Full resolution";
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeWithResolutionNonEmpty)
{
    std::vector<uint8_t> code = {0x80, 0x08, 0xC0, 0x01};
    std::optional<std::string> resolution = "Check firmware version";
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeCatchBlockSwallowsException)
{
    std::vector<uint8_t> code = {0x80, 0x30, 0xC0, 0x00};
    std::optional<std::string> resolution = std::nullopt;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeProgressWithResetReason)
{
    std::vector<uint8_t> code = {0x70, 0xC0, 0xC0, 0x01};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeProgressBpmpWdtPor)
{
    std::vector<uint8_t> code = {0x70, 0xC0, 0xC7, 0x81};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeErrorWithResetReason)
{
    std::vector<uint8_t> code = {0xB0, 0xC0, 0xC7, 0x81};
    std::optional<std::string> resolution = "Check reset source";
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeFmcProgressSc7)
{
    std::vector<uint8_t> code = {0x70, 0xC1, 0xC7, 0x83};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PostcodeToUint32AllZeros)
{
    std::vector<uint8_t> code = {0x00, 0x00, 0x00, 0x00};
    EXPECT_EQ(postcodeToUint32(code), 0x00000000u);
}

TEST_F(NvidiaPostCodeHandlerTest, PostcodeToUint32AllOnes)
{
    std::vector<uint8_t> code = {0xFF, 0xFF, 0xFF, 0xFF};
    EXPECT_EQ(postcodeToUint32(code), 0xFFFFFFFFu);
}

TEST_F(NvidiaPostCodeHandlerTest, PostcodeToUint32ByteOrder)
{
    std::vector<uint8_t> code = {0x01, 0x00, 0x00, 0x00};
    EXPECT_EQ(postcodeToUint32(code), 0x01000000u);
}

TEST_F(NvidiaPostCodeHandlerTest, GetFirmwareNameBelowSipMin)
{
    EXPECT_EQ(getFirmwareName(0xBF), "Unknown");
}

TEST_F(NvidiaPostCodeHandlerTest, GetFirmwareNameSipRangeMid)
{
    EXPECT_EQ(getFirmwareName(0xC2), "SiP-Range");
}

TEST_F(NvidiaPostCodeHandlerTest, GetFirmwareNameSipRangeMax)
{
    EXPECT_EQ(getFirmwareName(0xDF), "SiP-Range");
}

TEST_F(NvidiaPostCodeHandlerTest, GetFirmwareNameAboveSipMax)
{
    EXPECT_EQ(getFirmwareName(0xE0), "Unknown");
}

TEST_F(NvidiaPostCodeHandlerTest, GetPackageNumberBelowMin)
{
    auto result = getPackageNumber(0x2F);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, GetPackageNumberAboveMax)
{
    auto result = getPackageNumber(0x38);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceRomUsbSameTable)
{
    auto result = getResetReasonName(0xC0, 0x01, 0x03, 0x00);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "SYS_RESET_N");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceRomUsbLastEntry)
{
    auto result = getResetReasonName(0xC0, 0x01, 0x03, 0x37);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "SC7");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceFmcInstanceZero)
{
    auto result = getResetReasonName(0xC1, 0x01, 0x03, 0x00);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "SYS_RESET_N");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceWrongSubclassMatchingOpcode)
{
    auto result = getResetReasonName(0xC2, 0x01, 0x01, 0x00);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceErrorStatusTypeReturnsFalse)
{
    auto result = getResetReasonName(0xC1, 0x02, 0x03, 0x00);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeWithDescription)
{
    std::vector<uint8_t> code = {0xB0, 0xC0, 0xC0, 0x01};
    std::optional<std::string> resolution;
    std::optional<std::string> description =
        "Fail to enable I2C for Ext Messaging";
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution, description));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeEmptyDescription)
{
    std::vector<uint8_t> code = {0xB0, 0xC0, 0xC0, 0x01};
    std::optional<std::string> resolution;
    std::optional<std::string> description = "";
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution, description));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeFmcErrorWithResetReason)
{
    std::vector<uint8_t> code = {0xB0, 0xC1, 0xC0, 0x03};
    std::optional<std::string> resolution = "Check boot chain ledger";
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeErrorWithInstanceName)
{
    std::vector<uint8_t> code = {0xB0, 0xC1, 0xC0, 0x08};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeErrorNoInstanceName)
{
    std::vector<uint8_t> code = {0xB0, 0xC0, 0xC0, 0x02};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeUnknownSubclassAndCpu)
{
    std::vector<uint8_t> code = {0x80, 0x00, 0x00, 0x00};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiErrorManageabilityOperationAbove1000)
{
    std::vector<uint8_t> code = {0x80, 0x07, 0x10, 0x00};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeOperationLowerBoundMiss)
{
    std::vector<uint8_t> code = {0xB0, 0xC0, 0x00, 0x00};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiErrorComputingUnitKnownOperation)
{
    std::vector<uint8_t> code = {0x80, 0x07, 0x00, 0x02};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeOperationEndMiss)
{
    std::vector<uint8_t> code = {0xB0, 0xDF, 0x00, 0x3F};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiErrorComputingUnitUnknownOperationBelow1000)
{
    std::vector<uint8_t> code = {0x80, 0x07, 0x00, 0xFF};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeInstanceEndMiss)
{
    std::vector<uint8_t> code = {0xB0, 0xC9, 0x00, 0x16};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiErrorUnknownSubclass)
{
    std::vector<uint8_t> code = {0x80, 0xFF, 0x00, 0x02};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeNonStdExceptionPropagates)
{
    std::vector<uint8_t> code = {0x80, 0x00, 0x00, 0x00};
    std::optional<std::string> resolution;

    EXPECT_CALL(*bus_mock, sd_bus_call(_, _, _, _, _)).WillOnce(Throw(7));

    EXPECT_THROW(logNvidiaPostCode(bus, code, resolution), int);
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeCallNoReplyFailureIsCaught)
{
    std::vector<uint8_t> code = {0x80, 0x00, 0x00, 0x00};
    std::optional<std::string> resolution;

    EXPECT_CALL(*bus_mock, sd_bus_call(_, _, _, _, _)).WillOnce(Return(-EIO));

    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

// subclass = 0xE0 is above efiSubclassSipMax (0xDF): getSipSubclassName takes
// the false branch of (subclass >= min && subclass <= max) and returns
// "Unknown".
TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeSubclassAboveSipMax)
{
    std::vector<uint8_t> code = {0x80, 0xE0, 0x00, 0x01};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}
TEST_F(NvidiaPostCodeHandlerTest, PiErrorPeripheralKnownOperation)
{
    std::vector<uint8_t> code = {0x81, 0x07, 0x00, 0x02};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiErrorIoBusKnownOperation)
{
    std::vector<uint8_t> code = {0x82, 0x01, 0x00, 0x01};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiErrorSoftwareKnownOperation)
{
    std::vector<uint8_t> code = {0x83, 0x04, 0x00, 0x01};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiProgressComputingUnitInitBegin)
{
    std::vector<uint8_t> code = {0x40, 0x00, 0x00, 0x00};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiProgressComputingUnitInitEnd)
{
    std::vector<uint8_t> code = {0x40, 0x00, 0x00, 0x01};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiProgressUnknownOperationReturnEarly)
{
    std::vector<uint8_t> code = {0x40, 0x00, 0x10, 0x00};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiProgressIoBusInit)
{
    std::vector<uint8_t> code = {0x42, 0x01, 0x00, 0x00};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiProgressSoftwareInit)
{
    std::vector<uint8_t> code = {0x43, 0x01, 0x00, 0x01};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiErrorWithDescription)
{
    std::vector<uint8_t> code = {0x80, 0x07, 0x10, 0x00};
    std::optional<std::string> resolution = "Check manageability firmware";
    std::optional<std::string> description = "Manageability unit error";
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution, description));
}

TEST_F(NvidiaPostCodeHandlerTest, PiErrorPeripheralSubclassMin)
{
    std::vector<uint8_t> code = {0x81, 0x00, 0x00, 0x00};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiErrorPeripheralSubclassMax)
{
    std::vector<uint8_t> code = {0x81, 0x0E, 0x00, 0x00};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiErrorSoftwareSubclassMaxKnownOp)
{
    std::vector<uint8_t> code = {0x83, 0x12, 0x00, 0x0D};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiErrorIoBusSubclassMaxKnownOp)
{
    std::vector<uint8_t> code = {0x82, 0x0C, 0x00, 0x06};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiErrorOperationBelowThresholdNotInTable)
{
    std::vector<uint8_t> code = {0x80, 0x00, 0x00, 0xFF};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiProgressOperationBelowThresholdNotInTable)
{
    std::vector<uint8_t> code = {0x40, 0x00, 0x00, 0x02};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, PiProgressPeripheralKnownOperation)
{
    std::vector<uint8_t> code = {0x41, 0x00, 0x00, 0x02};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNamePiComputingUnitUnspecified)
{
    EXPECT_EQ(getSubclassName(0x00, 0x00), "EFI_COMPUTING_UNIT_UNSPECIFIED");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNamePiComputingUnitManageability)
{
    EXPECT_EQ(getSubclassName(0x00, 0x07), "EFI_COMPUTING_UNIT_MANAGEABILITY");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNamePiComputingUnitUnknown)
{
    EXPECT_EQ(getSubclassName(0x00, 0x08), "Unknown");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNamePiPeripheralFixedMedia)
{
    EXPECT_EQ(getSubclassName(0x01, 0x07), "EFI_PERIPHERAL_FIXED_MEDIA");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNamePiPeripheralUnknown)
{
    EXPECT_EQ(getSubclassName(0x01, 0x0F), "Unknown");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNamePiIoBusPci)
{
    EXPECT_EQ(getSubclassName(0x02, 0x01), "EFI_IO_BUS_PCI");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNamePiIoBusI2c)
{
    EXPECT_EQ(getSubclassName(0x02, 0x0C), "EFI_IO_BUS_I2C");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNamePiSoftwareDxeCore)
{
    EXPECT_EQ(getSubclassName(0x03, 0x04), "EFI_SOFTWARE_DXE_CORE");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNamePiSoftwareUnknown)
{
    EXPECT_EQ(getSubclassName(0x03, 0x15), "Unknown");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNameSipPscrom)
{
    EXPECT_EQ(getSubclassName(0x30, 0xC0), "PSCROM");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNameSipPscfmc)
{
    EXPECT_EQ(getSubclassName(0x30, 0xC1), "PSCFMC");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNameSipSubclassBelowRange)
{
    EXPECT_EQ(getSubclassName(0x30, 0xBF), "Unknown");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNameSipSubclassAboveRange)
{
    EXPECT_EQ(getSubclassName(0x30, 0xE0), "Unknown");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNameAbovePiMaxNotSip)
{
    EXPECT_EQ(getSubclassName(0x04, 0x00), "Unknown");
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiProgressInitBegin)
{
    auto result = getOperationName(0x00, 0x00, 0x01, 0x00, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_CU_PC_INIT_BEGIN");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiProgressInitEnd)
{
    auto result = getOperationName(0x00, 0x00, 0x01, 0x00, 0x0001);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_CU_PC_INIT_END");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiErrorKnown)
{
    auto result = getOperationName(0x00, 0x07, 0x02, 0x00, 0x0002);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_CU_EC_NOT_SUPPORTED");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiAboveThreshold)
{
    auto result = getOperationName(0x00, 0x07, 0x02, 0x00, 0x1000);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiBelowThresholdNotInTable)
{
    auto result = getOperationName(0x00, 0x07, 0x02, 0x00, 0x00FF);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiPeripheralProgressEnable)
{
    auto result = getOperationName(0x01, 0x00, 0x01, 0x00, 0x0004);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_P_PC_ENABLE");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiIoBusErrorNotConfigured)
{
    auto result = getOperationName(0x02, 0x01, 0x02, 0x00, 0x0004);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_IOB_EC_NOT_CONFIGURED");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiSoftwareProgressHandoff)
{
    auto result = getOperationName(0x03, 0x00, 0x01, 0x00, 0x0006);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_SW_PC_HANDOFF_TO_NEXT");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipKnown)
{
    auto result = getOperationName(0x30, 0xC0, 0x02, 0x01, 0xC001);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "PSC_ROM_EC_I2C_EXT_MSG_FAIL");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipUnknownOpcode)
{
    auto result = getOperationName(0x30, 0xC0, 0x02, 0x00, 0xC000);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiIteratorReachesEnd)
{
    auto result = getOperationName(0x03, 0x00, 0x02, 0x00, 0x0015);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipIteratorReachesEnd)
{
    // opcode=0x02 is past the last SiP table entry {0xDF,2,0x01}
    auto result = getOperationName(0x30, 0xDF, 0x02, 0x02, 0x0000);
    EXPECT_FALSE(result.has_value());
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNameSipSubclassMaxEntry)
{
    EXPECT_EQ(getSubclassName(0x30, 0xDF), "C2C_LPI_C1");
}

TEST_F(NvidiaPostCodeHandlerTest,
       GetSubclassNameBetweenPiAndSipClassWithSipSubclass)
{
    // classField=0x10 is above piClassMax but below efiClassSipMin;
    // subclass=0xC0 falls in SiP subclass range so sipSubclassNames is used
    EXPECT_EQ(getSubclassName(0x10, 0xC0), "PSCROM");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNamePiComputingUnitHostProcessor)
{
    EXPECT_EQ(getSubclassName(0x00, 0x01), "EFI_COMPUTING_UNIT_HOST_PROCESSOR");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNamePiComputingUnitCache)
{
    EXPECT_EQ(getSubclassName(0x00, 0x04), "EFI_COMPUTING_UNIT_CACHE");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNamePiComputingUnitChipset)
{
    EXPECT_EQ(getSubclassName(0x00, 0x06), "EFI_COMPUTING_UNIT_CHIPSET");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNamePiPeripheralDocking)
{
    EXPECT_EQ(getSubclassName(0x01, 0x0D), "EFI_PERIPHERAL_DOCKING");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNamePiSoftwareX64Exception)
{
    EXPECT_EQ(getSubclassName(0x03, 0x13), "EFI_SOFTWARE_X64_EXCEPTION");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNamePiSoftwareArmException)
{
    EXPECT_EQ(getSubclassName(0x03, 0x14), "EFI_SOFTWARE_ARM_EXCEPTION");
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiPeripheralProgressDisable)
{
    auto result = getOperationName(0x01, 0x00, 0x01, 0x00, 0x0002);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_P_PC_DISABLE");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiPeripheralProgressReconfig)
{
    auto result = getOperationName(0x01, 0x00, 0x01, 0x00, 0x0005);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_P_PC_RECONFIG");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiPeripheralProgressRemoved)
{
    auto result = getOperationName(0x01, 0x00, 0x01, 0x00, 0x0007);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_P_PC_REMOVED");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiPeripheralErrorNonSpecific)
{
    auto result = getOperationName(0x01, 0x00, 0x02, 0x00, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_P_EC_NON_SPECIFIC");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiPeripheralErrorDisabled)
{
    auto result = getOperationName(0x01, 0x00, 0x02, 0x00, 0x0001);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_P_EC_DISABLED");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiPeripheralErrorOutputError)
{
    auto result = getOperationName(0x01, 0x00, 0x02, 0x00, 0x0008);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_P_EC_OUTPUT_ERROR");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiIoBusProgressHotplug)
{
    auto result = getOperationName(0x02, 0x00, 0x01, 0x00, 0x0006);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_IOB_PC_HOTPLUG");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiIoBusErrorDisabled)
{
    auto result = getOperationName(0x02, 0x01, 0x02, 0x00, 0x0001);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_IOB_EC_DISABLED");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiIoBusErrorReadError)
{
    auto result = getOperationName(0x02, 0x01, 0x02, 0x00, 0x0007);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_IOB_EC_READ_ERROR");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiIoBusErrorResourceConflict)
{
    auto result = getOperationName(0x02, 0x01, 0x02, 0x00, 0x0009);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_IOB_EC_RESOURCE_CONFLICT");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNamePiSoftwareErrorFvCorrupted)
{
    auto result = getOperationName(0x03, 0x00, 0x02, 0x00, 0x0013);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_SW_EC_FV_CORRUPTED");
    }
}

TEST_F(NvidiaPostCodeHandlerTest,
       GetOperationNamePiSoftwareErrorInconsistentMemoryMap)
{
    auto result = getOperationName(0x03, 0x00, 0x02, 0x00, 0x0014);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "EFI_SW_EC_INCONSISTENT_MEMORY_MAP");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNameSipBpmpFw)
{
    EXPECT_EQ(getSubclassName(0x30, 0xC4), "BPMP_FW");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNameSipMb2)
{
    EXPECT_EQ(getSubclassName(0x30, 0xC5), "MB2");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNameSipAtfBl31)
{
    EXPECT_EQ(getSubclassName(0x30, 0xC6), "ATF_BL31");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNameSipOobhubFw)
{
    EXPECT_EQ(getSubclassName(0x30, 0xCB), "OOBHUB_FW");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNameSipRasFw)
{
    EXPECT_EQ(getSubclassName(0x30, 0xCC), "RAS_FW");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNameSipMseqFw)
{
    EXPECT_EQ(getSubclassName(0x30, 0xCD), "MSEQ_FW");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNameSipPcore0Fw)
{
    EXPECT_EQ(getSubclassName(0x30, 0xCE), "PCORE0_FW");
}

TEST_F(NvidiaPostCodeHandlerTest, GetSubclassNameSipC2cUphy5)
{
    EXPECT_EQ(getSubclassName(0x30, 0xDB), "C2C_UPHY5");
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipPscRomBootModeSel)
{
    auto result = getOperationName(0x30, 0xC0, 0x01, 0x02, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "PSC_ROM_PC_BOOT_MODE_SEL_DONE");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipPscRomRomExit)
{
    auto result = getOperationName(0x30, 0xC0, 0x01, 0x0B, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "PSC_ROM_PC_ROM_EXIT");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipPscFmcInit)
{
    auto result = getOperationName(0x30, 0xC1, 0x01, 0x01, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "PSC_FMC_PC_INIT");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipPscFmcBootstrap)
{
    auto result = getOperationName(0x30, 0xC1, 0x01, 0x09, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "PSC_FMC_PC_BOOTSTRAP");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipPscFmcStage1AuthFailed)
{
    auto result = getOperationName(0x30, 0xC1, 0x02, 0x09, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "PSC_FMC_EC_STAGE1_AUTHENTICATION_FAILED");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipPscFmcMemFuseCrcFailed)
{
    auto result = getOperationName(0x30, 0xC1, 0x02, 0x0D, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "PSC_FMC_EC_MEM_FUSE_CRC_FAILED");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipPscRtInit)
{
    auto result = getOperationName(0x30, 0xC2, 0x01, 0x01, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "PSC_RT_PC_INIT");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipMb1Init)
{
    auto result = getOperationName(0x30, 0xC3, 0x01, 0x01, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "MB1_PC_INIT");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipMb1Exit)
{
    auto result = getOperationName(0x30, 0xC3, 0x01, 0x12, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "MB1_PC_EXIT");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipMb1FuseIntegrityFailed)
{
    auto result = getOperationName(0x30, 0xC3, 0x02, 0x01, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "MB1_EC_FUSE_RECORD_INTEGRITY_FAILED");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipMb1UcfError)
{
    auto result = getOperationName(0x30, 0xC3, 0x02, 0x19, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "MB1_EC_UCF_ERROR");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipBpmpFwInitComplete)
{
    auto result = getOperationName(0x30, 0xC4, 0x01, 0x3F, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "BPMP_FW_PC_INIT_COMPLETE");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipMb2SetHwBreakpoint)
{
    auto result = getOperationName(0x30, 0xC5, 0x01, 0x03, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "MB2_PC_SET_HW_BREAK_POINT");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipBl31BootComplete)
{
    auto result = getOperationName(0x30, 0xC6, 0x01, 0x07, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "BL31_PC_BOOT_COMPLETE");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipBl31PscMailboxUnavail)
{
    auto result = getOperationName(0x30, 0xC6, 0x02, 0x01, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "BL31_EC_PSC_MAILBOX_UNAVAIL");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipOobhubMctpInit)
{
    auto result = getOperationName(0x30, 0xCB, 0x01, 0x03, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "OOBHUB_PC_MCTP_INIT");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipRasFwMgmtReady)
{
    auto result = getOperationName(0x30, 0xCC, 0x01, 0x01, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "RAS_FW_PC_MGMT_READY");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipMseqFwBootComplete)
{
    auto result = getOperationName(0x30, 0xCD, 0x01, 0x03, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "MSEQ_FW_PC_BOOT_COMPLETE");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipPcore0FwInit)
{
    auto result = getOperationName(0x30, 0xCE, 0x01, 0x01, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "PCORE0_FW_PC_INIT");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipPcore0FwUphyInitFailed)
{
    auto result = getOperationName(0x30, 0xCE, 0x02, 0x01, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "PCORE0_FW_EC_UPHY_INIT_FAILED");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipC2cGrs0PcInit)
{
    auto result = getOperationName(0x30, 0xD4, 0x01, 0x01, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "C2C_GRS0_PC_INIT");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipC2cGrs0PcTraining)
{
    auto result = getOperationName(0x30, 0xD4, 0x01, 0x02, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "C2C_GRS0_PC_TRAINING");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipC2cUphy0PcTraining)
{
    auto result = getOperationName(0x30, 0xD6, 0x01, 0x02, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "C2C_UPHY0_PC_TRAINING");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipC2cUphy5PcInit)
{
    auto result = getOperationName(0x30, 0xDB, 0x01, 0x01, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "C2C_UPHY5_PC_INIT");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipC2cUphy5EcTrainingFailed)
{
    auto result = getOperationName(0x30, 0xDB, 0x02, 0x01, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "C2C_UPHY5_EC_TRAINING_FAILED");
    }
}

TEST_F(NvidiaPostCodeHandlerTest, GetOperationNameSipC2cLpiS0PcInit)
{
    auto result = getOperationName(0x30, 0xDC, 0x01, 0x01, 0x0000);
    ASSERT_TRUE(result.has_value());
    if (result)
    {
        EXPECT_EQ(*result, "C2C_LPI_S0_PC_INIT");
    }
}
