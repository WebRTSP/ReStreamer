#pragma once

#include <map>
#include <unordered_map>

#include "RtspSession/StreamSession.h"
#include "RtspSession/MessageForwardMixin.h"

#include "Config.h"
#include "SessionsSharedData.h"


class Session : public rtsp::StreamSession, public rtsp::MessageForwardMixin
{
public:
    typedef ::SessionAuthTokenData AuthTokenData;
    typedef ::RecordMountpointData RecordMountpointData;
    typedef SessionsSharedData SharedData;

    Session(
        const Config*,
        SharedData*,
        const std::optional<std::string>& authCookie,
        const CreatePeer& createPeer,
        const rtsp::Session::SendRequest& sendRequest,
        const rtsp::Session::SendResponse& sendResponse) noexcept;
    Session(
        const Config*,
        SharedData*,
        const std::optional<std::string>& authCookie,
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

    bool authorizeAgent(const std::unique_ptr<rtsp::Request>& requestPtr) noexcept;
    bool hasValidCookie() const noexcept;
    bool authorize(const std::unique_ptr<rtsp::Request>&) noexcept override;

    bool handleRequest(std::unique_ptr<rtsp::Request>&&) noexcept override;
    bool handleResponse(
        const rtsp::Request&,
        std::unique_ptr<rtsp::Response>&&) noexcept override;

#if !defined(BUILD_AS_CAMERA_STREAMER) && !defined(BUILD_AS_V4L2_RESTREAMER)
    bool onGetParameterRequest(
        std::unique_ptr<rtsp::Request>&&) noexcept override;
#endif
    bool onListRequest(
        std::unique_ptr<rtsp::Request>&&) noexcept override;
    bool onSubscribeRequest(
        std::unique_ptr<rtsp::Request>&&) noexcept override;

private:
    const Config *const _config;
    SharedData *const _sharedData;
    const std::optional<std::string> _authCookie;
};
