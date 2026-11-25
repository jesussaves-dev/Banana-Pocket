#define main banana_retro_main
#include "../src/main.c"
#undef main

#include <assert.h>
#include <dlfcn.h>

static void check_options(struct native_runtime *runtime, uint8_t *sram, uint8_t *rtc,
                          unsigned (*unserialize_calls)(void))
{
    *sram = 0x31;
    *rtc = 0x41;
    assert(control_save_state(runtime, 0));
    assert(power_checkpoint(runtime));
    char token[65];
    assert(saves_resume_available(runtime->saves, token));
    *sram = 0x32;
    *rtc = 0x42;
    assert(control_flush(runtime));
    assert(core_persistence_restore_checkpoint(runtime->saves, token));
    assert(*sram == 0x31 && *rtc == 0x41);
    assert(core_persistence_load(runtime->saves, -1));
    assert(*sram == 0x32 && *rtc == 0x42);

    bool restart = false;
    assert(control_set_option(runtime, "fixture_mode", "second",
                              BANANA_RETRO_CTL_SCOPE_SESSION, &restart) && restart);
    *sram = 0xa1;
    *rtc = 0xb1;
    unsigned calls = unserialize_calls();
    assert(!core_persistence_load(runtime->saves, 0));
    assert(*sram == 0xa1 && *rtc == 0xb1 && unserialize_calls() == calls);
    assert(control_save_state(runtime, 1));
    assert(core_persistence_load(runtime->saves, 1));
    assert(!power_checkpoint(runtime));

    assert(core_step(2));
    assert(strcmp(core_option_get("fixture_mode"), "third") == 0);
    *sram = 0xa2;
    *rtc = 0xb2;
    calls = unserialize_calls();
    assert(!core_persistence_load(runtime->saves, 1));
    assert(*sram == 0xa2 && *rtc == 0xb2 && unserialize_calls() == calls);
    assert(control_save_state(runtime, 2));
    assert(core_persistence_load(runtime->saves, 2));
    assert(!power_checkpoint(runtime));

    assert(control_set_option(runtime, "fixture_mode", "first",
                              BANANA_RETRO_CTL_SCOPE_SESSION, &restart) && restart);
    assert(!core_persistence_load(runtime->saves, 2));
    assert(core_persistence_load(runtime->saves, 0));
    char recovered[65];
    assert(saves_resume_available(runtime->saves, recovered) && strcmp(token, recovered) == 0);

    runtime->started = false;
    assert(!control_save_state(runtime, 0));
    assert(!power_checkpoint(runtime));
    assert(!control_flush(runtime));
    assert(control_quit(runtime) && runtime->stopped && !runtime->committed);
    assert(saves_resume_available(runtime->saves, recovered) && strcmp(token, recovered) == 0);
}

static void check_quit(struct native_runtime *runtime, uint8_t *sram, uint8_t *rtc)
{
    *sram = 0x33;
    *rtc = 0x43;
    assert(control_quit(runtime) && runtime->stopped && runtime->committed);
    *sram = 0;
    *rtc = 0;
    assert(core_persistence_load(runtime->saves, -1));
    assert(*sram == 0x33 && *rtc == 0x43);

    runtime->stopped = false;
    runtime->committed = false;
    *sram = 0x34;
    *rtc = 0x44;
    assert(control_shutdown(runtime) && runtime->stopped && runtime->committed);
    *sram = 0;
    *rtc = 0;
    assert(core_persistence_load(runtime->saves, -1));
    assert(*sram == 0x34 && *rtc == 0x44);
    char token[65];
    assert(!saves_resume_available(runtime->saves, token));
}

int main(int argc, char **argv)
{
    if (argc != 6 || strcmp(argv[1], "--check-persistence") != 0)
        return banana_retro_main(argc, argv);

    bool options = strcmp(argv[2], "options") == 0;
    assert(options || strcmp(argv[2], "quit") == 0);
    if (options) assert(setenv("BANANA_RETRO_FIXTURE_MODE", "persistence-options", 1) == 0);
    assert(core_load(argv[3], argv[4], NULL));
    struct saves *saves = open_saves_profile(argv[5], argv[3], argv[4]);
    assert(saves);
    struct native_runtime runtime = { .saves = saves, .started = true, .save_state_slot = -1 };
    assert(core_step(1));

    void *fixture = dlopen(argv[3], RTLD_NOW | RTLD_LOCAL);
    assert(fixture);
    void *(*memory)(unsigned) = NULL;
    void *symbol = dlsym(fixture, "retro_get_memory_data");
    assert(symbol && sizeof(memory) == sizeof(symbol));
    memcpy(&memory, &symbol, sizeof(memory));
    uint8_t *sram = memory(RETRO_MEMORY_SAVE_RAM), *rtc = memory(RETRO_MEMORY_RTC);
    assert(sram && rtc);
    if (options) {
        unsigned (*calls)(void) = NULL;
        symbol = dlsym(fixture, "fixture_unserialize_calls");
        assert(symbol && sizeof(calls) == sizeof(symbol));
        memcpy(&calls, &symbol, sizeof(calls));
        check_options(&runtime, sram, rtc, calls);
        saves_close(saves);
        saves = open_saves_profile(argv[5], argv[3], argv[4]);
        assert(saves);
        assert(!core_persistence_load(saves, 1) && !core_persistence_load(saves, 2));
        assert(core_persistence_load(saves, 0));
    } else check_quit(&runtime, sram, rtc);

    assert(core_release());
    assert(dlclose(fixture) == 0);
    saves_close(saves);
    return EXIT_SUCCESS;
}
