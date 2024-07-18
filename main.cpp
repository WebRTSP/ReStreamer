#include <optional>
#include <deque>
#include <set>

#include <glib.h>

#include <libwebsockets.h>

#include <CxxPtr/CPtr.h>
#include <CxxPtr/GlibPtr.h>
#include "CxxPtr/libconfigDestroy.h"

#include "Helpers/ConfigHelpers.h"
#include "Helpers/LwsLog.h"

#include "Http/Log.h"
#include "Http/Config.h"

#include "RtStreaming/GstRtStreaming/Log.h"
#include "RtStreaming/GstRtStreaming/LibGst.h"

#include "Signalling/Log.h"

#include "Client/Log.h"

#include "Log.h"
#include "ReStreamer.h"


static const auto Log = ReStreamerLog;

#if BUILD_AS_CAMERA_STREAMER
#define CONFIG_FILE "camera-streamer.conf"
#elif BUILD_AS_V4L2_STREAMER
#define CONFIG_FILE "v4l2-streamer.conf"
#else
#define CONFIG_FILE "restreamer.conf"
#endif

static bool LoadConfig(http::Config* httpConfig, Config* config, const gchar* basePath)
{
    const std::deque<std::string> configDirs = ::ConfigDirs();
    if(configDirs.empty())
        return false;

    const std::string rootPath = "/";
    bool hasPublicStreamers = false;
    std::set<std::string> autoVisibilityStreamers;

    http::Config loadedHttpConfig = *httpConfig;
    Config loadedConfig = *config;

#if BUILD_AS_CAMERA_STREAMER || BUILD_AS_V4L2_STREAMER
    StreamerConfig streamerConfig;
    streamerConfig.visibility = StreamerConfig::Visibility::Auto;
#endif

#if BUILD_AS_CAMERA_STREAMER
    streamerConfig.type = StreamerConfig::Type::Camera;
#elif BUILD_AS_V4L2_STREAMER
    streamerConfig.type = StreamerConfig::Type::V4L2;
#endif

    for(const std::string& configDir: configDirs) {
        const std::string configFile = configDir + "/" CONFIG_FILE;
        if(!g_file_test(configFile.c_str(), G_FILE_TEST_IS_REGULAR)) {
            Log()->info("Config \"{}\" not found", configFile);
            continue;
        }

        config_t config;
        config_init(&config);
        ConfigDestroy ConfigDestroy(&config);

        Log()->info("Loading config \"{}\"", configFile);
        if(!config_read_file(&config, configFile.c_str())) {
            Log()->error("Fail load config. {}. {}:{}",
                config_error_text(&config),
                configFile,
                config_error_line(&config));
            return false;
        }

        const char* wwwRoot = nullptr;
        if(CONFIG_TRUE == config_lookup_string(&config, "www-root", &wwwRoot)) {
            loadedHttpConfig.wwwRoot = wwwRoot;
        }

        int wsPort = 0;
        if(CONFIG_TRUE == config_lookup_int(&config, "ws-port", &wsPort)) {
            loadedConfig.port = static_cast<unsigned short>(wsPort);
        } else if(CONFIG_TRUE == config_lookup_int(&config, "port", &wsPort)) { // for backward compatibility
            loadedConfig.port = static_cast<unsigned short>(wsPort);
        } else {
            loadedConfig.port = 0;
        }

        int loopbackOnly = false;
        if(CONFIG_TRUE == config_lookup_bool(&config, "loopback-only", &loopbackOnly)) {
            loadedHttpConfig.bindToLoopbackOnly = loopbackOnly != false;
            loadedConfig.bindToLoopbackOnly = loopbackOnly != false;
        }

        int httpPort = 0;
        if(CONFIG_TRUE == config_lookup_int(&config, "http-port", &httpPort)) {
            loadedHttpConfig.port = static_cast<unsigned short>(httpPort);
        } else {
            loadedHttpConfig.port = 0;
        }

        const char* stunServer = nullptr;
        const char* turnServer = nullptr;
        config_setting_t* webrtcConfig = config_lookup(&config, "webrtc");
        if(webrtcConfig && CONFIG_TRUE == config_setting_is_group(webrtcConfig)) {
            if(CONFIG_TRUE == config_setting_lookup_string(webrtcConfig, "stun-server", &stunServer)) {
                if(0 == g_ascii_strncasecmp(stunServer, "stun://", 7)) {
                    loadedConfig.webRTCConfig->iceServers.emplace_back(stunServer);
                } else {
                    Log()->error("STUN server URL should start with \"stun://\"");
                }
            }

            if(CONFIG_TRUE == config_setting_lookup_string(webrtcConfig, "turn-server", &turnServer)) {
               if(0 == g_ascii_strncasecmp(turnServer, "turn://", 7)) {
                    loadedConfig.webRTCConfig->iceServers.emplace_back(turnServer);
                } else {
                    Log()->error("TURN server URL should start with \"turn://\"");
               }
            }

            int minRtpPort;
            int rtpPortsCount;
            if(CONFIG_TRUE == config_setting_lookup_int(webrtcConfig, "min-rtp-port", &minRtpPort)) {
                if(minRtpPort < std::numeric_limits<uint16_t>::min() || minRtpPort > std::numeric_limits<uint16_t>::max()) {
                    Log()->error(
                        "min-rtp-port should be in [{}, {}]",
                        std::numeric_limits<uint16_t>::min(),
                        std::numeric_limits<uint16_t>::max());
                } else {
                    loadedConfig.webRTCConfig->minRtpPort = minRtpPort;

                    if(CONFIG_TRUE == config_setting_lookup_int(webrtcConfig, "rtp-ports-count", &rtpPortsCount)) {
                        if(minRtpPort < 1 || minRtpPort > std::numeric_limits<uint16_t>::max()) {
                            Log()->error(
                                "rtp-ports-count should be in [{}, {}]",
                                1,
                                std::numeric_limits<uint16_t>::max() - minRtpPort + 1);
                        } else {
                            loadedConfig.webRTCConfig->maxRtpPort = minRtpPort + rtpPortsCount - 1;
                        }
                    }
                }
            }
        } else { // Deprecated. For backward compatibility
            config_setting_t* stunServerConfig = config_lookup(&config, "stun");
            if(stunServerConfig && CONFIG_TRUE == config_setting_is_group(stunServerConfig)) {
                if(CONFIG_TRUE == config_setting_lookup_string(stunServerConfig, "server", &stunServer)) {
                    if(0 == g_ascii_strncasecmp(stunServer, "stun://", 7)) {
                        loadedConfig.webRTCConfig->iceServers.emplace_back(stunServer);
                    } else {
                        Log()->error("STUN server URL should start with \"stun://\"");
                    }
                }
            }

            if(CONFIG_TRUE == config_lookup_string(&config, "stun-server", &stunServer)) {
                if(0 == g_ascii_strncasecmp(stunServer, "stun://", 7)) {
                    loadedConfig.webRTCConfig->iceServers.emplace_back(stunServer);
                } else {
                    Log()->error("STUN server URL should start with \"stun://\"");
                }
            }
            if(CONFIG_TRUE == config_lookup_string(&config, "turn-server", &turnServer)) {
               if(0 == g_ascii_strncasecmp(turnServer, "turn://", 7)) {
                    loadedConfig.webRTCConfig->iceServers.emplace_back(turnServer);
                } else {
                    Log()->error("TURN server URL should start with \"turn://\"");
               }
            }
        }

        config_setting_t* agentsConfig = config_lookup(&config, "agents");
        if(agentsConfig && CONFIG_TRUE == config_setting_is_group(agentsConfig)) {
            const char* agentsStunServer = nullptr;
            if(CONFIG_TRUE == config_setting_lookup_string(agentsConfig, "stun-server", &agentsStunServer)) {
                if(0 == g_ascii_strncasecmp(agentsStunServer, "stun://", 7)) {
                    loadedConfig.agentsConfig.iceServers.emplace_back(agentsStunServer);
                } else {
                    Log()->error("STUN server URL should start with \"stun://\"");
                }
            }

            const char* agentsTurnServer = nullptr;
            if(CONFIG_TRUE == config_setting_lookup_string(agentsConfig, "turn-server", &agentsTurnServer)) {
               if(0 == g_ascii_strncasecmp(agentsTurnServer, "turn://", 7)) {
                    loadedConfig.agentsConfig.iceServers.emplace_back(agentsTurnServer);
                } else {
                    Log()->error("TURN server URL should start with \"turn://\"");
               }
            }
        }

        config_setting_t* debugConfig = config_lookup(&config, "debug");
        if(debugConfig && CONFIG_TRUE == config_setting_is_group(debugConfig)) {
            int logLevel = 0;
            if(CONFIG_TRUE == config_setting_lookup_int(debugConfig, "log-level", &logLevel)) {
                if(logLevel > 0) {
                    loadedConfig.logLevel =
                        static_cast<spdlog::level::level_enum>(
                            spdlog::level::critical - std::min<int>(logLevel, spdlog::level::critical));
                }
            }
            int lwsLogLevel = 0;
            if(CONFIG_TRUE == config_setting_lookup_int(debugConfig, "lws-log-level", &lwsLogLevel)) {
                if(lwsLogLevel > 0) {
                    loadedConfig.lwsLogLevel =
                        static_cast<spdlog::level::level_enum>(
                            spdlog::level::critical - std::min<int>(lwsLogLevel, spdlog::level::critical));
                }
            }
        }

        config_setting_t* signallingServerConfig = config_lookup(&config, "signalling-server");
        if(signallingServerConfig && CONFIG_TRUE == config_setting_is_group(signallingServerConfig)) {
            const char* host = nullptr;
            const char* uri = nullptr;
            const char* token = nullptr;
            int useTls = TRUE;
            config_setting_lookup_string(signallingServerConfig, "host", &host);
            config_setting_lookup_string(signallingServerConfig, "uri", &uri);
            config_setting_lookup_string(signallingServerConfig, "token", &token);
            config_setting_lookup_bool(signallingServerConfig, "tls", &useTls);
            if(host && uri) {
                g_autofree gchar* escapedUri = g_uri_escape_string(uri, nullptr, false);
                SignallingServer signallingServer(host, escapedUri, token, useTls);

                int port = 0;
                config_setting_lookup_int(signallingServerConfig, "port", &port);
                if(port > 0 )
                    signallingServer.serverPort = port;

                loadedConfig.signallingServer = signallingServer;
            }
        }

#if !BUILD_AS_CAMERA_STREAMER && !BUILD_AS_V4L2_STREAMER
        config_setting_t* streamersConfig = config_lookup(&config, "streamers");
        if(streamersConfig && CONFIG_TRUE == config_setting_is_list(streamersConfig)) {
            const int streamersCount = config_setting_length(streamersConfig);
            for(int streamerIdx = 0; streamerIdx < streamersCount; ++streamerIdx) {
                config_setting_t* streamerConfig =
                    config_setting_get_elem(streamersConfig, streamerIdx);
                if(!streamerConfig || CONFIG_FALSE == config_setting_is_group(streamerConfig)) {
                    Log()->warn("Wrong streamer config format. Streamer skipped.");
                    break;
                }
                const char* name;
                if(CONFIG_FALSE == config_setting_lookup_string(streamerConfig, "name", &name)) {
                    Log()->warn("Missing streamer name. Streamer skipped.");
                    break;
                }

                int restream = TRUE;
                config_setting_lookup_bool(streamerConfig, "restream", &restream);

                const char* agentToken = "";
                config_setting_lookup_string(streamerConfig, "record-token", &agentToken);
                config_setting_lookup_string(streamerConfig, "agent-token", &agentToken);

                const char* type = nullptr;
                config_setting_lookup_string(streamerConfig, "type", &type);

                StreamerConfig::Visibility visibility = StreamerConfig::Visibility::Auto;
                int isPublic = false;
                if(CONFIG_TRUE == config_setting_lookup_bool(streamerConfig, "public", &isPublic)) {
                    if(isPublic != FALSE) {
                        hasPublicStreamers = true;
                        visibility = StreamerConfig::Visibility::Public;
                    } else {
                        visibility = StreamerConfig::Visibility::Protected;
                    }
                } else {
                    autoVisibilityStreamers.emplace(name);
                }

                const char* description = nullptr;
                config_setting_lookup_string(streamerConfig, "description", &description);

                const char* forceH264ProfileLevelId = nullptr;
                config_setting_lookup_string(streamerConfig, "force-h264-profile-level-id", &forceH264ProfileLevelId);

                const char* uri = nullptr;
                if(CONFIG_FALSE == config_setting_lookup_string(streamerConfig, "uri", &uri))
                   config_setting_lookup_string(streamerConfig, "url", &uri);

                const char* username = nullptr;
                config_setting_lookup_string(streamerConfig, "username", &username);

                const char* password = nullptr;
                config_setting_lookup_string(streamerConfig, "password", &password);

                const char* dir = nullptr;
                config_setting_lookup_string(streamerConfig, "dir", &dir);

                const char* pipeline = nullptr;
                if(CONFIG_TRUE == config_setting_lookup_string(streamerConfig, "pipeline", &pipeline)) {
                   type = "pipeline";
                }

                StreamerConfig::Type streamerType;
                if((type == nullptr && dir != nullptr) || 0 == g_strcmp0(type, "player"))
                    streamerType = StreamerConfig::Type::FilePlayer;
                else if(type == nullptr || 0 == strcmp(type, "restreamer"))
                    streamerType = StreamerConfig::Type::ReStreamer;
#if ONVIF_SUPPORT
                else if(0 == strcmp(type, "onvif"))
                    streamerType = StreamerConfig::Type::ONVIFReStreamer;
#endif
                else if(0 == strcmp(type, "test"))
                    streamerType = StreamerConfig::Type::Test;
                else if(0 == strcmp(type, "record"))
                    streamerType = StreamerConfig::Type::Record;
                else if(0 == strcmp(type, "player"))
                    streamerType = StreamerConfig::Type::FilePlayer;
                else if(0 == strcmp(type, "proxy"))
                    streamerType = StreamerConfig::Type::Proxy;
                else if(0 == strcmp(type, "pipeline"))
                    streamerType = StreamerConfig::Type::Pipeline;
                else if(0 == strcmp(type, "camera"))
                    streamerType = StreamerConfig::Type::Camera;
                else if(0 == strcmp(type, "v4l2"))
                    streamerType = StreamerConfig::Type::V4L2;
                else {
                    Log()->warn("Unknown streamer type. Streamer skipped.");
                    break;
                }

                if(streamerType != StreamerConfig::Type::Record &&
                   streamerType != StreamerConfig::Type::FilePlayer &&
                   streamerType != StreamerConfig::Type::Proxy &&
                   streamerType != StreamerConfig::Type::Pipeline &&
                   streamerType != StreamerConfig::Type::Camera &&
                   streamerType != StreamerConfig::Type::V4L2 &&
                   !uri)
                {
                    Log()->warn("Missing streamer uri. Streamer skipped.");
                    break;
                }
                if(streamerType == StreamerConfig::Type::FilePlayer && !dir) {
                    Log()->warn("Missing player source dir. Streamer skipped.");
                    break;
                }

                const char* recordingsDir = nullptr;
                config_setting_lookup_string(streamerConfig, "recordings-dir", &recordingsDir);

                if(streamerType == StreamerConfig::Type::Record && !recordingsDir) {
                    Log()->warn("Missing recordings target dir. Streamer skipped.");
                    break;
                }

                int recordingsDirMaxSize = 1024;
                config_setting_lookup_int(streamerConfig, "recordings-dir-max-size", &recordingsDirMaxSize);
                if(recordingsDirMaxSize < 0) recordingsDirMaxSize = 0;

                int recordingChunkSize = 100;
                config_setting_lookup_int(streamerConfig, "recording-chunk-size", &recordingChunkSize);
                if(recordingChunkSize < 0) recordingChunkSize = 0;

                std::optional<RecordConfig> recordConfig;
                if(streamerType == StreamerConfig::Type::Record) {
                    g_autofree gchar* recorderDir = g_uri_escape_string(name, " ", false);
                    recordConfig.emplace(
                        !basePath || g_path_is_absolute(recordingsDir) != FALSE ?
                            std::filesystem::path(recordingsDir) / recorderDir :
                            std::filesystem::path(basePath) / recordingsDir / recorderDir,
                        recordingsDirMaxSize * (1ull << 20),
                        recordingChunkSize * (1ull << 20));
                }

                std::string streamerUri;
                if(streamerType == StreamerConfig::Type::FilePlayer) {
                    streamerUri =
                        (!basePath || g_path_is_absolute(dir) != FALSE ?
                            std::filesystem::path(GCharPtr(g_canonicalize_filename(dir, nullptr)).get()) :
                            std::filesystem::path(basePath) / dir).string();
                } else if(uri){
                    streamerUri = uri;
                }

                g_autofree gchar* escapedName = g_uri_escape_string(name, nullptr, false);
                loadedConfig.streamers.emplace(
                    escapedName,
                    StreamerConfig {
                        streamerType,
                        restream != FALSE,
                        visibility,
                        streamerUri,
                        pipeline ? pipeline : std::string(),
                        username ?
                            std::make_optional<std::string>(username) :
                            std::optional<std::string>(),
                        password ?
                            std::make_optional<std::string>(password) :
                            std::optional<std::string>(),
                        agentToken,
                        description ?
                            std::string(description) :
                            std::string(),
                        forceH264ProfileLevelId ?
                            std::string(forceH264ProfileLevelId) :
                            std::string(),
                        recordConfig });


                if(visibility != StreamerConfig::Visibility::Auto) {
                    loadedHttpConfig.indexPaths.emplace(
                        rootPath + name,
                        visibility == StreamerConfig::Visibility::Protected);
                }
            }
        }
#endif

#if BUILD_AS_CAMERA_STREAMER
        config_setting_t* cameraConfig = config_lookup(&config, "camera");
        if(cameraConfig && CONFIG_TRUE == config_setting_is_group(cameraConfig)) {
            streamerConfig.cameraConfig = CameraConfig();

            int width;
            int height;
            const bool widthPresent =
                CONFIG_TRUE == config_setting_lookup_int(cameraConfig, "width", &width);
            const bool heightPresent =
                CONFIG_TRUE == config_setting_lookup_int(cameraConfig, "height", &height);

            if(widthPresent && heightPresent) {
                if(width <= 0)
                    Log()->warn("\"width\" should be positive. Skipped.");
                else if(height <= 0 )
                    Log()->warn("\"height\" should be positive. Skipped.");
                else {
                    streamerConfig.cameraConfig->resolution =
                        CameraConfig::Resolution {
                            static_cast<unsigned>(width),
                            static_cast<unsigned>(height) };
                }
            } else if(widthPresent != heightPresent) {
                Log()->warn("Both \"width\" and \"height\" should be present. Skipped.");
            }

            int framerate;
            if(CONFIG_TRUE == config_setting_lookup_int(cameraConfig, "framerate", &framerate)) {
                if(framerate > 0)
                    streamerConfig.cameraConfig->framerate = framerate;
                else
                    Log()->warn("\"framerate\" should be positive. Skipped.");
            }
        }
#elif BUILD_AS_V4L2_STREAMER
        const char* edidFilePath;
        if(CONFIG_TRUE == config_lookup_string(&config, "edid-file", &edidFilePath)) {
            streamerConfig.edidFilePath = !basePath || g_path_is_absolute(edidFilePath) != FALSE ?
                std::filesystem::path(edidFilePath) :
                std::filesystem::path(basePath) / edidFilePath;
        }
#endif

#if BUILD_AS_CAMERA_STREAMER || BUILD_AS_V4L2_STREAMER
        int useHwEncoder;
        if(CONFIG_TRUE == config_lookup_bool(&config, "use-hw-encoder", &useHwEncoder)) {
            streamerConfig.useHwEncoder = useHwEncoder != FALSE;
        }
#endif

        const char* realm = nullptr;
        if(CONFIG_TRUE == config_lookup_string(&config, "realm", &realm)) {
            loadedHttpConfig.realm = realm;
        }

        config_setting_t* usersConfig = config_lookup(&config, "users");
        if(usersConfig && CONFIG_TRUE == config_setting_is_list(usersConfig)) {
            const int usersCount = config_setting_length(usersConfig);
            for(int userIdx = 0; userIdx < usersCount; ++userIdx) {
                config_setting_t* userConfig =
                    config_setting_get_elem(usersConfig, userIdx);
                if(!userConfig || CONFIG_FALSE == config_setting_is_group(userConfig)) {
                    Log()->warn("Wrong user config format. User skipped.");
                    break;
                }

                const char* login = nullptr;
                if(CONFIG_FALSE == config_setting_lookup_string(userConfig, "login", &login)) {
                    Log()->warn("Missing user login. User skipped.");
                    break;
                }

                const char* pass = nullptr;
                if(CONFIG_FALSE == config_setting_lookup_string(userConfig, "pass", &pass)) {
                    Log()->warn("Missing user password. User skipped.");
                    break;
                }

                loadedHttpConfig.passwd.emplace(login, pass);
            }
        }
    }

#if BUILD_AS_CAMERA_STREAMER
    loadedConfig.streamers.emplace(
        "Camera",
        streamerConfig);
#elif BUILD_AS_V4L2_STREAMER
    loadedConfig.streamers.emplace(
        "V4L2",
        streamerConfig);
#endif

    loadedConfig.authRequired = !loadedHttpConfig.passwd.empty();

    loadedHttpConfig.indexPaths.emplace(rootPath, loadedConfig.authRequired && !hasPublicStreamers);
    for(const std::string& streamerName: autoVisibilityStreamers) {
        loadedHttpConfig.indexPaths.emplace(rootPath + streamerName, loadedConfig.authRequired);
    }

    bool success = true;

    if(!loadedConfig.port) {
        Log()->error("\"ws-port\" config value should be specified");
        success = false;
    }

    if(success) {
        *httpConfig = loadedHttpConfig;
        *config = loadedConfig;
    }

    return success;
}

