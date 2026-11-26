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

/* SHA-256 of the exact content bytes this core consumed, available once core_load()
 * has succeeded. Bind save and checkpoint identity to it, never to a second read of the path. */
bool core_content_digest(unsigned char digest[SAVES_CONTENT_DIGEST_SIZE]);
/* One synchronous core per process. Paths and video callbacks must outlive core_release(). */
bool core_load(const char *core_path, const char *content_path, const struct core_video *video);
const char *core_name(void);
const struct retro_game_geometry *core_geometry(void);
bool core_step(unsigned frame);
bool core_save_memory_sizes(size_t *sram_size, size_t *rtc_size);
enum { CORE_OPTIONS_FINGERPRINT_SIZE = 4097 };
bool core_option_set(const char *key, const char *value);
bool core_options_fingerprint(char fingerprint[CORE_OPTIONS_FINGERPRINT_SIZE]);

enum core_load_result {
    CORE_LOAD_OK,
    CORE_LOAD_REJECTED,
    /* The core may have been partially mutated by unserialize. */
    CORE_LOAD_FAILED
};
/* These wrappers run only between core frames, while the core is quiescent. */
enum core_load_result core_persistence_load(struct saves *saves, int state_slot);
bool core_persistence_commit(struct saves *saves, int state_slot);
bool core_persistence_flush(struct saves *saves, bool battery_only);

enum core_checkpoint_result {
    CORE_CHECKPOINT_OK,
    CORE_CHECKPOINT_UNAVAILABLE,
    CORE_CHECKPOINT_ERROR
};
enum core_checkpoint_result core_persistence_checkpoint(struct saves *saves, char token[65]);
enum saves_resume_result core_persistence_restore_checkpoint(struct saves *saves, const char *token);
bool core_release(void);

#endif
