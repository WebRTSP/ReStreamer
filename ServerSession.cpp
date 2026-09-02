#include "ServerSession.h"

#include <glib.h>

#include "RtspParser/RtspParser.h"
#include "RtspParser/RtspSerialize.h"
#include "Helpers/TurnRestApi.h"

#include "Log.h"
#include "AgentServerSession.h"


ServerSession::ServerSession(
    const Config* config,
    SharedData* sharedData,
    const std::optional<std::string>& authCookie,
    const CreatePeer& createPeer,
    const CreatePeer& createRecordPeer,
    const rtsp::Session::SendRequest& sendRequest,
    const rtsp::Session::SendResponse& sendResponse) noexcept :
    StreamSession(
        config->webRTCConfig,
        createPeer,
        createRecordPeer,
        sendRequest,
        sendResponse),
    rtsp::MessageForwardMixin(SessionType::Regular, this),
    _config(config),
    _sharedData(sharedData),
    _authCookie(authCookie)
{
}

ServerSession::ServerSession(
    const Config* config,
    SharedData* sharedData,
    const std::optional<std::string>& authCookie,
    const CreatePeer& createPeer,
    const rtsp::Session::SendRequest& sendRequest,
    const rtsp::Session::SendResponse& sendResponse) noexcept :
    ServerSession(
        config,
        sharedData,
        authCookie,
        createPeer,
        {},
        sendRequest,
        sendResponse)
{
}

ServerSession::~ServerSession()
{
    for(auto& pair: _sharedData->recordMountpointsData) {
        RecordMountpointData& data = pair.second;
        data.subscriptions.erase(this);
    }
}

bool ServerSession::playEnabled(const std::string& uri) noexcept
{
    auto it = _config->streamers.find(uri);
    bool isSubstream = false;

    if(it == _config->streamers.end()) {
        const std::string::size_type separatorPos = uri.find_first_of(rtsp::UriSeparator);
        if(separatorPos == std::string::npos) {
            return false;
        }

        const std::string streamerName = uri.substr(0, separatorPos);
        it = _config->streamers.find(streamerName);
        isSubstream = true;
    }

    if(it == _config->streamers.end())
        return false;

    const StreamerConfig& streamerConfig = it->second;

    if(streamerConfig.type == StreamerConfig::Type::Record) {
        return streamerConfig.restream;
    }

    if(streamerConfig.type == StreamerConfig::Type::FilePlayer) {
        return isSubstream;
    }

    return true;
}

bool ServerSession::recordEnabled(const std::string& uri) noexcept
{
    auto it = _config->streamers.find(uri);
    return
        it != _config->streamers.end() &&
        it->second.type == StreamerConfig::Type::Record;
}

bool ServerSession::subscribeEnabled(const std::string& uri) noexcept
{
    auto it = _config->streamers.find(uri);
    return
        it != _config->streamers.end() &&
        it->second.type == StreamerConfig::Type::Record;
}

bool ServerSession::authorizeAgent(const std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    auto it = _config->streamers.find(requestPtr->uri);
    if(it == _config->streamers.end()) {
        log()->error("Can't find streamer \"{}\"", requestPtr->uri);
        return false;
    }

    switch(it->second.type) {
    case StreamerConfig::Type::Record:
        break;
    default:
        return false;
    }

    if(it->second.remoteAgentToken.empty())
        return true;

    const auto& [authType, credentials] = rtsp::ParseAuthentication(*requestPtr);

    if(authType != rtsp::Authentication::Bearer) // FIXME? only Bearer supported atm
        return false;

    return credentials.accessToken == it->second.remoteAgentToken;
}

bool ServerSession::hasValidCookie() const noexcept
{
    if(!_authCookie.has_value() || _authCookie->empty())
        return false;

    auto it = _sharedData->authTokens.find(_authCookie.value());
    if(it == _sharedData->authTokens.end())
        return false;

    const AuthTokenData& tokenData = it->second;

    if(tokenData.expiresAt < std::chrono::steady_clock::now())
        return false;

    return true;
}

bool ServerSession::authorize(const std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    auto authRequired = [this, &requestPtr] () {
        bool authRequired = true;

        if(requestPtr->uri == rtsp::WildcardUri) {
            authRequired = requestPtr->method != rtsp::Method::LIST && _config->authRequired;
        } else {
            const auto& [streamerName, substreamName] = rtsp::SplitUri(requestPtr->uri);
            auto streamerIt = _config->streamers.find(streamerName);
            typedef StreamerConfig::Visibility Visibility;
            authRequired =
                streamerIt != _config->streamers.end() &&
                (streamerIt->second.visibility == Visibility::Protected ||
                    (_config->authRequired && streamerIt->second.visibility == Visibility::Auto));
        }

        return authRequired;
    };

    switch(requestPtr->method) {
    case rtsp::Method::RECORD:
        return authorizeAgent(requestPtr);
    case rtsp::Method::LIST:
        if(!requestPtr->contentType.empty())
            return authorizeAgent(requestPtr);
        else if(authRequired())
            return hasValidCookie();
    case rtsp::Method::SUBSCRIBE:
    case rtsp::Method::DESCRIBE:
        if(authRequired())
            return hasValidCookie();
        break;
    default:
        break;
    }

    return StreamSession::authorize(requestPtr);
}

