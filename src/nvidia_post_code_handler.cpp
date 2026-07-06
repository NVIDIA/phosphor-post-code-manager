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
constexpr uint8_t piClassMax = 0x03;

// Opcode constants for reset source operations
// PSC_ROM (subclass 0xC0): opcodes 0x01 and 0x03 carry reset source in instance
constexpr uint8_t opcodeRomI2cMsgInit = 0x01;
constexpr uint8_t opcodeRomUsbMsgInit = 0x03;
// PSC_FMC (subclass 0xC1): opcode 0x03 carries reset source in instance
constexpr uint8_t opcodeFmcNvdlinkLsLinkUp = 0x03;

constexpr auto compareByFirst = [](const auto& entry, const auto& key) {
    return entry.first < key;
};

static constexpr std::array<std::string_view, 32> sipSubclassNames = {
    "PSCROM",     // 0xC0
    "PSCFMC",     // 0xC1
    "PSCRT",      // 0xC2
    "MB1",        // 0xC3
    "BPMP_FW",    // 0xC4
    "MB2",        // 0xC5
    "ATF_BL31",   // 0xC6
    "RMM",        // 0xC7
    "HAFNIUM",    // 0xC8
    "UEFI",       // 0xC9
    "UEFI_STMM",  // 0xCA
    "OOBHUB_FW",  // 0xCB
    "RAS_FW",     // 0xCC
    "MSEQ_FW",    // 0xCD
    "PCORE0_FW",  // 0xCE
    "PCORE1_FW",  // 0xCF
    "PCORE2_FW",  // 0xD0
    "PCORE3_FW",  // 0xD1
    "PCORE4_FW",  // 0xD2
    "PCORE5_FW",  // 0xD3
    "C2C_GRS0",   // 0xD4
    "C2C_GRS1",   // 0xD5
    "C2C_UPHY0",  // 0xD6
    "C2C_UPHY1",  // 0xD7
    "C2C_UPHY2",  // 0xD8
    "C2C_UPHY3",  // 0xD9
    "C2C_UPHY4",  // 0xDA
    "C2C_UPHY5",  // 0xDB
    "C2C_LPI_S0", // 0xDC
    "C2C_LPI_S1", // 0xDD
    "C2C_LPI_C0", // 0xDE
    "C2C_LPI_C1", // 0xDF
};

