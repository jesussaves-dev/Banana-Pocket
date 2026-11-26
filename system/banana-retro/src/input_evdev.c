#define _DEFAULT_SOURCE
#include "input_evdev.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum { MAX_EVENTS = 32, EVENT_BYTES = 64 * 1024 };
enum { MAX_DEVICE_BATCHES = 8 };

struct input_device {
	int fd;
	bool active;
	bool dropped;
	uint16_t suppressed;
	enum input_profile_kind profile;
	unsigned hat_axes;
	char path[PATH_MAX];
	struct input_pad_state pad;
};

struct input_evdev {
	int epoll_fd;
	int notify_fd;
	int directory_watch;
	int parent_watch;
	char directory[PATH_MAX];
	char parent[PATH_MAX];
	char directory_name[NAME_MAX + 1];
	struct input_device devices[RETRO_INPUT_MAX_DEVICES];
};

static const struct {
	unsigned code;
	unsigned id;
} generic_key_map[] = {
	{ BTN_SOUTH, 0 }, { BTN_EAST, 8 }, { BTN_NORTH, 9 }, { BTN_WEST, 1 },
	{ BTN_TL, 10 }, { BTN_TR, 11 }, { BTN_TL2, 12 }, { BTN_TR2, 13 },
	{ BTN_SELECT, 2 }, { BTN_START, 3 }, { BTN_THUMBL, 14 }, { BTN_THUMBR, 15 },
	{ BTN_DPAD_UP, 4 }, { BTN_DPAD_DOWN, 5 },
	{ BTN_DPAD_LEFT, 6 }, { BTN_DPAD_RIGHT, 7 }
};

static const size_t key_map_count = sizeof(generic_key_map) / sizeof(generic_key_map[0]);

static bool bit_test(const unsigned long *bits, unsigned bit)
{
	return (bits[bit / (sizeof(*bits) * CHAR_BIT)] &
		(1ul << (bit % (sizeof(*bits) * CHAR_BIT)))) != 0;
}

enum input_profile_kind input_profile_select(uint16_t vendor, uint16_t product,
					     const char *name)
{
	return vendor == 12068 && product == 1 && name &&
	       strcmp(name, "Retroid Pocket Classic Gamepad") == 0 ?
	       INPUT_PROFILE_RPC : INPUT_PROFILE_GENERIC;
}

void input_pad_event_profile(struct input_pad_state *s, enum input_profile_kind profile,
			     unsigned hat_axes, unsigned type, unsigned code, int value)
{
	if (!s)
		return;
	if (type == EV_KEY && (value == 0 || value == 1 || value == 2)) {
		/* RPC config and generic Linux positional mappings agree for these controls. */
		for (size_t i = 0; i < key_map_count; ++i) {
			if (generic_key_map[i].code == code) {
				uint16_t bit = (uint16_t)(1u << generic_key_map[i].id);
				if (value != 0) s->buttons |= bit;
				else s->buttons &= (uint16_t)~bit;
				break;
			}
		}
	}
	else if (profile == INPUT_PROFILE_GENERIC && (hat_axes & INPUT_HAT_X) &&
		 type == EV_ABS && code == ABS_HAT0X) {
		s->hat_x = value < 0 ? -1 : value > 0 ? 1 : 0;
	} else if (profile == INPUT_PROFILE_GENERIC && (hat_axes & INPUT_HAT_Y) &&
		   type == EV_ABS && code == ABS_HAT0Y) {
		s->hat_y = value < 0 ? -1 : value > 0 ? 1 : 0;
	}
}

bool input_pad_feed_event(struct input_pad_state *s, bool *dropped,
			 enum input_profile_kind profile, unsigned hat_axes,
			 unsigned type, unsigned code,
			 int value, input_pad_query_fn query, void *data)
{
	if (!s || !dropped) return false;
	if (type == EV_SYN && code == SYN_DROPPED) {
		*dropped = true;
		memset(s, 0, sizeof(*s));
		return true;
	}
	if (type == EV_SYN && code == SYN_REPORT && *dropped) {
		if (!query || !query(data, s)) return false;
		*dropped = false;
		return true;
	}
	if (*dropped || type == EV_SYN) return true;
	input_pad_event_profile(s, profile, hat_axes, type, code, value);
	return true;
}

