#include "ReStreamer.h"

#include <deque>
#include <map>
#include <string>
#include <chrono>

#include <CxxPtr/GlibPtr.h>
#include <CxxPtr/libwebsocketsPtr.h>

#include "Helpers/Actor.h"

#include "Http/HttpMicroServer.h"

#include "RtspParser/RtspParser.h"

#include "Signalling/WsServer.h"
#include "Signalling/WsClient.h"

#include "RtStreaming/GstRtStreaming/GstTestStreamer.h"
#include "RtStreaming/GstRtStreaming/GstReStreamer.h"

#include "RtStreaming/GstRtStreaming/GstTestStreamer2.h"
#include "RtStreaming/GstRtStreaming/GstReStreamer2.h"
#if ONVIF_SUPPORT
#include "RtStreaming/GstRtStreaming/ONVIF/ONVIFReStreamer.h"
#endif
#include "RtStreaming/GstRtStreaming/GstRecordStreamer.h"
#include "RtStreaming/GstRtStreaming/GstPipelineStreamer2.h"
#include "RtStreaming/GstRtStreaming/GstCameraStreamer.h"
#include "RtStreaming/GstRtStreaming/GstV4L2ReStreamer.h"

#include "Log.h"
#include "SessionsSharedData.h"
#if defined(BUILD_AS_CAMERA_STREAMER) || defined(BUILD_AS_V4L2_RESTREAMER)
#include "BasicServerSession.h"
#else
#include "ServerSession.h"
#include "AgentServerSession.h"
#endif
#include "AgentClientSession.h"

#if !defined(BUILD_AS_CAMERA_STREAMER) && !defined(BUILD_AS_V4L2_RESTREAMER)
#include "FileMonitor.h"
#endif


namespace {

const unsigned AuthTokenCleanupInterval = 15; // seconds

enum {
    MIN_RECONNECT_TIMEOUT = 3, // seconds
    MAX_RECONNECT_TIMEOUT = 10, // seconds
};

const auto Log = ReStreamerLog;

void OnNewAuthToken(
    SessionsSharedData* sessionsSharedData,
    const std::string& token,
    std::chrono::steady_clock::time_point expiresAt)
{
    sessionsSharedData->authTokens.emplace(token, SessionAuthTokenData { expiresAt });
}

void CleanupAuthTokens(SessionsSharedData* sessionsSharedData)
{
    const auto now = std::chrono::steady_clock::now();

    auto& authTokens = sessionsSharedData->authTokens;
    for(auto it = authTokens.begin(); it != authTokens.end();) {
        if(it->second.expiresAt < now)
            it = authTokens.erase(it);
        else
            ++it;
    }
}

void ScheduleAuthTokensCleanup(SessionsSharedData* sessionsSharedData) {
    GSourcePtr timeoutSourcePtr(g_timeout_source_new_seconds(AuthTokenCleanupInterval));
    GSource* timeoutSource = timeoutSourcePtr.get();
    g_source_set_callback(
        timeoutSource,
        [] (gpointer userData) -> gboolean {
            SessionsSharedData* sessionsSharedData = reinterpret_cast<SessionsSharedData*>(userData);
            CleanupAuthTokens(sessionsSharedData);
            return false;
        },
        sessionsSharedData,
        nullptr);
    GMainContext* threadContext = g_main_context_get_thread_default();
    g_source_attach(timeoutSource, threadContext ? threadContext : g_main_context_default());
}

void ClientDisconnected(const Config& config, WsClient& client)
{
    const unsigned reconnectTimeout =
        g_random_int_range(MIN_RECONNECT_TIMEOUT, MAX_RECONNECT_TIMEOUT + 1);
    Log()->info("Scheduling reconnect withing \"{}\" seconds...", reconnectTimeout);
    GSourcePtr timeoutSourcePtr(g_timeout_source_new_seconds(reconnectTimeout));
    GSource* timeoutSource = timeoutSourcePtr.get();
    struct UserData {
        const Config *const config;
        WsClient *const client;
    };
    g_source_set_callback(
        timeoutSource,
        [] (gpointer userData) -> gboolean {
            auto [config, client] = *static_cast<UserData*>(userData);

            if(config->signallingServer.has_value()) {
                client->connectAsAgent(
                    config->clientId,
                    config->signallingServer->agentId,
                    config->signallingServer->accessToken);
            }

            return false;
        },
        new UserData { .config = &config, .client = &client },
        [] (gpointer userData) { delete static_cast<UserData*>(userData); } );
    g_source_attach(timeoutSource, g_main_context_get_thread_default());
}

#if defined(BUILD_AS_CAMERA_STREAMER) || defined(BUILD_AS_V4L2_RESTREAMER)

typedef std::unique_ptr<GstStreamingSource> MountPoint;
std::unique_ptr<WebRTCPeer> CreatePeer(
    const Config* config,
    const MountPoint& mountPoint,
    const std::string& uri)
{
    return mountPoint ? mountPoint->createPeer() : nullptr;
}

struct ServerSessionFactory: public WsServer::SessionFactory
{
    ServerSessionFactory(
        const Config* config,
        SessionsSharedData* sharedData,
        const MountPoint& mountPoint
    ) : config(config), sharedData(sharedData), mountPoint(mountPoint) {}

