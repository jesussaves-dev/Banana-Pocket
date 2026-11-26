#pragma once
// banana-ui: what the main loop, the headless runner and the boot animation share.
#include "banana-retro-control.h"
#include "frontend.h"
#include "input.h"
#include "kms.h"
#include "util.h"
#include "compositor.h"
#include "library.h"
#include "frame.h"
#include "soft.h"
#include "splash.h"
#include "settings.h"
#include "i18n.h"
#include "settings_defaults.h"
#include "theme.h"
#include "sound.h"
#include "idle.h"
#include "menu.h"
#include "layout.h"
#include <algorithm>
#include <deque>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cctype>
#include <climits>
#include <cstring>
#include <iterator>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <future>
#include <list>
#include <sys/stat.h>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

using namespace banana;

static double nowMs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// usleep is obsolete in POSIX.1-2008; nanosleep is its replacement
static void sleepUs(long us) { timespec ts = { us / 1000000, (us % 1000000) * 1000 }; while (nanosleep(&ts, &ts) && errno == EINTR) {} }

static void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "banana-ui: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

// ---- the pause menu's reach into a game ---------------------------------------------------------------------
// A signal to a process and every descendant of it. The launcher is its own process group (runGame's setsid), but
// run-port and run-bananatube start the app with setsid again, so a group signal would miss the app itself: the
// tree is walked through /proc instead. Used to freeze a game (SIGSTOP) under the pause menu and thaw it (SIGCONT).
static void signalTree(pid_t root, int sig)
{
	if (root <= 0) return;
	std::vector<std::pair<pid_t, pid_t>> procs;	// (pid, ppid)
	if (DIR* d = opendir("/proc"))
	{
		while (dirent* e = readdir(d))
		{
			char* end = nullptr;
			long pid = strtol(e->d_name, &end, 10);
			if (pid <= 0 || *end) continue;
			char path[64], buf[512];
			snprintf(path, sizeof path, "/proc/%ld/stat", pid);
			FILE* f = fopen(path, "r");
			if (!f) continue;
			size_t n = fread(buf, 1, sizeof buf - 1, f);
			fclose(f);
			buf[n] = 0;
			const char* rp = strrchr(buf, ')');	// the command name can hold spaces and parentheses
			int ppid = 0;
			if (rp && sscanf(rp + 1, " %*c %d", &ppid) == 1) procs.push_back({ (pid_t)pid, (pid_t)ppid });
		}
		closedir(d);
	}
	std::vector<pid_t> tree{ root };
	for (size_t i = 0; i < tree.size(); i++)
		for (const auto& pr : procs)
			if (pr.second == tree[i] && std::find(tree.begin(), tree.end(), pr.first) == tree.end()) tree.push_back(pr.first);
	for (pid_t pid : tree) kill(pid, sig);
}

