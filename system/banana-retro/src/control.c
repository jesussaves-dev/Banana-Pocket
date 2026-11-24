#define _GNU_SOURCE
#include "control.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <unistd.h>

struct control_client {
	int fd;
	pid_t pid;
	bool authenticated;
	uint64_t last_request_id;
	unsigned char cached[BANANA_RETRO_CONTROL_MAX_PACKET];
	size_t cached_size;
};

struct control_server {
	int listener;
	struct control_client clients[2];
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	unsigned char session_id[16];
	uid_t peer_uid;
	pid_t ui_pid;
	pid_t sleep_pid;
	dev_t socket_dev;
	ino_t socket_ino;
	bool failed_handshake;
	uint64_t last_request_id;
	uint64_t last_mutation_id;
	uint16_t last_mutation_type;
	uint16_t last_mutation_result;
	struct control_ops ops;
};

static uint16_t get16(const unsigned char *p)
{ return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8); }
static uint32_t get32(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const unsigned char *p)
{ return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }
static void put16(unsigned char *p, uint16_t v)
{ p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (i * 8)); }
static void put64(unsigned char *p, uint64_t v)
{ put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static bool parse_number(const char *text, uint64_t max, uint64_t *value)
{
	if (!text || !*text) return false;
	uint64_t n = 0;
	for (; *text; ++text) {
		if (*text < '0' || *text > '9') return false;
		unsigned digit = (unsigned)(*text - '0');
		if (n > (max - digit) / 10) return false;
		n = n * 10 + digit;
	}
	*value = n;
	return true;
}

static bool append_bytes(unsigned char *payload, size_t *used,
			 const void *data, size_t size)
{
	if (size > BANANA_RETRO_CONTROL_MAX_PAYLOAD - *used) return false;
	if (size) memcpy(payload + *used, data, size);
	*used += size;
	return true;
}

static bool append_string(unsigned char *payload, size_t *used, const char *text)
{
	if (!text) text = "";
	size_t n = strlen(text);
	if (*used > BANANA_RETRO_CONTROL_MAX_PAYLOAD - 4 || n > UINT16_MAX ||
	    n > BANANA_RETRO_CONTROL_MAX_PAYLOAD - 4 - *used - 2) return false;
	put16(payload + *used, (uint16_t)n);
	*used += 2;
	return append_bytes(payload, used, text, n);
}

static void send_result(struct control_server *s, struct control_client *c,
			uint16_t type, uint64_t id, const unsigned char *body,
			size_t body_size, bool cache)
{
	unsigned char packet[BANANA_RETRO_CONTROL_MAX_PACKET];
	if (body_size > BANANA_RETRO_CONTROL_MAX_PAYLOAD - 2) return;
	memcpy(packet + BANANA_RETRO_CTL_MAGIC, BANANA_RETRO_CONTROL_MAGIC, 4);
	put16(packet + BANANA_RETRO_CTL_VERSION, BANANA_RETRO_CONTROL_VERSION);
	put16(packet + BANANA_RETRO_CTL_TYPE, (uint16_t)(type | 0x8000u));
	put64(packet + BANANA_RETRO_CTL_REQUEST_ID, id);
	memcpy(packet + BANANA_RETRO_CTL_SESSION_ID, s->session_id, sizeof(s->session_id));
	put16(packet + BANANA_RETRO_CTL_FLAGS, 0);
	put16(packet + BANANA_RETRO_CTL_LENGTH, (uint16_t)(body_size + 2));
	put16(packet + BANANA_RETRO_CONTROL_HEADER_SIZE, BANANA_RETRO_CTL_OK);
	if (body_size) memcpy(packet + BANANA_RETRO_CONTROL_HEADER_SIZE + 2, body, body_size);
	size_t packet_size = BANANA_RETRO_CONTROL_HEADER_SIZE + body_size + 2;
	if (cache) {
		memcpy(c->cached, packet, packet_size);
		c->cached_size = packet_size;
	}
	if (c->fd >= 0) {
		ssize_t n = send(c->fd, packet, packet_size, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (n != (ssize_t)packet_size) {
			close(c->fd);
			c->fd = -1;
			c->authenticated = false;
		}
	}
}

static void send_error(struct control_server *s, struct control_client *c,
		       uint16_t type, uint64_t id, uint16_t error)
{
	unsigned char packet[BANANA_RETRO_CONTROL_HEADER_SIZE + 2];
	memcpy(packet + BANANA_RETRO_CTL_MAGIC, BANANA_RETRO_CONTROL_MAGIC, 4);
	put16(packet + BANANA_RETRO_CTL_VERSION, BANANA_RETRO_CONTROL_VERSION);
	put16(packet + BANANA_RETRO_CTL_TYPE, (uint16_t)(type | 0x8000u));
	put64(packet + BANANA_RETRO_CTL_REQUEST_ID, id);
	memcpy(packet + BANANA_RETRO_CTL_SESSION_ID, s->session_id, 16);
	put16(packet + BANANA_RETRO_CTL_FLAGS, 0);
	put16(packet + BANANA_RETRO_CTL_LENGTH, 2);
	put16(packet + BANANA_RETRO_CONTROL_HEADER_SIZE, error);
	if (c->fd >= 0 && send(c->fd, packet, sizeof(packet), MSG_DONTWAIT | MSG_NOSIGNAL) !=
	    (ssize_t)sizeof(packet)) {
		close(c->fd);
		c->fd = -1;
		c->authenticated = false;
	}
}

static void close_client(struct control_client *c)
{
	if (c->fd >= 0) close(c->fd);
	memset(c, 0, sizeof(*c));
	c->fd = -1;
}

static bool open_client(struct control_server *s)
{
	int fd = accept4(s->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (fd < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
	struct control_client *c = NULL;
	for (size_t i = 0; i < sizeof(s->clients) / sizeof(s->clients[0]); ++i)
		if (s->clients[i].fd < 0) { c = &s->clients[i]; break; }
	if (!c) {
		close(fd);
		return true;
	}
	struct ucred cred;
	socklen_t size = sizeof(cred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) < 0 || size != sizeof(cred) ||
	    cred.uid != s->peer_uid || (cred.pid != s->ui_pid && cred.pid != s->sleep_pid)) {
		close(fd);
		return true;
	}
	c->pid = cred.pid;
	c->fd = fd;
	return true;
}

static bool encode_options(struct control_server *s, const unsigned char *request,
			   size_t request_size, unsigned char *body, size_t *body_size)
{
	if (request_size != 4) return false;
	uint16_t offset = get16(request), limit = get16(request + 2);
	if (limit == 0 || limit > 16) return false;
	size_t count = s->ops.option_count ? s->ops.option_count(s->ops.data) : 0;
	if (offset > count) return false;
	size_t used = 4;
	put16(body, (uint16_t)count);
	put16(body + 2, 0);
	uint16_t returned = 0;
	for (size_t i = offset; i < count && returned < limit; ++i) {
		const char *key, *description, *value, *def;
		const char *const *choices;
		size_t choice_count;
		bool visible, restart;
		if (!s->ops.option || !s->ops.option(s->ops.data, i, &key, &description,
				&value, &def, &choices, &choice_count, &visible, &restart))
			return false;
		unsigned char item[BANANA_RETRO_CONTROL_MAX_PAYLOAD];
		size_t item_used = 0;
		if (!append_string(item, &item_used, key) || !append_string(item, &item_used, description) ||
		    !append_string(item, &item_used, value) || !append_string(item, &item_used, def) ||
		    choice_count > UINT8_MAX || item_used > BANANA_RETRO_CONTROL_MAX_PAYLOAD - 4)
			return false;
		item[item_used++] = (unsigned char)choice_count;
		item[item_used++] = (unsigned char)((visible ? 1u : 0u) | (restart ? 2u : 0u));
		for (size_t j = 0; j < choice_count; ++j)
			if (!append_string(item, &item_used, choices[j])) return false;
		if (item_used > BANANA_RETRO_CONTROL_MAX_PAYLOAD - 2 - used) break;
		memcpy(body + used, item, item_used);
		used += item_used;
		++returned;
	}
	put16(body + 2, returned);
	*body_size = used;
	return true;
}

static uint16_t dispatch(struct control_server *s, struct control_client *c, uint16_t type,
			 const unsigned char *p, size_t n,
			 unsigned char *body, size_t *body_size)
{
	*body_size = 0;
	switch (type) {
	case BANANA_RETRO_CTL_HELLO:
		return n == 0 ? BANANA_RETRO_CTL_OK : BANANA_RETRO_CTL_INVALID;
	case BANANA_RETRO_CTL_GET_STATUS: {
		if (n != 0) return BANANA_RETRO_CTL_INVALID;
		struct control_status status = {0};
		if (s->ops.status) s->ops.status(s->ops.data, &status);
		put32(body, status.flags);
		put32(body + 4, status.pause_owners);
		put64(body + 8, s->last_request_id);
		put32(body + 16, status.capabilities);
		put32(body + 20, status.audio_state);
		put32(body + 24, status.audio_queued_frames);
		put64(body + 28, status.audio_underruns);
		put64(body + 36, 0); /* Operations complete synchronously at a safe point. */
		put64(body + 44, s->last_mutation_id);
		put16(body + 52, s->last_mutation_type);
		put16(body + 54, s->last_mutation_result);
		*body_size = 56;
		if (!append_string(body, body_size, status.core_id) ||
		    !append_string(body, body_size, status.core_version) ||
		    !append_string(body, body_size, status.content_id)) return BANANA_RETRO_CTL_IO_ERROR;
		return BANANA_RETRO_CTL_OK;
	}
	case BANANA_RETRO_CTL_PAUSE:
	case BANANA_RETRO_CTL_RESUME:
		if (n != 9 || (p[0] != BANANA_RETRO_CTL_OWNER_UI && p[0] != BANANA_RETRO_CTL_OWNER_SLEEP))
			return BANANA_RETRO_CTL_INVALID;
		if (c->pid != (p[0] == BANANA_RETRO_CTL_OWNER_UI ? s->ui_pid : s->sleep_pid))
			return BANANA_RETRO_CTL_STALE_SESSION;
		return s->ops.pause && s->ops.pause(s->ops.data, p[0], get64(p + 1),
			type == BANANA_RETRO_CTL_PAUSE) ? BANANA_RETRO_CTL_OK : BANANA_RETRO_CTL_BAD_STATE;
	case BANANA_RETRO_CTL_FLUSH_SRAM:
		if (n != 0) return BANANA_RETRO_CTL_INVALID;
		return s->ops.flush && s->ops.flush(s->ops.data) ? BANANA_RETRO_CTL_OK : BANANA_RETRO_CTL_IO_ERROR;
	case BANANA_RETRO_CTL_SAVE_STATE:
	case BANANA_RETRO_CTL_LOAD_STATE:
		if (n != 4 || get32(p) >= 10) return BANANA_RETRO_CTL_INVALID;
		if (type == BANANA_RETRO_CTL_SAVE_STATE)
			return s->ops.save_state && s->ops.save_state(s->ops.data, get32(p)) ?
				BANANA_RETRO_CTL_OK : BANANA_RETRO_CTL_IO_ERROR;
		return s->ops.load_state && s->ops.load_state(s->ops.data, get32(p)) ?
			BANANA_RETRO_CTL_OK : BANANA_RETRO_CTL_CORE_ERROR;
	case BANANA_RETRO_CTL_GET_OPTIONS:
		return encode_options(s, p, n, body, body_size) ? BANANA_RETRO_CTL_OK : BANANA_RETRO_CTL_INVALID;
	case BANANA_RETRO_CTL_SET_OPTION: {
		if (n < 5) return BANANA_RETRO_CTL_INVALID;
		uint16_t kl = get16(p), vl = get16(p + 2);
		unsigned scope = p[4];
		if (kl == 0 || vl == 0 || (size_t)kl + vl + 5 != n ||
		    scope > BANANA_RETRO_CTL_SCOPE_GAME || memchr(p + 5, 0, (size_t)kl + vl))
			return BANANA_RETRO_CTL_INVALID;
		char key[256], value[256];
		if (kl >= sizeof(key) || vl >= sizeof(value)) return BANANA_RETRO_CTL_INVALID;
		memcpy(key, p + 5, kl); key[kl] = 0;
		memcpy(value, p + 5 + kl, vl); value[vl] = 0;
		bool restart = false;
		if (!s->ops.set_option || !s->ops.set_option(s->ops.data, key, value, scope, &restart))
			return scope == BANANA_RETRO_CTL_SCOPE_SESSION ? BANANA_RETRO_CTL_INVALID : BANANA_RETRO_CTL_UNSUPPORTED;
		body[0] = restart ? 1u : 0u;
		*body_size = 1;
		return BANANA_RETRO_CTL_OK;
	}
	case BANANA_RETRO_CTL_RESET:
		if (n != 0) return BANANA_RETRO_CTL_INVALID;
		return s->ops.reset && s->ops.reset(s->ops.data) ? BANANA_RETRO_CTL_OK : BANANA_RETRO_CTL_CORE_ERROR;
	case BANANA_RETRO_CTL_QUIT:
		if (n != 0) return BANANA_RETRO_CTL_INVALID;
		return s->ops.quit && s->ops.quit(s->ops.data) ? BANANA_RETRO_CTL_OK : BANANA_RETRO_CTL_IO_ERROR;
	default:
		return BANANA_RETRO_CTL_UNSUPPORTED;
	}
}

static void handle_packet(struct control_server *s, struct control_client *c)
{
	unsigned char packet[BANANA_RETRO_CONTROL_MAX_PACKET];
	struct iovec iov = { .iov_base = packet, .iov_len = sizeof(packet) };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
	ssize_t n = recvmsg(c->fd, &msg, MSG_DONTWAIT);
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
	if (n <= 0) { close_client(c); return; }
	if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || (size_t)n < BANANA_RETRO_CONTROL_HEADER_SIZE ||
	    memcmp(packet + BANANA_RETRO_CTL_MAGIC, BANANA_RETRO_CONTROL_MAGIC, 4) != 0 ||
	    get16(packet + BANANA_RETRO_CTL_VERSION) != BANANA_RETRO_CONTROL_VERSION ||
	    get16(packet + BANANA_RETRO_CTL_FLAGS) != 0 ||
	    memcmp(packet + BANANA_RETRO_CTL_SESSION_ID, s->session_id, 16) != 0) {
		if (!c->authenticated) s->failed_handshake = true;
		close_client(c);
		return;
	}
	uint16_t type = get16(packet + BANANA_RETRO_CTL_TYPE);
	uint64_t id = get64(packet + BANANA_RETRO_CTL_REQUEST_ID);
	uint16_t length = get16(packet + BANANA_RETRO_CTL_LENGTH);
	if ((size_t)n != BANANA_RETRO_CONTROL_HEADER_SIZE + length ||
	    length > BANANA_RETRO_CONTROL_MAX_PAYLOAD || id == 0 || type >= 0x8000u) {
		if (!c->authenticated) s->failed_handshake = true;
		close_client(c);
		return;
	}
	if (!c->authenticated) {
		if (type != BANANA_RETRO_CTL_HELLO || length != 0) {
			s->failed_handshake = true;
			close_client(c);
			return;
		}
		c->authenticated = true;
	} else if (id == c->last_request_id && c->cached_size) {
		if (send(c->fd, c->cached, c->cached_size, MSG_DONTWAIT | MSG_NOSIGNAL) !=
		    (ssize_t)c->cached_size) close_client(c);
		return;
	} else if (id < s->last_request_id) {
		send_error(s, c, type, id, BANANA_RETRO_CTL_STALE_SESSION);
		return;
	} else if (type == BANANA_RETRO_CTL_HELLO) {
		send_error(s, c, type, id, BANANA_RETRO_CTL_INVALID);
		return;
	}
	if (id <= s->last_request_id) {
		send_error(s, c, type, id, BANANA_RETRO_CTL_STALE_SESSION);
		return;
	}
	unsigned char body[BANANA_RETRO_CONTROL_MAX_PAYLOAD] = {0};
	size_t body_size = 0;
	uint16_t result = dispatch(s, c, type, packet + BANANA_RETRO_CONTROL_HEADER_SIZE,
				   length, body, &body_size);
	s->last_request_id = id;
	c->last_request_id = id;
	if (type == BANANA_RETRO_CTL_PAUSE || type == BANANA_RETRO_CTL_RESUME ||
	    type == BANANA_RETRO_CTL_FLUSH_SRAM || type == BANANA_RETRO_CTL_SAVE_STATE ||
	    type == BANANA_RETRO_CTL_LOAD_STATE || type == BANANA_RETRO_CTL_SET_OPTION ||
	    type == BANANA_RETRO_CTL_RESET || type == BANANA_RETRO_CTL_QUIT) {
		s->last_mutation_id = id;
		s->last_mutation_type = type;
		s->last_mutation_result = result;
	}
	if (result != BANANA_RETRO_CTL_OK) {
		unsigned char response[BANANA_RETRO_CONTROL_MAX_PACKET];
		memcpy(response + BANANA_RETRO_CTL_MAGIC, BANANA_RETRO_CONTROL_MAGIC, 4);
		put16(response + BANANA_RETRO_CTL_VERSION, BANANA_RETRO_CONTROL_VERSION);
		put16(response + BANANA_RETRO_CTL_TYPE, (uint16_t)(type | 0x8000u));
		put64(response + BANANA_RETRO_CTL_REQUEST_ID, id);
		memcpy(response + BANANA_RETRO_CTL_SESSION_ID, s->session_id, 16);
		put16(response + BANANA_RETRO_CTL_FLAGS, 0);
		put16(response + BANANA_RETRO_CTL_LENGTH, 2);
		put16(response + BANANA_RETRO_CONTROL_HEADER_SIZE, result);
		memcpy(c->cached, response, BANANA_RETRO_CONTROL_HEADER_SIZE + 2);
		c->cached_size = BANANA_RETRO_CONTROL_HEADER_SIZE + 2;
		if (send(c->fd, response, c->cached_size, MSG_DONTWAIT | MSG_NOSIGNAL) !=
		    (ssize_t)c->cached_size) close_client(c);
		return;
	}
	send_result(s, c, type, id, body, body_size, true);
}

struct control_server *control_open_from_env(const struct control_ops *ops,
						     const unsigned char session_id[16])
{
	const char *path = getenv("BANANA_RETRO_CONTROL_SOCKET");
	const char *pid_text = getenv("BANANA_RETRO_CONTROL_UI_PID");
	if (!pid_text) pid_text = getenv("BANANA_RETRO_CONTROL_PEER_PID");
	const char *sleep_text = getenv("BANANA_RETRO_CONTROL_SLEEP_PID");
	if (!path || !*path) { errno = ENOENT; return NULL; }
	if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path) || !ops || !session_id) {
		errno = EINVAL; return NULL;
	}
	uint64_t pid = (uint64_t)getppid();
	if (pid_text && !parse_number(pid_text, INT_MAX, &pid)) { errno = EINVAL; return NULL; }
	if (pid == 0) { errno = EINVAL; return NULL; }
	struct control_server *s = calloc(1, sizeof(*s));
	if (!s) return NULL;
	s->listener = -1;
	for (size_t i = 0; i < sizeof(s->clients) / sizeof(s->clients[0]); ++i)
		s->clients[i].fd = -1;
	s->peer_uid = geteuid();
	s->ui_pid = (pid_t)pid;
	s->sleep_pid = s->ui_pid;
	if (sleep_text) {
		uint64_t sleep_pid;
		if (!parse_number(sleep_text, INT_MAX, &sleep_pid) || sleep_pid == 0) {
			free(s); errno = EINVAL; return NULL;
		}
		s->sleep_pid = (pid_t)sleep_pid;
	}
	memcpy(s->path, path, strlen(path) + 1);
	memcpy(s->session_id, session_id, 16);
	s->ops = *ops;
	struct stat st;
	if (lstat(path, &st) == 0) {
		if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid() || unlink(path) < 0) goto fail;
	} else if (errno != ENOENT) goto fail;
	s->listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (s->listener < 0) goto fail;
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	memcpy(address.sun_path, path, strlen(path) + 1);
	mode_t old = umask(0077);
	int rc = bind(s->listener, (struct sockaddr *)&address, sizeof(address));
	umask(old);
	if (rc < 0) goto fail;
	if (lstat(path, &st) < 0) goto fail;
	s->socket_dev = st.st_dev;
	s->socket_ino = st.st_ino;
	if (chmod(path, 0600) < 0 || listen(s->listener, 2) < 0) goto fail;
	return s;
