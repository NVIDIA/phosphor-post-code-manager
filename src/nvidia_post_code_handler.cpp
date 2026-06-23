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

#include <algorithm>
#include <array>
#include <format>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// NVIDIA TB500-specific post code format definitions
// 32-bit compressed boot progress code format:
// Bits 31:30 - Status Code Type (2 bits): 0x1=Progress, 0x2=Error, 0x3=Debug
// Bits 29:24 - Compressed Class (6 bits): Package identification for TB500
//              SiP original class 0xC0-0xC7 compresses to 0x30-0x37
//              Lower 3 bits indicate package number (0-7)
// Bits 23:16 - Subclass (8 bits): Firmware identification
//              SiP range 0xC0-0xDF: ROM(0xC0), PSCBL(0xC1), etc.
// Bits 15:0  - Operation (16 bits): bits[15:13]=SiP range marker,
//              bits[12:6]=instance, bits[5:0]=opcode

// D-Bus logging service constants
constexpr auto loggingService = "xyz.openbmc_project.Logging";
constexpr auto loggingObject = "/xyz/openbmc_project/logging";
constexpr auto loggingInterface = "xyz.openbmc_project.Logging.Create";
constexpr auto criticalSeverity =
    "xyz.openbmc_project.Logging.Entry.Level.Critical";
constexpr auto informationalSeverity =
    "xyz.openbmc_project.Logging.Entry.Level.Informational";
constexpr auto resolutionKey = "xyz.openbmc_project.Logging.Entry.Resolution";

// Mask definitions for extracting fields
constexpr uint32_t statusCodeTypeMask = 0xC0000000;
constexpr uint32_t statusCodeClassMask = 0x3F000000;
constexpr uint32_t statusCodeSubclassMask = 0x00FF0000;

constexpr uint32_t statusCodeInstanceMask = 0x00001FC0;
constexpr uint32_t statusCodeOpcodeMask = 0x0000003F;

// Shift values
constexpr uint32_t statusCodeTypeShift = 30;
constexpr uint32_t statusCodeClassShift = 24;
constexpr uint32_t statusCodeSubclassShift = 16;
constexpr uint32_t statusCodeInstanceShift = 6;

// Status Code Type definitions
constexpr uint8_t statusTypeProgress = 0x01;
constexpr uint8_t statusTypeError = 0x02;

// NVIDIA TB500-specific class definitions (6-bit compressed class field)
// SiP original class 0xC0-0xC7 compresses to 0x30-0x37 in the post code
constexpr uint8_t efiClassSipMin = 0x30;
constexpr uint8_t efiClassSipMax = 0x37;

// NVIDIA TB500-specific subclass definitions (Firmware identification)
constexpr uint8_t efiSubclassRom = 0xC0;
constexpr uint8_t efiSubclassPscbl = 0xC1;
constexpr uint8_t efiSubclassSipMin = 0xC0;
constexpr uint8_t efiSubclassSipMax = 0xDF;

// Opcode constants for reset source operations
// PSC_ROM (subclass 0xC0): opcodes 0x01 and 0x03 carry reset source in instance
constexpr uint8_t opcodeRomI2cMsgInit = 0x01;
constexpr uint8_t opcodeRomUsbMsgInit = 0x03;
// PSC_FMC (subclass 0xC1): opcode 0x03 carries reset source in instance
constexpr uint8_t opcodeFmcNvdlinkLsLinkUp = 0x03;

static constexpr std::array<std::string_view, 32> sipSubclassNames = {
    "PSCROM",           // 0xC0
    "PSCFMC",           // 0xC1
    "PSCRT",            // 0xC2
    "MB1",              // 0xC3
    "BPMP_FW",          // 0xC4
    "MB2",              // 0xC5
    "ATF_BL31",         // 0xC6
    "RMM",              // 0xC7
    "HAFNIUM",          // 0xC8
    "UEFI",             // 0xC9
    "UEFI_STMM",        // 0xCA
    "OOBHUB_FW",        // 0xCB
    "RAS_FW",           // 0xCC
    "MSEQ_FW",          // 0xCD
    "SEGMENT_CTRL0_FW", // 0xCE
    "SEGMENT_CTRL1_FW", // 0xCF
    "SEGMENT_CTRL2_FW", // 0xD0
    "SEGMENT_CTRL3_FW", // 0xD1
    "SEGMENT_CTRL4_FW", // 0xD2
    "SEGMENT_CTRL5_FW", // 0xD3
    "NVLINK_C2C_0",     // 0xD4
    "NVLINK_C2C_1",     // 0xD5
    "NVCLINK_0",        // 0xD6
    "NVCLINK_1",        // 0xD7
    "NVCLINK_2",        // 0xD8
    "NVCLINK_3",        // 0xD9
    "NVCLINK_4",        // 0xDA
    "NVCLINK_5",        // 0xDB
    "NVDLINK_S_0",      // 0xDC
    "NVDLINK_S_1",      // 0xDD
    "NVDLINK_C_0",      // 0xDE
    "NVDLINK_C_1",      // 0xDF
};

