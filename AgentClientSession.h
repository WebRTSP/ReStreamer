#pragma once

class Config; // #include "Config.h"
#include "RtspSession/StreamSession.h"
class SessionsSharedData; // #include "SessionsSharedData.h"


class AgentClientSession : public rtsp::StreamSession
{
public:
    typedef SessionsSharedData SharedData;

    AgentClientSession(
        const Config*,
        const SharedData*,
        std::string&& agentId,
        const CreatePeer& createPeer,
        const SendRequest& sendRequest,
        const SendResponse& sendResponse) noexcept;

protected:
    const WebRTCConfigPtr& webRTCConfig() const override { return _webRTCConfig; }

    bool authorize(const std::unique_ptr<rtsp::Request>&) noexcept override;

    bool listEnabled(const std::string& /*uri*/) noexcept override;
    bool playEnabled(const std::string& /*uri*/) noexcept override;

    bool onListRequest(std::unique_ptr<rtsp::Request>&&) noexcept override;
    bool onDescribeRequest(std::unique_ptr<rtsp::Request>&&) noexcept override;

    bool onGetParameterResponse(
        const rtsp::Request&,
        const rtsp::Response&) noexcept override;

private:
    const Config *const _config;

    std::chrono::steady_clock::time_point iceServersTime;
    WebRTCConfigPtr _webRTCConfig;

    const SharedData *const _sharedData;
    const std::string _agentId;

    std::optional<rtsp::CSeq> _iceServersRequest;
    std::deque<std::unique_ptr<rtsp::Request>> _pendingRequests;
};
