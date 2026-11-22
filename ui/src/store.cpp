// banana-store: the app store's helper. banana-ui draws the Store screen and runs this; this fetches, checks and installs.
//   banana-store refresh            fetch the index (kept in /data/banana/cache/store-index.json; the last good one is
//                                   used when the fetch fails: "done offline")
//   banana-store install <id>       download the app's tarball, check size + sha256 + manifest + minBanana, unpack it
//                                   to staging and rename it into /data/apps/<id>/ (an older copy is swapped out)
//   banana-store remove <id> [--wipe-data]   delete /data/apps/<id>/; its data (appdata/<id>) stays unless --wipe-data
// Progress in $BANANA_RUN/store.status, one line: "running <done> <total> <what>", then "done [offline]" or "error <why>".
// index.json: {"version":1,"apps":[{"id","name","version","description","size","minBanana","url","sha256"}]}.
// The tarball holds the app's folder's contents (app.json at its top, whose "id" must be the index's). Nothing outside
// the staging folder is written until every check has passed. Environment (the tests set these): BANANA_STORE_INDEX
// (the index's URL), BANANA_APPS (/data/apps), BANANA_APPDATA (/data/banana/appdata), BANANA_CACHE
// (/data/banana/cache), BANANA_RUN (/run/banana), BANANA_VERSION_FILE (/etc/banana/version).
#include "json.h"
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace banana;

namespace
{
	const char* env(const char* k, const char* d) { const char* v = getenv(k); return v && *v ? v : d; }
	std::string INDEX_URL, APPS, APPDATA, CACHE, RUN;

	void status(const std::string& s)
	{
		std::string p = RUN + "/store.status";
		std::string tmp = p + ".tmp";
		if (FILE* f = fopen(tmp.c_str(), "w")) { fprintf(f, "%s\n", s.c_str()); fclose(f); rename(tmp.c_str(), p.c_str()); }
	}
	int fail(const std::string& why) { status("error " + why); fprintf(stderr, "banana-store: %s\n", why.c_str()); return 1; }

	// ---- SHA-256 ----------------------------------------------------------------------------------------------------
	struct Sha256
	{
		uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
		uint8_t buf[64];
		size_t n = 0;
		uint64_t total = 0;
		static uint32_t ror(uint32_t x, int c) { return (x >> c) | (x << (32 - c)); }
		void block(const uint8_t* p)
		{
			static const uint32_t K[64] = {
				0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
				0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
				0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
				0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
				0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
				0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
				0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
				0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
			uint32_t w[64];
			for (int i = 0; i < 16; i++)
				w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
			for (int i = 16; i < 64; i++)
			{
				uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
				uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
				w[i] = w[i - 16] + s0 + w[i - 7] + s1;
			}
			uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
			for (int i = 0; i < 64; i++)
			{
				uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
				uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
				hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
			}
			h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
		}
		void add(const uint8_t* p, size_t len)
		{
			total += len;
			while (len)
			{
				size_t k = 64 - n < len ? 64 - n : len;
				memcpy(buf + n, p, k);
				n += k; p += k; len -= k;
				if (n == 64) { block(buf); n = 0; }
			}
		}
		std::string hex()
		{
			uint64_t bits = total * 8;
			uint8_t pad = 0x80;
			add(&pad, 1);
			pad = 0;
			while (n != 56) add(&pad, 1);
			uint8_t l[8];
			for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
			add(l, 8);
			char o[65];
			for (int i = 0; i < 8; i++) snprintf(o + i * 8, 9, "%08x", h[i]);
			return o;
		}
	};
	bool fileSha(const std::string& path, std::string& out)
	{
		FILE* f = fopen(path.c_str(), "rb");
		if (!f) return false;
		Sha256 s;
		uint8_t b[16384];
		size_t k;
		while ((k = fread(b, 1, sizeof b, f)) > 0) s.add(b, k);
		bool ok = !ferror(f);
		fclose(f);
		out = s.hex();
		return ok;
	}

