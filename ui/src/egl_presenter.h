#pragma once
// Phase 3C: rotate a 1240x1080 client frame into a 1080x1240 linear scanout buffer with EGL/GLES2, and present it
// through Display::presentGame() 1:1. Three linear GBM scanout slots; glFinish before the commit; source buffers
// stay referenced by the compositor until the frame's flip. The implementation is in egl_presenter.cpp.
#include "presenter.h"

namespace banana
{
	class EglPresenter : public Presenter
	{
	public:
		explicit EglPresenter(class Display& d);
		~EglPresenter() override;

		const char* name() const override { return "EGL rotate"; }
		bool rotated() const override { return true; }
		bool start(std::string& err) override;
		void stop() override;
		bool render(ClientBuffer& b, uint32_t& fb, std::string& err) override;
		void presented() override;
		void abandon() override;
		void flipDone() override;
		void detached() override;
		void forget(ClientBuffer& b) override;
		void quiesce(bool scanoutCleared) override;

	private:
		struct Impl;
		Impl* impl = nullptr;
	};
}
