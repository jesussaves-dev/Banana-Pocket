/* bananad: Banana's power daemon for the Retroid Pocket Classic (SM4450).
 *
 *   - Sleep: a short press of the power button, or (in the menu) a few idle minutes put the handheld into
 *     suspend-to-RAM. The running game is paused first (SIGSTOP to its process group) and resumed after, the hooks
 *     in /etc/banana/sleep.d run around it, and banana-ui is told to draw its panel again. Pressing power wakes it
 *     (the device tree marks it a wakeup source).
 *   - A long press of the power button shuts down cleanly.
 *   - Volume keys change the mixer; F (BTN_MODE) + volume changes the backlight.
 *   - A battery about to run out shuts the handheld down before the data partition can be cut off mid-write.
 *   - The charger: bananad watches the power supplies and pulses the vibrator when the cable goes in. The handheld
 *     has no LEDs, so there is nothing else to drive.
 *   - Brightness and volume are kept across reboots (/data/banana/backlight, /data/banana/volume).
 *   - Sound is the server's (PipeWire + WirePlumber, brought up by rcS's sound-server): ACP loads the RPC's UCM2
 *     profile and owns the DSP's routing and levels. bananad keeps ALSA's default device pointed at it
 *     (/run/banana/asound.conf, which /etc/asound.conf reads) and changes the amplifier's level for the volume keys.
 *
 * Settings: /etc/banana/bananad.conf, then /data/banana/bananad.conf (key = value lines, see bananad.conf); SIGHUP
 * reads them again (the menu's settings screen sends it).
 * It reads the input devices alongside banana-ui and the games (evdev allows many readers) and grabs nothing.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/un.h>

#include "banana-retro-control.h"

#define MAXDEV 32

/* usleep is obsolete in POSIX.1-2008; nanosleep is its replacement */
static void sleep_us(long us)
{
	struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
	while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

static struct {
	char mixer[64];			/* ALSA simple control the volume keys change; "auto": the playing card's own */
	int volume_step;		/* percent */
	int brightness_step;	/* percent of max */
	int autosleep_min;		/* idle minutes in the menu before sleeping; 0 = never */
	int power_long_ms;		/* holding power this long shuts down */
	int battery_off_pct;	/* at or under this, discharging: shut down */
	char mem_sleep[16];		/* deep (suspend to RAM) or s2idle */
	double sleep_check_hours;	/* asleep: wake this often to look at the battery (0: never) */
	int sleep_poweroff_pct;	/* found at or under this while asleep: shut down cleanly */
	int sleep_floor_pct;		/* discharging at or under this: refuse a sleep when no RTC alarm can wake it */
	double dim_min;			/* idle minutes in the menu before the panel dims (0: never) */
	int dim_pct;			/* dimmed to this percent of their brightness */
	int vibrate_ms;			/* the plug-in pulse length; the motor's drive is already the driver's maximum */
	int wake_audio_fast;		/* the sound server stays up through a sleep: sound at once on a wake, more standby */
	char sleep_skip[64];		/* debug: comma list of suspend steps to skip (stop, ui, mute, hooks, sync) */
	int autosleep_set;		/* autosleep_minutes was written down somewhere: it is asked for, not defaulted */
	int hud;			/* the bridge: text inside a game, and the unplug pause */
	char lang[8];			/* the bridge's language, from settings.conf (the menu's own choice) */
	int session_secs;		/* a running game's state and resume marker are refreshed this often (0: 180) */
} cfg = { "auto", 5, 8, 10, 2000, 3, "deep", 4, 5, 15, 2, 30, 600, 0, "", 0, 1, "en", 0 };

/* a suspend step named in sleep_skip: only ever set by hand (banana/bananad.conf), to bisect a sleep that comes straight
 * back on the handheld without rebuilding bananad */
static int skip(const char *what) { return cfg.sleep_skip[0] && strstr(cfg.sleep_skip, what) != NULL; }

static int devfd[MAXDEV], ndev;
static unsigned char devdrop[MAXDEV];	/* SYN_DROPPED seen: events up to the next SYN_REPORT are stale */
static void power_update(void);
static void audio_route(void);
static void jack_settle_begin(void);
static void jack_settle_poll(void);
static void jack_log(void);
static int ra_cmd(const char *cmd);
static int sound_wait(double seconds);
static void audio_retry_begin(void);
static void undim(void);
static void find_panel_bl(void);
static char panel_bl[64];	/* the panel's backlight name; banana-ui's idle dim owns it while it runs */
static int headphones;
static int fn_held, select_held, power_down;
static int panel_blanked;	/* bananad blanked the panel (a suspend it could not do): the next key brings it back */
static double power_since, last_input, ignore_power_until;
static void sleep_quiet_begin(void);	/* the panel dark and the codec muted, before the DSP sleeps */
static void sleep_quiet_end(void);	/* and back, on the way out of it */

static volatile sig_atomic_t reload;
static void on_hup(int sig) { (void)sig; reload = 1; }
static uint64_t retro_request_id;
static uint64_t retro_power_epoch;
static int retro_suspend_prepared;
static int retro_suspend_native;

static double mono(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsyslog(LOG_INFO, fmt, ap);
	va_end(ap);
}

static void trim(char *s)
{
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
	char *b = s;
	while (*b && isspace((unsigned char)*b)) b++;
	memmove(s, b, strlen(b) + 1);
}

/* load_conf <file> <asked>: read the settings. <asked> is 1 for the file on the data partition -- the one a person
 * or the settings screen wrote -- and 0 for the image's own defaults, which are only the starting point. It decides
 * which numbers count as a wish (see the idle rule in main()). */
static void load_conf(const char *path, int asked)
{
	FILE *f = fopen(path, "r");
	if (!f) return;
	char ln[256];
	while (fgets(ln, sizeof ln, f)) {
		char *eq = strchr(ln, '=');
		if (ln[0] == '#' || !eq) continue;
		*eq = 0;
		char *k = ln, *v = eq + 1;
		trim(k); trim(v);
		if (!strcmp(k, "mixer")) snprintf(cfg.mixer, sizeof cfg.mixer, "%s", v);
		else if (!strcmp(k, "volume_step")) cfg.volume_step = atoi(v);
		else if (!strcmp(k, "brightness_step")) cfg.brightness_step = atoi(v);
		else if (!strcmp(k, "autosleep_minutes")) { cfg.autosleep_min = atoi(v); if (asked) cfg.autosleep_set = 1; }
		else if (!strcmp(k, "power_long_ms")) cfg.power_long_ms = atoi(v);
		else if (!strcmp(k, "battery_shutdown_percent")) cfg.battery_off_pct = atoi(v);
		else if (!strcmp(k, "mem_sleep")) snprintf(cfg.mem_sleep, sizeof cfg.mem_sleep, "%s", v);
		else if (!strcmp(k, "sleep_check_hours")) cfg.sleep_check_hours = atof(v);
		else if (!strcmp(k, "sleep_poweroff_percent")) cfg.sleep_poweroff_pct = atoi(v);
		else if (!strcmp(k, "session_secs")) cfg.session_secs = atoi(v);
		else if (!strcmp(k, "sleep_floor_percent")) cfg.sleep_floor_pct = atoi(v);
		else if (!strcmp(k, "dim_minutes")) cfg.dim_min = atof(v);
		else if (!strcmp(k, "dim_percent")) cfg.dim_pct = atoi(v);
		else if (!strcmp(k, "vibrate_ms")) cfg.vibrate_ms = atoi(v);
		else if (!strcmp(k, "wake_audio_fast")) cfg.wake_audio_fast = !strcmp(v, "on") || !strcmp(v, "1");
		else if (!strcmp(k, "sleep_skip")) snprintf(cfg.sleep_skip, sizeof cfg.sleep_skip, "%s", v);
		else if (!strcmp(k, "hud")) cfg.hud = strcmp(v, "off") && strcmp(v, "0") ? 1 : 0;
		else if (!strcmp(k, "lang")) snprintf(cfg.lang, sizeof cfg.lang, "%s", v);
	}
	fclose(f);
}

/* the time zone the settings screen keeps in banana-ui's settings.conf (a POSIX rule), for sleep.log's times */
static void load_timezone(void)
{
	FILE *f = fopen("/data/banana/settings.conf", "r");
	if (!f) return;
	char ln[256];
	while (fgets(ln, sizeof ln, f)) {
		char *eq = strchr(ln, '=');
		if (ln[0] == '#' || !eq) continue;
		*eq = 0;
		char *k = ln, *v = eq + 1;
		trim(k); trim(v);
		if (!strcmp(k, "timezone") && *v) { setenv("TZ", v, 1); tzset(); }
	}
	fclose(f);
}

/* the language banana-ui keeps in its settings.conf, for the bridge's few strings (the menu owns the choice) */
static void load_lang(void)
{
	FILE *f = fopen("/data/banana/settings.conf", "r");
	if (!f) return;
	char ln[256];
	while (fgets(ln, sizeof ln, f)) {
		char *eq = strchr(ln, '=');
		if (ln[0] == '#' || !eq) continue;
		*eq = 0;
		char *k = ln, *v = eq + 1;
		trim(k); trim(v);
		if (!strcmp(k, "lang") && *v) snprintf(cfg.lang, sizeof cfg.lang, "%s", v);
	}
	fclose(f);
}

static int write_file(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);	/* as a shell's > does (sysfs takes it) */
	if (fd < 0) return -1;
	ssize_t n = write(fd, s, strlen(s));
	int e = errno;
	close(fd);
	if (n != (ssize_t)strlen(s)) { errno = n < 0 ? e : EIO; return -1; }
	errno = e;
	return 0;
}

static int read_int(const char *path, int def)
{
	FILE *f = fopen(path, "r");
	if (!f) return def;
	int v = def;
	if (fscanf(f, "%d", &v) != 1) v = def;
	fclose(f);
	return v;
}

static void read_str(const char *path, char *out, size_t n)
{
	out[0] = 0;
	FILE *f = fopen(path, "r");
	if (!f) return;
	if (fgets(out, (int)n, f)) trim(out);
	fclose(f);
}

/* fork + exec, no shell; waits */
static int run(char *const argv[])
{
	pid_t p = fork();
	if (p == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	if (p < 0) return -1;
	int st = 0;
	while (waitpid(p, &st, 0) < 0)
		if (errno != EINTR) return -1;
	if (WIFEXITED(st)) return WEXITSTATUS(st);
	if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
	return -1;
}

/* every executable in /etc/banana/sleep.d, in name order, with "pre" or "post"; background: not waited for (after a
 * wake, the game and the menu come first and Wi-Fi catches up) */
static void hooks_run(const char *when);
static void sleep_log(const char *fmt, ...);
static void hooks(const char *when, int background)
{
	pid_t p = fork();
	if (p == 0) {
		if (!background) hooks_run(when);
		else if (fork() == 0) { setsid(); hooks_run(when); _exit(0); }
		_exit(0);
	}
	if (p < 0 && !background) sleep_log("cannot start pre-suspend hooks: %s", strerror(errno));
	if (p > 0) while (waitpid(p, NULL, 0) < 0 && errno == EINTR) {}
}

static void pre_hook(char *const argv[])
{
	const double timeout = 10.0;
	double deadline = mono() + timeout;
	pid_t p = fork();
	if (p == 0) {
		if (setpgid(0, 0) < 0) _exit(126);
		execvp(argv[0], argv);
		_exit(127);
	}
	if (p < 0) {
		sleep_log("pre-suspend hook %s: fork failed: %s", argv[0], strerror(errno));
		return;
	}

	int st;
	for (;;) {
		pid_t waited = waitpid(p, &st, WNOHANG);
		if (waited == p) {
			if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
				sleep_log("pre-suspend hook %s: exited with status %d", argv[0], WEXITSTATUS(st));
			else if (WIFSIGNALED(st))
				sleep_log("pre-suspend hook %s: killed by signal %d", argv[0], WTERMSIG(st));
			return;
		}
		if (waited < 0 && errno != EINTR) {
			sleep_log("pre-suspend hook %s: waitpid failed: %s", argv[0], strerror(errno));
			return;
		}
		if (mono() >= deadline) break;
		sleep_us(20000);
	}
	/* The hook can be stuck in the kernel even after SIGKILL. Let init reap it when the pre-hook runner exits. */
	if (kill(-p, SIGKILL) < 0 && kill(p, SIGKILL) < 0 && errno != ESRCH)
		sleep_log("pre-suspend hook %s: SIGKILL failed: %s", argv[0], strerror(errno));
	sleep_log("pre-suspend hook %s: timed out after %.0f s, continuing suspend", argv[0], timeout);
}

static void hooks_run(const char *when)
{
	struct dirent **list;
	int n = scandir("/etc/banana/sleep.d", &list, NULL, alphasort);
	if (n < 0) return;
	for (int i = 0; i < n; i++) {
		char path[300];
		snprintf(path, sizeof path, "/etc/banana/sleep.d/%s", list[i]->d_name);
		struct stat st;
		if (list[i]->d_name[0] != '.' && !stat(path, &st) && S_ISREG(st.st_mode) && (st.st_mode & 0111)) {
			char *argv[] = { path, (char *)when, NULL };
			if (!strcmp(when, "pre")) pre_hook(argv);
			else run(argv);
		}
		free(list[i]);
	}
	free(list);
}

/* The pid file names a process, but a pid is reused: after a launcher is killed (or a game SIGKILLed) its file stays
 * with a pid that, later in the same boot, can belong to someone else -- and suspend SIGSTOPs that process's whole
 * group, shutdown SIGTERMs it and a wake SIGUSR1s it. So the file only counts when the process did not start *after*
 * the file was written: a reused pid always did. The process's age comes from /proc/uptime (monotonic); the file's is
 * a wall-clock difference, which a step of the clock (ntpd at Wi-Fi, hwclock) inflates -- and an inflated age reads
 * as a reused pid, so a running game would look gone. A pidfile cannot be older than the boot, so a file age beyond
 * the uptime means the clock moved, not the file: it is read as "written at boot", which still refuses a process
 * that began well after it. (Reading it as zero would let a stale file whose pid was reused pass for the whole
 * boot.) The 3 s slack covers the write/exec gap and the file's coarse timestamp. */
static unsigned long long proc_start_ticks(int pid)
{
	char path[64], buf[512];
	snprintf(path, sizeof path, "/proc/%d/stat", pid);
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	unsigned long long start = 0;
	if (fgets(buf, sizeof buf, f)) {
		char *rp = strrchr(buf, ')');	/* the command name can hold spaces and parentheses */
		/* after ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime cutime cstime
		 * priority nice threads itrealvalue starttime (field 22) */
		if (rp && sscanf(rp + 2, "%*s %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu", &start) != 1)
			start = 0;
	}
	fclose(f);
	return start;
}

static pid_t pid_of(const char *pidfile)
{
	int p = read_int(pidfile, 0);
	if (p <= 0 || kill(p, 0) != 0) return 0;
	struct stat st;
	unsigned long long start = proc_start_ticks(p);
	long hz = sysconf(_SC_CLK_TCK);
	if (start && hz > 0 && !stat(pidfile, &st)) {
		double up = 0;
		char b[32];
		read_str("/proc/uptime", b, sizeof b);
		if (sscanf(b, "%lf", &up) != 1) up = 0;
		double age = up - (double)start / (double)hz;	/* how long the process has run, seconds */
		double file = (double)time(NULL) - (double)st.st_mtime;	/* how long ago the file was written */
		if (up > 0 && file > up + 5.0) file = up;	/* older than the boot: the clock stepped, so it says nothing */
		if (up > 0 && age < file - 3.0) return 0;	/* it began after the file named it: a reused pid */
	}
	return p;
}

enum retro_power_result { RETRO_POWER_NONE, RETRO_POWER_OK, RETRO_POWER_FAILED, RETRO_POWER_BAD_STATE };

struct retro_resume_proof {
	char system[64];
	char core[64];
	char token[BANANA_RETRO_RESUME_TOKEN_SIZE + 1];
	/* A CHECKPOINT answered OK with no digest: the runtime committed battery data only, on purpose.
	 * A malformed reply or a transport failure never sets this. */
	int battery_only;
};

struct retro_session {
	pid_t child;
	unsigned char id[16];
	struct sockaddr_un address;
	char core[64];
	int rom_matches;
};

/* The UI's registration of the running game, "v1 <session> <leader pid> <start ticks> <rom>". It is written whole
 * before game.pid and removed after the child is reaped, so a daemon that captured it and still finds the same
 * leader alive knows which session it is acting for. Resume ownership is this session, never the ROM path. */
struct retro_owner {
	char session[33];
	pid_t pid;
	unsigned long long start;
	char rom[PATH_MAX];
};

/* LIVE: the session must still be running. SHUTDOWN: the captured session is being stopped by a shutdown that
 * holds /run/banana/shutting-down; its leader has gone by the time the record is written. */
enum retro_publish_mode { RETRO_PUBLISH_LIVE, RETRO_PUBLISH_SHUTDOWN };

static const char retro_run_dir[] = "/run/banana";
static const char retro_resume_dir[] = "/data/banana";

static int retro_hex(const char *s, size_t size)
{
	for (size_t i = 0; i < size; ++i)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
	return 1;
}

static int retro_record_id(const char *id)
{
	size_t n = strnlen(id, 64);
	return n > 0 && n < 64 && strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789_-") == n;
}

static int retro_rom_path(const char *rom)
{
	return rom && rom[0] == '/' && strnlen(rom, PATH_MAX) < PATH_MAX && !strpbrk(rom, "\r\n");
}

/* These run files contain one complete line. Do not trim a ROM filename or
 * accept a prefix of an invalid record. Only ENOENT means no record. */
static int retro_read_line(const char *path, char *line, size_t capacity)
{
	line[0] = '\0';
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) return errno == ENOENT ? RETRO_POWER_NONE : RETRO_POWER_FAILED;
	struct stat st;
	int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
	size_t size = 0;
	while (ok && size < capacity) {
		ssize_t got = read(fd, line + size, capacity - size);
		if (got > 0) size += (size_t)got;
		else if (got == 0) break;
		else if (errno != EINTR) ok = 0;
	}
	close(fd);
	if (!ok || size == 0 || size == capacity || line[size - 1] != '\n' ||
	    memchr(line, '\0', size) || memchr(line, '\r', size) || memchr(line, '\n', size - 1)) {
		line[0] = '\0';
		return RETRO_POWER_FAILED;
	}
	line[size - 1] = '\0';
	return RETRO_POWER_OK;
}

static unsigned long long retro_number(const char *s, unsigned long long max)
{
	if (!*s || s[strspn(s, "0123456789")] != '\0') return 0;
	errno = 0;
	unsigned long long n = strtoull(s, NULL, 10);
	return !errno && n <= max ? n : 0;
}

static void retro_put16(unsigned char *p, uint16_t v)
{ p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void retro_put64(unsigned char *p, uint64_t v)
{ for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (i * 8)); }
static uint16_t retro_get16(const unsigned char *p)
{ return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8); }
static uint64_t retro_get64(const unsigned char *p)
{
	uint64_t v = 0;
	for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8);
	return v;
}

/* A stopped leader (T, t) is still the registered process when its identity checks out: shutdown may meet one that
 * a suspend or a job-control stop left behind, and it must still be able to prove which session it acts for.
 * stopped_ok allows only those two states; a zombie or dead leader never is the session. */
static int retro_child_identity_valid(pid_t child, unsigned long long recorded_start,
				     unsigned long long current_start, pid_t child_group,
				     pid_t game_group, char proc_state, int stopped_ok)
{
	return child > 0 && recorded_start != 0 && current_start == recorded_start &&
		child_group > 0 && child_group == game_group && !strchr("ZXx", proc_state) &&
		(stopped_ok || !strchr("Tt", proc_state));
}

static int retro_read_session(const char *run_dir, pid_t game, const struct retro_owner *owner,
			      struct retro_session *session)
{
	char path[sizeof session->address.sun_path], record[PATH_MAX + 160];
	int len = snprintf(path, sizeof path, "%s/native-session", run_dir);
	if (len < 0 || (size_t)len >= sizeof path) return RETRO_POWER_FAILED;
	int result = retro_read_line(path, record, sizeof record);
	if (result != RETRO_POWER_OK) return result;
	char *fields[7], *rest = record;
	for (size_t i = 0; i < sizeof fields / sizeof *fields; ++i) {
		fields[i] = rest;
		char *space = strchr(rest, ' ');
		if (!space || space == rest) return RETRO_POWER_FAILED;
		*space = '\0';
		rest = space + 1;
	}
	const char *id = fields[1];
	pid_t child = (pid_t)retro_number(fields[2], INT_MAX);
	unsigned long long start = retro_number(fields[3], ULLONG_MAX);
	if (strcmp(fields[0], "v1") || strlen(id) != 32 || !retro_hex(id, 32) ||
	    !retro_record_id(fields[4]) || !retro_number(fields[5], LONG_MAX) ||
	    strcmp(fields[6], "ready") || !retro_rom_path(rest) ||
	    child <= 0 || start == 0 || game <= 0)
		return RETRO_POWER_FAILED;
	char proc_line[512];
	snprintf(path, sizeof path, "/proc/%d/stat", child);
	FILE *f = fopen(path, "r");
	if (!f) return RETRO_POWER_FAILED;
	int alive = fgets(proc_line, sizeof proc_line, f) != NULL;
	fclose(f);
	if (!alive) return RETRO_POWER_FAILED;
	char *state_at = strrchr(proc_line, ')');
	if (!state_at || state_at[1] != ' ' ||
	    !retro_child_identity_valid(child, start, proc_start_ticks(child), getpgid(child),
					getpgid(game), state_at[2], 0)) return RETRO_POWER_FAILED;
	/* Native proof binds to the registered session: a record naming another session is not this game's. */
	if (owner && strcmp(id, owner->session)) return RETRO_POWER_FAILED;
	session->child = child;
	session->rom_matches = owner && strcmp(rest, owner->rom) == 0;
	strcpy(session->core, fields[4]);
	for (size_t i = 0; i < sizeof session->id; ++i) {
		unsigned a = (unsigned)(id[2 * i] <= '9' ? id[2 * i] - '0' : id[2 * i] - 'a' + 10);
		unsigned b = (unsigned)(id[2 * i + 1] <= '9' ? id[2 * i + 1] - '0' : id[2 * i + 1] - 'a' + 10);
		session->id[i] = (unsigned char)((a << 4) | b);
	}
	session->address.sun_family = AF_UNIX;
	len = snprintf(session->address.sun_path, sizeof session->address.sun_path, "%s/retro-%s.sock", run_dir, id);
	return len > 0 && (size_t)len < sizeof session->address.sun_path ? RETRO_POWER_OK : RETRO_POWER_FAILED;
}

static pid_t retro_game_pid(const char *run_dir)
{
	char path[PATH_MAX], line[32];
	int len = snprintf(path, sizeof path, "%s/game.pid", run_dir);
	if (len < 0 || (size_t)len >= sizeof path || retro_read_line(path, line, sizeof line) != RETRO_POWER_OK) return 0;
	return (pid_t)retro_number(line, INT_MAX);
}

/* Parse only: whole line, bounded fields, no liveness. NONE: no registration. FAILED: unreadable or malformed. */
static int retro_read_registration(const char *run_dir, struct retro_owner *owner)
{
	char path[PATH_MAX], record[PATH_MAX + 128];
	memset(owner, 0, sizeof *owner);
	int len = snprintf(path, sizeof path, "%s/game.session", run_dir);
	if (len < 0 || (size_t)len >= sizeof path) return RETRO_POWER_FAILED;
	int result = retro_read_line(path, record, sizeof record);
	if (result != RETRO_POWER_OK) return result;
	char *fields[4], *rest = record;
	for (size_t i = 0; i < sizeof fields / sizeof *fields; ++i) {
		fields[i] = rest;
		char *space = strchr(rest, ' ');
		if (!space || space == rest) return RETRO_POWER_FAILED;
		*space = '\0';
		rest = space + 1;
	}
	pid_t pid = (pid_t)retro_number(fields[2], INT_MAX);
	unsigned long long start = retro_number(fields[3], ULLONG_MAX);
	if (strcmp(fields[0], "v1") || strlen(fields[1]) != 32 || !retro_hex(fields[1], 32) ||
	    pid <= 0 || start == 0 || !retro_rom_path(rest) || strlen(rest) >= sizeof owner->rom)
		return RETRO_POWER_FAILED;
	memcpy(owner->session, fields[1], 33);
	owner->pid = pid;
	owner->start = start;
	strcpy(owner->rom, rest);
	return RETRO_POWER_OK;
}

/* The registered leader is still the process that started: same start ticks, leading its own group, not a zombie
 * and (unless stopped_ok) not stopped, and still the one game.pid names. A reused PID fails on the start ticks. */
static int retro_owner_live(const char *run_dir, const struct retro_owner *owner, int stopped_ok)
{
	char path[64], line[512];
	if (!owner || owner->pid <= 0 || retro_game_pid(run_dir) != owner->pid) return 0;
	snprintf(path, sizeof path, "/proc/%d/stat", (int)owner->pid);
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	int read_ok = fgets(line, sizeof line, f) != NULL;
	fclose(f);
	char *state = read_ok ? strrchr(line, ')') : NULL;
	return state && state[1] == ' ' &&
		retro_child_identity_valid(owner->pid, owner->start, proc_start_ticks(owner->pid),
					   getpgid(owner->pid), owner->pid, state[2], stopped_ok);
}

/* The owner of the game game.pid names right now, or none. Anything missing, malformed or stale fails closed.
 * Only a shutdown capture accepts a stopped leader; every other caller still requires a running one. */
static int retro_capture_owner(const char *run_dir, pid_t game, struct retro_owner *owner, int mode)
{
	if (game <= 0 || retro_read_registration(run_dir, owner) != RETRO_POWER_OK ||
	    owner->pid != game || !retro_owner_live(run_dir, owner, mode == RETRO_PUBLISH_SHUTDOWN)) {
		memset(owner, 0, sizeof *owner);
		return 0;
	}
	return 1;
}

/* Revalidate a captured owner. The caller holds resume.lock, which serializes this check with every other publisher
 * and with the UI's settlements; the UI's retirement of the registration is not under that lock, so the registration
 * and the shutdown latch are both looked at here, after the lock was taken. A shutdown owns the record once the
 * latch exists: only its own publication (SHUTDOWN, which requires the latch) may change it, and a live session's
 * periodic publication or battery-only removal that began before the latch appeared is refused. */
static int retro_owner_current(const char *run_dir, const struct retro_owner *owner, int mode)
{
	struct retro_owner now;
	if (!owner || owner->session[0] == '\0' || retro_read_registration(run_dir, &now) != RETRO_POWER_OK ||
	    strcmp(now.session, owner->session) || now.pid != owner->pid || now.start != owner->start ||
	    strcmp(now.rom, owner->rom)) return 0;
	char latch[PATH_MAX];
	int len = snprintf(latch, sizeof latch, "%s/shutting-down", run_dir);
	if (len <= 0 || (size_t)len >= sizeof latch) return 0;
	const int latched = access(latch, F_OK) == 0;
	if (mode == RETRO_PUBLISH_SHUTDOWN) return latched;
	return !latched && retro_owner_live(run_dir, owner, 0);
}

static int retro_wait_fd(int fd, short events, double deadline)
{
	for (;;) {
		double left = deadline - mono();
		if (left <= 0) { errno = ETIMEDOUT; return 0; }
		int ms = (int)(left * 1000.0);
		if (ms < 1) ms = 1;
		struct pollfd pfd = { fd, events, 0 };
		int rc = poll(&pfd, 1, ms);
		if (rc < 0 && errno == EINTR) continue;
		if (rc <= 0) { if (rc == 0) errno = ETIMEDOUT; return 0; }
		if (pfd.revents & events) return 1;
		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = EPIPE; return 0; }
		return 0;
	}
}

