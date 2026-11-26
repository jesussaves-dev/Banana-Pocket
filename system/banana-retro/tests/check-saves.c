#define _GNU_SOURCE
#include "../src/saves.h"

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/statvfs.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Model the squashfs ancestor of a writable data mount, without a host mount. */
static struct stat immutable_root;
static unsigned immutable_syncs;
static bool on_immutable_root(int fd)
{
	struct stat st;
	return immutable_root.st_ino && fstat(fd, &st) == 0 &&
		st.st_dev == immutable_root.st_dev && st.st_ino == immutable_root.st_ino;
}
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
	if (on_immutable_root(fd)) { ++immutable_syncs; errno = EINVAL; return -1; }
	return __real_fsync(fd);
}
int __real_fstatvfs(int fd, struct statvfs *fs);
int __wrap_fstatvfs(int fd, struct statvfs *fs)
{
	int rc = __real_fstatvfs(fd, fs);
	if (rc == 0 && on_immutable_root(fd)) fs->f_flag |= ST_RDONLY;
	return rc;
}

static void put(const char *path,const void *data,size_t size)
{
	int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);const char *p=data;while(size){ssize_t n=write(fd,p,size);assert(n>0);p+=n;size-=(size_t)n;}assert(fsync(fd)==0);assert(close(fd)==0);
}
static uint8_t *read_bytes(const char *path,size_t *size)
{
	int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);assert(fd>=0);struct stat st;
	assert(fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>0);
	*size=(size_t)st.st_size;uint8_t *data=malloc(*size);assert(data);
	size_t off=0;while(off<*size){ssize_t n=read(fd,data+off,*size-off);assert(n>0);off+=(size_t)n;}
	assert(close(fd)==0);return data;
}
static void check_profile_fixture(const char *base,const char *fixtures,const char *profile,
				  const char *library,const char *rom_name,
				  const char *sram_name,const char *rtc_name,
				  size_t expected_sram_size,size_t expected_rtc_size)
{
	char legacy[4096],core_path[4096],content[4096],native[4096],sram_path[4096],rtc_path[4096];
	assert(snprintf(legacy,sizeof(legacy),"%s/%s",fixtures,profile)>0);
	assert(snprintf(core_path,sizeof(core_path),"%s/%s-core.so",base,profile)>0);put(core_path,profile,strlen(profile));
	assert(snprintf(content,sizeof(content),"%s/%s/%s",fixtures,profile,rom_name)>0);
	assert(snprintf(native,sizeof(native),"%s/profile-native-%s",base,profile)>0);
	assert(snprintf(sram_path,sizeof(sram_path),"%s/%s/%s",legacy,library,sram_name)>0);
	size_t sram_size=0,rtc_size=0;uint8_t *sram=read_bytes(sram_path,&sram_size),*rtc=NULL;
	if(rtc_name){assert(snprintf(rtc_path,sizeof(rtc_path),"%s/%s/%s",legacy,library,rtc_name)>0);rtc=read_bytes(rtc_path,&rtc_size);}
	assert(sram_size==expected_sram_size&&rtc_size==expected_rtc_size);
	struct saves *store=saves_open(native,core_path,content);assert(store);
	assert(saves_set_legacy_core_name(store,library));
	char receipt[65];
	assert(saves_legacy_preview(store,legacy,sram_size,rtc_size,receipt)==SAVES_LEGACY_READY);
	assert(saves_legacy_import(store,legacy,sram_size,rtc_size,receipt)==SAVES_LEGACY_READY);
	assert(saves_legacy_preview(store,legacy,sram_size,rtc_size,NULL)==SAVES_LEGACY_ALREADY_IMPORTED);
	uint8_t *memory=malloc(sram_size+rtc_size);assert(memory);
	assert(saves_load_memory(store,memory,sram_size,memory+sram_size,rtc_size));
	assert(memcmp(memory,sram,sram_size)==0&&(!rtc_size||memcmp(memory+sram_size,rtc,rtc_size)==0));
	/* The profile-produced raw files remain byte-for-byte available in the legacy namespace. */
	size_t after_size=0;uint8_t *after=read_bytes(sram_path,&after_size);
	assert(after_size==sram_size&&memcmp(after,sram,sram_size)==0);free(after);
	saves_close(store);free(memory);free(rtc);free(sram);
}
static void tree_remove(const char *path)
{
	DIR *d=opendir(path);if(!d){(void)unlink(path);return;}struct dirent *e;while((e=readdir(d))!=NULL){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;char child[4096];int n=snprintf(child,sizeof(child),"%s/%s",path,e->d_name);assert(n>0&&(size_t)n<sizeof(child));tree_remove(child);}closedir(d);assert(rmdir(path)==0);
}
static void only_entry(const char *dir,char *out,size_t size)
{
	DIR *d=opendir(dir);assert(d);struct dirent *e;bool got=false;while((e=readdir(d))!=NULL)if(e->d_name[0]!='.'){int n=snprintf(out,size,"%s/%s",dir,e->d_name);assert(n>0&&(size_t)n<size);got=true;break;}closedir(d);assert(got);
}
static void entry_with_file(const char *dir,const char *file,char *out,size_t size)
{
	DIR *d=opendir(dir);assert(d);struct dirent *e;bool got=false;
	while((e=readdir(d))!=NULL){if(e->d_name[0]=='.')continue;char candidate[4096],child[4096];
		int n=snprintf(candidate,sizeof(candidate),"%s/%s",dir,e->d_name);assert(n>0&&(size_t)n<sizeof(candidate));
		n=snprintf(child,sizeof(child),"%s/%s",candidate,file);assert(n>0&&(size_t)n<sizeof(child));
		struct stat st;if(stat(child,&st)==0){n=snprintf(out,size,"%s",candidate);assert(n>0&&(size_t)n<size);got=true;break;}}
	closedir(d);assert(got);
}
static uint64_t current(const char *dir)
{
	char p[4096],b[32];int n=snprintf(p,sizeof(p),"%s/CURRENT",dir);assert(n>0&&(size_t)n<sizeof(p));int fd=open(p,O_RDONLY);if(fd<0)fprintf(stderr,"CURRENT open failed: %s\n",p);assert(fd>=0);ssize_t r=read(fd,b,sizeof(b)-1);assert(r>0);assert(close(fd)==0);b[r]=0;return strtoull(b,NULL,10);
}
static void assert_current_slot(const char *dir,const void *expected,size_t size)
{
	char path[4096];
	int n=snprintf(path,sizeof(path),"%s/g%016llx/state-00",dir,(unsigned long long)current(dir));
	assert(n>0&&(size_t)n<sizeof(path));
	size_t actual_size;
	uint8_t *actual=read_bytes(path,&actual_size);
	assert(actual_size==size&&memcmp(actual,expected,size)==0);
	free(actual);
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

/* When set, runtime_run_token points the runtime's startup-error file here. */
static char startup_error_file[4096];
static void assert_startup_error(const char *expected)
{
	size_t size=0;uint8_t *got=read_bytes(startup_error_file,&size);
	assert(got&&size==strlen(expected)&&memcmp(got,expected,size)==0);free(got);
	assert(unlink(startup_error_file)==0);
}
static int runtime_run_token(const char *runtime,const char *core,const char *rom,
			     const char *root,bool signal_exit,int state_operation,int quick_resume,
			     const char *token)
{
	int pipes[2];assert(pipe(pipes)==0);pid_t pid=fork();assert(pid>=0);
	if(pid==0){
		close(pipes[0]);assert(dup2(pipes[1],STDERR_FILENO)>=0);close(pipes[1]);
		int nullfd=open("/dev/null",O_WRONLY);assert(nullfd>=0);assert(dup2(nullfd,STDOUT_FILENO)>=0);close(nullfd);
		assert(setenv("BANANA_RETRO_NATIVE_DIR",root,1)==0);
		if(startup_error_file[0])assert(setenv("BANANA_RETRO_STARTUP_ERROR_FILE",startup_error_file,1)==0);
		assert(setenv("BANANA_RETRO_TEST_PERSISTENCE","1",1)==0);
		if(quick_resume)assert(setenv("BANANA_RETRO_TEST_QUICK_RESUME","1",1)==0);
		else unsetenv("BANANA_RETRO_TEST_QUICK_RESUME");
		if(quick_resume==4||quick_resume==5)assert(setenv("BANANA_RETRO_TEST_UNSERIALIZE_OOM","1",1)==0);
		if(signal_exit)assert(setenv("BANANA_RETRO_TEST_WAIT_FOR_SIGNAL","1",1)==0);
		else unsetenv("BANANA_RETRO_TEST_WAIT_FOR_SIGNAL");
		char *args[12];int n=0;args[n++]=(char*)runtime;args[n++]=(char*)core;args[n++]=(char*)rom;args[n++]="--frames";args[n++]=signal_exit?"10000":"2";
		if(state_operation){args[n++]=state_operation==1?"--load-state":"--save-state";args[n++]="0";}
		if(quick_resume==5)args[n++]="--load-battery";
		if(token){args[n++]="--resume-checkpoint";args[n++]=(char*)token;}
		args[n]=NULL;
		execv(runtime,args);_exit(127);
	}
	close(pipes[1]);bool saw_run=false;char line[256];size_t used=0;char c;
	bool expect_no_run=quick_resume==4||quick_resume==6;
	while(read(pipes[0],&c,1)==1){
		if(c!='\n'){if(used+1<sizeof(line))line[used++]=c;continue;}
		line[used]=0;
		if(!strncmp(line,"fixture run ",12)){
			if(quick_resume&&!expect_no_run)
				assert(!strcmp(line,quick_resume==2?"fixture run 3":"fixture run 1"));
			saw_run=true;
			break;
		}
		used=0;
	}
	if(signal_exit){assert(saw_run);assert(kill(pid,SIGTERM)==0);}
	if(expect_no_run)assert(!saw_run);
	else if(quick_resume)assert(saw_run);
	while(read(pipes[0],&c,1)==1){}
	close(pipes[0]);int status;assert(waitpid(pid,&status,0)==pid);assert(WIFEXITED(status));	return WEXITSTATUS(status);
}

static int runtime_run(const char *runtime,const char *core,const char *rom,
		       const char *root,bool signal_exit,int state_operation,int quick_resume)
{
	char token[65], *resume=NULL;
	if(quick_resume==2||quick_resume==4||quick_resume==6){
		struct saves *s=saves_open_with_options(root,core,rom,"banana-retro-options-v1:");
		assert(s&&saves_resume_probe(s,token)==SAVES_RESUME_READY);saves_close(s);
		resume=token;
	}
	return runtime_run_token(runtime,core,rom,root,signal_exit,state_operation,quick_resume,resume);
}

static int legacy_cli_run(const char *runtime,const char *core,const char *rom,
			  const char *legacy,const char *native,bool importing,const char *receipt,
			  char *output,size_t capacity)
{
	int pipes[2];assert(pipe(pipes)==0);pid_t pid=fork();assert(pid>=0);
	if(pid==0){close(pipes[0]);assert(dup2(pipes[1],STDOUT_FILENO)>=0);close(pipes[1]);
		char *preview_args[]={(char*)runtime,"--legacy-preview",(char*)core,(char*)rom,(char*)legacy,(char*)native,NULL};
		char *import_args[]={(char*)runtime,"--legacy-import",(char*)receipt,(char*)core,(char*)rom,(char*)legacy,(char*)native,NULL};
		execv(runtime,importing?import_args:preview_args);_exit(127);}
	close(pipes[1]);size_t used=0;char b[256];ssize_t n;
	while((n=read(pipes[0],b,sizeof(b)))>0){size_t take=(size_t)n;if(take>capacity-used-1)take=capacity-used-1;memcpy(output+used,b,take);used+=take;}
	assert(close(pipes[0])==0);output[used]=0;int status;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status));return WEXITSTATUS(status);
}

