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
    assert(power_checkpoint(runtime, NULL));
    char token[65];
    assert(saves_resume_probe(runtime->saves, token) == SAVES_RESUME_READY);
    *sram = 0;
    *rtc = 0;
    assert(core_persistence_restore_checkpoint(runtime->saves, token) == SAVES_RESUME_READY);
    assert(*sram == 0x31 && *rtc == 0x41);

    *sram = 0x32;
    *rtc = 0x42;
    assert(control_flush(runtime));
    assert(core_persistence_restore_checkpoint(runtime->saves, token) != SAVES_RESUME_READY);
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
    /* A periodic checkpoint the core cannot serialize still preserves battery memory
     * through the battery-only path, and publishes no proof of its own. */
    char proof[65] = "stale";
    assert(power_checkpoint(runtime, proof) && !proof[0] && fixture->serialize_calls() == serialized);
    *sram = 0;
    *rtc = 0;
    assert(core_persistence_load(runtime->saves, -1) == CORE_LOAD_OK);
    assert(*sram == 0xa1 && *rtc == 0xb1);
    assert(!saves_state_present(runtime->saves, 1) && !saves_state_present(runtime->saves, 2));
    char ineligible[65];
    assert(saves_resume_probe(runtime->saves, ineligible) == SAVES_RESUME_NONE);

    runtime->started = false;
    assert(!control_save_state(runtime, 0));
    assert(!power_checkpoint(runtime, NULL));
    assert(!control_flush(runtime));
    assert(control_quit(runtime) && runtime->stopped && !runtime->committed);
    assert(saves_resume_probe(runtime->saves, ineligible) == SAVES_RESUME_NONE);

    runtime->started = true;
    runtime->stopped = false;
    assert(control_quit(runtime) && runtime->stopped && runtime->committed);
    /* A fresh mutation after the terminal commit is refused and leaves the committed state alone. */
    bool refused_restart = false;
    char kept_option[16];
    assert(core_option_get("fixture_mode") && strlen(core_option_get("fixture_mode")) < sizeof(kept_option));
    strcpy(kept_option, core_option_get("fixture_mode"));
    assert(!control_set_option(runtime, "fixture_mode", strcmp(kept_option, "third") ? "third" : "second",
                               BANANA_RETRO_CTL_SCOPE_SESSION, &refused_restart));
    assert(strcmp(core_option_get("fixture_mode"), kept_option) == 0);
    assert(!control_reset(runtime));
    assert(!control_pause(runtime, BANANA_RETRO_CTL_OWNER_UI, 0, true) && !runtime->ui_paused);
    assert(!control_prepare_sleep(runtime, 900, token) && !runtime->sleep_paused);
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

static void check_boot_rejected(char **argv, const char *token)
{
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(setenv("BANANA_RETRO_NATIVE_DIR", argv[5], 1) == 0);
        assert(setenv("BANANA_RETRO_TEST_PERSISTENCE", "1", 1) == 0);
        execl(argv[0], argv[0], argv[3], argv[4], "--frames", "1",
              "--resume-checkpoint", token, (char *)NULL);
        _exit(127);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == EXIT_FAILURE);
}

static void check_battery_only_shutdown(struct native_runtime *runtime, const struct fixture *fixture,
                                        char **argv, bool interrupt)
{
    *fixture->sram = 0x11;
    *fixture->rtc = 0x21;
    char token[65] = {0}, next[65] = {0};
    assert(power_checkpoint(runtime, token));
    assert_resume_token(runtime->saves, token);

    *fixture->sram = 0x12;
    *fixture->rtc = 0x22;
    assert(setenv("BANANA_RETRO_TEST_REJECT_SERIALIZE", "1", 1) == 0);
    /* Battery, manifest, then the resume cutoff. A failed cutoff fsync must
     * leave the old checkpoint eligible and fail the shutdown. */
    saves_test_fail_event(SAVES_TEST_FILE_FSYNC, 3);
    assert(!control_shutdown(runtime, next) && !next[0] && !runtime->stopped && !runtime->committed);
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
                (void)control_shutdown(runtime, next);
                _exit(2);
            }
            int status;
            assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                   WEXITSTATUS(status) == 86);
            if (i == 0) assert_resume_token(runtime->saves, token);
        }
    } else {
        fail_resume_unlink = true;
        assert(control_shutdown(runtime, next) && !next[0] && runtime->stopped && runtime->committed);
        assert(!fail_resume_unlink);
    }
    assert(saves_resume_probe(runtime->saves, next) == SAVES_RESUME_NONE);
    check_boot_rejected(argv, token);
    assert(saves_checkpoint_present(runtime->saves));
    check_battery_startup(argv, "12:22");
}

