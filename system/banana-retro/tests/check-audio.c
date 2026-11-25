#define _GNU_SOURCE
#include "../src/audio_alsa.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

struct fake_pcm {
    uint64_t now_ns;
    unsigned output_rate;
    unsigned requested_rate;
    unsigned latency_us;
    unsigned open_calls;
    unsigned close_calls;
    unsigned recover_calls;
    unsigned delay_calls;
    unsigned fail_opens;
    int recover_result;
    int wait_result;
    ssize_t write_script[16];
    size_t write_script_count;
    size_t write_script_index;
    int16_t written[4096 * 2];
    size_t written_frames;
};

static uint64_t fake_now(void *context)
{
    return ((struct fake_pcm *)context)->now_ns;
}

static int fake_open(void *context, unsigned rate, unsigned latency_us, void **handle,
                     struct audio_alsa_pcm_config *config)
{
    struct fake_pcm *fake = context;
    ++fake->open_calls;
    fake->requested_rate = rate;
    fake->latency_us = latency_us;
    if (fake->fail_opens != 0) {
        --fake->fail_opens;
        return -ENODEV;
    }
    config->rate = fake->output_rate;
    config->period_frames = 256;
    config->buffer_frames = 2048;
    *handle = fake;
    return 0;
}

static void fake_close(void *context, void *handle)
{
    struct fake_pcm *fake = context;
    assert(handle == fake);
    ++fake->close_calls;
}

static int fake_wait(void *context, void *handle, int wake_fd, int timeout_ms)
{
    struct fake_pcm *fake = context;
    assert(handle == fake);
    (void)wake_fd;
    (void)timeout_ms;
    return fake->wait_result ? fake->wait_result : AUDIO_ALSA_WAIT_READY;
}

static ssize_t fake_write(void *context, void *handle, const int16_t *samples,
                          size_t frames)
{
    struct fake_pcm *fake = context;
    assert(handle == fake);
    ssize_t result = (ssize_t)frames;
    if (fake->write_script_index < fake->write_script_count)
        result = fake->write_script[fake->write_script_index++];
    if (result <= 0)
        return result;
    size_t accepted = (size_t)result < frames ? (size_t)result : frames;
    assert(fake->written_frames + accepted <= 4096);
    memcpy(fake->written + fake->written_frames * 2, samples,
           accepted * 2 * sizeof(*samples));
    fake->written_frames += accepted;
    return (ssize_t)accepted;
}

static int fake_recover(void *context, void *handle, int error)
{
    struct fake_pcm *fake = context;
    assert(handle == fake);
    assert(error == -EPIPE || error == -ESTRPIPE);
    ++fake->recover_calls;
    return fake->recover_result;
}

static int fake_delay(void *context, void *handle, int64_t *frames)
{
    struct fake_pcm *fake = context;
    assert(handle == fake);
    ++fake->delay_calls;
    *frames = 123;
    return 0;
}

static const struct audio_alsa_pcm_ops fake_ops = {
    .open = fake_open,
    .close = fake_close,
    .wait = fake_wait,
    .write = fake_write,
    .recover = fake_recover,
    .delay = fake_delay,
    .now_ns = fake_now
};

static struct audio_alsa *new_audio(struct fake_pcm *fake, unsigned rate)
{
    fake->now_ns = 1000000000ull;
    fake->output_rate = rate;
    fake->wait_result = AUDIO_ALSA_WAIT_READY;
    return audio_alsa_create_with_ops(rate, &fake_ops, fake);
}

static void open_fake(struct audio_alsa *audio, struct fake_pcm *fake)
{
    assert(audio_alsa_pump_once(audio));
    assert(fake->open_calls == 1);
    assert(fake->latency_us == AUDIO_ALSA_LATENCY_US);
    struct audio_alsa_status status;
    audio_alsa_get_status(audio, &status);
    assert(status.state == AUDIO_ALSA_READY);
    assert(status.output_rate == fake->output_rate);
    assert(status.period_frames == 256 && status.buffer_frames == 2048);
}

