#pragma once

class Config; // #include "Config.h"
#include "RtspSession/ServerSession.h"
class SessionsSharedData; // #include "SessionsSharedData.h"


class SignallingClientSession : public rtsp::ServerSession
{
public:
    typedef SessionsSharedData SharedData;

    SignallingClientSession(
        const Config*,
        const SharedData*,
        const CreatePeer& createPeer,
        const SendRequest& sendRequest,
        const SendResponse& sendResponse) noexcept;

    bool onConnected() noexcept override;

protected:
    const WebRTCConfigPtr& webRTCConfig() const override { return _webRTCConfig; }

    bool onDescribeRequest(std::unique_ptr<rtsp::Request>&&) noexcept override;

    bool onGetParameterResponse(
        const rtsp::Request&,
        const rtsp::Response&) noexcept override;

private:
    const Config *const _config;
    WebRTCConfigPtr _webRTCConfig;
    const SharedData *const _sharedData;

    std::optional<rtsp::CSeq> _iceServersRequest;
    std::deque<std::unique_ptr<rtsp::Request>> _pendingRequests;
};