	// ---- libcurl, dlopen()ed as banana-scrape does it ------------------------------------------------------------------
	struct Curl
	{
		void* (*init)(void) = nullptr;
		int (*setopt)(void*, int, ...) = nullptr;
		int (*perform)(void*) = nullptr;
		int (*getinfo)(void*, int, ...) = nullptr;
		void (*cleanup)(void*) = nullptr;
		bool load()
		{
			void* l = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
			if (!l) return false;
			init = (void* (*)(void))dlsym(l, "curl_easy_init");
			setopt = (int (*)(void*, int, ...))dlsym(l, "curl_easy_setopt");
			perform = (int (*)(void*))dlsym(l, "curl_easy_perform");
			getinfo = (int (*)(void*, int, ...))dlsym(l, "curl_easy_getinfo");
			cleanup = (void (*)(void*))dlsym(l, "curl_easy_cleanup");
			return init && setopt && perform && getinfo && cleanup;
		}
	} cu;
	struct Dl
	{
		FILE* f;
		uint64_t got = 0, cap;
		std::string what;
		int lastPct = -1;
	};
	size_t onBody(char* p, size_t sz, size_t n, void* u)
	{
		Dl* d = (Dl*)u;
		size_t len = sz * n;
		d->got += len;
		if (d->got > d->cap) return 0;	// more than the index said: stop (curl reports a write error)
		if (fwrite(p, 1, len, d->f) != len) return 0;
		int pct = d->cap ? (int)(d->got * 100 / d->cap) : 0;
		if (pct != d->lastPct) { d->lastPct = pct; status("running " + std::to_string(pct) + " 100 " + d->what); }
		return len;
	}
	// the body of url into path, at most cap bytes; false on any failure (HTTP status other than 200, except file://)
	bool fetch(const std::string& url, const std::string& path, uint64_t cap, const std::string& what)
	{
		enum { WRITEDATA = 10001, URL = 10002, USERAGENT = 10018, WRITEFUNCTION = 20011, TIMEOUT = 13, NOSIGNAL = 99,
			FOLLOWLOCATION = 52, FAILONERROR = 45, CONNECTTIMEOUT = 78, PROTOCOLS_STR = 10318, RESPONSE_CODE = 0x200002 };
		void* h = cu.init();
		if (!h) return false;
		Dl d;
		d.f = fopen(path.c_str(), "wb");
		if (!d.f) { cu.cleanup(h); return false; }
		d.cap = cap;
		d.what = what;
		cu.setopt(h, URL, url.c_str());
		cu.setopt(h, USERAGENT, "BananaOS banana-store");
		cu.setopt(h, NOSIGNAL, 1L);
		cu.setopt(h, CONNECTTIMEOUT, 20L);
		cu.setopt(h, TIMEOUT, 1800L);
		cu.setopt(h, FOLLOWLOCATION, 1L);
		cu.setopt(h, FAILONERROR, 1L);
		cu.setopt(h, PROTOCOLS_STR, getenv("BANANA_STORE_ALLOW_FILE") ? "https,file" : "https");
		cu.setopt(h, WRITEFUNCTION, onBody);
		cu.setopt(h, WRITEDATA, &d);
		bool ok = cu.perform(h) == 0;
		cu.cleanup(h);
		if (fclose(d.f) != 0) ok = false;
		if (!ok) unlink(path.c_str());
		return ok;
	}

