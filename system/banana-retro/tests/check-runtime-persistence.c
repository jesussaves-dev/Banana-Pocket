#define main banana_retro_main
#include "../src/main.c"
#undef main

#include <assert.h>
#include <dlfcn.h>
#include <sys/wait.h>

static bool fail_resume_unlink;
int __real_unlinkat(int dirfd, const char *path, int flags);

int __wrap_unlinkat(int dirfd, const char *path, int flags)
{
    if (fail_resume_unlink && strcmp(path, "RESUME") == 0) {
        fail_resume_unlink = false;
        errno = EIO;
        return -1;
    }
    return __real_unlinkat(dirfd, path, flags);
}

struct fixture {
    uint8_t *sram, *rtc;
    unsigned (*serialize_calls)(void);
    unsigned (*unserialize_calls)(void);
    unsigned (*effective_mode)(void);
};

static void fixture_export(void *library, const char *name, void *dest, size_t size)
{
    void *symbol = dlsym(library, name);
    assert(symbol && size == sizeof(symbol));
    memcpy(dest, &symbol, size);
}

static void assert_resume_token(struct saves *saves, const char *expected)
{
    char token[65];
    assert(saves_resume_probe(saves, token) == SAVES_RESUME_READY);
    assert(strcmp(token, expected) == 0);
}

static void set_option(struct native_runtime *runtime, const char *value, bool expected_change)
{
    bool restart = !expected_change;
    assert(control_set_option(runtime, "fixture_mode", value,
                              BANANA_RETRO_CTL_SCOPE_SESSION, &restart));
    assert(restart == expected_change);
}

static void check_options(struct native_runtime *runtime, const struct fixture *fixture,
                          bool core_change)
{
    uint8_t *sram = fixture->sram, *rtc = fixture->rtc;
    *sram = 0x31;
    *rtc = 0x41;
    set_option(runtime, "first", false);
    bool restart = false;
    assert(!control_set_option(runtime, "fixture_mode", "invalid",
                               BANANA_RETRO_CTL_SCOPE_SESSION, &restart));
    assert(control_save_state(runtime, 0));
    assert(power_checkpoint(runtime));
    char token[65];
    assert(saves_resume_probe(runtime->saves, token) == SAVES_RESUME_READY);

    *sram = 0x32;
    *rtc = 0x42;
    assert(control_flush(runtime));
    assert(core_persistence_restore_checkpoint(runtime->saves, token) == SAVES_RESUME_READY);
    assert(*sram == 0x31 && *rtc == 0x41);
    assert(core_persistence_load(runtime->saves, -1) == CORE_LOAD_OK);
    assert(*sram == 0x32 && *rtc == 0x42);

    if (core_change)
        assert(setenv("BANANA_RETRO_TEST_CORE_SET_OPTION", "1", 1) == 0);
    else
        set_option(runtime, "second", true);
    assert(core_step(2));
    assert(strcmp(core_option_get("fixture_mode"), core_change ? "third" : "second") == 0);
    const unsigned effective = core_change ? 2u : 1u;
    assert(fixture->effective_mode() == effective);

    *sram = 0xa1;
    *rtc = 0xb1;
    const unsigned serialized = fixture->serialize_calls();
    const unsigned restored = fixture->unserialize_calls();
    assert(!control_load_state(runtime, 0) && !runtime->stopped && !runtime->failed_load);
    assert(!control_save_state(runtime, 1));
    set_option(runtime, "first", true);
    assert(fixture->effective_mode() == effective);
    assert(!control_load_state(runtime, 0) && !runtime->stopped && !runtime->failed_load);
    assert(core_persistence_restore_checkpoint(runtime->saves, token) == SAVES_RESUME_RETRY);
    assert(*sram == 0xa1 && *rtc == 0xb1 && fixture->unserialize_calls() == restored);
    assert(!control_save_state(runtime, 2));
    assert(!power_checkpoint(runtime) && fixture->serialize_calls() == serialized);
    assert(!saves_state_present(runtime->saves, 1) && !saves_state_present(runtime->saves, 2));
    assert_resume_token(runtime->saves, token);

    runtime->started = false;
    assert(!control_save_state(runtime, 0));
    assert(!power_checkpoint(runtime));
    assert(!control_flush(runtime));
    assert(control_quit(runtime) && runtime->stopped && !runtime->committed);
    assert_resume_token(runtime->saves, token);

    runtime->started = true;
    runtime->stopped = false;
    assert(control_quit(runtime) && runtime->stopped && runtime->committed);
    assert(saves_resume_probe(runtime->saves, token) == SAVES_RESUME_NONE);
    assert(fixture->serialize_calls() == serialized);
}