static void test_bounded_queue_and_pause(void)
{
    struct fake_pcm fake = {0};
    struct audio_alsa *audio = new_audio(&fake, 48000);
    assert(audio != NULL);
    int16_t samples[AUDIO_ALSA_QUEUE_FRAMES * 2 + 16];
    memset(samples, 0, sizeof(samples));
    assert(audio_alsa_write(audio, samples, AUDIO_ALSA_QUEUE_FRAMES + 16) ==
           AUDIO_ALSA_QUEUE_FRAMES);
    audio_alsa_set_paused(audio, true);
    assert(audio_alsa_write(audio, samples, 16) == 0);
    struct audio_alsa_status status;
    audio_alsa_get_status(audio, &status);
    assert(status.queued_frames == AUDIO_ALSA_QUEUE_FRAMES);
    assert(status.accepted_frames == AUDIO_ALSA_QUEUE_FRAMES);
    assert(status.overflow_frames == 16 && status.paused_frames == 16);
    audio_alsa_close(audio);
}

static void test_partial_write_xrun_and_rate(void)
{
    struct fake_pcm fake = {0};
    struct audio_alsa *audio = new_audio(&fake, 48000);
    assert(audio != NULL);
    open_fake(audio, &fake);
    int16_t samples[512 * 2];
    for (size_t i = 0; i < 512; ++i) {
        samples[2 * i] = 1000;
        samples[2 * i + 1] = -1000;
    }
    assert(audio_alsa_write(audio, samples, 512) == 512);
    fake.write_script[0] = 17;
    fake.write_script[1] = -EAGAIN;
    fake.write_script[2] = 20;
    fake.write_script[3] = -EPIPE;
    fake.write_script[4] = -EINTR;
    fake.write_script_count = 5;
    for (unsigned i = 0; i < 6; ++i)
        assert(audio_alsa_pump_once(audio));
    assert(fake.written_frames == AUDIO_ALSA_CHUNK_FRAMES);
    for (size_t i = 0; i < fake.written_frames; ++i) {
        assert(fake.written[2 * i] == 1000);
        assert(fake.written[2 * i + 1] == -1000);
    }
    assert(fake.recover_calls == 1 && fake.delay_calls >= 1);
    struct audio_alsa_status status;
    audio_alsa_get_status(audio, &status);
    assert(status.xrun_recoveries == 1 && status.delay_frames == 123);
    assert(status.accepted_frames == 512);
    audio_alsa_set_input_rate(audio, 32000);
    assert(audio_alsa_pump_once(audio));
    audio_alsa_get_status(audio, &status);
    assert(status.input_rate == 32000);
    assert(status.stale_frames >= 255);
    audio_alsa_set_paused(audio, true);
    assert(audio_alsa_write(audio, samples, 4) == 0);
    assert(audio_alsa_pump_once(audio));
    assert(fake.written_frames == AUDIO_ALSA_CHUNK_FRAMES * 3);
    for (size_t i = AUDIO_ALSA_CHUNK_FRAMES; i < fake.written_frames; ++i)
        assert(fake.written[2 * i] == 0 && fake.written[2 * i + 1] == 0);
    audio_alsa_close(audio);
    assert(fake.close_calls == 1);
}

static void test_reopen_and_drift(void)
{
    struct fake_pcm fake = { .fail_opens = 1 };
    struct audio_alsa *audio = new_audio(&fake, 8000);
    assert(audio != NULL);
    assert(audio_alsa_pump_once(audio));
    struct audio_alsa_status status;
    audio_alsa_get_status(audio, &status);
    assert(status.state == AUDIO_ALSA_DEGRADED && status.reopen_attempts == 1);
    assert(audio_alsa_pump_once(audio));
    assert(fake.open_calls == 1);
    fake.now_ns += 1000000000ull;
    assert(audio_alsa_pump_once(audio));
    audio_alsa_get_status(audio, &status);
    assert(status.state == AUDIO_ALSA_READY && status.reopen_successes == 1);
    assert(fake.open_calls == 2 && fake.requested_rate == 8000);

    int16_t samples[AUDIO_ALSA_QUEUE_FRAMES * 2] = {0};
    assert(audio_alsa_write(audio, samples, AUDIO_ALSA_QUEUE_FRAMES) == AUDIO_ALSA_QUEUE_FRAMES);
    assert(audio_alsa_pump_once(audio));
    audio_alsa_get_status(audio, &status);
    assert(status.drift_ppm > 0 && status.drift_ppm <= 5000);
    audio_alsa_close(audio);
}

