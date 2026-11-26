/* Exercise the daemon's authenticated wire client, policy and marker writer. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fixture_renameat(int, const char *, int, const char *);
static int fixture_fsync(int);
#define renameat fixture_renameat
#define fsync fixture_fsync
#define main bananad_main
#include "../bananad.c"
#undef main
#undef renameat
#undef fsync
#define SYSTEM_ID "gba"
#define CORE_ID "mgba"
#define ROM_PATH "/data/roms/gba/Game with spaces.gba"
#define OTHER_ROM "/data/roms/gba/Different Game.gba"
#define SESSION_ID "0123456789abcdef0123456789abcdef"
#define SOCKET_NAME "retro-" SESSION_ID ".sock"
#define TOKEN_A "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define TOKEN_B "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"
#define TOKEN_C "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"
#define SESSION_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define SESSION_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define RECORD(token) "banana-retro-v1\n" SYSTEM_ID ":" CORE_ID "\n" token "\n" ROM_PATH "\n"

static int interrupt_publish;
/* Directory durability fails after the rename has committed: the visible record is then the candidate. */
static int fail_directory_sync;
static int fixture_renameat(int olddir, const char *oldname, int newdir, const char *newname)
{
	if (interrupt_publish) _exit(73);
	return renameat(olddir, oldname, newdir, newname);
}

static int fixture_fsync(int fd)
{
	struct stat st;
	if (fail_directory_sync && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) { errno = EIO; return -1; }
	return fsync(fd);
}

struct fixture {
	const char *directory;
	const char *after;
	char legacy_id[BANANA_LEGACY_RESUME_ID_SIZE + 1];
	int shutdown, finalized;
};

static void fixture_path(const struct fixture *f, char out[PATH_MAX], const char *name)
{
	int size = snprintf(out, PATH_MAX, "%s/%s", f->directory, name);
	assert(size > 0 && size < PATH_MAX);
}

static void fixture_file(const struct fixture *f, const char *name, const char *bytes, size_t size)
{
	char path[PATH_MAX];
	fixture_path(f, path, name);
	FILE *fp = fopen(path, "w");
	assert(fp && fwrite(bytes, 1, size, fp) == size && fclose(fp) == 0);
}

static void fixture_unlink(const struct fixture *f, const char *name)
{
	char path[PATH_MAX];
	fixture_path(f, path, name);
	assert(unlink(path) == 0);
}

static void expect_record(const struct fixture *f, const char *expected)
{
	char path[PATH_MAX];
	fixture_path(f, path, "resume");
	FILE *fp = fopen(path, "r");
	if (!expected) {
		assert(!fp && errno == ENOENT);
		return;
	}
	char got[256];
	assert(fp);
	size_t size = fread(got, 1, sizeof got, fp);
	assert(feof(fp) && !ferror(fp) && fclose(fp) == 0);
	assert(size == strlen(expected) && !memcmp(got, expected, size));
}

static void read_legacy_id(const struct fixture *f, char id[BANANA_LEGACY_RESUME_ID_SIZE + 1])
{
	char path[PATH_MAX], line[128];
	fixture_path(f, path, "resume");
	FILE *fp = fopen(path, "r");
	assert(fp && fgets(line, sizeof line, fp) && !strcmp(line, ROM_PATH "\n"));
	const size_t prefix = sizeof(BANANA_LEGACY_RESUME_ID_PREFIX) - 1;
	assert(fgets(line, sizeof line, fp) && strlen(line) == prefix + BANANA_LEGACY_RESUME_ID_SIZE + 1);
	assert(!memcmp(line, BANANA_LEGACY_RESUME_ID_PREFIX, prefix));
	assert(retro_hex(line + prefix, BANANA_LEGACY_RESUME_ID_SIZE) &&
	       line[prefix + BANANA_LEGACY_RESUME_ID_SIZE] == '\n');
	assert(fgetc(fp) == EOF && !ferror(fp) && fclose(fp) == 0);
	memcpy(id, line + prefix, BANANA_LEGACY_RESUME_ID_SIZE);
	id[BANANA_LEGACY_RESUME_ID_SIZE] = '\0';
}

static void write_session(const struct fixture *f, pid_t peer, unsigned long long start,
			  const char *state, const char *rom)
{
	char line[256];
	int size = snprintf(line, sizeof line, "v1 %s %d %llu %s 100 %s %s\n",
		SESSION_ID, (int)peer, start, CORE_ID, state, rom);
	assert(size > 0 && (size_t)size < sizeof line);
	fixture_file(f, "native-session", line, (size_t)size);
}

enum peer_reply { REPLY_A, REPLY_B, REPLY_OLD, REPLY_EMPTY, REPLY_FAIL, REPLY_BAD_HEX, REPLY_SHORT,
		  REPLY_EXTRA, REPLY_WRONG_TYPE, REPLY_SHORT_HEADER, REPLY_CLOSED, REPLY_BAD_UNSUPPORTED };

static uint64_t expect_request(int fd, unsigned char *packet, uint16_t type, uint64_t epoch,
			       int proof, uint64_t previous_id)
{
	size_t payload = (epoch ? 8 : 0) + (proof ? 1 : 0);
	size_t header = BANANA_RETRO_CONTROL_HEADER_SIZE;
	const unsigned char session[16] = {1, 35, 69, 103, 137, 171, 205, 239, 1, 35, 69, 103, 137, 171, 205, 239};
	assert(recv(fd, packet, header + 9, MSG_TRUNC) == (ssize_t)(header + payload));
	assert(!memcmp(packet, BANANA_RETRO_CONTROL_MAGIC, 4));
	assert(retro_get16(packet + BANANA_RETRO_CTL_VERSION) == BANANA_RETRO_CONTROL_VERSION);
	assert(retro_get16(packet + BANANA_RETRO_CTL_TYPE) == type);
	assert(!memcmp(packet + BANANA_RETRO_CTL_SESSION_ID, session, sizeof session));
	assert(retro_get16(packet + BANANA_RETRO_CTL_FLAGS) == 0);
	assert((size_t)retro_get16(packet + BANANA_RETRO_CTL_LENGTH) == payload);
	if (epoch) assert(retro_get64(packet + header) == epoch);
	if (proof) assert(packet[header + payload - 1] == BANANA_RETRO_CTL_RESUME_PROOF);
	uint64_t id = retro_get64(packet + BANANA_RETRO_CTL_REQUEST_ID);
	assert(id > previous_id);
	return id;
}

static void reply_packet(int fd, unsigned char *packet, uint16_t type, uint16_t status, enum peer_reply reply)
{
	unsigned char *payload = packet + BANANA_RETRO_CONTROL_HEADER_SIZE;
	size_t n = 0;
	if (reply == REPLY_A || reply == REPLY_B || reply == REPLY_BAD_HEX || reply == REPLY_SHORT ||
	    reply == REPLY_EXTRA || reply == REPLY_BAD_UNSUPPORTED) {
		n = BANANA_RETRO_RESUME_TOKEN_SIZE;
		memcpy(payload + 2, reply == REPLY_B ? TOKEN_B : TOKEN_A, n);
		if (reply == REPLY_BAD_HEX) payload[2] = 'A';
		if (reply == REPLY_SHORT) --n;
		if (reply == REPLY_EXTRA) payload[2 + n++] = '0';
	}
	retro_put16(packet + BANANA_RETRO_CTL_TYPE, (uint16_t)(0x8000u | type));
	retro_put16(packet + BANANA_RETRO_CTL_LENGTH, (uint16_t)(2 + n));
	retro_put16(payload, status);
	size_t size = reply == REPLY_SHORT_HEADER ? 1 : BANANA_RETRO_CONTROL_HEADER_SIZE + 2 + n;
	assert(send(fd, packet, size, 0) == (ssize_t)size);
}

