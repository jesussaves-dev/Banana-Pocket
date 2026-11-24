#define _GNU_SOURCE
#include "audio_alsa.h"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

enum { REOPEN_DELAY_MS = 1000, IDLE_WAIT_MS = 250, MAX_PCM_POLLFDS = 8 };

struct queued_frame {
    float left;
    float right;
    uint32_t generation;
};

struct audio_alsa {
    struct queued_frame queue[AUDIO_ALSA_QUEUE_FRAMES];
    atomic_uint_fast64_t head;
    atomic_uint_fast64_t tail;
    atomic_uint generation;
    atomic_uint input_rate;
    atomic_bool paused;
    atomic_bool started;
    atomic_bool stopping;
    atomic_bool degraded_reported;
    atomic_int state;
    atomic_uint output_rate;
    atomic_uint period_frames;
    atomic_uint buffer_frames;
    atomic_int drift_ppm;
    atomic_int_fast64_t delay_frames;
    atomic_uint_fast64_t accepted_frames;
    atomic_uint_fast64_t overflow_frames;
    atomic_uint_fast64_t stale_frames;
    atomic_uint_fast64_t paused_frames;
    atomic_uint_fast64_t underrun_frames;
    atomic_uint_fast64_t xrun_recoveries;
    atomic_uint_fast64_t reopen_attempts;
    atomic_uint_fast64_t reopen_successes;
    int wake_fd;
    const struct audio_alsa_pcm_ops *ops;
    void *context;
    void *pcm;
    struct audio_alsa_pcm_config config;
    pthread_t thread;
    pthread_mutex_t ready_mutex;
    pthread_cond_t ready_cond;
    bool thread_started;
    uint64_t retry_at_ns;
    uint32_t worker_generation;
    unsigned worker_input_rate;
    float previous[2];
    float next[2];
    bool have_previous;
    bool have_next;
    double phase;
    double correction;
    int16_t pending[AUDIO_ALSA_CHUNK_FRAMES * 2];
    size_t pending_frames;
    size_t pending_offset;
};

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static uint64_t audio_now(struct audio_alsa *audio)
{
    return audio->ops->now_ns ? audio->ops->now_ns(audio->context) : monotonic_ns();
}

static void add_counter(atomic_uint_fast64_t *counter, uint64_t amount)
{
    (void)atomic_fetch_add_explicit(counter, amount, memory_order_relaxed);
}