static int retro_send_request(int fd, const unsigned char session[16], uint16_t type,
			      uint64_t id, uint64_t epoch, double deadline,
			      struct retro_resume_proof *proof)
{
	unsigned char packet[BANANA_RETRO_CONTROL_HEADER_SIZE + 2 + BANANA_RETRO_RESUME_TOKEN_SIZE] = {0};
	size_t payload = epoch ? 8 : 0;
	if (proof) {
		proof->token[0] = '\0';
		proof->battery_only = 0;
		packet[BANANA_RETRO_CONTROL_HEADER_SIZE + payload++] = BANANA_RETRO_CTL_RESUME_PROOF;
	}
	memcpy(packet, BANANA_RETRO_CONTROL_MAGIC, 4);
	retro_put16(packet + BANANA_RETRO_CTL_VERSION, BANANA_RETRO_CONTROL_VERSION);
	retro_put16(packet + BANANA_RETRO_CTL_TYPE, type);
	for (unsigned i = 0; i < 8; ++i) packet[BANANA_RETRO_CTL_REQUEST_ID + i] = (unsigned char)(id >> (i * 8));
	memcpy(packet + BANANA_RETRO_CTL_SESSION_ID, session, 16);
	if (epoch) retro_put64(packet + BANANA_RETRO_CONTROL_HEADER_SIZE, epoch);
	retro_put16(packet + BANANA_RETRO_CTL_LENGTH, (uint16_t)payload);
	size_t size = BANANA_RETRO_CONTROL_HEADER_SIZE + payload;
	ssize_t sent;
	for (;;) {
		if (!retro_wait_fd(fd, POLLOUT, deadline)) return -1;
		sent = send(fd, packet, size, MSG_NOSIGNAL);
		if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
		break;
	}
	if (sent != (ssize_t)size) return -1;
	ssize_t got;
	for (;;) {
		if (!retro_wait_fd(fd, POLLIN, deadline)) return -1;
		got = recv(fd, packet, sizeof(packet), MSG_TRUNC);
		if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
		break;
	}
	if (got < (ssize_t)(BANANA_RETRO_CONTROL_HEADER_SIZE + 2) || got > (ssize_t)sizeof packet ||
	    memcmp(packet, BANANA_RETRO_CONTROL_MAGIC, 4) != 0 ||
	    retro_get16(packet + BANANA_RETRO_CTL_VERSION) != BANANA_RETRO_CONTROL_VERSION ||
	    retro_get16(packet + BANANA_RETRO_CTL_TYPE) != (uint16_t)(type | 0x8000u) ||
	    retro_get64(packet + BANANA_RETRO_CTL_REQUEST_ID) != id ||
	    memcmp(packet + BANANA_RETRO_CTL_SESSION_ID, session, 16) != 0 ||
	    retro_get16(packet + BANANA_RETRO_CTL_FLAGS) != 0 ||
	    retro_get16(packet + BANANA_RETRO_CTL_LENGTH) != got - BANANA_RETRO_CONTROL_HEADER_SIZE) return -1;
	uint16_t status = retro_get16(packet + BANANA_RETRO_CONTROL_HEADER_SIZE);
	const size_t body_size = (size_t)got - BANANA_RETRO_CONTROL_HEADER_SIZE - 2;
	if (status > BANANA_RETRO_CTL_STALE_SESSION) return -1;
	if (body_size) {
		const char *token = (const char *)packet + BANANA_RETRO_CONTROL_HEADER_SIZE + 2;
		if (!proof || status != BANANA_RETRO_CTL_OK || body_size != BANANA_RETRO_RESUME_TOKEN_SIZE ||
		    !retro_hex(token, body_size)) return -1;
		memcpy(proof->token, token, body_size);
		proof->token[body_size] = '\0';
	} else if (proof && (type == BANANA_RETRO_CTL_CHECKPOINT || type == BANANA_RETRO_CTL_SHUTDOWN) &&
		   status == BANANA_RETRO_CTL_OK) {
		/* The runtime answered a request that asked for proof with OK and no digest: it committed battery
		 * data on purpose. The compatibility retry in retro_control_request asks for no proof, so its empty OK never lands here. */
		proof->battery_only = 1;
	}
	return status;
}

enum retro_intent { RETRO_INTENT_NONE, RETRO_INTENT_NORMAL, RETRO_INTENT_NATIVE, RETRO_INTENT_INVALID };

/* The UI publishes this before game.pid, and removes it after game.pid. A
 * typed boot stays native even before or after the launcher's own metadata. */
static enum retro_intent retro_read_intent(const char *run_dir, char system[64])
{
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)], line[7 + 64 + 1];
	int len = snprintf(path, sizeof path, "%s/game.native", run_dir);
	if (len < 0 || (size_t)len >= sizeof path) return RETRO_INTENT_INVALID;
	int found = retro_read_line(path, line, sizeof line);
	if (found == RETRO_POWER_NONE) return RETRO_INTENT_NONE;
	if (found != RETRO_POWER_OK) return RETRO_INTENT_INVALID;
	enum retro_intent intent;
	if (!strncmp(line, "native ", 7)) intent = RETRO_INTENT_NATIVE;
	else if (!strncmp(line, "normal ", 7)) intent = RETRO_INTENT_NORMAL;
	else return RETRO_INTENT_INVALID;
	if (!retro_record_id(line + 7)) return RETRO_INTENT_INVALID;
	strcpy(system, line + 7);
	return intent;
}

static int retro_control_request(const char *run_dir, pid_t game, const struct retro_owner *owner,
				 uint16_t type, uint64_t epoch, unsigned timeout_ms,
				 struct retro_resume_proof *proof)
{
	struct retro_session session = {0};
	if (proof) *proof = (struct retro_resume_proof){0};
	int found = retro_read_session(run_dir, game, owner, &session);
	if (proof && found != RETRO_POWER_FAILED) {
		enum retro_intent intent = retro_read_intent(run_dir, proof->system);
		if (found == RETRO_POWER_NONE && (intent == RETRO_INTENT_NATIVE || intent == RETRO_INTENT_INVALID))
			found = RETRO_POWER_FAILED;
	}
	if (found != RETRO_POWER_OK) return proof ? found : RETRO_POWER_NONE;
	double deadline = mono() + (double)timeout_ms / 1000.0;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) return RETRO_POWER_FAILED;
	if (connect(fd, (struct sockaddr *)&session.address, sizeof session.address) < 0) {
		if (errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK) {
			close(fd); return RETRO_POWER_FAILED;
		}
		if (!retro_wait_fd(fd, POLLOUT, deadline)) { close(fd); return RETRO_POWER_FAILED; }
		int error = 0;
		socklen_t error_size = sizeof(error);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) < 0 || error != 0) {
			if (error) errno = error;
			close(fd); return RETRO_POWER_FAILED;
		}
	}
	struct ucred cred;
	socklen_t cred_size = sizeof cred;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_size) < 0 ||
	    cred_size != sizeof cred || cred.pid != session.child || cred.uid != geteuid() ||
	    retro_request_id > UINT64_MAX - (proof ? 3u : 2u)) {
		close(fd);
		return RETRO_POWER_FAILED;
	}
	uint64_t hello_id = ++retro_request_id;
	int ok = retro_send_request(fd, session.id, BANANA_RETRO_CTL_HELLO, hello_id, 0, deadline, NULL);
	if (ok == BANANA_RETRO_CTL_OK) {
		ok = retro_send_request(fd, session.id, type, ++retro_request_id, epoch, deadline, proof);
		if (proof && ok == BANANA_RETRO_CTL_UNSUPPORTED)
			ok = retro_send_request(fd, session.id, type, ++retro_request_id, epoch, deadline, NULL);
	}
	close(fd);
	if (ok == BANANA_RETRO_CTL_OK) {
		if (proof && proof->token[0]) {
			if (session.rom_matches && proof->system[0]) strcpy(proof->core, session.core);
			else proof->token[0] = '\0';
		} else if (proof && proof->battery_only && !(session.rom_matches && proof->system[0])) {
			/* Only the session that owns the boot record's ROM may supersede it. */
			proof->battery_only = 0;
		}
		return RETRO_POWER_OK;
	}
	if (ok == BANANA_RETRO_CTL_BAD_STATE) return RETRO_POWER_BAD_STATE;
	return RETRO_POWER_FAILED;
}

enum retro_stage { RETRO_STAGE_SKIPPED, RETRO_STAGE_OK, RETRO_STAGE_FAILED };

/* The three outcomes of a power or checkpoint transaction, kept apart: the runtime's checkpoint (an
 * enum retro_power_result), the publication of the boot record that names it, and the removal of a record it
 * superseded (both an enum retro_stage). */
struct retro_power_report {
	int checkpoint;
	int publication;
	int cleanup;
};

struct retro_power_ops {
	void *data;
	int (*request)(void *data, uint16_t type, uint64_t epoch, unsigned timeout_ms);
	int (*request_proof)(void *data, pid_t game, const struct retro_owner *owner, uint16_t type, uint64_t epoch,
			     unsigned timeout_ms, struct retro_resume_proof *proof);
	int (*signal_group)(void *data, pid_t pg, int signal_number);
	int (*child_alive)(void *data, pid_t game);
	void (*wait_ms)(void *data, unsigned milliseconds);
	/* Publish the record for the owner's session (proof NULL: a RetroArch record), or remove the record that
	 * session owns. Both revalidate the owner under resume.lock and never touch another session's record. */
	int (*resume_marker)(void *data, const struct retro_owner *owner, int mode,
			     const struct retro_resume_proof *proof);
	int (*resume_invalidate)(void *data, const struct retro_owner *owner, int mode);
	void (*shutdown_finalize)(void *data);
	/* Optional: receives the last transaction's outcomes (production passes none; they are logged). */
	struct retro_power_report *report;
};

static const char *retro_stage_name(int stage)
{ return stage == RETRO_STAGE_OK ? "ok" : stage == RETRO_STAGE_FAILED ? "failed" : "skipped"; }

static const char *retro_checkpoint_name(int result)
{
	return result == RETRO_POWER_OK ? "ok" : result == RETRO_POWER_FAILED ? "failed" :
	       result == RETRO_POWER_BAD_STATE ? "bad state" : "none";
}

/* One line per transaction in which any stage went wrong, so a boot record that was not written or a record that
 * could not be removed is never mistaken for a checkpoint that failed (or the other way around). */
static void retro_report_finish(const struct retro_power_ops *ops, const char *what,
				const struct retro_power_report *rep)
{
	if (ops->report) *ops->report = *rep;
	if (rep->checkpoint == RETRO_POWER_FAILED || rep->checkpoint == RETRO_POWER_BAD_STATE ||
	    rep->publication == RETRO_STAGE_FAILED || rep->cleanup == RETRO_STAGE_FAILED)
		sleep_log("%s: checkpoint %s, boot record %s, cleanup %s", what, retro_checkpoint_name(rep->checkpoint),
			  retro_stage_name(rep->publication), retro_stage_name(rep->cleanup));
}

static int retro_system_request(void *data, uint16_t type, uint64_t epoch, unsigned timeout_ms)
{
	(void)data;
	return retro_control_request(retro_run_dir, pid_of("/run/banana/game.pid"), NULL,
				     type, epoch, timeout_ms, NULL);
}
static int retro_system_request_proof(void *data, pid_t game, const struct retro_owner *owner, uint16_t type,
				      uint64_t epoch, unsigned timeout_ms, struct retro_resume_proof *proof)
{
	(void)data;
	int result = retro_control_request(retro_run_dir, game, owner, type, epoch, timeout_ms, proof);
	if (result == RETRO_POWER_NONE && pid_of("/run/banana/game.pid") != game)
		return RETRO_POWER_FAILED;
	if (type == BANANA_RETRO_CTL_CHECKPOINT && result != RETRO_POWER_NONE) {
		static int battery_only_logged;
		if (result == RETRO_POWER_OK && !proof->token[0] && proof->battery_only) {
			if (!battery_only_logged)
				sleep_log("native periodic checkpoint: battery data only (core cannot checkpoint)");
			battery_only_logged = 1;
		} else if (result != RETRO_POWER_OK || !proof->token[0]) {
			sleep_log("native periodic checkpoint unconfirmed");
		}
	}
	return result;
}
static int retro_system_signal(void *data, pid_t pg, int signal_number)
{ (void)data; return pg > 0 ? kill(-pg, signal_number) : -1; }
static int retro_system_child_alive(void *data, pid_t game)
{ (void)data; return pid_of("/run/banana/game.pid") == game; }
static void retro_system_wait_ms(void *data, unsigned milliseconds)
{ (void)data; sleep_us((long)milliseconds * 1000L); }
static void retro_system_shutdown_finalize(void *data);
/* Publication, attempt reservation and settlement share this lock. Bound
 * contention so a wedged menu cannot hold up sleep or shutdown. */
static int retro_resume_lock(const char *directory)
{
	char path[PATH_MAX];
	int size = snprintf(path, sizeof path, "%s/resume.lock", directory);
	if (size <= 0 || (size_t)size >= sizeof path) return -1;
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) return -1;
	for (int waited = 0; waited < 20; ) {
		if (flock(fd, LOCK_EX | LOCK_NB) == 0) return fd;
		if (errno == EINTR) continue;
		if (errno != EWOULDBLOCK) break;
		sleep_us(2000);
		waited++;
	}
	close(fd);
	return -1;
}

static int retro_legacy_resume_id(char id[BANANA_LEGACY_RESUME_ID_SIZE + 1])
{
	unsigned char bytes[BANANA_LEGACY_RESUME_ID_SIZE / 2];
	ssize_t count = getrandom(bytes, sizeof bytes, GRND_NONBLOCK);
	if (count != (ssize_t)sizeof bytes) return 0;
	static const char hex[] = "0123456789abcdef";
	for (size_t i = 0; i < sizeof bytes; ++i) {
		id[2 * i] = hex[bytes[i] >> 4];
		id[2 * i + 1] = hex[bytes[i] & 15];
	}
	id[BANANA_LEGACY_RESUME_ID_SIZE] = '\0';
	return 1;
}

/* Ownership of the visible record is a two-entry stamp in /run/banana/resume-owner, written under resume.lock
 * before the record is renamed into place:
 *	banana-resume-owner-v1
 *	<previous entry or ->
 *	<candidate entry or ->
 * An entry is "<session|-> <key>", the key naming one publication ("N <digest> <system> <core> <rom>" for a native
 * record, "L <id> - - <rom>" for a RetroArch one). An interruption on either side of the rename leaves the visible
 * record matching exactly one entry, so the on-disk formats need no change. A record with no matching entry, an old
 * record without an ID, or an unreadable stamp is unowned, and nothing may remove an unowned record on a
 * session's behalf. The stamp lives in /run, so it never outlives the sessions it names. */
#define RETRO_KEY_MAX (PATH_MAX + 2 * 64 + BANANA_RETRO_RESUME_TOKEN_SIZE + 16)

/* 1: key written. 0: no record, or none that can be owned (no ID, malformed, oversized, a link). -1: unreadable. */
static int retro_visible_record(int dir, char key[RETRO_KEY_MAX])
{
	char bytes[PATH_MAX + 2 * 64 + BANANA_RETRO_RESUME_TOKEN_SIZE + 64];
	int fd = openat(dir, "resume", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) return errno == ENOENT || errno == ELOOP ? 0 : -1;
	struct stat st;
	size_t size = 0;
	int ok = fstat(fd, &st) == 0;
	if (ok && (!S_ISREG(st.st_mode) || st.st_size >= (off_t)sizeof bytes)) { close(fd); return 0; }
	while (ok && size < sizeof bytes) {
		ssize_t got = read(fd, bytes + size, sizeof bytes - size);
		if (got > 0) size += (size_t)got;
		else if (got == 0) break;
		else if (errno != EINTR) ok = 0;
	}
	if (close(fd) < 0) ok = 0;
	if (!ok) return -1;
	if (size == 0 || size == sizeof bytes || bytes[size - 1] != '\n' || memchr(bytes, '\0', size) ||
	    memchr(bytes, '\r', size)) return 0;
	bytes[size - 1] = '\0';
	char *line[4];
	size_t count = 0;
	char *p = bytes;
	for (;;) {
		if (count == 4) { count = 5; break; }	/* more lines than any record has */
		line[count++] = p;
		char *nl = strchr(p, '\n');
		if (!nl) break;
		*nl = '\0';
		p = nl + 1;
	}
	int len = -1;
	if (count == 4 && !strcmp(line[0], "banana-retro-v1")) {
		char *colon = strchr(line[1], ':');
		if (!colon || !retro_rom_path(line[3])) return 0;
		*colon = '\0';
		if (!retro_record_id(line[1]) || !retro_record_id(colon + 1) ||
		    strlen(line[2]) != BANANA_RETRO_RESUME_TOKEN_SIZE || !retro_hex(line[2], BANANA_RETRO_RESUME_TOKEN_SIZE))
			return 0;
		len = snprintf(key, RETRO_KEY_MAX, "N %s %s %s %s", line[2], line[1], colon + 1, line[3]);
	} else if (count == 2 && retro_rom_path(line[0]) &&
		   !strncmp(line[1], BANANA_LEGACY_RESUME_ID_PREFIX, sizeof(BANANA_LEGACY_RESUME_ID_PREFIX) - 1)) {
		const char *id = line[1] + sizeof(BANANA_LEGACY_RESUME_ID_PREFIX) - 1;
		if (strlen(id) != BANANA_LEGACY_RESUME_ID_SIZE || !retro_hex(id, BANANA_LEGACY_RESUME_ID_SIZE)) return 0;
		len = snprintf(key, RETRO_KEY_MAX, "L %s - - %s", id, line[0]);
	}
	return len > 0 && len < (int)RETRO_KEY_MAX ? 1 : 0;
}

/* A key names one publication: "N <digest> <system> <core> <rom>" or "L <id> - - <rom>". */
static int retro_stamp_key_valid(const char *key)
{
	char id[64];
	if (key[0] == 'L' && key[1] == ' ') {
		const char *p = key + 2;
		return strlen(p) > BANANA_LEGACY_RESUME_ID_SIZE + 5 && retro_hex(p, BANANA_LEGACY_RESUME_ID_SIZE) &&
			!memcmp(p + BANANA_LEGACY_RESUME_ID_SIZE, " - - ", 5) &&
			retro_rom_path(p + BANANA_LEGACY_RESUME_ID_SIZE + 5);
	}
	if (key[0] != 'N' || key[1] != ' ') return 0;
	const char *p = key + 2;
	if (strlen(p) < BANANA_RETRO_RESUME_TOKEN_SIZE + 1 || !retro_hex(p, BANANA_RETRO_RESUME_TOKEN_SIZE) ||
	    p[BANANA_RETRO_RESUME_TOKEN_SIZE] != ' ') return 0;
	p += BANANA_RETRO_RESUME_TOKEN_SIZE + 1;
	for (int field = 0; field < 2; ++field) {	/* system, then core */
		const char *space = strchr(p, ' ');
		if (!space || (size_t)(space - p) >= sizeof id) return 0;
		memcpy(id, p, (size_t)(space - p));
		id[space - p] = '\0';
		if (!retro_record_id(id)) return 0;
		p = space + 1;
	}
	return retro_rom_path(p);
}

/* One stamp entry: "-" (the previous entry only: nothing was visible) or "<session|-> <key>". session is empty for
 * an unowned publication. Anything else is damage. */
static int retro_stamp_entry(const char *entry, int allow_empty, const char **key, char session[33])
{
	session[0] = '\0';
	*key = NULL;
	if (allow_empty && !strcmp(entry, "-")) return 1;
	if (entry[0] == '-' && entry[1] == ' ') *key = entry + 2;
	else if (strlen(entry) >= 34 && entry[32] == ' ' && retro_hex(entry, 32)) {
		memcpy(session, entry, 32);
		session[32] = '\0';
		*key = entry + 33;
	} else return 0;
	return retro_stamp_key_valid(*key);
}

/* The session the stamp binds to this visible record. The whole stamp must be well formed -- exactly the header,
 * the previous entry and the candidate entry -- or the record is unowned: a damaged entry never lets the other one
 * stand in for it. The newest matching entry wins, and two different owners for one key name no owner at all. */
static int retro_stamp_owner(const char *run_dir, const char *key, char session[33])
{
	char path[PATH_MAX], bytes[2 * (RETRO_KEY_MAX + 40) + 32];
	int len = snprintf(path, sizeof path, "%s/resume-owner", run_dir);
	if (len < 0 || (size_t)len >= sizeof path) return 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) return 0;
	size_t size = 0;
	int ok = 1;
	while (ok && size < sizeof bytes) {
		ssize_t got = read(fd, bytes + size, sizeof bytes - size);
		if (got > 0) size += (size_t)got;
		else if (got == 0) break;
		else if (errno != EINTR) ok = 0;
	}
	close(fd);
	if (!ok || size == 0 || size == sizeof bytes || bytes[size - 1] != '\n' || memchr(bytes, '\0', size)) return 0;
	bytes[size - 1] = '\0';
	static const char header[] = "banana-resume-owner-v1";
	char *first = strchr(bytes, '\n'), *second = first ? strchr(first + 1, '\n') : NULL;
	if (!first || !second || strchr(second + 1, '\n') || (size_t)(first - bytes) != sizeof header - 1 ||
	    memcmp(bytes, header, sizeof header - 1))
		return 0;
	*first = '\0';
	*second = '\0';
	const char *previous_key, *candidate_key;
	char previous_session[33], candidate_session[33];
	if (!retro_stamp_entry(first + 1, 1, &previous_key, previous_session) ||
	    !retro_stamp_entry(second + 1, 0, &candidate_key, candidate_session)) return 0;
	if (previous_key && !strcmp(previous_key, candidate_key) && previous_session[0] && candidate_session[0] &&
	    strcmp(previous_session, candidate_session))
		return 0;	/* one key, two owners */
	const char *keys[2] = { candidate_key, previous_key };	/* candidate, then previous */
	const char *owners[2] = { candidate_session, previous_session };
	for (size_t i = 0; i < 2; ++i) {
		if (!keys[i] || strcmp(keys[i], key)) continue;
		if (!owners[i][0]) return 0;	/* published unowned */
		memcpy(session, owners[i], 33);
		return 1;
	}
	return 0;
}

static int retro_write_stamp(const char *run_dir, const char *previous, const char *candidate)
{
	char path[PATH_MAX], tmp[PATH_MAX];
	int a = snprintf(path, sizeof path, "%s/resume-owner", run_dir);
	int b = snprintf(tmp, sizeof tmp, "%s/resume-owner.tmp", run_dir);
	if (a < 0 || (size_t)a >= sizeof path || b < 0 || (size_t)b >= sizeof tmp) return 0;
	(void)unlink(tmp);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) return 0;
	FILE *f = fdopen(fd, "w");
	if (!f) { close(fd); (void)unlink(tmp); return 0; }
	int ok = fprintf(f, "banana-resume-owner-v1\n%s\n%s\n", previous, candidate) > 0 &&
		 fflush(f) == 0 && fsync(fd) == 0;
	if (fclose(f) != 0) ok = 0;
	if (ok) ok = rename(tmp, path) == 0;
	if (!ok) (void)unlink(tmp);
	return ok;
}

static int retro_owner_fields_valid(const struct retro_owner *owner)
{
	return owner && strnlen(owner->session, 33) == 32 && retro_hex(owner->session, 32) &&
		retro_rom_path(owner->rom);
}

/* Unlinks of the visible record whose directory sync has not succeeded, by directory. An unlink that returned 0 is
 * durable only after the sync, so a failed sync leaves a debt that outlives the call: every later invalidation of
 * this directory pays it first, even when the record is by then absent or another session's, and no other success
 * hides it. A publication's own directory sync settles it as well. Process-local: a restarted daemon re-reads the
 * directory, and a record this one unlinked is simply absent. */
#define RETRO_SYNC_DEBT_SLOTS 4
static char retro_sync_debt[RETRO_SYNC_DEBT_SLOTS][PATH_MAX];

static char *retro_sync_debt_find(const char *directory)
{
	for (size_t i = 0; i < RETRO_SYNC_DEBT_SLOTS; ++i)
		if (retro_sync_debt[i][0] && !strcmp(retro_sync_debt[i], directory)) return retro_sync_debt[i];
	return NULL;
}

/* Reserve before the unlink, so the debt can always be recorded once the record is gone: the slot is taken now. */
static int retro_sync_debt_reserve(const char *directory)
{
	if (!directory[0] || strlen(directory) >= PATH_MAX) return 0;
	if (retro_sync_debt_find(directory)) return 1;
	for (size_t i = 0; i < RETRO_SYNC_DEBT_SLOTS; ++i) {
		if (retro_sync_debt[i][0]) continue;
		strcpy(retro_sync_debt[i], directory);
		return 1;
	}
	return 0;
}

/* Sync the directory; a success settles every unlink made in it so far. */
static int retro_dir_sync(int dir, const char *directory)
{
	if (fsync(dir) < 0) return 0;
	char *slot = retro_sync_debt_find(directory);
	if (slot) slot[0] = '\0';
	return 1;
}

/* Publish under resume.lock, with the lock held and the owner already revalidated. */
static int retro_publish_locked(int dir, const char *directory, const char *run_dir, const struct retro_owner *owner,
				const struct retro_resume_proof *proof, const char *legacy_id)
{
	char key[RETRO_KEY_MAX], previous_key[RETRO_KEY_MAX], previous[RETRO_KEY_MAX + 40], candidate[RETRO_KEY_MAX + 40];
	const char *rom = owner->rom;
	int len = proof ? snprintf(key, sizeof key, "N %s %s %s %s", proof->token, proof->system, proof->core, rom)
			: snprintf(key, sizeof key, "L %s - - %s", legacy_id, rom);
	if (len <= 0 || len >= (int)sizeof key) return 0;
	int visible = retro_visible_record(dir, previous_key);
	if (visible < 0) return 0;
	strcpy(previous, "-");
	if (visible == 1) {
		char holder[33];
		if (retro_stamp_owner(run_dir, previous_key, holder)) {
			/* The candidate entry is chosen before the rename, so it would hand this session a record another
			 * one published. Identical bytes are not a reason to change who owns them. */
			if (!strcmp(previous_key, key) && strcmp(holder, owner->session)) {
				sleep_log("resume publication refused: the visible record belongs to session %.8s", holder);
				return 0;
			}
			snprintf(previous, sizeof previous, "%s %s", holder, previous_key);
		} else snprintf(previous, sizeof previous, "- %s", previous_key);
	}
	snprintf(candidate, sizeof candidate, "%s %s", owner->session, key);
	if (!retro_write_stamp(run_dir, previous, candidate)) {
		sleep_log("resume owner stamp failed: %s", strerror(errno));
		return 0;
	}
	/* One daemon writes this file. Discard a temp left by an interrupted
	 * write; exclusive creation avoids following a replacement link. */
	(void)unlinkat(dir, "resume.tmp", 0);
	int ok = 0;
	int fd = openat(dir, "resume.tmp", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
	if (fd >= 0) {
		FILE *f = fdopen(fd, "w");
		if (f) {
			int count = proof ? fprintf(f, "banana-retro-v1\n%s:%s\n%s\n%s\n",
						    proof->system, proof->core, proof->token, rom)
					  : fprintf(f, "%s\n" BANANA_LEGACY_RESUME_ID_PREFIX "%s\n", rom, legacy_id);
			ok = count > 0 && fflush(f) == 0 && fsync(fd) == 0;
			if (fclose(f) != 0) ok = 0;
		} else close(fd);
		if (ok) ok = renameat(dir, "resume.tmp", dir, "resume") == 0;
		if (!ok) (void)unlinkat(dir, "resume.tmp", 0);
	}
	/* No cleanup follows a failure here: the visible record is either still the previous one or already the
	 * candidate, and the stamp names the right session for both. */
	if (ok && !retro_dir_sync(dir, directory)) {
		sleep_log("resume directory sync failed: %s", strerror(errno));
		ok = 0;
	}
	return ok;
}

/* Publish the record for the owner's session. proof NULL writes a RetroArch record with a fresh ID. */
static int retro_write_resume(const char *directory, const char *run_dir, const struct retro_owner *owner,
			      int mode, const struct retro_resume_proof *proof)
{
	if (!retro_owner_fields_valid(owner) ||
	    (proof && (!retro_record_id(proof->system) || !retro_record_id(proof->core) ||
		       proof->token[BANANA_RETRO_RESUME_TOKEN_SIZE] != '\0' ||
		       !retro_hex(proof->token, BANANA_RETRO_RESUME_TOKEN_SIZE)))) return 0;
	char legacy_id[BANANA_LEGACY_RESUME_ID_SIZE + 1] = "";
	if (!proof && !retro_legacy_resume_id(legacy_id)) return 0;
	int dir = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dir < 0) return 0;
	int lock = retro_resume_lock(directory);
	if (lock < 0) { close(dir); return 0; }
	int ok = 0;
	if (retro_owner_current(run_dir, owner, mode)) ok = retro_publish_locked(dir, directory, run_dir, owner, proof, legacy_id);
	else sleep_log("resume publication refused: session %.8s is not the registered game", owner->session);
	close(lock);
	close(dir);
	return ok;
}