static void check_quit_and_sleep(struct native_runtime *runtime, const struct fixture *fixture,
                                 char **argv)
{
    uint8_t *sram = fixture->sram, *rtc = fixture->rtc;
    *sram = 0x33;
    *rtc = 0x43;
    char token[65] = {0}, next[65] = {0};
    assert(power_checkpoint(runtime, token));
    assert_resume_token(runtime->saves, token);

    *sram = 0x34;
    *rtc = 0x44;
    saves_test_fail_event(SAVES_TEST_RESUME_RENAME, 1);
    assert(!control_quit(runtime) && !runtime->stopped && !runtime->committed);
    assert(saves_resume_probe(runtime->saves, next) == SAVES_RESUME_NONE);
    check_boot_rejected(argv, token);
    assert(load_persistence(runtime->saves, -1, NULL, &(bool){false}));
    assert(*sram == 0x34 && *rtc == 0x44);

    *sram = 0x35;
    *rtc = 0x45;
    assert(setenv("BANANA_RETRO_TEST_REJECT_SERIALIZE", "1", 1) == 0);
    unsigned serialized = fixture->serialize_calls();
    saves_test_fail_event(SAVES_TEST_CURRENT_RENAME, 1);
    assert(!control_shutdown(runtime, next) && !next[0] && !runtime->stopped && !runtime->committed);
    assert(saves_resume_probe(runtime->saves, next) == SAVES_RESUME_NONE);
    assert(!control_pause(runtime, BANANA_RETRO_CTL_OWNER_SLEEP, 1, true));
    assert(!runtime->sleep_paused && fixture->serialize_calls() == serialized + 2);
    assert(saves_resume_probe(runtime->saves, token) == SAVES_RESUME_NONE);
    assert(saves_checkpoint_present(runtime->saves));
    check_battery_startup(argv, "35:45");

    /* A checkpoint commit that fails while acquiring the hold leaves no hold and no proof. */
    assert(unsetenv("BANANA_RETRO_TEST_REJECT_SERIALIZE") == 0);
    char held[65] = "stale", final[65] = {0};
    saves_test_fail_event(SAVES_TEST_CURRENT_RENAME, 1);
    assert(!pause_owner(runtime, BANANA_RETRO_CTL_OWNER_SLEEP, 1, true, held));
    assert(!held[0] && !runtime->sleep_paused);
    /* A held sleep fences every other persistent request, and the digest it acknowledged stays CURRENT... */
    assert(pause_owner(runtime, BANANA_RETRO_CTL_OWNER_SLEEP, 1, true, held) && strlen(held) == 64);
    assert_resume_token(runtime->saves, held);
    *sram = 0x36;
    unsigned held_serialized = fixture->serialize_calls();
    assert(!control_flush(runtime) && !control_save_state(runtime, 0) && !control_quit(runtime) &&
           !power_checkpoint(runtime, NULL));
    assert(fixture->serialize_calls() == held_serialized);
    assert_resume_token(runtime->saves, held);
    /* ... while the owner's own shutdown still completes under the hold, and replays without a new commit. */
    assert(control_shutdown(runtime, final) && strlen(final) == 64 && runtime->stopped && runtime->committed);
    assert_resume_token(runtime->saves, final);
    assert(control_quit(runtime) && fixture->serialize_calls() == held_serialized + 1);
    assert_resume_token(runtime->saves, final);
}

