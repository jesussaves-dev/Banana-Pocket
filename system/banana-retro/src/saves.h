#ifndef BANANA_RETRO_SAVES_H
#define BANANA_RETRO_SAVES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SAVES_STATE_SLOTS = 10, SAVES_CONTENT_DIGEST_SIZE = 32 };

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

/* Fixture and tool entry points: these hash content_path themselves, so the store
 * identity is only as stable as that file. Runtimes use saves_open_content(). */
struct saves *saves_open(const char *root, const char *core_path,
			 const char *content_path);
struct saves *saves_open_with_options(const char *root, const char *core_path,
				      const char *content_path, const char *options_fingerprint);
/* Bind the store to the SHA-256 of the exact content bytes the core consumed. The path
 * only names legacy files and the path hash; it never supplies the content identity. */
struct saves *saves_open_content(const char *root, const char *core_path,
		const char *content_path,
		const unsigned char content_digest[SAVES_CONTENT_DIGEST_SIZE],
		const char *options_fingerprint);
void saves_digest_content(const void *data, size_t size,
		unsigned char digest[SAVES_CONTENT_DIGEST_SIZE]);
/* Hash exactly size bytes with pread(), leaving the offset alone. Fails on a short
 * read or when the file is longer than size. */
bool saves_digest_content_fd(int fd, uint64_t size,
		unsigned char digest[SAVES_CONTENT_DIGEST_SIZE]);
/* Active session options affect state compatibility, while the launch profile
 * continues to identify the battery-save store. */
void saves_set_state_options(struct saves *saves, const char *options);
bool saves_checkpoint_options_match(const struct saves *saves);
bool saves_set_legacy_core_name(struct saves *saves, const char *library_name);
void saves_close(struct saves *saves);
bool saves_has_no_generation(struct saves *saves);
/* sram_size is the core's current capacity. A saved SRAM that is not larger than it loads as a prefix
 * and the rest of the core's buffer is left untouched; RTC size must match; a larger one fails. */
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
/* Publish a checkpoint and its resume marker after the generation is durable.
 * If non-null, token receives the exact manifest digest only on success. */
bool saves_commit_checkpoint(struct saves *saves, const void *sram, size_t sram_size,
		const void *rtc, size_t rtc_size, const void *checkpoint, size_t checkpoint_size,
		char token[65]);
enum saves_resume_result saves_resume_probe(struct saves *saves, char token[65]);
/* Load only the exact token's verified checkpoint for the CURRENT generation. state_size and sram_size
 * are the running core's capacities; the committed state and SRAM may be shorter (state_loaded and
 * sram_loaded receive their lengths, the prefix of each buffer that was written) but never longer. */
enum saves_resume_result saves_load_resume(struct saves *saves, const char *token,
		void *state, size_t state_size, size_t *state_loaded,
		void *sram, size_t sram_size, size_t *sram_loaded, void *rtc, size_t rtc_size);
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
	SAVES_TEST_RESUME_READ,
	SAVES_TEST_GENERATION_SCAN,
	SAVES_TEST_SLOT_ALLOC
};
/* Make every read of one file in one generation fail with err: "manifest" or "state-NN". Up
 * to four faults can be active; saves_test_fail_read(0, NULL, 0) clears them all. */
void saves_test_fail_read(uint64_t generation, const char *name, int err);
void saves_test_fail_event(enum saves_test_event event, unsigned occurrence);
void saves_test_crash_event(enum saves_test_event event);
void saves_test_set_hook(void (*hook)(void));
bool saves_test_downgrade_current_manifest(struct saves *saves, unsigned version);
#endif

#endif