// One command line to RetroArch through its stdin FIFO (run-retroarch holds the pipe open; stdin_cmd_enable). Never
// blocks: no reader means no RetroArch, and the line is dropped.
static bool retroarchCmd(const char* cmd)
{
	int fd = open("/run/banana/retroarch.cmd", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return false;
	std::string line = std::string(cmd) + "\n";
	ssize_t w;
	do w = write(fd, line.data(), line.size()); while (w < 0 && errno == EINTR);
	close(fd);
	return w == (ssize_t)line.size();
}

// A save state of the running game: "<rom stem>.state" for slot 0, ".stateN" for N, in /data/states or one of its
// per-core folders (RetroArch sorts them by core). The newest match wins; "" when the slot has none.
static std::string statePath(const std::string& rom, int slot)
{
	std::string base = rom.substr(rom.rfind('/') + 1);
	size_t dot = base.rfind('.');
	if (dot != std::string::npos && dot > 0) base = base.substr(0, dot);
	std::string name = base + ".state" + (slot > 0 ? std::to_string(slot) : std::string());
	std::vector<std::string> dirs = { "/data/states" };
	if (DIR* d = opendir("/data/states"))
	{
		while (dirent* e = readdir(d))
			if (e->d_name[0] != '.') dirs.push_back(std::string("/data/states/") + e->d_name);
		closedir(d);
	}
	std::string best;
	time_t bestAt = 0;
	for (const auto& dir : dirs)
	{
		struct stat st;
		std::string pth = dir + "/" + name;
		if (stat(pth.c_str(), &st) == 0 && S_ISREG(st.st_mode) && (best.empty() || st.st_mtime > bestAt)) { best = pth; bestAt = st.st_mtime; }
	}
	return best;
}

// How many discs a game has: the entries of an .m3u playlist (RetroArch's multi-disc format), else 1.
static int discCount(const std::string& rom)
{
	size_t dot = rom.rfind('.');
	std::string ext = dot == std::string::npos ? "" : rom.substr(dot + 1);
	for (auto& c : ext) c = (char)tolower((unsigned char)c);
	if (ext != "m3u") return 1;
	std::ifstream f(rom);
	std::string line;
	int n = 0;
	while (std::getline(f, line))
	{
		size_t a = line.find_first_not_of(" \t\r");
		if (a != std::string::npos && line[a] != '#') n++;
	}
	return std::max(1, n);
}

// ---- resume the game a shutdown interrupted ---------------------------------------------------------------
// A legacy record keeps its ROM path first for older menus. New publications add an ID; native records use the
// checkpoint digest. Reserve an attempt before launch, settle only that identity, and keep transient failures.
enum class Resume { None, Ready, Missing, Unavailable, Blocked };

struct ResumeRecord
{
	std::string rom, system, core, token;	// native digest with a profile, legacy publication ID without one
};

static bool resumeProfileId(std::string_view id)
{
	return !id.empty() && id.size() <= 63 &&
	       id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == std::string_view::npos;
}

static bool resumeHex(std::string_view value, size_t length)
{
	return value.size() == length && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

static bool resumeRom(std::string_view rom)
{
	return !rom.empty() && rom.front() == '/' && rom.size() < PATH_MAX &&
	       rom.find_first_of("\r\n") == std::string_view::npos;
}

static Resume resumeReadFile(const std::string& path, char* bytes, size_t capacity, std::string_view& text)
{
	int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
		return errno == ENOENT ? Resume::None : errno == ELOOP ? Resume::Missing : Resume::Unavailable;
	struct stat st;
	if (fstat(fd, &st) < 0) { close(fd); return Resume::Unavailable; }
	if (!S_ISREG(st.st_mode) || st.st_size >= (off_t)capacity) { close(fd); return Resume::Missing; }
	bool ok = true;
	size_t used = 0;
	while (used < capacity)
	{
		ssize_t n = read(fd, bytes + used, capacity - used);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) { ok = n == 0; break; }
		used += (size_t)n;
	}
	if (close(fd) < 0 || !ok) return Resume::Unavailable;
	if (used == capacity || memchr(bytes, '\0', used)) return Resume::Missing;
	text = {bytes, used};
	return Resume::Ready;
}

// Read one complete line, including checking that its newline was written.
static bool resumeLine(std::string_view& text, std::string_view& line)
{
	const size_t end = text.find('\n');
	if (end == std::string_view::npos) return false;
	line = text.substr(0, end);
	text.remove_prefix(end + 1);
	return true;
}

// The marker itself, whatever the retry counter says.
static Resume resumeRead(const std::string& data, ResumeRecord& record)
{
	record = {};
	constexpr std::string_view version = "banana-retro-v1", legacy = BANANA_LEGACY_RESUME_ID_PREFIX;
	// The native format is longer: version, system:core, digest and path, with line endings.
	constexpr size_t maxBytes = version.size() + 63 + 1 + 63 + BANANA_RETRO_RESUME_TOKEN_SIZE + PATH_MAX - 1 + 4;
	char bytes[maxBytes + 1];
	std::string_view lines;
	const Resume status = resumeReadFile(data + "/resume", bytes, sizeof bytes, lines);
	if (status != Resume::Ready) return status;
	const size_t firstEnd = lines.find('\n');
	std::string_view rom = lines.substr(0, firstEnd), system, core, token;
	if (rom == version)
	{
		if (firstEnd == std::string_view::npos || lines.back() != '\n') return Resume::Missing;
		lines.remove_prefix(firstEnd + 1);
		const size_t profileEnd = lines.find('\n');
		if (profileEnd == std::string_view::npos) return Resume::Missing;
		const std::string_view profile = lines.substr(0, profileEnd);
		const size_t separator = profile.find(':');
		if (separator == std::string_view::npos) return Resume::Missing;
		system = profile.substr(0, separator);
		core = profile.substr(separator + 1);
		if (!resumeProfileId(system) || !resumeProfileId(core)) return Resume::Missing;
		lines.remove_prefix(profileEnd + 1);
		const size_t tokenEnd = lines.find('\n');
		token = lines.substr(0, tokenEnd);
		if (tokenEnd == std::string_view::npos || !resumeHex(token, BANANA_RETRO_RESUME_TOKEN_SIZE))
			return Resume::Missing;
		rom = lines.substr(tokenEnd + 1);
		if (rom.empty()) return Resume::Missing;
		rom.remove_suffix(1);	// the final newline; nothing after it is permitted
	}
	else if (firstEnd != std::string_view::npos && firstEnd + 1 != lines.size())
	{
		lines.remove_prefix(firstEnd + 1);
		std::string_view id;
		if (!resumeLine(lines, id) || !lines.empty() || id.substr(0, legacy.size()) != legacy)
			return Resume::Missing;
		token = id.substr(legacy.size());
		if (!resumeHex(token, BANANA_LEGACY_RESUME_ID_SIZE)) return Resume::Missing;
	}
	else
	{
		while (!rom.empty() && (rom.back() == '\r' || rom.back() == ' ')) rom.remove_suffix(1);
	}
	if (!resumeRom(rom)) return Resume::Missing;
	record = {std::string(rom), std::string(system), std::string(core), std::string(token)};
	return Resume::Ready;
}

static constexpr int resumeMaxTries = 3;
// What run-libretro exits with when a typed boot resume ends before native readiness. After readiness it exits with
// the runtime's own status, so these two never describe a game that ran.
static constexpr int resumeExitRetry = 75;	// EX_TEMPFAIL: a later boot may succeed
static constexpr int resumeExitRejected = 78;	// EX_CONFIG: the record, profile or runtime refused it for good

static bool resumeSameRecord(const ResumeRecord& a, const ResumeRecord& b)
{
	return a.rom == b.rom && a.system == b.system && a.core == b.core && a.token == b.token;
}

// A nonnumeric header keeps old menus from treating another record's count as a global budget.
static constexpr std::string_view resumeTriesVersion = "banana-resume-tries-v1";

struct ResumeTries
{
	int tries = 0;
	ResumeRecord record;
	bool bare = false;	// only a number: an older image's global debt, not attributable to a record
};

static Resume resumeReadTries(const std::string& data, ResumeTries& out)
{
	out = {};
	char bytes[resumeTriesVersion.size() + PATH_MAX + 2 * 63 + BANANA_RETRO_RESUME_TOKEN_SIZE + 10];
	std::string_view lines, line;
	const Resume status = resumeReadFile(data + "/resume-tries", bytes, sizeof bytes, lines);
	if (status != Resume::Ready) return status;
	if (!resumeLine(lines, line)) { line = lines; lines = {}; }
	const bool versioned = line == resumeTriesVersion;
	if (versioned && !resumeLine(lines, line)) return Resume::Missing;
	if (line.empty() || line.size() > 3) return Resume::Missing;
	int tries = 0;
	for (char c : line)
	{
		if (c < '0' || c > '9') return Resume::Missing;
		tries = tries * 10 + (c - '0');
	}
	// A bare count from an older image cannot identify a record. Keep valid pre-header keyed counts.
	if (!versioned && lines.empty())
	{
		out.bare = true;
		return Resume::None;
	}
	std::string_view rom, system, core, token;
	if (!resumeLine(lines, rom) || !resumeLine(lines, system) || !resumeLine(lines, core) ||
	    !resumeLine(lines, token) || !lines.empty() || !resumeRom(rom)) return Resume::Missing;
	if (system.empty())
	{
		if (!core.empty() || (!token.empty() && !resumeHex(token, BANANA_LEGACY_RESUME_ID_SIZE)))
			return Resume::Missing;
	}
	else if (!resumeProfileId(system) || !resumeProfileId(core) || !resumeHex(token, BANANA_RETRO_RESUME_TOKEN_SIZE))
		return Resume::Missing;
	out = {tries, {std::string(rom), std::string(system), std::string(core), std::string(token)}};
	return Resume::Ready;
}

// Negative means this budget could not be established, not that another attempt is free.
static int resumeCountedTries(const std::string& data, const ResumeRecord& record)
{
	ResumeTries counted;
	const Resume status = resumeReadTries(data, counted);
	if (status == Resume::None) return 0;
	if (status != Resume::Ready) return -1;
	return resumeSameRecord(counted.record, record) ? counted.tries : 0;
}

// Unlinks whose directory sync has not succeeded yet, by data directory. An unlink that returned 0 is only
// durable after the sync, so a failed sync leaves a debt that outlives the call: any later retry pays it, even when
// the marker and the counter are already gone, and no other success (a count written, a later removal) hides it.
static std::set<std::string>& resumeSyncDebt()
{
	static std::set<std::string> owed;
	return owed;
}
static bool resumeSyncOwed(const std::string& data) { return resumeSyncDebt().count(data) != 0; }

// A successful sync of the directory settles every unlink made in it so far.
static bool resumeSync(const std::string& data)
{
	int dir = open(data.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dir < 0) return false;
	const bool ok = fsync(dir) == 0;
	if (close(dir) != 0 || !ok) return false;
	resumeSyncDebt().erase(data);
	return true;
}

// Store the count with its record: exclusive temp, sync, rename, directory sync. A count that cannot be made
// durable must not start an attempt.
static bool resumeWriteTries(const std::string& data, int tries, const ResumeRecord& record)
{
	const std::string path = data + "/resume-tries", tmp = path + ".tmp";
	const std::string text = std::string(resumeTriesVersion) + "\n" + std::to_string(tries) + "\n" +
	                         record.rom + "\n" + record.system + "\n" + record.core + "\n" + record.token + "\n";
	if (unlink(tmp.c_str()) < 0 && errno != ENOENT) return false;
	int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0) return false;
	bool ok = true;
	for (size_t off = 0; ok && off < text.size();)
	{
		ssize_t n = write(fd, text.data() + off, text.size() - off);
		if (n > 0) off += (size_t)n;
		else if (n < 0 && errno == EINTR) continue;
		else ok = false;
	}
	if (ok && fsync(fd) < 0) ok = false;
	if (close(fd) < 0) ok = false;
	if (ok && rename(tmp.c_str(), path.c_str()) < 0) ok = false;
	if (!ok) { unlink(tmp.c_str()); return false; }
	return resumeSync(data);
}

// rcK and bananad create this before they signal anything. From then on the daemon owns the record and the game.
static bool resumeLatched(const std::string& run) { return access((run + "/shutting-down").c_str(), F_OK) == 0; }

