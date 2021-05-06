#include "ReStreamer.h"

#include <CxxPtr/GlibPtr.h>
#include <CxxPtr/libwebsocketsPtr.h>

#include "Http/HttpServer.h"

#include "Signalling/WsServer.h"

#include "RtStreaming/GstRtStreaming/GstTestStreamer.h"
#include "RtStreaming/GstRtStreaming/GstReStreamer.h"

#include "Log.h"
#include "Session.h"


static const auto Log = ReStreamerLog;

static std::unique_ptr<WebRTCPeer>
CreatePeer(
    const Config* config,
    const std::string& uri)
{
    auto streamerIt = config->streamers.find(uri);

    if(streamerIt != config->streamers.end()) {
        const StreamerConfig& streamer = streamerIt->second;
        switch(streamer.type) {
        case StreamerConfig::Type::Test:
            return std::make_unique<GstTestStreamer>(streamer.uri);
        case StreamerConfig::Type::ReStreamer:
            return std::make_unique<GstReStreamer>(streamer.uri);
        default:
            return std::make_unique<GstTestStreamer>();
        }
    } else
        if(config->allowClientUrls)
            return std::make_unique<GstReStreamer>(uri);
        else
            return nullptr;
}

static std::unique_ptr<rtsp::ServerSession> CreateSession(
    const Config* config,
    Session::Cache* cache,
    const std::function<void (const rtsp::Request*) noexcept>& sendRequest,
    const std::function<void (const rtsp::Response*) noexcept>& sendResponse) noexcept
{
    return
        std::make_unique<Session>(
            config,
            cache,
            std::bind(CreatePeer, config, std::placeholders::_1),
            sendRequest, sendResponse);
}

int ReStreamerMain(const http::Config& httpConfig, const Config& config)
{
    GMainContextPtr contextPtr(g_main_context_new());
    GMainContext* context = contextPtr.get();
    g_main_context_push_thread_default(context);

    GMainLoopPtr loopPtr(g_main_loop_new(context, FALSE));
    GMainLoop* loop = loopPtr.get();

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

    http::Server httpServer(httpConfig, config.port, loop);
    signalling::WsServer server(
        config,
        loop,
        std::bind(
            CreateSession,
            &config,
            &sessionsCache,
            std::placeholders::_1,
            std::placeholders::_2));

    if(httpServer.init(lwsContext) && server.init(lwsContext))
        g_main_loop_run(loop);
    else
        return -1;

    return 0;
}
