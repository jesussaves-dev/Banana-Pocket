// shader_mask.h's unit test: the mask's alpha tables (docs/PLAN: the system-wide shader plan, section 3), the four
// panel rotations, true black, the composed base, the colour matrices and the gamma LUT. Built and run by
// ui/tests/run.sh; the header is pure (no DRM calls), so this needs no display.
#include "../src/shader_mask.h"
#include <cstdio>
#include <vector>

using namespace banana;

static int bad = 0;
#define CHECK(cond, ...) do { if (!(cond)) { bad++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void testTables()
{
	MaskGeom g;	// period 4, origin 0

	// CRT: 0x20 00 20 C0 per logical row
	CHECK(maskAlphaAt(SHADER_MASK_CRT_240P, g, 0, 0) == 0x20, "crt j0");
	CHECK(maskAlphaAt(SHADER_MASK_CRT_240P, g, 0, 1) == 0x00, "crt j1");
	CHECK(maskAlphaAt(SHADER_MASK_CRT_240P, g, 0, 2) == 0x20, "crt j2");
	CHECK(maskAlphaAt(SHADER_MASK_CRT_240P, g, 0, 3) == 0xC0, "crt j3");
	CHECK(maskAlphaAt(SHADER_MASK_CRT_240P, g, 0, 4) == 0x20, "crt wraps");

	// CRT at period 3: 18 00 C0
	MaskGeom g3; g3.rowPeriod = 3; g3.gridPeriod = 3;
	CHECK(maskAlphaAt(SHADER_MASK_CRT_240P, g3, 0, 0) == 0x18, "crt3 j0");
	CHECK(maskAlphaAt(SHADER_MASK_CRT_240P, g3, 0, 1) == 0x00, "crt3 j1");
	CHECK(maskAlphaAt(SHADER_MASK_CRT_240P, g3, 0, 2) == 0xC0, "crt3 j2");

	// aperture grille: rows x the three stripe columns
	static const uint8_t a4[4][3] = { { 0x3D, 0x10, 0x6A }, { 0x30, 0x00, 0x60 }, { 0x3D, 0x10, 0x6A }, { 0xA5, 0x90, 0xBA } };
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 6; x++)
			CHECK(maskAlphaAt(SHADER_MASK_APERTURE_GRILLE, g, x, y) == a4[y][x % 3], "aperture p4 %d,%d", x, y);
	static const uint8_t a3[3][3] = { { 0x3A, 0x0C, 0x67 }, { 0x30, 0x00, 0x60 }, { 0xA5, 0x90, 0xBA } };
	for (int y = 0; y < 3; y++)
		for (int x = 0; x < 3; x++)
			CHECK(maskAlphaAt(SHADER_MASK_APERTURE_GRILLE, g3, x, y) == a3[y][x], "aperture p3 %d,%d", x, y);

	// LCD grid, period 5: the last column and row are 0xA0, the corner 0xC8, the rest 0
	MaskGeom gl; gl.gridPeriod = 5;
	for (int y = 0; y < 10; y++)
		for (int x = 0; x < 10; x++)
		{
			int cx = x % 5, cy = y % 5;
			uint8_t want = cx == 4 && cy == 4 ? 0xC8 : (cx == 4 || cy == 4 ? 0xA0 : 0x00);
			CHECK(maskAlphaAt(SHADER_MASK_LCD_GRID, gl, x, y) == want, "lcd %d,%d", x, y);
		}

	// the origin shifts the pattern, including a negative origin
	MaskGeom go; go.originX = -2; go.originY = 3; go.rowPeriod = 5; go.gridPeriod = 5;
	CHECK(maskAlphaAt(SHADER_MASK_LCD_GRID, go, 0, 0) == 0x00, "lcd origin 0,0");
	CHECK(maskAlphaAt(SHADER_MASK_LCD_GRID, go, 1, 0) == 0x00, "lcd origin 1,0");	// x-origin = 3: not the last column
	CHECK(maskAlphaAt(SHADER_MASK_LCD_GRID, go, 2, 0) == 0xA0, "lcd origin last column");	// x-origin = 4
	CHECK(maskAlphaAt(SHADER_MASK_LCD_GRID, go, 2, 2) == 0xC8, "lcd origin corner");	// x-origin = 4, y-origin = -1 -> 4
	CHECK(maskAlphaAt(SHADER_MASK_OFF, g, 0, 0) == 0, "off");
}

