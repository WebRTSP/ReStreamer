#pragma once

#include <unordered_map>
#include <chrono>

#include "RtspSession/StreamSession.h"


struct SessionAuthTokenData {
    std::chrono::steady_clock::time_point expiresAt;
    // FIXME! add allowed IP
};

struct RecordMountpointData {
    bool recording = false;
    std::unordered_map<rtsp::StreamSession*, rtsp::MediaSessionId> subscriptions;
};

class ServerSession;
struct SessionsSharedData {
#if !defined(BUILD_AS_CAMERA_STREAMER) && !defined(BUILD_AS_V4L2_RESTREAMER)
    const std::string publicListCache;
    const std::string protectedListCache;
    const std::string agentListCache;
#endif

    std::unordered_map<std::string, const SessionAuthTokenData> authTokens;

#if !defined(BUILD_AS_CAMERA_STREAMER) && !defined(BUILD_AS_V4L2_RESTREAMER)
    std::map<std::string, RecordMountpointData, std::less<>> recordMountpointsData;
    std::map<std::string, std::string, std::less<>> mountpointsListsCache;
    std::map<std::string, ServerSession*, std::less<>> agentsMountpoints;
#endif
};
