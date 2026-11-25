#define _XOPEN_SOURCE 700
#include "saves.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

enum { HASH_SIZE = 32, MAX_MEMORY = 64 * 1024 * 1024, MAX_STATE = 256 * 1024 * 1024 };
enum { MAX_DESTINATION_ENTRIES = 512, MAX_DESTINATION_BYTES = 512 * 1024 * 1024 };
static const unsigned char magic[8] = {'B','R','S','A','V','E','0','1'};
#ifdef SAVES_TESTING
static size_t test_write_budget = SIZE_MAX;
static enum saves_test_event test_fail_event;
static unsigned test_fail_occurrence;
static enum saves_test_event test_crash_event;
static bool test_fail_enabled, test_crash_enabled;
static void (*test_hook)(void);
void saves_test_fail_write_after(size_t bytes) { test_write_budget = bytes; }
void saves_test_fail_event(enum saves_test_event event, unsigned occurrence)
{ test_fail_event=event;test_fail_occurrence=occurrence;test_fail_enabled=true; }
void saves_test_crash_event(enum saves_test_event event)
{ test_crash_event=event;test_crash_enabled=true; }
void saves_test_set_hook(void (*hook)(void)) { test_hook=hook; }
static bool test_event(enum saves_test_event event)
{
	if(test_crash_enabled&&test_crash_event==event)_exit(86);
	if(test_fail_enabled&&test_fail_event==event){
		if(test_fail_occurrence>1){test_fail_occurrence--;return false;}
		test_fail_enabled=false;errno=EIO;return true;
	}
	return false;
}
#endif

struct saves {
	int directory_fd;
	unsigned char core[HASH_SIZE], content[HASH_SIZE], path[HASH_SIZE], options[HASH_SIZE];
	unsigned char state_options[HASH_SIZE];
	char core_file[NAME_MAX + 1], core_name[NAME_MAX + 1], content_file[NAME_MAX + 1];
	char content_dir[PATH_MAX], destination_root[PATH_MAX];
};

struct manifest {
	uint64_t version, generation, sram_size, rtc_size, checkpoint_size, state_size[SAVES_STATE_SLOTS];
	unsigned char core[HASH_SIZE], content[HASH_SIZE], path[HASH_SIZE], options[HASH_SIZE];
	unsigned char memory_hash[HASH_SIZE], checkpoint_hash[HASH_SIZE], state_hash[SAVES_STATE_SLOTS][HASH_SIZE];
	unsigned char state_options[SAVES_STATE_SLOTS][HASH_SIZE], hash[HASH_SIZE];
};

