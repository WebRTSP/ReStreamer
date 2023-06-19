#include "ReStreamer.h"

#include <deque>
#include <map>
#include <string>
#include <chrono>

#include <CxxPtr/GlibPtr.h>
#include <CxxPtr/GioPtr.h>
#include <CxxPtr/libwebsocketsPtr.h>

#include "Helpers/Actor.h"

#include "Http/HttpMicroServer.h"

#include "Signalling/WsServer.h"

#include "RtStreaming/GstRtStreaming/GstTestStreamer.h"
#include "RtStreaming/GstRtStreaming/GstReStreamer.h"

#include "RtStreaming/GstRtStreaming/GstTestStreamer2.h"
#include "RtStreaming/GstRtStreaming/GstReStreamer2.h"
#include "RtStreaming/GstRtStreaming/ONVIFReStreamer.h"
#include "RtStreaming/GstRtStreaming/GstRecordStreamer.h"

#include "Log.h"
#include "Session.h"


namespace {

const unsigned AuthTokenCleanupInterval = 15; // seconds

enum {
    MAX_FILES_TO_CLEANUP = 10
};

void OnNewAuthToken(
    Session::SharedData* sessionsSharedData,
    const std::string& token,
    std::chrono::steady_clock::time_point expiresAt)
{
    sessionsSharedData->authTokens.emplace(token, Session::AuthTokenData { expiresAt });
}

void CleanupAuthTokens(Session::SharedData* sessionsSharedData)
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

void ScheduleAuthTokensCleanup(Session::SharedData* sessionsSharedData) {
    GSourcePtr timeoutSourcePtr(g_timeout_source_new_seconds(AuthTokenCleanupInterval));
    GSource* timeoutSource = timeoutSourcePtr.get();
    g_source_set_callback(timeoutSource,
        [] (gpointer userData) -> gboolean {
            Session::SharedData* sessionsSharedData = reinterpret_cast<Session::SharedData*>(userData);
            CleanupAuthTokens(sessionsSharedData);
            return false;
        }, sessionsSharedData, nullptr);
    g_source_attach(timeoutSource, g_main_context_get_thread_default());
}

struct MonitorContext {
    MonitorContext(const RecordConfig& config, GFilePtr&& dirPtr, GFileMonitorPtr&& monitor) :
        config(config), dirPtr(std::move(dirPtr)), monitorPtr(std::move(monitor)) {}