	// ---- files ------------------------------------------------------------------------------------------------------
	bool exists(const std::string& p) { struct stat st; return lstat(p.c_str(), &st) == 0; }
	bool isDir(const std::string& p) { struct stat st; return lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
	void mkdirs(const std::string& p)
	{
		for (size_t i = 1; i <= p.size(); i++)
			if (i == p.size() || p[i] == '/') mkdir(p.substr(0, i).c_str(), 0755);
	}
	// rm -rf, not following symlinks
	void rmrf(const std::string& p)
	{
		struct stat st;
		if (lstat(p.c_str(), &st) != 0) return;
		if (S_ISDIR(st.st_mode))
		{
			if (DIR* d = opendir(p.c_str()))
			{
				while (dirent* e = readdir(d))
					if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) rmrf(p + "/" + e->d_name);
				closedir(d);
			}
			rmdir(p.c_str());
		}
		else unlink(p.c_str());
	}
	bool readAll(const std::string& p, std::string& out)
	{
		std::ifstream f(p, std::ios::binary);
		if (!f) return false;
		std::stringstream ss;
		ss << f.rdbuf();
		out = ss.str();
		return true;
	}
	bool writeAtomic(const std::string& p, const std::string& data)
	{
		std::string tmp = p + ".tmp";
		FILE* f = fopen(tmp.c_str(), "wb");
		if (!f) return false;
		bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
		ok = fflush(f) == 0 && ok;
		ok = fsync(fileno(f)) == 0 && ok;
		ok = fclose(f) == 0 && ok;
		if (!ok || rename(tmp.c_str(), p.c_str()) != 0) { unlink(tmp.c_str()); return false; }
		return true;
	}
	// run argv, its stdout into *out (when given); true when it exits 0
	bool run(const std::vector<std::string>& argv, std::string* out)
	{
		int pp[2];
		if (pipe(pp) != 0) return false;
		pid_t pid = fork();
		if (pid < 0) { close(pp[0]); close(pp[1]); return false; }
		if (pid == 0)
		{
			dup2(pp[1], 1);
			close(pp[0]); close(pp[1]);
			int nul = open("/dev/null", O_WRONLY);
			if (nul >= 0) { dup2(nul, 2); close(nul); }
			std::vector<char*> a;
			for (auto& s : argv) a.push_back((char*)s.c_str());
			a.push_back(nullptr);
			execvp(a[0], a.data());
			_exit(127);
		}
		close(pp[1]);
		char b[4096];
		ssize_t k;
		while ((k = read(pp[0], b, sizeof b)) > 0) if (out) out->append(b, (size_t)k);
		close(pp[0]);
		int st = 0;
		while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
		return WIFEXITED(st) && WEXITSTATUS(st) == 0;
	}

	// ---- versions ---------------------------------------------------------------------------------------------------
	// "1.0.1" < "1.2": dotted numbers, the first non-numeric tail ("-dev") ignored; a version that is not one is 0
	std::vector<long> ver(const std::string& s)
	{
		std::vector<long> v;
		size_t i = 0;
		while (i < s.size() && isdigit((unsigned char)s[i]))
		{
			long n = 0;
			while (i < s.size() && isdigit((unsigned char)s[i])) n = n * 10 + (s[i++] - '0');
			v.push_back(n);
			if (i < s.size() && s[i] == '.') i++; else break;
		}
		return v;
	}
	bool verLess(const std::string& a, const std::string& b)
	{
		auto x = ver(a), y = ver(b);
		size_t n = x.size() > y.size() ? x.size() : y.size();
		for (size_t i = 0; i < n; i++)
		{
			long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
			if (p != q) return p < q;
		}
		return false;
	}
	bool safeId(const std::string& s)
	{
		if (s.empty() || s.size() > 48 || s[0] == '.') return false;
		for (char c : s) if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')) return false;
		return true;
	}

