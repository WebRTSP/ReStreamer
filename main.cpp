#include <deque>

#include <glib.h>

#include <libwebsockets.h>

#include <CxxPtr/CPtr.h>
#include "CxxPtr/libconfigDestroy.h"

#include "Helpers/ConfigHelpers.h"
#include "Helpers/LwsLog.h"

#include "Http/Log.h"
#include "Http/Config.h"

#include "RtStreaming/GstRtStreaming/LibGst.h"

#include "Signalling/Log.h"

#include "Log.h"
#include "ReStreamer.h"


static const auto Log = ReStreamerLog;


static bool LoadConfig(Config* config)
{
    const std::deque<std::string> configDirs = ::ConfigDirs();
    if(configDirs.empty())
        return false;

    Config loadedConfig = *config;

    for(const std::string& configDir: configDirs) {
        const std::string configFile = configDir + "/restreamer.conf";
        if(!g_file_test(configFile.c_str(),  G_FILE_TEST_IS_REGULAR)) {
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

        const char* host = nullptr;
        if(CONFIG_TRUE == config_lookup_string(&config, "host", &host)) {
            loadedConfig.serverName = host;
        }
        int port= 0;
        if(CONFIG_TRUE == config_lookup_int(&config, "port", &port)) {
            loadedConfig.port = static_cast<unsigned short>(port);
        }
        int loopbackOnly = false;
        if(CONFIG_TRUE == config_lookup_bool(&config, "loopback-only", &loopbackOnly)) {
            loadedConfig.bindToLoopbackOnly = loopbackOnly != false;
        }
        int securePort= 0;
        if(CONFIG_TRUE == config_lookup_int(&config, "secure-port", &port)) {
            loadedConfig.securePort = static_cast<unsigned short>(securePort);
        }
        const char* certificate = nullptr;
        if(CONFIG_TRUE == config_lookup_string(&config, "certificate", &certificate)) {
            loadedConfig.certificate = FullPath(configDir, certificate);
        }
        const char* privateKey = nullptr;
        if(CONFIG_TRUE == config_lookup_string(&config, "key", &privateKey)) {
            loadedConfig.key = FullPath(configDir, privateKey);
        }
        int allowClientUrls = false;
        if(CONFIG_TRUE == config_lookup_bool(&config, "allow-client-urls", &allowClientUrls)) {
            loadedConfig.allowClientUrls = allowClientUrls != false;
        }

        config_setting_t* stunServerConfig = config_lookup(&config, "stun");
        if(stunServerConfig && CONFIG_TRUE == config_setting_is_group(stunServerConfig)) {
            const char* server = nullptr;
            if(CONFIG_TRUE == config_setting_lookup_string(stunServerConfig, "server", &server)) {
                loadedConfig.stunServer = server;
            }
        }

        const char* stunServer = nullptr;
        if(CONFIG_TRUE == config_lookup_string(&config, "stun-server", &stunServer)) {
            loadedConfig.stunServer = stunServer;
        }

        const char* turnServer = nullptr;
        if(CONFIG_TRUE == config_lookup_string(&config, "turn-server", &turnServer)) {
            loadedConfig.turnServer = turnServer;
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

                const char* type = nullptr;
                config_setting_lookup_string(streamerConfig, "type", &type);

                const char* uri;
                if(CONFIG_FALSE == config_setting_lookup_string(streamerConfig, "uri", &uri) &&
                   CONFIG_FALSE == config_setting_lookup_string(streamerConfig, "url", &uri))
                {
                    Log()->warn("Missing streamer uri. Streamer skipped.");
                    break;
                }
                const char* description = nullptr;
                config_setting_lookup_string(streamerConfig, "description", &description);

                StreamerConfig::Type streamerType;
                if(nullptr == type || 0 == strcmp(type, "restreamer"))
                    streamerType = StreamerConfig::Type::ReStreamer;
                else if(0 == strcmp(type, "test"))
                    streamerType = StreamerConfig::Type::Test;
                else {
                    Log()->warn("Unknown streamer type. Streamer skipped.");
                    break;
                }

                CharPtr escapedNamePtr(
                    g_uri_escape_string(name, nullptr, false));
                if(!escapedNamePtr)
                    break; // insufficient memory?

                loadedConfig.streamers.emplace(
                    escapedNamePtr.get(),
                    StreamerConfig {
                        streamerType,
                        uri,
                        description ?
                            std::string(description) :
                            std::string() });
            }
        }
    }

    bool success = true;

    if(!loadedConfig.port) {
        Log()->error("Missing port");
        success = false;
    }

    if(!loadedConfig.stunServer.empty() &&
       g_ascii_strncasecmp(loadedConfig.stunServer.c_str(), "stun://", 7) != 0)
    {
        Log()->error("Stun server should start with \"stun://\"");
        success = false;
    }

    if(!loadedConfig.turnServer.empty() &&
       g_ascii_strncasecmp(loadedConfig.turnServer.c_str(), "turn://", 7) != 0)
    {
        Log()->error("TURN server URL should start with \"turn://\"");
        success = false;
    }

    if(success)
        *config = loadedConfig;

    return success;
}

int main(int argc, char *argv[])
{
    http::Config httpConfig {};
    httpConfig.bindToLoopbackOnly = false;
#ifdef SNAPCRAFT_BUILD
    const gchar* snapPath = g_getenv("SNAP");
    const gchar* snapName = g_getenv("SNAP_NAME");
    if(snapPath && snapName)
        httpConfig.wwwRoot = std::string(snapPath) + "/opt/" + snapName + "/www";
#endif
    Config config {};
    config.bindToLoopbackOnly = false;
    if(!LoadConfig(&config))
        return -1;

    InitLwsLogger(config.lwsLogLevel);
    InitHttpServerLogger(config.logLevel);
    InitWsServerLogger(config.logLevel);
    InitServerSessionLogger(config.logLevel);
    InitReStreamerLogger(config.logLevel);

    LibGst libGst;

    return ReStreamerMain(httpConfig, config);
}