    std::unique_ptr<rtsp::Session> createSession(
        std::optional<std::string>&& authCookie,
        const rtsp::Session::SendRequest& sendRequest,
        const rtsp::Session::SendResponse& sendResponse) noexcept override
    {
        return std::make_unique<BasicServerSession>(
            config,
            sharedData,
            authCookie,
            [this] (const std::string& uri) {
                return CreatePeer(config, mountPoint, uri);
            },
            sendRequest,
            sendResponse);
    }

private:
    const Config *const config;
    SessionsSharedData *const sharedData;
    const MountPoint& mountPoint;
};

struct ClientSessionFactory: public WsClient::SessionFactory
{
    ClientSessionFactory(
        const Config* config,
        SessionsSharedData* sharedData,
        const MountPoint& mountPoint
    ) : config(config), sharedData(sharedData), mountPoint(mountPoint) {}

    std::unique_ptr<rtsp::Session> createAgentSession(
        const std::string& /*clientId*/,
        std::string&& agentId,
        std::string&& /*accessToken*/,
        const rtsp::Session::SendRequest& sendRequest,
        const rtsp::Session::SendResponse& sendResponse) noexcept override
    {
        return
            std::make_unique<AgentClientSession>(
                config,
                sharedData,
                std::move(agentId),
                [this] (const std::string& uri) {
                    return CreatePeer(config, mountPoint, uri);
                },
                sendRequest,
                sendResponse);
    };

private:
    const Config *const config;
    SessionsSharedData *const sharedData;
    const MountPoint& mountPoint;
};

#else

enum class ListType {
    Public,
    Protected,
    Agent,
};

std::string GenerateList(const Config& config, ListType type) {
    const bool addPublicOnly = (type == ListType::Public) || (type == ListType::Agent);
    const bool skipProxy = type == ListType::Agent;
    std::string list;
    if(config.streamers.empty()) {
        list = "\r\n";
    } else {
        for(const auto& pair: config.streamers) {
            typedef StreamerConfig::Visibility Visibility;
            const bool isPublic =
                (pair.second.visibility == Visibility::Auto && !config.authRequired) ||
                pair.second.visibility == Visibility::Public;

            if(!pair.second.restream ||
                (addPublicOnly && !isPublic) ||
                (skipProxy && pair.second.type == StreamerConfig::Type::Proxy))
            {
                continue;
            }

            list += pair.first;
            list += ": ";
            list += pair.second.description;
            list += + "\r\n";
        }
    }

    return list;
}

typedef std::map<std::string, std::unique_ptr<GstStreamingSource>, std::less<>> MountPoints;

std::unique_ptr<WebRTCPeer> CreatePeer(
    const Config* config,
    MountPoints* mountPoints,
    const std::string& uri)
{
    const auto [streamerName, substreamName] = rtsp::SplitUri(uri);

    auto configStreamerIt = config->streamers.find(streamerName);
    if(configStreamerIt == config->streamers.end() || !configStreamerIt->second.restream)
        return nullptr;

    const StreamerConfig& streamerConfig = configStreamerIt->second;
    if(configStreamerIt->second.type == StreamerConfig::Type::FilePlayer) {
        g_autofree gchar* unEscapedSubstreamName =
            g_uri_unescape_string(std::string(substreamName).c_str(), nullptr);
        g_autofree gchar* reEscapedSubstreamName =
            g_uri_escape_string(unEscapedSubstreamName, " ()", false);

        GCharPtr fullPathPtr(g_build_filename(streamerConfig.uri.c_str(), reEscapedSubstreamName, nullptr));
        GCharPtr safePathPtr(g_canonicalize_filename(fullPathPtr.get(), nullptr));
        if(!g_str_has_prefix(safePathPtr.get(), streamerConfig.uri.c_str())) {
            Log()->error("Try to escape from file player dir detected: {}\n", uri);
            return nullptr;
        }

        GCharPtr fileUriPtr(g_filename_to_uri(safePathPtr.get(), nullptr, nullptr));
        if(!fileUriPtr) {
            Log()->error("Failed to create uri for {}\n", safePathPtr.get());
            return nullptr;
        }

        return std::make_unique<GstReStreamer>(fileUriPtr.get(), streamerConfig.forceH264ProfileLevelId);
    } else {
        auto streamerIt = mountPoints->find(streamerName);
        if(streamerIt != mountPoints->end()) {
            return streamerIt->second->createPeer();
        } else
            return nullptr;
    }
}

std::unique_ptr<WebRTCPeer> CreateRecordPeer(
    const Config* config,
    MountPoints* mountPoints,
    const std::string& uri)
{
    auto streamerIt = mountPoints->find(uri);
    if(streamerIt != mountPoints->end()) {
        return streamerIt->second->createRecordPeer();
    } else
        return nullptr;
}

struct AgentsDb: public WsServer::AgentsDb
{
    AgentsDb(const Config* config) : config(config) {}