	// ---- the index --------------------------------------------------------------------------------------------------
	// valid when it parses, says version 1 and every app has a safe id, an https url (file:// only for tests), a 64-hex
	// sha256 and a size
	bool validIndex(const std::string& text, std::string& why)
	{
		JPtr j = parseJson(text, &why);
		if (!j || !j->isObj()) { if (why.empty()) why = "not an object"; return false; }
		if ((int)j->num("version") != 1) { why = "unknown index version"; return false; }
		const JVal* apps = j->get("apps");
		if (!apps || !apps->isArr()) { why = "no apps"; return false; }
		for (auto& a : apps->arr)
		{
			if (!a->isObj() || !safeId(a->str("id"))) { why = "bad app id"; return false; }
			std::string sha = a->str("sha256"), url = a->str("url");
			if (sha.size() != 64 || sha.find_first_not_of("0123456789abcdef") != std::string::npos) { why = "bad sha256 for " + a->str("id"); return false; }
			if (url.compare(0, 8, "https://") && url.compare(0, 7, "file://")) { why = "bad url for " + a->str("id"); return false; }
			if (a->num("size") <= 0) { why = "no size for " + a->str("id"); return false; }
		}
		return true;
	}
	int cmdRefresh()
	{
		std::string cache = CACHE + "/store-index.json";
		mkdirs(CACHE);
		std::string tmp = cache + ".new", text, why;
		status("running 0 1 index");
		if (fetch(INDEX_URL, tmp, 4u << 20, "index") && readAll(tmp, text) && validIndex(text, why))
		{
			unlink(tmp.c_str());
			if (!writeAtomic(cache, text)) return fail("can't keep the index");
			status("done");
			return 0;
		}
		unlink(tmp.c_str());
		if (readAll(cache, text) && validIndex(text, why)) { status("done offline"); return 0; }
		return fail(why.empty() ? "no connection and no saved index" : "the index is not valid: " + why);
	}

