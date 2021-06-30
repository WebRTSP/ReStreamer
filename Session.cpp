#include "Session.h"

#include <glib.h>

#include "RtspParser/RtspParser.h"


Session::Session(
    const Config* config,
    Cache* cache,
    const std::function<std::unique_ptr<WebRTCPeer> (const std::string& uri)>& createPeer,
    const std::function<void (const rtsp::Request*)>& sendRequest,
    const std::function<void (const rtsp::Response*)>& sendResponse) noexcept :
    ServerSession(createPeer, sendRequest, sendResponse),
    _config(config), _cache(cache)
{
}

bool Session::onOptionsRequest(
    std::unique_ptr<rtsp::Request>& requestPtr) noexcept
{
    rtsp::Response response;
    prepareOkResponse(requestPtr->cseq, &response);

    response.headerFields.emplace("Public", "LIST, DESCRIBE, SETUP, PLAY, TEARDOWN");

    sendResponse(response);

    return true;
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
