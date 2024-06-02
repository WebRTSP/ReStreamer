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

#include "RtspParser/RtspParser.h"

#include "Signalling/WsServer.h"
#include "Client/WsClient.h"

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
#include "RtStreaming/GstRtStreaming/GstV4L2Streamer.h"

#include "Log.h"
#include "Session.h"
#include "SignallingClientSession.h"


namespace {

const unsigned AuthTokenCleanupInterval = 15; // seconds

enum {
    MAX_FILES_TO_CLEANUP = 10,

    MIN_RECONNECT_TIMEOUT = 3, // seconds
    MAX_RECONNECT_TIMEOUT = 10, // seconds
};

const auto Log = ReStreamerLog;

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
    GMainContext* threadContext = g_main_context_get_thread_default();
    g_source_attach(timeoutSource, threadContext ? threadContext : g_main_context_default());
}

struct RecordingsMonitorContext {
    RecordingsMonitorContext(const RecordConfig& config, GFilePtr&& dirPtr, GFileMonitorPtr&& monitor) :
        config(config), dirPtr(std::move(dirPtr)), monitorPtr(std::move(monitor)) {}

    const RecordConfig config;
    GFilePtr dirPtr;
    GFileMonitorPtr monitorPtr;
};

struct FilesMonitorsContext;

struct FilesMonitorContext {
    FilesMonitorContext(
        const FilesMonitorsContext *const monitorsContext,
        const std::string& streamer,
        GFilePtr&& dirPtr,
        GFileMonitorPtr&& monitor) :
        monitorsContext(monitorsContext),
        streamer(streamer),
        dirPtr(std::move(dirPtr)),
        monitorPtr(std::move(monitor)) {}

    const FilesMonitorsContext *const monitorsContext;
    const std::string streamer;
    GFilePtr dirPtr;
    GFileMonitorPtr monitorPtr;
    std::map<std::string, uint64_t> files; // file name -> file timestamp
};

struct GDateTimeLess {
    bool operator() (const GDateTimePtr& l, const GDateTimePtr& r) const {
        return g_date_time_compare(l.get(), r.get()) < 0;
    }
};

struct RecordingsCleanupContext {
    std::deque<RecordingsMonitorContext> monitors;
};

struct FilesMonitorsContext {
    FilesMonitorsContext(GMainContextPtr&& mainContextPtr, Session::SharedData* sharedData) :
        mainContextPtr(std::move(mainContextPtr)), sharedData(sharedData) {}

