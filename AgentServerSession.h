#pragma once

#include "RtspSession/Session.h"
#include "RtspSession/MessageForwardMixin.h"

#include "Config.h"
#include "SessionsSharedData.h"


class AgentServerSession : public rtsp::Session, public rtsp::MessageForwardMixin
{
public:
    typedef SessionsSharedData SharedData;

    AgentServerSession(
        const Config*,
        SharedData*,
        std::string&& agentId,
        const SendRequest&,
        const SendResponse&) noexcept;
    ~AgentServerSession() noexcept;

protected:
    bool onGetParameterRequest(std::unique_ptr<rtsp::Request>&&) noexcept;
    bool handleRequest(std::unique_ptr<rtsp::Request>&&) noexcept override;

    bool handleResponse(
        const rtsp::Request&,
        std::unique_ptr<rtsp::Response>&&) noexcept override;

private:
    const Config *const _config;
    SharedData *const _sharedData;
    const std::string _agentId;
};
