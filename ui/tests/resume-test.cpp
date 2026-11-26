// The wake path's recovery policy (src/resume.h, used by main.cpp's SIGUSR1 handler): a failed panel restore with a game
// resident must neither reopen the panel (that gives up DRM master) nor quit; with no game it reopens. Built and run
// by ui/tests/run.sh (no DRM device needed).
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

// Fault injection at the exact boundaries of the production lifecycle, in the house style of bananad's fixtures
// (they rename a syscall before including the code under test). The production code runs unmodified; each hook fires
// once, at the moment named, so a test can make the shutdown latch appear between two steps deterministically.
static void (*afterLock)();		// right after a flock(LOCK_EX) on a lock file succeeded: the lock is held
static void (*afterRegister)();		// right after game.session was renamed into place: registered, game.pid not yet written
static void (*beforeLatchLook)(int);	// before each look at the shutdown latch, with its ordinal (1, 2, ...)
static int latchLooks;
// Once, while a launch request is being taken: right after it was renamed to launch.claim, or -- for any code that
// consumes it by pathname -- right before /run/banana/launch is unlinked. A writer can publish a replacement there.
static void (*inRequestWindow)();
static void requestWindow()
{
	void (*hook)() = inRequestWindow;
	inRequestWindow = nullptr;
	hook();
}
static int hookedUnlink(const char* path)
{
	const size_t n = strlen(path);
	if (inRequestWindow && n >= 7 && !strcmp(path + n - 7, "/launch")) requestWindow();
	return unlink(path);
}
static int hookedFlock(int fd, int op)
{
	const int r = flock(fd, op);
	if (r == 0 && (op & LOCK_EX) && afterLock) { void (*hook)() = afterLock; afterLock = nullptr; hook(); }
	return r;
}
static int hookedRename(const char* from, const char* to)
{
	const int r = rename(from, to);
	if (r == 0 && afterRegister && strstr(to, "/game.session")) { void (*hook)() = afterRegister; afterRegister = nullptr; hook(); }
	if (r == 0 && inRequestWindow && strstr(to, "/launch.claim")) requestWindow();
	return r;
}
static int hookedAccess(const char* path, int mode)
{
	if (beforeLatchLook && strstr(path, "/shutting-down")) beforeLatchLook(++latchLooks);
	return access(path, mode);
}
static int failSyncs;	// the next N fsync calls fail with EIO (nothing is synced); 0: pass through
static int syncCalls;	// every fsync made, to prove a retry really asked the filesystem again
static int hookedFsync(int fd)
{
	++syncCalls;
	if (failSyncs > 0) { --failSyncs; errno = EIO; return -1; }
	return fsync(fd);
}
#define fsync hookedFsync
#define flock hookedFlock
#define rename hookedRename
#define access hookedAccess
#define unlink hookedUnlink

#include "../src/app.h"
#include "../src/resume.h"

using namespace banana;