using SipOpKey = std::tuple<uint8_t, uint8_t, uint8_t>;
using SipOpEntry = std::pair<SipOpKey, std::string_view>;
static const std::vector<SipOpEntry> sipOperationNames = []() {
    std::vector<SipOpEntry> v = {
        {{0xC0, 1, 0x01}, "PSC_ROM_PC_I2C_EXT_MSG_INIT"},
        {{0xC0, 1, 0x03}, "PSC_ROM_PC_USB_EXT_MSG_INIT"},
        {{0xC0, 2, 0x01}, "PSC_ROM_EC_I2C_EXT_MSG_FAIL"},
        {{0xC0, 2, 0x02}, "PSC_ROM_EC_BOOT_MODE_SEL_FAIL"},
        {{0xC0, 2, 0x03}, "PSC_ROM_EC_USB_EXT_MSG_FAIL"},
        {{0xC0, 2, 0x04}, "PSC_ROM_EC_EROT_GRANT_FAIL"},
        {{0xC0, 2, 0x05}, "PSC_ROM_EC_QSPI0_DEV_FAIL"},
        {{0xC0, 2, 0x06}, "PSC_ROM_EC_USB2_DEV_FAIL"},
        {{0xC0, 2, 0x07}, "PSC_ROM_EC_OCPRC_DEV_FAIL"},
        {{0xC0, 2, 0x08}, "PSC_ROM_EC_BOOT_CHAIN_EXHAUST"},
        {{0xC0, 2, 0x09}, "PSC_ROM_EC_BOOT_IMAGE_LOAD_FAIL"},
        {{0xC0, 2, 0x0A}, "PSC_ROM_EC_DOT_SLOT_EXHAUST"},
        {{0xC0, 2, 0x0B}, "PSC_ROM_EC_S2A_HEADER_CHECK_FAIL"},
        {{0xC0, 2, 0x0C}, "PSC_ROM_EC_S2A_SANITY_FAIL"},
        {{0xC0, 2, 0x0D}, "PSC_ROM_EC_S2A_INTEGRITY_FAIL"},
        {{0xC0, 2, 0x0E}, "PSC_ROM_EC_S2A_KEY_REVOKED"},
        {{0xC0, 2, 0x0F}, "PSC_ROM_EC_MUTABLE_DOT_HEADER_CHECK_FAIL"},
        {{0xC0, 2, 0x10}, "PSC_ROM_EC_MUTABLE_DOT_INTEGRITY_FAIL"},
        {{0xC0, 2, 0x11}, "PSC_ROM_EC_MUTABLE_DOT_SANITY_FAIL"},
        {{0xC0, 2, 0x12}, "PSC_ROM_EC_VOLATILE_DOT_PARITY_FAIL"},
        {{0xC0, 2, 0x13}, "PSC_ROM_EC_VOLATILE_DOT_HEADER_CHECK_FAIL"},
        {{0xC0, 2, 0x14}, "PSC_ROM_EC_VOLATILE_DOT_SANITY_FAIL"},
        {{0xC0, 2, 0x15}, "PSC_ROM_EC_CALIPTRA_ACK_FAIL"},
        {{0xC0, 2, 0x16}, "PSC_ROM_EC_FMC_CSH_SANITY_FAIL"},
        {{0xC0, 2, 0x17}, "PSC_ROM_EC_FMC_CSH_AUTHZ1_FAIL"},
        {{0xC0, 2, 0x18}, "PSC_ROM_EC_FMC_CSH_AUTHZ2_FAIL"},
        {{0xC0, 2, 0x19}, "PSC_ROM_EC_FMC_CSH_BIN_LIST_INTEGRITY_FAIL"},
        {{0xC0, 2, 0x1A}, "PSC_ROM_EC_FMC_IMAGE_SANITY_FAIL"},
        {{0xC0, 2, 0x1B}, "PSC_ROM_EC_FMC_IMAGE_INTEGRITY_FAIL"},
        {{0xC0, 2, 0x1C}, "PSC_ROM_EC_FMC_IMAGE_DECRYPTION_FAIL"},
        {{0xC0, 2, 0x1D}, "PSC_ROM_EC_FMC_IMAGE_OEM_RATCHET_FAIL"},
        {{0xC0, 2, 0x1E}, "PSC_ROM_EC_FMC_IMAGE_MUT_DOT_SVN_FUSE_RATCHET_FAIL"},
        {{0xC0, 2, 0x1F}, "PSC_ROM_EC_FMC_IMAGE_MUT_DOT_SVN_CSH_RATCHET_FAIL"},
        {{0xC0, 2, 0x20}, "PSC_ROM_EC_FMC_IMAGE_VOL_DOT_SVN_FUSE_RATCHET_FAIL"},
        {{0xC0, 2, 0x21}, "PSC_ROM_EC_FMC_IMAGE_VOL_DOT_SVN_CSH_RATCHET_FAIL"},
        {{0xC0, 2, 0x22}, "PSC_ROM_EC_FMC_IMAGE_VOL_DOT_CSH_RATCHET_FAIL"},
        {{0xC1, 1, 0x03}, "PSC_FMC_PC_NVDLINK_LS_LINK_UP"},
        {{0xC1, 2, 0x01}, "PSC_FMC_EC_FUSE_CRC_FAILED"},
        {{0xC1, 2, 0x02}, "PSC_FMC_EC_NVDLINK_LS_LINK_FAILED"},
        {{0xC1, 2, 0x03}, "PSC_FMC_EC_BOOT_CHAIN_LEDGER_INVALID"},
        {{0xC1, 2, 0x04}, "PSC_FMC_EC_BOOT_CHAIN_LEDGER_NOT_BOOTABLE"},
        {{0xC1, 2, 0x05}, "PSC_FMC_EC_BOOT_CHAIN_LEDGER_MISMATCH"},
        {{0xC1, 2, 0x06}, "PSC_FMC_EC_DEBUG_TOKEN_SANITY_FAIL"},
        {{0xC1, 2, 0x07}, "PSC_FMC_EC_DEBUG_TOKEN_AUTHENTICATION_FAIL"},
        {{0xC1, 2, 0x08}, "PSC_FMC_EC_SANITY_FAILED"},
        {{0xC1, 2, 0x0A}, "PSC_FMC_EC_STAGE2_AUTHENTICATION_FAILED"},
        {{0xC1, 2, 0x0B}, "PSC_FMC_SVN_CHECK_FAILED"},
        {{0xC1, 2, 0x0C}, "PSC_FMC_EC_HALT_DISABLED_SOCKET"},
        {{0xC3, 2, 0x02}, "MB1_EC_NVDLINK_HS_TRAIN_FAILED"},
        {{0xC3, 2, 0x03}, "MB1_EC_NVDLINK_LLI_INIT_FAILED"},
        {{0xC3, 2, 0x05}, "MB1_EC_CARVEOUT_ALLOC_FAILED"},
        {{0xC3, 2, 0x06}, "MB1_EC_SPD_READ_FAILED"},
        {{0xC3, 2, 0x07}, "MB1_EC_MEMORY_HETEROGENOUS"},
        {{0xC3, 2, 0x08}, "MB1_EC_MEMORY_UNSUPPORTED"},
        {{0xC3, 2, 0x09}, "MB1_EC_DRAM_POPULATION_UNSUPPORTED"},
        {{0xC3, 2, 0x0A}, "MB1_EC_DRAM_INIT_FAILED"},
        {{0xC3, 2, 0x0B}, "MB1_EC_ALIAS_CHECK_FAILED"},
        {{0xC3, 2, 0x0C}, "MB1_EC_DRAM_REGISTER_CHECK_FAILED"},
        {{0xC3, 2, 0x0D}, "MB1_EC_CHANNEL_LOW_COUNT"},
        {{0xC3, 2, 0x0E}, "MB1_EC_UNCORRECTED_ERROR_OVERFLOW"},
        {{0xC3, 2, 0x0F}, "MB1_EC_CHANNEL_RETIRED_TRAINING_BOOT"},
        {{0xC3, 2, 0x11}, "MB1_EC_CHANNEL_RETIRED_UNCORRECTED_ERROR_OVERFLOW"},
        {{0xC3, 2, 0x12}, "MB1_EC_CHANNEL_RETIRED_ALIAS_CHECK"},
        {{0xC3, 2, 0x13}, "MB1_EC_SEGMENT_CTRL_TLV_SANITY_FAILED"},
        {{0xC3, 2, 0x14}, "MB1_EC_NVLINK_C2C_TRAIN_FAILED"},
        {{0xC3, 2, 0x15}, "MB1_EC_NVCLINK_TRAIN_FAILED"},
        {{0xC3, 2, 0x16}, "MB1_EC_DRAM_ECC_FAILED"},
        {{0xC9, 2, 0x00}, "UEFI_EC_NO_SMBIOS_TABLE"},
        {{0xC9, 2, 0x01}, "UEFI_EC_SMBIOS_TRANSFER_FAILED"},
        {{0xC9, 2, 0x02}, "UEFI_EC_M2_NOT_DETECTED"},
        {{0xC9, 2, 0x03}, "UEFI_EC_M2_NO_EFI_PARTITION"},
        {{0xC9, 2, 0x04}, "UEFI_EC_M2_PARTITION_NOT_FAT"},
        {{0xC9, 2, 0x05}, "UEFI_EC_M2_NOT_NVME"},
        {{0xC9, 2, 0x06}, "UEFI_EC_REDFISH_MAC_INVALID"},
        {{0xC9, 2, 0x07}, "UEFI_EC_REDFISH_HOST_IP4_INVALID"},
        {{0xC9, 2, 0x08}, "UEFI_EC_REDFISH_HOST_IP4_SUBNET_MASK_INVALID"},
        {{0xC9, 2, 0x09}, "UEFI_EC_REDFISH_SERVICE_IP4_INVALID"},
        {{0xC9, 2, 0x0A}, "UEFI_EC_REDFISH_SERVICE_IP4_SUBNET_MASK_INVALID"},
        {{0xC9, 2, 0x0B}, "UEFI_EC_REDFISH_HOST_IP6_INVALID"},
        {{0xC9, 2, 0x0C}, "UEFI_EC_REDFISH_HOST_IP6_SUBNET_MASK_INVALID"},
        {{0xC9, 2, 0x0D}, "UEFI_EC_REDFISH_SERVICE_IP6_INVALID"},
        {{0xC9, 2, 0x0E}, "UEFI_EC_REDFISH_SERVICE_IP6_SUBNET_MASK_INVALID"},
        {{0xC9, 2, 0x0F}, "UEFI_EC_REDFISH_BOOTSTRAP_CREDENTIAL_FAILED"},
        {{0xC9, 2, 0x10}, "UEFI_EC_REDFISH_CONFIG_CHANGED_AND_REBOOT"},
        {{0xC9, 2, 0x11}, "UEFI_EC_TPM_INACCESSIBLE"},
        {{0xC9, 2, 0x12}, "UEFI_EC_TPM_NOT_INITIALIZED"},
        {{0xC9, 2, 0x13}, "UEFI_EC_TPM_PCR_BANK_NOT_SUPPORTED"},
        {{0xC9, 2, 0x14}, "UEFI_EC_TPM_SELF_TEST_FAILED"},
        {{0xC9, 2, 0x15}, "UEFI_EC_TPM_PPI_EXECUTE"},
        {{0xC9, 2, 0x16}, "UEFI_EC_TPM_CLEAR_FAILED"},
        {{0xC9, 2, 0x17}, "UEFI_EC_SECURE_BOOT_FAILED"},
        {{0xC9, 2, 0x18}, "UEFI_EC_C2C_INIT_FAILED"},
        {{0xD4, 2, 0x01}, "NVLINK_C2C0_EC_TRAINING_FAILED"},
        {{0xD5, 2, 0x01}, "NVLINK_C2C1_EC_TRAINING_FAILED"},
        {{0xD6, 2, 0x01}, "NVCLINK0_EC_TRAINING_FAILED"},
        {{0xD7, 2, 0x01}, "NVCLINK1_EC_TRAINING_FAILED"},
        {{0xD8, 2, 0x01}, "NVCLINK2_EC_TRAINING_FAILED"},
        {{0xD9, 2, 0x01}, "NVCLINK3_EC_TRAINING_FAILED"},
        {{0xDA, 2, 0x01}, "NVCLINK4_EC_TRAINING_FAILED"},
        {{0xDC, 2, 0x01}, "NVDLINK_S0_EC_TRAINING_FAILED"},
        {{0xDD, 2, 0x01}, "NVDLINK_S1_EC_TRAINING_FAILED"},
        {{0xDE, 2, 0x01}, "NVDLINK_C0_EC_TRAINING_FAILED"},
        {{0xDF, 2, 0x01}, "NVDLINK_C1_EC_TRAINING_FAILED"},
    };
    std::sort(v.begin(), v.end());
    return v;
}();

