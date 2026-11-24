#include "native_control.h"
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

static uint64_t processStart()
{
	std::ifstream f("/proc/self/stat");
	std::string line;
	if (!std::getline(f, line)) return 0;
	size_t close = line.rfind(") ");
	if (close == std::string::npos) return 0;
	std::istringstream fields(line.substr(close + 2));
	std::string value;
	for (int field = 3; field <= 22; ++field)
	{
		if (!(fields >> value)) return 0;
		if (field == 22)
		{
			uint64_t result = 0;
			for (char c : value) { if (c < '0' || c > '9') return 0; result = result * 10 + (unsigned)(c - '0'); }
			return result;
		}
	}
	return 0;
}

static bool writeMetadata(const std::string& id, uint64_t start, uint64_t timestamp, const char* state)
{
	std::ofstream f(std::string(getenv("BANANA_RUN_DIR")) + "/native-session");
	f << "v1 " << id << " " << (long)getpid() << " " << start << " mgba " << timestamp << " " << state
	  << " /rom/path with spaces.gba\n";
	return (bool)f;
}

int main()
{
	const std::string id = "0123456789abcdef0123456789abcdef";
	const uint64_t now = (uint64_t)std::time(nullptr), start = processStart();
	if (!start || !writeMetadata(id, start, now, "ready")) return 1;
	banana::NativeControl control;
	if (!control.sessionAttempt(id, "/rom/path with spaces.gba", now)) return 2;
	if (control.sessionAttempt("1123456789abcdef0123456789abcdef", "/rom/path with spaces.gba", now)) return 3;
	if (!writeMetadata(id, start, now, "ready") || control.sessionAttempt(id, "/rom/path with spaces.gba", now + 1)) return 4;
	if (!writeMetadata(id, start + 1, now, "ready") || control.sessionAttempt(id, "/rom/path with spaces.gba", now)) return 5;
	if (!writeMetadata(id, start, now, "starting") || !control.sessionAttempt(id, "/rom/path with spaces.gba", now)) return 6;
	if (control.sessionAttempt(id, "/rom/other.gba", now)) return 7;
	std::cout << "native-control-session-test: stale token, timestamp, PID start and path rejected\n";
	return 0;
}
