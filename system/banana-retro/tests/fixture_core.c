#include "libretro.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum stage {
    NEW, VERSION_CHECKED, ENVIRONMENT_SET, VIDEO_SET, SAMPLE_SET, BATCH_SET,
    POLL_SET, INPUT_SET, INITIALIZED, SYSTEM_QUERIED, GAME_LOADED, AV_QUERIED,
    CONTROLLER_SET, GAME_UNLOADED
};

static enum stage stage;
static retro_environment_t environment;
static retro_video_refresh_t video;
static retro_audio_sample_t sample;
static retro_audio_sample_batch_t batch;
static retro_input_poll_t poll_input;
static retro_input_state_t read_input;
static unsigned frame;
static uint8_t save_ram[4], save_rtc[2];
static const char *content_bytes;
static const char expected_content[] = "BRC1\n";

static void require(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "Fixture assertion failed: %s\n", message);
        exit(90);
    }
}

static void advance(enum stage prior, enum stage next, const char *event)
{
    require(stage == prior, event);
    stage = next;
    fprintf(stderr, "fixture %s\n", event);
}

static bool mode_is(const char *name)
{
    const char *mode = getenv("BANANA_RETRO_FIXTURE_MODE");
    return mode != NULL && strcmp(mode, name) == 0;
}

unsigned retro_api_version(void)
{
    advance(NEW, VERSION_CHECKED, "api");
#ifdef FIXTURE_WRONG_API
    return RETRO_API_VERSION + 1;
#else
    return RETRO_API_VERSION;
#endif
}

void retro_set_environment(retro_environment_t cb)
{
    advance(VERSION_CHECKED, ENVIRONMENT_SET, "set_environment");
    require(cb != NULL, "environment callback");
    environment = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
    advance(ENVIRONMENT_SET, VIDEO_SET, "set_video");
    require(cb != NULL, "video callback");
    video = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
    advance(VIDEO_SET, SAMPLE_SET, "set_sample");
    require(cb != NULL, "audio sample callback");
    sample = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
    advance(SAMPLE_SET, BATCH_SET, "set_batch");
    require(cb != NULL, "audio batch callback");
    batch = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
    advance(BATCH_SET, POLL_SET, "set_poll");
    require(cb != NULL, "input poll callback");
    poll_input = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
    advance(POLL_SET, INPUT_SET, "set_input");
    require(cb != NULL, "input state callback");
    read_input = cb;
}

void retro_init(void)
{
    advance(INPUT_SET, INITIALIZED, "init");
}

void retro_get_system_info(struct retro_system_info *info)
{
    advance(INITIALIZED, SYSTEM_QUERIED, "system_info");
    *info = (struct retro_system_info){
        .library_name = "Banana lifecycle fixture",
        .library_version = "1",
        .valid_extensions = "rom|gba|gb|nes",
        .need_fullpath = mode_is("path"),
        .block_extract = true
    };
}

bool retro_load_game(const struct retro_game_info *game)
{
    require(stage == SYSTEM_QUERIED, "load order");
    fputs("fixture load\n", stderr);
    require(game != NULL && game->path != NULL && game->meta == NULL,
            "content metadata");

    bool can_dupe = true;
    struct retro_hw_render_callback hardware = {.context_type = RETRO_HW_CONTEXT_OPENGLES2};
    enum retro_pixel_format format = mode_is("xrgb") ?
                                     RETRO_PIXEL_FORMAT_XRGB8888 :
                                     RETRO_PIXEL_FORMAT_RGB565;
    require(environment(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe) && !can_dupe,
            "duplicate capability");
    require(!environment(RETRO_ENVIRONMENT_SET_HW_RENDER, &hardware), "unsupported hardware");
    require(!environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, NULL), "null format");
    if (!mode_is("default-format"))
        require(environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format), "pixel format");

    if (mode_is("path")) {
        char bytes[sizeof(expected_content)];
        require(game->data == NULL && game->size == 0, "path-only content");
        FILE *file = fopen(game->path, "rb");
        require(file != NULL, "open path-only content");
        size_t length = fread(bytes, 1, sizeof(bytes), file);
        require(fclose(file) == 0, "close path-only content");
        if (length != sizeof(expected_content) - 1 ||
            memcmp(bytes, expected_content, length) != 0)
            return false;
    } else {
        require(game->data != NULL && game->size != 0, "memory content");
        if (game->size != sizeof(expected_content) - 1 ||
            memcmp(game->data, expected_content, game->size) != 0)
            return false;
        content_bytes = game->data;
    }

    stage = GAME_LOADED;
    return true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    advance(GAME_LOADED, AV_QUERIED, "av_info");
    *info = (struct retro_system_av_info){
        .geometry = {2, 2, 2, 2, 0.0f},
        .timing = {60.0, mode_is("bad-av") ? NAN : 48000.0}
    };
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    advance(AV_QUERIED, CONTROLLER_SET, "controller");
    require(port == 0 && device == RETRO_DEVICE_JOYPAD, "RetroPad on port zero");
}