/* Remove the visible record only if the stamp binds it to this session. Another session's record, an unowned one
 * and a missing one are left alone and reported as success -- once any earlier unlink's sync is paid: there is
 * nothing of this session's to invalidate, and a replacement is never removed on a retry's behalf. */
static int retro_invalidate_resume(const char *directory, const char *run_dir, const struct retro_owner *owner,
				   int mode)
{
	if (!retro_owner_fields_valid(owner)) return 0;
	int dir = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dir < 0) return 0;
	int lock = retro_resume_lock(directory);
	if (lock < 0) { close(dir); return 0; }
	int ok = 0;
	/* The debt is paid first, whatever the visible record is. */
	if (retro_owner_current(run_dir, owner, mode) &&
	    (!retro_sync_debt_find(directory) || retro_dir_sync(dir, directory))) {
		char key[RETRO_KEY_MAX], holder[33];
		int visible = retro_visible_record(dir, key);
		if (visible == 0) ok = 1;
		else if (visible == 1) {
			if (!retro_stamp_owner(run_dir, key, holder) || strcmp(holder, owner->session)) ok = 1;
			else if (!retro_sync_debt_reserve(directory)) ok = 0;
			else if (unlinkat(dir, "resume", 0) < 0) ok = errno == ENOENT ? retro_dir_sync(dir, directory) : 0;
			else ok = retro_dir_sync(dir, directory);
		}
	}
	close(lock);
	close(dir);
	return ok;
}

static int retro_system_resume_marker(void *data, const struct retro_owner *owner, int mode,
				      const struct retro_resume_proof *proof)
{
	(void)data;
	return retro_write_resume(retro_resume_dir, retro_run_dir, owner, mode, proof);
}
static int retro_system_resume_invalidate(void *data, const struct retro_owner *owner, int mode)
{
	(void)data;
	return retro_invalidate_resume(retro_resume_dir, retro_run_dir, owner, mode);
}
static const struct retro_power_ops retro_system_ops = {
	.request = retro_system_request, .signal_group = retro_system_signal,
	.request_proof = retro_system_request_proof,
	.child_alive = retro_system_child_alive, .wait_ms = retro_system_wait_ms,
	.resume_marker = retro_system_resume_marker,
	.resume_invalidate = retro_system_resume_invalidate,
	.shutdown_finalize = retro_system_shutdown_finalize
};

/* The sleep acknowledgement carries the proof of the checkpoint it committed. With the running session known the
 * boot record is published here, because nothing after a suspend can ask the runtime for it. A publication that
 * fails is never acknowledged and never removes the previous record, but it does not promise the record is
 * unchanged: a failure after the rename (the directory sync) can leave the candidate already visible. The stamp
 * names the right session for whichever record is visible, and boot accepts only the exact checkpoint CURRENT
 * names. */
static int retro_prepare_suspend(const struct retro_power_ops *ops, pid_t game, pid_t pg,
				 uint64_t epoch, int allow_stop, const struct retro_owner *owner)
{
	struct retro_resume_proof proof = {0};
	struct retro_power_report rep = { RETRO_POWER_NONE, RETRO_STAGE_SKIPPED, RETRO_STAGE_SKIPPED };
	int result = RETRO_POWER_NONE;
	if (game && allow_stop) {
		if (owner && ops->request_proof)
			result = ops->request_proof(ops->data, game, owner, BANANA_RETRO_CTL_PREPARE_SLEEP, epoch, 1500, &proof);
		else result = ops->request(ops->data, BANANA_RETRO_CTL_PREPARE_SLEEP, epoch, 1500);
	}
	rep.checkpoint = result;
	if (result == RETRO_POWER_OK) {
		if (owner && proof.token[0] && ops->resume_marker)
			rep.publication = ops->resume_marker(ops->data, owner, RETRO_PUBLISH_LIVE, &proof) ?
				RETRO_STAGE_OK : RETRO_STAGE_FAILED;
		retro_report_finish(ops, "sleep", &rep);
		return RETRO_POWER_OK;
	}
	retro_report_finish(ops, "sleep", &rep);
	if (result == RETRO_POWER_FAILED)
		(void)ops->request(ops->data, BANANA_RETRO_CTL_SLEEP_ABORT, epoch, 500);
	if (allow_stop && pg > 0 && ops->signal_group(ops->data, pg, SIGSTOP) < 0) {
		int e = errno;
		sleep_log("game: pre-suspend SIGSTOP failed (pid group %d, errno %d: %s)",
		    (int)pg, e, strerror(e));
	}
	return result;
}

static int retro_finish_suspend(const struct retro_power_ops *ops, int prepared, uint16_t type,
				uint64_t epoch, pid_t pg)
{
	int result = prepared ? ops->request(ops->data, type, epoch, 1500) : RETRO_POWER_NONE;
	if ((!prepared || result != RETRO_POWER_OK) && pg > 0 &&
	    ops->signal_group(ops->data, pg, SIGCONT) < 0) {
		int e = errno;
		sleep_log("game: suspend recovery SIGCONT failed (pid group %d, errno %d: %s)",
		    (int)pg, e, strerror(e));
	}
	return result;
}

static int retro_reconcile_after_wake(const struct retro_power_ops *ops, int prepared,
				      int native_attempted, uint64_t epoch, pid_t game, pid_t pg)
{
	if (!native_attempted) return RETRO_POWER_NONE;
	int type = prepared ? BANANA_RETRO_CTL_WAKE : BANANA_RETRO_CTL_SLEEP_ABORT;
	int result = ops->request(ops->data, (uint16_t)type, epoch, 1500);
	if ((prepared ? result != RETRO_POWER_OK : result == RETRO_POWER_FAILED) &&
	    ops->child_alive(ops->data, game) && pg > 0)
		(void)ops->signal_group(ops->data, pg, SIGTERM);
	return result;
}

static void retro_suspend_resume_group(const struct retro_power_ops *ops, int prepared, pid_t pg)
{
	if (!prepared && pg > 0 && ops->signal_group(ops->data, pg, SIGCONT) < 0) {
		int e = errno;
		sleep_log("game: suspend recovery SIGCONT failed (pid group %d, errno %d: %s)",
		    (int)pg, e, strerror(e));
	}
}

/* Complete the production suspend transaction after the platform reports either
 * a failed suspend or a real wake. Platform power itself remains an injected boundary. */
static int retro_suspend_complete(const struct retro_power_ops *ops, int prepared,
				 int native_attempted, int platform_slept, uint64_t epoch,
				 pid_t game, pid_t pg)
{
	if (platform_slept)
		return retro_reconcile_after_wake(ops, prepared, native_attempted, epoch, game, pg);
	int released = retro_finish_suspend(ops, prepared, BANANA_RETRO_CTL_SLEEP_ABORT, epoch, pg);
	if (native_attempted && !prepared) {
		int reconciled = ops->request(ops->data, BANANA_RETRO_CTL_SLEEP_ABORT, epoch, 1500);
		if (reconciled == RETRO_POWER_FAILED && ops->child_alive(ops->data, game) && pg > 0)
			(void)ops->signal_group(ops->data, pg, SIGTERM);
	}
	return released;
}

static int retro_suspend_failure_action(const struct retro_power_ops *ops, int prepared,
					int native_attempted, unsigned failures,
					unsigned fallback, uint64_t epoch,
					pid_t game, pid_t pg, int *released)
{
	if (fallback < 2 && failures <= 2) return 1;
	*released = retro_suspend_complete(ops, prepared, native_attempted, 0, epoch, game, pg);
	return 0;
}

static void retro_shutdown_child(const struct retro_power_ops *ops, pid_t game, pid_t pg, int native)
{
	if (native == RETRO_POWER_OK)
		for (unsigned i = 0; i < 30 && ops->child_alive(ops->data, game); ++i)
			ops->wait_ms(ops->data, 100);
	if (ops->child_alive(ops->data, game)) {
		if (pg > 0) (void)ops->signal_group(ops->data, pg, SIGTERM);
		else (void)kill(game, SIGTERM);
		for (unsigned i = 0; i < 50 && ops->child_alive(ops->data, game); ++i)
			ops->wait_ms(ops->data, 100);
	}
}

/* owner: the session captured before it was told to stop, or NULL when none could be proven. Without an owner
 * nothing is published and nothing is removed. */
static int retro_shutdown_session(const struct retro_power_ops *ops, pid_t game, pid_t pg,
				  uint64_t epoch, const struct retro_owner *owner)
{
	struct retro_resume_proof proof = {0};
	struct retro_power_report rep = { RETRO_POWER_NONE, RETRO_STAGE_SKIPPED, RETRO_STAGE_SKIPPED };
	int result = RETRO_POWER_NONE;
	if (game > 0) {
		result = ops->request_proof(ops->data, game, owner, BANANA_RETRO_CTL_SHUTDOWN, epoch, 3000, &proof);
		rep.checkpoint = result;
		retro_shutdown_child(ops, game, pg, result);
	}
	/* A failed or ambiguous request does not replace a previously committed target, and no failure removes a
	 * record: exact-token validation at boot decides whether it is still current. A menu shutdown has no newer
	 * record; an unsettled boot attempt stays for the UI to retry. Publication and cleanup failures are recorded
	 * in the report only: the checkpoint result and the finalization below do not depend on them. */
	if (game > 0 && owner && ops->resume_marker && (result == RETRO_POWER_NONE || result == RETRO_POWER_OK)) {
		if (result == RETRO_POWER_NONE || proof.token[0]) {
			rep.publication = ops->resume_marker(ops->data, owner, RETRO_PUBLISH_SHUTDOWN,
							     result == RETRO_POWER_OK ? &proof : NULL) ?
				RETRO_STAGE_OK : RETRO_STAGE_FAILED;
		} else if (proof.battery_only) {
			/* The bound runtime committed battery data and, by an OK reply to a proof request, no checkpoint:
			 * a record this session published names a superseded one. Only that session's record is removed. */
			rep.cleanup = ops->resume_invalidate &&
				ops->resume_invalidate(ops->data, owner, RETRO_PUBLISH_SHUTDOWN) ?
				RETRO_STAGE_OK : RETRO_STAGE_FAILED;
		} else {
			/* OK without a digest that did not identify itself as battery-only (a runtime that could not answer
			 * the proof request, or a launcher that is not this session's ROM): ambiguous, so nothing is removed.
			 * Boot validates whatever record is visible against the exact checkpoint CURRENT names. */
			sleep_log("shutdown reply did not identify a battery-only commit; resume record left as is");
		}
	}
	retro_report_finish(ops, "shutdown", &rep);
	if (ops->shutdown_finalize) ops->shutdown_finalize(ops->data);
	return result;
}

static uint64_t next_retro_power_epoch(void)
{
	if (retro_power_epoch == 0) {
		struct timespec ts;
		if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
			retro_power_epoch = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
	}
	if (retro_power_epoch < UINT64_MAX) ++retro_power_epoch;
	return retro_power_epoch;
}

/* banana-ui's pid, by its name in /proc. Cached: the scan ran on every wake and on every volume/brightness key's
 * OSD; it is repeated only when the cached pid has gone (banana-ui restarted) */
static pid_t ui_pid_cache;
static pid_t ui_pid(void)
{
	if (ui_pid_cache > 0 && kill(ui_pid_cache, 0) == 0) return ui_pid_cache;
	DIR *d = opendir("/proc");
	if (!d) return ui_pid_cache;
	struct dirent *e;
	pid_t r = 0;
	while (!r && (e = readdir(d))) {
		if (!isdigit((unsigned char)e->d_name[0])) continue;
		char path[300], comm[32];
		snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
		read_str(path, comm, sizeof comm);
		if (!strcmp(comm, "banana-ui")) r = atoi(e->d_name);
	}
	closedir(d);
	if (r > 0) ui_pid_cache = r;
	return r;
}

/* while banana-ui's compositor owns the panel it writes the 3D card file; the menu turns it into an overlay above a
 * client. A stopped compositor keeps the existing OSD/SHOW_MSG paths. */
static int compositor_on(void) { return access("/run/banana/compositor", F_OK) == 0; }
/* the menu's quick menu is up: the pad is grabbed, so its F-combos (F+B analog, F+volume backlight) are suspended
 * while power and a single volume key keep working */
static int overlay_open(void) { return access("/run/banana/overlay-open", F_OK) == 0; }

static void overlay_show(const char *kind, const char *title, const char *body, const char *badge, int ms)
{
	if (!compositor_on()) return;
	char v[512];
	snprintf(v, sizeof v, "%s|%s|%s|%s|%d\n", kind, title ? title : "", body ? body : "",
	         badge ? badge : "", ms);
	FILE *f = fopen("/run/banana/overlay.new", "w");
	if (!f) return;
	fputs(v, f);
	fclose(f);
	rename("/run/banana/overlay.new", "/run/banana/overlay");
}

/* the menu's level card for a volume or brightness key: what changed and its percent in /run/banana/osd, then
 * SIGUSR2 tells banana-ui to show it (it ignores a file older than a moment, e.g. one written during a game) */
static void osd(const char *what, int percent)
{
	char pct[32];
	snprintf(pct, sizeof pct, "%d%%", percent);
	overlay_show(what, what, pct, "", 2000);	// a client under the in-process compositor
	char v[64];
	snprintf(v, sizeof v, "%s %d\n", what, percent);
	FILE *f = fopen("/run/banana/osd.new", "w");
	if (!f) return;
	fputs(v, f);
	fclose(f);
	rename("/run/banana/osd.new", "/run/banana/osd");
	pid_t ui = ui_pid();
	if (ui > 0) kill(ui, SIGUSR2);
}

/* the headphone jack's state as the kernel has it (the codec's jack is an input device with a switch) */
static int headphones_in(void)
{
	for (int i = 0; i < ndev; i++) {
		unsigned long sw[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 };
		if (ioctl(devfd[i], EVIOCGSW(sizeof sw), sw) >= 0 &&
		    (sw[SW_HEADPHONE_INSERT / (8 * sizeof(long))] >> (SW_HEADPHONE_INSERT % (8 * sizeof(long))) & 1))
			return 1;
	}
	return 0;
}

/* The direct look at the jack (the still-to-do check in the port log): the input device that carries SW_HEADPHONE_INSERT,
 * its name and the state read now -- the same read headphones_in() acts on. The WCD937x MBHC (which backs that device)
 * is known to answer before it has measured the socket, so on a boot that comes up silent this says whether the codec
 * claimed "headphones in" with nothing plugged. */
static void jack_log(void)
{
	static int said_absent, said_present;
	size_t w = SW_HEADPHONE_INSERT / (8 * sizeof(long)), b = SW_HEADPHONE_INSERT % (8 * sizeof(long));
	int found = 0;
	for (int i = 0; i < ndev; i++) {
		unsigned long cap[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 };
		if (ioctl(devfd[i], EVIOCGBIT(EV_SW, sizeof cap), cap) < 0 || !((cap[w] >> b) & 1)) continue;
		unsigned long st[(SW_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = { 0 };
		int in = ioctl(devfd[i], EVIOCGSW(sizeof st), st) >= 0 && ((st[w] >> b) & 1);
		char name[64] = "?";
		ioctl(devfd[i], EVIOCGNAME(sizeof name), name);
		if (!said_present) say("sound: the jack switch on '%s' reads %s", name, in ? "in" : "out");
		found = 1;
	}
	if (found) said_present = 1;
	/* The jack is an input device that probes with the codec, and bananad can open the inputs before it appears: with
	 * no such device, headphones_in() answers "nothing in" by absence, not by measurement. Say which it is. */
	else if (!said_absent) { said_absent = 1; say("sound: no input device carries SW_HEADPHONE_INSERT yet"); }
}

/* whether the kernel has `code` down on this input device right now (0 when the device has no such key or can't say) */
static int key_held_now(int fd, int code)
{
	unsigned long k[KEY_MAX / (8 * sizeof(long)) + 1];
	memset(k, 0, sizeof k);
	if (ioctl(fd, EVIOCGKEY(sizeof k), k) < 0) return 0;
	return (k[code / (8 * sizeof(long))] >> (code % (8 * sizeof(long)))) & 1;
}

static void drain_inputs(void)
{
	struct input_event ev[64];
	for (int i = 0; i < ndev; i++)
		while (read(devfd[i], ev, sizeof ev) > 0) {}
}

/* Did the user really press power while it slept? The PMIC pwrkey is the only armed wake source and its IRQ can fire
 * on a glitch (and on the release of the very press that asked for the sleep), so a wake only counts when a real
 * KEY_POWER press is among the queued events. The events are consumed here, so the wake-up sequence's drain_inputs()
 * then finds nothing. */
static int wake_key_press(void)
{
	int pressed = 0;
	struct input_event ev[64];
	for (int i = 0; i < ndev; i++) {
		ssize_t r;
		while ((r = read(devfd[i], ev, sizeof ev)) > 0)
			for (ssize_t k = 0; k < r / (ssize_t)sizeof ev[0]; k++) {
				if (ev[k].type == EV_SYN && ev[k].code == SYN_DROPPED) {
					/* the queue overflowed and the press may be among what was lost: the key's state now says */
					if (key_held_now(devfd[i], KEY_POWER)) pressed = 1;
				}
				if (ev[k].type == EV_KEY && ev[k].code == KEY_POWER && ev[k].value == 1) pressed = 1;
			}
	}
	return pressed;
}

/* ---- sleep ------------------------------------------------------------------------------------------------------
 * Pressing power (or an idle timer): the game is paused, the pre-sleep hooks run (Wi-Fi down, 10-wifi), the card is
 * flushed (the kernel syncs too: this config does not set SUSPEND_SKIP_SYNC, so it is a belt-and-braces), an RTC alarm
 * is set for the battery check, and the SoC suspends to RAM. On waking,
 * the game and the menu go on first, then the post-sleep hooks in the background. Woken by the battery check:
 * straight back to sleep, without running anything again. */
static void shutdown_now(const char *why);
static void handle(const struct input_event *e);
static int battery_discharging_pct(void);
static int battery_pct_any(void);
static long battery_charge_uah(void);
static int sleep_fallback;		/* 0 as configured; 1 devices one at a time; 2 also s2idle (a suspend failed) */

static void sleep_log(const char *fmt, ...)
{
	char line[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	say("%s", line);
	const char *path = "/data/banana/logs/sleep.log";
	struct stat st;
	if (!stat(path, &st) && st.st_size > 64 * 1024) rename(path, "/data/banana/logs/sleep.log.1");
	FILE *f = fopen(path, "a");
	if (!f) return;
	time_t now = time(NULL);
	char when[32];
	strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
	fprintf(f, "%s %s\n", when, line);
	fclose(f);
}

/* Which interrupts fired across one suspend, for the wakes nobody asked for (the 12.7 s burst, the 1-3 s "did not take"
 * power-key wakes). wakeup_sources stays at 0 on this board, so the per-IRQ counts in /proc/interrupts either side of the
 * write to /sys/power/state are the only trace of the waker. Rows are summed over the CPUs and keyed by IRQ number; the
 * per-CPU noise (IPIs, the arch timer, Err/Mis) is dropped. One line per resume in logs/sleep-irq.log, with the branch
 * the wake took, so a phantom's rows can be read against a key wake's and an RTC wake's. Fixed arrays: no allocation. */
#define IRQ_MAX 384
struct irq_row { int irq; unsigned long n; char name[40]; };
static struct irq_row irq_before[IRQ_MAX], irq_after[IRQ_MAX];
static int irq_nbefore, irq_nafter;
static int charger_online(void);

static int irq_snapshot(struct irq_row *rows)
{
	FILE *f = fopen("/proc/interrupts", "r");
	if (!f) return 0;
	char ln[768];
	int n = 0;
	while (n < IRQ_MAX && fgets(ln, sizeof ln, f)) {
		char *p = ln;
		while (*p == ' ') p++;
		if (*p < '0' || *p > '9') continue;	/* the CPU header, IPI*, Err, Mis */
		char *end;
		long irq = strtol(p, &end, 10);
		if (*end != ':') continue;
		p = end + 1;
		unsigned long sum = 0;
		for (;;) {
			while (*p == ' ') p++;
			if (*p < '0' || *p > '9') break;
			sum += strtoul(p, &end, 10);
			p = end;
		}
		size_t len = strcspn(p, "\n");
		while (len && p[len - 1] == ' ') len--;
		if (len >= sizeof rows[n].name) { p += len - (sizeof rows[n].name - 1); len = sizeof rows[n].name - 1; }	/* the tail names the device */
		memcpy(rows[n].name, p, len);
		rows[n].name[len] = 0;
		if (strstr(rows[n].name, "arch_timer")) continue;
		rows[n].irq = (int)irq;
		rows[n].n = sum;
		n++;
	}
	fclose(f);
	return n;
}

static void irq_report(const char *kind, double slept)
{
	char line[900];
	size_t used = 0;
	int shown = 0;
	for (int i = 0; i < irq_nafter && used < sizeof line - 80; i++) {
		unsigned long was = 0;
		for (int j = 0; j < irq_nbefore; j++)
			if (irq_before[j].irq == irq_after[i].irq && !strcmp(irq_before[j].name, irq_after[i].name)) { was = irq_before[j].n; break; }
		if (irq_after[i].n == was) continue;
		int w = snprintf(line + used, sizeof line - used, " %d %s +%lu;", irq_after[i].irq, irq_after[i].name,
		    irq_after[i].n - was);
		if (w < 0 || (size_t)w >= sizeof line - used) break;
		used += (size_t)w;
		shown++;
	}
	line[used] = 0;
	const char *path = "/data/banana/logs/sleep-irq.log";
	struct stat st;
	if (!stat(path, &st) && st.st_size > 64 * 1024) rename(path, "/data/banana/logs/sleep-irq.log.1");
	FILE *f = fopen(path, "a");
	if (!f) return;
	time_t now = time(NULL);
	char when[32];
	strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
	fprintf(f, "%s %s after %.1f s, plugged %d:%s\n", when, kind, slept, charger_online(), shown ? line : " (no interrupt moved)");
	fclose(f);
}

/* What the kernel announced while it slept. A phantom wake leaves no wakeup event, but the glink traffic behind it (the
 * battery manager's notifications while charging) comes out as uevents queued on a socket nobody read during the suspend.
 * A private socket (the main one stays untouched) is emptied just before each suspend and read at each phantom, and the
 * names go in sleep-irq.log beside the interrupt rows: "power_supply/qcom-battmgr-usb change" says who spoke. */
static int uevent_open(void);
static int diag_fd = -2;

static void diag_flush(void)
{
	if (diag_fd == -2) diag_fd = uevent_open();
	if (diag_fd < 0) return;
	char buf[8192];
	while (recv(diag_fd, buf, sizeof buf, 0) > 0) { }
}

static void diag_report(const char *kind)
{
	if (diag_fd < 0) return;
	char line[600], buf[8192];
	size_t used = 0;
	int n_ev = 0;
	ssize_t n;
	while ((n = recv(diag_fd, buf, sizeof buf - 1, 0)) > 0) {
		buf[n] = 0;
		const char *act = "", *sub = "", *name = "";
		for (char *q = buf; q < buf + n; q += strlen(q) + 1) {
			if (!strncmp(q, "ACTION=", 7)) act = q + 7;
			else if (!strncmp(q, "SUBSYSTEM=", 10)) sub = q + 10;
			else if (!strncmp(q, "POWER_SUPPLY_NAME=", 18)) name = q + 18;
			else if (!*name && !strncmp(q, "DEVPATH=", 8)) { const char *sl = strrchr(q, '/'); name = sl ? sl + 1 : q + 8; }
		}
		n_ev++;
		int w = snprintf(line + used, sizeof line - used, " %s/%s %s;", sub, name, act);
		if (w > 0 && (size_t)w < sizeof line - used) used += (size_t)w;
	}
	line[used] = 0;
	FILE *f = fopen("/data/banana/logs/sleep-irq.log", "a");
	if (!f) return;
	fprintf(f, "  %s uevents (%d):%s\n", kind, n_ev, n_ev ? line : " none");
	fclose(f);
}

/* the RTC alarm that wakes the handheld for the battery check; 0 clears it. Returns the alarm's time (0: none) */
static time_t rtc_alarm(int seconds)
{
	write_file("/sys/class/rtc/rtc0/wakealarm", "0");
	if (seconds <= 0) return 0;
	char v[32];
	snprintf(v, sizeof v, "+%d", seconds);
	if (write_file("/sys/class/rtc/rtc0/wakealarm", v)) return 0;
	return time(NULL) + seconds;
}

/* The RPC's PMIC RTC has no alarm (`qcom,no-alarm`: the ADSP owns its interrupt), so wakealarm cannot be written and the
 * sleep_check_hours battery check -- and with it sleep_poweroff_percent -- can never fire. Say which at start: a
 * sleeping handheld then comes back only on the power key or the charger, so it must not be left asleep unattended or
 * the battery drains flat. */
static int rtc_wake_possible(void)
{
	return access("/sys/class/rtc/rtc0/wakealarm", W_OK) == 0;
}

/* Refuse to deep-sleep a nearly empty battery when nothing can wake it on a timer. With the RPC's ADSP-owned RTC alarm
 * there is no wakealarm, so a sleeping low battery would drain flat and never come back; instead the caller leaves the
 * panel dark and keeps the SoC awake, so the 60 s poll can still shut down cleanly at battery_shutdown_percent. */
static int sleep_too_low(void)
{
	if (rtc_wake_possible()) return 0;	/* a real alarm: the battery check wakes it in time */
	int pct = battery_discharging_pct();
	return pct >= 0 && pct <= cfg.sleep_floor_pct;
}

/* Why the kernel resumed. It cannot say on this board: /sys/power/pm_wakeup_irq is absent, and a real power-key wake
 * leaves every wakeup_sources counter at 0 (the PMIC's PON IRQ is not a tracked wakeup source; mmc0, which the table's
 * last_change used to name for every wake, never fires at all -- measured with a key wake on the handheld: pwrkey 0/0).
 * So report what bananad itself saw: the power key press (wake_key_press()), the cable, or neither -- plus the kernel's
 * global wakeup_count, which says whether it counted a wakeup event at all. */
static void wake_reason(char *out, size_t n, int key, int cable, unsigned long wk0)
{
	unsigned long wk = 0;
	FILE *f = fopen("/sys/power/wakeup_count", "r");
	if (f) { if (fscanf(f, "%lu", &wk) != 1) wk = 0; fclose(f); }
	const char *seen = key ? "power key" : cable ? "cable" : "no key and no cable";
	if (wk > wk0) snprintf(out, n, "wake: %s (kernel wakeup_count +%lu)", seen, wk - wk0);
	else snprintf(out, n, "wake: %s (the kernel counted no wakeup event)", seen);
}

/* The menu's handover, waited for rather than guessed at. After SIGHUP, banana-ui releases the DSP and closes its PCM
 * (Sound::release waits up to 2 s for the sound thread), publishing each step to /run/banana/sound-state. A PCM close
 * still in flight when the DSP is told to sleep is what the overnight hang on 03:40 might have been (logs/sleep-state
 * from the next sleep said "releasing (handover)" at the write of mem). So: 1 = it closed (ms), 0 = still open after
 * the menu's own 2 s, -1 = no note at all (a menu from before this, which gets the old fixed 200 ms). */
static int ui_release_wait(double *ms)
{
	double t = mono();
	for (;;) {
		char s[128] = "";
		read_str("/run/banana/sound-state", s, sizeof s);
		if (!s[0]) return -1;
		if (!strstr(s, "open") && !strstr(s, "releasing")) { *ms = (mono() - t) * 1000; return 1; }
		if (mono() - t > 2.0) { *ms = (mono() - t) * 1000; return 0; }
		sleep_us(20000);
	}
}

/* The menu's other handover: after SIGHUP it turns the CRTC off (Display::sleepOff) and answers in
 * /run/banana/panel-state, which this side set to "asked" before the signal. The sound-state wait above cannot stand
 * in for it: with nothing playing the sound device is already closed, so that wait returns before the menu has even
 * read the signal, and a menu frozen with its panel still on is the one a phantom wake lights. 1 = off (ms),
 * 0 = the menu could not, -1 = no answer in a second (a menu from before this). The second leaves room for the CRT
 * switch-off the menu may draw first (about 300 ms); the wait ends as soon as the answer is there. */
static int ui_panel_off_wait(double *ms)
{
	double t = mono();
	for (;;) {
		char s[32] = "";
		read_str("/run/banana/panel-state", s, sizeof s);
		*ms = (mono() - t) * 1000;
		if (!strcmp(s, "off")) return 1;
		if (!strcmp(s, "off failed")) return 0;
		if (mono() - t > 1.0) return -1;
		sleep_us(10000);
	}
}

/* What the handheld looked like as the kernel went under, on the data partition. A sleep whose resume never returns
 * leaves no "awake" line and no post hook, so this note (overwritten every sleep) is the only thing the next boot can
 * read: the mode, and whether the menu still had its PCM open -- a close in flight when the DSP was told to sleep is
 * the one thing the hang at 03:40 on the handheld could have been doing (logs/banana-ui.log.1: the device was
 * reopened 16 s before that sleep and handed over 200 ms before the write of "mem"). */
static void sleep_note(const char *why, const char *mode, int sources)
{
	char path[] = "/data/banana/logs/sleep-state";
	struct stat st;
	if (!stat(path, &st) && st.st_size > 32 * 1024) rename(path, "/data/banana/logs/sleep-state.1");
	FILE *f = fopen(path, "w");
	if (!f) return;
	time_t now = time(NULL);
	char when[32], sound[128] = "";
	strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
	read_str("/run/banana/sound-state", sound, sizeof sound);
	fprintf(f, "%s going to sleep (%s)\n", when, why);
	fprintf(f, "mode: %s, wakeup sources in the table: %d\n", mode, sources);
	fprintf(f, "menu: %d, sound device: %s\n", (int)ui_pid(), sound[0] ? sound : "(no note)");
	fprintf(f, "game: %d\n", (int)pid_of("/run/banana/game.pid"));
	fclose(f);
}

/* and the kernel's own record of the trip, on the way out (the note above is the only survivor of one that did not) */
static void sleep_note_resume(const char *what)
{
	FILE *f = fopen("/data/banana/logs/sleep-state", "a");
	if (!f) return;
	time_t now = time(NULL);
	char when[32];
	strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
	fprintf(f, "%s awake: %s\n", when, what);
	FILE *p = popen("dmesg 2>/dev/null | grep -E 'PM: (suspend|resume)' | tail -n 6", "r");
	if (p) {
		char ln[160];
		while (fgets(ln, sizeof ln, p)) fputs(ln, f);
		pclose(p);
	}
	fclose(f);
}

/* Only the owner may wake the handheld. The kernel arms every device that says it can wake -- the rsinput MCU's
 * serial (988000.serial), the Wi-Fi/ADSP remoteprocs (8a00000/3000000), gpio-keys (the volume keys), the thermal
 * sensors, mmc0 -- and any of them raising an interrupt during a sleep is a resume nobody asked for (the serial MCU
 * and the Wi-Fi link were the ones seen waking it). So just before suspending, leave the PMIC pwrkey armed, keep the
 * charger/USB supplies armed (the cable is a wake the owner did ask for -- it is the one press that has to work when
 * the pwrkey line glitches, which is exactly what it does: see the sleep.log's "wake with no key or cable"), and
 * disable the rest. The drivers re-arm their own wakeup on resume, which is why this runs before every sleep rather
 * than once at boot. mmc0 is a virtual source with no power/wakeup file to write, so it cannot be disarmed here;
 * logs/sleep-state records how many sources there are, and a wake now names the source whose count actually moved. */
static void wake_only_power(void)
{
	DIR *d = opendir("/sys/class/wakeup");
	if (!d) { sleep_log("wake sources: cannot open /sys/class/wakeup: %s", strerror(errno)); return; }
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "wakeup", 6) != 0) continue;
		char name[160] = "", p[PATH_MAX + 32], dev[PATH_MAX];
		snprintf(p, sizeof p, "/sys/class/wakeup/%s/name", e->d_name);
		read_str(p, name, sizeof name);
		snprintf(p, sizeof p, "/sys/class/wakeup/%s/device", e->d_name);
		if (!realpath(p, dev)) continue;	/* a virtual source (mmc0) has no power/wakeup to write */
		snprintf(p, sizeof p, "%s/power/wakeup", dev);
		int keep = strstr(name, "pwrkey") || strstr(name, "battmgr") || strstr(name, "ucsi");
		if (write_file(p, keep ? "enabled" : "disabled"))
			sleep_log("wake source %s: cannot set power/wakeup=%s: %s", name[0] ? name : e->d_name,
			    keep ? "enabled" : "disabled", strerror(errno));
	}
	closedir(d);
}