/* The exit-time persist in main() must advance CURRENT for an ordinary exit, but never past a checkpoint that a
 * sleep hold or an authenticated terminal commit already acknowledged. */
static void check_exit_persistence(struct native_runtime *runtime, const struct fixture *fixture)
{
    uint8_t *sram = fixture->sram, *rtc = fixture->rtc;
    char ordinary[65] = {0}, held[65] = {0}, after_release[65] = {0}, shutdown[65] = {0}, replay[65] = {0};

    /* Ordinary exit commits a new checkpoint. */
    *sram = 0x51;
    *rtc = 0x61;
    unsigned serialized = fixture->serialize_calls();
    assert(persist_on_exit(runtime, false, true));
    assert(fixture->serialize_calls() == serialized + 1);
    assert(saves_resume_probe(runtime->saves, ordinary) == SAVES_RESUME_READY);
    *sram = 0;
    assert(load_persistence(runtime->saves, -1, NULL, &(bool){false}) && *sram == 0x51);

    /* A plain exit under a sleep hold leaves the acknowledged checkpoint CURRENT, memory and proof included. */
    *sram = 0x53;
    *rtc = 0x63;
    assert(pause_owner(runtime, BANANA_RETRO_CTL_OWNER_SLEEP, 1, true, held) && strlen(held) == 64);
    assert(strcmp(held, ordinary) != 0);
    assert_resume_token(runtime->saves, held);
    *sram = 0x54;
    *rtc = 0x64;
    serialized = fixture->serialize_calls();
    assert(persist_on_exit(runtime, false, true));
    assert(fixture->serialize_calls() == serialized);
    assert_resume_token(runtime->saves, held);
    *sram = 0;
    *rtc = 0;
    assert(load_persistence(runtime->saves, -1, NULL, &(bool){false}) && *sram == 0x53 && *rtc == 0x63);

    /* Once the hold is released the exit persists normally again. */
    assert(pause_owner(runtime, BANANA_RETRO_CTL_OWNER_SLEEP, 1, false, NULL) && !runtime->sleep_paused);
    *sram = 0x55;
    *rtc = 0x65;
    assert(persist_on_exit(runtime, false, true));
    assert(fixture->serialize_calls() == serialized + 1);
    assert(saves_resume_probe(runtime->saves, after_release) == SAVES_RESUME_READY);
    assert(strcmp(after_release, held) != 0);

    /* An authenticated SHUTDOWN under a hold commits once; exit and a repeated SHUTDOWN replay it. */
    assert(pause_owner(runtime, BANANA_RETRO_CTL_OWNER_SLEEP, 2, true, held) && strlen(held) == 64);
    *sram = 0x56;
    *rtc = 0x66;
    assert(control_shutdown(runtime, shutdown) && strlen(shutdown) == 64 && runtime->stopped &&
           runtime->committed);
    assert_resume_token(runtime->saves, shutdown);
    *sram = 0x57;
    serialized = fixture->serialize_calls();
    assert(persist_on_exit(runtime, false, true));
    assert(control_shutdown(runtime, replay) && strcmp(replay, shutdown) == 0);
    assert(fixture->serialize_calls() == serialized);
    assert_resume_token(runtime->saves, shutdown);
    *sram = 0;
    assert(load_persistence(runtime->saves, -1, NULL, &(bool){false}) && *sram == 0x56);
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
    assert(!control_flush(runtime) && !power_checkpoint(runtime, NULL) &&
           !control_shutdown(runtime, NULL));
    assert(fixture->unserialize_calls() == restored + 1);
    assert(fixture->effective_mode() == 1);
    assert(strcmp(core_option_get("fixture_mode"), "first") == 0);
}

