#pragma once

#include <map>
#include <unordered_map>

#include "Signalling/ServerSession.h"

#include "Config.h"
#include "SessionsSharedData.h"


class Session : public ServerSession
{
public:
    typedef ::SessionAuthTokenData AuthTokenData;
    typedef ::RecordMountpointData RecordMountpointData;
    typedef SessionsSharedData SharedData;

    Session(
        const Config*,
        SharedData*,
        const CreatePeer& createPeer,
        const rtsp::Session::SendRequest& sendRequest,
        const rtsp::Session::SendResponse& sendResponse) noexcept;
    Session(
        const Config*,
        SharedData*,
        const CreatePeer& createPeer,
        const CreatePeer& createRecordPeer,
        const rtsp::Session::SendRequest& sendRequest,
        const rtsp::Session::SendResponse& sendResponse) noexcept;
    ~Session();

protected:
    bool listEnabled(const std::string& uri) noexcept override;
    bool playEnabled(const std::string& uri) noexcept override;
    bool recordEnabled(const std::string& uri) noexcept override;
    bool subscribeEnabled(const std::string& uri) noexcept override;
    bool authorizeAgent(const std::unique_ptr<rtsp::Request>& requestPtr) noexcept;
    bool isValidCookie(const std::optional<std::string>& authCookie) noexcept;
    bool authorize(const std::unique_ptr<rtsp::Request>&) noexcept override;

#if !defined(BUILD_AS_CAMERA_STREAMER) && !defined(BUILD_AS_V4L2_STREAMER)
    bool onGetParameterRequest(
        std::unique_ptr<rtsp::Request>&) noexcept override;
#endif
    bool onListRequest(
        std::unique_ptr<rtsp::Request>&) noexcept override;
    bool onSubscribeRequest(
        std::unique_ptr<rtsp::Request>&) noexcept override;

    bool handleResponse(
        const rtsp::Request&,
        std::unique_ptr<rtsp::Response>&) noexcept override;

    bool isProxyRequest(const rtsp::Request&) noexcept override;

    bool handleProxyRequest(
        std::unique_ptr<rtsp::Request>&) noexcept override;

private:
    class SessionHandle;
    struct ForwardedRequest {
        std::string sourceUri;
        rtsp::CSeq sourceCSeq;
        std::weak_ptr<SessionHandle> sourceSession;
    };

    struct MediaSessionInfo {
        std::weak_ptr<SessionHandle> mediaSessionOwner;
        std::string uri;
        rtsp::MediaSessionId mediaSession;
    };

    void startRecord(const std::string& uri, const rtsp::MediaSessionId& mediaSession) noexcept;

    rtsp::MediaSessionId registerAgentMediaSession(
        std::shared_ptr<SessionHandle>& agentSession,
        const std::string& uri,
        const rtsp::MediaSessionId& mediaSession) noexcept;

    bool forwardRequest(
        std::shared_ptr<SessionHandle>& sourceSession,
        const std::string& sourceUri,
        std::unique_ptr<rtsp::Request>& requestPtr) noexcept;
    bool forwardResponse(
        ForwardedRequest& sourceRequest,
        const rtsp::Request&,
        std::unique_ptr<rtsp::Response>&) noexcept;
    void forwardTeardown(const MediaSessionInfo&) noexcept;

    void teardownMediaSession(const rtsp::MediaSessionId&) noexcept override;

private:
    const Config *const _config;
    SharedData *const _sharedData;
    std::shared_ptr<SessionHandle> _handle;

    // reqest target side data
    std::map<rtsp::CSeq, ForwardedRequest> _forwardedRequests;

    // client side data
    std::map<rtsp::MediaSessionId, MediaSessionInfo> _clientMediaSession2agentMediaSession;

    // agent side data
    std::map<rtsp::MediaSessionId, MediaSessionInfo> _agentMediaSessions2clientMediaSession;
};