static int bad = 0;
#define CHECK(cond, ...) do { if (!(cond)) { bad++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void bootResumeRecords()
{
	char scratch[] = "/tmp/banana-ui-resume-XXXXXX";
	if (!mkdtemp(scratch)) { CHECK(false, "create resume fixture directory"); return; }
	const std::string dir(scratch), marker = dir + "/resume", tries = dir + "/resume-tries";
	const std::string rom = dir + "/ Orchard Tactics.gba", spacedRom = rom + " ";
	const std::string token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	const std::string head = "banana-retro-v1\ngba:mgba\n";
	const std::string typed = head + token + "\n" + spacedRom + "\n";
	std::ofstream(rom) << "ROM";
	std::ofstream(spacedRom) << "ROM";
	auto write = [&](const std::string& bytes)
	{
		const std::string temp = marker + ".tmp";
		std::ofstream f(temp, std::ios::binary);
		f.write(bytes.data(), bytes.size());
		f.close();
		CHECK(f && rename(temp.c_str(), marker.c_str()) == 0, "publish a fixture record");
	};
	auto count = [&]
	{
		std::ifstream f(tries);
		std::string version;
		int n = -1;
		return std::getline(f, version) && f >> n ? n : -1;
	};

	ResumeRecord record;
	std::ofstream(tries) << "2\n";	// debt left with no record at all
	CHECK(resumePlan(dir, record) == Resume::None && access(tries.c_str(), F_OK) != 0,
	      "an orphaned bare count is cleared instead of surviving the upgrade");
	write(rom + " \r\n");
	std::ofstream(tries) << "3\n";	// a bare downgrade count names no record
	CHECK(resumePlan(dir, record) == Resume::Ready && record.rom == rom && record.system.empty() &&
	      record.core.empty() && record.token.empty(), "old path-only record keeps its trim/launch behavior");
	CHECK(access(tries.c_str(), F_OK) != 0, "a bare old count is cleared instead of blocking a downgrade");
	CHECK(resumeBegin(dir, record) && access(marker.c_str(), F_OK) == 0 && count() == 1,
	      "a counted legacy attempt keeps its marker until the game is known good");
	resumeConfirm(dir, record);
	CHECK(access(marker.c_str(), F_OK) != 0 && access(tries.c_str(), F_OK) != 0,
	      "confirming a legacy launch consumes marker and counter");

	write(typed);
	CHECK(resumePlan(dir, record) == Resume::Ready && record.rom == spacedRom && record.system == "gba" &&
	      record.core == "mgba" && record.token == token, "native identity and path spaces preserved byte-for-byte");
	if (geteuid() != 0)
	{
		CHECK(chmod(marker.c_str(), 0) == 0 && resumePlan(dir, record) == Resume::Unavailable,
		      "unreadable marker is retryable, not invalid");
		CHECK(chmod(marker.c_str(), 0600) == 0 && resumePlan(dir, record) == Resume::Ready,
		      "the same record is usable when the marker can be read again");
	}
	const ResumeRecord native = record;
	CHECK(resumeBegin(dir, native) && access(marker.c_str(), F_OK) == 0 && count() == 1,
	      "a counted native attempt keeps its marker");

	// Transient failures keep the record and its count, whatever the way a launch fails short of a rejection.
	const struct { int status; const char* what; } transient[] = {
		{ resumeExitRetry << 8, "the launcher's retry status" }, { 0, "a clean exit without readiness" }, { 1 << 8, "a generic failure" },
		{ 2 << 8, "a usage failure" }, { 127 << 8, "an exec failure" }, { SIGKILL, "SIGKILL" }, { SIGTERM, "SIGTERM" },
		{ SIGABRT, "an abort" } };
	for (const auto& t : transient)
	{
		resumeSettle(dir, native, t.status);
		CHECK(resumeEnd(t.status) == ResumeEnd::Retry && access(marker.c_str(), F_OK) == 0 && count() == 1,
		      "%s must keep the record and its count", t.what);
	}

	// Permanent rejection and cancelling settle the record.
	resumeSettle(dir, native, resumeExitRejected << 8);
	CHECK(resumeEnd(resumeExitRejected << 8) == ResumeEnd::Rejected && access(marker.c_str(), F_OK) != 0 &&
	      access(tries.c_str(), F_OK) != 0, "a permanent rejection forgets marker and counter");
	write(typed);
	CHECK(resumeBegin(dir, native), "attempt before cancelling");
	resumeConfirm(dir, native);
	CHECK(access(marker.c_str(), F_OK) != 0 && access(tries.c_str(), F_OK) != 0 &&
	      resumePlan(dir, record) == Resume::None, "cancelling clears both marker and retries");

	// Readiness consumes the marker, and spends the counter, so healthy resumes never exhaust the limit.
	write(typed);
	CHECK(resumeBegin(dir, native), "attempt before readiness");
	resumeConfirm(dir, native);
	CHECK(access(marker.c_str(), F_OK) != 0 && access(tries.c_str(), F_OK) != 0, "readiness consumes marker and counter");
	// ... but not a newer record bananad wrote for the session that is now running.
	const std::string token2 = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
	write(typed);
	CHECK(resumeBegin(dir, native), "attempt before a refreshed record");
	write(head + token2 + "\n" + spacedRom + "\n");
	resumeConfirm(dir, native);
	CHECK(resumeRead(dir, record) == Resume::Ready && record.token == token2 && access(tries.c_str(), F_OK) != 0,
	      "readiness keeps a newer record but spends the counter");
	resumeConfirm(dir, record);

	// Exhaustion: three transient failures on three boots, then the plan blocks and no fourth attempt starts.
	write(typed);
	for (int boot = 1; boot <= resumeMaxTries; ++boot)
	{
		CHECK(resumePlan(dir, record) == Resume::Ready && resumeBegin(dir, record), "boot %d still attempts the record", boot);
		resumeSettle(dir, record, resumeExitRetry << 8);
	}
	CHECK(access(marker.c_str(), F_OK) == 0 && count() == 3, "the record survives its last transient failure");
	CHECK(resumePlan(dir, record) == Resume::Blocked, "native record obeys the exhausted boot retry limit");
	CHECK(!resumeBegin(dir, record), "an exhausted record cannot start another attempt");
	CHECK(access(marker.c_str(), F_OK) != 0 && access(tries.c_str(), F_OK) != 0, "exhaustion is cleared with the record");

	// A replacement record starts its own budget: exhausting A must neither block nor delete B.
	write(typed);
	for (int boot = 1; boot <= resumeMaxTries; ++boot)
	{
		CHECK(resumePlan(dir, record) == Resume::Ready && resumeBegin(dir, record), "A attempts %d", boot);
		resumeSettle(dir, record, resumeExitRetry << 8);
	}
	write(head + token2 + "\n" + spacedRom + "\n");
	CHECK(resumePlan(dir, record) == Resume::Ready && record.token == token2, "B inherits no exhausted budget");
	CHECK(resumeBegin(dir, record) && count() == 1,
	      "B starts at its own first attempt");
	resumeConfirm(dir, record);
	CHECK(access(marker.c_str(), F_OK) != 0 && access(tries.c_str(), F_OK) != 0, "cancelling B clears B and its count");

	// Hold the daemon's lock in the parent; do not wait forever if the UI regresses to a blocking flock.
	auto contend = [&](const std::string& replacement, auto probe)
	{
		int publisher = resumeLock(dir), done[2];
		CHECK(publisher >= 0, "take the publication lock");
		if (publisher < 0) return;
		if (pipe(done) < 0)
		{
			CHECK(false, "create contention pipe");
			resumeUnlock(publisher);
			return;
		}
		pid_t child = fork();
		if (child == 0)
		{
			close(done[0]);
			resumeUnlock(publisher);	// the parent's descriptor still holds the lock
			_exit(probe() ? 0 : 1);
		}
		close(done[1]);
		CHECK(child > 0, "fork the contending UI");
		if (child > 0)
		{
			struct pollfd wait = { done[0], POLLIN, 0 };
			CHECK(poll(&wait, 1, 1500) == 1, "UI returns while the publisher still holds the lock");
		}
		write(replacement);
		resumeUnlock(publisher);
		int status = 0;
		if (child > 0)
			CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
			      "contending UI refuses to change the record or its counter");
		close(done[0]);
	};

	const std::string refreshedBytes = head + token2 + "\n" + spacedRom + "\n";
	write(typed);
	CHECK(resumeBegin(dir, native), "count A before publication of B");
	contend(refreshedBytes, [&] { return !resumeBegin(dir, native) && !resumeConfirm(dir, native); });
	CHECK(count() == 1, "lock contention did not spend or increment A's count");
	CHECK(resumeConfirm(dir, native) && resumeRead(dir, record) == Resume::Ready && record.token == token2 &&
	      access(tries.c_str(), F_OK) != 0, "stale settlement preserves B and removes only A's count");
	resumeConfirm(dir, record);

	write("malformed\n");
	contend(refreshedBytes, [&] { ResumeRecord badRecord; return resumePlan(dir, badRecord) == Resume::Unavailable; });
	CHECK(resumePlan(dir, record) == Resume::Ready && record.token == token2,
	      "malformed-record cleanup cannot erase a replacement under the publisher lock");
	const std::string lockPath = dir + "/resume.lock";
	CHECK(unlink(lockPath.c_str()) == 0 && symlink(marker.c_str(), lockPath.c_str()) == 0, "force lock open to fail");
	CHECK(!resumeConfirm(dir, record) && !resumeBegin(dir, record) && access(marker.c_str(), F_OK) == 0,
	      "a lock-open failure does not erase or count a valid record");
	CHECK(unlink(lockPath.c_str()) == 0 && resumeConfirm(dir, record), "clear B after restoring the lock");

	const std::string legacyA = rom + "\n" BANANA_LEGACY_RESUME_ID_PREFIX "0123456789abcdef0123456789abcdef\n";
	const std::string legacyB = rom + "\n" BANANA_LEGACY_RESUME_ID_PREFIX "fedcba9876543210fedcba9876543210\n";
	write(legacyA);
	ResumeRecord a;
	CHECK(resumePlan(dir, a) == Resume::Ready, "first same-ROM legacy publication is offered");
	for (int i = 0; i < resumeMaxTries; ++i)
		CHECK(resumeBegin(dir, a), "spend legacy A's attempt %d", i + 1);
	CHECK(!resumeBegin(dir, a) && count() == 3, "legacy A hits the cap");
	write(legacyB);
	// HEAD used integer extraction, then only the marker's first line.
	int oldCount = 0;
	std::ifstream oldCounter(tries);
	oldCounter >> oldCount;
	CHECK(oldCount < resumeMaxTries && readLine(marker) == rom, "downgrade can still offer legacy B");
	CHECK(resumePlan(dir, record) == Resume::Ready && resumeBegin(dir, record) && count() == 1,
	      "same-ROM B gets its own budget");
	CHECK(!resumeBegin(dir, a) && resumeConfirm(dir, a) && count() == 1 &&
	      resumeRead(dir, record) == Resume::Ready && record.token == "fedcba9876543210fedcba9876543210",
	      "a stale legacy plan can neither start nor settle a counted replacement");
	resumeConfirm(dir, record);

	write(typed);
	std::ofstream(tries) << "2\n" << native.rom << '\n' << native.system << '\n' << native.core << '\n' << native.token << '\n';
	CHECK(resumePlan(dir, record) == Resume::Ready && resumeBegin(dir, record) && count() == 3 &&
	      resumePlan(dir, record) == Resume::Blocked, "migration preserves an existing native keyed budget");

	// A counter that cannot be written refuses the attempt (a hang could otherwise retry without bound).
	write(typed);
	CHECK(mkdir((tries + ".tmp").c_str(), 0700) == 0, "block the counter's temporary file");
	CHECK(!resumeBegin(dir, native) && access(marker.c_str(), F_OK) == 0 && access(tries.c_str(), F_OK) != 0,
	      "an uncountable attempt must not start, and keeps the record");
	rmdir((tries + ".tmp").c_str());
	std::ofstream(tries + ".tmp") << "interrupted counter write";
	CHECK(resumeBegin(dir, native) && count() == 1 && access((tries + ".tmp").c_str(), F_OK) != 0,
	      "an interrupted temp file cannot masquerade as a count or block the next reservation");
	std::ofstream(tries, std::ios::trunc) << "banana-resume-tries-v1\n1\n";
	CHECK(!resumeBegin(dir, native) && access(marker.c_str(), F_OK) == 0,
	      "a damaged counter cannot replenish the retry budget: the attempt is refused");
	CHECK(resumePlan(dir, record) == Resume::Blocked && access(marker.c_str(), F_OK) != 0 &&
	      access(tries.c_str(), F_OK) != 0,
	      "a damaged counter is a spent budget: record and counter go together instead of failing every boot");
	write(typed);
	CHECK(resumePlan(dir, record) == Resume::Ready && resumeBegin(dir, record) && count() == 1,
	      "the next record is not held back by the repaired state");
	resumeConfirm(dir, record);

	// Ownership: a record belongs to the session that bananad published it for, never to a ROM path.
	const std::string sessionA(32, 'a'), sessionB(32, 'b'), stampPath = dir + "/resume-owner";
	const std::string refresh = head + token2 + "\n" + spacedRom + "\n";
	ResumeRecord refreshed = native;
	refreshed.token = token2;
	auto entry = [&](const std::string& session, const ResumeRecord& r)
	{
		return session + " N " + r.token + " " + r.system + " " + r.core + " " + r.rom;
	};
	auto stamp = [&](const std::string& previous, const std::string& candidate)
	{
		std::ofstream(stampPath, std::ios::trunc) << "banana-resume-owner-v1\n" << previous << "\n" << candidate << "\n";
	};
	auto job = [&](const std::string& session, const ResumeRecord& reserved, bool settle, bool clean)
	{
		ResumeJob j;
		j.session = session;
		j.reserved = reserved;
		j.settle = settle;
		j.clean = clean;
		return j;
	};
	const ResumeRecord none;
	CHECK(resumeOwnerOf(dir, refreshed).empty(), "no stamp: nobody owns the record");
	stamp("-", entry(sessionA, refreshed));
	CHECK(resumeOwnerOf(dir, refreshed) == sessionA && resumeOwnerOf(dir, native).empty(),
	      "the stamp binds exactly the record it names");
	stamp(entry(sessionA, native), entry(sessionB, refreshed));
	CHECK(resumeOwnerOf(dir, native) == sessionA && resumeOwnerOf(dir, refreshed) == sessionB,
	      "an interruption on either side of the rename leaves the visible record with its own entry");
	stamp("-", "- N " + token2 + " gba mgba " + spacedRom);
	CHECK(resumeOwnerOf(dir, refreshed).empty(), "a record published unowned is not owned");
	std::ofstream(stampPath, std::ios::trunc) << "banana-resume-owner-v1\n-\n" << entry(sessionA, refreshed) << "\nextra\n";
	CHECK(resumeOwnerOf(dir, refreshed).empty(), "a stamp with more entries than the format has is damaged, not trusted");
	// Both entries are validated: a damaged one never lets the other stand in, and one key never has two owners.
	stamp(entry(sessionA, refreshed), "garbage");
	CHECK(resumeOwnerOf(dir, refreshed).empty(), "a damaged candidate entry leaves a matching previous entry unowned");
	stamp("garbage", entry(sessionA, refreshed));
	CHECK(resumeOwnerOf(dir, refreshed).empty(), "a damaged previous entry leaves a matching candidate unowned");
	stamp(entry(sessionA, refreshed), "-");
	CHECK(resumeOwnerOf(dir, refreshed).empty(), "the candidate entry cannot be the empty placeholder");
	stamp(entry(sessionA, refreshed), entry(sessionA, native).replace(0, 32, std::string(31, 'a')));
	CHECK(resumeOwnerOf(dir, refreshed).empty(), "a short session id in the other entry damages the stamp");
	{
		ResumeRecord shortToken = native;
		shortToken.token = "xyz";
		stamp(entry(sessionA, shortToken), entry(sessionA, refreshed));
		CHECK(resumeOwnerOf(dir, refreshed).empty(), "a malformed key in the other entry damages the stamp");
	}
	stamp(entry(sessionA, native), entry(sessionB, native));
	CHECK(resumeOwnerOf(dir, native).empty(), "two different owners for one key name no owner");
	stamp(entry(sessionA, refreshed), entry(sessionA, refreshed));
	CHECK(resumeOwnerOf(dir, refreshed) == sessionA, "the same session publishing the same key again still owns it");
	std::ofstream(stampPath, std::ios::trunc) << "banana-resume-owner-v2\n-\n" << entry(sessionA, refreshed) << "\n";
	CHECK(resumeOwnerOf(dir, refreshed).empty(), "an unknown stamp version is not trusted");
	std::ofstream(stampPath, std::ios::trunc) << "banana-resume-owner-v1\n-\n" << std::string(32, 'A') << " N " << token2
		<< " gba mgba " << spacedRom << "\n";
	CHECK(resumeOwnerOf(dir, refreshed).empty(), "a damaged session in the stamp is not an owner");
	ResumeRecord legacyRecord;
	legacyRecord.rom = rom;
	CHECK(resumeOwnerOf(dir, legacyRecord).empty(), "an old record without a publication ID can never be owned");

	// A clean return may meet bananad's publication lock: do not wait, change nothing, and complete on a later pass.
	write(typed);
	CHECK(resumeBegin(dir, native), "count the session before its return");
	write(refresh);
	stamp("-", entry(sessionA, refreshed));
	contend(refresh, [&] { return !resumeRunJob(dir, dir, job(sessionA, native, true, true)); });
	CHECK(access(marker.c_str(), F_OK) == 0 && count() == 1,
	      "a contended clean return leaves the refreshed record and its count untouched");
	CHECK(resumeRunJob(dir, dir, job(sessionA, native, true, true)) && access(marker.c_str(), F_OK) != 0 &&
	      access(tries.c_str(), F_OK) != 0, "the clean return completes after the publisher releases the lock");

	// A checkpoint bananad refreshed while the session ran, and bound to that session, is cleared with its reservation.
	write(typed);
	CHECK(resumeBegin(dir, native), "reserve before the refreshed checkpoint");
	write(refresh);
	stamp(entry(sessionA, native), entry(sessionA, refreshed));
	CHECK(resumeRunJob(dir, dir, job(sessionA, native, true, true)) && access(marker.c_str(), F_OK) != 0 &&
	      access(tries.c_str(), F_OK) != 0, "a checkpoint this session published is cleared with its reservation");

	// Blocker 1: another session's same-ROM checkpoint -- same or another profile -- is not this session's.
	write(typed);
	CHECK(resumeBegin(dir, native), "reserve A's boot record");
	write(refresh);
	stamp(entry(sessionA, native), entry(sessionB, refreshed));
	CHECK(resumeRunJob(dir, dir, job(sessionA, native, true, true)), "A's return completes");
	CHECK(resumeRead(dir, record) == Resume::Ready && record.token == token2 && access(tries.c_str(), F_OK) != 0,
	      "B's checkpoint for the same ROM survives A's return, and only A's own reservation is spent");
	const std::string otherCoreRefresh = "banana-retro-v1\ngba:other-core\n" + token2 + "\n" + spacedRom + "\n";
	ResumeRecord otherCore = refreshed;
	otherCore.core = "other-core";
	write(otherCoreRefresh);
	stamp("-", entry(sessionB, otherCore));
	CHECK(resumeRunJob(dir, dir, job(sessionA, native, true, true)) && access(marker.c_str(), F_OK) == 0,
	      "the same ROM under another profile belongs to its own session too");
	// ... and nothing a session does not own can be removed on its behalf.
	write(refresh);
	CHECK(resumeRunJob(dir, dir, job(sessionA, none, false, true)) && access(marker.c_str(), F_OK) == 0,
	      "with no stamp the same-ROM record is unowned and stays");
	stamp("-", "- N " + token2 + " gba mgba " + spacedRom);
	CHECK(resumeRunJob(dir, dir, job(sessionA, none, false, true)) && access(marker.c_str(), F_OK) == 0,
	      "a record published unowned stays");
	std::ofstream(stampPath, std::ios::trunc) << "garbage\n";
	CHECK(resumeRunJob(dir, dir, job(sessionA, none, false, true)) && access(marker.c_str(), F_OK) == 0,
	      "a damaged stamp never lets a session remove a record");
	unlink(stampPath.c_str());

	// A record for another ROM is a replacement: the clean return keeps it and drops only its own reservation.
	const std::string otherRom = dir + "/Other Game.gba";
	std::ofstream(otherRom) << "ROM";
	const std::string replacement = otherRom + "\n" BANANA_LEGACY_RESUME_ID_PREFIX "00112233445566778899aabbccddeeff\n";
	write(typed);
	CHECK(resumeBegin(dir, native), "reserve the session before an unrelated replacement");
	write(replacement);
	CHECK(resumeRunJob(dir, dir, job(sessionA, native, true, true)) && access(marker.c_str(), F_OK) == 0,
	      "the clean return keeps a record for another ROM");
	CHECK(resumeRead(dir, record) == Resume::Ready && record.rom == otherRom, "the replacement is still the plan");
	CHECK(access(tries.c_str(), F_OK) != 0, "the session's own reservation is removed");
	resumeConfirm(dir, record);

	// Counters follow the record that is removed, not the removal: another record's debt survives, an
	// unattributable one does not.
	write(refresh);
	stamp("-", entry(sessionA, refreshed));
	CHECK(resumeWriteTries(dir, 2, native), "a counter naming a record that is no longer visible");
	CHECK(resumeRunJob(dir, dir, job(sessionA, none, false, true)) && access(marker.c_str(), F_OK) != 0 && count() == 2,
	      "removing a session's record keeps the debt of a different record");
	write(refresh);
	std::ofstream(tries, std::ios::trunc) << "banana-resume-tries-v1\n1\n";
	CHECK(resumeRunJob(dir, dir, job(sessionA, none, false, true)) && access(marker.c_str(), F_OK) != 0 &&
	      access(tries.c_str(), F_OK) != 0, "a damaged counter beside a removed record is cleared with it");
	unlink(stampPath.c_str());

	// Readiness comes from the launcher's session metadata, for this session only.
	const std::string sid(32, 'a'), otherSid(32, 'b'), session = dir + "/native-session";
	auto metadata = [&](const std::string& line) { std::ofstream(session, std::ios::trunc) << line << "\n"; };
	CHECK(!resumeNativeReady(session, sid), "no metadata is not ready");
	metadata("v1 " + sid + " 123 456 mgba 1700000000 starting " + spacedRom);
	CHECK(!resumeNativeReady(session, sid), "a starting session is not ready");
	metadata("v1 " + sid + " 123 456 mgba 1700000000 ready " + spacedRom);
	CHECK(resumeNativeReady(session, sid), "a ready session is ready");
	CHECK(!resumeNativeReady(session, otherSid), "another session's readiness is not ours");
	CHECK(!resumeNativeReady(session, "short"), "a malformed session ID never matches");
	metadata("v2 " + sid + " 123 456 mgba 1700000000 ready " + spacedRom);
	CHECK(!resumeNativeReady(session, sid), "an unknown metadata version is not ready");
	metadata("v1 " + sid + " 123 456 mgba 1700000000 ready");
	CHECK(!resumeNativeReady(session, sid), "truncated metadata is not ready");
	unlink(session.c_str());

	auto invalid = [&](const std::string& bytes, const char* what)
	{
		write(bytes);
		CHECK(resumePlan(dir, record) == Resume::Missing, "%s accepted as a boot record", what);
		CHECK(access(marker.c_str(), F_OK) != 0, "planner removed the invalid record under the lock");
	};
	invalid(typed + "extra\n", "fifth line");
	invalid(typed + "\n", "trailing empty line");
	invalid(typed.substr(0, typed.size() - 1), "unterminated final line");
	invalid(head + token.substr(1) + "\n" + rom + "\n", "short token");
	invalid(head + "A" + token.substr(1) + "\n" + rom + "\n", "uppercase token");
	invalid(head + token + "\nrelative.gba\n", "relative native ROM path");
	invalid(head + token + "\n" + rom + "\r\n", "CR in native ROM path");
	invalid(head + token + "\n" + rom + std::string("\0.gba\n", 6), "NUL in native ROM path");
	invalid(head + token + "\n/" + std::string(PATH_MAX + 300, 'a') + "\n", "oversized record");
	invalid("banana-retro-v2\ngba:mgba\n" + token + "\n" + rom + "\n", "unknown version");
	invalid("banana-retro-v1\nmgba\n" + token + "\n" + rom + "\n", "missing system");
	invalid("banana-retro-v1\ngba:m/gba\n" + token + "\n" + rom + "\n", "invalid core ID");
	invalid("banana-retro-v1\n" + std::string(64, 'a') + ":mgba\n" + token + "\n" + rom + "\n", "oversized system ID");
	write("banana-retro-v1\nsystem-id:core-id\n" + token + "\n" + rom + "\n");
	CHECK(resumePlan(dir, record) == Resume::Ready && record.system == "system-id" &&
	      record.core == "core-id", "profile IDs accept hyphens");
	resumeConfirm(dir, record);
	CHECK(mkfifo(marker.c_str(), 0600) == 0 && resumePlan(dir, record) == Resume::Missing, "FIFO rejected");
	CHECK(symlink(rom.c_str(), marker.c_str()) == 0 && resumePlan(dir, record) == Resume::Missing, "symlink rejected");

	SystemDef system;
	system.id = "gb";
	system.run = "/usr/libexec/banana/run-retroarch \tgambatte %ROM%";
	CHECK(resumeCommandCore(system, "gb") == "gambatte", "stock launcher yields the configured core");
	CHECK(resumeCommandCore(system, "gbc").empty(), "other system with the same core is rejected");
	system.run += " && custom-command";
	CHECK(resumeCommandCore(system, "gb").empty(), "stock prefix does not qualify an arbitrary command");
	system.run = "/usr/local/bin/custom-runtime gambatte %ROM%";
	CHECK(resumeCommandCore(system, "gb").empty(), "unrelated custom runtime is not run or replaced");
	system.run = "/usr/libexec/banana/run-retroarch --resolved-core %ROM%";
	CHECK(resumeCommandCore(system, "gb").empty(), "RetroArch-only dispatch flag cannot carry a native restore");
	unlink(otherRom.c_str());
	unlink(rom.c_str());
	unlink(spacedRom.c_str());
	unlink((dir + "/resume.lock").c_str());
	rmdir(dir.c_str());
}

