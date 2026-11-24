#define _GNU_SOURCE
#ifdef BANANA_RETRO_RUNTIME_CORE
#include "libretro.h"

#include <stdint.h>
#include <string.h>

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static uint32_t core_counter;
static uint8_t core_sram[32];
static const uint16_t pixels[4] = { 1, 2, 3, 4 };

struct runtime_snapshot { uint32_t counter; uint8_t sram[32]; };

unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_set_environment(retro_environment_t callback) { env_cb = callback; }
void retro_set_video_refresh(retro_video_refresh_t callback) { video_cb = callback; }
void retro_set_audio_sample(retro_audio_sample_t callback) { audio_cb = callback; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback) { audio_batch_cb = callback; }
void retro_set_input_poll(retro_input_poll_t callback) { input_poll_cb = callback; }
void retro_set_input_state(retro_input_state_t callback) { input_state_cb = callback; }
void retro_init(void) { (void)env_cb; }
void retro_get_system_info(struct retro_system_info *info)
{ info->library_name = "runtime-fixture"; info->library_version = "1"; info->need_fullpath = false; }
bool retro_load_game(const struct retro_game_info *game)
{ if (!game) return false; memset(core_sram, 0, sizeof(core_sram)); core_counter = 0; return true; }
void retro_get_system_av_info(struct retro_system_av_info *info)
{
	memset(info, 0, sizeof(*info));
	info->geometry.base_width = info->geometry.max_width = 2;
	info->geometry.base_height = info->geometry.max_height = 2;
	info->geometry.aspect_ratio = 1.0f;
	info->timing.fps = 60.0;
	info->timing.sample_rate = 48000.0;
}
void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
void retro_run(void)
{
	++core_counter;
	core_sram[0] = (uint8_t)core_counter;
	if (input_poll_cb) input_poll_cb();
	if (input_state_cb) (void)input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
	if (video_cb) video_cb(pixels, 2, 2, 4);
	if (audio_cb) audio_cb(100, -100);
	if (audio_batch_cb) { const int16_t samples[4] = { 1, -1, 2, -2 }; (void)audio_batch_cb(samples, 2); }
}
void retro_reset(void) { core_counter = 0; core_sram[0] = 0; }
void retro_unload_game(void) {}
void retro_deinit(void) {}
void *retro_get_memory_data(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? core_sram : NULL; }
size_t retro_get_memory_size(unsigned id) { return id == RETRO_MEMORY_SAVE_RAM ? sizeof(core_sram) : 0; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{ (void)index; (void)enabled; (void)code; }
bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t count)
{ (void)type; (void)info; (void)count; return false; }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
size_t retro_serialize_size(void) { return sizeof(struct runtime_snapshot); }
bool retro_serialize(void *data, size_t size)
{
	if (!data || size != sizeof(struct runtime_snapshot)) return false;
	struct runtime_snapshot snapshot = { .counter = core_counter };
	memcpy(snapshot.sram, core_sram, sizeof(core_sram));
	memcpy(data, &snapshot, sizeof(snapshot));
	return true;
}
bool retro_unserialize(const void *data, size_t size)
{
	if (!data || size != sizeof(struct runtime_snapshot)) return false;
	struct runtime_snapshot snapshot;
	memcpy(&snapshot, data, sizeof(snapshot));
	core_counter = snapshot.counter;
	memcpy(core_sram, snapshot.sram, sizeof(core_sram));
	return true;
}
unsigned fixture_counter(void) { return core_counter; }
uint8_t *fixture_sram(void) { return core_sram; }

#else
#include "control.h"
#include "options.h"

#include <assert.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
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
#include <sys/wait.h>

#define main banana_retro_embedded_main
#include "../src/main.c"
#undef main

#define main banana_audio_test_main
#include "check-audio.c"
#undef main

extern bool core_environment_test(unsigned command, void *data);

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

struct peer_request { uint16_t type; uint8_t owner; uint64_t epoch, id; };

