#ifndef BANANA_RETRO_SAVES_H
#define BANANA_RETRO_SAVES_H

#include <stdbool.h>
#include <stddef.h>

enum { SAVES_STATE_SLOTS = 10 };

struct saves;
typedef bool (*saves_state_fn)(void *context, void *buffer, size_t size);
typedef bool (*saves_unserialize_fn)(void *context, const void *buffer, size_t size);

struct saves *saves_open(const char *root, const char *core_path,
			 const char *content_path);
struct saves *saves_open_with_options(const char *root, const char *core_path,
				      const char *content_path, const char *options_fingerprint);
void saves_close(struct saves *saves);
bool saves_load_memory(struct saves *saves, void *sram, size_t sram_size,
		       void *rtc, size_t rtc_size);
bool saves_load_state(struct saves *saves, unsigned slot, void *data, size_t size);
bool saves_commit(struct saves *saves, const void *sram, size_t sram_size,
		  const void *rtc, size_t rtc_size, int state_slot,
		  const void *state, size_t state_size);
bool saves_state_capture(saves_state_fn serialize, void *context, void *buffer, size_t size);
bool saves_state_restore(saves_unserialize_fn unserialize, void *context,
			 const void *buffer, size_t size);
#ifdef SAVES_TESTING
void saves_test_fail_write_after(size_t bytes);
#endif

/* These wrappers run only between core frames, while the core is quiescent. */
bool core_persistence_load(struct saves *saves, int state_slot);
bool core_persistence_commit(struct saves *saves, int state_slot);

#endif