static void cleanupScheduler()
{
	char scratch[] = "/tmp/banana-ui-cleanup-XXXXXX";
	if (!mkdtemp(scratch)) { CHECK(false, "create cleanup fixture directory"); return; }
	const std::string dir(scratch), marker = dir + "/resume", tries = dir + "/resume-tries";
	const std::string rom = dir + "/Orchard Tactics.gba";
	const std::string token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	const std::string typed = "banana-retro-v1\ngba:mgba\n" + token + "\n" + rom + "\n";
	std::ofstream(rom) << "ROM";
	auto write = [&](const std::string& bytes)
	{
		const std::string temp = marker + ".tmp";
		std::ofstream f(temp, std::ios::binary);
		f.write(bytes.data(), bytes.size());
		f.close();
		CHECK(f && rename(temp.c_str(), marker.c_str()) == 0, "publish a cleanup fixture record");
	};
	auto count = [&]
	{
		std::ifstream f(tries);
		std::string version;
		int n = -1;
		return std::getline(f, version) && f >> n ? n : -1;
	};

	CHECK(!resumeDeferred(dir, 0, -1), "no running game, menu launch or launch file is not deferred");
	CHECK(resumeDeferred(dir, 0, -1, true), "a cleanup still in flight defers the boot record");
	{
		std::ofstream launch(dir + "/launch");
		launch << rom << "\n";
	}
	CHECK(resumeDeferred(dir, 0, -1), "an existing launch file defers the boot record");
	CHECK(resumeDeferred(dir, 123, -1) && resumeDeferred(dir, 0, 0),
	      "a running game or a queued menu launch defers the boot record");
	CHECK(unlink((dir + "/launch").c_str()) == 0, "remove the launch file");

	const std::string sessionA(32, 'a'), sessionB(32, 'b');
	auto job = [&](const std::string& session, const ResumeRecord& reserved, bool settle, bool clean)
	{
		ResumeJob j;
		j.session = session;
		j.reserved = reserved;
		j.settle = settle;
		j.clean = clean;
		return j;
	};
	const ResumeRecord none;
	ResumeRecord record;
	write(typed);
	CHECK(resumePlan(dir, record) == Resume::Ready && resumeBegin(dir, record) && count() == 1,
	      "reserve the record the return will clean");
	ResumeCleanup cleanup;
	CHECK(cleanup.schedule(job(sessionA, none, false, false), 900) && !cleanup.pending(),
	      "a job with nothing to do is not scheduled");
	int publisher = resumeLock(dir);
	CHECK(publisher >= 0, "hold the daemon lock for the cleanup fixture");
	if (publisher < 0) { unlink(rom.c_str()); rmdir(dir.c_str()); return; }

	// The actual scheduler, not its helper: with the publisher holding the lock, one attempt fails, the next
	// becomes due 100 ms later, and the loop bound expires it without blocking or clearing the record.
	CHECK(cleanup.schedule(job(sessionA, record, true, true), 1000), "schedule the session's job");
	CHECK(cleanup.pending() && cleanup.ready(1000), "cleanup is due immediately after scheduling");
	CHECK(cleanup.step(1000, dir, dir) == ResumeCleanup::Step::Retried && cleanup.attempts == 1 &&
	      cleanup.pending() && cleanup.due == 1100, "a busy lock retries on the scheduler's interval");
	CHECK(cleanup.step(1099, dir, dir) == ResumeCleanup::Step::Idle && cleanup.attempts == 1,
	      "the scheduler waits for its due time");

	// A pending job is immutable to every other session: it is neither replaced nor extended.
	CHECK(!cleanup.schedule(job(sessionB, none, false, true), 1050) && cleanup.job.session == sessionA &&
	      cleanup.job.settle && cleanup.job.clean && resumeSameRecord(cleanup.job.reserved, record),
	      "another session's job is refused and the pending one is untouched");
	ResumeRecord other = record;
	other.token.assign(64, 'c');
	CHECK(!cleanup.schedule(job(sessionA, other, true, false), 1050) && resumeSameRecord(cleanup.job.reserved, record),
	      "the same session cannot swap the record it reserved");
	CHECK(cleanup.schedule(job(sessionA, none, false, true), 1050) && cleanup.attempts == 1 &&
	      cleanup.due == 1100 && cleanup.deadline == 3000, "the same session merges and keeps its window");

	int retries = 0;
	bool exhausted = false;
	for (double now = 1100; now <= 3000; now += 100)
	{
		const ResumeCleanup::Step step = cleanup.step(now, dir, dir);
		if (step == ResumeCleanup::Step::Retried) ++retries;
		else if (step == ResumeCleanup::Step::Exhausted) { exhausted = true; break; }
		else CHECK(false, "a held lock produced %d at %.0f ms", (int)step, now);
	}
	CHECK(exhausted && retries == ResumeCleanup::maxAttempts - 2 && !cleanup.pending() && cleanup.exhausted,
	      "the bounded window gives up after the configured attempts");
	CHECK(access(marker.c_str(), F_OK) == 0 && count() == 1,
	      "an exhausted cleanup leaves the record and its reservation for the next boot");

	// A later pass completes the same cleanup as soon as the publisher releases the lock.
	resumeUnlock(publisher);
	CHECK(cleanup.schedule(job(sessionA, record, true, true), 3100), "a later session job is accepted once idle");
	CHECK(cleanup.step(3100, dir, dir) == ResumeCleanup::Step::Done && !cleanup.pending() && !cleanup.exhausted,
	      "the scheduler completes when the lock is available");
	CHECK(access(marker.c_str(), F_OK) != 0 && access(tries.c_str(), F_OK) != 0,
	      "a completed cleanup removes the marker and its reservation");

	// The window itself expires an attempt even when the attempt count has not been reached.
	write(typed);
	CHECK(resumePlan(dir, record) == Resume::Ready && resumeBegin(dir, record), "reserve before a deadline probe");
	publisher = resumeLock(dir);
	CHECK(publisher >= 0, "hold the lock for the deadline probe");
	if (publisher >= 0)
	{
		cleanup.schedule(job(sessionA, record, true, false), 5000);
		CHECK(cleanup.step(7000, dir, dir) == ResumeCleanup::Step::Exhausted && cleanup.attempts == 1,
		      "the cleanup window expires independently of the attempt cap");
		resumeUnlock(publisher);
	}
	CHECK(access(marker.c_str(), F_OK) == 0, "an expired rejection cleanup leaves the record");
	cleanup.schedule(job(sessionA, record, true, false), 8000);
	CHECK(cleanup.step(8000, dir, dir) == ResumeCleanup::Step::Done && access(marker.c_str(), F_OK) != 0 &&
	      access(tries.c_str(), F_OK) != 0, "a retried rejection clears the record and its count");

	// Once a shutdown owns the record the scheduler stops without touching it.
	write(typed);
	CHECK(resumePlan(dir, record) == Resume::Ready && resumeBegin(dir, record), "reserve before a halt probe");
	cleanup.schedule(job(sessionA, record, true, true), 9000);
	CHECK(cleanup.step(9000, dir, dir, true) == ResumeCleanup::Step::Halted && !cleanup.pending() &&
	      access(marker.c_str(), F_OK) == 0 && count() == 1, "a halted cleanup settles nothing");
	CHECK(cleanup.step(9100, dir, dir, true) == ResumeCleanup::Step::Idle, "and an idle scheduler stays idle");
	cleanup.schedule(job(sessionA, record, true, true), 9200);
	cleanup.abandon();
	CHECK(!cleanup.pending() && cleanup.step(9300, dir, dir) == ResumeCleanup::Step::Idle &&
	      access(marker.c_str(), F_OK) == 0, "an abandoned job never runs");

	unlink((dir + "/resume.lock").c_str());
	unlink(marker.c_str());
	unlink(tries.c_str());
	unlink(rom.c_str());
	rmdir(dir.c_str());
}

// ---- the game registration bananad validates ------------------------------------------------------------------
static void sessionRegistration()
{
	char scratch[] = "/tmp/banana-ui-register-XXXXXX";
	if (!mkdtemp(scratch)) { CHECK(false, "create registration fixture directory"); return; }
	const std::string run(scratch), path = run + "/game.session", rom = "/data/roms/Orchard Tactics.gba";
	const std::string session(32, 'a');
	auto put = [&](const std::string& bytes)
	{
		unlink(path.c_str());
		std::ofstream(path, std::ios::binary) << bytes;
	};
	unsigned long long start = 0, again = 0;
	CHECK(resumeProcStart(getpid(), start) && start != 0 && resumeProcStart(getpid(), again) && again == start,
	      "a live process has a stable, non-zero start time");
	CHECK(!resumeProcStart(0x7ffffff0, start), "a process that does not exist has none");

	ResumeRegistration reg;
	CHECK(!resumeReadRegistration(run, reg) && reg.session.empty(), "no registration reads as none");
	CHECK(resumeRegister(run, session, getpid(), rom) && access((path + ".tmp").c_str(), F_OK) != 0,
	      "register the running game atomically");
	CHECK(resumeReadRegistration(run, reg) && reg.session == session && reg.pid == getpid() && reg.start == start &&
	      reg.rom == rom, "the registration names the session, process, start time and ROM, spaces and all");
	std::ifstream f(path);
	std::string line;
	std::getline(f, line);
	CHECK(line == "v1 " + session + " " + std::to_string((long)getpid()) + " " + std::to_string(start) + " " + rom,
	      "the registration's wire format is the one bananad parses");

	// A refused registration changes nothing already there.
	CHECK(!resumeRegister(run, std::string(31, 'a'), getpid(), rom) && !resumeRegister(run, std::string(32, 'A'), getpid(), rom) &&
	      !resumeRegister(run, session, getpid(), "relative.gba") && !resumeRegister(run, session, getpid(), "/a\nb.gba") &&
	      !resumeRegister(run, session, 0x7ffffff0, rom), "a malformed session, ROM or process is refused");
	CHECK(resumeReadRegistration(run, reg) && reg.session == session, "a refused registration leaves the old one");
	std::ofstream(path + ".tmp") << "stale";
	const std::string second(32, 'b');
	CHECK(resumeRegister(run, second, getpid(), rom) && resumeReadRegistration(run, reg) && reg.session == second &&
	      access((path + ".tmp").c_str(), F_OK) != 0, "a stale temporary file does not block a new registration");

	// Anything unparseable is no registration.
	const std::string s = session;
	for (const std::string& text : {
		std::string("v1 " + s + " 12 34 " + rom),						// no newline: a torn write
		std::string("v2 " + s + " 12 34 " + rom + "\n"),				// unknown version
		std::string("v1 " + s + " 12 34\n"),							// no ROM
		std::string("v1 " + s + " 0 34 " + rom + "\n"),					// pid 0
		std::string("v1 " + s + " 12 0 " + rom + "\n"),					// start 0
		std::string("v1 " + s + " 12x 34 " + rom + "\n"),				// not a number
		std::string("v1 " + std::string(32, 'A') + " 12 34 " + rom + "\n"),	// not lower-case hex
		std::string("v1 " + s.substr(1) + " 12 34 " + rom + "\n"),		// short session
		std::string("v1 " + s + " 12 34 relative.gba\n"),				// relative ROM
		std::string("v1 " + s + " 2147483648 34 " + rom + "\n"),		// pid out of range
		std::string("v1 " + s + " 12 34 " + rom + "\nextra\n"),			// more than one line
	})
	{
		put(text);
		CHECK(!resumeReadRegistration(run, reg) && reg.session.empty(), "an invalid registration is refused: %s", text.c_str());
	}
	unlink(path.c_str());
	CHECK(symlink("/etc/hostname", path.c_str()) == 0 && !resumeReadRegistration(run, reg),
	      "a symlink is not a registration");

	// Retiring is idempotent and removes exactly the registration.
	unlink(path.c_str());
	CHECK(resumeRegister(run, session, getpid(), rom), "register again");
	resumeRetire(run);
	CHECK(access(path.c_str(), F_OK) != 0 && !resumeReadRegistration(run, reg), "a retired registration is gone");
	resumeRetire(run);
	CHECK(access(path.c_str(), F_OK) != 0, "retiring nothing is harmless");

	// A child's registration validates against the child, and ends with it.
	pid_t child = fork();
	if (child == 0) { pause(); _exit(0); }
	CHECK(child > 0 && resumeRegister(run, session, child, rom) && resumeReadRegistration(run, reg) &&
	      reg.pid == child, "register another process");
	unsigned long long childStart = 0;
	CHECK(resumeProcStart(child, childStart) && childStart == reg.start, "its start time is the one registered");
	kill(child, SIGKILL);
	int status = 0;
	CHECK(waitpid(child, &status, 0) == child && !resumeProcStart(child, childStart),
	      "once reaped, the registered process has no start time to match");
	unlink(path.c_str());
	rmdir(run.c_str());
}

