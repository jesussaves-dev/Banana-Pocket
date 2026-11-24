#define _GNU_SOURCE
#include "control.h"
#include "options.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <unistd.h>

#define main banana_retro_embedded_main
#include "../src/main.c"
#undef main

struct fake {
	unsigned pauses;
	uint64_t sleep_epoch;
	unsigned flushes, saves, loads, resets, quits, option_sets;
	unsigned last_slot;
	bool changed;
	struct native_runtime *runtime;
};

static void status(void *data, struct control_status *out)
{
	struct fake *f = data;
	out->flags = 1u | (f->pauses ? 2u : 0u);
	out->pause_owners = f->pauses;
}
static bool pause_hold(void *data, unsigned owner, uint64_t epoch, bool acquire)
{
	struct fake *f = data;
	unsigned bit = owner == BANANA_RETRO_CTL_OWNER_UI ? 1u : 2u;
	if (owner == BANANA_RETRO_CTL_OWNER_SLEEP) {
		if (acquire) {
			if (!epoch || (f->pauses & bit && epoch != f->sleep_epoch) ||
			    (!(f->pauses & bit) && epoch <= f->sleep_epoch)) return false;
			f->sleep_epoch = epoch;
		} else if (!(f->pauses & bit) || epoch != f->sleep_epoch) return false;
	}
	if (f->runtime && owner == BANANA_RETRO_CTL_OWNER_UI)
		f->runtime->ui_paused = acquire;
	if (acquire) f->pauses |= bit; else f->pauses &= ~bit;
	return true;
}
static bool flush(void *data) { ++((struct fake *)data)->flushes; return true; }
static bool save_state(void *data, unsigned slot)
{ struct fake *f = data; ++f->saves; f->last_slot = slot; return true; }
static bool load_state(void *data, unsigned slot)
{ struct fake *f = data; ++f->loads; f->last_slot = slot; return true; }
static size_t option_count(void *data) { (void)data; return options_count(); }
static bool option(void *data, size_t index, const char **key, const char **description,
		   const char **value, const char **def, const char *const **choices,
		   size_t *choice_count, bool *visible, bool *restart)
{
	(void)data;
	const struct core_option *item = options_at(index);
	if (!item) return false;
	static const char *ptrs[OPTIONS_MAX_VALUES];
	for (size_t i = 0; i < item->choice_count; ++i) ptrs[i] = item->choices[i];
	*key = item->key; *description = item->description; *value = item->value;
	*def = item->default_value; *choices = ptrs; *choice_count = item->choice_count;
	*visible = item->visible; *restart = item->restart_required;
	return true;
}
static bool set_option(void *data, const char *key, const char *value, unsigned scope, bool *restart)
{
	struct fake *f = data;
	if (scope != BANANA_RETRO_CTL_SCOPE_SESSION) return false;
	const char *old = options_get(key);
	bool changed = old && strcmp(old, value) != 0;
	if (!options_set(key, value)) return false;
	*restart = changed;
	++f->option_sets;
	return true;
}
static bool reset(void *data) { ++((struct fake *)data)->resets; return true; }
static bool quit(void *data) { ++((struct fake *)data)->quits; return true; }

