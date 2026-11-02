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

#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace prebootdiag
{
namespace
{

struct Recorder
{
    std::vector<std::pair<std::string, std::string>> calls;
    NotifyCall fn()
    {
        return [this](const std::string& s, const std::string& p) {
            calls.emplace_back(s, p);
        };
    }
};

int run(std::vector<const char*> args, const NotifyCall& call,
        std::ostream& err)
{
    return notifyPostCode(static_cast<int>(args.size()), args.data(), call,
                          err);
}

TEST(PostCodeNotify, SendsPostCodeReceivedForEachKnownName)
{
    for (auto name : postCodeNames)
    {
        Recorder rec;
        std::ostringstream err;
        std::string n(name);
        EXPECT_EQ(
            run({"prebootdiag-postcode-notify", n.c_str()}, rec.fn(), err), 0);
        ASSERT_EQ(rec.calls.size(), 1U);
        EXPECT_EQ(rec.calls[0].first,
                  "com.nvidia.PreBootDiag.App.StateType.PostCodeReceived");
        EXPECT_EQ(rec.calls[0].second, R"({"PostCodeName":")" + n + R"("})");
        EXPECT_TRUE(err.str().empty());
    }
}

TEST(PostCodeNotify, RejectsUnknownNameWithoutCalling)
{
    Recorder rec;
    std::ostringstream err;
    EXPECT_EQ(run({"prebootdiag-postcode-notify", "PSC_FMC_PC_TYPO"}, rec.fn(),
                  err),
              1);
    EXPECT_TRUE(rec.calls.empty());
    EXPECT_EQ(err.str(), "Unknown PostCodeName 'PSC_FMC_PC_TYPO'\n");
}

TEST(PostCodeNotify, RejectsWrongArgumentCountWithUsage)
{
    Recorder rec;
    std::ostringstream err;
    EXPECT_EQ(run({"prebootdiag-postcode-notify"}, rec.fn(), err), 1);
    EXPECT_EQ(run({"prebootdiag-postcode-notify", "a", "b"}, rec.fn(), err), 1);
    EXPECT_TRUE(rec.calls.empty());
    EXPECT_NE(err.str().find("PSC_FMC_PC_BOOT_MODE_PREBOOT_DIAG"),
              std::string::npos);
}

TEST(PostCodeNotify, FailedCallExitsNonZero)
{
    std::ostringstream err;
    NotifyCall failing = [](const std::string&, const std::string&) {
        throw std::runtime_error("org.freedesktop.DBus.Error.ServiceUnknown");
    };
    EXPECT_EQ(
        run({"prebootdiag-postcode-notify", "MB2_PC_CCPLEX_PREBOOT_DIAG_ENTRY"},
            failing, err),
        1);
    EXPECT_NE(err.str().find("ServiceUnknown"), std::string::npos);
}

TEST(PostCodeNotify, PayloadOnlyForKnownNames)
{
    EXPECT_EQ(postCodePayload("MB2_PC_CCPLEX_PREBOOT_DIAG_ENTRY"),
              R"({"PostCodeName":"MB2_PC_CCPLEX_PREBOOT_DIAG_ENTRY"})");
    EXPECT_FALSE(postCodePayload(""));
    EXPECT_FALSE(postCodePayload("mb2_pc_ccplex_preboot_diag_entry"));
}

} // namespace
} // namespace prebootdiag
