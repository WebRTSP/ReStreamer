#include "Session.h"

#include <glib.h>

#include "RtspParser/RtspParser.h"


Session::Session(
    const Config* config,
    Cache* cache,
    const std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)>& createPeer,
    const std::function<void (const rtsp::Request*)>& sendRequest,
    const std::function<void (const rtsp::Response*)>& sendResponse) noexcept :
    ServerSession(config->iceServers, createPeer,sendRequest, sendResponse),
    _config(config), _cache(cache)
{
}

Session::Session(
    const Config* config,
    Cache* cache,
    const std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)>& createPeer,
    const std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)>& createRecordPeer,
    const std::function<void (const rtsp::Request*)>& sendRequest,
    const std::function<void (const rtsp::Response*)>& sendResponse) noexcept :
    ServerSession( config->iceServers, createPeer,createRecordPeer, sendRequest, sendResponse),
    _config(config), _cache(cache)
{
}

bool Session::recordEnabled(const std::string& uri) noexcept
{
    auto it = _config->streamers.find(uri);
    return
        it != _config->streamers.end() &&
        it->second.type == StreamerConfig::Type::Record;
}

bool Session::authorize(const std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    if(requestPtr->method != rtsp::Method::RECORD)
        return ServerSession::authorize(requestPtr);

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

bool Session::onListRequest(
    std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    if(_cache->list.empty()) {
        if(_config->streamers.empty())
            _cache->list = "\r\n";
        else {
            for(const auto& pair: _config->streamers) {
                _cache->list += pair.first;
                _cache->list += ": ";
                _cache->list += pair.second.description;
                _cache->list += + "\r\n";
            }
        }
    }

    sendOkResponse(requestPtr->cseq, "text/parameters", _cache->list);

    return true;
}