fail:
	{
		int saved = errno;
		control_close(s);
		errno = saved;
		return NULL;
	}
}

void control_close(struct control_server *s)
{
	if (!s) return;
	for (size_t i = 0; i < sizeof(s->clients) / sizeof(s->clients[0]); ++i)
		close_client(&s->clients[i]);
	if (s->listener >= 0) close(s->listener);
	if (s->path[0] && s->socket_ino) {
		struct stat st;
		if (lstat(s->path, &st) == 0 && S_ISSOCK(st.st_mode) &&
		    st.st_dev == s->socket_dev && st.st_ino == s->socket_ino)
			(void)unlink(s->path);
	}
	free(s);
}

int control_listener_fd(const struct control_server *s) { return s ? s->listener : -1; }
int control_client_fd(const struct control_server *s, unsigned slot)
{ return s && slot < 2 ? s->clients[slot].fd : -1; }
bool control_is_authenticated(const struct control_server *s)
{ return s && (s->clients[0].authenticated || s->clients[1].authenticated); }
bool control_has_pause(const struct control_server *s)
{
	struct control_status status = {0};
	if (s && s->ops.status) s->ops.status(s->ops.data, &status);
	return status.pause_owners != 0;
}
void control_handle_ready(struct control_server *s, bool listener_ready, unsigned client_ready_mask)
{
	if (!s || s->failed_handshake) return;
	if (listener_ready && !open_client(s)) return;
	for (unsigned i = 0; i < 2; ++i)
		if ((client_ready_mask & (1u << i)) && s->clients[i].fd >= 0) {
			handle_packet(s, &s->clients[i]);
			if (s->failed_handshake) break;
		}
}
