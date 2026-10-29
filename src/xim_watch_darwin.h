#pragma once

#ifdef __APPLE__
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum xim_darwin_event_flag
{
    XIM_DARWIN_STREAM_DROPPED = 1u << 0,
    XIM_DARWIN_ROOT_CHANGED = 1u << 1,
    XIM_DARWIN_ITEM_CREATED = 1u << 2,
    XIM_DARWIN_ITEM_REMOVED = 1u << 3,
    XIM_DARWIN_ITEM_RENAMED = 1u << 4,
    XIM_DARWIN_ITEM_MODIFIED = 1u << 5,
    XIM_DARWIN_ITEM_ATTRIB = 1u << 6,
    XIM_DARWIN_ITEM_IS_DIR = 1u << 7
};

/* The input paths are copied before this function returns. */
int xim_darwin_fsevents_set_paths(const char *const paths[], size_t count);
void xim_darwin_fsevents_shutdown(void);

/* Called on the FSEvents delivery queue. The arrays are valid for the call. */
void xim_darwin_fsevents_events(size_t count, const char *const paths[],
        const uint32_t event_flags[], uint32_t stream_flags);

#ifdef __cplusplus
}
#endif
#endif