static void sleep_mode(void)
{
	/* deep: suspend to RAM (TF-A turns the SoC off, DRAM in self-refresh); s2idle as the fallback */
	char modes[128];
	read_str("/sys/power/mem_sleep", modes, sizeof modes);
	const char *want = sleep_fallback >= 2 ? "s2idle" : cfg.mem_sleep;
	char chosen[32];
	snprintf(chosen, sizeof chosen, "[%s]", want);
	if (strstr(modes, want) && !strstr(modes, chosen)) write_file("/sys/power/mem_sleep", want);	/* only if it changes */
	write_file("/sys/power/pm_async", sleep_fallback == 0 ? "1" : "0");
}

/* the backlights off and on again without changing their level */
/* The backlight class's entries, and one of their files. Six callers below each had their own walk of this directory,
 * which is how they drifted apart (one wrote the panel another owned). Names are short ("backlight0", "panel"): a
 * longer one is not a backlight this image has, so it is skipped rather than truncated. */
#define MAX_BL 8
static int backlights(char names[][64], int max)
{
	DIR *d = opendir("/sys/class/backlight");
	if (!d) return 0;
	struct dirent *e;
	int n = 0;
	while (n < max && (e = readdir(d))) {
		if (e->d_name[0] == '.' || strlen(e->d_name) >= 64) continue;
		snprintf(names[n++], 64, "%s", e->d_name);
	}
	closedir(d);
	return n;
}

static void bl_file(char *out, size_t n, const char *name, const char *what)
{
	snprintf(out, n, "/sys/class/backlight/%.63s/%.31s", name, what);	/* the bounds keep -Wformat-truncation quiet */
}

static void backlights_power(int on)
{
	char bl[MAX_BL][64];
	int n = backlights(bl, MAX_BL);
	for (int i = 0; i < n; i++) {
		char p[320];
		bl_file(p, sizeof p, bl[i], "bl_power");
		write_file(p, on ? "0" : "4");
	}
}

static int charger_online(void);
/* Mass storage read-write "transfer mode" (usb-msc, Settings > System > Mass storage): the card is the computer's
 * disk and /data is unmounted here. Do not sleep the handheld then -- a suspend would stall the transfer mid-write. */
static int msc_rw(void)
{
	char m[8];
	read_str("/run/banana/usb-msc.mode", m, sizeof m);
	return !strcmp(m, "rw");
}
/* Quick resume's automatic state, written now: the quit-time ".auto" state is the one thing that survives a battery
 * that dies while the game sleeps in RAM (the next launch loads it). A RetroArch game that is running and not already
 * frozen by the pause menu is asked over the stdin FIFO (SAVE_STATE_AUTO, patch 0015) and answers in
 * /run/banana/autosave; bounded, so a wedged game costs a second and a half of the sleep, never the sleep. Only with
 * Quick resume on (the same key that makes RetroArch write it at quit). */
static int sleep_save_state(pid_t game)
{
	if (!game || skip("state")) return 0;
	char buf[64] = "";
	FILE *f = fopen("/data/retroarch/retroarch.cfg", "r");
	int on = 0;
	if (f) {
		char line[256];
		while (fgets(line, sizeof line, f))
			if (!strncmp(line, "savestate_auto_save ", 20)) on = strstr(line, "\"true\"") != NULL;
		fclose(f);
	}
	if (!on) return 0;
	char path[48];
	snprintf(path, sizeof path, "/proc/%d/stat", (int)game);
	f = fopen(path, "r");
	if (!f) return 0;
	char st[256] = "";
	if (fgets(st, sizeof st, f)) {
		char *rp = strrchr(st, ')');
		if (rp && rp[1] == ' ' && rp[2] == 'T') { fclose(f); return 0; }	/* the pause menu froze it: it cannot answer */
	}
	fclose(f);
	unlink("/run/banana/autosave");
	double t = mono();
	if (!ra_cmd("SAVE_STATE_AUTO")) return 0;
	for (;;) {
		f = fopen("/run/banana/autosave", "r");
		if (f) {
			if (fgets(buf, sizeof buf, f) && buf[0]) {
				fclose(f);
				int ok = !strncmp(buf, "ok", 2);
				sleep_log("state: saved in %.0f ms (%s)", (mono() - t) * 1000, strtok(buf, "\n"));
				return ok;
			}
			fclose(f);
		}
		if (mono() - t > 1.5) { sleep_log("state: no answer from the game after 1500 ms: sleeping anyway"); return 0; }
		sleep_us(20000);
	}
}

enum retro_checkpoint_result { RETRO_CHECKPOINT_FAILED, RETRO_CHECKPOINT_MARKED, RETRO_CHECKPOINT_BATTERY_ONLY };

/* Checkpoint, boot-record publication and cleanup are separate outcomes in *rep; the return value says only
 * whether the boot record now names the checkpoint just committed (MARKED), was correctly retired (BATTERY_ONLY),
 * or is unproven and untrusted (FAILED: the previous record was not removed, but a publication that failed
 * after its rename may already have made the candidate visible). A record that cannot be written or removed
 * never turns a committed checkpoint into a failed one in the report. */
static int session_checkpoint_run(const struct retro_power_ops *ops, pid_t game, const struct retro_owner *owner,
				  int (*legacy_save)(pid_t), struct retro_power_report *rep)
{
	struct retro_resume_proof proof = {0};
	int native = ops->request_proof(ops->data, game, owner, BANANA_RETRO_CTL_CHECKPOINT, 0, 1500, &proof);
	rep->checkpoint = native;
	if (native == RETRO_POWER_NONE) {
		rep->checkpoint = legacy_save(game) ? RETRO_POWER_OK : RETRO_POWER_FAILED;
		if (rep->checkpoint != RETRO_POWER_OK) return RETRO_CHECKPOINT_FAILED;
	} else if (native == RETRO_POWER_OK && !proof.token[0] && proof.battery_only) {
		/* The runtime committed battery data and no checkpoint: a record this session published names a
		 * checkpoint that is now superseded, so remove it. A record that session does not own stays. */
		if (!ops->child_alive(ops->data, game)) return RETRO_CHECKPOINT_FAILED;
		rep->cleanup = ops->resume_invalidate(ops->data, owner, RETRO_PUBLISH_LIVE) ?
			RETRO_STAGE_OK : RETRO_STAGE_FAILED;
		return rep->cleanup == RETRO_STAGE_OK ? RETRO_CHECKPOINT_BATTERY_ONLY : RETRO_CHECKPOINT_FAILED;
	} else if (native != RETRO_POWER_OK || !proof.token[0]) return RETRO_CHECKPOINT_FAILED;
	if (!ops->child_alive(ops->data, game)) return RETRO_CHECKPOINT_FAILED;
	rep->publication = ops->resume_marker(ops->data, owner, RETRO_PUBLISH_LIVE,
					      native == RETRO_POWER_OK ? &proof : NULL) ?
		RETRO_STAGE_OK : RETRO_STAGE_FAILED;
	return rep->publication == RETRO_STAGE_OK ? RETRO_CHECKPOINT_MARKED : RETRO_CHECKPOINT_FAILED;
}

static int session_checkpoint(const struct retro_power_ops *ops, pid_t game, const struct retro_owner *owner,
			      int (*legacy_save)(pid_t))
{
	struct retro_power_report rep = { RETRO_POWER_NONE, RETRO_STAGE_SKIPPED, RETRO_STAGE_SKIPPED };
	int result = session_checkpoint_run(ops, game, owner, legacy_save, &rep);
	retro_report_finish(ops, "checkpoint", &rep);
	return result;
}

/* Sessions that survive a hang: while a game runs, every 180 s (session_secs) its automatic state is written
 * and /data/banana/resume names the ROM (the menu's "resume?" card at the next boot, the state loaded by Quick
 * resume). The menu clears the marker when the game quits to it, so only a hang, a crash of the whole handheld or a
 * pulled battery leaves it behind. Written whole and renamed, so a cut during the write leaves the old one. */
static void session_tick(void)
{
	struct retro_owner owner;
	pid_t game = pid_of("/run/banana/game.pid");
	if (!game || access("/run/banana/shutting-down", F_OK) == 0 ||
	    !retro_capture_owner(retro_run_dir, game, &owner, RETRO_PUBLISH_LIVE)) return;
	(void)session_checkpoint(&retro_system_ops, game, &owner, sleep_save_state);
}

/* ---- standby: the SoC awake behind a dark panel ------------------------------------------------------------------- */
/* Two paths leave the SoC running with nothing to show: the low-battery refusal to sleep (it stays up so the 60 s poll
 * can shut down) and a suspend that failed. Park it while the panel is dark: both cpufreq policies at their minimum
 * (always together), the GPU on its lowest devfreq step, cpu1-7 offline. The values are read first and put back at
 * the next key press, so nothing depends on which clock profile was active. */
#define SB_POL 4
#define SB_CPU 8
static struct { char dir[160]; char gov[32]; int min, max; } sb_pol[SB_POL];
static int sb_npol, sb_on;
static char sb_online[SB_CPU];
static char sb_gpu[160], sb_gpu_gov[32];
static int sb_gpu_max;

static void standby_enter(void)
{
	if (sb_on) return;
	sb_on = 1;
	sb_npol = 0;
	DIR *d = opendir("/sys/devices/system/cpu/cpufreq");
	struct dirent *e;
	while (d && (e = readdir(d)) && sb_npol < SB_POL) {
		if (strncmp(e->d_name, "policy", 6)) continue;
		char p[160], v[240];
		snprintf(p, sizeof p, "/sys/devices/system/cpu/cpufreq/%.40s", e->d_name);
		snprintf(sb_pol[sb_npol].dir, sizeof sb_pol[0].dir, "%s", p);
		snprintf(v, sizeof v, "%s/cpuinfo_min_freq", p);
		int lo = read_int(v, -1);
		snprintf(v, sizeof v, "%s/scaling_min_freq", p);
		sb_pol[sb_npol].min = read_int(v, -1);
		snprintf(v, sizeof v, "%s/scaling_max_freq", p);
		sb_pol[sb_npol].max = read_int(v, -1);
		snprintf(v, sizeof v, "%s/scaling_governor", p);
		read_str(v, sb_pol[sb_npol].gov, sizeof sb_pol[0].gov);
		if (lo <= 0 || sb_pol[sb_npol].min <= 0 || sb_pol[sb_npol].max <= 0) continue;
		char lov[16];
		snprintf(lov, sizeof lov, "%d", lo);
		snprintf(v, sizeof v, "%s/scaling_max_freq", p);
		write_file(v, lov);	/* the cap first, so the floor never sits above it */
		snprintf(v, sizeof v, "%s/scaling_min_freq", p);
		write_file(v, lov);
		sb_npol++;
	}
	if (d) closedir(d);
	sb_gpu[0] = 0;
	DIR *dv = opendir("/sys/class/devfreq");
	while (dv && (e = readdir(dv))) {
		size_t n = strlen(e->d_name);
		if (n < 4 || strcmp(e->d_name + n - 4, ".gpu")) continue;
		char g[200];
		snprintf(sb_gpu, sizeof sb_gpu, "/sys/class/devfreq/%.100s", e->d_name);
		snprintf(g, sizeof g, "%s/governor", sb_gpu);
		read_str(g, sb_gpu_gov, sizeof sb_gpu_gov);
		write_file(g, "powersave");
		/* "powersave" written over "powersave" re-evaluates nothing: the GPU can sit at its top step under it.
		 * Cap the ceiling at the floor so it really drops. */
		char m[200], lo[24];
		snprintf(m, sizeof m, "%s/max_freq", sb_gpu);
		sb_gpu_max = read_int(m, -1);
		snprintf(g, sizeof g, "%s/min_freq", sb_gpu);
		read_str(g, lo, sizeof lo);
		if (sb_gpu_max > 0 && lo[0]) write_file(m, lo);
		break;
	}
	if (dv) closedir(dv);
	for (int i = 1; i < SB_CPU; i++) {
		char p[96];
		snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/online", i);
		sb_online[i] = read_int(p, -1) == 1;
		if (sb_online[i]) write_file(p, "0");
	}
	sleep_log("standby: clocks at the minimum and cpu1-7 offline while the panel is dark");
}

static void standby_exit(void)
{
	if (!sb_on) return;
	sb_on = 0;
	for (int i = 1; i < SB_CPU; i++) {
		char p[96];
		snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/online", i);
		if (sb_online[i]) write_file(p, "1");
	}
	/* the policy of a cluster whose cores all went offline is made again with the kernel's defaults: write every
	 * saved value back after the cores are up */
	for (int i = 0; i < sb_npol; i++) {
		char v[240], n[16];
		snprintf(v, sizeof v, "%.150s/scaling_max_freq", sb_pol[i].dir);
		snprintf(n, sizeof n, "%d", sb_pol[i].max);
		write_file(v, n);
		snprintf(v, sizeof v, "%.150s/scaling_min_freq", sb_pol[i].dir);
		snprintf(n, sizeof n, "%d", sb_pol[i].min);
		write_file(v, n);
		snprintf(v, sizeof v, "%.150s/scaling_governor", sb_pol[i].dir);
		if (sb_pol[i].gov[0]) write_file(v, sb_pol[i].gov);
	}
	if (sb_gpu[0] && sb_gpu_gov[0]) {
		char g[200];
		snprintf(g, sizeof g, "%s/governor", sb_gpu);
		write_file(g, sb_gpu_gov);
		if (sb_gpu_max > 0) {
			char m[200], n[24];
			snprintf(m, sizeof m, "%s/max_freq", sb_gpu);
			snprintf(n, sizeof n, "%d", sb_gpu_max);
			write_file(m, n);
		}
	}
	sleep_log("standby: clocks and cores back");
}

static void suspend_now(const char *why)
{
	double t0 = mono();
	if (msc_rw()) {
		sleep_log("not sleeping (%s): USB transfer mode (the card is the computer's disk)", why);
		return;
	}
	if (sleep_too_low()) {
		/* Nothing can wake it on a timer: a deep sleep here is a handheld that only comes back if someone presses
		 * power. Leave the panel dark (what a sleep looks like) and stay awake, so the 60 s battery poll can still
		 * shut down cleanly. The next key press brings the panel back (see panel_blanked in handle()). */
		sleep_log("not sleeping (%s): %d%% and no RTC wake alarm: dark, but watching the battery", why,
		    battery_discharging_pct());
		/* a sleep that did not happen must still be quiet: SIGHUP stops the menu's ambient loop and releases the sound
		 * device (the next key's SIGUSR1 brings both back). A game's own sound is left alone. */
		pid_t ui = ui_pid();
		if (ui > 0 && !pid_of("/run/banana/game.pid")) kill(ui, SIGHUP);
		backlights_power(0);
		panel_blanked = 1;
		if (!pid_of("/run/banana/game.pid")) standby_enter();	/* a game left running keeps its clocks */
		last_input = mono();
		return;
	}
	pid_t game = pid_of("/run/banana/game.pid");
	pid_t pg = game ? getpgid(game) : 0;
	uint64_t epoch = next_retro_power_epoch();
	struct retro_owner owner;
	int owned = game && retro_capture_owner(retro_run_dir, game, &owner, RETRO_PUBLISH_LIVE);
	int native = retro_prepare_suspend(&retro_system_ops, game, pg, epoch, !skip("stop"), owned ? &owner : NULL);
	retro_suspend_prepared = native == RETRO_POWER_OK;
	retro_suspend_native = native != RETRO_POWER_NONE;
	if (!retro_suspend_prepared) sleep_save_state(game);
	/* Native clients stop at a safe point; other games retain the process-group stop. */
	/* the menu's ambient loop, if any: stop it and have it release the DSP before the ADSP sleeps (SIGHUP). Wait for
	 * the close it publishes to /run/banana/sound-state instead of guessing at a delay -- a PCM close still in flight
	 * when the DSP goes under is the one thing the overnight hang of 03:40 was seen doing (logs/sleep-state says
	 * "releasing (handover)" at the write of mem); then darken the panel and mute the codec so nothing flashes or pops.
	 * With a game resident the menu is still the display's owner when its compositor shows the game (the same test as
	 * the SIGUSR1 after the wake), so it turns the panel off then too; the sound handover is the menu's alone. */
	if (!skip("ui") && (!game || compositor_on())) {
		pid_t u = ui_pid();
		if (u > 0) {
			write_file("/run/banana/panel-state", "asked");
			kill(u, SIGHUP);
			double pms = 0;
			int poff = ui_panel_off_wait(&pms);
			if (poff == 1) sleep_log("menu: panel off in %.0f ms", pms);
			else if (poff == 0) sleep_log("menu: could not turn the panel off: a phantom wake may light it");
			else sleep_log("menu: no answer about the panel after %.0f ms", pms);
			if (!game) {
				double ms = 0;
				int rel = ui_release_wait(&ms);
				if (rel == 1) sleep_log("menu: released the sound device in %.0f ms", ms);
				else if (rel == 0) sleep_log("menu: the sound device was still open %.0f ms after the handover: sleeping anyway", ms);
				else sleep_us(200000);	/* a menu from before sound-state: the old fixed wait */
			}
		}
	}
	if (!skip("mute")) sleep_quiet_begin();
	if (!skip("hooks")) hooks("pre", 0);
	if (!skip("sync")) sync();
	sleep_mode();
	wake_only_power();	/* the power key (and the cable) may bring it back (see its comment) */
	double t_prep = mono();
	if (cfg.sleep_skip[0]) sleep_log("sleep %s: skipping '%s' (debug)", why, cfg.sleep_skip);
	int cycles = 0, rtc_wakes = 0, failures = 0, spurious = 0, phantoms = 0;
	int wk_busy = 0;
	int plugged0 = charger_online();
	int pct0 = battery_discharging_pct();	/* -1 on the charger: a draining sleep is the one worth measuring */
	long q0 = battery_charge_uah();
	double asleep = 0;
	const char *woke = "?";
	int woke_key = 0, woke_cable = 0;
	unsigned long wk0 = 0;
	for (;;) {
		diag_flush();
		time_t alarm = rtc_alarm((int)(cfg.sleep_check_hours * 3600));
		double t_in = mono();
		struct timespec b0, b1;
		clock_gettime(CLOCK_BOOTTIME, &b0);	/* BOOTTIME counts the time asleep, MONOTONIC doesn't */
		if (cycles == 0) sleep_log("sleep (%s): prep %.0f ms, %.0f ms to suspend%s", why,
		    (t_prep - t0) * 1000, (t_in - t0) * 1000, game ? ", game paused" : "");
		char mode[32] = "";
		read_str("/sys/power/mem_sleep", mode, sizeof mode);
		{	/* how many wakeup sources the kernel still has armed, for the note (3 with the power key and the cable) */
			FILE *wf = fopen("/sys/kernel/debug/wakeup_sources", "r");
			int n = 0;
			char ln[256];
			if (wf) { while (fgets(ln, sizeof ln, wf)) n++; fclose(wf); }
			sleep_note(why, mode[0] ? mode : cfg.mem_sleep, n > 0 ? n - 1 : 0);
		}
		wk0 = 0;
		{
			FILE *f = fopen("/sys/power/wakeup_count", "r");
			if (f) { if (fscanf(f, "%lu", &wk0) != 1) wk0 = 0; fclose(f); }
		}
		/* Write the count back before suspending: EBUSY means a wake event arrived after it was read, so a suspend
		 * now would wake at once with no cause. Abort, log it, and try again (a few times, then go anyway). */
		if (wk_busy < 5) {
			char cs[32];
			snprintf(cs, sizeof cs, "%lu", wk0);
			if (write_file("/sys/power/wakeup_count", cs) && errno == EBUSY) {
				wk_busy++;
				sleep_log("suspend aborted: a wake event was pending (wakeup_count %lu, try %d)", wk0, wk_busy);
				sleep_us(100000);
				continue;
			}
		}
		irq_nbefore = irq_snapshot(irq_before);
		int r = write_file("/sys/power/state", "mem");
		int err = errno;
		irq_nafter = irq_snapshot(irq_after);	/* before anything in userspace has had a turn */
		/* a signal (SIGHUP from the menu, SIGCHLD) interrupted the write: nothing slept, so retry rather than
		 * counting it as a failed suspend and falling back to s2idle */
		if (r && err == EINTR) { rtc_alarm(0); sleep_us(50000); continue; }
		clock_gettime(CLOCK_BOOTTIME, &b1);
		/* the RTC clears wakealarm when the alarm fires: set before, empty now, is the battery check's wake */
		char pending[32];
		read_str("/sys/class/rtc/rtc0/wakealarm", pending, sizeof pending);
		int rtc_fired = alarm && (!pending[0] || time(NULL) >= alarm);
		rtc_alarm(0);
		cycles++;
		if (r) {
			/* it didn't suspend: devices one at a time, then the lighter s2idle, then give up and blank the panel */
			failures++;
			sleep_log("suspend failed (%s)%s", strerror(err), sleep_fallback < 2 ? ": trying the safer way" : "");
			int released = RETRO_POWER_NONE;
			if (retro_suspend_failure_action(&retro_system_ops, retro_suspend_prepared,
						 retro_suspend_native, (unsigned)failures,
						 (unsigned)sleep_fallback, epoch, game, pg, &released)) {
				sleep_fallback++; sleep_mode(); sleep_us(200000); continue;
			}
			woke = "failed";
			/* It never slept: leave the panel dark -- that is what a sleep would have looked like, and the
			 * next press brings it back (see panel_blanked in handle()) -- and let the game run again, because
			 * a frozen emulator behind a dark screen is worse. Nothing here is a wake, so the wake bookkeeping
			 * below is skipped. */
			if (retro_suspend_prepared && released != RETRO_POWER_OK)
				sleep_log("native suspend abort acknowledgement failed; process-group recovery was attempted");
			retro_suspend_prepared = 0;
			retro_suspend_native = 0;
			sleep_quiet_end();
			backlights_power(0);
			/* the pre hooks have run (10-wifi took Wi-Fi down): run the post ones too, or a failed suspend would
			 * leave the handheld with no network until the next sleep succeeds */
			hooks("post", 1);
			panel_blanked = 1;
			if (!game) standby_enter();
			sleep_log("could not suspend after %d tries: the panel is dark until the next press", failures);
			last_input = mono();
			return;
		}
		asleep += (b1.tv_sec - b0.tv_sec) + (b1.tv_nsec - b0.tv_nsec) / 1e9;
		/* why it woke: the battery check's alarm, or the power button */
		double slept_now = (b1.tv_sec - b0.tv_sec) + (b1.tv_nsec - b0.tv_nsec) / 1e9;
		if (rtc_fired) {
			irq_report("rtc", slept_now);
			rtc_wakes++;
			int pct = battery_discharging_pct();
			if (pct >= 0 && pct <= cfg.sleep_poweroff_pct) {
				sleep_log("battery check: %d%% after %.1f h asleep: shutting down", pct, asleep / 3600);
				retro_suspend_resume_group(&retro_system_ops, retro_suspend_prepared, pg);
				/* the game gets SIGTERM next, to write its saves */
				shutdown_now("battery empty while asleep");
				return;
			}
			continue;	/* enough battery: back to sleep at once */
		}
		/* The pwrkey is the only armed wake source and its IRQ is glitchy (it fires on the release of the very press
		 * that asked for this sleep), so a wake with no real KEY_POWER press and no cable change is the panel lighting
		 * up for nothing: go back to sleep, a bounded number of times, and only then stay awake. */
		woke_key = wake_key_press();
		int cable = charger_online();
		woke_cable = cable && !plugged0;
		/* the cable going the other way is the charger's edge, not a phantom: nothing to stay awake for, and it is
		 * not one of the three tries a real phantom gets */
		if (plugged0 && !cable) {
			plugged0 = 0;
			if (!woke_key) {
				irq_report("unplug", slept_now);
				sleep_log("the charger was unplugged: back to sleep");
				continue;
			}
		}
		/* Two kinds of phantom. One comes seconds after the suspend (the pwrkey glitch above): those are bounded, so a
		 * press the key check missed still gets the handheld up by the fourth. The other comes after a long sleep,
		 * about hourly, with the kernel counting no wakeup event (the battery gauge's notifications are the suspect:
		 * one per percent fits the hour; the percentage is logged to check). Staying awake after the third of those
		 * lit the panel for five minutes several times a night, so a slow one always goes back to sleep. */
		if (!woke_key && !woke_cable) {
			double slept = (b1.tv_sec - b0.tv_sec) + (b1.tv_nsec - b0.tv_nsec) / 1e9;
			int pct = battery_pct_any();
			if (slept >= 60) {
				irq_report("slow-phantom", slept);
				diag_report("slow-phantom");
				phantoms++;
				sleep_log("wake with no key or cable after %.0f s asleep, battery %d%%: back to sleep (slow phantom %d)",
				    slept, pct, phantoms);
				continue;
			}
			if (spurious < 3) {
				irq_report("short-phantom", slept);
				diag_report("short-phantom");
				spurious++;
				sleep_log("wake with no key or cable after %.1f s asleep, battery %d%%: back to sleep (%d/3)",
				    slept, pct, spurious);
				continue;
			}
		}
		woke = woke_key ? "power button" : woke_cable ? "charger" : "phantom, the fourth in a row";
		irq_report(woke_key ? "key" : woke_cable ? "cable" : "give-up", slept_now);
		break;
	}
	/* awake: the game and the menu first. A fallback (devices one at a time, or s2idle) was for this sleep: the
	 * next one starts from the configured mode again instead of staying in the safer, higher-power one all session */
	sleep_fallback = 0;
	double t_wake = mono();
	backlights_power(1);
	sleep_quiet_end();
	retro_suspend_resume_group(&retro_system_ops, retro_suspend_prepared, pg);
	pid_t ui = ui_pid();
	if (ui && (!game || compositor_on())) kill(ui, SIGUSR1);	/* draws the panel again (the clock and the battery moved on); a resident game needs the re-modeset too */
	double t_back = mono();
	undim();
	hooks("post", 1);
	if (retro_suspend_prepared) {
		if (!sound_wait(3.0)) sleep_log("native audio service is not ready after wake; runtime reopen remains pending");
		int woke_ok = retro_suspend_complete(&retro_system_ops, 1, 1, 1, epoch, game, pg);
		if (woke_ok != RETRO_POWER_OK) {
			sleep_log("native game wake acknowledgement failed; terminating the tracked session");
		}
		retro_suspend_prepared = 0;
		retro_suspend_native = 0;
	} else if (retro_suspend_native) {
		if (!sound_wait(3.0)) sleep_log("native audio service is not ready after wake; runtime reopen remains pending");
		int reconciled = retro_suspend_complete(&retro_system_ops, 0, 1, 1, epoch, game, pg);
		if (reconciled == RETRO_POWER_FAILED) {
			sleep_log("native sleep reconciliation failed after wake; terminating the tracked session");
		}
		retro_suspend_native = 0;
	} else if (pg > 0 && !retro_suspend_native && !cfg.wake_audio_fast) {
		/* the game's stream died with the server when it was stopped for the sleep: the driver has to be restarted
		 * (AUDIO_REINIT, the same command the jack uses), or the game runs on without sound for good */
		if (sound_wait(3.0)) {
			if (ra_cmd("AUDIO_REINIT")) sleep_log("audio: the game's driver was reinitialised after the wake (fast %d)", cfg.wake_audio_fast);
			else sleep_log("audio: could not reinitialise the game's driver after the wake");
		} else {
			/* no sink to open yet: a reinit now would fail the same way and leave the game silent for good */
			sleep_log("audio: the server is not ready; the game's driver is retried from the loop");
			audio_retry_begin();
		}
	}
	/* the press that woke us comes in now: it is not a new request */
	drain_inputs();
	power_update();
	jack_settle_begin();	/* the codec can answer wrong right after a wake: confirm it in the loop, without blocking */
	ignore_power_until = mono() + 1.0;
	power_down = 0;
	last_input = mono();
	char wr[256], pw[96] = "";
	wake_reason(wr, sizeof wr, woke_key, woke_cable, wk0);
	/* what the sleep cost: the gauge's own percentage either side. %/h only makes sense unplugged and over a real
	 * stretch of sleep -- this is the number an "Android-like" sleep is judged by. */
	int pct1 = battery_discharging_pct();
	long q1 = battery_charge_uah();
	if (pct0 >= 0 && pct1 >= 0)
	{
		if (asleep >= 180 && q0 > 0 && q1 > 0)
			snprintf(pw, sizeof pw, ", battery %d%% -> %d%% (%.0f mAh in %.1f h, %.0f mA%s)", pct0, pct1,
				(q0 - q1) / 1000.0, asleep / 3600.0, (q0 - q1) / 1000.0 / (asleep / 3600.0),
				spurious || phantoms ? "" : ", no stray wake");
		else
			snprintf(pw, sizeof pw, ", battery %d%% -> %d%%", pct0, pct1);
	}
	sleep_log("awake (%s) after %.0f s asleep%s%s, %s, game and menu back %.1f ms after the kernel%s", woke, asleep,
		rtc_wakes ? ", battery checked" : "", asleep < 5.0 ? " (it did not take)" : "", wr, (t_back - t_wake) * 1000, pw);
	sleep_note_resume(wr);
}

