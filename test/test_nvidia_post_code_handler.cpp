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
    EXPECT_EQ(*result, 0);
}

TEST_F(NvidiaPostCodeHandlerTest, GetPackageNumberSipPackage1)
{
    auto result = getPackageNumber(0x31);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1);
}

TEST_F(NvidiaPostCodeHandlerTest, GetPackageNumberSipMax)
{
    auto result = getPackageNumber(0x37);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 7);
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
    EXPECT_EQ(*result, "SYS_RESET_N");
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceRomI2cMsgInitBpmpWdt)
{
    auto result = getResetReasonName(0xC0, 0x01, 0x01, 0x1E);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "BPMP_WDT_POR");
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceRomI2cMsgInitLast)
{
    auto result = getResetReasonName(0xC0, 0x01, 0x01, 0x37);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "SC7");
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceRomUsbMsgInit)
{
    auto result = getResetReasonName(0xC0, 0x01, 0x03, 0x16);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "PSC_SW");
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
    EXPECT_EQ(*result, "SYS_RESET_N");
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceFmcNvdlinkSc7)
{
    auto result = getResetReasonName(0xC1, 0x01, 0x03, 0x1E);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "SC7");
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceFmcNvdlinkL0RstSys)
{
    auto result = getResetReasonName(0xC1, 0x01, 0x03, 0x04);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "L0_RST_REQ_N_SYS");
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
    EXPECT_EQ(*result, "A");
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
    EXPECT_EQ(*result, "C");
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
    EXPECT_EQ(*result, "CARVEOUT_ROOT_SRAM");
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
    EXPECT_EQ(*result, "SYS_RESET_N");
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceRomUsbLastEntry)
{
    auto result = getResetReasonName(0xC0, 0x01, 0x03, 0x37);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "SC7");
}

TEST_F(NvidiaPostCodeHandlerTest, GetResetSourceFmcInstanceZero)
{
    auto result = getResetReasonName(0xC1, 0x01, 0x03, 0x00);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "SYS_RESET_N");
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

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeOperationLowerBoundMiss)
{
    std::vector<uint8_t> code = {0xB0, 0xC0, 0x00, 0x00};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeOperationEndMiss)
{
    std::vector<uint8_t> code = {0xB0, 0xDF, 0x00, 0x3F};
    std::optional<std::string> resolution;
    EXPECT_NO_THROW(logNvidiaPostCode(bus, code, resolution));
}

TEST_F(NvidiaPostCodeHandlerTest, LogNvidiaPostCodeInstanceEndMiss)
{
    std::vector<uint8_t> code = {0xB0, 0xC9, 0x00, 0x16};
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
