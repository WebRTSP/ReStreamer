#pragma once

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

    struct SharedData {
        std::string listCache;
        std::unordered_map<std::string, const AuthTokenData> authTokens;
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

protected:
    bool listEnabled() noexcept override { return true; }
    bool recordEnabled(const std::string& uri) noexcept override;
    bool authorizeRecord(const std::unique_ptr<rtsp::Request>&) noexcept;
    bool authorize(
        const std::unique_ptr<rtsp::Request>&,
        const std::optional<std::string>& authCookie) noexcept override;

    bool onListRequest(
        std::unique_ptr<rtsp::Request>&) noexcept override;

private:
    const Config *const _config;
    SharedData *const _sharedData;
};