static void on_poweroff_deadline(int sig)
{
	(void)sig;
	sync();
	reboot(RB_POWER_OFF);
}

static void retro_system_shutdown_finalize(void *data)
{
	(void)data;
	sync();
	sleep_quiet_begin();	/* dark and muted now: a slow unmount must not look like a handheld that is still on */
	/* if rcK hangs (an exFAT unmount), force the power off: SIGALRM runs on_poweroff_deadline */
	struct sigaction sa = { 0 };
	sa.sa_handler = on_poweroff_deadline;
	sigaction(SIGALRM, &sa, NULL);
	alarm(8);
	/* busybox init runs /etc/init.d/rcK (unmount, sync) on SIGUSR2 = poweroff */
	if (kill(1, SIGUSR2)) reboot(RB_POWER_OFF);
}

/* Create the shutdown latch under resume.lock. A publication or invalidation checks the latch and writes the
 * record while holding that lock, so the latch either exists before the check (the live writer is refused) or is
 * made after the writer's rename (the record it published is then the one shutdown sees). The wait is bounded like
 * every other resume.lock acquisition: a wedged lock holder must not stop a shutdown, so without the lock the latch
 * is created anyway and the failure is logged. rcK serializes its own latch the same way. */
static void retro_latch_shutdown(const char *directory, const char *run_dir)
{
	char path[PATH_MAX];
	int len = snprintf(path, sizeof path, "%s/shutting-down", run_dir);
	if (len <= 0 || (size_t)len >= sizeof path) return;
	int lock = retro_resume_lock(directory);
	if (lock < 0) sleep_log("shutdown latch created without resume.lock: %s", strerror(errno));
	int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
	if (fd >= 0) close(fd);
	else sleep_log("shutdown latch not created: %s", strerror(errno));
	if (lock >= 0) close(lock);
}

static void shutdown_now(const char *why)
{
	say("shutting down (%s)", why);
	/* The menu's game-exit path must not clear the marker written below: the game leaving is this shutdown, not a
	 * quit to the menu. A file, because the menu cannot see this process's state. */
	retro_latch_shutdown(retro_resume_dir, retro_run_dir);
	/* Capture the registered session before anything stops it. After the reap its leader is gone, so this is the
	 * only point at which the daemon can prove which session it is acting for. With no game, or none it can
	 * prove, nothing is published and nothing removed: the UI still owns any unsettled boot-resume record. */
	pid_t game = pid_of("/run/banana/game.pid");
	struct retro_owner owner;
	int owned = game && retro_capture_owner(retro_run_dir, game, &owner, RETRO_PUBLISH_SHUTDOWN);
	pid_t pg = game ? getpgid(game) : 0;
	uint64_t epoch = game ? next_retro_power_epoch() : 0;
	/* Marker, checkpoint/exit handshake, and final platform shutdown are one tested order. */
	(void)retro_shutdown_session(&retro_system_ops, game, pg, epoch, owned ? &owner : NULL);
}

static double save_volume_at, save_backlight_at;	/* debounced: written a moment after the last key */
static double volume_reassert_at;	/* once, after the sound server has opened the card (it sets its own level) */

/* ---- sound: the card that plays ----------------------------------------------------------------------------------- */
static int card_play = -1;	/* the card sound plays on; -1: none yet (or none at all) */
static int card_ar = -1;	/* the audioreach card (the RPC): the sound server routes its DSP graph from the UCM2 profile */

/* the card, from /proc/asound/cards (" 0 [sm8450 ]: simple-card - ...") */
/* The card and its route through the ALSA C mixer: no `amixer` fork. The DSP rebuilds its controls with each
 * frontend graph, so every look-up opens and loads a fresh mixer instead of holding a handle whose elements may
 * point at controls that no longer exist. */
static int mixer_open(int card, snd_mixer_t **mx)
{
	char dev[16];
	snprintf(dev, sizeof dev, "hw:%d", card);
	if (snd_mixer_open(mx, 0) < 0) return -1;
	if (snd_mixer_attach(*mx, dev) < 0 || snd_mixer_selem_register(*mx, NULL, NULL) < 0 || snd_mixer_load(*mx) < 0) {
		snd_mixer_close(*mx);
		*mx = NULL;
		return -1;
	}
	return 0;
}

/* A simple mixer element by its short name ("Left PCM"), the way `amixer` resolves one. */
static snd_mixer_elem_t *selem(snd_mixer_t *mx, const char *name)
{
	snd_mixer_selem_id_t *sid;
	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, name);
	return snd_mixer_find_selem(mx, sid);
}

/* The first channel's raw value: amixer's sget prints one percent per channel, and the menu has always shown the
 * first. */
static int selem_raw(snd_mixer_elem_t *e, long *v)
{
	for (int c = 0; c <= SND_MIXER_SCHN_LAST; c++)
		if (snd_mixer_selem_has_playback_channel(e, (snd_mixer_selem_channel_id_t)c) &&
		    snd_mixer_selem_get_playback_volume(e, (snd_mixer_selem_channel_id_t)c, v) == 0)
			return 0;
	return -1;
}

/* The amp's gain is 0.25 dB a step: raw 0 is -127.75 dB, 511 is 0 dB. A percent linear in the raw value put 30 % at
 * -89 dB, silence, with everything audible in the top quarter. So on that amp 1..100 % spans only the last 50 dB
 * (raw 311..511, linear in dB, which is what the ear hears as even steps) and 0 % is the raw 0 mute. Other mixers keep
 * amixer's own arithmetic. The `volume` script uses the same arithmetic, so the keys and Settings agree on a level. */
#define AMP_RAW_MAX 511
#define AMP_RAW_FLOOR 311	/* -50 dB: where 1 % sits */

static int amp_range(long min, long max) { return min == 0 && max == AMP_RAW_MAX; }

static int selem_percent(snd_mixer_elem_t *e)
{
	long min, max, v;
	if (snd_mixer_selem_get_playback_volume_range(e, &min, &max) < 0 || max <= min) return -1;
	if (selem_raw(e, &v) < 0) return -1;
	if (amp_range(min, max)) {
		if (v <= 0) return 0;
		if (v <= AMP_RAW_FLOOR) return 1;
		return 1 + (int)rint((double)(v - AMP_RAW_FLOOR) * 99.0 / (double)(AMP_RAW_MAX - AMP_RAW_FLOOR));
	}
	return (int)rint((double)(v - min) / (double)(max - min) * 100.0);
}

static int selem_set_percent(snd_mixer_elem_t *e, int pct)
{
	long min, max;
	if (snd_mixer_selem_get_playback_volume_range(e, &min, &max) < 0) return -1;
	if (amp_range(min, max)) {
		long raw = pct <= 0 ? 0 : AMP_RAW_FLOOR + (long)rint((double)(pct - 1) * (AMP_RAW_MAX - AMP_RAW_FLOOR) / 99.0);
		return snd_mixer_selem_set_playback_volume_all(e, raw);
	}
	long v = (long)rint((double)pct * (double)(max - min) * 0.01);
	if (v == 0 && pct > 0) v++;	/* amixer: a nonzero percent is never silence */
	return snd_mixer_selem_set_playback_volume_all(e, min + v);	/* all channels, like sset */
}

/* The elements the volume keys drive. On the RPC that is the two AW88166 gains ("PCM" in the config, "Left PCM" and
 * "Right PCM" in the mixer); a card that is the amplifier itself answers to the same pair. pct < 0 reads and returns
 * the first element's percent, otherwise every element is set. */
static int volume_apply(int pct)
{
	snd_mixer_t *mx = NULL;
	snd_mixer_elem_t *el[2] = { NULL, NULL };
	if (card_play < 0 || mixer_open(card_play, &mx) < 0) return -1;
	const char *mix = cfg.mixer;
	if (!strcmp(mix, "auto")) mix = card_ar >= 0 ? "PCM" : "Master";
	if (!strcmp(mix, "PCM")) {
		el[0] = selem(mx, "Left PCM");
		el[1] = selem(mx, "Right PCM");
	} else if (!(el[0] = selem(mx, mix))) {
		el[0] = selem(mx, "Left PCM");
		el[1] = selem(mx, "Right PCM");
	}
	int found = 0, bad = 0, out = -1;
	for (int i = 0; i < 2; i++) {
		if (!el[i]) continue;
		found++;
		if (pct < 0) {
			int p = selem_percent(el[i]);
			if (p >= 0 && out < 0) out = p;
		} else if (selem_set_percent(el[i], pct) < 0) {
			bad = 1;
		}
	}
	snd_mixer_close(mx);
	if (!found || bad) return -1;
	return pct < 0 ? out : 0;
}

static void audio_find_cards(void)
{
	FILE *f = fopen("/proc/asound/cards", "r");
	if (!f) return;
	char ln[256];
	while (fgets(ln, sizeof ln, f)) {
		int n;
		if (sscanf(ln, " %d [", &n) != 1) continue;
		card_play = n;
		/* the RPC's card: audioreach, with the frontends (MultiMedia1/2) mixed into the speaker I2S and the codec
		 * by mixer controls whose names say so -- what alsa-ucm-conf's profile would set. Nothing does it on this
		 * image, so without this the DSP says "no backend DAIs enabled" and plays nothing. */
		snd_mixer_t *mx = NULL;
		if (mixer_open(n, &mx) == 0) {
			for (snd_mixer_elem_t *e = snd_mixer_first_elem(mx); e; e = snd_mixer_elem_next(e))
				if (snd_mixer_elem_get_type(e) == SND_MIXER_ELEM_SIMPLE &&
				    strstr(snd_mixer_selem_get_name(e), "SENARY_MI2S_RX Audio Mixer")) { card_ar = n; break; }
			snd_mixer_close(mx);
		}
		break;	/* the first card is the one sound plays on */
	}
	fclose(f);
}

/* The amplifier's level across reboots: the two raw values, one control per line. (The card's whole asound.state was
 * 38 KB for the same two numbers, and `alsactl store` walked all 197 controls after every key press.) A rename, so a
 * power cut leaves the old level, not half a file. */
static void volume_save(void)
{
	FILE *f = fopen("/data/banana/volume.new", "w");
	if (!f) return;
	int n = 0;
	snd_mixer_t *mx = NULL;
	if (card_play >= 0 && mixer_open(card_play, &mx) == 0) {
		static const char *names[] = { "Left PCM", "Right PCM" };
		for (int i = 0; i < 2; i++) {
			snd_mixer_elem_t *e = selem(mx, names[i]);
			long v;
			if (!e || selem_raw(e, &v) < 0) continue;
			if (fprintf(f, "%s %ld\n", names[i], v) > 0) n++;
		}
		snd_mixer_close(mx);
	}
	int ok = n > 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
	fclose(f);
	if (ok) rename("/data/banana/volume.new", "/data/banana/volume");
	else unlink("/data/banana/volume.new");
}

/* Put that level back. With no file -- the first boot after the update -- the card's asound.state is replayed once
 * instead; from then on the small file above owns it. */
static void volume_restore(void)
{
	if (card_play < 0) audio_find_cards();
	FILE *f = fopen("/data/banana/volume", "r");
	if (!f) {
		/* The first boot after the update: the card's asound.state, replayed once. Writing the small file after it
		 * is what ends the dependency (and keeps the +5 s reassert from forking alsactl a second time). */
		if (access("/data/banana/asound.state", R_OK) == 0) {
			char *argv[] = { "alsactl", "-f", "/data/banana/asound.state", "restore", NULL };
			run(argv);
		}
		volume_save();
		return;
	}
	snd_mixer_t *mx = NULL;
	int have = card_play >= 0 && mixer_open(card_play, &mx) == 0;
	char ln[96];
	while (fgets(ln, sizeof ln, f)) {
		char *sp = strrchr(ln, ' ');	/* the name has spaces in it ("Left PCM"), the value is after the last */
		if (!sp) continue;
		*sp++ = 0;
		char *end = NULL;
		long v = strtol(sp, &end, 10);
		if (end == sp) continue;
		snd_mixer_elem_t *e = have ? selem(mx, ln) : NULL;
		if (e) snd_mixer_selem_set_playback_volume_all(e, v);
	}
	if (have) snd_mixer_close(mx);
	fclose(f);
}

/* Named CTL access (what `amixer cset/sset` did), through the ALSA C API: works for DAPM muxes and enums too, not
 * only the simple-mixer elements. */
static int ctl_open(int card, snd_ctl_t **ctl)
{
	char dev[16];
	snprintf(dev, sizeof dev, "hw:%d", card);
	return snd_ctl_open(ctl, dev, 0);
}

static int ctl_set(int card, const char *name, const char *val)
{
	snd_ctl_t *ctl = NULL;
	snd_ctl_elem_id_t *id = NULL;
	snd_ctl_elem_info_t *info = NULL;
	snd_ctl_elem_value_t *v = NULL;
	int r = -1;
	if (ctl_open(card, &ctl) < 0) return -1;
	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, name);
	snd_ctl_elem_id_set_index(id, 0);
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_info_set_id(info, id);
	if (snd_ctl_elem_info(ctl, info) < 0) {
		/* A simple-mixer switch is "<name> Switch" in the raw CTL space ("HPHL" is "HPHL Switch" here); amixer's
		 * sget/sset resolve that short name, the CTL API does not. Try the suffixed name before giving up, so
		 * bananad's late-card fallback (below) can actually set the codec's switches. */
		char alt[192];
		snprintf(alt, sizeof alt, "%s Switch", name);
		snd_ctl_elem_id_set_name(id, alt);
		snd_ctl_elem_info_set_id(info, id);
		if (snd_ctl_elem_info(ctl, info) < 0) goto out;
	}
	unsigned count = snd_ctl_elem_info_get_count(info);
	snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);
	snd_ctl_elem_value_alloca(&v);
	snd_ctl_elem_value_set_id(v, id);
	if (type == SND_CTL_ELEM_TYPE_BOOLEAN) {
		int on = !strcmp(val, "on") || !strcmp(val, "1");
		for (unsigned i = 0; i < count; i++) snd_ctl_elem_value_set_boolean(v, i, on);
	} else if (type == SND_CTL_ELEM_TYPE_INTEGER) {
		char *end = NULL;
		long x = strtol(val, &end, 10);
		if (end == val || *end) goto out;
		for (unsigned i = 0; i < count; i++) snd_ctl_elem_value_set_integer(v, i, x);
	} else if (type == SND_CTL_ELEM_TYPE_INTEGER64) {
		char *end = NULL;
		long long x = strtoll(val, &end, 10);
		if (end == val || *end) goto out;
		for (unsigned i = 0; i < count; i++) snd_ctl_elem_value_set_integer64(v, i, x);
	} else if (type == SND_CTL_ELEM_TYPE_ENUMERATED) {
		unsigned item = 0, items = snd_ctl_elem_info_get_items(info);
		int found = 0;
		for (; item < items; item++) {
			snd_ctl_elem_info_set_item(info, item);
			const char *nm = snd_ctl_elem_info_get_item_name(info);
			if (nm && !strcmp(nm, val)) { found = 1; break; }
		}
		if (!found) goto out;
		for (unsigned i = 0; i < count; i++) snd_ctl_elem_value_set_enumerated(v, i, item);
	} else goto out;
	if (snd_ctl_elem_write(ctl, v) < 0) goto out;
	r = 0;
out:
	snd_ctl_close(ctl);
	return r;
}

static void cset(int card, const char *name, const char *val)
{
	if (ctl_set(card, name, val)) say("sound: mixer set %s %s -> failed", name, val);
}

/* Is a control registered yet? The codec's controls appear with the SoundWire codec a moment after the card itself, so
 * a set before that fails with "no such control" (the boot that set them without waiting logged all 14 as failed and
 * left the headphone path unset). audio_setup waits on one of them, bounded, before the block that sets them. */
static int ctl_exists(int card, const char *name)
{
	snd_ctl_t *ctl = NULL;
	snd_ctl_elem_id_t *id = NULL;
	snd_ctl_elem_info_t *info = NULL;
	if (ctl_open(card, &ctl) < 0) return 0;
	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, name);
	snd_ctl_elem_id_set_index(id, 0);
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_info_set_id(info, id);
	int r = snd_ctl_elem_info(ctl, info) == 0;
	snd_ctl_close(ctl);
	return r;
}

/* the switch controls (HPHL, HPHL_RDAC, ...) are simple-mixer elements: the same CTL API reaches them */

static int mixer_get(int card, const char *name, char *out, size_t n)
{
	out[0] = 0;
	snd_ctl_t *ctl = NULL;
	snd_ctl_elem_id_t *id = NULL;
	snd_ctl_elem_info_t *info = NULL;
	snd_ctl_elem_value_t *v = NULL;
	if (ctl_open(card, &ctl) < 0) return 0;
	snd_ctl_elem_id_alloca(&id);
	snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_id_set_name(id, name);
	snd_ctl_elem_id_set_index(id, 0);
	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_info_set_id(info, id);
	if (snd_ctl_elem_info(ctl, info) == 0) {
		snd_ctl_elem_value_alloca(&v);
		snd_ctl_elem_value_set_id(v, id);
		if (snd_ctl_elem_read(ctl, v) == 0) {
			snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);
			if (type == SND_CTL_ELEM_TYPE_BOOLEAN)
				snprintf(out, n, "%s", snd_ctl_elem_value_get_boolean(v, 0) ? "on" : "off");
			else if (type == SND_CTL_ELEM_TYPE_INTEGER)
				snprintf(out, n, "%ld", snd_ctl_elem_value_get_integer(v, 0));
			else if (type == SND_CTL_ELEM_TYPE_INTEGER64)
				snprintf(out, n, "%lld", snd_ctl_elem_value_get_integer64(v, 0));
			else if (type == SND_CTL_ELEM_TYPE_ENUMERATED) {
				unsigned item = snd_ctl_elem_value_get_enumerated(v, 0);
				if (item < snd_ctl_elem_info_get_items(info)) {
					snd_ctl_elem_info_set_item(info, item);
					snprintf(out, n, "%s", snd_ctl_elem_info_get_item_name(info));
				}
			}
		}
	}
	snd_ctl_close(ctl);
	return (int)strlen(out);
}

/* A sleep: the panel dark and the codec's playback volumes muted before the ADSP's clock stops, so the AMOLED does not
 * flash and the amplifiers do not pop; both put back on the way out. The brightness levels are held here and not saved
 * to the card: the user's own level is untouched. */
static char sleep_bl_name[4][128];
static int sleep_bl_val[4], sleep_bl_n;
static char sleep_rx0[32], sleep_rx1[32];
static void sleep_quiet_begin(void)
{
	char bl[MAX_BL][64];
	int n = backlights(bl, 4);	/* sleep_bl_name[] holds four */
	sleep_bl_n = 0;
	for (int i = 0; i < n; i++) {
		char p[600], mx[600];
		bl_file(p, sizeof p, bl[i], "brightness");
		bl_file(mx, sizeof mx, bl[i], "max_brightness");
		int cur = read_int(p, -1);
		if (cur < 0) continue;
		/* A panel that is already dark (the menu's burn-in guard blanked it, or an earlier sleep) has no level of
		 * its own to come back to, and saving the 0 would wake the handheld to a black screen. Come back to the
		 * user's brightness instead; with nothing written down, leave the panel to the menu's guard. */
		if (cur == 0) {
			int pm = read_int("/data/banana/backlight", -1), max = read_int(mx, 0);
			cur = (pm > 0 && max > 0) ? max * pm / 1000 : -1;
			if (cur == 0) cur = 1;
		}
		if (cur <= 0) continue;
		snprintf(sleep_bl_name[sleep_bl_n], sizeof sleep_bl_name[0], "%.127s", bl[i]);
		sleep_bl_val[sleep_bl_n++] = cur;
		write_file(p, "0");
	}
	if (card_ar >= 0) {
		mixer_get(card_ar, "RX_RX0 Digital Volume", sleep_rx0, sizeof sleep_rx0);
		mixer_get(card_ar, "RX_RX1 Digital Volume", sleep_rx1, sizeof sleep_rx1);
		cset(card_ar, "RX_RX0 Digital Volume", "0");
		cset(card_ar, "RX_RX1 Digital Volume", "0");
	}
}
static void sleep_quiet_end(void)
{
	for (int i = 0; i < sleep_bl_n; i++) {
		char p[600], v[32];
		bl_file(p, sizeof p, sleep_bl_name[i], "brightness");
		snprintf(v, sizeof v, "%d", sleep_bl_val[i]);
		write_file(p, v);
	}
	sleep_bl_n = 0;
	if (card_ar >= 0) {
		if (sleep_rx0[0]) cset(card_ar, "RX_RX0 Digital Volume", sleep_rx0);
		if (sleep_rx1[0]) cset(card_ar, "RX_RX1 Digital Volume", sleep_rx1);
	}
	sleep_rx0[0] = sleep_rx1[0] = 0;
}

/* The sound server is stopped for a sleep and comes back in the background on the way up (sleep.d/20-sound). A game
 * resumed before it is up finds its audio stream gone for good (RetroArch does not reopen the device on its own), so
 * the wake waits for the server to accept real PCM parameters before reinitialising the game's audio driver. The
 * daemon has no session, so the PipeWire runtime dir has to be set here. Bounded; without a server (the host test)
 * there is nothing to wait for. */
