#pragma once

#include <deque>

#include <spdlog/common.h>

#include "Signalling/Config.h"


struct StreamerConfig
{
    enum class Type {
        Test,
        ReStreamer,
        ONVIFReStreamer,
        Record,
    };

    Type type;
    std::string uri;
    std::string recordToken;
    std::string description;
    std::string forceH264ProfileLevelId;
};

struct Config : public signalling::Config
{
    spdlog::level::level_enum logLevel = spdlog::level::info;
    spdlog::level::level_enum lwsLogLevel = spdlog::level::warn;

    typedef std::deque<std::string> IceServers;
    IceServers iceServers;

    std::map<std::string, StreamerConfig> streamers;
    bool authRequired = true;
};
