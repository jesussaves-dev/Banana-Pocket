// The wake path's recovery policy (src/resume.h, used by main.cpp's SIGUSR1 handler): a failed panel restore with a game
// resident must neither reopen the panel (that gives up DRM master) nor quit; with no game it reopens. Built and run
// by ui/tests/run.sh (no DRM device needed).
#include "../src/resume.h"
#include <cstdio>

using namespace banana;

static int bad = 0;
#define CHECK(cond, ...) do { if (!(cond)) { bad++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main()
{
	// restore never succeeds: 50 attempts, a sleep between each, then the policy depends on the game
	int calls = 0, sleeps = 0;
	bool r = restoreRetry([] { return true; }, [&] { calls++; return false; }, [&] { sleeps++; });
	CHECK(!r && calls == 50 && sleeps == 50, "failing restore: result %d, %d attempts, %d sleeps", r, calls, sleeps);
	CHECK(resumeAction(r, 4242) == ResumeAction::KeepResident, "resident game: must keep the panel and DRM master");
	CHECK(resumeAction(r, 0) == ResumeAction::Reopen, "no game: reopen the panel");
	CHECK(resumeAction(r, -1) == ResumeAction::Reopen, "gamePid -1 is no game");

	// success on the 3rd attempt stops the retries
	calls = sleeps = 0;
	r = restoreRetry([] { return true; }, [&] { return ++calls == 3; }, [&] { sleeps++; });
	CHECK(r && calls == 3 && sleeps == 2, "late success: result %d, %d attempts, %d sleeps", r, calls, sleeps);
	CHECK(resumeAction(r, 4242) == ResumeAction::Resume && resumeAction(r, 0) == ResumeAction::Resume, "restored: resume");

	// a closed display is never asked to restore
	calls = 0;
	r = restoreRetry([] { return false; }, [&] { calls++; return true; }, [] {});
	CHECK(!r && calls == 0, "closed display: result %d, %d attempts", r, calls);

	if (bad) { printf("%d failures\n", bad); return 1; }
	printf("resume-test: ok\n");
	return 0;
}