static void check_battery_startup(char **argv, const char *expected)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(setenv("BANANA_RETRO_NATIVE_DIR", argv[5], 1) == 0);
        assert(setenv("BANANA_RETRO_TEST_PERSISTENCE", "1", 1) == 0);
        assert(setenv("BANANA_RETRO_TEST_QUICK_RESUME", "1", 1) == 0);
        assert(setenv("BANANA_RETRO_TEST_EXPECT_INITIAL_BATTERY", expected, 1) == 0);
        execl(argv[0], argv[0], argv[3], argv[4], "--frames", "1", (char *)NULL);
        _exit(127);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == EXIT_SUCCESS);
}

static void check_battery_only_shutdown(struct native_runtime *runtime, const struct fixture *fixture,
                                        char **argv, bool interrupt)
{
    *fixture->sram = 0x11;
    *fixture->rtc = 0x21;
    assert(power_checkpoint(runtime));
    char token[65];
    assert(saves_resume_probe(runtime->saves, token) == SAVES_RESUME_READY);

    *fixture->sram = 0x12;
    *fixture->rtc = 0x22;
    assert(setenv("BANANA_RETRO_TEST_REJECT_SERIALIZE", "1", 1) == 0);
    /* Battery, manifest, then the resume cutoff. A failed cutoff fsync must
     * leave the old checkpoint eligible and fail the shutdown. */
    saves_test_fail_event(SAVES_TEST_FILE_FSYNC, 3);
    assert(!control_shutdown(runtime) && !runtime->stopped && !runtime->committed);
    assert_resume_token(runtime->saves, token);
    if (interrupt) {
        const enum saves_test_event boundaries[] = {
            SAVES_TEST_AFTER_GENERATION_RENAME, SAVES_TEST_AFTER_CURRENT_RENAME,
        };
        for (unsigned i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
            pid_t child = fork();
            assert(child >= 0);
            if (child == 0) {
                saves_close(runtime->saves);
                runtime->saves = open_saves_profile(argv[5], argv[3], argv[4]);
                assert(runtime->saves);
                saves_test_crash_event(boundaries[i]);
                (void)control_shutdown(runtime);
                _exit(2);
            }
            int status;
            assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                   WEXITSTATUS(status) == 86);
            if (i == 0) assert_resume_token(runtime->saves, token);
        }
    } else {
        fail_resume_unlink = true;
        assert(control_shutdown(runtime) && runtime->stopped && runtime->committed);
        assert(!fail_resume_unlink);
    }
    assert(saves_resume_probe(runtime->saves, token) == SAVES_RESUME_NONE);
    assert(saves_checkpoint_present(runtime->saves));
    check_battery_startup(argv, "12:22");
}

static void check_quit_and_sleep(struct native_runtime *runtime, const struct fixture *fixture,
                                 char **argv)
{
    uint8_t *sram = fixture->sram, *rtc = fixture->rtc;
    *sram = 0x33;
    *rtc = 0x43;
    assert(power_checkpoint(runtime));
    char token[65];
    assert(saves_resume_probe(runtime->saves, token) == SAVES_RESUME_READY);

    *sram = 0x34;
    *rtc = 0x44;
    saves_test_fail_event(SAVES_TEST_RESUME_RENAME, 1);
    assert(!control_quit(runtime) && !runtime->stopped && !runtime->committed);
    assert_resume_token(runtime->saves, token);
    assert(load_persistence(runtime->saves, -1, true));
    assert(*sram == 0x33 && *rtc == 0x43);

    *sram = 0x35;
    *rtc = 0x45;
    assert(setenv("BANANA_RETRO_TEST_REJECT_SERIALIZE", "1", 1) == 0);
    unsigned serialized = fixture->serialize_calls();
    saves_test_fail_event(SAVES_TEST_CURRENT_RENAME, 1);
    assert(!control_shutdown(runtime) && !runtime->stopped && !runtime->committed);
    assert_resume_token(runtime->saves, token);
    assert(!control_pause(runtime, BANANA_RETRO_CTL_OWNER_SLEEP, 1, true));
    assert(!runtime->sleep_paused && fixture->serialize_calls() == serialized + 2);
    assert(saves_resume_probe(runtime->saves, token) == SAVES_RESUME_NONE);
    assert(saves_checkpoint_present(runtime->saves));
    check_battery_startup(argv, "35:45");
}

