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

std::shared_ptr<spdlog::logger> MakeReStreamerLogger(const std::string& context)
{
    const std::shared_ptr<spdlog::logger>& logger = ReStreamerLog();

    if(context.empty()) {
        return logger;
    } else {
        // have to go long road to avoid issues with duplicated names in loggers registry
        std::shared_ptr<spdlog::logger> loggerWithContext = std::make_shared<spdlog::logger>(
            logger->name(),
            std::make_shared<spdlog::sinks::stdout_sink_st>());
        loggerWithContext->set_level(logger->level());

#ifdef SNAPCRAFT_BUILD
        loggerWithContext->set_pattern("[" + context + "] [%n] [%l] %v");
#else
        loggerWithContext->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [" + context + "] [%n] [%l] %v");
#endif

        return loggerWithContext;
    }
}
