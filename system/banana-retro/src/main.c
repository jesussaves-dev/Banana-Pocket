#include "video_wayland.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_FRAMES = 10000 };

int main(int argc, char **argv)
{
    unsigned long frames;
    char *end;
    bool ok;
    struct wayland_video *video = NULL;
    struct core_video sink = { .submit = wayland_video_submit };
    bool native = argc == 6 && strcmp(argv[5], "--wayland") == 0;

    if ((!native && argc != 5) || strcmp(argv[3], "--frames") != 0) {
        fprintf(stderr, "Usage: %s CORE CONTENT --frames COUNT [--wayland]\n", argv[0]);
        return 2;
    }

    errno = 0;
    frames = strtoul(argv[4], &end, 10);
    if (argv[4][0] < '0' || argv[4][0] > '9' || *end != '\0' ||
        errno != 0 || frames == 0 || frames > MAX_FRAMES) {
        fprintf(stderr, "Frame count must be between 1 and %u\n", (unsigned)MAX_FRAMES);
        return 2;
    }

    if (native) {
        video = wayland_video_open();
        if (!video)
            return EXIT_FAILURE;
        sink.data = video;
    }
    ok = core_load(argv[1], argv[2], native ? &sink : NULL);
    if (ok && video)
        ok = wayland_video_configure(video, core_geometry());
    for (unsigned frame = 1; ok && frame <= frames; ++frame)
        ok = core_step(frame) && (!video || wayland_video_wait(video));

    if (ok && video && !wayland_video_presented(video)) {
        fputs("No native Wayland frame reached the display\n", stderr);
        ok = false;
    }

    if (!core_release())
        ok = false;

    wayland_video_close(video);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
