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

#include "post_code.hpp"

#include <sdbusplus/test/sdbus_mock.hpp>

#include <filesystem>
#include <fstream>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::NiceMock;

class PostCodeHandlersTest : public ::testing::Test
{
  protected:
    PostCodeHandlersTest() :
        bus_mock(std::make_unique<NiceMock<sdbusplus::SdBusMock>>()),
        bus(sdbusplus::get_mocked_new(bus_mock.get()))
    {}

    std::unique_ptr<NiceMock<sdbusplus::SdBusMock>> bus_mock;
    sdbusplus::bus_t bus;
};

TEST_F(PostCodeHandlersTest, FindExactMatch)
{
    PostCodeHandlers handlers;

    // Create handler
    PostCodeHandler handler;
    handler.name = "TestHandler";
    handler.primary = {0x01, 0x02};
    handler.secondary = {0x03};
    handlers.handlers.push_back(handler);

    // Find exact match
    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {0x03};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.find(code);
    EXPECT_NE(result, nullptr);
    EXPECT_EQ(result->name, "TestHandler");
}

TEST_F(PostCodeHandlersTest, FindNoMatch)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x99, 0x99};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.find(code);
    EXPECT_EQ(result, nullptr);
}

TEST_F(PostCodeHandlersTest, FindWithSecondaryOptional)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {0x99};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.find(code);
    EXPECT_NE(result, nullptr);
}

TEST_F(PostCodeHandlersTest, FindWithMaskExactMatch)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.mask = {0xFF, 0xFF};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.findWithMask(code);
    EXPECT_NE(result, nullptr);
}

TEST_F(PostCodeHandlersTest, FindWithMaskPartialMatch)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x00};
    handler.mask = {0xFF, 0x00};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x99};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.findWithMask(code);
    EXPECT_NE(result, nullptr);
}

TEST_F(PostCodeHandlersTest, FindWithMaskNoMatch)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.mask = {0xFF, 0xFF};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x99, 0x99};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.findWithMask(code);
    EXPECT_EQ(result, nullptr);
}

TEST_F(PostCodeHandlersTest, FindWithMaskSizeMismatch)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.mask = {0xFF};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.findWithMask(code);
    EXPECT_NE(result, nullptr);
}

TEST_F(PostCodeHandlersTest, FindWithMaskSecondaryMatch)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.mask = {0xFF, 0xFF};
    handler.secondary = {0x03};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {0x03};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.findWithMask(code);
    EXPECT_NE(result, nullptr);
}

TEST_F(PostCodeHandlersTest, FindWithMaskSecondaryMismatch)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.mask = {0xFF, 0xFF};
    handler.secondary = {0x03};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {0x99};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.findWithMask(code);
    EXPECT_EQ(result, nullptr);
}

TEST_F(PostCodeHandlersTest, HandleWithTargets)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.targets = {"test.target"};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    EXPECT_NO_THROW(handlers.handle(bus, code));
}

TEST_F(PostCodeHandlersTest, HandleWithEvent)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    PostCodeEvent event;
    event.name = "TestEvent";
    handler.event = event;
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    EXPECT_NO_THROW(handlers.handle(bus, code));
}

TEST_F(PostCodeHandlersTest, HandleNoMatch)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x99, 0x99};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    // Should not throw or crash
    EXPECT_NO_THROW(handlers.handle(bus, code));
}

TEST_F(PostCodeHandlersTest, LoadFromJson)
{
    PostCodeHandlers handlers;

    // Create temporary JSON file
    std::filesystem::path tempFile =
        std::filesystem::temp_directory_path() / "test_handlers.json";
    std::ofstream jsonFile(tempFile);
    jsonFile << R"([
        {
            "name": "TestHandler",
            "description": "Test Description",
            "primary": "0x0102",
            "secondary": "0x03",
            "targets": ["target1"],
            "mask": "0xFF00",
            "resolution": "Test resolution"
        }
    ])";
    jsonFile.close();

    EXPECT_NO_THROW(handlers.load(tempFile.string()));

    EXPECT_EQ(handlers.handlers.size(), 1);
    EXPECT_EQ(handlers.handlers[0].name, "TestHandler");

    std::filesystem::remove(tempFile);
}

TEST_F(PostCodeHandlersTest, LoadFromJsonInvalid)
{
    PostCodeHandlers handlers;

    std::filesystem::path tempFile =
        std::filesystem::temp_directory_path() / "test_invalid.json";
    std::ofstream jsonFile(tempFile);
    jsonFile << "invalid json";
    jsonFile.close();

    EXPECT_THROW(handlers.load(tempFile.string()), std::exception);

    std::filesystem::remove(tempFile);
}

