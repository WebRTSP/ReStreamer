#include "Session.h"

#include <glib.h>

#include "RtspParser/RtspParser.h"

#include "Log.h"


static const auto Log = ReStreamerLog;

class Session::SessionHandle
{
public:
    SessionHandle(Session* owner): _owner(owner) {}

    Session* operator -> () { return _owner; }
    const Session* operator -> () const { return _owner; }

private:
    Session* _owner;
};

Session::Session(
    const Config* config,
    SharedData* sharedData,
    const CreatePeer& createPeer,
    const rtsp::Session::SendRequest& sendRequest,
    const rtsp::Session::SendResponse& sendResponse) noexcept :
    ServerSession(config->webRTCConfig, createPeer, sendRequest, sendResponse),
    _config(config), _sharedData(sharedData), _handle(std::make_shared<SessionHandle>(this))
{
}

Session::Session(
    const Config* config,
    SharedData* sharedData,
    const CreatePeer& createPeer,
    const CreatePeer& createRecordPeer,
    const rtsp::Session::SendRequest& sendRequest,
    const rtsp::Session::SendResponse& sendResponse) noexcept :
    ServerSession(config->webRTCConfig, createPeer, createRecordPeer, sendRequest, sendResponse),
    _config(config), _sharedData(sharedData), _handle(std::make_shared<SessionHandle>(this))
{
}

Session::~Session() {
    for(auto& pair: _sharedData->recordMountpointsData) {
        RecordMountpointData& data = pair.second;
        data.subscriptions.erase(this);
    }

    auto& agentsMountpoints = _sharedData->agentsMountpoints;
    for(auto it = agentsMountpoints.begin(); it != agentsMountpoints.end();) {
        if(it->second == this) {
            it = agentsMountpoints.erase(it);
        } else {
            ++it;
        }
    }

    for(auto it = _forwardedRequests.begin(); it != _forwardedRequests.end();) {
        const rtsp::CSeq cseq = it->first;
        ++it;

        std::unique_ptr<rtsp::Response > responsePtr = std::make_unique<rtsp::Response>();
        prepareResponse(
            rtsp::StatusCode::BAD_GATEWAY,
            "Bad Gateway",
            cseq,
            std::string(),
            responsePtr.get());
        rtsp::Session::handleResponse(responsePtr);
    }
    assert(_forwardedRequests.empty());

    for(auto it = _clientMediaSession2agentMediaSession.begin();
        it != _clientMediaSession2agentMediaSession.end();)
    {
        const MediaSessionInfo& mediaSessionInfo = it->second;
        ++it;
        forwardTeardown(mediaSessionInfo);
    };
    _clientMediaSession2agentMediaSession.clear();

    for(auto it = _agentMediaSessions2clientMediaSession.begin();
        it != _agentMediaSessions2clientMediaSession.end();)
    {
        const MediaSessionInfo& mediaSessionInfo = it->second;
        ++it;
        forwardTeardown(mediaSessionInfo);
    };
    _agentMediaSessions2clientMediaSession.clear();
}

bool Session::playEnabled(const std::string& uri) noexcept
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

bool Session::recordEnabled(const std::string& uri) noexcept
{
    auto it = _config->streamers.find(uri);
    return
        it != _config->streamers.end() &&
        it->second.type == StreamerConfig::Type::Record;
}

bool Session::subscribeEnabled(const std::string& uri) noexcept
{
    auto it = _config->streamers.find(uri);
    return
        it != _config->streamers.end() &&
        it->second.type == StreamerConfig::Type::Record;
}

bool Session::authorizeRecord(const std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    if(requestPtr->method != rtsp::Method::RECORD)
        return false;

    auto it = _config->streamers.find(requestPtr->uri);
    if(it == _config->streamers.end())
        return false;

    if(it->second.type != StreamerConfig::Type::Record)
        return false;

    if(it->second.remoteAgentToken.empty())
        return true;

    const std::pair<rtsp::Authentication, std::string> authPair =
        rtsp::ParseAuthentication(*requestPtr);

    if(authPair.first != rtsp::Authentication::Bearer) // FIXME? only Bearer supported atm
        return false;

    return authPair.second == it->second.remoteAgentToken;
}