static void notify_worker(struct audio_alsa *audio)
{
    uint64_t one = 1;
    ssize_t n;
    do {
        n = write(audio->wake_fd, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
}

static void drain_wake_fd(struct audio_alsa *audio)
{
    uint64_t value;
    ssize_t n;
    do {
        n = read(audio->wake_fd, &value, sizeof(value));
    } while (n < 0 && errno == EINTR);
}

static void set_state(struct audio_alsa *audio, enum audio_alsa_state state)
{
    atomic_store_explicit(&audio->state, state, memory_order_release);
    if (state == AUDIO_ALSA_DEGRADED &&
        !atomic_exchange_explicit(&audio->degraded_reported, true, memory_order_acq_rel))
        fputs("audio: ALSA default unavailable; retrying\n", stderr);
    else if (state == AUDIO_ALSA_READY &&
             atomic_exchange_explicit(&audio->degraded_reported, false, memory_order_acq_rel))
        fputs("audio: ALSA default recovered\n", stderr);
    (void)pthread_mutex_lock(&audio->ready_mutex);
    (void)pthread_cond_broadcast(&audio->ready_cond);
    (void)pthread_mutex_unlock(&audio->ready_mutex);
}

static int alsa_open(void *context, unsigned requested_rate, unsigned latency_us,
                     void **handle, struct audio_alsa_pcm_config *config)
{
    (void)context;
    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_uframes_t period = 0, buffer = 0;
    unsigned rate = requested_rate;
    int direction = 0;
    int rc = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (rc < 0)
        return rc;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_sw_params_alloca(&sw);
    if ((rc = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
        (rc = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (rc = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (rc = snd_pcm_hw_params_set_channels(pcm, hw, 2)) < 0 ||
        (rc = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, &direction)) < 0) {
        snd_pcm_close(pcm);
        return rc;
    }
    unsigned buffer_time = latency_us;
    unsigned period_time = latency_us / 4;
    direction = 0;
    if ((rc = snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &buffer_time, &direction)) < 0) {
        snd_pcm_close(pcm);
        return rc;
    }
    direction = 0;
    if ((rc = snd_pcm_hw_params_set_period_time_near(pcm, hw, &period_time, &direction)) < 0 ||
        (rc = snd_pcm_hw_params(pcm, hw)) < 0 ||
        (rc = snd_pcm_hw_params_get_rate(hw, &rate, &direction)) < 0 ||
        (rc = snd_pcm_hw_params_get_period_size(hw, &period, &direction)) < 0 ||
        (rc = snd_pcm_hw_params_get_buffer_size(hw, &buffer)) < 0) {
        snd_pcm_close(pcm);
        return rc;
    }
    if ((rc = snd_pcm_sw_params_current(pcm, sw)) < 0 ||
        (rc = snd_pcm_sw_params_set_avail_min(pcm, sw, period)) < 0 ||
        (rc = snd_pcm_sw_params_set_start_threshold(pcm, sw, period)) < 0 ||
        (rc = snd_pcm_sw_params(pcm, sw)) < 0 ||
        (rc = snd_pcm_prepare(pcm)) < 0) {
        snd_pcm_close(pcm);
        return rc;
    }
    config->rate = rate;
    config->period_frames = (unsigned)period;
    config->buffer_frames = (unsigned)buffer;
    *handle = pcm;
    return 0;
}

static void alsa_close(void *context, void *handle)
{
    (void)context;
    if (handle)
        (void)snd_pcm_close(handle);
}

static int alsa_wait(void *context, void *handle, int wake_fd, int timeout_ms)
{
    (void)context;
    snd_pcm_t *pcm = handle;
    struct pollfd fds[MAX_PCM_POLLFDS + 1];
    int count = snd_pcm_poll_descriptors_count(pcm);
    if (count < 0 || count > MAX_PCM_POLLFDS)
        return -EIO;
    if (snd_pcm_poll_descriptors(pcm, fds, (unsigned)count) < 0)
        return -EIO;
    fds[count] = (struct pollfd){ .fd = wake_fd, .events = POLLIN };
    int rc;
    do {
        rc = poll(fds, (nfds_t)count + 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0)
        return -errno;
    if (rc == 0)
        return 0;
    if (fds[count].revents & (POLLIN | POLLERR | POLLHUP))
        return AUDIO_ALSA_WAIT_WAKE;
    unsigned short revents = 0;
    if (snd_pcm_poll_descriptors_revents(pcm, fds, (unsigned)count, &revents) < 0)
        return -EIO;
    return revents ? AUDIO_ALSA_WAIT_READY : 0;
}

static ssize_t alsa_write(void *context, void *handle, const int16_t *samples,
                          size_t frames)
{
    (void)context;
    snd_pcm_sframes_t rc = snd_pcm_writei(handle, samples, (snd_pcm_uframes_t)frames);
    return (ssize_t)rc;
}

static int alsa_recover(void *context, void *handle, int error)
{
    (void)context;
    return snd_pcm_recover(handle, error, 1);
}

static int alsa_delay(void *context, void *handle, int64_t *frames)
{
    (void)context;
    snd_pcm_sframes_t delay = 0;
    int rc = snd_pcm_delay(handle, &delay);
    if (rc >= 0)
        *frames = delay;
    return rc;
}

static const struct audio_alsa_pcm_ops alsa_ops = {
    .open = alsa_open,
    .close = alsa_close,
    .wait = alsa_wait,
    .write = alsa_write,
    .recover = alsa_recover,
    .delay = alsa_delay
};

static bool valid_pcm_ops(const struct audio_alsa_pcm_ops *ops)
{
    return ops && ops->open && ops->close && ops->wait && ops->write &&
           ops->recover && ops->delay;
}

struct audio_alsa *audio_alsa_create_with_ops(unsigned input_rate,
                                               const struct audio_alsa_pcm_ops *ops,
                                               void *context)
{
    if (input_rate < 8000 || input_rate > 192000 || !valid_pcm_ops(ops))
        return NULL;
    struct audio_alsa *audio = calloc(1, sizeof(*audio));
    if (!audio)
        return NULL;
    audio->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (audio->wake_fd < 0) {
        free(audio);
        return NULL;
    }
    int rc = pthread_mutex_init(&audio->ready_mutex, NULL);
    if (rc != 0) {
        close(audio->wake_fd);
        free(audio);
        return NULL;
    }
    rc = pthread_cond_init(&audio->ready_cond, NULL);
    if (rc != 0) {
        (void)pthread_mutex_destroy(&audio->ready_mutex);
        close(audio->wake_fd);
        free(audio);
        return NULL;
    }
    audio->ops = ops;
    audio->context = context;
    atomic_init(&audio->head, 0);
    atomic_init(&audio->tail, 0);
    atomic_init(&audio->generation, 1);
    atomic_init(&audio->input_rate, input_rate);
    atomic_init(&audio->paused, false);
    atomic_init(&audio->started, false);
    atomic_init(&audio->stopping, false);
    atomic_init(&audio->degraded_reported, false);
    atomic_init(&audio->state, AUDIO_ALSA_STOPPED);
    atomic_init(&audio->output_rate, 0);
    atomic_init(&audio->period_frames, 0);
    atomic_init(&audio->buffer_frames, 0);
    atomic_init(&audio->drift_ppm, 0);
    atomic_init(&audio->delay_frames, 0);
    atomic_init(&audio->accepted_frames, 0);
    atomic_init(&audio->overflow_frames, 0);
    atomic_init(&audio->stale_frames, 0);
    atomic_init(&audio->paused_frames, 0);
    atomic_init(&audio->underrun_frames, 0);
    atomic_init(&audio->xrun_recoveries, 0);
    atomic_init(&audio->reopen_attempts, 0);
    atomic_init(&audio->reopen_successes, 0);
    return audio;
}

struct audio_alsa *audio_alsa_open(unsigned input_rate)
{
    struct audio_alsa *audio = audio_alsa_create_with_ops(input_rate, &alsa_ops, NULL);
    if (!audio)
        return NULL;
    if (!audio_alsa_start(audio)) {
        audio_alsa_close(audio);
        return NULL;
    }
    return audio;
}

size_t audio_alsa_write(struct audio_alsa *audio, const int16_t *stereo, size_t frames)
{
    if (!audio || (!stereo && frames != 0) || frames == 0)
        return 0;
    if (atomic_load_explicit(&audio->stopping, memory_order_acquire))
        return 0;
    if (atomic_load_explicit(&audio->paused, memory_order_acquire)) {
        add_counter(&audio->paused_frames, frames);
        return 0;
    }
    uint64_t head = atomic_load_explicit(&audio->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&audio->tail, memory_order_acquire);
    uint64_t used = head - tail;
    size_t available = used < AUDIO_ALSA_QUEUE_FRAMES ?
        AUDIO_ALSA_QUEUE_FRAMES - (size_t)used : 0;
    size_t accepted = frames < available ? frames : available;
    uint32_t generation = atomic_load_explicit(&audio->generation, memory_order_acquire);
    for (size_t i = 0; i < accepted; ++i) {
        struct queued_frame *slot = &audio->queue[(head + i) % AUDIO_ALSA_QUEUE_FRAMES];
        slot->left = (float)stereo[2 * i] * (1.0f / 32768.0f);
        slot->right = (float)stereo[2 * i + 1] * (1.0f / 32768.0f);
        slot->generation = generation;
    }
    if (accepted != 0) {
        atomic_store_explicit(&audio->head, head + accepted, memory_order_release);
        atomic_store_explicit(&audio->started, true, memory_order_release);
        add_counter(&audio->accepted_frames, accepted);
        notify_worker(audio);
    }
    if (frames > accepted)
        add_counter(&audio->overflow_frames, frames - accepted);
    return accepted;
}

void audio_alsa_set_input_rate(struct audio_alsa *audio, unsigned rate)
{
    if (!audio || rate < 8000 || rate > 192000)
        return;
    unsigned previous = atomic_exchange_explicit(&audio->input_rate, rate, memory_order_acq_rel);
    if (previous != rate) audio_alsa_rebase(audio);
}

void audio_alsa_set_paused(struct audio_alsa *audio, bool paused)
{
    if (!audio)
        return;
    bool previous = atomic_exchange_explicit(&audio->paused, paused, memory_order_acq_rel);
    if (previous != paused) audio_alsa_rebase(audio);
}

void audio_alsa_rebase(struct audio_alsa *audio)
{
    if (!audio)
        return;
    (void)atomic_fetch_add_explicit(&audio->generation, 1, memory_order_release);
    notify_worker(audio);
}

static uint64_t queue_depth(const struct audio_alsa *audio)
{
    uint64_t head = atomic_load_explicit(&audio->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&audio->tail, memory_order_acquire);
    uint64_t depth = head - tail;
    return depth > AUDIO_ALSA_QUEUE_FRAMES ? AUDIO_ALSA_QUEUE_FRAMES : depth;
}

void audio_alsa_get_status(const struct audio_alsa *audio, struct audio_alsa_status *status)
{
    if (!audio || !status)
        return;
    *status = (struct audio_alsa_status){
        .state = (enum audio_alsa_state)atomic_load_explicit(&audio->state, memory_order_acquire),
        .input_rate = atomic_load_explicit(&audio->input_rate, memory_order_acquire),
        .output_rate = atomic_load_explicit(&audio->output_rate, memory_order_acquire),
        .period_frames = atomic_load_explicit(&audio->period_frames, memory_order_acquire),
        .buffer_frames = atomic_load_explicit(&audio->buffer_frames, memory_order_acquire),
        .queued_frames = (unsigned)queue_depth(audio),
        .drift_ppm = atomic_load_explicit(&audio->drift_ppm, memory_order_acquire),
        .delay_frames = atomic_load_explicit(&audio->delay_frames, memory_order_acquire),
        .accepted_frames = atomic_load_explicit(&audio->accepted_frames, memory_order_relaxed),
        .overflow_frames = atomic_load_explicit(&audio->overflow_frames, memory_order_relaxed),
        .stale_frames = atomic_load_explicit(&audio->stale_frames, memory_order_relaxed),
        .paused_frames = atomic_load_explicit(&audio->paused_frames, memory_order_relaxed),
        .underrun_frames = atomic_load_explicit(&audio->underrun_frames, memory_order_relaxed),
        .xrun_recoveries = atomic_load_explicit(&audio->xrun_recoveries, memory_order_relaxed),
        .reopen_attempts = atomic_load_explicit(&audio->reopen_attempts, memory_order_relaxed),
        .reopen_successes = atomic_load_explicit(&audio->reopen_successes, memory_order_relaxed)
    };
}

static bool pop_frame(struct audio_alsa *audio, uint32_t generation, float output[2])
{
    uint64_t tail = atomic_load_explicit(&audio->tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&audio->head, memory_order_acquire);
    while (tail < head) {
        struct queued_frame frame = audio->queue[tail % AUDIO_ALSA_QUEUE_FRAMES];
        atomic_store_explicit(&audio->tail, ++tail, memory_order_release);
        if (frame.generation == generation) {
            output[0] = frame.left;
            output[1] = frame.right;
            return true;
        }
        add_counter(&audio->stale_frames, 1);
    }
    return false;
}

static void reset_resampler(struct audio_alsa *audio)
{
    audio->have_previous = false;
    audio->have_next = false;
    audio->phase = 0.0;
    audio->correction = 0.0;
    audio->pending_frames = 0;
    audio->pending_offset = 0;
}

static int16_t float_to_s16(float value)
{
    if (value <= -1.0f) return INT16_MIN;
    if (value >= 1.0f) return INT16_MAX;
    float scaled = value * 32768.0f;
    return (int16_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
}

static void update_drift(struct audio_alsa *audio)
{
    unsigned rate = audio->worker_input_rate;
    double target = (double)rate * (AUDIO_ALSA_LATENCY_US / 1000000.0);
    if (target < 1.0) target = 1.0;
    double error = ((double)queue_depth(audio) - target) / target;
    double desired = error * 0.0005;
    if (desired > 0.005) desired = 0.005;
    if (desired < -0.005) desired = -0.005;
    audio->correction += (desired - audio->correction) * 0.05;
    int32_t ppm = (int32_t)(audio->correction * 1000000.0);
    atomic_store_explicit(&audio->drift_ppm, ppm, memory_order_release);
}

static bool render_chunk(struct audio_alsa *audio, bool paused)
{
    uint32_t generation = audio->worker_generation;
    unsigned output_rate = atomic_load_explicit(&audio->output_rate, memory_order_acquire);
    if (output_rate == 0 || audio->worker_input_rate == 0)
        return false;
    if (!audio->have_previous)
        audio->have_previous = pop_frame(audio, generation, audio->previous);
    if (!audio->have_next)
        audio->have_next = pop_frame(audio, generation, audio->next);
    if (!paused && !audio->have_previous &&
        !atomic_load_explicit(&audio->started, memory_order_acquire))
        return false;
    update_drift(audio);
    double ratio = (double)audio->worker_input_rate / output_rate * (1.0 + audio->correction);
    if (!(ratio > 0.0 && ratio < 16.0))
        return false;
    size_t underrun = 0;
    for (size_t i = 0; i < AUDIO_ALSA_CHUNK_FRAMES; ++i) {
        float left = 0.0f, right = 0.0f;
        if (!paused && audio->have_previous && audio->have_next) {
            left = audio->previous[0] + (audio->next[0] - audio->previous[0]) * (float)audio->phase;
            right = audio->previous[1] + (audio->next[1] - audio->previous[1]) * (float)audio->phase;
            audio->phase += ratio;
            while (audio->phase >= 1.0) {
                audio->phase -= 1.0;
                if (!audio->have_next) {
                    audio->have_previous = false;
                    audio->phase = 0.0;
                    break;
                }
                memcpy(audio->previous, audio->next, sizeof(audio->previous));
                audio->have_previous = true;
                audio->have_next = pop_frame(audio, generation, audio->next);
            }
        } else if (!paused) {
            ++underrun;
            if (!audio->have_next) {
                audio->have_previous = false;
                audio->phase = 0.0;
            }
        }
        audio->pending[2 * i] = float_to_s16(left);
        audio->pending[2 * i + 1] = float_to_s16(right);
    }
    if (underrun != 0)
        add_counter(&audio->underrun_frames, underrun);
    audio->pending_frames = AUDIO_ALSA_CHUNK_FRAMES;
    audio->pending_offset = 0;
    return true;
}

static void update_delay(struct audio_alsa *audio)
{
    int64_t delay = 0;
    if (audio->pcm && audio->ops->delay(audio->context, audio->pcm, &delay) >= 0)
        atomic_store_explicit(&audio->delay_frames, delay, memory_order_release);
}

static void close_pcm(struct audio_alsa *audio)
{
    if (audio->pcm) {
        audio->ops->close(audio->context, audio->pcm);
        audio->pcm = NULL;
    }
    reset_resampler(audio);
    atomic_store_explicit(&audio->output_rate, 0, memory_order_release);
    atomic_store_explicit(&audio->period_frames, 0, memory_order_release);
    atomic_store_explicit(&audio->buffer_frames, 0, memory_order_release);
}

static void mark_disconnected(struct audio_alsa *audio)
{
    close_pcm(audio);
    (void)atomic_fetch_add_explicit(&audio->generation, 1, memory_order_release);
    uint64_t now = audio_now(audio);
    audio->retry_at_ns = now + (uint64_t)REOPEN_DELAY_MS * 1000000ull;
    set_state(audio, AUDIO_ALSA_DEGRADED);
}

static int wait_for_wake(struct audio_alsa *audio, int timeout_ms)
{
    struct pollfd pfd = { .fd = audio->wake_fd, .events = POLLIN };
    int rc;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR && !atomic_load_explicit(&audio->stopping, memory_order_acquire));
    if (rc < 0)
        return -errno;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        return -EIO;
    if (pfd.revents & POLLIN) {
        drain_wake_fd(audio);
        return AUDIO_ALSA_WAIT_WAKE;
    }
    return 0;
}

static bool open_pcm(struct audio_alsa *audio, uint64_t now)
{
    if (now < audio->retry_at_ns)
        return false;
    set_state(audio, AUDIO_ALSA_CONNECTING);
    add_counter(&audio->reopen_attempts, 1);
    unsigned rate = atomic_load_explicit(&audio->input_rate, memory_order_acquire);
    struct audio_alsa_pcm_config config = {0};
    void *handle = NULL;
    int rc = audio->ops->open(audio->context, rate, AUDIO_ALSA_LATENCY_US, &handle, &config);
    if (rc < 0 || !handle || config.rate == 0 || config.period_frames == 0 || config.buffer_frames == 0) {
        if (handle)
            audio->ops->close(audio->context, handle);
        (void)atomic_fetch_add_explicit(&audio->generation, 1, memory_order_release);
        audio->retry_at_ns = now + (uint64_t)REOPEN_DELAY_MS * 1000000ull;
        set_state(audio, AUDIO_ALSA_DEGRADED);
        return false;
    }
    audio->pcm = handle;
    audio->config = config;
    atomic_store_explicit(&audio->output_rate, config.rate, memory_order_release);
    atomic_store_explicit(&audio->period_frames, config.period_frames, memory_order_release);
    atomic_store_explicit(&audio->buffer_frames, config.buffer_frames, memory_order_release);
    audio->worker_generation = atomic_load_explicit(&audio->generation, memory_order_acquire);
    audio->worker_input_rate = atomic_load_explicit(&audio->input_rate, memory_order_acquire);
    reset_resampler(audio);
    add_counter(&audio->reopen_successes, 1);
    set_state(audio, AUDIO_ALSA_READY);
    return true;
}

static bool recover_xrun(struct audio_alsa *audio, int error)
{
    int rc = audio->ops->recover(audio->context, audio->pcm, error);
    if (rc >= 0) {
        add_counter(&audio->xrun_recoveries, 1);
        return true;
    }
    if (rc == -ENODEV || rc == -EIO || rc == -EBADFD || rc == -EPIPE || rc == -ESTRPIPE) {
        mark_disconnected(audio);
        return false;
    }
    mark_disconnected(audio);
    return false;
}

static bool pump_once(struct audio_alsa *audio)
{
    if (atomic_load_explicit(&audio->stopping, memory_order_acquire))
        return true;
    uint64_t now = audio_now(audio);
    if (!audio->pcm) {
        if (now < audio->retry_at_ns)
            return true;
        (void)open_pcm(audio, now);
        return true;
    }
    uint32_t generation = atomic_load_explicit(&audio->generation, memory_order_acquire);
    if (generation != audio->worker_generation) {
        audio->worker_generation = generation;
        audio->worker_input_rate = atomic_load_explicit(&audio->input_rate, memory_order_acquire);
        reset_resampler(audio);
    }
    if (audio->pending_frames == 0) {
        bool paused = atomic_load_explicit(&audio->paused, memory_order_acquire);
        if (!render_chunk(audio, paused))
            return true;
    }
    int ready = audio->ops->wait(audio->context, audio->pcm, audio->wake_fd, 1000);
    if (ready == AUDIO_ALSA_WAIT_WAKE) {
        drain_wake_fd(audio);
        return true;
    }
    if (ready == 0) {
        update_delay(audio);
        return true;
    }
    if (ready < 0) {
        int error = -ready;
        if (error == EPIPE || error == ESTRPIPE)
            (void)recover_xrun(audio, -error);
        else
            mark_disconnected(audio);
        return true;
    }
    if (atomic_load_explicit(&audio->generation, memory_order_acquire) !=
        audio->worker_generation) {
        audio->worker_generation = atomic_load_explicit(&audio->generation, memory_order_acquire);
        audio->worker_input_rate = atomic_load_explicit(&audio->input_rate, memory_order_acquire);
        reset_resampler(audio);
        return true;
    }
    ssize_t written = audio->ops->write(audio->context, audio->pcm,
        audio->pending + 2 * audio->pending_offset,
        audio->pending_frames - audio->pending_offset);
    if (written > 0) {
        size_t amount = (size_t)written;
        size_t remaining = audio->pending_frames - audio->pending_offset;
        if (amount > remaining) {
            mark_disconnected(audio);
            return true;
        }
        audio->pending_offset += amount;
        if (audio->pending_offset == audio->pending_frames) {
            audio->pending_frames = 0;
            audio->pending_offset = 0;
        }
        update_delay(audio);
    } else if (written == 0 || written == -EAGAIN || written == -EWOULDBLOCK) {
        return true;
    } else if (written == -EPIPE || written == -ESTRPIPE) {
        (void)recover_xrun(audio, (int)written);
    } else {
        mark_disconnected(audio);
    }
    return true;
}

bool audio_alsa_pump_once(struct audio_alsa *audio)
{
    return audio && !audio->thread_started && pump_once(audio);
}

static void *worker_main(void *data)
{
    struct audio_alsa *audio = data;
    while (!atomic_load_explicit(&audio->stopping, memory_order_acquire)) {
        uint64_t now = audio_now(audio);
        if (!audio->pcm && now < audio->retry_at_ns) {
            uint64_t left = audio->retry_at_ns - now;
            int timeout = (int)((left + 999999ull) / 1000000ull);
            (void)wait_for_wake(audio, timeout > REOPEN_DELAY_MS ? REOPEN_DELAY_MS : timeout);
            continue;
        }
        if (audio->pcm && audio->pending_frames == 0 &&
            !atomic_load_explicit(&audio->paused, memory_order_acquire) &&
            !atomic_load_explicit(&audio->started, memory_order_acquire)) {
            (void)wait_for_wake(audio, IDLE_WAIT_MS);
            continue;
        }
        (void)pump_once(audio);
        if (audio->pcm && audio->pending_frames == 0 &&
            !atomic_load_explicit(&audio->paused, memory_order_acquire) &&
            queue_depth(audio) < 2) {
            (void)wait_for_wake(audio, IDLE_WAIT_MS);
        }
    }
    close_pcm(audio);
    set_state(audio, AUDIO_ALSA_STOPPED);
    return NULL;
}

bool audio_alsa_start(struct audio_alsa *audio)
{
    if (!audio || audio->thread_started)
        return false;
    atomic_store_explicit(&audio->stopping, false, memory_order_release);
    int rc = pthread_create(&audio->thread, NULL, worker_main, audio);
    if (rc != 0)
        return false;
    audio->thread_started = true;
    return true;
}

bool audio_alsa_wait_ready(struct audio_alsa *audio, unsigned timeout_ms)
{
    if (!audio)
        return false;
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
        return false;
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }
    (void)pthread_mutex_lock(&audio->ready_mutex);
    int rc = 0;
    while (atomic_load_explicit(&audio->state, memory_order_acquire) != AUDIO_ALSA_READY &&
           !atomic_load_explicit(&audio->stopping, memory_order_acquire) && rc == 0)
        rc = pthread_cond_timedwait(&audio->ready_cond, &audio->ready_mutex, &deadline);
    bool ready = atomic_load_explicit(&audio->state, memory_order_acquire) == AUDIO_ALSA_READY;
    (void)pthread_mutex_unlock(&audio->ready_mutex);
    return ready;
}

void audio_alsa_close(struct audio_alsa *audio)
{
    if (!audio)
        return;
    atomic_store_explicit(&audio->stopping, true, memory_order_release);
    notify_worker(audio);
    if (audio->thread_started) {
        int rc = pthread_join(audio->thread, NULL);
        if (rc != 0) {
            fprintf(stderr, "audio: worker join failed: %s\n", strerror(rc));
            return;
        }
    } else {
        close_pcm(audio);
    }
    set_state(audio, AUDIO_ALSA_STOPPED);
    if (close(audio->wake_fd) < 0)
        fprintf(stderr, "audio: wake fd close failed: %s\n", strerror(errno));
    int rc = pthread_cond_destroy(&audio->ready_cond);
    if (rc != 0)
        fprintf(stderr, "audio: condition destroy failed: %s\n", strerror(rc));
    rc = pthread_mutex_destroy(&audio->ready_mutex);
    if (rc != 0)
        fprintf(stderr, "audio: mutex destroy failed: %s\n", strerror(rc));
    free(audio);
}