// ---- one launch at a time: the production flow, driven the way the main loop drives it ---------------------------
struct FlowBed
{
	std::string data, run, rom, marker, tries;
	FlowBed()
	{
		char d[] = "/tmp/banana-ui-flow-XXXXXX", r[] = "/tmp/banana-ui-flowrun-XXXXXX";
		if (!mkdtemp(d) || !mkdtemp(r)) { CHECK(false, "create flow fixture directories"); return; }
		data = d;
		run = r;
		rom = data + "/Orchard Tactics.gba";
		marker = data + "/resume";
		tries = data + "/resume-tries";
		std::ofstream(rom) << "ROM";
	}
	~FlowBed()
	{
		const std::string cmd = "rm -rf '" + data + "' '" + run + "'";
		if (system(cmd.c_str()) != 0) {}
	}
	ResumeRecord native(char digit) const
	{
		ResumeRecord r;
		r.rom = rom;
		r.system = "gba";
		r.core = "mgba";
		r.token.assign(64, digit);
		return r;
	}
	void publish(const ResumeRecord& r) const
	{
		const std::string bytes = r.core.empty()
			? r.rom + "\n" BANANA_LEGACY_RESUME_ID_PREFIX + r.token + "\n"
			: "banana-retro-v1\n" + r.system + ":" + r.core + "\n" + r.token + "\n" + r.rom + "\n";
		std::ofstream(marker + ".tmp", std::ios::binary) << bytes;
		CHECK(rename((marker + ".tmp").c_str(), marker.c_str()) == 0, "publish a flow fixture record");
	}
	// The record is the one bananad published for `session`.
	void own(const std::string& session, const ResumeRecord& r) const
	{
		std::ofstream(run + "/resume-owner", std::ios::trunc) << "banana-resume-owner-v1\n-\n" << session << " N " << r.token
			<< " " << r.system << " " << r.core << " " << r.rom << "\n";
	}
	int count() const
	{
		std::ifstream f(tries);
		std::string version;
		int n = -1;
		return std::getline(f, version) && f >> n ? n : -1;
	}
	bool marked() const { return access(marker.c_str(), F_OK) == 0; }
	bool exists(const std::string& name) const { return access((run + "/" + name).c_str(), F_OK) == 0; }
	// A writer's atomic publication of a launch request: a complete file renamed into place.
	void request(const std::string& text) const
	{
		std::ofstream(run + "/launch.tmp", std::ios::trunc) << text << "\n";
		CHECK(rename((run + "/launch.tmp").c_str(), (run + "/launch").c_str()) == 0, "publish a launch request");
	}
	void report(const std::string& session, const char* state) const
	{
		std::ofstream(run + "/native-session", std::ios::trunc) << "v1 " << session << " 123 456 mgba 1700000000 " << state
			<< " " << rom << "\n";
	}
	void latch() const { std::ofstream(run + "/shutting-down") << ""; }
	std::string token() const
	{
		ResumeRecord r;
		return resumeRead(data, r) == Resume::Ready ? r.token : std::string();
	}
	ResumeRecord legacy() const
	{
		ResumeRecord r;
		r.rom = rom;
		r.token.assign(32, '0');
		return r;
	}
};

// The host fixture's stand-in for main.cpp's library and runGame. Admission -- what is held, dropped and started, and
// which reservation a start takes -- is the production ResumeFlow, driven in the main loop's order; only the fork itself
// is replaced, by a session id.
struct Desk
{
	struct Game { std::string system, rom; };
	FlowBed& bed;
	ResumeFlow& flow;
	std::vector<Game> library;	// in their current order: a rescan re-sorts it
	std::vector<std::string> ran;	// "<system>:<rom>" of every start, in order
	std::string session;	// the running game's session; empty: none
	ResumeFlow::Admit file = ResumeFlow::Admit::Idle, menu = ResumeFlow::Admit::Idle;
	int forks = 0;
	bool failFork = false;	// the next start's fork fails
	bool animating = false;	// the menu's launch animation is running: a press made, not yet queued (ResumeFlow::pressing)
	std::function<void()> inTeardown;	// teardown, before ended(): performance commands, history, audio
	std::function<void()> afterEnded;	// teardown, after ended()
	Desk(FlowBed& b, ResumeFlow& f): bed(b), flow(f) {}
	bool running() const { return !session.empty(); }
	ResumeFlow::Starter starter()
	{
		return [this](const ResumeFlow::Target& target, const ResumeRecord* attempt)
		{
			if (target.store()) return ResumeFlow::Launched::Opened;	// main.cpp's startTarget: the menu opens its screen
			for (const Game& g : library)
			{
				if (g.rom != target.rom || (!target.system.empty() && g.system != target.system)) continue;
				if (failFork) return ResumeFlow::Launched::Failed;
				char id[40];
				snprintf(id, sizeof id, "%032x", ++forks);
				session = id;
				flow.started(session, g.rom, attempt);
				ran.push_back(g.system + ":" + g.rom);
				return ResumeFlow::Launched::Started;
			}
			return ResumeFlow::Launched::Missing;
		};
	}
	// A press, as Library::launch records it: by identity, never by position.
	void press(const Game& g)
	{
		ResumeFlow::Target target;
		target.system = g.system;
		target.rom = g.rom;
		target.shelf = 0;
		flow.queue(target);
	}
	// One iteration of the main loop, in its order: a child that exited is reaped first (finishGame), then the
	// launch sources are looked at (pass). `exited`: the game ended before this iteration began.
	void iteration(double now, bool exited)
	{
		if (exited && running()) reap(now);
		pass(now);
	}
	// One pass of the main loop over the launch sources, in its order: the cleanup, the launch file, the menu press.
	void pass(double now)
	{
		flow.pressing = animating;
		flow.step(now);
		file = flow.admitFile(running(), starter());
		menu = flow.admitMenu(running(), starter());
	}
	void reap(double now, int status = 0)
	{
		// The production order (ResumeFlow::reap): waitpid succeeded, the request is claimed, then teardown, which
		// ends with ended(). The hooks stand for the slow work in teardown, where a writer can publish.
		flow.reap(1, [&](int, int* out) { *out = status; return 1; }, [&](int s)
		{
			if (inTeardown) inTeardown();
			flow.ended(s, now);
			if (afterEnded) afterEnded();
			session.clear();
		});
	}
};