// Do not wait on daemon I/O from the UI thread. All mutations require the same flock as daemon publication. With a
// run directory the shutdown latch is looked at only after the lock is held, so a mutation can never run in a window
// in which the daemon already owns the record: a look taken before the lock proves nothing once the lock is taken.
// Without one (the host fixtures of a single helper, the headless verb) nothing is latched.
static int resumeLock(const std::string& data, const std::string& run = {})
{
	int fd = open((data + "/resume.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) return -1;
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) { close(fd); return -1; }
	if (!run.empty() && resumeLatched(run)) { close(fd); return -1; }
	return fd;
}
static void resumeUnlock(int fd) { if (fd >= 0) close(fd); }

class ResumeGuard
{
	const int fd;
public:
	explicit ResumeGuard(const std::string& data, const std::string& run = {}): fd(resumeLock(data, run)) {}
	~ResumeGuard() { resumeUnlock(fd); }
	ResumeGuard(const ResumeGuard&) = delete;
	ResumeGuard& operator=(const ResumeGuard&) = delete;
	explicit operator bool() const { return fd >= 0; }
};

// Keep the counter until the marker's removal is durable, so a failed unlink cannot reset the retry limit. Only the
// named files are ever unlinked: a replacement record published after an earlier attempt is never touched by a
// retry whose own files are gone. What an earlier call unlinked but could not sync is paid first, every time.
static bool resumeRemove(const std::string& data, bool marker, bool counter)
{
	if (resumeSyncOwed(data) && !resumeSync(data)) return false;
	if (marker)
	{
		if (unlink((data + "/resume").c_str()) == 0) resumeSyncDebt().insert(data);
		else
		{
			if (errno != ENOENT) return false;
			if (!counter) return true;
			struct stat st;
			if (lstat((data + "/resume-tries").c_str(), &st) < 0) return errno == ENOENT;
		}
		if (!resumeSync(data)) return false;
	}
	if (counter)
	{
		if (unlink((data + "/resume-tries").c_str()) < 0) return errno == ENOENT;
		resumeSyncDebt().insert(data);
		return resumeSync(data);
	}
	return true;
}

// The boot decision. With a run directory (every production caller) the shutdown latch is honoured under the lock:
// planning clears missing and exhausted records, and none of that may happen once a shutdown owns the record.
static Resume resumePlan(const std::string& data, ResumeRecord& record, const std::string& run = {})
{
	record = {};
	const ResumeGuard lock(data, run);
	if (!lock) return Resume::Unavailable;
	Resume status = resumeRead(data, record);
	if (status == Resume::Ready || status == Resume::None)
	{
		ResumeTries counted;
		const Resume counter = resumeReadTries(data, counted);
		if (counter == Resume::Unavailable) return Resume::Unavailable;
		// A bare count from an older image names no record, so it cannot bound one; left in place it would still
		// stop a downgraded menu's first legacy record. Clear the debt instead of inheriting it.
		if (counted.bare && !resumeRemove(data, false, true)) return Resume::Unavailable;
		if (status == Resume::Ready)
		{
			// A counter that cannot be read leaves this record's budget unknown. It counts as spent, never as free:
			// the record and the damaged file are cleared together, so a damaged counter can neither refuse the
			// record on every boot nor buy it extra attempts.
			const int tries = counter == Resume::Missing ? resumeMaxTries :
				counter == Resume::Ready && resumeSameRecord(counted.record, record) ? counted.tries : 0;
			if (tries >= resumeMaxTries) status = Resume::Blocked;
			else if (access(record.rom.c_str(), R_OK) < 0)
				status = errno == ENOENT || errno == ENOTDIR ? Resume::Missing : Resume::Unavailable;
		}
	}
	// Decide and discard inside the same lock, even if a malformed marker could not supply an identity.
	if ((status == Resume::Missing || status == Resume::Blocked) && !resumeRemove(data, true, true))
		return Resume::Unavailable;
	return status;
}

// An attempt is about to start: count it against this record, durably, and keep the marker. False: it must not start.
static bool resumeBegin(const std::string& data, const ResumeRecord& record, const std::string& run = {})
{
	const ResumeGuard lock(data, run);
	ResumeRecord now;
	if (!lock || resumeRead(data, now) != Resume::Ready || !resumeSameRecord(now, record)) return false;
	const int tries = resumeCountedTries(data, record);
	return tries >= 0 && tries < resumeMaxTries && resumeWriteTries(data, tries + 1, record);
}

// Cancellation, rejection and readiness all settle only the observed record.
static bool resumeConfirm(const std::string& data, const ResumeRecord& launched, const std::string& run = {})
{
	if (launched.rom.empty()) return false;
	const ResumeGuard lock(data, run);
	if (!lock) return false;
	ResumeRecord now;
	const Resume status = resumeRead(data, now);
	if (status == Resume::Unavailable) return false;
	ResumeTries counted;
	const bool counter = resumeReadTries(data, counted) == Resume::Ready && resumeSameRecord(counted.record, launched);
	const bool marker = status == Resume::Ready && resumeSameRecord(now, launched);
	return resumeRemove(data, marker || (counter && status == Resume::None), counter);
}

// ---- ownership -----------------------------------------------------------------------------------------------------
// Ownership is never inferred from a ROM path. bananad, the only publisher, records which running session (S, the
// registration the game's own process wrote) published the record that is visible, in /run/banana/resume-owner:
//	banana-resume-owner-v1
//	<previous entry, or ->
//	<candidate entry, or ->
// An entry is "<S|-> <key>" (the previous entry may also be a bare "-"); the key is "N <digest> <system> <core> <rom>"
// for a native record and "L <id> - - <rom>" for a RetroArch one. The stamp is staged before the record is renamed
// into place, so an interruption on either side leaves the visible record matching exactly one entry. The whole stamp
// has to be well formed: a damaged entry never lets the other one stand in for it, and two owners for one key name
// none. Anything that does not match exactly -- no stamp, a damaged one, an old record without an ID, an entry
// published unowned -- is unowned, and no session may remove an unowned record.
static bool resumeStampKey(std::string_view key)
{
	auto word = [&key](std::string_view& out)
	{
		const size_t space = key.find(' ');
		if (space == std::string_view::npos) return false;
		out = key.substr(0, space);
		key.remove_prefix(space + 1);
		return true;
	};
	std::string_view kind, first, second, third;
	if (!word(kind)) return false;
	if (kind == "N")
		return word(first) && word(second) && word(third) && resumeHex(first, BANANA_RETRO_RESUME_TOKEN_SIZE) &&
		       resumeProfileId(second) && resumeProfileId(third) && resumeRom(key);
	return kind == "L" && word(first) && word(second) && word(third) && resumeHex(first, BANANA_LEGACY_RESUME_ID_SIZE) &&
	       second == "-" && third == "-" && resumeRom(key);
}

struct ResumeStampEntry
{
	std::string_view session, key;	// both empty: the bare "-" previous entry; session empty alone: published unowned
};

static bool resumeStampEntry(std::string_view entry, bool previous, ResumeStampEntry& out)
{
	out = {};
	if (previous && entry == "-") return true;
	if (entry.size() > 2 && entry.substr(0, 2) == "- ") out.key = entry.substr(2);
	else if (entry.size() > 33 && entry[32] == ' ' && resumeHex(entry.substr(0, 32), 32))
	{
		out.session = entry.substr(0, 32);
		out.key = entry.substr(33);
	}
	else return false;
	return resumeStampKey(out.key);
}

static std::string resumeOwnerOf(const std::string& run, const ResumeRecord& record)
{
	if (record.token.empty() || !resumeRom(record.rom)) return {};
	const bool native = !record.core.empty();
	const std::string key = std::string(native ? "N " : "L ") + record.token + " " +
		(native ? record.system : "-") + " " + (native ? record.core : "-") + " " + record.rom;
	char bytes[2 * (PATH_MAX + 2 * 64 + BANANA_RETRO_RESUME_TOKEN_SIZE + 40) + 64];
	std::string_view lines, header, previous, candidate;
	if (resumeReadFile(run + "/resume-owner", bytes, sizeof bytes, lines) != Resume::Ready) return {};
	if (!resumeLine(lines, header) || header != "banana-resume-owner-v1" || !resumeLine(lines, previous) ||
	    !resumeLine(lines, candidate) || !lines.empty()) return {};
	ResumeStampEntry before, after;
	if (!resumeStampEntry(previous, true, before) || !resumeStampEntry(candidate, false, after)) return {};
	if (!before.session.empty() && !after.session.empty() && before.key == after.key && before.session != after.session)
		return {};
	for (const ResumeStampEntry* entry : {&after, &before})	// the newest entry that names this record decides
		if (entry->key == key) return std::string(entry->session);
	return {};
}

// What one launch left to settle. A job names its session once and never changes it: a job for another session
// can neither replace nor extend it.
struct ResumeJob
{
	std::string session;	// S, the launch this job belongs to
	ResumeRecord reserved;	// the boot reservation that launch started (empty ROM: it started none)
	bool settle = false;	// spend `reserved`: readiness, rejection or a clean return
	bool clean = false;	// a clean return: remove the record bananad published for S
	bool any() const { return settle || clean; }
};

// Decide and remove under one hold of resume.lock. Only this launch's own state changes: its reservation (and that
// record's counter), and a record the stamp binds to S, with the counter that names it. False: the lock or the
// filesystem was busy and nothing was decided; the caller retries without waiting.
static bool resumeRunJob(const std::string& data, const std::string& run, const ResumeJob& job)
{
	if (job.session.empty()) return true;	// nothing to attribute anything to
	const ResumeGuard lock(data, run);
	if (!lock) return false;
	ResumeRecord now;
	const Resume status = resumeRead(data, now);
	if (status == Resume::Unavailable) return false;
	ResumeTries counted;
	const Resume counter = resumeReadTries(data, counted);
	if (counter == Resume::Unavailable) return false;
	const bool keyed = counter == Resume::Ready;
	bool marker = false, drop = false;
	if (job.settle && !job.reserved.rom.empty())
	{
		const bool ownCounter = keyed && resumeSameRecord(counted.record, job.reserved);
		marker = (status == Resume::Ready && resumeSameRecord(now, job.reserved)) || (ownCounter && status == Resume::None);
		drop = ownCounter;
	}
	if (job.clean && !marker && status == Resume::Ready && resumeOwnerOf(run, now) == job.session)
	{
		marker = true;
		// that record's own counter; a bare or damaged one beside it cannot belong to another record
		drop = drop || (keyed && resumeSameRecord(counted.record, now)) || counter == Resume::Missing || counted.bare;
	}
	return resumeRemove(data, marker, drop);
}

// A clean return or a rejection can meet bananad's publication lock. The UI never waits for it: the same decision
// is retried on the main loop's timer for a bounded window. The scheduler is shared with the host fixtures, so the
// retry, expiry and completion paths are exercised, not just the cleanup helpers.
class ResumeCleanup
{
public:
	enum class Step { Idle, Retried, Done, Exhausted, Halted };
	static constexpr int maxAttempts = 20;
	static constexpr double intervalMs = 100;
	static constexpr double windowMs = 2000;

	ResumeJob job;
	int attempts = 0;
	double due = 0;
	double deadline = 0;
	bool exhausted = false;

	// False: another session's job is pending, and it is neither replaced nor extended. The same session merges
	// into the pending job and keeps its original window.
	bool schedule(const ResumeJob& next, double now)
	{
		if (!next.any()) return true;
		if (active)
		{
			if (job.session != next.session) return false;
			if (!next.reserved.rom.empty())
			{
				if (!job.reserved.rom.empty() && !resumeSameRecord(job.reserved, next.reserved)) return false;
				job.reserved = next.reserved;
			}
			job.settle = job.settle || next.settle;
			job.clean = job.clean || next.clean;
			return true;
		}
		job = next;
		attempts = 0;
		due = now;
		deadline = now + windowMs;
		exhausted = false;
		active = true;
		return true;
	}
	bool pending() const { return active; }
	bool ready(double now) const { return active && now >= due; }
	// A shutdown owns the record from here on: nothing more is settled on its behalf.
	void abandon() { active = false; }

	Step step(double now, const std::string& data, const std::string& run, bool halted = false)
	{
		if (active && halted) { active = false; return Step::Halted; }
		if (!ready(now)) return Step::Idle;
		if (resumeRunJob(data, run, job))
		{
			active = false;
			return Step::Done;
		}
		if (++attempts >= maxAttempts || now >= deadline)
		{
			active = false;
			exhausted = true;
			return Step::Exhausted;
		}
		due = now + intervalMs;
		return Step::Retried;
	}

private:
	bool active = false;
};

// An already-running game, a queued menu launch, an existing launch file or a cleanup still in flight is an
// explicit request or unfinished business: it wins over the boot record, which is settled without a second execution.
static bool resumeDeferred(const std::string& run, int gamePid, int pendingLaunch, bool cleanupPending = false)
{
	return gamePid > 0 || pendingLaunch >= 0 || cleanupPending || access((run + "/launch").c_str(), F_OK) == 0;
}

// A native session identity: 32 bits from the kernel boot ID, a boot-scoped 64-bit sequence, and the UI PID.
// The sequence lives in the run directory rather than this process, so a UI restart cannot hand a live or stale
// session the identity of an earlier one. Allocation fails closed rather than reusing a value.
static bool resumeSessionId(const std::string& run, std::string& id)
{
	id.clear();
	std::ifstream boot("/proc/sys/kernel/random/boot_id");
	std::string bootId, prefix;
	if (!std::getline(boot, bootId)) return false;
	for (char c : bootId)
	{
		if (prefix.size() == 8) break;
		if (c == '-') continue;
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
		prefix += c;
	}
	if (prefix.size() != 8) return false;

	const std::string lockPath = run + "/session.lock", path = run + "/session.seq", tmp = path + ".tmp";
	int lock = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (lock < 0) return false;
	if (flock(lock, LOCK_EX | LOCK_NB) < 0) { close(lock); return false; }
	uint64_t sequence = 0;
	char text[32] = {};
	int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	bool ok = true;
	if (fd >= 0)
	{
		ssize_t n = read(fd, text, sizeof text - 1);
		if (close(fd) < 0 || n < 0) ok = false;
		else if (n > 0)
		{
			ok = text[n - 1] == '\n' && n <= 21;
			if (ok)
			{
				text[n - 1] = '\0';
				ok = text[0] != '\0';
				for (size_t i = 0; ok && i < (size_t)n - 1; ++i)
				{
					if (text[i] < '0' || text[i] > '9' ||
					    sequence > (UINT64_MAX - (unsigned)(text[i] - '0')) / 10)
					{ ok = false; break; }
					sequence = sequence * 10 + (unsigned)(text[i] - '0');
				}
			}
		}
	}
	else if (errno != ENOENT) ok = false;
	if (ok && sequence == UINT64_MAX) ok = false;
	if (ok)
	{
		++sequence;
		char next[32];
		int size = snprintf(next, sizeof next, "%llu\n", (unsigned long long)sequence);
		ok = size > 0 && (size_t)size < sizeof next;
		if (ok && unlink(tmp.c_str()) < 0 && errno != ENOENT) ok = false;
		int out = ok ? open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600) : -1;
		if (out < 0) ok = false;
		for (size_t off = 0; ok && off < (size_t)size;)
		{
			ssize_t n = write(out, next + off, (size_t)size - off);
			if (n > 0) off += (size_t)n;
			else if (n < 0 && errno == EINTR) continue;
			else ok = false;
		}
		if (ok && fsync(out) < 0) ok = false;
		if (out >= 0 && close(out) < 0) ok = false;
		if (ok && rename(tmp.c_str(), path.c_str()) < 0) ok = false;
		if (!ok) unlink(tmp.c_str());
		else if (!resumeSync(run)) ok = false;
	}
	if (flock(lock, LOCK_UN) < 0) ok = false;
	if (close(lock) < 0) ok = false;
	if (!ok) return false;
	char value[33];
	int size = snprintf(value, sizeof value, "%s%016llx%08x", prefix.c_str(),
		(unsigned long long)sequence, (unsigned)getpid());
	if (size != 32) return false;
	id.assign(value, (size_t)size);
	return true;
}

