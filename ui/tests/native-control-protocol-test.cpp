#include "native_control.h"
extern "C" {
#include "../../system/banana-retro/src/control.h"
}

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

struct Fixture
{
	std::atomic<bool> paused{false}, delayNextPause{false};
	std::atomic<unsigned> pauseTransitions{0}, saved{0}, loaded{0}, resets{0}, quits{0};
	std::mutex optionMutex;
	std::string optionValue = "Auto";
};

static const char* const regionChoices[] = { "Auto", "60 Hz" };

static void status(void* data, control_status* out)
{
	Fixture* f = (Fixture*)data;
	const bool paused = f->paused.load();
	out->flags = 1u | (paused ? 2u : 0u);
	out->pause_owners = paused ? BANANA_RETRO_CTL_OWNER_UI : 0;
	out->capabilities = BANANA_RETRO_CTL_CAP_OPTIONS | BANANA_RETRO_CTL_CAP_STATE;
	out->core_id = "fixture"; out->core_version = "1"; out->content_id = "/rom/protocol.gba";
}
static bool pause(void* data, unsigned owner, uint64_t, bool acquire)
{
	Fixture* f = (Fixture*)data;
	if (owner != BANANA_RETRO_CTL_OWNER_UI) return false;
	if (f->delayNextPause.exchange(false))
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1800));
	}
	if (f->paused.exchange(acquire) != acquire) ++f->pauseTransitions;
	return true;
}
static bool flush(void*) { return true; }
static bool save(void* data, unsigned slot) { ((Fixture*)data)->saved = slot + 1; return true; }
static bool load(void* data, unsigned slot) { ((Fixture*)data)->loaded = slot + 1; return true; }
static size_t optionCount(void*) { return 1; }
static bool option(void* data, size_t index, const char** key, const char** description,
		   const char** value, const char** defaultValue, const char* const** choices,
		   size_t* count, bool* visible, bool* restartRequired)
{
	if (index) return false;
	Fixture* f = (Fixture*)data;
	*key = "region"; *description = "Region";
	{
		std::lock_guard<std::mutex> lock(f->optionMutex);
		*value = f->optionValue.c_str();
	}
	*defaultValue = "Auto";
	*choices = regionChoices; *count = sizeof(regionChoices) / sizeof(regionChoices[0]);
	*visible = true; *restartRequired = false;
	return true;
}
static bool setOption(void* data, const char* key, const char* value, unsigned scope, bool* restart)
{
	Fixture* f = (Fixture*)data;
	if (!key || !value || !restart) return false;
	*restart = false;
	if (scope != BANANA_RETRO_CTL_SCOPE_SESSION || std::strcmp(key, "region") != 0) return false;
	bool validValue = false;
	for (const char* choice : regionChoices)
		if (std::strcmp(value, choice) == 0) validValue = true;
	if (!validValue) return false;
	std::lock_guard<std::mutex> lock(f->optionMutex);
	*restart = f->optionValue != value;
	if (*restart) f->optionValue = value;
	return true;
}
static bool reset(void* data) { ++((Fixture*)data)->resets; return true; }
static bool quit(void* data) { ++((Fixture*)data)->quits; return true; }

static uint64_t processStart()
{
	std::ifstream f("/proc/self/stat"); std::string line;
	if (!std::getline(f, line)) return 0;
	size_t close = line.rfind(") "); if (close == std::string::npos) return 0;
	std::istringstream fields(line.substr(close + 2)); std::string value;
	for (int field = 3; field <= 22; ++field)
	{
		if (!(fields >> value)) return 0;
		if (field == 22) return std::strtoull(value.c_str(), nullptr, 10);
	}
	return 0;
}

class MetadataFile
{
public:
	explicit MetadataFile(std::string path) : path_(std::move(path)) {}
	~MetadataFile() { if (!path_.empty()) (void)unlink(path_.c_str()); }
	const std::string& path() const { return path_; }
private:
	std::string path_;
};

class ServerPump
{
public:
	explicit ServerPump(control_server* server) : server_(server) {}
	~ServerPump() { stopAndClose(); }
	ServerPump(const ServerPump&) = delete;
	ServerPump& operator=(const ServerPump&) = delete;

