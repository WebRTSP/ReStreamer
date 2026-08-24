#include "BasicServerSession.h"

#include <glib.h>

#include "RtspParser/RtspParser.h"
#include "RtspParser/RtspSerialize.h"
#include "Helpers/TurnRestApi.h"

#include "Log.h"


BasicServerSession::BasicServerSession(
    const Config* config,
    SharedData* sharedData,
    const std::optional<std::string>& authCookie,
    const CreatePeer& createPeer,
    const rtsp::Session::SendRequest& sendRequest,
    const rtsp::Session::SendResponse& sendResponse) noexcept :
    StreamSession(
        config->webRTCConfig,
        createPeer,
        sendRequest,
        sendResponse),
    _config(config),
    _sharedData(sharedData),
    _authCookie(authCookie)
{
}

BasicServerSession::~BasicServerSession()
{
}

bool BasicServerSession::hasValidCookie() const noexcept
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

bool BasicServerSession::authorize(const std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    switch(requestPtr->method) {
    case rtsp::Method::OPTIONS:
    case rtsp::Method::DESCRIBE:
    case rtsp::Method::SETUP:
    case rtsp::Method::PLAY:
    case rtsp::Method::TEARDOWN:
    case rtsp::Method::GET_PARAMETER:
        return (_config->authRequired) ? hasValidCookie() : true;
    default:
        return false;
    }
}
