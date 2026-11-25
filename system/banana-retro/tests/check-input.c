#define _GNU_SOURCE
#include "../src/input_evdev.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

static bool mock_enabled;
static char mock_path[256];
static int mock_read_fd = -1, mock_write_fd = -1;
static unsigned long mock_keys[(KEY_MAX + sizeof(unsigned long) * 8) /
			       (sizeof(unsigned long) * 8)];

static void mock_key(unsigned code, bool down)
{
	unsigned long bit = 1ul << (code % (sizeof(unsigned long) * 8));
	if (down) mock_keys[code / (sizeof(unsigned long) * 8)] |= bit;
	else mock_keys[code / (sizeof(unsigned long) * 8)] &= ~bit;
}

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...)
{
	if (mock_enabled && strcmp(path, mock_path) == 0) {
		int pipes[2];
		if (pipe2(pipes, O_NONBLOCK | O_CLOEXEC) != 0) return -1;
		mock_read_fd = pipes[0];
		mock_write_fd = pipes[1];
		return mock_read_fd;
	}
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list args;
		va_start(args, flags);
		mode = (mode_t)va_arg(args, int);
		va_end(args);
	}
	return (flags & O_CREAT) ? __real_open(path, flags, mode) : __real_open(path, flags);
}

int __real_ioctl(int fd, unsigned long request, ...);
int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list args;
	va_start(args, request);
	void *data = va_arg(args, void *);
	va_end(args);
	if (fd != mock_read_fd || !mock_enabled) return __real_ioctl(fd, request, data);
	switch (_IOC_NR(request)) {
	case 0x02: {
		struct input_id *id = data;
		*id = (struct input_id){ .bustype = BUS_USB, .vendor = 12068, .product = 1 };
		return 0;
	}
	case 0x06:
		if (_IOC_SIZE(request) < strlen("Retroid Pocket Classic Gamepad") + 1) {
			errno = EINVAL; return -1;
		}
		strcpy(data, "Retroid Pocket Classic Gamepad");
		return 0;
	case 0x21: {
		unsigned long *bits = data;
		memset(bits, 0, _IOC_SIZE(request));
		const unsigned codes[] = { BTN_GAMEPAD, BTN_SOUTH, BTN_EAST, BTN_NORTH,
			BTN_WEST, BTN_SELECT, BTN_START, BTN_DPAD_UP, BTN_DPAD_DOWN,
			BTN_DPAD_LEFT, BTN_DPAD_RIGHT };
		for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
			bits[codes[i] / (sizeof(*bits) * 8)] |=
				1ul << (codes[i] % (sizeof(*bits) * 8));
		return _IOC_SIZE(request);
	}
	case 0x23:
		memset(data, 0, _IOC_SIZE(request));
		return _IOC_SIZE(request);
	case 0x18:
		memcpy(data, mock_keys, _IOC_SIZE(request));
		return _IOC_SIZE(request);
	default:
		if (_IOC_NR(request) >= 0x40 && _IOC_NR(request) <= 0x41) {
			struct input_absinfo *info = data;
			memset(info, 0, sizeof(*info));
			return 0;
		}
		errno = ENOTTY;
		return -1;
	}
}

static void mock_event(unsigned type, unsigned code, int value)
{
	struct input_event event = { .type = (uint16_t)type, .code = (uint16_t)code,
		.value = value };
	assert(mock_write_fd >= 0);
	if (type == EV_KEY) mock_key(code, value != 0);
	assert(write(mock_write_fd, &event, sizeof(event)) == (ssize_t)sizeof(event));
}

static void test_profiles_and_pad(void)
{
	struct input_pad_state pad = {0};
	assert(input_profile_select(12068, 1, "Retroid Pocket Classic Gamepad") == INPUT_PROFILE_RPC);
	assert(input_profile_select(12068, 2, "Retroid Pocket Classic Gamepad") == INPUT_PROFILE_GENERIC);
	assert(input_profile_select(12068, 1, "other") == INPUT_PROFILE_GENERIC);
	input_pad_event_profile(&pad, INPUT_PROFILE_RPC, 0, EV_KEY, BTN_EAST, 1);
	assert(input_pad_mask(&pad) == (1u << 8));
	input_pad_event_profile(&pad, INPUT_PROFILE_RPC, 0, EV_KEY, BTN_EAST, 2);
	assert(input_pad_mask(&pad) == (1u << 8));
	input_pad_event_profile(&pad, INPUT_PROFILE_RPC, 0, EV_KEY, BTN_EAST, 0);
	assert(input_pad_mask(&pad) == 0);
	input_pad_event_profile(&pad, INPUT_PROFILE_RPC, 0, EV_KEY, BTN_DPAD_UP, 1);
	input_pad_event_profile(&pad, INPUT_PROFILE_RPC, 0, EV_ABS, ABS_HAT0X, -1);
	assert(input_pad_mask(&pad) == (1u << 4));
	input_pad_event_profile(&pad, INPUT_PROFILE_RPC, 0, EV_KEY, BTN_DPAD_UP, 0);
	input_pad_event_profile(&pad, INPUT_PROFILE_RPC, 0, EV_ABS, ABS_HAT0X, 0);
	assert(input_pad_mask(&pad) == 0);
	input_pad_event_profile(&pad, INPUT_PROFILE_GENERIC, INPUT_HAT_X,
				 EV_ABS, ABS_HAT0X, -1);
	assert(input_pad_mask(&pad) == (1u << 6));
	input_pad_event_profile(&pad, INPUT_PROFILE_GENERIC, 0,
				 EV_ABS, ABS_HAT0Y, -1);
	assert(input_pad_mask(&pad) == (1u << 6));
}