	void start()
	{
		worker_ = std::thread([this] {
			try
			{
				while (!stop_.load())
				{
					pollfd fds[3] = {
						{ control_listener_fd(server_), POLLIN, 0 },
						{ control_client_fd(server_, 0), POLLIN, 0 },
						{ control_client_fd(server_, 1), POLLIN, 0 }
					};
					int rc = poll(fds, 3, 20);
					if (rc < 0)
					{
						if (errno == EINTR) continue;
						workerError_.store(errno);
						break;
					}
					unsigned mask = 0;
					if (fds[1].revents) mask |= 1u;
					if (fds[2].revents) mask |= 2u;
					control_handle_ready(server_, (fds[0].revents & POLLIN) != 0, mask);
				}
			}
			catch (...)
			{
				workerException_.store(true);
			}
		});
	}

	int workerError() const { return workerError_.load(); }
	bool workerException() const { return workerException_.load(); }
	void stopAndClose()
	{
		stop_.store(true);
		if (worker_.joinable()) worker_.join();
		if (server_)
		{
			control_close(server_);
			server_ = nullptr;
		}
	}

private:
	control_server* server_;
	std::atomic<bool> stop_{false};
	std::atomic<int> workerError_{0};
	std::atomic<bool> workerException_{false};
	std::thread worker_;
};

#define REQUIRE(stage, expression) \
	do { if (!(expression)) { std::cerr << "native-control-protocol-test: FAIL at " << stage; \
		if (pump.workerError()) std::cerr << " (server worker poll errno " << pump.workerError() << ")"; \
		if (pump.workerException()) std::cerr << " (server worker threw an exception)"; \
		std::cerr << '\n'; return 1; } } while (0)

