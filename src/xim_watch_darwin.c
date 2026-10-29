#include "xim_watch_darwin.h"

#ifdef __APPLE__
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdlib.h>

struct path_update
{
    CFArrayRef paths;
    unsigned long long generation;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static dispatch_queue_t control_queue;
static dispatch_queue_t event_queue;
static FSEventStreamRef stream;
static unsigned long long latest_generation;

static void no_op(void *context)
{
    (void)context;
}

static void send_stream_failure(void)
{
    xim_darwin_fsevents_events(0, NULL, NULL, XIM_DARWIN_STREAM_DROPPED);
}

static void deliver_events(ConstFSEventStreamRef stream_ref,
        void *context, size_t count, void *raw_paths,
        const FSEventStreamEventFlags event_flags[],
        const FSEventStreamEventId event_ids[])
{
    (void)stream_ref;
    (void)context;
    (void)event_ids;
    if (count > 4096)
    {
        send_stream_failure();
        return;
    }

    char **paths = (char **)raw_paths;
    uint32_t *translated = count == 0 ? NULL
        : (uint32_t *)calloc(count, sizeof(*translated));
    if (count != 0 && translated == NULL)
    {
        send_stream_failure();
        return;
    }
    uint32_t stream_flags = 0;
    for (size_t i = 0; i < count; ++i)
    {
        FSEventStreamEventFlags flags = event_flags[i];
        if (flags & (kFSEventStreamEventFlagMustScanSubDirs
                    | kFSEventStreamEventFlagUserDropped
                    | kFSEventStreamEventFlagKernelDropped
                    | kFSEventStreamEventFlagEventIdsWrapped))
            stream_flags |= XIM_DARWIN_STREAM_DROPPED;
        if (flags & kFSEventStreamEventFlagRootChanged)
            translated[i] |= XIM_DARWIN_ROOT_CHANGED;
        if (flags & kFSEventStreamEventFlagItemCreated)
            translated[i] |= XIM_DARWIN_ITEM_CREATED;
        if (flags & kFSEventStreamEventFlagItemRemoved)
            translated[i] |= XIM_DARWIN_ITEM_REMOVED;
        if (flags & kFSEventStreamEventFlagItemRenamed)
            translated[i] |= XIM_DARWIN_ITEM_RENAMED;
        if (flags & kFSEventStreamEventFlagItemModified)
            translated[i] |= XIM_DARWIN_ITEM_MODIFIED;
        if (flags & (kFSEventStreamEventFlagItemInodeMetaMod
                    | kFSEventStreamEventFlagItemFinderInfoMod
                    | kFSEventStreamEventFlagItemChangeOwner
                    | kFSEventStreamEventFlagItemXattrMod))
            translated[i] |= XIM_DARWIN_ITEM_ATTRIB;
        if (flags & kFSEventStreamEventFlagItemIsDir)
            translated[i] |= XIM_DARWIN_ITEM_IS_DIR;
    }
    xim_darwin_fsevents_events(count,
            (const char *const *)paths, translated, stream_flags);
    free(translated);
}

static void stop_stream(void)
{
    if (stream == NULL)
        return;
    FSEventStreamStop(stream);
    FSEventStreamInvalidate(stream);
    FSEventStreamRelease(stream);
    stream = NULL;
    // Complete callbacks that were queued before stop before the owner can
    // release its event context or path state.
    dispatch_sync_f(event_queue, NULL, no_op);
}

static void apply_update(void *context)
{
    struct path_update *update = (struct path_update *)context;
    pthread_mutex_lock(&lock);
    int current = update->generation == latest_generation;
    pthread_mutex_unlock(&lock);
    if (!current)
    {
        CFRelease(update->paths);
        free(update);
        return;
    }

    stop_stream();
    if (CFArrayGetCount(update->paths) > 0)
    {
        stream = FSEventStreamCreate(kCFAllocatorDefault, deliver_events,
                NULL, update->paths, kFSEventStreamEventIdSinceNow, 0.05,
                kFSEventStreamCreateFlagFileEvents
                    | kFSEventStreamCreateFlagWatchRoot
                    | kFSEventStreamCreateFlagNoDefer);
        if (stream == NULL)
            send_stream_failure();
        else
        {
            FSEventStreamSetDispatchQueue(stream, event_queue);
            if (!FSEventStreamStart(stream))
            {
                FSEventStreamInvalidate(stream);
                FSEventStreamRelease(stream);
                stream = NULL;
                dispatch_sync_f(event_queue, NULL, no_op);
                send_stream_failure();
            }
        }
    }
    CFRelease(update->paths);
    free(update);
}

static void stop_task(void *context)
{
    (void)context;
    stop_stream();
}

int xim_darwin_fsevents_set_paths(const char *const paths[], size_t count)
{
    CFMutableArrayRef path_array = CFArrayCreateMutable(kCFAllocatorDefault,
            (CFIndex)count, &kCFTypeArrayCallBacks);
    if (path_array == NULL)
        return 0;
    for (size_t i = 0; i < count; ++i)
    {
        CFStringRef path = CFStringCreateWithCString(kCFAllocatorDefault,
                paths[i], kCFStringEncodingUTF8);
        if (path == NULL)
        {
            CFRelease(path_array);
            return 0;
        }
        CFArrayAppendValue(path_array, path);
        CFRelease(path);
    }

    struct path_update *update =
        (struct path_update *)malloc(sizeof(*update));
    if (update == NULL)
    {
        CFRelease(path_array);
        return 0;
    }
    update->paths = path_array;
    pthread_mutex_lock(&lock);
    if (control_queue == NULL)
    {
        control_queue = dispatch_queue_create("org.xim.fsevents.control",
                DISPATCH_QUEUE_SERIAL);
        event_queue = dispatch_queue_create("org.xim.fsevents.delivery",
                DISPATCH_QUEUE_SERIAL);
    }
    if (control_queue == NULL || event_queue == NULL)
    {
        pthread_mutex_unlock(&lock);
        CFRelease(path_array);
        free(update);
        return 0;
    }
    update->generation = ++latest_generation;
    pthread_mutex_unlock(&lock);
    dispatch_sync_f(control_queue, update, apply_update);
    return 1;
}

void xim_darwin_fsevents_shutdown(void)
{
    pthread_mutex_lock(&lock);
    dispatch_queue_t queue = control_queue;
    if (queue != NULL)
        ++latest_generation;
    pthread_mutex_unlock(&lock);
    if (queue == NULL)
        return;
    dispatch_sync_f(queue, NULL, stop_task);
    dispatch_sync_f(event_queue, NULL, no_op);
}
#endif