static void flowLifecycle()
{
	const std::string sessionA(32, 'a'), sessionB(32, 'b'), sessionC(32, 'c');
	const int retry = resumeExitRetry << 8, rejected = resumeExitRejected << 8;

	// An unrelated launch's return can neither spend nor erase another launch's attempts, whatever its ROM.
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		const ResumeRecord a = bed.native('a');
		bed.publish(a);
		CHECK(resumePlan(bed.data, flow.boot) == Resume::Ready && !flow.deferred(0, -1) && flow.reserve(flow.boot) &&
		      flow.reserving() && bed.count() == 1, "the boot record is reserved before its launch");
		flow.started(sessionA, bed.rom, &flow.boot);
		CHECK(!flow.reserving() && flow.launch.active && flow.launch.session == sessionA &&
		      flow.launch.phase == ResumeFlow::Phase::Running && resumeSameRecord(flow.launch.reserved, a),
		      "the launch that starts the reservation takes it");
		CHECK(resumeRegister(bed.run, sessionA, getpid(), bed.rom) && bed.exists("game.session"), "the game is registered");
		CHECK(flow.ended(retry, 1000) == ResumeFlow::End::Kept && !flow.cleanup.pending() && !bed.exists("game.session"),
		      "an attempt that ends before readiness is retired and schedules nothing");
		CHECK(bed.marked() && bed.token() == a.token && bed.count() == 1, "its record and attempt survive for the next boot");
		// An unrelated manual launch of the same ROM, with no record of its own, returns clean.
		CHECK(flow.request(false) == ResumeFlow::Request::Run, "the next launch is admitted");
		flow.started(sessionB, bed.rom, nullptr);
		CHECK(flow.launch.reserved.rom.empty(), "a launch nobody reserved for holds no reservation");
		CHECK(resumeRegister(bed.run, sessionB, getpid(), bed.rom), "the second game is registered");
		CHECK(flow.ended(0, 2000) == ResumeFlow::End::Cleaning && !bed.exists("game.session") &&
		      flow.step(2000) == ResumeCleanup::Step::Done, "its clean return runs its own job");
		CHECK(bed.marked() && bed.token() == a.token && bed.count() == 1,
		      "A's record and its counted attempt survive an unrelated launch's return");
	}

	// A launch that is not the reservation's lets it lapse; it is never inherited later.
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		const ResumeRecord a = bed.native('a');
		bed.publish(a);
		CHECK(resumePlan(bed.data, flow.boot) == Resume::Ready && flow.reserve(flow.boot), "reserve");
		flow.started(sessionB, bed.rom + ".other", nullptr);
		CHECK(!flow.reserving() && flow.launch.reserved.rom.empty() && bed.marked() && bed.count() == 1,
		      "a launch of another ROM does not take the reservation and leaves the attempt counted");
		flow.started(sessionC, bed.rom, &flow.boot);
		CHECK(flow.launch.reserved.rom.empty(), "and the reservation is gone for good");
		CHECK(flow.reserve(a) && bed.count() == 2, "a new reservation counts a new attempt");
		flow.started(sessionB, bed.rom, nullptr);
		CHECK(flow.launch.reserved.rom.empty() && bed.count() == 2,
		      "a manual launch of the same ROM is not the typed native launch");
		flow.release();
	}

	// Native readiness settles the exact reservation; a busy lock only postpones it to the clean return.
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		const ResumeRecord a = bed.native('a');
		bed.publish(a);
		CHECK(resumePlan(bed.data, flow.boot) == Resume::Ready && flow.reserve(flow.boot), "reserve");
		flow.started(sessionA, bed.rom, &flow.boot);
		CHECK(flow.pollReady() == ResumeFlow::Readiness::No, "no launcher metadata is not ready");
		bed.report(sessionB, "ready");
		CHECK(flow.pollReady() == ResumeFlow::Readiness::No, "another session's readiness is not ours");
		bed.report(sessionA, "starting");
		CHECK(flow.pollReady() == ResumeFlow::Readiness::No, "a starting launcher is not ready");
		bed.report(sessionA, "ready");
		const int lock = resumeLock(bed.data);
		CHECK(lock >= 0 && flow.pollReady() == ResumeFlow::Readiness::Busy && bed.marked() && bed.count() == 1 &&
		      flow.launch.phase == ResumeFlow::Phase::Ready, "readiness met the lock: remembered, nothing spent");
		resumeUnlock(lock);
		CHECK(flow.ended(0, 1000) == ResumeFlow::End::Cleaning && flow.cleanup.job.settle && flow.cleanup.job.clean,
		      "the return carries the unspent reservation as a job");
		CHECK(flow.step(1000) == ResumeCleanup::Step::Done && !bed.marked() && bed.count() == -1,
		      "the job spends the reservation exactly once");
	}
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		const ResumeRecord a = bed.native('a');
		bed.publish(a);
		CHECK(resumePlan(bed.data, flow.boot) == Resume::Ready && flow.reserve(flow.boot), "reserve");
		flow.started(sessionA, bed.rom, &flow.boot);
		bed.report(sessionA, "ready");
		CHECK(flow.pollReady() == ResumeFlow::Readiness::Settled && !bed.marked() && bed.count() == -1 &&
		      flow.launch.reserved.rom.empty() && flow.launch.phase == ResumeFlow::Phase::Idle,
		      "readiness settles the reservation under the lock");
		CHECK(flow.pollReady() == ResumeFlow::Readiness::No, "and only once");
	}
	// A rejection settles; an ordinary failure before readiness keeps the record for the next boot.
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		bed.publish(bed.native('a'));
		CHECK(resumePlan(bed.data, flow.boot) == Resume::Ready && flow.reserve(flow.boot), "reserve");
		flow.started(sessionA, bed.rom, &flow.boot);
		CHECK(flow.ended(rejected, 1000) == ResumeFlow::End::Rejected && flow.cleanup.job.settle &&
		      !flow.cleanup.job.clean, "a rejection schedules only the settlement");
		CHECK(flow.step(1000) == ResumeCleanup::Step::Done && !bed.marked() && bed.count() == -1,
		      "the rejected record and its count are cleared");
	}
	// A launcher that wrote the readiness file but died before the UI saw it still counts as ready.
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		bed.publish(bed.native('a'));
		CHECK(resumePlan(bed.data, flow.boot) == Resume::Ready && flow.reserve(flow.boot), "reserve");
		flow.started(sessionA, bed.rom, &flow.boot);
		std::ofstream(bed.run + "/native-ready-" + sessionA) << "";
		CHECK(flow.ended(retry, 1000) == ResumeFlow::End::Cleaning && !bed.exists("native-ready-" + sessionA) &&
		      flow.step(1000) == ResumeCleanup::Step::Done && !bed.marked(), "readiness seen only at reap still settles");
	}

	// Legacy RetroArch has no readiness signal: starting is confirming.
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		bed.publish([&] { ResumeRecord r; r.rom = bed.rom; r.token.assign(32, '0'); return r; }());
		CHECK(resumePlan(bed.data, flow.boot) == Resume::Ready && flow.boot.core.empty() && flow.reserve(flow.boot),
		      "a legacy boot record is reserved");
		flow.started(sessionA, bed.rom, &flow.boot);
		CHECK(!bed.marked() && bed.count() == -1 && flow.launch.reserved.rom.empty(),
		      "starting the legacy game confirms it at once");
	}
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		bed.publish([&] { ResumeRecord r; r.rom = bed.rom; r.token.assign(32, '0'); return r; }());
		CHECK(resumePlan(bed.data, flow.boot) == Resume::Ready && flow.reserve(flow.boot), "reserve");
		const int lock = resumeLock(bed.data);
		flow.started(sessionA, bed.rom, &flow.boot);
		resumeUnlock(lock);
		CHECK(bed.marked() && flow.launch.phase == ResumeFlow::Phase::Ready, "a busy lock postpones the confirmation");
		CHECK(flow.ended(0, 1000) == ResumeFlow::End::Cleaning && flow.step(1000) == ResumeCleanup::Step::Done &&
		      !bed.marked() && bed.count() == -1, "and the return completes it");
	}

	// While a cleanup is pending, a queued launch is held -- not read, not dropped -- and runs exactly once.
	for (const bool expire : {false, true})
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		const ResumeRecord a = bed.native('a');
		bed.publish(a);
		CHECK(resumePlan(bed.data, flow.boot) == Resume::Ready && flow.reserve(flow.boot), "reserve");
		flow.started(sessionA, bed.rom, &flow.boot);
		bed.report(sessionA, "ready");
		CHECK(flow.pollReady() == ResumeFlow::Readiness::Settled, "A is ready and settled");
		bed.publish(bed.native('b'));
		bed.own(sessionA, bed.native('b'));
		const int lock = resumeLock(bed.data);	// bananad is publishing
		CHECK(lock >= 0 && flow.ended(0, 1000) == ResumeFlow::End::Cleaning, "A returns while the publisher holds the lock");
		CHECK(flow.request(false) == ResumeFlow::Request::Hold && flow.deferred(0, -1) && flow.deferred(0, 0),
		      "a launch request is held while the cleanup is pending");
		Desk desk(bed, flow);
		desk.library = {{"gba", bed.rom}};
		std::ofstream(bed.run + "/launch") << bed.rom << "\n";
		desk.pass(1000);
		CHECK(desk.file == ResumeFlow::Admit::Held && bed.exists("launch") && desk.ran.empty(), "the launch file stays unread");
		bool released = false;
		int runs = 0;
		double ranAt = 0;
		bool exhaustedAtRun = false;
		for (double now = 1000; now <= 4000; now += 50)
		{
			if (!expire && !released && now >= 1300) { resumeUnlock(lock); released = true; }
			desk.pass(now);
			CHECK(desk.file == ResumeFlow::Admit::Held || desk.file == ResumeFlow::Admit::Started ||
			      desk.file == ResumeFlow::Admit::Idle, "a held request is never dropped");
			if (desk.file == ResumeFlow::Admit::Started)
			{
				++runs;
				ranAt = now;
				exhaustedAtRun = flow.cleanup.exhausted;
				desk.reap(now);
				flow.step(now);
			}
		}
		if (expire) resumeUnlock(lock);
		CHECK(runs == 1 && desk.ran.size() == 1 && !bed.exists("launch"), "the held launch ran exactly once");
		if (expire)
			CHECK(exhaustedAtRun && ranAt >= 1000 + ResumeCleanup::windowMs - 100 && ranAt <= 3100,
			      "when the lock never frees, the window expiry admits it (at %.0f ms)", ranAt);
		else
			CHECK(ranAt == 1300 || ranAt == 1350, "release admits it at once (at %.0f ms)", ranAt);
	}

	// A cleanup that decides after another session's checkpoint exists can remove only its own state.
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		const ResumeRecord a = bed.native('a'), b = bed.native('b');
		bed.publish(a);
		CHECK(resumePlan(bed.data, flow.boot) == Resume::Ready && flow.reserve(flow.boot), "reserve");
		flow.started(sessionA, bed.rom, &flow.boot);
		bed.report(sessionA, "ready");
		int lock = resumeLock(bed.data);
		CHECK(lock >= 0 && flow.pollReady() == ResumeFlow::Readiness::Busy, "readiness is postponed");
		CHECK(flow.ended(0, 1000) == ResumeFlow::End::Cleaning && flow.step(1000) == ResumeCleanup::Step::Retried,
		      "A's job waits for the lock");
		bed.publish(b);
		bed.own(sessionB, b);
		resumeUnlock(lock);
		lock = -1;
		CHECK(flow.step(1100) == ResumeCleanup::Step::Done, "A's job runs");
		CHECK(bed.marked() && bed.token() == b.token, "B's same-ROM checkpoint survives A's cleanup");
		CHECK(bed.count() == -1, "A's own counter is spent with its reservation");
	}
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		const ResumeRecord a = bed.native('a'), b = bed.native('b');
		bed.publish(a);
		CHECK(resumePlan(bed.data, flow.boot) == Resume::Ready && flow.reserve(flow.boot), "reserve");
		flow.started(sessionA, bed.rom, &flow.boot);
		bed.report(sessionA, "ready");
		int lock = resumeLock(bed.data);
		CHECK(lock >= 0 && flow.pollReady() == ResumeFlow::Readiness::Busy, "readiness is postponed");
		CHECK(flow.ended(0, 1000) == ResumeFlow::End::Cleaning && flow.step(1000) == ResumeCleanup::Step::Retried, "waits");
		bed.publish(b);
		bed.own(sessionA, b);	// the very session that is ending published it
		resumeUnlock(lock);
		lock = -1;
		CHECK(flow.step(1100) == ResumeCleanup::Step::Done && !bed.marked() && bed.count() == -1,
		      "a checkpoint the ending session published is cleared with its reservation");
	}

	// The shutdown latch: the daemon owns the record, so nothing is admitted, settled or retired here.
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		const ResumeRecord a = bed.native('a');
		bed.publish(a);
		CHECK(resumePlan(bed.data, flow.boot) == Resume::Ready && flow.reserve(flow.boot), "reserve");
		flow.started(sessionA, bed.rom, &flow.boot);
		CHECK(resumeRegister(bed.run, sessionA, getpid(), bed.rom), "register the game");
		bed.report(sessionA, "ready");
		CHECK(!flow.latched() && flow.request(false) == ResumeFlow::Request::Run, "no latch: launches are admitted");
		bed.latch();
		CHECK(flow.latched() && flow.request(false) == ResumeFlow::Request::Drop &&
		      flow.request(true) == ResumeFlow::Request::Drop, "under the latch no launch is admitted");
		CHECK(flow.pollReady() == ResumeFlow::Readiness::No && bed.marked() && bed.count() == 1,
		      "readiness is not settled under the latch");
		CHECK(!flow.settle(a) && bed.marked() && bed.count() == 1, "an explicit settle is refused too");
		CHECK(!flow.reserve(a) && bed.count() == 1, "and no new attempt is counted");
		CHECK(flow.ended(0, 1000) == ResumeFlow::End::Latched && !flow.cleanup.pending() && bed.exists("game.session") &&
		      bed.marked() && bed.token() == a.token && bed.count() == 1,
		      "the return leaves the registration, the record and the debt for the daemon");
	}
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		const ResumeRecord a = bed.native('a'), b = bed.native('b');
		bed.publish(a);
		CHECK(resumePlan(bed.data, flow.boot) == Resume::Ready && flow.reserve(flow.boot), "reserve");
		flow.started(sessionA, bed.rom, &flow.boot);
		bed.report(sessionA, "ready");
		CHECK(flow.pollReady() == ResumeFlow::Readiness::Settled, "settled");
		bed.publish(b);
		bed.own(sessionA, b);
		const int lock = resumeLock(bed.data);
		CHECK(lock >= 0 && flow.ended(0, 1000) == ResumeFlow::End::Cleaning &&
		      flow.step(1000) == ResumeCleanup::Step::Retried, "a cleanup is pending");
		bed.latch();
		resumeUnlock(lock);
		CHECK(flow.step(1100) == ResumeCleanup::Step::Halted && !flow.cleanup.pending() && bed.marked() &&
		      bed.token() == b.token, "a shutdown that starts mid-cleanup halts it without removing anything");
		CHECK(flow.request(false) == ResumeFlow::Request::Drop, "and nothing launches after it");
	}

	// The boot record is not handed to a launch while the previous cleanup or a launch request is outstanding.
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		CHECK(!flow.deferred(0, -1) && flow.deferred(7, -1) && flow.deferred(0, 3), "the flow defers to a game or request");
		std::ofstream(bed.run + "/launch") << bed.rom << "\n";
		CHECK(flow.deferred(0, -1), "and to a launch file");
	}
}

