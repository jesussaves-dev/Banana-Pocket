#pragma once
// Phase 3C: rotate a 1240x1080 client frame into a 1080x1240 linear scanout buffer with EGL/GLES2, and present it
// through Display::presentGame() 1:1. Three linear GBM scanout slots; glFinish before the commit; source buffers
// stay referenced by the compositor until the frame's flip. The implementation is in egl_presenter.cpp.
#include "presenter.h"

namespace banana
{
	// Opt-in isolation variants (gates under /run/banana): the op the presenter runs instead of the full path.
	// Import: EGLImage + texture + glFinish. Pbuffer: the rotated draw into an ordinary EGL pbuffer. Clear: the
	// scanout slot FBO clear. All three skip the KMS submission; None is the normal 3C path.
	enum class DiagVariant { None, Import, Pbuffer, Clear, PlainFbo, BoRender, BoScanout, BoScanoutFb, BoPrime, BoFbAfter };

	class EglPresenter : public Presenter
	{
	public:
		explicit EglPresenter(class Display& d, DiagVariant diag = DiagVariant::None);
		~EglPresenter() override;

		const char* name() const override { return "EGL rotate"; }
		bool rotated() const override { return true; }
		bool start(std::string& err) override;
		void stop() override;
		bool render(ClientBuffer& b, uint32_t& fb, std::string& err) override;
		int takeFenceFd() override;
		void setHole(int x, int y, int w, int h) override;
		void presented() override;
		void abandon() override;
		void flipDone() override;
		void detached() override;
		void forget(ClientBuffer& b) override;
		void quiesce(bool scanoutCleared) override;
		bool diagnostic() const override { return diag != DiagVariant::None; }
		bool scanning() const override;

	private:
		struct Impl;
		Impl* impl = nullptr;
		DiagVariant diag = DiagVariant::None;
	};
}