static void put16(unsigned char *p, uint16_t v)
{ p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static void put64(unsigned char *p, uint64_t v)
{ put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t get16(const unsigned char *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint64_t get64(const unsigned char *p) { return (uint64_t)get16(p) | (uint64_t)get16(p+2)<<16 | (uint64_t)get16(p+4)<<32 | (uint64_t)get16(p+6)<<48; }

static size_t make_packet(unsigned char *packet, uint16_t type, uint64_t id,
			  const unsigned char session[16], const void *payload, size_t size)
{
	memcpy(packet, BANANA_RETRO_CONTROL_MAGIC, 4);
	put16(packet + 4, BANANA_RETRO_CONTROL_VERSION);
	put16(packet + 6, type); put64(packet + 8, id);
	memcpy(packet + 16, session, 16); put16(packet + 32, 0); put16(packet + 34, (uint16_t)size);
	if (size) memcpy(packet + BANANA_RETRO_CONTROL_HEADER_SIZE, payload, size);
	return BANANA_RETRO_CONTROL_HEADER_SIZE + size;
}

static uint16_t exchange_slot(int fd, struct control_server *server, unsigned slot,
			      const unsigned char *request, size_t request_size,
			      unsigned char *reply, size_t capacity)
{
	assert(send(fd, request, request_size, 0) == (ssize_t)request_size);
	control_handle_ready(server, false, 1u << slot);
	ssize_t n = recv(fd, reply, capacity, 0);
	assert(n >= (ssize_t)(BANANA_RETRO_CONTROL_HEADER_SIZE + 2));
	assert(memcmp(reply, BANANA_RETRO_CONTROL_MAGIC, 4) == 0);
	assert(get16(reply + 4) == BANANA_RETRO_CONTROL_VERSION);
	assert((get16(reply + 6) & 0x8000u) != 0);
	assert(get16(reply + 34) == (uint16_t)(n - BANANA_RETRO_CONTROL_HEADER_SIZE));
	return get16(reply + BANANA_RETRO_CONTROL_HEADER_SIZE);
}

static uint16_t exchange(int fd, struct control_server *server,
			 const unsigned char *request, size_t request_size,
			 unsigned char *reply, size_t capacity)
{
	return exchange_slot(fd, server, 0, request, request_size, reply, capacity);
}

static int connect_client(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	assert(fd >= 0);
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	assert(strlen(path) < sizeof(address.sun_path));
	strcpy(address.sun_path, path);
	assert(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
	return fd;
}

struct pause_wayland {
	struct wl_display *server;
	struct native_runtime runtime;
	struct control_server *control;
	struct fake fake;
	int control_fd;
	pthread_t server_thread;
	pthread_t wait_thread;
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	atomic_bool stopping;
	bool sync_seen;
	bool release_seen;
	bool sync_while_paused;
	bool release_while_paused;
	int wait_result;
};

static void server_buffer_destroy(struct wl_client *client, struct wl_resource *resource)
{ (void)client; wl_resource_destroy(resource); }
static const struct wl_buffer_interface server_buffer_impl = { .destroy = server_buffer_destroy };

static void server_pool_create_buffer(struct wl_client *client, struct wl_resource *pool,
				     uint32_t id, int32_t offset, int32_t width,
				     int32_t height, int32_t stride, uint32_t format)
{
	(void)pool; (void)offset; (void)width; (void)height; (void)stride; (void)format;
	struct wl_resource *buffer = wl_resource_create(client, &wl_buffer_interface, 1, id);
	assert(buffer);
	wl_resource_set_implementation(buffer, &server_buffer_impl, NULL, NULL);
	wl_buffer_send_release(buffer);
}
static void server_pool_destroy(struct wl_client *client, struct wl_resource *resource)
{ (void)client; wl_resource_destroy(resource); }
static void server_pool_resize(struct wl_client *client, struct wl_resource *resource, int32_t size)
{ (void)client; (void)resource; (void)size; }
static const struct wl_shm_pool_interface server_pool_impl = {
	.create_buffer = server_pool_create_buffer,
	.destroy = server_pool_destroy,
	.resize = server_pool_resize
};

static void server_shm_create_pool(struct wl_client *client, struct wl_resource *shm,
				   uint32_t id, int32_t fd, int32_t size)
{
	(void)shm; (void)size;
	close(fd);
	struct wl_resource *pool = wl_resource_create(client, &wl_shm_pool_interface, 1, id);
	assert(pool);
	wl_resource_set_implementation(pool, &server_pool_impl, NULL, NULL);
}
static void server_shm_release(struct wl_client *client, struct wl_resource *resource)
{ (void)client; (void)resource; }
static const struct wl_shm_interface server_shm_impl = {
	.create_pool = server_shm_create_pool,
	.release = server_shm_release
};

static void server_bind_shm(struct wl_client *client, void *data,
			    uint32_t version, uint32_t id)
{
	(void)data;
	struct wl_resource *resource = wl_resource_create(client, &wl_shm_interface,
			version < 1 ? version : 1, id);
	assert(resource);
	wl_resource_set_implementation(resource, &server_shm_impl, NULL, NULL);
	wl_shm_send_format(resource, WL_SHM_FORMAT_XRGB8888);
}

static void *wayland_server_thread(void *data)
{
	struct pause_wayland *test = data;
	struct wl_event_loop *loop = wl_display_get_event_loop(test->server);
	while (!atomic_load_explicit(&test->stopping, memory_order_acquire)) {
		(void)wl_event_loop_dispatch(loop, 10);
		wl_display_flush_clients(test->server);
	}
	return NULL;
}

static void sync_done(void *data, struct wl_callback *callback, uint32_t serial)
{
	(void)serial;
	struct pause_wayland *test = data;
	pthread_mutex_lock(&test->mutex);
	test->sync_seen = true;
	test->sync_while_paused = test->runtime.ui_paused;
	pthread_cond_broadcast(&test->condition);
	pthread_mutex_unlock(&test->mutex);
	wl_callback_destroy(callback);
}
static const struct wl_callback_listener sync_listener = { .done = sync_done };

static void client_buffer_release(void *data, struct wl_buffer *buffer)
{
	struct pause_wayland *test = data;
	pthread_mutex_lock(&test->mutex);
	test->release_seen = true;
	test->release_while_paused = test->runtime.ui_paused;
	pthread_cond_broadcast(&test->condition);
	pthread_mutex_unlock(&test->mutex);
	(void)buffer;
}
static const struct wl_buffer_listener client_buffer_listener = { .release = client_buffer_release };

struct registry_capture { struct wl_shm *shm; };
static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
			    const char *interface, uint32_t version)
{
	struct registry_capture *capture = data;
	if (strcmp(interface, "wl_shm") == 0)
		capture->shm = wl_registry_bind(registry, name, &wl_shm_interface,
				version < 1 ? version : 1);
}
static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{ (void)data; (void)registry; (void)name; }
static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove
};

static void *wait_thread(void *data)
{
	struct pause_wayland *test = data;
	test->wait_result = wait_until(&test->runtime, monotonic_ns() + 1000000000ull);
	return NULL;
}

static void test_wayland_while_paused(void)
{
	char directory[] = "/tmp/banana-retro-paused-wayland.XXXXXX";
	assert(mkdtemp(directory));
	assert(setenv("XDG_RUNTIME_DIR", directory, 1) == 0);
	struct pause_wayland test = {0};
	test.control_fd = -1;
	assert(pthread_mutex_init(&test.mutex, NULL) == 0);
	assert(pthread_cond_init(&test.condition, NULL) == 0);
	atomic_init(&test.stopping, false);
	test.server = wl_display_create(); assert(test.server);
	assert(wl_global_create(test.server, &wl_shm_interface, 1, NULL, server_bind_shm));
	const char *socket_name = wl_display_add_socket_auto(test.server); assert(socket_name);
	assert(setenv("WAYLAND_DISPLAY", socket_name, 1) == 0);
	assert(pthread_create(&test.server_thread, NULL, wayland_server_thread, &test) == 0);
	native_display = __wrap_wl_display_connect(NULL); assert(native_display);
	struct wl_registry *registry = wl_display_get_registry(native_display); assert(registry);
	struct registry_capture capture = {0};
	assert(wl_registry_add_listener(registry, &registry_listener, &capture) == 0);
	assert(wl_display_roundtrip(native_display) >= 0 && capture.shm);
	int memory_fd = memfd_create("banana-retro-paused-release", MFD_CLOEXEC);
	assert(memory_fd >= 0 && ftruncate(memory_fd, 4096) == 0);
	struct wl_shm_pool *pool = wl_shm_create_pool(capture.shm, memory_fd, 4096);
	assert(pool); close(memory_fd);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4,
							    WL_SHM_FORMAT_XRGB8888);
	assert(buffer && wl_buffer_add_listener(buffer, &client_buffer_listener, &test) == 0);
	wl_shm_pool_destroy(pool);
	struct wl_callback *sync = wl_display_sync(native_display); assert(sync);
	assert(wl_callback_add_listener(sync, &sync_listener, &test) == 0);
	assert(wl_display_flush(native_display) >= 0);
	test.runtime.ui_paused = true;
	test.runtime.signal_fd = -1;
	test.fake.pauses = 1;
	test.fake.runtime = &test.runtime;
	struct control_ops ops = { .data=&test.fake, .status=status, .pause=pause_hold };
	unsigned char session[16]; memset(session, 0x5a, sizeof(session));
	char control_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	assert(snprintf(control_path, sizeof(control_path), "%s/control.sock", directory) > 0);
	assert(setenv("BANANA_RETRO_CONTROL_SOCKET", control_path, 1) == 0);
	char pid_text[32]; assert(snprintf(pid_text, sizeof(pid_text), "%ld", (long)getpid()) > 0);
	assert(setenv("BANANA_RETRO_CONTROL_PEER_PID", pid_text, 1) == 0);
	test.control = control_open_from_env(&ops, session); assert(test.control);
	test.runtime.control = test.control;
	test.control_fd = connect_client(control_path);
	control_handle_ready(test.control, true, false);
	unsigned char request[BANANA_RETRO_CONTROL_MAX_PACKET], reply[BANANA_RETRO_CONTROL_MAX_PACKET];
	size_t packet_size = make_packet(request, BANANA_RETRO_CTL_HELLO, 1, session, NULL, 0);
	assert(exchange(test.control_fd, test.control, request, packet_size, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(pthread_create(&test.wait_thread, NULL, wait_thread, &test) == 0);
	/* Both sync dispatch and wl_buffer.release must be serviced before pause is released. */
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 3;
	pthread_mutex_lock(&test.mutex);
	while (!test.sync_seen || !test.release_seen)
		assert(pthread_cond_timedwait(&test.condition, &test.mutex, &deadline) == 0);
	assert(test.sync_while_paused && test.release_while_paused);
	pthread_mutex_unlock(&test.mutex);
	unsigned char hold[9] = { BANANA_RETRO_CTL_OWNER_UI };
	put64(hold + 1, 0);
	packet_size = make_packet(request, BANANA_RETRO_CTL_RESUME, 2, session, hold, sizeof(hold));
	assert(send(test.control_fd, request, packet_size, 0) == (ssize_t)packet_size);
	assert(recv(test.control_fd, reply, sizeof(reply), 0) ==
	       BANANA_RETRO_CONTROL_HEADER_SIZE + (ssize_t)sizeof(uint16_t));
	assert(get16(reply + BANANA_RETRO_CONTROL_HEADER_SIZE) == BANANA_RETRO_CTL_OK);
	assert(pthread_join(test.wait_thread, NULL) == 0 && test.wait_result == 1);
	atomic_store_explicit(&test.stopping, true, memory_order_release);
	assert(pthread_join(test.server_thread, NULL) == 0);
	close(test.control_fd);
	control_close(test.control);
	wl_buffer_destroy(buffer);
	wl_shm_destroy(capture.shm);
	wl_registry_destroy(registry);
	wl_display_disconnect(native_display);
	native_display = NULL;
	wl_display_destroy_clients(test.server);
	wl_display_destroy(test.server);
	assert(pthread_cond_destroy(&test.condition) == 0);
	assert(pthread_mutex_destroy(&test.mutex) == 0);
	assert(rmdir(directory) == 0);
}

static void test_two_clients(void)
{
	char directory[] = "/tmp/banana-retro-control-clients.XXXXXX";
	assert(mkdtemp(directory));
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	assert(snprintf(path, sizeof(path), "%s/control.sock", directory) > 0);
	assert(setenv("BANANA_RETRO_CONTROL_SOCKET", path, 1) == 0);
	char pid_text[32]; assert(snprintf(pid_text, sizeof(pid_text), "%ld", (long)getpid()) > 0);
	assert(setenv("BANANA_RETRO_CONTROL_PEER_PID", pid_text, 1) == 0);
	assert(unsetenv("BANANA_RETRO_CONTROL_SLEEP_PID") == 0);
	unsigned char session[16]; memset(session, 0x33, sizeof(session));
	struct fake fake = {0};
	struct control_ops ops = { .data=&fake, .status=status, .pause=pause_hold };
	struct control_server *server = control_open_from_env(&ops, session); assert(server);
	int ui = connect_client(path); control_handle_ready(server, true, 0);
	assert(control_client_fd(server, 0) >= 0);
	int sleep = connect_client(path); control_handle_ready(server, true, 0);
	assert(control_client_fd(server, 1) >= 0);
	unsigned char request[BANANA_RETRO_CONTROL_MAX_PACKET], reply[BANANA_RETRO_CONTROL_MAX_PACKET];
	size_t n = make_packet(request, BANANA_RETRO_CTL_HELLO, 1, session, NULL, 0);
	assert(exchange_slot(ui, server, 0, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	n = make_packet(request, BANANA_RETRO_CTL_HELLO, 2, session, NULL, 0);
	assert(exchange_slot(sleep, server, 1, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	unsigned char hold[9] = { BANANA_RETRO_CTL_OWNER_UI };
	n = make_packet(request, BANANA_RETRO_CTL_PAUSE, 3, session, hold, sizeof(hold));
	assert(exchange_slot(ui, server, 0, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	hold[0] = BANANA_RETRO_CTL_OWNER_SLEEP; put64(hold + 1, 41);
	n = make_packet(request, BANANA_RETRO_CTL_PAUSE, 4, session, hold, sizeof(hold));
	assert(exchange_slot(sleep, server, 1, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(fake.pauses == 3);
	hold[0] = BANANA_RETRO_CTL_OWNER_SLEEP;
	n = make_packet(request, BANANA_RETRO_CTL_RESUME, 5, session, hold, sizeof(hold));
	assert(exchange_slot(sleep, server, 1, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(fake.pauses == 1);
	hold[0] = BANANA_RETRO_CTL_OWNER_UI;
	n = make_packet(request, BANANA_RETRO_CTL_RESUME, 6, session, hold, sizeof(hold));
	assert(exchange_slot(ui, server, 0, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(fake.pauses == 0);
	close(ui); close(sleep);
	control_handle_ready(server, false, 3u);
	control_close(server);
	assert(rmdir(directory) == 0);
}

static void test_owner_identity(void)
{
	char directory[] = "/tmp/banana-retro-control-owner.XXXXXX";
	assert(mkdtemp(directory));
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	assert(snprintf(path, sizeof(path), "%s/control.sock", directory) > 0);
	assert(setenv("BANANA_RETRO_CONTROL_SOCKET", path, 1) == 0);
	char pid_text[32]; assert(snprintf(pid_text, sizeof(pid_text), "%ld", (long)getpid()) > 0);
	assert(setenv("BANANA_RETRO_CONTROL_UI_PID", pid_text, 1) == 0);
	assert(setenv("BANANA_RETRO_CONTROL_SLEEP_PID", "2147483647", 1) == 0);
	unsigned char session[16]; memset(session, 0x71, sizeof(session));
	struct fake fake = {0}; struct control_ops ops = { .data=&fake, .pause=pause_hold };
	struct control_server *server = control_open_from_env(&ops, session); assert(server);
	int fd = connect_client(path); control_handle_ready(server, true, 0);
	unsigned char request[BANANA_RETRO_CONTROL_MAX_PACKET], reply[BANANA_RETRO_CONTROL_MAX_PACKET];
	size_t n = make_packet(request, BANANA_RETRO_CTL_HELLO, 1, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	unsigned char hold[9] = { BANANA_RETRO_CTL_OWNER_SLEEP }; put64(hold + 1, 1);
	n = make_packet(request, BANANA_RETRO_CTL_PAUSE, 2, session, hold, sizeof(hold));
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_STALE_SESSION);
	assert(fake.pauses == 0);
	close(fd); control_handle_ready(server, false, 1u); control_close(server);
	assert(rmdir(directory) == 0);
	assert(unsetenv("BANANA_RETRO_CONTROL_UI_PID") == 0);
	assert(unsetenv("BANANA_RETRO_CONTROL_SLEEP_PID") == 0);
}

static void test_options(void)
{
	options_reset();
	struct retro_variable v0[] = { { "fixture_speed", "Speed; 1|2|3" }, { NULL, NULL } };
	assert(options_api_version() == 2 && options_register_v0(v0));
	assert(strcmp(options_get("fixture_speed"), "1") == 0);
	assert(options_set("fixture_speed", "2") && options_update_pending());
	options_clear_update();
	struct retro_variable updated_v0[] = { { "fixture_speed", "Speed; 1|3" }, { NULL, NULL } };
	assert(options_register_v0(updated_v0));
	assert(strcmp(options_get("fixture_speed"), "1") == 0);
	struct retro_core_option_definition v1[2] = {0};
	v1[0].key = "fixture_mode"; v1[0].desc = "Mode"; v1[0].default_value = "safe";
	v1[0].values[0].value = "safe"; v1[0].values[1].value = "fast";
	assert(options_register_v1(v1));
	assert(strcmp(options_get("fixture_mode"), "safe") == 0);
	struct retro_core_option_v2_definition def[2] = {0};
	def[0].key = "fixture_video"; def[0].desc = "Video"; def[0].default_value = "rgb";
	def[0].values[0].value = "rgb"; def[0].values[1].value = "xrgb";
	struct retro_core_options_v2 v2 = { .definitions = def };
	assert(options_register_v2(&v2));
	assert(options_count() == 3 && strcmp(options_get("fixture_video"), "rgb") == 0);
}

int main(void)
{
	test_options();
	test_wayland_while_paused();
	test_two_clients();
	test_owner_identity();
	char directory[] = "/tmp/banana-retro-control.XXXXXX";
	assert(mkdtemp(directory));
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	assert(snprintf(path, sizeof(path), "%s/control.sock", directory) > 0);
	assert(setenv("BANANA_RETRO_CONTROL_SOCKET", path, 1) == 0);
	char pid[32]; assert(snprintf(pid, sizeof(pid), "%ld", (long)getpid()) > 0);
	assert(setenv("BANANA_RETRO_CONTROL_PEER_PID", pid, 1) == 0);
	unsigned char session[16]; for (unsigned i = 0; i < sizeof(session); ++i) session[i] = (unsigned char)(i + 1);
	struct fake fake = {0};
	struct control_ops ops = { .data=&fake, .status=status, .pause=pause_hold,
		.flush=flush, .save_state=save_state, .load_state=load_state,
		.option_count=option_count, .option=option, .set_option=set_option,
		.reset=reset, .quit=quit };
	struct control_server *server = control_open_from_env(&ops, session);
	assert(server && control_listener_fd(server) >= 0);
	int fd = connect_client(path);
	control_handle_ready(server, true, false);
	unsigned char request[BANANA_RETRO_CONTROL_MAX_PACKET], reply[BANANA_RETRO_CONTROL_MAX_PACKET];
	size_t n = make_packet(request, BANANA_RETRO_CTL_HELLO, 1, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(control_is_authenticated(server));

	n = make_packet(request, BANANA_RETRO_CTL_GET_STATUS, 2, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert((get16(reply + 36 + 4) & 0xffffu) == 0);
	assert(get64(reply + 38 + 8) == 1);

	unsigned char hold[9] = { BANANA_RETRO_CTL_OWNER_UI };
	put64(hold + 1, 0);
	n = make_packet(request, BANANA_RETRO_CTL_PAUSE, 3, session, hold, sizeof(hold));
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(fake.pauses == 1);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(fake.pauses == 1); /* Duplicate replays the cached acknowledgement. */
	hold[0] = BANANA_RETRO_CTL_OWNER_SLEEP; put64(hold + 1, 9);
	n = make_packet(request, BANANA_RETRO_CTL_PAUSE, 4, session, hold, sizeof(hold));
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(fake.pauses == 3);
	n = make_packet(request, BANANA_RETRO_CTL_FLUSH_SRAM, 5, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK && fake.flushes == 1);
	unsigned char slot[4]; put32(slot, 3);
	n = make_packet(request, BANANA_RETRO_CTL_SAVE_STATE, 6, session, slot, 4);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK && fake.saves == 1 && fake.last_slot == 3);
	n = make_packet(request, BANANA_RETRO_CTL_LOAD_STATE, 7, session, slot, 4);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK && fake.loads == 1);
	assert(fake.pauses == 3); /* Persistence runs at a safe point while both owners hold pause. */
	hold[0] = BANANA_RETRO_CTL_OWNER_UI; put64(hold + 1, 0);
	n = make_packet(request, BANANA_RETRO_CTL_RESUME, 8, session, hold, sizeof(hold));
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(fake.pauses == 2);
	hold[0] = BANANA_RETRO_CTL_OWNER_SLEEP; put64(hold + 1, 8);
	n = make_packet(request, BANANA_RETRO_CTL_RESUME, 9, session, hold, sizeof(hold));
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_BAD_STATE);
	assert(fake.pauses == 2);
	put64(hold + 1, 9);
	n = make_packet(request, BANANA_RETRO_CTL_RESUME, 10, session, hold, sizeof(hold));
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(fake.pauses == 0);
	put32(slot, 10);
	n = make_packet(request, BANANA_RETRO_CTL_SAVE_STATE, 11, session, slot, 4);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_INVALID && fake.saves == 1);

	unsigned char page[4]; put16(page, 0); put16(page + 2, 8);
	n = make_packet(request, BANANA_RETRO_CTL_GET_OPTIONS, 12, session, page, sizeof(page));
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(get16(reply + 38) == 3 && get16(reply + 40) == 3);
	const char *key = "fixture_speed", *value = "3";
	unsigned char set[64]; put16(set, (uint16_t)strlen(key)); put16(set + 2, (uint16_t)strlen(value));
	set[4] = BANANA_RETRO_CTL_SCOPE_SESSION; memcpy(set + 5, key, strlen(key));
	memcpy(set + 5 + strlen(key), value, strlen(value));
	n = make_packet(request, BANANA_RETRO_CTL_SET_OPTION, 13, session, set, 5 + strlen(key) + strlen(value));
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(reply[38] == 1 && fake.option_sets == 1 && strcmp(options_get(key), value) == 0);
	set[4] = BANANA_RETRO_CTL_SCOPE_CORE;
	n = make_packet(request, BANANA_RETRO_CTL_SET_OPTION, 14, session, set, 5 + strlen(key) + strlen(value));
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_UNSUPPORTED);

	n = make_packet(request, 99, 15, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_UNSUPPORTED);
	n = make_packet(request, BANANA_RETRO_CTL_RESET, 16, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK && fake.resets == 1);
	n = make_packet(request, BANANA_RETRO_CTL_QUIT, 17, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK && fake.quits == 1);
	/* Older request IDs cannot execute again; the latest ID reconciles a reconnect. */
	n = make_packet(request, BANANA_RETRO_CTL_RESET, 16, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_STALE_SESSION && fake.resets == 1);
	close(fd);
	control_handle_ready(server, false, 1u);
	fd = connect_client(path); control_handle_ready(server, true, false);
	n = make_packet(request, BANANA_RETRO_CTL_HELLO, 18, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	n = make_packet(request, BANANA_RETRO_CTL_GET_STATUS, 19, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(get64(reply + 38 + 8) == 18);
	assert(get64(reply + 38 + 44) == 17);
	assert(get16(reply + 38 + 52) == BANANA_RETRO_CTL_QUIT);
	assert(get16(reply + 38 + 54) == BANANA_RETRO_CTL_OK);

	/* Truncated authenticated packets close the channel without invoking an operation. */
	assert(send(fd, request, BANANA_RETRO_CONTROL_HEADER_SIZE - 1, 0) == BANANA_RETRO_CONTROL_HEADER_SIZE - 1);
	control_handle_ready(server, false, 1u);
	close(fd);
	fd = connect_client(path); control_handle_ready(server, true, false);
	n = make_packet(request, BANANA_RETRO_CTL_HELLO, 20, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	n = make_packet(request, BANANA_RETRO_CTL_GET_STATUS, 21, session, NULL, 0);
	request[34] = 1; /* Declared payload length does not match the seqpacket. */
	assert(send(fd, request, n, 0) == (ssize_t)n);
	control_handle_ready(server, false, 1u);
	close(fd);
	fd = connect_client(path); control_handle_ready(server, true, false);
	n = make_packet(request, BANANA_RETRO_CTL_HELLO, 22, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	unsigned char oversized[BANANA_RETRO_CONTROL_MAX_PACKET + 1] = {0};
	assert(send(fd, oversized, sizeof(oversized), 0) == (ssize_t)sizeof(oversized));
	control_handle_ready(server, false, 1u);
	close(fd);
	control_close(server);

	/* A peer with the wrong session cannot complete HELLO and disables this session's control. */
	server = control_open_from_env(&ops, session); assert(server);
	fd = connect_client(path); control_handle_ready(server, true, false);
	unsigned char wrong[16]; memcpy(wrong, session, 16); wrong[0] ^= 0xff;
	n = make_packet(request, BANANA_RETRO_CTL_HELLO, 1, wrong, NULL, 0);
	assert(send(fd, request, n, 0) == (ssize_t)n);
	control_handle_ready(server, false, 1u);
	assert(!control_is_authenticated(server));
	close(fd); control_close(server);

	/* A HELLO with an operation payload is a failed handshake, not an authenticated session. */
	server = control_open_from_env(&ops, session); assert(server);
	fd = connect_client(path); control_handle_ready(server, true, false);
	unsigned char hello_payload = 1;
	n = make_packet(request, BANANA_RETRO_CTL_HELLO, 1, session, &hello_payload, 1);
	assert(send(fd, request, n, 0) == (ssize_t)n);
	control_handle_ready(server, false, 1u);
	assert(!control_is_authenticated(server));
	close(fd); control_close(server);
	assert(rmdir(directory) == 0);
	puts("control fixtures passed");
	return 0;
}