static void serve(int listener, uint16_t type, uint64_t epoch, enum peer_reply reply,
		  const struct fixture *f, const char *before)
{
	int fd = accept(listener, NULL, NULL);
	assert(fd >= 0);
	unsigned char packet[BANANA_RETRO_CONTROL_HEADER_SIZE + 3 + BANANA_RETRO_RESUME_TOKEN_SIZE];
	uint64_t id = expect_request(fd, packet, BANANA_RETRO_CTL_HELLO, 0, 0, 0);
	reply_packet(fd, packet, BANANA_RETRO_CTL_HELLO, BANANA_RETRO_CTL_OK, REPLY_EMPTY);
	id = expect_request(fd, packet, type, epoch, 1, id);
	expect_record(f, before);
	uint16_t status = reply == REPLY_OLD || reply == REPLY_BAD_UNSUPPORTED ? BANANA_RETRO_CTL_UNSUPPORTED :
			  reply == REPLY_FAIL ? BANANA_RETRO_CTL_IO_ERROR : BANANA_RETRO_CTL_OK;
	if (reply != REPLY_CLOSED) {
		reply_packet(fd, packet, reply == REPLY_WRONG_TYPE ? BANANA_RETRO_CTL_WAKE : type, status, reply);
		if (reply == REPLY_OLD) {
			expect_request(fd, packet, type, epoch, 0, id);
			expect_record(f, before);
			reply_packet(fd, packet, type, BANANA_RETRO_CTL_OK, REPLY_EMPTY);
		}
		/* The single legacy exchange above is the only permitted retry. */
		assert(recv(fd, packet, sizeof packet, 0) == 0);
	}
	close(fd);
}

static pid_t start_peer(const struct fixture *f, uint16_t type, uint64_t epoch, enum peer_reply reply,
			const char *metadata_rom, const char *before)
{
	int ready[2];
	assert(pipe(ready) == 0);
	pid_t peer = fork();
	assert(peer >= 0);
	if (peer == 0) {
		close(ready[0]);
		int listener = socket(AF_UNIX, SOCK_SEQPACKET, 0);
		assert(listener >= 0);
		struct sockaddr_un address = { .sun_family = AF_UNIX };
		int n = snprintf(address.sun_path, sizeof address.sun_path, "%s/" SOCKET_NAME, f->directory);
		assert(n > 0 && (size_t)n < sizeof address.sun_path);
		assert(bind(listener, (struct sockaddr *)&address, sizeof address) == 0 && listen(listener, 1) == 0);
		assert(write(ready[1], "r", 1) == 1);
		close(ready[1]);
		serve(listener, type, epoch, reply, f, before);
		close(listener);
		_exit(0);
	}
	close(ready[1]);
	char ready_byte;
	assert(read(ready[0], &ready_byte, 1) == 1 && ready_byte == 'r');
	close(ready[0]);
	unsigned long long start = proc_start_ticks(peer);
	assert(start);
	write_session(f, peer, start, "ready", metadata_rom);
	return peer;
}

static void expect_exit(pid_t peer, int expected)
{
	int status;
	assert(waitpid(peer, &status, 0) == peer && WIFEXITED(status) && WEXITSTATUS(status) == expected);
}

/* The fixture's own session: this process, registered as the game exactly as the UI's child registers itself. */
static struct retro_owner fixture_owner(const char *session, pid_t pid, const char *rom)
{
	struct retro_owner owner = { .pid = pid };
	assert(strlen(session) == 32 && strlen(rom) < sizeof owner.rom);
	strcpy(owner.session, session);
	strcpy(owner.rom, rom);
	owner.start = proc_start_ticks(pid);
	assert(owner.start);
	return owner;
}

static void write_registration(const struct fixture *f, const struct retro_owner *owner)
{
	char line[PATH_MAX + 160];
	int size = snprintf(line, sizeof line, "v1 %s %d %llu %s\n", owner->session, (int)owner->pid, owner->start,
			    owner->rom);
	assert(size > 0 && (size_t)size < sizeof line);
	fixture_file(f, "game.session", line, (size_t)size);
}

static void write_game_pid(const struct fixture *f, pid_t pid)
{
	char line[32];
	int size = snprintf(line, sizeof line, "%d\n", (int)pid);
	assert(size > 0);
	fixture_file(f, "game.pid", line, (size_t)size);
}

static void set_shutdown(struct fixture *f, int on)
{
	char path[PATH_MAX];
	fixture_path(f, path, "shutting-down");
	f->shutdown = on;
	if (on) fixture_file(f, "shutting-down", "", 0);
	else (void)unlink(path);
}

static int publish(const struct fixture *f, const struct retro_owner *owner, int mode,
		   const struct retro_resume_proof *proof)
{
	return retro_write_resume(f->directory, f->directory, owner, mode, proof);
}

static int invalidate(const struct fixture *f, const struct retro_owner *owner, int mode)
{
	return retro_invalidate_resume(f->directory, f->directory, owner, mode);
}

/* Who the stamp says owns the record that is visible now: the session, or NULL for none. */
static const char *visible_owner(const struct fixture *f, char session[33])
{
	char key[RETRO_KEY_MAX];
	int dir = open(f->directory, O_RDONLY | O_DIRECTORY);
	assert(dir >= 0);
	int seen = retro_visible_record(dir, key);
	close(dir);
	return seen == 1 && retro_stamp_owner(f->directory, key, session) ? session : NULL;
}

static void expect_owner(const struct fixture *f, const char *expected)
{
	char session[33];
	const char *got = visible_owner(f, session);
	assert(expected ? got && !strcmp(got, expected) : !got);
}

static int request_proof(void *data, pid_t game, const struct retro_owner *owner, uint16_t op, uint64_t epoch,
			 unsigned timeout, struct retro_resume_proof *proof)
{
	const struct fixture *f = data;
	assert((op == BANANA_RETRO_CTL_CHECKPOINT && !epoch && timeout == 1500) ||
	       (op == BANANA_RETRO_CTL_PREPARE_SLEEP && epoch && timeout == 1500) ||
	       (op == BANANA_RETRO_CTL_SHUTDOWN && epoch && timeout == 3000));
	return retro_control_request(f->directory, game, owner, op, epoch, timeout, proof);
}

static int child_alive(void *data, pid_t game)
{
	const struct fixture *f = data;
	assert(game == getpid());
	return !f->shutdown;
}

static int resume_marker(void *data, const struct retro_owner *owner, int mode,
			 const struct retro_resume_proof *proof)
{
	return publish(data, owner, mode, proof);
}

static int resume_invalidate(void *data, const struct retro_owner *owner, int mode)
{
	return invalidate(data, owner, mode);
}

static void finalize(void *data)
{
	struct fixture *f = data;
	assert(f->shutdown);
	if (f->legacy_id[0]) {
		char next[BANANA_LEGACY_RESUME_ID_SIZE + 1];
		read_legacy_id(f, next);
		assert(strcmp(next, f->legacy_id) != 0); /* same ROM, new publication on shutdown */
	} else expect_record(f, f->after);
	++f->finalized;
}

static unsigned legacy_calls;
static int legacy_save(pid_t game) { ++legacy_calls; return game == getpid(); }