using SipOpKey = std::tuple<uint8_t, uint8_t, uint8_t>;
using SipOpEntry = std::pair<SipOpKey, std::string_view>;
static const std::vector<SipOpEntry> sipOperationNames = []() {
    std::vector<SipOpEntry> v = {
        {{0xC0, 1, 0x01}, "PSC_ROM_PC_I2C_EXT_MSG_INIT"},
        {{0xC0, 1, 0x02}, "PSC_ROM_PC_BOOT_MODE_SEL_DONE"},
        {{0xC0, 1, 0x03}, "PSC_ROM_PC_USB_EXT_MSG_INIT"},
        {{0xC0, 1, 0x04}, "PSC_ROM_PC_QSPI0_DEV_INIT"},
        {{0xC0, 1, 0x05}, "PSC_ROM_PC_USB2_DEV_INIT"},
        {{0xC0, 1, 0x06}, "PSC_ROM_PC_OCPRC_DEV_INIT"},
        {{0xC0, 1, 0x07}, "PSC_ROM_PC_BOOT_CHAIN_SEL"},
        {{0xC0, 1, 0x08}, "PSC_ROM_PC_BOOT_IMAGE_LOAD_DONE"},
        {{0xC0, 1, 0x09}, "PSC_ROM_PC_DOT_S2A_VALIDATION_DONE"},
        {{0xC0, 1, 0x0A}, "PSC_ROM_PC_FMC_VALIDATION_DONE"},
        {{0xC0, 1, 0x0B}, "PSC_ROM_PC_ROM_EXIT"},
        {{0xC0, 1, 0x0C}, "PSC_ROM_PC_DOT_RECOVERY"},
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
        {{0xC1, 1, 0x01}, "PSC_FMC_PC_INIT"},
        {{0xC1, 1, 0x02}, "PSC_FMC_PC_BOOT_MODE"},
        {{0xC1, 1, 0x03}, "PSC_FMC_PC_LPI_LS_LINK_UP"},
        {{0xC1, 1, 0x04}, "PSC_FMC_PC_FW_QSPI_REINIT"},
        {{0xC1, 1, 0x05}, "PSC_FMC_PC_BOOT_CHAIN_LEDGER_SELECT"},
        {{0xC1, 1, 0x06}, "PSC_FMC_PC_DATA_QSPI_INIT"},
        {{0xC1, 1, 0x07}, "PSC_FMC_PC_DEBUG_TOKEN_LOAD"},
        {{0xC1, 1, 0x08}, "PSC_FMC_PC_BINARY_LOAD"},
        {{0xC1, 1, 0x09}, "PSC_FMC_PC_BOOTSTRAP"},
        {{0xC1, 1, 0x0A}, "PSC_FMC_PC_WAIT_FOR_DOT_CAK_STATUS"},
        {{0xC1, 1, 0x0B}, "PSC_FMC_PC_CSA"},
        {{0xC1, 1, 0x0C}, "PSC_FMC_PC_PLDM_T5_READY"},
        {{0xC1, 1, 0x0D}, "PSC_FMC_PC_EARLY_BOOTVARS_LOAD"},
        {{0xC1, 1, 0x0E}, "PSC_FMC_PC_EXIT"},
        {{0xC1, 1, 0x0F}, "PSC_FMC_PC_SS_DETECTION"},
        {{0xC1, 1, 0x10}, "PSC_FMC_PC_SOCKET_ID_PREWAR_DONE"},
        {{0xC1, 1, 0x11}, "PSC_FMC_PC_SOCKET_ID_PREWAR_PREV_APPLIED"},
        {{0xC1, 1, 0x12}, "PSC_FMC_PC_SOCKET_ID_PREWAR_SKIPPED"},
        {{0xC1, 1, 0x13}, "PSC_FMC_PC_SOCKET_ID_WAR_SKIPPED"},
        {{0xC1, 1, 0x14}, "PSC_FMC_PC_SOCKET_ID_WAR_APPLIED_CORRECT"},
        {{0xC1, 1, 0x15}, "PSC_FMC_PC_SOCKET_ID_WAR_SUCCESS"},
        {{0xC1, 2, 0x01}, "PSC_FMC_EC_FUSE_CRC_FAILED"},
        {{0xC1, 2, 0x02}, "PSC_FMC_EC_LPI_LS_LINK_FAILED"},
        {{0xC1, 2, 0x03}, "PSC_FMC_EC_BOOT_CHAIN_LEDGER_INVALID"},
        {{0xC1, 2, 0x04}, "PSC_FMC_EC_BOOT_CHAIN_LEDGER_NOT_BOOTABLE"},
        {{0xC1, 2, 0x05}, "PSC_FMC_EC_BOOT_CHAIN_LEDGER_MISMATCH"},
        {{0xC1, 2, 0x06}, "PSC_FMC_EC_DEBUG_TOKEN_SANITY_FAIL"},
        {{0xC1, 2, 0x07}, "PSC_FMC_EC_DEBUG_TOKEN_AUTHENTICATION_FAIL"},
        {{0xC1, 2, 0x08}, "PSC_FMC_EC_SANITY_FAILED"},
        {{0xC1, 2, 0x09}, "PSC_FMC_EC_STAGE1_AUTHENTICATION_FAILED"},
        {{0xC1, 2, 0x0A}, "PSC_FMC_EC_STAGE2_AUTHENTICATION_FAILED"},
        {{0xC1, 2, 0x0B}, "PSC_FMC_EC_SVN_CHECK_FAILED"},
        {{0xC1, 2, 0x0C}, "PSC_FMC_EC_HALT_DISABLED_SOCKET"},
        {{0xC1, 2, 0x0D}, "PSC_FMC_EC_MEM_FUSE_CRC_FAILED"},
        {{0xC1, 2, 0x0E}, "PSC_FMC_EC_CALIPTRA_MAILBOX_FAILED"},
        {{0xC1, 2, 0x0F}, "PSC_FMC_EC_CSA_FAILED"},
        {{0xC1, 2, 0x10}, "PSC_FMC_EC_DOT_FAILED"},
        {{0xC1, 2, 0x11}, "PSC_FMC_EC_EARLY_BOOTVARS_LOAD_FAILED"},
        {{0xC1, 2, 0x12}, "PSC_FMC_EC_SOCKET_ID_PREWAR_SCB_FAILED"},
        {{0xC1, 2, 0x13}, "PSC_FMC_EC_SOCKET_ID_PREWAR_SDIE_STRAP_FAILED"},
        {{0xC1, 2, 0x14}, "PSC_FMC_EC_SOCKET_ID_WAR_S_C_FAILED"},
        {{0xC1, 2, 0x15}, "PSC_FMC_EC_SOCKET_ID_WAR_S_C_FAILED_AFTER_APPLY"},
        {{0xC2, 1, 0x01}, "PSC_RT_PC_INIT"},
        {{0xC2, 1, 0x02}, "PSC_RT_PC_PLDM_T5_READY"},
        {{0xC3, 1, 0x01}, "MB1_PC_INIT"},
        {{0xC3, 1, 0x03}, "MB1_PC_POWER_PROFILE_SELECT"},
        {{0xC3, 1, 0x04}, "MB1_PC_C2CLPI_HS_TRAIN"},
        {{0xC3, 1, 0x05}, "MB1_PC_C2CLLI_INIT"},
        {{0xC3, 1, 0x06}, "MB1_PC_C2CGRS_START"},
        {{0xC3, 1, 0x07}, "MB1_PC_UPHY_CALIB"},
        {{0xC3, 1, 0x08}, "MB1_PC_C2CUPHY_START"},
        {{0xC3, 1, 0x09}, "MB1_PC_BOOTSTRAP"},
        {{0xC3, 1, 0x0A}, "MB1_PC_SPD_READ"},
        {{0xC3, 1, 0x0B}, "MB1_PC_DRAM_INIT"},
        {{0xC3, 1, 0x0C}, "MB1_PC_PCORE_INIT"},
        {{0xC3, 1, 0x0D}, "MB1_PC_C2CGRS_INIT"},
        {{0xC3, 1, 0x0E}, "MB1_PC_C2CUPHY_INIT"},
        {{0xC3, 1, 0x0F}, "MB1_PC_CSWP_CONFIG"},
        {{0xC3, 1, 0x10}, "MB1_PC_DRAM_ECC_INIT"},
        {{0xC3, 1, 0x11}, "MB1_PC_WAIT_FOR_CSWP_DEBUG"},
        {{0xC3, 1, 0x12}, "MB1_PC_EXIT"},
        {{0xC3, 1, 0x13}, "MB1_PC_UCF_INIT"},
        {{0xC3, 2, 0x01}, "MB1_EC_FUSE_RECORD_INTEGRITY_FAILED"},
        {{0xC3, 2, 0x02}, "MB1_EC_C2CLPI_HS_TRAIN_FAILED"},
        {{0xC3, 2, 0x03}, "MB1_EC_C2CLLI_INIT_FAILED"},
        {{0xC3, 2, 0x04}, "MB1_EC_RAS_POLL_FAILED"},
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
        {{0xC3, 2, 0x0F}, "MB1_EC_CHANNEL_RETIRED_TRAINING"},
        {{0xC3, 2, 0x10}, "MB1_EC_CHANNEL_RETIRED_TRAINING"},
        {{0xC3, 2, 0x11}, "MB1_EC_CHANNEL_RETIRED_UNCORRECTED_ERROR_OVERFLOW"},
        {{0xC3, 2, 0x12}, "MB1_EC_CHANNEL_RETIRED_ALIAS_CHECK"},
        {{0xC3, 2, 0x13}, "MB1_EC_PCORE_TLV_SANITY_FAILED"},
        {{0xC3, 2, 0x14}, "MB1_EC_C2CGRS_TRAIN_FAILED"},
        {{0xC3, 2, 0x15}, "MB1_EC_C2CUPHY_TRAIN_FAILED"},
        {{0xC3, 2, 0x16}, "MB1_EC_DRAM_ECC_FAILED"},
        {{0xC3, 2, 0x17}, "MB1_EC_PLLE_LOCK_FAILED"},
        {{0xC3, 2, 0x18}, "MB1_EC_SEGMENT_CTRL_PXIR_MBOX_FAILED"},
        {{0xC3, 2, 0x19}, "MB1_EC_UCF_ERROR"},
        {{0xC4, 1, 0x00}, "BPMP_FW_PC_LOGGING_INIT"},
        {{0xC4, 1, 0x01}, "BPMP_FW_PC_CLK_INIT"},
        {{0xC4, 1, 0x02}, "BPMP_FW_PC_CLK_CAL"},
        {{0xC4, 1, 0x03}, "BPMP_FW_PC_REGULATOR_INIT"},
        {{0xC4, 1, 0x04}, "BPMP_FW_PC_AVFS_INIT"},
        {{0xC4, 1, 0x05}, "BPMP_FW_PC_IPMU_INIT"},
        {{0xC4, 1, 0x06}, "BPMP_FW_PC_CLK_LATE"},
        {{0xC4, 1, 0x07}, "BPMP_FW_PC_C2C_INIT"},
        {{0xC4, 1, 0x08}, "BPMP_FW_PC_CLK_POST"},
        {{0xC4, 1, 0x09}, "BPMP_FW_PC_SLC_INIT"},
        {{0xC4, 1, 0x0A}, "BPMP_FW_PC_MRQ_AVAILABLE"},
        {{0xC4, 1, 0x3F}, "BPMP_FW_PC_INIT_COMPLETE"},
        {{0xC5, 1, 0x01}, "MB2_PC_INIT"},
        {{0xC5, 1, 0x02}, "MB2_PC_DRAM_ECC_INIT"},
        {{0xC5, 1, 0x03}, "MB2_PC_SET_HW_BREAK_POINT"},
        {{0xC5, 1, 0x04}, "MB2_PC_EXIT"},
        {{0xC6, 1, 0x01}, "BL31_PC_EARLY_PLAT_SETUP_STARTED"},
        {{0xC6, 1, 0x02}, "BL31_PC_EARLY_PLAT_SETUP_COMPLETE"},
        {{0xC6, 1, 0x03}, "BL31_PC_PSC_MAILBOX_INIT_COMPLETE"},
        {{0xC6, 1, 0x04}, "BL31_PC_RME_GPT_MEM_MAP_COMPLETE"},
        {{0xC6, 1, 0x05}, "BL31_PC_EL3_RMM_SHARED_MEM_MAP_COMPLETE"},
        {{0xC6, 1, 0x06}, "BL31_PC_LATE_PLAT_SETUP_COMPLETE"},
        {{0xC6, 1, 0x07}, "BL31_PC_BOOT_COMPLETE"},
        {{0xC6, 1, 0x08}, "BL31_PC_PLAT_PARAMS_PARSING_COMPLETE"},
        {{0xC6, 1, 0x09}, "BL31_PC_RME_NS_DRAM_BANK_MAPPING_COMPLETE"},
        {{0xC6, 2, 0x01}, "BL31_EC_PSC_MAILBOX_UNAVAIL"},
        {{0xC6, 2, 0x02}, "BL31_EC_TEGRA_BPMP_IPC_INIT_FAILED"},
        {{0xC6, 2, 0x03}, "BL31_EC_FDT_CHECK_HEADER_INVALID"},
        {{0xC6, 2, 0x04}, "BL31_EC_BPMP_IVC_BASE_ADDRESS_INVALID"},
        {{0xC6, 2, 0x05}, "BL31_EC_FDT_SOCKET0_PARSE_FAILED"},
        {{0xC6, 2, 0x06}, "BL31_EC_FDT_SOCKET0_NODE_NOT_FOUND"},
        {{0xC6, 2, 0x07}, "BL31_EC_FDT_SOCKET1_PARSE_FAILED"},
        {{0xC6, 2, 0x08}, "BL31_EC_FDT_SOCKET1_NODE_NOT_FOUND"},
        {{0xC6, 2, 0x09}, "BL31_EC_FDT_GPU_NODES_INVALID"},
        {{0xC6, 2, 0x0A}, "BL31_EC_FDT_PCIE_RC_NODES_INVALID"},
        {{0xC6, 2, 0x0B}, "BL31_EC_FDT_BDF_RANGE_NODES_INVALID"},
        {{0xC6, 2, 0x0C}, "BL31_EC_FDT_CXL_T3_MEM_NODES_INVALID"},
        {{0xC6, 2, 0x0D}, "BL31_EC_FDT_CXL_CHBCR_REGS_NODES_INVALID"},
        {{0xC6, 2, 0x0E}, "BL31_EC_FDT_COH_DEV_MEM_NODES_INVALID"},
        {{0xC6, 2, 0x0F}, "BL31_EC_FDT_PCIE_NC_DEV_MEM_NODES_INVALID"},
        {{0xC6, 2, 0x10}, "BL31_EC_FDT_SMMU_NODES_INVALID"},
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
        {{0xCB, 1, 0x01}, "OOBHUB_PC_INIT"},
        {{0xCB, 1, 0x03}, "OOBHUB_PC_MCTP_INIT"},
        {{0xCC, 1, 0x00}, "RAS_FW_PC_MGMT_INIT"},
        {{0xCC, 1, 0x01}, "RAS_FW_PC_MGMT_READY"},
        {{0xCC, 1, 0x02}, "RAS_FW_PC_COMPUTE_DIE_READY"},
        {{0xCC, 1, 0x03}, "RAS_FW_PC_ATF_READY"},
        {{0xCC, 1, 0x04}, "RAS_FW_PC_APEI_DONE"},
        {{0xCD, 1, 0x01}, "MSEQ_FW_PC_INIT"},
        {{0xCD, 1, 0x02}, "MSEQ_FW_PC_MSEQS_BOOTSTRAP"},
        {{0xCD, 1, 0x03}, "MSEQ_FW_PC_BOOT_COMPLETE"},
        {{0xCE, 1, 0x01}, "PCORE0_FW_PC_INIT"},
        {{0xCE, 1, 0x02}, "PCORE0_FW_PC_UPHY_INIT"},
        {{0xCE, 2, 0x01}, "PCORE0_FW_EC_UPHY_INIT_FAILED"},
        {{0xCF, 1, 0x01}, "PCORE1_FW_PC_INIT"},
        {{0xCF, 1, 0x02}, "PCORE1_FW_PC_UPHY_INIT"},
        {{0xCF, 2, 0x01}, "PCORE1_FW_EC_UPHY_INIT_FAILED"},
        {{0xD0, 1, 0x01}, "PCORE2_FW_PC_INIT"},
        {{0xD0, 1, 0x02}, "PCORE2_FW_PC_UPHY_INIT"},
        {{0xD0, 2, 0x01}, "PCORE2_FW_EC_UPHY_INIT_FAILED"},
        {{0xD1, 1, 0x01}, "PCORE3_FW_PC_INIT"},
        {{0xD1, 1, 0x02}, "PCORE3_FW_PC_UPHY_INIT"},
        {{0xD1, 2, 0x01}, "PCORE3_FW_EC_UPHY_INIT_FAILED"},
        {{0xD2, 1, 0x01}, "PCORE4_FW_PC_INIT"},
        {{0xD2, 1, 0x02}, "PCORE4_FW_PC_UPHY_INIT"},
        {{0xD2, 2, 0x01}, "PCORE4_FW_EC_UPHY_INIT_FAILED"},
        {{0xD3, 1, 0x01}, "PCORE5_FW_PC_INIT"},
        {{0xD3, 1, 0x02}, "PCORE5_FW_PC_UPHY_INIT"},
        {{0xD3, 2, 0x01}, "PCORE5_FW_EC_UPHY_INIT_FAILED"},
        {{0xD4, 1, 0x01}, "C2C_GRS0_PC_INIT"},
        {{0xD4, 1, 0x02}, "C2C_GRS0_PC_TRAINING"},
        {{0xD4, 2, 0x01}, "C2C_GRS0_EC_TRAINING_FAILED"},
        {{0xD5, 1, 0x01}, "C2C_GRS1_PC_INIT"},
        {{0xD5, 1, 0x02}, "C2C_GRS1_PC_TRAINING"},
        {{0xD5, 2, 0x01}, "C2C_GRS1_EC_TRAINING_FAILED"},
        {{0xD6, 1, 0x01}, "C2C_UPHY0_PC_INIT"},
        {{0xD6, 1, 0x02}, "C2C_UPHY0_PC_TRAINING"},
        {{0xD6, 2, 0x01}, "C2C_UPHY0_EC_TRAINING_FAILED"},
        {{0xD7, 1, 0x01}, "C2C_UPHY1_PC_INIT"},
        {{0xD7, 1, 0x02}, "C2C_UPHY1_PC_TRAINING"},
        {{0xD7, 2, 0x01}, "C2C_UPHY1_EC_TRAINING_FAILED"},
        {{0xD8, 1, 0x01}, "C2C_UPHY2_PC_INIT"},
        {{0xD8, 1, 0x02}, "C2C_UPHY2_PC_TRAINING"},
        {{0xD8, 2, 0x01}, "C2C_UPHY2_EC_TRAINING_FAILED"},
        {{0xD9, 1, 0x01}, "C2C_UPHY3_PC_INIT"},
        {{0xD9, 1, 0x02}, "C2C_UPHY3_PC_TRAINING"},
        {{0xD9, 2, 0x01}, "C2C_UPHY3_EC_TRAINING_FAILED"},
        {{0xDA, 1, 0x01}, "C2C_UPHY4_PC_INIT"},
        {{0xDA, 1, 0x02}, "C2C_UPHY4_PC_TRAINING"},
        {{0xDA, 2, 0x01}, "C2C_UPHY4_EC_TRAINING_FAILED"},
        {{0xDB, 1, 0x01}, "C2C_UPHY5_PC_INIT"},
        {{0xDB, 1, 0x02}, "C2C_UPHY5_PC_TRAINING"},
        {{0xDB, 2, 0x01}, "C2C_UPHY5_EC_TRAINING_FAILED"},
        {{0xDC, 1, 0x01}, "C2C_LPI_S0_PC_INIT"},
        {{0xDC, 2, 0x01}, "C2C_LPI_S0_EC_TRAINING_FAILED"},
        {{0xDD, 1, 0x01}, "C2C_LPI_S1_PC_INIT"},
        {{0xDD, 2, 0x01}, "C2C_LPI_S1_EC_TRAINING_FAILED"},
        {{0xDE, 1, 0x01}, "C2C_LPI_C0_PC_INIT"},
        {{0xDE, 2, 0x01}, "C2C_LPI_C0_EC_TRAINING_FAILED"},
        {{0xDF, 1, 0x01}, "C2C_LPI_C1_PC_INIT"},
        {{0xDF, 2, 0x01}, "C2C_LPI_C1_EC_TRAINING_FAILED"},
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

    append("PSC_FMC_PC_LPI_LS_LINK_UP", fmcResetInstances);
    append("PSC_FMC_EC_DEBUG_TOKEN_SANITY_FAIL", debugTokenInstances);
    append("PSC_FMC_EC_DEBUG_TOKEN_AUTHENTICATION_FAIL", debugTokenInstances);

    append("PSC_FMC_EC_SANITY_FAILED", binaryInstances);
    append("PSC_FMC_EC_STAGE2_AUTHENTICATION_FAILED", binaryInstances);
    append("PSC_FMC_EC_SVN_CHECK_FAILED", binaryInstances);

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

    static constexpr std::array<InstData, 4> bootModeInst = {{
        {0x01, "COLD_BOOT"},
        {0x02, "RECOVERY"},
        {0x03, "IST"},
        {0x04, "DIAG_BOOT"},
    }};
    append("PSC_FMC_PC_BOOT_MODE", bootModeInst);

    static constexpr std::array<InstData, 14> fmcBootstrapInst = {{
        {0x00, "CALIPTRA_FW"},
        {0x01, "OOBHUB"},
        {0x02, "RAS"},
        {0x03, "MB1"},
        {0x04, "MSEQ"},
        {0x05, "SEGMENT_CTRL_FW_0"},
        {0x06, "SEGMENT_CTRL_FW_1"},
        {0x07, "SEGMENT_CTRL_FW_2"},
        {0x08, "SEGMENT_CTRL_FW_3"},
        {0x09, "SEGMENT_CTRL_FW_4"},
        {0x0A, "SEGMENT_CTRL_FW_5"},
        {0x0B, "MB2"},
        {0x0C, "BPMP_FW"},
        {0x0D, "BOOT_BPMP_IST"},
    }};
    append("PSC_FMC_PC_BOOTSTRAP", fmcBootstrapInst);

    static constexpr std::array<InstData, 1> fmcInitInst = {{
        {0x00, "QUEUE_SIZES_1"},
    }};
    append("PSC_FMC_PC_INIT", fmcInitInst);

    append("PSC_FMC_PC_DEBUG_TOKEN_LOAD", debugTokenInstances);

    static constexpr std::array<InstData, 4> dotCakInst = {{
        {0x00, "CAK_INSTALLED"},
        {0x01, "CAK_SKIPPED"},
        {0x02, "BMC_WAIT"},
        {0x03, "WAIT_RESET"},
    }};
    append("PSC_FMC_PC_WAIT_FOR_DOT_CAK_STATUS", dotCakInst);

    static constexpr std::array<InstData, 2> csaFailInst = {{
        {0x00, "CSA_FAIL_CHAIN_MISMATCH"},
        {0x01, "CSA_FAIL_HASH_MISMATCH"},
    }};
    append("PSC_FMC_EC_CSA_FAILED", csaFailInst);

    append("PSC_FMC_PC_BINARY_LOAD", binaryInstances);
    append("PSC_FMC_EC_STAGE1_AUTHENTICATION_FAILED", binaryInstances);

    static constexpr std::array<InstData, 4> mb1BootstrapInst = {{
        {0x00, "NVDLINK_C0"},
        {0x01, "NVDLINK_C1"},
        {0x02, "NVDLINK_S0"},
        {0x03, "NVDLINK_S1"},
    }};
    append("MB1_PC_BOOTSTRAP", mb1BootstrapInst);

    static constexpr std::array<InstData, 3> cswpConfigInst = {{
        {0x00, "DISABLED"},
        {0x01, "ENABLED_ON_USB2"},
        {0x02, "ENABLED_ON_USB3"},
    }};
    append("MB1_PC_CSWP_CONFIG", cswpConfigInst);

    static constexpr std::array<InstData, 3> mb2BreakpointInst = {{
        {0x00, "ATF"},
        {0x01, "HAFNIUM"},
        {0x02, "UEFI"},
    }};
    append("MB2_PC_SET_HW_BREAK_POINT", mb2BreakpointInst);

    std::sort(v.begin(), v.end());
    return v;
}();

using PiSubKey = std::pair<uint8_t, uint8_t>;
using PiSubEntry = std::pair<PiSubKey, std::string_view>;
static const std::vector<PiSubEntry> piSubclassNames = []() {
    std::vector<PiSubEntry> v = {
        {{0x00, 0x00}, "EFI_COMPUTING_UNIT_UNSPECIFIED"},
        {{0x00, 0x01}, "EFI_COMPUTING_UNIT_HOST_PROCESSOR"},
        {{0x00, 0x02}, "EFI_COMPUTING_UNIT_FIRMWARE_PROCESSOR"},
        {{0x00, 0x03}, "EFI_COMPUTING_UNIT_IO_PROCESSOR"},
        {{0x00, 0x04}, "EFI_COMPUTING_UNIT_CACHE"},
        {{0x00, 0x05}, "EFI_COMPUTING_UNIT_MEMORY"},
        {{0x00, 0x06}, "EFI_COMPUTING_UNIT_CHIPSET"},
        {{0x00, 0x07}, "EFI_COMPUTING_UNIT_MANAGEABILITY"},
        {{0x01, 0x00}, "EFI_PERIPHERAL_UNSPECIFIED"},
        {{0x01, 0x01}, "EFI_PERIPHERAL_KEYBOARD"},
        {{0x01, 0x02}, "EFI_PERIPHERAL_MOUSE"},
        {{0x01, 0x03}, "EFI_PERIPHERAL_LOCAL_CONSOLE"},
        {{0x01, 0x04}, "EFI_PERIPHERAL_REMOTE_CONSOLE"},
        {{0x01, 0x05}, "EFI_PERIPHERAL_SERIAL_PORT"},
        {{0x01, 0x06}, "EFI_PERIPHERAL_PARALLEL_PORT"},
        {{0x01, 0x07}, "EFI_PERIPHERAL_FIXED_MEDIA"},
        {{0x01, 0x08}, "EFI_PERIPHERAL_REMOVABLE_MEDIA"},
        {{0x01, 0x09}, "EFI_PERIPHERAL_AUDIO_INPUT"},
        {{0x01, 0x0A}, "EFI_PERIPHERAL_AUDIO_OUTPUT"},
        {{0x01, 0x0B}, "EFI_PERIPHERAL_LCD_DEVICE"},
        {{0x01, 0x0C}, "EFI_PERIPHERAL_NETWORK"},
        {{0x01, 0x0D}, "EFI_PERIPHERAL_DOCKING"},
        {{0x01, 0x0E}, "EFI_PERIPHERAL_TPM"},
        {{0x02, 0x00}, "EFI_IO_BUS_UNSPECIFIED"},
        {{0x02, 0x01}, "EFI_IO_BUS_PCI"},
        {{0x02, 0x02}, "EFI_IO_BUS_USB"},
        {{0x02, 0x03}, "EFI_IO_BUS_IBA"},
        {{0x02, 0x04}, "EFI_IO_BUS_AGP"},
        {{0x02, 0x05}, "EFI_IO_BUS_PC_CARD"},
        {{0x02, 0x06}, "EFI_IO_BUS_LPC"},
        {{0x02, 0x07}, "EFI_IO_BUS_SCSI"},
        {{0x02, 0x08}, "EFI_IO_BUS_ATA_ATAPI"},
        {{0x02, 0x09}, "EFI_IO_BUS_FC"},
        {{0x02, 0x0A}, "EFI_IO_BUS_IP_NETWORK"},
        {{0x02, 0x0B}, "EFI_IO_BUS_SMBUS"},
        {{0x02, 0x0C}, "EFI_IO_BUS_I2C"},
        {{0x03, 0x00}, "EFI_SOFTWARE_UNSPECIFIED"},
        {{0x03, 0x01}, "EFI_SOFTWARE_SEC"},
        {{0x03, 0x02}, "EFI_SOFTWARE_PEI_CORE"},
        {{0x03, 0x03}, "EFI_SOFTWARE_PEI_MODULE"},
        {{0x03, 0x04}, "EFI_SOFTWARE_DXE_CORE"},
        {{0x03, 0x05}, "EFI_SOFTWARE_DXE_BS_DRIVER"},
        {{0x03, 0x06}, "EFI_SOFTWARE_DXE_RT_DRIVER"},
        {{0x03, 0x07}, "EFI_SOFTWARE_SMM_DRIVER"},
        {{0x03, 0x08}, "EFI_SOFTWARE_EFI_APPLICATION"},
        {{0x03, 0x09}, "EFI_SOFTWARE_EFI_OS_LOADER"},
        {{0x03, 0x0A}, "EFI_SOFTWARE_RT"},
        {{0x03, 0x0B}, "EFI_SOFTWARE_AL"},
        {{0x03, 0x0C}, "EFI_SOFTWARE_EBC_EXCEPTION"},
        {{0x03, 0x0D}, "EFI_SOFTWARE_IA32_EXCEPTION"},
        {{0x03, 0x0E}, "EFI_SOFTWARE_IPF_EXCEPTION"},
        {{0x03, 0x0F}, "EFI_SOFTWARE_PEI_SERVICE"},
        {{0x03, 0x10}, "EFI_SOFTWARE_EFI_BOOT_SERVICE"},
        {{0x03, 0x11}, "EFI_SOFTWARE_EFI_RUNTIME_SERVICE"},
        {{0x03, 0x12}, "EFI_SOFTWARE_EFI_DXE_SERVICE"},
        {{0x03, 0x13}, "EFI_SOFTWARE_X64_EXCEPTION"},
        {{0x03, 0x14}, "EFI_SOFTWARE_ARM_EXCEPTION"},
    };
    std::sort(v.begin(), v.end());
    return v;
}();

// Shared PI operation codes: key is {classField, statusType, operation}.
// Operations >= 0x1000 are subclass-specific and not decoded here.
using PiOpKey = std::tuple<uint8_t, uint8_t, uint16_t>;
using PiOpEntry = std::pair<PiOpKey, std::string_view>;
static const std::vector<PiOpEntry> piOperationNames = []() {
    std::vector<PiOpEntry> v = {
        {{0x00, 0x01, 0x0000}, "EFI_CU_PC_INIT_BEGIN"},
        {{0x00, 0x01, 0x0001}, "EFI_CU_PC_INIT_END"},
        {{0x00, 0x02, 0x0000}, "EFI_CU_EC_NON_SPECIFIC"},
        {{0x00, 0x02, 0x0001}, "EFI_CU_EC_DISABLED"},
        {{0x00, 0x02, 0x0002}, "EFI_CU_EC_NOT_SUPPORTED"},
        {{0x00, 0x02, 0x0003}, "EFI_CU_EC_NOT_DETECTED"},
        {{0x00, 0x02, 0x0004}, "EFI_CU_EC_NOT_CONFIGURED"},
        {{0x01, 0x01, 0x0000}, "EFI_P_PC_INIT"},
        {{0x01, 0x01, 0x0001}, "EFI_P_PC_RESET"},
        {{0x01, 0x01, 0x0002}, "EFI_P_PC_DISABLE"},
        {{0x01, 0x01, 0x0003}, "EFI_P_PC_PRESENCE_DETECT"},
        {{0x01, 0x01, 0x0004}, "EFI_P_PC_ENABLE"},
        {{0x01, 0x01, 0x0005}, "EFI_P_PC_RECONFIG"},
        {{0x01, 0x01, 0x0006}, "EFI_P_PC_DETECTED"},
        {{0x01, 0x01, 0x0007}, "EFI_P_PC_REMOVED"},
        {{0x01, 0x02, 0x0000}, "EFI_P_EC_NON_SPECIFIC"},
        {{0x01, 0x02, 0x0001}, "EFI_P_EC_DISABLED"},
        {{0x01, 0x02, 0x0002}, "EFI_P_EC_NOT_SUPPORTED"},
        {{0x01, 0x02, 0x0003}, "EFI_P_EC_NOT_DETECTED"},
        {{0x01, 0x02, 0x0004}, "EFI_P_EC_NOT_CONFIGURED"},
        {{0x01, 0x02, 0x0005}, "EFI_P_EC_INTERFACE_ERROR"},
        {{0x01, 0x02, 0x0006}, "EFI_P_EC_CONTROLLER_ERROR"},
        {{0x01, 0x02, 0x0007}, "EFI_P_EC_INPUT_ERROR"},
        {{0x01, 0x02, 0x0008}, "EFI_P_EC_OUTPUT_ERROR"},
        {{0x02, 0x01, 0x0000}, "EFI_IOB_PC_INIT"},
        {{0x02, 0x01, 0x0001}, "EFI_IOB_PC_RESET"},
        {{0x02, 0x01, 0x0002}, "EFI_IOB_PC_DISABLE"},
        {{0x02, 0x01, 0x0003}, "EFI_IOB_PC_DETECT"},
        {{0x02, 0x01, 0x0004}, "EFI_IOB_PC_ENABLE"},
        {{0x02, 0x01, 0x0005}, "EFI_IOB_PC_RECONFIG"},
        {{0x02, 0x01, 0x0006}, "EFI_IOB_PC_HOTPLUG"},
        {{0x02, 0x02, 0x0000}, "EFI_IOB_EC_NON_SPECIFIC"},
        {{0x02, 0x02, 0x0001}, "EFI_IOB_EC_DISABLED"},
        {{0x02, 0x02, 0x0002}, "EFI_IOB_EC_NOT_SUPPORTED"},
        {{0x02, 0x02, 0x0003}, "EFI_IOB_EC_NOT_DETECTED"},
        {{0x02, 0x02, 0x0004}, "EFI_IOB_EC_NOT_CONFIGURED"},
        {{0x02, 0x02, 0x0005}, "EFI_IOB_EC_INTERFACE_ERROR"},
        {{0x02, 0x02, 0x0006}, "EFI_IOB_EC_CONTROLLER_ERROR"},
        {{0x02, 0x02, 0x0007}, "EFI_IOB_EC_READ_ERROR"},
        {{0x02, 0x02, 0x0008}, "EFI_IOB_EC_WRITE_ERROR"},
        {{0x02, 0x02, 0x0009}, "EFI_IOB_EC_RESOURCE_CONFLICT"},
        {{0x03, 0x01, 0x0000}, "EFI_SW_PC_LOAD"},
        {{0x03, 0x01, 0x0001}, "EFI_SW_PC_INIT"},
        {{0x03, 0x01, 0x0002}, "EFI_SW_PC_EXIT_BS"},
        {{0x03, 0x01, 0x0003}, "EFI_SW_PC_SHUTDOWN"},
        {{0x03, 0x01, 0x0004}, "EFI_SW_PC_RESET"},
        {{0x03, 0x01, 0x0005}, "EFI_SW_PC_OS_BOOT"},
        {{0x03, 0x01, 0x0006}, "EFI_SW_PC_HANDOFF_TO_NEXT"},
        {{0x03, 0x02, 0x0000}, "EFI_SW_EC_NON_SPECIFIC"},
        {{0x03, 0x02, 0x0001}, "EFI_SW_EC_LOAD_ERROR"},
        {{0x03, 0x02, 0x0002}, "EFI_SW_EC_INVALID_PARAMETER"},
        {{0x03, 0x02, 0x0003}, "EFI_SW_EC_UNSUPPORTED"},
        {{0x03, 0x02, 0x0004}, "EFI_SW_EC_INVALID_BUFFER"},
        {{0x03, 0x02, 0x0005}, "EFI_SW_EC_OUT_OF_RESOURCES"},
        {{0x03, 0x02, 0x0006}, "EFI_SW_EC_ABORTED"},
        {{0x03, 0x02, 0x0007}, "EFI_SW_EC_ILLEGAL_SOFTWARE_STATE"},
        {{0x03, 0x02, 0x0008}, "EFI_SW_EC_ILLEGAL_HARDWARE_STATE"},
        {{0x03, 0x02, 0x0009}, "EFI_SW_EC_START_ERROR"},
        {{0x03, 0x02, 0x000A}, "EFI_SW_EC_BAD_DATE_TIME"},
        {{0x03, 0x02, 0x000B}, "EFI_SW_EC_CFG_INVALID"},
        {{0x03, 0x02, 0x000C}, "EFI_SW_EC_CFG_CLR_REQUEST"},
        {{0x03, 0x02, 0x000D}, "EFI_SW_EC_CFG_DEFAULT"},
        {{0x03, 0x02, 0x000E}, "EFI_SW_EC_PWD_INVALID"},
        {{0x03, 0x02, 0x000F}, "EFI_SW_EC_PWD_CLR_REQUEST"},
        {{0x03, 0x02, 0x0010}, "EFI_SW_EC_PWD_CLEARED"},
        {{0x03, 0x02, 0x0011}, "EFI_SW_EC_EVENT_LOG_FULL"},
        {{0x03, 0x02, 0x0012}, "EFI_SW_EC_WRITE_PROTECTED"},
        {{0x03, 0x02, 0x0013}, "EFI_SW_EC_FV_CORRUPTED"},
        {{0x03, 0x02, 0x0014}, "EFI_SW_EC_INCONSISTENT_MEMORY_MAP"},
    };
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

std::string_view getSubclassName(uint8_t classField, uint8_t subclass)
{
    if (classField <= piClassMax)
    {
        PiSubKey key{classField, subclass};
        auto it = std::lower_bound(piSubclassNames.begin(),
                                   piSubclassNames.end(), key, compareByFirst);
        return (it != piSubclassNames.end() && it->first == key)
                   ? it->second
                   : "Unknown";
    }
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

std::optional<std::string_view> getOperationName(
    uint8_t classField, uint8_t subclass, uint8_t statusType, uint8_t opcode,
    uint16_t operation)
{
    if (classField <= piClassMax)
    {
        if (operation >= 0x1000)
        {
            return std::nullopt;
        }
        PiOpKey key{classField, statusType, operation};
        auto it = std::lower_bound(piOperationNames.begin(),
                                   piOperationNames.end(), key, compareByFirst);
        return (it != piOperationNames.end() && it->first == key)
                   ? std::optional{it->second}
                   : std::nullopt;
    }
    SipOpKey key{subclass, statusType, opcode};
    auto it = std::lower_bound(sipOperationNames.begin(),
                               sipOperationNames.end(), key, compareByFirst);
    return (it != sipOperationNames.end() && it->first == key)
               ? std::optional{it->second}
               : std::nullopt;
}

static std::optional<std::string_view> getSipInstanceName(
    std::string_view opName, uint8_t instance)
{
    InstanceKey key{opName, instance};
    auto it = std::lower_bound(sipInstanceNames.begin(), sipInstanceNames.end(),
                               key, compareByFirst);
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
    auto it =
        std::lower_bound(arr.begin(), arr.end(), instance, compareByFirst);
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
    uint16_t operation = static_cast<uint16_t>(postcodeValue & 0x0000FFFFU);
    bool isPiCode = classField <= piClassMax;
    auto cpuNum = getPackageNumber(classField);

    auto opName =
        getOperationName(classField, subclass, statusType, opcode, operation);

    // Progress codes without a known operation name carry no additional
    // context.
    if (statusType == statusTypeProgress && !opName)
    {
        return;
    }

    std::string_view componentName = getSubclassName(classField, subclass);

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
    std::format_to(std::back_inserter(logMsg), "reported by {}", componentName);

    if (opName)
    {
        if (!isPiCode)
        {
            if (auto r =
                    getResetReasonName(subclass, statusType, opcode, instance))
            {
                std::format_to(std::back_inserter(logMsg), ", reset reason {}",
                               *r);
            }
            else if (auto i = getSipInstanceName(*opName, instance))
            {
                std::format_to(std::back_inserter(logMsg), ", instance {}", *i);
            }
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
        additionalData.emplace("NVIDIA_FIRMWARE", std::string(componentName));
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