void input_pad_device_lost(struct input_pad_state *s, bool *dropped)
{
	if (s) memset(s, 0, sizeof(*s));
	if (dropped) *dropped = false;
}

void input_pad_resync(struct input_pad_state *s, const unsigned long *keys,
		      const int8_t hat[2], unsigned key_words,
		      enum input_profile_kind profile, unsigned hat_axes)
{
	if (!s || !keys || !hat)
		return;
	s->buttons = 0;
	for (size_t i = 0; i < key_map_count; ++i) {
		unsigned code = generic_key_map[i].code;
		unsigned word = code / (sizeof(*keys) * CHAR_BIT);
		if (word < key_words && bit_test(keys, code))
			s->buttons |= (uint16_t)(1u << generic_key_map[i].id);
	}
	s->hat_x = profile == INPUT_PROFILE_GENERIC && (hat_axes & INPUT_HAT_X) ?
		(hat[0] < 0 ? -1 : hat[0] > 0 ? 1 : 0) : 0;
	s->hat_y = profile == INPUT_PROFILE_GENERIC && (hat_axes & INPUT_HAT_Y) ?
		(hat[1] < 0 ? -1 : hat[1] > 0 ? 1 : 0) : 0;
}

uint16_t input_pad_mask(const struct input_pad_state *s)
{
	uint16_t mask;
	if (!s)
		return 0;
	mask = s->buttons;
	if (s->hat_y < 0) mask |= 1u << 4;
	if (s->hat_y > 0) mask |= 1u << 5;
	if (s->hat_x < 0) mask |= 1u << 6;
	if (s->hat_x > 0) mask |= 1u << 7;
	return mask;
}

static bool read_caps(int fd, unsigned long *keys, unsigned long *abs)
{
	memset(keys, 0, ((KEY_MAX + 1 + CHAR_BIT * sizeof(*keys) - 1) /
			 (CHAR_BIT * sizeof(*keys))) * sizeof(*keys));
	memset(abs, 0, ((ABS_MAX + 1 + CHAR_BIT * sizeof(*abs) - 1) /
			(CHAR_BIT * sizeof(*abs))) * sizeof(*abs));
	return ioctl(fd, EVIOCGBIT(EV_KEY, ((KEY_MAX + 1 + CHAR_BIT * sizeof(*keys) - 1) /
						 (CHAR_BIT * sizeof(*keys))) * sizeof(*keys)), keys) >= 0 &&
	       ioctl(fd, EVIOCGBIT(EV_ABS, ((ABS_MAX + 1 + CHAR_BIT * sizeof(*abs) - 1) /
						(CHAR_BIT * sizeof(*abs))) * sizeof(*abs)), abs) >= 0;
}

static bool query_device_state(struct input_device *dev, struct input_pad_state *pad)
{
	unsigned long keys[(KEY_MAX + sizeof(unsigned long) * CHAR_BIT) /
			   (sizeof(unsigned long) * CHAR_BIT)];
	int8_t hat[2] = {0, 0};
	struct input_absinfo ai;
	memset(keys, 0, sizeof(keys));
	if (ioctl(dev->fd, EVIOCGKEY(sizeof(keys)), keys) < 0) return false;
	if ((dev->hat_axes & INPUT_HAT_X) && ioctl(dev->fd, EVIOCGABS(ABS_HAT0X), &ai) == 0)
		hat[0] = ai.value < 0 ? -1 : ai.value > 0 ? 1 : 0;
	if ((dev->hat_axes & INPUT_HAT_Y) && ioctl(dev->fd, EVIOCGABS(ABS_HAT0Y), &ai) == 0)
		hat[1] = ai.value < 0 ? -1 : ai.value > 0 ? 1 : 0;
	input_pad_resync(pad, keys, hat, (unsigned)(sizeof(keys) / sizeof(keys[0])),
			 dev->profile, dev->hat_axes);
	return true;
}

static bool query_device_cb(void *data, struct input_pad_state *pad)
{
	return query_device_state(data, pad);
}

