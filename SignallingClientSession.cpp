#include "SignallingClientSession.h"

#include <glib.h>

#include <CxxPtr/CPtr.h>

#include "RtspParser/RtspParser.h"

#include "Config.h"
#include "SessionsSharedData.h"


SignallingClientSession::SignallingClientSession(
    const Config* config,
    const SharedData* sharedData,
    const std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)>& createPeer,
    const std::function<void (const rtsp::Request*)>& sendRequest,
    const std::function<void (const rtsp::Response*)>& sendResponse) noexcept :
    ServerSession(config->webRTCConfig, createPeer, sendRequest, sendResponse),
    _config(config), _sharedData(sharedData)
{
}

bool SignallingClientSession::onConnected() noexcept
{
    const SignallingServer& target = _config->signallingServer.value();
    sendList(
        target.uri,
        _sharedData->agentListCache,
        target.token);

    return true;
}
