#ifndef BANANA_RETRO_CONTROL_INTERNAL_H
#define BANANA_RETRO_CONTROL_INTERNAL_H

#include "banana-retro-control.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct control_server;

struct control_status {
	uint32_t flags;
	uint32_t pause_owners;
	uint64_t last_request_id;
	uint32_t capabilities;
	uint32_t audio_state;
	uint32_t audio_queued_frames;
	uint64_t audio_underruns;
	const char *core_id;
	const char *core_version;
	const char *content_id;
};

struct control_ops {
	void *data;
	void (*status)(void *data, struct control_status *status);
	bool (*pause)(void *data, unsigned owner, uint64_t epoch, bool acquire);
	/* Optional. PREPARE_SLEEP uses it so the committed checkpoint's digest can be returned; the
	 * token is filled only when a checkpoint was committed. Without it PREPARE_SLEEP uses pause(). */
	bool (*prepare_sleep)(void *data, uint64_t epoch, char *resume_token);
	bool (*checkpoint)(void *data, char *resume_token);
	bool (*shutdown)(void *data, char *resume_token);
	bool (*flush)(void *data);
	bool (*save_state)(void *data, unsigned slot);
	bool (*load_state)(void *data, unsigned slot);
	size_t (*option_count)(void *data);
	bool (*option)(void *data, size_t index, const char **key,
		       const char **description, const char **value,
		       const char **default_value, const char *const **choices,
		       size_t *choice_count, bool *visible, bool *restart_required);
	bool (*set_option)(void *data, const char *key, const char *value,
			unsigned scope, bool *restart_required);
	bool (*reset)(void *data);
	bool (*quit)(void *data);
};

/* UI and sleep peers are distinguished by SO_PEERCRED PID; an unset sleep PID
 * disables that role, and equal configured PIDs are rejected as ambiguous. */
struct control_server *control_open_from_env(const struct control_ops *ops,
						     const unsigned char session_id[16]);
void control_close(struct control_server *server);
int control_listener_fd(const struct control_server *server);
int control_client_fd(const struct control_server *server, unsigned slot);
bool control_is_authenticated(const struct control_server *server);
bool control_has_pause(const struct control_server *server);
void control_handle_ready(struct control_server *server, bool listener_ready,
			  unsigned client_ready_mask);

#endif