static void check_option_mutation_during_restore(struct native_runtime *runtime,
                                                 const struct fixture *fixture)
{
    assert(control_save_state(runtime, 0));
    unsigned restored = fixture->unserialize_calls();
    assert(setenv("BANANA_RETRO_TEST_SET_OPTION_DURING_UNSERIALIZE", "1", 1) == 0);
    assert(!control_load_state(runtime, 0));
    assert(runtime->failed_load && runtime->stopped);
    /* Another ready peer can request persistence in the same dispatch cycle. */
    assert(!control_flush(runtime) && !power_checkpoint(runtime) && !control_shutdown(runtime));
    assert(fixture->unserialize_calls() == restored + 1);
    assert(fixture->effective_mode() == 1);
    assert(strcmp(core_option_get("fixture_mode"), "first") == 0);
}

static void check_transient_resume(struct native_runtime *runtime, const struct fixture *fixture)
{
    *fixture->sram = 0x37;
    *fixture->rtc = 0x47;
    assert(power_checkpoint(runtime));
    char token[65];
    assert(saves_resume_probe(runtime->saves, token) == SAVES_RESUME_READY);
    const unsigned restored = fixture->unserialize_calls();
    const enum saves_test_event faults[] = {SAVES_TEST_RESUME_ALLOC, SAVES_TEST_RESUME_READ};
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        /* The first occurrence is the probe, the second is the restore read. */
        for (unsigned occurrence = 1; occurrence <= 2; ++occurrence) {
            *fixture->sram = 0xa3;
            *fixture->rtc = 0xb3;
            saves_test_fail_event(faults[i], occurrence);
            assert(!load_persistence(runtime->saves, -1, true));
            assert(*fixture->sram == 0xa3 && *fixture->rtc == 0xb3);
            assert(fixture->unserialize_calls() == restored);
            assert_resume_token(runtime->saves, token);
        }
    }
    assert(load_persistence(runtime->saves, -1, true));
    assert(*fixture->sram == 0x37 && *fixture->rtc == 0x47);
    assert(fixture->unserialize_calls() == restored + 1);
}

static void check_legacy_slot(struct native_runtime *runtime, const struct fixture *fixture,
                              unsigned version)
{
    const uint8_t sram[4] = {0x38}, rtc[2] = {0x48};
    /* This payload was produced with a different effective option value. */
    const unsigned state = 0x01000001;
    assert(saves_commit(runtime->saves, sram, sizeof(sram), rtc, sizeof(rtc), 0,
                        &state, sizeof(state)));
    assert(saves_test_downgrade_current_manifest(runtime->saves, version));
    assert(saves_state_present(runtime->saves, 0));
    *fixture->sram = 0xa4;
    *fixture->rtc = 0xb4;
    unsigned restored = fixture->unserialize_calls();
    assert(!control_load_state(runtime, 0) && !runtime->stopped && !runtime->failed_load);
    assert(*fixture->sram == 0xa4 && *fixture->rtc == 0xb4);
    assert(fixture->unserialize_calls() == restored);
    assert(core_persistence_load(runtime->saves, -1) == CORE_LOAD_OK);
    assert(*fixture->sram == 0x38 && *fixture->rtc == 0x48);
    assert(control_flush(runtime) && saves_state_present(runtime->saves, 0));
    assert(!control_load_state(runtime, 0) && !runtime->stopped && !runtime->failed_load);
    assert(fixture->unserialize_calls() == restored);
}

