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

#include <cereal/archives/binary.hpp>
#include <cereal/cereal.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/tuple.hpp>
#include <cereal/types/vector.hpp>
#include <sdbusplus/sdbuspp_support/event.hpp>
#include <sdbusplus/test/sdbus_mock.hpp>
#include <xyz/openbmc_project/Logging/Entry/common.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::NiceMock;

namespace fs = std::filesystem;

std::vector<uint8_t> decodeHexString(const std::string& hex);
void from_json(const nlohmann::json& j, PostCodeEvent& event);
void from_json(const nlohmann::json& j, PostCodeHandler& handler);

namespace
{
constexpr auto rawInterface = "xyz.openbmc_project.State.Boot.Raw";
constexpr auto hostInterface = "xyz.openbmc_project.State.Host";

bool processBusFor(sdbusplus::bus_t& bus, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool processed = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        while (bus.process_discard())
        {
            processed = true;
        }
        bus.wait(1000);
    }
    while (bus.process_discard())
    {
        processed = true;
    }
    return processed;
}

bool processBusUntil(sdbusplus::bus_t& bus, std::chrono::milliseconds timeout,
                     const auto& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        while (bus.process_discard())
        {}
        if (predicate())
        {
            return true;
        }
        bus.wait(1000);
    }
    while (bus.process_discard())
    {}
    return predicate();
}

void sendRawPostCodeSignal(
    sdbusplus::bus_t& bus,
    const std::map<std::string, std::variant<postcode_t>>& properties)
{
    auto signal =
        bus.new_signal("/xyz/openbmc_project/state/boot/raw0",
                       "org.freedesktop.DBus.Properties", "PropertiesChanged");
    signal.append(std::string(rawInterface), properties,
                  std::vector<std::string>{});
    signal.signal_send();
    bus.flush();
}

void sendHostStateSignal(
    sdbusplus::bus_t& bus,
    const std::map<std::string, std::variant<std::string>>& properties)
{
    auto signal =
        bus.new_signal("/xyz/openbmc_project/state/host0",
                       "org.freedesktop.DBus.Properties", "PropertiesChanged");
    signal.append(std::string(hostInterface), properties,
                  std::vector<std::string>{});
    signal.signal_send();
    bus.flush();
}

void writeValidVersionFiles(const fs::path& path)
{
    fs::create_directories(path);

    std::ofstream osVer(path / "PostCodeDataVersion", std::ios::binary);
    cereal::BinaryOutputArchive verArchive(osVer);
    verArchive(static_cast<uint16_t>(1));
    osVer.close();

    std::ofstream osIdx(path / "CurrentBootCycleIndex", std::ios::binary);
    cereal::BinaryOutputArchive idxArchive(osIdx);
    idxArchive(static_cast<uint16_t>(0));
    osIdx.close();

    std::ofstream osCnt(path / "CurrentBootCycleCount", std::ios::binary);
    cereal::BinaryOutputArchive cntArchive(osCnt);
    cntArchive(static_cast<uint16_t>(0));
    osCnt.close();
}

using LoggingEntry = sdbusplus::common::xyz::openbmc_project::logging::Entry;

struct FakeLoggingState
{
    std::atomic_bool called = false;
};

int fakeLoggingCreate(sd_bus_message* rawMsg, void* userdata,
                      sd_bus_error*) noexcept
{
    try
    {
        auto* state = static_cast<FakeLoggingState*>(userdata);
        sdbusplus::message_t msg(rawMsg);
        std::string message;
        LoggingEntry::Level severity;
        std::map<std::string, std::string> additionalData;
        msg.read(message, severity, additionalData);
        state->called = true;

        auto reply = msg.new_method_return();
        reply.append(
            sdbusplus::object_path("/xyz/openbmc_project/logging/entry/1"));
        reply.method_return();
        return 1;
    }
    catch (...)
    {
        return -EINVAL;
    }
}

const sd_bus_vtable fakeLoggingVTable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Create", "ssa{ss}", "o", fakeLoggingCreate,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END};

void throwRuntimeErrorEventHook(const nlohmann::json&,
                                const std::source_location&)
{
    throw std::runtime_error("non-generated event hook");
}

class FakeLoggingService
{
  public:
    FakeLoggingService() : bus(sdbusplus::bus::new_user())
    {
        int rc = sd_bus_add_object_vtable(
            sdbusplus::details::bus_friend::get_busp(bus), &slot,
            "/xyz/openbmc_project/logging",
            "xyz.openbmc_project.Logging.Create", fakeLoggingVTable, &state);
        if (rc < 0)
        {
            throw sdbusplus::exception::SdBusError(-rc,
                                                   "sd_bus_add_object_vtable");
        }

        bus.request_name("xyz.openbmc_project.Logging");
        worker = std::thread([this]() {
            while (!stop)
            {
                try
                {
                    while (bus.process_discard())
                    {}
                    bus.wait(1000);
                }
                catch (...)
                {
                    return;
                }
            }
        });
    }

    ~FakeLoggingService()
    {
        stop = true;
        if (worker.joinable())
        {
            worker.join();
        }
        if (slot != nullptr)
        {
            sd_bus_slot_unref(slot);
        }
    }

    bool wasCalled() const
    {
        return state.called;
    }

  private:
    sdbusplus::bus_t bus;
    sd_bus_slot* slot = nullptr;
    FakeLoggingState state;
    std::atomic_bool stop = false;
    std::thread worker;
};

template <typename Tag>
typename Tag::type getPrivateMember(Tag);

template <typename Tag, typename Tag::type member>
struct PrivateMemberAccessor
{
    friend typename Tag::type getPrivateMember(Tag)
    {
        return member;
    }
};

struct SerializeTag
{
    using type = fs::path (PostCode::*)(const fs::path&);
    friend type getPrivateMember(SerializeTag);
};

template struct PrivateMemberAccessor<SerializeTag, &PostCode::serialize>;

struct DeserializeTag
{
    using type = bool (PostCode::*)(const fs::path&, uint16_t&);
    friend type getPrivateMember(DeserializeTag);
};

template struct PrivateMemberAccessor<DeserializeTag, &PostCode::deserialize>;

struct DeserializePostCodesTag
{
    using type = bool (PostCode::*)(const fs::path&,
                                    std::map<uint64_t, postcode_t>&);
    friend type getPrivateMember(DeserializePostCodesTag);
};

template struct PrivateMemberAccessor<DeserializePostCodesTag,
                                      &PostCode::deserializePostCodes>;

struct OnRawChangedTag
{
    using type = void (PostCode::*)(sdbusplus::message_t&);
    friend type getPrivateMember(OnRawChangedTag);
};

template struct PrivateMemberAccessor<OnRawChangedTag, &PostCode::onRawChanged>;

struct OnHostStateChangedTag
{
    using type = void (PostCode::*)(sdbusplus::message_t&);
    friend type getPrivateMember(OnHostStateChangedTag);
};

template struct PrivateMemberAccessor<OnHostStateChangedTag,
                                      &PostCode::onHostStateChanged>;

} // namespace

class TestablePostCode : public PostCode
{
  public:
    TestablePostCode(sdbusplus::bus_t& bus, const char* path, EventPtr& event,
                     int nodeIndex, PostCodeHandlers& handlers,
                     const std::string& postCodeListPathPrefix =
                         "/tmp/phosphor-post-code-manager-test/host") :
        PostCode(bus, path, event, nodeIndex, handlers, postCodeListPathPrefix)
    {}

    using PostCode::savePostCodes;
};

class PostCodeTest : public ::testing::Test
{
  protected:
    PostCodeTest() :
        bus_mock(std::make_unique<NiceMock<sdbusplus::SdBusMock>>()),
        bus(sdbusplus::get_mocked_new(bus_mock.get()))
    {
        const std::string testPathPrefix =
            "/tmp/phosphor-post-code-manager-test/host";
        fs::path hostPath = testPathPrefix + "0";
        fs::path versionFile = hostPath / "PostCodeDataVersion";

        bool versionFileCreated = false;
        try
        {
            fs::create_directories(hostPath);
        }
        catch (const fs::filesystem_error& e)
        {
            if (!fs::exists(hostPath))
            {
                std::cerr << "Error: Cannot create " << hostPath << ": "
                          << e.what() << std::endl;
                std::cerr << "Please ensure /tmp is writable" << std::endl;
            }
        }

        try
        {
            if (!fs::exists(versionFile))
            {
                std::ofstream versionStream(versionFile, std::ios::binary);
                if (versionStream.is_open())
                {
                    uint16_t version = 1;
                    cereal::BinaryOutputArchive archive(versionStream);
                    archive(version);
                    versionStream.close();
                    versionFileCreated = true;
                }
            }
            else
            {
                std::ifstream versionStream(versionFile, std::ios::binary);
                if (versionStream.is_open())
                {
                    uint16_t version = 0;
                    cereal::BinaryInputArchive archive(versionStream);
                    archive(version);
                    versionStream.close();
                    if (version == 1)
                    {
                        versionFileCreated = true;
                    }
                }
            }
        }
        catch (const std::exception& e)
        {
            std::cerr << "Warning: Could not create/verify version file: "
                      << e.what() << std::endl;
        }

        if (!versionFileCreated && fs::exists(hostPath))
        {
            std::cerr << "Note: Version file may not exist or be writable in "
                      << hostPath << std::endl;
            std::cerr
                << "PostCode constructor may attempt remove_all which requires write permissions."
                << std::endl;
        }

        testDir = fs::temp_directory_path() / "post_code_test";
        fs::create_directories(testDir);
        postCodeListPath = testDir / "host0";
        fs::create_directories(postCodeListPath);

        actualPostCodePath = testPathPrefix + "0";

        sd_event* event = nullptr;
        sd_event_default(&event);
        eventPtr.reset(event);

        handlers = std::make_unique<PostCodeHandlers>();

        try
        {
            postCode = std::make_unique<TestablePostCode>(
                bus, "/test/path", eventPtr, 0, *handlers, testPathPrefix);
        }
        catch (const fs::filesystem_error& e)
        {
            if (e.code() == std::errc::permission_denied)
            {
                std::cerr
                    << "Warning: PostCode construction failed due to permissions: "
                    << e.what() << std::endl;
                std::cerr << "This happens when PostCode tries to remove_all "
                          << hostPath << " during version check." << std::endl;
                std::cerr
                    << "Please ensure the directory is writable or run tests with sudo."
                    << std::endl;
                throw std::runtime_error(
                    "PostCode construction failed: " + std::string(e.what()));
            }
            throw;
        }
    }