static void run_persistence_callbacks(const char *runtime,const char *mode,const char *core,
                                      const char *rom,const char *root)
{
	pid_t pid=fork();assert(pid>=0);
	if(pid==0){
		execl(runtime,runtime,"--check-persistence",mode,core,rom,root,(char *)NULL);
		_exit(127);
	}
	int status;
	assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
}

static bool runtime_has_save(const char *root,const char *core,const char *rom)
{
	struct saves *s=saves_open_with_options(root,core,rom,"banana-retro-options-v1:");if(!s)return false;uint8_t sram[4]={0},rtc[2]={0};bool ok=saves_load_memory(s,sram,sizeof(sram),rtc,sizeof(rtc));saves_close(s);return ok;
}

static void assert_resume_snapshot(struct saves *s,const char *expected_token,
                                   uint8_t state,uint8_t sram,uint8_t rtc)
{
	char token[65];
	uint8_t saved_state=0,saved_sram=0,saved_rtc=0;
	assert(saves_resume_probe(s,token)==SAVES_RESUME_READY&&strcmp(token,expected_token)==0);
	assert(saves_load_resume(s,token,&saved_state,1,&saved_sram,1,&saved_rtc,1)==SAVES_RESUME_READY);
	assert(saved_state==state&&saved_sram==sram&&saved_rtc==rtc);
}

static void check_resume_publication(const char *root,const char *core,const char *content)
{
	struct saves *s=saves_open(root,core,content);assert(s);
	char token[65],identity[4096],old_generation[4096];
	assert(saves_commit_checkpoint(s,"a",1,"b",1,"c",1,token));
	assert_resume_snapshot(s,token,'c','a','b');
	only_entry(root,identity,sizeof(identity));
	uint64_t pinned=current(identity);
	assert(snprintf(old_generation,sizeof(old_generation),"%s/g%016llx",
	                identity,(unsigned long long)pinned)>0);

	assert(saves_commit(s,"d",1,"e",1,-1,NULL,0));
	uint8_t ordinary_sram=0,ordinary_rtc=0;
	assert(saves_load_memory(s,&ordinary_sram,1,&ordinary_rtc,1));
	assert(ordinary_sram=='d'&&ordinary_rtc=='e');
	char ineligible[65];
	assert(saves_resume_probe(s,ineligible)==SAVES_RESUME_NONE);

	/* CURRENT is already durable when the RESUME rename fails or is interrupted. */
	uint64_t ordinary_generation=current(identity);
	saves_test_fail_event(SAVES_TEST_RESUME_RENAME,1);
	assert(!saves_commit_checkpoint(s,"f",1,"g",1,"h",1,NULL));
	assert(current(identity)>ordinary_generation);
	assert(saves_resume_probe(s,ineligible)==SAVES_RESUME_NONE);

	pid_t child=fork();assert(child>=0);
	if(child==0){
		struct saves *other=saves_open(root,core,content);
		if(!other)_exit(1);
		saves_test_crash_event(SAVES_TEST_RESUME_RENAME);
		(void)saves_commit_checkpoint(other,"i",1,"j",1,"k",1,NULL);
		_exit(2);
	}
	int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86);
	saves_close(s);
	s=saves_open(root,core,content);assert(s);
	assert(saves_resume_probe(s,ineligible)==SAVES_RESUME_NONE);
	assert(saves_load_memory(s,&ordinary_sram,1,&ordinary_rtc,1));
	assert(ordinary_sram=='i'&&ordinary_rtc=='j');
	for(unsigned i=0;i<3;i++)assert(saves_commit(s,"l",1,"m",1,-1,NULL,0));
	assert(saves_resume_probe(s,ineligible)==SAVES_RESUME_NONE);

	/* A rename whose fsync fails may be visible, but both snapshots must survive. */
	saves_test_fail_event(SAVES_TEST_PARENT_DIR_FSYNC,3);
	assert(!saves_commit_checkpoint(s,"n",1,"o",1,"c",1,NULL));
	assert(access(old_generation,F_OK)==0);
	char replacement[65];
	assert(saves_resume_probe(s,replacement)==SAVES_RESUME_READY&&strcmp(token,replacement)!=0);
	assert_resume_snapshot(s,replacement,'c','n','o');
	uint8_t unused=0;
	assert(saves_load_resume(s,token,&unused,1,&unused,1,&unused,1)==SAVES_RESUME_RETRY);

	assert(saves_commit_checkpoint(s,"p",1,"q",1,"r",1,NULL));
	assert(saves_resume_probe(s,replacement)==SAVES_RESUME_READY);
	assert_resume_snapshot(s,replacement,'r','p','q');
	assert(access(old_generation,F_OK)<0&&errno==ENOENT);

	/* An older marker may remain on disk after CURRENT switches to battery-only. */
	child=fork();assert(child>=0);
	if(child==0){
		struct saves *writer=saves_open(root,core,content);
		if(!writer)_exit(1);
		saves_test_crash_event(SAVES_TEST_AFTER_CURRENT_RENAME);
		(void)saves_commit_battery_only(writer,"s",1,"t",1);
		_exit(2);
	}
	assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86);
	char marker[4096];
	assert(snprintf(marker,sizeof(marker),"%s/RESUME",identity)>0&&access(marker,F_OK)==0);
	assert(saves_resume_probe(s,replacement)==SAVES_RESUME_NONE);

	/* Ordinary writes and failed marker replacement must carry the cutoff forward. */
	struct saves *other=saves_open(root,core,content);assert(other);
	assert(saves_commit(other,"x",1,"y",1,-1,NULL,0));
	assert(saves_resume_probe(other,replacement)==SAVES_RESUME_NONE);
	saves_test_fail_event(SAVES_TEST_RESUME_RENAME,1);
	assert(!saves_commit_checkpoint(other,"x",1,"y",1,"z",1,NULL));
	assert(saves_resume_probe(other,replacement)==SAVES_RESUME_NONE);
	assert(saves_load_memory(s,&ordinary_sram,1,&ordinary_rtc,1));
	assert(ordinary_sram=='x'&&ordinary_rtc=='y');
	char cutoff[4096];
	assert(snprintf(cutoff,sizeof(cutoff),"%s/g%016llx/resume-floor",identity,
		(unsigned long long)current(identity))>0&&unlink(cutoff)==0);
	assert(saves_resume_probe(s,replacement)==SAVES_RESUME_NONE&&access(marker,F_OK)==0);
	assert(saves_load_memory(s,&ordinary_sram,1,&ordinary_rtc,1));
	assert(ordinary_sram=='x'&&ordinary_rtc=='y');
	assert(saves_commit_checkpoint(other,"u",1,"v",1,"w",1,NULL));
	assert(saves_resume_probe(other,replacement)==SAVES_RESUME_READY);
	assert(saves_commit(s,"x",1,"y",1,-1,NULL,0));
	assert(saves_resume_probe(s,replacement)==SAVES_RESUME_NONE);

	/* A required cutoff that is missing or malformed under the exact checkpoint is permanent
	 * metadata: it can never load on a later boot. A superseded or unrelated token stays
	 * retryable, and a repaired record loads again. */
	char exact[65],unrelated[65];
	assert(saves_commit_checkpoint(other,"A",1,"B",1,"C",1,exact));
	assert(saves_resume_probe(other,replacement)==SAVES_RESUME_READY&&!strcmp(replacement,exact));
	char exact_floor[4096];
	uint8_t exact_state=0,exact_sram=0,exact_rtc=0;
	memset(unrelated,'0',64);unrelated[64]=0;
	assert(snprintf(exact_floor,sizeof(exact_floor),"%s/g%016llx/resume-floor",identity,
		(unsigned long long)current(identity))>0);
	assert(unlink(exact_floor)==0);
	assert(saves_resume_probe(other,replacement)==SAVES_RESUME_INVALID&&!strcmp(replacement,exact));
	assert(saves_load_resume(other,exact,&exact_state,1,&exact_sram,1,&exact_rtc,1)==SAVES_RESUME_INVALID);
	assert(saves_load_resume(other,unrelated,&exact_state,1,&exact_sram,1,&exact_rtc,1)==SAVES_RESUME_RETRY);
	assert(saves_resume_clear_if(other,exact));
	assert(saves_resume_probe(other,replacement)==SAVES_RESUME_NONE);
	assert(saves_commit_checkpoint(other,"D",1,"E",1,"F",1,exact));
	assert(saves_resume_probe(other,replacement)==SAVES_RESUME_READY&&!strcmp(replacement,exact));
	assert(snprintf(exact_floor,sizeof(exact_floor),"%s/g%016llx/resume-floor",identity,
		(unsigned long long)current(identity))>0);
	put(exact_floor,"x",1);
	assert(saves_resume_probe(other,replacement)==SAVES_RESUME_INVALID&&!strcmp(replacement,exact));
	saves_close(other);
	saves_close(s);
}

