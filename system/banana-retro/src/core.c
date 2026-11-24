#define _XOPEN_SOURCE 700

#include "core.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { MAX_CONTENT_BYTES = 64 * 1024 * 1024 };

struct core_api {
    unsigned (*retro_api_version)(void);
    void (*retro_set_environment)(retro_environment_t);
    void (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_audio_sample)(retro_audio_sample_t);
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t);
    void (*retro_set_input_state)(retro_input_state_t);
    void (*retro_init)(void);
    void (*retro_get_system_info)(struct retro_system_info *);
    bool (*retro_load_game)(const struct retro_game_info *);
    void (*retro_get_system_av_info)(struct retro_system_av_info *);
    void (*retro_set_controller_port_device)(unsigned, unsigned);
    void (*retro_run)(void);
    void (*retro_unload_game)(void);
    void (*retro_deinit)(void);
};

struct frame_observation {
    unsigned width;
    unsigned height;
    size_t pitch;
    uint32_t first_pixel;
    size_t video_frames;
    size_t audio_frames;
    int16_t first_left;
    int16_t first_right;
    int16_t last_left;
    int16_t last_right;
    size_t input_polls;
    size_t input_queries;
    bool invalid;
};

static struct {
    void *library;
    struct core_api api;
    bool initialized;
    bool loaded;
    void *content;
    struct retro_system_av_info av;
    struct core_video video;
    unsigned capacity_width, capacity_height;
    unsigned rotation;
    bool running;
    enum retro_pixel_format pixel_format;
    struct frame_observation frame;
} session;

static void *required_export(const char *name)
{
    dlerror();
    void *symbol = dlsym(session.library, name);
    const char *error = dlerror();
    if (error != NULL || symbol == NULL) {
        fprintf(stderr, "Missing core export %s\n", name);
        return NULL;
    }
    return symbol;
}

static bool resolve_exports(void)
{
    /* POSIX supports obtaining function pointers from dlsym. Copy their
     * representation to keep the build free of non-C99 casts. */
#define RESOLVE(member) do { \
    void *symbol = required_export(#member); \
    if (symbol == NULL) \
        return false; \
    if (sizeof(session.api.member) != sizeof(symbol)) { \
        fputs("Unsupported function pointer representation\n", stderr); \
        return false; \
    } \
    memcpy(&session.api.member, &symbol, sizeof(symbol)); \
} while (0)

    RESOLVE(retro_api_version);
    RESOLVE(retro_set_environment);
    RESOLVE(retro_set_video_refresh);
    RESOLVE(retro_set_audio_sample);
    RESOLVE(retro_set_audio_sample_batch);
    RESOLVE(retro_set_input_poll);
    RESOLVE(retro_set_input_state);
    RESOLVE(retro_init);
    RESOLVE(retro_get_system_info);
    RESOLVE(retro_load_game);
    RESOLVE(retro_get_system_av_info);
    RESOLVE(retro_set_controller_port_device);
    RESOLVE(retro_run);
    RESOLVE(retro_unload_game);
    RESOLVE(retro_deinit);
#undef RESOLVE

    static const char *const unused_exports[] = {
        "retro_reset", "retro_serialize_size", "retro_serialize",
        "retro_unserialize", "retro_cheat_reset", "retro_cheat_set",
        "retro_load_game_special", "retro_get_region",
        "retro_get_memory_data", "retro_get_memory_size"
    };
    for (size_t i = 0; i < sizeof(unused_exports) / sizeof(unused_exports[0]); ++i) {
        if (required_export(unused_exports[i]) == NULL)
            return false;
    }
    return true;
}

static bool valid_av(const struct retro_system_av_info *av);

static bool set_geometry(const struct retro_game_geometry *geometry)
{
    if (geometry->base_width == 0 || geometry->base_height == 0 ||
        geometry->base_width > session.av.geometry.max_width ||
        geometry->base_height > session.av.geometry.max_height ||
        !isfinite(geometry->aspect_ratio))
        return false;

    session.av.geometry.base_width = geometry->base_width;
    session.av.geometry.base_height = geometry->base_height;
    session.av.geometry.aspect_ratio = geometry->aspect_ratio;
    return true;
}

