#include "Session.h"

#include <glib.h>

#include "RtspParser/RtspParser.h"


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

bool Session::recordEnabled(const std::string& uri) noexcept
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

bool Session::authorize(
    const std::unique_ptr<rtsp::Request>& requestPtr,
    const std::optional<std::string>& authCookie) noexcept
{
    switch(requestPtr->method) {
    case rtsp::Method::RECORD:
        return authorizeRecord(requestPtr);
    case rtsp::Method::DESCRIBE:
        if(_config->authorizeDescribe)
            break;
        else
            return ServerSession::authorize(requestPtr, authCookie);
    default:
        return ServerSession::authorize(requestPtr, authCookie);
    }

    if(!_config->authorizeDescribe)
        return true;

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

bool Session::onListRequest(
    std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    if(_sharedData->listCache.empty()) {
        if(_config->streamers.empty())
            _sharedData->listCache = "\r\n";
        else {
            for(const auto& pair: _config->streamers) {
                _sharedData->listCache += pair.first;
                _sharedData->listCache += ": ";
                _sharedData->listCache += pair.second.description;
                _sharedData->listCache += + "\r\n";
            }
        }
    }

    sendOkResponse(requestPtr->cseq, "text/parameters", _sharedData->listCache);

    return true;
}
