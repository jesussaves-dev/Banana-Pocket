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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

enum { HASH_SIZE = 32, MAX_MEMORY = 64 * 1024 * 1024, MAX_STATE = 256 * 1024 * 1024 };
static const unsigned char magic[8] = {'B','R','S','A','V','E','0','1'};
#ifdef SAVES_TESTING
static size_t test_write_budget = SIZE_MAX;
void saves_test_fail_write_after(size_t bytes) { test_write_budget = bytes; }
#endif

struct saves {
	char directory[PATH_MAX];
	unsigned char core[HASH_SIZE], content[HASH_SIZE], path[HASH_SIZE], options[HASH_SIZE];
};

struct manifest {
	uint64_t generation, sram_size, rtc_size, state_size[SAVES_STATE_SLOTS];
	unsigned char core[HASH_SIZE], content[HASH_SIZE], path[HASH_SIZE], options[HASH_SIZE];
	unsigned char memory_hash[HASH_SIZE], state_hash[SAVES_STATE_SLOTS][HASH_SIZE];
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

static void put64(unsigned char *p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(unsigned char)(v>>(i*8));}
static uint64_t get64(const unsigned char *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(i*8);return v;}
static size_t manifest_size(void){return 8+8*(4+SAVES_STATE_SLOTS)+HASH_SIZE*(5+SAVES_STATE_SLOTS)+HASH_SIZE;}
static void manifest_encode(const struct manifest *m,unsigned char *b)
{
	size_t o=0;memcpy(b+o,magic,8);o+=8;put64(b+o,1);o+=8;put64(b+o,m->generation);o+=8;put64(b+o,m->sram_size);o+=8;put64(b+o,m->rtc_size);o+=8;
	for(unsigned i=0;i<SAVES_STATE_SLOTS;i++){put64(b+o,m->state_size[i]);o+=8;}
	memcpy(b+o,m->core,HASH_SIZE);o+=HASH_SIZE;memcpy(b+o,m->content,HASH_SIZE);o+=HASH_SIZE;memcpy(b+o,m->path,HASH_SIZE);o+=HASH_SIZE;memcpy(b+o,m->options,HASH_SIZE);o+=HASH_SIZE;memcpy(b+o,m->memory_hash,HASH_SIZE);o+=HASH_SIZE;
	for(unsigned i=0;i<SAVES_STATE_SLOTS;i++){memcpy(b+o,m->state_hash[i],HASH_SIZE);o+=HASH_SIZE;}
	digest_bytes(b,o,b+o);
}
static bool manifest_decode(const unsigned char *b,size_t n,struct manifest *m)
{
	if(n!=manifest_size()||memcmp(b,magic,8)!=0||get64(b+8)!=1)return false;
	unsigned char hash[HASH_SIZE];digest_bytes(b,n-HASH_SIZE,hash);if(memcmp(hash,b+n-HASH_SIZE,HASH_SIZE)!=0)return false;
	size_t o=16;m->generation=get64(b+o);o+=8;m->sram_size=get64(b+o);o+=8;m->rtc_size=get64(b+o);o+=8;
	for(unsigned i=0;i<SAVES_STATE_SLOTS;i++){m->state_size[i]=get64(b+o);o+=8;}
	memcpy(m->core,b+o,HASH_SIZE);o+=HASH_SIZE;memcpy(m->content,b+o,HASH_SIZE);o+=HASH_SIZE;memcpy(m->path,b+o,HASH_SIZE);o+=HASH_SIZE;memcpy(m->options,b+o,HASH_SIZE);o+=HASH_SIZE;memcpy(m->memory_hash,b+o,HASH_SIZE);o+=HASH_SIZE;
	for(unsigned i=0;i<SAVES_STATE_SLOTS;i++){memcpy(m->state_hash[i],b+o,HASH_SIZE);o+=HASH_SIZE;}return true;
}
static bool read_exact(const char *path,void *data,size_t size)
{
	int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return false;size_t off=0;while(off<size){ssize_t n=read(fd,(char*)data+off,size-off);if(n>0)off+=(size_t)n;else if(n<0&&errno==EINTR)continue;else break;}
	char extra;ssize_t more;do{more=read(fd,&extra,1);}while(more<0&&errno==EINTR);bool ok=off==size&&more==0;if(close(fd)<0)ok=false;return ok;
}
static bool write_file(const char *path,const void *data,size_t size)
{
	int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(fd<0)return false;size_t off=0;
	while(off<size){size_t chunk=size-off;
#ifdef SAVES_TESTING
		if(test_write_budget!=SIZE_MAX){if(test_write_budget==0){errno=ENOSPC;test_write_budget=SIZE_MAX;break;}if(chunk>test_write_budget)chunk=test_write_budget;}
#endif
		ssize_t n=write(fd,(const char*)data+off,chunk);if(n>0){off+=(size_t)n;
#ifdef SAVES_TESTING
			if(test_write_budget!=SIZE_MAX)test_write_budget-=(size_t)n;
#endif
		}else if(n<0&&errno==EINTR)continue;else break;}
	bool ok=off==size&&fsync(fd)==0;if(close(fd)<0)ok=false;if(!ok)(void)unlink(path);return ok;
}
static bool make_dirs(const char *path)
{
	char b[PATH_MAX];size_t n=strlen(path);if(n==0||n>=sizeof(b))return false;memcpy(b,path,n+1);
	for(char *p=b+1;*p;p++)if(*p=='/'){*p=0;if(mkdir(b,0700)<0&&errno!=EEXIST)return false;*p='/';}
	if(mkdir(b,0700)<0&&errno!=EEXIST)return false;
	struct stat st;return stat(b,&st)==0&&S_ISDIR(st.st_mode);
}
static bool current_generation(const char *dir,uint64_t *generation)
{
	char p[PATH_MAX],buf[32];int n=snprintf(p,sizeof(p),"%s/CURRENT",dir);if(n<0||(size_t)n>=sizeof(p))return false;
	int fd=open(p,O_RDONLY|O_CLOEXEC);if(fd<0)return false;ssize_t r=read(fd,buf,sizeof(buf)-1);close(fd);if(r<=0)return false;buf[r]=0;char *end;errno=0;unsigned long long v=strtoull(buf,&end,10);if(errno||end==buf||*end!='\n'||end[1]!='\0')return false;*generation=(uint64_t)v;return true;
}
static bool path_parts(char *out,size_t cap,const char *a,const char *b,const char *c)
{
	size_t na=strlen(a),nb=strlen(b),nc=c?strlen(c):0;
	if(na+1+nb+(c?1+nc:0)+1>cap)return false;
	memcpy(out,a,na);out[na]='/';memcpy(out+na+1,b,nb);
	if(c){out[na+1+nb]='/';memcpy(out+na+2+nb,c,nc);}
	out[na+1+nb+(c?1+nc:0)]=0;return true;
}
static bool gen_path(char *p,size_t n,const char *dir,uint64_t g,const char *file)
{char leaf[32];int r=snprintf(leaf,sizeof(leaf),"g%016" PRIx64,g);return r>0&&(size_t)r<sizeof(leaf)&&path_parts(p,n,dir,leaf,file);}
static bool generation_dir(char *p,size_t n,const char *dir,uint64_t g)
{char leaf[32];int r=snprintf(leaf,sizeof(leaf),"g%016" PRIx64,g);return r>0&&(size_t)r<sizeof(leaf)&&path_parts(p,n,dir,leaf,NULL);}
static bool load_manifest(struct saves *s,uint64_t g,struct manifest *m)
{
	char p[PATH_MAX];if(!gen_path(p,sizeof(p),s->directory,g,"manifest"))return false;unsigned char b[1024];if(!read_exact(p,b,manifest_size())||!manifest_decode(b,manifest_size(),m))return false;
	if(m->generation!=g||m->sram_size>MAX_MEMORY||m->rtc_size>MAX_MEMORY||m->sram_size+m->rtc_size>MAX_MEMORY)return false;
	if(memcmp(m->core,s->core,HASH_SIZE)||memcmp(m->content,s->content,HASH_SIZE)||memcmp(m->path,s->path,HASH_SIZE)||memcmp(m->options,s->options,HASH_SIZE))return false;
	for(unsigned i=0;i<SAVES_STATE_SLOTS;i++)if(m->state_size[i]>MAX_STATE)return false;
	return true;
}
static bool validate_payloads(struct saves *s,const struct manifest *m)
{
	char p[PATH_MAX];size_t mem=(size_t)(m->sram_size+m->rtc_size);if(mem){unsigned char *b=malloc(mem);if(!b)return false;if(!gen_path(p,sizeof(p),s->directory,m->generation,"memory")){free(b);return false;}bool ok=read_exact(p,b,mem);unsigned char h[HASH_SIZE];if(ok){digest_bytes(b,mem,h);ok=memcmp(h,m->memory_hash,HASH_SIZE)==0;}free(b);if(!ok)return false;}
	for(unsigned i=0;i<SAVES_STATE_SLOTS;i++)if(m->state_size[i]){unsigned char *b=malloc((size_t)m->state_size[i]);if(!b)return false;char f[32];(void)snprintf(f,sizeof(f),"state-%02u",i);if(!gen_path(p,sizeof(p),s->directory,m->generation,f)){free(b);return false;}bool ok=read_exact(p,b,(size_t)m->state_size[i]);unsigned char h[HASH_SIZE];if(ok){digest_bytes(b,(size_t)m->state_size[i],h);ok=memcmp(h,m->state_hash[i],HASH_SIZE)==0;}free(b);if(!ok)return false;}
	return true;
}
static bool find_generation(struct saves *s,struct manifest *m)
{
	uint64_t current=0;if(current_generation(s->directory,&current)&&load_manifest(s,current,m)&&validate_payloads(s,m))return true;
	DIR *d=opendir(s->directory);if(!d)return false;uint64_t candidates[256];size_t count=0;struct dirent *e;
	while((e=readdir(d))!=NULL&&count<256){if(e->d_name[0]!='g'||strlen(e->d_name)!=17)continue;char *end;errno=0;uint64_t g=strtoull(e->d_name+1,&end,16);if(errno==0&&*end==0&&g!=current)candidates[count++]=g;}closedir(d);
	for(size_t i=0;i<count;i++)for(size_t j=i+1;j<count;j++)if(candidates[j]>candidates[i]){uint64_t x=candidates[i];candidates[i]=candidates[j];candidates[j]=x;}
	for(size_t i=0;i<count;i++)if(load_manifest(s,candidates[i],m)&&validate_payloads(s,m))return true;
	return false;
}
static uint64_t highest_generation(const char *dir)
{
	DIR *d=opendir(dir);if(!d)return 0;uint64_t highest=0;struct dirent *e;
	while((e=readdir(d))!=NULL){if(e->d_name[0]!='g'||strlen(e->d_name)!=17)continue;char *end;errno=0;uint64_t g=strtoull(e->d_name+1,&end,16);if(errno==0&&*end==0&&g>highest)highest=g;}closedir(d);return highest;
}
static void remove_temp(const char *dir)
{
	DIR *d=opendir(dir);if(!d)return;struct dirent *e;char p[PATH_MAX];while((e=readdir(d))!=NULL){if(e->d_name[0]=='.')continue;int n=snprintf(p,sizeof(p),"%s/%s",dir,e->d_name);if(n>0&&(size_t)n<sizeof(p))(void)unlink(p);}closedir(d);(void)rmdir(dir);
}
static void prune_generations(const char *dir,uint64_t current,uint64_t previous)
{
	DIR *d=opendir(dir);if(!d)return;struct dirent *e;char p[PATH_MAX];
	while((e=readdir(d))!=NULL){if(e->d_name[0]!='g'||strlen(e->d_name)!=17)continue;char *end;errno=0;uint64_t g=strtoull(e->d_name+1,&end,16);if(errno||*end||g==current||g==previous)continue;if(path_parts(p,sizeof(p),dir,e->d_name,NULL))remove_temp(p);}
	closedir(d);
}
struct saves *saves_open_with_options(const char *root,const char *core_path,const char *content_path,const char *options)
{
	if(!root||!core_path||!content_path||!options||strlen(options)>4096)return NULL;
	char *core=realpath(core_path,NULL),*content=realpath(content_path,NULL);if(!core||!content){free(core);free(content);return NULL;}
	struct saves *s=calloc(1,sizeof(*s));if(!s){free(core);free(content);return NULL;}bool ok=digest_file(core,s->core)&&digest_file(content,s->content);digest_bytes(content,strlen(content),s->path);digest_bytes(options,strlen(options),s->options);
	if(ok&&make_dirs(root)){struct sha256 h;unsigned char key[HASH_SIZE];sha_init(&h);sha_update(&h,s->core,HASH_SIZE);sha_update(&h,s->content,HASH_SIZE);sha_update(&h,s->path,HASH_SIZE);sha_update(&h,s->options,HASH_SIZE);sha_final(&h,key);static const char hex[]="0123456789abcdef";char leaf[HASH_SIZE*2+1];for(unsigned i=0;i<HASH_SIZE;i++){leaf[2*i]=hex[key[i]>>4];leaf[2*i+1]=hex[key[i]&15];}leaf[64]=0;int n=snprintf(s->directory,sizeof(s->directory),"%s/%s",root,leaf);ok=n>0&&(size_t)n<sizeof(s->directory)&&make_dirs(s->directory);}else ok=false;
	free(core);free(content);if(!ok){free(s);return NULL;}return s;
}
struct saves *saves_open(const char *root,const char *core_path,const char *content_path)
{return saves_open_with_options(root,core_path,content_path,"banana-retro-options-v1:none");}
void saves_close(struct saves *s){free(s);}
bool saves_state_capture(saves_state_fn serialize,void *context,void *buffer,size_t size)
{return serialize&&buffer&&size>0&&serialize(context,buffer,size);}
bool saves_state_restore(saves_unserialize_fn unserialize,void *context,const void *buffer,size_t size)
{return unserialize&&buffer&&size>0&&unserialize(context,buffer,size);}
bool saves_load_memory(struct saves *s,void *sram,size_t ss,void *rtc,size_t rs)
{
	if(!s||(!sram&&ss)||(!rtc&&rs))return false;
	struct manifest m;if(!find_generation(s,&m)||m.sram_size!=ss||m.rtc_size!=rs)return false;
	if(ss+rs==0)return true;
	size_t n=ss+rs;unsigned char *b=malloc(n);if(!b)return false;char p[PATH_MAX];if(!gen_path(p,sizeof(p),s->directory,m.generation,"memory")){free(b);return false;}bool ok=read_exact(p,b,n);if(ok){if(ss)memcpy(sram,b,ss);if(rs)memcpy(rtc,b+ss,rs);}free(b);return ok;
}
bool saves_load_state(struct saves *s,unsigned slot,void *data,size_t size)
{
	if(!s||slot>=SAVES_STATE_SLOTS||(!data&&size))return false;
	struct manifest m;if(!find_generation(s,&m)||m.state_size[slot]!=size||size==0)return false;char f[32],p[PATH_MAX];(void)snprintf(f,sizeof(f),"state-%02u",slot);return gen_path(p,sizeof(p),s->directory,m.generation,f)&&read_exact(p,data,size);
}
bool saves_commit(struct saves *s,const void *sram,size_t ss,const void *rtc,size_t rs,int slot,const void *state,size_t state_size)
{
	if(!s||(!sram&&ss)||(!rtc&&rs)||ss>MAX_MEMORY||rs>MAX_MEMORY||ss+rs>MAX_MEMORY||slot>=(int)SAVES_STATE_SLOTS||slot< -1||(!state&&state_size)||state_size>MAX_STATE||(slot<0&&state_size)||(slot>=0&&state_size==0))return false;
	struct manifest prev={0};bool have=find_generation(s,&prev);uint64_t highest=highest_generation(s->directory);uint64_t generation=highest> (have?prev.generation:0)?highest+1:(have?prev.generation+1:1);if(generation==0)return false;
	struct manifest m={.generation=generation,.sram_size=ss,.rtc_size=rs};memcpy(m.core,s->core,HASH_SIZE);memcpy(m.content,s->content,HASH_SIZE);memcpy(m.path,s->path,HASH_SIZE);memcpy(m.options,s->options,HASH_SIZE);
	char temp[PATH_MAX],final[PATH_MAX],p[PATH_MAX];int n=snprintf(temp,sizeof(temp),"%s/.tmp-%ld-%016" PRIx64 "-XXXXXX",s->directory,(long)getpid(),generation);if(n<0||(size_t)n>=sizeof(temp)||!mkdtemp(temp))return false;
	bool ok=true;size_t mem=ss+rs;if(mem){unsigned char *b=malloc(mem);if(!b)ok=false;else{if(ss)memcpy(b,sram,ss);if(rs)memcpy(b+ss,rtc,rs);digest_bytes(b,mem,m.memory_hash);ok=path_parts(p,sizeof(p),temp,"memory",NULL)&&write_file(p,b,mem);free(b);}}
	for(unsigned i=0;ok&&i<SAVES_STATE_SLOTS;i++){
		if((int)i==slot){m.state_size[i]=state_size;digest_bytes(state,state_size,m.state_hash[i]);char f[32];(void)snprintf(f,sizeof(f),"state-%02u",i);ok=path_parts(p,sizeof(p),temp,f,NULL)&&write_file(p,state,state_size);}
		else if(have&&prev.state_size[i]){unsigned char *b=malloc((size_t)prev.state_size[i]);if(!b){ok=false;break;}char f[32];(void)snprintf(f,sizeof(f),"state-%02u",i);ok=gen_path(p,sizeof(p),s->directory,prev.generation,f)&&read_exact(p,b,(size_t)prev.state_size[i]);if(ok){m.state_size[i]=prev.state_size[i];memcpy(m.state_hash[i],prev.state_hash[i],HASH_SIZE);ok=path_parts(p,sizeof(p),temp,f,NULL)&&write_file(p,b,(size_t)m.state_size[i]);}free(b);}
	}
	if(ok){size_t z=manifest_size();unsigned char *b=malloc(z);if(!b)ok=false;else{manifest_encode(&m,b);ok=path_parts(p,sizeof(p),temp,"manifest",NULL)&&write_file(p,b,z);free(b);}}
	int dfd=open(temp,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(ok&&(dfd<0||fsync(dfd)<0))ok=false;if(dfd>=0)close(dfd);
	if(ok)ok=generation_dir(final,sizeof(final),s->directory,generation)&&rename(temp,final)==0;
	int rootfd=open(s->directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(ok&&(rootfd<0||fsync(rootfd)<0))ok=false;
	if(ok){char cp[PATH_MAX],ct[PATH_MAX],leaf[64];int r=snprintf(leaf,sizeof(leaf),"CURRENT.tmp-%ld-%016" PRIx64,(long)getpid(),generation);char text[32];n=snprintf(text,sizeof(text),"%" PRIu64 "\n",generation);ok=r>0&&(size_t)r<sizeof(leaf)&&path_parts(cp,sizeof(cp),s->directory,"CURRENT",NULL)&&path_parts(ct,sizeof(ct),s->directory,leaf,NULL)&&n>0&&(size_t)n<sizeof(text)&&write_file(ct,text,(size_t)n)&&rename(ct,cp)==0;if(!ok&&r>0&&(size_t)r<sizeof(leaf)&&path_parts(ct,sizeof(ct),s->directory,leaf,NULL))(void)unlink(ct);}
	if(rootfd>=0){if(ok&&fsync(rootfd)<0)ok=false;if(ok)prune_generations(s->directory,generation,have?prev.generation:0);if(ok)(void)fsync(rootfd);close(rootfd);}if(!ok)remove_temp(temp);
	return ok;
}
