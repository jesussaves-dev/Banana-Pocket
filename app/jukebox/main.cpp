// Jukebox: a music player for Banana, a Wayland client of banana-ui's compositor.
//
// Audio is decoded in-process (libavformat/libavcodec/libswresample) and played through SDL's ALSA driver, which is
// PipeWire here: the app is a pure sound-server client. Decoding ourselves, instead of driving mpv, is what lets the
// visualizer read the very samples being played, and gives tags, embedded covers and exact seeks from one code path.
//
// Threads: the UI/main thread, one decoder thread per track, SDL's audio callback, one library scanner, one cover
// loader. The main thread blocks only in SDL_WaitEventTimeout. Track advance and the sleep-timer fade are driven by
// tick(), which runs whether or not the panel is drawn.
//
// Env: JUKEBOX_MUSIC (library root, default /data/music), BANANA_APP_DATA (cache + state), JUKEBOX_FONTS (a folder
// holding PixelifySans-*.ttf and Inter-*.ttf), JUKEBOX_SELFTEST=<outdir> (render screens to PNGs and exit; host only).
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

static volatile sig_atomic_t g_stop = 0;
static void onStopSignal(int) { g_stop = 1; }

namespace
{
	const int RATE = 48000;
	const int VIZ_N = 4096;		// the visualizer's tap on the audio callback (mono floats)
	const int FFT_N = 1024;
	const int BANDS = 28;

	std::string envOr(const char* k, const char* d) { const char* v = std::getenv(k); return (v && *v) ? v : d; }
	double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
	void logLine(const std::string& s) { fprintf(stderr, "jukebox: %s\n", s.c_str()); fflush(stderr); }

