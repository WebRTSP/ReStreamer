#include "SnapCoturnHelpers.h"

#include <glib.h>

#include "Log.h"


namespace {

enum {
    COTURN_STOP_TRY_COUNT = 5,
};

const auto Log = ReStreamerLog;

bool TryStopCoturn(bool disable)
{
    g_autoptr(GError) error = nullptr;
    gint exitStatus = 0;

    const gchar* snapName = g_getenv("SNAP_NAME");
    if(!snapName) {
        Log()->error("Can't get SNAP_NAME environment variable");

        return false;
    }

    g_autofree gchar* command =
        disable ?
            g_strdup_printf("snapctl stop %s.Coturn --disable", snapName) :
            g_strdup_printf("snapctl stop %s.Coturn", snapName);
    if(!g_spawn_command_line_sync(
        command,
        nullptr,
        nullptr,
        &exitStatus,
        &error) ||
        !g_spawn_check_wait_status(exitStatus, &error))
    {
        Log()->error(
            fmt::runtime(
                disable ?
                    "Failed to disable Coturn: {}" :
                    "Failed to stop Coturn: {}"),
            error->message);

        return false;
    }

    Log()->info(
        disable ?
            "Coturn disabled" :
            "Coturn stopped");

    return true;
}

}

void ConfigureCoturn(Config* config)
{
    assert(config->useServerMode() && config->agentsConfig.useCoturn);

    const gchar* snapName = g_getenv("SNAP_NAME");
    if(!snapName) {
        Log()->error("Can't get SNAP_NAME environment variable");

        return;
    }

    const gchar* snapCommon = g_getenv("SNAP_COMMON");
    if(!snapCommon) {
        Log()->error("Can't get SNAP_COMMON environment variable");

        return;
    }

    g_autoptr(GError) error = nullptr;
    gint exitStatus = 0;

    g_autofree gchar* setPublicIPCommand = nullptr;
    if(config->publicIp.has_value())
        setPublicIPCommand = g_strdup_printf("snapctl set public-ip=%s", config->publicIp->c_str());
    const gchar* unsetPublicIPCommand = "snapctl unset public-ip";
    if(!g_spawn_command_line_sync(
        setPublicIPCommand ? setPublicIPCommand : unsetPublicIPCommand,
        nullptr,
        nullptr,
        &exitStatus,
        &error) ||
        !g_spawn_check_wait_status(exitStatus, &error))
    {
        Log()->error("Failed to set \"public-ip\": {}", error->message);

        return;
    }

    g_autofree gchar* pwgenStdout = nullptr;
    if(!g_spawn_command_line_sync(
        "pwgen --secure --capitalize 127",
        &pwgenStdout,
        nullptr,
        &exitStatus,
        &error) ||
        !g_spawn_check_wait_status(exitStatus, &error))
    {
        Log()->error("Failed to generate TURN REST API secret", error->message);

        return;
    }

    std::string staticAuthSecret = pwgenStdout;
    if(staticAuthSecret.back() == '\n')
        staticAuthSecret.pop_back();

    g_autofree gchar* deleteSecretsCmd = g_strdup_printf(
        "turnadmin --db=%s/turndb --delete-all-secret --realm=%s",
        snapCommon,
        snapName);

    if(!g_spawn_command_line_sync(
        deleteSecretsCmd,
        nullptr,
        nullptr,
        &exitStatus,
        &error) ||
        !g_spawn_check_wait_status(exitStatus, &error))
    {
        Log()->error("Failed to delete old TURN REST API secrets: {}", error->message);

        return;
    }

    g_autofree gchar* setSecretCmd = g_strdup_printf(
        "turnadmin --db=%s/turndb --set-secret=%s --realm=%s",
        snapCommon,
        staticAuthSecret.c_str(),
        snapName);

    if(!g_spawn_command_line_sync(
        setSecretCmd,
        nullptr,
        nullptr,
        &exitStatus,
        &error) ||
        !g_spawn_check_wait_status(exitStatus, &error))
    {
        Log()->error("Failed to set TURN REST API secret: {}", error->message);

        return;
    }

    g_autofree gchar* startCommand =
        g_strdup_printf("snapctl start %s.Coturn", snapName);
    if(!g_spawn_command_line_sync(
        startCommand,
        nullptr,
        nullptr,
        &exitStatus,
        &error) ||
        !g_spawn_check_wait_status(exitStatus, &error))
    {
        Log()->error("Failed to enable Coturn: {}", error->message);

        return;
    }

    Log()->info("Coturn configured and started");

    config->coturnConfig.staticAuthSecret.emplace(std::move(staticAuthSecret));
}

bool StopCoturn(bool disable)
{
    // to workaround "error running snapctl: snap "rtsp-to-webrtsp" has "install-snap" change in progress"
    // have to try multiple times
    for(guint i = 0; i <= COTURN_STOP_TRY_COUNT; ++i) {
        if(i != 0) {
            const int delay = i;
            Log()->info("Sleeping for {} seconds before try to disable Coturn another time...", delay);
            sleep(delay);
        }

        if(TryStopCoturn(disable))
            return true;
    }

    return false;
}
