#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <chrono>
#include <filesystem>

#include <spdlog/common.h>

#include "Client/Config.h"
#include "Signalling/Config.h"

#include "RtStreaming/WebRTCConfig.h"


struct SignallingServer : public client::Config
{
    SignallingServer(
        const std::string& server,
        const std::string& uri,
        const std::string& token,
        bool useTls) :
        client::Config {server, signalling::Config().port, useTls},
        uri(uri),
        token(token)
    {
    }

    std::string uri;
    std::string token;
};

struct CameraConfig
{
    struct Resolution {
        unsigned width;
        unsigned height;
    };
    std::optional<Resolution> resolution;
    std::optional<unsigned> framerate;
};

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
#if ONVIF_SUPPORT
        ONVIFReStreamer,
#endif
        Record,
        FilePlayer,
        Proxy,
        Pipeline,
        Camera,
        V4L2,
    };

    enum class Visibility {
        Auto, // Protected if authentication is required, Public if not.
        Public,
        Protected, // Accessible only for authenticated users
    };

    Type type;
    bool restream = true;
    Visibility visibility = Visibility::Protected;
    std::string uri;
    std::string pipeline;
    std::optional<std::string> username;
    std::optional<std::string> password;
    std::string remoteAgentToken;
    std::string description;
    std::string forceH264ProfileLevelId;
    std::optional<RecordConfig> recordConfig;
    std::optional<std::string> edidFilePath;
    std::optional<CameraConfig> cameraConfig;
    bool useHwEncoder = true;
};

struct AgentsConfig
{
    WebRTCConfig::IceServers iceServers;

#ifdef SNAPCRAFT_BUILD
    bool useCoturn = true;
#else
    bool useCoturn = false;
#endif
};

struct CoturnConfig
{
    std::string realm = "WebRTSP/ReStreamer";
    uint16_t port = 3478;
    std::optional<std::string> staticAuthSecret;
    std::chrono::seconds passwordTTL = std::chrono::hours(1);
};

struct Config : public signalling::Config
{
    spdlog::level::level_enum logLevel = spdlog::level::info;
    spdlog::level::level_enum lwsLogLevel = spdlog::level::warn;

    std::optional<SignallingServer> signallingServer;

    std::map<std::string, StreamerConfig> streamers; // escaped streamer name -> StreamerConfig
    bool authRequired = true;

    std::shared_ptr<WebRTCConfig> webRTCConfig = std::make_shared<WebRTCConfig>();

    std::optional<std::string> publicIp;

    AgentsConfig agentsConfig;

    CoturnConfig coturnConfig;

    bool useAgentMode() const { return signallingServer.has_value(); }
    bool useServerMode() const { return !signallingServer.has_value(); }
};
