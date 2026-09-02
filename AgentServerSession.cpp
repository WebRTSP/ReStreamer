#include "AgentServerSession.h"

#include "RtspParser/RtspParser.h"
#include "RtspParser/RtspSerialize.h"

#include "Helpers/TurnRestApi.h"


AgentServerSession::AgentServerSession(
    const Config* config,
    SharedData* sharedData,
    std::string&& agentId,
    const SendRequest& sendRequest,
    const SendResponse& sendResponse) noexcept :
    rtsp::Session(sendRequest, sendResponse),
    rtsp::MessageForwardMixin(rtsp::MessageForwardMixin::SessionType::Agent, this),
    _config(config),
    _sharedData(sharedData),
    _agentId(std::move(agentId))
{
    // multiple stale sessions are possible,
    // but only last one can be active
    _sharedData->agentsSessions[_agentId] = this;
}

AgentServerSession::~AgentServerSession() noexcept
{
    // it can be stale session
    auto it = _sharedData->agentsSessions.find(_agentId);
    if(it != _sharedData->agentsSessions.end() && it->second == this) {
        _sharedData->agentsSessions.erase(it);
    }
}

bool AgentServerSession::handleRequest(std::unique_ptr<rtsp::Request>&& requestPtr) noexcept
{
    switch(requestPtr->method) {
    case rtsp::Method::GET_PARAMETER:
        return onGetParameterRequest(std::move(requestPtr));
    default:
        return forwardMediaSessionRequest(std::move(requestPtr));
    }
}

bool AgentServerSession::onGetParameterRequest(std::unique_ptr<rtsp::Request>&& requestPtr) noexcept
{
    std::string contentType = requestPtr->contentType;

    if(contentType.empty())
        return rtsp::Session::onGetParameterRequest(std::move(requestPtr));

    if(contentType != rtsp::TextParametersContentType)
        return false;

    rtsp::ParametersNames names;
    if(!rtsp::ParseParametersNames(requestPtr->body, &names))
        return false;

    auto nameIt = names.find("ice-servers");
    if(names.end() == nameIt)
        return false;

    const AgentsConfig& agentsConfig = _config->agentsConfig;
    rtsp::Parameters parameters;

    const CoturnConfig& coturnConfig = _config->coturnConfig;

    if(agentsConfig.useCoturn && _config->publicIp && coturnConfig.staticAuthSecret) {
        const std::string coturnEndpoint = *_config->publicIp + ":" + std::to_string(coturnConfig.port);
        parameters.emplace("stun-server",
            "stun://" + coturnEndpoint);
        parameters.emplace("turn-server",
            GenerateTURNServerUrl(
                requestPtr->uri,
                coturnConfig.passwordTTL,
                coturnConfig.staticAuthSecret.value(),
                coturnEndpoint,
                false));
    }

    for(const std::string& iceServer: agentsConfig.iceServers) {
        if(0 == iceServer.compare(0, 5, "stun:"))
            parameters.emplace("stun-server", iceServer);
        else if(0 == iceServer.compare(0, 5, "turn:"))
            parameters.emplace("turn-server", iceServer);
        else if(0 == iceServer.compare(0, 6, "turns:"))
            parameters.emplace("turns-server", iceServer);
    }

    std::string body;
    rtsp::Serialize(parameters, &body);

    sendOkResponse(
        requestPtr->cseq,
        rtsp::MediaSessionId(),
        rtsp::TextParametersContentType,
        std::move(body));

    return true;
}

bool AgentServerSession::handleResponse(
    const rtsp::Request& request,
    std::unique_ptr<rtsp::Response>&& responsePtr) noexcept
{
    if(std::optional<bool> response = tryForwardResponse(request, std::move(responsePtr)))
        return response.value();

    return false;
}
