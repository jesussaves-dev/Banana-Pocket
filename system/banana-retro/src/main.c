#include "core.h"

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

    if (argc != 5 || strcmp(argv[3], "--frames") != 0) {
        fprintf(stderr, "Usage: %s CORE CONTENT --frames COUNT\n", argv[0]);
        return 2;
    }

    errno = 0;
    frames = strtoul(argv[4], &end, 10);
    if (argv[4][0] < '0' || argv[4][0] > '9' || *end != '\0' ||
        errno != 0 || frames == 0 || frames > MAX_FRAMES) {
        fprintf(stderr, "Frame count must be between 1 and %u\n", (unsigned)MAX_FRAMES);
        return 2;
    }

    ok = core_load(argv[1], argv[2]);
    for (unsigned frame = 1; ok && frame <= frames; ++frame)
        ok = core_step(frame);

    if (!core_release())
        ok = false;

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
