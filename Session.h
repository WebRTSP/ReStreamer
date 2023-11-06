#pragma once

#include <map>
#include <unordered_map>

#include "Signalling/ServerSession.h"

#include "Config.h"


class Session : public ServerSession
{
public:
    struct AuthTokenData {
        std::chrono::steady_clock::time_point expiresAt;
        // FIXME! add allowed IP
    };

    struct RecordMountpointData {
        bool recording = false;
        std::unordered_map<ServerSession*, rtsp::SessionId> subscriptions;
    };

    struct SharedData {
        const std::string listCache;
        std::unordered_map<std::string, const AuthTokenData> authTokens;
        std::map<std::string, RecordMountpointData> recordMountpointsData;
        std::map<std::string, std::string> mountpointsListsCache;
    };

    Session(
        const Config*,
        SharedData*,
        const CreatePeer& createPeer,
        const rtsp::Session::SendRequest& sendRequest,
        const rtsp::Session::SendResponse& sendResponse) noexcept;
    Session(
        const Config*,
        SharedData*,
        const CreatePeer& createPeer,
        const CreatePeer& createRecordPeer,
        const rtsp::Session::SendRequest& sendRequest,
        const rtsp::Session::SendResponse& sendResponse) noexcept;
    ~Session();

protected:
    bool listEnabled(const std::string& uri) noexcept override;
    bool playEnabled(const std::string& uri) noexcept override;
    bool recordEnabled(const std::string& uri) noexcept override;
    bool subscribeEnabled(const std::string& uri) noexcept override;
    bool authorizeRecord(const std::unique_ptr<rtsp::Request>&) noexcept;
    bool authorize(const std::unique_ptr<rtsp::Request>&) noexcept override;

    bool onListRequest(
        std::unique_ptr<rtsp::Request>&) noexcept override;
    bool onSubscribeRequest(
        std::unique_ptr<rtsp::Request>&) noexcept override;

private:
    void startRecord(const std::string& uri, const rtsp::SessionId& mediaSession) noexcept;

private:
    const Config *const _config;
    SharedData *const _sharedData;
};
