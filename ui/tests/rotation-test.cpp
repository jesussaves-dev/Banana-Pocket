#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../src/rotate.h"
#include "../src/turn.h"

// Phase 3C: the presenter's coordinate map must be the same turn the CPU renderer already uses (turn.h, rot 90),
// and the quad's corner UVs must interpolate to rotateUv() at every target pixel. No GPU or DRM here.
int main()
{
	int fails = 0;
	// 1. rotateSource agrees with turnRect on an asymmetric image (not square, like 1240x1080)
	{
		enum { W = 7, H = 5 };
		uint32_t src[H][W];
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W; x++)
				src[y][x] = (uint32_t)(y * 100 + x);
		std::vector<uint32_t> dst((size_t)W * H);
		banana::turnRect(&src[0][0], (uint8_t*)dst.data(), H * sizeof(uint32_t), W, H, 90, 0, 0, W, H);
		for (uint32_t py = 0; py < (uint32_t)W; py++)
			for (uint32_t px = 0; px < (uint32_t)H; px++)
			{
				uint32_t x = 0, y = 0;
				banana::rotateSource(px, py, W, H, x, y);
				if (dst[(size_t)py * H + px] != src[y][x])
				{
					fprintf(stderr, "FAIL turnRect: target (%u,%u) -> source (%u,%u)\n", px, py, x, y);
					fails++;
				}
			}
	}
	// 2. the quad's four corner UVs, interpolated at a target pixel, equal rotateUv() there
	{
		const float q[4][4] = {	// clip x, clip y, u, v (egl_presenter.cpp)
			{ -1, -1, 0, 1 },
			{  1, -1, 0, 0 },
			{ -1,  1, 1, 1 },
			{  1,  1, 1, 0 },
		};
		const uint32_t W = 1240, H = 1080, SW = H, SH = W;
		const uint32_t samples[][2] = { { 0, 0 }, { SW - 1, 0 }, { 0, SH - 1 }, { SW - 1, SH - 1 }, { 617, 333 } };
		for (auto& s : samples)
		{
			uint32_t px = s[0], py = s[1];
			// clip position of the pixel centre (DRM row = GL row: DRM 0 is the quad's bottom in clip terms)
			float cx = 2.0f * ((px + 0.5f) / SW) - 1.0f;
			float cy = 2.0f * ((py + 0.5f) / SH) - 1.0f;
			float u = q[0][2] + (q[1][2] - q[0][2]) * (cx + 1) * 0.5f + (q[2][2] - q[0][2]) * (cy + 1) * 0.5f;
			float v = q[0][3] + (q[1][3] - q[0][3]) * (cx + 1) * 0.5f + (q[2][3] - q[0][3]) * (cy + 1) * 0.5f;
			// the quad is axis-aligned, so the two bilinear terms do not multiply; this is the exact mapping
			float eu = 0, ev = 0;
			banana::rotateUv(px, py, W, H, eu, ev);
			if (fabsf(u - eu) > 1e-5f || fabsf(v - ev) > 1e-5f)
			{
				fprintf(stderr, "FAIL uv: target (%u,%u) quad (%.6f,%.6f) expected (%.6f,%.6f)\n", px, py, u, v, eu, ev);
				fails++;
			}
		}
	}
	// 3. fitRotated: any source is letterboxed into the 1240x1080 logical output, turned onto the 1080x1240 panel
	struct Fit { uint32_t w, h, x, y, vw, vh; };
	const Fit cases[] = {
		{1240, 1080, 0, 0, 1080, 1240},
		{620, 540, 0, 0, 1080, 1240},		// same aspect, half size: scaled up to fill
		{640, 480, 75, 0, 930, 1240},		// 4:3 is wider than 1240:1080: bars along the panel's short axis
		{1280, 720, 191, 0, 698, 1240},		// 16:9, wider still
		{1080, 1240, 0, 149, 1080, 941},	// a portrait window: bars along the panel's long axis
	};
	for (const Fit& c : cases)
	{
		uint32_t x, y, vw, vh;
		banana::fitRotated(c.w, c.h, 1240, 1080, x, y, vw, vh);
		if (x != c.x || y != c.y || vw != c.vw || vh != c.vh)
		{
			fprintf(stderr, "FAIL fit %ux%u: got (%u,%u %ux%u) expected (%u,%u %ux%u)\n", c.w, c.h, x, y, vw, vh, c.x, c.y, c.vw, c.vh);
			fails++;
		}
		if (x + vw > 1080 || y + vh > 1240) { fprintf(stderr, "FAIL fit %ux%u: outside the panel\n", c.w, c.h); fails++; }
	}
	printf("rotation-test: %s\n", fails ? "failed" : "ok");
	return fails ? 1 : 0;
}
