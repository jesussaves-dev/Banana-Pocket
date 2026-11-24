#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace banana
{
struct NativeOption
{
	std::string key, description, value, defaultValue;
	std::vector<std::string> choices;
	bool visible = true, restartRequired = false;
};

struct NativeStatus
{
	uint32_t flags = 0, pauseOwners = 0, capabilities = 0;
	bool ready() const { return (flags & 1u) != 0; }
	bool paused() const { return (flags & 2u) != 0; }
};

class NativeControl
{
public:
	bool connectSession(const std::string& id, const std::string& rom, uint64_t launchEpoch);
	bool sessionAttempt(const std::string& id, const std::string& rom, uint64_t launchEpoch);
	void clear();
	bool active() const { return active_; }
	bool status(NativeStatus& out);
	bool pause(bool acquire);
	int saveState(unsigned slot);
	int loadState(unsigned slot);
	int reset();
	int quit();
	int options(std::vector<NativeOption>& out);
	int setOption(const std::string& key, const std::string& value, bool& restartRequired);

private:
	int request(uint16_t type, const std::vector<unsigned char>& payload,
	            std::vector<unsigned char>& body);
	std::string id_, socket_;
	uint64_t requestId_ = 0;
	bool active_ = false;
};
}
