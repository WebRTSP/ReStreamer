#pragma once

#include "Http/Config.h"

#include "Config.h"


int ReStreamerMain(
    const http::Config&,
    const Config&,
    bool useGlobalDefaultContext);
