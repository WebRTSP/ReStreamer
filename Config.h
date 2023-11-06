#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <filesystem>

#include <spdlog/common.h>

#include "Signalling/Config.h"


struct RecordConfig
{
    enum: uint64_t {
        MIN_MAX_DIR_SIZE = 100 * (1ull << 20), // 100 Mb
        MIN_MAX_FILE_SIZE = 10 * (1ull << 20), // 10 Mb
    };

    RecordConfig(const std::filesystem::path& dir, uint64_t maxDirSize, uint64_t maxFileSize) :
        dir(dir),
        maxDirSize(std::max<uint64_t>(maxDirSize, MIN_MAX_DIR_SIZE)),
        maxFileSize(std::max<uint64_t>(maxFileSize, MIN_MAX_FILE_SIZE))
    {}

    const std::filesystem::path dir;
    const uint64_t maxDirSize;
    const uint64_t maxFileSize;
};

struct StreamerConfig
{
    enum class Type {
        Test,
        ReStreamer,
        ONVIFReStreamer,
        Record,
        FilePlayer,
    };

    enum class Visibility {
        Auto, // Protected if authentication is required, Public if not.
        Public,
        Protected, // Accessible only for authenticated users
    };

    bool restream;
    Visibility visibility;
    Type type;
    std::string uri;
    std::optional<std::string> username;
    std::optional<std::string> password;
    std::string recordToken;
    std::string description;
    std::string forceH264ProfileLevelId;
    std::optional<RecordConfig> recordConfig;
};

struct Config : public signalling::Config
{
    spdlog::level::level_enum logLevel = spdlog::level::info;
    spdlog::level::level_enum lwsLogLevel = spdlog::level::warn;

    typedef std::deque<std::string> IceServers;
    IceServers iceServers;

    std::map<std::string, StreamerConfig> streamers; // escaped streamer name -> StreamerConfig
    bool authRequired = true;
};