static void check_exchange(struct retro_power_ops *ops, const char *rom, uint16_t type, enum peer_reply reply,
			   const char *metadata_rom, const char *before, const char *after, int result)
{
	struct fixture *f = ops->data;
	struct retro_owner owner = fixture_owner(SESSION_ID, getpid(), rom);
	set_shutdown(f, type == BANANA_RETRO_CTL_SHUTDOWN);
	f->finalized = 0;
	f->after = after;
	uint64_t epoch = f->shutdown ? 31337 : type == BANANA_RETRO_CTL_PREPARE_SLEEP ? 31338 : 0;
	pid_t peer = start_peer(f, type, epoch, reply, metadata_rom, before);
	unsigned before_legacy = legacy_calls;
	if (f->shutdown) {
		assert(retro_shutdown_session(ops, getpid(), getpgrp(), epoch, &owner) == result);
		assert(f->finalized == 1);
	} else if (type == BANANA_RETRO_CTL_PREPARE_SLEEP) {
		/* Only acknowledged holds are exercised here: a failed one signals the group. */
		assert(retro_prepare_suspend(ops, getpid(), getpgrp(), epoch, 1, &owner) == result);
	} else assert(session_checkpoint(ops, getpid(), &owner, legacy_save) == result);
	assert(legacy_calls == before_legacy);
	expect_record(f, after);
	expect_exit(peer, 0);
	fixture_unlink(f, "native-session");
	fixture_unlink(f, SOCKET_NAME);
}

/* A second game process: it leads its own session as the UI's child does after setsid(). */
static pid_t spawn_leader(void)
{
	pid_t leader = fork();
	assert(leader >= 0);
	if (leader == 0) {
		if (setsid() < 0) _exit(70);
		for (;;) pause();
	}
	for (int i = 0; i < 400 && getpgid(leader) != leader; ++i) usleep(5000);
	assert(getpgid(leader) == leader);
	return leader;
}

static void reap_leader(pid_t leader)
{
	assert(kill(leader, SIGKILL) == 0);
	assert(waitpid(leader, NULL, 0) == leader);
}

struct session { pid_t leader; struct retro_owner owner; };

/* Register a running game the way the UI's child does: registration first, then game.pid. */
static struct session start_session(const struct fixture *f, const char *id, const char *rom)
{
	struct session s = { .leader = spawn_leader() };
	s.owner = fixture_owner(id, s.leader, rom);
	write_registration(f, &s.owner);
	write_game_pid(f, s.leader);
	return s;
}

/* The UI's reap: the leader is gone, then the registration and game.pid are retired. */
static void retire_session(const struct fixture *f, struct session *s)
{
	reap_leader(s->leader);
	fixture_unlink(f, "game.session");
	fixture_unlink(f, "game.pid");
}

static void stamp_text(const struct fixture *f, char out[1024])
{
	char path[PATH_MAX];
	fixture_path(f, path, "resume-owner");
	FILE *fp = fopen(path, "r");
	assert(fp);
	size_t size = fread(out, 1, 1023, fp);
	assert(feof(fp) && fclose(fp) == 0);
	out[size] = '\0';
}

/* Requirement 1: the registration binds a session to a live group leader, start ticks and ROM. */
static void check_registration(struct fixture *f)
{
	struct retro_owner got;
	struct session a = start_session(f, SESSION_A, ROM_PATH);
	assert(retro_capture_owner(f->directory, a.leader, &got, RETRO_PUBLISH_LIVE) && !strcmp(got.session, SESSION_A) &&
	       got.pid == a.leader && got.start == a.owner.start && !strcmp(got.rom, ROM_PATH));
	/* A caller that names another game, or a game.pid naming another process, captures nothing. */
	assert(!retro_capture_owner(f->directory, a.leader + 1, &got, RETRO_PUBLISH_LIVE) && !got.session[0]);
	write_game_pid(f, getpid());
	assert(!retro_capture_owner(f->directory, a.leader, &got, RETRO_PUBLISH_LIVE));
	write_game_pid(f, a.leader);
	/* A reused PID has other start ticks. */
	struct retro_owner stale = a.owner;
	stale.start += 1;
	write_registration(f, &stale);
	assert(!retro_capture_owner(f->directory, a.leader, &got, RETRO_PUBLISH_LIVE));
	write_registration(f, &a.owner);
	assert(retro_capture_owner(f->directory, a.leader, &got, RETRO_PUBLISH_LIVE));
	/* A registered process that does not lead its group is not the tracked game. */
	pid_t follower = fork();
	assert(follower >= 0);
	if (follower == 0) for (;;) pause();
	struct retro_owner member = fixture_owner(SESSION_A, follower, ROM_PATH);
	write_registration(f, &member);
	write_game_pid(f, follower);
	assert(!retro_capture_owner(f->directory, follower, &got, RETRO_PUBLISH_LIVE));
	reap_leader(follower);
	write_registration(f, &a.owner);
	write_game_pid(f, a.leader);
	/* Malformed registrations fail closed. */
	char line[PATH_MAX + 160];
	const char *shapes[] = {
		"v2 %s %d %llu %s\n", "v1 %.31s %d %llu %s\n", "v1 AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA %d %llu %s\n",
		"v1 %s 0 %llu %s\n", "v1 %s %d 0 %s\n", "v1 %s %d %llu relative.gba\n", "v1 %s %d %llu %s",
		"v1 %s %d %llu %s\nextra\n", "v1 %s %d %llu\n"
	};
	for (size_t i = 0; i < sizeof shapes / sizeof *shapes; ++i) {
		const char *shape = shapes[i];
		int size;
		if (i == 2) size = snprintf(line, sizeof line, shape, (int)a.leader, a.owner.start, ROM_PATH);
		else if (i == 3) size = snprintf(line, sizeof line, shape, SESSION_A, a.owner.start, ROM_PATH);
		else if (i == 4) size = snprintf(line, sizeof line, shape, SESSION_A, (int)a.leader, ROM_PATH);
		else if (i == 5) size = snprintf(line, sizeof line, shape, SESSION_A, (int)a.leader, a.owner.start);
		else if (i == 8) size = snprintf(line, sizeof line, shape, SESSION_A, (int)a.leader, a.owner.start);
		else size = snprintf(line, sizeof line, shape, SESSION_A, (int)a.leader, a.owner.start, ROM_PATH);
		assert(size > 0 && (size_t)size < sizeof line);
		fixture_file(f, "game.session", line, (size_t)size);
		assert(!retro_capture_owner(f->directory, a.leader, &got, RETRO_PUBLISH_LIVE));
	}
	fixture_file(f, "game.session", "v1 " SESSION_A " 1 2 /r\0x\n", sizeof("v1 " SESSION_A " 1 2 /r\0x\n") - 1);
	assert(!retro_capture_owner(f->directory, a.leader, &got, RETRO_PUBLISH_LIVE));
	/* No registration, an exited leader, and a retired session all capture nothing. */
	fixture_unlink(f, "game.session");
	assert(!retro_capture_owner(f->directory, a.leader, &got, RETRO_PUBLISH_LIVE));
	write_registration(f, &a.owner);
	reap_leader(a.leader);
	assert(!retro_capture_owner(f->directory, a.leader, &got, RETRO_PUBLISH_LIVE));
	fixture_unlink(f, "game.session");
	fixture_unlink(f, "game.pid");
}

