#include <cstdio>
#include "../src/dmabuf.h"

// the Phase 3B buffer rules: one plane, linear, the two formats the Phase 2 matrix measured
int main()
{
	int fails = 0;
	auto check = [&](bool got, bool want, const char* what) {
		if (got != want) { fprintf(stderr, "FAIL %s\n", what); fails++; }
	};
	const uint64_t LINEAR = DRM_FORMAT_MOD_LINEAR, COMPRESSED = 0x0500000000000001ull;
	check(banana::dmabufSupported(DRM_FORMAT_XRGB8888, LINEAR, 1), true, "XR24 linear");
	check(banana::dmabufSupported(DRM_FORMAT_ARGB8888, LINEAR, 1), true, "AR24 linear");
	check(banana::dmabufSupported(DRM_FORMAT_XRGB8888, COMPRESSED, 1), false, "compressed rejected");
	check(banana::dmabufSupported(DRM_FORMAT_RGB565, LINEAR, 1), false, "unmeasured format rejected");
	check(banana::dmabufSupported(DRM_FORMAT_XRGB8888, LINEAR, 2), false, "multi-plane rejected");
	printf("dmabuf-test: %s\n", fails ? "failed" : "ok");
	return fails ? 1 : 0;
}
