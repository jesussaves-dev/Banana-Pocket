/* Exercise the daemon's wire client separately from the lifecycle policy mock. */
#define main bananad_main
#include "../bananad.c"
#undef main
#include <assert.h>

static void check_reply(int size, int expected)
{
	int pair[2];
	assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0);
	unsigned char session[16] = {7};
	pid_t peer = fork();
	assert(peer >= 0);
	if (!peer) {
		close(pair[0]);
		unsigned char packet[BANANA_RETRO_CONTROL_HEADER_SIZE + 32] = {0};
		assert(recv(pair[1], packet, sizeof(packet), 0) == BANANA_RETRO_CONTROL_HEADER_SIZE);
		assert(retro_get16(packet + BANANA_RETRO_CTL_TYPE) == BANANA_RETRO_CTL_CHECKPOINT);
		retro_put16(packet + BANANA_RETRO_CTL_TYPE, BANANA_RETRO_CTL_CHECKPOINT | 0x8000u);
		retro_put16(packet + BANANA_RETRO_CTL_LENGTH, 2);
		if (size) assert(send(pair[1], packet, size, 0) == size);
		close(pair[1]);
		_exit(0);
	}
	close(pair[1]);
	assert(retro_send_request(pair[0], session, BANANA_RETRO_CTL_CHECKPOINT, 1, 0,
		mono() + 1.5) == expected);
	close(pair[0]);
	int status;
	assert(waitpid(peer, &status, 0) == peer && WIFEXITED(status) && !WEXITSTATUS(status));
}

static int request(void *data, uint16_t op, uint64_t epoch, unsigned timeout)
{
	assert(op == BANANA_RETRO_CTL_CHECKPOINT && !epoch && timeout == 1500);
	return *(int *)data;
}
static unsigned legacy_calls;
static int legacy_save(pid_t game) { ++legacy_calls; return game == 123; }

int main(void)
{
	int result = RETRO_POWER_OK;
	struct retro_power_ops ops = { .request = request, .data = &result };
	assert(session_checkpoint(&ops, 123, legacy_save));
	result = RETRO_POWER_FAILED;
	assert(!session_checkpoint(&ops, 123, legacy_save) && !legacy_calls);
	result = RETRO_POWER_NONE;
	assert(session_checkpoint(&ops, 123, legacy_save) && legacy_calls == 1);
	check_reply(BANANA_RETRO_CONTROL_HEADER_SIZE + 2, BANANA_RETRO_CTL_OK);
	check_reply(1, -1);
	check_reply(0, -1);
	check_reply(BANANA_RETRO_CONTROL_HEADER_SIZE + 32, -1);
	return 0;
}
