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
        const std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)>& createPeer,
        const std::function<void (const rtsp::Request*)>& sendRequest,
        const std::function<void (const rtsp::Response*)>& sendResponse) noexcept;
    Session(
        const Config*,
        SharedData*,
        const std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)>& createPeer,
        const std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)>& createRecordPeer,
        const std::function<void (const rtsp::Request*)>& sendRequest,
        const std::function<void (const rtsp::Response*)>& sendResponse) noexcept;

protected:
    bool listEnabled() noexcept override { return true; }
    bool recordEnabled(const std::string& uri) noexcept override;
    bool authorize(const std::unique_ptr<rtsp::Request>&) noexcept override;
    bool authorize(
        const std::unique_ptr<rtsp::Request>&,
        const std::optional<std::string>& authCookie) noexcept override;

    bool onListRequest(
        std::unique_ptr<rtsp::Request>&) noexcept override;

private:
    const Config *const _config;
    SharedData *const _sharedData;
};