	std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
	std::string baseName(const std::string& p) { size_t i = p.find_last_of('/'); return i == std::string::npos ? p : p.substr(i + 1); }
	std::string dirName(const std::string& p) { size_t i = p.find_last_of('/'); return i == std::string::npos ? "." : p.substr(0, i); }
	std::string stem(const std::string& p) { std::string b = baseName(p); size_t i = b.find_last_of('.'); return i == std::string::npos || i == 0 ? b : b.substr(0, i); }
	bool fileExists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }
	std::string clean(std::string s) { for (auto& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return s; }
	std::string clock(double s)
	{
		int t = s < 0 ? 0 : (int)s;
		char b[24];
		if (t >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
		else snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
		return b;
	}

	// ---- colour --------------------------------------------------------------------------------------------------
	SDL_Color rgb(int r, int g, int b, int a = 255) { SDL_Color c; c.r = (Uint8)r; c.g = (Uint8)g; c.b = (Uint8)b; c.a = (Uint8)a; return c; }
	void toHsv(SDL_Color c, float& h, float& s, float& v)
	{
		float r = c.r / 255.f, g = c.g / 255.f, b = c.b / 255.f;
		float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
		v = mx; s = mx > 0 ? d / mx : 0;
		if (d <= 0) h = 0;
		else if (mx == r) h = 60.f * std::fmod((g - b) / d, 6.f);
		else if (mx == g) h = 60.f * ((b - r) / d + 2.f);
		else h = 60.f * ((r - g) / d + 4.f);
		if (h < 0) h += 360.f;
	}
	SDL_Color hsv(float h, float s, float v)
	{
		h = std::fmod(h, 360.f); if (h < 0) h += 360.f;
		float c = v * s, x = c * (1.f - std::fabs(std::fmod(h / 60.f, 2.f) - 1.f)), m = v - c, r = 0, g = 0, b = 0;
		switch ((int)(h / 60.f) % 6)
		{
		case 0: r = c; g = x; break;
		case 1: r = x; g = c; break;
		case 2: g = c; b = x; break;
		case 3: g = x; b = c; break;
		case 4: r = x; b = c; break;
		default: r = c; b = x; break;
		}
		return rgb((int)((r + m) * 255.f + .5f), (int)((g + m) * 255.f + .5f), (int)((b + m) * 255.f + .5f));
	}
	SDL_Color mix(SDL_Color a, SDL_Color b, float t)
	{
		t = std::max(0.f, std::min(1.f, t));
		return rgb((int)(a.r + (b.r - a.r) * t), (int)(a.g + (b.g - a.g) * t), (int)(a.b + (b.b - a.b) * t));
	}

	const SDL_Color INK = rgb(0x14, 0x10, 0x1f), CREAM = rgb(0xff, 0xf4, 0xd6);
	const SDL_Color BANANA = rgb(0xff, 0xd2, 0x1e), CYAN = rgb(0x00, 0xdb, 0xff);

	// What a cover gives the screen: two accents bright enough to carry on the dark ground, whatever the art was.
	struct Pal { SDL_Color a, b; bool valid; };
	Pal fallbackPal() { Pal p; p.a = BANANA; p.b = CYAN; p.valid = false; return p; }
	Pal paletteOf(SDL_Surface* s)	// s is ARGB8888, already small
	{
		const int NB = 12;
		double w[NB] = {}, sr[NB] = {}, sg[NB] = {}, sb[NB] = {};
		const Uint32* px = (const Uint32*)s->pixels;
		for (int y = 0; y < s->h; y++)
			for (int x = 0; x < s->w; x++)
			{
				Uint32 p = px[y * (s->pitch / 4) + x];
				SDL_Color c = rgb((p >> 16) & 255, (p >> 8) & 255, p & 255);
				float h, sa, v; toHsv(c, h, sa, v);
				if (sa < 0.28f || v < 0.28f) continue;	// greys and near-blacks say nothing about the cover's colour
				int bi = (int)(h / 30.f) % NB;
				double wt = (double)sa * v;
				w[bi] += wt; sr[bi] += c.r * wt; sg[bi] += c.g * wt; sb[bi] += c.b * wt;
			}
		int b1 = -1;
		for (int i = 0; i < NB; i++) if (w[i] > 0 && (b1 < 0 || w[i] > w[b1])) b1 = i;
		if (b1 < 0 || w[b1] < s->w * s->h * 0.02) return fallbackPal();
		int b2 = -1;
		for (int i = 0; i < NB; i++)
		{
			int d = std::min((i - b1 + NB) % NB, (b1 - i + NB) % NB);
			if (d >= 3 && w[i] > w[b1] * 0.12 && (b2 < 0 || w[i] > w[b2])) b2 = i;
		}
		auto lift = [](double r, double g, double b, double wt)
		{
			float h, sa, v; toHsv(rgb((int)(r / wt), (int)(g / wt), (int)(b / wt)), h, sa, v);
			return hsv(h, std::max(sa, 0.55f), std::max(v, 0.82f));
		};
		Pal p; p.valid = true;
		p.a = lift(sr[b1], sg[b1], sb[b1], w[b1]);
		if (b2 >= 0) p.b = lift(sr[b2], sg[b2], sb[b2], w[b2]);
		else { float h, sa, v; toHsv(p.a, h, sa, v); p.b = hsv(h + 48.f, sa, 1.f); }
		return p;
	}

	// area-average down to at most `mx` on the long side (covers can be 3000 px; the GPU shouldn't be shown that)
	SDL_Surface* shrink(SDL_Surface* src, int mx)
	{
		SDL_Surface* s = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_ARGB8888, 0);
		if (!s) return nullptr;
		int big = std::max(s->w, s->h);
		if (big <= mx) return s;
		int dw = std::max(1, s->w * mx / big), dh = std::max(1, s->h * mx / big);
		SDL_Surface* d = SDL_CreateRGBSurfaceWithFormat(0, dw, dh, 32, SDL_PIXELFORMAT_ARGB8888);
		if (!d) { SDL_FreeSurface(s); return nullptr; }
		const Uint32* sp = (const Uint32*)s->pixels; Uint32* dp = (Uint32*)d->pixels;
		int sps = s->pitch / 4, dps = d->pitch / 4;
		for (int y = 0; y < dh; y++)
		{
			int y0 = y * s->h / dh, y1 = std::max(y0 + 1, (y + 1) * s->h / dh);
			for (int x = 0; x < dw; x++)
			{
				int x0 = x * s->w / dw, x1 = std::max(x0 + 1, (x + 1) * s->w / dw);
				unsigned long a = 0, r = 0, g = 0, b = 0, n = 0;
				for (int yy = y0; yy < y1; yy++)
					for (int xx = x0; xx < x1; xx++) { Uint32 p = sp[yy * sps + xx]; a += p >> 24; r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++; }
				dp[y * dps + x] = ((Uint32)(a / n) << 24) | ((Uint32)(r / n) << 16) | ((Uint32)(g / n) << 8) | (Uint32)(b / n);
			}
		}
		SDL_FreeSurface(s);
		return d;
	}

	// ---- library -------------------------------------------------------------------------------------------------
	struct Track
	{
		std::string path, title, artist, album;
		int no = 0, disc = 0, year = 0;
		double dur = 0;
		long mtime = 0, size = 0;
	};
	struct Album
	{
		std::string name, artist, dir, artPath;	// artPath: the track whose folder art or embedded picture stands for the album
		std::vector<int> tracks;
		int year = 0;
	};
	struct Library { std::vector<Track> tracks; std::vector<Album> albums; };

	bool isAudio(const std::string& p)
	{
		static const char* ext[] = { "mp3", "flac", "ogg", "oga", "opus", "m4a", "aac", "wav", "wma", "ape", "mka", "aif", "aiff" };
		size_t i = p.find_last_of('.');
		if (i == std::string::npos) return false;
		std::string e = lower(p.substr(i + 1));
		for (const char* x : ext) if (e == x) return true;
		return false;
	}
	std::string tag(AVFormatContext* fc, int si, const char* k)
	{
		AVDictionaryEntry* e = nullptr;
		if (si >= 0) e = av_dict_get(fc->streams[si]->metadata, k, nullptr, 0);
		if (!e || !*e->value) e = av_dict_get(fc->metadata, k, nullptr, 0);
		return e && e->value ? clean(e->value) : std::string();
	}
	bool probe(const std::string& p, Track& t)
	{
		AVFormatContext* fc = nullptr;
		if (avformat_open_input(&fc, p.c_str(), nullptr, nullptr) < 0) return false;
		avformat_find_stream_info(fc, nullptr);
		int si = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		if (si < 0) { avformat_close_input(&fc); return false; }
		t.path = p;
		t.title = tag(fc, si, "title"); t.artist = tag(fc, si, "artist");
		std::string aa = tag(fc, si, "album_artist");
		if (!aa.empty()) t.artist = aa;
		t.album = tag(fc, si, "album");
		t.no = atoi(tag(fc, si, "track").c_str());
		t.disc = atoi(tag(fc, si, "disc").c_str());
		t.year = atoi(tag(fc, si, "date").c_str());
		double d = fc->duration > 0 ? fc->duration / (double)AV_TIME_BASE : 0;
		if (d <= 0 && fc->streams[si]->duration > 0) d = fc->streams[si]->duration * av_q2d(fc->streams[si]->time_base);
		t.dur = d;
		avformat_close_input(&fc);
		if (t.title.empty()) t.title = clean(stem(p));
		return true;
	}

	void walk(const std::string& dir, std::vector<std::string>& out, int depth)
	{
		if (depth > 8) return;
		DIR* d = opendir(dir.c_str());
		if (!d) return;
		while (dirent* e = readdir(d))
		{
			if (e->d_name[0] == '.') continue;
			std::string p = dir + "/" + e->d_name;
			struct stat st;
			if (stat(p.c_str(), &st) != 0) continue;
			if (S_ISDIR(st.st_mode)) walk(p, out, depth + 1);
			else if (S_ISREG(st.st_mode) && isAudio(p)) out.push_back(p);
		}
		closedir(d);
	}

	std::string unknownArtist() { return "Unknown artist"; }
	std::string cmpKey(const std::string& s) { return lower(s); }

	std::unique_ptr<Library> buildLibrary(std::vector<Track>&& ts)
	{
		std::unique_ptr<Library> L(new Library);
		L->tracks = std::move(ts);
		for (auto& t : L->tracks) if (t.artist.empty()) t.artist = unknownArtist();
		std::sort(L->tracks.begin(), L->tracks.end(), [](const Track& a, const Track& b)
		{
			if (dirName(a.path) != dirName(b.path)) return cmpKey(dirName(a.path)) < cmpKey(dirName(b.path));
			if (a.disc != b.disc) return a.disc < b.disc;
			if (a.no != b.no) return a.no < b.no;
			return cmpKey(a.path) < cmpKey(b.path);
		});
		std::map<std::string, int> idx;
		for (size_t i = 0; i < L->tracks.size(); i++)
		{
			const Track& t = L->tracks[i];
			std::string name = t.album.empty() ? baseName(dirName(t.path)) : t.album;
			std::string key = dirName(t.path) + "\x1f" + name;
			auto it = idx.find(key);
			if (it == idx.end())
			{
				Album a; a.name = name; a.artist = t.artist; a.dir = dirName(t.path); a.artPath = t.path; a.year = t.year;
				L->albums.push_back(a);
				it = idx.emplace(key, (int)L->albums.size() - 1).first;
			}
			Album& a = L->albums[(size_t)it->second];
			a.tracks.push_back((int)i);
			if (a.artist != t.artist) a.artist = "Various artists";
			if (!a.year) a.year = t.year;
		}
		std::sort(L->albums.begin(), L->albums.end(), [](const Album& a, const Album& b)
		{
			if (a.artist != b.artist) return cmpKey(a.artist) < cmpKey(b.artist);
			if (a.year != b.year) return a.year < b.year;
			return cmpKey(a.name) < cmpKey(b.name);
		});
		return L;
	}

	// ---- covers ----------------------------------------------------------------------------------------------------
	SDL_Surface* loadCoverSurface(const std::string& trackPath)
	{
		static const char* names[] = { "cover.jpg", "cover.png", "folder.jpg", "folder.png", "front.jpg", "front.png", "album.jpg", "album.png",
			"Cover.jpg", "Cover.png", "Folder.jpg", "Folder.png", "Front.jpg", "AlbumArt.jpg" };
		std::string d = dirName(trackPath);
		for (const char* n : names)
		{
			std::string p = d + "/" + n;
			if (fileExists(p)) if (SDL_Surface* s = IMG_Load(p.c_str())) return s;
		}
		AVFormatContext* fc = nullptr;
		if (avformat_open_input(&fc, trackPath.c_str(), nullptr, nullptr) < 0) return nullptr;
		SDL_Surface* out = nullptr;
		for (unsigned i = 0; i < fc->nb_streams && !out; i++)
			if (fc->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)
			{
				const AVPacket& pk = fc->streams[i]->attached_pic;
				if (SDL_RWops* rw = SDL_RWFromConstMem(pk.data, pk.size)) out = IMG_Load_RW(rw, 1);
			}
		avformat_close_input(&fc);
		return out;
	}

	// ---- lyrics ----------------------------------------------------------------------------------------------------
	struct Lyric { double t; std::string s; };
	std::vector<Lyric> loadLyrics(const std::string& trackPath)
	{
		std::vector<Lyric> out;
		FILE* f = fopen((dirName(trackPath) + "/" + stem(trackPath) + ".lrc").c_str(), "r");
		if (!f) return out;
		char line[1024];
		while (fgets(line, sizeof line, f))
		{
			std::string s = line;
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
			std::vector<double> times;
			size_t p = 0;
			while (p < s.size() && s[p] == '[')
			{
				size_t e = s.find(']', p);
				if (e == std::string::npos) break;
				int m = 0; double sec = 0;
				if (sscanf(s.substr(p + 1, e - p - 1).c_str(), "%d:%lf", &m, &sec) == 2) times.push_back(m * 60 + sec);
				p = e + 1;
			}
			if (times.empty()) continue;
			std::string text = s.substr(p);
			while (!text.empty() && text.front() == ' ') text.erase(text.begin());
			for (double t : times) out.push_back({ t, text });
		}
		fclose(f);
		std::stable_sort(out.begin(), out.end(), [](const Lyric& a, const Lyric& b) { return a.t < b.t; });
		return out;
	}

	// ---- FFT ---------------------------------------------------------------------------------------------------------
	struct Fft
	{
		float cs[FFT_N / 2], sn[FFT_N / 2], win[FFT_N];
		int rev[FFT_N];
		Fft()
		{
			for (int i = 0; i < FFT_N / 2; i++) { cs[i] = (float)std::cos(2 * M_PI * i / FFT_N); sn[i] = (float)-std::sin(2 * M_PI * i / FFT_N); }
			for (int i = 0; i < FFT_N; i++) win[i] = 0.5f - 0.5f * (float)std::cos(2 * M_PI * i / (FFT_N - 1));
			int bits = 0; while ((1 << bits) < FFT_N) bits++;
			for (int i = 0; i < FFT_N; i++) { int r = 0; for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b); rev[i] = r; }
		}
		void run(const float* in, float* re, float* im) const
		{
			for (int i = 0; i < FFT_N; i++) { re[rev[i]] = in[i] * win[i]; im[rev[i]] = 0; }
			for (int len = 2; len <= FFT_N; len <<= 1)
			{
				int half = len / 2, step = FFT_N / len;
				for (int i = 0; i < FFT_N; i += len)
					for (int k = 0; k < half; k++)
					{
						float wr = cs[k * step], wi = sn[k * step];
						int a = i + k, b = a + half;
						float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
						re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
					}
			}
		}
	};

	// ---- loudness (ReplayGain / R128 tags; nothing is analysed on the device) ---------------------------------------
	struct RG { double trackDb = 0, albumDb = 0, trackPeak = 0, albumPeak = 0; bool hasTrack = false, hasAlbum = false; };
	bool parseDb(const std::string& s, double& out)
	{
		if (s.empty()) return false;
		char* e = nullptr;
		double v = strtod(s.c_str(), &e);
		if (e == s.c_str() || !std::isfinite(v)) return false;
		out = v;
		return true;
	}
	double r128ToDb(double q78) { return q78 / 256.0 + 5.0; }
	RG readRG(AVFormatContext* fc, int si)
	{
		RG r;
		double v;
		if (parseDb(tag(fc, si, "replaygain_track_gain"), v)) { r.trackDb = v; r.hasTrack = true; }
		if (parseDb(tag(fc, si, "replaygain_album_gain"), v)) { r.albumDb = v; r.hasAlbum = true; }
		if (parseDb(tag(fc, si, "replaygain_track_peak"), v) && v > 0) r.trackPeak = v;
		if (parseDb(tag(fc, si, "replaygain_album_peak"), v) && v > 0) r.albumPeak = v;
		// Opus carries R128_*_GAIN in Q7.8 dB against -23 LUFS; ReplayGain's reference is -18 LUFS
		if (!r.hasTrack && parseDb(tag(fc, si, "r128_track_gain"), v)) { r.trackDb = r128ToDb(v); r.hasTrack = true; }
		if (!r.hasAlbum && parseDb(tag(fc, si, "r128_album_gain"), v)) { r.albumDb = r128ToDb(v); r.hasAlbum = true; }
		return r;
	}
	// mode 0 off, 1 track, 2 album (falls back to the track gain). A boost never goes past the peak, or past unity when the peak is unknown.
	float rgLinear(const RG& r, int mode)
	{
		if (mode == 0) return 1.f;
		bool album = mode == 2 && r.hasAlbum;
		if (!album && !r.hasTrack) return 1.f;
		double db = album ? r.albumDb : r.trackDb;
		double peak = album ? r.albumPeak : r.trackPeak;
		double g = std::pow(10.0, db / 20.0);
		g = std::min(g, peak > 0 ? 1.0 / peak : 1.0);
		return (float)std::max(0.03, std::min(g, 4.0));
	}

	// ---- audio engine ----------------------------------------------------------------------------------------------
	// One decoder thread feeds a PCM ring. At the end of a file it opens the queue's next entry (setNext) and keeps writing
	// into the same ring, leaving a marker where the new song starts; the callback flips position and gain at that frame.
	struct Engine
	{
		static constexpr float FADE_LONG = 1.f / (0.08f * RATE);	// pause / resume: 80 ms
		static constexpr float FADE_SHORT = 1.f / (0.01f * RATE);	// seek / song start: 10 ms
		struct Marker { uint64_t at; std::string path; RG rg; };
		struct Src { AVFormatContext* fc = nullptr; AVCodecContext* cc = nullptr; SwrContext* swr = nullptr; int si = -1; RG rg; };

		SDL_AudioDeviceID dev = 0;
		std::mutex m;
		std::condition_variable cv;
		std::vector<uint8_t> ring;
		size_t rd = 0, wr = 0, fill = 0;
		std::thread th;
		std::atomic<bool> stop{ false }, eof{ false }, ended{ false }, paused{ true };
		std::atomic<double> seekReq{ -1 };
		std::atomic<float> gain{ 1.f };	// the sleep timer's fade
		std::atomic<int> rgMode{ 1 };
		std::atomic<int> switched{ 0 }, nextFailed{ 0 };
		std::atomic<bool> rampReset{ false }, pauseReady{ false };
		std::atomic<float> mulTarget{ 1.f }, mulStep{ FADE_SHORT };
		float mul = 0.f;	// the callback's pause/seek ramp (only the callback writes it)
		bool fresh = false, devPaused = true;	// main thread
		double startSec = 0;
		uint64_t consumed = 0;	// frames the callback has taken since startSec
		uint64_t playedAbs = 0, writtenAbs = 0;	// ring frames in and out since the track was opened
		std::deque<Marker> markers;
		std::string playPath, nextPath, badPath, failedPath;	// under m
		RG playRg;	// under m: the song being heard
		float viz[VIZ_N];
		std::atomic<unsigned> vizPos{ 0 };
		bool open_ = false;

		void initBuffers()
		{
			ring.assign((size_t)RATE * 4 * 2, 0);
			memset(viz, 0, sizeof viz);
		}
		bool init()
		{
			initBuffers();
			SDL_AudioSpec want, have;
			SDL_zero(want);
			want.freq = RATE; want.format = AUDIO_S16SYS; want.channels = 2;
			want.samples = 4096;	// AudioReach wants big periods (BananaTube settled on 0.4 s)
			want.callback = &Engine::callback; want.userdata = this;
			dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
			if (!dev) { logLine(std::string("no audio device: ") + SDL_GetError()); return false; }
			if (have.format != AUDIO_S16SYS || have.channels != 2 || have.freq != RATE)
				logLine("audio spec differs: " + std::to_string(have.freq) + " Hz, " + std::to_string(have.channels) + " ch");
			SDL_PauseAudioDevice(dev, 1);
			devPaused = true;
			return true;
		}
		static void callback(void* ud, Uint8* out, int len)
		{
			Engine* e = (Engine*)ud;
			int frames = len / 4;
			if (e->rampReset.exchange(false)) e->mul = 0.f;
			bool hold = e->paused.load() && e->mul <= 0.f;	// faded out and paused: play silence and leave the ring alone
			size_t n = 0;
			int bi = frames + 1;	// the frame where a new song's gain takes over
			float rgA, rgB;
			{
				std::lock_guard<std::mutex> lk(e->m);
				int mode = e->rgMode.load();
				rgA = rgB = rgLinear(e->playRg, mode);
				if (!hold)
				{
					n = std::min((size_t)len, e->fill);
					size_t first = std::min(n, e->ring.size() - e->rd);
					memcpy(out, &e->ring[e->rd], first);
					if (n > first) memcpy(out + first, &e->ring[0], n - first);
					e->rd = (e->rd + n) % e->ring.size();
					e->fill -= n;
					uint64_t before = e->playedAbs, got = n / 4;
					e->playedAbs += got;
					e->consumed += got;
					while (!e->markers.empty() && e->playedAbs >= e->markers.front().at)
					{
						const Marker& mk = e->markers.front();
						bi = (int)std::min(mk.at > before ? mk.at - before : 0, (uint64_t)frames);
						e->startSec = 0; e->consumed = e->playedAbs - mk.at;
						e->playPath = mk.path; e->playRg = mk.rg;
						e->switched++;
						e->markers.pop_front();
					}
					rgB = rgLinear(e->playRg, mode);
					if (n < (size_t)len && e->eof.load() && e->fill == 0 && e->open_) e->ended = true;
				}
			}
			if (!hold) e->cv.notify_all();
			if (n < (size_t)len) memset(out + n, 0, (size_t)len - n);
			int16_t* s = (int16_t*)out;
			float tgt = e->mulTarget.load(), st = e->mulStep.load(), gs = e->gain.load(), mul = e->mul;
			unsigned wp = e->vizPos.load(std::memory_order_relaxed);
			for (int i = 0; i < frames; i++)
			{
				if (mul < tgt) mul = std::min(tgt, mul + st); else if (mul > tgt) mul = std::max(tgt, mul - st);
				float g = gs * mul * (i < bi ? rgA : rgB);
				float l = std::max(-32768.f, std::min(32767.f, s[2 * i] * g)), r = std::max(-32768.f, std::min(32767.f, s[2 * i + 1] * g));
				s[2 * i] = (int16_t)l; s[2 * i + 1] = (int16_t)r;
				e->viz[(wp + (unsigned)i) & (VIZ_N - 1)] = (l + r) * (0.5f / 32768.f);
			}
			e->mul = mul;
			e->vizPos.store(wp + (unsigned)frames, std::memory_order_release);
			if (e->paused.load() && mul <= 0.f) e->pauseReady = true;
		}

		void closeTrack()
		{
			stop = true;
			cv.notify_all();
			if (th.joinable()) th.join();
			std::lock_guard<std::mutex> lk(m);
			rd = wr = fill = 0; consumed = 0; open_ = false;
			playedAbs = writtenAbs = 0; markers.clear(); playRg = RG(); failedPath.clear();
			eof = false; ended = false; stop = false; seekReq = -1; switched = 0; nextFailed = 0;
		}
		void openTrack(const std::string& path, double start)
		{
			closeTrack();
			{
				std::lock_guard<std::mutex> lk(m);
				startSec = start; consumed = 0; open_ = true; playPath = path;
			}
			fresh = true; mulStep = FADE_SHORT; rampReset = true;
			th = std::thread([this, path, start] { decode(path, start); });
		}
		void seek(double t)
		{
			std::string pp;
			bool reopen;
			{
				std::lock_guard<std::mutex> lk(m);
				reopen = !markers.empty();	// the decoder already moved on to the next song: go back to this one
				pp = playPath;
			}
			if (reopen) { openTrack(pp, std::max(0.0, t)); return; }
			seekReq = std::max(0.0, t);
			cv.notify_all();
		}
		// What to play after this song ("" for nothing). Anything but a repeat of the standing choice drops a song already prefetched.
		void setNext(const std::string& p)
		{
			bool reopen = false;
			std::string pp;
			{
				std::lock_guard<std::mutex> lk(m);
				bool pending = !markers.empty();	// the decoder already took its next song
				if (pending && markers.back().path == p) nextPath.clear();
				else { nextPath = p; reopen = pending; }
				if (p != badPath) badPath.clear();
				pp = playPath;
			}
			if (reopen) openTrack(pp, pos());
			cv.notify_all();
		}
		std::string takeFailed() { std::lock_guard<std::mutex> lk(m); return failedPath; }
		double pos()
		{
			std::lock_guard<std::mutex> lk(m);
			return startSec + (double)consumed / RATE;
		}
		void setPaused(bool p)
		{
			paused = p;
			if (p) { mulTarget = 0.f; mulStep = FADE_LONG; return; }	// the device stops once the fade has run out (tickAudio)
			mulTarget = 1.f; mulStep = fresh ? FADE_SHORT : FADE_LONG; fresh = false;
			if (dev) { SDL_PauseAudioDevice(dev, 0); devPaused = false; }
		}
		void tickAudio()
		{
			if (pauseReady.exchange(false) && paused && dev && !devPaused) { SDL_PauseAudioDevice(dev, 1); devPaused = true; }
		}
		bool fading() const { return dev && paused && !devPaused; }
		bool push(const uint8_t* d, size_t n)
		{
			size_t done = 0;
			while (done < n)
			{
				std::unique_lock<std::mutex> lk(m);
				cv.wait_for(lk, std::chrono::milliseconds(40), [&] { return stop.load() || seekReq.load() >= 0 || ring.size() - fill > 0; });
				if (stop || seekReq.load() >= 0) return false;
				size_t space = ring.size() - fill;
				size_t c = std::min(space, n - done);
				size_t first = std::min(c, ring.size() - wr);
				memcpy(&ring[wr], d + done, first);
				if (c > first) memcpy(&ring[0], d + done + first, c - first);
				wr = (wr + c) % ring.size(); fill += c; done += c; writtenAbs += c / 4;
			}
			return true;
		}
		static void closeSrc(Src& s)
		{
			if (s.swr) swr_free(&s.swr);
			if (s.cc) avcodec_free_context(&s.cc);
			if (s.fc) avformat_close_input(&s.fc);
			s = Src();
		}
		static bool openSrc(const std::string& path, Src& s)
		{
			if (avformat_open_input(&s.fc, path.c_str(), nullptr, nullptr) >= 0 && avformat_find_stream_info(s.fc, nullptr) >= 0)
			{
				const AVCodec* codec = nullptr;
				s.si = av_find_best_stream(s.fc, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
				if (s.si >= 0 && codec && (s.cc = avcodec_alloc_context3(codec)) && avcodec_parameters_to_context(s.cc, s.fc->streams[s.si]->codecpar) >= 0 &&
					avcodec_open2(s.cc, codec, nullptr) >= 0)
				{
					AVChannelLayout out;
					av_channel_layout_default(&out, 2);
					if (swr_alloc_set_opts2(&s.swr, &out, AV_SAMPLE_FMT_S16, RATE, &s.cc->ch_layout, s.cc->sample_fmt, s.cc->sample_rate, 0, nullptr) >= 0 && swr_init(s.swr) >= 0)
					{
						s.rg = readRG(s.fc, s.si);
						return true;
					}
				}
			}
			closeSrc(s);
			return false;
		}
		void decode(std::string path, double start)
		{
			Src s;
			AVPacket* pk = av_packet_alloc();
			AVFrame* fr = av_frame_alloc();
			std::vector<uint8_t> buf;
			bool ok = openSrc(path, s);
			if (!ok) { logLine("cannot decode " + path); eof = true; }
			else { std::lock_guard<std::mutex> lk(m); playRg = s.rg; }
			auto doSeek = [&](double t)
			{
				av_seek_frame(s.fc, -1, (int64_t)(t * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
				avcodec_flush_buffers(s.cc);
				swr_init(s.swr);
				{
					std::lock_guard<std::mutex> lk(m);
					rd = wr = fill = 0; startSec = t; consumed = 0; eof = false; ended = false;
					writtenAbs = playedAbs; markers.clear();
				}
				if (mulTarget.load() > 0.5f) mulStep = FADE_SHORT;
				rampReset = true;
			};
			// the file ran out: continue straight into the queue's next song, if there is one and it opens
			auto tryNext = [&]() -> bool
			{
				std::string np;
				{
					std::lock_guard<std::mutex> lk(m);
					np = nextPath;
					if (np.empty() || np == badPath) return false;
				}
				Src n;
				if (!openSrc(np, n))
				{
					logLine("cannot decode " + np);
					std::lock_guard<std::mutex> lk(m);
					badPath = np; failedPath = np; nextFailed++;
					return false;
				}
				closeSrc(s);
				s = n;
				std::lock_guard<std::mutex> lk(m);
				markers.push_back({ writtenAbs, np, s.rg });
				nextPath.clear();	// taken: whatever follows this song is the App's next word
				eof = false; ended = false;
				return true;
			};
			if (ok && start > 0.05) doSeek(start);
			bool atEnd = false;
			while (ok && !stop)
			{
				double sr = seekReq.exchange(-1);
				if (sr >= 0) { doSeek(sr); atEnd = false; continue; }
				if (atEnd)	// parked at the end: a seek, or a next song, may still come
				{
					if (tryNext()) { atEnd = false; continue; }
					std::unique_lock<std::mutex> lk(m);
					cv.wait_for(lk, std::chrono::milliseconds(60));
					continue;
				}
				int r = av_read_frame(s.fc, pk);
				bool flushing = r < 0;
				if (!flushing && pk->stream_index != s.si) { av_packet_unref(pk); continue; }
				if (avcodec_send_packet(s.cc, flushing ? nullptr : pk) < 0 && !flushing) { av_packet_unref(pk); continue; }
				av_packet_unref(pk);
				bool interrupted = false;
				while (!stop && avcodec_receive_frame(s.cc, fr) >= 0)
				{
					int cap = swr_get_out_samples(s.swr, fr->nb_samples);
					buf.resize((size_t)cap * 4);
					uint8_t* o = buf.data();
					int got = swr_convert(s.swr, &o, cap, (const uint8_t**)fr->extended_data, fr->nb_samples);
					av_frame_unref(fr);
					if (got > 0 && !push(buf.data(), (size_t)got * 4)) { interrupted = true; break; }
				}
				if (interrupted) continue;	// a seek (or a stop) arrived mid-frame
				if (flushing)
				{
					if (!tryNext()) { atEnd = true; std::lock_guard<std::mutex> lk(m); eof = true; }
				}
			}
			closeSrc(s);
			av_packet_free(&pk);
			av_frame_free(&fr);
		}
		~Engine() { closeTrack(); if (dev) SDL_CloseAudioDevice(dev); }
	};

	// ---- fonts + text ----------------------------------------------------------------------------------------------
	bool fontHas(TTF_Font* f, const std::string& s)
	{
		for (size_t i = 0; i < s.size();)
		{
			unsigned char c = (unsigned char)s[i];
			uint32_t cp; int n;
			if (c < 0x80) { cp = c; n = 1; } else if ((c >> 5) == 6) { cp = c & 31; n = 2; } else if ((c >> 4) == 14) { cp = c & 15; n = 3; } else { cp = c & 7; n = 4; }
			for (int k = 1; k < n && i + (size_t)k < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i + (size_t)k] & 63);
			i += (size_t)n;
			if (cp > 32 && !TTF_GlyphIsProvided32(f, cp)) return false;
		}
		return true;
	}

	struct Tx { SDL_Texture* t = nullptr; int w = 0, h = 0; };
	struct CoverTex { SDL_Texture* tex = nullptr; int w = 0, h = 0; Pal pal = fallbackPal(); bool asked = false, none = false; };
	struct Job { std::string key, path; int px; };
	struct Done { std::string key; SDL_Surface* surf; Pal pal; };

	enum Btn { UP, DOWN, LEFT, RIGHT, BA, BB, BX, BY, BL, BR, BSTART, BSELECT, NBTN };
	enum class Screen { LIB, ALBUM, NOW };

	struct App
	{
		SDL_Window* win = nullptr;
		SDL_Renderer* ren = nullptr;
		SDL_GameController* pad = nullptr;
		int W = 640, H = 557;
		float u = 5.f;			// layout unit: 1% of the short side
		int cx = 0, cw = 640;	// the content column (wide panels are not stretched past it)
		TTF_Font *fHuge = nullptr, *fBig = nullptr, *fMed = nullptr, *fSmall = nullptr, *fTiny = nullptr;	// Pixelify (huge, big) and Inter (the rest)
		TTF_Font *iBig = nullptr;	// Inter at the big size: a title Pixelify has no glyph for
		std::map<std::string, Tx> texts;
		std::map<std::string, CoverTex> covers;

		std::string musicDir, dataDir;
		std::unique_ptr<Library> lib;
		std::mutex libM;
		std::unique_ptr<Library> pendingLib;
		std::atomic<int> scanSeen{ 0 }, scanTotal{ 0 };
		std::atomic<bool> scanning{ true }, quitting{ false };
		std::thread scanTh, coverTh;
		std::mutex jobM;
		std::condition_variable jobCv;
		std::vector<Job> jobs;
		std::vector<Done> done;

		Engine eng;
		bool audioOk = false;
		std::vector<Track> queue, queueOrig;
		int qi = -1;
		bool shuffle = false;
		int repeat = 0;			// 0 off, 1 all, 2 one
		int vizStyle = 0;
		int loudness = 1;		// 0 off, 1 track, 2 album (ReplayGain tags)
		int pfIdx = -1;			// the queue entry the engine has been told to play after this one
		std::set<std::string> badPaths;	// songs that would not open
		bool playing = false;
		Track cur;
		std::vector<Lyric> lyrics;
		double curDur = 0;

		Screen screen = Screen::LIB;
		int tab = 0;			// 0 albums, 1 songs
		int sel = 0, scroll = 0;	// albums grid (0 is "shuffle everything") or songs
		int albumIdx = -1, aSel = 0, aScroll = 0;
		bool quit = false;
		bool dirty = true;
		std::string toast;
		double toastUntil = 0, quitArmUntil = 0;
		double sleepAt = 0;
		int sleepMin = 0;
		bool sleepFading = false;
		double lastSaved = 0;
		int lastSecDrawn = -1;

		Pal palTarget = fallbackPal();
		SDL_Color accA = BANANA, accB = CYAN;	// the live, eased accents
		Fft fft;
		float bars[BANDS] = {}, peaks[BANDS] = {};
		double lastFrame = 0;
		int held[NBTN] = {};
		double heldAt[NBTN] = {}, heldNext[NBTN] = {};
		bool screenOff = false;
		double screenCheck = 0;
		std::string fontDir;

		// ---- setup -----------------------------------------------------------------------------------------------------
		TTF_Font* openFont(const char* name, const char* fallbackName, int px)
		{
			std::vector<std::string> dirs;	// JUKEBOX_FONTS may hold several folders, colon separated
			for (size_t a = 0; a <= fontDir.size();)
			{
				size_t b = fontDir.find(':', a);
				if (b == std::string::npos) b = fontDir.size();
				if (b > a) dirs.push_back(fontDir.substr(a, b - a));
				a = b + 1;
			}
			dirs.push_back("/usr/share/banana/rnds/fonts"); dirs.push_back("/usr/share/banana/banana/fonts");
			for (const std::string& d : dirs)
			{
				if (d.empty()) continue;
				TTF_Font* f = TTF_OpenFont((d + "/" + name).c_str(), px);
				if (f) return f;
			}
			for (const std::string& d : dirs)
			{
				if (d.empty()) continue;
				TTF_Font* f = TTF_OpenFont((d + "/" + fallbackName).c_str(), px);
				if (f) return f;
			}
			return nullptr;
		}
		bool makeFonts()
		{
			auto sz = [&](float k) { return std::max(10, (int)(u * k)); };
			fHuge = openFont("PixelifySans-Medium.ttf", "Inter-SemiBold.ttf", sz(8.f));
			fBig = openFont("PixelifySans-Medium.ttf", "Inter-SemiBold.ttf", sz(5.4f));
			iBig = openFont("Inter-SemiBold.ttf", "PixelifySans-Medium.ttf", sz(4.6f));
			fMed = openFont("Inter-SemiBold.ttf", "PixelifySans-Medium.ttf", sz(3.7f));
			fSmall = openFont("Inter-Regular.ttf", "PixelifySans-Regular.ttf", sz(3.0f));
			fTiny = openFont("Inter-Regular.ttf", "PixelifySans-Regular.ttf", sz(2.5f));
			return fHuge && fBig && iBig && fMed && fSmall && fTiny;
		}
		void layout()
		{
			SDL_GetRendererOutputSize(ren, &W, &H);
			u = std::min(W, H) / 100.f;
			cw = std::min(W, (int)(H * 1.35f));
			cx = (W - cw) / 2;
		}

		// ---- text ------------------------------------------------------------------------------------------------------
		Tx text(TTF_Font* f, const std::string& s, SDL_Color c, int maxW = 0, int wrap = 0)
		{
			if (s.empty()) return Tx();
			char kb[96];
			snprintf(kb, sizeof kb, "%p|%02x%02x%02x|%d|%d|", (void*)f, c.r, c.g, c.b, maxW, wrap);
			std::string key = kb + s;
			auto it = texts.find(key);
			if (it != texts.end()) return it->second;
			if (texts.size() > 500) { for (auto& kv : texts) SDL_DestroyTexture(kv.second.t); texts.clear(); }
			std::string str = s;
			if (maxW > 0 && !wrap)
			{
				int w = 0, h = 0;
				TTF_SizeUTF8(f, str.c_str(), &w, &h);
				while (w > maxW && str.size() > 1)
				{
					size_t e = str.size() - 1;
					while (e > 0 && (str[e] & 0xC0) == 0x80) e--;
					str.erase(e);
					std::string t2 = str + "\xE2\x80\xA6";
					TTF_SizeUTF8(f, t2.c_str(), &w, &h);
					if (w <= maxW) { str = t2; break; }
				}
			}
			SDL_Surface* sf = wrap > 0 ? TTF_RenderUTF8_Blended_Wrapped(f, str.c_str(), c, (Uint32)wrap) : TTF_RenderUTF8_Blended(f, str.c_str(), c);
			Tx t;
			if (sf)
			{
				t.t = SDL_CreateTextureFromSurface(ren, sf);
				t.w = sf->w; t.h = sf->h;
				SDL_FreeSurface(sf);
			}
			texts[key] = t;
			return t;
		}
		int drawText(TTF_Font* f, const std::string& s, SDL_Color c, int x, int y, int maxW = 0, int wrap = 0, int align = 0)
		{
			Tx t = text(f, s, c, maxW, wrap);
			if (!t.t) return 0;
			SDL_Rect d = { align == 1 ? x - t.w / 2 : align == 2 ? x - t.w : x, y, t.w, t.h };
			SDL_RenderCopy(ren, t.t, nullptr, &d);
			return t.h;
		}
		TTF_Font* titleFont(const std::string& s) { return fontHas(fBig, s) ? fBig : iBig; }

		// ---- drawing helpers -------------------------------------------------------------------------------------------
		void fill(int x, int y, int w, int h, SDL_Color c, int a = 255)
		{
			SDL_SetRenderDrawBlendMode(ren, a < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
			SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, (Uint8)a);
			SDL_Rect r = { x, y, w, h };
			SDL_RenderFillRect(ren, &r);
		}
		void frame(int x, int y, int w, int h, int t, SDL_Color c)
		{
			fill(x, y, w, t, c); fill(x, y + h - t, w, t, c); fill(x, y, t, h, c); fill(x + w - t, y, t, h, c);
		}
		void gradient(int x, int y, int w, int h, SDL_Color top, SDL_Color bot)
		{
			SDL_Vertex v[4];
			SDL_Color cs[4] = { top, top, bot, bot };
			float xs[4] = { (float)x, (float)(x + w), (float)x, (float)(x + w) }, ys[4] = { (float)y, (float)y, (float)(y + h), (float)(y + h) };
			for (int i = 0; i < 4; i++) { v[i].position.x = xs[i]; v[i].position.y = ys[i]; v[i].color = cs[i]; v[i].tex_coord.x = 0; v[i].tex_coord.y = 0; }
			int idx[6] = { 0, 1, 2, 1, 3, 2 };
			SDL_RenderGeometry(ren, nullptr, v, 4, idx, 6);
		}
		void triangle(float x, float y, float s, SDL_Color c)	// a play triangle with its left edge at x, centred on y
		{
			SDL_Vertex v[3];
			v[0].position.x = x; v[0].position.y = y - s / 2; v[1].position.x = x; v[1].position.y = y + s / 2; v[2].position.x = x + s * 0.9f; v[2].position.y = y;
			for (auto& q : v) { q.color = c; q.tex_coord.x = q.tex_coord.y = 0; }
			SDL_RenderGeometry(ren, nullptr, v, 3, nullptr, 0);
		}
		void disc(int cxp, int cyp, int r, SDL_Color c)
		{
			SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
			for (int dy = -r; dy <= r; dy++)
			{
				int dx = (int)std::sqrt((double)(r * r - dy * dy));
				SDL_RenderDrawLine(ren, cxp - dx, cyp + dy, cxp + dx, cyp + dy);
			}
		}

		// ---- covers ----------------------------------------------------------------------------------------------------
		CoverTex& cover(const std::string& path, int px)
		{
			std::string key = path + "#" + std::to_string(px);
			CoverTex& c = covers[key];
			if (!c.asked)
			{
				c.asked = true;
				std::lock_guard<std::mutex> lk(jobM);
				jobs.push_back({ key, path, px });
				jobCv.notify_one();
			}
			return c;
		}
		void coverLoop()
		{
			for (;;)
			{
				Job j;
				{
					std::unique_lock<std::mutex> lk(jobM);
					jobCv.wait(lk, [&] { return quitting.load() || !jobs.empty(); });
					if (quitting) return;
					j = jobs.back(); jobs.pop_back();	// newest first: what the user just scrolled to
				}
				Done d; d.key = j.key; d.surf = nullptr; d.pal = fallbackPal();
				if (SDL_Surface* raw = loadCoverSurface(j.path))
				{
					d.surf = shrink(raw, j.px);
					SDL_FreeSurface(raw);
					if (d.surf)
					{
						SDL_Surface* tiny = shrink(d.surf, 48);
						if (tiny) { d.pal = paletteOf(tiny); SDL_FreeSurface(tiny); }
					}
				}
				std::lock_guard<std::mutex> lk(jobM);
				done.push_back(d);
			}
		}
		void collectCovers()
		{
			std::vector<Done> got;
			{
				std::lock_guard<std::mutex> lk(jobM);
				got.swap(done);
			}
			for (Done& d : got)
			{
				CoverTex& c = covers[d.key];
				if (d.surf)
				{
					c.tex = SDL_CreateTextureFromSurface(ren, d.surf);
					c.w = d.surf->w; c.h = d.surf->h; c.pal = d.pal;
					SDL_FreeSurface(d.surf);
				}
				else c.none = true;
				dirty = true;
			}
		}
		void drawCover(const std::string& path, int px, int x, int y, int size, SDL_Color shadow, bool shadowOn)
		{
			if (shadowOn) fill(x + (int)(u * 1.1f), y + (int)(u * 1.1f), size, size, shadow);
			CoverTex& c = cover(path, px);
			if (c.tex)
			{
				int w = size, h = size;
				if (c.w > c.h) h = size * c.h / c.w; else if (c.h > c.w) w = size * c.w / c.h;
				SDL_Rect d = { x + (size - w) / 2, y + (size - h) / 2, w, h };
				SDL_RenderCopy(ren, c.tex, nullptr, &d);
				return;
			}
			// no picture (yet): a record, so the slot never looks broken
			fill(x, y, size, size, rgb(0x24, 0x1d, 0x38));
			disc(x + size / 2, y + size / 2, size * 40 / 100, rgb(0x0c, 0x09, 0x14));
			disc(x + size / 2, y + size / 2, size * 14 / 100, accA);
			disc(x + size / 2, y + size / 2, std::max(2, size * 2 / 100), rgb(0x0c, 0x09, 0x14));
		}

		// ---- library scan ------------------------------------------------------------------------------------------------
		std::string cachePath() { return dataDir + "/library.tsv"; }
		void loadCache(std::map<std::string, Track>& out)
		{
			FILE* f = fopen(cachePath().c_str(), "r");
			if (!f) return;
			char line[4096];
			while (fgets(line, sizeof line, f))
			{
				std::string s = line;
				while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
				std::vector<std::string> p;
				size_t a = 0;
				for (;;) { size_t b = s.find('\t', a); p.push_back(s.substr(a, b == std::string::npos ? b : b - a)); if (b == std::string::npos) break; a = b + 1; }
				if (p.size() != 11 || p[0] != "t1") continue;
				Track t;
				t.path = p[1]; t.mtime = atol(p[2].c_str()); t.size = atol(p[3].c_str());
				t.title = p[4]; t.artist = p[5]; t.album = p[6]; t.no = atoi(p[7].c_str()); t.disc = atoi(p[8].c_str());
				t.dur = atof(p[9].c_str()); t.year = atoi(p[10].c_str());
				out[t.path] = t;
			}
			fclose(f);
		}
		void saveCache(const std::vector<Track>& ts)
		{
			std::string tmp = cachePath() + ".tmp";
			FILE* f = fopen(tmp.c_str(), "w");
			if (!f) return;
			for (const Track& t : ts)
				fprintf(f, "t1\t%s\t%ld\t%ld\t%s\t%s\t%s\t%d\t%d\t%.2f\t%d\n", clean(t.path).c_str(), t.mtime, t.size, t.title.c_str(), t.artist.c_str(), t.album.c_str(), t.no, t.disc, t.dur, t.year);
			if (fclose(f) == 0) rename(tmp.c_str(), cachePath().c_str());
		}
		void scanLoop()
		{
			std::map<std::string, Track> cache;
			loadCache(cache);
			std::vector<std::string> files;
			walk(musicDir, files, 0);
			scanTotal = (int)files.size();
			// show the cached view at once; the walk and any re-probing follow
			if (!cache.empty())
			{
				std::vector<Track> early;
				for (const std::string& p : files) { auto it = cache.find(p); if (it != cache.end()) early.push_back(it->second); }
				if (!early.empty()) { std::lock_guard<std::mutex> lk(libM); pendingLib = buildLibrary(std::move(early)); }
			}
			std::vector<Track> all;
			bool changed = false;
			for (const std::string& p : files)
			{
				if (quitting) return;
				struct stat st;
				if (stat(p.c_str(), &st) != 0) continue;
				auto it = cache.find(p);
				if (it != cache.end() && it->second.mtime == (long)st.st_mtime && it->second.size == (long)st.st_size) all.push_back(it->second);
				else
				{
					Track t;
					if (!probe(p, t)) continue;
					t.mtime = (long)st.st_mtime; t.size = (long)st.st_size;
					all.push_back(t);
					changed = true;
				}
				scanSeen = (int)all.size();
			}
			if (changed || all.size() != cache.size()) saveCache(all);
			{
				std::lock_guard<std::mutex> lk(libM);
				pendingLib = buildLibrary(std::move(all));
			}
			scanning = false;
		}

		// ---- state -------------------------------------------------------------------------------------------------------
		std::string statePath() { return dataDir + "/state.txt"; }
		void saveState()
		{
			FILE* f = fopen(statePath().c_str(), "w");
			if (!f) return;
			fprintf(f, "path=%s\npos=%.1f\nshuffle=%d\nrepeat=%d\nviz=%d\nloud=%d\n", qi >= 0 ? cur.path.c_str() : "", qi >= 0 ? eng.pos() : 0.0, shuffle ? 1 : 0, repeat, vizStyle, loudness);
			fclose(f);
			lastSaved = nowSec();
		}
		void loadState()
		{
			FILE* f = fopen(statePath().c_str(), "r");
			if (!f) return;
			char line[2048];
			std::string path; double pos = 0;
			while (fgets(line, sizeof line, f))
			{
				std::string s = line;
				while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
				size_t e = s.find('=');
				if (e == std::string::npos) continue;
				std::string k = s.substr(0, e), v = s.substr(e + 1);
				if (k == "path") path = v; else if (k == "pos") pos = atof(v.c_str());
				else if (k == "shuffle") shuffle = v == "1"; else if (k == "repeat") repeat = std::max(0, std::min(2, atoi(v.c_str())));
				else if (k == "viz") vizStyle = std::max(0, std::min(2, atoi(v.c_str())));
				else if (k == "loud") { loudness = std::max(0, std::min(2, atoi(v.c_str()))); eng.rgMode = loudness; }
			}
			fclose(f);
			if (!path.empty() && fileExists(path))
			{
				Track t;
				if (probe(path, t))
				{
					if (t.artist.empty()) t.artist = unknownArtist();
					queue = { t }; queueOrig = queue; qi = 0;
					startTrack(t, pos > 3 && pos < t.dur - 3 ? pos : 0, false);
				}
			}
		}

		// ---- playback ----------------------------------------------------------------------------------------------------
		void startTrack(const Track& t, double at, bool play)
		{
			cur = t; curDur = t.dur;
			lyrics = loadLyrics(t.path);
			if (audioOk) { eng.openTrack(t.path, at); eng.setPaused(!play); }
			playing = play && audioOk;
			CoverTex& c = cover(t.path, 512);
			if (c.tex || c.none) palTarget = c.pal;
			dirty = true;
			prefetch();
		}
		// ---- gapless: tell the engine which song follows, and follow it when it gets there ----
		int nextIdx()
		{
			if (qi < 0 || queue.empty()) return -1;
			if (repeat == 2) return qi;
			int n = (int)queue.size();
			for (int k = 1; k <= n; k++)
			{
				int i = qi + k;
				if (i >= n) { if (repeat != 1) return -1; i %= n; }
				if (!badPaths.count(queue[(size_t)i].path)) return i;
			}
			return -1;
		}
		void prefetch()
		{
			pfIdx = nextIdx();
			if (audioOk) eng.setNext(pfIdx >= 0 ? queue[(size_t)pfIdx].path : std::string());
		}
		void pollGapless()
		{
			if (!audioOk) return;
			if (eng.nextFailed.exchange(0) > 0)
			{
				std::string fp = eng.takeFailed();
				badPaths.insert(fp);
				std::string name = clean(stem(fp));
				for (const Track& t : queue) if (t.path == fp) name = t.title;
				say("Skipped: " + name);
				prefetch();
			}
			int n = eng.switched.exchange(0);
			for (int k = 0; k < n && qi >= 0; k++)
			{
				if (pfIdx < 0 || pfIdx >= (int)queue.size()) break;
				qi = pfIdx; cur = queue[(size_t)qi]; curDur = cur.dur;
				lyrics = loadLyrics(cur.path);
				CoverTex& c = cover(cur.path, 512);
				if (c.tex || c.none) palTarget = c.pal;
				dirty = true;
				pfIdx = nextIdx();
				saveState();
			}
		}
		void playQueue(std::vector<Track> q, int start)
		{
			if (q.empty()) return;
			queueOrig = q;
			queue = q;
			qi = start;
			if (shuffle) reshuffle();
			startTrack(queue[(size_t)qi], 0, true);
			saveState();
		}
		void reshuffle()
		{
			if (queue.empty()) return;
			Track now = queue[(size_t)std::max(0, qi)];
			std::vector<Track> rest;
			for (const Track& t : queueOrig) if (t.path != now.path) rest.push_back(t);
			static std::mt19937 rng((unsigned)time(nullptr));
			std::shuffle(rest.begin(), rest.end(), rng);
			queue.clear(); queue.push_back(now);
			queue.insert(queue.end(), rest.begin(), rest.end());
			qi = 0;
		}
		void toggleShuffle()
		{
			shuffle = !shuffle;
			if (qi >= 0)
			{
				if (shuffle) reshuffle();
				else
				{
					queue = queueOrig; qi = 0;
					for (size_t i = 0; i < queue.size(); i++) if (queue[i].path == cur.path) qi = (int)i;
				}
			}
			prefetch();
			say(shuffle ? "Shuffle on" : "Shuffle off");
		}
		void cycleLoudness()
		{
			loudness = (loudness + 1) % 3;
			eng.rgMode = loudness;
			say(loudness == 0 ? "Loudness: off" : loudness == 1 ? "Loudness: track" : "Loudness: album");
		}
		void cycleRepeat()
		{
			repeat = (repeat + 1) % 3;
			prefetch();
			say(repeat == 0 ? "Repeat off" : repeat == 1 ? "Repeat all" : "Repeat one");
		}
		void togglePlay()
		{
			if (qi < 0) return;
			if (!audioOk) { say("No sound device"); return; }
			playing = !playing;
			eng.setPaused(!playing);
			if (!playing) saveState();
			dirty = true;
		}
		void advance(bool autoEnd)
		{
			if (qi < 0) return;
			if (autoEnd)	// the song ended with nothing prefetched (or the prefetch failed)
			{
				int n = nextIdx();
				if (n >= 0) { qi = n; startTrack(queue[(size_t)qi], 0, true); }
				else { playing = false; eng.setPaused(true); startTrack(queue[(size_t)qi], 0, false); }	// the queue ran out: cued at the top of the last song
				saveState();
				return;
			}
			if (qi + 1 < (int)queue.size()) { qi++; startTrack(queue[(size_t)qi], 0, true); }
			else { qi = 0; startTrack(queue[0], 0, playing); }
			saveState();
		}
		void previous()
		{
			if (qi < 0) return;
			if (eng.pos() > 3.0 || qi == 0) { eng.seek(0); return; }
			qi--; startTrack(queue[(size_t)qi], 0, playing);
		}
		void say(const std::string& s) { toast = s; toastUntil = nowSec() + 2.2; dirty = true; }
		void cycleSleep()
		{
			static const int opts[] = { 0, 15, 30, 60 };
			int i = 0;
			for (int k = 0; k < 4; k++) if (opts[k] == sleepMin) i = k;
			sleepMin = opts[(i + 1) % 4];
			sleepFading = false; eng.gain = 1.f;
			if (sleepMin) { sleepAt = nowSec() + sleepMin * 60.0; say("Sleep in " + std::to_string(sleepMin) + " min"); }
			else { sleepAt = 0; say("Sleep timer off"); }
		}

		// ---- per-loop work (runs whether or not the panel is drawn) --------------------------------------------------------
		void tick(double now)
		{
			collectCovers();
			{
				std::lock_guard<std::mutex> lk(libM);
				if (pendingLib) { lib = std::move(pendingLib); sel = std::min(sel, (int)lib->albums.size()); dirty = true; }
			}
			pollGapless();
			eng.tickAudio();
			if (audioOk && eng.ended.exchange(false) && playing) advance(true);
			if (sleepAt > 0)
			{
				double left = sleepAt - now;
				if (left <= 0)
				{
					sleepAt = 0; sleepMin = 0; sleepFading = false;
					if (playing) { playing = false; eng.setPaused(true); saveState(); }
					eng.gain = 1.f;
					say("Sleep timer ended");
				}
				else if (left < 30.0) { sleepFading = true; eng.gain = (float)(left / 30.0); }
			}
			if (qi >= 0)
			{
				CoverTex& c = cover(cur.path, 512);
				if ((c.tex || c.none) && (palTarget.a.r != c.pal.a.r || palTarget.a.g != c.pal.a.g || palTarget.a.b != c.pal.a.b || palTarget.valid != c.pal.valid)) palTarget = c.pal;
			}
			if (playing && now - lastSaved > 15) saveState();
			if (now - screenCheck > 0.5)
			{
				screenCheck = now;
				bool off = access("/run/banana/screen-off", F_OK) == 0;
				if (off != screenOff) { screenOff = off; dirty = true; }
			}
			// held d-pad: a slow start, then a steady repeat
			for (int b = UP; b <= RIGHT; b++)
				if (held[b] && now >= heldNext[b]) { heldNext[b] = now + 0.085; press((Btn)b, true); }
			if (now > toastUntil && !toast.empty()) { toast.clear(); dirty = true; }
		}
		bool animating()
		{
			if (screenOff) return false;
			if (playing && screen == Screen::NOW) return true;
			if (eng.fading()) return true;
			auto close = [](SDL_Color a, SDL_Color b) { return std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b) < 6; };
			if (!close(accA, palTarget.a) || !close(accB, palTarget.b)) return true;
			for (int i = 0; i < BANDS; i++) if (bars[i] > 0.01f) return true;
			return !toast.empty();
		}

		// ---- input -------------------------------------------------------------------------------------------------------
		int gridCols() { return 4; }
		int tileSize() { return (int)((cw - 8 * u - (gridCols() - 1) * 2 * u) / gridCols()); }
		int gridTop() { return (int)(u * 15); }
		int gridRows() { int rowH = tileSize() + (int)(2 * u); return std::max(1, (int)((H - gridTop() - 17 * u) / rowH)); }
		int songRowH() { return (int)(u * 7.4f); }
		int songRows() { return std::max(1, (int)((H - gridTop() - 12 * u) / songRowH())); }
		int albumRowH() { return (int)(u * 5.6f); }
		int albumRows() { return std::max(1, (int)((H - (u * 31)) / albumRowH())); }
		int albumCount() { return lib ? (int)lib->albums.size() + 1 : 1; }
		void keepVisible(int s, int rows, int& sc, int total) { if (s < sc) sc = s; if (s >= sc + rows) sc = s - rows + 1; sc = std::max(0, std::min(sc, std::max(0, total - rows))); }

		std::vector<Track> albumTracks(const Album& a) { std::vector<Track> q; for (int i : a.tracks) q.push_back(lib->tracks[(size_t)i]); return q; }

		void press(Btn b, bool repeating = false)
		{
			dirty = true;
			pollGapless();
			if (b == BSELECT) { cycleSleep(); return; }
			if (b == BX) { toggleShuffle(); return; }
			if (b == BY) { cycleRepeat(); return; }
			if (screen == Screen::NOW)
			{
				switch (b)
				{
				case BA: togglePlay(); break;
				case BB: screen = albumIdx >= 0 && lib ? Screen::ALBUM : Screen::LIB; break;
				case BSTART: cycleLoudness(); break;
				case BL: previous(); break;
				case BR: advance(false); break;
				case LEFT: if (qi >= 0) eng.seek(std::max(0.0, eng.pos() - (repeating ? 6.0 : 10.0))); break;
				case RIGHT: if (qi >= 0) eng.seek(std::min(std::max(0.0, curDur - 1.0), eng.pos() + (repeating ? 6.0 : 10.0))); break;
				case UP: vizStyle = (vizStyle + 2) % 3; break;
				case DOWN: vizStyle = (vizStyle + 1) % 3; break;
				default: break;
				}
				return;
			}
			if (!lib) { if (b == BB) quitOrArm(); return; }
			if (screen == Screen::LIB)
			{
				int total = tab == 0 ? albumCount() : (int)lib->tracks.size();
				int cols = tab == 0 ? gridCols() : 1;
				switch (b)
				{
				case LEFT: if (sel % cols) sel--; break;
				case RIGHT: if (sel % cols < cols - 1 && sel + 1 < total) sel++; break;
				case UP: if (sel - cols >= 0) sel -= cols; break;
				case DOWN: if (sel + cols < total) sel += cols; else if (sel / cols < (total - 1) / cols) sel = total - 1; break;
				case BL: case BR: tab = 1 - tab; sel = 0; scroll = 0; break;
				case BSTART: if (qi >= 0) screen = Screen::NOW; break;
				case BB: quitOrArm(); break;
				case BA:
					if (tab == 0)
					{
						if (sel == 0) shuffleEverything();
						else { albumIdx = sel - 1; aSel = 0; aScroll = 0; screen = Screen::ALBUM; }
					}
					else if (!lib->tracks.empty()) { playQueue(lib->tracks, sel); screen = Screen::NOW; }
					break;
				default: break;
				}
				keepVisible(tab == 0 ? sel / cols : sel, tab == 0 ? gridRows() : songRows(), scroll, tab == 0 ? (total + cols - 1) / cols : total);
				return;
			}
			// album page
			if (albumIdx < 0 || albumIdx >= (int)lib->albums.size()) { screen = Screen::LIB; return; }
			const Album& a = lib->albums[(size_t)albumIdx];
			int n = (int)a.tracks.size();
			switch (b)
			{
			case UP: if (aSel > 0) aSel--; break;
			case DOWN: if (aSel + 1 < n) aSel++; break;
			case BA: playQueue(albumTracks(a), aSel); screen = Screen::NOW; break;
			case BB: screen = Screen::LIB; break;
			case BSTART: if (qi >= 0) screen = Screen::NOW; break;
			case BL: albumIdx = (albumIdx + (int)lib->albums.size() - 1) % (int)lib->albums.size(); aSel = 0; aScroll = 0; break;
			case BR: albumIdx = (albumIdx + 1) % (int)lib->albums.size(); aSel = 0; aScroll = 0; break;
			default: break;
			}
			if (albumIdx >= 0 && albumIdx < (int)lib->albums.size())
				keepVisible(aSel, albumRows(), aScroll, (int)lib->albums[(size_t)albumIdx].tracks.size());
		}
		void quitOrArm()
		{
			if (!playing && qi < 0) { quit = true; return; }
			if (nowSec() < quitArmUntil) { quit = true; return; }
			quitArmUntil = nowSec() + 2.0;
			say(playing ? "Press B again to quit. Music stops." : "Press B again to quit");
		}
		void shuffleEverything()
		{
			if (!lib || lib->tracks.empty()) return;
			bool was = shuffle;
			shuffle = true;
			queueOrig = lib->tracks; queue = queueOrig; qi = 0;
			static std::mt19937 rng((unsigned)time(nullptr) ^ 0x5bd1e995u);
			qi = (int)(rng() % queue.size());
			cur = queue[(size_t)qi];
			reshuffle();
			startTrack(queue[(size_t)qi], 0, true);
			screen = Screen::NOW;
			if (!was) say("Shuffle on");
		}
		void onButton(Btn b, bool down)
		{
			double now = nowSec();
			if (b <= RIGHT)
			{
				held[b] = down;
				if (down) { heldAt[b] = now; heldNext[b] = now + 0.35; }
				if (!down) return;
			}
			else if (!down) return;
			press(b);
		}
		void handle(const SDL_Event& e)
		{
			if (e.type == SDL_QUIT) quit = true;
			else if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
			else if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP)
			{
				bool d = e.type == SDL_CONTROLLERBUTTONDOWN;
				switch (e.cbutton.button)
				{
				case SDL_CONTROLLER_BUTTON_DPAD_UP: onButton(UP, d); break;
				case SDL_CONTROLLER_BUTTON_DPAD_DOWN: onButton(DOWN, d); break;
				case SDL_CONTROLLER_BUTTON_DPAD_LEFT: onButton(LEFT, d); break;
				case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: onButton(RIGHT, d); break;
				case SDL_CONTROLLER_BUTTON_A: onButton(BA, d); break;
				case SDL_CONTROLLER_BUTTON_B: onButton(BB, d); break;
				case SDL_CONTROLLER_BUTTON_X: onButton(BX, d); break;
				case SDL_CONTROLLER_BUTTON_Y: onButton(BY, d); break;
				case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: onButton(BL, d); break;
				case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: onButton(BR, d); break;
				case SDL_CONTROLLER_BUTTON_START: onButton(BSTART, d); break;
				case SDL_CONTROLLER_BUTTON_BACK: onButton(BSELECT, d); break;
				default: break;
				}
			}
			else if ((e.type == SDL_KEYDOWN && !e.key.repeat) || e.type == SDL_KEYUP)	// the host's stand-in pad
			{
				bool d = e.type == SDL_KEYDOWN;
				switch (e.key.keysym.sym)
				{
				case SDLK_UP: onButton(UP, d); break;
				case SDLK_DOWN: onButton(DOWN, d); break;
				case SDLK_LEFT: onButton(LEFT, d); break;
				case SDLK_RIGHT: onButton(RIGHT, d); break;
				case SDLK_RETURN: onButton(BA, d); break;
				case SDLK_ESCAPE: onButton(BB, d); break;
				case SDLK_x: onButton(BX, d); break;
				case SDLK_y: onButton(BY, d); break;
				case SDLK_q: onButton(BL, d); break;
				case SDLK_e: onButton(BR, d); break;
				case SDLK_TAB: onButton(BSTART, d); break;
				case SDLK_s: onButton(BSELECT, d); break;
				default: break;
				}
			}
			else if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_EXPOSED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED))
			{
				if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) layout();
				dirty = true;
			}
		}

		// ---- the visualizer -----------------------------------------------------------------------------------------------
		void analyse(double dt)
		{
			float in[FFT_N], re[FFT_N], im[FFT_N];
			unsigned wp = eng.vizPos.load(std::memory_order_acquire);
			bool live = playing && audioOk;
			for (int i = 0; i < FFT_N; i++) in[i] = live ? eng.viz[(wp - FFT_N + (unsigned)i) & (VIZ_N - 1)] : 0.f;
			fft.run(in, re, im);
			float fall = (float)std::pow(0.0005, dt);	// bars fall to nothing in about a third of a second
			for (int b = 0; b < BANDS; b++)
			{
				int lo = (int)std::pow(300.0, (double)b / BANDS), hi = std::max(lo + 1, (int)std::pow(300.0, (double)(b + 1) / BANDS));
				float m = 0;
				for (int k = lo + 1; k <= hi + 1 && k < FFT_N / 2; k++) m = std::max(m, std::sqrt(re[k] * re[k] + im[k] * im[k]));
				m /= (FFT_N / 4.f);
				float v = (20.f * std::log10(m + 1e-6f) + 62.f) / 56.f;
				v = std::max(0.f, std::min(1.f, v));
				bars[b] = v > bars[b] ? v : bars[b] * fall;
				if (bars[b] >= peaks[b]) peaks[b] = bars[b]; else peaks[b] = std::max(bars[b], peaks[b] - (float)dt * 0.55f);
			}
		}
		void drawViz(int x, int y, int w, int h)
		{
			SDL_Color top = accB, low = accA;
			if (vizStyle == 0)
			{
				float gap = u * 0.5f, bw = (w - gap * (BANDS - 1)) / BANDS;
				int segH = std::max(2, (int)(u * 1.3f)), segGap = std::max(1, (int)(u * 0.35f));
				for (int b = 0; b < BANDS; b++)
				{
					int bx = x + (int)(b * (bw + gap)), bwi = (int)bw;
					int n = (int)(bars[b] * h / (segH + segGap));
					for (int s = 0; s < n; s++)
						fill(bx, y + h - (s + 1) * (segH + segGap), bwi, segH, mix(low, top, (float)s * (segH + segGap) / h));
					int pk = (int)(peaks[b] * h / (segH + segGap));
					if (pk > n) fill(bx, y + h - (pk + 1) * (segH + segGap), bwi, segH, CREAM);
				}
			}
			else if (vizStyle == 1)
			{
				float gap = u * 0.35f, bw = (w - gap * (BANDS * 2 - 1)) / (BANDS * 2);
				int mid = y + h / 2;
				for (int b = 0; b < BANDS; b++)
				{
					int hh = std::max(2, (int)(bars[b] * h / 2));
					SDL_Color c = mix(low, top, (float)b / BANDS);
					int l = x + w / 2 - (int)((b + 1) * (bw + gap)), r = x + w / 2 + (int)(b * (bw + gap));
					fill(l, mid - hh, (int)bw, hh * 2, c);
					fill(r, mid - hh, (int)bw, hh * 2, c);
				}
			}
			else
			{
				unsigned wp = eng.vizPos.load(std::memory_order_acquire);
				bool live = playing && audioOk;
				int n = std::min(w, 480);
				int px = 0, py = y + h / 2;
				for (int pass = 0; pass < 3; pass++)
				{
					SDL_Color c = pass == 0 ? mix(accA, INK, 0.5f) : pass == 1 ? accA : mix(accB, CREAM, 0.4f);
					SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
					int off = pass == 0 ? (int)(u * 0.9f) : 0;
					for (int i = 0; i < n; i++)
					{
						float s = live ? eng.viz[(wp - 2048 + (unsigned)(i * 4)) & (VIZ_N - 1)] : 0.f;
						int qx = x + i * w / n, qy = y + h / 2 - (int)(std::max(-1.f, std::min(1.f, s * 2.6f)) * h * 0.46f) + off;
						if (i) { SDL_RenderDrawLine(ren, px, py, qx, qy); SDL_RenderDrawLine(ren, px, py + 1, qx, qy + 1); }
						px = qx; py = qy;
					}
				}
			}
		}

		// ---- screens -----------------------------------------------------------------------------------------------------
		void background()
		{
			float h, s, v; toHsv(accA, h, s, v);
			SDL_Color top = hsv(h, 0.55f, 0.20f), bot = hsv(h + 20.f, 0.6f, 0.07f);
			gradient(0, 0, W, H, top, bot);
		}
		void miniPlayer()
		{
			if (qi < 0) return;
			int y = H - (int)(u * 9.5f), h = (int)(u * 7.5f), x = cx + (int)(u * 4), w = cw - (int)(u * 8);
			fill(x, y, w, h, INK, 200);
			frame(x, y, w, h, std::max(1, (int)(u * 0.35f)), accA);
			int cs = h - (int)(u * 1.6f);
			drawCover(cur.path, 192, x + (int)(u * 0.8f), y + (int)(u * 0.8f), cs, accB, false);
			int tx = x + cs + (int)(u * 2.2f);
			drawText(fMed, cur.title, CREAM, tx, y + (int)(u * 0.9f), w - cs - (int)(u * 8));
			drawText(fTiny, cur.artist, mix(CREAM, INK, 0.35f), tx, y + (int)(u * 4.3f), w - cs - (int)(u * 8));
			if (playing) triangle((float)(x + w - u * 3.4f), (float)(y + h / 2), u * 2.2f, accA);
			else { fill(x + w - (int)(u * 3.4f), y + h / 2 - (int)(u * 1.1f), (int)(u * 0.8f), (int)(u * 2.2f), CREAM); fill(x + w - (int)(u * 2.2f), y + h / 2 - (int)(u * 1.1f), (int)(u * 0.8f), (int)(u * 2.2f), CREAM); }
			double p = curDur > 0 ? eng.pos() / curDur : 0;
			fill(x, y + h - (int)(u * 0.5f), (int)(w * std::max(0.0, std::min(1.0, p))), (int)(u * 0.5f), accA);
		}
		void header(const char* title)
		{
			drawText(fHuge, title, CREAM, cx + (int)(u * 4), (int)(u * 2.4f));
			TTF_Font* f = fMed;
			int x = cx + cw - (int)(u * 4);
			const char* names[2] = { "Albums", "Songs" };
			for (int i = 1; i >= 0; i--)
			{
				Tx t = text(f, names[i], i == tab ? CREAM : mix(CREAM, INK, 0.55f));
				x -= t.w;
				if (t.t) { SDL_Rect d = { x, (int)(u * 5.2f), t.w, t.h }; SDL_RenderCopy(ren, t.t, nullptr, &d); }
				if (i == tab) fill(x, (int)(u * 5.2f) + t.h + (int)(u * 0.5f), t.w, std::max(2, (int)(u * 0.55f)), accA);
				x -= (int)(u * 3.2f);
			}
		}
		void drawLibrary()
		{
			header("Jukebox");
			if (!lib)
			{
				drawText(fMed, scanning ? "Reading your music" : "No music found", CREAM, cx + cw / 2, H / 2 - (int)(u * 4), 0, 0, 1);
				if (scanning && scanTotal > 0) drawText(fSmall, std::to_string((int)scanSeen) + " of " + std::to_string((int)scanTotal) + " songs", mix(CREAM, INK, 0.4f), cx + cw / 2, H / 2 + (int)(u * 1), 0, 0, 1);
				return;
			}
			if (lib->tracks.empty())
			{
				drawText(fBig, "No music yet", CREAM, cx + cw / 2, H / 2 - (int)(u * 9), cw - (int)(u * 10), 0, 1);
				drawText(fSmall, "Copy songs to " + musicDir + ", folders are fine,", mix(CREAM, INK, 0.3f), cx + cw / 2, H / 2 - (int)(u * 1), cw - (int)(u * 10), 0, 1);
				drawText(fSmall, "then open Jukebox again.", mix(CREAM, INK, 0.3f), cx + cw / 2, H / 2 + (int)(u * 3), cw - (int)(u * 10), 0, 1);
				return;
			}
			if (tab == 0) drawGrid(); else drawSongs();
			miniPlayer();
			if (qi < 0) drawText(fTiny, "A open   L1 R1 switch   X shuffle   Y repeat   Select sleep", mix(CREAM, INK, 0.45f), cx + cw / 2, H - (int)(u * 5.2f), cw, 0, 1);
		}
		void drawGrid()
		{
			int ts = tileSize(), gap = (int)(2 * u), x0 = cx + (int)(4 * u), rows = gridRows();
			int total = albumCount();
			for (int r = 0; r < rows; r++)
				for (int c = 0; c < gridCols(); c++)
				{
					int i = (scroll + r) * gridCols() + c;
					if (i >= total) break;
					int x = x0 + c * (ts + gap), y = gridTop() + r * (ts + gap);
					bool on = i == sel;
					if (i == 0)
					{
						fill(x, y, ts, ts, BANANA);
						drawText(fBig, "Shuffle", INK, x + ts / 2, y + ts / 2 - (int)(u * 5), ts - (int)u, 0, 1);
						drawText(fBig, "all", INK, x + ts / 2, y + ts / 2 + (int)(u * 0.5f), ts - (int)u, 0, 1);
					}
					else drawCover(lib->albums[(size_t)i - 1].artPath, 192, x, y, ts, accB, on);
					if (on) frame(x - (int)(u * 0.6f), y - (int)(u * 0.6f), ts + (int)(u * 1.2f), ts + (int)(u * 1.2f), std::max(2, (int)(u * 0.6f)), CREAM);
				}
			// the caption for the selected tile
			int cy = gridTop() + rows * (ts + gap) + (int)(u * 0.5f);
			if (sel == 0) { drawText(fMed, "Shuffle everything", CREAM, x0, cy, cw - (int)(u * 8)); drawText(fSmall, std::to_string(lib->tracks.size()) + " songs", mix(CREAM, INK, 0.35f), x0, cy + (int)(u * 4.3f)); }
			else
			{
				const Album& a = lib->albums[(size_t)sel - 1];
				drawText(titleFont(a.name) == fBig ? fMed : fMed, a.name, CREAM, x0, cy, cw - (int)(u * 8));
				std::string sub = a.artist + (a.year ? ", " + std::to_string(a.year) : "") + ", " + std::to_string(a.tracks.size()) + (a.tracks.size() == 1 ? " song" : " songs");
				drawText(fSmall, sub, mix(CREAM, INK, 0.3f), x0, cy + (int)(u * 4.3f), cw - (int)(u * 8));
			}
		}
		void drawSongs()
		{
			int rh = songRowH(), rows = songRows(), x0 = cx + (int)(4 * u), w = cw - (int)(8 * u);
			for (int r = 0; r < rows; r++)
			{
				int i = scroll + r;
				if (i >= (int)lib->tracks.size()) break;
				const Track& t = lib->tracks[(size_t)i];
				int y = gridTop() + r * rh;
				bool on = i == sel, now = qi >= 0 && cur.path == t.path;
				if (on) { fill(x0, y, w, rh - (int)(u * 0.5f), accA, 70); fill(x0, y, (int)(u * 0.7f), rh - (int)(u * 0.5f), accA); }
				int tx = x0 + (int)(u * 2.2f);
				if (now) triangle((float)(x0 + u * 1.0f), (float)(y + rh / 2 - u * 0.3f), u * 1.6f, accB);
				drawText(fMed, t.title, CREAM, tx + (int)(u * 1.2f), y + (int)(u * 0.5f), w - (int)(u * 14));
				drawText(fTiny, t.artist, mix(CREAM, INK, 0.4f), tx + (int)(u * 1.2f), y + (int)(u * 4.2f), w - (int)(u * 14));
				if (t.dur > 0) drawText(fSmall, clock(t.dur), mix(CREAM, INK, 0.4f), x0 + w - (int)(u * 1), y + (int)(u * 1.6f), 0, 0, 2);
			}
		}
		void drawAlbum()
		{
			if (!lib || albumIdx < 0 || albumIdx >= (int)lib->albums.size()) return;
			const Album& a = lib->albums[(size_t)albumIdx];
			int x0 = cx + (int)(4 * u), cs = (int)(u * 20);
			drawCover(a.artPath, 192, x0, (int)(u * 4), cs, accB, true);
			int tx = x0 + cs + (int)(u * 4), tw = cw - cs - (int)(u * 12);
			int y = (int)(u * 4);
			TTF_Font* tf = titleFont(a.name);
			y += drawText(tf, a.name, CREAM, tx, y, 0, tw);
			y += (int)(u * 0.8f);
			drawText(fMed, a.artist, accA, tx, y, tw);
			std::string sub = std::to_string(a.tracks.size()) + (a.tracks.size() == 1 ? " song" : " songs");
			if (a.year) sub = std::to_string(a.year) + ", " + sub;
			drawText(fSmall, sub, mix(CREAM, INK, 0.35f), tx, y + (int)(u * 4.6f), tw);
			int rh = albumRowH(), top = (int)(u * 28), rows = albumRows(), w = cw - (int)(8 * u);
			for (int r = 0; r < rows; r++)
			{
				int i = aScroll + r;
				if (i >= (int)a.tracks.size()) break;
				const Track& t = lib->tracks[(size_t)a.tracks[(size_t)i]];
				int yy = top + r * rh;
				bool on = i == aSel, now = qi >= 0 && cur.path == t.path;
				if (on) { fill(x0, yy, w, rh - (int)(u * 0.4f), accA, 70); fill(x0, yy, (int)(u * 0.7f), rh - (int)(u * 0.4f), accA); }
				if (now) triangle((float)(x0 + u * 1.3f), (float)(yy + rh / 2 - u * 0.2f), u * 1.6f, accB);
				else drawText(fSmall, t.no > 0 ? std::to_string(t.no) : std::to_string(i + 1), mix(CREAM, INK, 0.45f), x0 + (int)(u * 3.4f), yy + (int)(u * 1.1f), 0, 0, 2);
				drawText(fMed, t.title, CREAM, x0 + (int)(u * 5.4f), yy + (int)(u * 0.9f), w - (int)(u * 16));
				if (t.dur > 0) drawText(fSmall, clock(t.dur), mix(CREAM, INK, 0.4f), x0 + w - (int)u, yy + (int)(u * 1.2f), 0, 0, 2);
			}
			miniPlayer();
		}
		int currentLyric(double pos)
		{
			int k = -1;
			for (size_t i = 0; i < lyrics.size(); i++) { if (lyrics[i].t <= pos + 0.15) k = (int)i; else break; }
			return k;
		}
		void drawNow()
		{
			if (qi < 0) { drawText(fMed, "Nothing playing", CREAM, cx + cw / 2, H / 2, 0, 0, 1); return; }
			double pos = eng.pos();
			int m = (int)(u * 4);
			int cs = (int)(std::min(cw * 0.42f, H * 0.40f));
			int x0 = cx + m, y0 = m;
			drawCover(cur.path, 512, x0, y0, cs, accB, true);
			int tx = x0 + cs + (int)(u * 4.5f), tw = cx + cw - m - tx;
			int ty = y0;
			TTF_Font* tf = titleFont(cur.title);
			SDL_Rect clip = { tx, ty, tw, (int)(cs * 0.64f) };
			SDL_RenderSetClipRect(ren, &clip);
			ty += drawText(tf, cur.title, CREAM, tx, ty, 0, tw);
			SDL_RenderSetClipRect(ren, nullptr);
			ty = std::max(ty, y0 + (int)(u * 8));
			ty = std::min(ty, y0 + (int)(cs * 0.64f)) + (int)(u * 0.8f);
			ty += drawText(fMed, cur.artist, accA, tx, ty, tw) + (int)(u * 0.6f);
			if (!cur.album.empty()) drawText(fSmall, cur.album, mix(CREAM, INK, 0.3f), tx, ty, tw);
			// lyrics when the song has them, else the visualizer gets the room
			int bandTop = y0 + cs + (int)(u * 4), barY = H - (int)(u * 11);
			bool hasLyr = !lyrics.empty();
			int vizTop = bandTop;
			if (hasLyr)
			{
				int k = currentLyric(pos);
				int ly = bandTop;
				auto line = [&](int idx, TTF_Font* f, SDL_Color c)
				{
					if (idx < 0 || idx >= (int)lyrics.size() || lyrics[(size_t)idx].s.empty()) return 0;
					return drawText(f, lyrics[(size_t)idx].s, c, x0, ly, 0, cw - 2 * m);
				};
				ly += line(k - 1, fSmall, mix(CREAM, INK, 0.55f)) + (int)(u * 0.6f);
				TTF_Font* lf = (k >= 0 && lyrics[(size_t)k].s.size() > 36) ? fMed : iBig;
				ly += line(k, lf, CREAM) + (int)(u * 0.6f);
				line(k + 1, fSmall, mix(CREAM, INK, 0.55f));
				vizTop = bandTop + (int)(u * 15);
			}
			int vh = barY - (int)(u * 2) - vizTop;
			if (vh > (int)(u * 6)) drawViz(x0, vizTop, cw - 2 * m, vh);
			// the progress "groove"
			int by = barY + (int)(u * 1.6f), bh = std::max(3, (int)(u * 1.1f));
			int bx = x0 + (int)(u * 3.4f), bw = cw - 2 * m - (int)(u * 3.4f);
			if (playing) { fill(x0, by - (int)(u * 0.9f), (int)(u * 0.9f), (int)(u * 3), CREAM); fill(x0 + (int)(u * 1.7f), by - (int)(u * 0.9f), (int)(u * 0.9f), (int)(u * 3), CREAM); }
			else triangle((float)x0, (float)(by + bh / 2), u * 2.6f, accA);
			fill(bx, by, bw, bh, CREAM, 50);
			double frac = curDur > 0 ? std::max(0.0, std::min(1.0, pos / curDur)) : 0;
			fill(bx, by, (int)(bw * frac), bh, accA);
			fill(bx + (int)(bw * frac) - (int)(u * 0.4f), by - (int)(u * 0.7f), (int)(u * 0.8f), bh + (int)(u * 1.4f), CREAM);
			drawText(fSmall, clock(pos), CREAM, bx, by + bh + (int)(u * 1.2f));
			drawText(fSmall, curDur > 0 ? clock(curDur) : "", mix(CREAM, INK, 0.4f), bx + bw, by + bh + (int)(u * 1.2f), 0, 0, 2);
			std::string st;
			if (shuffle) st += "Shuffle   ";
			if (repeat == 1) st += "Repeat all   "; else if (repeat == 2) st += "Repeat one   ";
			if (sleepAt > 0) st += "Sleep in " + std::to_string((int)std::ceil((sleepAt - nowSec()) / 60.0)) + " min";
			if (!st.empty()) drawText(fTiny, st, accB, cx + cw / 2, by + bh + (int)(u * 1.7f), 0, 0, 1);
		}
		void render()
		{
			double now = nowSec();
			double dt = lastFrame > 0 ? std::min(0.2, now - lastFrame) : 0.033;
			lastFrame = now;
			// ease the accents toward the cover's
			float k = 1.f - (float)std::pow(0.0004, dt);
			accA = mix(accA, palTarget.a, k); accB = mix(accB, palTarget.b, k);
			analyse(dt);
			background();
			if (screen == Screen::LIB) drawLibrary();
			else if (screen == Screen::ALBUM) drawAlbum();
			else drawNow();
			if (!toast.empty())
			{
				Tx t = text(fMed, toast, CREAM);
				int pw = t.w + (int)(u * 5), ph = t.h + (int)(u * 2.4f), x = cx + (cw - pw) / 2, y = H - (int)(u * 20);
				fill(x, y, pw, ph, INK, 235);
				frame(x, y, pw, ph, std::max(2, (int)(u * 0.4f)), accA);
				SDL_Rect d = { x + (pw - t.w) / 2, y + (ph - t.h) / 2, t.w, t.h };
				if (t.t) SDL_RenderCopy(ren, t.t, nullptr, &d);
			}
			SDL_RenderPresent(ren);
			dirty = false;
			lastSecDrawn = (int)eng.pos();
		}

		// ---- run ----------------------------------------------------------------------------------------------------------
		bool setup(bool headless)
		{
			musicDir = envOr("JUKEBOX_MUSIC", "/data/music");
			dataDir = envOr("BANANA_APP_DATA", "/data/banana/appdata/jukebox");
			fontDir = envOr("JUKEBOX_FONTS", "");
			std::string mk = "mkdir -p '" + dataDir + "' 2>/dev/null";
			if (system(mk.c_str()) != 0) logLine("cannot make " + dataDir);
			SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
			if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) { logLine(std::string("SDL: ") + SDL_GetError()); return false; }
			TTF_Init();
			IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
			for (int i = 0; i < SDL_NumJoysticks(); i++) if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); if (pad) break; }
			int ww = 1024, wh = 768;
			if (const char* sz = std::getenv("JUKEBOX_SIZE")) sscanf(sz, "%dx%d", &ww, &wh);
			win = SDL_CreateWindow("Jukebox", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh, headless ? SDL_WINDOW_SHOWN : (SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN));
			if (!win) { logLine(std::string("no window: ") + SDL_GetError()); return false; }
			ren = SDL_CreateRenderer(win, -1, headless ? SDL_RENDERER_SOFTWARE : (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC));
			if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
			if (!ren) { logLine(std::string("no renderer: ") + SDL_GetError()); return false; }
			SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
			layout();
			logLine("surface " + std::to_string(W) + "x" + std::to_string(H));
			if (!makeFonts()) { logLine(std::string("no fonts: ") + TTF_GetError()); return false; }
			audioOk = eng.init();
			scanTh = std::thread([this] { scanLoop(); });
			coverTh = std::thread([this] { coverLoop(); });
			return true;
		}
		void shutdown()
		{
			saveState();
			quitting = true;
			jobCv.notify_all();
			eng.closeTrack();
			if (scanTh.joinable()) scanTh.join();
			if (coverTh.joinable()) coverTh.join();
		}
		int run()
		{
			if (!setup(false)) return 1;
			signal(SIGTERM, onStopSignal); signal(SIGINT, onStopSignal); signal(SIGHUP, onStopSignal);
			loadState();
			if (qi >= 0) palTarget = cover(cur.path, 512).pal;
			while (!quit && !g_stop)
			{
				double now = nowSec();
				int timeout = screenOff ? 400 : animating() ? 30 : 250;
				SDL_Event e;
				if (SDL_WaitEventTimeout(&e, timeout)) { handle(e); while (SDL_PollEvent(&e)) handle(e); }
				now = nowSec();
				tick(now);
				if (screenOff) continue;
				if (qi >= 0 && (int)eng.pos() != lastSecDrawn && (playing || screen == Screen::NOW)) dirty = true;
				if (dirty || animating()) render();
			}
			shutdown();
			return 0;
		}

		// ---- host selftest: renders each screen to a PNG and checks the engine ---------------------------------------------
		bool shot(const std::string& dir, const char* name)
		{
			render();
			SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
			if (!s) return false;
			SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
			bool ok = IMG_SavePNG(s, (dir + "/" + name).c_str()) == 0;
			SDL_FreeSurface(s);
			return ok;
		}
		void settle(double sec)
		{
			double end = nowSec() + sec;
			while (nowSec() < end) { SDL_Event e; while (SDL_PollEvent(&e)) handle(e); tick(nowSec()); if (animating() || dirty) render(); SDL_Delay(15); }
		}
		int selftest(const std::string& dir)
		{
			if (!setup(true)) return 1;
			int fails = 0;
			auto check = [&](bool ok, const std::string& what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str()); if (!ok) fails++; };
			for (int i = 0; i < 400 && (scanning || !lib); i++) settle(0.05);
			check(lib && !lib->tracks.empty(), "library scanned: " + std::to_string(lib ? lib->tracks.size() : 0) + " songs, " + std::to_string(lib ? lib->albums.size() : 0) + " albums");
			settle(1.5);
			for (int i = 0; i < 20; i++) { render(); settle(0.1); }
			check(shot(dir, "1-albums.png"), "albums grid");
			press(RIGHT); press(RIGHT); settle(0.6);
			shot(dir, "1b-albums-sel.png");
			press(BL); settle(0.4); check(shot(dir, "2-songs.png"), "songs list");
			press(BL); sel = 1; scroll = 0;
			press(BA); settle(0.8); check(screen == Screen::ALBUM && shot(dir, "3-album.png"), "album page");
			press(BA); settle(0.2);
			check(screen == Screen::NOW && qi >= 0, "A on a song starts it: " + cur.title);
			if (audioOk) { settle(1.8); double p = eng.pos(); check(p > 0.8 && p < 3.0, "position advances: " + std::to_string(p)); }
			for (int s = 0; s < 3; s++) { vizStyle = s; settle(0.35); char n[32]; snprintf(n, sizeof n, "4-now-viz%d.png", s); check(shot(dir, n), n); }
			if (audioOk)
			{
				double a = eng.pos(); eng.seek(a + 4); settle(0.6);
				double b = eng.pos(); check(b > a + 3.0, "seek forward: " + std::to_string(a) + " -> " + std::to_string(b));
				int before = qi; eng.seek(std::max(0.0, curDur - 0.4)); settle(2.5);
				check(qi == before + 1 || (qi == 0 && before + 1 == (int)queue.size()), "track end advances the queue: " + std::to_string(before) + " -> " + std::to_string(qi));
				float mx = 0; for (int i = 0; i < BANDS; i++) mx = std::max(mx, bars[i]);
				check(mx > 0.05f, "visualizer sees the audio: peak " + std::to_string(mx));
				togglePlay(); settle(0.2); check(!playing, "pause");
				togglePlay(); settle(0.2); check(playing, "resume");
				sleepAt = nowSec() + 1.0; settle(1.6); check(!playing, "sleep timer pauses at zero");
			}
			if (lib && lib->albums.size() > 2)
			{
				albumIdx = 2; playQueue(albumTracks(lib->albums[2]), 0); screen = Screen::NOW; vizStyle = 0;
				if (audioOk) eng.seek(4.2);
				settle(1.5);
				check(!lyrics.empty() && currentLyric(eng.pos()) >= 1, "lyrics follow the song: line " + std::to_string(currentLyric(eng.pos())));
				check(shot(dir, "5-now-lyrics.png"), "now playing with lyrics");
			}

			// ---- engine, off the audio device: the callback is pulled by hand, so the output can be inspected ----
			{
				auto gapFile = [&](const char* n) { return dir + "/gap/" + n; };
				auto pump = [](Engine& e, size_t frames, std::vector<int16_t>& o)
				{
					std::vector<int16_t> chunk(4096);
					while (o.size() / 2 < frames)
					{
						for (int w = 0; w < 500; w++) { { std::lock_guard<std::mutex> lk(e.m); if (e.fill >= chunk.size() * 2 || e.eof.load()) break; } SDL_Delay(2); }
						Engine::callback(&e, (Uint8*)chunk.data(), (int)(chunk.size() * 2));
						o.insert(o.end(), chunk.begin(), chunk.end());
					}
				};
				auto peak = [](const std::vector<int16_t>& o, size_t a, size_t b)
				{
					int m = 0;
					for (size_t i = a * 2; i < std::min(b * 2, o.size()); i++) m = std::max(m, std::abs((int)o[i]));
					return m;
				};
				auto zeroRun = [](const std::vector<int16_t>& o, size_t a, size_t b)
				{
					size_t best = 0, run = 0;
					for (size_t i = a; i < std::min(b, o.size() / 2); i++) { if (o[2 * i] == 0 && o[2 * i + 1] == 0) best = std::max(best, ++run); else run = 0; }
					return best;
				};
				RG r; r.hasTrack = true; r.trackDb = 6; r.trackPeak = 0.5;
				check(std::fabs(rgLinear(r, 1) - 1.995f) < 0.01f, "ReplayGain: +6 dB boost allowed under the peak");
				r.trackPeak = 0.9; check(std::fabs(rgLinear(r, 1) - 1.111f) < 0.01f, "ReplayGain: boost stops at the peak");
				r.trackPeak = 0; check(rgLinear(r, 1) == 1.f, "ReplayGain: no boost past unity without a peak");
				r.trackDb = -6; check(std::fabs(rgLinear(r, 1) - 0.501f) < 0.01f, "ReplayGain: -6 dB cut");
				check(rgLinear(r, 0) == 1.f && rgLinear(RG(), 1) == 1.f, "ReplayGain: off, or untagged, leaves the level alone");
				r.hasAlbum = true; r.albumDb = -3;
				check(std::fabs(rgLinear(r, 2) - 0.708f) < 0.01f && std::fabs(rgLinear(r, 1) - 0.501f) < 0.01f, "ReplayGain: album mode uses the album gain");
				check(std::fabs(r128ToDb(-512) - 3.0) < 1e-9, "R128 gain: Q7.8 against -23 LUFS becomes ReplayGain's -18 LUFS");
				double dv = 0;
				check(parseDb("-6.54 dB", dv) && std::fabs(dv + 6.54) < 1e-9 && !parseDb("", dv) && !parseDb("loud", dv), "ReplayGain: tag text parses");

				{	// A (tagged -6 dB) then B (untagged): one unbroken stream, B at full level, A at half
					Engine g; g.initBuffers(); g.paused = false; g.setPaused(false);
					g.setNext(gapFile("b.flac")); g.openTrack(gapFile("a.flac"), 0);
					std::vector<int16_t> o; pump(g, 150000, o);
					size_t z = zeroRun(o, 2400, 142000);
					int pa = peak(o, 4800, 70000), pb = peak(o, 76000, 142000);
					check(z < 16, "gapless: no silence across the song change (longest zero run " + std::to_string(z) + " frames)");
					check(pa > 1300 && pa < 1600 && pb > 2750 && pb < 3050, "gapless: song gain follows the song (A " + std::to_string(pa) + ", B " + std::to_string(pb) + ")");
					double p = g.pos();
					check(g.switched.load() == 1 && p > 1.4 && p < 1.6, "gapless: engine reports the switch, position restarts (" + std::to_string(p) + " s)");
					g.closeTrack();
				}
				{	// the next song will not open: reported once; a replacement is picked up from the parked decoder
					Engine g; g.initBuffers(); g.paused = false; g.setPaused(false);
					g.setNext(gapFile("bad.flac")); g.openTrack(gapFile("a.flac"), 0);
					for (int w = 0; w < 500 && g.nextFailed.load() == 0; w++) SDL_Delay(2);
					check(g.nextFailed.load() == 1 && g.takeFailed() == gapFile("bad.flac"), "unreadable next song is reported");
					g.setNext(gapFile("b.flac"));
					std::vector<int16_t> o; pump(g, 150000, o);
					check(zeroRun(o, 2400, 142000) < 16 && peak(o, 76000, 142000) > 2750, "playback continues into the replacement song");
					g.closeTrack();
				}
				{	// seeking while the decoder is already on the next song goes back to this one
					Engine g; g.initBuffers(); g.paused = false; g.setPaused(false);
					g.setNext(gapFile("b.flac")); g.openTrack(gapFile("a.flac"), 0);
					for (int w = 0; w < 500; w++) { { std::lock_guard<std::mutex> lk(g.m); if (!g.markers.empty()) break; } SDL_Delay(2); }
					g.seek(0.5);
					std::vector<int16_t> o; pump(g, 12000, o);
					double p = g.pos();
					check(p > 0.7 && p < 0.85 && g.switched.load() == 0, "seek with a song prefetched stays in the playing song (" + std::to_string(p) + " s)");
					g.setNext("");	// and dropping the choice leaves nothing pending
					g.closeTrack();
				}
				{	// pause fades out, holds silent without eating the ring, resume fades in
					Engine g; g.initBuffers(); g.paused = false; g.rgMode = 0; g.setPaused(false);
					g.openTrack(gapFile("a.flac"), 0); g.setPaused(false);
					std::vector<int16_t> o; pump(g, 20000, o);
					check(peak(o, 15000, 20000) > 2750, "fades: full level while playing");
					o.clear(); g.setPaused(true); pump(g, 4096, o);
					int pf = peak(o, 0, 256), pe = peak(o, 3900, 4096);
					check(pf > 2000 && pe == 0, "fades: pause ramps to silence (" + std::to_string(pf) + " -> " + std::to_string(pe) + ")");
					size_t fillBefore; { std::lock_guard<std::mutex> lk(g.m); fillBefore = g.fill; }
					o.clear(); pump(g, 8192, o);
					size_t fillAfter; { std::lock_guard<std::mutex> lk(g.m); fillAfter = g.fill; }
					check(peak(o, 0, 8192) == 0 && fillBefore == fillAfter, "fades: paused output is silent and the song does not move");
					o.clear(); g.setPaused(false); pump(g, 6144, o);
					int p0 = peak(o, 0, 96), p1 = peak(o, 4400, 6144);
					check(p0 < 400 && p1 > 2750, "fades: resume ramps in (" + std::to_string(p0) + " -> " + std::to_string(p1) + ")");
					g.closeTrack();
				}
			}
			shutdown();
			printf("%d failures\n", fails);
			return fails ? 1 : 0;
		}
	};
}

int main()
{
	av_log_set_level(AV_LOG_ERROR);
	App app;
	if (const char* d = std::getenv("JUKEBOX_SELFTEST")) return app.selftest(d);
	return app.run();
}