static void testRotation(int W, int H, int rot, const MaskGeom& g, ShaderMask m, int pitchPad)
{
	int pw = rot == 90 || rot == 270 ? H : W;
	int ph = rot == 90 || rot == 270 ? W : H;
	size_t pitch = (size_t)pw + pitchPad;
	std::vector<uint32_t> dst(pitch * ph, 0xDEADBEEFu);
	maskFill(dst.data(), pitch, W, H, rot, m, g);

	int wrong = 0;
	for (int py = 0; py < ph; py++)
	{
		for (int px = 0; px < pw; px++)
		{
			int x, y;
			if (rot == 90) { x = py; y = H - 1 - px; }
			else if (rot == 270) { x = W - 1 - py; y = px; }
			else if (rot == 180) { x = W - 1 - px; y = H - 1 - py; }
			else { x = px; y = py; }
			uint32_t want = (uint32_t)maskAlphaAt(m, g, x, y) << 24;
			if (dst[(size_t)py * pitch + px] != want) wrong++;
		}
		for (int px = pw; px < (int)pitch; px++)
			if (dst[(size_t)py * pitch + px] != 0xDEADBEEFu) { wrong++; break; }
	}
	CHECK(wrong == 0, "%dx%d rot %d: %d pixels differ from the logical mask", W, H, rot, wrong);
	printf("mask %dx%d rot %d: %s\n", W, H, rot, wrong ? "MISMATCH" : "ok");
}

static void testCompose()
{
	const uint32_t mask[4] = { 0xC0000000u, 0x20000000u, 0x00000000u, 0xC0000000u };
	std::vector<uint32_t> base(4);

	// no frame: the mask's black with its alpha scaled by the slider
	maskCompose(base.data(), nullptr, mask, 4, 50);
	CHECK(base[0] == 0x60000000u, "compose 50%% of C0: %08x", base[0]);
	CHECK(base[1] == 0x10000000u, "compose 50%% of 20: %08x", base[1]);
	CHECK(base[2] == 0x00000000u, "compose 0");
	// black stays exactly black (the mask must never lift #000000)
	for (uint32_t v : base) CHECK((v & 0x00FFFFFFu) == 0, "compose lifted black: %08x", v);
	maskCompose(base.data(), nullptr, mask, 4, 100);
	CHECK(base[0] == 0xC0000000u, "compose 100%%");
	maskCompose(base.data(), nullptr, mask, 4, 0);
	CHECK(base[0] == 0x00000000u, "compose 0%%");

	// the frame (premultiplied) is opaque outside its hole and shows the mask through the hole
	const uint32_t frame[4] = { 0xFF112233u, 0x00000000u, 0x80112233u, 0x80112233u };
	maskCompose(base.data(), frame, mask, 4, 100);
	CHECK(base[0] == 0xFF112233u, "opaque frame: %08x", base[0]);
	CHECK(base[1] == 0x20000000u, "hole shows the mask: %08x", base[1]);
	CHECK(base[2] == 0x80112233u, "empty mask over a frame leaves it: %08x", base[2]);
	// half-transparent frame over the 0xC0 mask: alpha 0x80 + 0xC0*(255-0x80)/255 = 0x80 + 0x60 = 0xE0
	CHECK(base[3] == 0xE0112233u, "half frame over mask: %08x", base[3]);
}

static void testGeom()
{
	MaskGeom g;
	g = maskGeomFor("gb", 60, 36, 1120, true);
	CHECK(g.rowPeriod == 7 && g.gridPeriod == 7 && g.originX == 60 && g.originY == 36, "gb geom");
	g = maskGeomFor("gbc", 60, 36, 1120, true);
	CHECK(g.rowPeriod == 7 && g.originX == 60 && g.originY == 36, "gbc geom");
	g = maskGeomFor("gba", 20, 140, 1200, true);
	CHECK(g.rowPeriod == 5 && g.gridPeriod == 5 && g.originX == 20 && g.originY == 140, "gba geom");
	g = maskGeomFor("pico8", 108, 28, 1024, true);
	CHECK(g.rowPeriod == 8 && g.originX == 108 && g.originY == 28, "pico8 geom");
	// 224-line, 256 wide at 4x: the hole is 1024 wide
	g = maskGeomFor("nes", 108, 92, 1024, true);
	CHECK(g.rowPeriod == 4 && g.originX == 108 && g.originY == 92, "256-wide 4x geom");
	// 320x240 at 3x: the hole is 960 wide
	g = maskGeomFor("psx", 140, 180, 960, true);
	CHECK(g.rowPeriod == 3 && g.originX == 140 && g.originY == 180, "320-wide 3x geom");
	// not integer scaled (a frame's hole): period 4 at the hole's own origin
	g = maskGeomFor("n64", 100, 90, 1000, false);
	CHECK(g.rowPeriod == 4 && g.originX == 100 && g.originY == 90, "unknown geom");
	g = maskGeomFor(nullptr, 7, 8, 9, true);
	CHECK(g.rowPeriod == 4 && g.originX == 7 && g.originY == 8, "null id geom");
	// a known system that is not integer scaled (the Banana frame turns it off): its source pixels are not 7 wide
	g = maskGeomFor("gb", 40, 30, 1000, false);
	CHECK(g.rowPeriod == 4 && g.gridPeriod == 4 && g.originX == 40 && g.originY == 30, "gb not integer scaled");

	// no frame and integer scaling: where the game sits on the 1240x1080 panel
	int x = -1, y = -1, w = -1;
	CHECK(maskIntegerHole("gb", 1240, 1080, x, y, w) && x == 60 && y == 36 && w == 1120, "gb hole %d,%d %d", x, y, w);
	CHECK(maskIntegerHole("gbc", 1240, 1080, x, y, w) && x == 60 && y == 36 && w == 1120, "gbc hole %d,%d %d", x, y, w);
	CHECK(maskIntegerHole("gba", 1240, 1080, x, y, w) && x == 20 && y == 140 && w == 1200, "gba hole %d,%d %d", x, y, w);
	CHECK(maskIntegerHole("pico8", 1240, 1080, x, y, w) && x == 108 && y == 28 && w == 1024, "pico8 hole %d,%d %d", x, y, w);
	CHECK(maskIntegerHole("snes", 1240, 1080, x, y, w) && x == 108 && y == 92 && w == 1024, "snes hole %d,%d %d", x, y, w);
	CHECK(!maskIntegerHole("psx", 1240, 1080, x, y, w), "psx's size depends on its core and the game");
	CHECK(!maskIntegerHole("", 1240, 1080, x, y, w), "no system, no hole");
}