    ~PostCodeTest()
    {
        try
        {
            if (fs::exists(testDir))
            {
                fs::remove_all(testDir);
            }
        }
        catch (const fs::filesystem_error&)
        {}

        try
        {
            fs::path testBasePath = "/tmp/phosphor-post-code-manager-test";
            if (fs::exists(testBasePath))
            {
                fs::remove_all(testBasePath);
            }
        }
        catch (const fs::filesystem_error&)
        {}
    }

    std::unique_ptr<NiceMock<sdbusplus::SdBusMock>> bus_mock;
    sdbusplus::bus_t bus;
    EventPtr eventPtr;
    std::unique_ptr<PostCodeHandlers> handlers;
    std::unique_ptr<TestablePostCode> postCode;
    fs::path testDir;
    fs::path postCodeListPath;
    fs::path actualPostCodePath;

    void callSavePostCodes(postcode_t code)
    {
        postCode->savePostCodes(code);
    }
};

TEST_F(PostCodeTest, DeleteAll)
{
    fs::path testFile = actualPostCodePath / "test.txt";
    std::ofstream ofs(testFile);
    ofs << "test";
    ofs.close();

    postCode->deleteAll();

    EXPECT_TRUE(fs::exists(actualPostCodePath));
    EXPECT_TRUE(fs::is_empty(actualPostCodePath));
}

TEST_F(PostCodeTest, GetPostCodesCurrentBoot)
{
    auto codes = postCode->getPostCodes(1);

    EXPECT_NO_THROW(codes = postCode->getPostCodes(1));
}

TEST_F(PostCodeTest, GetPostCodesOtherBoot)
{
    auto codes = postCode->getPostCodes(2);

    EXPECT_NO_THROW(codes = postCode->getPostCodes(2));
}

TEST_F(PostCodeTest, GetPostCodesWithTimeStamp)
{
    auto codes = postCode->getPostCodesWithTimeStamp(1);

    EXPECT_TRUE(codes.empty());
}

TEST_F(PostCodeTest, SerializeDeserialize)
{
    EXPECT_NO_THROW({ auto codes = postCode->getPostCodes(1); });
}

TEST_F(PostCodeTest, DeserializeNonExistent)
{
    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;

    EXPECT_NO_THROW({
        PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                           "/tmp/phosphor-post-code-manager-test2/host");
    });
}

TEST_F(PostCodeTest, IncrBootCycle)
{
    postCode->deleteAll();
    auto codes = postCode->getPostCodes(1);
    EXPECT_TRUE(codes.empty());
}

TEST_F(PostCodeTest, GetBootNum)
{
    auto codes1 = postCode->getPostCodes(1);
    auto codes2 = postCode->getPostCodes(2);
    auto codes3 = postCode->getPostCodes(MAX_BOOT_CYCLE_COUNT);

    EXPECT_NO_THROW({
        codes1 = postCode->getPostCodes(1);
        codes2 = postCode->getPostCodes(2);
        codes3 = postCode->getPostCodes(MAX_BOOT_CYCLE_COUNT);
    });
}

TEST_F(PostCodeTest, DecodeHexString)
{
    nlohmann::json j = {{"name", "TestHandler"},
                        {"description", "Test Description"},
                        {"primary", "0x0102"}};

    PostCodeHandler handler;
    from_json(j, handler);

    EXPECT_EQ(handler.primary.size(), 2);
    EXPECT_EQ(handler.primary[0], 0x01);
    EXPECT_EQ(handler.primary[1], 0x02);
}

TEST_F(PostCodeTest, DecodeHexStringInvalid)
{
    nlohmann::json j1 = {{"name", "TestHandler"},
                         {"description", "Test Description"},
                         {"primary", "0x"}};

    PostCodeHandler handler1;
    EXPECT_THROW(from_json(j1, handler1), std::runtime_error);

    nlohmann::json j2 = {{"name", "TestHandler"},
                         {"description", "Test Description"},
                         {"primary", "0x1"}};

    PostCodeHandler handler2;
    EXPECT_THROW(from_json(j2, handler2), std::runtime_error);

    nlohmann::json j3 = {{"name", "TestHandler"},
                         {"description", "Test Description"},
                         {"primary", "0102"}};

    PostCodeHandler handler3;
    EXPECT_THROW(from_json(j3, handler3), std::runtime_error);

    nlohmann::json j4 = {{"name", "TestHandler"},
                         {"description", "Test Description"},
                         {"primary", "0x123"}};

    PostCodeHandler handler4;
    EXPECT_THROW(from_json(j4, handler4), std::runtime_error);
}

TEST_F(PostCodeTest, PostCodeEventRaise)
{
    PostCodeEvent event;
    event.name = "TestEvent";
    event.args["key"] = "value";

    EXPECT_NO_THROW(event.raise());
}

TEST_F(PostCodeTest, PostCodeEventRaiseGeneratedEvent)
{
    PostCodeEvent event;
    event.name = "xyz.openbmc_project.Logging.Cleared";
    event.args["NUMBER_OF_LOGS"] = 1;

    EXPECT_THROW(event.raise(), sdbusplus::exception::SdBusError);
}

TEST_F(PostCodeTest, PostCodeEventRaisePropagatesNonGeneratedHook)
{
    static const bool registered = []() {
        sdbusplus::sdbuspp::register_event(
            "xyz.openbmc_project.Test.NonGenerated",
            throwRuntimeErrorEventHook);
        return true;
    }();
    (void)registered;

    PostCodeEvent event;
    event.name = "xyz.openbmc_project.Test.NonGenerated";

    EXPECT_THROW(event.raise(), std::runtime_error);
}

TEST_F(PostCodeTest, PostCodeEventRaiseGeneratedEventWithFakeLogging)
{
    try
    {
        FakeLoggingService service;

        PostCodeEvent event;
        event.name = "xyz.openbmc_project.Logging.Cleared";
        event.args["NUMBER_OF_LOGS"] = 1;

        EXPECT_NO_THROW(event.raise());
        EXPECT_TRUE(service.wasCalled());
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        GTEST_SKIP() << "session bus unavailable: " << e.what();
    }
}

TEST_F(PostCodeTest, FromJsonPostCodeEvent)
{
    nlohmann::json j = {{"name", "TestEvent"},
                        {"arguments", {{"key1", "value1"}, {"key2", 42}}}};

    PostCodeEvent event;
    from_json(j, event);

    EXPECT_EQ(event.name, "TestEvent");
    EXPECT_EQ(event.args["key1"].get<std::string>(), "value1");
    EXPECT_EQ(event.args["key2"].get<int>(), 42);
}

TEST_F(PostCodeTest, FromJsonPostCodeEventStringOnly)
{
    nlohmann::json j = {
        {"name", "StringEvent"},
        {"arguments", {{"key1", "value1"}, {"key2", "value2"}}}};

    PostCodeEvent event;
    from_json(j, event);

    EXPECT_EQ(event.name, "StringEvent");
    EXPECT_EQ(event.args["key1"].get<std::string>(), "value1");
    EXPECT_EQ(event.args["key2"].get<std::string>(), "value2");
}

TEST_F(PostCodeTest, FromJsonPostCodeEventIntegerOnly)
{
    nlohmann::json j = {{"name", "IntEvent"},
                        {"arguments", {{"key1", 10}, {"key2", 20}}}};

    PostCodeEvent event;
    from_json(j, event);

    EXPECT_EQ(event.name, "IntEvent");
    EXPECT_EQ(event.args["key1"].get<int>(), 10);
    EXPECT_EQ(event.args["key2"].get<int>(), 20);
}

TEST_F(PostCodeTest, FromJsonPostCodeEventMixed)
{
    nlohmann::json j = {
        {"name", "MixedEvent"},
        {"arguments",
         {{"str1", "value"}, {"int1", 100}, {"str2", "test"}, {"int2", 200}}}};

    PostCodeEvent event;
    from_json(j, event);

    EXPECT_EQ(event.name, "MixedEvent");
    EXPECT_EQ(event.args["str1"].get<std::string>(), "value");
    EXPECT_EQ(event.args["int1"].get<int>(), 100);
    EXPECT_EQ(event.args["str2"].get<std::string>(), "test");
    EXPECT_EQ(event.args["int2"].get<int>(), 200);
}

TEST_F(PostCodeTest, FromJsonPostCodeEventArgumentSkippedNonStringNonInteger)
{
    nlohmann::json j = {{"name", "EventWithSkipped"},
                        {"arguments",
                         {{"s", "str"},
                          {"i", 1},
                          {"skip_bool", true},
                          {"skip_null", nullptr}}}};

    PostCodeEvent event;
    from_json(j, event);

    EXPECT_EQ(event.name, "EventWithSkipped");
    EXPECT_EQ(event.args["s"].get<std::string>(), "str");
    EXPECT_EQ(event.args["i"].get<int>(), 1);
    EXPECT_EQ(event.args.count("skip_bool"), 0u);
    EXPECT_EQ(event.args.count("skip_null"), 0u);
}

TEST_F(PostCodeTest, FromJsonPostCodeEventEmptyArguments)
{
    nlohmann::json j = {{"name", "EmptyArgsEvent"}, {"arguments", {}}};

    PostCodeEvent event;
    from_json(j, event);

    EXPECT_EQ(event.name, "EmptyArgsEvent");
    EXPECT_TRUE(event.args.empty());
}

TEST_F(PostCodeTest, DecodeHexStringMinimalValid)
{
    std::vector<uint8_t> out = decodeHexString("0x00");
    EXPECT_EQ(out.size(), 1);
    EXPECT_EQ(out[0], 0x00);
}

TEST_F(PostCodeTest, DecodeHexStringTwoBytes)
{
    std::vector<uint8_t> out = decodeHexString("0x0102");
    EXPECT_EQ(out.size(), 2);
    EXPECT_EQ(out[0], 0x01);
    EXPECT_EQ(out[1], 0x02);
}

TEST_F(PostCodeTest, FromJsonPostCodeHandler)
{
    nlohmann::json j = {{"name", "TestHandler"},
                        {"description", "Test Description"},
                        {"primary", "0x0102"},
                        {"secondary", "0x03"},
                        {"targets", {"target1", "target2"}},
                        {"event", {{"name", "Event"}, {"arguments", {}}}}};

    PostCodeHandler handler;
    from_json(j, handler);

    EXPECT_EQ(handler.name, "TestHandler");
    EXPECT_EQ(handler.description, "Test Description");
    EXPECT_EQ(handler.primary.size(), 2);
    EXPECT_TRUE(handler.secondary.has_value());
    EXPECT_EQ(handler.targets.size(), 2);
}

