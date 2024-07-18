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
        const CreatePeer& createPeer,
        const SendRequest& sendRequest,
        const SendResponse& sendResponse) noexcept;

    bool onConnected() noexcept override;

private:
    const Config *const _config;
    const SharedData *const _sharedData;
};