bool ServerSession::handleRequest(std::unique_ptr<rtsp::Request>&& requestPtr) noexcept
{
    const auto [streamerName, substreamName] = rtsp::SplitUri(requestPtr->uri);
    auto streamerIt = _config->streamers.find(streamerName);
    if(streamerIt != _config->streamers.end() &&
        streamerIt->second.type == StreamerConfig::Type::Proxy)
    {
        if(!requestPtr->session.empty())
            return forwardMediaSessionRequest(std::move(requestPtr));

        auto agentSessionIt = _sharedData->agentsSessions.find(streamerName);
        if(agentSessionIt != _sharedData->agentsSessions.end()) {
            return forwardRequest(
                std::move(requestPtr),
                std::string(!substreamName.empty() ? substreamName : rtsp::WildcardUri),
                agentSessionIt->second);
        }

        sendBadGatewayResponse(requestPtr->cseq);

        return true;
    }

    return rtsp::StreamSession::handleRequest(std::move(requestPtr));
}

bool ServerSession::handleResponse(
    const rtsp::Request& request,
    std::unique_ptr<rtsp::Response>&& responsePtr) noexcept
{
    if(std::optional<bool> response = tryForwardResponse(request, std::move(responsePtr)))
        return response.value();

    return StreamSession::handleResponse(request, std::move(responsePtr));
}

bool ServerSession::onGetParameterRequest(
    std::unique_ptr<rtsp::Request>&& requestPtr) noexcept
{
    std::string contentType = requestPtr->contentType;

    if(contentType.empty())
        return StreamSession::onGetParameterRequest(std::move(requestPtr));

    if(contentType != rtsp::TextParametersContentType)
        return false;

    const bool agentAuthorized = authorizeAgent(requestPtr);
    if(!agentAuthorized)
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

bool ServerSession::listEnabled(const std::string& uri) noexcept
{
    if(uri == rtsp::WildcardUri)
        return true;

    auto streamerIt = _config->streamers.find(uri);
    if(streamerIt == _config->streamers.end())
        return false;

    return streamerIt->second.type == StreamerConfig::Type::FilePlayer;
}

bool ServerSession::onListRequest(
    std::unique_ptr<rtsp::Request>&& requestPtr) noexcept
{
    const std::string& uri = requestPtr->uri;
    std::string contentType = requestPtr->contentType;

    if(!listEnabled(uri))
        return false;

    if(!contentType.empty() || !requestPtr->body.empty())
        return false;

    if(uri == rtsp::WildcardUri) {
        sendOkResponse(
            requestPtr->cseq,
            rtsp::TextParametersContentType,
            std::string(
                hasValidCookie() ?
                    _sharedData->protectedListCache :
                    _sharedData->publicListCache));
        return true;
    }

    auto streamerIt = _config->streamers.find(uri);
    if(streamerIt == _config->streamers.end())
        return false;

    auto sendCachedListResponse = [this, &uri, cseq = requestPtr->cseq] () {
        auto listIt = _sharedData->mountpointsListsCache.find(uri);
        if(listIt == _sharedData->mountpointsListsCache.end()) {
            sendOkResponse(
                cseq,
                rtsp::TextParametersContentType,
                "\r\n");
        } else {
            sendOkResponse(
                cseq,
                rtsp::TextParametersContentType,
                std::string(listIt->second));
        }
    };

    switch(streamerIt->second.type) {
    case StreamerConfig::Type::FilePlayer:
        sendCachedListResponse();
        return true;
    default:
        return false;
    }
}

bool ServerSession::onSubscribeRequest(
    std::unique_ptr<rtsp::Request>&& requestPtr) noexcept
{
    auto it = _config->streamers.find(requestPtr->uri);
    if(it == _config->streamers.end())
        return false;

    if(it->second.type != StreamerConfig::Type::Record)
        return false;

    if(!it->second.restream)
        return false;

    RecordMountpointData& data = _sharedData->recordMountpointsData[requestPtr->uri];
    auto selfIt = data.subscriptions.find(this);
    if(selfIt != data.subscriptions.end()) {
        log()->error("Second try to subscribe to the same streamer \"{}\"", requestPtr->uri);
        return false;
    }

    rtsp::MediaSessionId mediaSessionId = nextMediaSession();
    if(!data.recording) {
        log()->info("Streamer \"{}\" not active yet. Subscribing...", requestPtr->uri);
        data.subscriptions.emplace(this, mediaSessionId);
    }

    sendOkResponse(requestPtr->cseq, mediaSessionId);

    if(data.recording) {
        log()->info("Streamer \"{}\" already active. Starting record to client...", requestPtr->uri);
        startRecordToClient(requestPtr->uri, mediaSessionId);
    }

    return true;
}