using InstanceKey = std::pair<std::string_view, uint8_t>;
using InstanceEntry = std::pair<InstanceKey, std::string_view>;
using InstData = std::pair<uint8_t, std::string_view>;

static constexpr std::array<InstData, 36> binaryInstances = {{
    {0x00, "L1PT_COPY_1"},
    {0x01, "L1PT_COPY_2"},
    {0x02, "L2PT"},
    {0x03, "PSC_BCT"},
    {0x04, "CALIPTRA_FMC"},
    {0x05, "OOBHUB"},
    {0x06, "RAS_FW"},
    {0x07, "RAS_BCT"},
    {0x08, "MB1"},
    {0x09, "MB1_BCT"},
    {0x0A, "NVDLINK"},
    {0x0B, "NVLINK_C2C"},
    {0x0C, "NVCLINK"},
    {0x0D, "CMET_COPY_1"},
    {0x0E, "CMET_COPY_2"},
    {0x0F, "MEM_BCT"},
    {0x10, "MSEQ_FW"},
    {0x11, "PXIR_FW"},
    {0x12, "MB2"},
    {0x13, "MB2_BCT"},
    {0x14, "ATF_FP"},
    {0x15, "BPMP_FW"},
    {0x16, "BPMP_FW_DTB"},
    {0x17, "HAFNIUM_FP"},
    {0x18, "SECURE_PARTITIONS_FP"},
    {0x19, "RMM"},
    {0x1A, "UEFI"},
    {0x1B, "PSC_RT_T"},
    {0x1C, "PSC_RT_D"},
    {0x1D, "BPMP_IST"},
    {0x1E, "BPMP_ICT"},
    {0x1F, "BPMP_APPLET"},
    {0x20, "BPMP_MEM_DTB"},
    {0x21, "DEBUG_TOKEN"},
    {0x22, "BPMP_DIAG_FW"},
    {0x23, "CPU_DIAG_FW"},
}};