enum class ResumeEnd { Retry, Rejected };
// Without readiness, only the launcher's explicit rejection settles a typed boot resume.
static ResumeEnd resumeEnd(int status)
{
	if (WIFEXITED(status) && WEXITSTATUS(status) == resumeExitRejected) return ResumeEnd::Rejected;
	return ResumeEnd::Retry;
}
static bool resumeSettle(const std::string& data, const ResumeRecord& launched, int status)
{
	return resumeEnd(status) == ResumeEnd::Retry || resumeConfirm(data, launched);
}
// run-libretro's session metadata, "v1 <session> <pid> <start> <core> <started> <state> <rom>": has this session
// reached "ready"? Only the launcher writes it, after the runtime published readiness.
static bool resumeNativeReady(const std::string& path, std::string_view session)
{
	std::ifstream f(path);
	std::string line;
	if (session.size() != 32 || !std::getline(f, line)) return false;
	std::string_view rest = line;
	std::string_view field[7];
	for (auto& out : field)
	{
		size_t end = rest.find(' ');
		if (end == std::string_view::npos) return false;
		out = rest.substr(0, end);
		rest.remove_prefix(end + 1);
	}
	return field[0] == "v1" && field[1] == session && field[6] == "ready";
}

// ---- the running game's registration -------------------------------------------------------------------------------
// "v1 <S> <leader pid> <start ticks> <rom>" in /run/banana/game.session. The game's own process writes it,
// atomically, after setsid() and before game.pid; the UI removes it once the child is reaped. bananad acts for a
// session only while this names a live group leader with the same start ticks, so a reused PID, a stale file, or a
// registration retired in the middle of a publication cannot stand in for the running game.
struct ResumeRegistration
{
	std::string session, rom;
	pid_t pid = 0;
	unsigned long long start = 0;
};

// Field 22 of /proc/<pid>/stat, in clock ticks since boot.
static bool resumeProcStart(pid_t pid, unsigned long long& start)
{
	std::ifstream f("/proc/" + std::to_string((long)pid) + "/stat");
	std::string line;
	if (!std::getline(f, line)) return false;
	const size_t paren = line.rfind(')');
	if (paren == std::string::npos || paren + 2 >= line.size()) return false;
	std::string_view rest = std::string_view(line).substr(paren + 2);	// state is field 3
	for (int skipped = 0; skipped < 19; ++skipped)
	{
		const size_t space = rest.find(' ');
		if (space == std::string_view::npos) return false;
		rest.remove_prefix(space + 1);
	}
	const std::string_view digits = rest.substr(0, rest.find(' '));
	unsigned long long value = 0;
	if (digits.empty()) return false;
	for (char c : digits)
	{
		if (c < '0' || c > '9' || value > (UINT64_MAX - 9) / 10) return false;
		value = value * 10 + (unsigned)(c - '0');
	}
	start = value;
	return value != 0;
}