TEST_F(PostCodeHandlersTest, HandleWithResolution)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.resolution = "Test resolution";
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    EXPECT_NO_THROW(handlers.handle(bus, code));
}

TEST_F(PostCodeHandlersTest, HandleWithTargetsAndEvent)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.targets = {"test.target"};
    PostCodeEvent event;
    event.name = "TestEvent";
    handler.event = event;
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    EXPECT_NO_THROW(handlers.handle(bus, code));
}

TEST_F(PostCodeHandlersTest, FindWithMaskEmptyHandlers)
{
    PostCodeHandlers handlers;

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.findWithMask(code);
    EXPECT_EQ(result, nullptr);
}

TEST_F(PostCodeHandlersTest, FindWithMaskNoMask)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.findWithMask(code);
    EXPECT_NE(result, nullptr);
}

TEST_F(PostCodeHandlersTest, FindEmptyHandlers)
{
    PostCodeHandlers handlers;

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.find(code);
    EXPECT_EQ(result, nullptr);
}

TEST_F(PostCodeHandlersTest, FindWithMaskMultipleHandlers)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler1;
    handler1.primary = {0x99, 0x99};
    handler1.mask = {0xFF, 0xFF};
    handlers.handlers.push_back(handler1);

    PostCodeHandler handler2;
    handler2.primary = {0x01, 0x02};
    handler2.mask = {0xFF, 0xFF};
    handlers.handlers.push_back(handler2);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.findWithMask(code);
    EXPECT_NE(result, nullptr);
    EXPECT_EQ(result->primary, handler2.primary);
}

TEST_F(PostCodeHandlersTest, FindWithMaskMaskMismatchOneByte)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x00};
    handler.mask = {0xFF, 0xF0};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x12};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.findWithMask(code);
    EXPECT_EQ(result, nullptr);
}

TEST_F(PostCodeHandlersTest, FindWithMaskExactMatchNoSecondaryInHandler)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.secondary = std::nullopt;
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {0x99};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.findWithMask(code);
    EXPECT_NE(result, nullptr);
    EXPECT_EQ(result->primary, primary);
}

TEST_F(PostCodeHandlersTest, HandlerWithAllOptionalsSet)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.secondary = {0x03};
    handler.mask = {0xFF, 0xFF};
    handler.resolution = "Resolution text";
    PostCodeEvent event;
    event.name = "TestEvent";
    handler.event = event;
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {0x03};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* found = handlers.findWithMask(code);
    EXPECT_NE(found, nullptr);
    EXPECT_TRUE(found->secondary.has_value());
    EXPECT_EQ(*found->secondary, secondary);
    EXPECT_TRUE(found->event.has_value());
    EXPECT_TRUE(found->resolution.has_value());

    EXPECT_NO_THROW(handlers.handle(bus, code));
}

TEST_F(PostCodeHandlersTest, FindWithMaskMaskMatchFirstHandlerSkipSecond)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler1;
    handler1.primary = {0x01, 0x00};
    handler1.mask = {0xFF, 0xF0};
    handlers.handlers.push_back(handler1);

    PostCodeHandler handler2;
    handler2.primary = {0x99, 0x99};
    handler2.mask = {0xFF, 0xFF};
    handlers.handlers.push_back(handler2);

    primarycode_t primary = {0x01, 0x05};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.findWithMask(code);
    EXPECT_NE(result, nullptr);
    EXPECT_EQ(result->primary, handler1.primary);
}

TEST_F(PostCodeHandlersTest, FindWithMaskMaskSizeMismatchUsesExactMatch)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.mask = {0xFF};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.findWithMask(code);
    EXPECT_NE(result, nullptr);
    EXPECT_EQ(result->primary, handler.primary);
}

TEST_F(PostCodeHandlersTest, FindWithMaskMaskSizeMismatchNoMatch)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.mask = {0xFF, 0xFF};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.findWithMask(code);
    EXPECT_EQ(result, nullptr);
}

TEST_F(PostCodeHandlersTest, FindWithMaskMaskLoopAllBytesMatch)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.mask = {0xFF, 0x0F};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.findWithMask(code);
    EXPECT_NE(result, nullptr);
}

TEST_F(PostCodeHandlersTest, FindExactMatchWithSecondary)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.secondary = {0x03};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {0x03};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.find(code);
    EXPECT_NE(result, nullptr);
    EXPECT_EQ(result->secondary->at(0), 0x03);
}

TEST_F(PostCodeHandlersTest, FindNoMatchPrimarySameSecondaryDiffers)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.secondary = {0x03};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {0xFF};
    postcode_t code = std::make_tuple(primary, secondary);

    const PostCodeHandler* result = handlers.find(code);
    EXPECT_EQ(result, nullptr);
}
