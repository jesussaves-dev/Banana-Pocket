#pragma once
// Phase 3C diagnostic trace, used only while the EGL path is being validated: one line per milestone, flushed and
// fsync'd so a hard hang/reset does not lose the last evidence. Disabled (no file, no cost) unless traceInit is
// called with a path.
namespace banana
{
	void traceInit(const char* path);	// a path to enable, nullptr to disable
	void tracef(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
}