static void testColor()
{
	// 16.16 coefficients: the kernel shifts them and the DSPP keeps bit 15, so the low 17 bits are zero
	for (int c = 0; c < COLOR_PROFILE_COUNT; c++)
		for (int i = 0; i < 9; i++)
			CHECK((kColorCtm[c][i] & 0x1FFFFu) == 0, "ctm %d[%d] low bits: %016llx", c, i, (unsigned long long)kColorCtm[c][i]);
	// each row sums to at most 1.0, or a white pixel clips
	for (int c = 0; c < COLOR_PROFILE_COUNT; c++)
		for (int r = 0; r < 3; r++)
		{
			uint64_t sum = kColorCtm[c][r * 3] + kColorCtm[c][r * 3 + 1] + kColorCtm[c][r * 3 + 2];
			CHECK(sum <= (0x10000ull << 16), "ctm %d row %d sums to %llx", c, r, (unsigned long long)sum);
		}

	// the LUT: 1024 entries, entry 0 is 0, the top is the full 16-bit range, and it never goes backwards
	std::vector<drm_color_lut> lut(1024);
	colorLutFill(lut.data(), 1024, COLOR_PROFILE_BYPASS, 0);
	CHECK(lut[0].red == 0 && lut[0].green == 0 && lut[0].blue == 0, "lut entry 0");
	CHECK(lut[1023].red == 65535 && lut[1023].green == 65535 && lut[1023].blue == 65535, "lut top");
	for (unsigned i = 1; i < lut.size(); i++)
	{
		CHECK(lut[i].red >= lut[i - 1].red, "lut red decreases at %u", i);
		CHECK(lut[i].green >= lut[i - 1].green, "lut green decreases at %u", i);
		CHECK(lut[i].blue >= lut[i - 1].blue, "lut blue decreases at %u", i);
		CHECK(lut[i].red == lut[i].green && lut[i].green == lut[i].blue, "bypass is neutral at %u", i);
	}
	// night light pulls green and blue down, and shares the one LUT
	colorLutFill(lut.data(), 1024, COLOR_PROFILE_BYPASS, 2);
	CHECK(lut[1023].red == 65535 && lut[1023].green == (uint16_t)(65535 * 0.85 + 0.5) && lut[1023].blue == (uint16_t)(65535 * 0.65 + 0.5), "warmer gains");
	// the PVM profile bends the ramp (2.4/2.2), leaving entry 0 at black
	colorLutFill(lut.data(), 1024, COLOR_PROFILE_PVM_6500K, 0);
	CHECK(lut[0].red == 0, "pvm entry 0");
	CHECK(lut[512].red < 32768, "pvm midpoint: %u", lut[512].red);
	CHECK(lut[1023].red == 65535, "pvm top: %u", lut[1023].red);

	// the plane alpha for the slider's ten steps
	CHECK(maskPlaneAlpha(10) == 6554 && maskPlaneAlpha(50) == 32768 && maskPlaneAlpha(100) == 65535, "maskPlaneAlpha");
}

int main()
{
	testTables();
	MaskGeom g;
	g.rowPeriod = 4; g.gridPeriod = 5; g.originX = 108; g.originY = 92;
	for (int rot : { 0, 90, 180, 270 })
		for (ShaderMask m : { SHADER_MASK_CRT_240P, SHADER_MASK_APERTURE_GRILLE, SHADER_MASK_LCD_GRID, SHADER_MASK_OFF })
			testRotation(8, 6, rot, g, m, 0), testRotation(8, 6, rot, g, m, 3);
	// the real panel once (1240x1080 turned to 1080x1240), so the buffer's own size is covered
	testRotation(1240, 1080, 90, g, SHADER_MASK_CRT_240P, 0);
	testCompose();
	testGeom();
	testColor();
	printf("shader-mask: %s\n", bad ? "FAILED" : "all checks passed");
	return bad != 0;
}