static bool environment(unsigned command, void *data)
{
    if (data == NULL)
        return false;

    switch (command) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = session.video.submit != NULL;
        return true;
    case RETRO_ENVIRONMENT_SET_ROTATION:
        if (!session.video.submit || *(const unsigned *)data > 3)
            return false;
        session.rotation = *(const unsigned *)data;
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        return session.video.submit && session.running && set_geometry(data);
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
        const struct retro_system_av_info *av = data;
        if (!session.video.submit || !session.running || !valid_av(av) ||
            av->geometry.max_width > session.capacity_width ||
            av->geometry.max_height > session.capacity_height)
            return false;
        session.av = *av;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        break;
    default:
        return false;
    }

    enum retro_pixel_format format = *(const enum retro_pixel_format *)data;
    switch (format) {
    case RETRO_PIXEL_FORMAT_0RGB1555:
    case RETRO_PIXEL_FORMAT_RGB565:
    case RETRO_PIXEL_FORMAT_XRGB8888:
        session.pixel_format = format;
        return true;
    default:
        return false;
    }
}

static void video(const void *data, unsigned width, unsigned height, size_t pitch)
{
    struct frame_observation *frame = &session.frame;
    size_t pixel_size = session.pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;

    if (data != NULL &&
        (data == RETRO_HW_FRAME_BUFFER_VALID || width == 0 || height == 0 ||
         (session.loaded && (width > session.av.geometry.max_width ||
                             height > session.av.geometry.max_height)) ||
         width > pitch / pixel_size || height > SIZE_MAX / pitch)) {
        frame->invalid = true;
        return;
    }

    if (session.video.submit) {
        if (session.running &&
            !session.video.submit(session.video.data, data, width, height, pitch,
                                  session.pixel_format, &session.av.geometry, session.rotation))
            frame->invalid = true;
        return;
    }
    if (data == NULL)
        return;
    if (pixel_size == 4) {
        memcpy(&frame->first_pixel, data, sizeof(frame->first_pixel));
    } else {
        uint16_t pixel;
        memcpy(&pixel, data, sizeof(pixel));
        frame->first_pixel = pixel;
    }
    frame->width = width;
    frame->height = height;
    frame->pitch = pitch;
    ++frame->video_frames;
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
    struct frame_observation *frame = &session.frame;

    if (frames == 0)
        return 0;
    if (data == NULL || frames > SIZE_MAX / (2 * sizeof(*data)) ||
        frames > SIZE_MAX - frame->audio_frames) {
        frame->invalid = true;
        return 0;
    }

    if (frame->audio_frames == 0) {
        frame->first_left = data[0];
        frame->first_right = data[1];
    }
    frame->last_left = data[2 * (frames - 1)];
    frame->last_right = data[2 * (frames - 1) + 1];
    frame->audio_frames += frames;
    return frames;
}

static void audio_sample(int16_t left, int16_t right)
{
    int16_t stereo[] = {left, right};
    audio_batch(stereo, 1);
}

static void input_poll(void)
{
    ++session.frame.input_polls;
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)port;
    (void)device;
    (void)index;
    (void)id;
    ++session.frame.input_queries;
    return 0;
}

static ssize_t read_retry(int fd, void *data, size_t bytes)
{
    ssize_t n;
    do {
        n = read(fd, data, bytes);
    } while (n < 0 && errno == EINTR);
    return n;
}

static bool prepare_content(struct retro_game_info *game, bool need_fullpath)
{
    struct stat info;
    bool ok = false;
    /* A FIFO should fail the regular-file check without blocking open(). */
    int fd = open(game->path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "Cannot open content %s: %s\n", game->path, strerror(errno));
        return false;
    }

    if (fstat(fd, &info) < 0) {
        fprintf(stderr, "Cannot stat content %s: %s\n", game->path, strerror(errno));
        goto done;
    }
    if (!S_ISREG(info.st_mode) || info.st_size == 0) {
        fputs("Content must be a nonempty regular file\n", stderr);
        goto done;
    }
    if (info.st_size < 0 || info.st_size > MAX_CONTENT_BYTES) {
        fputs("Content exceeds the 64 MiB limit\n", stderr);
        goto done;
    }

    if (!need_fullpath) {
        size_t size = (size_t)info.st_size;
        unsigned char *data = malloc(size);
        if (data == NULL) {
            fputs("Cannot allocate content buffer\n", stderr);
            goto done;
        }
        session.content = data;
        size_t offset = 0;
        while (offset < size) {
            ssize_t n = read_retry(fd, data + offset, size - offset);
            if (n <= 0) {
                fputs("Cannot read complete content file\n", stderr);
                goto done;
            }
            offset += (size_t)n;
        }

        unsigned char extra;
        if (read_retry(fd, &extra, 1) != 0) {
            fputs("Content changed or could not be read\n", stderr);
            goto done;
        }
        game->data = data;
        game->size = size;
    }
    ok = true;