static bool open_device(struct input_evdev *in, const char *path)
{
	unsigned long keys[(KEY_MAX + sizeof(unsigned long) * CHAR_BIT) /
			   (sizeof(unsigned long) * CHAR_BIT)];
	unsigned long abs[(ABS_MAX + sizeof(unsigned long) * CHAR_BIT) /
			  (sizeof(unsigned long) * CHAR_BIT)];
	struct input_device *dev = NULL;
	struct input_id id;
	struct epoll_event event;
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return false;
	if (ioctl(fd, EVIOCGID, &id) < 0 || !read_caps(fd, keys, abs)) {
		close(fd);
		return false;
	}
	char name[128] = {0};
	if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
		close(fd);
		return false;
	}
	name[sizeof(name) - 1] = '\0';
	bool rpc = input_profile_select(id.vendor, id.product, name) == INPUT_PROFILE_RPC;
	bool gamepad = bit_test(keys, BTN_GAMEPAD) || bit_test(keys, BTN_SOUTH) ||
		       bit_test(keys, BTN_A);
	bool key_dpad = bit_test(keys, BTN_DPAD_UP) && bit_test(keys, BTN_DPAD_DOWN) &&
		bit_test(keys, BTN_DPAD_LEFT) && bit_test(keys, BTN_DPAD_RIGHT);
	unsigned hat_axes = (bit_test(abs, ABS_HAT0X) ? INPUT_HAT_X : 0) |
		(bit_test(abs, ABS_HAT0Y) ? INPUT_HAT_Y : 0);
	bool hat_dpad = hat_axes == (INPUT_HAT_X | INPUT_HAT_Y);
	bool rpc_controls = bit_test(keys, BTN_SOUTH) && bit_test(keys, BTN_EAST) &&
		bit_test(keys, BTN_NORTH) && bit_test(keys, BTN_WEST) &&
		bit_test(keys, BTN_SELECT) && bit_test(keys, BTN_START) && key_dpad;
	if ((rpc && !rpc_controls) || (!rpc && (!gamepad || (!hat_dpad && !key_dpad)))) {
		close(fd);
		return false;
	}
	for (size_t i = 0; i < RETRO_INPUT_MAX_DEVICES; ++i)
		if (!in->devices[i].active) { dev = &in->devices[i]; break; }
	if (!dev) { close(fd); return false; }
	memset(dev, 0, sizeof(*dev));
	dev->fd = fd;
	dev->active = true;
	dev->profile = rpc ? INPUT_PROFILE_RPC : INPUT_PROFILE_GENERIC;
	dev->hat_axes = rpc ? 0 : hat_axes;
	if (snprintf(dev->path, sizeof(dev->path), "%s", path) >= (int)sizeof(dev->path)) {
		close(fd); memset(dev, 0, sizeof(*dev)); return false;
	}
	memset(&event, 0, sizeof(event));
	event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
	event.data.ptr = dev;
	if (epoll_ctl(in->epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
		close(fd); memset(dev, 0, sizeof(*dev)); return false;
	}
	/* Startup key and hat state is sampled before the first core frame. */
	(void)query_device_state(dev, &dev->pad);
	return true;
}

static void remove_device(struct input_evdev *in, struct input_device *dev)
{
	if (!dev->active)
		return;
	(void)epoll_ctl(in->epoll_fd, EPOLL_CTL_DEL, dev->fd, NULL);
	close(dev->fd);
	input_pad_device_lost(&dev->pad, &dev->dropped);
	memset(dev, 0, sizeof(*dev));
}

static void discover(struct input_evdev *in)
{
	DIR *dir = opendir(in->directory);
	if (!dir)
		return;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, "event", 5) != 0)
			continue;
		char path[PATH_MAX];
		if (snprintf(path, sizeof(path), "%s/%s", in->directory, entry->d_name) >=
		    (int)sizeof(path))
			continue;
		bool exists = false;
		for (size_t i = 0; i < RETRO_INPUT_MAX_DEVICES; ++i)
			if (in->devices[i].active && strcmp(in->devices[i].path, path) == 0)
				exists = true;
		if (!exists) (void)open_device(in, path);
	}
	closedir(dir);
}