// PSC_ROM reset-source instance set (56 entries).
static constexpr std::array<InstData, 56> romResetInstances = {{
    {0x00, "SYS_RESET_N"},
    {0x01, "CSDC_RTC_XTAL"},
    {0x02, "VREFRO_POWER_BAD"},
    {0x03, "FMON_32K"},
    {0x04, "FMON_OSC"},
    {0x05, "POD_RTC"},
    {0x06, "POD_IO"},
    {0x07, "POD_PLUS_IO_SPLL"},
    {0x08, "POD_PLUS_IO_VMON"},
    {0x09, "POD_PLUS_SOC"},
    {0x0A, "VMON_PLUS_UV"},
    {0x0B, "VMON_PLUS_OV"},
    {0x0C, "FUSECRC_FAULT"},
    {0x0D, "OSC_FAULT"},
    {0x0E, "BPMP_BOOT_FAULT"},
    {0x0F, "SCPM_BPMP_CORE_CLK"},
    {0x10, "SCPM_PSC_SE_CLK"},
    {0x11, "VMON_SOC_MIN"},
    {0x12, "VMON_SOC_MAX"},
    {0x13, "NVJTAG_SEL_MONITOR"},
    {0x14, "L0_RST_REQ_N"},
    {0x15, "NV_THERM_FAULT"},
    {0x16, "PSC_SW"},
    {0x17, "POD_NVDLINK_0"},
    {0x18, "POD_NVDLINK_1"},
    {0x19, "BPMP_FMON"},
    {0x1A, "FMON_SPLL_OUT"},
    {0x1B, "L1_RST_REQ_N"},
    {0x1C, "OCP_RECOVERY"},
    {0x1D, "AO_WDT_POR"},
    {0x1E, "BPMP_WDT_POR"},
    {0x1F, "RAS_WDT_POR"},
    {0x20, "TOP_0_WDT_POR"},
    {0x21, "TOP_1_WDT_POR"},
    {0x22, "TOP_2_WDT_POR"},
    {0x23, "PSC_WDT_POR"},
    {0x24, "OOBHUB_WDT_POR"},
    {0x25, "MSS_SEQ_WDT_POR"},
    {0x26, "SW_MAIN"},
    {0x27, "L0L1_RST_OUT_N"},
    {0x28, "HSM"},
    {0x29, "CSITE_SW"},
    {0x2A, "AO_WDT_DBG"},
    {0x2B, "BPMP_WDT_DBG"},
    {0x2C, "RAS_WDT_DBG"},
    {0x2D, "TOP_0_WDT_DBG"},
    {0x2E, "TOP_1_WDT_DBG"},
    {0x2F, "TOP_2_WDT_DBG"},
    {0x30, "PSC_WDT_DBG"},
    {0x31, "TSC_0_WDT_DBG"},
    {0x32, "TSC_1_WDT_DBG"},
    {0x33, "OOBHUB_WDT_DBG"},
    {0x34, "MSS_SEQ_WDT_DBG"},
    {0x35, "L2_RST_REQ_N"},
    {0x36, "L2_RST_OUT_N"},
    {0x37, "SC7"},
}};