bool Session::authorizeAgentList(const std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    if(requestPtr->method != rtsp::Method::LIST)
        return false;

    auto it = _config->streamers.find(requestPtr->uri);
    if(it == _config->streamers.end())
        return false;

    if(it->second.type != StreamerConfig::Type::Proxy) {
        const std::string& contentType = rtsp::RequestContentType(*requestPtr);
        return contentType.empty() && requestPtr->body.empty();
    }

    if(it->second.remoteAgentToken.empty())
        return true;

    const std::pair<rtsp::Authentication, std::string> authPair =
        rtsp::ParseAuthentication(*requestPtr);

    if(authPair.first != rtsp::Authentication::Bearer) // FIXME? only Bearer supported atm
        return false;

    return authPair.second == it->second.remoteAgentToken;
}

bool Session::isValidCookie(const std::optional<std::string>& authCookie) noexcept
{
    if(!authCookie)
        return false;

    auto it = _sharedData->authTokens.find(authCookie.value());
    if(it == _sharedData->authTokens.end())
        return false;

    const AuthTokenData& tokenData = it->second;

    if(tokenData.expiresAt < std::chrono::steady_clock::now())
        return false;

    return true;
}

bool Session::authorize(const std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    auto authRequired = [this, &requestPtr] () {
        bool authRequired = true;

        if(requestPtr->uri == rtsp::WildcardUri) {
            authRequired = _config->authRequired;
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
        return authorizeRecord(requestPtr);
    case rtsp::Method::LIST:
        if(!rtsp::RequestContentType(*requestPtr).empty())
            return authorizeAgentList(requestPtr);
        else if(authRequired())
            return isValidCookie(authCookie());
    case rtsp::Method::SUBSCRIBE:
    case rtsp::Method::DESCRIBE:
        if(authRequired())
            return isValidCookie(authCookie());
        break;
    }

    return ServerSession::authorize(requestPtr);
}

bool Session::listEnabled(const std::string& uri) noexcept
{
    if(uri == "*")
        return true;

    auto streamerIt = _config->streamers.find(uri);
    if(streamerIt == _config->streamers.end())
        return false;

    if(
        streamerIt->second.type == StreamerConfig::Type::FilePlayer ||
        streamerIt->second.type == StreamerConfig::Type::Proxy)
    {
        return true;
    }

    return false;
}

bool Session::onListRequest(
    std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    const std::string& uri = requestPtr->uri;
    const std::string& contentType = rtsp::RequestContentType(*requestPtr);

    if(!listEnabled(uri))
        return false;

    if(uri == rtsp::WildcardUri) {
        if(!contentType.empty() || !requestPtr->body.empty()) {
            return false;
        } else if(isValidCookie(authCookie())) {
            sendOkResponse(requestPtr->cseq, rtsp::TextParametersContentType, _sharedData->protectedListCache);
        } else {
            sendOkResponse(requestPtr->cseq, rtsp::TextParametersContentType, _sharedData->publicListCache);
        }

        return true;
    }

    auto streamerIt = _config->streamers.find(uri);
    if(streamerIt == _config->streamers.end())
        return false;

    if(streamerIt->second.type != StreamerConfig::Type::Proxy &&
        (!contentType.empty() || !requestPtr->body.empty()))
    {
        return false;
    }

    auto sendCachedListResponse =
        [this, &uri, cseq = requestPtr->cseq] () {
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
                    listIt->second);
            }
        };

    switch(streamerIt->second.type) {
        case StreamerConfig::Type::FilePlayer: {
            sendCachedListResponse();
            return true;
        }
        case StreamerConfig::Type::Proxy: {
            if(contentType.empty()) {
                sendCachedListResponse();
            } else {
                rtsp::Parameters inList;
                if(rtsp::ParseParameters(requestPtr->body, &inList)) {
                    std::string list;
                    for(auto& name2desc: inList) {
                        list += uri;
                        list += rtsp::UriSeparator;
                        list += name2desc.first;
                        list += ": ";
                        list += name2desc.second;
                        list += "\r\n";
                    }
                    _sharedData->agentsMountpoints[uri] = this;
                    _sharedData->mountpointsListsCache[uri] = list;
                    sendOkResponse(requestPtr->cseq);
                } else {
                    return false;
                }
            }
            return true;
        }
        default:
            break;
    }

    return false;
}

