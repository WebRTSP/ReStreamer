#pragma once

#include <map>
#include <unordered_map>

#include "Signalling/ServerSession.h"

#include "Config.h"
#include "SessionsSharedData.h"


class Session : public ServerSession
{
public:
    typedef SessionAuthTokenData AuthTokenData;
    typedef RecordMountpointData RecordMountpointData;
    typedef SessionsSharedData SharedData;

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
    bool isValidCookie(const std::optional<std::string>& authCookie) noexcept;
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