void retro_run(void)
{
    require(stage == CONTROLLER_SET, "run order");
    ++frame;
    fprintf(stderr, "fixture run %u\n", frame);

    poll_input();
    require(read_input(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A) == 0,
            "neutral RetroPad");
    require(read_input(1, RETRO_DEVICE_ANALOG, 0, RETRO_DEVICE_ID_ANALOG_X) == 0,
            "neutral unsupported input");

    if (mode_is("xrgb")) {
        uint32_t pixels[2][3] = {{0x00123400 + frame, 0, 0}, {0, 0, 0}};
        video(pixels, 2, 2, sizeof(pixels[0]));
    } else {
        uint16_t pixels[2][3] = {{(uint16_t)(0x2300 + frame), 0, 0}, {0, 0, 0}};
        video(pixels, 2, 2, mode_is("bad-pitch") ? 0 : sizeof(pixels[0]));
    }

    int16_t stereo[] = {(int16_t)frame, -2, 3, -4, (int16_t)(4 + frame), -6};
    if (mode_is("samples")) {
        for (size_t i = 0; i < sizeof(stereo) / sizeof(stereo[0]); i += 2)
            sample(stereo[i], stereo[i + 1]);
    } else {
        require(batch(stereo, 3) == 3, "audio batch acceptance");
        require(batch(NULL, 0) == 0, "empty audio batch");
    }
}

void retro_unload_game(void)
{
    require(stage == AV_QUERIED || stage == CONTROLLER_SET, "unload order");
    if (content_bytes != NULL)
        require(memcmp(content_bytes, expected_content, sizeof(expected_content) - 1) == 0,
                "content lifetime through unload");
    content_bytes = NULL;
    stage = GAME_UNLOADED;
    fputs("fixture unload\n", stderr);
}

void retro_deinit(void)
{
    require(stage == GAME_UNLOADED || stage == SYSTEM_QUERIED, "deinit order");
    stage = NEW;
    fputs("fixture deinit\n", stderr);
}

void retro_reset(void) {}
size_t retro_serialize_size(void) { return sizeof(frame); }
bool retro_serialize(void *data, size_t len)
{
    if (data == NULL || len != sizeof(frame)) return false;
    memcpy(data, &frame, sizeof(frame));
    return true;
}
bool retro_unserialize(const void *data, size_t len)
{
    if (data == NULL || len != sizeof(frame)) return false;
    memcpy(&frame, data, sizeof(frame));
    return true;
}
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index;
    (void)enabled;
    (void)code;
}
bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t count)
{
    (void)type;
    (void)info;
    (void)count;
    return false;
}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned id)
{
    if (id == RETRO_MEMORY_SAVE_RAM) return save_ram;
    if (id == RETRO_MEMORY_RTC) return save_rtc;
    return NULL;
}
#ifndef FIXTURE_MISSING_EXPORT
size_t retro_get_memory_size(unsigned id)
{
    if (id == RETRO_MEMORY_SAVE_RAM) return sizeof(save_ram);
    if (id == RETRO_MEMORY_RTC) return sizeof(save_rtc);
    return 0;
}
#endif