bool Session::onSubscribeRequest(
    std::unique_ptr<rtsp::Request>& requestPtr) noexcept
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
        Log()->error("[{}] Second try to subscribe to the same streamer \"{}\"", sessionLogId.c_str(), requestPtr->uri);
        return false;
    }

    rtsp::SessionId mediaSessionId = nextSessionId();
    if(!data.recording) {
        Log()->info("[{}] Streamer \"{}\" not active yet. Subscribing...", sessionLogId.c_str(), requestPtr->uri);
        data.subscriptions.emplace(this, mediaSessionId);
    }

    sendOkResponse(requestPtr->cseq, mediaSessionId);

    if(data.recording) {
        Log()->info("[{}] Streamer \"{}\" already active. Starting record to client...", sessionLogId.c_str(), requestPtr->uri);
        startRecordToClient(requestPtr->uri, mediaSessionId);
    }

    return true;
}

bool Session::handleResponse(
    const rtsp::Request& request,
    std::unique_ptr<rtsp::Response>& responsePtr) noexcept
{
    auto it = _forwardedRequests.find(request.cseq);
    if(it != _forwardedRequests.end()) {
        ForwardedRequest& sourceRequest = it->second;
        bool success = forwardResponse(sourceRequest, request, responsePtr);
        _forwardedRequests.erase(it);
        return success;
    } else
        return ServerSession::handleResponse(request, responsePtr);
}

bool Session::isProxyRequest(const rtsp::Request& request) noexcept
{
    const rtsp::SessionId& mediaSessionId = rtsp::RequestSession(request);
    if(!mediaSessionId.empty() &&
        _agentMediaSessions2clientMediaSession.find(mediaSessionId) != _agentMediaSessions2clientMediaSession.end())
    {
        return true;
    }

    const std::string& uri = request.uri;
    const std::string& streamerName = rtsp::SplitUri(uri).first;
    auto it = _config->streamers.find(streamerName);
    if(it == _config->streamers.end())
        return false;

    return it->second.type == StreamerConfig::Type::Proxy;
}

bool Session::handleProxyRequest(std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    assert(isProxyRequest(*requestPtr));

    const rtsp::SessionId& mediaSessionId = rtsp::RequestSession(*requestPtr);

    if(!mediaSessionId.empty()) {
        auto it = _agentMediaSessions2clientMediaSession.find(mediaSessionId);
        if(it != _agentMediaSessions2clientMediaSession.end()) {
            const MediaSessionInfo& target = it->second;
            std::shared_ptr<SessionHandle> targetSession = target.mediaSessionOwner.lock();
            if(!targetSession) {
                assert(false); // FIXME? send back TEARDOWN for media session
                return false;
            }

            if(requestPtr->method == rtsp::Method::TEARDOWN)
                teardownMediaSession(mediaSessionId);

            std::string sourceUri;
            sourceUri.swap(requestPtr->uri);
            requestPtr->uri = target.uri;
            rtsp::SetRequestSession(requestPtr.get(), target.mediaSession);
            (*targetSession)->forwardRequest(_handle, sourceUri, requestPtr);

            return true;
        }
    }

    auto [streamerName, substream] = rtsp::SplitUri(requestPtr->uri);

    auto agentMountpointIt = _sharedData->agentsMountpoints.find(streamerName);
    if(agentMountpointIt == _sharedData->agentsMountpoints.end())
        return false;

    rtsp::SessionId agentMediaSession;
    if(!mediaSessionId.empty()) {
        auto it = _clientMediaSession2agentMediaSession.find(mediaSessionId);
        if(it != _clientMediaSession2agentMediaSession.end()) {
            agentMediaSession = it->second.mediaSession;
        }

        if(requestPtr->method == rtsp::Method::TEARDOWN)
            teardownMediaSession(mediaSessionId);
    } else {
        assert(requestPtr->method == rtsp::Method::DESCRIBE);
    }

    assert(!substream.empty());
    std::string sourceUri;
    sourceUri.swap(requestPtr->uri);
    requestPtr->uri.swap(substream);
    if(!agentMediaSession.empty())
        rtsp::SetRequestSession(requestPtr.get(), agentMediaSession);

    Session* agentSession = agentMountpointIt->second;
    return agentSession->forwardRequest(_handle, sourceUri, requestPtr);
}