static void check_transient_resume(struct native_runtime *runtime, const struct fixture *fixture)
{
    *fixture->sram = 0x37;
    *fixture->rtc = 0x47;
    char token[65] = {0};
    assert(power_checkpoint(runtime, token));
    assert_resume_token(runtime->saves, token);
    const unsigned restored = fixture->unserialize_calls();
    const enum saves_test_event faults[] = {SAVES_TEST_RESUME_ALLOC, SAVES_TEST_RESUME_READ};
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        *fixture->sram = 0xa3;
        *fixture->rtc = 0xb3;
        saves_test_fail_event(faults[i], 1);
        bool rejected = true;
        assert(!load_persistence(runtime->saves, -1, token, &rejected));
        /* A transient read or allocation failure is never a permanent rejection. */
        assert(!rejected);
        assert(*fixture->sram == 0xa3 && *fixture->rtc == 0xb3);
        assert(fixture->unserialize_calls() == restored);
        assert_resume_token(runtime->saves, token);
    }
    assert(load_persistence(runtime->saves, -1, token, &(bool){false}));
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
    saves_test_fail_event(SAVES_TEST_RESUME_ALLOC, 1);
    char *args[] = {argv[0], argv[3], argv[4], "--frames", "1", "--resume-checkpoint", token, NULL};
    assert(banana_retro_main(7, args) == EXIT_FAILURE);
    assert_resume_token(saves, token);
}

static void write_content(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    assert(file && fwrite(data, 1, size, file) == size && fclose(file) == 0);
}

/* Store identity must follow the bytes the core consumed, not whatever the path holds
 * once the core has loaded. The memory and need_fullpath loaders are both covered. */
static void check_content_identity(const char *mode, char **argv)
{
    static const char original[] = "BRC1\n", replacement[] = "BRC2\n";
    const size_t size = sizeof(original) - 1;
    if (strcmp(mode, "content-memory") != 0)
        assert(setenv("BANANA_RETRO_FIXTURE_MODE", "path", 1) == 0);
    if (strcmp(mode, "content-mutated") == 0) {
        /* The core read the original, then another writer changed the file under it. */
        assert(setenv("BANANA_RETRO_TEST_MUTATE_CONTENT_AFTER_READ", "1", 1) == 0);
        assert(!core_load(argv[3], argv[4], NULL));
        assert(core_release());
        write_content(argv[4], original, size);
        return;
    }
    assert(core_load(argv[3], argv[4], NULL));
    unsigned char consumed[SAVES_CONTENT_DIGEST_SIZE], expected[SAVES_CONTENT_DIGEST_SIZE];
    assert(core_content_digest(consumed));
    saves_digest_content(original, size, expected);
    assert(memcmp(consumed, expected, sizeof(expected)) == 0);

    write_content(argv[4], replacement, size);
    struct saves *bound = open_saves_profile(argv[5], argv[3], argv[4]);
    assert(bound && saves_commit(bound, "a", 1, "b", 1, -1, NULL, 0));
    char fingerprint[CORE_OPTIONS_FINGERPRINT_SIZE];
    assert(core_options_fingerprint(fingerprint));
    write_content(argv[4], original, size);
    struct saves *restored = saves_open_with_options(argv[5], argv[3], argv[4], fingerprint);
    uint8_t sram = 0, rtc = 0;
    assert(restored && saves_load_memory(restored, &sram, 1, &rtc, 1) && sram == 'a' && rtc == 'b');
    saves_close(restored);
    saves_close(bound);
    assert(core_release());
}

int main(int argc, char **argv)
{
    if (argc != 6 || strcmp(argv[1], "--check-persistence") != 0)
        return banana_retro_main(argc, argv);

    const char *mode = argv[2];
    if (strncmp(mode, "content-", 8) == 0) {
        check_content_identity(mode, argv);
        return EXIT_SUCCESS;
    }
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
    else if (strcmp(mode, "exit-persist") == 0) check_exit_persistence(&runtime, &fixture);
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
        assert(saves && load_persistence(saves, 0, NULL, &(bool){false}));
        assert(core_release());
    }
    assert(dlclose(library) == 0);
    if (transient) check_transient_startup(saves, argv);
    saves_close(saves);
    return EXIT_SUCCESS;
}
