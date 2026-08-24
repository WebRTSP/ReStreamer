#pragma once

#include <memory>

#include "RtspSession/StreamSession.h"
#include "RtspSession/MessageForwardMixin.h"

#include "Config.h"
#include "SessionsSharedData.h"


class BasicServerSession : public rtsp::StreamSession
{
public:
    typedef ::SessionAuthTokenData AuthTokenData;
    typedef SessionsSharedData SharedData;

    BasicServerSession(
        const Config*,
        SharedData*,
        const std::optional<std::string>& authCookie,
        const CreatePeer& createPeer,
        const rtsp::Session::SendRequest& sendRequest,
        const rtsp::Session::SendResponse& sendResponse) noexcept;
    ~BasicServerSession();

protected:
    bool hasValidCookie() const noexcept;
    bool authorize(const std::unique_ptr<rtsp::Request>&) noexcept override;

private:
    const Config *const _config;
    SharedData *const _sharedData;
    const std::optional<std::string> _authCookie;
};