int main(int argc, char *argv[])
{
    http::Config httpConfig {};
    httpConfig.bindToLoopbackOnly = false;

    const gchar* basePath = nullptr;

#ifdef SNAPCRAFT_BUILD
    const gchar* snapPath = g_getenv("SNAP");
    const gchar* snapName = g_getenv("SNAP_NAME");
    if(snapPath && snapName) {
        GCharPtr wwwRootPtr(g_build_path(G_DIR_SEPARATOR_S, snapPath, "opt", snapName, "www", NULL));
        httpConfig.wwwRoot = wwwRootPtr.get();
    }

    basePath = g_getenv("SNAP_COMMON");
#endif

    Config config {};
    config.bindToLoopbackOnly = false;
    if(!LoadConfig(&httpConfig, &config, basePath))
        return -1;

#ifdef SNAPCRAFT_BUILD
    const gchar* snapCommon = g_getenv("SNAP_COMMON");
    if(!g_path_is_absolute(httpConfig.wwwRoot.c_str()) && snapCommon) {
        GCharPtr wwwRootPtr(g_build_path(G_DIR_SEPARATOR_S, snapCommon, httpConfig.wwwRoot.c_str(), NULL));
        httpConfig.wwwRoot = wwwRootPtr.get();
    }
#endif

    InitLwsLogger(config.lwsLogLevel);
    InitHttpServerLogger(config.logLevel);
    InitWsServerLogger(config.logLevel);
    InitWsClientLogger(config.logLevel);
    InitClientSessionLogger(config.logLevel);
    InitServerSessionLogger(config.logLevel);
    InitGstRtStreamingLogger(config.logLevel);
    InitReStreamerLogger(config.logLevel);

    LibGst libGst;

    return ReStreamerMain(httpConfig, config, true);
}