static bool watch_directory(struct input_evdev *in)
{
	if (in->directory_watch >= 0) return true;
	in->directory_watch = inotify_add_watch(in->notify_fd, in->directory,
		IN_CREATE | IN_ATTRIB | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
	return in->directory_watch >= 0;
}

struct input_evdev *input_evdev_open(const char *directory)
{
	struct input_evdev *in = calloc(1, sizeof(*in));
	if (!in) return NULL;
	for (size_t i = 0; i < RETRO_INPUT_MAX_DEVICES; ++i) in->devices[i].fd = -1;
	in->directory_watch = -1;
	in->parent_watch = -1;
	in->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	in->notify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (in->epoll_fd < 0 || in->notify_fd < 0 ||
	    snprintf(in->directory, sizeof(in->directory), "%s", directory) >=
	    (int)sizeof(in->directory)) {
		input_evdev_close(in); return NULL;
	}
	char parent_copy[PATH_MAX];
	if (snprintf(parent_copy, sizeof(parent_copy), "%s", directory) >= (int)sizeof(parent_copy)) {
		input_evdev_close(in); return NULL;
	}
	char *slash = strrchr(parent_copy, '/');
	if (slash) {
		if (snprintf(in->directory_name, sizeof(in->directory_name), "%s", slash + 1) >=
		    (int)sizeof(in->directory_name)) { input_evdev_close(in); return NULL; }
		if (slash == parent_copy) slash[1] = '\0'; else *slash = '\0';
	} else {
		if (snprintf(in->directory_name, sizeof(in->directory_name), "%s", parent_copy) >=
		    (int)sizeof(in->directory_name)) { input_evdev_close(in); return NULL; }
		strcpy(parent_copy, ".");
	}
	if (snprintf(in->parent, sizeof(in->parent), "%s", parent_copy) >= (int)sizeof(in->parent)) {
		input_evdev_close(in); return NULL;
	}
	in->parent_watch = inotify_add_watch(in->notify_fd, in->parent,
		IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
	if (in->parent_watch < 0 || (!watch_directory(in) && errno != ENOENT)) {
		input_evdev_close(in); return NULL;
	}
	struct epoll_event event = { .events = EPOLLIN, .data.ptr = NULL };
	if (epoll_ctl(in->epoll_fd, EPOLL_CTL_ADD, in->notify_fd, &event) < 0) {
		input_evdev_close(in); return NULL;
	}
	discover(in);
	return in;
}

int input_evdev_wait(struct input_evdev *in, int timeout_ms)
{
	struct epoll_event events[MAX_EVENTS];
	int n;
	do { n = epoll_wait(in->epoll_fd, events, MAX_EVENTS, timeout_ms); }
	while (n < 0 && errno == EINTR);
	return n;
}

int input_evdev_fd(const struct input_evdev *in)
{
	return in ? in->epoll_fd : -1;
}

static void process_notifications(struct input_evdev *in)
{
	char buffer[EVENT_BYTES];
	ssize_t bytes;
	while ((bytes = read(in->notify_fd, buffer, sizeof(buffer))) > 0) {
		for (size_t off = 0; off + sizeof(struct inotify_event) <= (size_t)bytes;) {
			struct inotify_event *ev = (struct inotify_event *)(void *)(buffer + off);
			size_t size = sizeof(*ev) + ev->len;
			if (size > (size_t)bytes - off) break;
			if (ev->mask & IN_Q_OVERFLOW) {
				for (size_t k = 0; k < RETRO_INPUT_MAX_DEVICES; ++k)
					remove_device(in, &in->devices[k]);
				if (in->directory_watch < 0) (void)watch_directory(in);
				discover(in);
			} else if (ev->wd == in->parent_watch && ev->len &&
				   strcmp(ev->name, in->directory_name) == 0) {
				if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
					for (size_t k = 0; k < RETRO_INPUT_MAX_DEVICES; ++k)
						remove_device(in, &in->devices[k]);
					if (in->directory_watch >= 0)
						(void)inotify_rm_watch(in->notify_fd, in->directory_watch);
					in->directory_watch = -1;
				} else if (ev->mask & (IN_CREATE | IN_MOVED_TO)) {
					if (watch_directory(in)) discover(in);
				}
			} else if (ev->wd == in->directory_watch && ev->len &&
				   strncmp(ev->name, "event", 5) == 0) {
				char path[PATH_MAX];
				if (snprintf(path, sizeof(path), "%s/%s", in->directory,
					     ev->name) < (int)sizeof(path)) {
					if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
						for (size_t k = 0; k < RETRO_INPUT_MAX_DEVICES; ++k)
							if (in->devices[k].active &&
							    strcmp(path, in->devices[k].path) == 0)
								remove_device(in, &in->devices[k]);
					} else if (ev->mask & (IN_CREATE | IN_MOVED_TO | IN_ATTRIB)) {
						bool found = false;
						for (size_t k = 0; k < RETRO_INPUT_MAX_DEVICES; ++k)
							if (in->devices[k].active &&
							    strcmp(path, in->devices[k].path) == 0) found = true;
						if (!found) (void)open_device(in, path);
					}
				}
			} else if (ev->wd == in->directory_watch && (ev->mask & IN_IGNORED)) {
				in->directory_watch = -1;
			}
			off += size;
		}
	}
}

