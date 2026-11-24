#ifndef BANANA_RETRO_AUDIO_ALSA_H
#define BANANA_RETRO_AUDIO_ALSA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum {
	AUDIO_ALSA_QUEUE_FRAMES = 8192,
	AUDIO_ALSA_CHUNK_FRAMES = 256,
	AUDIO_ALSA_LATENCY_US = 48000,
	AUDIO_ALSA_WAIT_READY = 1,
	AUDIO_ALSA_WAIT_WAKE = 2
};

enum audio_alsa_state {
	AUDIO_ALSA_STOPPED,
	AUDIO_ALSA_CONNECTING,
	AUDIO_ALSA_READY,
	AUDIO_ALSA_DEGRADED
};

struct audio_alsa_pcm_config {
	unsigned rate;
	unsigned period_frames;
	unsigned buffer_frames;
};

struct audio_alsa_pcm_ops {
	uint64_t (*now_ns)(void *context);
	int (*open)(void *context, unsigned rate, unsigned latency_us, void **handle,
		    struct audio_alsa_pcm_config *config);
	void (*close)(void *context, void *handle);
	int (*wait)(void *context, void *handle, int wake_fd, int timeout_ms);
	ssize_t (*write)(void *context, void *handle, const int16_t *stereo, size_t frames);
	int (*recover)(void *context, void *handle, int error);
	int (*delay)(void *context, void *handle, int64_t *frames);
};

struct audio_alsa_status {
	enum audio_alsa_state state;
	unsigned input_rate;
	unsigned output_rate;
	unsigned period_frames;
	unsigned buffer_frames;
	unsigned queued_frames;
	int drift_ppm;
	int64_t delay_frames;
	uint64_t accepted_frames;
	uint64_t overflow_frames;
	uint64_t stale_frames;
	uint64_t paused_frames;
	uint64_t underrun_frames;
	uint64_t xrun_recoveries;
	uint64_t reopen_attempts;
	uint64_t reopen_successes;
};

struct audio_alsa;
struct audio_alsa *audio_alsa_create_with_ops(unsigned input_rate,
		const struct audio_alsa_pcm_ops *ops, void *context);
struct audio_alsa *audio_alsa_open(unsigned input_rate);
bool audio_alsa_start(struct audio_alsa *audio);
bool audio_alsa_pump_once(struct audio_alsa *audio);
bool audio_alsa_wait_ready(struct audio_alsa *audio, unsigned timeout_ms);
size_t audio_alsa_write(struct audio_alsa *audio, const int16_t *stereo, size_t frames);
void audio_alsa_set_input_rate(struct audio_alsa *audio, unsigned rate);
void audio_alsa_set_paused(struct audio_alsa *audio, bool paused);
void audio_alsa_rebase(struct audio_alsa *audio);
void audio_alsa_get_status(const struct audio_alsa *audio, struct audio_alsa_status *status);
void audio_alsa_close(struct audio_alsa *audio);

void core_set_audio_output(struct audio_alsa *audio);
double core_sample_rate(void);

#endif