// ---- admission: what is started, held and dropped, through the production flow ---------------------------------------
static void launchAdmission()
{
	const std::string sessionA(32, 'a');
	using Admit = ResumeFlow::Admit;
	using Boot = ResumeFlow::Boot;
	using Launched = ResumeFlow::Launched;

	// A boot reservation belongs to the boot attempt and to nothing else.
	{	// A request waiting from outside wins: the legacy record is settled, not executed twice, and it starts once.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		desk.library = {{"gba", bed.rom}};
		bed.publish(bed.legacy());
		CHECK(flow.plan() == Resume::Ready && flow.boot.core.empty(), "a legacy record is planned");
		std::ofstream(bed.run + "/launch") << bed.rom << "\n";
		CHECK(flow.bootResume(0, desk.starter()) == Boot::Deferred && !bed.marked() && bed.count() == -1 &&
		      desk.ran.empty() && !flow.reserving(), "an explicit request wins: its record is settled and not started");
		CHECK(flow.admitFile(false, desk.starter()) == Admit::Started && desk.ran.size() == 1 &&
		      flow.launch.reserved.rom.empty(), "the request starts once, holding no reservation");
	}
	{	// The launch animation is a press that is not queued yet: no launch file and no boot record may take its slot.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		desk.library = {{"gba", bed.rom}};
		std::ofstream(bed.run + "/launch") << bed.rom << "\n";
		desk.animating = true;
		desk.pass(0);
		CHECK(desk.file == Admit::Held && !desk.running() && desk.ran.empty() && bed.exists("launch"),
		      "a launch file waits, unread, while a press is animating");
		desk.animating = false;
		desk.press({"gba", bed.rom});
		desk.pass(100);
		CHECK(desk.file == Admit::Held && desk.menu == Admit::Started && desk.ran.size() == 1 && !bed.exists("launch"),
		      "the press that finished animating starts, and the file that waited behind it is discarded");
	}
	{	// A queued press is not admitted while a newer press is animating: the newer one replaces it (latest intent).
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		const std::string other = bed.run + "/other.gba";
		desk.library = {{"gba", bed.rom}, {"gba", other}};
		desk.press({"gba", bed.rom});
		desk.animating = true;
		desk.pass(0);
		CHECK(desk.menu == Admit::Held && desk.ran.empty() && flow.queuedLaunch(),
		      "an older queued press waits, still queued, while a newer press animates");
		desk.pass(50);
		CHECK(desk.menu == Admit::Held && desk.ran.empty(), "and keeps waiting for as long as the animation runs");
		desk.animating = false;
		desk.press({"gba", other});	// the animation ends: the newer press replaces the queued one
		desk.pass(100);
		CHECK(desk.menu == Admit::Started && desk.ran.size() == 1 && desk.ran[0] == "gba:" + other,
		      "only the newer press starts, exactly once");
		desk.pass(150);
		CHECK(desk.ran.size() == 1 && !flow.queuedLaunch(), "the older press is gone, not started later");
	}
	{	// The loop's poll() has no timer for a press the animation just finished or one only the animation held back.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		const ResumeFlow::Target target{"gba", bed.rom, 0};
		CHECK(!flow.admissionDue(false, false), "nothing is waiting: poll() may sleep");
		CHECK(flow.admissionDue(true, false), "a press the library holds, not yet queued, must wake poll()");
		flow.queue(target);
		CHECK(!flow.admissionDue(false, true), "a queued press under a newer animation waits for frames, not a spin");
		CHECK(flow.admissionDue(false, false), "the newer animation ended with nothing pending: the queued press wakes poll()");
		ResumeJob pending;
		pending.session = std::string(32, 'a');
		pending.clean = true;
		CHECK(flow.cleanup.schedule(pending, 0), "a session's cleanup is scheduled");
		CHECK(flow.cleanup.pending() && !flow.admissionDue(false, false),
		      "a press waiting on a cleanup is woken by the cleanup's timer, not by a spin");
	}
	// A removal whose directory sync failed leaves a debt that no later success may hide.
	{	// The marker and counter are gone on the retry: the sync is still retried, and a replacement is untouched.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		const ResumeRecord a = bed.native('a'), b = bed.native('b');
		bed.publish(a);
		failSyncs = 1;
		CHECK(!flow.settle(a) && !bed.marked() && bed.count() == -1 && flow.syncOwed(),
		      "the marker is unlinked but its sync failed: not settled, and the debt is kept");
		bed.publish(b);	// the daemon publishes a replacement before the retry
		const int before = syncCalls;
		failSyncs = 1;
		CHECK(!flow.settle(a) && flow.syncOwed() && syncCalls > before && bed.marked() && bed.token() == b.token,
		      "a retry with nothing left to unlink still retries the sync, and a failing one stays owed");
		failSyncs = 0;
		CHECK(flow.settle(a) && !flow.syncOwed() && bed.marked() && bed.token() == b.token,
		      "the sync finally succeeds: settled, the debt is cleared, the replacement record is untouched");
	}
	{	// The boot record's owed settlement reports the sync debt, bounded, and is never reported as settled early.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		const ResumeRecord a = bed.native('a');
		bed.publish(a);
		failSyncs = 1;
		CHECK(!flow.settleBoot(a) && flow.owing() && flow.syncOwed() && !bed.marked(),
		      "the unlink succeeded and the sync failed: owed, not settled");
		failSyncs = 1;
		CHECK(flow.stepOwed(0) == ResumeCleanup::Step::Retried && flow.owing() && flow.syncOwed(),
		      "the retry's sync fails too: still owed");
		CHECK(flow.stepOwed(100) == ResumeCleanup::Step::Done && !flow.owing() && !flow.syncOwed(),
		      "settled only when the sync has succeeded");
	}
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		const ResumeRecord a = bed.native('a');
		bed.publish(a);
		failSyncs = 1000;
		CHECK(!flow.settleBoot(a) && flow.owing(), "owed");
		ResumeCleanup::Step last = ResumeCleanup::Step::Idle;
		for (double now = 0; now <= 4000 && flow.owing(); now += 100) last = flow.stepOwed(now);
		CHECK(last == ResumeCleanup::Step::Exhausted && !flow.owing() && flow.syncOwed(),
		      "when the sync never succeeds the window ends in Exhausted, and the debt is still reported");
		failSyncs = 0;
		CHECK(flow.settle(a) && !flow.syncOwed(), "a later settlement pays it");
	}
	{	// A return's cleanup job: the record it removed is not "done" until its removal is synced.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		const ResumeRecord a = bed.native('a');
		bed.publish(a);
		bed.own(sessionA, a);
		flow.started(sessionA, bed.rom, nullptr);
		failSyncs = 1;
		CHECK(flow.ended(0, 1000) == ResumeFlow::End::Cleaning, "the return schedules the clean-up");
		CHECK(flow.step(1000) == ResumeCleanup::Step::Retried && !bed.marked() && flow.syncOwed(),
		      "the unlink happened but not its sync: retried, not done");
		CHECK(flow.step(1100) == ResumeCleanup::Step::Done && !flow.syncOwed(),
		      "done once the retry has made the removal durable, with nothing left to unlink");
	}
	{	// The boot record yields to the animating press the same way, and its settlement is not lost.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		desk.library = {{"gba", bed.rom}};
		bed.publish(bed.legacy());
		CHECK(flow.plan() == Resume::Ready, "planned");
		flow.pressing = true;
		CHECK(flow.bootResume(0, desk.starter()) == Boot::Deferred && desk.ran.empty() && !bed.marked() &&
		      bed.count() == -1 && !flow.owing(), "the boot game does not start while a press is animating; its record is settled");
	}
	// A boot record whose settlement met the lock is owed, retried within a bound, and never reported as settled.
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		desk.library = {{"gba", bed.rom}};
		bed.publish(bed.legacy());
		CHECK(flow.plan() == Resume::Ready, "planned");
		std::ofstream(bed.run + "/launch") << bed.rom << "\n";
		const int lock = resumeLock(bed.data);
		CHECK(lock >= 0 && flow.bootResume(0, desk.starter()) == Boot::DeferredUnsettled && bed.marked() && flow.owing(),
		      "the lock is busy: the record stays, and the result says it was not settled");
		CHECK(flow.stepOwed(0) == ResumeCleanup::Step::Retried && flow.stepOwed(50) == ResumeCleanup::Step::Idle &&
		      bed.marked() && flow.owing(), "it is retried on the timer, not at once");
		resumeUnlock(lock);
		CHECK(flow.stepOwed(100) == ResumeCleanup::Step::Done && !bed.marked() && !flow.owing(),
		      "and settled once the lock is released");
	}
	{	// The lock is never released: the retry is bounded, reports exhaustion and leaves the record for the next boot.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		bed.publish(bed.legacy());
		CHECK(flow.plan() == Resume::Ready, "planned");
		std::ofstream(bed.run + "/launch") << bed.rom << "\n";
		const int lock = resumeLock(bed.data);
		CHECK(lock >= 0 && flow.bootResume(0, desk.starter()) == Boot::DeferredUnsettled, "owed");
		ResumeCleanup::Step last = ResumeCleanup::Step::Idle;
		double t = 0;
		for (; t <= 3000 && last != ResumeCleanup::Step::Exhausted; t += 100) last = flow.stepOwed(t);
		CHECK(last == ResumeCleanup::Step::Exhausted && t <= 2100 && bed.marked() && !flow.owing() &&
		      flow.stepOwed(t) == ResumeCleanup::Step::Idle, "the retry is bounded and the record stays");
		resumeUnlock(lock);
	}
	{	// A shutdown that starts while the settlement is owed takes the record over: nothing is spent.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		bed.publish(bed.legacy());
		CHECK(flow.plan() == Resume::Ready, "planned");
		std::ofstream(bed.run + "/launch") << bed.rom << "\n";
		const int lock = resumeLock(bed.data);
		CHECK(lock >= 0 && flow.bootResume(0, desk.starter()) == Boot::DeferredUnsettled, "owed");
		bed.latch();
		resumeUnlock(lock);
		CHECK(flow.stepOwed(0) == ResumeCleanup::Step::Halted && bed.marked() && !flow.owing(),
		      "the latch halts it and leaves the record to the daemon");
	}
	{	// The lock arrives between the reservation and the settlement of a game that left the library.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		bed.publish(bed.legacy());
		CHECK(flow.plan() == Resume::Ready, "planned");
		int lock = -1;
		const ResumeFlow::Starter lockThenMissing = [&](const ResumeFlow::Target&, const ResumeRecord*)
		{
			lock = resumeLock(bed.data);
			return Launched::Missing;
		};
		CHECK(flow.bootResume(0, lockThenMissing) == Boot::MissingUnsettled && lock >= 0 && bed.marked() && flow.owing(),
		      "a missing game whose record could not be cleared is reported as unsettled");
		resumeUnlock(lock);
		CHECK(flow.stepOwed(0) == ResumeCleanup::Step::Done && !bed.marked() && bed.count() == -1, "and is cleared on retry");
	}
	{	// The cancel path and the incompatible-command path go through the same owed settlement.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		bed.publish(bed.legacy());
		CHECK(flow.plan() == Resume::Ready, "planned");
		const int lock = resumeLock(bed.data);
		CHECK(lock >= 0 && !flow.settleBoot(flow.boot) && bed.marked() && flow.owing(), "a refused settlement is owed");
		resumeUnlock(lock);
		CHECK(flow.stepOwed(0) == ResumeCleanup::Step::Done && !bed.marked(), "and completes");
	}
	{	// An external launch of the same ROM that starts while a reservation is outstanding does not take it.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		desk.library = {{"gba", bed.rom}};
		bed.publish(bed.legacy());
		CHECK(flow.plan() == Resume::Ready && flow.reserve(flow.boot) && bed.count() == 1, "the boot record is reserved");
		std::ofstream(bed.run + "/launch") << bed.rom << "\n";
		desk.pass(1000);
		CHECK(desk.file == Admit::Started && !flow.reserving() && flow.launch.reserved.rom.empty() && bed.marked() &&
		      bed.count() == 1, "the external launch starts without the reservation; the record and its count stay");
		desk.reap(2000);
		CHECK(flow.step(2000) == ResumeCleanup::Step::Done && bed.marked() && bed.count() == 1,
		      "and its return cannot spend a record it never started");
	}
	{	// The boot attempt starts through its own call: no launch file, and the start settles the legacy record.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		desk.library = {{"gba", bed.rom}};
		bed.publish(bed.legacy());
		CHECK(flow.plan() == Resume::Ready, "planned");
		CHECK(flow.bootResume(0, desk.starter()) == Boot::Started && desk.ran.size() == 1 && !bed.exists("launch") &&
		      !bed.marked() && bed.count() == -1 && flow.launch.reserved.rom.empty(),
		      "the boot attempt starts the game itself and settles its record");
	}
	{	// A typed record's attempt attaches to its own launch and waits for readiness.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		desk.library = {{"gba", bed.rom}};
		const ResumeRecord a = bed.native('a');
		bed.publish(a);
		CHECK(flow.plan() == Resume::Ready, "planned");
		CHECK(flow.bootResume(0, desk.starter()) == Boot::Started && flow.launch.phase == ResumeFlow::Phase::Running &&
		      resumeSameRecord(flow.launch.reserved, a) && bed.marked() && bed.count() == 1,
		      "the typed attempt is its launch's reservation until the runtime is ready");
	}
	{	// A record whose game left the library is settled, and a launch that cannot fork leaves it counted.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		bed.publish(bed.legacy());
		CHECK(flow.plan() == Resume::Ready, "planned");
		CHECK(flow.bootResume(0, desk.starter()) == Boot::Missing && !bed.marked() && bed.count() == -1 &&
		      !flow.reserving(), "a game that is not in the library settles its record");
		bed.publish(bed.legacy());
		CHECK(flow.plan() == Resume::Ready, "planned again");
		const ResumeFlow::Starter cannotFork = [](const ResumeFlow::Target&, const ResumeRecord*) { return Launched::Failed; };
		CHECK(flow.bootResume(0, cannotFork) == Boot::Failed && bed.marked() && bed.count() == 1 && !flow.reserving(),
		      "no child: the record stays, counted, for the next boot");
		bed.latch();
		CHECK(flow.bootResume(0, desk.starter()) == Boot::Latched && bed.marked() && bed.count() == 1,
		      "under the latch the boot resume touches nothing");
	}

	// A held request is a game, not a position, and two held sources never share a slot.
	const auto returnsWhileLocked = [&](FlowBed& bed, Desk& desk, const Desk::Game& first)
	{
		desk.press(first);
		desk.pass(0);
		CHECK(desk.menu == Admit::Started && desk.running(), "the first game starts");
		const int lock = resumeLock(bed.data);	// bananad is publishing when it returns
		desk.reap(1000);
		CHECK(lock >= 0 && desk.flow.cleanup.pending(), "its cleanup waits for the publisher");
		return lock;
	};
	{	// Menu press B and launch file C wait together while the shelves are re-sorted; B runs, by identity.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		const std::string shared = bed.data + "/Shared.gb", other = bed.data + "/Other.gba";
		desk.library = {{"gba", bed.rom}, {"gba", shared}, {"gbc", shared}, {"gba", other}};
		const int lock = returnsWhileLocked(bed, desk, desk.library[0]);
		desk.press({"gbc", shared});	// B: the gbc entry of a path two systems share
		std::ofstream(bed.run + "/launch") << other << "\n";	// C
		desk.pass(1000);
		CHECK(desk.file == Admit::Held && desk.menu == Admit::Held && desk.ran.size() == 1 && bed.exists("launch"),
		      "both are held, neither read nor started");
		std::reverse(desk.library.begin(), desk.library.end());	// a rescan re-sorts and grows the shelf
		desk.library.insert(desk.library.begin(), {"gba", bed.data + "/New.gba"});
		resumeUnlock(lock);
		desk.pass(1100);
		CHECK(desk.ran.size() == 2 && desk.ran[1] == "gbc:" + shared, "B starts, by system and path: %s",
		      desk.ran.back().c_str());
		CHECK(desk.menu == Admit::Started && desk.file == Admit::Held && !bed.exists("launch"),
		      "C waited, unread, behind B, and B's admission dropped it");
		desk.pass(1200);
		CHECK(desk.file == Admit::Idle && desk.ran.size() == 2, "C never starts over B");
	}
	{	// B is a short-lived menu launch: the game exits before the loop's next pass. C must not become a second launch.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		const std::string other = bed.data + "/Other.gba";
		desk.library = {{"gba", bed.rom}, {"gba", other}};
		desk.press(desk.library[0]);
		std::ofstream(bed.run + "/launch") << other << "\n";
		desk.pass(0);
		CHECK(desk.file == Admit::Held && desk.menu == Admit::Started && desk.ran.size() == 1, "B starts with C waiting behind it");
		desk.reap(10);	// the child forked, ran and was reaped at the top of the next pass, before file admission
		for (double now : {20.0, 100.0, 2000.0, 5000.0}) desk.pass(now);
		CHECK(desk.ran.size() == 1 && !bed.exists("launch"), "B's quick return leaves C dropped: exactly one launch (%zu)",
		      desk.ran.size());
		std::ofstream(bed.run + "/launch") << other << "\n";	// a request made after the game ended is a new request
		desk.pass(6000);
		CHECK(desk.file == Admit::Started && desk.ran.size() == 2 && desk.ran[1] == "gba:" + other, "a later file still starts");
	}
	{	// B's fork fails: nothing was admitted, so C is still waiting and runs once.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		const std::string other = bed.data + "/Other.gba";
		desk.library = {{"gba", bed.rom}, {"gba", other}};
		desk.press(desk.library[0]);
		std::ofstream(bed.run + "/launch") << other << "\n";
		desk.failFork = true;
		desk.pass(0);
		CHECK(desk.menu == Admit::Failed && desk.file == Admit::Held && bed.exists("launch") && desk.ran.empty(),
		      "a failed fork leaves the file waiting");
		desk.failFork = false;
		desk.pass(10);
		CHECK(desk.file == Admit::Started && desk.ran.size() == 1 && desk.ran[0] == "gba:" + other, "C then runs once");
	}
	// The launch file's lifetime rules, with the production event order: the loop reaps first, then looks at the
	// launch file and the press (Desk::iteration). A request is "during" a game from its admission until its reap.
	const auto fileText = [](const FlowBed& bed)
	{
		std::ifstream f(bed.run + "/launch");
		std::string line;
		std::getline(f, line);
		return line;
	};
	{	// Published during the game, which exits before the next iteration: the reap discards it, nothing starts.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		const std::string other = bed.data + "/Other.gba";
		desk.library = {{"gba", bed.rom}, {"gba", other}};
		desk.press(desk.library[0]);
		desk.iteration(0, false);
		CHECK(desk.menu == Admit::Started && desk.running() && desk.ran.size() == 1, "B starts with nothing waiting");
		bed.request(other);	// C is published while B runs
		desk.iteration(10, true);	// B exits before the loop looks again: reaped first, then the file is looked at
		CHECK(desk.file == Admit::Idle && desk.ran.size() == 1 && !bed.exists("launch") && !bed.exists("launch.claim"),
		      "C, made during B, never becomes a second game (%zu launches)", desk.ran.size());
		for (double now : {20.0, 100.0, 2000.0, 5000.0}) desk.iteration(now, false);
		CHECK(desk.ran.size() == 1, "and it is not run later either");
		bed.request(other);	// published after the reap: a new request
		desk.iteration(6000, false);
		CHECK(desk.file == Admit::Started && desk.ran.size() == 2 && desk.ran[1] == "gba:" + other,
		      "a request after the game ended starts once");
	}
	{	// The same for a game the launch file itself started, and for a request made while it still runs.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		const std::string other = bed.data + "/Other.gba", third = bed.data + "/Third.gba";
		desk.library = {{"gba", bed.rom}, {"gba", other}, {"gba", third}};
		bed.request(bed.rom);
		desk.iteration(0, false);
		CHECK(desk.file == Admit::Started && desk.running() && !bed.exists("launch"), "the file starts its game");
		bed.request(other);
		desk.iteration(10, false);
		CHECK(desk.file == Admit::Dropped && desk.ran.size() == 1 && !bed.exists("launch"), "a request made while it runs is dropped");
		bed.request(third);
		desk.iteration(20, true);
		CHECK(desk.ran.size() == 1 && !bed.exists("launch"), "one made just before a rapid exit is dropped too");
	}
	{	// What the reap discards is exactly what it saw: a request published right after it is a new request.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		const std::string other = bed.data + "/Other.gba", later = bed.data + "/Later.gba";
		desk.library = {{"gba", bed.rom}, {"gba", other}, {"gba", later}};
		desk.press(desk.library[0]);
		desk.iteration(0, false);
		bed.request(other);
		static const FlowBed* windowBed;
		static std::string windowText;
		windowBed = &bed;
		windowText = later;
		inRequestWindow = [] { windowBed->request(windowText); };
		desk.iteration(10, true);
		CHECK(!inRequestWindow, "the reap took the request that was there");
		inRequestWindow = nullptr;
		CHECK(desk.file == Admit::Started && desk.ran.size() == 2 && desk.ran[1] == "gba:" + later,
		      "the request published after the reap runs, and the one before it does not (%zu launches)", desk.ran.size());
	}
	{	// The reap's claim is the cutoff and comes first: before teardown, so a request published during teardown --
	// before or after the session's job is scheduled -- is a new request, while one made during the game is not.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		const std::string old = bed.data + "/Old.gba", mid = bed.data + "/Mid.gba", late = bed.data + "/Late.gba";
		desk.library = {{"gba", bed.rom}, {"gba", old}, {"gba", mid}, {"gba", late}};
		desk.press(desk.library[0]);
		desk.iteration(0, false);
		bed.request(old);	// made while the game ran: before the claim
		int teardowns = 0;
		desk.inTeardown = [&]
		{
			teardowns++;
			CHECK(!bed.exists("launch") && !bed.exists("launch.claim"), "the claim was taken before teardown began");
			bed.request(mid);	// published during teardown, before ended()
		};
		desk.iteration(10, true);
		CHECK(teardowns == 1 && desk.file == Admit::Started && desk.ran.size() == 2 && desk.ran[1] == "gba:" + mid,
		      "the request from teardown runs once, and the one from the game does not (%zu launches)", desk.ran.size());
		desk.inTeardown = nullptr;
		desk.afterEnded = [&] { bed.request(late); };	// published during teardown, after ended()
		desk.iteration(20, true);
		CHECK(desk.ran.size() == 3 && desk.ran[2] == "gba:" + late, "a request after the job was scheduled is kept too");
	}
	{	// The same cutoff when a writer publishes between the claim and the teardown: that is a new request as well.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		const std::string old = bed.data + "/Old.gba", fresh = bed.data + "/Fresh.gba";
		desk.library = {{"gba", bed.rom}, {"gba", old}, {"gba", fresh}};
		desk.press(desk.library[0]);
		desk.iteration(0, false);
		bed.request(old);
		static const FlowBed* windowBed;
		static std::string windowText;
		windowBed = &bed;
		windowText = fresh;
		inRequestWindow = [] { windowBed->request(windowText); };
		bool tornDown = false;
		desk.inTeardown = [&] { tornDown = true; CHECK(!inRequestWindow && bed.exists("launch"), "the claim is already made, the newer request waits"); };
		desk.iteration(10, true);
		inRequestWindow = nullptr;
		CHECK(tornDown && desk.ran.size() == 2 && desk.ran[1] == "gba:" + fresh, "only the request after the claim runs");
	}
	{	// reap() acts only on a reaped child: a running one (WNOHANG 0) or an interrupted wait leaves the request and the
	// registration alone; a child that is already gone (ECHILD) ends the lifetime and reports the signal status.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		const std::string other = bed.data + "/Other.gba";
		desk.library = {{"gba", bed.rom}, {"gba", other}};
		desk.press(desk.library[0]);
		desk.iteration(0, false);
		bed.request(other);
		int torn = 0, seen = -1;
		const auto teardown = [&](int status) { torn++; seen = status; };
		CHECK(!flow.reap(1, [](int, int*) { errno = 0; return 0; }, teardown) && torn == 0 && bed.exists("launch") &&
		      flow.launch.active, "a child that still runs claims nothing");
		CHECK(!flow.reap(1, [](int, int*) { errno = EINTR; return -1; }, teardown) && torn == 0 && bed.exists("launch"),
		      "an interrupted wait claims nothing");
		CHECK(flow.reap(1, [](int, int*) { errno = ECHILD; return -1; }, teardown) && torn == 1 && seen == SIGCHLD &&
		      !bed.exists("launch"), "a child already gone ends the lifetime as a signal");
		CHECK(flow.reap(1, [](int pid, int* status) { *status = 9; return pid; }, teardown) && seen == 9,
		      "a reaped child hands its own status to the teardown");
	}
	{	// A claim left by a launcher that died between taking a request and removing it is never run, and does not
	// survive the next claim: rename replaces it, and the reap discards the request that took its place.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		const std::string stale = bed.data + "/Stale.gba", other = bed.data + "/Other.gba", next = bed.data + "/Next.gba";
		desk.library = {{"gba", bed.rom}, {"gba", stale}, {"gba", other}, {"gba", next}};
		desk.press(desk.library[0]);
		desk.iteration(0, false);
		std::ofstream(bed.run + "/launch.claim") << stale << "\n";
		bed.request(other);
		desk.iteration(10, true);
		CHECK(desk.ran.size() == 1 && !bed.exists("launch") && !bed.exists("launch.claim"),
		      "the reap replaces the stale claim, discards the live request, and leaves nothing behind");
		std::ofstream(bed.run + "/launch.claim") << stale << "\n";
		for (double now : {20.0, 100.0, 2000.0}) desk.iteration(now, false);
		CHECK(desk.ran.size() == 1, "a stale claim is never taken for a request");
		bed.request(next);
		desk.iteration(3000, false);
		CHECK(desk.file == Admit::Started && desk.ran.size() == 2 && desk.ran[1] == "gba:" + next && !bed.exists("launch.claim"),
		      "the next request runs once and clears the stale claim");
	}
	{	// A replacement published while a request is being taken is never read, moved or deleted by that call.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		const std::string gone = bed.data + "/Gone.gba", other = bed.data + "/Other.gba";
		desk.library = {{"gba", bed.rom}, {"gba", other}};
		static const FlowBed* windowBed;
		static std::string windowText;
		windowBed = &bed;
		windowText = other;
		bed.request(gone);	// no such game: the call that takes it starts nothing
		inRequestWindow = [] { windowBed->request(windowText); };
		desk.iteration(0, false);
		CHECK(!inRequestWindow && desk.file == Admit::Missing && desk.ran.empty(), "the old request was the one taken, and it names no game");
		inRequestWindow = nullptr;
		CHECK(bed.exists("launch") && fileText(bed) == other && !bed.exists("launch.claim"),
		      "the replacement is still waiting, unread (%s)", fileText(bed).c_str());
		desk.iteration(10, false);
		CHECK(desk.file == Admit::Started && desk.ran.size() == 1 && desk.ran[0] == "gba:" + other, "and it runs once");
		// The same window while a press discards the request that waited behind it.
		FlowBed bed2;
		ResumeFlow flow2(bed2.data, bed2.run);
		Desk desk2(bed2, flow2);
		desk2.library = {{"gba", bed2.rom}, {"gba", other}};
		windowBed = &bed2;
		desk2.press(desk2.library[0]);
		bed2.request(gone);
		inRequestWindow = [] { windowBed->request(windowText); };
		desk2.iteration(0, false);
		CHECK(!inRequestWindow && desk2.menu == Admit::Started && bed2.exists("launch") && fileText(bed2) == other,
		      "a press discards the request it saw, not the one published after it");
		inRequestWindow = nullptr;
		desk2.iteration(10, true);
		CHECK(desk2.ran.size() == 1 && !bed2.exists("launch"), "and the game's reap then discards that one: one launch");
	}
	{	// The Store screen starts no game: it admits nothing, so the waiting request is neither dropped nor lost.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		const std::string other = bed.data + "/Other.gba";
		desk.library = {{"gba", bed.rom}, {"gba", other}};
		bed.request(other);
		desk.press({"", "banana:store"});
		desk.iteration(0, false);
		CHECK(desk.menu == Admit::Opened && desk.file == Admit::Held && desk.ran.empty() && desk.forks == 0 &&
		      !flow.launch.active && !desk.running(), "the Store opens its screen without a game");
		CHECK(bed.exists("launch") && fileText(bed) == other, "the waiting request is still there");
		desk.iteration(10, false);
		CHECK(desk.file == Admit::Started && desk.ran.size() == 1 && desk.ran[0] == "gba:" + other, "and it runs once");
		// A launch file naming the Store is a path that is not a game: it starts nothing.
		bed.request("banana:store");
		desk.iteration(20, true);
		CHECK(desk.ran.size() == 1 && !bed.exists("launch"), "a request made during the game is discarded");
		bed.request("banana:store");
		desk.iteration(6000, false);
		CHECK(desk.file == Admit::Missing && desk.ran.size() == 1 && desk.forks == 1, "a file cannot open the Store");
	}
	{	// A failed fork is not a game's lifetime: nothing is reaped, nothing is discarded, a request made after runs.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		const std::string other = bed.data + "/Other.gba";
		desk.library = {{"gba", bed.rom}, {"gba", other}};
		desk.press(desk.library[0]);
		desk.failFork = true;
		desk.iteration(0, false);
		CHECK(desk.menu == Admit::Failed && desk.ran.empty() && !flow.launch.active && !desk.running(), "no child was forked");
		bed.request(other);
		desk.failFork = false;
		desk.iteration(10, true);
		CHECK(desk.file == Admit::Started && desk.ran.size() == 1 && desk.ran[0] == "gba:" + other,
		      "a request after the failure is not discarded as a game's");
		// A launch file whose own fork fails is consumed, starts nothing, and leaves the next request alone.
		desk.reap(20);
		desk.pass(20);
		bed.request(bed.rom);
		desk.failFork = true;
		desk.iteration(6000, false);
		CHECK(desk.file == Admit::Failed && !bed.exists("launch") && desk.ran.size() == 1 && !flow.launch.active,
		      "a failed file launch is consumed and starts nothing");
		desk.failFork = false;
		bed.request(bed.rom);
		desk.iteration(7000, false);
		CHECK(desk.file == Admit::Started && desk.ran.size() == 2, "the next request runs");
	}
	{	// B leaves the library while held: it is dropped by name, and C is not lost behind it.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		const std::string held = bed.data + "/Held.gbc", other = bed.data + "/Other.gba";
		desk.library = {{"gba", bed.rom}, {"gbc", held}, {"gba", other}};
		const int lock = returnsWhileLocked(bed, desk, desk.library[0]);
		desk.press({"gbc", held});
		std::ofstream(bed.run + "/launch") << other << "\n";
		desk.pass(1000);
		desk.library.erase(desk.library.begin() + 1);
		resumeUnlock(lock);
		desk.pass(1100);
		CHECK(desk.menu == Admit::Missing && desk.ran.size() == 1 && bed.exists("launch"),
		      "the vanished game starts nothing, and the file is still waiting");
		desk.pass(1200);
		CHECK(desk.file == Admit::Started && desk.ran.size() == 2 && desk.ran[1] == "gba:" + other && !bed.exists("launch"),
		      "the launch file then runs exactly once");
	}
	{	// A later press replaces an earlier one that is still held: the player changed their mind.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		desk.library = {{"gba", bed.rom}, {"gba", bed.data + "/One.gba"}, {"gba", bed.data + "/Two.gba"}};
		const int lock = returnsWhileLocked(bed, desk, desk.library[0]);
		desk.press(desk.library[1]);
		desk.press(desk.library[2]);
		resumeUnlock(lock);
		desk.pass(1100);
		CHECK(desk.ran.size() == 2 && desk.ran[1] == "gba:" + bed.data + "/Two.gba", "the latest press starts, once");
	}
	{	// A press while a game runs, or under the latch, is dropped, not kept.
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		Desk desk(bed, flow);
		desk.library = {{"gba", bed.rom}, {"gba", bed.data + "/One.gba"}};
		desk.press(desk.library[0]);
		desk.pass(0);
		desk.press(desk.library[1]);
		desk.pass(10);
		CHECK(desk.menu == Admit::Dropped && desk.ran.size() == 1 && !flow.queuedLaunch(), "a running game drops a press");
		desk.reap(20);
		flow.step(20);
		bed.latch();
		desk.press(desk.library[1]);
		desk.pass(30);
		CHECK(desk.menu == Admit::Dropped && desk.ran.size() == 1 && !flow.queuedLaunch(), "the latch drops a press");
	}
}

