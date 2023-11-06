#include "Session.h"

#include <glib.h>

#include "RtspParser/RtspParser.h"

#include "Log.h"


static const auto Log = ReStreamerLog;

Session::Session(
    const Config* config,
    SharedData* sharedData,
    const CreatePeer& createPeer,
    const rtsp::Session::SendRequest& sendRequest,
    const rtsp::Session::SendResponse& sendResponse) noexcept :
    ServerSession(config->iceServers, createPeer,sendRequest, sendResponse),
    _config(config), _sharedData(sharedData)
{
}

Session::Session(
    const Config* config,
    SharedData* sharedData,
    const CreatePeer& createPeer,
    const CreatePeer& createRecordPeer,
    const rtsp::Session::SendRequest& sendRequest,
    const rtsp::Session::SendResponse& sendResponse) noexcept :
    ServerSession(config->iceServers, createPeer, createRecordPeer, sendRequest, sendResponse),
    _config(config), _sharedData(sharedData)
{
}

Session::~Session() {
    for(auto& pair :_sharedData->recordMountpointsData) {
        RecordMountpointData& data = pair.second;
        data.subscriptions.erase(this);
    }
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

    if(it->second.recordToken.empty())
        return true;

    const std::pair<rtsp::Authentication, std::string> authPair =
        rtsp::ParseAuthentication(*requestPtr);

    if(authPair.first != rtsp::Authentication::Bearer) // FIXME? only Bearer supported atm
        return false;

    return authPair.second == it->second.recordToken;
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
    switch(requestPtr->method) {
    case rtsp::Method::RECORD:
        return authorizeRecord(requestPtr);
    case rtsp::Method::SUBSCRIBE:
    case rtsp::Method::DESCRIBE:
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

        if(authRequired) {
            return isValidCookie(authCookie());
        }
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

    if(streamerIt->second.type == StreamerConfig::Type::FilePlayer)
        return true;

    return false;
}

bool Session::onListRequest(
    std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    const std::string& uri = requestPtr->uri;

    if(!listEnabled(uri))
        return false;

    if(uri == "*") {
        if(isValidCookie(authCookie())) {
            sendOkResponse(requestPtr->cseq, "text/parameters", _sharedData->protectedListCache);
        } else {
            sendOkResponse(requestPtr->cseq, "text/parameters", _sharedData->publicListCache);
        }

        return true;
    }

    auto streamerIt = _config->streamers.find(uri);
    if(streamerIt == _config->streamers.end())
        return false;

    if(streamerIt->second.type != StreamerConfig::Type::FilePlayer) {
        sendOkResponse(
            requestPtr->cseq,
            "text/parameters",
            fmt::format("{}: {}\r\n", uri, streamerIt->second.description));
        return true;
    }

    auto listIt = _sharedData->mountpointsListsCache.find(uri);
    if(listIt == _sharedData->mountpointsListsCache.end()) {
        sendOkResponse(
            requestPtr->cseq,
            "text/parameters",
            "\r\n");
        return true;
    } else {
        sendOkResponse(
            requestPtr->cseq,
            "text/parameters",
            listIt->second);
        return true;
    }
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
        Log()->error("Second try to subscribe to the same streamer \"{}\"", requestPtr->uri);
        return false;
    }

    rtsp::SessionId mediaSessionId = nextSessionId();
    if(!data.recording) {
        Log()->info("Streamer \"{}\" not active yet. Subscribing...", requestPtr->uri);
        data.subscriptions.emplace(this, mediaSessionId);
    }

    sendOkResponse(requestPtr->cseq, mediaSessionId);

    if(data.recording) {
        Log()->info("Streamer \"{}\" already active. Starting record to client...", requestPtr->uri);
        startRecordToClient(requestPtr->uri, mediaSessionId);
    }

    return true;
}