static int sound_probe_once(void)
{
	snd_pcm_t *pc = NULL;
	int ok = 0;
	if (snd_pcm_open(&pc, "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) == 0) {
		ok = snd_pcm_set_params(pc, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 48000, 1, 15000) == 0;
		snd_pcm_close(pc);
	}
	return ok;
}

/* A wake whose sound server was not ready in time: the loop tries the probe again every 2 s (bounded) and
 * reinitialises the game's driver the moment it passes, instead of a single AUDIO_REINIT into a dead sink. */
static double audio_retry_at;
static int audio_retry_left;

static void audio_retry_begin(void)
{
	audio_retry_left = 5;
	audio_retry_at = mono() + 2.0;
}

static void audio_retry_poll(void)
{
	audio_retry_at = 0;
	if (sound_probe_once()) {
		sleep_log("audio: the server is back; %s", ra_cmd("AUDIO_REINIT") ? "the game's driver was reinitialised" : "could not reinitialise the game's driver");
		return;
	}
	if (--audio_retry_left > 0) audio_retry_at = mono() + 2.0;
	else sleep_log("audio: the server never came back; the game stays silent until the next route change");
}

static int sound_wait(double seconds)
{
	setenv("XDG_RUNTIME_DIR", "/run/banana/wayland", 1);
	setenv("PIPEWIRE_RUNTIME_DIR", "/run/banana/wayland", 1);
	double t = mono();
	int seen = 0;
	/* the server script says when the Speaker sink is in PipeWire (/run/banana/sound-ready, removed by "down"): a probe
	 * alone passes while the server is still starting, and the game's stream then fails with "Host is down" for good */
	if (access("/usr/libexec/banana/sound-server", X_OK) == 0) {
		while (access("/run/banana/sound-ready", F_OK) != 0) {
			if (mono() - t >= 8.0) { sleep_log("audio: the server is not ready after 8000 ms"); break; }
			sleep_us(50000);
		}
		t = mono();	/* the probe below gets its own bound: the sink just appeared, the first opens can still fail */
	}
	for (;;) {
		if (sound_probe_once()) return 1;
		if (!seen) {
			/* the server, or the hook that is still bringing it up (a sleep that did not take wakes within a second,
			 * before sound-server up has even started pipewire: giving up then left the game with no sound for good) */
			FILE *p = popen("pidof pipewire 2>/dev/null || ps 2>/dev/null | grep '[s]ound-server up'", "r");
			seen = p && fgetc(p) != EOF;
			if (p) pclose(p);
		}
		if (!seen && mono() - t >= 1.5) return 0;
		if (mono() - t >= seconds) { sleep_log("audio: the server is not back after %.0f ms", (mono() - t) * 1000); return 0; }
		sleep_us(100000);
	}
}

/* The jack's answer, confirmed without blocking the loop. The WCD937x's MBHC answers before it has measured the socket:
 * on the Retroid Pocket Classic the first read of a boot (and, worse, a first read after a wake) can say "headphones in"
 * with nothing plugged in, and acting on that routes the sound to a codec feeding an empty jack -- a silent handheld,
 * until something else writes the speaker route back. So the first reading is taken now and the main loop re-reads a
 * couple of hundred ms later; two agreeing readings are believed. A real plug-in still takes effect within a few hundred
 * ms, and its own switch event drives the same variable. (This used to sleep here, holding the loop for up to 1.2 s.) */
static double jack_settle_at;		/* when the loop takes the next reading (mono seconds; 0: none pending) */
static int jack_settle_val, jack_settle_tries;

static void jack_settle_begin(void)
{
	jack_settle_val = headphones_in();
	jack_settle_tries = 0;
	jack_settle_at = mono() + 0.2;
}

static void jack_settle_poll(void)
{
	jack_settle_at = 0;
	int v = headphones_in();
	if (v != jack_settle_val && ++jack_settle_tries < 6) {
		jack_settle_val = v;			/* not settled: look again in a moment */
		jack_settle_at = mono() + 0.2;
		return;
	}
	if (v != headphones) { headphones = v; audio_route(); }
}

static void audio_route(void)
{
	static int last = -1;
	// say it when the answer moves: on the RPC this is the switch between the speaker I2S and the codec, and a
	// jack that changes its mind with nothing plugged in is exactly what silences the handheld
	if (card_ar >= 0 && last != headphones)
		say("sound: routing to the %s (jack read %s)", headphones ? "headphones" : "speakers",
		    headphones_in() ? "in" : "out");
	last = headphones;
	/* The audioreach card plays at 48 kHz (the rate the DSP graph and the amps expect), so "default"'s playback and
	 * capture are pinned to S16 stereo 48 kHz: every open gets the same stream instead of renegotiating it, and
	 * ALSA's plug resamples a core that asks for another rate. */
	/* Everything that opens ALSA's "default" -- the menu, the launchers, the games -- goes to the sound server
	 * (PipeWire), which owns the card through ACP and the UCM2 profile in /usr/share/alsa/ucm2. The routing and
	 * the levels are ACP's then, so the jack switch is a device choice there, not an "hw:N" here. bananad still
	 * writes this file because /etc/asound.conf points at it and every app opens it afresh. */
	char conf[256];
	int n;
	n = snprintf(conf, sizeof conf,
			"# bananad: sound plays through the server (PipeWire)\n"
			"pcm.!default { type pipewire }\n"
			"ctl.!default { type pipewire }\n");
	/* snprintf returns what it would have written, not what it did: clamp before the buffer goes to fwrite, or a
	 * truncated (or negative) n would read past conf */
	if (n < 0) n = 0;
	if ((size_t)n >= sizeof conf) n = (int)sizeof conf - 1;
	FILE *f = fopen("/run/banana/asound.conf.new", "w");
	if (f) {
		fwrite(conf, 1, (size_t)n, f);
		fclose(f);
		rename("/run/banana/asound.conf.new", "/run/banana/asound.conf");
	}
	/* the control the volume keys change, for the settings screen's Volume (the volume script's sset) */
	f = fopen("/run/banana/mixer.new", "w");
	if (f) {
		const char *mix = cfg.mixer;
		if (!strcmp(mix, "auto"))
			/* On the RPC the volume that is *heard* is the two AW88166 amplifiers' own gain -- the kernel
			 * exposes them as "Left PCM"/"Right PCM", where 0 is mute and 511 is 0 dB, the loudest -- not the
			 * DSP's per-stream volume, which the graph latches when it is instantiated, so changing it
			 * mid-stream does nothing. "PCM" is the name the volume script expands to those two controls; it
			 * drives both. */
			mix = card_ar >= 0 ? "PCM" : "Master";
		fprintf(f, "%d %s\n", card_play, mix);
		fclose(f);
		rename("/run/banana/mixer.new", "/run/banana/mixer");
	}
	/* a running RetroArch game opens its sound again where it plays now: alsa-lib reads its configuration once per
	 * process (Banana's RetroArch patch forgets it on AUDIO_REINIT). The menu's own sound watches the file itself. */
	ra_cmd("AUDIO_REINIT");
	say("sound: playback on card %d (%s), 48 kHz", card_play, headphones ? "headphones" : "speakers");
}

/* Is this the Retroid Pocket Classic? Its AudioReach card is the only one whose mixer controls (the frontends'
 * "SENARY_..." switches) appear a moment after the card binds; on any other machine there is nothing to wait for and
 * waiting would only stall the start. Read once. */
static int on_rpc(void)
{
	static int r = -1;
	if (r < 0) {
		r = 0;
		FILE *f = fopen("/proc/device-tree/compatible", "rb");
		if (f) {
			char b[256];
			size_t n = fread(b, 1, sizeof b - 1, f);
			fclose(f);
			b[n] = 0;
			for (char *p = b; p < b + n; p++) if (!*p) *p = ' ';
			if (strstr(b, "sm4450") || strstr(b, "sm8450")) r = 1;
		}
	}
	return r;
}

static void audio_setup(void)
{
	audio_find_cards();
	/* rcS starts us right after the card appears and the DSP makes its audioreach controls a moment later, so on the
	 * RPC look again for a few seconds rather than give the route and the "PCM" level up on the first look. On any
	 * other machine there is no such control to wait for, and waiting would only stall the start. */
	for (int i = 0; on_rpc() && card_ar < 0 && i < 40; i++) { sleep_us(100000); audio_find_cards(); }
	if (card_play < 0) { audio_route(); return; }	/* no sound card at all (QEMU, a host with none): nothing to set */
	if (card_ar < 0) say("sound: no audioreach card (the volume keys have nothing to drive)");
	else say("sound: the audioreach card is %d", card_ar);
	audio_route();
	/* The RPC's DSP latches its routes when a graph is instantiated, and the sound server (ACP + the UCM2 profile)
	 * owns them now. The codec's own DAPM switches below are still re-asserted here, belt-and-braces: bananad runs
	 * after rcS and after restore_settings() replays asound.state, and a card that enumerated late would otherwise
	 * run at the reset levels -- silent or very quiet -- all session. Then the amps' level -- not the frontend's
	 * `stream0.vol_ctrl0 ...`, which is the user's volume (the volume keys and the saved setting own it). */
	if (card_ar >= 0) {
		/* One line every boot, because this codec's jack is what decides where sound goes and it is known to answer
		 * before it has measured anything: "headphones in" here with nothing plugged in is the whole story of a
		 * silent menu -- the sound server's device choice follows the same stale answer. */
		say("sound: the jack says %s at start", headphones ? "headphones in" : "nothing in");
		jack_log();	/* the per-device switch, next to the answer bananad acts on */
		/* The codec's DAPM controls (the switches and muxes below) can be registered a moment after the card's
		 * simple-mixer controls, so a set before that fails ("no such control") and the headphone path is left
		 * unset. Wait for one of them, bounded, and say so if it never appears. */
		int have = 0;
		for (int i = 0; i < 25 && !(have = ctl_exists(card_ar, "RX_MACRO RX0 MUX")); i++) sleep_us(200000);
		if (!have) say("sound: the codec's mux controls are not up: the headphone path may be unset");
		/* The codec's and the RX macro's output volumes and the mic's: the UCM2 profile's own BootSequence levels,
		 * re-asserted here in case the card enumerated after rcS ran (a late card would otherwise sit at the reset
		 * levels -- silent or very quiet -- all session). */
		cset(card_ar, "RX_RX0 Digital Volume", "84");
		cset(card_ar, "RX_RX1 Digital Volume", "84");
		cset(card_ar, "HPHL Volume", "20");
		cset(card_ar, "HPHR Volume", "20");
		cset(card_ar, "ADC2 Volume", "10");
		/* The amplifiers' level is the user's volume (the volume keys and Settings > Volume own it, and
		 * bananad's own restore_settings() has just put the saved level back). Only a card that has no saved
		 * level -- a fresh one -- gets Banana's starting point; setting it here every boot was what made the
		 * volume jump back to it. */
		if (access("/data/banana/volume", R_OK) != 0 && access("/data/banana/asound.state", R_OK) != 0) {
			cset(card_ar, "Left PCM Playback Volume", "460");
			cset(card_ar, "Right PCM Playback Volume", "460");
		}
		/* The headphone path, set here rather than only in rcS: bananad's restore_settings() replays the saved
		 * level after rcS has run, which puts these back to their reset state, so rcS's own copy is not enough.
		 * The speaker path (SENARY MI2S -> AW88166) is separate and does not need them. */
		cset(card_ar, "HPHL", "on");
		cset(card_ar, "HPHR", "on");
		cset(card_ar, "HPHL_RDAC", "on");
		cset(card_ar, "HPHR_RDAC", "on");
		cset(card_ar, "HPHL_COMP", "on");
		cset(card_ar, "HPHR_COMP", "on");
		cset(card_ar, "RX HPH Mode", "CLS_H_LOHIFI");
		cset(card_ar, "RX_HPH PWR Mode", "LOHIFI");
		cset(card_ar, "RX_MACRO RX0 MUX", "AIF1_PB");
		cset(card_ar, "RX_MACRO RX1 MUX", "AIF1_PB");
		cset(card_ar, "RX INT0_1 MIX1 INP0", "RX0");
		cset(card_ar, "RX INT1_1 MIX1 INP0", "RX1");
		cset(card_ar, "RX INT0 DEM MUX", "CLSH_DSM_OUT");
		cset(card_ar, "RX INT1 DEM MUX", "CLSH_DSM_OUT");
	}
}

/* ---- the bridge: one line into a running game (docs/superpowers/specs/2026-11-05-banana-bridge-design.md) ---- */

/* shortcut: the bridge carries its own three-language table instead of the JSON catalogs; when a fourth language
 * lands, fold these four strings into the catalogs and generate the table at boot. */
enum { HUD_VOLUME, HUD_BRIGHT, HUD_LOW, HUD_EMPTY, HUD_NKEYS };
static const char *const hud_strings[3][HUD_NKEYS] = {
	{ "BN V %d", "BN B %d", "Battery low: %d %%", "Battery empty, shutting down" },
	{ "BN V %d", "BN B %d", "Batería baja: %d %%", "Batería vacía, apagando" },
	{ "BN V %d", "BN B %d", "Akku schwach: %d %%", "Akku leer, fährt herunter" },
};

static void hud_fmt(int key, int value, char *buf, size_t n)
{
	int l = !strcmp(cfg.lang, "es") ? 1 : !strcmp(cfg.lang, "de") ? 2 : 0;
	if (key == HUD_EMPTY) snprintf(buf, n, "%s", hud_strings[l][key]);
	else snprintf(buf, n, hud_strings[l][key], value);
}

/* the bridge speaks only to a game: in the menu banana-ui draws the pill, and nothing here writes /run/banana/osd.
 * The four paths are static so --selftest cheevos can point them at a scratch directory. */
static const char *game_pid_path = "/run/banana/game.pid";
static const char *cheevos_path = "/run/banana/cheevos";
static const char *hud_live_path = "/run/banana/hud-live";
static const char *ra_path = "/run/banana/retroarch.cmd";

static int hud_on(void) { return cfg.hud && pid_of(game_pid_path) > 0; }

static int hud_show(const char *text)
{
	/* under banana-ui's compositor the card on the overlay plane is the one place it shows (RetroArch's own message
	 * box would say it a second time); with the compositor off it is RetroArch's message box */
	if (compositor_on()) { if (cfg.hud) overlay_show("hud", text, "", "", 3000); return 0; }
	if (!hud_on()) return 0;
	char line[256];
	snprintf(line, sizeof line, "SHOW_MSG %s", text);
	return ra_cmd(line);
}

/* one line of /run/banana/cheevos: unlocked, total, title, description, points, rarity, badge. Fills out with what
 * the bridge shows ("Sonic (+15)") and returns 1; 0 when the line is not one (fewer than 7 fields, no title).
 * A title cannot carry a tab or a newline (they are the record's own separators), so nothing is escaped here. */
static int cheevos_line(const char *line);

static int ino_cheev = -1;	/* the achievement file's own watch: the directory only reports its creation */
static long cheevos_off;	/* where the last read stopped; a shrunken file means a new game */

static void cheevos_watch(int ino)
{
	if (ino < 0) return;
	if (ino_cheev >= 0) inotify_rm_watch(ino, ino_cheev);
	ino_cheev = inotify_add_watch(ino, cheevos_path, IN_MODIFY | IN_CLOSE_WRITE);
}

/* Shows every line appended since the last call and returns how many the game was actually sent: the menu skips its
 * own toasts only for those, so a line that could not be delivered still reaches the player after the game. */
static int cheevos_poll(void)
{
	int shown = 0;
	FILE *f = fopen(cheevos_path, "r");
	if (!f) { cheevos_off = 0; return 0; }
	if (fseek(f, 0, SEEK_END) || ftell(f) < cheevos_off) cheevos_off = 0;	/* a new game's file starts empty */
	fseek(f, cheevos_off, SEEK_SET);
	char ln[1024];
	while (fgets(ln, sizeof ln, f)) {
		cheevos_off += (long)strlen(ln);
		if (!cheevos_line(ln)) continue;
		if (hud_on()) {
			shown++;
			FILE *m = fopen(hud_live_path, "w");	/* the menu must not toast it again */
			if (m) fclose(m);
		} else if (compositor_on()) {
			char c[1024];
			snprintf(c, sizeof c, "%s", ln);
			size_t len = strlen(c);
			while (len && (c[len - 1] == '\n' || c[len - 1] == '\r')) c[--len] = 0;
			char *f[7];
			int nf = 0;
			f[nf++] = c;
			for (char *p = c; *p && nf < 7; ++p)
				if (*p == '\t') { *p = 0; f[nf++] = p + 1; }
			if (nf == 7 && f[2][0]) overlay_show("achievement", f[2], f[3], f[6], 4000);
		}
	}
	fclose(f);
	return shown;
}

static int cheevos_line(const char *line)
{
	char c[1024];
	snprintf(c, sizeof c, "%s", line);
	size_t len = strlen(c);
	while (len && (c[len - 1] == '\n' || c[len - 1] == '\r')) c[--len] = 0;
	char *f[7];
	int nf = 0;
	f[nf++] = c;
	for (char *p = c; *p && nf < 7; ++p)
		if (*p == '\t') { *p = 0; f[nf++] = p + 1; }
	return nf == 7 && f[2][0];
}

/* --selftest hud: runs no daemon and touches nothing; only the formatter and the achievement parser, so the bridge's
 * text can be checked on a workstation */
struct retro_power_mock {
	unsigned calls[32];
	unsigned stops, continues;
	uint64_t epoch;
	unsigned owners;
	int service_alive, checkpoint_ok, child_alive;
	uint16_t lose_ack_type;
	char order[64];
	size_t order_size;
	unsigned waits, terminate_after_waits;
	int term_requested;
	int child_stopped, identity_mismatch, renderer_alive;
	int resume_marked;
	int battery_only, marker_fail;
};

static void retro_mock_record(struct retro_power_mock *mock, char operation)
{
	if (mock->order_size + 1 < sizeof(mock->order)) {
		mock->order[mock->order_size++] = operation;
		mock->order[mock->order_size] = 0;
	}
}

static int retro_mock_request(void *data, uint16_t type, uint64_t epoch, unsigned timeout_ms)
{
	struct retro_power_mock *mock = data;
	if (!timeout_ms || type >= 32 || (!epoch && type != BANANA_RETRO_CTL_CHECKPOINT)) return RETRO_POWER_FAILED;
	++mock->calls[type];
	if (mock->child_stopped || mock->identity_mismatch || !mock->renderer_alive)
		return RETRO_POWER_NONE;
	if (!mock->service_alive) return RETRO_POWER_FAILED;
	int result = RETRO_POWER_OK;
	if (type == BANANA_RETRO_CTL_PREPARE_SLEEP) {
		if (epoch <= mock->epoch || !mock->checkpoint_ok) result = RETRO_POWER_FAILED;
		else { mock->epoch = epoch; mock->owners |= 2u; retro_mock_record(mock, 'P'); }
	} else if (type == BANANA_RETRO_CTL_SLEEP_ABORT || type == BANANA_RETRO_CTL_WAKE) {
		if (epoch != mock->epoch || !(mock->owners & 2u)) result = RETRO_POWER_BAD_STATE;
		else { mock->owners &= ~2u; retro_mock_record(mock, type == BANANA_RETRO_CTL_WAKE ? 'W' : 'A'); }
	} else if (type == BANANA_RETRO_CTL_SHUTDOWN) {
		if (epoch <= mock->epoch || !mock->checkpoint_ok) result = RETRO_POWER_FAILED;
		else { mock->epoch = epoch; retro_mock_record(mock, 'X'); }
	}
	/* Model a completed mutation whose response was delayed past the caller's deadline. */
	if (result == RETRO_POWER_OK && type == mock->lose_ack_type) {
		mock->lose_ack_type = 0;
		return RETRO_POWER_FAILED;
	}
	return result;
}

/* the publish mode the last record write used; no_token: an OK reply with no digest, which is battery-only unless
 * `ambiguous` says the runtime could not identify it as one (the compatibility retry, or another ROM's launcher) */
static int retro_mock_last_mode = -1, retro_mock_no_token, retro_mock_ambiguous;
static int retro_mock_request_proof(void *data, pid_t game, const struct retro_owner *owner, uint16_t type,
				    uint64_t epoch, unsigned timeout_ms, struct retro_resume_proof *proof)
{
	int result = retro_mock_request(data, type, epoch, timeout_ms);
	if (result == RETRO_POWER_OK) {
		struct retro_power_mock *mock = data;
		strcpy(proof->system, "gba");
		strcpy(proof->core, "mgba");
		if (!owner || retro_mock_no_token || (type == BANANA_RETRO_CTL_CHECKPOINT && mock->battery_only)) {
			proof->token[0] = '\0';	/* no owner: the real client discards the token */
			proof->battery_only = !retro_mock_ambiguous &&
				((type == BANANA_RETRO_CTL_CHECKPOINT && mock->battery_only) ||
				 (type == BANANA_RETRO_CTL_SHUTDOWN && retro_mock_no_token));
		} else memset(proof->token, 'a', BANANA_RETRO_RESUME_TOKEN_SIZE);
	}
	return result;
}

static int retro_mock_signal(void *data, pid_t pg, int signal_number)
{
	struct retro_power_mock *mock = data;
	if (pg <= 0) return -1;
	if (signal_number == SIGSTOP) { ++mock->stops; retro_mock_record(mock, 'S'); }
	else if (signal_number == SIGCONT) { ++mock->continues; retro_mock_record(mock, 'C'); }
	else if (signal_number == SIGTERM) { mock->term_requested = 1; retro_mock_record(mock, 'T'); }
	return 0;
}

static int retro_mock_child_alive(void *data, pid_t game)
{ return game > 0 && ((struct retro_power_mock *)data)->child_alive; }

static void retro_mock_wait_ms(void *data, unsigned milliseconds)
{
	struct retro_power_mock *mock = data;
	if (milliseconds != 100) return;
	++mock->waits;
	if (mock->terminate_after_waits && mock->waits >= mock->terminate_after_waits &&
	    (mock->term_requested || mock->calls[BANANA_RETRO_CTL_SHUTDOWN]))
		mock->child_alive = 0;
}

static int retro_mock_resume_marker(void *data, const struct retro_owner *owner, int mode,
				    const struct retro_resume_proof *proof)
{
	struct retro_power_mock *mock = data;
	retro_mock_last_mode = mode;
	mock->resume_marked = !mock->marker_fail && owner && proof && !strcmp(proof->system, "gba") &&
		!strcmp(proof->core, "mgba") && retro_hex(proof->token, BANANA_RETRO_RESUME_TOKEN_SIZE);
	retro_mock_record(mock, mock->resume_marked ? 'M' : 'E');
	return mock->resume_marked;
}

static int retro_mock_resume_invalidate(void *data, const struct retro_owner *owner, int mode)
{
	struct retro_power_mock *mock = data;
	retro_mock_last_mode = mode;
	retro_mock_record(mock, 'I');
	return owner && !mock->marker_fail;
}

static int retro_mock_legacy_save(pid_t game)
{ (void)game; return 0; }

static void retro_mock_shutdown_finalize(void *data)
{ retro_mock_record(data, 'F'); }

static int selftest_retro_lifecycle(void)
{
	struct retro_power_mock mock = {
		.service_alive=1, .checkpoint_ok=1, .child_alive=1, .renderer_alive=1
	};
	struct retro_power_ops ops = {
		.data = &mock, .request = retro_mock_request, .signal_group = retro_mock_signal,
		.request_proof = retro_mock_request_proof,
		.child_alive = retro_mock_child_alive, .wait_ms = retro_mock_wait_ms,
		.resume_marker = retro_mock_resume_marker, .resume_invalidate = retro_mock_resume_invalidate,
		.shutdown_finalize = retro_mock_shutdown_finalize
	};
	int failed = 0;
	struct retro_owner owner = { .session = "0123456789abcdef0123456789abcdef", .pid = 100, .start = 7,
				     .rom = "/game.rom" };
#define CHECK(name, expr) do { int ok = (expr); printf("retro lifecycle %s: %s\n", (name), ok ? "ok" : "FAIL"); if (!ok) failed = 1; } while (0)
	CHECK("start-time identity", retro_child_identity_valid(100, 500, 500, 100, 100, 'S', 0) &&
	      !retro_child_identity_valid(100, 500, 501, 100, 100, 'S', 0) &&
	      !retro_child_identity_valid(100, 500, 500, 100, 100, 'T', 0) &&
	      retro_child_identity_valid(100, 500, 500, 100, 100, 'T', 1) &&
	      retro_child_identity_valid(100, 500, 500, 100, 100, 't', 1) &&
	      !retro_child_identity_valid(100, 500, 501, 100, 100, 'T', 1) &&
	      !retro_child_identity_valid(100, 500, 500, 100, 100, 'Z', 1) &&
	      !retro_child_identity_valid(100, 500, 500, 100, 100, 'X', 1));
	/* Production prepare/wake transitions preserve an independently held UI overlay. */
	mock.owners = 1u;
	int prepared = retro_prepare_suspend(&ops, 100, 100, 41, 1, NULL);
	CHECK("overlay prepare", prepared == RETRO_POWER_OK && mock.stops == 0 &&
	      mock.owners == 3u && strcmp(mock.order, "P") == 0);
	int aborted = retro_suspend_complete(&ops, 1, 1, 0, 41, 100, 100);
	CHECK("suspend abort", aborted == RETRO_POWER_OK && mock.continues == 0 &&
	      mock.owners == 1u && strcmp(mock.order, "PA") == 0);
	mock.owners = 0;
	prepared = retro_prepare_suspend(&ops, 100, 100, 42, 1, NULL);
	CHECK("native suspend wake", prepared == RETRO_POWER_OK && mock.owners == 2u);
	int woke = retro_suspend_complete(&ops, 1, 1, 1, 42, 100, 100);
	CHECK("native wake", woke == RETRO_POWER_OK && mock.owners == 0 &&
	      strcmp(mock.order, "PAPW") == 0);

	/* A delayed/lost ack can follow a real pause; ABORT reconciles it before group fallback. */
	mock.lose_ack_type = BANANA_RETRO_CTL_PREPARE_SLEEP;
	prepared = retro_prepare_suspend(&ops, 100, 100, 43, 1, NULL);
	CHECK("late prepare acknowledgement", prepared == RETRO_POWER_FAILED &&
	      mock.owners == 0 && mock.stops == 1 && strstr(mock.order, "PAS") != NULL);
	(void)retro_suspend_complete(&ops, 0, 1, 0, 43, 100, 100);
	CHECK("process-group recovery", mock.continues == 1);
	CHECK("duplicate and stale epoch", retro_mock_request(&mock, BANANA_RETRO_CTL_PREPARE_SLEEP, 43, 100) ==
	      RETRO_POWER_FAILED && retro_mock_request(&mock, BANANA_RETRO_CTL_WAKE, 42, 100) == RETRO_POWER_BAD_STATE);

	/* Renderer/service loss falls back to the group; stopped and stale children cannot use native control. */
	mock.service_alive = 0;
	prepared = retro_prepare_suspend(&ops, 100, 100, 44, 1, NULL);
	CHECK("service loss fallback", prepared == RETRO_POWER_FAILED && mock.stops == 2);
	mock.service_alive = 1;
	mock.renderer_alive = 0;
	prepared = retro_prepare_suspend(&ops, 100, 100, 44, 1, NULL);
	CHECK("renderer loss fallback", prepared == RETRO_POWER_NONE && mock.stops == 3);
	mock.renderer_alive = 1;
	mock.child_stopped = 1;
	prepared = retro_prepare_suspend(&ops, 100, 100, 44, 1, NULL);
	CHECK("stopped child fallback", prepared == RETRO_POWER_NONE && mock.stops == 4);
	mock.child_stopped = 0;
	mock.identity_mismatch = 1;
	prepared = retro_prepare_suspend(&ops, 100, 100, 44, 1, NULL);
	CHECK("stale PID fallback", prepared == RETRO_POWER_NONE && mock.stops == 5);
	mock.identity_mismatch = 0;

	/* Checkpoint failure leaves no native hold and takes the process-group path. */
	mock.checkpoint_ok = 0;
	prepared = retro_prepare_suspend(&ops, 100, 100, 45, 1, NULL);
	CHECK("failed checkpoint", prepared == RETRO_POWER_FAILED && mock.stops == 6);
	mock.checkpoint_ok = 1;
	prepared = retro_prepare_suspend(&ops, 100, 100, 46, 1, NULL);
	mock.service_alive = 0;
	woke = retro_suspend_complete(&ops, 1, 1, 1, 46, 100, 100);
	CHECK("failed wake recovery", prepared == RETRO_POWER_OK && woke == RETRO_POWER_FAILED &&
	      mock.term_requested && mock.child_alive);
	mock.service_alive = 1;
	mock.term_requested = 0;
	prepared = retro_prepare_suspend(&ops, 100, 100, 47, 1, NULL);
	mock.lose_ack_type = BANANA_RETRO_CTL_SLEEP_ABORT;
	woke = retro_suspend_complete(&ops, 0, 1, 1, 47, 100, 100);
	CHECK("ambiguous abort recovery", prepared == RETRO_POWER_OK && woke == RETRO_POWER_FAILED &&
	      mock.owners == 0 && mock.term_requested);
	mock.term_requested = 0;
	prepared = retro_prepare_suspend(&ops, 100, 100, 52, 1, NULL);
	int released = RETRO_POWER_NONE;
	int retry1 = retro_suspend_failure_action(&ops, 1, 1, 1, 0, 52, 100, 100, &released);
	int retry2 = retro_suspend_failure_action(&ops, 1, 1, 2, 1, 52, 100, 100, &released);
	int retry3 = retro_suspend_failure_action(&ops, 1, 1, 3, 2, 52, 100, 100, &released);
	CHECK("kernel failure retry and abort ordering", prepared == RETRO_POWER_OK && retry1 && retry2 &&
	      !retry3 && released == RETRO_POWER_OK && mock.owners == 0);

	mock.service_alive = 1;
	mock.checkpoint_ok = 1;
	mock.child_alive = 1;
	mock.term_requested = 0;
	mock.waits = 0;
	mock.terminate_after_waits = 2;
	CHECK("direct shutdown marker order and reap", retro_shutdown_session(&ops, 100, 100, 53,
	      &owner) == RETRO_POWER_OK && mock.resume_marked && !mock.child_alive &&
	      !mock.term_requested && mock.waits == 2 && strstr(mock.order, "XMF") != NULL &&
	      retro_mock_last_mode == RETRO_PUBLISH_SHUTDOWN);
	mock.child_alive = 1;
	mock.waits = 0;
	mock.terminate_after_waits = 1;
	mock.checkpoint_ok = 0;
	CHECK("shutdown checkpoint failure fallback", retro_shutdown_session(&ops, 100, 100, 54,
	      &owner) == RETRO_POWER_FAILED && !mock.child_alive && mock.term_requested &&
	      mock.waits == 1 && mock.resume_marked && strstr(mock.order, "XMFTF") != NULL);
	mock.checkpoint_ok = 1;
	mock.term_requested = 0;
	mock.child_alive = 1;
	mock.waits = 0;
	mock.terminate_after_waits = 3;
	mock.service_alive = 0;
	CHECK("shutdown process-group fallback", retro_shutdown_session(&ops, 100, 100, 55,
	      &owner) ==
	      RETRO_POWER_FAILED && !mock.child_alive && mock.term_requested && mock.waits == 3);
	mock.child_alive = 1;
	mock.waits = 0;
	mock.term_requested = 0;
	mock.terminate_after_waits = 0;
	CHECK("shutdown bounded wait", retro_shutdown_session(&ops, 100, 100, 56,
	      &owner) ==
	      RETRO_POWER_FAILED && mock.child_alive && mock.waits == 50 && mock.term_requested);
	mock.child_alive = 0;
	mock.resume_marked = 1;
	(void)retro_shutdown_session(&ops, 0, 0, 0, NULL);
	CHECK("menu shutdown preserves unsettled resume marker", mock.resume_marked);
	/* A shutdown that could not prove which session it stops publishes nothing and removes nothing. */
	mock.service_alive = 1; mock.child_alive = 0; mock.waits = 0; mock.terminate_after_waits = 0;
	mock.order_size = 0; mock.order[0] = 0;
	CHECK("shutdown without a provable owner touches no record", retro_shutdown_session(&ops, 100, 100, 57,
	      NULL) == RETRO_POWER_OK && strcmp(mock.order, "XF") == 0);
	/* A runtime that committed no checkpoint at shutdown invalidates only the ending session's own record. */
	mock.order_size = 0; mock.order[0] = 0; retro_mock_no_token = 1;
	CHECK("shutdown without a checkpoint invalidates through the owner only", retro_shutdown_session(&ops, 100, 100,
	      58, &owner) == RETRO_POWER_OK && strcmp(mock.order, "XIF") == 0 &&
	      retro_mock_last_mode == RETRO_PUBLISH_SHUTDOWN);
	/* The same empty reply, when the runtime did not identify it as a battery-only commit, removes nothing. */
	mock.order_size = 0; mock.order[0] = 0; retro_mock_ambiguous = 1;
	CHECK("an ambiguous shutdown reply invalidates nothing", retro_shutdown_session(&ops, 100, 100,
	      59, &owner) == RETRO_POWER_OK && strcmp(mock.order, "XF") == 0);
	retro_mock_ambiguous = 0;
	retro_mock_no_token = 0;

	/* A held sleep acknowledged with a proof publishes the matching boot record. */
	mock.service_alive = 1; mock.checkpoint_ok = 1; mock.child_alive = 1; mock.renderer_alive = 1;
	mock.owners = 0; mock.resume_marked = 0; mock.order_size = 0; mock.order[0] = 0;
	prepared = retro_prepare_suspend(&ops, 100, 100, 60, 1, &owner);
	CHECK("suspend publishes proof", prepared == RETRO_POWER_OK && mock.resume_marked &&
	      strcmp(mock.order, "PM") == 0 && retro_mock_last_mode == RETRO_PUBLISH_LIVE);
	(void)retro_suspend_complete(&ops, 1, 1, 1, 60, 100, 100);
	/* Without a known ROM there is nothing to name: no record is written. */
	mock.resume_marked = 0; mock.order_size = 0; mock.order[0] = 0;
	prepared = retro_prepare_suspend(&ops, 100, 100, 61, 1, NULL);
	CHECK("suspend without rom writes no record", prepared == RETRO_POWER_OK && !mock.resume_marked &&
	      strcmp(mock.order, "P") == 0);
	(void)retro_suspend_complete(&ops, 1, 1, 1, 61, 100, 100);
	/* A record that cannot be written leaves whatever record exists: no blanket removal follows. */
	mock.order_size = 0; mock.order[0] = 0; mock.marker_fail = 1;
	prepared = retro_prepare_suspend(&ops, 100, 100, 62, 1, &owner);
	CHECK("suspend record failure removes nothing", prepared == RETRO_POWER_OK &&
	      strcmp(mock.order, "PE") == 0);
	mock.marker_fail = 0;
	(void)retro_suspend_complete(&ops, 1, 1, 1, 62, 100, 100);
	/* A checkpoint that fails keeps the process-group path and publishes nothing. */
	mock.order_size = 0; mock.order[0] = 0; mock.checkpoint_ok = 0;
	prepared = retro_prepare_suspend(&ops, 100, 100, 63, 1, &owner);
	CHECK("suspend failure publishes no record", prepared == RETRO_POWER_FAILED &&
	      strchr(mock.order, 'M') == NULL);
	mock.checkpoint_ok = 1;

	/* The runtime answers OK with no digest when it committed battery data only. */
	mock.order_size = 0; mock.order[0] = 0; mock.battery_only = 1;
	CHECK("battery-only checkpoint invalidates through the owner",
	      session_checkpoint(&ops, 100, &owner, retro_mock_legacy_save) == RETRO_CHECKPOINT_BATTERY_ONLY &&
	      !mock.resume_marked && strcmp(mock.order, "I") == 0 && retro_mock_last_mode == RETRO_PUBLISH_LIVE);
	mock.order_size = 0; mock.order[0] = 0; mock.child_alive = 0;
	CHECK("battery-only checkpoint after exit leaves record",
	      session_checkpoint(&ops, 100, &owner, retro_mock_legacy_save) == RETRO_CHECKPOINT_FAILED &&
	      mock.order[0] == 0);
	mock.child_alive = 1; mock.marker_fail = 1;
	CHECK("battery-only record removal failure is reported",
	      session_checkpoint(&ops, 100, &owner, retro_mock_legacy_save) == RETRO_CHECKPOINT_FAILED);
	mock.marker_fail = 0; mock.battery_only = 0;
	mock.order_size = 0; mock.order[0] = 0; mock.service_alive = 0;
	CHECK("checkpoint transport failure leaves record",
	      session_checkpoint(&ops, 100, &owner, retro_mock_legacy_save) == RETRO_CHECKPOINT_FAILED &&
	      mock.order[0] == 0);
	mock.service_alive = 1;
	CHECK("checkpoint with proof publishes record",
	      session_checkpoint(&ops, 100, &owner, retro_mock_legacy_save) == RETRO_CHECKPOINT_MARKED &&
	      mock.resume_marked && strcmp(mock.order, "M") == 0);
	/* A shutdown whose record cannot be written removes nothing. */
	mock.order_size = 0; mock.order[0] = 0; mock.marker_fail = 1;
	mock.child_alive = 0; mock.waits = 0; mock.terminate_after_waits = 0;
	CHECK("shutdown record failure removes nothing", retro_shutdown_session(&ops, 100, 100, 70,
	      &owner) == RETRO_POWER_OK && strcmp(mock.order, "XEF") == 0);
	mock.marker_fail = 0;
#undef CHECK
	return failed ? 1 : 0;
}

static int selftest_hud(void)
{
	char b[160];
	hud_fmt(HUD_VOLUME, 62, b, sizeof b); printf("hud en: %s\n", b);
	hud_fmt(HUD_BRIGHT, 80, b, sizeof b); printf("hud en: %s\n", b);
	hud_fmt(HUD_LOW, 9, b, sizeof b); printf("hud en: %s\n", b);
	hud_fmt(HUD_EMPTY, 0, b, sizeof b); printf("hud en: %s\n", b);
	snprintf(cfg.lang, sizeof cfg.lang, "es");
	hud_fmt(HUD_VOLUME, 62, b, sizeof b); printf("hud es: %s\n", b);
	snprintf(cfg.lang, sizeof cfg.lang, "de");
	hud_fmt(HUD_LOW, 9, b, sizeof b); printf("hud de: %s\n", b);
	snprintf(cfg.lang, sizeof cfg.lang, "en");
	int hud = cfg.hud;
	cfg.hud = 0; printf("hud off: %d\n", hud_on());
	cfg.hud = 1; printf("hud no game: %d\n", hud_on());
	cfg.hud = hud;
	printf("cheevos 4 fields: %d\n", cheevos_line("1\t2\tSonic\tDesc"));
	printf("cheevos 6 fields: %d\n", cheevos_line("1\t10\tSonic\tSpin fast\t15\t5"));
	return 0;
}

/* --selftest cheevos DIR: the poll's own state machine over a scratch directory -- a new game's empty file, an
 * append, a shorter file, and the two ways a line must not be marked as shown. */
static int selftest_cheevos(const char *dir)
{
	char pid[512], ce[512], hl[512], cmd[512];
	snprintf(pid, sizeof pid, "%s/game.pid", dir);
	snprintf(ce, sizeof ce, "%s/cheevos", dir);
	snprintf(hl, sizeof hl, "%s/hud-live", dir);
	snprintf(cmd, sizeof cmd, "%s/cmd", dir);
	game_pid_path = pid; cheevos_path = ce; hud_live_path = hl; ra_path = cmd;
	FILE *f = fopen(pid, "w");
	if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
	f = fopen(cmd, "w");	/* a regular file stands in for the FIFO: opening it is all ra_cmd asks of it */
	if (f) fclose(f);

	unlink(ce); unlink(hl);
	printf("cheevos absent: %d %d\n", cheevos_poll(), access(hl, F_OK) == 0);

	f = fopen(ce, "w");
	if (f) { fputs("1\t2\tOne\tFirst\t10\t0\t\n1\t2\tTwo\tSecond\t5\t0\t\n", f); fclose(f); }
	int n = cheevos_poll();
	printf("cheevos two: %d %d\n", n, access(hl, F_OK) == 0);

	unlink(hl);
	f = fopen(ce, "a");
	if (f) { fputs("1\t2\tThree\tThird\t1\t0\t\n", f); fclose(f); }
	n = cheevos_poll();
	printf("cheevos append: %d %d\n", n, access(hl, F_OK) == 0);

	unlink(hl);
	f = fopen(ce, "w");	/* the next game's file replaces it and is shorter than the offset was */
	if (f) { fputs("1\t2\tFour\tFourth\t1\t0\t\n", f); fclose(f); }
	n = cheevos_poll();
	printf("cheevos shrink: %d %d\n", n, access(hl, F_OK) == 0);

	cfg.hud = 0;
	unlink(hl);
	f = fopen(ce, "a");
	if (f) { fputs("1\t2\tFive\tFifth\t1\t0\t\n", f); fclose(f); }
	n = cheevos_poll();
	printf("cheevos off: %d %d\n", n, access(hl, F_OK) == 0);
	cfg.hud = 1;

	return 0;
}

static void volume(int dir)
{
	/* One volume, and it is the amplifiers' -- the system's, the way ROCKNIX's keys drive the default sink. The
	 * keys change the same control whether the menu or a game is on top, and a game plays through it, so there is
	 * nothing for "in a game" to mean differently. The DSP's per-stream gain cannot be this (a frontend's graph
	 * latches it at the open), and RetroArch's own volume is not the system's either: a game left at 150% would
	 * come back there however quiet the menu had been. */
	int pct = volume_apply(-1);
	if (pct < 0) return;
	pct += dir > 0 ? cfg.volume_step : -cfg.volume_step;
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	if (volume_apply(pct) < 0) return;
	save_volume_at = mono() + 2;
	volume_reassert_at = 0;	/* the user's level now, not the boot one */
	/* The level is shown by whoever owns the panel: the menu gets its pill (banana-ui's OSD, which it is drawing),
	 * a game gets the same number in RetroArch's message box, written to the FIFO the launcher already hears. The
	 * volume itself is one amplifier level either way. */
	/* over banana-ui's compositor (menu or game) the level pill on the overlay plane; with the compositor off a game gets the line
	 * in RetroArch's message box. "On-screen info while playing" off keeps a game's screen clear either way. */
	int game = pid_of("/run/banana/game.pid") > 0;
	if (game && !compositor_on()) { char b[96]; hud_fmt(HUD_VOLUME, pct, b, sizeof b); hud_show(b); }
	else if (!game || cfg.hud) osd("volume", pct);
}

/* Below the backlight's lowest level the picture itself is scaled: banana-ui folds this percent into the panel's GAMMA_LUT
 * (ui/src/shader_mask.h colorLutFill). /run/banana/pic-scale is the state (the menu's slider resets it to 100); the
 * level is saved beside the backlight and put back at boot. */
static const int pic_steps[] = { 100, 70, 50, 35 };
#define PIC_N ((int)(sizeof pic_steps / sizeof pic_steps[0]))

static int pic_index(void)
{
	int v = read_int("/run/banana/pic-scale", 100);
	int best = 0;
	if (v < 1) return 0;	/* a stray 0 is no scale, not the darkest one */
	for (int i = 0; i < PIC_N; i++)
		if (v <= pic_steps[i]) best = i;
	return best;
}

static int pic_publish(int idx)
{
	FILE *f = fopen("/run/banana/pic-scale.new", "w");
	if (!f) { say("pic-scale: can't write"); return -1; }
	int bad = fprintf(f, "%d\n", pic_steps[idx]) < 0;
	if (fclose(f) != 0 || bad) { say("pic-scale: write failed"); return -1; }
	if (rename("/run/banana/pic-scale.new", "/run/banana/pic-scale") != 0) {
		say("pic-scale: rename failed");
		return -1;
	}
	return 0;
}

static void brightness(int dir, int step_pct, int may_scale)
{
	char bl[MAX_BL][64];
	int n = backlights(bl, MAX_BL), pct = -1;
	int pic = may_scale ? pic_index() : 0, at_floor = n > 0, scaled = 0;
	/* up first undoes the picture scale; the backlight stays where it is */
	if (dir > 0 && pic > 0) {
		if (pic_publish(pic - 1) == 0) { pic--; scaled = 1; }
		n = 0;
	}
	for (int i = 0; i < n; i++) {
		char p[320], v[32];
		bl_file(p, sizeof p, bl[i], "max_brightness");
		int max = read_int(p, 0);
		bl_file(p, sizeof p, bl[i], "brightness");
		int cur = read_int(p, -1);
		if (cur < 0) { at_floor = 0; cur = 0; }	/* unreadable: the level is unknown, not at the floor */
		int step = max * step_pct / 100;
		if (step < 1) step = 1;
		int floor = max * 2 / 100;	/* a night-mode floor: 2%, dim but never a dead-looking panel */
		if (floor < 1) floor = 1;
		int nv = cur + dir * step;
		if (cur > floor) at_floor = 0;
		if (nv < floor) nv = floor;
		if (nv > max) nv = max;
		snprintf(v, sizeof v, "%d", nv);
		write_file(p, v);
		if (max > 0) pct = nv * 100 / max;
	}
	/* a down step that found every backlight already at its floor goes on into the picture scale */
	if (dir < 0 && may_scale && at_floor && pic < PIC_N - 1) {
		if (pic_publish(pic + 1) == 0) { pic++; scaled = 1; }
	}
	if (scaled) {
		pct = pic > 0 ? 1 : 2;	/* the card has no fractions: a scaled picture reads 1%, the lit floor 2% */
	}
	save_backlight_at = mono() + 2;
	if (pct >= 0) {
		int game = pid_of("/run/banana/game.pid") > 0;
		if (game && !compositor_on()) {
			char b[96];
			hud_fmt(HUD_BRIGHT, pct, b, sizeof b);
			hud_show(b);
		} else if (!game || cfg.hud)
			osd("brightness", pct);
	}
}

/* both backlights at a fraction of their maximum */
static void backlight_set_permille(int pm)
{
	char bl[MAX_BL][64];
	int n = backlights(bl, MAX_BL);
	for (int i = 0; i < n; i++) {
		char p[320], v[32];
		bl_file(p, sizeof p, bl[i], "max_brightness");
		int max = read_int(p, 0);
		if (max <= 0) continue;
		int nv = max * pm / 1000;
		if (nv < 1) nv = 1;
		bl_file(p, sizeof p, bl[i], "brightness");
		snprintf(v, sizeof v, "%d", nv);
		write_file(p, v);
	}
}

/* the first backlight's level, in thousandths of its maximum */
static int backlight_permille(void)
{
	char bl[MAX_BL][64];
	int n = backlights(bl, MAX_BL), r = -1;
	for (int i = 0; i < n && r < 0; i++) {
		char p[320];
		bl_file(p, sizeof p, bl[i], "max_brightness");
		int max = read_int(p, 0);
		bl_file(p, sizeof p, bl[i], "brightness");
		if (max > 0) r = read_int(p, 0) * 1000 / max;
	}
	return r;
}

/* the panel's backlight, from the backlight-path script. The RPC has one panel and one backlight, and the script
 * returns it whatever name it is asked for ("panel" here). While banana-ui is managing its own idle dim (it writes
 * /run/banana/panel-ui, see ui/src/main.cpp), the menu's own dim leaves that backlight to it: it would otherwise
 * dim twice and undo banana-ui's restore. Only the name is kept: the empty string means nothing to single out. */
static void find_panel_bl(void)
{
	FILE *p = popen("/usr/libexec/banana/backlight-path panel 2>/dev/null", "r");
	if (!p) return;
	char path[256] = "";
	if (fgets(path, sizeof path, p)) path[strcspn(path, "\n")] = 0;
	pclose(p);
	if (!path[0] || strncmp(path, "/sys/class/backlight/", 21)) return;
	const char *s = strrchr(path, '/');
	if (!s || !s[1]) return;
	snprintf(panel_bl, sizeof panel_bl, "%s", s + 1);
	/* published for banana-ui, whose idle timer dims the panel itself (ui/src/main.cpp) */
	FILE *f = fopen("/run/banana/backlight-panel.new", "w");
	if (f) {
		fprintf(f, "%s\n", path);
		fclose(f);
		rename("/run/banana/backlight-panel.new", "/run/banana/backlight-panel");
	}
	say("panel backlight: %s", panel_bl);
}

/* idle in the menu: the panel down to dim_percent of what it was, back at the first press (the level is
 * remembered here, not saved: the user's brightness is untouched) */
static int dimmed, dim_count;
static char dim_name[4][256];
static int dim_level[4];
static void dim(void)
{
	char bl[MAX_BL][64];
	int n = backlights(bl, 4);	/* dim_name[] holds four */
	dim_count = 0;
	for (int i = 0; i < n; i++) {
		if (panel_bl[0] && !strcmp(bl[i], panel_bl) && !access("/run/banana/panel-ui", F_OK)) continue;
		char p[600], v[32];
		snprintf(dim_name[dim_count], sizeof dim_name[0], "%.255s", bl[i]);
		bl_file(p, sizeof p, dim_name[dim_count], "brightness");
		int cur = read_int(p, -1);
		if (cur < 0) continue;
		dim_level[dim_count++] = cur;
		int nv = cur * cfg.dim_pct / 100;
		if (nv < 1) nv = 1;
		snprintf(v, sizeof v, "%d", nv);
		write_file(p, v);
	}
	dimmed = dim_count > 0;
	/* the level before dimming, for the settings screen (opened by the press that undims) */
	if (dimmed) {
		char p[600];
		bl_file(p, sizeof p, dim_name[0], "max_brightness");
		int max = read_int(p, 0);
		if (max > 0) { char v[16]; snprintf(v, sizeof v, "%d\n", dim_level[0] * 1000 / max); write_file("/run/banana/dim", v); }
	}
}
static void undim(void)
{
	if (!dimmed) return;
	for (int i = 0; i < dim_count; i++) {
		char p[600], v[32];
		bl_file(p, sizeof p, dim_name[i], "brightness");
		snprintf(v, sizeof v, "%d", dim_level[i]);
		write_file(p, v);
	}
	dimmed = 0;
	unlink("/run/banana/dim");
}

static void save_settings(double now)
{
	if (save_backlight_at > 0 && now >= save_backlight_at) {
		save_backlight_at = 0;
		int pm = backlight_permille();
		if (pm >= 0) {	/* no backlight (or no readable max): keep the last good level */
			char v[16];
			snprintf(v, sizeof v, "%d\n", pm);
			FILE *f = fopen("/data/banana/backlight", "w");
			if (f) { fputs(v, f); fclose(f); }
		}
		FILE *g = fopen("/data/banana/bl-scale", "w");
		if (g) { fprintf(g, "%d\n", pic_steps[pic_index()]); fclose(g); }
	}
	if (save_volume_at > 0 && now >= save_volume_at) {
		save_volume_at = 0;
		volume_save();
	}
}

static void restore_settings(void)
{
	int pm = read_int("/data/banana/backlight", -1);
	if (pm > 0 && pm <= 1000) backlight_set_permille(pm);
	{
		int sc = read_int("/data/banana/bl-scale", 100);
		if (sc < 100 && sc > 0) {
			FILE *f = fopen("/run/banana/pic-scale", "w");
			if (f) {
				if (fprintf(f, "%d\n", sc) < 0) say("pic-scale: restore write failed");
				if (fclose(f) != 0) say("pic-scale: restore close failed");
			}
		}
	}
	/* started again while the panel was dimmed (/run/banana/dim: the level before): undimmed, as the last one would */
	int dimmed_from = read_int("/run/banana/dim", -1);
	if (dimmed_from > 0 && dimmed_from <= 1000) { backlight_set_permille(dimmed_from); unlink("/run/banana/dim"); }
	volume_restore();
}

/* the battery: its directory under /sys/class/power_supply, capacity and whether it charges */
static int battery_state(char *name, size_t n, int *pct, int *charging)
{
	DIR *d = opendir("/sys/class/power_supply");
	if (!d) return 0;
	struct dirent *e;
	int found = 0;
	while (!found && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[320], s[32];
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
		read_str(p, s, sizeof s);
		if (strcmp(s, "Battery")) continue;
		snprintf(name, n, "%s", e->d_name);
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
		*pct = read_int(p, -1);
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
		read_str(p, s, sizeof s);
		*charging = !strcmp(s, "Charging");
		found = 1;
	}
	closedir(d);
	return found;
}

/* a charger plugged in: a power supply that isn't the battery (USB, Mains) and is online. The battery's own status is
 * no use (the amber charger light on ROCKNIX's handhelds lit and went out again): the gauge says "Charging" only for a
 * moment after the cable goes in, then "Discharging" or "Not charging" while it still charges. */
static int charger_online(void)
{
	DIR *d = opendir("/sys/class/power_supply");
	if (!d) return 0;
	struct dirent *e;
	int on = 0;
	while (!on && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[320], s[32];
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
		read_str(p, s, sizeof s);
		if (!strcmp(s, "Battery")) continue;
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/online", e->d_name);
		on = read_int(p, 0) > 0;
	}
	closedir(d);
	return on;
}

/* ---- the vibrator ----------------------------------------------------------------------------------------------- */
static int vib_fd = -1;		/* the FF input, held open: an effect belongs to the fd that uploaded it */
static int vib_id = -1;		/* the uploaded FF_RUMBLE effect, reused between pulses (no per-pulse re-upload) */
static int vib_warned;		/* "no FF input" said once, not on every cable */
static double vib_off_at;	/* when the running pulse stops (mono seconds; 0: none) */
static void vib_open(void);
static void vib_stop(void);

/* The vibrator is the PMIC's PM7250B motor, a module in the image (CONFIG_INPUT_PM8XXX_VIBRATOR=m) that nothing loads
 * at boot. modprobe it once so the FF input (pm8xxx_vib_ffmemless) exists; already loaded is a no-op. */
static void vibrator_load(void)
{
	char *argv[] = { "modprobe", "pm8xxx_vibrator", NULL };
	run(argv);
	vib_open();
}

/* The FF input, held open: an effect belongs to the fd that uploaded it, so opening and closing per pulse (or letting
 * another holder take the device) is how a pulse silently does nothing. The device's name carries "vib"
 * (pm8xxx_vib_ffmemless) and its event number depends on what else probed that boot, so the number does not matter. */
static void vib_open(void)
{
	if (vib_fd >= 0) return;
	DIR *d = opendir("/dev/input");
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "event", 5)) continue;
		char p[300], name[64] = "";
		snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
		int f = open(p, O_RDWR | O_CLOEXEC);
		if (f < 0) continue;
		ioctl(f, EVIOCGNAME(sizeof name), name);
		if (strstr(name, "vib")) { vib_fd = f; vib_id = -1; say("vibrator: %s", e->d_name); break; }
		close(f);
	}
	closedir(d);
	if (vib_fd < 0 && !vib_warned) { vib_warned = 1; say("vibrator: no FF input found"); }
}