/* Requirements 2, 3 and 8: the two-entry stamp, publisher revalidation, owner-only removal, no blanket unlink. */
static void check_ownership(struct fixture *f)
{
	const struct retro_resume_proof proof_a = { .system = SYSTEM_ID, .core = CORE_ID, .token = TOKEN_A };
	const struct retro_resume_proof proof_b = { .system = SYSTEM_ID, .core = CORE_ID, .token = TOKEN_B };
	const struct retro_resume_proof proof_c = { .system = SYSTEM_ID, .core = CORE_ID, .token = TOKEN_C };
	char path[PATH_MAX], text[1024];
	fixture_path(f, path, "resume");
	assert(unlink(path) == 0 || errno == ENOENT);

	/* Session A publishes; the stamp binds the visible record to it, with the empty record as "previous". */
	struct session a = start_session(f, SESSION_A, ROM_PATH);
	assert(publish(f, &a.owner, RETRO_PUBLISH_LIVE, &proof_a));
	expect_record(f, RECORD(TOKEN_A));
	expect_owner(f, SESSION_A);
	stamp_text(f, text);
	assert(!strcmp(text, "banana-resume-owner-v1\n-\n" SESSION_A " N " TOKEN_A " " SYSTEM_ID " " CORE_ID " " ROM_PATH "\n"));

	/* Another session of the same ROM replaces it; the stamp keeps A's binding as the previous entry. */
	struct session b;
	retire_session(f, &a);
	b = start_session(f, SESSION_B, ROM_PATH);
	/* A's stale publication, battery-only invalidation and shutdown publication are refused outright. */
	assert(!publish(f, &a.owner, RETRO_PUBLISH_LIVE, &proof_c));
	assert(!invalidate(f, &a.owner, RETRO_PUBLISH_LIVE));
	set_shutdown(f, 1);
	assert(!publish(f, &a.owner, RETRO_PUBLISH_SHUTDOWN, &proof_c));
	set_shutdown(f, 0);
	expect_record(f, RECORD(TOKEN_A));
	/* B did not publish A's record, so B's battery-only commit leaves it alone. */
	assert(invalidate(f, &b.owner, RETRO_PUBLISH_LIVE));
	expect_record(f, RECORD(TOKEN_A));
	/* A second owner of an identical visible key is refused before anything is staged: the candidate entry would
	 * otherwise claim A's record before the rename. */
	stamp_text(f, text);
	assert(!publish(f, &b.owner, RETRO_PUBLISH_LIVE, &proof_a));
	expect_record(f, RECORD(TOKEN_A));
	expect_owner(f, SESSION_A);
	{
		char after[1024];
		stamp_text(f, after);
		assert(!strcmp(text, after));
	}
	assert(publish(f, &b.owner, RETRO_PUBLISH_LIVE, &proof_b));
	expect_record(f, RECORD(TOKEN_B));
	expect_owner(f, SESSION_B);
	stamp_text(f, text);
	assert(strstr(text, "\n" SESSION_A " N " TOKEN_A " ") && strstr(text, "\n" SESSION_B " N " TOKEN_B " "));

	/* The interrupted publication: before the rename the visible record still maps to its own owner. */
	pid_t interrupted = fork();
	assert(interrupted >= 0);
	if (interrupted == 0) {
		interrupt_publish = 1;
		(void)publish(f, &b.owner, RETRO_PUBLISH_LIVE, &proof_c);
		_exit(1);
	}
	expect_exit(interrupted, 73);
	expect_record(f, RECORD(TOKEN_B));
	expect_owner(f, SESSION_B);
	stamp_text(f, text);
	assert(strstr(text, "\n" SESSION_B " N " TOKEN_B " ") && strstr(text, "\n" SESSION_B " N " TOKEN_C " "));
	/* After the rename the candidate entry is the visible one. */
	assert(publish(f, &b.owner, RETRO_PUBLISH_LIVE, &proof_c));
	expect_owner(f, SESSION_B);
	/* A visible record that no entry names, a damaged stamp, or none at all, is unowned: nothing removes it. */
	fixture_file(f, "resume-owner", "banana-resume-owner-v1\n-\n- N " TOKEN_C " " SYSTEM_ID " " CORE_ID " " ROM_PATH "\n",
		     sizeof("banana-resume-owner-v1\n-\n- N " TOKEN_C " " SYSTEM_ID " " CORE_ID " " ROM_PATH "\n") - 1);
	expect_owner(f, NULL);
	assert(invalidate(f, &b.owner, RETRO_PUBLISH_LIVE));
	expect_record(f, RECORD(TOKEN_C));
	fixture_file(f, "resume-owner", "garbage\n", sizeof("garbage\n") - 1);
	expect_owner(f, NULL);
	assert(invalidate(f, &b.owner, RETRO_PUBLISH_LIVE));
	expect_record(f, RECORD(TOKEN_C));
	fixture_unlink(f, "resume-owner");
	assert(invalidate(f, &b.owner, RETRO_PUBLISH_LIVE));
	expect_record(f, RECORD(TOKEN_C));
	/* An old record with no publication ID can never be owned. */
	fixture_file(f, "resume", ROM_PATH "\n", sizeof(ROM_PATH "\n") - 1);
	assert(invalidate(f, &b.owner, RETRO_PUBLISH_LIVE) && access(path, F_OK) == 0);

	/* Only the session that published a record may invalidate it, and then only it. */
	assert(publish(f, &b.owner, RETRO_PUBLISH_LIVE, &proof_c));
	assert(invalidate(f, &b.owner, RETRO_PUBLISH_LIVE));
	expect_record(f, NULL);
	assert(invalidate(f, &b.owner, RETRO_PUBLISH_LIVE)); /* nothing left to remove is not a failure */

	/* The whole stamp is read: an extra line, a damaged entry, a missing one, or two owners for one key leave the
	 * record unowned even when the other entry names it exactly. Nothing then removes it on a session's behalf. */
	{
#define ENTRY(session, token) session " N " token " " SYSTEM_ID " " CORE_ID " " ROM_PATH "\n"
		static const char *const stamps[] = {
			"banana-resume-owner-v1\n" ENTRY(SESSION_B, TOKEN_B) ENTRY(SESSION_B, TOKEN_C) "extra\n",
			"banana-resume-owner-v1\n" ENTRY(SESSION_B, TOKEN_B) "garbage\n",
			"banana-resume-owner-v1\n" "garbage\n" ENTRY(SESSION_B, TOKEN_B),
			"banana-resume-owner-v1\n" ENTRY(SESSION_B, TOKEN_B),
			"banana-resume-owner-v1\n" ENTRY(SESSION_B, TOKEN_C) ENTRY(SESSION_B, TOKEN_B) "\n",
			"banana-resume-owner-v1\n" ENTRY("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", TOKEN_C) ENTRY(SESSION_B, TOKEN_B),
			"banana-resume-owner-v1\n" ENTRY(SESSION_B, TOKEN_B) ENTRY("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", TOKEN_C),
			"banana-resume-owner-v1\n" ENTRY(SESSION_B, "xyz") ENTRY(SESSION_B, TOKEN_B),
			"banana-resume-owner-v1\n" ENTRY(SESSION_A, TOKEN_B) ENTRY(SESSION_B, TOKEN_B),
		};
		assert(publish(f, &b.owner, RETRO_PUBLISH_LIVE, &proof_b));
		for (size_t i = 0; i < sizeof stamps / sizeof *stamps; ++i) {
			fixture_file(f, "resume-owner", stamps[i], strlen(stamps[i]));
			expect_owner(f, NULL);
			assert(invalidate(f, &b.owner, RETRO_PUBLISH_LIVE));
			expect_record(f, RECORD(TOKEN_B));
		}
		/* The same record with a well-formed stamp is B's, so the checks above are about the stamp alone. */
		static const char *const owned[] = {
			"banana-resume-owner-v1\n-\n" ENTRY(SESSION_B, TOKEN_B),
			"banana-resume-owner-v1\n" ENTRY(SESSION_B, TOKEN_C) ENTRY(SESSION_B, TOKEN_B),
			"banana-resume-owner-v1\n" ENTRY(SESSION_B, TOKEN_B) ENTRY(SESSION_B, TOKEN_B),
		};
		for (size_t i = 0; i < sizeof owned / sizeof *owned; ++i) {
			fixture_file(f, "resume-owner", owned[i], strlen(owned[i]));
			expect_owner(f, SESSION_B);
		}
#undef ENTRY
	}

	/* The latch is checked under the lock, for a live session too: once a shutdown owns the record, a periodic
	 * publication or battery-only removal that passed its earlier look at the latch is refused. */
	set_shutdown(f, 1);
	assert(!publish(f, &b.owner, RETRO_PUBLISH_LIVE, &proof_c));
	assert(!invalidate(f, &b.owner, RETRO_PUBLISH_LIVE));
	expect_record(f, RECORD(TOKEN_B));
	set_shutdown(f, 0);

	/* A stamp that cannot be staged aborts the publication before the record is touched. */
	assert(publish(f, &b.owner, RETRO_PUBLISH_LIVE, &proof_a));
	char tmp[PATH_MAX];
	fixture_path(f, tmp, "resume-owner.tmp");
	assert(mkdir(tmp, 0700) == 0);
	assert(!publish(f, &b.owner, RETRO_PUBLISH_LIVE, &proof_b));
	expect_record(f, RECORD(TOKEN_A));
	assert(rmdir(tmp) == 0);
	/* A record that cannot be staged fails without removing the one in place, and owns nothing new. */
	char record_tmp[PATH_MAX];
	fixture_path(f, record_tmp, "resume.tmp");
	assert(mkdir(record_tmp, 0700) == 0);
	assert(!publish(f, &b.owner, RETRO_PUBLISH_LIVE, &proof_b));
	expect_record(f, RECORD(TOKEN_A));
	expect_owner(f, SESSION_B);
	assert(rmdir(record_tmp) == 0);

	/* Publication races a reap: the UI reaps B and retires its registration after the daemon captured it. */
	struct retro_owner captured;
	assert(retro_capture_owner(f->directory, b.leader, &captured, RETRO_PUBLISH_LIVE));
	assert(unlink(path) == 0);
	retire_session(f, &b);
	assert(!publish(f, &captured, RETRO_PUBLISH_LIVE, &proof_c));
	expect_record(f, NULL); /* a record the cleanup removed is not resurrected */
	assert(!invalidate(f, &captured, RETRO_PUBLISH_LIVE));

	/* Shutdown publication is for the captured session only, under the latch, while it is still registered. */
	struct session c = start_session(f, SESSION_A, ROM_PATH);
	assert(retro_capture_owner(f->directory, c.leader, &captured, RETRO_PUBLISH_LIVE));
	reap_leader(c.leader);
	assert(!publish(f, &captured, RETRO_PUBLISH_LIVE, &proof_a)); /* LIVE needs a live leader */
	assert(!publish(f, &captured, RETRO_PUBLISH_SHUTDOWN, &proof_a)); /* SHUTDOWN needs the latch */
	set_shutdown(f, 1);
	assert(publish(f, &captured, RETRO_PUBLISH_SHUTDOWN, &proof_a));
	expect_record(f, RECORD(TOKEN_A));
	expect_owner(f, SESSION_A);
	struct retro_owner other = captured;
	strcpy(other.session, SESSION_B);
	assert(!publish(f, &other, RETRO_PUBLISH_SHUTDOWN, &proof_b)); /* the registration names someone else */
	expect_record(f, RECORD(TOKEN_A));
	fixture_unlink(f, "game.session");
	assert(!publish(f, &captured, RETRO_PUBLISH_SHUTDOWN, &proof_b)); /* a retired registration is stale */
	expect_record(f, RECORD(TOKEN_A));
	assert(!invalidate(f, &captured, RETRO_PUBLISH_SHUTDOWN));
	expect_record(f, RECORD(TOKEN_A));
	set_shutdown(f, 0);
	fixture_unlink(f, "game.pid");
	assert(unlink(path) == 0);
	fixture_unlink(f, "resume-owner");
}

