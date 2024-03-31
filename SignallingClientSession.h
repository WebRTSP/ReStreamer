#pragma once

class Config; // #include "Config.h"
#include "Signalling/ServerSession.h"
class SessionsSharedData; // #include "SessionsSharedData.h"


class SignallingClientSession : public ServerSession
{
public:
    typedef SessionsSharedData SharedData;

    SignallingClientSession(
        const Config*,
        const SharedData*,
        const std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)>& createPeer,
        const std::function<void (const rtsp::Request*)>& sendRequest,
        const std::function<void (const rtsp::Response*)>& sendResponse) noexcept;

    bool onConnected() noexcept override;

private:
    const Config *const _config;
    const SharedData *const _sharedData;
};
