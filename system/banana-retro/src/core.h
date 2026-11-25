#ifndef BANANA_RETRO_CORE_H
#define BANANA_RETRO_CORE_H

#include "libretro.h"
#include "saves.h"

struct core_video {
    void *data;
    bool (*submit)(void *data, const void *pixels, unsigned width, unsigned height,
                   size_t pitch, enum retro_pixel_format format,
                   const struct retro_game_geometry *geometry, unsigned rotation);
};

/* One synchronous core per process. Paths and video callbacks must outlive core_release(). */
bool core_load(const char *core_path, const char *content_path, const struct core_video *video);
const char *core_name(void);
const struct retro_game_geometry *core_geometry(void);
bool core_step(unsigned frame);
bool core_save_memory_sizes(size_t *sram_size, size_t *rtc_size);
enum { CORE_OPTIONS_FINGERPRINT_SIZE = 4097 };
bool core_option_set(const char *key, const char *value);
bool core_option_set(const char *key, const char *value);
bool core_options_fingerprint(char fingerprint[CORE_OPTIONS_FINGERPRINT_SIZE]);
bool core_persistence_checkpoint(struct saves *saves);
bool core_persistence_restore_checkpoint(struct saves *saves, const char *token);
bool core_release(void);

#endif