// PSC_FMC reset-source instance set (31 entries).
static constexpr std::array<InstData, 31> fmcResetInstances = {{
    {0x00, "SYS_RESET_N"},
    {0x01, "FUSECRC_FAULT"},
    {0x02, "POD_RTC"},
    {0x03, "NVJTAG_SEL_MONITOR"},
    {0x04, "L0_RST_REQ_N_SYS"},
    {0x05, "L0_RST_REQ_N_M0"},
    {0x06, "L0_RST_REQ_N_M1"},
    {0x07, "L0_RST_REQ_N_M2"},
    {0x08, "L0_RST_REQ_N_M3"},
    {0x09, "NV_THERM_FAULT"},
    {0x0A, "POD_NVDLINK_0"},
    {0x0B, "POD_NVDLINK_1"},
    {0x0C, "POD_NVLINK_C2C_0"},
    {0x0D, "POD_NVLINK_C2C_1"},
    {0x0E, "VMON_CPU_MIN"},
    {0x0F, "VMON_CPU_MAX"},
    {0x10, "FMON_SPLL_OUT"},
    {0x11, "L1_RST_REQ_N_SYS"},
    {0x12, "L1_RST_REQ_N_M0"},
    {0x13, "L1_RST_REQ_N_M1"},
    {0x14, "L1_RST_REQ_N_M2"},
    {0x15, "L1_RST_REQ_N_M3"},
    {0x16, "CSITE_SW"},
    {0x17, "TSC_0_WDT_DBG"},
    {0x18, "TSC_1_WDT_DBG"},
    {0x19, "L2_RST_REQ_N_SYS"},
    {0x1A, "L2_RST_REQ_N_M0"},
    {0x1B, "L2_RST_REQ_N_M1"},
    {0x1C, "L2_RST_REQ_N_M2"},
    {0x1D, "L2_RST_REQ_N_M3"},
    {0x1E, "SC7"},
}};

