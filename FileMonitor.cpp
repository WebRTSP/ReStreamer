#include "FileMonitor.h"

#include "Log.h"


namespace {

enum {
    MAX_FILES_TO_CLEANUP = 10,
};

const auto Log = ReStreamerLog;

struct GDateTimeLess {
    bool operator() (const GDateTimePtr& l, const GDateTimePtr& r) const {
        return g_date_time_compare(l.get(), r.get()) < 0;
    }
};

struct FileData {
    GFilePtr filePtr;
    guint64 fileSize;
};

void PostDirContent(
    GMainContext* mainContext,
    SessionsSharedData* sharedData,
    FilesMonitorContext* monitorContext)
{
    GSourcePtr idleSourcePtr(g_idle_source_new());
    GSource* idleSource = idleSourcePtr.get();

    struct CallbackData {
        const std::string streamer;
        SessionsSharedData *const sharedData;
        const std::string list;
    };

    std::string list;
    for(const auto& pair: monitorContext->files) {
        list += pair.first;
        list += ": ";

        GDateTimePtr timePtr(g_date_time_new_from_unix_utc(pair.second));
        GCharPtr isoTime(timePtr ? g_date_time_format_iso8601(timePtr.get()) : nullptr);
        if(isoTime) {
            list += isoTime.get();
        } else {
            list += std::to_string(pair.second);
        }

        list += "\r\n";
    }

    CallbackData* callbackData = new CallbackData {
        monitorContext->streamer,
        sharedData,
        std::move(list) };
    g_source_set_callback(
        idleSource,
        [] (gpointer userData) -> gboolean {
            const CallbackData* callbackData = reinterpret_cast<CallbackData*>(userData);

            const std::string& streamer = callbackData->streamer;
            const std::string& list = callbackData->list;

            Log()->debug("Dir content changed for \"{}\"", streamer);
            Log()->trace(list);

            callbackData->sharedData->mountpointsListsCache[streamer] = list;

            return false;
        },
        callbackData,
        [] (gpointer userData) {
            delete reinterpret_cast<CallbackData*>(userData);
        });
    g_source_attach(idleSource, mainContext);
}

void FilesDirChanged(
    GFileMonitor* monitor,
    GFile* file,
    GFile* /*otherFile*/,
    GFileMonitorEvent eventType,
    gpointer userData)
{
    FilesMonitorContext& context = *static_cast<FilesMonitorContext*>(userData);
    const FilesMonitorsContext& monitorsContext = *context.monitorsContext;

    switch(eventType) {
        case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT: {
            g_autofree gchar* fileName = g_file_get_basename(file);
            g_autoptr(GFileInfo) fileInfo =
                g_file_query_info(
                    file,
                    G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_TIME_CREATED,
                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                    NULL,
                    NULL);
            if(g_file_info_get_file_type(fileInfo) != G_FILE_TYPE_REGULAR)
                break;

            g_autoptr(GDateTime) fileTime = g_file_info_get_creation_date_time(fileInfo);
            if(fileName && fileTime) {
                g_autofree gchar* escapedFileName(g_uri_escape_string(fileName, nullptr, false));
                context.files.emplace(
                    escapedFileName,
                    g_date_time_to_unix(fileTime));

                // FIXME! protect from too frequent changes
                PostDirContent(monitorsContext.mainContextPtr.get(), monitorsContext.sharedData, &context);
            } else {
                assert(false); // FIXME?
            }
            break;
        }
        case G_FILE_MONITOR_EVENT_DELETED: {
            g_autofree gchar* fileName = g_file_get_basename(file);
            if(fileName) {
                g_autofree gchar* escapedFileName(g_uri_escape_string(fileName, nullptr, false));

                context.files.erase(escapedFileName);

                // FIXME! protect from too frequent changes
                PostDirContent(monitorsContext.mainContextPtr.get(), monitorsContext.sharedData, &context);
            } else {
                assert(false); // FIXME?
            }
            break;
        }
        default:
            break;
    }
}

void RecordingsDirChanged(
    GFileMonitor* monitor,
    GFile* /*file*/,
    GFile* /*otherFile*/,
    GFileMonitorEvent eventType,
    gpointer userData)
{
    RecordingsMonitorContext& monitorContext = *static_cast<RecordingsMonitorContext*>(userData);

    if(eventType != G_FILE_MONITOR_EVENT_CREATED && eventType != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
        return;

    std::map<GDateTimePtr, FileData, GDateTimeLess> candidatesToDelete;
    guint64 dirSize = 0;

    g_autoptr(GFileEnumerator) enumerator(
        g_file_enumerate_children(
            monitorContext.dirPtr.get(),
            G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
            nullptr,
            nullptr));

    if(enumerator) {
        GFileInfo* childInfo;
        GFile* child;
        for(
            gboolean iterated = g_file_enumerator_iterate(enumerator, &childInfo, &child, nullptr, nullptr);
            iterated && childInfo && child;
            iterated = g_file_enumerator_iterate(enumerator, &childInfo, &child, nullptr, nullptr))
        {
            switch(g_file_info_get_file_type(childInfo)) {
                case G_FILE_TYPE_REGULAR: {
                    const guint64 fileSize = g_file_info_get_size(childInfo);
                    dirSize += fileSize;

                    if(g_autoptr(GDateTime) fileTime = g_file_info_get_modification_date_time(childInfo)) {
                        if(candidatesToDelete.size() < MAX_FILES_TO_CLEANUP ||
                            g_date_time_compare((--candidatesToDelete.end())->first.get(), fileTime) > 0)
                        {
                            candidatesToDelete.emplace(
                                g_date_time_ref(fileTime),
                                FileData {
                                    GFilePtr(G_FILE(g_object_ref(child))),
                                    fileSize });
                        }
                        if(candidatesToDelete.size() > MAX_FILES_TO_CLEANUP) {
                            candidatesToDelete.erase(--candidatesToDelete.end());
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    if(candidatesToDelete.empty())
        return;

    auto it = candidatesToDelete.begin();
    while(it != candidatesToDelete.end() && dirSize > monitorContext.config.maxDirSize) {
        dirSize -= it->second.fileSize;
        g_file_delete(it->second.filePtr.get(), nullptr, nullptr);
        ++it;
    }
}

}

void FilesMonitorsInitAction(
    FilesMonitorsContext& context,
    const std::deque<std::pair<std::string, std::string>>& monitorList)
{
    for(const std::pair<std::string, std::string>& pair: monitorList) {
        GFilePtr monitorDirPtr(g_file_new_for_path(pair.second.c_str()));
        GFileMonitorPtr dirMonitorPtr(
            g_file_monitor_directory(
                monitorDirPtr.get(),
                G_FILE_MONITOR_NONE,
                nullptr,
                nullptr));
        if(dirMonitorPtr) {
            g_file_monitor_set_rate_limit(dirMonitorPtr.get(), 5000);
            FilesMonitorContext& monitorContext =
                context.monitors.emplace_back(
                    &context,
                    pair.first,
                    GFilePtr(g_object_ref(monitorDirPtr.get())),
                    std::move(dirMonitorPtr));
            g_signal_connect(
                monitorContext.monitorPtr.get(),
                "changed",
                G_CALLBACK(FilesDirChanged),
                &monitorContext);

            g_autoptr(GFileEnumerator) enumerator(
                g_file_enumerate_children(
                    monitorDirPtr.get(),
                    G_FILE_ATTRIBUTE_TIME_CREATED,
                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                    nullptr,
                    nullptr));

            if(enumerator) {
                GFileInfo* childInfo;
                GFile* child;
                for(
                    gboolean iterated = g_file_enumerator_iterate(enumerator, &childInfo, &child, nullptr, nullptr);
                    iterated && childInfo && child;
                    iterated = g_file_enumerator_iterate(enumerator, &childInfo, &child, nullptr, nullptr))
                {
                    switch(g_file_info_get_file_type(childInfo)) {
                        case G_FILE_TYPE_REGULAR: {
                            const char* fileName = g_file_info_get_name(childInfo);
                            g_autoptr(GDateTime) fileTime = g_file_info_get_creation_date_time(childInfo);
                            if(fileName && fileTime) {
                                GCharPtr escapedFileNamePtr(g_uri_escape_string(fileName, nullptr, false));
                                monitorContext.files.emplace(
                                    escapedFileNamePtr.get(),
                                    g_date_time_to_unix(fileTime));
                            }
                            break;
                        }
                        default:
                            break;
                    }
                }
            }

            PostDirContent(context.mainContextPtr.get(), context.sharedData, &monitorContext);
        }
    }
}


void RecordingsCleanupInitAction(
    RecordingsCleanupContext& context,
    const std::deque<RecordConfig>& cleanupList)
{
    for(const RecordConfig& config: cleanupList) {
        GFilePtr monitorDirPtr(g_file_new_for_path(config.dir.c_str()));
        GFileMonitorPtr dirMonitorPtr(
            g_file_monitor_directory(
                monitorDirPtr.get(),
                G_FILE_MONITOR_NONE,
                nullptr,
                nullptr));
        if(dirMonitorPtr) {
            g_file_monitor_set_rate_limit(dirMonitorPtr.get(), 5000);
            RecordingsMonitorContext& monitorContext =
                context.monitors.emplace_back(
                    config,
                    std::move(monitorDirPtr),
                    std::move(dirMonitorPtr));
            g_signal_connect(
                monitorContext.monitorPtr.get(),
                "changed",
                G_CALLBACK(RecordingsDirChanged),
                &monitorContext);
        }
    }
}

