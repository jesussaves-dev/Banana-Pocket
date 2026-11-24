#ifndef BANANA_RETRO_CONTROL_H
#define BANANA_RETRO_CONTROL_H

#include <stdint.h>

/* SOCK_SEQPACKET protocol. All integers are unsigned little-endian. The 36-byte
 * header is encoded field by field; this declaration is never sent raw. Requests
 * use the operation type, responses set bit 15, and flags must be zero. Each
 * response payload begins with a u16 result code. HELLO has an empty payload;
 * every request carries this session's 16-byte ID in the header. */
#define BANANA_RETRO_CONTROL_MAGIC "BRTC"
#define BANANA_RETRO_CONTROL_VERSION 1u
#define BANANA_RETRO_CONTROL_HEADER_SIZE 36u
#define BANANA_RETRO_CONTROL_MAX_PAYLOAD 4096u
#define BANANA_RETRO_CONTROL_MAX_PACKET (BANANA_RETRO_CONTROL_HEADER_SIZE + BANANA_RETRO_CONTROL_MAX_PAYLOAD)

enum banana_retro_control_offset {
	BANANA_RETRO_CTL_MAGIC = 0,
	BANANA_RETRO_CTL_VERSION = 4,
	BANANA_RETRO_CTL_TYPE = 6,
	BANANA_RETRO_CTL_REQUEST_ID = 8,
	BANANA_RETRO_CTL_SESSION_ID = 16,
	BANANA_RETRO_CTL_FLAGS = 32,
	BANANA_RETRO_CTL_LENGTH = 34,
};

enum banana_retro_control_type {
	BANANA_RETRO_CTL_HELLO = 1,
	BANANA_RETRO_CTL_GET_STATUS = 2,
	BANANA_RETRO_CTL_PAUSE = 3,
	BANANA_RETRO_CTL_RESUME = 4,
	BANANA_RETRO_CTL_FLUSH_SRAM = 5,
	BANANA_RETRO_CTL_SAVE_STATE = 6,
	BANANA_RETRO_CTL_LOAD_STATE = 7,
	BANANA_RETRO_CTL_GET_OPTIONS = 8,
	BANANA_RETRO_CTL_SET_OPTION = 9,
	BANANA_RETRO_CTL_RESET = 10,
	BANANA_RETRO_CTL_QUIT = 11,
};

enum banana_retro_control_result {
	BANANA_RETRO_CTL_OK = 0,
	BANANA_RETRO_CTL_ACCEPTED = 1,
	BANANA_RETRO_CTL_INVALID = 2,
	BANANA_RETRO_CTL_UNSUPPORTED = 3,
	BANANA_RETRO_CTL_BUSY = 4,
	BANANA_RETRO_CTL_BAD_STATE = 5,
	BANANA_RETRO_CTL_IO_ERROR = 6,
	BANANA_RETRO_CTL_CORE_ERROR = 7,
	BANANA_RETRO_CTL_STALE_SESSION = 8,
};

enum banana_retro_control_owner {
	BANANA_RETRO_CTL_OWNER_UI = 1,
	BANANA_RETRO_CTL_OWNER_SLEEP = 2,
};

enum banana_retro_control_option_scope {
	BANANA_RETRO_CTL_SCOPE_SESSION = 0,
	BANANA_RETRO_CTL_SCOPE_CORE = 1,
	BANANA_RETRO_CTL_SCOPE_GAME = 2,
};

enum banana_retro_control_capability {
	BANANA_RETRO_CTL_CAP_OPTIONS = 1u << 0,
	BANANA_RETRO_CTL_CAP_MEMORY = 1u << 1,
	BANANA_RETRO_CTL_CAP_STATE = 1u << 2,
};

/* Payloads after the common header:
 * PAUSE/RESUME: u8 owner, u64 epoch (zero for UI; increasing nonzero sleep epoch).
 * SAVE_STATE/LOAD_STATE: u32 slot. GET_OPTIONS: u16 offset, u16 descriptor limit.
 * SET_OPTION: u16 key bytes, u16 value bytes, u8 scope, key bytes, value bytes.
 * GET_OPTIONS response body: u16 total, u16 returned, then returned descriptors;
 * each descriptor is four (u16 length, UTF-8 bytes) strings (key, description,
 * current value, default), u8 choice count, u8 flags (bit 0 visible, bit 1 restart
 * required), then that many length-prefixed choices. Page again at offset+returned.
 * GET_STATUS response body: u32 flags (bit 0 ready, bit 1 paused), u32 owner mask,
 * u64 previous completed request ID, u32 capabilities, u32 PCM state, u32 queued
 * PCM frames, u64 underruns, u64 outstanding ID (zero for this synchronous MVP),
 * u64 last mutation ID, u16 last mutation type, u16 last mutation result, then
 * three length-prefixed strings: core name, core version, and content identity.
 */

#endif