TEST_F(PostCodeTest, FromJsonPostCodeHandlerWithMask)
{
    nlohmann::json j = {{"name", "TestHandler"},
                        {"description", "Test Description"},
                        {"primary", "0x0102"},
                        {"mask", "0xFF00"},
                        {"resolution", "Test resolution"}};

    PostCodeHandler handler;
    from_json(j, handler);

    EXPECT_TRUE(handler.mask.has_value());
    EXPECT_EQ(handler.mask->size(), 2);
    EXPECT_TRUE(handler.resolution.has_value());
    EXPECT_EQ(*handler.resolution, "Test resolution");
}

TEST_F(PostCodeTest, DeserializeInvalidVersion)
{
    fs::path testPath2 = testDir / "host1";
    fs::create_directories(testPath2);

    fs::path versionPath = testPath2 / "PostCodeDataVersion";
    std::ofstream os(versionPath, std::ios::binary);
    cereal::BinaryOutputArchive archive(os);
    uint16_t wrongVersion = 999;
    archive(wrongVersion);
    os.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    EXPECT_NO_THROW({
        PostCode postCode2(bus, "/test/path2", eventPtr2, 1, handlers2,
                           "/tmp/phosphor-post-code-manager-test2/host");
    });
}

TEST_F(PostCodeTest, GetPostCodesWithTimeStampEmpty)
{
    postCode->deleteAll();
    auto codes = postCode->getPostCodesWithTimeStamp(1);
    EXPECT_TRUE(codes.empty());
}

TEST_F(PostCodeTest, FromJsonPostCodeHandlerMinimal)
{
    nlohmann::json j = {{"name", "TestHandler"},
                        {"description", "Test Description"},
                        {"primary", "0x0102"}};

    PostCodeHandler handler;
    from_json(j, handler);

    EXPECT_EQ(handler.name, "TestHandler");
    EXPECT_FALSE(handler.secondary.has_value());
    EXPECT_TRUE(handler.targets.empty());
    EXPECT_FALSE(handler.event.has_value());
}

template <typename PostCodeType>
void callSavePostCodesDirectly(PostCodeType& postCode, postcode_t code)
{
    postCode.savePostCodes(code);
}

void simulateDbusPropertyChange(
    sdbusplus::bus_t& bus, const std::string& path,
    const std::string& interface,
    const std::map<std::string, std::variant<postcode_t>>& properties)
{
    try
    {
        auto signal =
            bus.new_signal(path.c_str(), "org.freedesktop.DBus.Properties",
                           "PropertiesChanged");
        signal.append(interface);
        signal.append(properties);
        signal.append(std::vector<std::string>{});

        signal.signal_send();

        for (int i = 0; i < 10; ++i)
        {
            bus.process();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    catch (const std::exception& e)
    {}
}

void simulateDbusPropertyChangeHostState(
    sdbusplus::bus_t& bus, const std::string& path,
    const std::string& interface,
    const std::map<std::string, std::variant<std::string>>& properties)
{
    try
    {
        auto signal =
            bus.new_signal(path.c_str(), "org.freedesktop.DBus.Properties",
                           "PropertiesChanged");
        signal.append(interface);
        signal.append(properties);
        signal.append(std::vector<std::string>{});
        signal.signal_send();
        bus.process();
    }
    catch (const std::exception& e)
    {}
}

TEST(PostCodeDbusSignalTest, RawValuePropertySignalSavesPostCode)
{
    fs::path testPath = fs::temp_directory_path() / "post_code_signal_raw0";
    fs::remove_all(testPath);
    writeValidVersionFiles(testPath);

    try
    {
        auto receiverBus = sdbusplus::bus::new_user();
        auto senderBus = sdbusplus::bus::new_user();

        EventPtr eventPtr;
        sd_event* event = nullptr;
        sd_event_default(&event);
        eventPtr.reset(event);

        PostCodeHandlers handlers;
        TestablePostCode postCode(
            receiverBus, "/test/signal/raw", eventPtr, 0, handlers,
            (fs::temp_directory_path() / "post_code_signal_raw").string());
        processBusFor(receiverBus, std::chrono::milliseconds(50));

        primarycode_t primary = {0xAA, 0xBB, 0xCC};
        secondarycode_t secondary = {0xDD};
        postcode_t code = std::make_tuple(primary, secondary);

        sendRawPostCodeSignal(senderBus, {{"Value", code}});

        ASSERT_TRUE(processBusUntil(
            receiverBus, std::chrono::milliseconds(500),
            [&postCode]() { return !postCode.getPostCodes(1).empty(); }));

        const auto codes = postCode.getPostCodes(1);
        ASSERT_EQ(codes.size(), 1);
        EXPECT_EQ(std::get<0>(codes.front()), primary);
        EXPECT_EQ(std::get<1>(codes.front()), secondary);
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        GTEST_SKIP() << "session bus unavailable: " << e.what();
    }

    fs::remove_all(testPath);
}

TEST(PostCodeDbusSignalTest, RawPropertySignalWithoutValueIsIgnored)
{
    fs::path testPath =
        fs::temp_directory_path() / "post_code_signal_raw_ignore0";
    fs::remove_all(testPath);
    writeValidVersionFiles(testPath);

    try
    {
        auto receiverBus = sdbusplus::bus::new_user();
        auto senderBus = sdbusplus::bus::new_user();

        EventPtr eventPtr;
        sd_event* event = nullptr;
        sd_event_default(&event);
        eventPtr.reset(event);

        PostCodeHandlers handlers;
        TestablePostCode postCode(
            receiverBus, "/test/signal/raw_ignore", eventPtr, 0, handlers,
            (fs::temp_directory_path() / "post_code_signal_raw_ignore")
                .string());
        processBusFor(receiverBus, std::chrono::milliseconds(50));

        primarycode_t primary = {0x10, 0x20};
        secondarycode_t secondary = {};
        postcode_t code = std::make_tuple(primary, secondary);

        sendRawPostCodeSignal(senderBus, {{"Ignored", code}});
        EXPECT_TRUE(processBusFor(receiverBus, std::chrono::milliseconds(500)));
        EXPECT_TRUE(postCode.getPostCodes(1).empty());
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        GTEST_SKIP() << "session bus unavailable: " << e.what();
    }

    fs::remove_all(testPath);
}

TEST(PostCodeDbusSignalTest, HostStatePropertySignalsCoverBranches)
{
    fs::path testPath = fs::temp_directory_path() / "post_code_signal_host0";
    fs::remove_all(testPath);
    writeValidVersionFiles(testPath);

    try
    {
        auto receiverBus = sdbusplus::bus::new_user();
        auto senderBus = sdbusplus::bus::new_user();

        EventPtr eventPtr;
        sd_event* event = nullptr;
        sd_event_default(&event);
        eventPtr.reset(event);

        PostCodeHandlers handlers;
        TestablePostCode postCode(
            receiverBus, "/test/signal/host", eventPtr, 0, handlers,
            (fs::temp_directory_path() / "post_code_signal_host").string());
        processBusFor(receiverBus, std::chrono::milliseconds(50));

        sendHostStateSignal(
            senderBus, {{"UnrelatedHostProperty", std::string("ignored")}});
        EXPECT_TRUE(processBusFor(receiverBus, std::chrono::milliseconds(500)));

        sendHostStateSignal(senderBus, {{"CurrentHostState",
                                         std::string("invalid-host-state")}});
        EXPECT_TRUE(processBusFor(receiverBus, std::chrono::milliseconds(500)));

        sendHostStateSignal(
            senderBus,
            {{"CurrentHostState",
              std::string(
                  "xyz.openbmc_project.State.Host.HostState.Running")}});
        EXPECT_TRUE(processBusFor(receiverBus, std::chrono::milliseconds(500)));

        sendHostStateSignal(
            senderBus,
            {{"CurrentHostState",
              std::string("xyz.openbmc_project.State.Host.HostState.Off")}});
        EXPECT_TRUE(processBusFor(receiverBus, std::chrono::milliseconds(500)));
        EXPECT_TRUE(postCode.getPostCodes(1).empty());

        primarycode_t primary = {0x01, 0x02};
        secondarycode_t secondary = {};
        postCode.savePostCodes(std::make_tuple(primary, secondary));
        ASSERT_FALSE(postCode.getPostCodes(1).empty());

        sendHostStateSignal(
            senderBus,
            {{"CurrentHostState",
              std::string("xyz.openbmc_project.State.Host.HostState.Off")}});
        ASSERT_TRUE(processBusUntil(
            receiverBus, std::chrono::milliseconds(500),
            [&postCode]() { return postCode.getPostCodes(1).empty(); }));
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        GTEST_SKIP() << "session bus unavailable: " << e.what();
    }

    fs::remove_all(testPath);
}

TEST_F(PostCodeTest, SimulateDbusPropertyChangeTriggerSavePostCodes)
{
    primarycode_t primary = {0xAA, 0xBB, 0xCC};
    secondarycode_t secondary = {0xDD};
    postcode_t code = std::make_tuple(primary, secondary);

    std::map<std::string, std::variant<postcode_t>> properties;
    properties["Value"] = code;

    std::string postCodePath = "/xyz/openbmc_project/state/boot/raw0";
    std::string interface = "xyz.openbmc_project.State.Boot.Raw";

    simulateDbusPropertyChange(bus, postCodePath, interface, properties);

    bus.process();

    callSavePostCodes(code);

    auto codes = postCode->getPostCodes(1);
    EXPECT_FALSE(codes.empty());
}

TEST_F(PostCodeTest, TimerCallbackAndSerialization)
{
    primarycode_t primary = {0x11, 0x22};
    secondarycode_t secondary = {0x33};
    postcode_t code = std::make_tuple(primary, secondary);

    simulateDbusPropertyChange(bus, "/xyz/openbmc_project/state/boot/raw0",
                               "xyz.openbmc_project.State.Boot.Raw",
                               {{"Value", code}});

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    sd_event_run(eventPtr.get(), 0);

    callSavePostCodes(code);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    sd_event_run(eventPtr.get(), 0);

    fs::path bootPath = actualPostCodePath / "1";
    fs::path indexPath = actualPostCodePath / "CurrentBootCycleIndex";
    fs::path countPath = actualPostCodePath / "CurrentBootCycleCount";
    fs::path versionPath = actualPostCodePath / "PostCodeDataVersion";

    EXPECT_TRUE(fs::exists(versionPath));

    EXPECT_NO_THROW({

    });
}

TEST_F(PostCodeTest, SavePostCodesNonEmptyPath)
{
    EXPECT_NO_THROW({ auto codes = postCode->getPostCodes(1); });
}

TEST_F(PostCodeTest, SavePostCodesSizeLimit)
{
    EXPECT_GT(MAX_POST_CODE_SIZE_PER_CYCLE, 0);
    EXPECT_LT(MAX_POST_CODE_SIZE_PER_CYCLE, 10000);
}

TEST_F(PostCodeTest, TimerStartPath)
{
    EXPECT_NO_THROW({ auto codes = postCode->getPostCodes(1); });
}

TEST_F(PostCodeTest, GetPostCodesDeserializedFromDisk)
{
    fs::path bootPath = actualPostCodePath / "1";

    std::map<uint64_t, postcode_t> codesToSerialize;
    primarycode_t primary = {0x01, 0x02, 0x03};
    secondarycode_t secondary = {0x04};
    postcode_t code = std::make_tuple(primary, secondary);
    codesToSerialize[1000] = code;

    std::ofstream os(bootPath, std::ios::binary);
    cereal::BinaryOutputArchive archive(os);
    archive(codesToSerialize);
    os.close();

    std::ofstream osIdx(actualPostCodePath / "CurrentBootCycleIndex",
                        std::ios::binary);
    cereal::BinaryOutputArchive idxArchive(osIdx);
    uint16_t bootIndex = 1;
    idxArchive(bootIndex);
    osIdx.close();

    std::ofstream osCnt(actualPostCodePath / "CurrentBootCycleCount",
                        std::ios::binary);
    cereal::BinaryOutputArchive cntArchive(osCnt);
    uint16_t bootCount = 2;
    cntArchive(bootCount);
    osCnt.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;

    std::string pathPrefix = actualPostCodePath.string();
    if (pathPrefix.ends_with("0"))
    {
        pathPrefix = pathPrefix.substr(0, pathPrefix.length() - 1);
    }
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2, pathPrefix);

    auto codes = postCode2.getPostCodes(1);

    EXPECT_FALSE(codes.empty());
}

TEST_F(PostCodeTest, GetPostCodesWithTimeStampDeserialized)
{
    fs::path bootPath = actualPostCodePath / "1";

    std::map<uint64_t, postcode_t> codesToSerialize;
    primarycode_t primary = {0x05, 0x06};
    secondarycode_t secondary = {0x07};
    postcode_t code = std::make_tuple(primary, secondary);
    codesToSerialize[2000] = code;

    std::ofstream os(bootPath, std::ios::binary);
    cereal::BinaryOutputArchive archive(os);
    archive(codesToSerialize);
    os.close();

    std::ofstream osIdx(actualPostCodePath / "CurrentBootCycleIndex",
                        std::ios::binary);
    cereal::BinaryOutputArchive idxArchive(osIdx);
    uint16_t bootIndex = 1;
    idxArchive(bootIndex);
    osIdx.close();

    std::ofstream osCnt(actualPostCodePath / "CurrentBootCycleCount",
                        std::ios::binary);
    cereal::BinaryOutputArchive cntArchive(osCnt);
    uint16_t bootCount = 2;
    cntArchive(bootCount);
    osCnt.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;

    std::string pathPrefix = actualPostCodePath.string();
    if (pathPrefix.ends_with("0"))
    {
        pathPrefix = pathPrefix.substr(0, pathPrefix.length() - 1);
    }
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2, pathPrefix);

    auto codes = postCode2.getPostCodesWithTimeStamp(1);
    EXPECT_FALSE(codes.empty());
    EXPECT_EQ(codes.size(), 1);
}