// Debug-token instance set (5 entries).
static constexpr std::array<InstData, 5> debugTokenInstances = {{
    {0x00, "FW_LIFECYCLE"},
    {0x01, "ARM_JTAG"},
    {0x02, "NV_JTAG"},
    {0x03, "DIAG_BOOT"},
    {0x04, "BPMP_FW_DEBUG_FS"},
}};

static const std::vector<InstanceEntry> sipInstanceNames = []() {
    std::vector<InstanceEntry> v;
    v.reserve(450);

    auto append = [&v](std::string_view opName,
                       std::span<const InstData> data) {
        for (const auto& [inst, name] : data)
            v.push_back({{opName, inst}, name});
    };

    append("PSC_ROM_PC_I2C_EXT_MSG_INIT", romResetInstances);
    append("PSC_ROM_EC_I2C_EXT_MSG_FAIL", romResetInstances);
    append("PSC_ROM_PC_USB_EXT_MSG_INIT", romResetInstances);
    append("PSC_ROM_EC_USB_EXT_MSG_FAIL", romResetInstances);

    static constexpr std::array<InstData, 2> fmcSections = {{
        {0x00, "PSC_FMC_TEXT"},
        {0x01, "PSC_FMC_DATA"},
    }};
    append("PSC_ROM_EC_FMC_IMAGE_SANITY_FAIL", fmcSections);
    append("PSC_ROM_EC_FMC_IMAGE_INTEGRITY_FAIL", fmcSections);
    append("PSC_ROM_EC_FMC_IMAGE_DECRYPTION_FAIL", fmcSections);
    append("PSC_ROM_EC_FMC_IMAGE_OEM_RATCHET_FAIL", fmcSections);
    append("PSC_ROM_EC_FMC_IMAGE_MUT_DOT_SVN_FUSE_RATCHET_FAIL", fmcSections);
    append("PSC_ROM_EC_FMC_IMAGE_MUT_DOT_SVN_CSH_RATCHET_FAIL", fmcSections);
    append("PSC_ROM_EC_FMC_IMAGE_VOL_DOT_SVN_FUSE_RATCHET_FAIL", fmcSections);
    append("PSC_ROM_EC_FMC_IMAGE_VOL_DOT_SVN_CSH_RATCHET_FAIL", fmcSections);
    append("PSC_ROM_EC_FMC_IMAGE_VOL_DOT_CSH_RATCHET_FAIL", fmcSections);

    append("PSC_FMC_PC_NVDLINK_LS_LINK_UP", fmcResetInstances);
    append("PSC_FMC_EC_DEBUG_TOKEN_SANITY_FAIL", debugTokenInstances);
    append("PSC_FMC_EC_DEBUG_TOKEN_AUTHENTICATION_FAIL", debugTokenInstances);

    append("PSC_FMC_EC_SANITY_FAILED", binaryInstances);
    append("PSC_FMC_EC_STAGE2_AUTHENTICATION_FAILED", binaryInstances);
    append("PSC_FMC_SVN_CHECK_FAILED", binaryInstances);

    static constexpr std::array<InstData, 3> fuseCrcInst = {{
        {0x00, "SYSTEM Dielet"},
        {0x01, "Core Dielet"},
        {0x02, "Memory Dielet"},
    }};
    append("PSC_FMC_EC_FUSE_CRC_FAILED", fuseCrcInst);

    static constexpr std::array<InstData, 34> carveoutInst = {{
        {0x01, "CARVEOUT_ROOT_RMM"},
        {0x02, "CARVEOUT_CSWP"},
        {0x03, "CARVEOUT_RAS_APEI"},
        {0x05, "CARVEOUT_BPMP_RAS"},
        {0x06, "CARVEOUT_BPMP"},
        {0x07, "CARVEOUT_BPMP_ROOT"},
        {0x08, "CARVEOUT_BPMPIST_PARAMS"},
        {0x09, "CARVEOUT_MSEQ_DATA"},
        {0x0A, "CARVEOUT_EMC_TRAINING_R0"},
        {0x0B, "CARVEOUT_EMC_TRAINING_R1"},
        {0x0C, "CARVEOUT_MSEQP"},
        {0x0D, "CARVEOUT_MSEQ_HSS"},
        {0x0F, "CARVEOUT_ETR"},
        {0x10, "CARVEOUT_PSC"},
        {0x11, "CARVEOUT_PSC_CPUTZ"},
        {0x12, "CARVEOUT_LFA_STAGING_NVAUXP"},
        {0x13, "CARVEOUT_LFA_STAGING_CCPLEX"},
        {0x14, "CARVEOUT_PSC_SYSRAM"},
        {0x15, "CARVEOUT_CMET"},
        {0x21, "CARVEOUT_ROOT_SRAM"},
        {0x22, "CARVEOUT_RAS_UEFIMM"},
        {0x23, "CARVEOUT_RMM"},
        {0x24, "CARVEOUT_CCPLEX_INTERWORLD_SHMEM"},
        {0x25, "CARVEOUT_ROOT_RAS"},
        {0x26, "CARVEOUT_TZDRAM"},
        {0x27, "CARVEOUT_BPMP_CPUNS"},
        {0x28, "CARVEOUT_RAS"},
        {0x29, "CARVEOUT_PSC_ROOT"},
        {0x2A, "CARVEOUT_L1L2_PT"},
        {0x2B, "CARVEOUT_VPR"},
        {0x2C, "CARVEOUT_UEFI"},
        {0x2D, "CARVEOUT_PROFILING"},
        {0x2E, "CARVEOUT_OS"},
        {0x2F, "CARVEOUT_GR"},
        // Note: 0x30–0x33 entries below are appended individually
    }};
    append("MB1_EC_CARVEOUT_ALLOC_FAILED", carveoutInst);
    // Remaining carveout entries (index gap after 0x2F)
    static constexpr std::array<InstData, 4> carveoutInst2 = {{
        {0x30, "CARVEOUT_CCPLEX_LA_BUFFERS"},
        {0x31, "CARVEOUT_GPT"},
        {0x32, "CARVEOUT_IST_USB"},
        {0x33, "CARVEOUT_BPMP_IST"},
    }};
    append("MB1_EC_CARVEOUT_ALLOC_FAILED", carveoutInst2);

    std::sort(v.begin(), v.end());
    return v;
}();