static void check_transient_startup(struct saves *saves, char **argv)
{
    char token[65];
    assert(saves_resume_probe(saves, token) == SAVES_RESUME_READY);
    assert(setenv("BANANA_RETRO_NATIVE_DIR", argv[5], 1) == 0);
    assert(setenv("BANANA_RETRO_TEST_PERSISTENCE", "1", 1) == 0);
    assert(setenv("BANANA_RETRO_TEST_QUICK_RESUME", "1", 1) == 0);
    saves_test_fail_event(SAVES_TEST_RESUME_ALLOC, 2);
    char *args[] = {argv[0], argv[3], argv[4], "--frames", "1", NULL};
    assert(banana_retro_main(5, args) == EXIT_FAILURE);
    assert_resume_token(saves, token);
}

int main(int argc, char **argv)
{
    if (argc != 6 || strcmp(argv[1], "--check-persistence") != 0)
        return banana_retro_main(argc, argv);

    const char *mode = argv[2];
    bool late_option = strcmp(mode, "option-during-restore") == 0;
    bool options = strncmp(mode, "options-", 8) == 0;
    bool legacy = strncmp(mode, "legacy-", 7) == 0;
    if (options || legacy || late_option)
        assert(setenv("BANANA_RETRO_FIXTURE_MODE", "persistence-options", 1) == 0);
    assert(core_load(argv[3], argv[4], NULL));
    struct saves *saves = open_saves_profile(argv[5], argv[3], argv[4]);
    assert(saves);
    struct native_runtime runtime = { .saves = saves, .started = true, .save_state_slot = -1 };
    assert(core_step(1));

    void *library = dlopen(argv[3], RTLD_NOW | RTLD_LOCAL);
    assert(library);
    void *(*memory)(unsigned) = NULL;
    fixture_export(library, "retro_get_memory_data", &memory, sizeof(memory));
    struct fixture fixture = {
        .sram = memory(RETRO_MEMORY_SAVE_RAM), .rtc = memory(RETRO_MEMORY_RTC)
    };
    assert(fixture.sram && fixture.rtc);
    fixture_export(library, "fixture_serialize_calls", &fixture.serialize_calls,
                   sizeof(fixture.serialize_calls));
    fixture_export(library, "fixture_unserialize_calls", &fixture.unserialize_calls,
                   sizeof(fixture.unserialize_calls));
    fixture_export(library, "fixture_effective_mode", &fixture.effective_mode,
                   sizeof(fixture.effective_mode));
    const bool quit = strcmp(mode, "quit") == 0;
    const bool transient = strcmp(mode, "transient-resume") == 0;
    if (options) check_options(&runtime, &fixture, strcmp(mode, "options-core") == 0);
    else if (late_option) check_option_mutation_during_restore(&runtime, &fixture);
    else if (legacy) check_legacy_slot(&runtime, &fixture, (unsigned)(mode[7] - '0'));
    else if (quit) check_quit_and_sleep(&runtime, &fixture, argv);
    else if (strcmp(mode, "battery-crash") == 0 || strcmp(mode, "battery-unlink") == 0)
        check_battery_only_shutdown(&runtime, &fixture, argv, strcmp(mode, "battery-crash") == 0);
    else {
        assert(transient);
        check_transient_resume(&runtime, &fixture);
    }

    assert(core_release());
    if (options) {
        /* Reopen the store and load through the normal startup path. */
        saves_close(saves);
        assert(dlclose(library) == 0);
        assert(core_load(argv[3], argv[4], NULL));
        library = dlopen(argv[3], RTLD_NOW | RTLD_LOCAL);
        assert(library);
        saves = open_saves_profile(argv[5], argv[3], argv[4]);
        assert(saves && load_persistence(saves, 0, true));
        assert(core_release());
    }
    assert(dlclose(library) == 0);
    if (transient) check_transient_startup(saves, argv);
    saves_close(saves);
    return EXIT_SUCCESS;
}