static void test_resync(void)
{
	struct input_pad_state pad = { .buttons = 0xffff };
	unsigned long keys[(KEY_MAX + sizeof(unsigned long) * 8) /
			    (sizeof(unsigned long) * 8)] = {0};
	int8_t hat[2] = {0, 0};
	keys[BTN_SOUTH / (sizeof(unsigned long) * 8)] |=
		1ul << (BTN_SOUTH % (sizeof(unsigned long) * 8));
	hat[1] = 1;
	input_pad_resync(&pad, keys, hat, (unsigned)(sizeof(keys) / sizeof(keys[0])),
			 INPUT_PROFILE_GENERIC, INPUT_HAT_X | INPUT_HAT_Y);
	assert(input_pad_mask(&pad) == ((1u << 0) | (1u << 5)));
	memset(keys, 0, sizeof(keys));
	hat[1] = 0;
	input_pad_resync(&pad, keys, hat, (unsigned)(sizeof(keys) / sizeof(keys[0])),
			 INPUT_PROFILE_GENERIC, INPUT_HAT_X | INPUT_HAT_Y);
	assert(input_pad_mask(&pad) == 0);
}

struct fake_query_state { bool south; int8_t hat_y; enum input_profile_kind profile; unsigned hat_axes; };

static bool query_fake(void *data, struct input_pad_state *pad)
{
	struct fake_query_state *fake = data;
	unsigned long keys[(KEY_MAX + sizeof(unsigned long) * 8) /
			    (sizeof(unsigned long) * 8)] = {0};
	int8_t hat[2] = {0, fake->hat_y};
	if (fake->south)
		keys[BTN_SOUTH / (sizeof(unsigned long) * 8)] |=
			1ul << (BTN_SOUTH % (sizeof(unsigned long) * 8));
	input_pad_resync(pad, keys, hat, (unsigned)(sizeof(keys) / sizeof(keys[0])),
			 fake->profile, fake->hat_axes);
	return true;
}

static void test_syn_dropped_and_loss(void)
{
	struct input_pad_state pad = {0};
	bool dropped = false;
	struct fake_query_state fake = { .south = true, .hat_y = 0,
		.profile = INPUT_PROFILE_RPC };
	assert(input_pad_feed_event(&pad, &dropped, INPUT_PROFILE_RPC, 0, EV_KEY,
				     BTN_EAST, 1, query_fake, &fake));
	assert(input_pad_mask(&pad) == (1u << 8));
	assert(input_pad_feed_event(&pad, &dropped, INPUT_PROFILE_RPC, 0, EV_SYN,
				     SYN_DROPPED, 0, query_fake, &fake));
	assert(dropped && input_pad_mask(&pad) == 0);
	assert(input_pad_feed_event(&pad, &dropped, INPUT_PROFILE_RPC, 0, EV_KEY,
				     BTN_EAST, 0, query_fake, &fake));
	assert(input_pad_feed_event(&pad, &dropped, INPUT_PROFILE_RPC, 0, EV_SYN,
				     SYN_REPORT, 0, query_fake, &fake));
	assert(!dropped && input_pad_mask(&pad) == (1u << 0));
	fake.south = false;
	fake.hat_y = -1;
	fake.profile = INPUT_PROFILE_GENERIC;
	fake.hat_axes = INPUT_HAT_Y;
	assert(input_pad_feed_event(&pad, &dropped, INPUT_PROFILE_GENERIC, INPUT_HAT_Y, EV_SYN,
				     SYN_DROPPED, 0, query_fake, &fake));
	assert(input_pad_feed_event(&pad, &dropped, INPUT_PROFILE_GENERIC, INPUT_HAT_Y, EV_SYN,
				     SYN_REPORT, 0, query_fake, &fake));
	assert(input_pad_mask(&pad) == (1u << 4));
	input_pad_device_lost(&pad, &dropped);
	assert(!dropped && input_pad_mask(&pad) == 0);
}