uint32_t postcodeToUint32(const std::vector<uint8_t>& code)
{
    return (static_cast<uint32_t>(code[0]) << 24) |
           (static_cast<uint32_t>(code[1]) << 16) |
           (static_cast<uint32_t>(code[2]) << 8) |
           (static_cast<uint32_t>(code[3]));
}

uint8_t extractStatusType(uint32_t postcode)
{
    return static_cast<uint8_t>(
        (postcode & statusCodeTypeMask) >> statusCodeTypeShift);
}

uint8_t extractClass(uint32_t postcode)
{
    return static_cast<uint8_t>(
        (postcode & statusCodeClassMask) >> statusCodeClassShift);
}

uint8_t extractSubclass(uint32_t postcode)
{
    return static_cast<uint8_t>(
        (postcode & statusCodeSubclassMask) >> statusCodeSubclassShift);
}

uint16_t extractInstance(uint32_t postcode)
{
    return static_cast<uint16_t>(
        (postcode & statusCodeInstanceMask) >> statusCodeInstanceShift);
}

uint8_t extractOpcode(uint32_t postcode)
{
    return static_cast<uint8_t>(postcode & statusCodeOpcodeMask);
}

std::string_view getFirmwareName(uint8_t subclass)
{
    if (subclass == efiSubclassRom)
    {
        return "ROM";
    }
    if (subclass == efiSubclassPscbl)
    {
        return "PSCBL";
    }
    if (subclass >= efiSubclassSipMin && subclass <= efiSubclassSipMax)
    {
        return "SiP-Range";
    }
    return "Unknown";
}

static std::string_view getSipSubclassName(uint8_t subclass)
{
    if (subclass >= efiSubclassSipMin && subclass <= efiSubclassSipMax)
    {
        return sipSubclassNames[subclass - efiSubclassSipMin];
    }
    return "Unknown";
}

std::optional<int> getPackageNumber(uint8_t classField)
{
    if (classField >= efiClassSipMin && classField <= efiClassSipMax)
    {
        return classField & 0x7;
    }
    return std::nullopt;
}

static std::optional<std::string_view> getSipOperationName(
    uint8_t subclass, uint8_t type, uint8_t opcode)
{
    SipOpKey key{subclass, type, opcode};
    auto it = std::lower_bound(
        sipOperationNames.begin(), sipOperationNames.end(), key,
        [](const SipOpEntry& e, const SipOpKey& k) { return e.first < k; });
    if (it != sipOperationNames.end() && it->first == key)
    {
        return it->second;
    }
    return std::nullopt;
}

static std::optional<std::string_view> getSipInstanceName(
    std::string_view opName, uint8_t instance)
{
    InstanceKey key{opName, instance};
    auto it =
        std::lower_bound(sipInstanceNames.begin(), sipInstanceNames.end(), key,
                         [](const InstanceEntry& e, const InstanceKey& k) {
                             return e.first < k;
                         });
    if (it != sipInstanceNames.end() && it->first == key)
    {
        return it->second;
    }
    return std::nullopt;
}