static bool resumeRegister(const std::string& run, const std::string& session, pid_t pid, const std::string& rom)
{
	unsigned long long start = 0;
	if (!resumeHex(session, 32) || !resumeRom(rom) || !resumeProcStart(pid, start)) return false;
	const std::string path = run + "/game.session", tmp = path + ".tmp";
	const std::string text = "v1 " + session + " " + std::to_string((long)pid) + " " + std::to_string(start) + " " +
	                         rom + "\n";
	if (unlink(tmp.c_str()) < 0 && errno != ENOENT) return false;
	int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) return false;
	bool ok = true;
	for (size_t off = 0; ok && off < text.size();)
	{
		ssize_t n = write(fd, text.data() + off, text.size() - off);
		if (n > 0) off += (size_t)n;
		else if (n < 0 && errno == EINTR) continue;
		else ok = false;
	}
	if (close(fd) < 0) ok = false;
	if (ok && rename(tmp.c_str(), path.c_str()) < 0) ok = false;
	if (!ok) unlink(tmp.c_str());
	return ok;
}

// Retired by the UI after it reaped the child and before any cleanup is scheduled. A shutdown keeps it: bananad
// still has to prove which session it is stopping.
static void resumeRetire(const std::string& run)
{
	if (unlink((run + "/game.session").c_str()) < 0 && errno != ENOENT) logf("resume: could not retire the game registration");
}

enum class ResumeStart { Go, Latched, Failed };
// The forked game's own start, in the one order that closes the race with a shutdown. rcK and bananad create the
// latch first and read game.pid second; the child publishes game.pid first and looks at the latch second. In every
// interleaving either the shutdown reads this child's group (and signals it) or the child sees the latch and never
// executes the game, so no group runs that no shutdown can name. A latch that exists before anything is published
// stops the child before it registers. A child that cannot register or publish is not started either: nothing could
// attribute a record to it or stop it.
static ResumeStart resumeChildStart(const std::string& run, const std::string& session, pid_t pid, const std::string& rom)
{
	if (resumeLatched(run)) return ResumeStart::Latched;
	if (!resumeRegister(run, session, pid, rom)) return ResumeStart::Failed;
	{
		std::ofstream path(run + "/game.path");
		path << rom << "\n";
		path.close();
		if (!path) return ResumeStart::Failed;
	}
	{
		std::ofstream tracked(run + "/game.pid");
		tracked << pid << "\n";
		tracked.close();
		if (!tracked) return ResumeStart::Failed;
	}
	return resumeLatched(run) ? ResumeStart::Latched : ResumeStart::Go;
}

// The forked game's signal state. banana-ui handles TERM, INT, HUP, USR1 and USR2 with a handler that writes to its
// own self-pipe (main.cpp onSignal) and ignores SIGPIPE; a fork inherits all of that, and only exec resets it. The
// child is exposed to a shutdown's group TERM from the moment it publishes game.pid, and that TERM must end it:
// delivered to the inherited handler it would be swallowed (and written into the UI's pipe) and the child would
// execute the game afterwards. So the dispositions are restored before anything is published, not after the last
// latch look. Nothing is blocked either: a TERM then terminates the child wherever it is before exec.
static void resumeChildSignals()
{
	for (int sig : {SIGTERM, SIGINT, SIGHUP, SIGUSR1, SIGUSR2, SIGPIPE}) signal(sig, SIG_DFL);
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, nullptr);
}

// What the forked child runs first (main.cpp runGame): signals effective, then the ordered publication above.
static ResumeStart resumeChildEnter(const std::string& run, const std::string& session, pid_t pid, const std::string& rom)
{
	resumeChildSignals();
	return resumeChildStart(run, session, pid, rom);
}

static bool resumeReadRegistration(const std::string& run, ResumeRegistration& out)
{
	out = {};
	char bytes[PATH_MAX + 160];
	std::string_view lines, line;
	if (resumeReadFile(run + "/game.session", bytes, sizeof bytes, lines) != Resume::Ready ||
	    !resumeLine(lines, line) || !lines.empty()) return false;
	std::string_view field[4];
	for (auto& f : field)
	{
		const size_t space = line.find(' ');
		if (space == std::string_view::npos || space == 0) return false;
		f = line.substr(0, space);
		line.remove_prefix(space + 1);
	}
	auto number = [](std::string_view digits, unsigned long long limit, unsigned long long& value)
	{
		value = 0;
		if (digits.empty()) return false;
		for (char c : digits)
		{
			if (c < '0' || c > '9' || value > (limit - 9) / 10) return false;
			value = value * 10 + (unsigned)(c - '0');
		}
		return value != 0;
	};
	unsigned long long pid = 0, start = 0;
	if (field[0] != "v1" || !resumeHex(field[1], 32) || !number(field[2], INT_MAX, pid) ||
	    !number(field[3], UINT64_MAX, start) || !resumeRom(line)) return false;
	out = {std::string(field[1]), std::string(line), (pid_t)pid, start};
	return true;
}

// ---- one launch at a time ---------------------------------------------------------------------------------------
// Who launched (S), which record was attempted (R), and who published the visible record are three different
// facts, and no two are ever matched by ROM path. The flow keeps them apart:
//   - a boot reservation counts an attempt against R, durably, before launch; it belongs to no session yet;
//   - it attaches to S only when that exact reservation starts the child (same ROM, same kind of launch);
//   - a manual launch, a launch hook or a later game never inherits it, so one game's return cannot spend another
//     record's attempts;
//   - cleanup is one immutable job per session, so a later session cannot overwrite it, and the next launch is held
//     (not dropped) until it is done or its window expires;
//   - once the shutdown latch exists the daemon owns the record: nothing is admitted, settled or retired here.
class ResumeFlow
{
public:
	enum class Request { Run, Hold, Drop };
	enum class Readiness { No, Busy, Settled };
	enum class End { Idle, Latched, Kept, Rejected, Cleaning };
	enum class Phase { Idle, Running, Ready };	// of the launch's reservation: none/settled, awaiting readiness, ready

	struct Launch
	{
		bool active = false;
		std::string session;
		ResumeRecord reserved;
		Phase phase = Phase::Idle;
	};

	ResumeFlow(std::string dataDir, std::string runDir): data(std::move(dataDir)), run(std::move(runDir)) {}

	const std::string data, run;
	ResumeRecord boot;	// what resumePlan found at start; attached to a launch only through reserve() and started()
	ResumeCleanup cleanup;
	Launch launch;

	bool latched() const { return resumeLatched(run); }

	// A launch request names its game and never a position in a shelf: shelves are re-sorted by every rescan and
	// every favourite, so an index held for a moment can be a different game when it runs.
	struct Target
	{
		std::string system;	// the id of the system or app that owns the game; empty: the request names a path only
		std::string rom;	// the game's path
		int shelf = -1;		// the shelf the press came from: where the starter looks first, never what it starts
		// The Store entry is a press on a shelf (never a launch-file or boot request) that opens a screen in the menu.
		bool store() const { return shelf >= 0 && rom == "banana:store"; }
	};
	enum class Source { Menu, File };
	enum class Launched { Started, Opened, Missing, Failed };
	// Resolve the target in the library and start it (runGame). `attempt` is the boot record when this is that
	// record's own launch, else null. Started: a child was forked. Opened: the menu handled the request itself
	// (the Store screen); no child exists, so nothing was admitted that a game's lifetime could own.
	using Starter = std::function<Launched(const Target&, const ResumeRecord* attempt)>;
	enum class Admit { Idle, Held, Dropped, Started, Opened, Missing, Failed };
	// DeferredUnsettled / MissingUnsettled: the same decisions, but settling the record met the publication lock
	// or the filesystem and did not complete. The record is owed a retry (stepOwed), never reported as settled.
	enum class Boot { Latched, Deferred, DeferredUnsettled, Uncounted, Missing, MissingUnsettled, Started, Failed };

	// The launch file (/run/banana/launch) is a one-slot mailbox with one consumer, this flow.
	//   publish   a writer renames a complete file into place (a plain write is seen empty or half-written, and a
	//             request taken in that state is lost). A later publication replaces an earlier unread one.
	//   consume   claimRequest() renames the file to launch.claim, a name no writer uses, and reads and removes that
	//             one inode. rename(2) takes exactly the file at the path at that instant, so a replacement published
	//             at any later point is a new request this call never read, moved or deleted. No code here unlinks the
	//             pathname itself.
	//   precedence  a shutdown latch or a running game drops it; a pending cleanup holds it unread; a menu press goes
	//             first and the file waits behind it; the boot record yields to it.
	//   discard   a game's lifetime runs from its admission until its reap. A request that exists when the game
	//             is admitted (it waited behind the press) was made during that lifetime, and so was one visible
	//             at the reap's claim: reap() makes that claim as the first thing after a successful waitpid, before
	//             any teardown, and it is the cutoff. Such a request is discarded, never run afterwards because the
	//             game ended before the next pass looked at it. The cutoff is the atomic claim, not the exact moment
	//             the child exited: a request published after it -- during teardown included -- is a new request.
	//             A press that starts nothing (its game gone, a failed fork, the Store screen) is not a game's
	//             lifetime and discards nothing.

