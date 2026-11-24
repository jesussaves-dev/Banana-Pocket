#ifndef BANANA_RETRO_CORE_H
#define BANANA_RETRO_CORE_H

#include <stdbool.h>

/* One synchronous core per process. Paths must outlive core_release(). */
bool core_load(const char *core_path, const char *content_path);
bool core_step(unsigned frame);
bool core_release(void);

#endif