struct fake_paced_runtime {
	uint64_t now;
	uint64_t cost_ns;
	double fps;
	double change_fps;
	unsigned waits;
	unsigned frames;
	unsigned idle_waits;
	int wait_result;
};

static uint64_t fake_now(void *data)
{
	return ((struct fake_paced_runtime *)data)->now;
}

static int fake_wait(void *data, uint64_t deadline)
{
	struct fake_paced_runtime *fake = data;
	++fake->waits;
	if (fake->wait_result <= 0) return fake->wait_result;
	if (fake->now < deadline) {
		fake->now = deadline;
		++fake->idle_waits;
	}
	return 1;
}

static bool fake_frame(void *data, unsigned frame)
{
	struct fake_paced_runtime *fake = data;
	assert(frame == fake->frames + 1);
	++fake->frames;
	fake->now += fake->cost_ns;
	fake->cost_ns = 0;
	if (fake->change_fps > 0) {
		fake->fps = fake->change_fps;
		fake->change_fps = 0;
	}
	return true;
}

static double fake_fps(void *data)
{
	return ((struct fake_paced_runtime *)data)->fps;
}

static struct retro_paced_ops fake_ops(struct fake_paced_runtime *fake)
{
	return (struct retro_paced_ops){ .data = fake, .now_ns = fake_now,
		.wait_until = fake_wait, .run_frame = fake_frame, .refresh_rate = fake_fps };
}

static void test_clock_rate(double fps)
{
	struct fake_paced_runtime fake = { .now = 1000000000ull, .fps = fps,
		.wait_result = 1 };
	struct retro_paced_ops ops = fake_ops(&fake);
	struct retro_paced_runner runner;
	const uint64_t frames = 1000000;
	assert(retro_paced_start(&runner, fps, fake.now));
	for (unsigned frame = 1; frame <= frames; ++frame)
		assert(retro_paced_run_one(&runner, &ops, frame) == RETRO_PACED_FRAME);
	long double expected = 1000000000.0L +
		(long double)(frames + 1) * 1000000000.0L / (long double)fps;
	assert(fabsl((long double)runner.clock.deadline_ns - expected) < 2.0L);
	assert(fake.frames == frames && fake.idle_waits == frames);
}

static void test_overrun_and_rate_change(void)
{
	struct fake_paced_runtime fake = { .now = 1000000000ull, .fps = 60.0,
		.cost_ns = 500000000ull, .wait_result = 1 };
	struct retro_paced_ops ops = fake_ops(&fake);
	struct retro_paced_runner runner;
	assert(retro_paced_start(&runner, fake.fps, fake.now));
	for (unsigned frame = 1; frame <= 3; ++frame)
		assert(retro_paced_run_one(&runner, &ops, frame) == RETRO_PACED_FRAME);
	assert(runner.clock.overruns == 3 && runner.clock.rebases == 1);
	assert(runner.clock.deadline_ns > fake.now);
	assert(retro_paced_run_one(&runner, &ops, 4) == RETRO_PACED_FRAME);
	assert(fake.idle_waits == 2);
	fake.change_fps = 50.0;
	assert(retro_paced_run_one(&runner, &ops, 5) == RETRO_PACED_FRAME);
	assert(runner.fps == 50.0 && runner.clock.deadline_ns > fake.now);
}

static void test_paced_wait_results(void)
{
	struct fake_paced_runtime fake = { .now = 1000000000ull, .fps = 60.0,
		.wait_result = -1 };
	struct retro_paced_ops ops = fake_ops(&fake);
	struct retro_paced_runner runner;
	assert(retro_paced_start(&runner, fake.fps, fake.now));
	assert(retro_paced_run_one(&runner, &ops, 1) == RETRO_PACED_FAILED);
	assert(fake.frames == 0);
	fake.wait_result = 0;
	assert(retro_paced_run_one(&runner, &ops, 1) == RETRO_PACED_STOPPED);
	assert(fake.frames == 0);
}

