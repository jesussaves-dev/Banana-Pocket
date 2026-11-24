#define _GNU_SOURCE
#include "video_wayland.h"
#include "input_evdev.h"
#include "audio_alsa.h"
#include "saves.h"
#include "control.h"
#include "options.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

const char *core_name(void);
const char *core_version(void);
uint32_t core_capabilities(void);
bool core_reset(void);
const char *core_option_get(const char *key);
bool core_option_set(const char *key, const char *value);
size_t core_option_count(void);
const struct core_option *core_option_at(size_t index);
bool core_persistence_flush(struct saves *saves);

enum { MAX_FRAMES = 10000 };

static struct wl_display *native_display;

struct wl_display *__real_wl_display_connect(const char *name);
struct wl_display *__wrap_wl_display_connect(const char *name)
{
    native_display = __real_wl_display_connect(name);
    return native_display;
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void input_snapshot(void *data, uint16_t masks[RETRO_INPUT_MAX_DEVICES])
{
    struct input_evdev *input = data;
    if (!input_evdev_snapshot(input, masks))
        memset(masks, 0, sizeof(uint16_t) * RETRO_INPUT_MAX_DEVICES);
}

/* Service input and Wayland without letting frame callbacks pace retro_run(). */
struct native_runtime {
    struct input_evdev *input;
    struct audio_alsa *audio;
    struct saves *saves;
    struct control_server *control;
    struct retro_paced_runner *runner;
    int signal_fd;
    bool stopped;
    bool failed_load;
    bool committed;
    bool ui_paused;
    bool sleep_paused;
    uint64_t sleep_epoch;
    int save_state_slot;
    const char *content_path;
};

static uint32_t pause_owners(const struct native_runtime *runtime)
{
    return (runtime->ui_paused ? 1u : 0u) | (runtime->sleep_paused ? 2u : 0u);
}

static void control_status(void *data, struct control_status *status)
{
    const struct native_runtime *runtime = data;
    status->flags = 1u | (pause_owners(runtime) ? 2u : 0u);
    status->pause_owners = pause_owners(runtime);
    status->capabilities = core_capabilities();
    status->core_id = core_name();
    status->core_version = core_version();
    status->content_id = runtime->content_path;
    struct audio_alsa_status audio = {0};
    audio_alsa_get_status(runtime->audio, &audio);
    status->audio_state = (uint32_t)audio.state;
    status->audio_queued_frames = audio.queued_frames;
    status->audio_underruns = audio.underrun_frames;
}

static bool control_pause(void *data, unsigned owner, uint64_t epoch, bool acquire)
{
    struct native_runtime *runtime = data;
    bool was_paused = pause_owners(runtime) != 0;
    if (owner == BANANA_RETRO_CTL_OWNER_UI) {
        runtime->ui_paused = acquire;
    } else if (owner == BANANA_RETRO_CTL_OWNER_SLEEP) {
        if (acquire) {
            if (!epoch || (runtime->sleep_paused && epoch != runtime->sleep_epoch) ||
                (!runtime->sleep_paused && epoch <= runtime->sleep_epoch)) return false;
            runtime->sleep_epoch = epoch;
            runtime->sleep_paused = true;
        } else {
            if (!runtime->sleep_paused || epoch != runtime->sleep_epoch) return false;
            runtime->sleep_paused = false;
        }
    } else return false;
    bool paused = pause_owners(runtime) != 0;
    if (runtime->audio) audio_alsa_set_paused(runtime->audio, paused);
    if (was_paused && !paused && runtime->runner) {
        return retro_paced_start(runtime->runner, core_refresh_rate(), monotonic_ns());
    }
    return true;
}

static bool control_flush(void *data)
{
    struct native_runtime *runtime = data;
    return runtime->saves && core_persistence_flush(runtime->saves);
}

static bool control_save_state(void *data, unsigned slot)
{
    struct native_runtime *runtime = data;
    return runtime->saves && core_persistence_commit(runtime->saves, (int)slot);
}

static bool control_load_state(void *data, unsigned slot)
{
    struct native_runtime *runtime = data;
    bool ok = runtime->saves && core_persistence_load(runtime->saves, (int)slot);
    if (!ok) {
        runtime->failed_load = true;
        runtime->stopped = true;
    }
    return ok;
}

static size_t control_option_count(void *data)
{
    (void)data;
    return core_option_count();
}

static bool control_option(void *data, size_t index, const char **key,
                           const char **description, const char **value,
                           const char **default_value, const char *const **choices,
                           size_t *choice_count, bool *visible, bool *restart_required)
{
    (void)data;
    const struct core_option *option = core_option_at(index);
    if (!option) return false;
    static const char *choice_ptrs[OPTIONS_MAX_VALUES];
    for (size_t i = 0; i < option->choice_count; ++i) choice_ptrs[i] = option->choices[i];
    *key = option->key;
    *description = option->description;
    *value = option->value;
    *default_value = option->default_value;
    *choices = choice_ptrs;
    *choice_count = option->choice_count;
    *visible = option->visible;
    *restart_required = option->restart_required;
    return true;
}

static bool control_set_option(void *data, const char *key, const char *value,
                               unsigned scope, bool *restart_required)
{
    (void)data;
    if (scope != BANANA_RETRO_CTL_SCOPE_SESSION) return false;
    const char *previous = core_option_get(key);
    bool changed = previous && strcmp(previous, value) != 0;
    if (!core_option_set(key, value)) return false;
    *restart_required = changed;
    return true;
}

static bool control_reset(void *data)
{
    struct native_runtime *runtime = data;
    if (!core_reset()) return false;
    if (runtime->audio) audio_alsa_rebase(runtime->audio);
    if (runtime->input && !input_evdev_poll(runtime->input)) return false;
    if (runtime->runner)
        return retro_paced_start(runtime->runner, core_refresh_rate(), monotonic_ns());
    return true;
}

static bool control_quit(void *data)
{
    struct native_runtime *runtime = data;
    if (!runtime->saves || !core_persistence_commit(runtime->saves, runtime->save_state_slot)) return false;
    runtime->committed = true;
    runtime->stopped = true;
    return true;
}

static bool parse_session_id(const char *text, unsigned char session_id[16])
{
    if (!text || strlen(text) != 32) return false;
    for (unsigned i = 0; i < 16; ++i) {
        unsigned char a = (unsigned char)text[2 * i], b = (unsigned char)text[2 * i + 1];
        unsigned hi = a >= '0' && a <= '9' ? a - '0' :
                      a >= 'a' && a <= 'f' ? a - 'a' + 10 :
                      a >= 'A' && a <= 'F' ? a - 'A' + 10 : 16;
        unsigned lo = b >= '0' && b <= '9' ? b - '0' :
                      b >= 'a' && b <= 'f' ? b - 'a' + 10 :
                      b >= 'A' && b <= 'F' ? b - 'A' + 10 : 16;
        if (hi > 15 || lo > 15) return false;
        session_id[i] = (unsigned char)((hi << 4) | lo);
    }
    return true;
}

static int wait_until(void *data, uint64_t deadline_ns)
{
    struct native_runtime *runtime = data;
    int display_fd = wl_display_get_fd(native_display);
    int input_fd = input_evdev_fd(runtime->input);
    for (;;) {
        if (runtime->stopped) return 0;
        if (wl_display_dispatch_pending(native_display) < 0)
            return -1;
        uint64_t now = monotonic_ns();
        if (!now) return -1;
        bool paused = pause_owners(runtime) != 0;
        uint64_t deadline = runtime->runner ? runtime->runner->clock.deadline_ns : deadline_ns;
        if (!paused && now >= deadline) return 1;
        if (wl_display_prepare_read(native_display) != 0)
            continue;
        struct pollfd fds[6] = {
            { .fd = display_fd, .events = POLLIN },
            { .fd = input_fd, .events = POLLIN },
            { .fd = runtime->signal_fd, .events = POLLIN },
            { .fd = control_listener_fd(runtime->control), .events = POLLIN },
            { .fd = control_client_fd(runtime->control, 0), .events = POLLIN },
            { .fd = control_client_fd(runtime->control, 1), .events = POLLIN }
        };
        if (wl_display_flush(native_display) < 0) {
            if (errno != EAGAIN) { wl_display_cancel_read(native_display); return -1; }
            fds[0].events |= POLLOUT;
        }
        uint64_t delta = paused ? 0 : deadline - now;
        struct timespec timeout = { .tv_sec = (time_t)(delta / 1000000000ull),
            .tv_nsec = (long)(delta % 1000000000ull) };
        int rc = ppoll(fds, 6, paused ? NULL : &timeout, NULL);
        if (rc < 0 && errno == EINTR) {
            wl_display_cancel_read(native_display);
            continue;
        }
        if (rc < 0) { wl_display_cancel_read(native_display); return -1; }
        if (fds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
            if (wl_display_read_events(native_display) < 0) return -1;
        } else {
            wl_display_cancel_read(native_display);
        }
        if (fds[0].revents & POLLOUT && wl_display_flush(native_display) < 0 && errno != EAGAIN)
            return -1;
        if (fds[0].revents & POLLNVAL || fds[1].revents & (POLLERR | POLLHUP | POLLNVAL) ||
            fds[2].revents & (POLLERR | POLLHUP | POLLNVAL))
            return -1;
        if (fds[1].revents & POLLIN && !input_evdev_poll(runtime->input))
            return -1;
        if (runtime->control &&
            (fds[3].revents & (POLLIN | POLLERR | POLLHUP) ||
             fds[4].revents & (POLLIN | POLLERR | POLLHUP) ||
             fds[5].revents & (POLLIN | POLLERR | POLLHUP))) {
            unsigned clients = 0;
            if (fds[4].revents & (POLLIN | POLLERR | POLLHUP)) clients |= 1u;
            if (fds[5].revents & (POLLIN | POLLERR | POLLHUP)) clients |= 2u;
            control_handle_ready(runtime->control, (fds[3].revents & POLLIN) != 0, clients);
        }
        if (fds[2].revents & POLLIN) {
            struct signalfd_siginfo info;
            ssize_t bytes = read(runtime->signal_fd, &info, sizeof(info));
            if (bytes < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            if (bytes != (ssize_t)sizeof(info)) return -1;
            runtime->stopped = true;
            return 0;
        }
    }
}

static bool run_frame(void *data, unsigned frame)
{
    (void)data;
    return core_step(frame);
}

static uint64_t paced_now(void *data)
{
    (void)data;
    return monotonic_ns();
}

static double refresh_rate(void *data)
{
    (void)data;
    return core_refresh_rate();
}

int main(int argc, char **argv)
{
    unsigned long frames = MAX_FRAMES;
    char *end = NULL;
    bool forever = argc >= 4 && strcmp(argv[3], "--wayland") == 0;
    bool native = forever;
    int load_state_slot = -1, save_state_slot = -1;
    bool skip_save_commit = false;
    bool stopped = false, ran = false, ok;
    struct wayland_video *video = NULL;
    struct input_evdev *input = NULL;
    struct retro_paced_runner runner;
    struct native_runtime runtime = {0};
    struct core_video sink = { .submit = wayland_video_submit };
    int signal_fd = -1;
    unsigned frame = 1;
#ifdef SAVES_TESTING
    const char *test_persist_env = getenv("BANANA_RETRO_TEST_PERSISTENCE");
    bool test_persistence = test_persist_env && strcmp(test_persist_env, "1") == 0;
#else
    bool test_persistence = false;
#endif

    if (!forever && (argc < 5 || strcmp(argv[3], "--frames") != 0)) {
        fprintf(stderr, "Usage: %s CORE CONTENT --frames COUNT [--wayland] [--load-state N] [--save-state N]\n", argv[0]);
        fprintf(stderr, "       %s CORE CONTENT --wayland [--load-state N] [--save-state N]\n", argv[0]);
        return 2;
    }
    if (!forever) {
        errno = 0;
        frames = strtoul(argv[4], &end, 10);
        if (argv[4][0] < '0' || argv[4][0] > '9' || *end != '\0' || errno != 0 ||
            frames == 0 || frames > MAX_FRAMES) {
            fprintf(stderr, "Frame count must be between 1 and %u\n", (unsigned)MAX_FRAMES);
            return 2;
        }
    }
    for (int i = forever ? 4 : 5; i < argc; ++i) {
        if (strcmp(argv[i], "--wayland") == 0) {
            native = true;
        } else if ((strcmp(argv[i], "--state-slot") == 0 ||
                    strcmp(argv[i], "--load-state") == 0 ||
                    strcmp(argv[i], "--save-state") == 0) && i + 1 < argc) {
            bool both = strcmp(argv[i], "--state-slot") == 0;
            bool load = both || strcmp(argv[i], "--load-state") == 0;
            bool save = both || strcmp(argv[i], "--save-state") == 0;
            char *slot_end = NULL;
            errno = 0;
            unsigned long slot = strtoul(argv[++i], &slot_end, 10);
            if (errno || slot_end == argv[i] || *slot_end != '\0' || slot >= SAVES_STATE_SLOTS) {
                fprintf(stderr, "State slot must be between 0 and %u\n", SAVES_STATE_SLOTS - 1);
                return 2;
            }
            if (load) load_state_slot = (int)slot;
            if (save) save_state_slot = (int)slot;
        } else {
            fprintf(stderr, "Unknown runtime option: %s\n", argv[i]);
            return 2;
        }
    }
    if ((load_state_slot >= 0 || save_state_slot >= 0) && !native && !test_persistence) {
        fputs("State slots require --wayland\n", stderr);
        return 2;
    }
    if (native) {
        video = wayland_video_open();
        if (!video || !native_display) return EXIT_FAILURE;
        sink.data = video;
        const char *directory = getenv("BANANA_RETRO_INPUT_DIR");
        input = input_evdev_open(directory ? directory : "/dev/input");
        if (!input) { wayland_video_close(video); return EXIT_FAILURE; }
        runtime.input = input;
        core_set_input_source(input, input_snapshot);
    }
    if (native || test_persistence) {
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        if (sigprocmask(SIG_BLOCK, &signals, NULL) < 0) {
            input_evdev_close(input); wayland_video_close(video); return EXIT_FAILURE;
        }
        signal_fd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
        if (signal_fd < 0) {
            input_evdev_close(input); wayland_video_close(video); return EXIT_FAILURE;
        }
        runtime.signal_fd = signal_fd;
    }
    runtime.runner = &runner;
    runtime.content_path = argv[2];
    runtime.save_state_slot = save_state_slot;
    ok = core_load(argv[1], argv[2], native ? &sink : NULL);
    if (ok && (native || test_persistence)) {
        const char *root = getenv("BANANA_RETRO_NATIVE_DIR");
        if (!root || !*root) root = "/data/banana/retro/saves";
        runtime.saves = saves_open(root, argv[1], argv[2]);
        if (!runtime.saves) {
            fputs("Native save storage unavailable\n", stderr);
            skip_save_commit = load_state_slot >= 0;
            ok = false;
        } else if (!core_persistence_load(runtime.saves, load_state_slot)) {
            fputs("No compatible native save or requested state slot\n", stderr);
            if (load_state_slot >= 0) {
                skip_save_commit = true;
                ok = false;
            }
        }
    }
    if (ok && video) ok = wayland_video_configure(video, core_geometry());
    if (ok && video) {
        double sample_rate = core_sample_rate();
        if (sample_rate < 8000.0 || sample_rate > 192000.0) {
            fprintf(stderr, "Unsupported core audio sample rate %.3f Hz\n", sample_rate);
            ok = false;
        } else {
            runtime.audio = audio_alsa_open((unsigned)(sample_rate + 0.5));
        }
        if (!runtime.audio) ok = false;
        if (ok && runtime.audio) {
            core_set_audio_output(runtime.audio);
            if (!audio_alsa_wait_ready(runtime.audio, 3000)) {
                struct audio_alsa_status status;
                audio_alsa_get_status(runtime.audio, &status);
                fprintf(stderr, "Audio unavailable: state=%d retries=%llu\n",
                        (int)status.state, (unsigned long long)status.reopen_attempts);
            } else {
                struct audio_alsa_status status;
                audio_alsa_get_status(runtime.audio, &status);
                fprintf(stderr, "Audio ALSA default: rate=%u period=%u buffer=%u frames\n",
                        status.output_rate, status.period_frames, status.buffer_frames);
            }
        }
    }
    if (ok && native && getenv("BANANA_RETRO_CONTROL_SOCKET")) {
        unsigned char session_id[16];
        struct control_ops control_ops = {
            .data = &runtime,
            .status = control_status,
            .pause = control_pause,
            .flush = control_flush,
            .save_state = control_save_state,
            .load_state = control_load_state,
            .option_count = control_option_count,
            .option = control_option,
            .set_option = control_set_option,
            .reset = control_reset,
            .quit = control_quit
        };
        const char *session_text = getenv("BANANA_RETRO_SESSION_ID");
        if (parse_session_id(session_text, session_id))
            runtime.control = control_open_from_env(&control_ops, session_id);
        else errno = EINVAL;
        if (!runtime.control)
            fprintf(stderr, "Native control unavailable: %s\n", strerror(errno));
    }
    if (ok && video) ok = retro_paced_start(&runner, core_refresh_rate(), monotonic_ns());
    struct retro_paced_ops paced_ops = {
        .data = &runtime,
        .now_ns = paced_now,
        .wait_until = wait_until,
        .run_frame = run_frame,
        .refresh_rate = refresh_rate
    };
    while (ok && (forever || frame <= frames) && !stopped) {
        if (video) {
            enum retro_paced_result result = retro_paced_run_one(&runner, &paced_ops, frame);
            if (result == RETRO_PACED_STOPPED) {
                stopped = runtime.stopped;
                break;
            }
            if (result == RETRO_PACED_FAILED) ok = false;
            else { ran = true; ++frame; }
        } else {
            ok = core_step(frame);
            if (ok) ran = true;
            ++frame;
#ifdef SAVES_TESTING
            if (ok && test_persistence) {
                fflush(stdout);
                const char *wait_env = getenv("BANANA_RETRO_TEST_WAIT_FOR_SIGNAL");
                int timeout = wait_env ? 5000 : 0;
                struct pollfd pfd = { .fd = signal_fd, .events = POLLIN };
                int rc;
                do { rc = poll(&pfd, 1, timeout); } while (rc < 0 && errno == EINTR);
                if (rc > 0 && (pfd.revents & POLLIN)) {
                    struct signalfd_siginfo info;
                    if (read(signal_fd, &info, sizeof(info)) != (ssize_t)sizeof(info)) ok = false;
                    else { stopped = true; runtime.stopped = true; }
                } else if (rc < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) ok = false;
            }
#endif
        }
    }
    if (runtime.failed_load) ok = false;
    if (ok && video && !stopped) ok = wayland_video_wait(video);
    if (ok && video && ran && !wayland_video_presented(video)) {
        fputs("No native Wayland frame reached the display\n", stderr);
        ok = false;
    }
    skip_save_commit = skip_save_commit || runtime.failed_load || runtime.committed;
    if (runtime.control) control_close(runtime.control);
    if (runtime.saves && !skip_save_commit &&
        !core_persistence_commit(runtime.saves, save_state_slot)) {
        fputs("Native save generation commit failed\n", stderr);
        ok = false;
    }
    if (!core_release()) ok = false;
    saves_close(runtime.saves);
    core_set_audio_output(NULL);
    if (runtime.audio) {
        struct audio_alsa_status status;
        audio_alsa_get_status(runtime.audio, &status);
        fprintf(stderr, "Audio status: accepted=%llu overflow=%llu underrun=%llu xrun=%llu "
                "reopens=%llu queue=%u delay=%lld\n",
                (unsigned long long)status.accepted_frames,
                (unsigned long long)status.overflow_frames,
                (unsigned long long)status.underrun_frames,
                (unsigned long long)status.xrun_recoveries,
                (unsigned long long)status.reopen_successes, status.queued_frames,
                (long long)status.delay_frames);
        audio_alsa_close(runtime.audio);
    }
    wayland_video_close(video);
    input_evdev_close(input);
    if (signal_fd >= 0) close(signal_fd);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