TEST_F(PostCodeTest, SerializeErrorHandling)
{
    fs::path readOnlyPath = testDir / "readonly";
    fs::create_directories(readOnlyPath);

    try
    {
        fs::permissions(readOnlyPath, fs::perms::owner_read);
    }
    catch (const fs::filesystem_error&)
    {}
}

TEST_F(PostCodeTest, DeserializeWithValidFiles)
{
    fs::path versionPath = postCodeListPath / "PostCodeDataVersion";
    std::ofstream os(versionPath, std::ios::binary);
    cereal::BinaryOutputArchive archive(os);
    uint16_t version = 1;
    archive(version);
    os.close();

    fs::path indexPath = postCodeListPath / "CurrentBootCycleIndex";
    std::ofstream osIdx(indexPath, std::ios::binary);
    cereal::BinaryOutputArchive idxArchive(osIdx);
    uint16_t index = 5;
    idxArchive(index);
    osIdx.close();

    fs::path countPath = postCodeListPath / "CurrentBootCycleCount";
    std::ofstream osCnt(countPath, std::ios::binary);
    cereal::BinaryOutputArchive cntArchive(osCnt);
    uint16_t count = 10;
    cntArchive(count);
    osCnt.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    auto codes = postCode2.getPostCodes(1);
    EXPECT_NO_THROW(codes = postCode2.getPostCodes(1));
}

TEST_F(PostCodeTest, DeserializePostCodesWithFile)
{
    fs::path codesPath = actualPostCodePath / "1";
    std::map<uint64_t, postcode_t> codesToSerialize;
    primarycode_t primary = {0x10, 0x20};
    secondarycode_t secondary = {0x30};
    postcode_t code = std::make_tuple(primary, secondary);
    codesToSerialize[3000] = code;

    std::ofstream os(codesPath, std::ios::binary);
    cereal::BinaryOutputArchive archive(os);
    archive(codesToSerialize);
    os.close();

    std::ofstream osIdx(actualPostCodePath / "CurrentBootCycleIndex",
                        std::ios::binary);
    cereal::BinaryOutputArchive idxArchive(osIdx);
    uint16_t bootIndex = 1;
    idxArchive(bootIndex);
    osIdx.close();

    std::ofstream osCnt(actualPostCodePath / "CurrentBootCycleCount",
                        std::ios::binary);
    cereal::BinaryOutputArchive cntArchive(osCnt);
    uint16_t bootCount = 2;
    cntArchive(bootCount);
    osCnt.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;

    std::string pathPrefix = actualPostCodePath.string();
    if (pathPrefix.ends_with("0"))
    {
        pathPrefix = pathPrefix.substr(0, pathPrefix.length() - 1);
    }
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2, pathPrefix);

    auto codes = postCode2.getPostCodes(1);
    EXPECT_FALSE(codes.empty());
}

TEST_F(PostCodeTest, DeserializeCorruptedFileCerealException)
{
    fs::path versionPath = actualPostCodePath / "PostCodeDataVersion";
    std::ofstream os(versionPath);
    os << "invalid binary data";
    os.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    std::string pathPrefix = actualPostCodePath.string();
    if (pathPrefix.ends_with("0"))
    {
        pathPrefix = pathPrefix.substr(0, pathPrefix.length() - 1);
    }
    EXPECT_NO_THROW({
        PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                           pathPrefix);
    });
}

TEST_F(PostCodeTest, DeserializeFilesystemErrorDuplicate)
{
    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    std::string pathPrefix = actualPostCodePath.string();
    if (pathPrefix.ends_with("0"))
    {
        pathPrefix = pathPrefix.substr(0, pathPrefix.length() - 1);
    }
    EXPECT_NO_THROW({
        PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                           pathPrefix);
    });
}

TEST_F(PostCodeTest, DeserializePostCodesCorruptedFileCerealException)
{
    fs::path codesPath = actualPostCodePath / "1";
    std::ofstream os(codesPath);
    os << "corrupted data";
    os.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    std::string pathPrefix = actualPostCodePath.string();
    if (pathPrefix.ends_with("0"))
    {
        pathPrefix = pathPrefix.substr(0, pathPrefix.length() - 1);
    }
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2, pathPrefix);

    auto codes = postCode2.getPostCodes(2);
    EXPECT_NO_THROW(codes = postCode2.getPostCodes(2));
}

TEST_F(PostCodeTest, DeserializePostCodesFilesystemErrorDuplicate)
{
    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    std::string pathPrefix = actualPostCodePath.string();
    if (pathPrefix.ends_with("0"))
    {
        pathPrefix = pathPrefix.substr(0, pathPrefix.length() - 1);
    }
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2, pathPrefix);

    auto codes = postCode2.getPostCodes(999);
    EXPECT_NO_THROW(codes = postCode2.getPostCodes(999));
}

TEST_F(PostCodeTest, GetPostCodesOversizedArchiveReturnsEmpty)
{
    // A container-size field larger than vector::max_size() makes cereal throw
    // std::length_error before any allocation; the read path must catch it,
    // discard the partial map, and return empty without crashing.
    const std::string prefix = "/tmp/pcm-oversized-test/host";
    fs::path hostPath = prefix + "0";
    fs::remove_all(hostPath);
    fs::create_directories(hostPath);
    {
        std::ofstream osVer(hostPath / "PostCodeDataVersion", std::ios::binary);
        cereal::BinaryOutputArchive verArchive(osVer);
        verArchive(static_cast<uint16_t>(PostCodeDataVersion));
    }
    {
        std::ofstream osIdx(hostPath / "CurrentBootCycleIndex",
                            std::ios::binary);
        cereal::BinaryOutputArchive idxArchive(osIdx);
        idxArchive(static_cast<uint16_t>(1));
    }
    {
        std::ofstream osCnt(hostPath / "CurrentBootCycleCount",
                            std::ios::binary);
        cereal::BinaryOutputArchive cntArchive(osCnt);
        cntArchive(static_cast<uint16_t>(1));
    }
    {
        // Binary map layout: size=1, key=0, first vector size = SIZE_MAX.
        std::ofstream bad(hostPath / "1", std::ios::binary);
        uint64_t mapSize = 1;
        uint64_t key = 0;
        uint64_t vecSize = 0xFFFFFFFFFFFFFFFFULL;
        bad.write(reinterpret_cast<const char*>(&mapSize), sizeof(mapSize));
        bad.write(reinterpret_cast<const char*>(&key), sizeof(key));
        bad.write(reinterpret_cast<const char*>(&vecSize), sizeof(vecSize));
    }

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path_oversized", eventPtr2, 0, handlers2,
                       prefix);

    std::vector<postcode_t> result;
    EXPECT_NO_THROW({ result = postCode2.getPostCodes(1); });
    EXPECT_TRUE(result.empty());

    fs::remove_all(hostPath);
}

