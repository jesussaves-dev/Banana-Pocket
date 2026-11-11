// AtomicBuf's unit test: the hand-built drm_mode_atomic property list must group the way the kernel reads it --
// count_objs object groups, each count[i] properties taken in order from props/vals -- and it must fail rather than
// truncate when a frame needs more than it holds. Built and run by ui/tests/run.sh (no DRM device needed).
#include "../src/kms.h"
#include <cstdio>

using banana::AtomicBuf;

static int bad = 0;
#define CHECK(cond, ...) do { if (!(cond)) { bad++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main()
{
	AtomicBuf r;
	// two properties on the primary (38), one on the overlay (44): two object groups, three properties
	r.add(38, 10, 0xAAAA);
	r.add(38, 11, 0xBBBB);
	r.add(44, 20, 0xCCCC);
	CHECK(r.groups == 2, "groups %d", r.groups);
	CHECK(r.n == 3, "n %d", r.n);
	CHECK(r.counts[0] == 2 && r.counts[1] == 1, "counts %u,%u", r.counts[0], r.counts[1]);
	CHECK(r.objs[0] == 38 && r.objs[1] == 44, "objs is one id per group, the kernel's layout: %u,%u", r.objs[0], r.objs[1]);
	CHECK(r.props[0] == 10 && r.props[1] == 11 && r.props[2] == 20, "props");
	CHECK(r.vals[0] == 0xAAAA && r.vals[2] == 0xCCCC, "values");
	CHECK(!r.overflow, "unexpected overflow");

	// property 0 means "the plane has no such property": skipped, and it does not open a group
	r.clear();
	CHECK(r.n == 0 && r.groups == 0 && !r.overflow, "clear");
	r.add(38, 0, 1);
	CHECK(r.n == 0 && r.groups == 0, "zero property id was kept");

	// the page flip's user_data is the pending counter, so an event without a matching add is a bug
	// (checked by commitFrame's call sites; here only that a full frame fits)
	r.clear();
	for (int i = 1; i <= 11; i++) r.add(38, (uint32_t)i, (uint64_t)i);	// the primary plane's properties
	for (int i = 1; i <= 10; i++) r.add(44, (uint32_t)i, (uint64_t)(i + 100));	// the overlay plane's
	CHECK(r.n == 21 && r.groups == 2 && !r.overflow, "a full frame: n %d groups %d overflow %d", r.n, r.groups, r.overflow);
	CHECK(r.counts[0] == 11 && r.counts[1] == 10, "a full frame's counts %u,%u", r.counts[0], r.counts[1]);

	// too many properties: refuse, do not silently drop one
	r.clear();
	for (int i = 1; i <= 33; i++) r.add(38, (uint32_t)i, 0);
	CHECK(r.overflow && r.n == AtomicBuf::kMaxProps, "property overflow: n %d overflow %d", r.n, r.overflow);

	// too many objects: same
	r.clear();
	for (int i = 1; i <= AtomicBuf::kMaxGroups + 1; i++) r.add((uint32_t)(100 + i), 1, 0);
	CHECK(r.overflow, "group overflow");

	printf("atomic-buf: %s\n", bad ? "FAILED" : "all checks passed");
	return bad != 0;
}