    const RecordConfig config;
    GFilePtr dirPtr;
    GFileMonitorPtr monitorPtr;
};

struct GDateTimeLess
{
    bool operator() (const GDateTimePtr& l, const GDateTimePtr& r) const {
        return g_date_time_compare(l.get(), r.get()) < 0;
    }
};

struct RecordingsCleanupContext {
    std::deque<MonitorContext> monitors;
};

struct FileData {
    GFilePtr filePtr;
    guint64 fileSize;
};

void RecordingsDirChanged(
    GFileMonitor* monitor,
    GFile* /*file*/,
    GFile* /*otherFile*/,
    GFileMonitorEvent eventType,
    gpointer userData)
{
    MonitorContext& monitorContext = *static_cast<MonitorContext*>(userData);

    if(eventType != G_FILE_MONITOR_EVENT_CREATED && eventType != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
        return;

    std::map<GDateTimePtr, FileData, GDateTimeLess> candidatesToDelete;
    guint64 dirSize = 0;

    g_autoptr(GFileEnumerator) enumerator(
        g_file_enumerate_children(
            monitorContext.dirPtr.get(),
            G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
            nullptr,
            nullptr));

    if(enumerator) {
        GFileInfo* childInfo;
        GFile* child;
        for(
            gboolean iterated = g_file_enumerator_iterate(enumerator, &childInfo, &child, nullptr, nullptr);
            iterated && childInfo && child;
            iterated = g_file_enumerator_iterate(enumerator, &childInfo, &child, nullptr, nullptr))
        {
            switch(g_file_info_get_file_type(childInfo)) {
                case G_FILE_TYPE_REGULAR: {
                    const guint64 fileSize = g_file_info_get_size(childInfo);
                    dirSize += fileSize;

                    if(g_autoptr(GDateTime) fileTime = g_file_info_get_modification_date_time(childInfo)) {
                        if(candidatesToDelete.size() < MAX_FILES_TO_CLEANUP ||
                            g_date_time_compare((--candidatesToDelete.end())->first.get(), fileTime) > 0)
                        {
                            candidatesToDelete.emplace(
                                g_date_time_ref(fileTime),
                                FileData {
                                    GFilePtr(G_FILE(g_object_ref(child))),
                                    fileSize });
                        }
                        if(candidatesToDelete.size() > MAX_FILES_TO_CLEANUP) {
                            candidatesToDelete.erase(--candidatesToDelete.end());
                        }
                    }
                    break;
                }
            }
        }
    }

    if(candidatesToDelete.empty())
        return;

    auto it = candidatesToDelete.begin();
    while(it != candidatesToDelete.end() && dirSize > monitorContext.config.maxDirSize) {
        dirSize -= it->second.fileSize;
        g_file_delete(it->second.filePtr.get(), nullptr, nullptr);
        ++it;
    }
}

void RecordingsCleanupInitAction(
    RecordingsCleanupContext& context,
    const std::deque<RecordConfig>& cleanupList)
{
    for(const RecordConfig& config: cleanupList) {
        GFilePtr monitorDirPtr(g_file_new_for_path(config.dir.c_str()));
        GFileMonitorPtr dirMonitorPtr(
            g_file_monitor_directory(
                monitorDirPtr.get(),
                G_FILE_MONITOR_NONE,
                nullptr,
                nullptr));
        if(dirMonitorPtr) {
            g_file_monitor_set_rate_limit(dirMonitorPtr.get(), 5000);
            MonitorContext& monitorContext =
                context.monitors.emplace_back(
                    config,
                    std::move(monitorDirPtr),
                    std::move(dirMonitorPtr));
            g_signal_connect(
                monitorContext.monitorPtr.get(),
                "changed",
                G_CALLBACK(RecordingsDirChanged),
                &monitorContext);
        }
    }
}

}

static const auto Log = ReStreamerLog;

typedef std::map<std::string, std::unique_ptr<GstStreamingSource>> MountPoints;

static std::unique_ptr<WebRTCPeer>
CreatePeer(
    const Config* config,
    MountPoints* mountPoints,
    const std::string& uri)
{
    auto configStreamerIt = config->streamers.find(uri);
    if(configStreamerIt == config->streamers.end() || !configStreamerIt->second.restream)
        return nullptr;

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

static std::unique_ptr<ServerSession> CreateSession(
    const Config* config,
    MountPoints* mountPoints,
    Session::SharedData* sharedData,
    const rtsp::Session::SendRequest& sendRequest,
    const rtsp::Session::SendResponse& sendResponse)
{
    std::unique_ptr<Session> session =
        std::make_unique<Session>(
            config,
            sharedData,
            std::bind(CreatePeer, config, mountPoints, std::placeholders::_1),
            std::bind(CreateRecordPeer, config, mountPoints, std::placeholders::_1),
            sendRequest, sendResponse);

    return session;
}

static void OnRecorderConnected(Session::SharedData* sharedData, const std::string& uri)
{
    Log()->info("Recorder connected to \"{}\" streamer", uri);

    Session::RecordMountpointData& data = sharedData->recordMountpointsData[uri];

    data.recording = true;

    std::unordered_map<ServerSession*, rtsp::SessionId> subscriptions;
    data.subscriptions.swap(subscriptions);
    for(auto& session2session: subscriptions) {
        ServerSession* session = session2session.first;
        const rtsp::SessionId& mediaSession = session2session.second;
        session->startRecordToClient(uri, mediaSession);
    }
}

static void OnRecorderDisconnected(Session::SharedData* sharedData, const std::string& uri)
{
    Log()->info("Recorder disconnected from \"{}\" streamer", uri);

    auto it = sharedData->recordMountpointsData.find(uri);
    if(it == sharedData->recordMountpointsData.end()) {
        return;
    }

    Session::RecordMountpointData& data = it->second;
    data.recording = false;
    assert(data.subscriptions.empty());
}

int ReStreamerMain(const http::Config& httpConfig, const Config& config)
{
    GMainContextPtr contextPtr(g_main_context_new());
    GMainContext* context = contextPtr.get();
    g_main_context_push_thread_default(context);

    GMainLoopPtr loopPtr(g_main_loop_new(context, FALSE));
    GMainLoop* loop = loopPtr.get();

    Session::SharedData sessionsSharedData;

    std::deque<RecordConfig> cleanupList;

    MountPoints mountPoints;
    for(const auto& pair: config.streamers) {
        if(pair.second.type != StreamerConfig::Type::Record && !pair.second.restream)
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
        case StreamerConfig::Type::ONVIFReStreamer:
            mountPoints.emplace(
                pair.first,
                std::make_unique<ONVIFReStreamer>(
                    pair.second.uri,
                    pair.second.forceH264ProfileLevelId));
            break;
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
        }
    }

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
    for(std::string iceServer: config.iceServers) {
        if(0 == iceServer.compare(0, 7, "stun://")) {
            iceServer.erase(5, 2); // "stun://..." -> "stun:..."
            configJs += fmt::format("const STUNServer = \"{}\";\r\n", iceServer);
            break;
        }
    }

    signalling::WsServer server(
        config,
        loop,
        std::bind(
            CreateSession,
            &config,
            &mountPoints,
            &sessionsSharedData,
            std::placeholders::_1,
            std::placeholders::_2));

    std::unique_ptr<http::MicroServer> httpServerPtr;
    if(httpConfig.port || (httpConfig.securePort && !httpConfig.certificate.empty() && !httpConfig.key.empty())) {
        httpServerPtr =
            std::make_unique<http::MicroServer>(
                httpConfig,
                configJs,
                std::bind(OnNewAuthToken, &sessionsSharedData, std::placeholders::_1, std::placeholders::_2),
                context);
    }

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

    if((!httpServerPtr || httpServerPtr->init()) && server.init(lwsContext))
        g_main_loop_run(loop);
    else
        return -1;

    return 0;
}