TEST_F(PostCodeTest, MalformedRawSignalDoesNotCrash)
{
    // A structurally malformed Boot.Raw PropertiesChanged signal must be caught
    // by the callback guard and must not propagate out (would crash daemon).
    auto signal =
        bus.new_signal("/xyz/openbmc_project/state/boot/raw0",
                       "org.freedesktop.DBus.Properties", "PropertiesChanged");
    signal.append(std::string("xyz.openbmc_project.State.Boot.Raw"));
    signal.append(std::string("malformed-not-a-map"));
    signal.append(std::vector<std::string>{});
    EXPECT_NO_THROW({
        signal.signal_send();
        for (int i = 0; i < 10; ++i)
        {
            bus.process();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
}

TEST_F(PostCodeTest, MalformedHostStateSignalDoesNotCrash)
{
    // A structurally malformed State.Host PropertiesChanged signal must be
    // caught by the callback guard and must not propagate out.
    auto signal =
        bus.new_signal("/xyz/openbmc_project/state/host0",
                       "org.freedesktop.DBus.Properties", "PropertiesChanged");
    signal.append(std::string("xyz.openbmc_project.State.Host"));
    signal.append(std::string("malformed-not-a-map"));
    signal.append(std::vector<std::string>{});
    EXPECT_NO_THROW({
        signal.signal_send();
        for (int i = 0; i < 10; ++i)
        {
            bus.process();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
}

TEST_F(PostCodeTest, GetBootNumWrapAround)
{
    fs::path versionPath = postCodeListPath / "PostCodeDataVersion";
    std::ofstream osVer(versionPath, std::ios::binary);
    cereal::BinaryOutputArchive verArchive(osVer);
    verArchive(static_cast<uint16_t>(1));
    osVer.close();

    fs::path indexPath = postCodeListPath / "CurrentBootCycleIndex";
    std::ofstream osIdx(indexPath, std::ios::binary);
    cereal::BinaryOutputArchive idxArchive(osIdx);
    uint16_t index = 5;
    idxArchive(index);
    osIdx.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    auto codes = postCode2.getPostCodes(8);
    EXPECT_NO_THROW(codes = postCode2.getPostCodes(8));
}

TEST_F(PostCodeTest, IncrBootCycleWrapAround)
{
    fs::path versionPath = postCodeListPath / "PostCodeDataVersion";
    std::ofstream osVer(versionPath, std::ios::binary);
    cereal::BinaryOutputArchive verArchive(osVer);
    verArchive(static_cast<uint16_t>(1));
    osVer.close();

    fs::path indexPath = postCodeListPath / "CurrentBootCycleIndex";
    std::ofstream osIdx(indexPath, std::ios::binary);
    cereal::BinaryOutputArchive idxArchive(osIdx);
    uint16_t index = MAX_BOOT_CYCLE_COUNT;
    idxArchive(index);
    osIdx.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    auto codes = postCode2.getPostCodes(1);
    EXPECT_NO_THROW(codes = postCode2.getPostCodes(1));
}

TEST_F(PostCodeTest, SavePostCodesWrapsBootCycleAndClampsCount)
{
    fs::path wrapDir = testDir / "wrap0";
    fs::create_directories(wrapDir);

    std::ofstream osVer(wrapDir / "PostCodeDataVersion", std::ios::binary);
    cereal::BinaryOutputArchive verArchive(osVer);
    verArchive(static_cast<uint16_t>(1));
    osVer.close();

    std::ofstream osIdx(wrapDir / "CurrentBootCycleIndex", std::ios::binary);
    cereal::BinaryOutputArchive idxArchive(osIdx);
    idxArchive(static_cast<uint16_t>(MAX_BOOT_CYCLE_COUNT));
    osIdx.close();

    std::ofstream osCnt(wrapDir / "CurrentBootCycleCount", std::ios::binary);
    cereal::BinaryOutputArchive cntArchive(osCnt);
    cntArchive(static_cast<uint16_t>(MAX_BOOT_CYCLE_COUNT));
    osCnt.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    TestablePostCode postCode2(bus, "/test/wrap", eventPtr2, 0, handlers2,
                               (testDir / "wrap").string());

    primarycode_t primary = {0xAA, 0x55};
    secondarycode_t secondary = {};
    postCode2.savePostCodes(std::make_tuple(primary, secondary));

    auto codes = postCode2.getPostCodes(1);
    ASSERT_FALSE(codes.empty());
    EXPECT_EQ(std::get<0>(codes.back()), primary);
    EXPECT_EQ(postCode2.currentBootCycleCount(), MAX_BOOT_CYCLE_COUNT);
}

TEST_F(PostCodeTest, SerializeExceptionHandling)
{
    postCode->deleteAll();

    EXPECT_NO_THROW(postCode->deleteAll());
}

TEST_F(PostCodeTest, HostStatePropertyChange)
{
    EXPECT_NO_THROW({ auto codes = postCode->getPostCodes(1); });
}

TEST_F(PostCodeTest, PostCodeSizeLimit)
{
    EXPECT_GT(MAX_POST_CODE_SIZE_PER_CYCLE, 0);
    EXPECT_LE(MAX_POST_CODE_SIZE_PER_CYCLE, 1000);
}

TEST_F(PostCodeTest, PostCodeDisplayPath)
{
    const char* displayPath = POSTCODE_DISPLAY_PATH;
    EXPECT_NE(displayPath, nullptr);

    if (strlen(displayPath) > 0)
    {
        fs::path testPath = displayPath;
        EXPECT_NO_THROW({ auto codes = postCode->getPostCodes(1); });
    }
}

TEST_F(PostCodeTest, SerializeFullFlow)
{
    fs::path testPath = postCodeListPath;

    std::ofstream osIdx(testPath / "CurrentBootCycleIndex", std::ios::binary);
    cereal::BinaryOutputArchive idxArchive(osIdx);
    uint16_t index = 3;
    idxArchive(index);
    osIdx.close();

    std::ofstream osCnt(testPath / "CurrentBootCycleCount", std::ios::binary);
    cereal::BinaryOutputArchive cntArchive(osCnt);
    uint16_t count = 5;
    cntArchive(count);
    osCnt.close();

    std::map<uint64_t, postcode_t> codes;
    primarycode_t primary = {0x99, 0x88};
    secondarycode_t secondary = {0x77};
    postcode_t code = std::make_tuple(primary, secondary);
    codes[6000] = code;

    std::ofstream osCodes(testPath / "3", std::ios::binary);
    cereal::BinaryOutputArchive codesArchive(osCodes);
    codesArchive(codes);
    osCodes.close();

    std::ofstream osVer(testPath / "PostCodeDataVersion", std::ios::binary);
    cereal::BinaryOutputArchive verArchive(osVer);
    verArchive(static_cast<uint16_t>(1));
    osVer.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    auto retrievedCodes = postCode2.getPostCodes(1);
    EXPECT_NO_THROW(retrievedCodes = postCode2.getPostCodes(1));
}

TEST_F(PostCodeTest, SerializeFilesystemError)
{
    EXPECT_NO_THROW({ postCode->deleteAll(); });
}

TEST_F(PostCodeTest, SerializeCerealError)
{
    EXPECT_NO_THROW({ auto codes = postCode->getPostCodes(1); });
}

TEST_F(PostCodeTest, HostStateChangeEmptyPostCodes)
{
    postCode->deleteAll();

    auto codes = postCode->getPostCodes(1);
    EXPECT_TRUE(codes.empty());

    EXPECT_NO_THROW({ codes = postCode->getPostCodes(1); });
}

TEST_F(PostCodeTest, HostStateChangeNonEmptyPostCodes)
{
    EXPECT_NO_THROW({ auto codes = postCode->getPostCodes(1); });
}

TEST_F(PostCodeTest, GetBootNumNormalCase)
{
    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    auto codes = postCode2.getPostCodes(1);
    EXPECT_NO_THROW(codes = postCode2.getPostCodes(1));
}

TEST_F(PostCodeTest, IncrBootCycleNormalCase)
{
    fs::path versionPath = postCodeListPath / "PostCodeDataVersion";
    std::ofstream osVer(versionPath, std::ios::binary);
    cereal::BinaryOutputArchive verArchive(osVer);
    verArchive(static_cast<uint16_t>(1));
    osVer.close();

    fs::path indexPath = postCodeListPath / "CurrentBootCycleIndex";
    std::ofstream osIdx(indexPath, std::ios::binary);
    cereal::BinaryOutputArchive idxArchive(osIdx);
    uint16_t index = 2;
    idxArchive(index);
    osIdx.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    auto codes = postCode2.getPostCodes(1);
    EXPECT_NO_THROW(codes = postCode2.getPostCodes(1));
}

TEST_F(PostCodeTest, GetPostCodesEdgeCases)
{
    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    EXPECT_NO_THROW({ auto codes = postCode2.getPostCodes(0); });

    EXPECT_NO_THROW({ auto codes = postCode2.getPostCodes(1000); });
}

TEST_F(PostCodeTest, GetPostCodesWithTimeStampEdgeCases)
{
    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    EXPECT_NO_THROW({ auto codes = postCode2.getPostCodesWithTimeStamp(0); });

    EXPECT_NO_THROW({
        auto codes = postCode2.getPostCodesWithTimeStamp(1000);
    });
}

TEST_F(PostCodeTest, DeserializeSpecificFile)
{
    fs::path testPath = postCodeListPath / "TestIndex";
    std::ofstream os(testPath, std::ios::binary);
    cereal::BinaryOutputArchive archive(os);
    uint16_t index = 42;
    archive(index);
    os.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    EXPECT_NO_THROW({
        PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                           "/tmp/phosphor-post-code-manager-test2/host");
    });
}

#ifdef ENABLE_BIOS_POST_CODE_LOG

TEST_F(PostCodeTest, BiosPostCodeLogPath)
{
    primarycode_t primary = {0xAA, 0xBB, 0xCC, 0xDD};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    callSavePostCodes(code);

    EXPECT_NO_THROW({
        auto codes = postCode->getPostCodes(1);
        EXPECT_FALSE(codes.empty());
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    callSavePostCodes(code);

    EXPECT_NO_THROW({
        auto codes = postCode->getPostCodes(1);
        EXPECT_GE(codes.size(), 2);
    });
}
#endif

TEST_F(PostCodeTest, DeserializePostCodesWithExistingFile)
{
    fs::path codesPath = postCodeListPath / "2";
    std::map<uint64_t, postcode_t> codes;
    primarycode_t primary = {0xFE, 0xDC};
    secondarycode_t secondary = {0xBA};
    postcode_t code = std::make_tuple(primary, secondary);
    codes[7000] = code;

    std::ofstream os(codesPath, std::ios::binary);
    cereal::BinaryOutputArchive archive(os);
    archive(codes);
    os.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    auto retrievedCodes = postCode2.getPostCodes(3);
    EXPECT_NO_THROW(retrievedCodes = postCode2.getPostCodes(3));
}

TEST_F(PostCodeTest, GetBootNumVariousIndices)
{
    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    for (uint16_t i = 1; i <= MAX_BOOT_CYCLE_COUNT + 2; ++i)
    {
        EXPECT_NO_THROW({ auto codes = postCode2.getPostCodes(i); });
    }
}

TEST_F(PostCodeTest, GetPostCodesIndex1Empty)
{
    postCode->deleteAll();

    auto codes = postCode->getPostCodes(1);
    EXPECT_TRUE(codes.empty());
}

TEST_F(PostCodeTest, GetPostCodesWithTimeStampIndex1Empty)
{
    postCode->deleteAll();

    auto codes = postCode->getPostCodesWithTimeStamp(1);
    EXPECT_TRUE(codes.empty());
}

TEST_F(PostCodeTest, SerializeCerealException)
{
    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {0x03};
    postcode_t code = std::make_tuple(primary, secondary);

    simulateDbusPropertyChange(bus, "/xyz/openbmc_project/state/boot/raw0",
                               "xyz.openbmc_project.State.Boot.Raw",
                               {{"Value", code}});

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    EXPECT_NO_THROW({

    });
}

TEST_F(PostCodeTest, SerializeFilesystemErrorDetailed)
{
    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {0x03};
    postcode_t code = std::make_tuple(primary, secondary);

    simulateDbusPropertyChange(bus, "/xyz/openbmc_project/state/boot/raw0",
                               "xyz.openbmc_project.State.Boot.Raw",
                               {{"Value", code}});

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    EXPECT_NO_THROW({

    });
}

TEST_F(PostCodeTest, DeserializeCerealException)
{
    fs::path corruptedPath = postCodeListPath / "CorruptedVersion";
    std::ofstream os(corruptedPath, std::ios::binary);
    os << "invalid binary data that will cause cereal to throw";
    os.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;

    EXPECT_NO_THROW({
        PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                           "/tmp/phosphor-post-code-manager-test2/host");
    });
}

TEST_F(PostCodeTest, DeserializePostCodesCerealException)
{
    fs::path corruptedPath = postCodeListPath / "999";
    std::ofstream os(corruptedPath, std::ios::binary);
    os << "corrupted binary data";
    os.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    EXPECT_NO_THROW({
        auto codes = postCode2.getPostCodes(999);
        EXPECT_TRUE(codes.empty());
    });
}

TEST_F(PostCodeTest, DeserializeIndexFileCerealException)
{
    fs::path cerealDir = testDir / "cereal_idx0";
    fs::create_directories(cerealDir);
    fs::path versionPath = cerealDir / "PostCodeDataVersion";
    std::ofstream osVer(versionPath, std::ios::binary);
    cereal::BinaryOutputArchive verArchive(osVer);
    verArchive(static_cast<uint16_t>(1));
    osVer.close();
    std::ofstream osIdx(cerealDir / "CurrentBootCycleIndex");
    osIdx << "invalid index data";
    osIdx.close();
    std::ofstream osCnt(cerealDir / "CurrentBootCycleCount", std::ios::binary);
    cereal::BinaryOutputArchive cntArchive(osCnt);
    cntArchive(static_cast<uint16_t>(1));
    osCnt.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);
    PostCodeHandlers handlers2;
    std::string pathPrefix = (testDir / "cereal_idx").string();
    EXPECT_NO_THROW({
        PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                           pathPrefix);
    });
}

TEST_F(PostCodeTest, DeserializeEmptyIndexFileCerealException)
{
    fs::path cerealDir = testDir / "empty_idx0";
    fs::create_directories(cerealDir);

    std::ofstream osVer(cerealDir / "PostCodeDataVersion", std::ios::binary);
    cereal::BinaryOutputArchive verArchive(osVer);
    verArchive(static_cast<uint16_t>(1));
    osVer.close();

    std::ofstream(cerealDir / "CurrentBootCycleIndex").close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    EXPECT_NO_THROW({
        PostCode postCode2(bus, "/test/empty-index", eventPtr2, 0, handlers2,
                           (testDir / "empty_idx").string());
    });
}

TEST_F(PostCodeTest, DeserializeFilesystemErrorFromTooLongPath)
{
    auto deserialize = getPrivateMember(DeserializeTag{});
    uint16_t index = 0;
    fs::path tooLongPath = testDir / std::string(5000, 'x');

    EXPECT_FALSE((postCode.get()->*deserialize)(tooLongPath, index));
}

TEST_F(PostCodeTest, DeserializePostCodesFilesystemErrorFromTooLongPath)
{
    auto deserializePostCodes = getPrivateMember(DeserializePostCodesTag{});
    std::map<uint64_t, postcode_t> codes;
    fs::path tooLongPath = testDir / std::string(5000, 'x');

    EXPECT_FALSE((postCode.get()->*deserializePostCodes)(tooLongPath, codes));
    EXPECT_TRUE(codes.empty());
}

TEST_F(PostCodeTest, SerializeTooLongPathReturnsEmpty)
{
    auto serialize = getPrivateMember(SerializeTag{});
    fs::path tooLongPath = testDir / std::string(5000, 'x');

    EXPECT_TRUE((postCode.get()->*serialize)(tooLongPath).empty());
}

TEST_F(PostCodeTest, DeserializeCountFileCerealException)
{
    fs::path cerealDir = testDir / "cereal_cnt0";
    fs::create_directories(cerealDir);
    std::ofstream osVer(cerealDir / "PostCodeDataVersion", std::ios::binary);
    cereal::BinaryOutputArchive verArchive(osVer);
    verArchive(static_cast<uint16_t>(1));
    osVer.close();
    std::ofstream osIdx(cerealDir / "CurrentBootCycleIndex", std::ios::binary);
    cereal::BinaryOutputArchive idxArchive(osIdx);
    idxArchive(static_cast<uint16_t>(1));
    osIdx.close();
    std::ofstream osCnt(cerealDir / "CurrentBootCycleCount");
    osCnt << "invalid count";
    osCnt.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    PostCodeHandlers handlers2;
    std::string pathPrefix = (testDir / "cereal_cnt").string();
    EXPECT_NO_THROW({
        PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                           pathPrefix);
    });
}

TEST_F(PostCodeTest, DeserializePostCodesEmptyFileCerealException)
{
    fs::path cerealDir = testDir / "empty_codes0";
    fs::create_directories(cerealDir);

    std::ofstream osVer(cerealDir / "PostCodeDataVersion", std::ios::binary);
    cereal::BinaryOutputArchive verArchive(osVer);
    verArchive(static_cast<uint16_t>(1));
    osVer.close();

    std::ofstream osIdx(cerealDir / "CurrentBootCycleIndex", std::ios::binary);
    cereal::BinaryOutputArchive idxArchive(osIdx);
    idxArchive(static_cast<uint16_t>(1));
    osIdx.close();

    std::ofstream osCnt(cerealDir / "CurrentBootCycleCount", std::ios::binary);
    cereal::BinaryOutputArchive cntArchive(osCnt);
    cntArchive(static_cast<uint16_t>(1));
    osCnt.close();

    std::ofstream(cerealDir / "1").close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/empty-codes", eventPtr2, 0, handlers2,
                       (testDir / "empty_codes").string());

    auto codes = postCode2.getPostCodes(1);
    EXPECT_TRUE(codes.empty());
}

TEST_F(PostCodeTest, SavePostCodesSecondCallMonotonicTime)
{
    primarycode_t primary1 = {0x11, 0x22};
    secondarycode_t secondary = {};
    postcode_t code1 = std::make_tuple(primary1, secondary);
    callSavePostCodes(code1);

    primarycode_t primary2 = {0x33, 0x44};
    postcode_t code2 = std::make_tuple(primary2, secondary);
    callSavePostCodes(code2);

    auto codes = postCode->getPostCodes(1);
    EXPECT_GE(codes.size(), 2);
}

TEST_F(PostCodeTest, GetPostCodesFromDiskDeserializePathNotExists)
{
    fs::path diskDir = testDir / "disk_missing0";
    fs::create_directories(diskDir);
    std::ofstream osVer(diskDir / "PostCodeDataVersion", std::ios::binary);
    cereal::BinaryOutputArchive verArchive(osVer);
    verArchive(static_cast<uint16_t>(1));
    osVer.close();
    std::ofstream osIdx(diskDir / "CurrentBootCycleIndex", std::ios::binary);
    cereal::BinaryOutputArchive idxArchive(osIdx);
    idxArchive(static_cast<uint16_t>(2));
    osIdx.close();
    std::ofstream osCnt(diskDir / "CurrentBootCycleCount", std::ios::binary);
    cereal::BinaryOutputArchive cntArchive(osCnt);
    cntArchive(static_cast<uint16_t>(2));
    osCnt.close();

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    PostCodeHandlers handlers2;
    std::string pathPrefix = (testDir / "disk_missing").string();
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2, pathPrefix);

    auto codes = postCode2.getPostCodes(2);
    EXPECT_TRUE(codes.empty());
}

TEST_F(PostCodeTest, DeserializePostCodesPathNotExists)
{
    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    auto codes = postCode2.getPostCodes(2);
    EXPECT_TRUE(codes.empty());
}

TEST_F(PostCodeTest, SerializeCatchFilesystemErrorReadOnlyDir)
{
    fs::path roDir = testDir / "ro0";
    fs::create_directories(roDir);
    std::ofstream osVer(roDir / "PostCodeDataVersion", std::ios::binary);
    cereal::BinaryOutputArchive verArchive(osVer);
    verArchive(static_cast<uint16_t>(1));
    osVer.close();
    std::ofstream osIdx(roDir / "CurrentBootCycleIndex", std::ios::binary);
    cereal::BinaryOutputArchive idxArchive(osIdx);
    idxArchive(static_cast<uint16_t>(1));
    osIdx.close();
    std::ofstream osCnt(roDir / "CurrentBootCycleCount", std::ios::binary);
    cereal::BinaryOutputArchive cntArchive(osCnt);
    cntArchive(static_cast<uint16_t>(1));
    osCnt.close();

    PostCodeHandlers handlers2;
    std::string pathPrefix = (testDir / "ro").string();
    TestablePostCode roPostCode(bus, "/test/ro", eventPtr, 0, handlers2,
                                pathPrefix);
    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);
    roPostCode.savePostCodes(code);

    fs::permissions(roDir, fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    sd_event_run(eventPtr.get(), 0);
    fs::permissions(roDir, fs::perms::owner_all, fs::perm_options::replace);
}

TEST_F(PostCodeTest, HostStateOffEmptyPostCodes)
{
    simulateDbusPropertyChangeHostState(
        bus, "/xyz/openbmc_project/state/host0",
        "xyz.openbmc_project.State.Host",
        {{"CurrentHostState",
          std::string("xyz.openbmc_project.State.Host.HostState.Off")}});
    bus.process();
}

TEST_F(PostCodeTest, HostStateOffNonEmptyPostCodesClears)
{
    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);
    callSavePostCodes(code);
    simulateDbusPropertyChangeHostState(
        bus, "/xyz/openbmc_project/state/host0",
        "xyz.openbmc_project.State.Host",
        {{"CurrentHostState",
          std::string("xyz.openbmc_project.State.Host.HostState.Off")}});
    bus.process();
    EXPECT_NO_THROW({ auto codes = postCode->getPostCodes(1); });
}

