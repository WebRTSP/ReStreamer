#include "Log.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>


static std::shared_ptr<spdlog::logger> Logger;


void InitReStreamerLogger(spdlog::level::level_enum level)
{
    if(!Logger) {
        Logger = spdlog::stdout_logger_st("ReStreamer");
#ifdef SNAPCRAFT_BUILD
        Logger->set_pattern("[%n] [%l] %v");
#endif
    }

    Logger->set_level(level);
}

const std::shared_ptr<spdlog::logger>& ReStreamerLog()
{
    if(!Logger) {
#ifdef NDEBUG
        InitReStreamerLogger(spdlog::level::info);
#else
        InitReStreamerLogger(spdlog::level::debug);
#endif
    }

    return Logger;
}