static char process_state(pid_t pid)
{
	char path[64], line[512];
	snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
	FILE *fp = fopen(path, "r");
	if (!fp) return '?';
	char *got = fgets(line, sizeof line, fp);
	fclose(fp);
	char *at = got ? strrchr(line, ')') : NULL;
	return at && at[1] == ' ' ? at[2] : '?';
}

static void wait_state(pid_t pid, const char *states)
{
	for (int i = 0; i < 600 && !strchr(states, process_state(pid)); ++i) usleep(5000);
	assert(strchr(states, process_state(pid)));
}

/* A stopped leader is still the registered session when its identity checks out. Only a shutdown capture takes
 * it: every other capture, and every identity failure, behaves as for a running leader. */
static void check_stopped_leader(struct fixture *f)
{
	const struct retro_resume_proof proof_a = { .system = SYSTEM_ID, .core = CORE_ID, .token = TOKEN_A };
	struct retro_owner got;
	char path[PATH_MAX];
	fixture_path(f, path, "resume");
	struct session a = start_session(f, SESSION_A, ROM_PATH);
	assert(kill(a.leader, SIGSTOP) == 0);
	wait_state(a.leader, "Tt");
	assert(!retro_capture_owner(f->directory, a.leader, &got, RETRO_PUBLISH_LIVE) && !got.session[0]);
	assert(retro_capture_owner(f->directory, a.leader, &got, RETRO_PUBLISH_SHUTDOWN) &&
	       !strcmp(got.session, SESSION_A) && got.pid == a.leader && got.start == a.owner.start &&
	       !strcmp(got.rom, ROM_PATH));
	/* The identity checks still apply to a stopped leader. */
	struct retro_owner stale = a.owner;
	stale.start += 1;
	write_registration(f, &stale);
	assert(!retro_capture_owner(f->directory, a.leader, &got, RETRO_PUBLISH_SHUTDOWN) && !got.session[0]);
	write_registration(f, &a.owner);
	write_game_pid(f, getpid());
	assert(!retro_capture_owner(f->directory, a.leader, &got, RETRO_PUBLISH_SHUTDOWN));
	write_game_pid(f, a.leader);
	pid_t follower = fork();
	assert(follower >= 0);
	if (follower == 0) for (;;) pause();
	assert(kill(follower, SIGSTOP) == 0);
	wait_state(follower, "Tt");
	struct retro_owner member = fixture_owner(SESSION_A, follower, ROM_PATH);
	write_registration(f, &member);
	write_game_pid(f, follower);
	assert(!retro_capture_owner(f->directory, follower, &got, RETRO_PUBLISH_SHUTDOWN)); /* not its group's leader */
	reap_leader(follower);
	write_registration(f, &a.owner);
	write_game_pid(f, a.leader);
	/* The captured stopped session can publish under the latch, and a live publication still cannot. */
	assert(retro_capture_owner(f->directory, a.leader, &got, RETRO_PUBLISH_SHUTDOWN));
	assert(!publish(f, &got, RETRO_PUBLISH_LIVE, &proof_a));
	set_shutdown(f, 1);
	assert(publish(f, &got, RETRO_PUBLISH_SHUTDOWN, &proof_a));
	expect_record(f, RECORD(TOKEN_A));
	expect_owner(f, SESSION_A);
	set_shutdown(f, 0);
	assert(unlink(path) == 0);
	/* A zombie is never the session, stopped or not. */
	assert(kill(a.leader, SIGKILL) == 0);
	wait_state(a.leader, "Z");
	assert(!retro_capture_owner(f->directory, a.leader, &got, RETRO_PUBLISH_SHUTDOWN) && !got.session[0]);
	assert(waitpid(a.leader, NULL, 0) == a.leader);
	fixture_unlink(f, "game.session");
	fixture_unlink(f, "game.pid");
	fixture_unlink(f, "resume-owner");
}

/* The latch is created under resume.lock: a writer that holds the lock has finished before the latch exists, and
 * a lock that stays held cannot stop it. */