// ---- the shutdown latch against the lifecycle's own steps ------------------------------------------------------------
static const FlowBed* hookBed;
static std::string rckSeen;
static void latchNow() { hookBed->latch(); }
// rcK's opening, in its order: the latch first, game.pid second.
static std::string rckSnapshot()
{
	hookBed->latch();
	std::ifstream f(hookBed->run + "/game.pid");
	std::string pid;
	std::getline(f, pid);
	return pid;
}
static void rckNow() { rckSeen = rckSnapshot(); }
static void rckAtFinalLook(int look)
{
	if (look != 2) return;
	beforeLatchLook = nullptr;
	rckSeen = rckSnapshot();
}

// The UI's inherited TERM handler: a write to its self-pipe (main.cpp onSignal).
static int termPipe[2];
static void swallowTerm(int)
{
	const char c = 't';
	const int e = errno;
	if (write(termPipe[1], &c, 1) < 0) {}
	errno = e;
}

static void shutdownInterleavings()
{
	const std::string sessionA(32, 'a');

	// Startup: a shutdown that owns the record leaves it as it is, whatever planning would have decided.
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		bed.publish(bed.native('a'));
		std::remove(bed.rom.c_str());	// planning clears a record whose ROM is gone
		bed.latch();
		CHECK(flow.plan() == Resume::Unavailable && bed.marked(), "planning under the latch removes nothing");
	}
	// The latch appears after a caller's own look and before it holds the lock: every mutation checks it under the lock.
	{	// planning
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		bed.publish(bed.native('a'));
		std::remove(bed.rom.c_str());
		hookBed = &bed;
		afterLock = latchNow;
		CHECK(flow.plan() == Resume::Unavailable && bed.marked(), "a latch that appears under the lock stops planning");
	}
	{	// counting the boot attempt
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		bed.publish(bed.native('a'));
		CHECK(flow.plan() == Resume::Ready, "planned");
		hookBed = &bed;
		afterLock = latchNow;
		CHECK(!flow.reserve(flow.boot) && bed.marked() && bed.count() == -1, "no attempt is counted under a new latch");
	}
	{	// readiness settling
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		bed.publish(bed.native('a'));
		CHECK(flow.plan() == Resume::Ready && flow.reserve(flow.boot), "reserved");
		flow.started(sessionA, bed.rom, &flow.boot);
		bed.report(sessionA, "ready");
		hookBed = &bed;
		afterLock = latchNow;
		CHECK(flow.pollReady() == ResumeFlow::Readiness::Busy && bed.marked() && bed.count() == 1,
		      "readiness cannot settle once the latch appears under the lock");
	}
	{	// an explicit settlement, and a pending cleanup job
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		bed.publish(bed.native('a'));
		CHECK(flow.plan() == Resume::Ready, "planned");
		hookBed = &bed;
		afterLock = latchNow;
		CHECK(!flow.settle(flow.boot) && bed.marked(), "an explicit settlement is refused too");
	}
	{
		FlowBed bed;
		ResumeFlow flow(bed.data, bed.run);
		bed.publish(bed.native('a'));
		CHECK(flow.plan() == Resume::Ready && flow.reserve(flow.boot), "reserved");
		flow.started(sessionA, bed.rom, &flow.boot);
		std::ofstream(bed.run + "/native-ready-" + sessionA) << "";
		CHECK(flow.ended(0, 1000) == ResumeFlow::End::Cleaning, "the return schedules its job");
		hookBed = &bed;
		afterLock = latchNow;
		CHECK(flow.step(1000) == ResumeCleanup::Step::Retried && bed.marked() && bed.count() == 1,
		      "the job removes nothing once the latch appears under the lock");
		CHECK(flow.step(1100) == ResumeCleanup::Step::Halted && bed.marked() && bed.count() == 1, "and then halts");
	}

	// The forked game against rcK, which latches first and reads game.pid second. The game starts only if rcK is
	// bound to see it: a latch that comes at any point makes the child stop before it executes anything.
	const pid_t me = getpid();
	{	// the latch already exists
		FlowBed bed;
		hookBed = &bed;
		bed.latch();
		CHECK(resumeChildStart(bed.run, sessionA, me, bed.rom) == ResumeStart::Latched && !bed.exists("game.session") &&
		      !bed.exists("game.pid"), "a latched shutdown stops the child before it registers");
	}
	{	// rcK runs between registration and game.pid: it reads nothing, and the child must not execute the game
		FlowBed bed;
		hookBed = &bed;
		rckSeen = "unset";
		afterRegister = rckNow;
		CHECK(resumeChildStart(bed.run, sessionA, me, bed.rom) == ResumeStart::Latched && rckSeen.empty(),
		      "rcK found no game.pid, so the child sees the latch and never executes (rcK saw '%s')", rckSeen.c_str());
	}
	{	// rcK runs after game.pid is written and before the child's look: it names the group
		FlowBed bed;
		hookBed = &bed;
		rckSeen = "unset";
		latchLooks = 0;
		beforeLatchLook = rckAtFinalLook;
		const ResumeStart got = resumeChildStart(bed.run, sessionA, me, bed.rom);
		beforeLatchLook = nullptr;
		CHECK(rckSeen == std::to_string((long)me) && got == ResumeStart::Latched,
		      "rcK saw the published group and the child still stops (rcK saw '%s')", rckSeen.c_str());
	}
	{	// no shutdown: the game starts, and a later rcK reads its group
		FlowBed bed;
		hookBed = &bed;
		CHECK(resumeChildStart(bed.run, sessionA, me, bed.rom) == ResumeStart::Go && bed.exists("game.session"),
		      "without a latch the child registers and starts");
		CHECK(rckSnapshot() == std::to_string((long)me), "and rcK, arriving later, reads the group to signal");
		ResumeRegistration seen;
		CHECK(resumeReadRegistration(bed.run, seen) && seen.session == sessionA && seen.pid == me,
		      "its registration names the session and leader");
	}
	{	// a child that cannot publish is not started
		CHECK(resumeChildStart("/nonexistent-banana-run", sessionA, me, "/rom") == ResumeStart::Failed,
		      "a child that cannot register is not started");
	}
	{	// Group TERM after the child's final latch look and before exec. banana-ui's handler writes to its self-pipe and
		// every fork inherits it; the production child start must have made TERM terminate before it published game.pid.
		struct sigaction handler = {}, previous = {};
		handler.sa_handler = swallowTerm;
		CHECK(pipe2(termPipe, O_CLOEXEC | O_NONBLOCK) == 0 && sigaction(SIGTERM, &handler, &previous) == 0,
		      "install the UI's inherited TERM handler");
		const auto run = [&](bool shutdown, int& status, std::string& published)
		{
			FlowBed bed;
			hookBed = &bed;
			int up[2], down[2];
			CHECK(pipe2(up, O_CLOEXEC) == 0 && pipe2(down, O_CLOEXEC) == 0, "create the child's sync pipes");
			const pid_t child = fork();
			if (child == 0)
			{
				setsid();	// as runGame's child
				if (resumeChildEnter(bed.run, sessionA, getpid(), bed.rom) != ResumeStart::Go) _exit(126);
				char c = 'c';	// the final latch look has passed: the signal lands here
				while (write(up[1], &c, 1) < 0 && errno == EINTR) {}
				while (read(down[0], &c, 1) < 0 && errno == EINTR) {}
				execl("/bin/sh", "sh", "-c", "exit 42", (char*)nullptr);
				_exit(127);
			}
			char c = 0;
			CHECK(child > 0 && read(up[0], &c, 1) == 1, "the child passed its final latch look");
			if (shutdown)
			{
				published = rckSnapshot();	// rcK: latch, then game.pid, then the group
				const long group = atol(published.c_str());
				CHECK(group == (long)child && kill(-(pid_t)group, SIGTERM) == 0, "rcK names and signals the child's group");
			}
			while (write(down[1], &c, 1) < 0 && errno == EINTR) {}
			CHECK(waitpid(child, &status, 0) == child, "reap the child");
			close(up[0]); close(up[1]); close(down[0]); close(down[1]);
		};
		int status = 0;
		std::string published;
		run(true, status, published);
		CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM,
		      "a TERM after the final check terminates the child instead of reaching the UI's handler (status 0x%x)", status);
		run(false, status, published);
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42, "control: without a shutdown the child executes (status 0x%x)", status);
		sigaction(SIGTERM, &previous, nullptr);
		close(termPipe[0]);
		close(termPipe[1]);
	}
}

