#include "native_control.h"
#include "banana-retro-control.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>

namespace banana
{
namespace
{
static std::string runtimeDirectory()
{
	const char* value = std::getenv("BANANA_RUN_DIR");
	return value && *value ? value : "/run/banana";
}
static uint16_t get16(const unsigned char* p) { return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8); }
static uint32_t get32(const unsigned char* p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const unsigned char* p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }
static void put16(unsigned char* p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char* p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static void put64(unsigned char* p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static bool processStart(pid_t pid, uint64_t& start)
{
	std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
	std::string line;
	if (!std::getline(f, line)) return false;
	size_t close = line.rfind(") ");
	if (close == std::string::npos) return false;
	std::istringstream fields(line.substr(close + 2));
	std::string value;
	for (int field = 3; field <= 22; ++field)
	{
		if (!(fields >> value)) return false;
		if (field == 22)
		{
			if (value.empty()) return false;
			start = 0;
			for (char c : value)
			{
				if (c < '0' || c > '9' || start > (UINT64_MAX - (unsigned)(c - '0')) / 10) return false;
				start = start * 10 + (unsigned)(c - '0');
			}
			return true;
		}
	}
	return false;
}

static bool readString(const std::vector<unsigned char>& bytes, size_t& at, std::string& out)
{
	if (at + 2 > bytes.size()) return false;
	uint16_t n = get16(bytes.data() + at);
	at += 2;
	if (n > bytes.size() - at) return false;
	out.assign(reinterpret_cast<const char*>(bytes.data() + at), n);
	at += n;
	return true;
}
}

bool NativeControl::sessionAttempt(const std::string& id, const std::string& rom, uint64_t launchEpoch)
{
	if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
	std::ifstream metadata(runtimeDirectory() + "/native-session");
	std::string version, token, core, state, path;
	long long pid = 0;
	uint64_t recordedStart = 0, timestamp = 0;
	if (!(metadata >> version >> token >> pid >> recordedStart >> core >> timestamp >> state) || version != "v1" || token != id || pid <= 0)
		return false;
	std::getline(metadata, path);
	if (!path.empty() && path[0] == ' ') path.erase(0, 1);
	if (path != rom || timestamp < launchEpoch) return false;
	uint64_t actualStart = 0;
	if (!processStart((pid_t)pid, actualStart) || actualStart != recordedStart) return false;
	if (timestamp > (uint64_t)time(nullptr) + 2u) return false;
	return state == "starting" || state == "ready";
}

bool NativeControl::connectSession(const std::string& id, const std::string& rom, uint64_t launchEpoch)
{
	clear();
	if (!sessionAttempt(id, rom, launchEpoch)) return false;
	std::ifstream metadata(runtimeDirectory() + "/native-session");
	std::string version, token, core, state, path;
	long long pid = 0;
	uint64_t recordedStart = 0, timestamp = 0;
	if (!(metadata >> version >> token >> pid >> recordedStart >> core >> timestamp >> state) ||
	    version != "v1" || token != id || state != "ready") return false;
	if (requestSessionId_ != id)
	{
		requestSessionId_ = id;
		requestId_ = 0;
	}
	id_ = id;
	socket_ = runtimeDirectory() + "/retro-" + id + ".sock";
	NativeStatus current;
	active_ = status(current) && current.ready();
	return active_;
}

void NativeControl::clear()
{
	id_.clear(); socket_.clear(); active_ = false;
}

int NativeControl::request(uint16_t type, const std::vector<unsigned char>& payload,
			   std::vector<unsigned char>& body, bool* mutationSent)
{
	if (mutationSent) *mutationSent = false;
	if (id_.empty() || requestId_ > UINT64_MAX - 2 || payload.size() > BANANA_RETRO_CONTROL_MAX_PAYLOAD) return -1;
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	timeval timeout{ 1, 500000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	if (socket_.size() >= sizeof(addr.sun_path)) { close(fd); return -1; }
	std::memcpy(addr.sun_path, socket_.c_str(), socket_.size() + 1);
	if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { close(fd); return -1; }
	auto sendPacket = [&](uint16_t op, const std::vector<unsigned char>& data, uint64_t rid) {
		std::vector<unsigned char> p(BANANA_RETRO_CONTROL_HEADER_SIZE + data.size());
		std::memcpy(p.data(), BANANA_RETRO_CONTROL_MAGIC, 4);
		put16(p.data() + BANANA_RETRO_CTL_VERSION, BANANA_RETRO_CONTROL_VERSION);
		put16(p.data() + BANANA_RETRO_CTL_TYPE, op);
		put64(p.data() + BANANA_RETRO_CTL_REQUEST_ID, rid);
		for (size_t i = 0; i < 16; ++i)
		{
			unsigned a = id_[i * 2] <= '9' ? id_[i * 2] - '0' : id_[i * 2] - 'a' + 10;
			unsigned b = id_[i * 2 + 1] <= '9' ? id_[i * 2 + 1] - '0' : id_[i * 2 + 1] - 'a' + 10;
			p[BANANA_RETRO_CTL_SESSION_ID + i] = (unsigned char)((a << 4) | b);
		}
		put16(p.data() + BANANA_RETRO_CTL_FLAGS, 0);
		put16(p.data() + BANANA_RETRO_CTL_LENGTH, (uint16_t)data.size());
		if (!data.empty()) std::memcpy(p.data() + BANANA_RETRO_CONTROL_HEADER_SIZE, data.data(), data.size());
		return send(fd, p.data(), p.size(), MSG_NOSIGNAL) == (ssize_t)p.size();
	};
	auto receive = [&](uint16_t op, uint64_t rid, std::vector<unsigned char>& data) {
		unsigned char p[BANANA_RETRO_CONTROL_MAX_PACKET];
		ssize_t n = recv(fd, p, sizeof(p), 0);
		if (n < (ssize_t)(BANANA_RETRO_CONTROL_HEADER_SIZE + 2) ||
		    std::memcmp(p + BANANA_RETRO_CTL_MAGIC, BANANA_RETRO_CONTROL_MAGIC, 4) != 0 ||
		    get16(p + BANANA_RETRO_CTL_VERSION) != BANANA_RETRO_CONTROL_VERSION ||
		    get16(p + BANANA_RETRO_CTL_TYPE) != (uint16_t)(op | 0x8000u) ||
		    get64(p + BANANA_RETRO_CTL_REQUEST_ID) != rid ||
		    get16(p + BANANA_RETRO_CTL_LENGTH) != (uint16_t)(n - BANANA_RETRO_CONTROL_HEADER_SIZE)) return -1;
		unsigned char expected[16];
		for (size_t i = 0; i < 16; ++i)
		{
			unsigned a = id_[i * 2] <= '9' ? id_[i * 2] - '0' : id_[i * 2] - 'a' + 10;
			unsigned b = id_[i * 2 + 1] <= '9' ? id_[i * 2 + 1] - '0' : id_[i * 2 + 1] - 'a' + 10;
			expected[i] = (unsigned char)((a << 4) | b);
		}
		if (std::memcmp(p + BANANA_RETRO_CTL_SESSION_ID, expected, 16) != 0) return -1;
		uint16_t result = get16(p + BANANA_RETRO_CONTROL_HEADER_SIZE);
		data.assign(p + BANANA_RETRO_CONTROL_HEADER_SIZE + 2, p + n);
		return (int)result;
	};
	uint64_t hello = ++requestId_;
	std::vector<unsigned char> empty, ignored;
	if (!sendPacket(BANANA_RETRO_CTL_HELLO, empty, hello) || receive(BANANA_RETRO_CTL_HELLO, hello, ignored) != BANANA_RETRO_CTL_OK)
	{ close(fd); return -1; }
	uint64_t rid = ++requestId_;
	if (!sendPacket(type, payload, rid)) { close(fd); return -1; }
	if (mutationSent) *mutationSent = true;
	int result = receive(type, rid, body);
	close(fd);
	return result;
}

bool NativeControl::status(NativeStatus& out)
{
	std::vector<unsigned char> body;
	if (request(BANANA_RETRO_CTL_GET_STATUS, {}, body) != BANANA_RETRO_CTL_OK || body.size() < 56) return false;
	out.flags = get32(body.data());
	out.pauseOwners = get32(body.data() + 4);
	out.capabilities = get32(body.data() + 16);
	return true;
}

NativePauseResult NativeControl::pause(bool acquire)
{
	std::vector<unsigned char> payload(9, 0), body;
	payload[0] = BANANA_RETRO_CTL_OWNER_UI;
	const uint16_t type = acquire ? BANANA_RETRO_CTL_PAUSE : BANANA_RETRO_CTL_RESUME;
	for (unsigned attempt = 0; attempt < 2; ++attempt)
	{
		bool sent = false;
		int result = request(type, payload, body, &sent);
		if (result == BANANA_RETRO_CTL_OK) return NativePauseResult::Applied;
		if (result >= 0 && result != BANANA_RETRO_CTL_STALE_SESSION)
			return NativePauseResult::Rejected;
		if (!sent) return NativePauseResult::Rejected;

		NativeStatus current;
		if (!status(current)) return NativePauseResult::Ambiguous;
		bool held = (current.pauseOwners & BANANA_RETRO_CTL_OWNER_UI) != 0;
		if (held == acquire) return NativePauseResult::Applied;
	}
	return NativePauseResult::Ambiguous;
}

int NativeControl::saveState(unsigned slot)
{
	std::vector<unsigned char> p(4), body;
	put32(p.data(), slot);
	return request(BANANA_RETRO_CTL_SAVE_STATE, p, body);
}
int NativeControl::loadState(unsigned slot)
{
	std::vector<unsigned char> p(4), body;
	put32(p.data(), slot);
	return request(BANANA_RETRO_CTL_LOAD_STATE, p, body);
}
int NativeControl::reset() { std::vector<unsigned char> b; return request(BANANA_RETRO_CTL_RESET, {}, b); }
int NativeControl::quit() { std::vector<unsigned char> b; return request(BANANA_RETRO_CTL_QUIT, {}, b); }

int NativeControl::options(std::vector<NativeOption>& out)
{
	out.clear();
	for (uint16_t offset = 0; ;)
	{
		std::vector<unsigned char> p(4), body;
		put16(p.data(), offset); put16(p.data() + 2, 16);
		int result = request(BANANA_RETRO_CTL_GET_OPTIONS, p, body);
		if (result != BANANA_RETRO_CTL_OK) return result;
		if (body.size() < 4) return -1;
		uint16_t total = get16(body.data()), returned = get16(body.data() + 2);
		size_t at = 4;
		for (uint16_t n = 0; n < returned; ++n)
		{
			NativeOption opt;
			if (!readString(body, at, opt.key) || !readString(body, at, opt.description) ||
			    !readString(body, at, opt.value) || !readString(body, at, opt.defaultValue) || at + 2 > body.size()) return -1;
			unsigned choices = body[at++], flags = body[at++];
			opt.visible = (flags & 1u) != 0; opt.restartRequired = (flags & 2u) != 0;
			for (unsigned choice = 0; choice < choices; ++choice)
			{
				std::string value;
				if (!readString(body, at, value)) return -1;
				opt.choices.push_back(std::move(value));
			}
			if (opt.visible) out.push_back(std::move(opt));
		}
		if (offset + returned >= total) return BANANA_RETRO_CTL_OK;
		if (!returned || offset > UINT16_MAX - returned) return -1;
		offset = (uint16_t)(offset + returned);
	}
}

int NativeControl::setOption(const std::string& key, const std::string& value, bool& restartRequired)
{
	if (key.size() > UINT16_MAX || value.size() > UINT16_MAX) return BANANA_RETRO_CTL_INVALID;
	std::vector<unsigned char> p(5 + key.size() + value.size()), body;
	put16(p.data(), (uint16_t)key.size()); put16(p.data() + 2, (uint16_t)value.size());
	p[4] = BANANA_RETRO_CTL_SCOPE_SESSION;
	std::memcpy(p.data() + 5, key.data(), key.size());
	std::memcpy(p.data() + 5 + key.size(), value.data(), value.size());
	int result = request(BANANA_RETRO_CTL_SET_OPTION, p, body);
	if (result == BANANA_RETRO_CTL_OK)
	{
		if (body.size() != 1) return -1;
		restartRequired = body[0] != 0;
	}
	return result;
}
}