// Binary search a sorted instance table by instance ID. Works for both
// contiguous and sparse tables since lookup is by key, not by index.
std::optional<std::string_view> findInstanceName(std::span<const InstData> arr,
                                                 uint16_t instance)
{
    auto it = std::lower_bound(
        arr.begin(), arr.end(), instance,
        [](const InstData& e, uint16_t k) { return e.first < k; });
    if (it != arr.end() && it->first == instance)
    {
        return it->second;
    }
    return std::nullopt;
}

std::optional<std::string_view> getResetReasonName(
    uint8_t subclass, uint8_t statusType, uint8_t opcode, uint16_t instance)
{
    if (statusType != statusTypeProgress)
    {
        return std::nullopt;
    }

    if (subclass == efiSubclassRom &&
        (opcode == opcodeRomI2cMsgInit || opcode == opcodeRomUsbMsgInit))
    {
        return findInstanceName(romResetInstances, instance);
    }

    if (subclass == efiSubclassPscbl && opcode == opcodeFmcNvdlinkLsLinkUp)
    {
        return findInstanceName(fmcResetInstances, instance);
    }

    return std::nullopt;
}

void logNvidiaPostCode(sdbusplus::bus_t& bus, const std::vector<uint8_t>& code,
                       const std::optional<std::string>& resolution,
                       const std::optional<std::string>& description)
{
    if (code.size() != 4)
    {
        return;
    }

    uint32_t postcodeValue = postcodeToUint32(code);
    uint8_t statusType = extractStatusType(postcodeValue);

    if (statusType != statusTypeProgress && statusType != statusTypeError)
    {
        return;
    }

    uint8_t classField = extractClass(postcodeValue);
    uint8_t subclass = extractSubclass(postcodeValue);
    uint8_t opcode = extractOpcode(postcodeValue);
    uint8_t instance = static_cast<uint8_t>(extractInstance(postcodeValue));
    auto cpuNum = getPackageNumber(classField);

    auto opName = getSipOperationName(subclass, statusType, opcode);

    // Progress codes without a known operation name carry no additional
    // context.
    if (statusType == statusTypeProgress && !opName)
    {
        return;
    }

    std::string logMsg;
    logMsg.reserve(256);

    if (description && !description->empty())
    {
        logMsg = *description;
    }
    if (cpuNum)
    {
        if (!logMsg.empty())
        {
            logMsg += ' ';
        }
        std::format_to(std::back_inserter(logMsg), "on CPU {}", *cpuNum);
    }
    if (!logMsg.empty())
    {
        logMsg += ", ";
    }
    std::format_to(std::back_inserter(logMsg), "reported by {}",
                   getSipSubclassName(subclass));

    if (opName)
    {
        if (auto r = getResetReasonName(subclass, statusType, opcode, instance))
        {
            std::format_to(std::back_inserter(logMsg), ", reset reason {}", *r);
        }
        else if (auto i = getSipInstanceName(*opName, instance))
        {
            std::format_to(std::back_inserter(logMsg), ", instance {}", *i);
        }
        std::format_to(std::back_inserter(logMsg), " ({}, Code 0x{:08X}).",
                       *opName, postcodeValue);
    }
    else
    {
        std::format_to(std::back_inserter(logMsg), " (Code 0x{:08X}).",
                       postcodeValue);
    }

    std::map<std::string, std::string> additionalData;
    if (resolution && !resolution->empty())
    {
        additionalData.emplace(resolutionKey, *resolution);
    }

    if (statusType == statusTypeError)
    {
        additionalData.emplace("REDFISH_MESSAGE_ID",
                               "Platform.1.0.1.PlatformError");
        additionalData.emplace("NVIDIA_POST_CODE",
                               std::format("0x{:08X}", postcodeValue));
        additionalData.emplace("NVIDIA_CPU_NUM",
                               cpuNum ? std::to_string(*cpuNum)
                                      : std::string("Unknown"));
        additionalData.emplace("NVIDIA_FIRMWARE",
                               std::string(getSipSubclassName(subclass)));
        additionalData.emplace("NVIDIA_INSTANCE",
                               std::to_string(static_cast<unsigned>(instance)));
        additionalData.emplace(
            "NVIDIA_OPCODE",
            std::format("0x{:02X}", static_cast<unsigned>(opcode)));
    }

    try
    {
        auto method = bus.new_method_call(loggingService, loggingObject,
                                          loggingInterface, "Create");
        method.append(logMsg);
        method.append(std::string(statusType == statusTypeProgress
                                      ? informationalSeverity
                                      : criticalSeverity));
        method.append(additionalData);
        bus.call_noreply(method);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Failed to log NVIDIA POST code: " << e.what()
                  << std::endl;
    }
}
