#pragma once

#include <memory>

#include <spdlog/spdlog.h>


void InitReStreamerLogger(spdlog::level::level_enum level);
const std::shared_ptr<spdlog::logger>& ReStreamerLog();
std::shared_ptr<spdlog::logger> MakeReStreamerLogger(const std::string& context);