static void test_disconnect_and_thread_shutdown(void)
{
    struct fake_pcm fake = {0};
    struct audio_alsa *audio = new_audio(&fake, 48000);
    assert(audio != NULL);
    open_fake(audio, &fake);
    int16_t samples[512 * 2] = {0};
    assert(audio_alsa_write(audio, samples, 512) == 512);
    fake.write_script[0] = -ENODEV;
    fake.write_script_count = 1;
    assert(audio_alsa_pump_once(audio));
    struct audio_alsa_status status;
    audio_alsa_get_status(audio, &status);
    assert(status.state == AUDIO_ALSA_DEGRADED && fake.close_calls == 1);
    fake.now_ns += 1000000000ull;
    assert(audio_alsa_pump_once(audio));
    audio_alsa_get_status(audio, &status);
    assert(status.state == AUDIO_ALSA_READY && status.reopen_successes == 2);
    audio_alsa_close(audio);
    assert(fake.close_calls == 2);

    struct fake_pcm threaded = {0};
    audio = new_audio(&threaded, 48000);
    assert(audio != NULL && audio_alsa_start(audio));
    assert(audio_alsa_wait_ready(audio, 1000));
    assert(audio_alsa_sleep(audio, true));
    assert(threaded.close_calls == 1 && !audio_alsa_wait_ready(audio, 1));
    assert(audio_alsa_sleep(audio, false) && audio_alsa_wait_ready(audio, 1000));
    audio_alsa_close(audio);
    assert(threaded.open_calls == 2 && threaded.close_calls == 2);
}

static void test_resampling_and_negative_drift(void)
{
    struct fake_pcm fast = { .now_ns = 1000000000ull, .output_rate = 24000,
                             .wait_result = AUDIO_ALSA_WAIT_READY };
    struct audio_alsa *audio = audio_alsa_create_with_ops(48000, &fake_ops, &fast);
    assert(audio != NULL);
    open_fake(audio, &fast);
    int16_t samples[AUDIO_ALSA_QUEUE_FRAMES * 2];
    for (size_t i = 0; i < AUDIO_ALSA_QUEUE_FRAMES; ++i) {
        samples[2 * i] = 2000;
        samples[2 * i + 1] = -2000;
    }
    assert(audio_alsa_write(audio, samples, AUDIO_ALSA_QUEUE_FRAMES) == AUDIO_ALSA_QUEUE_FRAMES);
    assert(audio_alsa_pump_once(audio));
    struct audio_alsa_status status;
    audio_alsa_get_status(audio, &status);
    assert(status.drift_ppm > 0 && status.drift_ppm <= 5000);
    assert(status.queued_frames >= 7678 && status.queued_frames <= 7680);
    assert(fast.written_frames == AUDIO_ALSA_CHUNK_FRAMES);
    for (size_t i = 0; i < fast.written_frames; ++i) {
        assert(fast.written[2 * i] == 2000);
        assert(fast.written[2 * i + 1] == -2000);
    }
    audio_alsa_close(audio);

    struct fake_pcm slow = {0};
    audio = new_audio(&slow, 8000);
    assert(audio != NULL);
    open_fake(audio, &slow);
    int16_t short_input[128 * 2];
    for (size_t i = 0; i < 128; ++i) {
        short_input[2 * i] = 2000;
        short_input[2 * i + 1] = -2000;
    }
    assert(audio_alsa_write(audio, short_input, 128) == 128);
    assert(audio_alsa_pump_once(audio));
    audio_alsa_get_status(audio, &status);
    assert(status.drift_ppm < 0 && status.drift_ppm >= -5000);
    assert(status.underrun_frames != 0);
    audio_alsa_close(audio);
}

int main(void)
{
    test_bounded_queue_and_pause();
    test_partial_write_xrun_and_rate();
    test_reopen_and_drift();
    test_disconnect_and_thread_shutdown();
    test_resampling_and_negative_drift();
    puts("banana-retro ALSA queue and worker checks passed");
    return 0;
}