bool input_evdev_poll(struct input_evdev *in)
{
	struct epoll_event events[MAX_EVENTS];
	int n;
	do { n = epoll_wait(in->epoll_fd, events, MAX_EVENTS, 0); }
	while (n < 0 && errno == EINTR);
	if (n < 0) return false;
	for (int i = 0; i < n; ++i) {
		if (events[i].data.ptr == NULL)
			continue;
		struct input_device *dev = events[i].data.ptr;
		if (events[i].events & (EPOLLERR | EPOLLHUP)) { remove_device(in, dev); continue; }
		struct input_event batch[64];
		for (unsigned batch_count = 0; batch_count < MAX_DEVICE_BATCHES; ++batch_count) {
			ssize_t bytes = read(dev->fd, batch, sizeof(batch));
			if (bytes < 0 && errno == EINTR) continue;
			if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
			if (bytes <= 0 || bytes % (ssize_t)sizeof(batch[0]) != 0) {
				remove_device(in, dev); break;
			}
			for (size_t j = 0; j < (size_t)bytes / sizeof(batch[0]); ++j) {
				if (!input_pad_feed_event(&dev->pad, &dev->dropped,
					dev->profile, dev->hat_axes,
					batch[j].type, batch[j].code, batch[j].value,
					query_device_cb, dev)) {
					remove_device(in, dev);
					break;
				}
				if (!dev->dropped) dev->suppressed &= input_pad_mask(&dev->pad);
			}
			if (!dev->active) break;
		}
	}
	for (int i = 0; i < n; ++i)
		if (events[i].data.ptr == NULL) process_notifications(in);
	return true;
}

bool input_evdev_resume(struct input_evdev *in)
{
	if (!input_evdev_poll(in)) return false;
	for (unsigned i = 0; i < RETRO_INPUT_MAX_DEVICES; ++i) {
		struct input_device *dev = &in->devices[i];
		if (!dev->active) continue;
		if (!query_device_state(dev, &dev->pad)) {
			remove_device(in, dev);
			continue;
		}
		dev->suppressed = input_pad_mask(&dev->pad);
	}
	return true;
}

uint16_t input_evdev_mask(const struct input_evdev *in, unsigned port)
{
	return in && port < RETRO_INPUT_MAX_DEVICES && in->devices[port].active ?
		input_pad_mask(&in->devices[port].pad) & ~in->devices[port].suppressed : 0;
}

bool input_evdev_snapshot(struct input_evdev *in, uint16_t masks[RETRO_INPUT_MAX_DEVICES])
{
	if (!in || !masks || !input_evdev_poll(in))
		return false;
	for (unsigned port = 0; port < RETRO_INPUT_MAX_DEVICES; ++port)
		masks[port] = input_evdev_mask(in, port);
	return true;
}