    std::optional<AgentCredentials>
        registerAgent(const std::string& clientId) noexcept override
        { return {}; }; // dynamic agents not supported

    bool authenticateAgent(
        const std::string& clientId,
        const std::string& agentId,
        const std::string& accessToken) noexcept override;

private:
    const Config *const config;
};

bool AgentsDb::authenticateAgent(
    const std::string& clientId,
    const std::string& agentId,
    const std::string& accessToken) noexcept
{
    auto it = config->streamers.find(agentId);
    if(it == config->streamers.end())
        return false;

    const StreamerConfig& streamerConfig = it->second;
    if(streamerConfig.type != StreamerConfig::Type::Proxy)
        return false;

    return streamerConfig.remoteAgentToken == accessToken;
}

struct ServerSessionFactory: public WsServer::SessionFactory
{
    ServerSessionFactory(
        const Config* config,
        SessionsSharedData* sharedData,
        MountPoints* mountPoints
    ) : config(config), sharedData(sharedData), mountPoints(mountPoints) {}

    std::unique_ptr<rtsp::Session> createSession(
        std::optional<std::string>&& authCookie,
        const rtsp::Session::SendRequest& sendRequest,
        const rtsp::Session::SendResponse& sendResponse) noexcept override
    {
        return std::make_unique<ServerSession>(
            config,
            sharedData,
            authCookie,
            [this] (const std::string& uri) {
                return CreatePeer(config, mountPoints, uri);
            },
            [this] (const std::string& uri) {
                return CreateRecordPeer(config, mountPoints, uri);
            },
            sendRequest,
            sendResponse);
    }

    std::unique_ptr<rtsp::Session> createAgentSession(
        std::string&& /*clientId*/,
        std::string&& agentId,
        const rtsp::Session::SendRequest& sendRequest,
        const rtsp::Session::SendResponse& sendResponse) noexcept override
    {
        return std::make_unique<AgentServerSession>(
            config,
            sharedData,
            std::move(agentId),
            sendRequest,
            sendResponse);
    }

private:
    const Config *const config;
    SessionsSharedData *const sharedData;
    MountPoints *const mountPoints;
};

struct ClientSessionFactory: public WsClient::SessionFactory
{
    ClientSessionFactory(
        const Config* config,
        SessionsSharedData* sharedData,
        MountPoints* mountPoints
    ) : config(config), sharedData(sharedData), mountPoints(mountPoints) {}

    std::unique_ptr<rtsp::Session> createAgentSession(
        const std::string& /*clientId*/,
        std::string&& agentId,
        std::string&& /*accessToken*/,
        const rtsp::Session::SendRequest& sendRequest,
        const rtsp::Session::SendResponse& sendResponse) noexcept override
    {
        return
            std::make_unique<AgentClientSession>(
                config,
                sharedData,
                std::move(agentId),
                [this] (const std::string& uri) {
                    return CreatePeer(config, mountPoints, uri);
                },
                sendRequest,
                sendResponse);
    };

private:
    const Config *const config;
    SessionsSharedData *const sharedData;
    MountPoints *const mountPoints;
};

void OnRecorderConnected(SessionsSharedData* sharedData, const std::string& uri)
{
    Log()->info("Recorder connected to \"{}\" streamer", uri);

    RecordMountpointData& data = sharedData->recordMountpointsData[uri];

    data.recording = true;

    std::unordered_map<rtsp::StreamSession*, rtsp::MediaSessionId> subscriptions;
    data.subscriptions.swap(subscriptions);
    for(auto& session2session: subscriptions) {
        rtsp::StreamSession* session = session2session.first;
        const rtsp::MediaSessionId& mediaSession = session2session.second;
        session->startRecordToClient(uri, mediaSession);
    }
}

void OnRecorderDisconnected(SessionsSharedData* sharedData, const std::string& uri)
{
    Log()->info("Recorder disconnected from \"{}\" streamer", uri);

    auto it = sharedData->recordMountpointsData.find(uri);
    if(it == sharedData->recordMountpointsData.end()) {
        return;
    }

    RecordMountpointData& data = it->second;
    data.recording = false;
    assert(data.subscriptions.empty());
}
#endif

}