struct sha256 { uint32_t h[8]; uint64_t bits; unsigned char block[64]; size_t used; };
static const uint32_t k256[64] = {
	0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
	0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
	0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
	0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
	0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
	0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
	0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
	0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static uint32_t rr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static void sha_block(struct sha256 *s, const unsigned char *p)
{
	uint32_t w[64], a,b,c,d,e,f,g,h;
	for (unsigned i=0;i<16;i++) w[i]=(uint32_t)p[i*4]<<24|(uint32_t)p[i*4+1]<<16|(uint32_t)p[i*4+2]<<8|p[i*4+3];
	for (unsigned i=16;i<64;i++) { uint32_t x=w[i-15],y=w[i-2]; w[i]=w[i-16]+(rr(x,7)^rr(x,18)^(x>>3))+w[i-7]+(rr(y,17)^rr(y,19)^(y>>10)); }
	a=s->h[0];b=s->h[1];c=s->h[2];d=s->h[3];e=s->h[4];f=s->h[5];g=s->h[6];h=s->h[7];
	for(unsigned i=0;i<64;i++){uint32_t t1=h+(rr(e,6)^rr(e,11)^rr(e,25))+((e&f)^(~e&g))+k256[i]+w[i];uint32_t t2=(rr(a,2)^rr(a,13)^rr(a,22))+((a&b)^(a&c)^(b&c));h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
	s->h[0]+=a;s->h[1]+=b;s->h[2]+=c;s->h[3]+=d;s->h[4]+=e;s->h[5]+=f;s->h[6]+=g;s->h[7]+=h;
}
static void sha_init(struct sha256 *s)
{
	*s=(struct sha256){.h={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
}
static void sha_update(struct sha256 *s,const void *data,size_t n)
{
	const unsigned char *p=data;s->bits+=(uint64_t)n*8;
	while(n){size_t take=64-s->used;if(take>n)take=n;memcpy(s->block+s->used,p,take);s->used+=take;p+=take;n-=take;if(s->used==64){sha_block(s,s->block);s->used=0;}}
}
static void sha_final(struct sha256 *s,unsigned char out[HASH_SIZE])
{
	uint64_t bits=s->bits;s->block[s->used++]=0x80;if(s->used>56){memset(s->block+s->used,0,64-s->used);sha_block(s,s->block);s->used=0;}memset(s->block+s->used,0,56-s->used);
	for(unsigned i=0;i<8;i++)s->block[63-i]=(unsigned char)(bits>>(i*8));
	sha_block(s,s->block);
	for(unsigned i=0;i<8;i++){out[i*4]=s->h[i]>>24;out[i*4+1]=s->h[i]>>16;out[i*4+2]=s->h[i]>>8;out[i*4+3]=s->h[i];}
}
static bool digest_file(const char *path,unsigned char out[HASH_SIZE])
{
	int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return false;struct sha256 s;sha_init(&s);unsigned char b[16384];bool ok=true;
	for(;;){ssize_t n=read(fd,b,sizeof(b));if(n>0)sha_update(&s,b,(size_t)n);else if(n==0)break;else if(errno!=EINTR){ok=false;break;}}
	if(close(fd)<0)ok=false;
	if(ok)sha_final(&s,out);
	return ok;
}
static bool digest_bytes(const void *data,size_t n,unsigned char out[HASH_SIZE])
{struct sha256 s;sha_init(&s);sha_update(&s,data,n);sha_final(&s,out);return true;}
static void digest_u64(struct sha256 *s,uint64_t value)
{unsigned char b[8];for(unsigned i=0;i<8;i++)b[i]=(unsigned char)(value>>(i*8));sha_update(s,b,sizeof(b));}
static void digest_string(struct sha256 *s,const char *value)
{size_t n=strlen(value);digest_u64(s,(uint64_t)n);sha_update(s,value,n);}
static void hex_digest(const unsigned char digest[HASH_SIZE],char out[65])
{static const char hex[]="0123456789abcdef";for(unsigned i=0;i<HASH_SIZE;i++){out[i*2]=hex[digest[i]>>4];out[i*2+1]=hex[digest[i]&15];}out[64]=0;}

static void put64(unsigned char *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(unsigned char)(v>>(i*8));}
static uint64_t get64(const unsigned char *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(i*8);return v;}
enum {
	MANIFEST_V1_SIZE = 8 + 8 * (4 + SAVES_STATE_SLOTS) + HASH_SIZE * (6 + SAVES_STATE_SLOTS),
	MANIFEST_V2_SIZE = MANIFEST_V1_SIZE + 8 + HASH_SIZE,
	MANIFEST_SIZE = MANIFEST_V2_SIZE + SAVES_STATE_SLOTS * HASH_SIZE
};

static void manifest_encode(const struct manifest *m, unsigned char *b, uint64_t version)
{
	size_t o=0;memcpy(b+o,magic,8);o+=8;put64(b+o,version);o+=8;put64(b+o,m->generation);o+=8;put64(b+o,m->sram_size);o+=8;put64(b+o,m->rtc_size);o+=8;
	if(version>=2){put64(b+o,m->checkpoint_size);o+=8;}
	for(unsigned i=0;i<SAVES_STATE_SLOTS;i++){put64(b+o,m->state_size[i]);o+=8;}
	memcpy(b+o,m->core,HASH_SIZE);o+=HASH_SIZE;memcpy(b+o,m->content,HASH_SIZE);o+=HASH_SIZE;memcpy(b+o,m->path,HASH_SIZE);o+=HASH_SIZE;memcpy(b+o,m->options,HASH_SIZE);o+=HASH_SIZE;memcpy(b+o,m->memory_hash,HASH_SIZE);o+=HASH_SIZE;
	if(version>=2){memcpy(b+o,m->checkpoint_hash,HASH_SIZE);o+=HASH_SIZE;}
	memcpy(b+o,m->state_hash,sizeof(m->state_hash));o+=sizeof(m->state_hash);
	if(version>=3){memcpy(b+o,m->state_options,sizeof(m->state_options));o+=sizeof(m->state_options);}
	digest_bytes(b,o,b+o);
}
static bool manifest_decode(const unsigned char *b,size_t n,struct manifest *m)
{
	if(memcmp(b,magic,8)!=0)return false;
	uint64_t version=get64(b+8);
	if(!((version==1&&n==MANIFEST_V1_SIZE)||(version==2&&n==MANIFEST_V2_SIZE)||
	     (version==3&&n==MANIFEST_SIZE)))return false;
	memset(m,0,sizeof(*m));
	digest_bytes(b,n-HASH_SIZE,m->hash);
	if(memcmp(m->hash,b+n-HASH_SIZE,HASH_SIZE)!=0)return false;
	m->version=version;
	size_t o=16;m->generation=get64(b+o);o+=8;m->sram_size=get64(b+o);o+=8;m->rtc_size=get64(b+o);o+=8;
	if(version>=2){m->checkpoint_size=get64(b+o);o+=8;}
	for(unsigned i=0;i<SAVES_STATE_SLOTS;i++){m->state_size[i]=get64(b+o);o+=8;}
	memcpy(m->core,b+o,HASH_SIZE);o+=HASH_SIZE;memcpy(m->content,b+o,HASH_SIZE);o+=HASH_SIZE;memcpy(m->path,b+o,HASH_SIZE);o+=HASH_SIZE;memcpy(m->options,b+o,HASH_SIZE);o+=HASH_SIZE;memcpy(m->memory_hash,b+o,HASH_SIZE);o+=HASH_SIZE;
	if(version>=2){memcpy(m->checkpoint_hash,b+o,HASH_SIZE);o+=HASH_SIZE;}
	memcpy(m->state_hash,b+o,sizeof(m->state_hash));o+=sizeof(m->state_hash);
	/* Missing fingerprints stay zero when these payloads are copied forward. */
	if(version>=3)memcpy(m->state_options,b+o,sizeof(m->state_options));
	return true;
}
enum file_result { FILE_OK, FILE_INVALID, FILE_ERROR };

static enum file_result missing_or_io_error(void)
{
	return errno==ENOENT||errno==ENOTDIR||errno==ELOOP ? FILE_INVALID : FILE_ERROR;
}

static enum file_result read_bytes(int fd,void *data,size_t size)
{
	size_t off=0;
	while(off<size){
		ssize_t n=read(fd,(char *)data+off,size-off);
		if(n>0)off+=(size_t)n;
		else if(n==0||errno!=EINTR)return FILE_ERROR;
	}
	return FILE_OK;
}

static enum file_result read_exact_status(int dirfd,const char *name,void *data,size_t size)
{
	int fd=openat(dirfd,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
	if(fd<0)return missing_or_io_error();
	struct stat st;
	enum file_result result=FILE_ERROR;
	if(fstat(fd,&st)==0)
		result=S_ISREG(st.st_mode)&&st.st_size>=0&&(uintmax_t)st.st_size==size ?
			read_bytes(fd,data,size) : FILE_INVALID;
	if(close(fd)<0)return FILE_ERROR;
	return result;
}

static bool read_exact_at(int dirfd,const char *name,void *data,size_t size)
{
	return read_exact_status(dirfd,name,data,size)==FILE_OK;
}
static bool write_file_at(int dirfd,const char *name,const void *data,size_t size)
{
	int fd=openat(dirfd,name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return false;size_t off=0;
	while(off<size){size_t chunk=size-off;
#ifdef SAVES_TESTING
		if(test_write_budget!=SIZE_MAX){if(test_write_budget==0){errno=ENOSPC;test_write_budget=SIZE_MAX;break;}if(chunk>test_write_budget)chunk=test_write_budget;}
#endif
		ssize_t n=write(fd,(const char*)data+off,chunk);if(n>0){off+=(size_t)n;
#ifdef SAVES_TESTING
			if(test_write_budget!=SIZE_MAX)test_write_budget-=(size_t)n;
#endif
		}else if(n<0&&errno==EINTR)continue;else break;}
	bool ok=off==size;
#ifdef SAVES_TESTING
	if(ok&&test_event(SAVES_TEST_FILE_FSYNC))ok=false;
#endif
	if(ok&&fsync(fd)<0)ok=false;
	if(close(fd)<0)ok=false;
	if(!ok)(void)unlinkat(dirfd,name,0);
	return ok;
}
static bool sync_parent_directory(int fd)
{
#ifdef SAVES_TESTING
	if(test_event(SAVES_TEST_PARENT_DIR_FSYNC))return false;
#endif
	int rc;do{rc=fsync(fd);}while(rc<0&&errno==EINTR);return rc==0;
}
static int make_dirs(const char *path)
{
	if(!path||!*path)return -1;
	bool absolute=path[0]=='/';
	int fd=open(absolute?"/":".",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
	if(fd<0)return -1;
	const char *p=path;
	while(*p=='/')++p;
	while(*p){
		const char *start=p;while(*p&&*p!='/')++p;size_t n=(size_t)(p-start);
		while(*p=='/')++p;
		if(n==1&&start[0]=='.')continue;
		if((n==2&&start[0]=='.'&&start[1]=='.')||n==0||n>NAME_MAX){close(fd);errno=EINVAL;return -1;}
		char component[NAME_MAX+1];memcpy(component,start,n);component[n]=0;
		bool created = false;
		const int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
		int child = openat(fd, component, flags);
		if (child < 0 && errno == ENOENT) {
			created = mkdirat(fd, component, 0700) == 0;
			if (!created && errno != EEXIST) { close(fd); return -1; }
			child = openat(fd, component, flags);
		}
		if(child<0){close(fd);return -1;}
		struct stat st;if(fstat(child,&st)<0){int saved=errno;close(child);close(fd);errno=saved;return -1;}
		if(!S_ISDIR(st.st_mode)){close(child);close(fd);errno=ENOTDIR;return -1;}
		/* Retry the parent sync even for existing entries after a prior failure. */
		if (!sync_parent_directory(fd)) {
			int saved = errno;
			struct statvfs fs;
			/* An existing entry on the immutable image needs no publication. */
			if (created || fstatvfs(fd, &fs) < 0 || !(fs.f_flag & ST_RDONLY)) {
				close(child); close(fd); errno = saved; return -1;
			}
		}
		if(close(fd)<0){close(child);return -1;}
		fd=child;
	}
	return fd;
}
static bool generation_name(char name[32],uint64_t g)
{int n=snprintf(name,32,"g%016" PRIx64,g);return n==17;}
static bool open_generation(int rootfd,uint64_t g,int *generation_fd)
{
	char name[32];
	if(!generation_name(name,g)){errno=EINVAL;return false;}
	*generation_fd=openat(rootfd,name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
	return *generation_fd>=0;
}
static enum file_result current_generation_status(int rootfd,uint64_t *generation,bool *has_floor)
{
	char buf[32];
	int fd=openat(rootfd,"CURRENT",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
	if(fd<0)return missing_or_io_error();
	struct stat st;
	enum file_result result=FILE_ERROR;
	if(fstat(fd,&st)==0)
		result=S_ISREG(st.st_mode)&&st.st_size>0&&st.st_size<(off_t)sizeof(buf) ?
			read_bytes(fd,buf,(size_t)st.st_size) : FILE_INVALID;
	if(close(fd)<0)return FILE_ERROR;
	if(result!=FILE_OK)return result;
	size_t size=(size_t)st.st_size;
	buf[size]=0;
	char *end;errno=0;unsigned long long v=strtoull(buf,&end,10);
	if(errno||!v||end==buf||end!=buf+size-1||*end!='\n')return FILE_INVALID;
	*generation=(uint64_t)v;
	if(has_floor)*has_floor=buf[0]=='+';
	return FILE_OK;
}

static bool current_generation(int rootfd,uint64_t *generation)
{
	return current_generation_status(rootfd,generation,NULL)==FILE_OK;
}

/* A leading '+' tells newer readers to require the cutoff file. Older readers
 * still parse the same generation number and use the usual battery and slot files. */
static enum file_result read_resume_floor(struct saves *s,uint64_t *floor)
{
	uint64_t generation;
	bool has_floor;
	*floor=0;
	enum file_result result=current_generation_status(s->directory_fd,&generation,&has_floor);
	if(result!=FILE_OK||!has_floor)return result;
	int gfd;
	if(!open_generation(s->directory_fd,generation,&gfd))return missing_or_io_error();
	unsigned char data[8+HASH_SIZE],hash[HASH_SIZE];
	result=read_exact_status(gfd,"resume-floor",data,sizeof(data));
	if(close(gfd)<0)return FILE_ERROR;
	if(result!=FILE_OK)return result;
	digest_bytes(data,8,hash);
	*floor=get64(data);
	return *floor&&*floor<=generation&&memcmp(hash,data+8,HASH_SIZE)==0 ? FILE_OK : FILE_INVALID;
}
static DIR *directory_stream(int rootfd)
{
	int fd=openat(rootfd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)return NULL;
	DIR *d=fdopendir(fd);if(!d)close(fd);return d;
}
static enum file_result load_manifest_status(struct saves *s,uint64_t g,struct manifest *m)
{
	int gfd;
	if(!open_generation(s->directory_fd,g,&gfd))return missing_or_io_error();
	int fd=openat(gfd,"manifest",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
	enum file_result result=fd<0 ? missing_or_io_error() : FILE_OK;
	if(close(gfd)<0)result=FILE_ERROR;
	if(fd<0)return result;
	unsigned char b[MANIFEST_SIZE];
	struct stat st;
	size_t size=0;
	if(result==FILE_OK){
		if(fstat(fd,&st)<0)result=FILE_ERROR;
		else if(!S_ISREG(st.st_mode)||
			(st.st_size!=MANIFEST_V1_SIZE&&st.st_size!=MANIFEST_V2_SIZE&&
			 st.st_size!=MANIFEST_SIZE))result=FILE_INVALID;
		else{
			size=(size_t)st.st_size;
			result=read_bytes(fd,b,size);
		}
	}
	if(close(fd)<0)return FILE_ERROR;
	if(result!=FILE_OK)return result;
	if(!manifest_decode(b,size,m)||m->generation!=g||m->sram_size>MAX_MEMORY||
	   m->rtc_size>MAX_MEMORY||m->sram_size+m->rtc_size>MAX_MEMORY||
	   m->checkpoint_size>MAX_STATE)return FILE_INVALID;
	if(memcmp(m->core,s->core,HASH_SIZE)||memcmp(m->content,s->content,HASH_SIZE)||
	   memcmp(m->path,s->path,HASH_SIZE)||memcmp(m->options,s->options,HASH_SIZE))
		return FILE_INVALID;
	for(unsigned i=0;i<SAVES_STATE_SLOTS;i++)if(m->state_size[i]>MAX_STATE)return FILE_INVALID;
	return FILE_OK;
}

static bool load_manifest(struct saves *s,uint64_t g,struct manifest *m)
{
	return load_manifest_status(s,g,m)==FILE_OK;
}
#ifdef SAVES_TESTING
bool saves_test_downgrade_current_manifest(struct saves *s,unsigned version)
{
	uint64_t generation=0;if(!s||version<1||version>3||!current_generation(s->directory_fd,&generation))return false;
	int gfd=-1;if(!open_generation(s->directory_fd,generation,&gfd))return false;
	unsigned char encoded[MANIFEST_SIZE],legacy[MANIFEST_SIZE];bool ok=read_exact_at(gfd,"manifest",encoded,sizeof(encoded));
	struct manifest m={0};if(ok)ok=manifest_decode(encoded,sizeof(encoded),&m);
	/* v3 writers used the unqualified options hash when saving a slot. */
	if(ok&&version==3)
		for(unsigned i=0;i<SAVES_STATE_SLOTS;i++)
			if(m.state_size[i])memcpy(m.state_options[i],s->options,HASH_SIZE);
	if(ok){manifest_encode(&m,legacy,version);int fd=openat(gfd,"manifest",O_WRONLY|O_TRUNC|O_CLOEXEC|O_NOFOLLOW);if(fd<0)ok=false;else{size_t off=0,size=version==1?MANIFEST_V1_SIZE:version==2?MANIFEST_V2_SIZE:MANIFEST_SIZE;while(ok&&off<size){ssize_t n=write(fd,legacy+off,size-off);if(n>0)off+=(size_t)n;else if(n<0&&errno==EINTR)continue;else ok=false;}if(ok&&fsync(fd)<0)ok=false;if(close(fd)<0)ok=false;}}
	if(close(gfd)<0)ok=false;
	return ok;
}
#endif
static bool validate_payloads(struct saves *s,const struct manifest *m)
{
	int gfd;if(!open_generation(s->directory_fd,m->generation,&gfd))return false;bool valid=true;size_t mem=(size_t)(m->sram_size+m->rtc_size);
	if(mem){unsigned char *b=malloc(mem);if(!b)valid=false;else{unsigned char h[HASH_SIZE];valid=read_exact_at(gfd,"memory",b,mem);if(valid){digest_bytes(b,mem,h);valid=memcmp(h,m->memory_hash,HASH_SIZE)==0;}free(b);}}
	for(unsigned i=0;valid&&i<SAVES_STATE_SLOTS;i++)if(m->state_size[i]){unsigned char *b=malloc((size_t)m->state_size[i]);if(!b){valid=false;break;}char f[32];(void)snprintf(f,sizeof(f),"state-%02u",i);unsigned char h[HASH_SIZE];valid=read_exact_at(gfd,f,b,(size_t)m->state_size[i]);if(valid){digest_bytes(b,(size_t)m->state_size[i],h);valid=memcmp(h,m->state_hash[i],HASH_SIZE)==0;}free(b);}
	close(gfd);return valid;
}
static bool find_generation(struct saves *s,struct manifest *m)
{
	uint64_t current=0;if(current_generation(s->directory_fd,&current)&&load_manifest(s,current,m)&&validate_payloads(s,m))return true;
	DIR *d=directory_stream(s->directory_fd);if(!d)return false;uint64_t candidates[256];size_t count=0;struct dirent *e;
	while((e=readdir(d))!=NULL&&count<256){if(e->d_name[0]!='g'||strlen(e->d_name)!=17)continue;char *end;errno=0;uint64_t g=strtoull(e->d_name+1,&end,16);if(errno==0&&*end==0&&g!=current)candidates[count++]=g;}closedir(d);
	for(size_t i=0;i<count;i++)for(size_t j=i+1;j<count;j++)if(candidates[j]>candidates[i]){uint64_t x=candidates[i];candidates[i]=candidates[j];candidates[j]=x;}
	for(size_t i=0;i<count;i++)if(load_manifest(s,candidates[i],m)&&validate_payloads(s,m))return true;
	return false;
}
static enum file_result load_candidate_payload(struct saves *s,const struct manifest *m,
						 const char *name,size_t size,
						 const unsigned char expected[HASH_SIZE],
						 unsigned char **payload)
{
	*payload=NULL;
	if(size==0)return FILE_OK;
	unsigned char *b;
#ifdef SAVES_TESTING
	if(!strcmp(name,"checkpoint")&&test_event(SAVES_TEST_RESUME_ALLOC)){
		errno=ENOMEM;
		b=NULL;
	}else
#endif
		b=malloc(size);
	if(!b)return FILE_ERROR;
	int gfd;
	if(!open_generation(s->directory_fd,m->generation,&gfd)){
		enum file_result result=missing_or_io_error();
		free(b);
		return result;
	}
	enum file_result result;
#ifdef SAVES_TESTING
	if(!strcmp(name,"checkpoint")&&test_event(SAVES_TEST_RESUME_READ))result=FILE_ERROR;
	else
#endif
		result=read_exact_status(gfd,name,b,size);
	if(close(gfd)<0)result=FILE_ERROR;
	if(result==FILE_OK){
		unsigned char hash[HASH_SIZE];
		digest_bytes(b,size,hash);
		if(memcmp(hash,expected,HASH_SIZE))result=FILE_INVALID;
	}
	if(result==FILE_OK)*payload=b;
	else free(b);
	return result;
}
static bool find_generation_payload(struct saves *s,struct manifest *m,
					    const char *name,size_t size,
					    size_t sram_size,size_t rtc_size,
					    unsigned char **payload)
{
	uint64_t current=0,candidates[257];size_t count=0;
	if(current_generation(s->directory_fd,&current))candidates[count++]=current;
	DIR *d=directory_stream(s->directory_fd);if(!d)return false;struct dirent *e;
	while((e=readdir(d))!=NULL&&count<257){if(e->d_name[0]!='g'||strlen(e->d_name)!=17)continue;char *end;errno=0;uint64_t g=strtoull(e->d_name+1,&end,16);if(errno||*end||g==current)continue;candidates[count++]=g;}closedir(d);
	for(size_t i=0;i<count;i++)for(size_t j=i+1;j<count;j++)if(candidates[j]>candidates[i]&&candidates[i]!=current){uint64_t x=candidates[i];candidates[i]=candidates[j];candidates[j]=x;}
	for(size_t i=0;i<count;i++){
		if(!load_manifest(s,candidates[i],m)||!validate_payloads(s,m))continue;
		if(strcmp(name,"memory")==0&&(m->sram_size!=sram_size||m->rtc_size!=rtc_size))continue;
		if(name[0]=='s'){
			unsigned slot=(unsigned)(name[6]-'0')*10u+(unsigned)(name[7]-'0');
			if(m->state_size[slot]&&memcmp(m->state_options[slot],s->state_options,HASH_SIZE))
				return false;
			if(m->state_size[slot]!=size)continue;
		}
		if(strcmp(name,"checkpoint")==0&&m->checkpoint_size!=size)continue;
#ifdef SAVES_TESTING
		if(test_hook)test_hook();
#endif
		const unsigned char *expected=m->memory_hash;
		if(name[0]=='s')expected=m->state_hash[(unsigned)(name[6]-'0')*10u+(unsigned)(name[7]-'0')];
		if(strcmp(name,"checkpoint")==0)expected=m->checkpoint_hash;
		if(load_candidate_payload(s,m,name,size,expected,payload)==FILE_OK)return true;
	}
	return false;
}
static uint64_t highest_generation(int rootfd)
{
	DIR *d=directory_stream(rootfd);if(!d)return 0;uint64_t highest=0;struct dirent *e;
	while((e=readdir(d))!=NULL){if(e->d_name[0]!='g'||strlen(e->d_name)!=17)continue;char *end;errno=0;uint64_t g=strtoull(e->d_name+1,&end,16);if(errno==0&&*end==0&&g>highest)highest=g;}closedir(d);return highest;
}
/* Only an unused store can safely start with blank core memory after a failed load. */
bool saves_has_no_generation(struct saves *s)
{
	DIR *dir = directory_stream(s->directory_fd);
	if (!dir) return false;
	struct dirent *entry;
	errno = 0;
	while ((entry = readdir(dir)) != NULL) {
		if (!strcmp(entry->d_name, "CURRENT") ||
		    (entry->d_name[0] == 'g' && strlen(entry->d_name) == 17)) break;
	}
	bool empty = !entry && !errno;
	return closedir(dir) == 0 && empty;
}

static void remove_generation_at(int rootfd,const char *name)
{
	int fd=openat(rootfd,name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return;DIR *d=fdopendir(fd);if(!d){close(fd);return;}struct dirent *e;while((e=readdir(d))!=NULL)if(strcmp(e->d_name,".")&&strcmp(e->d_name,".."))(void)unlinkat(fd,e->d_name,0);closedir(d);(void)unlinkat(rootfd,name,AT_REMOVEDIR);
}
enum { RESUME_GENERATION_SIZE = 85, RESUME_VERIFIED_OPTIONS = 4 };

struct resume_marker {
	uint64_t generation;
	unsigned version;
	char hash[65];
};

/* An absent or malformed marker has no target. An I/O failure leaves its
 * target unknown, so callers must preserve the generations. */
static bool read_resume_marker(struct saves *s,struct resume_marker *marker)
{
	memset(marker,0,sizeof(*marker));
	int fd=openat(s->directory_fd,"RESUME",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
	if(fd<0)return errno==ENOENT||errno==ELOOP;
	char data[RESUME_GENERATION_SIZE];
	struct stat st;
	bool ok=fstat(fd,&st)==0;
	size_t size=0,offset=0;
	if(ok&&S_ISREG(st.st_mode)&&(st.st_size==3||st.st_size==68||st.st_size==RESUME_GENERATION_SIZE))
		size=(size_t)st.st_size;
	while(ok&&offset<size){
		ssize_t n=read(fd,data+offset,size-offset);
		if(n>0)offset+=(size_t)n;
		else if(n<0&&errno==EINTR)continue;
		else ok=false;
	}
	if(close(fd)<0)ok=false;
	if(!ok)return false;
	if(size==RESUME_GENERATION_SIZE&&data[0]=='v'&&(data[1]=='3'||data[1]=='4')&&
	   data[2]=='\n'&&data[19]=='\n'&&data[84]=='\n'&&
	   strspn(data+3,"0123456789abcdef")==16&&strspn(data+20,"0123456789abcdef")==64){
		data[19]=0;
		marker->generation=strtoull(data+3,NULL,16);
		if(marker->generation){
			memcpy(marker->hash,data+20,64);
			marker->version=(unsigned)(data[1]-'0');
		}
	}else if(size==68&&!memcmp(data,"v2\n",3)&&data[67]=='\n'&&
		 strspn(data+3,"0123456789abcdef")==64){
		memcpy(marker->hash,data+3,64);
		marker->version=2;
	}else if(size==3&&!memcmp(data,"v1\n",3))marker->version=1;
	return true;
}

/* Keep legacy checkpoints readable, including any possible legacy marker target. */
static bool legacy_resume_candidate(const struct resume_marker *marker,const struct manifest *m)
{
	if(!m->checkpoint_size||m->version>=3)return false;
	if(marker->version==1)return true;
	if(marker->version!=2)return false;
	char hash[65];
	hex_digest(m->checkpoint_hash,hash);
	return strcmp(marker->hash,hash)==0;
}

static uint64_t latest_checkpoint(struct saves *s,bool validate)
{
	DIR *d=directory_stream(s->directory_fd);if(!d)return 0;
	uint64_t latest=0;struct dirent *e;
	while((e=readdir(d))!=NULL){
		if(e->d_name[0]!='g'||strlen(e->d_name)!=17)continue;
		char *end;errno=0;uint64_t g=strtoull(e->d_name+1,&end,16);
		struct manifest m;
		if(errno||*end||g<=latest||!load_manifest(s,g,&m)||!m.checkpoint_size)continue;
		unsigned char *data=NULL;
		if(!validate||load_candidate_payload(s,&m,"checkpoint",(size_t)m.checkpoint_size,
						    m.checkpoint_hash,&data)==FILE_OK)latest=g;
		free(data);
	}
	closedir(d);
	return latest;
}

static void prune_generations(struct saves *s,uint64_t current,uint64_t previous)
{
	struct resume_marker marker;
	if(!read_resume_marker(s,&marker))return;
	uint64_t checkpoint=latest_checkpoint(s,false);
	DIR *d=directory_stream(s->directory_fd);if(!d)return;struct dirent *e;
	while((e=readdir(d))!=NULL){
		if(e->d_name[0]!='g'||strlen(e->d_name)!=17)continue;
		char *end;errno=0;uint64_t g=strtoull(e->d_name+1,&end,16);
		if(errno||*end||g==current||g==previous||g==checkpoint||g==marker.generation)continue;
		struct manifest m;
		if(marker.version==1||marker.version==2){
			enum file_result result=load_manifest_status(s,g,&m);
			if(result==FILE_ERROR||(result==FILE_OK&&legacy_resume_candidate(&marker,&m)))continue;
		}
		remove_generation_at(s->directory_fd,e->d_name);
	}
	closedir(d);
}

static void digest_verified_options(const unsigned char options[HASH_SIZE],
				    unsigned char verified[HASH_SIZE])
{
	/* Earlier writers recorded requested options. Qualify newly captured
	 * slots without changing the manifest format or battery store identity. */
	static const char prefix[]="banana-retro-verified-state-options-v1:";
	struct sha256 h;
	sha_init(&h);
	sha_update(&h,prefix,sizeof(prefix)-1);
	sha_update(&h,options,HASH_SIZE);
	sha_final(&h,verified);
}

struct saves *saves_open_with_options(const char *root,const char *core_path,const char *content_path,const char *options)
{
	if(!root||!core_path||!content_path||!options||strlen(root)>=PATH_MAX||strlen(options)>4096)return NULL;
	char *core=realpath(core_path,NULL),*content=realpath(content_path,NULL);if(!core||!content){free(core);free(content);return NULL;}
	struct saves *s=calloc(1,sizeof(*s));if(!s){free(core);free(content);return NULL;}s->directory_fd=-1;
	memcpy(s->destination_root,root,strlen(root)+1);
	bool ok=digest_file(core,s->core)&&digest_file(content,s->content);digest_bytes(content,strlen(content),s->path);digest_bytes(options,strlen(options),s->options);
	digest_verified_options(s->options,s->state_options);
	int root_fd=ok?make_dirs(root):-1;
	if(root_fd<0)ok=false;
	if(ok){struct sha256 h;unsigned char key[HASH_SIZE];sha_init(&h);sha_update(&h,s->core,HASH_SIZE);sha_update(&h,s->content,HASH_SIZE);sha_update(&h,s->path,HASH_SIZE);sha_update(&h,s->options,HASH_SIZE);sha_final(&h,key);static const char hex[]="0123456789abcdef";char leaf[HASH_SIZE*2+1];for(unsigned i=0;i<HASH_SIZE;i++){leaf[2*i]=hex[key[i]>>4];leaf[2*i+1]=hex[key[i]&15];}leaf[64]=0;
		const char *cb=strrchr(core,'/'),*rb=strrchr(content,'/');cb=cb?cb+1:core;rb=rb?rb+1:content;
		if(strlen(cb)>NAME_MAX||strlen(rb)>NAME_MAX)ok=false;
		else{memcpy(s->core_file,cb,strlen(cb)+1);memcpy(s->content_file,rb,strlen(rb)+1);}
		const char *slash=strrchr(content,'/');size_t dir_size=slash?(size_t)(slash-content):1u;
		const char *dir=slash?content:".";
		if(dir_size>=sizeof(s->content_dir))ok=false;
		else{memcpy(s->content_dir,dir,dir_size);s->content_dir[dir_size]=0;}
		if(ok&&mkdirat(root_fd,leaf,0700)<0&&errno!=EEXIST)ok=false;
		int identity_fd=-1;
		if(ok){identity_fd=openat(root_fd,leaf,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);if(identity_fd<0)ok=false;}
		struct stat st;
		if(ok&&(fstat(identity_fd,&st)<0||!S_ISDIR(st.st_mode)))ok=false;
		/* Also sync on EEXIST so a retry completes a prior failed publication. */
		if(ok&&!sync_parent_directory(root_fd))ok=false;
		if(ok)s->directory_fd=identity_fd;else if(identity_fd>=0)close(identity_fd);
	}
	if(root_fd>=0)close(root_fd);
	free(core);free(content);if(!ok){if(s->directory_fd>=0)close(s->directory_fd);free(s);return NULL;}return s;
}
struct saves *saves_open(const char *root,const char *core_path,const char *content_path)
{return saves_open_with_options(root,core_path,content_path,"banana-retro-options-v1:none");}
void saves_set_state_options(struct saves *s,const char *options)
{
	unsigned char hash[HASH_SIZE];
	digest_bytes(options,strlen(options),hash);
	digest_verified_options(hash,s->state_options);
}
bool saves_checkpoint_options_match(const struct saves *s)
{
	unsigned char verified[HASH_SIZE];
	digest_verified_options(s->options,verified);
	return memcmp(s->state_options,verified,HASH_SIZE)==0;
}
bool saves_set_legacy_core_name(struct saves *s,const char *name)
{
	if(!s||!name||!*name||strlen(name)>NAME_MAX||strchr(name,'/')||!strcmp(name,".")||!strcmp(name,".."))return false;
	memcpy(s->core_name,name,strlen(name)+1);return true;
}
void saves_close(struct saves *s){if(s){if(s->directory_fd>=0)close(s->directory_fd);free(s);}}
static bool add_legacy_directory(char paths[5][PATH_MAX],unsigned *count,
				 const char *root,const char *directory)
{
	char path[PATH_MAX];int n=directory&&*directory?
		snprintf(path,sizeof(path),"%s/%s",root,directory):snprintf(path,sizeof(path),"%s",root);
	if(n<=0||(size_t)n>=sizeof(path))return false;
	for(unsigned i=0;i<*count;i++)if(strcmp(paths[i],path)==0)return true;
	if(*count>=5)return false;
	memcpy(paths[*count],path,(size_t)n+1);++*count;return true;
}
static unsigned legacy_directories(struct saves *s,const char *root,char paths[5][PATH_MAX],
				   char stem[NAME_MAX+1])
{
	char core[NAME_MAX+1];size_t n=strlen(s->content_file);if(n>NAME_MAX)return 0;
	memcpy(stem,s->content_file,n+1);char *dot=strrchr(stem,'.');if(dot)*dot=0;
	if(!*stem)return 0;
	memcpy(core,s->core_file,strlen(s->core_file)+1);
	char *ext=strstr(core,"_libretro.so");if(ext&&ext[12]=='\0')*ext=0;
	unsigned count=0;
	if(!add_legacy_directory(paths,&count,root,NULL)||
	   !add_legacy_directory(paths,&count,root,core))return 0;
	if(strcmp(core,s->core_file)!=0&&!add_legacy_directory(paths,&count,root,s->core_file))return 0;
	if(*s->core_name&&!add_legacy_directory(paths,&count,root,s->core_name))return 0;
	if(!add_legacy_directory(paths,&count,s->content_dir,NULL))return 0;
	return count;
}
static int legacy_file(const char *directory,const char *stem,const char *suffix,
		       size_t expected,char path[PATH_MAX])
{
	int n=snprintf(path,PATH_MAX,"%s/%s.%s",directory,stem,suffix);
	if(n<=0||n>=PATH_MAX)return -1;
	int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
	if(fd<0)return errno==ENOENT?0:-1;
	struct stat st;bool valid=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>=0&&
		(uintmax_t)st.st_size==(uintmax_t)expected;
	if(close(fd)<0)valid=false;
	return valid?1:-1;
}
struct legacy_selection { char sram[PATH_MAX],rtc[PATH_MAX]; };
static enum saves_legacy_result legacy_select(struct saves *s,const char *root,size_t ss,size_t rs,
					      struct legacy_selection *selected)
{
	if(!s||!root||!*root||ss>MAX_MEMORY||rs>MAX_MEMORY||ss+rs>MAX_MEMORY||ss==0)
		return SAVES_LEGACY_INVALID;
	char directories[5][PATH_MAX],stem[NAME_MAX+1];
	unsigned count=legacy_directories(s,root,directories,stem),present=0;
	if(!count)return SAVES_LEGACY_INVALID;
	struct legacy_selection match={0};bool complete=false;
	for(unsigned i=0;i<count;i++){
		char sram[PATH_MAX],rtc[PATH_MAX];
		int sn=legacy_file(directories[i],stem,"srm",ss,sram);
		int rn=rs?legacy_file(directories[i],stem,"rtc",rs,rtc):0;
		if(sn<0||rn<0)return SAVES_LEGACY_INVALID;
		if(!sn&&!rn)continue;
		if(++present>1)return SAVES_LEGACY_AMBIGUOUS;
		if(sn&&(!rs||rn)){
			memcpy(match.sram,sram,strlen(sram)+1);
			if(rs)memcpy(match.rtc,rtc,strlen(rtc)+1);
			complete=true;
		}
	}
	if(!present)return SAVES_LEGACY_NONE;
	if(!complete)return SAVES_LEGACY_INVALID;
	if(selected)*selected=match;
	return SAVES_LEGACY_READY;
}
static bool stat_same_file(const struct stat *a,const struct stat *b)
{
	return a->st_dev==b->st_dev&&a->st_ino==b->st_ino&&a->st_size==b->st_size&&
		a->st_mtim.tv_sec==b->st_mtim.tv_sec&&a->st_mtim.tv_nsec==b->st_mtim.tv_nsec&&
		a->st_ctim.tv_sec==b->st_ctim.tv_sec&&a->st_ctim.tv_nsec==b->st_ctim.tv_nsec;
}
static bool legacy_read(const char *path,unsigned char *out,size_t size,unsigned char hash[HASH_SIZE])
{
	int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return false;
	struct stat before,after;bool ok=fstat(fd,&before)==0&&S_ISREG(before.st_mode)&&before.st_size>=0&&
		(uintmax_t)before.st_size==(uintmax_t)size;
	size_t off=0;while(ok&&off<size){ssize_t n=read(fd,out+off,size-off);if(n>0)off+=(size_t)n;else if(n<0&&errno==EINTR)continue;else ok=false;}
	if(ok)ok=fstat(fd,&after)==0&&stat_same_file(&before,&after);
	if(close(fd)<0)ok=false;
	if(ok)digest_bytes(out,size,hash);
	return ok;
}
struct destination_budget { unsigned entries; uint64_t bytes; };
static bool stat_same_file(const struct stat *a,const struct stat *b);
static void digest_stat(struct sha256 *hash,const struct stat *st)
{
	digest_u64(hash,(uint64_t)st->st_mode);digest_u64(hash,(uint64_t)st->st_dev);
	digest_u64(hash,(uint64_t)st->st_ino);digest_u64(hash,(uint64_t)st->st_size);
	digest_u64(hash,(uint64_t)st->st_mtim.tv_sec);digest_u64(hash,(uint64_t)st->st_mtim.tv_nsec);
	digest_u64(hash,(uint64_t)st->st_ctim.tv_sec);digest_u64(hash,(uint64_t)st->st_ctim.tv_nsec);
}
static bool digest_destination_file(struct sha256 *hash,int dirfd,const char *name,
				   struct destination_budget *budget)
{
	int fd=openat(dirfd,name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return false;
	struct stat before,after;bool ok=fstat(fd,&before)==0&&S_ISREG(before.st_mode)&&before.st_size>=0;
	if(ok&&((uint64_t)before.st_size>MAX_DESTINATION_BYTES-budget->bytes))ok=false;
	if(ok)budget->bytes+=(uint64_t)before.st_size;
	unsigned char block[16384];uint64_t read_bytes=0;
	while(ok){ssize_t n=read(fd,block,sizeof(block));if(n>0){
		if((uint64_t)n>(uint64_t)before.st_size-read_bytes){ok=false;break;}
		read_bytes+=(uint64_t)n;sha_update(hash,block,(size_t)n);
	}else if(n==0)break;else if(errno!=EINTR)ok=false;}
	if(ok&&read_bytes!=(uint64_t)before.st_size)ok=false;
	if(ok)ok=fstat(fd,&after)==0&&stat_same_file(&before,&after);
	if(close(fd)<0)ok=false;
	return ok;
}
static bool digest_destination_directory(struct sha256 *hash,int dirfd,unsigned depth,
					struct destination_budget *budget)
{
	struct stat before,after;if(fstat(dirfd,&before)<0||!S_ISDIR(before.st_mode))return false;
	int scanfd=openat(dirfd,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(scanfd<0)return false;
	DIR *d=fdopendir(scanfd);if(!d){close(scanfd);return false;}
	struct dirent *e;bool ok=true;
	while(ok&&(e=readdir(d))!=NULL){
		if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
		if(++budget->entries>MAX_DESTINATION_ENTRIES){ok=false;break;}
		struct stat st;if(fstatat(dirfd,e->d_name,&st,AT_SYMLINK_NOFOLLOW)<0){ok=false;break;}
		digest_string(hash,e->d_name);digest_stat(hash,&st);
		if(S_ISREG(st.st_mode))ok=digest_destination_file(hash,dirfd,e->d_name,budget);
		else if(S_ISDIR(st.st_mode)){
			if(depth>=2){ok=false;break;}
			int child=openat(dirfd,e->d_name,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
			if(child<0)ok=false;
			else{ok=digest_destination_directory(hash,child,depth+1,budget);if(close(child)<0)ok=false;}
		}
	}
	if(closedir(d)<0)ok=false;
	if(ok)ok=fstat(dirfd,&after)==0&&stat_same_file(&before,&after);
	return ok;
}
static bool destination_fingerprint(struct saves *s,unsigned char out[HASH_SIZE])
{
	struct sha256 hash;sha_init(&hash);struct stat root_stat;
	if(fstat(s->directory_fd,&root_stat)<0||!S_ISDIR(root_stat.st_mode))return false;
	digest_u64(&hash,(uint64_t)root_stat.st_ino);digest_u64(&hash,(uint64_t)root_stat.st_dev);
	struct destination_budget budget={0};
	if(!digest_destination_directory(&hash,s->directory_fd,0,&budget))return false;
	sha_final(&hash,out);return true;
}
static bool legacy_receipt(struct saves *s,const char *root,size_t ss,size_t rs,
			   const struct legacy_selection *selection,const unsigned char *memory,
			   char receipt[65])
{
	unsigned char sram_hash[HASH_SIZE],rtc_hash[HASH_SIZE],destination[HASH_SIZE],out[HASH_SIZE];
	digest_bytes(memory,ss,sram_hash);if(rs)digest_bytes(memory+ss,rs,rtc_hash);else memset(rtc_hash,0,sizeof(rtc_hash));
	if(!destination_fingerprint(s,destination))return false;
	struct sha256 hash;sha_init(&hash);sha_update(&hash,s->core,HASH_SIZE);sha_update(&hash,s->content,HASH_SIZE);
	sha_update(&hash,s->path,HASH_SIZE);sha_update(&hash,s->options,HASH_SIZE);
	digest_string(&hash,s->destination_root);digest_string(&hash,root);
	digest_string(&hash,selection->sram);digest_string(&hash,selection->rtc);
	digest_u64(&hash,(uint64_t)ss);digest_u64(&hash,(uint64_t)rs);
	sha_update(&hash,sram_hash,HASH_SIZE);sha_update(&hash,rtc_hash,HASH_SIZE);sha_update(&hash,destination,HASH_SIZE);
	sha_final(&hash,out);hex_digest(out,receipt);return true;
}
static bool saves_commit_internal(struct saves *s,const void *sram,size_t ss,const void *rtc,size_t rs,
				  int slot,const void *state,size_t state_size,
				  const void *checkpoint,size_t checkpoint_size,bool battery_only,
				  struct manifest *published);
static enum saves_legacy_result legacy_prepare(struct saves *s,const char *root,size_t ss,size_t rs,
					       const char *expected_receipt,char receipt[65],
					       unsigned char **memory_out)
{
	struct manifest existing={0};if(find_generation(s,&existing))return SAVES_LEGACY_ALREADY_IMPORTED;
	struct legacy_selection selection={0};enum saves_legacy_result result=legacy_select(s,root,ss,rs,&selection);
	if(result!=SAVES_LEGACY_READY)return result;
	if(ss>SIZE_MAX-rs)return SAVES_LEGACY_INVALID;
	unsigned char *memory=malloc(ss+rs);if(!memory)return SAVES_LEGACY_INVALID;
	unsigned char digest[HASH_SIZE];bool ok=legacy_read(selection.sram,memory,ss,digest)&&
		(!rs||legacy_read(selection.rtc,memory+ss,rs,digest));
	if(ok)ok=legacy_receipt(s,root,ss,rs,&selection,memory,receipt);
	if(!ok){free(memory);return SAVES_LEGACY_INVALID;}
	if(expected_receipt&&strcmp(expected_receipt,receipt)!=0){free(memory);return SAVES_LEGACY_CHANGED;}
	*memory_out=memory;return SAVES_LEGACY_READY;
}
enum saves_legacy_result saves_legacy_preview(struct saves *s,const char *root,size_t ss,size_t rs,
					      char receipt[65])
{
	if(!s||flock(s->directory_fd,LOCK_SH)<0)return SAVES_LEGACY_INVALID;
	unsigned char *memory=NULL;char generated[65];
	enum saves_legacy_result result=legacy_prepare(s,root,ss,rs,NULL,generated,&memory);
	if(result==SAVES_LEGACY_READY&&receipt)memcpy(receipt,generated,sizeof(generated));
	free(memory);(void)flock(s->directory_fd,LOCK_UN);return result;
}
enum saves_legacy_result saves_legacy_import(struct saves *s,const char *root,size_t ss,size_t rs,
					      const char *receipt)
{
	if(!s||!receipt||strlen(receipt)!=64||flock(s->directory_fd,LOCK_EX)<0)return SAVES_LEGACY_INVALID;
	unsigned char *memory=NULL;char generated[65];
	enum saves_legacy_result result=legacy_prepare(s,root,ss,rs,receipt,generated,&memory);
	if(result==SAVES_LEGACY_READY){
		bool committed=saves_commit_internal(s,memory,ss,memory+ss,rs,-1,NULL,0,NULL,0,false,NULL);
		result=committed?SAVES_LEGACY_READY:SAVES_LEGACY_INVALID;
	}
	free(memory);(void)flock(s->directory_fd,LOCK_UN);return result;
}
bool saves_state_capture(saves_state_fn serialize,void *context,void *buffer,size_t size)
{return serialize&&buffer&&size>0&&serialize(context,buffer,size);}
bool saves_state_restore(saves_unserialize_fn unserialize,void *context,const void *buffer,size_t size)
{return unserialize&&buffer&&size>0&&unserialize(context,buffer,size);}
bool saves_load_memory(struct saves *s,void *sram,size_t ss,void *rtc,size_t rs)
{
	if(!s||(!sram&&ss)||(!rtc&&rs))return false;
	if(ss>MAX_MEMORY||rs>MAX_MEMORY||ss+rs>MAX_MEMORY)return false;
	struct manifest m={0};unsigned char *b=NULL;size_t n=ss+rs;
	if(!find_generation_payload(s,&m,"memory",n,ss,rs,&b)){free(b);return false;}
	if(ss)memcpy(sram,b,ss);
	if(rs)memcpy(rtc,b+ss,rs);
	free(b);return true;
}
bool saves_load_state(struct saves *s,unsigned slot,void *data,size_t size)
{
	if(!s||slot>=SAVES_STATE_SLOTS||(!data&&size))return false;
	if(!size||size>MAX_STATE)return false;
	char f[32];(void)snprintf(f,sizeof(f),"state-%02u",slot);struct manifest m={0};unsigned char *b=NULL;
	if(!find_generation_payload(s,&m,f,size,SIZE_MAX,SIZE_MAX,&b)){free(b);return false;}
	memcpy(data,b,size);free(b);return true;
}
static bool saves_commit_internal(struct saves *s,const void *sram,size_t ss,const void *rtc,size_t rs,
				  int slot,const void *state,size_t state_size,
				  const void *checkpoint,size_t checkpoint_size,bool battery_only,
				  struct manifest *published)
{
	if(!s||(!sram&&ss)||(!rtc&&rs)||ss>MAX_MEMORY||rs>MAX_MEMORY||ss+rs>MAX_MEMORY||slot>=(int)SAVES_STATE_SLOTS||slot< -1||(!state&&state_size)||state_size>MAX_STATE||(slot<0&&state_size)||(slot>=0&&state_size==0)||(!checkpoint&&checkpoint_size)||checkpoint_size>MAX_STATE)return false;
	struct manifest prev={0};bool have=find_generation(s,&prev);
	if (!have && !saves_has_no_generation(s)) return false;
	uint64_t highest=highest_generation(s->directory_fd);uint64_t generation=highest> (have?prev.generation:0)?highest+1:(have?prev.generation+1:1);if(generation==0)return false;
	uint64_t floor=0;
	if(battery_only)floor=generation;
	else if(have){
		enum file_result result=read_resume_floor(s,&floor);
		if(result==FILE_ERROR)return false;
		/* Recovery from a damaged CURRENT starts from the validated battery data. */
		if(result==FILE_INVALID)floor=generation-1;
	}
	struct manifest m={.generation=generation,.sram_size=ss,.rtc_size=rs,.checkpoint_size=checkpoint_size};memcpy(m.core,s->core,HASH_SIZE);memcpy(m.content,s->content,HASH_SIZE);memcpy(m.path,s->path,HASH_SIZE);memcpy(m.options,s->options,HASH_SIZE);
	char temp[64],final[32];int n=-1,tdfd=-1;bool made=false;
	for(unsigned i=0;i<100;i++){n=snprintf(temp,sizeof(temp),".tmp-%ld-%016" PRIx64 "-%u",(long)getpid(),generation,i);if(n<0||(size_t)n>=sizeof(temp))return false;if(mkdirat(s->directory_fd,temp,0700)==0){made=true;break;}if(errno!=EEXIST)return false;}
	if(!made||!generation_name(final,generation))return false;
	tdfd=openat(s->directory_fd,temp,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
	if(tdfd<0){(void)unlinkat(s->directory_fd,temp,AT_REMOVEDIR);return false;}
	bool ok=true;size_t mem=ss+rs;if(mem){unsigned char *b=malloc(mem);if(!b)ok=false;else{if(ss)memcpy(b,sram,ss);if(rs)memcpy(b+ss,rtc,rs);digest_bytes(b,mem,m.memory_hash);ok=write_file_at(tdfd,"memory",b,mem);free(b);}}
	if(ok&&checkpoint_size){digest_bytes(checkpoint,checkpoint_size,m.checkpoint_hash);ok=write_file_at(tdfd,"checkpoint",checkpoint,checkpoint_size);}
	for(unsigned i=0;ok&&i<SAVES_STATE_SLOTS;i++){
		char f[32];(void)snprintf(f,sizeof(f),"state-%02u",i);
		if((int)i==slot){m.state_size[i]=state_size;memcpy(m.state_options[i],s->state_options,HASH_SIZE);digest_bytes(state,state_size,m.state_hash[i]);ok=write_file_at(tdfd,f,state,state_size);}
		else if(have&&prev.state_size[i]){unsigned char *b=NULL;if(load_candidate_payload(s,&prev,f,(size_t)prev.state_size[i],prev.state_hash[i],&b)!=FILE_OK){ok=false;break;}m.state_size[i]=prev.state_size[i];memcpy(m.state_options[i],prev.state_options[i],HASH_SIZE);memcpy(m.state_hash[i],prev.state_hash[i],HASH_SIZE);ok=write_file_at(tdfd,f,b,(size_t)m.state_size[i]);free(b);}
	}
	if(ok){
		unsigned char encoded[MANIFEST_SIZE];
		manifest_encode(&m,encoded,3);
		memcpy(m.hash,encoded+sizeof(encoded)-HASH_SIZE,HASH_SIZE);
		ok=write_file_at(tdfd,"manifest",encoded,sizeof(encoded));
	}
#ifdef SAVES_TESTING
	if(ok)test_event(SAVES_TEST_AFTER_MANIFEST_WRITE);
#endif
	if(ok&&floor){
		unsigned char data[8+HASH_SIZE];
		put64(data,floor);
		digest_bytes(data,8,data+8);
		ok=write_file_at(tdfd,"resume-floor",data,sizeof(data));
	}
#ifdef SAVES_TESTING
	if(ok&&test_event(SAVES_TEST_TEMP_DIR_FSYNC))ok=false;
#endif
	if(ok&&fsync(tdfd)<0)ok=false;
	if(close(tdfd)<0)ok=false;
	tdfd=-1;
#ifdef SAVES_TESTING
	if(ok&&test_event(SAVES_TEST_GENERATION_RENAME))ok=false;
#endif
	if(ok&&renameat(s->directory_fd,temp,s->directory_fd,final)<0)ok=false;
#ifdef SAVES_TESTING
	if(ok)test_event(SAVES_TEST_AFTER_GENERATION_RENAME);
	if(ok&&test_event(SAVES_TEST_PARENT_DIR_FSYNC))ok=false;
#endif
	if(ok&&fsync(s->directory_fd)<0)ok=false;
	char current_temp[64],text[32];int tn=snprintf(text,sizeof(text),"%s%" PRIu64 "\n",floor?"+":"",generation);int cn=snprintf(current_temp,sizeof(current_temp),".CURRENT-%ld-%016" PRIx64,(long)getpid(),generation);
	if(ok)ok=tn>0&&(size_t)tn<sizeof(text)&&cn>0&&(size_t)cn<sizeof(current_temp)&&write_file_at(s->directory_fd,current_temp,text,(size_t)tn);
#ifdef SAVES_TESTING
	if(ok&&test_event(SAVES_TEST_CURRENT_RENAME))ok=false;
#endif
	if(ok&&renameat(s->directory_fd,current_temp,s->directory_fd,"CURRENT")<0)ok=false;
	if(!ok&&cn>0&&(size_t)cn<sizeof(current_temp))(void)unlinkat(s->directory_fd,current_temp,0);
#ifdef SAVES_TESTING
	if(ok)test_event(SAVES_TEST_AFTER_CURRENT_RENAME);
	if(ok&&test_event(SAVES_TEST_PARENT_DIR_FSYNC))ok=false;
#endif
	if(ok&&fsync(s->directory_fd)<0)ok=false;
	if(ok){
		prune_generations(s,generation,have?prev.generation:0);
		(void)fsync(s->directory_fd);
		if(published)*published=m;
	}
	if(tdfd>=0)close(tdfd);
	if(!ok)remove_generation_at(s->directory_fd,temp);
	return ok;
}
bool saves_commit(struct saves *s,const void *sram,size_t ss,const void *rtc,size_t rs,int slot,const void *state,size_t state_size)
{
	if(!s||flock(s->directory_fd,LOCK_EX)<0)return false;
	bool ok=saves_commit_internal(s,sram,ss,rtc,rs,slot,state,state_size,NULL,0,false,NULL);
	(void)flock(s->directory_fd,LOCK_UN);
	return ok;
}
static bool retire_resume_marker(struct saves *s);

bool saves_commit_battery_only(struct saves *s,const void *sram,size_t ss,const void *rtc,size_t rs)
{
	if(!s||flock(s->directory_fd,LOCK_EX)<0)return false;
	bool ok=saves_commit_internal(s,sram,ss,rtc,rs,-1,NULL,0,NULL,0,true,NULL);
	/* CURRENT already excludes the old resume. Removing it also helps older readers. */
	if(ok)(void)retire_resume_marker(s);
	(void)flock(s->directory_fd,LOCK_UN);
	return ok;
}
static bool publish_resume(struct saves *s,const struct manifest *m)
{
	char data[RESUME_GENERATION_SIZE+1],temp[64],hash[65];
	hex_digest(m->hash,hash);
	int n=snprintf(data,sizeof(data),"v%u\n%016" PRIx64 "\n%s\n",
		(unsigned)RESUME_VERIFIED_OPTIONS,m->generation,hash);
	int tn=snprintf(temp,sizeof(temp),".RESUME-%ld",(long)getpid());
	if(n!=RESUME_GENERATION_SIZE||tn<0||(size_t)tn>=sizeof(temp))return false;
	(void)unlinkat(s->directory_fd,temp,0);
	bool ok=write_file_at(s->directory_fd,temp,data,RESUME_GENERATION_SIZE);
#ifdef SAVES_TESTING
	if(ok&&test_event(SAVES_TEST_RESUME_RENAME))ok=false;
#endif
	if(ok&&renameat(s->directory_fd,temp,s->directory_fd,"RESUME")<0)ok=false;
	if(!ok){(void)unlinkat(s->directory_fd,temp,0);return false;}
	/* The previous marker target was pinned during generation publication.
	 * The next successful generation commit can reclaim it. */
	return sync_parent_directory(s->directory_fd);
}

bool saves_commit_checkpoint(struct saves *s,const void *sram,size_t ss,const void *rtc,size_t rs,
			     const void *checkpoint,size_t checkpoint_size)
{
	if(!s||!checkpoint_size||!saves_checkpoint_options_match(s)||
	   flock(s->directory_fd,LOCK_EX)<0)return false;
	struct manifest m;
	bool ok=saves_commit_internal(s,sram,ss,rtc,rs,-1,NULL,0,checkpoint,checkpoint_size,false,&m)&&
		publish_resume(s,&m);
	(void)flock(s->directory_fd,LOCK_UN);
	return ok;
}
bool saves_state_present(struct saves *s,unsigned slot)
{
	if(!s||slot>=SAVES_STATE_SLOTS)return false;
	struct manifest m={0};return find_generation(s,&m)&&m.state_size[slot]>0;
}
bool saves_checkpoint_present(struct saves *s)
{
	return s&&latest_checkpoint(s,true)!=0;
}
bool saves_load_checkpoint(struct saves *s,void *data,size_t size)
{
	if(!s||!data||!size||size>MAX_STATE)return false;
	struct manifest m={0};unsigned char *b=NULL;
	if(!find_generation_payload(s,&m,"checkpoint",size,SIZE_MAX,SIZE_MAX,&b)){free(b);return false;}
	memcpy(data,b,size);free(b);return true;
}
static enum saves_resume_result resume_file_result(enum file_result result)
{
	if(result==FILE_OK)return SAVES_RESUME_READY;
	return result==FILE_INVALID ? SAVES_RESUME_INVALID : SAVES_RESUME_RETRY;
}

static enum saves_resume_result resume_generation(struct saves *s,struct manifest *m,char token[65])
{
	token[0]=0;
	struct resume_marker marker;
	if(!read_resume_marker(s,&marker))return SAVES_RESUME_RETRY;
	/* Older writers did not verify the core's effective options. Keep those
	 * snapshots and their markers for recovery without automatically loading them. */
	if(marker.version!=RESUME_VERIFIED_OPTIONS||!saves_checkpoint_options_match(s))
		return SAVES_RESUME_NONE;
	uint64_t floor;
	enum file_result result=read_resume_floor(s,&floor);
	if(result==FILE_ERROR)return SAVES_RESUME_RETRY;
	if(result==FILE_INVALID||marker.generation<=floor)return SAVES_RESUME_NONE;
	result=load_manifest_status(s,marker.generation,m);
	memcpy(token,marker.hash,sizeof(marker.hash));
	if(result!=FILE_OK)return resume_file_result(result);
	char hash[65];
	hex_digest(m->hash,hash);
	return m->version==3&&m->checkpoint_size&&strcmp(token,hash)==0 ?
		SAVES_RESUME_READY : SAVES_RESUME_INVALID;
}

static enum saves_resume_result load_resume_payloads(struct saves *s,const struct manifest *m,
						     unsigned char **memory,unsigned char **checkpoint)
{
	enum file_result result=load_candidate_payload(s,m,"memory",
		(size_t)(m->sram_size+m->rtc_size),m->memory_hash,memory);
	if(result==FILE_OK)
		result=load_candidate_payload(s,m,"checkpoint",(size_t)m->checkpoint_size,
			m->checkpoint_hash,checkpoint);
	return resume_file_result(result);
}

enum saves_resume_result saves_resume_probe(struct saves *s,char token[65])
{
	if(!token)return SAVES_RESUME_RETRY;
	token[0]=0;
	if(!s||flock(s->directory_fd,LOCK_SH)<0)return SAVES_RESUME_RETRY;
	struct manifest m;
	unsigned char *checkpoint=NULL,*memory=NULL;
	enum saves_resume_result result=resume_generation(s,&m,token);
	if(result==SAVES_RESUME_READY)result=load_resume_payloads(s,&m,&memory,&checkpoint);
	free(memory);
	free(checkpoint);
	(void)flock(s->directory_fd,LOCK_UN);
	return result;
}

enum saves_resume_result saves_load_resume(struct saves *s,const char *expected_token,void *state,size_t size,
					  void *sram,size_t ss,void *rtc,size_t rs)
{
	if(!s||!expected_token||!state||!size||size>MAX_STATE||(!sram&&ss)||(!rtc&&rs)||
	   ss>MAX_MEMORY||rs>MAX_MEMORY-ss||flock(s->directory_fd,LOCK_SH)<0)
		return SAVES_RESUME_RETRY;
	struct manifest m;
	char token[65];
	unsigned char *checkpoint=NULL,*memory=NULL;
	enum saves_resume_result result=resume_generation(s,&m,token);
	if(strcmp(expected_token,token))result=SAVES_RESUME_RETRY;
	if(result==SAVES_RESUME_READY){
		result=m.checkpoint_size==size&&m.sram_size==ss&&m.rtc_size==rs ?
			load_resume_payloads(s,&m,&memory,&checkpoint) : SAVES_RESUME_INVALID;
	}
	if(result==SAVES_RESUME_READY){
		memcpy(state,checkpoint,size);
		if(ss)memcpy(sram,memory,ss);
		if(rs)memcpy(rtc,memory+ss,rs);
	}
	free(checkpoint);
	free(memory);
	(void)flock(s->directory_fd,LOCK_UN);
	return result;
}
static bool retire_resume_marker(struct saves *s)
{
	return (unlinkat(s->directory_fd,"RESUME",0)==0||errno==ENOENT)&&
		sync_parent_directory(s->directory_fd);
}

bool saves_resume_clear_if(struct saves *s,const char token[65])
{
	if(!s||!token||strlen(token)!=64||flock(s->directory_fd,LOCK_EX)<0)return false;
	struct resume_marker marker;
	bool ok=read_resume_marker(s,&marker);
	if(ok&&marker.version==RESUME_VERIFIED_OPTIONS&&strcmp(marker.hash,token)==0)
		ok=retire_resume_marker(s);
	(void)flock(s->directory_fd,LOCK_UN);
	return ok;
}
