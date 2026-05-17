#include "stun.h"

#include <string>
#include <future>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <gio/gio.h>

#include <stun/usages/bind.h>
#include <nice/agent.h>

#include <CxxPtr/GlibPtr.h>

#include "RtStreaming/GstRtStreaming/Helpers.h"

#include "Log.h"


namespace
{

struct LibNiceUnref
{
    void operator() (NiceAgent* agent)
        { g_object_unref(agent); }
};

typedef
    std::unique_ptr<
        NiceAgent,
        LibNiceUnref> NiceAgentPtr;

enum {
    DEFAULT_STUN_PORT = 3478
};

}

std::optional<std::string> DetectPublicIP(const WebRTCConfig& webRTCConfig)
{
    const auto& log = ReStreamerLog();

    std::optional<std::string> stunUrl;

    using GstRtStreaming::IceServerType;
    using GstRtStreaming::ParseIceServerType;
    for(const std::string& iceServer: webRTCConfig.iceServers) {
        if(IceServerType::Stun ==  ParseIceServerType(iceServer)) {
            stunUrl = iceServer;
            break;
        }
    }

    if(!stunUrl) {
        stunUrl = "stun://stun.l.google.com:19302";
        log->info("There is no STUN server provided in config. Using hardcoded one to detect public IP: {}", *stunUrl);
    }

    g_autoptr(GError) error = nullptr;
    g_autoptr(GUri) uri = g_uri_parse(stunUrl->c_str(), G_URI_FLAGS_HAS_PASSWORD, &error);
    if(!uri) {
        log->error("Failed to parse STUN server URL \"{}\": {}", *stunUrl, error->message);
        return {};
    }

    const gchar* scheme = g_uri_get_scheme(uri);
    const gchar* host = g_uri_get_host(uri);
    const gint port = g_uri_get_port(uri);

    std::future<std::pair<std::string, std::string>> future = std::async(
        std::launch::async,
        [host, port] () noexcept -> std::pair<std::string, std::string> {
            GMainContextPtr mainContextPtr(g_main_context_new());
            GMainContext* mainContext = mainContextPtr.get();

            GMainLoopPtr mainLoopPtr(g_main_loop_new(mainContext, FALSE));
            GMainLoop* mainLoop = mainLoopPtr.get();

            NiceAgentPtr niceAgentPtr(nice_agent_new(mainContext, NICE_COMPATIBILITY_RFC5245));
            NiceAgent* niceAgent = niceAgentPtr.get();

            struct Context {
                GMainLoop* mainLoop;
                std::string externalIp;
                std::string externalIpV6;
            } context = {
                .mainLoop = mainLoop
            };

            g_object_set(
                G_OBJECT(niceAgent),
                "controlling-mode", TRUE,
                "stun-server", host,
                "stun-server-port", port > 0 ? port : DEFAULT_STUN_PORT,
                nullptr);

            auto newCandidateFull = + [] (
                NiceAgent* agent,
                NiceCandidate* candidate,
                gpointer userData)
            {
                Context& context = *static_cast<Context*>(userData);
                if(candidate->type == NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE) {
                    gchar address[NICE_ADDRESS_STRING_LEN];
                    nice_address_to_string(&candidate->addr, address);
                    switch(candidate->addr.s.addr.sa_family) {
                    case AF_INET:
                        context.externalIp = address;
                        break;
                    case AF_INET6:
                        context.externalIpV6 = address;
                        break;
                    }
                }
            };
            g_signal_connect(
                G_OBJECT(niceAgent),
                "new-candidate-full",  G_CALLBACK(newCandidateFull),
                &context);

            auto candidateGatheringDone = + [] (
                NiceAgent* agent,
                guint streamId,
                gpointer userData)
            {
                Context& context = *static_cast<Context*>(userData);
                g_main_loop_quit(context.mainLoop);
            };
            g_signal_connect(
                G_OBJECT(niceAgent),
                "candidate-gathering-done",  G_CALLBACK(candidateGatheringDone),
                &context);

            const guint streamId = nice_agent_add_stream(niceAgent, 1);
            if(streamId == 0)
                return {};

            auto niceRecv = + [] (
                NiceAgent* agent,
                guint streamId,
                guint componentId,
                guint len,
                gchar* buf,
                gpointer data)
            {
            };
            if(!nice_agent_attach_recv(
                niceAgent,
                streamId,
                1,
                mainContext,
                niceRecv,
                   nullptr))
            {
                return {};
            }

            if(!nice_agent_gather_candidates(niceAgent, streamId))
                return {};

            g_main_loop_run(mainLoop);

            return std::make_pair(context.externalIp, context.externalIpV6);
        });

    const std::string publicIp = future.get().first;
    if(publicIp.empty()) {
        log->warn("Failed to detect public IP");
    } else {
        log->info("Detected public IP: {}", publicIp);
    }

    return publicIp;
}