static void sessionIdentity()
{
	char scratch[] = "/tmp/banana-ui-session-XXXXXX";
	if (!mkdtemp(scratch)) { CHECK(false, "create session fixture directory"); return; }
	const std::string dir(scratch), path = dir + "/session.seq";
	std::string id, next;
	CHECK(resumeSessionId(dir, id) && id.size() == 32 &&
	      id.find_first_not_of("0123456789abcdef") == std::string::npos,
	      "a session identity is 128 bits of lower-case hex");
	for (int i = 0; i < 16; ++i)
		CHECK(resumeSessionId(dir, next) && next.size() == 32 && next != id,
		      "a later UI instance cannot reuse a session identity");
	// A restarted process (a new PID) still obtains a distinct identity from the boot-scoped sequence.
	pid_t child = fork();
	if (child == 0)
	{
		std::string restarted;
		_exit(resumeSessionId(dir, restarted) && restarted.size() == 32 && restarted != next ? 0 : 1);
	}
	int status = 0;
	CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
	      "a restarted UI process obtains a distinct session identity");
	CHECK(access((path + ".tmp").c_str(), F_OK) != 0, "the allocator leaves no temporary sequence file");

	// Allocation fails closed on metadata it cannot trust, rather than reusing an old identity.
	std::ofstream(path, std::ios::trunc) << "garbage\n";
	CHECK(!resumeSessionId(dir, next) && next.empty(), "a malformed sequence refuses a new session identity");
	std::ofstream(path, std::ios::trunc) << "18446744073709551615\n";
	CHECK(!resumeSessionId(dir, next) && next.empty(), "an exhausted sequence refuses a new session identity");
	CHECK(unlink((dir + "/session.lock").c_str()) == 0 && symlink(path.c_str(), (dir + "/session.lock").c_str()) == 0,
	      "replace the sequence lock with a symlink");
	CHECK(!resumeSessionId(dir, next) && next.empty(), "an untrusted sequence lock refuses a new session identity");
	CHECK(unlink((dir + "/session.lock").c_str()) == 0, "remove the symlink");
	std::ofstream(path, std::ios::trunc) << "0\n";
	CHECK(resumeSessionId(dir, next) && next.size() == 32, "a restored sequence allocates again");

	unlink(path.c_str());
	unlink((path + ".tmp").c_str());
	unlink((dir + "/session.lock").c_str());
	rmdir(dir.c_str());
}

int main()
{
	// restore never succeeds: 50 attempts, a sleep between each, then the policy depends on the game
	int calls = 0, sleeps = 0;
	bool r = restoreRetry([] { return true; }, [&] { calls++; return false; }, [&] { sleeps++; });
	CHECK(!r && calls == 50 && sleeps == 50, "failing restore: result %d, %d attempts, %d sleeps", r, calls, sleeps);
	CHECK(resumeAction(r, 4242) == ResumeAction::KeepResident, "resident game: must keep the panel and DRM master");
	CHECK(resumeAction(r, 0) == ResumeAction::Reopen, "no game: reopen the panel");
	CHECK(resumeAction(r, -1) == ResumeAction::Reopen, "gamePid -1 is no game");

	// success on the 3rd attempt stops the retries
	calls = sleeps = 0;
	r = restoreRetry([] { return true; }, [&] { return ++calls == 3; }, [&] { sleeps++; });
	CHECK(r && calls == 3 && sleeps == 2, "late success: result %d, %d attempts, %d sleeps", r, calls, sleeps);
	CHECK(resumeAction(r, 4242) == ResumeAction::Resume && resumeAction(r, 0) == ResumeAction::Resume, "restored: resume");

	// a closed display is never asked to restore
	calls = 0;
	r = restoreRetry([] { return false; }, [&] { calls++; return true; }, [] {});
	CHECK(!r && calls == 0, "closed display: result %d, %d attempts", r, calls);

	bootResumeRecords();
	cleanupScheduler();
	sessionRegistration();
	flowLifecycle();
	launchAdmission();
	shutdownInterleavings();
	sessionIdentity();

	if (bad) { printf("%d failures\n", bad); return 1; }
	printf("resume-test: ok\n");
	return 0;
}