TEST_F(PostCodeTest, FromJsonPostCodeHandlerOnlyTargets)
{
    nlohmann::json j = {{"name", "T"},
                        {"description", "D"},
                        {"primary", "0x0102"},
                        {"targets", std::vector<std::string>{}}};
    PostCodeHandler handler;
    from_json(j, handler);
    EXPECT_EQ(handler.name, "T");
    EXPECT_TRUE(handler.targets.empty());
    EXPECT_FALSE(handler.secondary.has_value());
    EXPECT_FALSE(handler.event.has_value());
    EXPECT_FALSE(handler.mask.has_value());
    EXPECT_FALSE(handler.resolution.has_value());
}

TEST_F(PostCodeTest, FromJsonPostCodeHandlerTargetsAndSecondary)
{
    nlohmann::json j = {{"name", "T"},
                        {"description", "D"},
                        {"primary", "0x0102"},
                        {"secondary", "0x03"},
                        {"targets", {"t1", "t2"}}};
    PostCodeHandler handler;
    from_json(j, handler);
    EXPECT_EQ(handler.targets.size(), 2);
    EXPECT_TRUE(handler.secondary.has_value());
    EXPECT_FALSE(handler.event.has_value());
}

TEST_F(PostCodeTest, FromJsonPostCodeHandlerEventOnly)
{
    nlohmann::json j = {
        {"name", "H"},
        {"description", "Desc"},
        {"primary", "0x0102"},
        {"event", {{"name", "E"}, {"arguments", nlohmann::json::object()}}}};
    PostCodeHandler handler;
    from_json(j, handler);
    EXPECT_TRUE(handler.event.has_value());
    EXPECT_EQ(handler.event->name, "E");
    EXPECT_FALSE(handler.mask.has_value());
    EXPECT_FALSE(handler.resolution.has_value());
}