int ReStreamerMain(
    const http::Config& httpConfig,
    const Config& config,
    bool useGlobalDefaultContext)
{
    GMainContextPtr contextPtr(
        useGlobalDefaultContext ?
            g_main_context_ref(g_main_context_default()) :
            g_main_context_new());
    GMainContext* context = contextPtr.get();
    if(!useGlobalDefaultContext) {
        g_main_context_push_thread_default(context);
    }

    GMainLoopPtr loopPtr(g_main_loop_new(context, FALSE));
    GMainLoop* loop = loopPtr.get();

    SessionsSharedData sessionsSharedData {
#if !defined(BUILD_AS_CAMERA_STREAMER) && !defined(BUILD_AS_V4L2_RESTREAMER)
        .publicListCache = GenerateList(config, ListType::Public),
        .protectedListCache = GenerateList(config, ListType::Protected),
        .agentListCache = GenerateList(config, ListType::Agent),
#endif
    };

    std::deque<RecordConfig> cleanupList;
    std::deque<std::pair<std::string, std::string>> monitorList;

#if defined(BUILD_AS_CAMERA_STREAMER) 
    const std::optional<CameraConfig>& cameraConfig = config.cameraConfig;
    std::optional<GstCameraStreamer::VideoResolution> resolution;
    std::optional<unsigned> framerate;
    if(cameraConfig) {
        if(cameraConfig->resolution) {
            resolution = GstCameraStreamer::VideoResolution {
                cameraConfig->resolution->width,
                cameraConfig->resolution->height };
        }
        framerate = cameraConfig->framerate;
    }
    MountPoint mountPoint = std::make_unique<GstCameraStreamer>(
        resolution,
        framerate,
        std::optional<std::string>(),
        config.useHwEncoder);
#elif defined(BUILD_AS_V4L2_RESTREAMER)
    MountPoint mountPoint = std::make_unique<GstV4L2ReStreamer>(
            config.edidFilePath,
            std::optional<GstV4L2ReStreamer::VideoResolution>(),
            std::optional<std::string>(),
            config.useHwEncoder);
#else
    MountPoints mountPoints;
    for(const auto& pair: config.streamers) {
        if((pair.second.type != StreamerConfig::Type::Record || !pair.second.recordConfig) && !pair.second.restream)
            continue;

        switch(pair.second.type) {
        case StreamerConfig::Type::Test:
            mountPoints.emplace(pair.first, std::make_unique<GstTestStreamer2>(pair.second.uri));
            break;
        case StreamerConfig::Type::ReStreamer:
            mountPoints.emplace(
                pair.first,
                std::make_unique<GstReStreamer2>(
                    pair.second.uri,
                    pair.second.forceH264ProfileLevelId));
            break;
#if ONVIF_SUPPORT
        case StreamerConfig::Type::ONVIFReStreamer:
            mountPoints.emplace(
                pair.first,
                std::make_unique<ONVIFReStreamer>(
                    pair.second.uri,
                    pair.second.forceH264ProfileLevelId,
                    pair.second.username,
                    pair.second.password));
            break;
#endif
        case StreamerConfig::Type::Record:
            if(pair.second.recordConfig) {
                cleanupList.push_back(*pair.second.recordConfig);
            }
            typedef GstRecordStreamer::RecordOptions RecordOptions;
            mountPoints.emplace(
                pair.first,
                std::make_unique<GstRecordStreamer>(
                    pair.second.recordConfig ?
                        std::optional<RecordOptions>({
                            pair.second.recordConfig->dir,
                            pair.second.recordConfig->maxFileSize}) :
                        std::optional<RecordOptions>(),
                    std::bind(OnRecorderConnected, &sessionsSharedData, pair.first),
                    std::bind(OnRecorderDisconnected, &sessionsSharedData, pair.first)));
            break;
        case StreamerConfig::Type::FilePlayer:
            monitorList.emplace_back(pair.first, pair.second.uri);
            break;
        case StreamerConfig::Type::Pipeline:
            mountPoints.emplace(
                pair.first,
                std::make_unique<GstPipelineStreamer2>(pair.second.pipeline));
            break;
        default:
            break;
        }
    }
#endif

    ScheduleAuthTokensCleanup(&sessionsSharedData);

    lws_context_creation_info lwsInfo {};
    lwsInfo.gid = -1;
    lwsInfo.uid = -1;
    lwsInfo.options = LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
#if defined(LWS_WITH_GLIB)
    lwsInfo.options |= LWS_SERVER_OPTION_GLIB;
    lwsInfo.foreign_loops = reinterpret_cast<void**>(&loop);
#endif

    LwsContextPtr lwsContextPtr(lws_create_context(&lwsInfo));
    lws_context* lwsContext = lwsContextPtr.get();

    std::string configJs =
        fmt::format("const WebRTSPPort = {};\r\n", config.port);
    for(std::string iceServer: config.webRTCConfig->iceServers) {
        if(0 == iceServer.compare(0, 7, "stun://")) {
            iceServer.erase(5, 2); // "stun://..." -> "stun:..."
            configJs += fmt::format("const STUNServer = \"{}\";\r\n", iceServer);
            break;
        }
    }

#if defined(BUILD_AS_CAMERA_STREAMER) || defined(BUILD_AS_V4L2_RESTREAMER)
    ServerSessionFactory serverSessionFactory(&config, &sessionsSharedData, mountPoint);
    ClientSessionFactory clientSessionFactory(&config, &sessionsSharedData, mountPoint);
#else
    AgentsDb agentsDb(&config);
    ServerSessionFactory serverSessionFactory(&config, &sessionsSharedData, &mountPoints);
    ClientSessionFactory clientSessionFactory(&config, &sessionsSharedData, &mountPoints);
#endif

    std::unique_ptr<WsClient> signallingClient;
    if(config.useAgentMode()) {
        assert(config.signallingServer.has_value());
        signallingClient = std::make_unique<WsClient>(
            *config.signallingServer,
            &clientSessionFactory,
            [&config] (WsClient& client, unsigned /*statusCode*/) {
                ClientDisconnected(config, client);
            });
    }

    std::unique_ptr<WsServer> serverPtr;
    if(config.useServerMode()) {
#if defined(BUILD_AS_CAMERA_STREAMER) || defined(BUILD_AS_V4L2_RESTREAMER)
        serverPtr = std::make_unique<WsServer>(
            config,
            &serverSessionFactory);
#else
        serverPtr = std::make_unique<WsServer>(
            config,
            &serverSessionFactory,
            &agentsDb);
#endif
    }

    std::unique_ptr<http::MicroServer> httpServerPtr;
    if(config.useServerMode() && httpConfig.port) {
        httpServerPtr =
            std::make_unique<http::MicroServer>(
                httpConfig,
                configJs,
                std::bind(OnNewAuthToken, &sessionsSharedData, std::placeholders::_1, std::placeholders::_2),
                context);
    }

#if !defined(BUILD_AS_CAMERA_STREAMER) && !defined(BUILD_AS_V4L2_RESTREAMER)
    std::unique_ptr<RecordingsCleanupContext> recordingsCleanupContext;
    std::unique_ptr<Actor> recordingsCleanupActor;
    if(!cleanupList.empty()) {
        recordingsCleanupContext = std::make_unique<RecordingsCleanupContext>();
        recordingsCleanupActor = std::make_unique<Actor>();
        recordingsCleanupActor->postAction(
            std::bind(
                RecordingsCleanupInitAction,
                std::ref(*recordingsCleanupContext),
                std::ref(cleanupList)));
    }

    std::unique_ptr<FilesMonitorsContext> filesMonitorsContext;
    std::unique_ptr<Actor> filesMonitorsActor;
    if(!monitorList.empty()) {
        filesMonitorsContext =
            std::make_unique<FilesMonitorsContext>(
                GMainContextPtr(g_main_context_ref(contextPtr.get())),
                &sessionsSharedData);
        filesMonitorsActor = std::make_unique<Actor>();
        filesMonitorsActor->postAction(
            std::bind(
                FilesMonitorsInitAction,
                std::ref(*filesMonitorsContext),
                std::ref(monitorList)));
    }
#endif

    if((!httpServerPtr || httpServerPtr->init()) &&
        (!serverPtr || serverPtr->init(loop, lwsContext)) &&
        (!signallingClient || signallingClient->init(loop)))
    {
        if(signallingClient && config.signallingServer.has_value()) {
            signallingClient->connectAsAgent(
                config.clientId,
                config.signallingServer->agentId,
                config.signallingServer->accessToken);
        }

        g_main_loop_run(loop);
    } else
        return -1;

    return 0;
}