    const GMainContextPtr mainContextPtr;
    Session::SharedData *const sharedData;
    std::deque<FilesMonitorContext> monitors;
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
    RecordingsMonitorContext& monitorContext = *static_cast<RecordingsMonitorContext*>(userData);

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
            RecordingsMonitorContext& monitorContext =
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

void PostDirContent(
    GMainContext* mainContext,
    Session::SharedData* sharedData,
    FilesMonitorContext* monitorContext)
{
    GSourcePtr idleSourcePtr(g_idle_source_new());
    GSource* idleSource = idleSourcePtr.get();

    struct CallbackData {
        const std::string streamer;
        Session::SharedData *const sharedData;
        const std::string list;
    };

    std::string list;
    for(const auto& pair: monitorContext->files) {
        list += pair.first;
        list += ": ";

        GDateTimePtr timePtr(g_date_time_new_from_unix_utc(pair.second));
        GCharPtr isoTime(time ? g_date_time_format_iso8601(timePtr.get()) : nullptr);
        if(isoTime) {
            list += isoTime.get();
        } else {
            list += std::to_string(pair.second);
        }

        list += "\r\n";
    }

    CallbackData* callbackData = new CallbackData { monitorContext->streamer, sharedData, std::move(list) };
    g_source_set_callback(
        idleSource,
        [] (gpointer userData) -> gboolean {
            const CallbackData* callbackData = reinterpret_cast<CallbackData*>(userData);

            const std::string& streamer = callbackData->streamer;
            const std::string& list = callbackData->list;

            Log()->debug("Dir content changed for \"{}\"", streamer);
            Log()->debug(list);

            callbackData->sharedData->mountpointsListsCache[streamer] = list;

            return false;
        },
        callbackData,
        [] (gpointer userData) {
            delete reinterpret_cast<CallbackData*>(userData);
        });
    g_source_attach(idleSource, mainContext);
}

void FilesDirChanged(
    GFileMonitor* monitor,
    GFile* file,
    GFile* /*otherFile*/,
    GFileMonitorEvent eventType,
    gpointer userData)
{
    FilesMonitorContext& context = *static_cast<FilesMonitorContext*>(userData);
    const FilesMonitorsContext& monitorsContext = *context.monitorsContext;

    switch(eventType) {
        case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT: {
            g_autofree gchar* fileName = g_file_get_basename(file);
            g_autoptr(GFileInfo) fileInfo =
                g_file_query_info(
                    file,
                    G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_TIME_CREATED,
                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                    NULL,
                    NULL);
            if(g_file_info_get_file_type(fileInfo) != G_FILE_TYPE_REGULAR)
                break;

            const std::string& escapedStreamerName = context.streamer;

            g_autoptr(GDateTime) fileTime = g_file_info_get_creation_date_time(fileInfo);
            if(fileName && fileTime) {
                g_autofree gchar* escapedFileName(g_uri_escape_string(fileName, nullptr, false));
                context.files.emplace(
                    escapedStreamerName + rtsp::UriSeparator + escapedFileName,
                    g_date_time_to_unix(fileTime));

                // FIXME! protect from too frequent changes
                PostDirContent(monitorsContext.mainContextPtr.get(), monitorsContext.sharedData, &context);
            } else {
                assert(false); // FIXME?
            }
            break;
        }
        case G_FILE_MONITOR_EVENT_DELETED: {
            g_autofree gchar* fileName = g_file_get_basename(file);
            if(fileName) {
                const std::string& escapedStreamerName = context.streamer;
                g_autofree gchar* escapedFileName(g_uri_escape_string(fileName, nullptr, false));

                context.files.erase(escapedStreamerName + rtsp::UriSeparator + escapedFileName);

                // FIXME! protect from too frequent changes
                PostDirContent(monitorsContext.mainContextPtr.get(), monitorsContext.sharedData, &context);
            } else {
                assert(false); // FIXME?
            }
            break;
        }
    }
}

void FilesMonitorsInitAction(
    FilesMonitorsContext& context,
    const std::deque<std::pair<std::string, std::string>>& monitorList)
{
    for(const std::pair<std::string, std::string>& pair: monitorList) {
        const std::string& escapedStreamerName = pair.first;

        GFilePtr monitorDirPtr(g_file_new_for_path(pair.second.c_str()));
        GFileMonitorPtr dirMonitorPtr(
            g_file_monitor_directory(
                monitorDirPtr.get(),
                G_FILE_MONITOR_NONE,
                nullptr,
                nullptr));
        if(dirMonitorPtr) {
            g_file_monitor_set_rate_limit(dirMonitorPtr.get(), 5000);
            FilesMonitorContext& monitorContext =
                context.monitors.emplace_back(
                    &context,
                    pair.first,
                    GFilePtr(g_object_ref(monitorDirPtr.get())),
                    std::move(dirMonitorPtr));
            g_signal_connect(
                monitorContext.monitorPtr.get(),
                "changed",
                G_CALLBACK(FilesDirChanged),
                &monitorContext);

            g_autoptr(GFileEnumerator) enumerator(
                g_file_enumerate_children(
                    monitorDirPtr.get(),
                    G_FILE_ATTRIBUTE_TIME_CREATED,
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
                            const char* fileName = g_file_info_get_name(childInfo);
                            g_autoptr(GDateTime) fileTime = g_file_info_get_creation_date_time(childInfo);
                            if(fileName && fileTime) {
                                GCharPtr escapedFileNamePtr(g_uri_escape_string(fileName, nullptr, false));
                                monitorContext.files.emplace(
                                    escapedStreamerName + rtsp::UriSeparator + escapedFileNamePtr.get(),
                                    g_date_time_to_unix(fileTime));
                            }
                            break;
                        }
                    }
                }
            }

            PostDirContent(context.mainContextPtr.get(), context.sharedData, &monitorContext);
        }
    }
}

}

typedef std::map<std::string, std::unique_ptr<GstStreamingSource>> MountPoints;

