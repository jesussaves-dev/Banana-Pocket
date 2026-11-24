#define _GNU_SOURCE
#include "../src/saves.h"

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void put(const char *path,const void *data,size_t size)
{
	int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);const char *p=data;while(size){ssize_t n=write(fd,p,size);assert(n>0);p+=n;size-=(size_t)n;}assert(fsync(fd)==0);assert(close(fd)==0);
}
static void tree_remove(const char *path)
{
	DIR *d=opendir(path);if(!d){(void)unlink(path);return;}struct dirent *e;while((e=readdir(d))!=NULL){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;char child[4096];int n=snprintf(child,sizeof(child),"%s/%s",path,e->d_name);assert(n>0&&(size_t)n<sizeof(child));tree_remove(child);}closedir(d);assert(rmdir(path)==0);
}
static void only_entry(const char *dir,char *out,size_t size)
{
	DIR *d=opendir(dir);assert(d);struct dirent *e;bool got=false;while((e=readdir(d))!=NULL)if(e->d_name[0]!='.'){int n=snprintf(out,size,"%s/%s",dir,e->d_name);assert(n>0&&(size_t)n<size);got=true;break;}closedir(d);assert(got);
}
static uint64_t current(const char *dir)
{
	char p[4096],b[32];int n=snprintf(p,sizeof(p),"%s/CURRENT",dir);assert(n>0&&(size_t)n<sizeof(p));int fd=open(p,O_RDONLY);assert(fd>=0);ssize_t r=read(fd,b,sizeof(b)-1);assert(r>0);assert(close(fd)==0);b[r]=0;return strtoull(b,NULL,10);
}
static bool serialize_ok(void *ctx,void *buffer,size_t size){memcpy(buffer,ctx,size);return true;}
static bool serialize_fail(void *ctx,void *buffer,size_t size){(void)ctx;(void)buffer;(void)size;return false;}
static bool unserialize_ok(void *ctx,const void *buffer,size_t size){memcpy(ctx,buffer,size);return true;}
static bool unserialize_fail(void *ctx,const void *buffer,size_t size){(void)ctx;(void)buffer;(void)size;return false;}

static char mutate_path[4096],replacement_path[4096];
static bool mutate_as_symlink;
static uint8_t replacement_byte;
static void mutate_after_validation(void)
{
	saves_test_set_hook(NULL);
	if(mutate_as_symlink){assert(unlink(mutate_path)==0);assert(symlink(replacement_path,mutate_path)==0);}
	else put(mutate_path,&replacement_byte,sizeof(replacement_byte));
}

static int runtime_run(const char *runtime,const char *core,const char *rom,
		       const char *root,bool signal_exit,int state_operation)
{
	int pipes[2];assert(pipe(pipes)==0);pid_t pid=fork();assert(pid>=0);
	if(pid==0){
		close(pipes[0]);assert(dup2(pipes[1],STDERR_FILENO)>=0);close(pipes[1]);
		int nullfd=open("/dev/null",O_WRONLY);assert(nullfd>=0);assert(dup2(nullfd,STDOUT_FILENO)>=0);close(nullfd);
		assert(setenv("BANANA_RETRO_NATIVE_DIR",root,1)==0);
		assert(setenv("BANANA_RETRO_TEST_PERSISTENCE","1",1)==0);
		if(signal_exit)assert(setenv("BANANA_RETRO_TEST_WAIT_FOR_SIGNAL","1",1)==0);
		else unsetenv("BANANA_RETRO_TEST_WAIT_FOR_SIGNAL");
		char *args[10];int n=0;args[n++]=(char*)runtime;args[n++]=(char*)core;args[n++]=(char*)rom;args[n++]="--frames";args[n++]=signal_exit?"10000":"2";
		if(state_operation){args[n++]=state_operation==1?"--load-state":"--save-state";args[n++]="0";}args[n]=NULL;
		execv(runtime,args);_exit(127);
	}
	close(pipes[1]);bool saw_run=false;char line[256];size_t used=0;char c;
	while(read(pipes[0],&c,1)==1){if(c=='\n'){line[used]=0;if(strstr(line,"fixture run 1")){saw_run=true;break;}used=0;}else if(used+1<sizeof(line))line[used++]=c;}
	if(signal_exit){assert(saw_run);assert(kill(pid,SIGTERM)==0);}
	while(read(pipes[0],&c,1)==1){}
	close(pipes[0]);int status;assert(waitpid(pid,&status,0)==pid);assert(WIFEXITED(status));	return WEXITSTATUS(status);
}