static void test_empty_source_blocks(void)
{
	char path[] = "/tmp/banana-retro-input.XXXXXX";
	assert(mkdtemp(path) != NULL);
	struct input_evdev *input = input_evdev_open(path);
	assert(input != NULL);
	struct timespec start, end;
	assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	assert(input_evdev_wait(input, 25) == 0);
	assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
	long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000L +
		(end.tv_nsec - start.tv_nsec) / 1000000L;
	assert(elapsed_ms >= 15 && elapsed_ms < 1000);
	uint16_t masks[RETRO_INPUT_MAX_DEVICES] = {0xffff, 0xffff, 0xffff, 0xffff};
	assert(input_evdev_snapshot(input, masks));
	for (size_t i = 0; i < RETRO_INPUT_MAX_DEVICES; ++i) assert(masks[i] == 0);
	char event_path[256];
	assert(snprintf(event_path, sizeof(event_path), "%s/event-test", path) > 0);
	int fd = open(event_path, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
	assert(fd >= 0 && close(fd) == 0);
	assert(input_evdev_wait(input, 100) > 0);
	assert(input_evdev_poll(input));
	assert(unlink(event_path) == 0);
	assert(input_evdev_wait(input, 100) > 0);
	assert(input_evdev_poll(input));
	assert(input_evdev_mask(input, 0) == 0);
	input_evdev_close(input);
	assert(rmdir(path) == 0);
}

static void test_hotplug_lifecycle(void)
{
	char directory[] = "/tmp/banana-retro-pad.XXXXXX";
	assert(mkdtemp(directory) != NULL);
	assert(snprintf(mock_path, sizeof(mock_path), "%s/event0", directory) > 0);
	struct input_evdev *input = input_evdev_open(directory);
	assert(input != NULL);
	int node = __real_open(mock_path, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
	assert(node >= 0 && close(node) == 0);
	memset(mock_keys, 0, sizeof(mock_keys));
	mock_key(BTN_SOUTH, true);
	mock_enabled = true;
	assert(input_evdev_wait(input, 100) > 0);
	uint16_t masks[RETRO_INPUT_MAX_DEVICES] = {0};
	assert(input_evdev_snapshot(input, masks));
	assert(masks[0] == (1u << 0));
	mock_event(EV_KEY, BTN_EAST, 1);
	assert(input_evdev_wait(input, 100) > 0 && input_evdev_snapshot(input, masks));
	assert(masks[0] == ((1u << 0) | (1u << 8)));
	mock_event(EV_KEY, BTN_EAST, 0);
	assert(input_evdev_wait(input, 100) > 0 && input_evdev_snapshot(input, masks));
	assert(masks[0] == (1u << 0));
	mock_event(EV_SYN, SYN_DROPPED, 0);
	assert(input_evdev_wait(input, 100) > 0 && input_evdev_snapshot(input, masks));
	assert(masks[0] == 0);
	mock_event(EV_KEY, BTN_EAST, 1);
	memset(mock_keys, 0, sizeof(mock_keys));
	mock_key(BTN_NORTH, true);
	mock_event(EV_SYN, SYN_REPORT, 0);
	assert(input_evdev_wait(input, 100) > 0 && input_evdev_snapshot(input, masks));
	assert(masks[0] == (1u << 9));
	/* The UI owned the fd grab during these transitions. */
	mock_key(BTN_NORTH, false);
	mock_key(BTN_EAST, true);
	assert(input_evdev_resume(input) && input_evdev_snapshot(input, masks) && masks[0] == 0);
	mock_event(EV_KEY, BTN_EAST, 2);
	assert(input_evdev_snapshot(input, masks) && masks[0] == 0);
	mock_event(EV_KEY, BTN_EAST, 0);
	mock_event(EV_KEY, BTN_EAST, 1);
	assert(input_evdev_snapshot(input, masks) && masks[0] == (1u << 8));
	assert(close(mock_write_fd) == 0);
	mock_write_fd = -1;
	assert(unlink(mock_path) == 0);
	assert(input_evdev_wait(input, 100) > 0 && input_evdev_snapshot(input, masks));
	assert(masks[0] == 0);
	mock_read_fd = -1;
	mock_enabled = false;
	input_evdev_close(input);
	assert(rmdir(directory) == 0);
}

static void test_directory_appearance(void)
{
	char parent[] = "/tmp/banana-retro-parent.XXXXXX";
	assert(mkdtemp(parent) != NULL);
	char path[256];
	assert(snprintf(path, sizeof(path), "%s/input", parent) > 0);
	struct input_evdev *input = input_evdev_open(path);
	assert(input != NULL);
	assert(mkdir(path, 0700) == 0);
	assert(input_evdev_wait(input, 100) > 0);
	assert(input_evdev_poll(input));
	assert(rmdir(path) == 0);
	assert(input_evdev_wait(input, 100) > 0);
	assert(input_evdev_poll(input));
	input_evdev_close(input);
	assert(rmdir(parent) == 0);
}

int main(void)
{
	test_profiles_and_pad();
	test_resync();
	test_syn_dropped_and_loss();
	test_clock_rate(50.0);
	test_clock_rate(59.94);
	test_clock_rate(60.0);
	test_overrun_and_rate_change();
	test_paced_wait_results();
	test_empty_source_blocks();
	test_hotplug_lifecycle();
	test_directory_appearance();
	puts("banana-retro input and timing checks passed");
	return 0;
}
