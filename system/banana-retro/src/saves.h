#ifndef BANANA_RETRO_SAVES_H
#define BANANA_RETRO_SAVES_H

#include <stdbool.h>
#include <stddef.h>

enum { SAVES_STATE_SLOTS = 10 };

enum saves_legacy_result {
	SAVES_LEGACY_NONE,
	SAVES_LEGACY_READY,
	SAVES_LEGACY_ALREADY_IMPORTED,
	SAVES_LEGACY_AMBIGUOUS,
	SAVES_LEGACY_CHANGED,
	SAVES_LEGACY_INVALID
};

enum saves_resume_result {
	/* No eligible marker, including legacy markers that require a cold start. */
	SAVES_RESUME_NONE,
	SAVES_RESUME_READY,
	/* A confirmed rejection. Probe returns the marker token for conditional removal. */
	SAVES_RESUME_INVALID,
	/* The attempt failed without establishing that the marker is invalid. */
	SAVES_RESUME_RETRY
};

struct saves;
typedef bool (*saves_state_fn)(void *context, void *buffer, size_t size);
typedef bool (*saves_unserialize_fn)(void *context, const void *buffer, size_t size);

struct saves *saves_open(const char *root, const char *core_path,
			 const char *content_path);
struct saves *saves_open_with_options(const char *root, const char *core_path,
				      const char *content_path, const char *options_fingerprint);
/* Active session options affect state compatibility, while the launch profile
 * continues to identify the battery-save store. */
void saves_set_state_options(struct saves *saves, const char *options);
bool saves_checkpoint_options_match(const struct saves *saves);
bool saves_set_legacy_core_name(struct saves *saves, const char *library_name);
void saves_close(struct saves *saves);
bool saves_has_no_generation(struct saves *saves);
bool saves_load_memory(struct saves *saves, void *sram, size_t sram_size,
		       void *rtc, size_t rtc_size);
/* Preview hashes the profile, coherent source set, file contents, and destination
 * state into a 64-character receipt. Import requires that receipt and rejects
 * any changed snapshot. It copies into the native store without changing the
 * source files. */
enum saves_legacy_result saves_legacy_preview(struct saves *saves,
		const char *legacy_root, size_t sram_size, size_t rtc_size,
		char receipt[65]);
enum saves_legacy_result saves_legacy_import(struct saves *saves,
		const char *legacy_root, size_t sram_size, size_t rtc_size,
		const char *receipt);
bool saves_load_state(struct saves *saves, unsigned slot, void *data, size_t size);
bool saves_state_present(struct saves *saves, unsigned slot);
bool saves_checkpoint_present(struct saves *saves);
bool saves_load_checkpoint(struct saves *saves, void *data, size_t size);
/* Publish a checkpoint and its resume marker after the generation is durable. */
bool saves_commit_checkpoint(struct saves *saves, const void *sram, size_t sram_size,
		const void *rtc, size_t rtc_size, const void *checkpoint, size_t checkpoint_size);
enum saves_resume_result saves_resume_probe(struct saves *saves, char token[65]);
/* Load the checkpoint and memory only while the verified marker token matches. */
enum saves_resume_result saves_load_resume(struct saves *saves, const char *token,
		void *state, size_t state_size,
		void *sram, size_t sram_size, void *rtc, size_t rtc_size);
bool saves_resume_clear_if(struct saves *saves, const char token[65]);
/* Publish battery memory and supersede all older automatic resume markers. */
bool saves_commit_battery_only(struct saves *saves, const void *sram, size_t sram_size,
		const void *rtc, size_t rtc_size);
bool saves_commit(struct saves *saves, const void *sram, size_t sram_size,
		  const void *rtc, size_t rtc_size, int state_slot,
		  const void *state, size_t state_size);
bool saves_state_capture(saves_state_fn serialize, void *context, void *buffer, size_t size);
bool saves_state_restore(saves_unserialize_fn unserialize, void *context,
			 const void *buffer, size_t size);
#ifdef SAVES_TESTING
void saves_test_fail_write_after(size_t bytes);
enum saves_test_event {
	SAVES_TEST_FILE_FSYNC,
	SAVES_TEST_TEMP_DIR_FSYNC,
	SAVES_TEST_PARENT_DIR_FSYNC,
	SAVES_TEST_GENERATION_RENAME,
	SAVES_TEST_CURRENT_RENAME,
	SAVES_TEST_RESUME_RENAME,
	SAVES_TEST_AFTER_MANIFEST_WRITE,
	SAVES_TEST_AFTER_GENERATION_RENAME,
	SAVES_TEST_AFTER_CURRENT_RENAME,
	SAVES_TEST_AFTER_VALIDATION,
	SAVES_TEST_RESUME_ALLOC,
	SAVES_TEST_RESUME_READ
};
void saves_test_fail_event(enum saves_test_event event, unsigned occurrence);
void saves_test_crash_event(enum saves_test_event event);
void saves_test_set_hook(void (*hook)(void));
bool saves_test_downgrade_current_manifest(struct saves *saves, unsigned version);
#endif

#endif