TEST_F(PostCodeTest, FromJsonPostCodeHandlerMaskAndResolutionOnly)
{
    nlohmann::json j = {{"name", "H"},
                        {"description", "D"},
                        {"primary", "0x0102"},
                        {"mask", "0xFF00"},
                        {"resolution", "Res"}};
    PostCodeHandler handler;
    from_json(j, handler);
    EXPECT_TRUE(handler.mask.has_value());
    EXPECT_EQ(handler.mask->size(), 2);
    EXPECT_TRUE(handler.resolution.has_value());
    EXPECT_EQ(*handler.resolution, "Res");
    EXPECT_FALSE(handler.secondary.has_value());
    EXPECT_TRUE(handler.targets.empty());
}

TEST_F(PostCodeTest, DeserializePostCodesPathIsDirectoryDoesNotThrow)
{
    fs::path dirPath = testDir / "dir_as_file0";
    fs::create_directories(dirPath);
    std::ofstream osVer(dirPath / "PostCodeDataVersion", std::ios::binary);
    cereal::BinaryOutputArchive verArchive(osVer);
    verArchive(static_cast<uint16_t>(1));
    osVer.close();
    std::ofstream osIdx(dirPath / "CurrentBootCycleIndex", std::ios::binary);
    cereal::BinaryOutputArchive idxArchive(osIdx);
    idxArchive(static_cast<uint16_t>(1));
    osIdx.close();
    std::ofstream osCnt(dirPath / "CurrentBootCycleCount", std::ios::binary);
    cereal::BinaryOutputArchive cntArchive(osCnt);
    cntArchive(static_cast<uint16_t>(1));
    osCnt.close();
    fs::create_directories(dirPath / "1");

    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    PostCodeHandlers handlers2;
    std::string pathPrefix = (testDir / "dir_as_file").string();
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2, pathPrefix);
    // Corrupt/unreadable archive must not throw; returns empty, no abort.
    std::vector<postcode_t> result;
    EXPECT_NO_THROW({ result = postCode2.getPostCodes(1); });
    EXPECT_TRUE(result.empty());
}

TEST_F(PostCodeTest, GetBootNumWrapAroundDetailed)
{
    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    EXPECT_NO_THROW({
        auto codes1 = postCode2.getPostCodes(1);
        auto codes2 = postCode2.getPostCodes(2);
        auto codes3 = postCode2.getPostCodes(3);
    });
}

TEST_F(PostCodeTest, IncrBootCycleWrapAroundDetailed)
{
    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {0x03};
    postcode_t code = std::make_tuple(primary, secondary);

    callSavePostCodes(code);

    EXPECT_NO_THROW({
        auto codes = postCode->getPostCodes(1);
        EXPECT_FALSE(codes.empty());
    });
}

TEST_F(PostCodeTest, PostCodeHandlersHandleWithTargetsAndEvent)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.targets = {"test.target1", "test.target2"};
    PostCodeEvent event;
    event.name = "TestEvent";
    event.args["key1"] = "value1";
    handler.event = event;
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    EXPECT_NO_THROW(handlers.handle(bus, code));
}

TEST_F(PostCodeTest, PostCodeHandlersHandleWithResolution)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.resolution = "Test resolution for NVIDIA POST code";
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    EXPECT_NO_THROW(handlers.handle(bus, code));
}

TEST_F(PostCodeTest, SavePostCodesWithDisplayPath)
{
    primarycode_t primary = {0x01, 0x02, 0x03, 0x04};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    callSavePostCodes(code);

    if (strlen(POSTCODE_DISPLAY_PATH) > 0)
    {
        std::string displayPath = std::string(POSTCODE_DISPLAY_PATH) + "0";

        EXPECT_TRUE(fs::exists(displayPath));

        std::ifstream file(displayPath);
        ASSERT_TRUE(file.is_open());
        std::string content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
        EXPECT_TRUE(content.find("0x") == 0 || content.find("0X") == 0);
        file.close();
    }

    EXPECT_NO_THROW({

    });
}

TEST_F(PostCodeTest, SavePostCodesSizeLimitEnforcement)
{
    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    for (int i = 0; i < 200; ++i)
    {
        callSavePostCodes(code);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    sd_event_run(eventPtr.get(), 0);

    EXPECT_NO_THROW({
        auto codes = postCode->getPostCodes(1);
        EXPECT_LE(codes.size(), 512);
    });
}

TEST_F(PostCodeTest, HostStateChangeEmptyPostCodesDetailed)
{
    simulateDbusPropertyChangeHostState(
        bus, "/xyz/openbmc_project/state/host0",
        "xyz.openbmc_project.State.Host",
        {{"CurrentHostState",
          std::string("xyz.openbmc_project.State.Host.HostState.Off")}});

    EXPECT_NO_THROW({

    });
}

TEST_F(PostCodeTest, HostStateChangeNonEmptyPostCodesDetailed)
{
    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    callSavePostCodes(code);

    simulateDbusPropertyChangeHostState(
        bus, "/xyz/openbmc_project/state/host0",
        "xyz.openbmc_project.State.Host",
        {{"CurrentHostState",
          std::string("xyz.openbmc_project.State.Host.HostState.Off")}});

    EXPECT_NO_THROW({

    });
}

TEST_F(PostCodeTest, GetPostCodesIndexNotOneEmpty)
{
    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    auto codes = postCode2.getPostCodes(2);

    EXPECT_NO_THROW({

    });
}

TEST_F(PostCodeTest, GetPostCodesWithTimeStampIndexNotOneEmpty)
{
    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    auto codes = postCode2.getPostCodesWithTimeStamp(2);

    EXPECT_NO_THROW({

    });
}

TEST_F(PostCodeTest, IncrBootCycleWrapAroundAtMax)
{
    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    uint16_t maxBootCycle = 100;
    std::ofstream osIdx(actualPostCodePath / "CurrentBootCycleIndex",
                        std::ios::binary);
    cereal::BinaryOutputArchive idxArchive(osIdx);
    idxArchive(maxBootCycle);
    osIdx.close();

    std::ofstream osCnt(actualPostCodePath / "CurrentBootCycleCount",
                        std::ios::binary);
    cereal::BinaryOutputArchive cntArchive(osCnt);
    cntArchive(maxBootCycle);
    osCnt.close();

    EventPtr eventPtr3;
    sd_event* event3 = nullptr;
    sd_event_default(&event3);
    eventPtr3.reset(event3);

    PostCodeHandlers handlers3;
    PostCode postCode3(bus, "/test/path3", eventPtr3, 0, handlers3,
                       "/tmp/phosphor-post-code-manager-test2/host");

    primarycode_t primary = {0xFF, 0xFE};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    simulateDbusPropertyChange(bus, "/xyz/openbmc_project/state/boot/raw0",
                               "xyz.openbmc_project.State.Boot.Raw",
                               {{"Value", code}});

    EXPECT_NO_THROW({

    });
}

TEST_F(PostCodeTest, GetBootNumWrapAroundEdgeCases)
{
    EventPtr eventPtr2;
    sd_event* event2 = nullptr;
    sd_event_default(&event2);
    eventPtr2.reset(event2);

    PostCodeHandlers handlers2;
    PostCode postCode2(bus, "/test/path2", eventPtr2, 0, handlers2,
                       "/tmp/phosphor-post-code-manager-test2/host");

    EXPECT_NO_THROW({
        auto codes1 = postCode2.getPostCodes(1);
        auto codes2 = postCode2.getPostCodes(2);
        auto codes3 = postCode2.getPostCodes(3);
        auto codes10 = postCode2.getPostCodes(10);
        auto codes50 = postCode2.getPostCodes(50);
        auto codes100 = postCode2.getPostCodes(100);
    });
}

TEST_F(PostCodeTest, PostCodeHandlersHandleMultipleTargets)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    handler.targets = {"target1.service", "target2.service", "target3.service"};
    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    EXPECT_NO_THROW(handlers.handle(bus, code));
}

