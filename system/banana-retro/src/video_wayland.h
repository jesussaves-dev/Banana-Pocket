#ifndef BANANA_RETRO_VIDEO_WAYLAND_H
#define BANANA_RETRO_VIDEO_WAYLAND_H

#include "core.h"

struct wayland_video;

struct wayland_video *wayland_video_open(void);
bool wayland_video_configure(struct wayland_video *video, const struct retro_game_geometry *geometry);
bool wayland_video_submit(void *video, const void *pixels, unsigned width, unsigned height,
                          size_t pitch, enum retro_pixel_format format,
                          const struct retro_game_geometry *geometry, unsigned rotation);
bool wayland_video_wait(struct wayland_video *video);
bool wayland_video_presented(const struct wayland_video *video);
void wayland_video_close(struct wayland_video *video);

#endif