	// What to do with a launch request (a menu press or the launch file) on this pass. Hold keeps the request
	// exactly where it is, unread and unexecuted, until the previous session's cleanup is done or its window has
	// expired; Drop is a game already running, or a shutdown that started: nothing may start. A menu press and a
	// launch file that are both waiting never share a slot: the press goes first, and the file stays on disk unread
	// until the press has started its game (and is then dropped, as any request is while a game runs) or was
	// dropped itself.
	// A menu press is the player's request from the moment the launch animation starts, but it reaches queue() only
	// when the animation ends. Until then a launch file or the boot record must not take the slot: the press would
	// then be dropped as "a game is running". main.cpp sets this each pass from the menu's launch view.
	bool pressing = false;

	Request request(bool gameRunning, Source source = Source::Menu) const
	{
		if (latched() || gameRunning) return Request::Drop;
		if (cleanup.pending()) return Request::Hold;
		// A newer press that is still animating supersedes a queued older one (queue() replaces it when the
		// animation ends), so the older press must not be admitted in between: latest intent wins, for both sources.
		if (pressing) return Request::Hold;
		return source == Source::File && hasQueued ? Request::Hold : Request::Run;
	}
	bool deferred(int gamePid, int pendingLaunch) const
	{
		return resumeDeferred(run, gamePid, pendingLaunch, cleanup.pending() || hasQueued || pressing);
	}

	// A menu press: kept by identity until admitted. A later press replaces it (the player changed their mind).
	void queue(const Target& target) { queued = target; hasQueued = true; }
	bool queuedLaunch() const { return hasQueued; }
	// The loop must not block in poll() while a press can move: one the menu has just finished animating (pending in
	// the library, not yet queued) or a queued one that nothing but the animation was holding back. Neither has a
	// timer of its own, so the loop would otherwise sleep until the next key or the minute tick. A queued press that
	// waits for a cleanup is woken by the cleanup's timer instead.
	bool admissionDue(bool pendingPress, bool launchAnimating) const
	{
		return pendingPress || (hasQueued && !launchAnimating && !cleanup.pending());
	}

	Admit admitMenu(bool gameRunning, const Starter& start)
	{
		if (!hasQueued) return Admit::Idle;
		const Request decision = request(gameRunning, Source::Menu);
		if (decision == Request::Hold) return Admit::Held;
		const Target target = queued;
		hasQueued = false;
		if (decision == Request::Drop) return Admit::Dropped;
		const Admit result = admitted(start(target, nullptr));
		// A launch file that waited behind this press was made while the press was in flight, so it belongs to the
		// game's lifetime and is discarded with the press (see the mailbox rules above). Only a child counts: a press
		// that started nothing (its game gone, a failed fork) leaves the file eligible, and so does the Store screen.
		if (result == Admit::Started && claimRequest(nullptr)) logf("launch file ignored: a menu launch was admitted first");
		return result;
	}

	std::string lastFileRequest;	// the launch file's text as last read, for the log

	Admit admitFile(bool gameRunning, const Starter& start)
	{
		if (access((run + "/launch").c_str(), F_OK) != 0) return Admit::Idle;
		const Request decision = request(gameRunning, Source::File);
		if (decision == Request::Hold) return Admit::Held;	// the file stays, unread
		std::string want;
		if (!claimRequest(&want)) return Admit::Idle;	// it vanished since the probe: nothing was taken
		lastFileRequest = want;
		if (decision == Request::Drop) return Admit::Dropped;
		if (want.empty()) return Admit::Missing;
		Target target;
		target.rom = want;
		return admitted(start(target, nullptr));
	}

	// The boot resume, once its window has closed. The record's attempt is counted, started through this call and
	// no other path, and settled by how that launch goes; nothing else can take it.
	Boot bootResume(int gamePid, const Starter& start)
	{
		if (latched()) return Boot::Latched;
		if (deferred(gamePid, -1)) return settleBoot(boot) ? Boot::Deferred : Boot::DeferredUnsettled;
		if (!reserve(boot)) return Boot::Uncounted;
		Target target;
		target.system = boot.system;
		target.rom = boot.rom;
		const Launched result = start(target, &boot);
		// No child started (the fork failed): the counted record stays for the next boot. A launch command found
		// incompatible has already settled it, or owes the settlement (settleBoot).
		release();
		if (result == Launched::Missing) return settleBoot(boot) ? Boot::Missing : Boot::MissingUnsettled;
		return result == Launched::Started ? Boot::Started : Boot::Failed;
	}

	// Settle the boot record, or owe it. A refusal (the lock was busy or the filesystem failed) is kept and retried by
	// stepOwed() on the main loop's timer, for the same bounded window as a return's cleanup; it is never dropped
	// silently. Under the latch nothing is owed: the daemon owns the record.
	bool settleBoot(const ResumeRecord& record)
	{
		if (record.rom.empty() || settle(record)) return true;
		if (!latched())
		{
			owed = record;
			hasOwed = true;
			owedAttempts = 0;
			owedDue = 0;
			owedDeadline = -1;
		}
		return false;
	}
	bool owing() const { return hasOwed; }
	double owedAt() const { return owedDue; }
	// An unlink this process made whose directory sync has not succeeded (see resumeSyncDebt): the record is gone
	// but its removal is not durable, whatever a settlement says about the record itself.
	bool syncOwed() const { return resumeSyncOwed(data); }
	ResumeCleanup::Step stepOwed(double now)
	{
		using Step = ResumeCleanup::Step;
		if (!hasOwed) return Step::Idle;
		if (latched()) { hasOwed = false; return Step::Halted; }
		if (now < owedDue) return Step::Idle;
		if (owedDeadline < 0) owedDeadline = now + ResumeCleanup::windowMs;
		if (resumeConfirm(data, owed, run)) { hasOwed = false; return Step::Done; }
		if (++owedAttempts >= ResumeCleanup::maxAttempts || now >= owedDeadline)
		{
			hasOwed = false;
			return Step::Exhausted;
		}
		owedDue = now + ResumeCleanup::intervalMs;
		return Step::Retried;
	}

	// The boot decision. A shutdown that already owns the record leaves it as it is.
	Resume plan() { return resumePlan(data, boot, run); }

	// Count one attempt against `record` and hold it for the launch that is about to start.
	bool reserve(const ResumeRecord& record)
	{
		if (latched() || !resumeBegin(data, record, run)) return false;
		pending = record;
		hasPending = true;
		return true;
	}
	void release() { hasPending = false; }
	bool reserving() const { return hasPending; }

	// Spend the exact record, unless a shutdown owns it. False: nothing was settled.
	bool settle(const ResumeRecord& record) const { return !latched() && resumeConfirm(data, record, run); }

	// The child of `session` was forked. `attempt` is the boot record when this launch is that record's own attempt
	// (the explicit call path of bootResume); every other launch -- a menu press, the launch file, an app -- passes
	// null and so can never take a reservation, whatever ROM or kind it names.
	void started(const std::string& session, const std::string& rom, const ResumeRecord* attempt)
	{
		launch = {};
		launch.active = true;
		launch.session = session;
		// The first launch after a reservation consumes it. It attaches only if it is the reserved attempt itself;
		// any other launch lets it lapse, unspent, with the record and its count left for the next boot.
		const bool mine = attempt && hasPending && resumeSameRecord(pending, *attempt) && pending.rom == rom;
		hasPending = false;
		if (!mine) return;
		launch.reserved = pending;
		if (!pending.core.empty()) { launch.phase = Phase::Running; return; }
		// RetroArch has no readiness signal: a started game is its confirmation.
		if (settle(launch.reserved)) launch.reserved = {};
		else launch.phase = Phase::Ready;
	}

	// The launcher published this session as ready. Readiness is remembered even if settling met the lock.
	Readiness pollReady()
	{
		if (!launch.active || launch.phase != Phase::Running || latched() ||
		    !resumeNativeReady(run + "/native-session", launch.session)) return Readiness::No;
		if (resumeConfirm(data, launch.reserved, run))
		{
			launch.reserved = {};
			launch.phase = Phase::Idle;
			return Readiness::Settled;
		}
		launch.phase = Phase::Ready;
		return Readiness::Busy;
	}