int main()
{
	const std::string id = "abcdef0123456789abcdef0123456789";
	const char* dir = std::getenv("BANANA_RUN_DIR");
	if (!dir || !*dir || !processStart())
	{
		std::cerr << "native-control-protocol-test: BANANA_RUN_DIR and /proc process identity are required\n";
		return 1;
	}
	const std::string socket = std::string(dir) + "/retro-" + id + ".sock";
	MetadataFile metadata(std::string(dir) + "/native-session");
	std::ofstream mf(metadata.path());
	mf << "v1 " << id << " " << (long)getpid() << " " << processStart() << " fixture "
	   << (uint64_t)std::time(nullptr) << " ready /rom/protocol.gba\n";
	mf.close();
	if (!mf)
	{
		std::cerr << "native-control-protocol-test: failed to write session metadata " << metadata.path() << '\n';
		return 2;
	}
	if (setenv("BANANA_RETRO_CONTROL_SOCKET", socket.c_str(), 1) < 0 ||
	    setenv("BANANA_RETRO_SESSION_ID", id.c_str(), 1) < 0 ||
	    setenv("BANANA_RETRO_CONTROL_UI_PID", std::to_string((long)getpid()).c_str(), 1) < 0)
	{
		std::perror("native-control-protocol-test: setenv");
		return 3;
	}
	Fixture fixture;
	control_ops ops{};
	ops.data = &fixture; ops.status = status; ops.pause = pause; ops.flush = flush; ops.save_state = save; ops.load_state = load;
	ops.option_count = optionCount; ops.option = option; ops.set_option = setOption; ops.reset = reset; ops.quit = quit;
	unsigned char session[16] = { 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89,
				      0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89 };
	control_server* server = control_open_from_env(&ops, session);
	if (!server)
	{
		const int error = errno;
		if (error == EPERM)
		{
			std::cerr << "skip: native-control-protocol-test (sandbox denied local socket bind)\n";
			return 77;
		}
		errno = error;
		std::perror("native-control-protocol-test: control_open_from_env");
		return 4;
	}
	ServerPump pump(server);
	try
	{
		pump.start();
		banana::NativeControl client;
		REQUIRE("connect initial session", client.connectSession(id, "/rom/protocol.gba", (uint64_t)std::time(nullptr)));
		banana::NativeStatus current;
		REQUIRE("read ready status", client.status(current) && current.ready() && (current.capabilities & BANANA_RETRO_CTL_CAP_STATE));
		REQUIRE("acquire UI pause", client.pause(true) == banana::NativePauseResult::Applied && fixture.paused.load());
		REQUIRE("save state slot 3", client.saveState(3) == BANANA_RETRO_CTL_OK && fixture.saved.load() == 4);
		REQUIRE("load state slot 4", client.loadState(4) == BANANA_RETRO_CTL_OK && fixture.loaded.load() == 5);
		std::vector<banana::NativeOption> options;
		REQUIRE("read options", client.options(options) == BANANA_RETRO_CTL_OK && options.size() == 1 &&
			options[0].description == "Region" && options[0].choices.size() == 2);
		bool restartRequired = false;
		const int invalidOptionResult = client.setOption("region", "not-a-choice", restartRequired);
		if (invalidOptionResult != BANANA_RETRO_CTL_INVALID)
		{
			std::cerr << "native-control-protocol-test: FAIL at reject invalid session option: expected INVALID("
				  << BANANA_RETRO_CTL_INVALID << "), actual " << invalidOptionResult;
			if (pump.workerError()) std::cerr << " (server worker poll errno " << pump.workerError() << ")";
			if (pump.workerException()) std::cerr << " (server worker threw an exception)";
			std::cerr << '\n';
			return 1;
		}
		{
			std::lock_guard<std::mutex> lock(fixture.optionMutex);
			REQUIRE("invalid option preserves current value", fixture.optionValue == "Auto");
		}
		const int unknownKeyResult = client.setOption("unknown-region", "60 Hz", restartRequired);
		if (unknownKeyResult != BANANA_RETRO_CTL_INVALID)
		{
			std::cerr << "native-control-protocol-test: FAIL at reject unknown option key: expected INVALID("
				  << BANANA_RETRO_CTL_INVALID << "), actual " << unknownKeyResult << '\n';
			return 1;
		}
		{
			std::lock_guard<std::mutex> lock(fixture.optionMutex);
			REQUIRE("unknown key preserves current value", fixture.optionValue == "Auto");
		}
		restartRequired = true;
		REQUIRE("setting existing option value", client.setOption("region", "Auto", restartRequired) == BANANA_RETRO_CTL_OK && !restartRequired);
		restartRequired = false;
		REQUIRE("change supported option", client.setOption("region", "60 Hz", restartRequired) == BANANA_RETRO_CTL_OK && restartRequired);
		{
			std::lock_guard<std::mutex> lock(fixture.optionMutex);
			REQUIRE("observe updated option", fixture.optionValue == "60 Hz");
		}
		REQUIRE("reset core", client.reset() == BANANA_RETRO_CTL_OK && fixture.resets.load() == 1);
		REQUIRE("request quit", client.quit() == BANANA_RETRO_CTL_OK && fixture.quits.load() == 1);
		REQUIRE("release UI pause", client.pause(false) == banana::NativePauseResult::Applied && !fixture.paused.load());
		for (unsigned cycle = 0; cycle < 3; ++cycle)
		{
			REQUIRE("reacquire UI pause", client.pause(true) == banana::NativePauseResult::Applied && fixture.paused.load());
			client.clear(); // reopening the pause menu reconnects to the same native session
			REQUIRE("reconnect same session", client.connectSession(id, "/rom/protocol.gba", (uint64_t)std::time(nullptr)));
			REQUIRE("release UI pause after reconnect", client.pause(false) == banana::NativePauseResult::Applied && !fixture.paused.load());
		}
		fixture.delayNextPause = true;
		REQUIRE("reconcile delayed pause acknowledgment", client.pause(true) == banana::NativePauseResult::Applied && fixture.paused.load());
		REQUIRE("resume after delayed acknowledgment", client.pause(false) == banana::NativePauseResult::Applied && !fixture.paused.load());
		REQUIRE("verify pause transition count", fixture.pauseTransitions.load() == 10);
		REQUIRE("server worker remained healthy", pump.workerError() == 0 && !pump.workerException());
		pump.stopAndClose();
		std::cout << "native-control-protocol-test: authenticated pause cycles, same-session reconnects, delayed/lost acknowledgment reconciliation and control operations passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "native-control-protocol-test: exception during protocol fixture: " << error.what();
		if (pump.workerError()) std::cerr << " (server worker poll errno " << pump.workerError() << ")";
		if (pump.workerException()) std::cerr << " (server worker threw an exception)";
		std::cerr << '\n';
		return 1;
	}
	catch (...)
	{
		std::cerr << "native-control-protocol-test: unknown exception during protocol fixture";
		if (pump.workerError()) std::cerr << " (server worker poll errno " << pump.workerError() << ")";
		if (pump.workerException()) std::cerr << " (server worker threw an exception)";
		std::cerr << '\n';
		return 1;
	}
}
