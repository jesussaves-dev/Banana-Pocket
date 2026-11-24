#include "native_control.h"
extern "C" {
#include "../../system/banana-retro/src/control.h"
}

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

struct Fixture
{
	bool paused = false, restart = false;
	unsigned saved = 0, loaded = 0, resets = 0, quits = 0;
	std::string optionValue = "Auto";
};

static void status(void* data, control_status* out)
{
	Fixture* f = (Fixture*)data;
	out->flags = 1u | (f->paused ? 2u : 0u);
	out->pause_owners = f->paused ? BANANA_RETRO_CTL_OWNER_UI : 0;
	out->capabilities = BANANA_RETRO_CTL_CAP_OPTIONS | BANANA_RETRO_CTL_CAP_STATE;
	out->core_id = "fixture"; out->core_version = "1"; out->content_id = "/rom/protocol.gba";
}
static bool pause(void* data, unsigned owner, uint64_t, bool acquire)
{ Fixture* f=(Fixture*)data; if (owner != BANANA_RETRO_CTL_OWNER_UI) return false; f->paused=acquire; return true; }
static bool flush(void*) { return true; }
static bool save(void* data, unsigned slot) { ((Fixture*)data)->saved=slot+1; return true; }
static bool load(void* data, unsigned slot) { ((Fixture*)data)->loaded=slot+1; return true; }
static size_t optionCount(void*) { return 1; }
static bool option(void*, size_t index, const char** key, const char** description,
		   const char** value, const char** defaultValue, const char* const** choices,
		   size_t* count, bool* visible, bool* restartRequired)
{
	static const char* values[] = { "Auto", "60 Hz" };
	if (index) return false;
	*key="region"; *description="Region"; *value="Auto"; *defaultValue="Auto";
	*choices=values; *count=2; *visible=true; *restartRequired=false;
	return true;
}
static bool setOption(void* data, const char* key, const char* value, unsigned, bool* restart)
{
	Fixture* f=(Fixture*)data;
	if (std::string(key) != "region" || std::string(value) == "unsupported") return false;
	f->optionValue=value; f->restart=true; *restart=true; return true;
}
static bool reset(void* data) { ++((Fixture*)data)->resets; return true; }
static bool quit(void* data) { ++((Fixture*)data)->quits; return true; }

static uint64_t processStart()
{
	std::ifstream f("/proc/self/stat"); std::string line;
	if (!std::getline(f,line)) return 0;
	size_t close=line.rfind(") "); if(close==std::string::npos) return 0;
	std::istringstream fields(line.substr(close+2)); std::string value;
	for(int field=3;field<=22;++field) { if(!(fields>>value)) return 0; if(field==22) return std::strtoull(value.c_str(),nullptr,10); }
	return 0;
}

int main()
{
	const std::string id="abcdef0123456789abcdef0123456789";
	const char* dir=std::getenv("BANANA_RUN_DIR");
	if(!dir || !*dir || !processStart()) return 1;
	std::string socket=std::string(dir)+"/retro-"+id+".sock";
	std::string metadata=std::string(dir)+"/native-session";
	std::ofstream mf(metadata);
	mf<<"v1 "<<id<<" "<<(long)getpid()<<" "<<processStart()<<" fixture "<<(uint64_t)std::time(nullptr)
	  <<" ready /rom/protocol.gba\n";
	mf.close(); if(!mf) return 2;
	if(setenv("BANANA_RETRO_CONTROL_SOCKET",socket.c_str(),1)<0 ||
	   setenv("BANANA_RETRO_SESSION_ID",id.c_str(),1)<0 ||
	   setenv("BANANA_RETRO_CONTROL_UI_PID",std::to_string((long)getpid()).c_str(),1)<0) return 3;
	Fixture fixture;
	control_ops ops{};
	ops.data=&fixture; ops.status=status; ops.pause=pause; ops.flush=flush; ops.save_state=save; ops.load_state=load;
	ops.option_count=optionCount; ops.option=option; ops.set_option=setOption; ops.reset=reset; ops.quit=quit;
	unsigned char session[16]={0xab,0xcd,0xef,0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,0x01,0x23,0x45,0x67,0x89};
	control_server* server=control_open_from_env(&ops,session);
	if(!server)
	{
		if (errno == EPERM) return 77; // sandbox denied local socket bind; control fixtures cover the server independently
		std::perror("control_open_from_env"); return 4;
	}
	std::atomic<bool> stop{false};
	std::thread pump([&]{
		while(!stop.load())
		{
			pollfd fds[3]={{control_listener_fd(server),POLLIN,0},
				{control_client_fd(server,0),POLLIN,0},{control_client_fd(server,1),POLLIN,0}};
			int rc=poll(fds,3,20);
			if(rc<0) continue;
			unsigned mask=0;
			if(fds[1].revents) mask|=1u;
			if(fds[2].revents) mask|=2u;
			control_handle_ready(server,(fds[0].revents&POLLIN)!=0,mask);
		}
	});
	banana::NativeControl client;
	if(!client.connectSession(id,"/rom/protocol.gba",(uint64_t)std::time(nullptr))) return 5;
	banana::NativeStatus current;
	if(!client.status(current)||!current.ready()||!(current.capabilities&BANANA_RETRO_CTL_CAP_STATE)) return 6;
	if(!client.pause(true)||!fixture.paused) return 7;
	if(client.saveState(3)!=BANANA_RETRO_CTL_OK||fixture.saved!=4) return 8;
	if(client.loadState(4)!=BANANA_RETRO_CTL_OK||fixture.loaded!=5) return 9;
	std::vector<banana::NativeOption> options;
	if(client.options(options)!=BANANA_RETRO_CTL_OK||options.size()!=1||options[0].description!="Region"||options[0].choices.size()!=2) return 10;
	bool restartRequired=false;
	if(client.setOption("region","60 Hz",restartRequired)!=BANANA_RETRO_CTL_OK||!restartRequired||fixture.optionValue!="60 Hz") return 11;
	if(client.setOption("region","unsupported",restartRequired)!=BANANA_RETRO_CTL_UNSUPPORTED) return 12;
	if(client.reset()!=BANANA_RETRO_CTL_OK||fixture.resets!=1) return 13;
	if(client.quit()!=BANANA_RETRO_CTL_OK||fixture.quits!=1) return 14;
	if(!client.pause(false)||fixture.paused) return 15;
	stop.store(true); pump.join(); control_close(server);
	std::cout<<"native-control-protocol-test: authenticated UI pause, status, state, options, reset, quit and unsupported result passed\n";
	return 0;
}