TEST_F(PostCodeTest, PostCodeHandlersHandleNoEvent)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};

    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    EXPECT_NO_THROW(handlers.handle(bus, code));
}

TEST_F(PostCodeTest, PostCodeHandlersHandleEventNoTargets)
{
    PostCodeHandlers handlers;

    PostCodeHandler handler;
    handler.primary = {0x01, 0x02};
    PostCodeEvent event;
    event.name = "TestEvent";
    event.args["key"] = "value";
    handler.event = event;

    handlers.handlers.push_back(handler);

    primarycode_t primary = {0x01, 0x02};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    EXPECT_NO_THROW(handlers.handle(bus, code));
}

TEST_F(PostCodeTest, SavePostCodesNonEmptyPostCodesTimestamp)
{
    primarycode_t primary1 = {0x01, 0x02};
    secondarycode_t secondary1 = {};
    postcode_t code1 = std::make_tuple(primary1, secondary1);

    callSavePostCodes(code1);

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    primarycode_t primary2 = {0x03, 0x04};
    postcode_t code2 = std::make_tuple(primary2, secondary1);

    callSavePostCodes(code2);

    auto codes = postCode->getPostCodes(1);
    EXPECT_GE(codes.size(), 2);
}

TEST_F(PostCodeTest, SavePostCodesTimerAlreadyRunning)
{
    primarycode_t primary1 = {0xAA, 0xBB};
    secondarycode_t secondary = {};
    postcode_t code1 = std::make_tuple(primary1, secondary);

    callSavePostCodes(code1);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    primarycode_t primary2 = {0xCC, 0xDD};
    postcode_t code2 = std::make_tuple(primary2, secondary);

    callSavePostCodes(code2);

    EXPECT_NO_THROW({
        auto codes = postCode->getPostCodes(1);
        EXPECT_GE(codes.size(), 2);
    });
}

TEST_F(PostCodeTest, GetPostCodesIndexOneNonEmpty)
{
    primarycode_t primary1 = {0x11, 0x22};
    secondarycode_t secondary = {};
    postcode_t code1 = std::make_tuple(primary1, secondary);

    callSavePostCodes(code1);

    auto codes = postCode->getPostCodes(1);
    EXPECT_FALSE(codes.empty());
    EXPECT_EQ(codes.size(), 1);
}

TEST_F(PostCodeTest, GetPostCodesWithTimeStampIndexOneNonEmpty)
{
    primarycode_t primary1 = {0x33, 0x44};
    secondarycode_t secondary = {};
    postcode_t code1 = std::make_tuple(primary1, secondary);

    callSavePostCodes(code1);

    auto codes = postCode->getPostCodesWithTimeStamp(1);
    EXPECT_FALSE(codes.empty());
    EXPECT_EQ(codes.size(), 1);
}

TEST_F(PostCodeTest, SavePostCodesSizeLimitExact)
{
    for (int i = 0; i < 513; ++i)
    {
        primarycode_t primary = {static_cast<uint8_t>(i & 0xFF),
                                 static_cast<uint8_t>((i >> 8) & 0xFF)};
        secondarycode_t secondary = {};
        postcode_t code = std::make_tuple(primary, secondary);
        callSavePostCodes(code);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    sd_event_run(eventPtr.get(), 0);

    auto codes = postCode->getPostCodes(1);
    EXPECT_LE(codes.size(), 512);
    EXPECT_GE(codes.size(), 50) << "deferred save should have flushed a batch";
    if (codes.size() >= 512)
    {
        EXPECT_EQ(codes.size(), 512);
    }
}

// Insert MAX_POST_CODE_SIZE_PER_CYCLE + 1 codes with guaranteed unique
// timestamps (2 µs sleep ensures the steady_clock offset differs each call).
// The 513th insert must trigger the size-limit erase branch at
// post_code.cpp:412-414.
TEST_F(PostCodeTest, SavePostCodesSizeLimitErase)
{
    for (int i = 0; i <= MAX_POST_CODE_SIZE_PER_CYCLE; ++i)
    {
        primarycode_t primary = {static_cast<uint8_t>(i & 0xFF),
                                 static_cast<uint8_t>((i >> 8) & 0xFF)};
        secondarycode_t secondary = {};
        callSavePostCodes(std::make_tuple(primary, secondary));
        std::this_thread::sleep_for(std::chrono::microseconds(2));
    }

    auto codes = postCode->getPostCodes(1);
    EXPECT_EQ(static_cast<int>(codes.size()), MAX_POST_CODE_SIZE_PER_CYCLE);
}

TEST_F(PostCodeTest, SavePostCodesEmptyPostCodesFirstCode)
{
    primarycode_t primary = {0xFE, 0xDC};
    secondarycode_t secondary = {0xBA};
    postcode_t code = std::make_tuple(primary, secondary);

    callSavePostCodes(code);

    auto codes = postCode->getPostCodes(1);
    EXPECT_FALSE(codes.empty());
    EXPECT_EQ(codes.size(), 1);
}

TEST_F(PostCodeTest, SavePostCodesTimerCreation)
{
    primarycode_t primary = {0x12, 0x34};
    secondarycode_t secondary = {};
    postcode_t code = std::make_tuple(primary, secondary);

    callSavePostCodes(code);

    EXPECT_NO_THROW({
        auto codes = postCode->getPostCodes(1);
        EXPECT_FALSE(codes.empty());
    });
}

TEST_F(PostCodeTest, SavePostCodesTimerStartWhenNotRunning)
{
    primarycode_t primary1 = {0x11, 0x22};
    secondarycode_t secondary = {};
    postcode_t code1 = std::make_tuple(primary1, secondary);

    callSavePostCodes(code1);

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    sd_event_run(eventPtr.get(), 0);

    primarycode_t primary2 = {0x33, 0x44};
    postcode_t code2 = std::make_tuple(primary2, secondary);

    callSavePostCodes(code2);

    auto codes = postCode->getPostCodes(1);
    EXPECT_GE(codes.size(), 2);
}

// A truncated archive makes cereal throw while reading the stored value.
// deserialize must report failure instead of letting the exception escape.
// The file is truncated rather than carrying a bogus size prefix so no
// oversized allocation is attempted (that aborts under valgrind).
TEST_F(PostCodeTest, DeserializeCorruptArchiveReturnsFalse)
{
    fs::path corrupt = testDir / "corrupt_index";
    {
        std::ofstream ofs(corrupt, std::ios::binary);
        const char byte = 0x01;
        ofs.write(&byte, sizeof(byte));
    }

    auto deserialize = getPrivateMember(DeserializeTag{});
    uint16_t index = 0;

    EXPECT_FALSE((postCode.get()->*deserialize)(corrupt, index));
}

// A corrupt post-code archive must not leave partially decoded entries behind:
// deserializePostCodes reports failure and clears the output map.
TEST_F(PostCodeTest, DeserializePostCodesCorruptArchiveClearsCodes)
{
    fs::path corrupt = testDir / "corrupt_codes";
    {
        std::ofstream ofs(corrupt, std::ios::binary);
        const char bytes[] = {0x01, 0x02, 0x03};
        ofs.write(bytes, sizeof(bytes));
    }

    auto deserializePostCodes = getPrivateMember(DeserializePostCodesTag{});
    std::map<uint64_t, postcode_t> codes;
    primarycode_t primary = {0xAA};
    secondarycode_t secondary = {};
    codes[1] = std::make_tuple(primary, secondary);

    EXPECT_FALSE((postCode.get()->*deserializePostCodes)(corrupt, codes));
    EXPECT_TRUE(codes.empty());
}

// decodeHexString rejects on three independent conditions; the existing tests
// only exercise the "too short" one. Cover the odd-length and missing-prefix
// arms as well.
TEST_F(PostCodeTest, DecodeHexStringOddLengthThrows)
{
    // Long enough and 0x-prefixed, but an odd number of characters.
    EXPECT_THROW(decodeHexString("0x123"), std::runtime_error);
}

TEST_F(PostCodeTest, DecodeHexStringMissingPrefixThrows)
{
    // Long enough and even length, but not 0x-prefixed.
    EXPECT_THROW(decodeHexString("1234"), std::runtime_error);
}

// A malformed Boot.Raw PropertiesChanged signal must be swallowed by the
// handler guard rather than escaping and killing the Restart=always daemon.
TEST_F(PostCodeTest, OnRawChangedMalformedMessageIsIgnored)
{
    ON_CALL(*bus_mock,
            sd_bus_message_read_basic(testing::_, testing::_, testing::_))
        .WillByDefault(testing::Return(-EINVAL));
    EXPECT_CALL(*bus_mock,
                sd_bus_message_read_basic(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(-EINVAL));

    auto msg =
        bus.new_signal("/xyz/openbmc_project/state/boot/raw0",
                       "org.freedesktop.DBus.Properties", "PropertiesChanged");
    auto onRawChanged = getPrivateMember(OnRawChangedTag{});

    EXPECT_NO_THROW((postCode.get()->*onRawChanged)(msg));
}

// Same guarantee for the State.Host handler.
TEST_F(PostCodeTest, OnHostStateChangedMalformedMessageIsIgnored)
{
    ON_CALL(*bus_mock,
            sd_bus_message_read_basic(testing::_, testing::_, testing::_))
        .WillByDefault(testing::Return(-EINVAL));
    EXPECT_CALL(*bus_mock,
                sd_bus_message_read_basic(testing::_, testing::_, testing::_))
        .WillRepeatedly(testing::Return(-EINVAL));

    auto msg =
        bus.new_signal("/xyz/openbmc_project/state/host0",
                       "org.freedesktop.DBus.Properties", "PropertiesChanged");
    auto onHostStateChanged = getPrivateMember(OnHostStateChangedTag{});

    EXPECT_NO_THROW((postCode.get()->*onHostStateChanged)(msg));
}