	// The main loop's reap of the tracked child, in the order it must run. `wait` is waitpid(pid, &status, WNOHANG)
	// (injected so the order can be tested); false: the child is still running and nothing happened. Once it was
	// reaped (or was already gone: ECHILD, reported as SIGCHLD) its lifetime is over, and the launch request
	// visible at that instant was published while it ran. It is discarded here, by claiming it, before `teardown`
	// does anything slow (performance commands, history, audio) and before any later pass can look at the path:
	// that claim is the request-lifetime cutoff. Whatever is published afterwards is a new request and is left
	// alone. `teardown` receives the wait status and ends with ended().
	template <class Wait, class Teardown>
	bool reap(int pid, Wait&& wait, Teardown&& teardown)
	{
		int status = 0;
		const pid_t r = wait(pid, &status);
		if (r != pid && !(r < 0 && errno == ECHILD)) return false;
		if (claimRequest(nullptr)) logf("launch file ignored: it was published while a game ran");
		teardown(r == pid ? status : SIGCHLD);
		return true;
	}

	// The reaped child's registration is retired and the one cleanup job for its session is scheduled. The launch
	// request was already dealt with by reap(); this never touches it.
	End ended(int status, double now)
	{
		if (!launch.active) return End::Idle;
		const Launch done = launch;
		launch = {};
		const std::string readyFile = run + "/native-ready-" + done.session;
		const bool ready = done.phase == Phase::Ready ||
			(done.phase == Phase::Running && access(readyFile.c_str(), F_OK) == 0);
		unlink(readyFile.c_str());
		if (latched()) return End::Latched;	// the daemon owns the record; the registration stays for it
		resumeRetire(run);
		ResumeJob job;
		job.session = done.session;
		job.reserved = done.reserved;
		End end = End::Cleaning;
		if (done.phase == Phase::Running && !ready)
		{
			// Before readiness only the launcher's rejection settles a typed boot record; anything else is a
			// failed attempt that keeps the record and its count for the next boot.
			if (resumeEnd(status) != ResumeEnd::Rejected) return End::Kept;
			job.settle = true;
			end = End::Rejected;
		}
		else
		{
			job.settle = !done.reserved.rom.empty();
			job.clean = true;
		}
		return cleanup.schedule(job, now) ? end : End::Kept;
	}

	ResumeCleanup::Step step(double now) { return cleanup.step(now, data, run, latched()); }

private:
	// Take ownership of the launch request at the pathname right now (the mailbox rules above). false: none was
	// there, or it could not be taken, in which case it is left exactly where it is.
	bool claimRequest(std::string* text) const
	{
		const std::string claim = run + "/launch.claim";
		if (rename((run + "/launch").c_str(), claim.c_str()) != 0)
		{
			if (errno != ENOENT) logf("launch file: cannot take the request: %s", strerror(errno));
			return false;
		}
		if (text)
		{
			text->clear();
			std::ifstream f(claim);
			std::getline(f, *text);
		}
		unlink(claim.c_str());
		return true;
	}
	static Admit admitted(Launched result)
	{
		switch (result)
		{
		case Launched::Started: return Admit::Started;
		case Launched::Opened: return Admit::Opened;
		case Launched::Missing: return Admit::Missing;
		case Launched::Failed: break;
		}
		return Admit::Failed;
	}
	ResumeRecord pending;
	bool hasPending = false;
	ResumeRecord owed;
	bool hasOwed = false;
	int owedAttempts = 0;
	double owedDue = 0, owedDeadline = -1;
	Target queued;
	bool hasQueued = false;
};

static constexpr char bootResumeLauncher[] = "/usr/libexec/banana/run-retroarch";

// Only the stock three-word command can carry a typed resume. Do not interpret
// or replace a user's shell program with a different command on boot.
static std::string resumeCommandCore(const SystemDef& system, std::string_view expectedSystem)
{
	if (system.app || system.id != expectedSystem) return {};
	std::string_view command = system.run;
	auto word = [&]()
	{
		size_t start = command.find_first_not_of(" \t");
		if (start == std::string_view::npos) return std::string_view{};
		command.remove_prefix(start);
		std::string_view result = command.substr(0, command.find_first_of(" \t"));
		command.remove_prefix(result.size());
		return result;
	};
	if (word() != bootResumeLauncher) return {};
	std::string_view core = word();
	if (!resumeProfileId(core) || core.front() == '-' || word() != "%ROM%" || !word().empty()) return {};
	return std::string(core);
}

static std::pair<int, int> launchGameLocation(Library& lib, const std::string& rom, std::string_view system = {})
{
	for (int s = 0; s < lib.systemCount(); ++s)
	{
		const Shelf& shelf = lib.shelf(s);
		if (shelf.favorites || (!system.empty() && shelf.def.id != system)) continue;
		for (int i = 0; i < (int)shelf.games.size(); ++i)
			if (shelf.games[i].path == rom) return {s, i};
	}
	return {-1, -1};
}

// The game a target names, found by system and path now: the shelf the press came from is looked at first (so a launch
// from Favorites or Recent stays on that shelf), then the library. Never a position carried over from when the request
// was made. (-1, -1): the game is not there any more.
static std::pair<int, int> launchTargetLocation(Library& lib, const ResumeFlow::Target& target)
{
	const int current = lib.currentShelf();
	if (target.shelf >= 0 && target.shelf == current && current < lib.systemCount())
	{
		const Shelf& shelf = lib.shelf(current);
		for (int i = 0; i < (int)shelf.games.size(); ++i)
			if (shelf.games[i].path == target.rom && (target.system.empty() || lib.systemOf(i).id == target.system))
				return {current, i};
	}
	const std::pair<int, int> found = launchGameLocation(lib, target.rom, target.system);
	return found.first < 0 && !target.system.empty() ? lib.appEntry(target.system, target.rom) : found;
}


// ---- the Source with a few host settings on top -------------------------------------------------------------------
struct Host : Library
{
	using Library::Library;
	bool motion = true;
	bool topMotion() override { return motion; }
	std::string batteryShown = "icon";		// "icon", or "text": the percentage too
	std::string batteryMode() override { return batteryShown; }
	std::string boxLookShown = "pixel";
	std::string boxLook() override { return boxLookShown; }
	bool blue = false;		// blue_banana: the Genesis style's secret
	bool blueBanana() override { return blue; }
};


struct Options
{
	std::string assets = "/usr/share/banana/rnds", splash = "/usr/share/banana/splash", banana = "/usr/share/banana/banana";
	std::string systems = "/etc/banana/systems.conf", roms = "/data/roms", data = "/data/banana";
	std::string top = "DSI-1";
	std::string headless, script;
	std::string theme;		// --theme FILE: that theme's .json instead of the settings' (a theme being made)
	int w = 640, h = 480;
};

static std::string envOr(const char* name, const std::string& def)
{
	const char* v = getenv(name);
	return v && *v ? v : def;
}

// ---- held buttons repeat, like a list in any frontend -----------------------------------------------------------
struct Repeat
{
	Key key = Key::NONE;
	double since = 0, last = 0;
	bool due(double now)
	{
		if (key == Key::NONE) return false;
		double held = now - since, gap = held > 1500 ? 60 : 120;
		if (held < 400 || now - last < gap) return false;
		last = now;
		return true;
	}
	int wait(double now) const
	{
		if (key == Key::NONE) return -1;
		double held = now - since;
		if (held < 400) return (int)std::ceil(400 - held);
		double gap = held > 1500 ? 60 : 120;
		return (int)std::max(0.0, std::ceil(last + gap - now));
	}
};

static bool uiButton(Key k, Menu::Button& b)
{
	switch (k)
	{
	case Key::LEFT: b = Menu::LEFT; return true;
	case Key::RIGHT: b = Menu::RIGHT; return true;
	case Key::UP: b = Menu::UP; return true;
	case Key::DOWN: b = Menu::DOWN; return true;
	case Key::A: b = Menu::A; return true;
	case Key::B: b = Menu::B; return true;
	case Key::X: b = Menu::X; return true;
	case Key::Y: b = Menu::Y; return true;
	case Key::L: b = Menu::L; return true;
	case Key::R: b = Menu::R; return true;
	default: return false;
	}
}

static const char* soundOf(Key k, Menu::View before, Menu::View after)
{
	switch (k)
	{
	case Key::LEFT: case Key::RIGHT: case Key::UP: case Key::DOWN: case Key::L: case Key::R: return "scroll";
	case Key::A: return after == Menu::LAUNCH && before != Menu::LAUNCH ? "ready" : "select";
	case Key::B: return "back";
	case Key::Y: return "fav";
	case Key::X: return "select";
	default: return nullptr;
	}
}

// The shoulder buttons jump by letter in a game list; the D-pad still steps one game at a time. R: the next title
// letter's first game; L: this letter's first game, or the previous letter's. The list is sorted by title, so a
// letter's games are one run (Library::letterStep). In a library only -- on the home shelf L/R still walk the
// systems -- and true only when it consumed the key (so the caller does not then nudge by one).
static bool letterJump(Menu& ui, Library& lib, Menu::Button b)
{
	if ((b != Menu::L && b != Menu::R) || ui.view() != Menu::LIBRARY || lib.gameCount() <= 1) return false;
	int at = ui.gameIndex();
	int to = lib.letterStep(at, b == Menu::R ? 1 : -1);
	if (to >= 0 && to != at) ui.jumpTo(to);
	return true;
}

