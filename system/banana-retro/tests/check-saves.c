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

int main(void)
{
	char tmp[]="/tmp/banana-retro-saves.XXXXXX";char *base=mkdtemp(tmp);assert(base);
	char root[4096],core[4096],content[4096],content2[4096];assert(snprintf(root,sizeof(root),"%s/native",base)>0);assert(snprintf(core,sizeof(core),"%s/core.so",base)>0);assert(snprintf(content,sizeof(content),"%s/a/rom.bin",base)>0);assert(snprintf(content2,sizeof(content2),"%s/b/rom.bin",base)>0);
	assert(mkdir(strcat(strcpy((char[4096]){0},base),"/a"),0700)==0);assert(mkdir(strcat(strcpy((char[4096]){0},base),"/b"),0700)==0);
	put(core,"core-build-one",14);put(content,"same-name-a",11);put(content2,"same-name-b",11);
	struct saves *s=saves_open(root,core,content);assert(s);uint8_t sram[]={1,2,3,4},rtc[]={9,8},state0[]={3,1,4,1,5};
	assert(!saves_load_memory(s,sram,sizeof(sram),rtc,sizeof(rtc)));
	assert(saves_commit(s,sram,sizeof(sram),rtc,sizeof(rtc),0,state0,sizeof(state0)));
	uint8_t got_sram[4]={0},got_rtc[2]={0},got_state[5]={0};assert(saves_load_memory(s,got_sram,sizeof(got_sram),got_rtc,sizeof(got_rtc)));assert(!memcmp(sram,got_sram,sizeof(sram))&&!memcmp(rtc,got_rtc,sizeof(rtc)));assert(saves_load_state(s,0,got_state,sizeof(got_state))&&!memcmp(state0,got_state,sizeof(state0)));
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
	/* The same save hook is used after normal completion and a signalfd SIGTERM. */
	assert(saves_commit(s,sram,sizeof(sram),rtc,sizeof(rtc),-1,NULL,0));sigset_t mask;sigemptyset(&mask);sigaddset(&mask,SIGTERM);assert(sigprocmask(SIG_BLOCK,&mask,NULL)==0);int sigfd=signalfd(-1,&mask,SFD_CLOEXEC);assert(sigfd>=0);assert(raise(SIGTERM)==0);struct signalfd_siginfo info;assert(read(sigfd,&info,sizeof(info))==(ssize_t)sizeof(info));assert(info.ssi_signo==SIGTERM);assert(saves_commit(s,sram2,sizeof(sram2),rtc,sizeof(rtc),-1,NULL,0));assert(close(sigfd)==0);
	saves_close(collision);saves_close(s);tree_remove(base);puts("banana-retro native save checks passed");return 0;
}