/* a short pulse on the vibrator: the FF_RUMBLE effect is uploaded once and reused (id >= 0 updates it in place), played,
 * and stopped on the main loop's own clock (vib_off_at) -- never with a sleep, which would stall input and the
 * charger edge for the pulse's length */
static void vibrate(int ms)
{
	vib_open();
	if (vib_fd < 0) return;
	if (skip("vib")) { say("vibrator: pulse suppressed (sleep_skip)"); return; }	/* debug: prove the edge without buzzing */
	struct ff_effect fx;
	memset(&fx, 0, sizeof fx);
	fx.type = FF_RUMBLE;
	fx.id = vib_id;			/* -1 the first time (allocate); its own id after (update) */
	fx.replay.length = (unsigned short)ms;
	fx.u.rumble.strong_magnitude = 0xffff;
	fx.u.rumble.weak_magnitude = 0xffff;
	if (ioctl(vib_fd, EVIOCSFF, &fx) < 0) {
		/* the device went away (module reloaded) or another holder took the effect: start the fd over once */
		say("vibrator: EVIOCSFF failed (%s), reopening", strerror(errno));
		close(vib_fd);
		vib_fd = -1;
		vib_id = -1;
		vib_warned = 0;
		vib_open();
		if (vib_fd < 0) return;
		fx.id = -1;
		if (ioctl(vib_fd, EVIOCSFF, &fx) < 0) { say("vibrator: upload failed (%s)", strerror(errno)); return; }
	}
	vib_id = fx.id;
	struct input_event ev;
	memset(&ev, 0, sizeof ev);
	ev.type = EV_FF;
	ev.code = fx.id;
	ev.value = 1;
	if (write(vib_fd, &ev, sizeof ev) != (ssize_t)sizeof ev) { say("vibrator: play failed (%s)", strerror(errno)); return; }
	vib_off_at = mono() + ms / 1000.0;
}

/* stop a running pulse; called from the main loop when vib_off_at comes due */
static void vib_stop(void)
{
	if (vib_fd < 0 || vib_id < 0) return;
	struct input_event ev;
	memset(&ev, 0, sizeof ev);
	ev.type = EV_FF;
	ev.code = vib_id;
	ev.value = 0;
	if (write(vib_fd, &ev, sizeof ev) < 0) {}
}

/* The power state: a battery reading, the charger's edge (the plug pulse) and one log line when either moves. The
 * handheld has no LEDs, so there is nothing to drive; this runs on the power_supply uevents and the battery poll. */
