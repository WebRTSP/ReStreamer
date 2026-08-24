#pragma once

#include <map>
#include <deque>

#include <CxxPtr/GlibPtr.h>
#include <CxxPtr/GioPtr.h>

#include "Config.h"
#include "SessionsSharedData.h"


struct FilesMonitorsContext;

struct FilesMonitorContext {
    FilesMonitorContext(
        const FilesMonitorsContext *const monitorsContext,
        const std::string& streamer,
        GFilePtr&& dirPtr,
        GFileMonitorPtr&& monitor) :
        monitorsContext(monitorsContext),
        streamer(streamer),
        dirPtr(std::move(dirPtr)),
        monitorPtr(std::move(monitor)) {}

    const FilesMonitorsContext *const monitorsContext;
    const std::string streamer;
    GFilePtr dirPtr;
    GFileMonitorPtr monitorPtr;
    std::map<std::string, uint64_t> files; // file name -> file timestamp
};

struct FilesMonitorsContext {
    FilesMonitorsContext(GMainContextPtr&& mainContextPtr, SessionsSharedData* sharedData) :
        mainContextPtr(std::move(mainContextPtr)), sharedData(sharedData) {}

    const GMainContextPtr mainContextPtr;
    SessionsSharedData *const sharedData;
    std::deque<FilesMonitorContext> monitors;
};

void FilesMonitorsInitAction(
    FilesMonitorsContext& context,
    const std::deque<std::pair<std::string, std::string>>& monitorList);

struct RecordingsMonitorContext {
    RecordingsMonitorContext(const RecordConfig& config, GFilePtr&& dirPtr, GFileMonitorPtr&& monitor) :
        config(config), dirPtr(std::move(dirPtr)), monitorPtr(std::move(monitor)) {}

    const RecordConfig config;
    GFilePtr dirPtr;
    GFileMonitorPtr monitorPtr;
};

struct RecordingsCleanupContext {
    std::deque<RecordingsMonitorContext> monitors;
};

void RecordingsCleanupInitAction(
    RecordingsCleanupContext& context,
    const std::deque<RecordConfig>& cleanupList);