static void check_legacy_resume(const char *root,const char *core,const char *content)
{
	struct saves *s=saves_open(root,core,content);assert(s);
	assert(saves_commit(s,"a",1,"b",1,0,"d",1));
	assert(saves_commit_checkpoint(s,"a",1,"b",1,"c",1,NULL));
	assert(saves_test_downgrade_current_manifest(s,2));
	char identity[4096],marker[4096],token[65];
	only_entry(root,identity,sizeof(identity));
	assert(snprintf(marker,sizeof(marker),"%s/RESUME",identity)>0);
	const char v2[]="v2\n2e7d2c03a9507ae265ecf5b5356885a53393a2029d241394997265a1a25aefc6\n";
	put(marker,v2,sizeof(v2)-1);
	assert(saves_resume_probe(s,token)==SAVES_RESUME_NONE&&!token[0]);
	put(marker,"v1\n",3);
	assert(saves_resume_probe(s,token)==SAVES_RESUME_NONE&&!token[0]);
	put(marker,v2,sizeof(v2)-1);
	for(unsigned i=0;i<3;i++)assert(saves_commit(s,"e",1,"f",1,-1,NULL,0));
	uint8_t state=0,sram=0,rtc=0;
	assert(saves_state_present(s,0)&&!saves_load_state(s,0,&state,1));
	assert_current_slot(identity,"d",1);
	assert(saves_load_checkpoint(s,&state,1)&&state=='c');
	assert(saves_load_memory(s,&sram,1,&rtc,1)&&sram=='e'&&rtc=='f');
	assert(saves_resume_probe(s,token)==SAVES_RESUME_NONE&&!token[0]);
	saves_test_fail_event(SAVES_TEST_RESUME_RENAME,1);
	assert(!saves_commit_checkpoint(s,"e",1,"f",1,"xy",2,NULL));
	for(unsigned i=0;i<3;i++)assert(saves_commit(s,"e",1,"f",1,-1,NULL,0));
	assert(saves_load_checkpoint(s,&state,1)&&state=='c');
	assert(saves_commit_checkpoint(s,"g",1,"h",1,"i",1,NULL));
	assert(saves_resume_probe(s,token)==SAVES_RESUME_READY);
	assert_resume_snapshot(s,token,'i','g','h');
	assert(saves_state_present(s,0)&&!saves_load_state(s,0,&state,1));
	assert_current_slot(identity,"d",1);

	/* The old generation-bound marker still selects cold start when its writer
	 * could have mislabeled the effective core options. */
	assert(saves_test_downgrade_current_manifest(s,3));
	uint64_t generation=current(identity);
	char manifest[4096];
	assert(snprintf(manifest,sizeof(manifest),"%s/g%016llx/manifest",
	                identity,(unsigned long long)generation)>0);
	size_t manifest_size;
	uint8_t *bytes=read_bytes(manifest,&manifest_size);
	assert(manifest_size>32);
	const char hex[]="0123456789abcdef";
	for(unsigned i=0;i<32;i++){
		uint8_t byte=bytes[manifest_size-32+i];
		token[2*i]=hex[byte>>4];
		token[2*i+1]=hex[byte&15];
	}
	free(bytes);
	token[64]=0;
	char v3[86];
	assert(snprintf(v3,sizeof(v3),"v3\n%016llx\n%s\n",
	                (unsigned long long)generation,token)==85);
	put(marker,v3,sizeof(v3)-1);
	assert(saves_resume_probe(s,token)==SAVES_RESUME_NONE&&!token[0]);
	for(unsigned i=0;i<3;i++)assert(saves_commit(s,"e",1,"f",1,-1,NULL,0));
	assert(saves_resume_probe(s,token)==SAVES_RESUME_NONE);
	assert(saves_load_checkpoint(s,&state,1)&&state=='i');
	assert_current_slot(identity,"d",1);
	saves_close(s);
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

static void corrupt_byte(const char *path)
{
	int fd=open(path,O_RDWR|O_CLOEXEC|O_NOFOLLOW);assert(fd>=0);
	unsigned char b;assert(pread(fd,&b,1,0)==1);b^=0xff;assert(pwrite(fd,&b,1,0)==1);
	assert(fsync(fd)==0&&close(fd)==0);
}
static void generation_file(const char *identity,uint64_t generation,const char *name,char *out,size_t size)
{
	int n=snprintf(out,size,"%s/g%016llx/%s",identity,(unsigned long long)generation,name);
	assert(n>0&&(size_t)n<size);
}
/* A damaged manual slot must neither roll battery memory back to an older generation nor
 * stop later commits from publishing newer SRAM and RTC. */
static void check_slot_damage_isolated(const char *root,const char *core,const char *content)
{
	struct saves *s=saves_open(root,core,content);assert(s);
	assert(saves_commit(s,"a",1,"r",1,0,"S",1));
	assert(saves_commit(s,"b",1,"r",1,1,"T",1));
	assert(saves_commit(s,"c",1,"s",1,-1,NULL,0));
	char identity[4096],path[4096];only_entry(root,identity,sizeof(identity));
	uint64_t newest=current(identity);
	generation_file(identity,newest,"state-00",path,sizeof(path));corrupt_byte(path);
	uint8_t sram=0,rtc=0,state=0;
	assert(saves_load_memory(s,&sram,1,&rtc,1)&&sram=='c'&&rtc=='s');
	/* The previous generation still holds a verifying copy of the slot. */
	assert(saves_state_present(s,0)&&saves_load_state(s,0,&state,1)&&state=='S');
	generation_file(identity,newest-1,"state-00",path,sizeof(path));corrupt_byte(path);
	sram=rtc=0;
	assert(saves_load_memory(s,&sram,1,&rtc,1)&&sram=='c'&&rtc=='s');
	assert(!saves_state_present(s,0)&&!saves_load_state(s,0,&state,1));
	assert(saves_state_present(s,1)&&saves_load_state(s,1,&state,1)&&state=='T');
	/* With no verifying copy left the slot is dropped, and the new battery data is kept. */
	assert(saves_commit(s,"d",1,"t",1,-1,NULL,0));
	sram=rtc=0;
	assert(saves_load_memory(s,&sram,1,&rtc,1)&&sram=='d'&&rtc=='t');
	assert(!saves_state_present(s,0)&&saves_state_present(s,1));
	state=0;assert(saves_load_state(s,1,&state,1)&&state=='T');
	saves_close(s);
}
static struct saves *seed_two_slots(const char *root,const char *core,const char *content,
				    char *identity,size_t size)
{
	struct saves *s=saves_open(root,core,content);assert(s);
	assert(saves_commit(s,"a",1,"r",1,0,"S",1));
	assert(saves_commit(s,"b",1,"r",1,1,"T",1));
	assert(saves_commit(s,"c",1,"s",1,-1,NULL,0));
	only_entry(root,identity,size);
	return s;
}
static int slot_value(struct saves *s,unsigned slot)
{
	uint8_t value=0;
	return saves_state_present(s,slot)&&saves_load_state(s,slot,&value,1) ? (int)value : -1;
}
static void assert_memory(struct saves *s,uint8_t sram,uint8_t rtc)
{
	uint8_t a=0,b=0;
	assert(saves_load_memory(s,&a,1,&b,1)&&a==sram&&b==rtc);
}
/* A copy of one manual slot that cannot be read is damage to that slot alone. It must not block
 * the commit that carries SRAM and RTC, and the newest copy that does verify must be kept. A
 * failed directory scan or a resource failure says nothing about the copies, so it still fails
 * the commit and leaves CURRENT and every slot as they were. */
static void check_slot_copy_read_errors(const char *base,const char *core,const char *content)
{
	char root[4096],identity[4096],path[4096];struct saves *s;uint64_t newest;
	/* An unreadable newest copy falls back to the older verifying copy. */
	assert(snprintf(root,sizeof(root),"%s/slot-read-fallback",base)>0);
	s=seed_two_slots(root,core,content,identity,sizeof(identity));newest=current(identity);
	saves_test_fail_read(newest,"state-00",EIO);
	assert(saves_commit(s,"d",1,"t",1,-1,NULL,0));
	assert(slot_value(s,0)=='S');
	saves_test_fail_read(0,NULL,0);
	assert_memory(s,'d','t');assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T');
	saves_close(s);
	/* With no copy readable or valid, only that slot is left out of the new generation. */
	assert(snprintf(root,sizeof(root),"%s/slot-read-dropped",base)>0);
	s=seed_two_slots(root,core,content,identity,sizeof(identity));newest=current(identity);
	generation_file(identity,newest-1,"state-00",path,sizeof(path));corrupt_byte(path);
	saves_test_fail_read(newest,"state-00",EIO);
	assert(saves_commit(s,"d",1,"t",1,-1,NULL,0));
	assert_memory(s,'d','t');assert(slot_value(s,0)==-1&&slot_value(s,1)=='T');
	/* The unreadable copy stays retained: once readable again it is found, and the next commit
	 * carries it forward instead of losing it. */
	saves_test_fail_read(0,NULL,0);
	assert(slot_value(s,0)=='S');
	assert(saves_commit(s,"e",1,"u",1,-1,NULL,0));
	assert_memory(s,'e','u');assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T');
	saves_close(s);
	/* An older generation whose manifest cannot be read is skipped the same way. */
	assert(snprintf(root,sizeof(root),"%s/slot-manifest-read",base)>0);
	s=seed_two_slots(root,core,content,identity,sizeof(identity));newest=current(identity);
	generation_file(identity,newest,"state-00",path,sizeof(path));corrupt_byte(path);
	saves_test_fail_read(newest-1,"manifest",EIO);
	assert(saves_commit(s,"d",1,"t",1,-1,NULL,0));
	assert_memory(s,'d','t');assert(slot_value(s,0)==-1&&slot_value(s,1)=='T');
	/* The generation with the unreadable manifest is kept, so the slot returns once it reads. */
	saves_test_fail_read(0,NULL,0);
	assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T');
	saves_close(s);
	/* Of two verifying copies the newest is the one carried forward. */
	assert(snprintf(root,sizeof(root),"%s/slot-newest-copy",base)>0);
	s=saves_open(root,core,content);assert(s);
	assert(saves_commit(s,"a",1,"r",1,0,"S",1));
	assert(saves_commit(s,"b",1,"r",1,0,"U",1));
	assert(saves_commit(s,"c",1,"s",1,-1,NULL,0));
	assert_memory(s,'c','s');assert(slot_value(s,0)=='U');
	saves_close(s);
	/* A directory scan failure fails closed: nothing is published and no slot is dropped. */
	assert(snprintf(root,sizeof(root),"%s/slot-scan-failure",base)>0);
	s=seed_two_slots(root,core,content,identity,sizeof(identity));newest=current(identity);
	saves_test_fail_event(SAVES_TEST_GENERATION_SCAN,1);
	assert(!saves_commit(s,"d",1,"t",1,-1,NULL,0));
	assert(current(identity)==newest);assert_memory(s,'c','s');
	assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T');
	assert(saves_commit(s,"d",1,"t",1,-1,NULL,0));
	assert_memory(s,'d','t');assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T');
	saves_close(s);
	/* So does a resource failure, whether it hits the payload buffer or opening a copy. */
	assert(snprintf(root,sizeof(root),"%s/slot-resource-failure",base)>0);
	s=seed_two_slots(root,core,content,identity,sizeof(identity));newest=current(identity);
	saves_test_fail_event(SAVES_TEST_SLOT_ALLOC,1);
	assert(!saves_commit(s,"d",1,"t",1,-1,NULL,0));
	assert(current(identity)==newest);assert_memory(s,'c','s');
	saves_test_fail_read(newest,"state-00",EMFILE);
	assert(!saves_commit(s,"d",1,"t",1,-1,NULL,0));
	saves_test_fail_read(0,NULL,0);
	assert(current(identity)==newest);assert_memory(s,'c','s');
	assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T');
	assert(saves_commit(s,"d",1,"t",1,-1,NULL,0));
	assert_memory(s,'d','t');assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T');
	saves_close(s);
	assert(snprintf(root,sizeof(root),"%s/slot-resource-manifest",base)>0);
	s=seed_two_slots(root,core,content,identity,sizeof(identity));newest=current(identity);
	generation_file(identity,newest,"state-00",path,sizeof(path));corrupt_byte(path);
	saves_test_fail_read(newest-1,"manifest",ENFILE);
	assert(!saves_commit(s,"d",1,"t",1,-1,NULL,0));
	saves_test_fail_read(0,NULL,0);
	assert(current(identity)==newest);assert_memory(s,'c','s');
	assert(saves_commit(s,"d",1,"t",1,-1,NULL,0));
	assert_memory(s,'d','t');assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T');
	saves_close(s);
}
static int log_begin(const char *path)
{
	fflush(stderr);
	int saved=dup(STDERR_FILENO);assert(saved>=0);
	int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);assert(fd>=0);
	assert(dup2(fd,STDERR_FILENO)>=0&&close(fd)==0);
	return saved;
}
static void log_end(int saved,const char *path,char *out,size_t size)
{
	fflush(stderr);
	assert(dup2(saved,STDERR_FILENO)>=0&&close(saved)==0);
	int fd=open(path,O_RDONLY);assert(fd>=0);
	ssize_t n=read(fd,out,size-1);assert(n>=0&&close(fd)==0);
	out[n]=0;
}
static unsigned count_generations(const char *identity)
{
	DIR *d=opendir(identity);assert(d);
	unsigned n=0;struct dirent *e;
	while((e=readdir(d))!=NULL)if(e->d_name[0]=='g'&&strlen(e->d_name)==17)n++;
	assert(closedir(d)==0);
	return n;
}
/* A battery commit whose stderr diagnostics are captured. */
static void commit_logged(struct saves *s,const char *logfile,char *log,size_t size)
{
	int saved=log_begin(logfile);
	bool ok=saves_commit(s,"e",1,"u",1,-1,NULL,0);
	log_end(saved,logfile,log,size);
	assert(ok);
}
/* Slot 0 holds 'S' in the older and 'U' in the newest retained generation; slot 1 holds 'T'. */
static struct saves *seed_versions(const char *root,const char *core,const char *content,
				   char *identity,size_t size)
{
	struct saves *s=saves_open(root,core,content);assert(s);
	assert(saves_commit(s,"a",1,"r",1,0,"S",1));
	assert(saves_commit(s,"b",1,"r",1,1,"T",1));
	assert(saves_commit(s,"c",1,"s",1,0,"U",1));
	only_entry(root,identity,size);
	assert(count_generations(identity)==2);
	return s;
}
/* A slot copy that cannot be read may only be temporarily damaged. Commits go on, an older
 * verified copy serves as a fallback, and neither that copy nor the unreadable newer one may be
 * pruned or shadowed until the newer one reads again, nor may a persistent fault accumulate
 * generations. */
