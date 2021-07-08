#pragma once

#include <spdlog/common.h>

#include "Signalling/Config.h"


struct StreamerConfig
{
    enum class Type {
        Test,
        ReStreamer,
    };

    Type type;
    std::string uri;
    std::string description;
};

struct Config : public signalling::Config
{
    spdlog::level::level_enum logLevel = spdlog::level::info;
    spdlog::level::level_enum lwsLogLevel = spdlog::level::warn;

    bool allowClientUrls = true;

    std::string stunServer;
    std::string turnServer;

    std::map<std::string, StreamerConfig> streamers;
};
