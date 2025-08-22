#include "stun.h"

#include <string>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <gio/gio.h>

#include <stun/usages/bind.h>

#include "Log.h"


#define DEFAULT_STUN_SERVICE "3478"

G_DEFINE_AUTOPTR_CLEANUP_FUNC(addrinfo, freeaddrinfo)


std::optional<std::string> DetectPublicIP(const WebRTCConfig& webRTCConfig)
{
    const auto& log = ReStreamerLog();

    std::optional<std::string> stunUrl;

    for(const std::string& iceServer: webRTCConfig.iceServers) {
        if(0 == iceServer.compare(0, 5, "stun:")) {
            stunUrl = iceServer;
            break;
        }
    }
    if(!stunUrl) {
        stunUrl = "stun://stun.l.google.com:19302";
        log->info("There is no STUN server provided in config. Using hardcoded one to detect public IP: {}", *stunUrl);
    }

    g_autoptr(GError) error = nullptr;
    g_autoptr(GUri) uri = g_uri_parse(stunUrl->c_str(), G_URI_FLAGS_HAS_PASSWORD, &error);
    if(!uri) {
        log->error("Failed to parse STUN server URL \"{}\": {}", *stunUrl, error->message);
        return {};
    }

    const gchar* scheme = g_uri_get_scheme(uri);
    const gchar* host = g_uri_get_host(uri);
    gint port = g_uri_get_port(uri);

    addrinfo hints {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_DGRAM
    };

    for(guint i = 0; i < 5; ++i) {
        if(i != 0) {
            const guint baseDelay = 1 << std::min(i, 8u);
            const int delay = g_random_int_range(baseDelay,  baseDelay << 1);
            log->info("Sleeping for {} seconds before try to get public IP another time...", delay);
            sleep(delay);
        }

        g_autofree gchar* service = (port != -1) ? g_strdup_printf("%i", port) : nullptr;
        g_autoptr(addrinfo) addrinfos = nullptr;
        if(int ret = getaddrinfo(host, service ? service : DEFAULT_STUN_SERVICE, &hints, &addrinfos)) {
            log->error("Failed to resolve \"{}\": {}", host, gai_strerror(ret));
            return {};
        }

        addrinfo* ai = addrinfos;
        sockaddr_storage publicAddressStorage;
        socklen_t publicAddressStorageLen = sizeof(publicAddressStorage);

        const StunUsageBindReturn bindRet =
            stun_usage_bind_run(
                ai->ai_addr,
                ai->ai_addrlen,
                &publicAddressStorage,
                &publicAddressStorageLen);
        if(bindRet != STUN_USAGE_BIND_RETURN_SUCCESS) {
            log->error("Failed to do STUN bind requst");
            continue;
        }
        assert(publicAddressStorage.ss_family == AF_INET);

        const sockaddr_in& publicAddress = *reinterpret_cast<sockaddr_in*>(&publicAddressStorage);

        char publicIp[INET6_ADDRSTRLEN];
        if(!inet_ntop(publicAddress.sin_family, &publicAddress.sin_addr, publicIp, sizeof(publicIp))) {
            log->error("Failed to stringize IP address");
            continue;
        }

        log->info("Detected public IP: {}", publicIp);

        return publicIp;
    }

    log->warn("Failed to detect public IP");
    return {};
}