static bool runtime_has_save(const char *root,const char *core,const char *rom)
{
	struct saves *s=saves_open(root,core,rom);if(!s)return false;bool ok=saves_load_memory(s,NULL,0,NULL,0);saves_close(s);return ok;
}

static void preserve_seed(const char *root,const char *core,const char *rom,uint8_t value)
{
	struct saves *s=saves_open(root,core,rom);assert(s);assert(saves_commit(s,&value,1,NULL,0,-1,NULL,0));saves_close(s);
}

static void assert_saved_byte(const char *root,const char *core,const char *rom,uint8_t value)
{
	struct saves *s=saves_open(root,core,rom);assert(s);uint8_t got=0;assert(saves_load_memory(s,&got,1,NULL,0));assert(got==value);saves_close(s);
}

static unsigned path_components(const char *path)
{
	unsigned count=0;while(*path){while(*path=='/')++path;if(!*path)break;++count;while(*path&&*path!='/')++path;}return count;
}

int main(int argc,char **argv)
{
	char tmp[]="/tmp/banana-retro-saves.XXXXXX";char *base=mkdtemp(tmp);assert(base);
	char root[4096],core[4096],content[4096],content2[4096];assert(snprintf(root,sizeof(root),"%s/native",base)>0);assert(snprintf(core,sizeof(core),"%s/core.so",base)>0);assert(snprintf(content,sizeof(content),"%s/a/rom.bin",base)>0);assert(snprintf(content2,sizeof(content2),"%s/b/rom.bin",base)>0);
	assert(mkdir(strcat(strcpy((char[4096]){0},base),"/a"),0700)==0);assert(mkdir(strcat(strcpy((char[4096]){0},base),"/b"),0700)==0);
	put(core,"core-build-one",14);put(content,"same-name-a",11);put(content2,"same-name-b",11);
	struct saves *s=saves_open(root,core,content);assert(s);uint8_t sram[]={1,2,3,4},rtc[]={9,8},state0[]={3,1,4,1,5};
	assert(!saves_load_memory(s,sram,sizeof(sram),rtc,sizeof(rtc)));
	assert(saves_commit(s,sram,sizeof(sram),rtc,sizeof(rtc),0,state0,sizeof(state0)));
	uint8_t got_sram[4]={0},got_rtc[2]={0},got_state[5]={0};assert(saves_load_memory(s,got_sram,sizeof(got_sram),got_rtc,sizeof(got_rtc)));assert(!memcmp(sram,got_sram,sizeof(sram))&&!memcmp(rtc,got_rtc,sizeof(rtc)));assert(saves_load_state(s,0,got_state,sizeof(got_state))&&!memcmp(state0,got_state,sizeof(state0)));
	/* Preview/import follows production discovery and preserves read-only legacy files. */
	char legacy[4096],legacy_sram[4096],legacy_rtc[4096],legacy_native[4096];
	assert(snprintf(legacy,sizeof(legacy),"%s/legacy",base)>0&&mkdir(legacy,0700)==0);
	assert(snprintf(legacy_sram,sizeof(legacy_sram),"%s/rom.srm",legacy)>0);
	assert(snprintf(legacy_rtc,sizeof(legacy_rtc),"%s/rom.rtc",legacy)>0);
	assert(snprintf(legacy_native,sizeof(legacy_native),"%s/legacy-native",base)>0);
	put(legacy_sram,sram,sizeof(sram));put(legacy_rtc,rtc,sizeof(rtc));
	struct saves *legacy_store=saves_open(legacy_native,core,content);assert(legacy_store);
	assert(saves_legacy_preview(legacy_store,legacy,sizeof(sram),sizeof(rtc))==SAVES_LEGACY_READY);
	assert(chmod(legacy_sram,0400)==0&&chmod(legacy_rtc,0400)==0);
	assert(saves_legacy_import(legacy_store,legacy,sizeof(sram),sizeof(rtc))==SAVES_LEGACY_READY);
	assert(saves_legacy_preview(legacy_store,legacy,sizeof(sram),sizeof(rtc))==SAVES_LEGACY_ALREADY_IMPORTED);
	memset(got_sram,0,sizeof(got_sram));memset(got_rtc,0,sizeof(got_rtc));
	assert(saves_load_memory(legacy_store,got_sram,sizeof(got_sram),got_rtc,sizeof(got_rtc)));
	assert(!memcmp(got_sram,sram,sizeof(sram))&&!memcmp(got_rtc,rtc,sizeof(rtc)));
	int legacy_fd=open(legacy_sram,O_RDONLY);assert(legacy_fd>=0);uint8_t legacy_byte=0;assert(read(legacy_fd,&legacy_byte,1)==1&&legacy_byte==sram[0]);assert(close(legacy_fd)==0);
	saves_close(legacy_store);
	/* Wrong lengths and duplicate root/per-core candidates fail closed. */
	char bad_legacy[4096],bad_sram[4096],bad_core_dir[4096],bad_core_file[4096];
	assert(snprintf(bad_legacy,sizeof(bad_legacy),"%s/legacy-bad",base)>0&&mkdir(bad_legacy,0700)==0);
	assert(snprintf(bad_sram,sizeof(bad_sram),"%s/rom.srm",bad_legacy)>0);put(bad_sram,"x",1);
	struct saves *bad_store=saves_open(legacy_native,core,content2);assert(bad_store);
	assert(saves_legacy_preview(bad_store,bad_legacy,4,0)==SAVES_LEGACY_INVALID);
	put(bad_sram,sram,sizeof(sram));
	assert(snprintf(bad_core_dir,sizeof(bad_core_dir),"%s/core.so",bad_legacy)>0&&mkdir(bad_core_dir,0700)==0);
	assert(snprintf(bad_core_file,sizeof(bad_core_file),"%s/rom.srm",bad_core_dir)>0);put(bad_core_file,sram,sizeof(sram));
	assert(saves_legacy_preview(bad_store,bad_legacy,4,0)==SAVES_LEGACY_AMBIGUOUS);
	saves_close(bad_store);
	/* A second committed generation retains untouched explicit slots. */
	uint8_t sram2[]={7,7,7,7};assert(saves_commit(s,sram2,sizeof(sram2),rtc,sizeof(rtc),-1,NULL,0));memset(got_state,0,sizeof(got_state));assert(saves_load_state(s,0,got_state,sizeof(got_state))&&!memcmp(state0,got_state,sizeof(state0)));
	/* A truncated newest payload is rejected and the previous generation remains loadable. */
	char ident[4096],gen[4096],payload[4096];only_entry(root,ident,sizeof(ident));uint64_t g=current(ident);assert(snprintf(gen,sizeof(gen),"%s/g%016llx",ident,(unsigned long long)g)>0);assert(snprintf(payload,sizeof(payload),"%s/memory",gen)>0);put(payload,"x",1);assert(saves_load_memory(s,got_sram,sizeof(got_sram),got_rtc,sizeof(got_rtc)));assert(!memcmp(got_sram,sram,sizeof(sram)));
	/* Corrupt metadata independently; a newly committed generation still falls back safely. */
	assert(saves_commit(s,sram2,sizeof(sram2),rtc,sizeof(rtc),-1,NULL,0));g=current(ident);assert(snprintf(gen,sizeof(gen),"%s/g%016llx",ident,(unsigned long long)g)>0);assert(snprintf(payload,sizeof(payload),"%s/manifest",gen)>0);put(payload,"bad",3);assert(saves_load_memory(s,got_sram,sizeof(got_sram),got_rtc,sizeof(got_rtc)));assert(!memcmp(got_sram,sram,sizeof(sram)));
	/* Interrupted and ENOSPC-style writes cannot replace CURRENT or its valid generation. */
	saves_test_fail_write_after(2);assert(!saves_commit(s,sram2,sizeof(sram2),rtc,sizeof(rtc),-1,NULL,0));assert(saves_load_memory(s,got_sram,sizeof(got_sram),got_rtc,sizeof(got_rtc)));assert(!memcmp(got_sram,sram,sizeof(sram)));
	saves_test_fail_write_after(sizeof(sram2)+sizeof(rtc));assert(!saves_commit(s,sram2,sizeof(sram2),rtc,sizeof(rtc),-1,NULL,0));assert(saves_load_memory(s,got_sram,sizeof(got_sram),got_rtc,sizeof(got_rtc)));assert(!memcmp(got_sram,sram,sizeof(sram)));
	/* Zero-byte regions serialize and restore without dereferencing null pointers. */
	assert(saves_commit(s,NULL,0,NULL,0,-1,NULL,0));assert(saves_load_memory(s,NULL,0,NULL,0));
	/* Core build and same-basename content identities are isolated. */
	struct saves *collision=saves_open(root,core,content2);assert(collision);uint8_t c[]={42};assert(!saves_load_memory(collision,c,sizeof(c),NULL,0));assert(saves_commit(collision,c,sizeof(c),NULL,0,-1,NULL,0));
	struct saves *optioned=saves_open_with_options(root,core,content,"region=ntsc;rtc=on");assert(optioned);assert(!saves_load_memory(optioned,NULL,0,NULL,0));assert(saves_commit(optioned,sram,sizeof(sram),rtc,sizeof(rtc),-1,NULL,0));struct saves *option_mismatch=saves_open_with_options(root,core,content,"region=pal;rtc=on");assert(option_mismatch);assert(!saves_load_memory(option_mismatch,NULL,0,NULL,0));saves_close(option_mismatch);saves_close(optioned);
	put(core,"core-build-two",14);struct saves *incompatible=saves_open(root,core,content);assert(incompatible);assert(!saves_load_memory(incompatible,NULL,0,NULL,0));saves_close(incompatible);put(core,"core-build-one",14);
	uint8_t serialized[4]={0},decoded[4]={0},source[]={6,2,6,4};assert(saves_state_capture(serialize_ok,source,serialized,sizeof(serialized)));assert(!memcmp(source,serialized,sizeof(source)));assert(!saves_state_capture(serialize_fail,source,serialized,sizeof(serialized)));assert(saves_state_restore(unserialize_ok,decoded,serialized,sizeof(serialized)));assert(!memcmp(source,decoded,sizeof(source)));assert(!saves_state_restore(unserialize_fail,decoded,serialized,sizeof(serialized)));
	/* New nested roots retry parent syncs after a failed mkdir publication. */
	char nested_root[4096],identity_root[4096],existing_root[4096];assert(snprintf(nested_root,sizeof(nested_root),"%s/new/nested/save-root",base)>0);saves_test_fail_event(SAVES_TEST_PARENT_DIR_FSYNC,path_components(base)+1);assert(!saves_open(nested_root,core,content));struct saves *nested=saves_open(nested_root,core,content);assert(nested);uint8_t durable_byte=0x6d;assert(saves_commit(nested,&durable_byte,1,NULL,0,-1,NULL,0));saves_close(nested);assert_saved_byte(nested_root,core,content,durable_byte);
	/* A failed identity-parent sync is retried even though the identity dir exists. */
	assert(snprintf(identity_root,sizeof(identity_root),"%s/identity/nested/root",base)>0);saves_test_fail_event(SAVES_TEST_PARENT_DIR_FSYNC,path_components(identity_root)+1);assert(!saves_open(identity_root,core,content));struct saves *identity=saves_open(identity_root,core,content);assert(identity);assert(saves_commit(identity,&durable_byte,1,NULL,0,-1,NULL,0));saves_close(identity);assert_saved_byte(identity_root,core,content,durable_byte);
	/* A failed sync while reopening an existing identity leaves its generation usable. */
	assert(snprintf(existing_root,sizeof(existing_root),"%s/existing-root",base)>0);preserve_seed(existing_root,core,content,0x39);saves_test_fail_event(SAVES_TEST_PARENT_DIR_FSYNC,path_components(existing_root)+1);assert(!saves_open(existing_root,core,content));assert_saved_byte(existing_root,core,content,0x39);
	/* A rejected payload leaves caller memory untouched when no fallback exists. */
	char invalid_root[4096],invalid_ident[4096],invalid_gen[4096],invalid_payload[4096];assert(snprintf(invalid_root,sizeof(invalid_root),"%s/invalid",base)>0);struct saves *invalid=saves_open(invalid_root,core,content);assert(invalid);uint8_t valid_byte=0x41,untouched=0xa5,state_data[]={8,6,7},state_out[]={0xaa,0xaa,0xaa};assert(saves_commit(invalid,&valid_byte,1,NULL,0,0,state_data,sizeof(state_data)));only_entry(invalid_root,invalid_ident,sizeof(invalid_ident));uint64_t ig=current(invalid_ident);assert(snprintf(invalid_gen,sizeof(invalid_gen),"%s/g%016llx",invalid_ident,(unsigned long long)ig)>0);assert(snprintf(invalid_payload,sizeof(invalid_payload),"%s/memory",invalid_gen)>0);put(invalid_payload,"x",1);assert(!saves_load_memory(invalid,&untouched,1,NULL,0));assert(untouched==0xa5);put(invalid_payload,&valid_byte,1);assert(snprintf(invalid_payload,sizeof(invalid_payload),"%s/state-00",invalid_gen)>0);put(invalid_payload,"bad",3);assert(!saves_load_state(invalid,0,state_out,sizeof(state_out)));assert(state_out[0]==0xaa&&state_out[1]==0xaa&&state_out[2]==0xaa);saves_close(invalid);
	/* Revalidate the exact restore buffer after a deterministic replacement race. */
	char race_root[4096],race_ident[4096],race_gen[4096];assert(snprintf(race_root,sizeof(race_root),"%s/race",base)>0);struct saves *race=saves_open(race_root,core,content);assert(race);uint8_t old_byte=0x31,new_byte=0x72,got_byte=0;assert(saves_commit(race,&old_byte,1,NULL,0,-1,NULL,0));assert(saves_commit(race,&new_byte,1,NULL,0,-1,NULL,0));only_entry(race_root,race_ident,sizeof(race_ident));uint64_t rg=current(race_ident);assert(snprintf(race_gen,sizeof(race_gen),"%s/g%016llx",race_ident,(unsigned long long)rg)>0);assert(snprintf(mutate_path,sizeof(mutate_path),"%s/memory",race_gen)>0);assert(snprintf(replacement_path,sizeof(replacement_path),"%s/replacement",base)>0);replacement_byte=0x55;put(replacement_path,&replacement_byte,1);mutate_as_symlink=false;saves_test_set_hook(mutate_after_validation);assert(saves_load_memory(race,&got_byte,1,NULL,0));assert(got_byte==old_byte);
	assert(saves_commit(race,&new_byte,1,NULL,0,-1,NULL,0));rg=current(race_ident);assert(snprintf(race_gen,sizeof(race_gen),"%s/g%016llx",race_ident,(unsigned long long)rg)>0);assert(snprintf(mutate_path,sizeof(mutate_path),"%s/memory",race_gen)>0);mutate_as_symlink=true;saves_test_set_hook(mutate_after_validation);got_byte=0;assert(saves_load_memory(race,&got_byte,1,NULL,0));assert(got_byte==old_byte);assert(unlink(mutate_path)==0);saves_close(race);
	/* Every failed pre-publication operation recovers the old complete generation. */
	struct {enum saves_test_event event;unsigned occurrence;bool current_may_advance;} failures[]={
		{SAVES_TEST_FILE_FSYNC,1,false},{SAVES_TEST_FILE_FSYNC,2,false},
		{SAVES_TEST_FILE_FSYNC,3,false},{SAVES_TEST_TEMP_DIR_FSYNC,1,false},
		{SAVES_TEST_GENERATION_RENAME,1,false},{SAVES_TEST_PARENT_DIR_FSYNC,1,false},
		{SAVES_TEST_CURRENT_RENAME,1,false},{SAVES_TEST_PARENT_DIR_FSYNC,2,true}};
	for(size_t i=0;i<sizeof(failures)/sizeof(failures[0]);i++){char fr[4096];assert(snprintf(fr,sizeof(fr),"%s/fault-%zu",base,i)>0);preserve_seed(fr,core,content,0x11);struct saves *fs=saves_open(fr,core,content);assert(fs);saves_test_fail_event(failures[i].event,failures[i].occurrence);assert(!saves_commit(fs,"\x22",1,NULL,0,-1,NULL,0));saves_close(fs);assert_saved_byte(fr,core,content,failures[i].current_may_advance?0x22:0x11);}
	/* Partial data and manifest writes also leave the last committed generation intact. */
	for(size_t budget_i=0;budget_i<2;budget_i++){char fr[4096];assert(snprintf(fr,sizeof(fr),"%s/short-%zu",base,budget_i)>0);preserve_seed(fr,core,content,0x11);struct saves *fs=saves_open(fr,core,content);assert(fs);saves_test_fail_write_after(budget_i?6:0);assert(!saves_commit(fs,"\x22",1,NULL,0,-1,NULL,0));saves_close(fs);assert_saved_byte(fr,core,content,0x11);}
	/* Kill a child at each publication boundary, then recover by reopening storage. */
	enum saves_test_event crashes[]={SAVES_TEST_AFTER_MANIFEST_WRITE,SAVES_TEST_AFTER_GENERATION_RENAME,SAVES_TEST_AFTER_CURRENT_RENAME};
	for(size_t i=0;i<sizeof(crashes)/sizeof(crashes[0]);i++){char cr[4096];assert(snprintf(cr,sizeof(cr),"%s/crash-%zu",base,i)>0);preserve_seed(cr,core,content,0x11);pid_t child=fork();assert(child>=0);if(child==0){struct saves *cs=saves_open(cr,core,content);if(!cs)_exit(1);saves_test_crash_event(crashes[i]);(void)saves_commit(cs,"\x22",1,NULL,0,-1,NULL,0);_exit(2);}int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86);assert_saved_byte(cr,core,content,i==2?0x22:0x11);}
	/* Exercise the real executable's normal-exit and SIGTERM persistence path. */
	assert(argc==7);const char *runtime=argv[1],*fixture=argv[2];char rom[4096],rr[4096];assert(snprintf(rom,sizeof(rom),"%s/runtime.rom",base)>0);put(rom,"BRC1\n",5);assert(snprintf(rr,sizeof(rr),"%s/runtime-normal",base)>0);assert(runtime_run(runtime,fixture,rom,rr,false,0)==0);assert(runtime_has_save(rr,fixture,rom));assert(snprintf(rr,sizeof(rr),"%s/runtime-term",base)>0);assert(runtime_run(runtime,fixture,rom,rr,true,0)==0);assert(runtime_has_save(rr,fixture,rom));
	for(int i=3;i<7;i++){assert(snprintf(rr,sizeof(rr),"%s/runtime-optional-%d",base,i)>0);assert(runtime_run(runtime,argv[i],rom,rr,false,0)==0);assert(runtime_has_save(rr,argv[i],rom));}
	assert(snprintf(rr,sizeof(rr),"%s/runtime-state-load",base)>0);assert(runtime_run(runtime,argv[3],rom,rr,false,1)!=0);assert(!runtime_has_save(rr,argv[3],rom));assert(snprintf(rr,sizeof(rr),"%s/runtime-state-save",base)>0);assert(runtime_run(runtime,argv[3],rom,rr,false,2)!=0);assert(!runtime_has_save(rr,argv[3],rom));
	saves_close(collision);saves_close(s);tree_remove(base);puts("banana-retro native save checks passed");return 0;
}