static std::unique_ptr<WebRTCPeer>
CreatePeer(
    const Config* config,
    MountPoints* mountPoints,
    const std::string& uri)
{
    const auto& [streamerName, substreamName] = rtsp::SplitUri(uri);

    auto configStreamerIt = config->streamers.find(streamerName);
    if(configStreamerIt == config->streamers.end() || !configStreamerIt->second.restream)
        return nullptr;

    const StreamerConfig& streamerConfig = configStreamerIt->second;
    if(configStreamerIt->second.type == StreamerConfig::Type::FilePlayer) {
        g_autofree gchar* unEscapedSubstreamName = g_uri_unescape_string(substreamName.c_str(), nullptr);
        g_autofree gchar* reEscapedSubstreamName = g_uri_escape_string(unEscapedSubstreamName, " ()", false);

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

namespace {

std::unique_ptr<rtsp::Session> CreateSignallingSession(
    const Config* config,
    MountPoints* mountPoints,
    const Session::SharedData* sharedData,
    const rtsp::Session::SendRequest& sendRequest,
    const rtsp::Session::SendResponse& sendResponse)
{
    return
        std::make_unique<SignallingClientSession>(
            config,
            sharedData,
            std::bind(CreatePeer, config, mountPoints, std::placeholders::_1),
            sendRequest, sendResponse);
}

void ClientDisconnected(client::WsClient& client)
{
    const unsigned reconnectTimeout =
        g_random_int_range(MIN_RECONNECT_TIMEOUT, MAX_RECONNECT_TIMEOUT + 1);
    Log()->info("Scheduling reconnect withing \"{}\" seconds...", reconnectTimeout);
    GSourcePtr timeoutSourcePtr(g_timeout_source_new_seconds(reconnectTimeout));
    GSource* timeoutSource = timeoutSourcePtr.get();
    g_source_set_callback(timeoutSource,
        [] (gpointer userData) -> gboolean {
            static_cast<client::WsClient*>(userData)->connect();
            return false;
        }, &client, nullptr);
    g_source_attach(timeoutSource, g_main_context_get_thread_default());
}

}

static void OnRecorderConnected(Session::SharedData* sharedData, const std::string& uri)
{
    Log()->info("Recorder connected to \"{}\" streamer", uri);

    Session::RecordMountpointData& data = sharedData->recordMountpointsData[uri];

    data.recording = true;

    std::unordered_map<ServerSession*, rtsp::MediaSessionId> subscriptions;
    data.subscriptions.swap(subscriptions);
    for(auto& session2session: subscriptions) {
        ServerSession* session = session2session.first;
        const rtsp::MediaSessionId& mediaSession = session2session.second;
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

    Session::SharedData sessionsSharedData {
        .publicListCache = GenerateList(config, ListType::Public),
        .protectedListCache = GenerateList(config, ListType::Protected),
        .agentListCache = GenerateList(config, ListType::Agent),
    };

    std::deque<RecordConfig> cleanupList;
    std::deque<std::pair<std::string, std::string>> monitorList;

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
        case StreamerConfig::Type::Camera:
            mountPoints.emplace(
                pair.first,
                std::make_unique<GstCameraStreamer>(
                    std::optional<GstCameraStreamer::VideoResolution>(),
                    std::optional<std::string>(),
                    pair.second.useHwEncoder));
            break;
        case StreamerConfig::Type::V4L2:
            mountPoints.emplace(
                pair.first,
                std::make_unique<GstV4L2Streamer>(
                    pair.second.edidFilePath,
                    std::optional<GstV4L2Streamer::VideoResolution>(),
                    std::optional<std::string>(),
                    pair.second.useHwEncoder));
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
    for(std::string iceServer: config.webRTCConfig->iceServers) {
        if(0 == iceServer.compare(0, 7, "stun://")) {
            iceServer.erase(5, 2); // "stun://..." -> "stun:..."
            configJs += fmt::format("const STUNServer = \"{}\";\r\n", iceServer);
            break;
        }
    }

    std::unique_ptr<signalling::WsServer> serverPtr;
    std::unique_ptr<client::WsClient> signallingClient;
    if(config.signallingServer) {
        signallingClient = std::make_unique<client::WsClient>(
            *config.signallingServer,
            loop,
            std::bind(
                CreateSignallingSession,
                &config,
                &mountPoints,
                &sessionsSharedData,
                std::placeholders::_1,
                std::placeholders::_2),
            std::bind(ClientDisconnected, std::placeholders::_1));
    }
#if NDEBUG
    else {
#else
    {
#endif
        serverPtr = std::make_unique<signalling::WsServer>(
            config,
            loop,
            std::bind(
                CreateSession,
                &config,
                &mountPoints,
                &sessionsSharedData,
                std::placeholders::_1,
                std::placeholders::_2));
    }

    std::unique_ptr<http::MicroServer> httpServerPtr;
#if NDEBUG
    if(httpConfig.port && !config.signallingServer) {
#else
    if(httpConfig.port) {
#endif
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

    if((!httpServerPtr || httpServerPtr->init()) &&
        (!serverPtr || serverPtr->init(lwsContext)) &&
        (!signallingClient || signallingClient->init()))
    {
        if(signallingClient)
            signallingClient->connect();

        g_main_loop_run(loop);
    } else
        return -1;

    return 0;
}
