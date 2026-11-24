#ifndef BANANA_RETRO_CORE_H
#define BANANA_RETRO_CORE_H

#include "libretro.h"

struct core_video {
    void *data;
    bool (*submit)(void *data, const void *pixels, unsigned width, unsigned height,
                   size_t pitch, enum retro_pixel_format format,
                   const struct retro_game_geometry *geometry, unsigned rotation);
};

/* One synchronous core per process. Paths and video callbacks must outlive core_release(). */
bool core_load(const char *core_path, const char *content_path, const struct core_video *video);
const struct retro_game_geometry *core_geometry(void);
bool core_step(unsigned frame);
bool core_release(void);

#endif
