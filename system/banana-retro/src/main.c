#define _GNU_SOURCE
#include "video_wayland.h"
#include "input_evdev.h"
#include "audio_alsa.h"
#include "saves.h"

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
    int signal_fd;
    bool stopped;
};

static int wait_until(void *data, uint64_t deadline_ns)
{
    struct native_runtime *runtime = data;
    int display_fd = wl_display_get_fd(native_display);
    int input_fd = input_evdev_fd(runtime->input);
    for (;;) {
        if (wl_display_dispatch_pending(native_display) < 0)
            return -1;
        uint64_t now = monotonic_ns();
        if (!now) return -1;
        if (now >= deadline_ns) return 1;
        if (wl_display_prepare_read(native_display) != 0)
            continue;
        struct pollfd fds[3] = {
            { .fd = display_fd, .events = POLLIN },
            { .fd = input_fd, .events = POLLIN },
            { .fd = runtime->signal_fd, .events = POLLIN }
        };
        if (wl_display_flush(native_display) < 0) {
            if (errno != EAGAIN) { wl_display_cancel_read(native_display); return -1; }
            fds[0].events |= POLLOUT;
        }
        uint64_t delta = deadline_ns - now;
        struct timespec timeout = { .tv_sec = (time_t)(delta / 1000000000ull),
            .tv_nsec = (long)(delta % 1000000000ull) };
        int rc = ppoll(fds, 3, &timeout, NULL);
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
    if ((load_state_slot >= 0 || save_state_slot >= 0) && !native) {
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
    ok = core_load(argv[1], argv[2], native ? &sink : NULL);
    if (ok && native) {
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
    if (ok && video) ok = retro_paced_start(&runner, core_refresh_rate(), monotonic_ns());
    struct retro_paced_ops paced_ops = {
        .data = &runtime,
        .now_ns = paced_now,
        .wait_until = wait_until,
        .run_frame = run_frame,
        .refresh_rate = refresh_rate
    };
    unsigned frame = 1;
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
        }
    }
    if (ok && video && !stopped) ok = wayland_video_wait(video);
    if (ok && video && ran && !wayland_video_presented(video)) {
        fputs("No native Wayland frame reached the display\n", stderr);
        ok = false;
    }
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
