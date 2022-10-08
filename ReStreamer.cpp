#include "ReStreamer.h"

#include <CxxPtr/GlibPtr.h>
#include <CxxPtr/libwebsocketsPtr.h>

#include "Http/HttpServer.h"

#include "Signalling/WsServer.h"

#include "RtStreaming/GstRtStreaming/GstTestStreamer.h"
#include "RtStreaming/GstRtStreaming/GstReStreamer.h"

#include "RtStreaming/GstRtStreaming/GstTestStreamer2.h"
#include "RtStreaming/GstRtStreaming/GstReStreamer2.h"
#include "RtStreaming/GstRtStreaming/ONVIFReStreamer.h"
#include "RtStreaming/GstRtStreaming/GstRecordStreamer.h"

#include "Log.h"
#include "Session.h"


static const auto Log = ReStreamerLog;

typedef std::map<std::string, std::unique_ptr<GstStreamingSource>> MountPoints;

static std::unique_ptr<WebRTCPeer>
CreatePeer(
    const Config* config,
    MountPoints* mountPoints,
    const std::string& uri)
{
    auto streamerIt = mountPoints->find(uri);
    if(streamerIt != mountPoints->end()) {
        return streamerIt->second->createPeer();
    } else
        return nullptr;
}

static std::unique_ptr<WebRTCPeer>
CreateRecordPeer(
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

static std::unique_ptr<rtsp::ServerSession> CreateSession(
    const Config* config,
    MountPoints* mountPoints,
    Session::Cache* cache,
    const std::function<void (const rtsp::Request*) noexcept>& sendRequest,
    const std::function<void (const rtsp::Response*) noexcept>& sendResponse) noexcept
{
    std::unique_ptr<Session> session =
        std::make_unique<Session>(
            config,
            cache,
            std::bind(CreatePeer, config, mountPoints, std::placeholders::_1),
            std::bind(CreateRecordPeer, config, mountPoints, std::placeholders::_1),
            sendRequest, sendResponse);

    return session;
}

int ReStreamerMain(const http::Config& httpConfig, const Config& config)
{
    GMainContextPtr contextPtr(g_main_context_new());
    GMainContext* context = contextPtr.get();
    g_main_context_push_thread_default(context);

    GMainLoopPtr loopPtr(g_main_loop_new(context, FALSE));
    GMainLoop* loop = loopPtr.get();

    MountPoints mountPoints;
    for(const auto& pair: config.streamers) {
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
        case StreamerConfig::Type::ONVIFReStreamer:
            mountPoints.emplace(
                pair.first,
                std::make_unique<ONVIFReStreamer>(
                    pair.second.uri,
                    pair.second.forceH264ProfileLevelId));
            break;
        case StreamerConfig::Type::Record:
            mountPoints.emplace(pair.first, std::make_unique<GstRecordStreamer>());
            break;
        }
    }

    Session::Cache sessionsCache;

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
    for(std::string iceServer: config.iceServers) {
        if(0 == iceServer.compare(0, 7, "stun://")) {
            iceServer.erase(5, 2); // "stun://..." -> "stun:..."
            configJs += fmt::format("const STUNServer = \"{}\";\r\n", iceServer);
            break;
        }
    }

    std::unique_ptr<http::Server> httpServerPtr;
    if(httpConfig.port || (httpConfig.securePort && !httpConfig.certificate.empty() && !httpConfig.key.empty())) {
        httpServerPtr = std::make_unique<http::Server>(httpConfig, configJs, loop);
    }

    signalling::WsServer server(
        config,
        loop,
        std::bind(
            CreateSession,
            &config,
            &mountPoints,
            &sessionsCache,
            std::placeholders::_1,
            std::placeholders::_2));

    if((!httpServerPtr || httpServerPtr->init(lwsContext)) && server.init(lwsContext))
        g_main_loop_run(loop);
    else
        return -1;

    return 0;
}