void input_evdev_close(struct input_evdev *in)
{
	if (!in) return;
	for (size_t i = 0; i < RETRO_INPUT_MAX_DEVICES; ++i)
		remove_device(in, &in->devices[i]);
	if (in->notify_fd >= 0) close(in->notify_fd);
	if (in->epoll_fd >= 0) close(in->epoll_fd);
	free(in);
}

bool retro_frame_clock_start(struct retro_frame_clock *c, double fps, uint64_t now)
{
	if (!c || !(fps >= 1.0 && fps <= 1000.0)) return false;
	long double scaled = 1000000000.0L / (long double)fps;
	uint64_t whole = (uint64_t)scaled;
	if (!whole) return false;
	*c = (struct retro_frame_clock){ .deadline_ns = now + whole,
		.period_ns = whole, .remainder = (uint64_t)((scaled - whole) * 1000000000.0L) };
	return true;
}

static void next_period(struct retro_frame_clock *c)
{
	c->deadline_ns += c->period_ns;
	c->phase += c->remainder;
	if (c->phase >= 1000000000ull) { ++c->deadline_ns; c->phase -= 1000000000ull; }
}

uint64_t retro_frame_clock_advance(struct retro_frame_clock *c, uint64_t now)
{
	if (!c) return 0;
	next_period(c);
	if (c->deadline_ns <= now) {
		if (c->overruns != UINT64_MAX) ++c->overruns;
		if (++c->catchups > 2) {
			if (c->rebases != UINT64_MAX) ++c->rebases;
			c->deadline_ns = now + c->period_ns;
			c->phase = 0;
			c->catchups = 0;
		}
	} else c->catchups = 0;
	return c->deadline_ns;
}

void retro_frame_stats_record(struct retro_frame_stats *s, uint64_t start_ns,
			      uint64_t deadline_ns, uint64_t period_ns)
{
	static const uint64_t edges[RETRO_FRAME_JITTER_BUCKETS - 1] = {
		250000ull, 500000ull, 1000000ull, 2000000ull, 4000000ull, 8000000ull, 16000000ull };
	if (!s || !start_ns) return;
	++s->frames;
	if (start_ns > deadline_ns) {
		uint64_t late = start_ns - deadline_ns;
		s->late_sum_ns += late;
		++s->late_n;
		if (late > s->late_max_ns) s->late_max_ns = late;
		if (period_ns && late > period_ns / 4) ++s->late_frames;
	}
	if (s->last_start_ns && start_ns >= s->last_start_ns) {
		/* Every interval since the last explicit discontinuity (restart, rate change) is pacing evidence, a long
		 * stall included; only the stall count is kept apart so a hitch stays visible next to the jitter. */
		uint64_t interval = start_ns - s->last_start_ns;
		uint64_t jitter = interval > period_ns ? interval - period_ns : period_ns - interval;
		unsigned bucket = 0;
		while (bucket < RETRO_FRAME_JITTER_BUCKETS - 1 && jitter >= edges[bucket]) ++bucket;
		++s->jitter[bucket];
		if (period_ns && interval > RETRO_FRAME_STALL_PERIODS * period_ns) ++s->stalls;
		if (!s->intervals || interval < s->interval_min_ns) s->interval_min_ns = interval;
		if (interval > s->interval_max_ns) s->interval_max_ns = interval;
		s->interval_sum_ns += interval;
		++s->intervals;
	}
	s->last_start_ns = start_ns;
}

