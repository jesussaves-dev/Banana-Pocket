#pragma once
// Presentation strategies for one client frame. DirectPresenter is the Phase 3B path (the client's framebuffer on
// the primary, unrotated); EglPresenter is Phase 3C (one EGL rotate pass into a linear scanout pool). The compositor
// owns exactly one and calls it only while the DRM fd and its EGL context are alive: render() and forget() are
// never called after stop().
#include <cstdint>
#include <string>

namespace banana
{
	struct ClientBuffer;

	class Presenter
	{
	public:
		virtual ~Presenter() = default;
		virtual const char* name() const = 0;
		virtual bool rotated() const = 0;	// true when the output is panel-oriented (a real 3C rotation)
		virtual bool acceptsShm() const { return false; }	// render() can upload a wl_shm source
		virtual bool start(std::string& err) = 0;
		virtual void stop() = 0;
		// Draw the client frame into a scanout framebuffer. On true, the GPU has finished reading the source (the
		// implementation calls glFinish or the direct path has no GPU read); the caller commits fb and then calls
		// presented() or abandon(). The source buffer is still referenced by the caller.
		virtual bool render(ClientBuffer& b, uint32_t& fb, std::string& err) = 0;
		// 3F: the sync file for the scanout buffer's GPU writes (owned by the caller, close it after the commit),
		// -1 when render() completed synchronously (glFinish) or the path has no GPU writes. Take once per render.
		virtual int takeFenceFd() { return -1; }
		// Where render() places the frame, in logical pixels (w == 0: the whole output). Only a rotating presenter can.
		virtual void setHole(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
		virtual void presented() = 0;	// the commit succeeded: the rendered FB is on the plane
		virtual void abandon() = 0;	// the commit failed: return the rendered slot, drop the frame
		virtual void flipDone() = 0;	// the page-flip event for the current FB completed
		virtual void detached() = 0;	// the menu took the primary back: the current FB is no longer scanned
		virtual void forget(ClientBuffer& b) = 0;	// the buffer is about to be freed: release its GPU/DRM objects
		// Called once at the start of Compositor::stop(): scanoutCleared says the primary no longer scans any
		// presenter-owned framebuffer, so freeing DRM framebuffers is safe now.
		virtual void quiesce(bool scanoutCleared) { (void)scanoutCleared; }
		// True for the opt-in 3C isolation variants: render() ran the diagnostic operation and the frame must
		// be released without any KMS submission.
		virtual bool diagnostic() const { return false; }
		// True while a presenter-owned framebuffer is (or may be) on the primary plane. The compositor may only
		// replace a presenter when this is false.
		virtual bool scanning() const { return false; }
	};

	class DirectPresenter : public Presenter
	{
	public:
		explicit DirectPresenter(class Display& d) : d(d) {}
		const char* name() const override { return "direct (unrotated)"; }
		bool rotated() const override { return false; }
		bool start(std::string& err) override;
		void stop() override {}
		bool render(ClientBuffer& b, uint32_t& fb, std::string& err) override;
		void presented() override {}
		void abandon() override {}
		void flipDone() override {}
		void detached() override {}
		void forget(ClientBuffer& b) override;
		void quiesce(bool scanoutCleared) override { allowRmfb = scanoutCleared; }

	private:
		Display& d;
		bool allowRmfb = true;	// false only if stop could not clear the plane (the DRM close then releases the FB)
	};
}