done:
    if (close(fd) < 0) {
        fprintf(stderr, "Cannot close content: %s\n", strerror(errno));
        ok = false;
    }
    return ok;
}

static bool valid_av(const struct retro_system_av_info *av)
{
    const struct retro_game_geometry *geometry = &av->geometry;
    const struct retro_system_timing *timing = &av->timing;

    return geometry->base_width > 0 && geometry->base_height > 0 &&
           geometry->base_width <= geometry->max_width &&
           geometry->base_height <= geometry->max_height &&
           isfinite(geometry->aspect_ratio) && (session.video.submit || geometry->aspect_ratio >= 0.0f) &&
           isfinite(timing->fps) && timing->fps > 0.0 &&
           isfinite(timing->sample_rate) && timing->sample_rate > 0.0;
}

bool core_load(const char *core_path, const char *content_path, const struct core_video *video_sink)
{
    struct core_api *api = &session.api;
    struct retro_system_info info = {0};
    struct retro_game_info game = {.path = content_path};

    char *absolute = realpath(core_path, NULL);
    if (absolute == NULL) {
        fprintf(stderr, "Cannot open core %s: %s\n", core_path, strerror(errno));
        return false;
    }
    session.library = dlopen(absolute, RTLD_NOW | RTLD_LOCAL);
    free(absolute);
    if (session.library == NULL) {
        fprintf(stderr, "Cannot load core %s: %s\n", core_path, dlerror());
        return false;
    }
    if (!resolve_exports())
        return false;

    unsigned version = api->retro_api_version();
    if (version != RETRO_API_VERSION) {
        fprintf(stderr, "Unsupported Libretro API version %u (expected %u)\n",
                version, (unsigned)RETRO_API_VERSION);
        return false;
    }

    if (video_sink)
        session.video = *video_sink;
    session.pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
    api->retro_set_environment(environment);
    api->retro_set_video_refresh(video);
    api->retro_set_audio_sample(audio_sample);
    api->retro_set_audio_sample_batch(audio_batch);
    api->retro_set_input_poll(input_poll);
    api->retro_set_input_state(input_state);
    api->retro_init();
    session.initialized = true;
    api->retro_get_system_info(&info);
    if (!prepare_content(&game, info.need_fullpath))
        return false;
    if (!api->retro_load_game(&game)) {
        fputs("Core rejected content\n", stderr);
        return false;
    }
    session.loaded = true;

    api->retro_get_system_av_info(&session.av);
    if (!valid_av(&session.av)) {
        fputs("Core returned invalid AV information\n", stderr);
        return false;
    }
    session.capacity_width = session.av.geometry.max_width;
    session.capacity_height = session.av.geometry.max_height;
    api->retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    return true;
}

const struct retro_game_geometry *core_geometry(void)
{
    return &session.av.geometry;
}

bool core_step(unsigned frame)
{
    session.frame = (struct frame_observation){0};
    session.running = true;
    session.api.retro_run();
    session.running = false;

    const struct frame_observation *result = &session.frame;
    if (result->invalid) {
        fputs("Core returned invalid frame data\n", stderr);
        return false;
    }
    if (session.video.submit)
        return true;
    return printf("frame %u video=%zu %ux%u pitch=%zu pixel=%08" PRIx32
                  " audio=%zu first=%d,%d last=%d,%d input=%zu/%zu\n",
                  frame, result->video_frames, result->width, result->height,
                  result->pitch, result->first_pixel, result->audio_frames,
                  (int)result->first_left, (int)result->first_right,
                  (int)result->last_left, (int)result->last_right,
                  result->input_polls, result->input_queries) >= 0;
}

bool core_release(void)
{
    bool ok = true;

    if (session.loaded)
        session.api.retro_unload_game();
    if (session.initialized)
        session.api.retro_deinit();
    if (session.library != NULL && dlclose(session.library) != 0) {
        fprintf(stderr, "Cannot close core: %s\n", dlerror());
        ok = false;
    }
    free(session.content);
    memset(&session, 0, sizeof(session));
    return ok;
}