static void check_slot_retention(const char *base,const char *core,const char *content)
{
	char root[4096],identity[4096],path[4096],logfile[4096],log[1024];
	struct saves *s;uint64_t g3;
	assert(snprintf(logfile,sizeof(logfile),"%s/slot-log.txt",base)>0);
	/* Transient: the copy reads again after several commits and pruning passes. */
	assert(snprintf(root,sizeof(root),"%s/slot-retain-transient",base)>0);
	s=seed_versions(root,core,content,identity,sizeof(identity));g3=current(identity);
	saves_test_fail_read(g3,"state-00",EIO);
	commit_logged(s,logfile,log,sizeof(log));
	assert(strstr(log,"Native save slot 0:")&&strstr(log,"unreadable, kept")&&!strstr(log,"slot 1"));
	assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T');
	for(unsigned i=0;i<5;i++)commit_logged(s,logfile,log,sizeof(log));
	assert(count_generations(identity)==4);
	assert_memory(s,'e','u');assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T');
	saves_test_fail_read(0,NULL,0);
	saves_test_fail_read(g3,"manifest",EIO);
	commit_logged(s,logfile,log,sizeof(log));
	/* Every empty slot walks past that manifest; it is reported once. */
	assert(strstr(log,"manifest unreadable")&&!strstr(strstr(log,"manifest unreadable")+1,"manifest unreadable"));
	assert(count_generations(identity)==4&&slot_value(s,0)=='S');
	saves_test_fail_read(0,NULL,0);
	assert(slot_value(s,0)=='U'&&slot_value(s,1)=='T');
	commit_logged(s,logfile,log,sizeof(log));
	assert(log[0]==0&&count_generations(identity)==3);
	assert(slot_value(s,0)=='U'&&slot_value(s,1)=='T');
	/* The copy's verified source is this slot's fallback until a later commit carries a
	 * verified copy of its own; damage to the newest copy must not lose the slot. */
	generation_file(identity,current(identity),"state-00",path,sizeof(path));corrupt_byte(path);
	assert(slot_value(s,0)=='U');
	saves_close(s);
	/* The fallback is the newest verified older copy. */
	assert(snprintf(root,sizeof(root),"%s/slot-retain-newest",base)>0);
	s=saves_open(root,core,content);assert(s);
	assert(saves_commit(s,"a",1,"r",1,0,"S",1));
	assert(saves_commit(s,"b",1,"r",1,0,"V",1));
	assert(saves_commit(s,"c",1,"r",1,0,"U",1));
	only_entry(root,identity,sizeof(identity));g3=current(identity);
	saves_test_fail_read(g3,"state-00",EIO);
	commit_logged(s,logfile,log,sizeof(log));
	assert(slot_value(s,0)=='V');
	saves_test_fail_read(0,NULL,0);
	assert(slot_value(s,0)=='U');
	saves_close(s);
	/* Persistent, with no verifying copy: kept and bounded, found again once readable. */
	assert(snprintf(root,sizeof(root),"%s/slot-retain-persistent",base)>0);
	s=seed_versions(root,core,content,identity,sizeof(identity));g3=current(identity);
	generation_file(identity,g3-1,"state-00",path,sizeof(path));corrupt_byte(path);
	saves_test_fail_read(g3,"state-00",EIO);
	commit_logged(s,logfile,log,sizeof(log));
	assert(strstr(log,"no verified copy"));
	for(unsigned i=0;i<6;i++)commit_logged(s,logfile,log,sizeof(log));
	assert(count_generations(identity)==3);
	assert(slot_value(s,0)==-1&&slot_value(s,1)=='T');
	saves_test_fail_read(0,NULL,0);
	assert(slot_value(s,0)=='U');
	commit_logged(s,logfile,log,sizeof(log));
	assert(count_generations(identity)==3&&slot_value(s,0)=='U');
	generation_file(identity,current(identity),"state-00",path,sizeof(path));corrupt_byte(path);
	assert(slot_value(s,0)=='U');
	saves_close(s);
	/* A second fault, on the fallback itself, must not cost the fallback. */
	assert(snprintf(root,sizeof(root),"%s/slot-retain-second-fault",base)>0);
	s=seed_versions(root,core,content,identity,sizeof(identity));g3=current(identity);
	saves_test_fail_read(g3,"state-00",EIO);
	commit_logged(s,logfile,log,sizeof(log));
	saves_test_fail_read(g3-1,"state-00",EIO);
	commit_logged(s,logfile,log,sizeof(log));
	saves_test_fail_read(0,NULL,0);
	saves_test_fail_read(g3,"state-00",EIO);
	assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T');
	saves_test_fail_read(0,NULL,0);
	assert(slot_value(s,0)=='U');
	saves_close(s);
	/* A commit that failed to publish leaves an orphan above CURRENT. It must not displace
	 * the committed copy that a later fault hides. */
	assert(snprintf(root,sizeof(root),"%s/slot-retain-orphan",base)>0);
	s=seed_versions(root,core,content,identity,sizeof(identity));g3=current(identity);
	saves_test_fail_event(SAVES_TEST_CURRENT_RENAME,1);
	assert(!saves_commit(s,"x",1,"y",1,0,"D",1));
	assert(current(identity)==g3);
	saves_test_fail_read(g3,"state-00",EIO);
	commit_logged(s,logfile,log,sizeof(log));
	commit_logged(s,logfile,log,sizeof(log));
	saves_test_fail_read(0,NULL,0);
	assert(slot_value(s,0)=='U'&&slot_value(s,1)=='T');
	saves_close(s);
	/* Checkpoint, battery-only and plain commits treat degraded slots alike. */
	assert(snprintf(root,sizeof(root),"%s/slot-retain-kinds",base)>0);
	s=seed_versions(root,core,content,identity,sizeof(identity));g3=current(identity);
	saves_test_fail_read(g3,"state-00",EIO);
	saves_test_fail_read(g3,"state-01",EIO);
	assert(saves_commit_checkpoint(s,"p",1,"q",1,"k",1,NULL));
	assert(saves_commit_battery_only(s,"e",1,"u",1));
	commit_logged(s,logfile,log,sizeof(log));
	assert(count_generations(identity)<=6);
	assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T');
	saves_test_fail_read(0,NULL,0);
	assert(slot_value(s,0)=='U'&&slot_value(s,1)=='T');
	commit_logged(s,logfile,log,sizeof(log));
	commit_logged(s,logfile,log,sizeof(log));
	assert(slot_value(s,0)=='U'&&slot_value(s,1)=='T'&&count_generations(identity)<=3);
	saves_close(s);
	/* Explicit replacement supersedes the unresolved copies; a zero-length slot is refused. */
	assert(snprintf(root,sizeof(root),"%s/slot-retain-replace",base)>0);
	s=seed_versions(root,core,content,identity,sizeof(identity));g3=current(identity);
	saves_test_fail_read(g3,"state-00",EIO);
	commit_logged(s,logfile,log,sizeof(log));
	assert(saves_commit(s,"e",1,"u",1,1,"Z",1));
	assert(count_generations(identity)==4&&slot_value(s,0)=='S'&&slot_value(s,1)=='Z');
	uint64_t before=current(identity);
	assert(!saves_commit(s,"x",1,"y",1,0,NULL,0));
	assert(current(identity)==before&&slot_value(s,0)=='S');
	assert(saves_commit(s,"e",1,"u",1,0,"W",1));
	assert(count_generations(identity)==2&&slot_value(s,0)=='W'&&slot_value(s,1)=='Z');
	commit_logged(s,logfile,log,sizeof(log));
	assert(log[0]==0&&slot_value(s,0)=='W'&&slot_value(s,1)=='Z');
	saves_test_fail_read(0,NULL,0);
	saves_close(s);
	/* A verified source, displaced by a damaged newer copy, is retained as the one fallback
	 * until a later commit carries a verified copy of its own. Retention stays bounded. */
	assert(snprintf(root,sizeof(root),"%s/slot-retain-source",base)>0);
	s=seed_versions(root,core,content,identity,sizeof(identity));g3=current(identity);
	generation_file(identity,g3,"state-00",path,sizeof(path));corrupt_byte(path);
	commit_logged(s,logfile,log,sizeof(log));
	assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T'&&count_generations(identity)==3);
	generation_file(identity,g3-1,"manifest",path,sizeof(path));assert(access(path,F_OK)==0);
	for(unsigned i=0;i<3;i++){
		generation_file(identity,current(identity),"state-00",path,sizeof(path));corrupt_byte(path);
		commit_logged(s,logfile,log,sizeof(log));
		assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T'&&count_generations(identity)==3);
		generation_file(identity,g3-1,"manifest",path,sizeof(path));assert(access(path,F_OK)==0);
	}
	commit_logged(s,logfile,log,sizeof(log));
	assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T'&&count_generations(identity)==2);
	generation_file(identity,g3-1,"manifest",path,sizeof(path));
	assert(access(path,F_OK)<0&&errno==ENOENT);
	/* The released state still has one fallback; losing both copies leaves none. */
	generation_file(identity,current(identity),"state-00",path,sizeof(path));corrupt_byte(path);
	assert(slot_value(s,0)=='S');
	generation_file(identity,current(identity)-1,"state-00",path,sizeof(path));corrupt_byte(path);
	assert(slot_value(s,0)==-1);
	saves_close(s);
}
/* A possible legacy resume target whose manifest cannot be read is kept by pruning. */
static void check_legacy_prune_protection(const char *base,const char *core,const char *content)
{
	char root[4096],identity[4096],marker[4096],manifest[4096];
	assert(snprintf(root,sizeof(root),"%s/legacy-prune",base)>0);
	struct saves *s=saves_open(root,core,content);assert(s);
	for(unsigned i=0;i<SAVES_STATE_SLOTS;i++){
		uint8_t value=(uint8_t)('a'+i);
		assert(saves_commit(s,"a",1,"b",1,(int)i,&value,1));
	}
	assert(saves_commit_checkpoint(s,"a",1,"b",1,"c",1,NULL));
	assert(saves_test_downgrade_current_manifest(s,2));
	only_entry(root,identity,sizeof(identity));
	uint64_t legacy=current(identity);
	const char v2[]="v2\n2e7d2c03a9507ae265ecf5b5356885a53393a2029d241394997265a1a25aefc6\n";
	assert(snprintf(marker,sizeof(marker),"%s/RESUME",identity)>0);
	put(marker,v2,sizeof(v2)-1);
	generation_file(identity,legacy,"manifest",manifest,sizeof(manifest));
	assert(saves_commit(s,"e",1,"f",1,-1,NULL,0));
	static const int errors[]={ENFILE,EIO};
	for(unsigned i=0;i<2;i++){
		saves_test_fail_read(legacy,"manifest",errors[i]);
		assert(saves_commit(s,"e",1,"f",1,-1,NULL,0));
		assert(saves_commit(s,"e",1,"f",1,-1,NULL,0));
		assert(access(manifest,F_OK)==0);
		saves_test_fail_read(0,NULL,0);
	}
	uint8_t state=0;
	assert(saves_load_checkpoint(s,&state,1)&&state=='c');
	saves_close(s);
}
/* A commit whose generation directory was published but whose CURRENT rename failed. The runtime
 * never acknowledged it, so it is not committed data: CURRENT still names the last real commit. */