	// ---- install ----------------------------------------------------------------------------------------------------
	std::string osVersion()
	{
		std::string v;
		std::ifstream(env("BANANA_VERSION_FILE", "/etc/banana/version")) >> v;
		return v;
	}
	// refuse what could write outside the staging folder: absolute or .. names, links pointing out, hard links, devices
	bool listingSafe(const std::string& tar, std::string& why)
	{
		std::string names, verbose;
		if (!run({ "tar", "-tf", tar }, &names) || !run({ "tar", "-tvf", tar }, &verbose)) { why = "the package can't be read"; return false; }
		auto bad = [](const std::string& p) { return p.compare(0, 1, "/") == 0 || p == ".." || p.compare(0, 3, "../") == 0 ||
			p.find("/../") != std::string::npos || (p.size() >= 3 && p.compare(p.size() - 3, 3, "/..") == 0); };
		std::istringstream ns(names);
		for (std::string ln; std::getline(ns, ln);)
			if (bad(ln)) { why = "the package has an unsafe path"; return false; }
		std::istringstream vs(verbose);
		for (std::string ln; std::getline(vs, ln);)
		{
			if (ln.empty()) continue;
			if (ln[0] == 'h' || ln[0] == 'b' || ln[0] == 'c' || ln[0] == 'p') { why = "the package has a special file"; return false; }
			size_t arrow = ln.find(" -> ");
			if (ln[0] == 'l' && arrow != std::string::npos && bad(ln.substr(arrow + 4))) { why = "the package has an unsafe link"; return false; }
		}
		return true;
	}
	const JVal* findApp(const JPtr& j, const std::string& id)
	{
		for (auto& a : j->get("apps")->arr) if (a->str("id") == id) return a.get();
		return nullptr;
	}
	int cmdInstall(const std::string& id)
	{
		if (!safeId(id)) return fail("bad app id");
		std::string text, why;
		if (!readAll(CACHE + "/store-index.json", text) || !validIndex(text, why)) return fail("no index: refresh first");
		JPtr idx = parseJson(text);
		const JVal* a = findApp(idx, id);
		if (!a) return fail(id + " is not in the store");
		std::string need = a->str("minBanana"), have = osVersion();
		if (!need.empty() && verLess(have, need)) return fail("needs Banana " + need + " (this is " + (have.empty() ? "unknown" : have) + ")");
		if (!cu.load()) return fail("libcurl is missing");
		std::string stage = APPS + "/.staging";
		rmrf(stage + "/" + id + ".d");
		rmrf(stage + "/" + id + ".tar");
		mkdirs(stage);
		if (!isDir(stage)) return fail("can't make the staging folder");
		std::string tar = stage + "/" + id + ".tar", dir = stage + "/" + id + ".d";
		uint64_t size = (uint64_t)a->num("size");
		status("running 0 100 " + a->str("name", id));
		if (!fetch(a->str("url"), tar, size, a->str("name", id))) { rmrf(tar); return fail("the download failed"); }
		struct stat st;
		std::string sum;
		if (stat(tar.c_str(), &st) != 0 || (uint64_t)st.st_size != size) { rmrf(tar); return fail("the download is the wrong size"); }
		if (!fileSha(tar, sum) || sum != a->str("sha256")) { rmrf(tar); return fail("the download does not match its hash"); }
		status("running 100 100 " + a->str("name", id));
		if (!listingSafe(tar, why)) { rmrf(tar); return fail(why); }
		mkdirs(dir);
		if (!run({ "tar", "-xf", tar, "-C", dir }, nullptr)) { rmrf(tar); rmrf(dir); return fail("the package can't be unpacked"); }
		rmrf(tar);
		JPtr man = readJsonFile(dir + "/app.json", &why);
		if (!man || !man->isObj()) { rmrf(dir); return fail("the package has no app.json"); }
		if (man->str("id") != id) { rmrf(dir); return fail("the package is another app"); }
		std::string mneed = man->str("minBanana");
		if (!mneed.empty() && verLess(have, mneed)) { rmrf(dir); return fail("needs Banana " + mneed); }
		// the swap: an older copy steps aside, the new one steps in; if that fails the old one is put back
		std::string dest = APPS + "/" + id, old = stage + "/" + id + ".old";
		rmrf(old);
		bool had = exists(dest);
		if (had && rename(dest.c_str(), old.c_str()) != 0) { rmrf(dir); return fail("can't replace the installed copy"); }
		if (rename(dir.c_str(), dest.c_str()) != 0)
		{
			if (had) rename(old.c_str(), dest.c_str());
			rmrf(dir);
			return fail("can't put the app in place");
		}
		rmrf(old);
		mkdirs(APPDATA + "/" + id);
		status("done");
		return 0;
	}
	int cmdRemove(const std::string& id, bool wipe)
	{
		if (!safeId(id)) return fail("bad app id");
		std::string dest = APPS + "/" + id;
		if (!isDir(dest)) return fail(id + " is not installed");
		// out of the scan's way first (a half-deleted app folder must not show), then deleted
		std::string gone = APPS + "/.staging/" + id + ".gone";
		mkdirs(APPS + "/.staging");
		rmrf(gone);
		if (rename(dest.c_str(), gone.c_str()) != 0) return fail("can't remove " + id);
		rmrf(gone);
		if (wipe) rmrf(APPDATA + "/" + id);
		status("done");
		return 0;
	}
}

int main(int argc, char** argv)
{
	INDEX_URL = env("BANANA_STORE_INDEX", "https://jesussaves-dev.github.io/Banana-Pocket/store/index.json");
	APPS = env("BANANA_APPS", "/data/apps");
	APPDATA = env("BANANA_APPDATA", "/data/banana/appdata");
	CACHE = env("BANANA_CACHE", "/data/banana/cache");
	RUN = env("BANANA_RUN", "/run/banana");
	std::string cmd = argc > 1 ? argv[1] : "";
	if (cmd == "refresh")
	{
		if (!cu.load()) { status("error libcurl is missing"); return 1; }
		return cmdRefresh();
	}
	if (cmd == "install" && argc > 2) return cmdInstall(argv[2]);
	if (cmd == "remove" && argc > 2) return cmdRemove(argv[2], argc > 3 && !strcmp(argv[3], "--wipe-data"));
	fprintf(stderr, "usage: banana-store refresh | install <id> | remove <id> [--wipe-data]\n");
	return 2;
}