static void check_shutdown_latch(struct fixture *f)
{
	const struct retro_resume_proof proof_a = { .system = SYSTEM_ID, .core = CORE_ID, .token = TOKEN_A };
	char latch[PATH_MAX];
	fixture_path(f, latch, "shutting-down");
	struct session a = start_session(f, SESSION_A, ROM_PATH);
	for (int held_for_ever = 0; held_for_ever < 2; ++held_for_ever) {
		int ready[2], answer[2], release[2];
		assert(pipe(ready) == 0 && pipe(answer) == 0 && pipe(release) == 0);
		pid_t writer = fork();
		assert(writer >= 0);
		if (writer == 0) {
			/* A writer past its latch check, inside the critical section. */
			int lock = retro_resume_lock(f->directory);
			char mark = 'r';
			if (lock < 0 || write(ready[1], &mark, 1) != 1) _exit(60);
			if (held_for_ever) {
				if (read(release[0], &mark, 1) != 1) _exit(61);
			} else usleep(5000);
			mark = access(latch, F_OK) == 0 ? 'L' : 'A';
			if (write(answer[1], &mark, 1) != 1) _exit(62);
			close(lock);
			_exit(0);
		}
		char mark;
		assert(read(ready[0], &mark, 1) == 1);
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		retro_latch_shutdown(f->directory, f->directory);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		assert(access(latch, F_OK) == 0);	/* bounded: it is there either way */
		double took = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
		assert(took < 1.0);
		if (held_for_ever) {
			/* The lock was never obtained; the latch was created anyway. */
			assert(took >= 0.03 && write(release[1], &mark, 1) == 1);
		}
		assert(read(answer[0], &mark, 1) == 1);
		/* Released: it waited. Held for ever: it did not. */
		assert(mark == (held_for_ever ? 'L' : 'A'));
		expect_exit(writer, 0);
		close(ready[0]); close(ready[1]); close(answer[0]); close(answer[1]); close(release[0]); close(release[1]);
		assert(!publish(f, &a.owner, RETRO_PUBLISH_LIVE, &proof_a));	/* after the latch no live writer succeeds */
		assert(!invalidate(f, &a.owner, RETRO_PUBLISH_LIVE));
		expect_record(f, NULL);
		assert(unlink(latch) == 0);
	}
	/* No lock file to take (the card is not mounted): the latch still appears. */
	retro_latch_shutdown("/nonexistent-banana-data", f->directory);
	assert(access(latch, F_OK) == 0 && unlink(latch) == 0);
	retire_session(f, &a);
}

/* The rename is the commit. When the directory sync after it fails, the candidate is already the visible record:
 * the failure is reported, durability is unknown, and nothing rolls it back or hides it. */
static void check_directory_sync(struct fixture *f)
{
	const struct retro_resume_proof proof_a = { .system = SYSTEM_ID, .core = CORE_ID, .token = TOKEN_A };
	const struct retro_resume_proof proof_b = { .system = SYSTEM_ID, .core = CORE_ID, .token = TOKEN_B };
	const struct retro_resume_proof proof_c = { .system = SYSTEM_ID, .core = CORE_ID, .token = TOKEN_C };
	char text[1024];
	struct session a = start_session(f, SESSION_A, ROM_PATH);
	assert(publish(f, &a.owner, RETRO_PUBLISH_LIVE, &proof_a));
	expect_record(f, RECORD(TOKEN_A));
	fail_directory_sync = 1;
	assert(!publish(f, &a.owner, RETRO_PUBLISH_LIVE, &proof_b));
	fail_directory_sync = 0;
	expect_record(f, RECORD(TOKEN_B));	/* the candidate is visible although the publication reported failure */
	expect_owner(f, SESSION_A);	/* and the stamp names it for this session, not the superseded one */
	stamp_text(f, text);
	assert(strstr(text, "\n" SESSION_A " N " TOKEN_A " ") && strstr(text, "\n" SESSION_A " N " TOKEN_B " "));
	/* Not a rollback: a retry of the same publication succeeds on the record that is already there. */
	assert(publish(f, &a.owner, RETRO_PUBLISH_LIVE, &proof_b));
	expect_record(f, RECORD(TOKEN_B));
	/* The same under the shutdown latch. */
	set_shutdown(f, 1);
	fail_directory_sync = 1;
	assert(!publish(f, &a.owner, RETRO_PUBLISH_SHUTDOWN, &proof_c));
	fail_directory_sync = 0;
	set_shutdown(f, 0);
	expect_record(f, RECORD(TOKEN_C));
	expect_owner(f, SESSION_A);
	/* An invalidation whose unlink committed but whose sync failed reports failure with the record gone. */
	fail_directory_sync = 1;
	assert(!invalidate(f, &a.owner, RETRO_PUBLISH_LIVE));
	fail_directory_sync = 0;
	expect_record(f, NULL);
	/* The unsynced unlink is a debt that outlives the call. A retry that finds the record absent, or replaced by
	 * another publication, still pays it first and fails while the sync does; it never removes the replacement. */
	fail_directory_sync = 1;
	assert(!invalidate(f, &a.owner, RETRO_PUBLISH_LIVE));	/* absent */
	fixture_file(f, "resume", RECORD(TOKEN_A), sizeof(RECORD(TOKEN_A)) - 1);	/* no stamp names it: foreign */
	assert(!invalidate(f, &a.owner, RETRO_PUBLISH_LIVE));
	fail_directory_sync = 0;
	expect_record(f, RECORD(TOKEN_A));
	assert(invalidate(f, &a.owner, RETRO_PUBLISH_LIVE));	/* the sync succeeds: the debt is paid */
	expect_record(f, RECORD(TOKEN_A));	/* the replacement is untouched */
	fixture_unlink(f, "resume");
	fail_directory_sync = 1;
	assert(invalidate(f, &a.owner, RETRO_PUBLISH_LIVE));	/* nothing is owed now, so nothing is synced */
	fail_directory_sync = 0;
	retire_session(f, &a);
	fixture_unlink(f, "resume-owner");
}