static struct saves *seed_orphan(const char *base,const char *name,const char *core,const char *content,
				 char *identity,size_t size,uint64_t *committed,bool checkpoint)
{
	char root[4096];
	assert(snprintf(root,sizeof(root),"%s/%s",base,name)>0);
	struct saves *s=seed_versions(root,core,content,identity,size);
	*committed=current(identity);
	saves_test_fail_event(SAVES_TEST_CURRENT_RENAME,1);
	assert(checkpoint ? !saves_commit_checkpoint(s,"x",1,"y",1,"k",1,NULL) :
			    !saves_commit(s,"x",1,"y",1,0,"D",1));
	assert(current(identity)==*committed&&count_generations(identity)==3);
	return s;
}
/* A generation above CURRENT must never become authoritative: not as the fallback for a damaged
 * committed copy, not as the source of carried-forward slots, and not as the checkpoint. */
static void check_orphan_generations(const char *base,const char *core,const char *content)
{
	char identity[4096],path[4096],logfile[4096],log[1024];
	struct saves *s;uint64_t g3;
	assert(snprintf(logfile,sizeof(logfile),"%s/orphan-log.txt",base)>0);
	/* Slot loading: the newest committed copy is damaged; the older committed copy is the fallback. */
	s=seed_orphan(base,"orphan-slot-load",core,content,identity,sizeof(identity),&g3,false);
	assert(slot_value(s,0)=='U');
	generation_file(identity,g3,"state-00",path,sizeof(path));corrupt_byte(path);
	assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T');
	saves_close(s);
	/* Battery loading: likewise for SRAM and RTC. */
	s=seed_orphan(base,"orphan-memory",core,content,identity,sizeof(identity),&g3,false);
	assert_memory(s,'c','s');
	generation_file(identity,g3,"memory",path,sizeof(path));corrupt_byte(path);
	assert_memory(s,'b','r');
	saves_close(s);
	/* Carry-forward: the orphan's slot copy must not enter the next committed generation, and the
	 * orphan is removed by that commit. */
	s=seed_orphan(base,"orphan-carry",core,content,identity,sizeof(identity),&g3,false);
	generation_file(identity,g3,"state-00",path,sizeof(path));corrupt_byte(path);
	commit_logged(s,logfile,log,sizeof(log));
	assert(slot_value(s,0)=='S'&&slot_value(s,1)=='T');
	assert_memory(s,'e','u');
	assert(count_generations(identity)==3);
	/* The verified source of the carried slot stays as its fallback. */
	generation_file(identity,current(identity),"state-00",path,sizeof(path));corrupt_byte(path);
	assert(slot_value(s,0)=='S');
	saves_close(s);
	/* The previous generation a commit retains is the committed fallback, not the orphan, when
	 * CURRENT's own battery memory is damaged. */
	s=seed_orphan(base,"orphan-previous",core,content,identity,sizeof(identity),&g3,false);
	generation_file(identity,g3,"memory",path,sizeof(path));corrupt_byte(path);
	commit_logged(s,logfile,log,sizeof(log));
	assert_memory(s,'e','u');
	generation_file(identity,g3-1,"manifest",path,sizeof(path));assert(access(path,F_OK)==0);
	generation_file(identity,g3+1,"manifest",path,sizeof(path));assert(access(path,F_OK)!=0);
	assert(count_generations(identity)==3);
	/* The carried slot keeps its newer verified source, not the older committed fallback. */
	generation_file(identity,current(identity),"state-00",path,sizeof(path));corrupt_byte(path);
	assert(slot_value(s,0)=='U');
	saves_close(s);
	/* An orphan checkpoint is not a checkpoint, and no later prune keeps it alive. */
	s=seed_orphan(base,"orphan-checkpoint",core,content,identity,sizeof(identity),&g3,true);
	uint8_t k=0;
	assert(!saves_checkpoint_present(s)&&!saves_load_checkpoint(s,&k,1));
	commit_logged(s,logfile,log,sizeof(log));
	assert(count_generations(identity)==2&&!saves_checkpoint_present(s));
	saves_close(s);
	/* An interruption immediately after CURRENT moves must not turn an orphan into history. */
	s=seed_orphan(base,"orphan-publish-crash",core,content,identity,sizeof(identity),&g3,false);
	pid_t child=fork();assert(child>=0);
	if(child==0){
		saves_test_crash_event(SAVES_TEST_AFTER_CURRENT_RENAME);
		(void)saves_commit(s,"e",1,"u",1,-1,NULL,0);
		_exit(2);
	}
	int status;
	assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86);
	assert(current(identity)==g3+2);
	generation_file(identity,g3+1,"manifest",path,sizeof(path));assert(access(path,F_OK)!=0&&errno==ENOENT);
	generation_file(identity,g3+2,"state-00",path,sizeof(path));corrupt_byte(path);
	assert(slot_value(s,0)=='U');
	saves_close(s);
	/* A RESUME marker that names an orphan cannot keep it alive: the commit that wrote it never published. */
	s=seed_orphan(base,"orphan-marker",core,content,identity,sizeof(identity),&g3,false);
	{
		char text[86];
		assert(snprintf(text,sizeof(text),"v4\n%016llx\n%064x\n",(unsigned long long)(g3+1),0)==85);
		assert(snprintf(path,sizeof(path),"%s/RESUME",identity)>0);
		put(path,text,85);
	}
	commit_logged(s,logfile,log,sizeof(log));
	assert(count_generations(identity)==2);
	generation_file(identity,g3+1,"manifest",path,sizeof(path));assert(access(path,F_OK)!=0);
	saves_close(s);
	/* A CURRENT that cannot be read says nothing about which commit is last: fail closed rather
	 * than guess from the directory. (A root user ignores the mode, so skip there.) */
	if(geteuid()!=0){
		uint8_t sram=0,rtc=0;
		s=seed_orphan(base,"orphan-current-unreadable",core,content,identity,sizeof(identity),&g3,false);
		assert(snprintf(path,sizeof(path),"%s/CURRENT",identity)>0);
		assert(chmod(path,0)==0);
		assert(!saves_load_memory(s,&sram,1,&rtc,1)&&slot_value(s,0)==-1);
		assert(!saves_commit(s,"z",1,"z",1,-1,NULL,0)&&count_generations(identity)==3);
		assert(chmod(path,0600)==0);
		assert_memory(s,'c','s');
		assert(slot_value(s,0)=='U');
		saves_close(s);
	}
}
/* The reader shipped before the CURRENT-generation check consults RESUME and the cutoff file
 * named by a '+' CURRENT, and nothing else. Report whether it would restore the checkpoint. */
static bool baseline_reader_would_resume(const char *identity)
{
	char path[4096],line[32]={0},generation[32]={0};
	int n=snprintf(path,sizeof(path),"%s/RESUME",identity);assert(n>0&&(size_t)n<sizeof(path));
	FILE *marker=fopen(path,"r");if(!marker)return false;
	bool ok=fgets(line,sizeof(line),marker)&&fgets(generation,sizeof(generation),marker);
	assert(fclose(marker)==0);
	if(!ok||strcmp(line,"v4\n"))return false;
	n=snprintf(path,sizeof(path),"%s/CURRENT",identity);assert(n>0&&(size_t)n<sizeof(path));
	marker=fopen(path,"r");assert(marker&&fgets(line,sizeof(line),marker));assert(fclose(marker)==0);
	uint64_t floor=0;
	if(line[0]=='+'){
		generation_file(identity,strtoull(line,NULL,10),"resume-floor",path,sizeof(path));
		size_t size=0;uint8_t *data=read_bytes(path,&size);assert(size>=8);
		for(unsigned i=0;i<8;i++)floor|=(uint64_t)data[i]<<(i*8);
		free(data);
	}
	return strtoull(generation,NULL,16)>floor;
}
/* Any commit that is not a checkpoint carries newer battery memory. A downgraded runtime must
 * find the cutoff, whether the commit saved a slot or only flushed memory. */
