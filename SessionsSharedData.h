#pragma once

#include <unordered_map>


struct SessionAuthTokenData {
    std::chrono::steady_clock::time_point expiresAt;
    // FIXME! add allowed IP
};

struct RecordMountpointData {
    bool recording = false;
    std::unordered_map<ServerSession*, rtsp::SessionId> subscriptions;
};

struct SessionsSharedData {
    const std::string publicListCache;
    const std::string protectedListCache;
    std::unordered_map<std::string, const SessionAuthTokenData> authTokens;
    std::map<std::string, RecordMountpointData> recordMountpointsData;
    std::map<std::string, std::string> mountpointsListsCache;
};