int main(void)
{
	/* The registered game must lead its own group, as the UI's child does after setsid(). */
	if (getpgrp() != getpid()) assert(setsid() == getpid());
	char work[] = "/tmp/banana-checkpoint-XXXXXX";
	assert(mkdtemp(work));
	struct fixture f = { .directory = work };
	struct retro_power_report report;
#define EXPECT_REPORT(c, p, k) assert(report.checkpoint == (c) && report.publication == (p) && report.cleanup == (k))
	struct retro_power_ops ops = {
		.data = &f, .request_proof = request_proof, .child_alive = child_alive,
		.resume_marker = resume_marker, .resume_invalidate = resume_invalidate, .shutdown_finalize = finalize,
		.report = &report
	};
	char path[PATH_MAX], rom[PATH_MAX + 1];
	fixture_path(&f, path, "game.path");
	fixture_file(&f, "game.path", ROM_PATH "\0x\n", sizeof(ROM_PATH "\0x\n") - 1);
	assert(retro_read_line(path, rom, sizeof rom) == RETRO_POWER_FAILED && !*rom);
	fixture_file(&f, "game.path", ROM_PATH "\n", sizeof(ROM_PATH "\n") - 1);
	assert(retro_read_line(path, rom, sizeof rom) == RETRO_POWER_OK && !strcmp(rom, ROM_PATH));
	fixture_unlink(&f, "game.path");

	check_registration(&f);
	check_ownership(&f);
	check_stopped_leader(&f);
	check_shutdown_latch(&f);
	check_directory_sync(&f);

	/* This process is the registered game for the exchanges below. */
	struct retro_owner owner = fixture_owner(SESSION_ID, getpid(), rom);
	write_registration(&f, &owner);
	write_game_pid(&f, getpid());

	assert(session_checkpoint(&ops, getpid(), &owner, legacy_save) && legacy_calls == 1);
	read_legacy_id(&f, f.legacy_id);
	expect_owner(&f, SESSION_ID);
	fixture_file(&f, "game.native", "normal " SYSTEM_ID "\n", sizeof("normal " SYSTEM_ID "\n") - 1);
	set_shutdown(&f, 1);
	assert(retro_shutdown_session(&ops, getpid(), getpgrp(), 1, &owner) == RETRO_POWER_NONE);
	assert(f.finalized == 1);
	f.legacy_id[0] = '\0';
	fixture_path(&f, path, "resume");
	assert(unlink(path) == 0);

	unsigned long long self_start = owner.start;
	set_shutdown(&f, 0);
	write_session(&f, getpid(), self_start, "starting", rom);
	assert(!session_checkpoint(&ops, getpid(), &owner, legacy_save));
	write_session(&f, getpid(), self_start + 1, "ready", rom);
	assert(!session_checkpoint(&ops, getpid(), &owner, legacy_save));
	write_session(&f, getpid(), self_start, "ready", rom);
	assert(!session_checkpoint(&ops, getpid(), &owner, legacy_save)); /* missing socket */
	fixture_file(&f, "native-session", "v1 truncated", sizeof("v1 truncated") - 1);
	assert(!session_checkpoint(&ops, getpid(), &owner, legacy_save));
	fixture_unlink(&f, "native-session");
	fixture_file(&f, "game.native", "native " SYSTEM_ID "\n", sizeof("native " SYSTEM_ID "\n") - 1);
	assert(!session_checkpoint(&ops, getpid(), &owner, legacy_save));
	set_shutdown(&f, 1);
	f.after = NULL;
	assert(retro_shutdown_session(&ops, getpid(), getpgrp(), 1, &owner) == RETRO_POWER_FAILED);
	fixture_unlink(&f, "game.native");
	assert(legacy_calls == 1);
	expect_record(&f, NULL);

	const uint16_t checkpoint = BANANA_RETRO_CTL_CHECKPOINT, shutdown = BANANA_RETRO_CTL_SHUTDOWN;
	check_exchange(&ops, rom, checkpoint, REPLY_A, rom, NULL, NULL, 0); /* unknown system */
	fixture_file(&f, "game.native", "normal " SYSTEM_ID "\n", sizeof("normal " SYSTEM_ID "\n") - 1);
	/* Native proof binds to the registered session. A launcher whose metadata names another session is not
	 * reached at all: the peer below would answer a request, so only the binding keeps this one from succeeding. */
	{
		struct retro_owner other_session = fixture_owner(SESSION_B, getpid(), rom);
		struct retro_resume_proof refused;
		pid_t peer = start_peer(&f, checkpoint, 0, REPLY_A, rom, NULL);
		assert(retro_control_request(work, getpid(), &other_session, checkpoint, 0, 300, &refused) ==
		       RETRO_POWER_FAILED && !refused.token[0]);
		assert(kill(peer, SIGKILL) == 0 && waitpid(peer, NULL, 0) == peer);
		fixture_unlink(&f, "native-session");
		fixture_unlink(&f, SOCKET_NAME);
	}
	check_exchange(&ops, rom, checkpoint, REPLY_A, rom, NULL, RECORD(TOKEN_A), 1);
	EXPECT_REPORT(RETRO_POWER_OK, RETRO_STAGE_OK, RETRO_STAGE_SKIPPED);
	expect_owner(&f, SESSION_ID);
	const enum peer_reply failures[] = { REPLY_OLD, REPLY_FAIL, REPLY_BAD_HEX, REPLY_SHORT,
					    REPLY_EXTRA, REPLY_WRONG_TYPE, REPLY_SHORT_HEADER, REPLY_CLOSED,
					    REPLY_BAD_UNSUPPORTED };
	for (size_t i = 0; i < sizeof failures / sizeof *failures; ++i) {
		check_exchange(&ops, rom, checkpoint, failures[i], rom, RECORD(TOKEN_A), RECORD(TOKEN_A), 0);
		assert(report.publication == RETRO_STAGE_SKIPPED && report.cleanup == RETRO_STAGE_SKIPPED);
	}
	/* An OK reply with no digest is the runtime's deliberate battery-only commit: the record this session
	 * published is removed. A session of another ROM cannot remove it, and neither can one that does not own it. */
	check_exchange(&ops, rom, checkpoint, REPLY_EMPTY, OTHER_ROM, RECORD(TOKEN_A), RECORD(TOKEN_A), 0);
	fixture_file(&f, "resume-owner", "banana-resume-owner-v1\n-\n" SESSION_B " N " TOKEN_A " " SYSTEM_ID " " CORE_ID " "
		     ROM_PATH "\n", sizeof("banana-resume-owner-v1\n-\n" SESSION_B " N " TOKEN_A " " SYSTEM_ID " " CORE_ID " "
		     ROM_PATH "\n") - 1);
	check_exchange(&ops, rom, checkpoint, REPLY_EMPTY, rom, RECORD(TOKEN_A), RECORD(TOKEN_A),
		       RETRO_CHECKPOINT_BATTERY_ONLY); /* same ROM, owned by another session */
	fixture_unlink(&f, "resume-owner");
	check_exchange(&ops, rom, checkpoint, REPLY_EMPTY, rom, RECORD(TOKEN_A), RECORD(TOKEN_A),
		       RETRO_CHECKPOINT_BATTERY_ONLY); /* unowned */
	struct retro_resume_proof proof_a = { .system = SYSTEM_ID, .core = CORE_ID, .token = TOKEN_A };
	assert(publish(&f, &owner, RETRO_PUBLISH_LIVE, &proof_a));
	check_exchange(&ops, rom, checkpoint, REPLY_EMPTY, rom, RECORD(TOKEN_A), NULL, RETRO_CHECKPOINT_BATTERY_ONLY);
	EXPECT_REPORT(RETRO_POWER_OK, RETRO_STAGE_SKIPPED, RETRO_STAGE_OK);
	assert(publish(&f, &owner, RETRO_PUBLISH_LIVE, &proof_a));
	/* A publisher that cannot take the record lock refuses rather than racing a settlement that may be
	 * reading the record it would replace. */
	int held = retro_resume_lock(work);
	assert(held >= 0);
	assert(!publish(&f, &owner, RETRO_PUBLISH_LIVE, &proof_a));
	assert(!invalidate(&f, &owner, RETRO_PUBLISH_LIVE));
	expect_record(&f, RECORD(TOKEN_A));
	close(held);
	assert(publish(&f, &owner, RETRO_PUBLISH_LIVE, &proof_a));
	expect_record(&f, RECORD(TOKEN_A));
	check_exchange(&ops, rom, checkpoint, REPLY_B, OTHER_ROM, RECORD(TOKEN_A), RECORD(TOKEN_A), 0);
	fixture_path(&f, path, "resume.tmp");
	assert(mkdir(path, 0700) == 0);
	check_exchange(&ops, rom, checkpoint, REPLY_B, rom, RECORD(TOKEN_A), RECORD(TOKEN_A), 0);
	EXPECT_REPORT(RETRO_POWER_OK, RETRO_STAGE_FAILED, RETRO_STAGE_SKIPPED);
	assert(rmdir(path) == 0);

	struct retro_resume_proof proof = { .system = SYSTEM_ID, .core = CORE_ID, .token = TOKEN_B };
	struct retro_owner bad_owner = owner;
	strcpy(bad_owner.rom, "/bad\nrom");
	assert(!publish(&f, &bad_owner, RETRO_PUBLISH_LIVE, &proof));
	strcpy(bad_owner.rom, "/bad\rrom");
	assert(!publish(&f, &bad_owner, RETRO_PUBLISH_LIVE, &proof));
	strcpy(bad_owner.rom, "relative");
	assert(!publish(&f, &bad_owner, RETRO_PUBLISH_LIVE, &proof));
	bad_owner = owner;
	strcpy(bad_owner.session, "NOT-A-SESSION");
	assert(!publish(&f, &bad_owner, RETRO_PUBLISH_LIVE, &proof));
	assert(!publish(&f, NULL, RETRO_PUBLISH_LIVE, &proof));
	struct retro_resume_proof malformed = proof;
	strcpy(malformed.core, "../mgba");
	assert(!publish(&f, &owner, RETRO_PUBLISH_LIVE, &malformed));
	malformed = proof;
	strcpy(malformed.system, "gba:gba");
	assert(!publish(&f, &owner, RETRO_PUBLISH_LIVE, &malformed));
	malformed = proof;
	malformed.token[0] = 'A';
	assert(!publish(&f, &owner, RETRO_PUBLISH_LIVE, &malformed));
	pid_t interrupted = fork();
	assert(interrupted >= 0);
	if (interrupted == 0) {
		interrupt_publish = 1;
		(void)publish(&f, &owner, RETRO_PUBLISH_LIVE, &proof);
		_exit(1);
	}
	expect_exit(interrupted, 73);
	expect_record(&f, RECORD(TOKEN_A));
	expect_owner(&f, SESSION_ID);
	assert(access(path, F_OK) == 0); /* after writing the temp, before rename */

	check_exchange(&ops, rom, shutdown, REPLY_B, rom, RECORD(TOKEN_A), RECORD(TOKEN_B), RETRO_POWER_OK);
	EXPECT_REPORT(RETRO_POWER_OK, RETRO_STAGE_OK, RETRO_STAGE_SKIPPED);
	assert(access(path, F_OK) < 0 && errno == ENOENT);
	/* Only a positively identified, bound battery-only reply (OK, no digest, to a request that asked for proof, from
	 * the launcher of this ROM) supersedes the record this session published. */
	check_exchange(&ops, rom, shutdown, REPLY_EMPTY, rom, RECORD(TOKEN_B), NULL, RETRO_POWER_OK);
	EXPECT_REPORT(RETRO_POWER_OK, RETRO_STAGE_SKIPPED, RETRO_STAGE_OK);
	set_shutdown(&f, 0);	/* a live publication is refused under the latch */
	assert(publish(&f, &owner, RETRO_PUBLISH_LIVE, &proof));
	/* An empty reply from another ROM's launcher, and a runtime that cannot answer the proof request and only
	 * succeeds on the compatibility retry, are ambiguous: neither authorizes deleting anything. */
	check_exchange(&ops, rom, shutdown, REPLY_EMPTY, OTHER_ROM, RECORD(TOKEN_B), RECORD(TOKEN_B), RETRO_POWER_OK);
	check_exchange(&ops, rom, shutdown, REPLY_OLD, rom, RECORD(TOKEN_B), RECORD(TOKEN_B), RETRO_POWER_OK);
	set_shutdown(&f, 0);
	assert(publish(&f, &owner, RETRO_PUBLISH_LIVE, &proof));
	set_shutdown(&f, 1);
	f.after = RECORD(TOKEN_B);
	assert(retro_shutdown_session(&ops, 0, 0, 0, NULL) == RETRO_POWER_NONE);
	expect_record(&f, RECORD(TOKEN_B));
	check_exchange(&ops, rom, shutdown, REPLY_FAIL, rom, RECORD(TOKEN_B), RECORD(TOKEN_B), RETRO_POWER_FAILED);
	EXPECT_REPORT(RETRO_POWER_FAILED, RETRO_STAGE_SKIPPED, RETRO_STAGE_SKIPPED);
	check_exchange(&ops, rom, shutdown, REPLY_A, OTHER_ROM, RECORD(TOKEN_B), RECORD(TOKEN_B), RETRO_POWER_OK);

	/* A shutdown that could not prove its session (no registration) publishes nothing and removes nothing. */
	set_shutdown(&f, 0);	/* a live publication is refused under the latch */
	assert(publish(&f, &owner, RETRO_PUBLISH_LIVE, &proof));
	set_shutdown(&f, 1);
	f.after = RECORD(TOKEN_B);
	assert(retro_shutdown_session(&ops, getpid(), getpgrp(), 31339, NULL) == RETRO_POWER_NONE);
	expect_record(&f, RECORD(TOKEN_B));

	/* A held sleep publishes the record for the checkpoint it acknowledged. */
	const uint16_t prepare = BANANA_RETRO_CTL_PREPARE_SLEEP;
	check_exchange(&ops, rom, prepare, REPLY_A, rom, RECORD(TOKEN_B), RECORD(TOKEN_A), RETRO_POWER_OK);
	check_exchange(&ops, rom, prepare, REPLY_B, OTHER_ROM, RECORD(TOKEN_A), RECORD(TOKEN_A), RETRO_POWER_OK);
	check_exchange(&ops, rom, prepare, REPLY_EMPTY, rom, RECORD(TOKEN_A), RECORD(TOKEN_A), RETRO_POWER_OK);
	fixture_path(&f, path, "resume.tmp");
	assert(mkdir(path, 0700) == 0);
	/* A publication that fails is not followed by a removal: the superseded record stays for boot to reject. */
	check_exchange(&ops, rom, prepare, REPLY_B, rom, RECORD(TOKEN_A), RECORD(TOKEN_A), RETRO_POWER_OK);
	assert(rmdir(path) == 0);

	/* The three outcomes are reported apart. A boot record that cannot be written leaves a committed checkpoint
	 * reported as committed, and finalization of a shutdown still runs. */
	fixture_path(&f, path, "resume.tmp");
	assert(mkdir(path, 0700) == 0);
	check_exchange(&ops, rom, shutdown, REPLY_B, rom, RECORD(TOKEN_A), RECORD(TOKEN_A), RETRO_POWER_OK);
	EXPECT_REPORT(RETRO_POWER_OK, RETRO_STAGE_FAILED, RETRO_STAGE_SKIPPED);
	assert(rmdir(path) == 0);
	/* A shutdown whose boot record was renamed into place but could not be synced reports the publication as
	 * failed, still finalizes, and leaves the candidate visible and owned. */
	fail_directory_sync = 1;
	check_exchange(&ops, rom, shutdown, REPLY_B, rom, RECORD(TOKEN_A), RECORD(TOKEN_B), RETRO_POWER_OK);
	fail_directory_sync = 0;
	EXPECT_REPORT(RETRO_POWER_OK, RETRO_STAGE_FAILED, RETRO_STAGE_SKIPPED);
	expect_owner(&f, SESSION_ID);
	/* A record that cannot be removed is a cleanup failure of a checkpoint that did commit. */
	set_shutdown(&f, 0);
	assert(publish(&f, &owner, RETRO_PUBLISH_LIVE, &proof_a));
	int cleanup_lock = retro_resume_lock(work);
	assert(cleanup_lock >= 0);
	check_exchange(&ops, rom, checkpoint, REPLY_EMPTY, rom, RECORD(TOKEN_A), RECORD(TOKEN_A), RETRO_CHECKPOINT_FAILED);
	EXPECT_REPORT(RETRO_POWER_OK, RETRO_STAGE_SKIPPED, RETRO_STAGE_FAILED);
	check_exchange(&ops, rom, shutdown, REPLY_EMPTY, rom, RECORD(TOKEN_A), RECORD(TOKEN_A), RETRO_POWER_OK);
	EXPECT_REPORT(RETRO_POWER_OK, RETRO_STAGE_SKIPPED, RETRO_STAGE_FAILED);
	close(cleanup_lock);

	static const char *const names[] = { "game.native", "game.pid", "game.session", "resume", "resume-owner",
					     "resume-owner.tmp", "resume.lock", "resume.tmp", "shutting-down" };
	for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
		fixture_path(&f, path, names[i]);
		(void)unlink(path);
	}
	assert(rmdir(work) == 0);
	return 0;
}