// a favourite starred or unstarred: once the shelf shows, the Favorites collection and the favourite systems are put
// right, and the cursor stays on the system it was on
template <class U> static bool rearrange(Host& lib, U& ui)
{
	if (!lib.favouritesChanged() || ui.view() != Menu::HOME) return false;
	int at = ui.systemIndex();
	std::string id = at >= 0 && at < lib.systemCount() ? lib.shelf(at).def.id : "";
	lib.arrange();
	int to = 0;
	for (int i = 0; i < lib.systemCount(); i++)
		if (lib.shelf(i).def.id == id) to = i;
	ui.dataChanged();
	ui.setHome(to);
	return true;
}

// ---- games copied onto the card while the menu runs (file sharing) ------------------------------------------------
// inotify on the ROM folders; the library is read again once the copying has been quiet for 2 s, and only while the
// menu shows its home screen (a game list under the cursor isn't reshuffled)
struct RomWatch
{
	int fd = -1;
	double due = 0;
	std::map<int, std::string> dirOf;	// each watch's folder
	static constexpr uint32_t EVENTS = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE;
	void add(const std::string& d)
	{
		int wd = inotify_add_watch(fd, d.c_str(), EVENTS);
		if (wd >= 0) dirOf[wd] = d;
	}
	void watch(const std::vector<std::string>& dirs)
	{
		if (fd < 0) fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
		if (fd < 0) return;
		for (auto& d : dirs) add(d);	// a folder that isn't there yet is caught by its parent's watch, then added here
	}
	void read(double now)
	{
		alignas(inotify_event) char buf[4096];
		bool any = false;
		for (ssize_t n; (n = ::read(fd, buf, sizeof buf)) > 0;)
		{
			any = true;
			// a folder made or moved in (a game folder copied over file sharing): watched at once, so the games
			// copied into it afterwards are seen too (the rescan 2 s after the folder came may find it still empty)
			for (char* p = buf; p < buf + n;)
			{
				auto* e = (inotify_event*)p;
				auto it = dirOf.find(e->wd);
				if ((e->mask & IN_ISDIR) && (e->mask & (IN_CREATE | IN_MOVED_TO)) && e->len && it != dirOf.end() && e->name[0] != '.')
					add(it->second + "/" + e->name);
				p += sizeof(inotify_event) + e->len;
			}
		}
		if (any) due = now + 2000;
	}
};

// ---- boot timing: seconds since the kernel started, for logs/boot.txt (rcS gathers /run/banana/boot-times) ------
static void bootMark(const char* what)
{
	timespec t;
	clock_gettime(CLOCK_BOOTTIME, &t);
	double s = t.tv_sec + t.tv_nsec / 1e9;
	logf("%s at %.2f s after the kernel started", what, s);
	if (FILE* f = fopen("/run/banana/boot-times", "a")) { fprintf(f, "%-28s %6.2f s\n", what, s); fclose(f); }
}

// ---- the low-battery notice ---------------------------------------------------------------------------------------
// At 20%, 10% and 5% on the battery: a notice over the top panel for 8 s, which any
// button hides. Each step once, until the charger has been on; a step passed during a game shows when the menu is
// back. bananad does the shutting down (battery_shutdown_percent); this only says it is coming.
struct LowBattery
{
	int shownStep = 101, percent = -1;
	double until = 0;
	std::string title, body;

	bool showing(double now) const { return until > now; }
	// true when the notice came or went
	bool check(const Library::Power& p, int shutdownAt, double now)
	{
		if (p.percent < 0) return false;
		if (p.charging)
		{
			shownStep = 101;
			return hide(now);
		}
		int step = p.percent <= 5 ? 5 : p.percent <= 10 ? 10 : p.percent <= 20 ? 20 : 0;
		if (!step || step >= shownStep) return false;
		shownStep = step;
		percent = p.percent;
		until = now + 8000;
		std::string off = std::to_string(shutdownAt) + "%";
		if (step == 5)
		{
			title = tr("Battery nearly empty") + ": " + std::to_string(percent) + "%";
			body = tr("Save your game now. Banana Pocket closes it and shuts down at ") + off + ".";
		}
		else
		{
			title = tr("Battery low") + ": " + std::to_string(percent) + "%";
			body = step == 10 ? tr("Plug in the charger soon. Banana Pocket closes the game and shuts down at ") + off + "."
				: tr("Time to find the charger.");
		}
		return true;
	}
	bool hide(double now)
	{
		bool was = showing(now);
		until = 0;
		return was;
	}
};

// A charger plugged in or out: a short notice ("Charging" / "On battery") on the same overlay as the low-battery one.
// The status bar's battery also grows a bolt while charging (rnds::Source::charging, from the same power_supply);
// bananad plays the vibration.
struct PlugNotice
{
	int seen = -1;	// -1: not known yet (no notice at boot), 0: on battery, 1: charging
	double until = 0;
	std::string title, body;
	int percent = -1;
	bool showing(double now) const { return until > now; }
	bool check(const Library::Power& p, double now)
	{
		int s = p.charging ? 1 : 0;
		if (seen < 0 || s == seen) { seen = s; return false; }
		seen = s;
		until = now + 2500;
		percent = p.percent;
		title = s ? "Charging" : "On battery";
		body = p.percent >= 0 ? std::to_string(p.percent) + (s ? "% and charging" : "% left")
			: (s ? "The charger is in" : "The charger is out");
		return true;
	}
};

// The overlay's precedence, in one place for the menu (main.cpp) and the headless harness (headless.cpp), so the tests
// run the same choice the handheld makes: the battery notice, else the charger's, else the volume/brightness level,
// else the pause menu (pauseDraw draws it and returns true when it is open), else the achievement toasts. It returns
// which one it chose, so a test can say what was on the screen and not only that something was.
template <class View, class PauseDraw>
static const char* drawOverlay(View& v, const LowBattery& low, const PlugNotice& plug, const std::string& osdLabel, int osdPercent,
	double osdUntil, double t, PauseDraw pauseDraw)
{
	if (low.showing(t)) { v.notice(low.title, low.body, low.percent); return "battery"; }
	if (plug.showing(t)) { v.notice(plug.title, plug.body, plug.percent, plug.seen == 1); return "plug"; }
	if (osdUntil > t) { v.level(osdLabel, osdPercent); return "level"; }
	if (pauseDraw()) return "pause";
	v.drawToast(t);
	return "toast";
}

static int shutdownPercent(const std::string& data)
{
	return atoi(getConfValue(data + "/bananad.conf", "battery_shutdown_percent",
		getConfValue("/etc/banana/bananad.conf", "battery_shutdown_percent", "3")).c_str());
}

// ---- the charger's uevents -----------------------------------------------------------------------------------------
// The menu asks /sys for the battery once a minute (and after a press), so a cable going in used to sit unnoticed
// until something else redrew the top bar. Watch the kernel's uevent group instead: a power_supply event is the
// charger going in or out, which repaints the battery (and its bolt) and brings the plug notice up at once.
static int powerUeventOpen()
{
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
	if (fd < 0) return -1;
	struct sockaddr_nl a = {};
	a.nl_family = AF_NETLINK;
	a.nl_groups = 1;	// the kernel's uevents
	if (bind(fd, (struct sockaddr*)&a, sizeof a) < 0) { close(fd); return -1; }
	return fd;
}

// true when what was queued includes a power_supply event (the rest of the group is dropped: input, mmc...)
static bool powerUeventDrain(int fd)
{
	char buf[8192];
	bool hit = false;
	for (;;)
	{
		ssize_t n = read(fd, buf, sizeof buf - 1);
		if (n <= 0) break;
		buf[n] = 0;
		for (ssize_t i = 0; i < n; )
		{
			const char* s = buf + i;
			size_t l = strnlen(s, (size_t)(n - i));
			if (l && !strncmp(s, "SUBSYSTEM=power_supply", 22)) hit = true;
			i += (ssize_t)l + 1;
		}
	}
	return hit;
}


// headless.cpp: the test harness; boot_anim.cpp: the splash and the iris into the menu
int runHeadless(const Options& o, Host& lib, const std::string& variant, const std::string& theme, double scanMs);
void playSplash(Display& disp, SoftBackend& be, const Options& o, const std::string& variant, bool on,
	std::atomic<bool>& ready, std::mutex& readyM, std::condition_variable& readyCv, Sound& sound, int sig, bool waitAudio);
void forgetSplash();	// free its layers once it is over
void irisToMenu(Display& disp, SoftBackend& be, Menu& ui);