static void power_update(void)
{
	char bat[256] = "";
	int pct = -1, charging = 0;
	if (!battery_state(bat, sizeof bat, &pct, &charging)) pct = 100;
	int plugged = charger_online();
	static int prev_plugged = -1;
	if (prev_plugged >= 0 && prev_plugged != plugged) {
		say("charger %s", plugged ? "plugged in" : "unplugged");	/* banana-ui shows the notice (menu and game) */
	}
	if (prev_plugged == 0 && plugged) vibrate(cfg.vibrate_ms);	/* the cable went in (not at boot, nor on unplug) */
	prev_plugged = plugged;
	if (plugged) charging = 1;	/* plugged in counts as charging (see charger_online) */
	static int last_pct = -1, last_chg = -1;
	if (pct != last_pct || charging != last_chg) {
		last_pct = pct;
		last_chg = charging;
		say("battery %d%%%s", pct, charging ? ", charging" : "");
	}
}

/* the kernel's uevents (no udev here): the battery and the charger, and drivers for USB devices plugged in */
static int uevent_open(void)
{
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	if (fd < 0) return -1;
	struct sockaddr_nl a = { .nl_family = AF_NETLINK, .nl_groups = 1 };
	if (bind(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
	return fd;
}

/* a program in the background, not waited for (a double fork: no zombie, SIGCHLD stays default) */
static void spawn_bg(char *const argv[])
{
	pid_t p = fork();
	if (p == 0) {
		if (fork() == 0) { execv(argv[0], argv); _exit(127); }
		_exit(0);
	}
	if (p > 0) while (waitpid(p, NULL, 0) < 0 && errno == EINTR) {}
}

/* a USB device's driver, as udev would load it: the module its MODALIAS names (an Xbox pad's xpad, a USB serial
 * adapter's); the handheld's own hardware is built into the kernel */
static void load_driver(const char *alias)
{
	if (strncmp(alias, "usb:", 4)) return;
	char a[256];
	snprintf(a, sizeof a, "%s", alias);
	char *argv[] = { "/sbin/modprobe", "-q", a, NULL };
	spawn_bg(argv);
}

/* the USB devices there before bananad started (a pad plugged in at boot) */
static void load_drivers_present(void)
{
	DIR *d = opendir("/sys/bus/usb/devices");
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[320], alias[256];
		snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/modalias", e->d_name);
		FILE *f = fopen(p, "r");
		if (!f) continue;
		if (fgets(alias, sizeof alias, f)) { alias[strcspn(alias, "\n")] = 0; load_driver(alias); }
		fclose(f);
	}
	closedir(d);
}

/* the second SD card: mounted, and its games offered to the library, to match the slot (/usr/libexec/banana/sd2).
 * In the background (a slow card's mount mustn't hold up the power button or sleep), one scan at a time: a card
 * going in sends several uevents */
static void sd2_scan(void)
{
	char *argv[] = { "/bin/sh", "-c", "flock /run/banana/sd2.lock /usr/libexec/banana/sd2 scan </dev/null >/dev/null 2>&1 &", NULL };
	run(argv);
}

/* the kernel's uevents: true when one was about the battery or the charger; a device added gets its driver */
static int uevent_read(int fd)
{
	char buf[8192];
	ssize_t n;
	int hit = 0;
	while ((n = recv(fd, buf, sizeof buf - 1, 0)) > 0) {
		buf[n] = 0;
		const char *alias = NULL;
		int add = 0, block = 0, mmc = 0, udc = 0;
		for (char *p = buf; p < buf + n; p += strlen(p) + 1) {
			if (!strcmp(p, "SUBSYSTEM=power_supply")) hit = 1;
			else if (!strcmp(p, "SUBSYSTEM=block")) block = 1;
			else if (!strcmp(p, "SUBSYSTEM=udc")) udc = 1;
			else if (!strncmp(p, "DEVNAME=mmcblk", 14)) mmc = 1;
			else if (!strcmp(p, "ACTION=add")) add = 1;
			else if (!strncmp(p, "MODALIAS=", 9)) alias = p + 9;
		}
		if (add && alias) load_driver(alias);
		if (block && mmc) sd2_scan();	/* a card in or out of the second slot (the script looks at what's there) */
		if (add && udc && !msc_rw()) {	/* an OTG role switch back to peripheral: rcS's one-shot run cannot see this */
			/* one role switch can send several add events, and two usb-gadget runs at once tear the gadget
			 * down; one run every couple of seconds is plenty */
			static double last_udc;
			double u = mono();
			if (u - last_udc >= 2.0) {
				last_udc = u;
				char *argv[] = { "/usr/libexec/banana/usb-gadget", NULL };
				spawn_bg(argv);
			}
		}
	}
	return hit;
}

/* Discover available input devices. */
static void open_inputs(void)
{
	for (int i = 0; i < ndev; i++) close(devfd[i]);
	ndev = 0;
	memset(devdrop, 0, sizeof devdrop);
	DIR *d = opendir("/dev/input");
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) && ndev < MAXDEV) {
		if (strncmp(e->d_name, "event", 5)) continue;
		char path[300];
		snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) continue;
		/* only keys and switches reach us (handle() reads nothing else): the touchscreens' and the sticks' motion during
		 * a game woke bananad 60-200 times a second for nothing. The kernel drops a report left empty, so no wake;
		 * EV_SYN stays, its report is what wakes us for the rest (no EVIOCSMASK, an old kernel: everything, as before) */
		unsigned long types = (1ul << EV_SYN) | (1ul << EV_KEY) | (1ul << EV_SW);
		struct input_mask m = { .type = 0, .codes_size = sizeof types, .codes_ptr = (uint64_t)(uintptr_t)&types };
		ioctl(fd, EVIOCSMASK, &m);
		devfd[ndev++] = fd;
	}
	closedir(d);
}

/* A command to a running RetroArch game: its stdin command interface, through the FIFO run-retroarch holds open
 * (bananad's volume keys already write to it). Returns 1 when the line was handed over; 0 when no game runs or the
 * reader closed between the check and the write, which is what keeps the menu's own toasts alive. */
static int ra_cmd(const char *cmd)
{
	int fd = open(ra_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return 0;
	size_t len = strlen(cmd);
	int ok = write(fd, cmd, len) == (ssize_t)len && write(fd, "\n", 1) == 1;
	close(fd);
	return ok;
}

/* After a SYN_DROPPED the release of a key bananad was tracking may be gone, and a lost release leaves Fn or Select
 * held (the combos fire for good) or power "down" (the sleep never starts). Read what the device holds now and let go
 * of what it doesn't -- only for keys this device has, since the same flag may be held on another one. */
static void resync_keys(int fd)
{
	unsigned long caps[KEY_MAX / (8 * sizeof(long)) + 1];
	memset(caps, 0, sizeof caps);
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof caps), caps) < 0) return;
	static const struct { int code; int *flag; } keys[] = { { BTN_MODE, &fn_held }, { BTN_SELECT, &select_held }, { KEY_POWER, &power_down } };
	for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
		int c = keys[i].code;
		if (!((caps[c / (8 * sizeof(long))] >> (c % (8 * sizeof(long)))) & 1)) continue;
		if (!*keys[i].flag || key_held_now(fd, c)) continue;
		struct input_event ev;
		memset(&ev, 0, sizeof ev);
		ev.type = EV_KEY;
		ev.code = c;
		ev.value = 0;
		handle(&ev);
	}
}

/* the power press that woke a panel bananad had blanked: it and its release must not start another sleep */
static int swallow_power;

static void handle(const struct input_event *e)
{
	double now = mono();
	if (e->type == EV_KEY) { last_input = now; undim(); if (panel_blanked) { panel_blanked = 0; if (e->code == KEY_POWER && e->value) swallow_power = 1; standby_exit(); backlights_power(1); pid_t u = ui_pid(); if (u > 0 && (!pid_of("/run/banana/game.pid") || compositor_on())) kill(u, SIGUSR1); } }
	if (e->type == EV_SW && e->code == SW_HEADPHONE_INSERT) {
		if (headphones != e->value) {
			/* unplugged while a game runs: it would blast the speaker, so pause it; plugging back in does not
			 * resume -- the player decides */
			if (!e->value && hud_on()) ra_cmd("PAUSE_TOGGLE");
			headphones = e->value;
			audio_route();
		}
		return;
	}
	if (e->type != EV_KEY) return;
	/* a volume key held down: the kernel's autorepeat (gpio-keys-volume's), a step every 150 ms so the level card keeps
	 * up; every other key's repeats mean nothing here */
	if (e->value == 2) {
		static double last_repeat;
		if ((e->code != KEY_VOLUMEUP && e->code != KEY_VOLUMEDOWN) || now - last_repeat < 0.15) return;
		last_repeat = now;
	}
	switch (e->code) {
	case BTN_MODE: fn_held = e->value; break;
	case BTN_SELECT: select_held = e->value; break;
	/* F (MODE, the button under the screen) + B, in a running game: the d-pad toggles between the d-pad and the
	 * analogue stick (RetroArch's TOGGLE_ANALOG_DPAD, the command banana's RetroArch patch adds). The RPC has no
	 * analogue stick, so this is what lets a game that reads only the stick -- a Dreamcast racer -- be played with
	 * the d-pad, and hands the d-pad back for a game that wants it as a d-pad. */
	case BTN_SOUTH: if (e->value && fn_held && !overlay_open()) ra_cmd("TOGGLE_ANALOG_DPAD"); break;
	case KEY_POWER:
		if (now < ignore_power_until) break;
		if (swallow_power) { if (!e->value) swallow_power = 0; break; }
		/* Transfer mode (Settings > System > Mass storage, read-write): the menu is stopped and /data is the
		 * computer's. A press brings the card back -- usb-msc off ejects the disk from the host, mounts /data and
		 * restarts the menu -- rather than sleeping. Eject on the computer first if it is safe to do so. */
		if (e->value && msc_rw()) {
			char *argv[] = { "/usr/libexec/banana/usb-msc", "off", NULL };
			spawn_bg(argv);
			break;
		}
		/* The menu's burn-in guard has the screen dimmed or blanked (/run/banana/screen-off): this press is to wake
		 * it, so it must not also start a sleep -- two things at once is what makes waking it feel broken. The next
		 * press, screen on, sleeps it as usual. */
		if (e->value && access("/run/banana/screen-off", F_OK) == 0) break;
		if (e->value) { power_down = 1; power_since = now; }
		else if (power_down) { power_down = 0; suspend_now("power button"); }
		break;
	/* F (MODE) + volume is the backlight, in the menu and in a game under banana-ui's compositor, in steps down to a
	 * 2% night-mode floor: there MODE is Banana's pause menu, which opens on MODE's release and not after a combo.
	 * With the compositor off a game keeps Select + volume (MODE is RetroArch's own menu there, covering the pill), and
	 * F + volume is left alone in it rather than silently changing the level behind that menu. */
	case KEY_VOLUMEUP:
	case KEY_VOLUMEDOWN:
		if (e->value) {
			int dir = e->code == KEY_VOLUMEUP ? 1 : -1;
			int game = pid_of("/run/banana/game.pid") > 0, comp = compositor_on();
			if (game && !comp && fn_held) break;
			int bright = (comp || !game) ? fn_held : select_held;
			if (bright && !overlay_open())
				brightness(dir, game && !comp ? 10 : cfg.brightness_step, comp || !game);
			else volume(dir);
		}
		break;
	}
}

/* percent, or -1 when there is no battery or it is charging */
/* the gauge's charge counter in uAh (-1 on the charger, like the percentage): the number a sleep is measured by --
 * one capacity step is 1% of 5 Ah, far too coarse for a few hours */
static long battery_charge_uah(void)
{
	if (charger_online()) return -1;
	DIR *d = opendir("/sys/class/power_supply");
	if (!d) return -1;
	struct dirent *e;
	long r = -1;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[320], s[32];
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
		read_str(p, s, sizeof s);
		if (strcmp(s, "Battery")) continue;
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/charge_counter", e->d_name);
		r = (long)read_int(p, -1);
		break;
	}
	closedir(d);
	return r;
}

/* the gauge's percentage whatever the charger is doing, for the logs; -1 with no battery */
static int battery_pct_any(void)
{
	DIR *d = opendir("/sys/class/power_supply");
	if (!d) return -1;
	struct dirent *e;
	int r = -1;
	while (r < 0 && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[320], s[32];
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
		read_str(p, s, sizeof s);
		if (strcmp(s, "Battery")) continue;
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
		read_str(p, s, sizeof s);
		if (s[0]) r = atoi(s);
	}
	closedir(d);
	return r;
}

static int battery_discharging_pct(void)
{
	/* (the gauge says "Discharging" while a charger charges it, see charger_online: plugged in at 2% must not
	 * shut down) */
	if (charger_online()) return -1;
	DIR *d = opendir("/sys/class/power_supply");
	if (!d) return -1;
	struct dirent *e;
	int r = -1;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		char p[320], s[32];
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
		read_str(p, s, sizeof s);
		if (strcmp(s, "Battery")) continue;
		snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
		read_str(p, s, sizeof s);
		if (!strcmp(s, "Discharging")) {
			snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
			r = read_int(p, -1);
		}
		break;
	}
	closedir(d);
	return r;
}

/* The idle timer of a slate (the Retroid Pocket Classic). There is nothing to close, so an idle suspend has no
 * meaning -- and on this SoC it still costs the picture: the DSI panel and its backlight resume is a gap ROCKNIX has
 * not closed either. So an idle handheld dims and stays awake, which is the battery saving you can see coming. The
 * power button still sleeps it, and writing autosleep_minutes in /data/banana/bananad.conf (Settings > Power) asks
 * for the timer back: the image's own default is not an ask. */
static void slate_sleep_default(void)
{
	if (cfg.autosleep_set || cfg.autosleep_min <= 0)
		return;
	say("sleep: idle only dims (%g min), never suspends; autosleep_minutes asks for the timer", cfg.dim_min);
	cfg.autosleep_min = 0;
}

/* Periodic USB housekeeping: (1) log the Type-C roles when they change, to diagnose a reverse-orientation plug
 * (syslogd -> /run/banana/messages); (2) re-bind the gadget when a port role flip cleared its UDC. A reverse
 * hotplug drops the gadget -- configfs UDC goes empty and /sys/class/udc reads "not attached" -- and nothing
 * re-binds it (there is no UDC "add" event), so the host sees nothing even though the port, PHY and mux are fine.
 * Writing the UDC back recovers it (verified on the unit); it is harmless at boot and while the cable is out. */
static void usb_housekeeping(double now)
{
	static double next;
	static char said[48];
	if (now < next) return;
	next = now + 3;
	DIR *d = opendir("/sys/class/udc");
	if (!d) return;
	int configured = 0;
	char udcname[64] = "";
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		if (!udcname[0]) snprintf(udcname, sizeof udcname, "%.63s", e->d_name);
		char p[300], st[32];
		snprintf(p, sizeof p, "/sys/class/udc/%s/state", e->d_name);
		read_str(p, st, sizeof st);
		if (!strcmp(st, "configured")) configured = 1;
	}
	closedir(d);
	char dr[32] = "", pr[32] = "", cur[16] = "", pcur[16] = "";
	read_str("/sys/class/typec/port0/data_role", dr, sizeof dr);
	read_str("/sys/class/typec/port0/power_role", pr, sizeof pr);
	sscanf(dr, "%15s", cur);
	sscanf(pr, "%15s", pcur);
	char state[64];
	snprintf(state, sizeof state, "%s/%s/%s", cur[0] ? cur : "?", pcur[0] ? pcur : "?",
	    configured ? "up" : "down");
	if (strcmp(state, said)) {
		snprintf(said, sizeof said, "%.47s", state);
		say("usb: typec data_role=%s power_role=%s controller=%s", cur[0] ? cur : "?",
		    pcur[0] ? pcur : "?", configured ? "configured" : "not configured");
	}
	if (configured || !udcname[0]) return;
	char bound[16] = "";
	read_str("/sys/kernel/config/usb_gadget/banana/UDC", bound, sizeof bound);
	if (bound[0]) return;	/* still bound (unplugged, or a host that never dropped it) */
	if (access("/sys/kernel/config/usb_gadget/banana/functions/ncm.usb0", F_OK) ||
	    access("/sys/kernel/config/usb_gadget/banana/configs/c.1/ncm.usb0", F_OK))
		return;		/* the tree is incomplete: usb-gadget owns the build */
	if (write_file("/sys/kernel/config/usb_gadget/banana/UDC", udcname) == 0)
		say("usb: re-bound the gadget to %s (a port role flip had cleared it)", udcname);
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	if (argc > 2 && !strcmp(argv[1], "--selftest") && !strcmp(argv[2], "cheevos"))
		return selftest_cheevos(argc > 3 ? argv[3] : "/tmp");
	if (argc > 2 && !strcmp(argv[1], "--selftest") && !strcmp(argv[2], "retro-lifecycle"))
		return selftest_retro_lifecycle();
	if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest_hud();
	openlog("bananad", LOG_PID, LOG_DAEMON);
	load_conf("/etc/banana/bananad.conf", 0);
	load_conf("/data/banana/bananad.conf", 1);
	load_timezone();
	load_lang();
	signal(SIGCHLD, SIG_DFL);
	/* a game that closes its stdin between the FIFO open and the write must not kill the daemon */
	signal(SIGPIPE, SIG_IGN);
	struct sigaction sa = { 0 };
	sa.sa_handler = on_hup;		/* no SA_RESTART: poll() returns, and the loop reloads */
	sigaction(SIGHUP, &sa, NULL);
	mkdir("/run/banana", 0755);
	{
		FILE *pidfile = fopen("/run/banana/bananad.pid", "w");
		if (pidfile) {
			(void)fchmod(fileno(pidfile), 0600);
			if (fprintf(pidfile, "%ld\n", (long)getpid()) < 0 || fflush(pidfile) != 0)
				say("could not publish the power-control peer PID");
			fclose(pidfile);
		}
	}
	/* debugfs carries the wakeup_sources table wake_reason() names the waker from (the RPC's pm_wakeup_irq is
	 * absent); mounting it again is a harmless EBUSY if something else already did */
	mkdir("/sys/kernel/debug", 0755);
	if (mount("none", "/sys/kernel/debug", "debugfs", 0, NULL) && errno != EBUSY) {}
	find_panel_bl();	/* which backlight banana-ui's idle dim owns */

	int ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	int ino_in = -1, ino_run = -1;
	if (ino >= 0) {
		ino_in = inotify_add_watch(ino, "/dev/input", IN_CREATE | IN_DELETE);
		ino_run = inotify_add_watch(ino, "/run/banana", IN_CREATE | IN_MOVED_TO);	/* the /run/banana/vibrate test trigger */
		cheevos_watch(ino);
	}
	open_inputs();
	headphones = headphones_in();	/* the codec's first answer; the loop confirms (and corrects) it (see jack_settle) */
	jack_settle_begin();
	slate_sleep_default();	/* the idle rule: dim but don't suspend */
	/* the saved volumes first, then the route: the restore also brings back the speakers' switch as it was last
	 * saved (off if headphones were in then), and the route sets it from the jack as it is now */
	restore_settings();
	audio_setup();
	/* ACP's UCM2 profile sets the amplifier's level itself when the sound server opens the card, and rcS starts
	 * the server behind us: once that has settled, put the saved level back. Five seconds is past the server's
	 * start and before anyone has reached for a volume key; a key press cancels it. */
	volume_reassert_at = mono() + 5;
	int uev = uevent_open();
	sd2_scan();	/* a card already in the second slot at boot */
	load_drivers_present();
	vibrator_load();	/* so the plug-in's pulse has a motor to use (the module is not loaded at boot) */
	power_update();
	last_input = mono();
	double next_battery = 0, next_session = 0;
	int low_count = 0;
	say("running: auto-sleep %d min, %s%s", cfg.autosleep_min, cfg.mem_sleep,
	    rtc_wake_possible() ? "" : ", no RTC wake alarm (only power/charger wakes it)");

	for (;;) {
		struct pollfd pf[MAXDEV + 2];
		int n = 0;
		for (int i = 0; i < ndev; i++) pf[n++] = (struct pollfd){ devfd[i], POLLIN, 0 };
		int ino_at = -1, uev_at = -1;
		if (ino >= 0) { ino_at = n; pf[n++] = (struct pollfd){ ino, POLLIN, 0 }; }
		if (uev >= 0) { uev_at = n; pf[n++] = (struct pollfd){ uev, POLLIN, 0 }; }
		/* sleep until something is due: the battery look (a minute), the dim or the auto-sleep deadline, a pending
		 * save, the power button's hold; nothing wakes bananad otherwise */
		double tnow = mono(), wait = next_battery - tnow;
		int game = pid_of("/run/banana/game.pid") != 0;
		/* a job that must not be slept through (banana-scrape's downloads): its pid in /run/banana/awake.pid; looked
		 * at again every 5 s, so the idle sleep comes once it is over */
		int awake = pid_of("/run/banana/awake.pid") != 0;
		if (awake && wait > 5) wait = 5;
		if (game && next_session - tnow < wait) wait = next_session - tnow;	/* the session's save (below) */
		/* (only deadlines still ahead: one already passed was acted on after the last poll, or is held back by what
		 * the action checks (the charger, no backlight) and a uevent or an input event brings it back; counted
		 * here it spun this loop every 20 ms, on the charger for as long as the menu sat idle) */
		if (!game) {
			if (cfg.autosleep_min > 0 && !awake) { double d = cfg.autosleep_min * 60.0 - (tnow - last_input); if (d > 0 && d < wait) wait = d; }
			if (cfg.dim_min > 0 && !dimmed) { double d = cfg.dim_min * 60.0 - (tnow - last_input); if (d > 0 && d < wait) wait = d; }
		}
		if ((save_volume_at > 0 || save_backlight_at > 0) && wait > 0.5) wait = 0.5;
		if (volume_reassert_at > 0 && volume_reassert_at - tnow < wait) wait = volume_reassert_at - tnow;
		if (power_down && wait > 0.1) wait = 0.1;
		if (vib_off_at > 0 && vib_off_at - tnow < wait) wait = vib_off_at - tnow;	/* a pulse's stop */
		if (jack_settle_at > 0 && jack_settle_at - tnow < wait) wait = jack_settle_at - tnow;	/* the jack's second look */
		if (audio_retry_at > 0 && audio_retry_at - tnow < wait) wait = audio_retry_at - tnow;	/* the game's driver, after a late sound server */
		int timeout = wait <= 0 ? 20 : (int)(wait * 1000) + 20;
		if (poll(pf, n, timeout) < 0 && errno != EINTR) {
			int e = errno;
			say("poll: %s", strerror(e));
			return 1;
		}
		double now = mono();
		if (audio_retry_at > 0 && now >= audio_retry_at) audio_retry_poll();
		if (vib_off_at > 0 && now >= vib_off_at) { vib_stop(); vib_off_at = 0; }
		if (jack_settle_at > 0 && now >= jack_settle_at) jack_settle_poll();
		if (volume_reassert_at > 0 && now >= volume_reassert_at) { volume_reassert_at = 0; volume_restore(); }
		if (reload) {
			reload = 0;
			cfg.autosleep_set = 0;
			load_conf("/etc/banana/bananad.conf", 0);
			load_conf("/data/banana/bananad.conf", 1);
			slate_sleep_default();	/* the settings screen may have just written a timer, or taken it away */
			load_timezone();
			load_lang();	/* the settings screen may have just changed the language */
			save_volume_at = mono() + 2;	/* the settings screen may have set the volume: keep it as the keys' is kept */
			power_update();
			say("settings read again: auto-sleep %d min", cfg.autosleep_min);
		}

		for (int i = 0; i < ndev; i++) {
			if (!(pf[i].revents & POLLIN)) continue;
			struct input_event ev[64];
			ssize_t r;
			while ((r = read(devfd[i], ev, sizeof ev)) > 0)
				for (int k = 0; k < r / (ssize_t)sizeof ev[0]; k++) {
					if (ev[k].type == EV_SYN && ev[k].code == SYN_DROPPED) { devdrop[i] = 1; continue; }
					if (devdrop[i]) {
						if (ev[k].type == EV_SYN && ev[k].code == SYN_REPORT) { devdrop[i] = 0; resync_keys(devfd[i]); }
						continue;
					}
					handle(&ev[k]);
				}
		}
		if (uev_at >= 0 && (pf[uev_at].revents & POLLIN) && uevent_read(uev)) power_update();
		save_settings(now);
		if (ino_at >= 0 && (pf[ino_at].revents & POLLIN)) {
			char buf[4096];
			ssize_t r;
			int inputs_changed = 0, vibrate_req = 0, cheevos_changed = 0;
			while ((r = read(ino, buf, sizeof buf)) > 0)
				for (char *p = buf; p < buf + r; ) {
					struct inotify_event *e = (struct inotify_event *)p;
					if (e->len && e->wd == ino_run && !strcmp(e->name, "vibrate")) vibrate_req = 1;
					if (e->len && e->wd == ino_run && !strcmp(e->name, "cheevos")) cheevos_changed = 1;
					if (e->wd == ino_cheev) cheevos_changed = 1;
					if ((e->mask & (IN_CREATE | IN_DELETE)) && e->wd == ino_in) inputs_changed = 1;
					p += sizeof(struct inotify_event) + e->len;
				}
			if (cheevos_changed) { cheevos_watch(ino); cheevos_poll(); }	/* the re-add catches the file's re-creation between games */
			if (vibrate_req) {
				/* `touch /run/banana/vibrate` (a test: the plug-in pulse), or a length in ms written to it (the menu's
				 * CRT degauss thump, renamed into place so the number is there by the time IN_MOVED_TO arrives) */
				char s[16] = "";
				read_str("/run/banana/vibrate", s, sizeof s);
				unlink("/run/banana/vibrate");
				int ms = atoi(s);
				vibrate(ms > 0 && ms <= 2000 ? ms : cfg.vibrate_ms);
			}
			if (inputs_changed) {
				sleep_us(200000);	/* udev/mdev sets the node's permissions */
				open_inputs();
				jack_log();		/* a jack that probed after bananad opened the inputs shows up here, not at start */
				jack_settle_begin();	/* and its state is read again: the boot route may have run before it existed */
			}
		}

		if (power_down && now - power_since >= cfg.power_long_ms / 1000.0) { power_down = 0; shutdown_now("power held"); }

		/* idle in the menu (no game running): the panel dims, then it sleeps */
		game = pid_of("/run/banana/game.pid") != 0;
		if (cfg.dim_min > 0 && !dimmed && !game && now - last_input >= cfg.dim_min * 60.0) dim();
		/* (not while charging: a sleep would put the charging light out; the menu stays dimmed instead) */
		if (cfg.autosleep_min > 0 && !game && now - last_input >= cfg.autosleep_min * 60.0 && !charger_online() &&
		    !pid_of("/run/banana/awake.pid"))
			suspend_now("idle");

		if (!game) next_session = now + (cfg.session_secs > 0 ? cfg.session_secs : 180);
		else if (now >= next_session) { next_session = now + (cfg.session_secs > 0 ? cfg.session_secs : 180); session_tick(); }
		usb_housekeeping(now);	/* log the Type-C roles; re-bind a gadget a role flip cleared */
		if (now >= next_battery) {
			next_battery = now + 60;
			power_update();	/* a capacity change has no uevent on every gauge */
			int pct = battery_discharging_pct();
			/* two thresholds, once each, and only while a game is on top (the menu has the charge icon) */
			static int warned = 1000;
			if (pct >= 0 && pct <= 10) {
				int band = pct <= 5 ? 5 : 10;
				if (band != warned) {
					char b[96];
					warned = band;
					hud_fmt(HUD_LOW, pct, b, sizeof b);
					hud_show(b);
				}
			} else if (pct > 10) warned = 1000;
			/* the first reading at or under the floor warns while the second shuts down (a load spike is not a
			 * shutdown), so the line is on screen for the whole of that minute */
			if (pct >= 0 && pct <= cfg.battery_off_pct && low_count == 0) {
				char b[96];
				hud_fmt(HUD_EMPTY, 0, b, sizeof b);
				hud_show(b);
			}
			low_count = pct >= 0 && pct <= cfg.battery_off_pct ? low_count + 1 : 0;
			if (low_count >= 2) shutdown_now("battery empty");	/* two readings in a row: not a load spike */
		}
	}
}
