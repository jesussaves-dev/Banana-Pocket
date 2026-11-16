#include "theme.h"
#include "frontend.h"
#include "../third_party/stb_image.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <map>
#include <sstream>
#include <cerrno>
#include <sys/stat.h>

namespace banana
{
	static const char* IMAGE_DIR = "/usr/share/banana/themes";
	// the card's themes folder; BANANA_THEMES points somewhere else (the headless test)
	static const char* cardDir() { const char* e = getenv("BANANA_THEMES"); return e && *e ? e : "/data/banana/themes"; }

	// ---- a small JSON reader: objects, strings, numbers, true/false/null, arrays skipped; enough for a theme --------
	struct Json
	{
		const std::string& s;
		size_t i = 0;
		bool ok = true;
		explicit Json(const std::string& t) : s(t) {}
		void ws() { while (i < s.size() && (isspace((unsigned char)s[i]) || s[i] == '/')) { if (s[i] == '/') skipComment(); else i++; } }
		void skipComment()	// // and /* */ comments are allowed in a theme (JSON itself has none)
		{
			if (i + 1 < s.size() && s[i + 1] == '/') { while (i < s.size() && s[i] != '\n') i++; }
			else if (i + 1 < s.size() && s[i + 1] == '*') { size_t e = s.find("*/", i + 2); i = e == std::string::npos ? s.size() : e + 2; }
			else { ok = false; i = s.size(); }
		}
		std::string str()
		{
			std::string r;
			if (s[i] != '"') { ok = false; return r; }
			for (i++; i < s.size() && s[i] != '"'; i++)
			{
				if (s[i] == '\\' && i + 1 < s.size()) { i++; r += s[i] == 'n' ? '\n' : s[i] == 't' ? '\t' : s[i]; }
				else r += s[i];
			}
			if (i >= s.size()) ok = false;
			i++;
			return r;
		}
		// a value: strings and numbers as text, an object's members flattened into out as "key.sub", the rest skipped
		void value(const std::string& key, std::map<std::string, std::string>& out)
		{
			ws();
			if (i >= s.size()) { ok = false; return; }
			if (s[i] == '"') { out[key] = str(); return; }
			if (s[i] == '{') { object(key, out); return; }
			if (s[i] == '[') { int d = 0; do { if (s[i] == '[') d++; else if (s[i] == ']') d--; else if (s[i] == '"') { str(); continue; } i++; } while (i < s.size() && d > 0); return; }
			size_t b = i;
			while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '.' || s[i] == '-' || s[i] == '+')) i++;
			if (b == i) { ok = false; return; }
			out[key] = s.substr(b, i - b);
		}
		void object(const std::string& prefix, std::map<std::string, std::string>& out)
		{
			i++;	// {
			for (;;)
			{
				ws();
				if (i < s.size() && s[i] == '}') { i++; return; }
				std::string k = str();
				ws();
				if (i >= s.size() || s[i] != ':') { ok = false; return; }
				i++;
				value(prefix.empty() ? k : prefix + "." + k, out);
				if (!ok) return;
				ws();
				if (i < s.size() && s[i] == ',') { i++; continue; }
				if (i < s.size() && s[i] == '}') { i++; return; }
				ok = false; return;
			}
		}
	};

	static bool readTheme(const std::string& path, std::map<std::string, std::string>& out, std::string* why)
	{
		std::ifstream f(path);
		if (!f) { if (why) *why = "can't open " + path; return false; }
		std::stringstream ss; ss << f.rdbuf();
		std::string t = ss.str();
		Json j(t);
		j.ws();
		if (j.i >= t.size() || t[j.i] != '{') { if (why) *why = path + ": not a JSON object"; return false; }
		j.object("", out);
		if (!j.ok) { if (why) *why = path + ": a JSON error near byte " + std::to_string(j.i); return false; }
		return true;
	}

	// "#rgb", "#rrggbb" or "#rrggbbaa" (CSS order) -> 0xAARRGGBB
	static bool parseColor(std::string v, uint32_t& out)
	{
		if (v.empty() || v[0] != '#') return false;
		v.erase(0, 1);
		if (!std::all_of(v.begin(), v.end(), [](char c) { return isxdigit((unsigned char)c); })) return false;
		if (v.size() == 3) v = std::string{ v[0], v[0], v[1], v[1], v[2], v[2] };
		if (v.size() == 6) v += "ff";
		if (v.size() != 8) return false;
		uint32_t rgba = (uint32_t)strtoul(v.c_str(), nullptr, 16);
		out = (rgba >> 8) | (rgba & 0xff) << 24;
		return true;
	}
	static uint32_t mix(uint32_t c, uint32_t to, float t)	// RGB only
	{
		auto ch = [&](int sh) { float a = (c >> sh) & 0xff, b = (to >> sh) & 0xff; return (uint32_t)(a + (b - a) * t + 0.5f) << sh; };
		return ch(16) | ch(8) | ch(0);
	}

	static std::string pathFor(const std::string& stem)
	{
		std::string p = std::string(cardDir()) + "/" + stem + ".json";
		if (std::ifstream(p)) return p;
		return std::string(IMAGE_DIR) + "/" + stem + ".json";
	}

	std::string themePath(const std::string& value)
	{
		if (value.compare(0, 5, "user:") == 0) return pathFor(value.substr(5));
		if (value.compare(0, 5, "file:") == 0) return value.substr(5);
		return "";
	}

	std::vector<UserTheme> userThemes()
	{
		std::vector<UserTheme> r;
		for (const char* dir : { cardDir(), IMAGE_DIR })
		{
			DIR* d = opendir(dir);
			if (!d) continue;
			std::vector<std::string> stems;
			while (dirent* e = readdir(d))
			{
				std::string n = e->d_name;
				if (n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0 && n[0] != '.') stems.push_back(n.substr(0, n.size() - 5));
			}
			closedir(d);
			std::sort(stems.begin(), stems.end());
			for (auto& st : stems)
			{
				if (std::any_of(r.begin(), r.end(), [&](const UserTheme& u) { return u.value == "user:" + st; })) continue;
				std::map<std::string, std::string> kv;
				std::string name = st, style = "classic";
				if (readTheme(std::string(dir) + "/" + st + ".json", kv, nullptr))
				{
					if (!kv["name"].empty()) name = kv["name"];
					if (!kv["style"].empty()) { style = kv["style"]; for (char& ch : style) ch = (char)tolower((unsigned char)ch); }
				}
				r.push_back({ "user:" + st, name, style });
			}
		}
		return r;
	}

	std::vector<std::string> themeStyles(const std::vector<UserTheme>& themes)
	{
		static const char* ORDER[] = { "classic", "snes", "genesis" };
		std::vector<std::string> known, other;
		for (const char* o : ORDER)
			if (std::any_of(themes.begin(), themes.end(), [&](const UserTheme& t) { return t.style == o; })) known.push_back(o);
		for (auto& t : themes)
			if (std::find(std::begin(ORDER), std::end(ORDER), t.style) == std::end(ORDER) && std::find(other.begin(), other.end(), t.style) == other.end())
				other.push_back(t.style);
		std::sort(other.begin(), other.end());
		known.insert(known.end(), other.begin(), other.end());
		return known;
	}

	std::string themeValue(const std::string& value, const std::string& key)
	{
		std::string p = themePath(value);
		std::map<std::string, std::string> kv;
		if (p.empty() || !readTheme(p, kv, nullptr)) return "";
		auto it = kv.find(key);
		return it == kv.end() ? "" : it->second;
	}

	std::string themeDir(const std::string& value)
	{
		std::string p = themePath(value);
		size_t k = p.rfind('/');
		return k == std::string::npos ? "." : p.substr(0, k);
	}

	// ---- the card's copy of the image's themes ----------------------------------------------------------------------
	static bool readAll(const std::string& p, std::string& out)
	{
		std::ifstream f(p, std::ios::binary);
		if (!f) return false;
		std::ostringstream ss;
		ss << f.rdbuf();
		out = ss.str();
		return true;
	}

	static std::string fnv(const std::string& d)
	{
		uint64_t h = 1469598103934665603ull;
		for (unsigned char c : d) { h ^= c; h *= 1099511628211ull; }
		char b[17];
		snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
		return b;
	}

	// every regular file under dir, as paths relative to it (dot files and folders skipped)
	static void listFiles(const std::string& dir, const std::string& rel, std::vector<std::string>& out)
	{
		DIR* d = opendir((dir + (rel.empty() ? "" : "/" + rel)).c_str());
		if (!d) return;
		while (dirent* e = readdir(d))
		{
			std::string n = e->d_name;
			if (n.empty() || n[0] == '.') continue;
			std::string r = rel.empty() ? n : rel + "/" + n;
			struct stat st;
			if (stat((dir + "/" + r).c_str(), &st) != 0) continue;
			if (S_ISDIR(st.st_mode)) listFiles(dir, r, out);
			else if (S_ISREG(st.st_mode)) out.push_back(r);
		}
		closedir(d);
	}

	static bool mkdirs(const std::string& path)
	{
		for (size_t k = 1; k <= path.size(); k++)
			if (k == path.size() || path[k] == '/')
			{
				std::string p = path.substr(0, k);
				if (mkdir(p.c_str(), 0755) != 0 && errno != EEXIST) return false;
			}
		return true;
	}

	static bool writeAll(const std::string& p, const std::string& data)
	{
		size_t k = p.rfind('/');
		if (k != std::string::npos && !mkdirs(p.substr(0, k))) return false;
		std::string tmp = p + ".new";
		{
			std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
			if (!f || !f.write(data.data(), (std::streamsize)data.size())) return false;
		}
		return rename(tmp.c_str(), p.c_str()) == 0;
	}

	std::string syncCardThemes(const std::string& card, const std::string& image, const std::string& version)
	{
		struct stat st;
		if (version.empty() || stat(image.c_str(), &st) != 0) return "";
		const std::string manifest = card + "/.image-files";
		std::map<std::string, std::string> seeded;	// relative path -> its hash when it was copied
		std::string had, m;
		bool known = readAll(manifest, m);
		if (known)
		{
			std::istringstream in(m);
			std::string ln;
			std::getline(in, had);
			while (std::getline(in, ln))
			{
				size_t t = ln.find('\t');
				if (t != std::string::npos) seeded[ln.substr(t + 1)] = ln.substr(0, t);
			}
			if (had == version) return "";
		}
		bool fresh = stat(card.c_str(), &st) != 0;
		std::vector<std::string> files;
		listFiles(image, "", files);
		std::sort(files.begin(), files.end());
		std::string out = version + "\n";
		int added = 0, refreshed = 0, kept = 0, saved = 0, failed = 0;
		for (auto& r : files)
		{
			std::string img, cur;
			if (!readAll(image + "/" + r, img)) continue;
			const std::string h = fnv(img), dst = card + "/" + r;
			bool have = readAll(dst, cur);
			std::string record = h;
			if (!have) { if (writeAll(dst, img)) added++; else failed++; }
			else if (cur == img) {}
			else
			{
				const std::string ch = fnv(cur);
				auto it = seeded.find(r);
				if (known && it != seeded.end() && it->second != ch) { kept++; record = it->second; }	// the user's edit
				else if (known && it == seeded.end()) { kept++; continue; }						// theirs, not ours
				else
				{
					if (!known && !fresh) { if (writeAll(card + "/.before-update/" + r, cur)) saved++; else { failed++; continue; } }
					if (writeAll(dst, img)) refreshed++; else failed++;
				}
			}
			out += record + "\t" + r + "\n";
		}
		if (!writeAll(manifest, out)) failed++;
		char b[200];
		snprintf(b, sizeof b, "%s -> %s: %d added, %d refreshed, %d kept as changed, %d saved to .before-update, %d failed",
			known ? had.c_str() : fresh ? "none" : "unrecorded", version.c_str(), added, refreshed, kept, saved, failed);
		return b;
	}

	std::string styleName(const std::string& style)
	{
		if (style == "classic") return "Classic";
		if (style == "snes") return "SNES";
		if (style == "genesis") return "Genesis";
		std::string n = style;
		if (!n.empty()) n[0] = (char)toupper((unsigned char)n[0]);
		return n;
	}

	// a file a theme names: relative to its folder (or absolute), used only when it's there
	static std::string themeFile(const std::string& dir, const std::string& name)
	{
		if (name.empty()) return "";
		std::string p = name[0] == '/' ? name : dir + "/" + name;
		return std::ifstream(p) ? p : "";
	}

	// a picture the theme names (a wallpaper, the logo, an icon): only one stb_image reads (PNG or JPEG) and at most
	// 4096 px a side, checked from its header before anything is decoded (an 8192 x 8192 wallpaper decoded is 256 MB,
	// and banana-ui stays in memory under the games); otherwise the default is used, and the log says why
	static std::string themeImage(const std::string& dir, const std::string& name)
	{
		std::string p = themeFile(dir, name);
		if (p.empty())
		{
			if (!name.empty()) fprintf(stderr, "banana-ui: theme: %s isn't there (the default instead)\n", name.c_str());
			return "";
		}
		int w = 0, h = 0, n = 0;
		if (!stbi_info(p.c_str(), &w, &h, &n))
		{
			fprintf(stderr, "banana-ui: theme: %s isn't a picture Banana reads (PNG or JPEG; the default instead)\n", p.c_str());
			return "";
		}
		if (w > 4096 || h > 4096)
		{
			fprintf(stderr, "banana-ui: theme: %s is %dx%d, the largest is 4096 (the default instead)\n", p.c_str(), w, h);
			return "";
		}
		return p;
	}

	static bool sCrisp = false;
	bool themeCrisp() { return sCrisp; }

	std::string applyTheme(const std::string& value, const std::string& accentName, std::string* why)
	{
		rnds::themeColors.clear();
		rnds::themeFiles = rnds::ThemeFiles();
		sCrisp = false;	// (a plain Dark/Light, or a theme that can't be read, keeps the rounded boxes)
		rnds::setAccent(accentName);
		std::string path = themePath(value);
		if (path.empty()) return value == "light" ? "light" : "dark";
		std::map<std::string, std::string> kv;
		if (!readTheme(path, kv, why)) return "dark";
		std::string dir = path.substr(0, path.rfind('/'));
		std::string base = kv["base"] == "light" ? "light" : "dark";
		sCrisp = kv["crisp"] == "true";	// the retro geometry the layout menus also draw with (layout.cpp)
		// the accent: one colour (its lighter and darker shades worked out) or each of the five named
		uint32_t c;
		if (parseColor(kv["accent"], c))
		{
			rnds::Accent a;
			a.base = c & 0xffffff;
			a.light = mix(a.base, 0xffffff, 0.6f); a.dark = mix(a.base, 0, 0.62f);
			a.band = mix(a.base, 0xffffff, 0.3f); a.low = mix(a.base, 0, 0.2f);
			rnds::setAccent(a);
		}
		else if (parseColor(kv["accent.base"], c))
		{
			rnds::Accent a = rnds::accent;
			a.base = c & 0xffffff;
			uint32_t v;
			a.light = parseColor(kv["accent.light"], v) ? v & 0xffffff : mix(a.base, 0xffffff, 0.6f);
			a.dark = parseColor(kv["accent.dark"], v) ? v & 0xffffff : mix(a.base, 0, 0.62f);
			a.band = parseColor(kv["accent.band"], v) ? v & 0xffffff : mix(a.base, 0xffffff, 0.3f);
			a.low = parseColor(kv["accent.low"], v) ? v & 0xffffff : mix(a.base, 0, 0.2f);
			rnds::setAccent(a);
		}
		else if (!kv["accent"].empty()) rnds::setAccent(kv["accent"]);	// one of Banana's (yellow, blue, ...)
		// the palette's colours by name
		for (auto& e : kv)
			if (e.first.compare(0, 7, "colors.") == 0 && parseColor(e.second, c)) rnds::themeColors[e.first.substr(7)] = c;
		// its files: fonts, the logo, the screens' pictures, system icons and menu sounds, and the dots' switch
		rnds::ThemeFiles& f = rnds::themeFiles;
		f.fontRegular = themeFile(dir, kv["fonts.regular"]);
		f.fontMedium = themeFile(dir, kv["fonts.medium"]);
		f.logo = themeImage(dir, kv["logo"]);
		f.wallTop = themeImage(dir, kv["background.top"]);
		f.dither = kv["background.dither"] != "false";
		for (auto& e : kv)
		{
			if (e.first.compare(0, 6, "icons.") == 0) { std::string p = themeImage(dir, e.second); if (!p.empty()) f.icons[e.first.substr(6)] = p; }
			if (e.first.compare(0, 7, "sounds.") == 0)
			{
				if (e.second == "off") f.sounds[e.first.substr(7)] = "off";
				else { std::string p = themeFile(dir, e.second); if (!p.empty()) f.sounds[e.first.substr(7)] = p; }
			}
		}
		return base;
	}

	std::string themedIcon(const std::string& systemId, const std::string& def)
	{
		auto it = rnds::themeFiles.icons.find(systemId);
		return it == rnds::themeFiles.icons.end() ? def : it->second;
	}
}
