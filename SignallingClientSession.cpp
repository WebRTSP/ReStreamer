#include "SignallingClientSession.h"

#include <glib.h>

#include <CxxPtr/CPtr.h>

#include "RtspParser/RtspParser.h"

#include "Config.h"
#include "SessionsSharedData.h"


SignallingClientSession::SignallingClientSession(
    const Config* config,
    const SharedData* sharedData,
    const CreatePeer& createPeer,
    const SendRequest& sendRequest,
    const SendResponse& sendResponse) noexcept :
    ServerSession(config->webRTCConfig, createPeer, sendRequest, sendResponse),
    _config(config),
    _webRTCConfig(std::make_shared<WebRTCConfig>(*_config->webRTCConfig)),
    _sharedData(sharedData)
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

bool SignallingClientSession::onDescribeRequest(std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    _pendingRequests.emplace_back(std::move(requestPtr));
    if(_iceServersRequest)
        return true;

    const SignallingServer& target = _config->signallingServer.value();
    _iceServersRequest = requestGetParameter(
        target.uri,
        rtsp::TextParametersContentType,
        "ice-servers\r\n",
        target.token);

    return true;
}

bool SignallingClientSession::onGetParameterResponse(
    const rtsp::Request& request,
    const rtsp::Response& response) noexcept
{
    if(!_iceServersRequest || *_iceServersRequest != response.cseq)
        return false;

    _iceServersRequest.reset();

    if(!ServerSession::onGetParameterResponse(request, response))
        return false;

    rtsp::Parameters parameters;
    if(!rtsp::ParseParameters(response.body, &parameters))
        return false;

    WebRTCConfig::IceServers iceServers;

    auto stunServerIt = parameters.find("stun-server");
    if(parameters.end() != stunServerIt && !stunServerIt->second.empty())
        iceServers.push_back(stunServerIt->second);

    auto turnServerIt = parameters.find("turn-server");
    if(parameters.end() != turnServerIt && !turnServerIt->second.empty())
        iceServers.push_back(turnServerIt->second);

    auto turnsServerIt = parameters.find("turns-server");
    if(parameters.end() != turnsServerIt && !turnsServerIt->second.empty())
        iceServers.push_back(turnsServerIt->second);

    std::shared_ptr<WebRTCConfig> webRTCConfig = std::make_shared<WebRTCConfig>(*_config->webRTCConfig);
    if(!iceServers.empty())
        webRTCConfig->iceServers.swap(iceServers);
    _webRTCConfig = webRTCConfig;

    auto pendingRequests = std::move(_pendingRequests);
    for(auto& request: pendingRequests) {
        ServerSession::onDescribeRequest(request);
    }

    return true;
}
