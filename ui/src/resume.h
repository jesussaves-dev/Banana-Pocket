// What the UI does on SIGUSR1 (bananad's wake): retry the panel restore, then pick the recovery. Pulled out of main.cpp's
// signal handler so tests/resume-test.cpp can pin the policy without a DRM device.
#pragma once

namespace banana
{
	enum class ResumeAction
	{
		Resume,			// the panel is back: re-arm the compositor, redraw
		KeepResident,	// restore failed with a game resident: keep DRM master (a reopen would give it up), log only
		Reopen,			// restore failed with no game: close and reopen the panel
	};

	inline ResumeAction resumeAction(bool restored, int gamePid)
	{
		if (restored) return ResumeAction::Resume;
		return gamePid > 0 ? ResumeAction::KeepResident : ResumeAction::Reopen;
	}

	// up to `tries` restore attempts (20 ms apart in the UI: about a second) while the display is open
	template <class IsOpen, class Restore, class Sleep>
	inline bool restoreRetry(IsOpen isOpen, Restore restore, Sleep sleep, int tries = 50)
	{
		bool ok = false;
		for (int i = 0; isOpen() && !ok && i < tries; i++)
		{
			ok = restore();
			if (!ok) sleep();
		}
		return ok;
	}
}
