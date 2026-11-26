#ifndef BANANA_RETRO_INPUT_EVDEV_H
#define BANANA_RETRO_INPUT_EVDEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "libretro.h"

enum { RETRO_PAD_BUTTONS = 16, RETRO_INPUT_MAX_DEVICES = 4 };
enum { INPUT_HAT_X = 1, INPUT_HAT_Y = 2 };
enum input_profile_kind { INPUT_PROFILE_GENERIC, INPUT_PROFILE_RPC };

struct input_evdev;

struct input_evdev *input_evdev_open(const char *device_directory);
int input_evdev_wait(struct input_evdev *input, int timeout_ms);
int input_evdev_fd(const struct input_evdev *input);
bool input_evdev_resume(struct input_evdev *input);
bool input_evdev_poll(struct input_evdev *input);
uint16_t input_evdev_mask(const struct input_evdev *input, unsigned port);
bool input_evdev_snapshot(struct input_evdev *input, uint16_t masks[RETRO_INPUT_MAX_DEVICES]);
void input_evdev_close(struct input_evdev *input);
enum input_profile_kind input_profile_select(uint16_t vendor, uint16_t product,
					     const char *name);

/* Event-level seam used by deterministic fixtures and the evdev reader. */
struct input_pad_state {
	uint16_t buttons;
	int8_t hat_x;
	int8_t hat_y;
};
void input_pad_event_profile(struct input_pad_state *state, enum input_profile_kind profile,
			     unsigned hat_axes, unsigned type, unsigned code, int value);
typedef bool (*input_pad_query_fn)(void *data, struct input_pad_state *state);
bool input_pad_feed_event(struct input_pad_state *state, bool *dropped,
			  enum input_profile_kind profile, unsigned hat_axes, unsigned type,
			  unsigned code, int value, input_pad_query_fn query, void *data);
void input_pad_device_lost(struct input_pad_state *state, bool *dropped);
void input_pad_resync(struct input_pad_state *state, const unsigned long *keys,
		      const int8_t hat[2], unsigned key_words,
		      enum input_profile_kind profile, unsigned hat_axes);
uint16_t input_pad_mask(const struct input_pad_state *state);

struct retro_frame_clock {
	uint64_t deadline_ns;
	uint64_t period_ns;
	uint64_t remainder;
	uint64_t phase;
	uint64_t overruns;
	uint64_t rebases;
	unsigned catchups;
};
bool retro_frame_clock_start(struct retro_frame_clock *clock, double fps,
			     uint64_t now_ns);
uint64_t retro_frame_clock_advance(struct retro_frame_clock *clock,
				   uint64_t now_ns);
enum retro_paced_result {
	RETRO_PACED_FAILED = -1,
	RETRO_PACED_STOPPED = 0,
	RETRO_PACED_FRAME = 1
};
struct retro_paced_ops {
	void *data;
	uint64_t (*now_ns)(void *data);
	int (*wait_until)(void *data, uint64_t deadline_ns);
	bool (*run_frame)(void *data, unsigned frame);
	double (*refresh_rate)(void *data);
};
enum { RETRO_FRAME_JITTER_BUCKETS = 8 };
/* An interval longer than this many periods is counted as a stall (it stays in the interval statistics). */
enum { RETRO_FRAME_STALL_PERIODS = 8 };
/* Room for the telemetry line with every field at UINT64_MAX; formatting never truncates into a buffer this size. */
enum { RETRO_FRAME_STATS_LINE_MAX = 1536 };
/* Opt-in aggregate frame statistics: constant memory, no per-frame output. */
struct retro_frame_stats {
	uint64_t frames;
	uint64_t intervals;
	uint64_t restarts;
	uint64_t rate_changes;
	uint64_t stalls;
	uint64_t last_start_ns;
	uint64_t interval_min_ns;
	uint64_t interval_max_ns;
	uint64_t interval_sum_ns;
	uint64_t late_frames;	/* started more than a quarter period after the deadline */
	uint64_t late_n;	/* started after the deadline at all: the divisor of the mean lateness */
	uint64_t late_max_ns;
	uint64_t late_sum_ns;
	uint64_t jitter[RETRO_FRAME_JITTER_BUCKETS];
};
struct retro_paced_runner {
	struct retro_frame_clock clock;
	double fps;
	bool telemetry;
	struct retro_frame_stats stats;
};
void retro_frame_stats_record(struct retro_frame_stats *stats, uint64_t start_ns,
			      uint64_t deadline_ns, uint64_t period_ns);
int retro_frame_stats_format(const struct retro_paced_runner *runner, char *buffer, size_t size);
bool retro_paced_start(struct retro_paced_runner *runner, double fps, uint64_t now_ns);
/* Re-anchor the clock (resume). With telemetry off this is exactly retro_paced_start. With telemetry on it keeps the
 * statistics and the cumulative deadline counters, and the interval across the discontinuity is not recorded. A
 * failed restart leaves the runner untouched. */
bool retro_paced_restart(struct retro_paced_runner *runner, double fps, uint64_t now_ns);
enum retro_paced_result retro_paced_run_one(struct retro_paced_runner *runner,
						     const struct retro_paced_ops *ops,
						     unsigned frame);

void core_set_input_source(void *data, void (*snapshot)(void *data,
							   uint16_t masks[RETRO_INPUT_MAX_DEVICES]));
double core_refresh_rate(void);

#endif