static void check_superseded_checkpoint_downgrade(const char *root,const char *core,const char *content)
{
	struct saves *s=saves_open(root,core,content);assert(s);
	char token[65],identity[4096],probe[65];
	assert(saves_commit_checkpoint(s,"a",1,"b",1,"c",1,token));
	only_entry(root,identity,sizeof(identity));
	assert(baseline_reader_would_resume(identity));
	assert(saves_commit(s,"d",1,"e",1,0,"S",1));
	assert(!baseline_reader_would_resume(identity));
	assert(saves_resume_probe(s,probe)==SAVES_RESUME_NONE);
	assert(saves_commit_checkpoint(s,"f",1,"g",1,"h",1,token));
	assert(baseline_reader_would_resume(identity));
	assert(saves_commit(s,"i",1,"j",1,-1,NULL,0));
	assert(!baseline_reader_would_resume(identity));
	assert(saves_resume_probe(s,probe)==SAVES_RESUME_NONE);
	/* Two checkpoints in a row: the second is durable in CURRENT but its marker never lands. */
	assert(saves_commit_checkpoint(s,"k",1,"l",1,"m",1,token));
	assert(baseline_reader_would_resume(identity));
	saves_test_fail_event(SAVES_TEST_RESUME_RENAME,1);
	assert(!saves_commit_checkpoint(s,"n",1,"o",1,"p",1,NULL));
	assert(!baseline_reader_would_resume(identity));
	assert(saves_resume_probe(s,probe)==SAVES_RESUME_NONE);
	saves_close(s);
}
/* Save identity binds to the digest the caller measured, not to the file's later contents. */
static void check_content_bound_identity(const char *root,const char *core,const char *rom)
{
	put(rom,"original",8);
	unsigned char digest[SAVES_CONTENT_DIGEST_SIZE],other[SAVES_CONTENT_DIGEST_SIZE];
	saves_digest_content("original",8,digest);
	int fd=open(rom,O_RDONLY|O_CLOEXEC);assert(fd>=0);
	assert(saves_digest_content_fd(fd,8,other)&&!memcmp(digest,other,sizeof(digest)));
	assert(!saves_digest_content_fd(fd,7,other)&&!saves_digest_content_fd(fd,9,other));
	assert(close(fd)==0);
	struct saves *bound=saves_open_content(root,core,rom,digest,"banana-retro-options-v1:");
	assert(bound&&saves_commit(bound,"x",1,"y",1,-1,NULL,0));
	put(rom,"replaced",8);
	uint8_t sram=0,rtc=0;
	struct saves *by_path=saves_open_with_options(root,core,rom,"banana-retro-options-v1:");
	assert(by_path&&!saves_load_memory(by_path,&sram,1,&rtc,1));
	struct saves *again=saves_open_content(root,core,rom,digest,"banana-retro-options-v1:");
	assert(again&&saves_load_memory(again,&sram,1,&rtc,1)&&sram=='x'&&rtc=='y');
	assert(!saves_open_content(root,core,rom,NULL,"banana-retro-options-v1:"));
	saves_close(again);saves_close(by_path);saves_close(bound);
}
int main(int argc,char **argv)
{
	char tmp[]="/tmp/banana-retro-saves.XXXXXX";char *base=mkdtemp(tmp);assert(base);
	char root[4096],core[4096],content[4096],content2[4096];assert(snprintf(root,sizeof(root),"%s/native",base)>0);assert(snprintf(core,sizeof(core),"%s/core.so",base)>0);assert(snprintf(content,sizeof(content),"%s/a/rom.bin",base)>0);assert(snprintf(content2,sizeof(content2),"%s/b/rom.bin",base)>0);
	assert(mkdir(strcat(strcpy((char[4096]){0},base),"/a"),0700)==0);assert(mkdir(strcat(strcpy((char[4096]){0},base),"/b"),0700)==0);
	put(core,"core-build-one",14);put(content,"same-name-a",11);put(content2,"same-name-b",11);
	assert(argc==8);
	check_profile_fixture(base,argv[7],"mgba","mGBA","mgba.gba","mgba.srm",NULL,131072,0);
	check_profile_fixture(base,argv[7],"gambatte","Gambatte","gambatte.gb","gambatte.srm","gambatte.rtc",32768,8);
	check_profile_fixture(base,argv[7],"fceumm","FCEUmm","fceumm.nes","fceumm.srm",NULL,8192,0);
	struct saves *s=saves_open(root,core,content);assert(s);uint8_t sram[]={1,2,3,4},rtc[]={9,8},state0[]={3,1,4,1,5};
	assert(!saves_load_memory(s,sram,sizeof(sram),rtc,sizeof(rtc)));
	assert(saves_commit(s,sram,sizeof(sram),rtc,sizeof(rtc),0,state0,sizeof(state0)));
	uint8_t got_sram[4]={0},got_rtc[2]={0},got_state[5]={0};assert(saves_load_memory(s,got_sram,sizeof(got_sram),got_rtc,sizeof(got_rtc)));assert(!memcmp(sram,got_sram,sizeof(sram))&&!memcmp(rtc,got_rtc,sizeof(rtc)));assert(saves_load_state(s,0,got_state,sizeof(got_state))&&!memcmp(state0,got_state,sizeof(state0)));
	/* A generation written by the approved v1 importer still opens after checkpoint metadata upgrades manifests. */
	assert(saves_test_downgrade_current_manifest(s,1));saves_close(s);s=saves_open(root,core,content);assert(s);
	memset(got_sram,0,sizeof(got_sram));memset(got_rtc,0,sizeof(got_rtc));memset(got_state,0,sizeof(got_state));
	assert(saves_load_memory(s,got_sram,sizeof(got_sram),got_rtc,sizeof(got_rtc)));
	assert(!memcmp(sram,got_sram,sizeof(sram))&&!memcmp(rtc,got_rtc,sizeof(rtc)));
	assert(saves_state_present(s,0)&&!saves_load_state(s,0,got_state,sizeof(got_state)));
	char v1_identity[4096];
	only_entry(root,v1_identity,sizeof(v1_identity));
	assert_current_slot(v1_identity,state0,sizeof(state0));
	assert(saves_commit(s,sram,sizeof(sram),rtc,sizeof(rtc),0,state0,sizeof(state0)));
	/* Preview/import follows production discovery and preserves read-only legacy files. */
	char legacy[4096],legacy_sram[4096],legacy_rtc[4096],legacy_native[4096];
	assert(snprintf(legacy,sizeof(legacy),"%s/legacy",base)>0&&mkdir(legacy,0700)==0);
	assert(snprintf(legacy_sram,sizeof(legacy_sram),"%s/rom.srm",legacy)>0);
	assert(snprintf(legacy_rtc,sizeof(legacy_rtc),"%s/rom.rtc",legacy)>0);
	assert(snprintf(legacy_native,sizeof(legacy_native),"%s/legacy-native",base)>0);
	put(legacy_sram,sram,sizeof(sram));put(legacy_rtc,rtc,sizeof(rtc));
	struct saves *legacy_store=saves_open(legacy_native,core,content);assert(legacy_store);
	char legacy_receipt[65];
	assert(saves_legacy_preview(legacy_store,legacy,sizeof(sram),sizeof(rtc),legacy_receipt)==SAVES_LEGACY_READY);
	struct saves *changed_profile=saves_open_with_options(legacy_native,core,content,"different-options");assert(changed_profile);
	assert(saves_legacy_import(changed_profile,legacy,sizeof(sram),sizeof(rtc),legacy_receipt)==SAVES_LEGACY_CHANGED);
	saves_close(changed_profile);
	uint8_t replacement_sram[]={4,3,2,1};put(legacy_sram,replacement_sram,sizeof(replacement_sram));
	assert(saves_legacy_import(legacy_store,legacy,sizeof(sram),sizeof(rtc),legacy_receipt)==SAVES_LEGACY_CHANGED);
	assert(!saves_load_memory(legacy_store,got_sram,sizeof(got_sram),got_rtc,sizeof(got_rtc)));
	put(legacy_sram,sram,sizeof(sram));
	assert(saves_legacy_preview(legacy_store,legacy,sizeof(sram),sizeof(rtc),legacy_receipt)==SAVES_LEGACY_READY);
	assert(chmod(legacy_sram,0400)==0&&chmod(legacy_rtc,0400)==0);
	assert(saves_legacy_import(legacy_store,legacy,sizeof(sram),sizeof(rtc),legacy_receipt)==SAVES_LEGACY_READY);
	assert(saves_legacy_preview(legacy_store,legacy,sizeof(sram),sizeof(rtc),NULL)==SAVES_LEGACY_ALREADY_IMPORTED);
	memset(got_sram,0,sizeof(got_sram));memset(got_rtc,0,sizeof(got_rtc));
	assert(saves_load_memory(legacy_store,got_sram,sizeof(got_sram),got_rtc,sizeof(got_rtc)));
	assert(!memcmp(got_sram,sram,sizeof(sram))&&!memcmp(got_rtc,rtc,sizeof(rtc)));
	int legacy_fd=open(legacy_sram,O_RDONLY);assert(legacy_fd>=0);uint8_t legacy_byte=0;assert(read(legacy_fd,&legacy_byte,1)==1&&legacy_byte==sram[0]);assert(close(legacy_fd)==0);
	/* Divergent RetroArch data is previewed as already imported; neither namespace is overwritten. */
	assert(chmod(legacy_sram,0600)==0);
	uint8_t ra_divergent[]={7,7,7,7};put(legacy_sram,ra_divergent,sizeof(ra_divergent));
	assert(saves_legacy_preview(legacy_store,legacy,sizeof(sram),sizeof(rtc),NULL)==SAVES_LEGACY_ALREADY_IMPORTED);
	assert(saves_legacy_import(legacy_store,legacy,sizeof(sram),sizeof(rtc),legacy_receipt)==SAVES_LEGACY_ALREADY_IMPORTED);
	memset(got_sram,0,sizeof(got_sram));assert(saves_load_memory(legacy_store,got_sram,sizeof(got_sram),got_rtc,sizeof(got_rtc)));
	assert(!memcmp(got_sram,sram,sizeof(sram)));
	saves_close(legacy_store);
	/* A destination write after preview invalidates confirmation and is preserved. */
	char changed_dest_root[4096];assert(snprintf(changed_dest_root,sizeof(changed_dest_root),"%s/changed-dest",base)>0);
	struct saves *changed_dest=saves_open(changed_dest_root,core,content);assert(changed_dest);
	char dest_receipt[65];assert(saves_legacy_preview(changed_dest,legacy,sizeof(sram),sizeof(rtc),dest_receipt)==SAVES_LEGACY_READY);
	uint8_t destination_byte=0x55;assert(saves_commit(changed_dest,&destination_byte,1,NULL,0,-1,NULL,0));
	assert(saves_legacy_import(changed_dest,legacy,sizeof(sram),sizeof(rtc),dest_receipt)==SAVES_LEGACY_ALREADY_IMPORTED);
	uint8_t destination_out=0;assert(saves_load_memory(changed_dest,&destination_out,1,NULL,0)&&destination_out==destination_byte);
	saves_close(changed_dest);
	/* Wrong lengths and duplicate root/per-core candidates fail closed. */
	char bad_legacy[4096],bad_sram[4096],bad_core_dir[4096],bad_core_file[4096],bad_core_rtc[4096];
	assert(snprintf(bad_legacy,sizeof(bad_legacy),"%s/legacy-bad",base)>0&&mkdir(bad_legacy,0700)==0);
	assert(snprintf(bad_sram,sizeof(bad_sram),"%s/rom.srm",bad_legacy)>0);put(bad_sram,"x",1);
	struct saves *bad_store=saves_open(legacy_native,core,content2);assert(bad_store);
	assert(saves_legacy_preview(bad_store,bad_legacy,4,0,NULL)==SAVES_LEGACY_INVALID);
	assert(saves_legacy_import(bad_store,bad_legacy,4,0,"0000000000000000000000000000000000000000000000000000000000000000")==SAVES_LEGACY_INVALID);
	uint8_t failed_import_target[4];assert(!saves_load_memory(bad_store,failed_import_target,sizeof(failed_import_target),NULL,0));
	/* A special file is a malformed raw save even when no writer is attached. */
	assert(unlink(bad_sram)==0&&mkfifo(bad_sram,0600)==0);
	assert(saves_legacy_preview(bad_store,bad_legacy,4,0,NULL)==SAVES_LEGACY_INVALID);
	assert(saves_legacy_import(bad_store,bad_legacy,4,0,"0000000000000000000000000000000000000000000000000000000000000000")==SAVES_LEGACY_INVALID);
	assert(!saves_load_memory(bad_store,failed_import_target,sizeof(failed_import_target),NULL,0));
	assert(unlink(bad_sram)==0);
	put(bad_sram,sram,sizeof(sram));
	assert(snprintf(bad_core_dir,sizeof(bad_core_dir),"%s/core.so",bad_legacy)>0&&mkdir(bad_core_dir,0700)==0);
	assert(snprintf(bad_core_rtc,sizeof(bad_core_rtc),"%s/rom.rtc",bad_core_dir)>0);put(bad_core_rtc,rtc,sizeof(rtc));
	assert(saves_legacy_preview(bad_store,bad_legacy,4,sizeof(rtc),NULL)==SAVES_LEGACY_AMBIGUOUS);
	assert(saves_legacy_import(bad_store,bad_legacy,4,sizeof(rtc),"0000000000000000000000000000000000000000000000000000000000000000")==SAVES_LEGACY_AMBIGUOUS);
	assert(snprintf(bad_core_file,sizeof(bad_core_file),"%s/rom.srm",bad_core_dir)>0);put(bad_core_file,sram,sizeof(sram));
	assert(saves_legacy_preview(bad_store,bad_legacy,4,0,NULL)==SAVES_LEGACY_AMBIGUOUS);
	assert(saves_legacy_import(bad_store,bad_legacy,4,0,"0000000000000000000000000000000000000000000000000000000000000000")==SAVES_LEGACY_AMBIGUOUS);
	saves_close(bad_store);
	/* A publication failure leaves the raw source untouched and no partial native generation visible. */
	char failed_import_root[4096];assert(snprintf(failed_import_root,sizeof(failed_import_root),"%s/import-failed",base)>0);
	struct saves *failed_import=saves_open(failed_import_root,core,content);assert(failed_import);
	put(legacy_sram,sram,sizeof(sram));
	char failed_receipt[65];assert(saves_legacy_preview(failed_import,legacy,sizeof(sram),sizeof(rtc),failed_receipt)==SAVES_LEGACY_READY);
	saves_test_fail_write_after(2);
	assert(saves_legacy_import(failed_import,legacy,sizeof(sram),sizeof(rtc),failed_receipt)==SAVES_LEGACY_INVALID);
	assert(!saves_load_memory(failed_import,failed_import_target,sizeof(failed_import_target),got_rtc,sizeof(got_rtc)));
	legacy_fd=open(legacy_sram,O_RDONLY);assert(legacy_fd>=0);assert(read(legacy_fd,&legacy_byte,1)==1&&legacy_byte==sram[0]);assert(close(legacy_fd)==0);
	saves_close(failed_import);
	/* The per-core legacy layout is independently discoverable and importable. */
	char per_core_root[4096],per_core_dir[4096],per_core_sram[4096],per_core_rtc[4096],per_core_native[4096];
	assert(snprintf(per_core_root,sizeof(per_core_root),"%s/legacy-per-core",base)>0&&mkdir(per_core_root,0700)==0);
	assert(snprintf(per_core_dir,sizeof(per_core_dir),"%s/core.so",per_core_root)>0&&mkdir(per_core_dir,0700)==0);
	assert(snprintf(per_core_sram,sizeof(per_core_sram),"%s/rom.srm",per_core_dir)>0);
	assert(snprintf(per_core_rtc,sizeof(per_core_rtc),"%s/rom.rtc",per_core_dir)>0);
	assert(snprintf(per_core_native,sizeof(per_core_native),"%s/per-core-native",base)>0);
	put(per_core_sram,sram,sizeof(sram));put(per_core_rtc,rtc,sizeof(rtc));
	struct saves *per_core_store=saves_open(per_core_native,core,content);assert(per_core_store);
	char per_core_receipt[65];
	assert(saves_legacy_preview(per_core_store,per_core_root,sizeof(sram),sizeof(rtc),per_core_receipt)==SAVES_LEGACY_READY);
	assert(saves_legacy_import(per_core_store,per_core_root,sizeof(sram),sizeof(rtc),per_core_receipt)==SAVES_LEGACY_READY);
	saves_close(per_core_store);
	/* RetroArch uses the library-reported core name for sorted per-core save directories. */
	char library_root[4096],library_dir[4096],library_sram[4096],library_rtc[4096],library_native[4096];
	assert(snprintf(library_root,sizeof(library_root),"%s/legacy-library-name",base)>0&&mkdir(library_root,0700)==0);
	assert(snprintf(library_dir,sizeof(library_dir),"%s/Banana lifecycle fixture",library_root)>0&&mkdir(library_dir,0700)==0);
	assert(snprintf(library_sram,sizeof(library_sram),"%s/rom.srm",library_dir)>0);put(library_sram,sram,sizeof(sram));
	assert(snprintf(library_rtc,sizeof(library_rtc),"%s/rom.rtc",library_dir)>0);put(library_rtc,rtc,sizeof(rtc));
	assert(snprintf(library_native,sizeof(library_native),"%s/library-native",base)>0);
	struct saves *library_store=saves_open(library_native,core,content);assert(library_store);
	assert(saves_set_legacy_core_name(library_store,"Banana lifecycle fixture"));
	char library_receipt[65];
	assert(saves_legacy_preview(library_store,library_root,sizeof(sram),sizeof(rtc),library_receipt)==SAVES_LEGACY_READY);
	assert(saves_legacy_import(library_store,library_root,sizeof(sram),sizeof(rtc),library_receipt)==SAVES_LEGACY_READY);
	saves_close(library_store);
	/* Checkpoint records bind to a valid, exact-build native generation. */
	char checkpoint_root[4096],other_core[4096],checkpoint_ident[4096],checkpoint_gen[4096],checkpoint_state[4096];
	assert(snprintf(checkpoint_root,sizeof(checkpoint_root),"%s/checkpoint",base)>0);
	struct saves *checkpoint=saves_open(checkpoint_root,core,content);assert(checkpoint);
	uint8_t checkpoint_data[]={2,7,1,8};
	assert(saves_commit(checkpoint,sram,sizeof(sram),rtc,sizeof(rtc),9,checkpoint_data,sizeof(checkpoint_data)));
	assert(saves_commit_checkpoint(checkpoint,sram,sizeof(sram),rtc,sizeof(rtc),checkpoint_data,sizeof(checkpoint_data),NULL));
	assert(saves_checkpoint_present(checkpoint));
	char resume_hash[65];assert(saves_resume_probe(checkpoint,resume_hash)==SAVES_RESUME_READY);uint8_t checkpoint_out[sizeof(checkpoint_data)];
	assert(saves_load_checkpoint(checkpoint,checkpoint_out,sizeof(checkpoint_out))&&!memcmp(checkpoint_out,checkpoint_data,sizeof(checkpoint_data)));
	assert(saves_load_state(checkpoint,9,checkpoint_out,sizeof(checkpoint_out))&&!memcmp(checkpoint_out,checkpoint_data,sizeof(checkpoint_data)));
	/* Failed publication leaves the old resume generation usable. */
	saves_test_fail_write_after(0);
	uint8_t replacement_state[]={9,3,2,3};
	assert(!saves_commit_checkpoint(checkpoint,sram,sizeof(sram),rtc,sizeof(rtc),replacement_state,sizeof(replacement_state),NULL));
	assert(saves_resume_probe(checkpoint,resume_hash)==SAVES_RESUME_READY);
	char rejected_generation_hash[65];memcpy(rejected_generation_hash,resume_hash,sizeof(resume_hash));
	assert(saves_commit_checkpoint(checkpoint,sram,sizeof(sram),rtc,sizeof(rtc),replacement_state,sizeof(replacement_state),NULL));
	assert(saves_resume_probe(checkpoint,resume_hash)==SAVES_RESUME_READY);
	assert(strcmp(rejected_generation_hash,resume_hash)!=0);
	assert(saves_resume_clear_if(checkpoint,rejected_generation_hash));
	assert(saves_resume_probe(checkpoint,rejected_generation_hash)==SAVES_RESUME_READY&&strcmp(rejected_generation_hash,resume_hash)==0);
	assert(snprintf(other_core,sizeof(other_core),"%s/other-core.so",base)>0);put(other_core,"different-core",14);
	struct saves *stale_checkpoint=saves_open(checkpoint_root,other_core,content);assert(stale_checkpoint);
	assert(saves_resume_probe(stale_checkpoint,resume_hash)==SAVES_RESUME_NONE);saves_close(stale_checkpoint);
	struct saves *option_mismatch_checkpoint=saves_open_with_options(checkpoint_root,core,content,"options-changed");assert(option_mismatch_checkpoint);
	assert(saves_resume_probe(option_mismatch_checkpoint,resume_hash)==SAVES_RESUME_NONE);saves_close(option_mismatch_checkpoint);
	/* A checksum failure invalidates resume metadata; it cannot be unserialized. */
	entry_with_file(checkpoint_root,"RESUME",checkpoint_ident,sizeof(checkpoint_ident));uint64_t checkpoint_generation=current(checkpoint_ident);
	assert(snprintf(checkpoint_gen,sizeof(checkpoint_gen),"%s/g%016llx",checkpoint_ident,(unsigned long long)checkpoint_generation)>0);
	assert(snprintf(checkpoint_state,sizeof(checkpoint_state),"%s/checkpoint",checkpoint_gen)>0);
	put(checkpoint_state,(uint8_t[sizeof(replacement_state)]){0},sizeof(replacement_state));
	assert(saves_resume_probe(checkpoint,resume_hash)==SAVES_RESUME_INVALID);assert(resume_hash[0]);assert(saves_checkpoint_present(checkpoint));
	assert(saves_resume_clear_if(checkpoint,resume_hash));saves_close(checkpoint);
	char publication_root[4096],legacy_resume_root[4096];
	assert(snprintf(publication_root,sizeof(publication_root),"%s/resume-publication",base)>0);
	check_resume_publication(publication_root,core,content);
	assert(snprintf(legacy_resume_root,sizeof(legacy_resume_root),"%s/legacy-resume",base)>0);
	check_legacy_resume(legacy_resume_root,core,content);
	char isolation_root[4096],downgrade_root[4096],binding_root[4096],binding_rom[4096];
	assert(snprintf(isolation_root,sizeof(isolation_root),"%s/slot-isolation",base)>0);
	check_slot_damage_isolated(isolation_root,core,content);
	check_slot_copy_read_errors(base,core,content);
	check_slot_retention(base,core,content);
	check_legacy_prune_protection(base,core,content);
	check_orphan_generations(base,core,content);
	assert(snprintf(downgrade_root,sizeof(downgrade_root),"%s/downgrade-cutoff",base)>0);
	check_superseded_checkpoint_downgrade(downgrade_root,core,content);
	assert(snprintf(binding_root,sizeof(binding_root),"%s/content-binding",base)>0);
	assert(snprintf(binding_rom,sizeof(binding_rom),"%s/a/binding.rom",base)>0);
	check_content_bound_identity(binding_root,core,binding_rom);
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
	assert(stat("/", &immutable_root) == 0);
	char absolute_root[4096];
	assert(snprintf(absolute_root, sizeof(absolute_root), "%s/data/retro/saves", base) > 0);
	preserve_seed(absolute_root, core, content, 0x73);
	assert_saved_byte(absolute_root, core, content, 0x73);
	assert(immutable_syncs > 0);
	immutable_root.st_ino = 0;
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
	const char *runtime=argv[1],*fixture=argv[2];char rom[4096],rr[4096];assert(snprintf(rom,sizeof(rom),"%s/runtime.rom",base)>0);put(rom,"BRC1\n",5);assert(snprintf(rr,sizeof(rr),"%s/runtime-normal",base)>0);assert(runtime_run(runtime,fixture,rom,rr,false,0,0)==0);assert(runtime_has_save(rr,fixture,rom));assert(snprintf(rr,sizeof(rr),"%s/runtime-term",base)>0);assert(runtime_run(runtime,fixture,rom,rr,true,0,0)==0);assert(runtime_has_save(rr,fixture,rom));
	/* A failed cold load must not replace the only generation with blank SRAM. */
	char damaged_ident[4096], damaged_memory[4096];
	only_entry(rr, damaged_ident, sizeof(damaged_ident));
	uint64_t damaged_gen = current(damaged_ident);
	assert(snprintf(damaged_memory, sizeof(damaged_memory), "%s/g%016llx/memory",
		damaged_ident, (unsigned long long)damaged_gen) > 0);
	put(damaged_memory, "bad", 3);
	assert(runtime_run(runtime, fixture, rom, rr, false, 0, 0) != 0);
	assert(current(damaged_ident) == damaged_gen);
	for(int i=3;i<7;i++){assert(snprintf(rr,sizeof(rr),"%s/runtime-optional-%d",base,i)>0);assert(runtime_run(runtime,argv[i],rom,rr,false,0,0)==0);assert(runtime_has_save(rr,argv[i],rom));}
	assert(snprintf(rr,sizeof(rr),"%s/runtime-state-load",base)>0);assert(runtime_run(runtime,argv[3],rom,rr,false,1,0)!=0);assert(!runtime_has_save(rr,argv[3],rom));assert(snprintf(rr,sizeof(rr),"%s/runtime-state-save",base)>0);assert(runtime_run(runtime,argv[3],rom,rr,false,2,0)!=0);assert(runtime_has_save(rr,argv[3],rom));
	assert(snprintf(rr,sizeof(rr),"%s/runtime-quick-resume-no-serializer",base)>0);
	assert(runtime_run(runtime,argv[3],rom,rr,true,0,1)==0);
	assert(runtime_has_save(rr,argv[3],rom));
	assert(snprintf(rr,sizeof(rr),"%s/runtime-quit-callbacks",base)>0);
	run_persistence_callbacks(runtime,"quit",fixture,rom,rr);
	const char *regressions[]={"battery-crash","battery-unlink","option-during-restore",
		"options-ui","options-core","transient-resume","exit-persist","legacy-1","legacy-2","legacy-3",
		"content-memory","content-path","content-mutated"};
	for(size_t i=0;i<sizeof(regressions)/sizeof(regressions[0]);i++){
		assert(snprintf(rr,sizeof(rr),"%s/runtime-%s",base,regressions[i])>0);
		run_persistence_callbacks(runtime,regressions[i],fixture,rom,rr);
	}

	/* A first-frame failure cannot replace a requested slot or the old resume. */
	assert(snprintf(rr,sizeof(rr),"%s/runtime-failed-startup",base)>0);
	assert(runtime_run(runtime,fixture,rom,rr,false,2,1)==0);
	char failed_ident[4096];
	only_entry(rr,failed_ident,sizeof(failed_ident));
	uint64_t original_generation=current(failed_ident);
	struct saves *failed_start=saves_open_with_options(rr,fixture,rom,"banana-retro-options-v1:");
	assert(failed_start);
	char original_token[65],after_failure[65];
	assert(saves_resume_probe(failed_start,original_token)==SAVES_RESUME_READY);
	uint32_t original_state=0,after_state=0;
	assert(saves_load_state(failed_start,0,&original_state,sizeof(original_state)));
	assert(setenv("BANANA_RETRO_FIXTURE_MODE","bad-pitch",1)==0);
	assert(runtime_run(runtime,fixture,rom,rr,false,2,2)!=0);
	assert(unsetenv("BANANA_RETRO_FIXTURE_MODE")==0);
	assert(current(failed_ident)==original_generation);
	assert(saves_resume_probe(failed_start,after_failure)==SAVES_RESUME_READY&&strcmp(original_token,after_failure)==0);
	assert(saves_load_state(failed_start,0,&after_state,sizeof(after_state))&&after_state==original_state);
	saves_close(failed_start);
	assert(snprintf(rr,sizeof(rr),"%s/runtime-quick-resume",base)>0);assert(runtime_run(runtime,fixture,rom,rr,false,0,1)==0);assert(runtime_run(runtime,fixture,rom,rr,false,0,2)==0);
	char resume_ident[4096],resume_gen[4096],resume_payload[4096];only_entry(rr,resume_ident,sizeof(resume_ident));uint64_t resume_generation=current(resume_ident);
	assert(snprintf(resume_gen,sizeof(resume_gen),"%s/g%016llx",resume_ident,(unsigned long long)resume_generation)>0);
	assert(snprintf(resume_payload,sizeof(resume_payload),"%s/checkpoint",resume_gen)>0);
	struct saves *missing=saves_open_with_options(rr,fixture,rom,"banana-retro-options-v1:");
	char missing_token[65];
	assert(missing&&saves_resume_probe(missing,missing_token)==SAVES_RESUME_READY);
	saves_close(missing);
	assert(unlink(resume_payload)==0);
	/* The launcher tells a permanent rejection (the checkpoint can never restore) from a failure a later boot may
	 * overcome by the startup error the runtime leaves. */
	assert(snprintf(startup_error_file,sizeof(startup_error_file),"%s/startup-error",base)>0);
	assert(runtime_run_token(runtime,fixture,rom,rr,false,0,6,missing_token)==1);
	assert_startup_error("resume-rejected\n");
	assert(runtime_run_token(runtime,fixture,rom,rr,false,0,6,"invalid")==2);
	assert(current(resume_ident)==resume_generation);
	assert(runtime_run(runtime,fixture,rom,rr,false,0,3)==0);
	/* A marked checkpoint with the wrong serialization size must never fall back
	   to an older checkpoint even if that older payload fits this core. */
	struct saves *wrong_size=saves_open_with_options(rr,fixture,rom,"banana-retro-options-v1:");
	assert(wrong_size);
	assert(saves_commit_checkpoint(wrong_size,sram,sizeof(sram),rtc,sizeof(rtc),"x",1,NULL));
	saves_close(wrong_size);
	assert(runtime_run(runtime,fixture,rom,rr,false,0,6)!=0);
	assert(snprintf(rr,sizeof(rr),"%s/runtime-rejected-resume",base)>0);
	assert(runtime_run(runtime,fixture,rom,rr,false,2,1)==0);
	struct saves *rejected_resume=saves_open_with_options(rr,fixture,rom,"banana-retro-options-v1:");assert(rejected_resume);
	char rejected_hash[65],after_rejection[65];
	only_entry(rr,resume_ident,sizeof(resume_ident));
	resume_generation=current(resume_ident);
	assert(saves_resume_probe(rejected_resume,rejected_hash)==SAVES_RESUME_READY);
	assert(runtime_run(runtime,fixture,rom,rr,false,0,4)!=0);
	assert(current(resume_ident)==resume_generation);
	assert(saves_resume_probe(rejected_resume,after_rejection)==SAVES_RESUME_READY&&
		strcmp(rejected_hash,after_rejection)==0);
	assert(saves_checkpoint_present(rejected_resume));uint32_t preserved_checkpoint=0;
	assert(saves_load_checkpoint(rejected_resume,&preserved_checkpoint,sizeof(preserved_checkpoint)));
	assert(saves_state_present(rejected_resume,0));uint32_t preserved_slot=0;
	assert(saves_load_state(rejected_resume,0,&preserved_slot,sizeof(preserved_slot)));
	assert(runtime_run(runtime,fixture,rom,rr,false,0,2)==0);
	const uint8_t recent_sram[4]={0x56},recent_rtc[2]={0x78};
	assert(saves_commit(rejected_resume,recent_sram,sizeof(recent_sram),recent_rtc,sizeof(recent_rtc),-1,NULL,0));
	resume_generation=current(resume_ident);
	assert(saves_resume_probe(rejected_resume,after_rejection)==SAVES_RESUME_NONE);
	assert(runtime_run_token(runtime,fixture,rom,rr,false,0,4,rejected_hash)!=0);
	assert_startup_error("persistence-load\n");
	startup_error_file[0]=0;
	assert(current(resume_ident)==resume_generation);
	assert(saves_resume_probe(rejected_resume,after_rejection)==SAVES_RESUME_NONE);
	saves_close(rejected_resume);
	assert(setenv("BANANA_RETRO_TEST_EXPECT_INITIAL_BATTERY","56:78",1)==0);
	assert(runtime_run(runtime,fixture,rom,rr,false,0,5)==0);
	assert(unsetenv("BANANA_RETRO_TEST_EXPECT_INITIAL_BATTERY")==0);
	char cli_legacy[4096],cli_native[4096],cli_sram[4096],cli_rtc[4096],cli_state[4096],cli_output[256];
	assert(snprintf(cli_legacy,sizeof(cli_legacy),"%s/cli-legacy",base)>0&&mkdir(cli_legacy,0700)==0);
	assert(snprintf(cli_native,sizeof(cli_native),"%s/cli-native",base)>0);
	assert(snprintf(cli_sram,sizeof(cli_sram),"%s/runtime.srm",cli_legacy)>0);
	assert(snprintf(cli_rtc,sizeof(cli_rtc),"%s/runtime.rtc",cli_legacy)>0);
	assert(snprintf(cli_state,sizeof(cli_state),"%s/runtime.state.auto",cli_legacy)>0);
	uint32_t legacy_state=9;
	put(cli_state,&legacy_state,sizeof(legacy_state));
	assert(legacy_cli_run(runtime,fixture,rom,cli_legacy,cli_native,false,NULL,cli_output,sizeof(cli_output))==0);
	assert(strstr(cli_output,"ok none")!=NULL);
	put(cli_sram,sram,sizeof(sram));put(cli_rtc,rtc,sizeof(rtc));
	assert(legacy_cli_run(runtime,fixture,rom,cli_legacy,cli_native,false,NULL,cli_output,sizeof(cli_output))==0);
	assert(strstr(cli_output,"ok ready sram=4 rtc=2")!=NULL);
	char *receipt_at=strstr(cli_output," receipt=");assert(receipt_at&&strlen(receipt_at+9)>=64);
	char cli_receipt[65];memcpy(cli_receipt,receipt_at+9,64);cli_receipt[64]=0;
	assert(legacy_cli_run(runtime,fixture,rom,cli_legacy,cli_native,true,cli_receipt,cli_output,sizeof(cli_output))==0);
	assert(strstr(cli_output,"ok imported sram=4 rtc=2")!=NULL);
	assert(legacy_cli_run(runtime,fixture,rom,cli_legacy,cli_native,false,NULL,cli_output,sizeof(cli_output))==0);
	assert(strstr(cli_output,"ok already-imported")!=NULL);
	struct saves *imported=saves_open_with_options(cli_native,fixture,rom,"banana-retro-options-v1:");
	assert(imported&&!saves_checkpoint_present(imported)&&!saves_state_present(imported,0));
	assert(saves_resume_probe(imported,resume_hash)==SAVES_RESUME_NONE);
	saves_close(imported);
	assert(runtime_run(runtime,fixture,rom,cli_native,false,0,1)==0);
	assert(runtime_run(runtime,fixture,rom,cli_native,false,0,2)==0);
	saves_close(collision);saves_close(s);tree_remove(base);puts("banana-retro native save checks passed");return 0;
}
