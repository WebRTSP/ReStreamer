#include "Log.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>


static std::shared_ptr<spdlog::logger> Logger;

void InitReStreamerLogger(spdlog::level::level_enum level)
{
    spdlog::sink_ptr sink = std::make_shared<spdlog::sinks::stdout_sink_st>();

    Logger = std::make_shared<spdlog::logger>("ReStreamer", sink);

    Logger->set_level(level);
}

const std::shared_ptr<spdlog::logger>& ReStreamerLog()
{
    if(!Logger)
#ifndef NDEBUG
        InitReStreamerLogger(spdlog::level::debug);
#else
        InitReStreamerLogger(spdlog::level::info);
#endif

    return Logger;
}