bool Session::forwardRequest(
    std::shared_ptr<SessionHandle>& sourceSession,
    const std::string& sourceUri,
    std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    rtsp::Request* attachedRequest = attachRequest(requestPtr);

    if(requestPtr->cseq != rtsp::CSeq() && !sourceUri.empty()) { // source session doesn't need answer
        assert(!requestPtr->uri.empty());
        const bool added = _forwardedRequests.emplace(
            attachedRequest->cseq,
            ForwardedRequest {
                sourceUri,
                requestPtr->cseq,
                sourceSession }).second;
        assert(added);
    }

    Log()->debug(
        "[{}] Forwarding request:\n"
        "Uri: \"{}\" -> \"{}\"\n"
        "CSeq: {} -> {}",
        sessionLogId.c_str(),
        sourceUri, attachedRequest->uri,
        requestPtr->cseq, attachedRequest->cseq);

    sendRequest(*attachedRequest);

    if(attachedRequest->method == rtsp::Method::TEARDOWN)
        teardownMediaSession(rtsp::RequestSession(*attachedRequest));

    return true;
}

rtsp::SessionId Session::registerAgentMediaSession(
    std::shared_ptr<SessionHandle>& agentSession,
    const std::string& uri,
    const rtsp::SessionId& mediaSession) noexcept
{
    rtsp::SessionId ownMediaSession = nextSessionId();
    bool added = _clientMediaSession2agentMediaSession.emplace(
        ownMediaSession,
        MediaSessionInfo {
            agentSession,
            uri,
            mediaSession }).second;
    assert(added);

    return ownMediaSession;
}

bool Session::forwardResponse(
    ForwardedRequest& sourceRequest,
    const rtsp::Request& request,
    std::unique_ptr<rtsp::Response>& responsePtr) noexcept
{
    const std::string sourceUri = sourceRequest.sourceUri;
    std::shared_ptr<SessionHandle> proxySession = sourceRequest.sourceSession.lock();
    const rtsp::CSeq sourceCSeq = sourceRequest.sourceCSeq;

    if(!proxySession) {
        assert(false); // FIXME! send back TEARDOWN for media session
        return false;
    }

    responsePtr->cseq = sourceCSeq;

    if(responsePtr->statusCode == rtsp::StatusCode::OK) {
        const rtsp::SessionId& mediaSessionId = rtsp::ResponseSession(*responsePtr);
        if(mediaSessionId.empty()) {
            assert(false);
            return false;
        }

        if(request.method == rtsp::Method::DESCRIBE) {
            const rtsp::SessionId clientMediaSessionId =
                (*proxySession)->registerAgentMediaSession(_handle, request.uri, mediaSessionId);
            _agentMediaSessions2clientMediaSession.emplace(
                mediaSessionId,
                MediaSessionInfo {
                    proxySession,
                    sourceUri,
                    clientMediaSessionId });
        }
    }

    (*proxySession)->sendResponse(*responsePtr);

    return true;
}

void Session::forwardTeardown(const MediaSessionInfo& target) noexcept
{
    std::shared_ptr<SessionHandle> targetSession = target.mediaSessionOwner.lock();
    if(!targetSession) // already dead session
        return;

    std::unique_ptr<rtsp::Request> requestPtr =
        std::make_unique<rtsp::Request>();
    requestPtr->method = rtsp::Method::TEARDOWN;
    requestPtr->uri = target.uri;
    requestPtr->cseq = rtsp::CSeq(); // FIXME? no response required
    rtsp::SetRequestSession(requestPtr.get(), target.mediaSession);
    (*targetSession)->forwardRequest(_handle, std::string(), requestPtr);
}

void Session::teardownMediaSession(const rtsp::SessionId& mediaSession) noexcept
{
    assert(!mediaSession.empty());
    if(mediaSession.empty())
        return;

#ifndef NDEBUG
    const bool hasClientSession = _clientMediaSession2agentMediaSession.count(mediaSession) != 0;
    const bool hasAgentSession = _agentMediaSessions2clientMediaSession.count(mediaSession) != 0;
    assert(hasClientSession != hasAgentSession);
#endif

    if(_clientMediaSession2agentMediaSession.erase(mediaSession) == 0 &&
        _agentMediaSessions2clientMediaSession.erase(mediaSession) == 0)
    {
        ServerSession::teardownMediaSession(mediaSession);
    }
}