int retro_frame_stats_format(const struct retro_paced_runner *r, char *buffer, size_t size)
{
	if (!r || !buffer || !size) return -1;
	const struct retro_frame_stats *s = &r->stats;
	unsigned long long mean = s->intervals ? s->interval_sum_ns / s->intervals : 0;
	unsigned long long late_mean = s->late_n ? s->late_sum_ns / s->late_n : 0;
	int n = snprintf(buffer, size,
		"Frame telemetry: frames=%llu intervals=%llu restarts=%llu rate_changes=%llu stalls_gt8p=%llu "
		"period_ns=%llu interval_min_ns=%llu interval_mean_ns=%llu interval_max_ns=%llu "
		"late_frames=%llu late_started=%llu late_mean_ns=%llu late_max_ns=%llu "
		"missed_deadlines=%llu rebases=%llu "
		"jitter_lt250us=%llu jitter_lt500us=%llu jitter_lt1ms=%llu jitter_lt2ms=%llu "
		"jitter_lt4ms=%llu jitter_lt8ms=%llu jitter_lt16ms=%llu jitter_ge16ms=%llu",
		(unsigned long long)s->frames, (unsigned long long)s->intervals,
		(unsigned long long)s->restarts, (unsigned long long)s->rate_changes,
		(unsigned long long)s->stalls, (unsigned long long)r->clock.period_ns,
		(unsigned long long)s->interval_min_ns, mean, (unsigned long long)s->interval_max_ns,
		(unsigned long long)s->late_frames, (unsigned long long)s->late_n, late_mean, (unsigned long long)s->late_max_ns,
		(unsigned long long)r->clock.overruns, (unsigned long long)r->clock.rebases,
		(unsigned long long)s->jitter[0], (unsigned long long)s->jitter[1],
		(unsigned long long)s->jitter[2], (unsigned long long)s->jitter[3],
		(unsigned long long)s->jitter[4], (unsigned long long)s->jitter[5],
		(unsigned long long)s->jitter[6], (unsigned long long)s->jitter[7]);
	return n < 0 || (size_t)n >= size ? -1 : n;
}

bool retro_paced_start(struct retro_paced_runner *runner, double fps, uint64_t now)
{
	if (!runner || !retro_frame_clock_start(&runner->clock, fps, now)) return false;
	runner->fps = fps;
	runner->telemetry = false;
	runner->stats = (struct retro_frame_stats){ 0 };
	return true;
}

bool retro_paced_restart(struct retro_paced_runner *runner, double fps, uint64_t now)
{
	if (!runner) return false;
	/* Telemetry off: the resume is exactly the original start, counters and all. */
	if (!runner->telemetry) return retro_paced_start(runner, fps, now);
	struct retro_frame_stats stats = runner->stats;
	uint64_t overruns = runner->clock.overruns, rebases = runner->clock.rebases;
	if (!retro_paced_start(runner, fps, now)) return false;
	/* The deadline and rebase counters are cumulative for the run, like the frame totals beside them. */
	runner->clock.overruns = overruns;
	runner->clock.rebases = rebases;
	stats.last_start_ns = 0;
	++stats.restarts;
	runner->stats = stats;
	runner->telemetry = true;
	return true;
}

enum retro_paced_result retro_paced_run_one(struct retro_paced_runner *runner,
						     const struct retro_paced_ops *ops,
						     unsigned frame)
{
	if (!runner || !ops || !ops->now_ns || !ops->wait_until || !ops->run_frame ||
	    !ops->refresh_rate)
		return RETRO_PACED_FAILED;
	int waited = ops->wait_until(ops->data, runner->clock.deadline_ns);
	if (waited == 0) return RETRO_PACED_STOPPED;
	if (waited < 0) return RETRO_PACED_FAILED;
	if (runner->telemetry)
		retro_frame_stats_record(&runner->stats, ops->now_ns(ops->data),
					 runner->clock.deadline_ns, runner->clock.period_ns);
	if (!ops->run_frame(ops->data, frame)) return RETRO_PACED_FAILED;
	uint64_t now = ops->now_ns(ops->data);
	if (!now) return RETRO_PACED_FAILED;
	double fps = ops->refresh_rate(ops->data);
	if (fps != runner->fps) {
		uint64_t overruns = runner->clock.overruns;
		uint64_t rebases = runner->clock.rebases;
		if (!retro_frame_clock_start(&runner->clock, fps, now)) return RETRO_PACED_FAILED;
		runner->clock.overruns = overruns;
		runner->clock.rebases = rebases;
		runner->fps = fps;
		runner->stats.last_start_ns = 0;
		if (runner->telemetry) ++runner->stats.rate_changes;
	} else {
		(void)retro_frame_clock_advance(&runner->clock, now);
	}
	return RETRO_PACED_FRAME;
}