static uint16_t sleep_peer_exchange(int command_fd, int event_fd,
				    struct control_server *server,
				    struct peer_request request)
{
	assert(write(command_fd, &request, sizeof(request)) == (ssize_t)sizeof(request));
	char ready; assert(read(event_fd, &ready, 1) == 1 && ready == 'R');
	control_handle_ready(server, false, 1u << 1);
	unsigned char result[2]; assert(read(event_fd, result, 2) == 2);
	return get16(result);
}

static void sleep_peer_process(const char *path, const unsigned char session[16],
			       int command_fd, int event_fd)
{
	char start;
	assert(read(command_fd, &start, 1) == 1 && start == 'C');
	int fd = connect_client(path);
	unsigned char request[BANANA_RETRO_CONTROL_MAX_PACKET];
	unsigned char reply[BANANA_RETRO_CONTROL_MAX_PACKET];
	uint64_t id = 1;
	size_t n = make_packet(request, BANANA_RETRO_CTL_HELLO, id++, session, NULL, 0);
	assert(send(fd, request, n, 0) == (ssize_t)n);
	assert(write(event_fd, "R", 1) == 1);
	assert(read(fd, reply, sizeof(reply)) >= BANANA_RETRO_CONTROL_HEADER_SIZE + 2);
	assert(write(event_fd, reply + BANANA_RETRO_CONTROL_HEADER_SIZE, 2) == 2);
	struct peer_request op;
	while (read(command_fd, &op, sizeof(op)) == (ssize_t)sizeof(op)) {
		if (op.type == 0) break;
		unsigned char payload[9] = { op.owner };
		put64(payload + 1, op.epoch);
		const void *data = op.type == BANANA_RETRO_CTL_PAUSE || op.type == BANANA_RETRO_CTL_RESUME ?
			payload : NULL;
		size_t size = data ? sizeof(payload) : 0;
		n = make_packet(request, op.type, op.id ? op.id : id++, session, data, size);
		assert(send(fd, request, n, 0) == (ssize_t)n);
		assert(write(event_fd, "R", 1) == 1);
		ssize_t response_size = recv(fd, reply, sizeof(reply), 0);
		assert(response_size >= BANANA_RETRO_CONTROL_HEADER_SIZE + 2);
		assert(write(event_fd, reply + BANANA_RETRO_CONTROL_HEADER_SIZE, 2) == 2);
	}
	close(fd);
	_exit(0);
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
	unsigned char session[16]; memset(session, 0x33, sizeof(session));
	struct fake fake = {0};
	struct control_ops ops = { .data=&fake, .status=status, .pause=pause_hold };
	char ui_pid[32]; assert(snprintf(ui_pid, sizeof(ui_pid), "%ld", (long)getpid()) > 0);
	assert(setenv("BANANA_RETRO_CONTROL_UI_PID", ui_pid, 1) == 0);
	assert(setenv("BANANA_RETRO_CONTROL_SLEEP_PID", ui_pid, 1) == 0);
	errno = 0;
	assert(control_open_from_env(&ops, session) == NULL && errno == EINVAL);
	int commands[2], events[2];
	assert(pipe(commands) == 0 && pipe(events) == 0);
	pid_t sleep_pid = fork(); assert(sleep_pid >= 0);
	if (sleep_pid == 0) {
		close(commands[1]); close(events[0]);
		sleep_peer_process(path, session, commands[0], events[1]);
	}
	close(commands[0]); close(events[1]);
	char sleep_pid_text[32];
	assert(snprintf(sleep_pid_text, sizeof(sleep_pid_text), "%ld", (long)sleep_pid) > 0);
	assert(setenv("BANANA_RETRO_CONTROL_SLEEP_PID", sleep_pid_text, 1) == 0);
	struct control_server *server = control_open_from_env(&ops, session); assert(server);
	int ui = connect_client(path);
	control_handle_ready(server, true, 0);
	assert(control_client_fd(server, 0) >= 0);
	unsigned char request[BANANA_RETRO_CONTROL_MAX_PACKET], reply[BANANA_RETRO_CONTROL_MAX_PACKET];
	size_t packet_size = make_packet(request, BANANA_RETRO_CTL_HELLO, 1, session, NULL, 0);
	assert(exchange_slot(ui, server, 0, request, packet_size, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(write(commands[1], "C", 1) == 1);
	char ready; assert(read(events[0], &ready, 1) == 1 && ready == 'R');
	control_handle_ready(server, true, 0);
	assert(control_client_fd(server, 1) >= 0);
	control_handle_ready(server, false, 1u << 1);
	unsigned char hello_result[2]; assert(read(events[0], hello_result, 2) == 2);
	assert(get16(hello_result) == BANANA_RETRO_CTL_OK);
	unsigned char hold[9] = { BANANA_RETRO_CTL_OWNER_UI };
	put64(hold + 1, 0);
	packet_size = make_packet(request, BANANA_RETRO_CTL_PAUSE, 2, session, hold, sizeof(hold));
	assert(exchange_slot(ui, server, 0, request, packet_size, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(fake.pauses == 1);
	struct peer_request op = { .type=BANANA_RETRO_CTL_GET_STATUS, .id=2 };
	assert(sleep_peer_exchange(commands[1], events[0], server, op) == BANANA_RETRO_CTL_OK);
	op = (struct peer_request){ .type=BANANA_RETRO_CTL_FLUSH_SRAM, .id=2 };
	assert(sleep_peer_exchange(commands[1], events[0], server, op) == BANANA_RETRO_CTL_UNSUPPORTED);
	assert(fake.pauses == 1 && fake.flushes == 0); /* ACL precedes replay on colliding role IDs. */
	op = (struct peer_request){ .type=BANANA_RETRO_CTL_PAUSE,
		.owner=BANANA_RETRO_CTL_OWNER_SLEEP, .epoch=41, .id=3 };
	assert(sleep_peer_exchange(commands[1], events[0], server, op) == BANANA_RETRO_CTL_OK);
	assert(fake.pauses == 3);
	static const uint16_t forbidden[] = { BANANA_RETRO_CTL_FLUSH_SRAM,
		BANANA_RETRO_CTL_SAVE_STATE, BANANA_RETRO_CTL_LOAD_STATE,
		BANANA_RETRO_CTL_GET_OPTIONS, BANANA_RETRO_CTL_SET_OPTION,
		BANANA_RETRO_CTL_RESET, BANANA_RETRO_CTL_QUIT };
	for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
		op = (struct peer_request){ .type=forbidden[i], .id=4 };
		assert(sleep_peer_exchange(commands[1], events[0], server, op) == BANANA_RETRO_CTL_UNSUPPORTED);
		assert(fake.pauses == 3 && fake.flushes == 0 && fake.saves == 0 && fake.loads == 0 &&
			fake.resets == 0 && fake.quits == 0 && fake.option_sets == 0);
	}
	op = (struct peer_request){ .type=BANANA_RETRO_CTL_GET_STATUS, .id=4 };
	assert(sleep_peer_exchange(commands[1], events[0], server, op) == BANANA_RETRO_CTL_OK);
	packet_size = make_packet(request, BANANA_RETRO_CTL_GET_STATUS, 3, session, NULL, 0);
	assert(exchange_slot(ui, server, 0, request, packet_size, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	op = (struct peer_request){ .type=BANANA_RETRO_CTL_RESUME,
		.owner=BANANA_RETRO_CTL_OWNER_SLEEP, .epoch=41, .id=5 };
	assert(sleep_peer_exchange(commands[1], events[0], server, op) == BANANA_RETRO_CTL_OK);
	assert(fake.pauses == 1);
	hold[0] = BANANA_RETRO_CTL_OWNER_UI;
	packet_size = make_packet(request, BANANA_RETRO_CTL_RESUME, 4, session, hold, sizeof(hold));
	assert(exchange_slot(ui, server, 0, request, packet_size, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(fake.pauses == 0);
	op = (struct peer_request){0}; assert(write(commands[1], &op, sizeof(op)) == (ssize_t)sizeof(op));
	close(commands[1]); close(events[0]); close(ui);
	int status_code; assert(waitpid(sleep_pid, &status_code, 0) == sleep_pid);
	assert(WIFEXITED(status_code) && WEXITSTATUS(status_code) == 0);
	control_close(server);
	assert(rmdir(directory) == 0);
	assert(unsetenv("BANANA_RETRO_CONTROL_UI_PID") == 0);
	assert(unsetenv("BANANA_RETRO_CONTROL_SLEEP_PID") == 0);
	assert(unsetenv("BANANA_RETRO_CONTROL_SOCKET") == 0);
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
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_UNSUPPORTED);
	assert(fake.pauses == 0);
	close(fd); control_handle_ready(server, false, 1u); control_close(server);
	assert(rmdir(directory) == 0);
	assert(unsetenv("BANANA_RETRO_CONTROL_UI_PID") == 0);
	assert(unsetenv("BANANA_RETRO_CONTROL_SLEEP_PID") == 0);
}

static void test_replay_history_eviction(void)
{
	char directory[] = "/tmp/banana-retro-control-history.XXXXXX";
	assert(mkdtemp(directory));
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	assert(snprintf(path, sizeof(path), "%s/control.sock", directory) > 0);
	assert(setenv("BANANA_RETRO_CONTROL_SOCKET", path, 1) == 0);
	char pid[32]; assert(snprintf(pid, sizeof(pid), "%ld", (long)getpid()) > 0);
	assert(setenv("BANANA_RETRO_CONTROL_UI_PID", pid, 1) == 0);
	assert(unsetenv("BANANA_RETRO_CONTROL_SLEEP_PID") == 0);
	unsigned char session[16]; memset(session, 0x8c, sizeof(session));
	struct fake fake = {0};
	struct control_ops ops = { .data=&fake, .status=status, .pause=pause_hold };
	struct control_server *server = control_open_from_env(&ops, session);
	assert(server);
	unsigned char request[BANANA_RETRO_CONTROL_MAX_PACKET];
	unsigned char reply[BANANA_RETRO_CONTROL_MAX_PACKET];
	int fd = connect_client(path);
	control_handle_ready(server, true, false);
	size_t n = make_packet(request, BANANA_RETRO_CTL_HELLO, 1, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	unsigned char hold[9] = { BANANA_RETRO_CTL_OWNER_UI };
	put64(hold + 1, 0);
	n = make_packet(request, BANANA_RETRO_CTL_PAUSE, 2, session, hold, sizeof(hold));
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(fake.pauses == 1);
	close(fd);
	control_handle_ready(server, false, 1u);

	/* Fifteen later successes fill the other history slots without evicting request 2. */
	fd = connect_client(path);
	control_handle_ready(server, true, false);
	n = make_packet(request, BANANA_RETRO_CTL_HELLO, 3, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	for (uint64_t id = 4; id <= 17; ++id) {
		n = make_packet(request, BANANA_RETRO_CTL_GET_STATUS, id, session, NULL, 0);
		assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	}
	n = make_packet(request, BANANA_RETRO_CTL_PAUSE, 2, session, hold, sizeof(hold));
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(fake.pauses == 1); /* The retained response is replayed across reconnect. */

	/* The sixteenth later success wraps the 16-entry role history and evicts request 2. */
	n = make_packet(request, BANANA_RETRO_CTL_GET_STATUS, 18, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	close(fd);
	control_handle_ready(server, false, 1u);
	fd = connect_client(path);
	control_handle_ready(server, true, false);
	n = make_packet(request, BANANA_RETRO_CTL_HELLO, 19, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	n = make_packet(request, BANANA_RETRO_CTL_PAUSE, 2, session, hold, sizeof(hold));
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_STALE_SESSION);
	assert(fake.pauses == 1); /* Evicted mutation IDs are rejected without re-execution. */
	close(fd);
	control_handle_ready(server, false, 1u);
	control_close(server);
	assert(rmdir(directory) == 0);
	assert(unsetenv("BANANA_RETRO_CONTROL_UI_PID") == 0);
	assert(unsetenv("BANANA_RETRO_CONTROL_SOCKET") == 0);
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

static void test_option_environment(void)
{
	options_reset();
	struct retro_variable v0[] = { { "env_speed", "Speed; slow|fast" }, { NULL, NULL } };
	assert(core_environment_test(RETRO_ENVIRONMENT_SET_VARIABLES, v0));
	assert(core_environment_test(RETRO_ENVIRONMENT_GET_VARIABLE, NULL));
	assert(core_environment_test(RETRO_ENVIRONMENT_SET_VARIABLES, NULL));
	assert(options_count() == 1);
	struct retro_variable malformed_v0[] = { { "env_bad", NULL }, { NULL, NULL } };
	assert(!core_environment_test(RETRO_ENVIRONMENT_SET_VARIABLES, malformed_v0));
	assert(core_environment_test(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, NULL));
	assert(!core_environment_test(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, NULL));
	assert(!core_environment_test(RETRO_ENVIRONMENT_SET_VARIABLE, NULL));
	assert(!core_environment_test(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, NULL));
	assert(!core_environment_test(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL, NULL));
	assert(!core_environment_test(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL, NULL));
	struct retro_core_options_intl intl = { .us=NULL, .local=NULL };
	assert(!core_environment_test(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL, &intl));
	struct retro_core_options_v2_intl intl_v2 = { .us=NULL, .local=NULL };
	assert(!core_environment_test(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL, &intl_v2));
	struct retro_core_options_v2 malformed_v2 = { .definitions=NULL };
	assert(!core_environment_test(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &malformed_v2));
	assert(options_count() == 1);
	struct retro_core_option_definition intl_def[2] = {0};
	intl_def[0].key = "env_intl"; intl_def[0].desc = "Intl";
	intl_def[0].default_value = "on"; intl_def[0].values[0].value = "on";
	struct retro_core_options_intl valid_intl = { .us=intl_def, .local=NULL };
	assert(core_environment_test(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL, &valid_intl));
	struct retro_core_option_v2_definition intl_v2_def[2] = {0};
	intl_v2_def[0].key = "env_intl_v2"; intl_v2_def[0].desc = "Intl v2";
	intl_v2_def[0].default_value = "on"; intl_v2_def[0].values[0].value = "on";
	struct retro_core_options_v2 intl_v2_us = { .definitions=intl_v2_def };
	struct retro_core_options_v2_intl valid_intl_v2 = { .us=&intl_v2_us, .local=NULL };
	assert(core_environment_test(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL, &valid_intl_v2));
	struct retro_variable get = { .key="env_speed" };
	assert(core_environment_test(RETRO_ENVIRONMENT_GET_VARIABLE, &get));
	assert(strcmp(get.value, "slow") == 0);
	struct retro_variable unknown_get = { .key="env_unknown", .value="sentinel" };
	assert(core_environment_test(RETRO_ENVIRONMENT_GET_VARIABLE, &unknown_get));
	assert(unknown_get.value == NULL);
	struct retro_variable bad_get = { .key=NULL };
	assert(!core_environment_test(RETRO_ENVIRONMENT_GET_VARIABLE, &bad_get));
	struct retro_variable bad_set = { .key="env_speed", .value=NULL };
	assert(!core_environment_test(RETRO_ENVIRONMENT_SET_VARIABLE, &bad_set));
	struct retro_variable set = { .key="env_speed", .value="fast" };
	assert(core_environment_test(RETRO_ENVIRONMENT_SET_VARIABLE, &set));
	bool updated = false;
	assert(core_environment_test(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated);
	struct retro_core_option_display display = { .key="env_speed", .visible=false };
	assert(core_environment_test(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display));
	display.key = NULL;
	assert(!core_environment_test(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display));
	assert(core_environment_test(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, NULL));
	assert(options_count() == 0);
	assert(core_environment_test(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, NULL));
	assert(options_count() == 0);
}

static void remove_tree(const char *path)
{
	DIR *directory = opendir(path);
	if (!directory) { assert(unlink(path) == 0); return; }
	struct dirent *entry;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
		char child[4096];
		int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
		assert(n > 0 && (size_t)n < sizeof(child));
		remove_tree(child);
	}
	assert(closedir(directory) == 0);
	assert(rmdir(path) == 0);
}

static void test_runtime_operations(const char *core_path)
{
	char base[] = "/tmp/banana-retro-runtime.XXXXXX";
	assert(mkdtemp(base));
	char content[256], save_root[256];
	assert(snprintf(content, sizeof(content), "%s/content.rom", base) > 0);
	assert(snprintf(save_root, sizeof(save_root), "%s/saves", base) > 0);
	int content_fd = open(content, O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(content_fd >= 0 && write(content_fd, "runtime", 7) == 7 && close(content_fd) == 0);
	assert(core_load(core_path, content, NULL));
	struct saves *saves = saves_open(save_root, core_path, content); assert(saves);
	void *fixture = dlopen(core_path, RTLD_NOW | RTLD_LOCAL); assert(fixture);
	void *symbol = dlsym(fixture, "fixture_counter"); assert(symbol);
	unsigned (*counter)(void) = NULL; assert(sizeof(counter) == sizeof(symbol));
	memcpy(&counter, &symbol, sizeof(counter));
	symbol = dlsym(fixture, "fixture_sram"); assert(symbol);
	uint8_t *(*sram)(void) = NULL; assert(sizeof(sram) == sizeof(symbol));
	memcpy(&sram, &symbol, sizeof(sram));
	uint8_t *memory = sram(); assert(memory);

	struct fake_pcm pcm = {0};
	struct audio_alsa *audio = new_audio(&pcm, 48000); assert(audio);
	open_fake(audio, &pcm);
	core_set_audio_output(audio);
	struct native_runtime runtime = { .audio=audio, .saves=saves, .save_state_slot=2 };
	char socket_path[256], ui_pid[32];
	assert(snprintf(socket_path, sizeof(socket_path), "%s/control.sock", base) > 0);
	assert(snprintf(ui_pid, sizeof(ui_pid), "%ld", (long)getpid()) > 0);
	assert(setenv("BANANA_RETRO_CONTROL_SOCKET", socket_path, 1) == 0);
	assert(setenv("BANANA_RETRO_CONTROL_UI_PID", ui_pid, 1) == 0);
	assert(unsetenv("BANANA_RETRO_CONTROL_SLEEP_PID") == 0);
	unsigned char session[16]; memset(session, 0xa7, sizeof(session));
	struct control_ops ops = {
		.data=&runtime, .status=control_status, .pause=control_pause,
		.flush=control_flush, .save_state=control_save_state,
		.load_state=control_load_state, .option_count=control_option_count,
		.option=control_option, .set_option=control_set_option,
		.reset=control_reset, .quit=control_quit
	};
	struct control_server *server = control_open_from_env(&ops, session); assert(server);
	int control_fd = connect_client(socket_path); control_handle_ready(server, true, 0);
	unsigned char request[BANANA_RETRO_CONTROL_MAX_PACKET], reply[BANANA_RETRO_CONTROL_MAX_PACKET];
	size_t packet_size = make_packet(request, BANANA_RETRO_CTL_HELLO, 1, session, NULL, 0);
	assert(exchange(control_fd, server, request, packet_size, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(core_step(1) && counter() == 1);
	unsigned char hold[9] = { BANANA_RETRO_CTL_OWNER_UI }; put64(hold + 1, 0);
	packet_size = make_packet(request, BANANA_RETRO_CTL_PAUSE, 2, session, hold, sizeof(hold));
	assert(exchange(control_fd, server, request, packet_size, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(control_pause(&runtime, BANANA_RETRO_CTL_OWNER_SLEEP, 71, true));
	assert(pause_owners(&runtime) == 3);
	packet_size = make_packet(request, BANANA_RETRO_CTL_RESUME, 3, session, hold, sizeof(hold));
	assert(exchange(control_fd, server, request, packet_size, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(pause_owners(&runtime) == 2);
	assert(core_step(2));
	struct audio_alsa_status audio_status;
	audio_alsa_get_status(audio, &audio_status);
	assert(audio_status.paused_frames > 0);
	assert(control_pause(&runtime, BANANA_RETRO_CTL_OWNER_SLEEP, 71, false));
	assert(pause_owners(&runtime) == 0);
	assert(core_step(3));
	audio_alsa_get_status(audio, &audio_status);
	assert(audio_status.accepted_frames > 0);

	memory[0] = 0xa5;
	unsigned saved_counter = counter();
	unsigned char slot[4]; put32(slot, 2);
	packet_size = make_packet(request, BANANA_RETRO_CTL_SAVE_STATE, 4, session, slot, sizeof(slot));
	assert(exchange(control_fd, server, request, packet_size, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	memory[0] = 0x42;
	assert(core_step(4));
	assert(memory[0] != 0xa5);
	packet_size = make_packet(request, BANANA_RETRO_CTL_LOAD_STATE, 5, session, slot, sizeof(slot));
	assert(exchange(control_fd, server, request, packet_size, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(memory[0] == 0xa5 && counter() == saved_counter);

	packet_size = make_packet(request, BANANA_RETRO_CTL_RESET, 6, session, NULL, 0);
	assert(exchange(control_fd, server, request, packet_size, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(counter() == 0 && memory[0] == 0);
	memory[0] = 0x39;
	packet_size = make_packet(request, BANANA_RETRO_CTL_FLUSH_SRAM, 7, session, NULL, 0);
	assert(exchange(control_fd, server, request, packet_size, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	memory[0] = 0x11;
	assert(core_persistence_load(saves, -1));
	assert(memory[0] == 0x39);

	memory[0] = 0x5a;
	packet_size = make_packet(request, BANANA_RETRO_CTL_QUIT, 8, session, NULL, 0);
	assert(exchange(control_fd, server, request, packet_size, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(runtime.stopped && runtime.committed);
	memory[0] = 0x22;
	assert(core_persistence_load(saves, 2));
	assert(memory[0] == 0x5a);
	assert(core_release());
	close(control_fd);
	control_close(server);
	assert(unsetenv("BANANA_RETRO_CONTROL_SOCKET") == 0);
	assert(unsetenv("BANANA_RETRO_CONTROL_UI_PID") == 0);
	core_set_audio_output(NULL);
	audio_alsa_close(audio);
	saves_close(saves);
	assert(dlclose(fixture) == 0);
	assert(unlink(content) == 0);
	remove_tree(save_root);
	assert(rmdir(base) == 0);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	test_option_environment();
	test_runtime_operations(argv[1]);
	test_options();
	test_wayland_while_paused();
	test_two_clients();
	test_owner_identity();
	test_replay_history_eviction();
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
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_UNSUPPORTED);
	assert(fake.pauses == 1);
	n = make_packet(request, BANANA_RETRO_CTL_FLUSH_SRAM, 5, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK && fake.flushes == 1);
	unsigned char slot[4]; put32(slot, 3);
	n = make_packet(request, BANANA_RETRO_CTL_SAVE_STATE, 6, session, slot, 4);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK && fake.saves == 1 && fake.last_slot == 3);
	n = make_packet(request, BANANA_RETRO_CTL_LOAD_STATE, 7, session, slot, 4);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK && fake.loads == 1);
	assert(fake.pauses == 1); /* Persistence runs at a safe point while UI holds pause. */
	hold[0] = BANANA_RETRO_CTL_OWNER_UI; put64(hold + 1, 0);
	n = make_packet(request, BANANA_RETRO_CTL_RESUME, 8, session, hold, sizeof(hold));
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
	assert(send(fd, request, n, 0) == (ssize_t)n);
	control_handle_ready(server, false, 1u);
	assert(fake.resets == 1); /* Drop the acknowledgement after the real reset callback. */
	n = make_packet(request, BANANA_RETRO_CTL_QUIT, 17, session, NULL, 0);
	assert(send(fd, request, n, 0) == (ssize_t)n);
	control_handle_ready(server, false, 1u);
	assert(fake.quits == 1); /* A later mutation replaces the status mutation record. */
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
	/* A reconnect recovers reset 16 after quit 17 replaced the status mutation record. */
	n = make_packet(request, BANANA_RETRO_CTL_RESET, 16, session, NULL, 0);
	assert(exchange(fd, server, request, n, reply, sizeof(reply)) == BANANA_RETRO_CTL_OK);
	assert(fake.resets == 1);

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
#endif
