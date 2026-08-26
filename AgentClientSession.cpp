#include "AgentClientSession.h"

#include <glib.h>

#include <CxxPtr/CPtr.h>

#include "RtspParser/RtspParser.h"

#include "Config.h"
#include "SessionsSharedData.h"


AgentClientSession::AgentClientSession(
    const Config* config,
    const SharedData* sharedData,
    std::string&& agentId,
    const CreatePeer& createPeer,
    const SendRequest& sendRequest,
    const SendResponse& sendResponse) noexcept :
    StreamSession(config->webRTCConfig, createPeer, {}, sendRequest, sendResponse),
    _config(config),
    _webRTCConfig(std::make_shared<WebRTCConfig>(*_config->webRTCConfig)),
    _sharedData(sharedData),
    _agentId(std::move(agentId))
{
}

bool AgentClientSession::listEnabled(const std::string& uri) noexcept
{
#if !defined(BUILD_AS_CAMERA_STREAMER) && !defined(BUILD_AS_V4L2_RESTREAMER)
    return uri == rtsp::WildcardUri;
#else
    return false;
#endif
}

bool AgentClientSession::playEnabled(const std::string& uri) noexcept
{
#if !defined(BUILD_AS_CAMERA_STREAMER) && !defined(BUILD_AS_V4L2_RESTREAMER)
    auto it = _config->streamers.find(uri);
    if(it == _config->streamers.end())
        return false;

    const StreamerConfig& streamerConfig = it->second;
    switch(streamerConfig.type) {
        case StreamerConfig::Type::Test:
        case StreamerConfig::Type::ReStreamer:
#if ONVIF_SUPPORT
        case StreamerConfig::Type::ONVIFReStreamer:
#endif
        case StreamerConfig::Type::Pipeline:
            return streamerConfig.visibility == StreamerConfig::Visibility::Auto ||
                streamerConfig.visibility == StreamerConfig::Visibility::Public;
        case StreamerConfig::Type::Record:
        case StreamerConfig::Type::FilePlayer:
        case StreamerConfig::Type::Proxy:
            break;
    }

    return false;
#else
    return StreamSession::playEnabled(uri);
#endif
}

bool AgentClientSession::onListRequest(
    std::unique_ptr<rtsp::Request>&& requestPtr) noexcept
{
#if !defined(BUILD_AS_CAMERA_STREAMER) && !defined(BUILD_AS_V4L2_RESTREAMER)
    if(requestPtr->uri == rtsp::WildcardUri) {
        sendOkResponse(
            requestPtr->cseq,
            rtsp::TextParametersContentType,
            std::string(_sharedData->agentListCache));
        return true;
    }
#endif

    return false;
}

bool AgentClientSession::onDescribeRequest(
    std::unique_ptr<rtsp::Request>&& requestPtr) noexcept
{
    _pendingRequests.emplace_back(std::move(requestPtr));
    if(_iceServersRequest)
        return true;

    const SignallingServer& target = _config->signallingServer.value();
    _iceServersRequest = requestGetParameter(
        !target.uri.empty() ? target.uri : _agentId,
        rtsp::TextParametersContentType,
        "ice-servers\r\n");

    return true;
}

bool AgentClientSession::onGetParameterResponse(
    const rtsp::Request& request,
    const rtsp::Response& response) noexcept
{
    if(!_iceServersRequest || *_iceServersRequest != response.cseq)
        return false;

    _iceServersRequest.reset();

    if(!StreamSession::onGetParameterResponse(request, response))
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
        StreamSession::onDescribeRequest(std::move(request));
    }

    return true;
}
