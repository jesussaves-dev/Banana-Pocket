#include "egl_presenter.h"
#include "client_buffer.h"
#include "kms.h"
#include "rotate.h"
#include "trace.h"
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

// Phase 3C: three 1080x1240 LINEAR XR24 scanout slots, each with a DRM framebuffer and an EGLImage-backed FBO.
// A client's 1240x1080 linear XR24/AR24 buffer is imported as an EGLImage, sampled by a full-target quad rotated
// 90 degrees clockwise (ui/src/rotate.h), and glFinish()'d before the compositor commits the slot's FB.
//
// Lifetimes: the compositor owns the ClientBuffers and keeps each source referenced until the frame's flip; this
// presenter owns the source EGLImages/textures (destroyed by forget(), called before the ClientBuffer is freed) and
// the slots (destroyed in stop()). It never calls drmModeRmFB: the compositor blanks the primary before stop() and
// the DRM file close releases the remaining framebuffer ids (documented in docs/PLAN_DRM_COMPOSITOR.md).
namespace banana
{
	namespace
	{
		constexpr unsigned kTraceFrames = 16;	// durable per-frame trace budget

		void logf(const char* fmt, ...)
		{
			va_list ap;
			va_start(ap, fmt);
			fputs("egl: ", stderr);
			vfprintf(stderr, fmt, ap);
			fputc('\n', stderr);
			va_end(ap);
		}

		const char* diagName(DiagVariant v)
		{
			switch (v)
			{
			case DiagVariant::Import: return "import/bind only";
			case DiagVariant::Pbuffer: return "rotated draw to EGL pbuffer";
			case DiagVariant::Clear: return "scanout-slot clear only";
			case DiagVariant::PlainFbo: return "rotated draw to plain texture FBO";
			case DiagVariant::BoRender: return "GBM clear, RENDERING only";
			case DiagVariant::BoScanout: return "GBM clear, RENDERING|SCANOUT";
			case DiagVariant::BoScanoutFb: return "GBM clear, RENDERING|SCANOUT, AddFB2 registered";
			case DiagVariant::BoPrime: return "GBM clear, explicit PRIME before EGL";
			case DiagVariant::BoFbAfter: return "GBM clear, AddFB2 after EGL";
			case DiagVariant::None: break;
			}
			return "off";
		}

		bool hasExt(const char* list, const char* want)
		{
			if (!list || !want || !*want) return false;
			size_t n = strlen(want);
			for (const char* p = list; (p = strstr(p, want)); p += n)
				if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
			return false;
		}

		GLuint compile(GLenum type, const char* src, std::string& err)
		{
			GLuint s = glCreateShader(type);
			glShaderSource(s, 1, &src, nullptr);
			glCompileShader(s);
			GLint ok = 0;
			glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
			if (!ok)
			{
				char log[512] = "";
				glGetShaderInfoLog(s, sizeof log, nullptr, log);
				err = std::string("shader: ") + log;
				glDeleteShader(s);
				return 0;
			}
			return s;
		}
	}

	struct EglPresenter::Impl
	{
		struct Source { ClientBuffer* b; EGLImage image; GLuint tex; };
		enum State { Free, Rendering, Rendered, Current };
		struct Slot
		{
			gbm_bo* bo = nullptr;
			uint32_t fb = 0;
			EGLImage image = EGL_NO_IMAGE_KHR;
			GLuint tex = 0, fbo = 0;
			State state = Free;
			bool flipPending = false;
		};

		Display* d = nullptr;
		int gbmFd = -1;
		gbm_device* gbm = nullptr;
		EGLDisplay edpy = EGL_NO_DISPLAY;
		EGLConfig cfg = nullptr;
		EGLContext ctx = EGL_NO_CONTEXT;
		EGLSurface pbuffer = EGL_NO_SURFACE;
		PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
		PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
		PFNGLEGLIMAGETARGETTEXTURE2DOESPROC targetTexture = nullptr;
		PFNEGLCREATESYNCKHRPROC createSync = nullptr;
		PFNEGLDESTROYSYNCKHRPROC destroySync = nullptr;
		PFNEGLDUPNATIVEFENCEFDANDROIDPROC dupFence = nullptr;
		bool fenceOk = false;	// 3F: EGL_ANDROID_native_fence_sync is available (else glFinish)
		int fenceFd = -1;		// the rendered slot's GPU sync file, taken by the compositor
		GLuint program = 0, vbo = 0;
		GLint aPos = -1, aUv = -1, uTex = -1;
		Slot slot[3];
		Slot* current = nullptr;
		Slot* rendered = nullptr;
		std::vector<Source> sources;
		std::string dumpPath;
		bool dumpDone = false;
		bool allowRmfb = false;		// set by quiesce() when the primary no longer scans a slot
		bool freshSource = false;	// diagnostic: a new EGLImage+texture per commit (default: cached)
		DiagVariant diag = DiagVariant::None;	// opt-in isolation variant; None is the normal 3C path
		EGLSurface diagPbuf = EGL_NO_SURFACE;	// variant B's ordinary EGL pbuffer
		GLuint diagTex = 0, diagFbo = 0;	// B': the ordinary texture-backed target
		struct DiagTarget { gbm_bo* bo = nullptr; EGLImage image = EGL_NO_IMAGE_KHR; GLuint tex = 0, fbo = 0; uint32_t fb = 0; };
		DiagTarget diagTargets[3];		// C2: three BOs with the selected usage flags
		unsigned diagNext = 0;			// C2 round-robin over the three targets
		unsigned diagOps = 0;		// only the first three diagnostic operations are traced
		unsigned frames = 0;
		int sw = 0, sh = 0;	// the panel: 1080x1240
		uint32_t lastW = 0, lastH = 0;	// the last source size, to log a size change once

		void destroySource(Source& s)
		{
			if (s.tex) { glDeleteTextures(1, &s.tex); s.tex = 0; }
			if (s.image != EGL_NO_IMAGE_KHR && destroyImage) { destroyImage(edpy, s.image); s.image = EGL_NO_IMAGE_KHR; }
		}
		Slot* acquire()
		{
			for (Slot& s : slot)
				if (s.state == Free) return &s;
			return nullptr;
		}
		// Create an EGLImage and texture for a client buffer. keepFd is true in the per-commit diagnostic mode,
		// where the fd must stay open for the next import; the cached mode closes it here, as before.
		bool makeSource(ClientBuffer& b, Source& out, std::string& err, bool keepFd)
		{
			if (b.fd < 0) { err = "the source fd is gone"; return false; }
			EGLint attrs[] = {
				EGL_WIDTH, (EGLint)b.w,
				EGL_HEIGHT, (EGLint)b.h,
				EGL_LINUX_DRM_FOURCC_EXT, (EGLint)b.format,
				EGL_DMA_BUF_PLANE0_FD_EXT, b.fd,
				EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)b.off,
				EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)b.stride,
				EGL_NONE
			};
			EGLImage img = createImage(edpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
			if (img == EGL_NO_IMAGE_KHR)
			{
				err = "eglCreateImageKHR failed (0x" + std::to_string(eglGetError()) + ")";
				return false;
			}
			GLuint tex = 0;
			glGenTextures(1, &tex);
			glBindTexture(GL_TEXTURE_2D, tex);
			targetTexture(GL_TEXTURE_2D, img);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			if (glGetError() != GL_NO_ERROR)
			{
				glBindTexture(GL_TEXTURE_2D, 0);
				glDeleteTextures(1, &tex);
				destroyImage(edpy, img);
				err = "binding the source EGLImage failed";
				return false;
			}
			if (!keepFd) { close(b.fd); b.fd = -1; }
			out = { &b, img, tex };
			return true;
		}
		// The cached baseline: one EGLImage/texture per wl_buffer, bound once.
		Source* source(ClientBuffer& b, std::string& err)
		{
			for (Source& s : sources)
				if (s.b == &b) return &s;
			Source s;
			if (!makeSource(b, s, err, false)) return nullptr;
			b.eglImage = s.image;
			b.tex = s.tex;
			sources.push_back(s);
			logf("source: %ux%u fmt=%.4s mod=0x%016llx image ok", b.w, b.h, (const char*)&b.format, (unsigned long long)b.modifier);
			if (frames < kTraceFrames)
				tracef("source: cached serial=%llu res=%p image=%p tex=%u", (unsigned long long)b.serial,
				       (void*)b.res, (void*)s.image, s.tex);
			return &sources.back();
		}
		// The diagnostic: a fresh EGLImage and texture for this commit only; the fd stays open for the next import.
		bool sourceFresh(ClientBuffer& b, Source& out, std::string& err)
		{
			if (!makeSource(b, out, err, true)) return false;
			if (frames < kTraceFrames)
				tracef("source: fresh serial=%llu res=%p image=%p tex=%u", (unsigned long long)b.serial,
				       (void*)b.res, (void*)out.image, out.tex);
			return true;
		}
		void dropFresh(Source& s)
		{
			glBindTexture(GL_TEXTURE_2D, 0);	// never delete a texture still bound
			if (frames < kTraceFrames)
				tracef("source: fresh destroy serial=%llu res=%p image=%p tex=%u",
				       (unsigned long long)s.b->serial, (void*)s.b->res, (void*)s.image, s.tex);
			destroySource(s);
		}
		// The opt-in isolation variants: one operation + glFinish, no KMS. Only the first three operations trace.
		bool renderDiag(ClientBuffer& b, std::string& err)
		{
			const unsigned op = ++diagOps;
			Source* src = nullptr;
			if (diag != DiagVariant::Clear && diag != DiagVariant::BoRender && diag != DiagVariant::BoScanout &&
			    diag != DiagVariant::BoScanoutFb && diag != DiagVariant::BoPrime && diag != DiagVariant::BoFbAfter)
			{
				src = source(b, err);
				if (!src) return false;
				if (op <= 3) tracef("diag op %u: source ok", op);
			}
			GLenum e = GL_NO_ERROR;
			if (diag == DiagVariant::Import)
			{
				if (op <= 3) tracef("diag op %u: finish begin", op);
				glFinish();
				e = glGetError();
			}
			else if (diag == DiagVariant::Pbuffer)
			{
				if (diagPbuf == EGL_NO_SURFACE || !eglMakeCurrent(edpy, diagPbuf, diagPbuf, ctx))
				{ err = "the diagnostic pbuffer is not current"; return false; }
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				glViewport(0, 0, sw, sh);
				glDisable(GL_BLEND);
				glDisable(GL_DEPTH_TEST);
				glClearColor(0, 0, 0, 1);
				glClear(GL_COLOR_BUFFER_BIT);
				glUseProgram(program);
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, src->tex);
				glUniform1i(uTex, 0);
				glBindBuffer(GL_ARRAY_BUFFER, vbo);
				glEnableVertexAttribArray(aPos);
				glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
				glEnableVertexAttribArray(aUv);
				glVertexAttribPointer(aUv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
				if (op <= 3) tracef("diag op %u: draw begin", op);
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
				glFinish();
				e = glGetError();
			}
			else if (diag == DiagVariant::Clear)
			{
				Slot* s = acquire();
				if (!s) { err = "no free scanout slot"; return false; }
				s->state = Rendering;
				glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
				glViewport(0, 0, sw, sh);
				glDisable(GL_BLEND);
				glDisable(GL_DEPTH_TEST);
				glClearColor(0, 0, 0, 1);
				if (op <= 3) tracef("diag op %u: clear begin", op);
				glClear(GL_COLOR_BUFFER_BIT);
				glFinish();
				e = glGetError();
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				s->state = Free;
			}
			else if (diag == DiagVariant::PlainFbo)
			{
				glBindFramebuffer(GL_FRAMEBUFFER, diagFbo);
				glViewport(0, 0, sw, sh);
				glDisable(GL_BLEND);
				glDisable(GL_DEPTH_TEST);
				glClearColor(0, 0, 0, 1);
				glClear(GL_COLOR_BUFFER_BIT);
				glUseProgram(program);
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, src->tex);
				glUniform1i(uTex, 0);
				glBindBuffer(GL_ARRAY_BUFFER, vbo);
				glEnableVertexAttribArray(aPos);
				glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
				glEnableVertexAttribArray(aUv);
				glVertexAttribPointer(aUv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
				if (op <= 3) tracef("diag op %u: draw begin", op);
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
				glFinish();
				e = glGetError();
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
			}
			else if (diag == DiagVariant::BoRender || diag == DiagVariant::BoScanout || diag == DiagVariant::BoScanoutFb ||
			         diag == DiagVariant::BoPrime || diag == DiagVariant::BoFbAfter)
			{
				DiagTarget& t = diagTargets[diagNext++ % 3];
				glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
				glViewport(0, 0, sw, sh);
				glDisable(GL_BLEND);
				glDisable(GL_DEPTH_TEST);
				glClearColor(0, 0, 0, 1);
				if (op <= 3) tracef("diag op %u: clear begin target=%u", op, (diagNext - 1) % 3);
				glClear(GL_COLOR_BUFFER_BIT);
				glFinish();
				e = glGetError();
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
			}
			if (op <= 3) tracef("diag op %u: finish done err=0x%x", op, e);
			if (e != GL_NO_ERROR) { err = "GL error 0x" + std::to_string(e); return false; }
			return true;
		}

		void destroyTarget(DiagTarget& t)
		{
			// The FB goes first: it pins the BO, and no plane ever scans a diagnostic target (no KMS).
			if (t.fb && d && d->drmFd() >= 0) { drmModeRmFB(d->drmFd(), t.fb); t.fb = 0; }
			if (t.fbo) { glDeleteFramebuffers(1, &t.fbo); t.fbo = 0; }
			if (t.tex) { glDeleteTextures(1, &t.tex); t.tex = 0; }
			if (t.image != EGL_NO_IMAGE_KHR && edpy != EGL_NO_DISPLAY && destroyImage)
			{ destroyImage(edpy, t.image); t.image = EGL_NO_IMAGE_KHR; }
			if (t.bo) { gbm_bo_destroy(t.bo); t.bo = nullptr; }
		}
		// B': an ordinary 1080x1240 RGBA texture attached to a plain FBO (no GBM, no DRM FB).
		bool addPlainFbo(std::string& err)
		{
			glGenTextures(1, &diagTex);
			glBindTexture(GL_TEXTURE_2D, diagTex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glGenFramebuffers(1, &diagFbo);
			glBindFramebuffer(GL_FRAMEBUFFER, diagFbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, diagTex, 0);
			GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
			GLenum e = glGetError();
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			if (st != GL_FRAMEBUFFER_COMPLETE || e != GL_NO_ERROR)
			{ err = "the plain texture FBO is incomplete (status=0x" + std::to_string(st) + " gl=0x" + std::to_string(e) + ")"; return false; }
			logf("diag target: plain texture %dx%d fbo=%u tex=%u complete", sw, sh, diagFbo, diagTex);
			tracef("diag target: plain texture %dx%d fbo=%u tex=%u complete", sw, sh, diagFbo, diagTex);
			return true;
		}
		// C2: the same three-BO/EGLImage/FBO arrangement as the production slots, with the usage flags and the
		// PRIME/AddFB2 sequence varied by variant (never submitted to KMS).
		bool addDiagTargets(std::string& err)
		{
			const bool primeOnly = diag == DiagVariant::BoPrime;		// D: PRIME + close, no AddFB2
			const bool beforeEgl = diag == DiagVariant::BoScanoutFb;	// C2-C: PRIME + AddFB2 + close, before EGL
			const bool afterEgl = diag == DiagVariant::BoFbAfter;		// E: EGL first, then PRIME (hit) + AddFB2, no close
			const uint32_t usage = diag == DiagVariant::BoRender
				? GBM_BO_USE_RENDERING : (GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
			for (unsigned i = 0; i < 3; i++)
			{
				DiagTarget& t = diagTargets[i];
				uint64_t mods[1] = { DRM_FORMAT_MOD_LINEAR };
				t.bo = gbm_bo_create_with_modifiers2(gbm, (uint32_t)sw, (uint32_t)sh, GBM_FORMAT_XRGB8888, mods, 1, usage);
				if (!t.bo) { err = std::string("diag gbm_bo_create_with_modifiers2: ") + strerror(errno); return false; }
				uint32_t format = gbm_bo_get_format(t.bo);
				uint64_t modifier = gbm_bo_get_modifier(t.bo);
				int planes = gbm_bo_get_plane_count(t.bo);
				uint32_t stride = gbm_bo_get_stride_for_plane(t.bo, 0);
				uint32_t off = gbm_bo_get_offset(t.bo, 0);
				if (format != GBM_FORMAT_XRGB8888 || modifier != DRM_FORMAT_MOD_LINEAR || planes != 1)
				{ err = "the diagnostic BO is not one-plane XR24 LINEAR"; return false; }
				int fd = gbm_bo_get_fd_for_plane(t.bo, 0);
				if (fd < 0) { err = std::string("diag gbm_bo_get_fd_for_plane: ") + strerror(errno); return false; }
				if (primeOnly || beforeEgl)
				{
					uint32_t handle = 0;
					if (drmPrimeFDToHandle(d->drmFd(), fd, &handle))
					{ int e = errno; close(fd); err = std::string("diag drmPrimeFDToHandle: ") + strerror(e); return false; }
					if (beforeEgl)
					{
						uint32_t handles[4] = { handle }, pitches[4] = { stride }, offsets[4] = { off };
						uint64_t m[4] = { modifier };
						int ret = drmModeAddFB2WithModifiers(d->drmFd(), (uint32_t)sw, (uint32_t)sh, format, handles, pitches, offsets, m, &t.fb, DRM_MODE_FB_MODIFIERS);
						if (ret)
						{ int e = errno; t.fb = 0; drmCloseBufferHandle(d->drmFd(), handle); close(fd); err = std::string("diag AddFB2WithModifiers: ") + strerror(e); return false; }
					}
					drmCloseBufferHandle(d->drmFd(), handle);	// this explicit PRIME owns it (D and C2-C)
				}
				EGLint attrs[] = {
					EGL_WIDTH, sw, EGL_HEIGHT, sh,
					EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
					EGL_DMA_BUF_PLANE0_FD_EXT, fd,
					EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)off,
					EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
					EGL_NONE
				};
				t.image = createImage(edpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
				if (!afterEgl) close(fd);	// the EGLImage holds its own reference; E keeps the fd for the post-EGL registration
				if (t.image == EGL_NO_IMAGE_KHR)
				{ if (afterEgl) close(fd); err = "diag EGLImage failed (0x" + std::to_string(eglGetError()) + ")"; return false; }
				glGenTextures(1, &t.tex);
				glBindTexture(GL_TEXTURE_2D, t.tex);
				targetTexture(GL_TEXTURE_2D, t.image);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				glGenFramebuffers(1, &t.fbo);
				glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
				GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
				GLenum e = glGetError();
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				if (st != GL_FRAMEBUFFER_COMPLETE || e != GL_NO_ERROR)
				{ if (afterEgl) close(fd); err = "diagnostic FBO is incomplete (status=0x" + std::to_string(st) + " gl=0x" + std::to_string(e) + ")"; return false; }
				if (afterEgl)
				{
					uint32_t handle = 0;
					if (drmPrimeFDToHandle(d->drmFd(), fd, &handle))
					{ int e2 = errno; close(fd); err = std::string("diag drmPrimeFDToHandle (post-EGL): ") + strerror(e2); return false; }
					uint32_t handles[4] = { handle }, pitches[4] = { stride }, offsets[4] = { off };
					uint64_t m[4] = { modifier };
					int ret = drmModeAddFB2WithModifiers(d->drmFd(), (uint32_t)sw, (uint32_t)sh, format, handles, pitches, offsets, m, &t.fb, DRM_MODE_FB_MODIFIERS);
					// no drmCloseBufferHandle here: the handle is Mesa's EGL-import handle (prime-table hit)
					close(fd);
					if (ret)
					{ int e2 = errno; t.fb = 0; err = std::string("diag AddFB2WithModifiers (post-EGL): ") + strerror(e2); return false; }
				}
				logf("diag target %u: usage=0x%x fmt=%.4s mod=0x%016llx planes=%d stride=%u off=%u fb=%u image ok fbo complete",
				     i, usage, (const char*)&format, (unsigned long long)modifier, planes, stride, off, t.fb);
				tracef("diag target %u: usage=0x%x fmt=%.4s mod=0x%016llx planes=%d stride=%u off=%u fb=%u image ok fbo complete",
				       i, usage, (const char*)&format, (unsigned long long)modifier, planes, stride, off, t.fb);
			}
			return true;
		}
		bool addSlot(std::string& err)
		{
			for (Slot& s : slot)
			{
				uint64_t mods[1] = { DRM_FORMAT_MOD_LINEAR };
				s.bo = gbm_bo_create_with_modifiers2(gbm, (uint32_t)sw, (uint32_t)sh, GBM_FORMAT_XRGB8888, mods, 1,
				                                     GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
				if (!s.bo) { err = std::string("gbm_bo_create_with_modifiers2: ") + strerror(errno); return false; }
				uint32_t format = gbm_bo_get_format(s.bo);
				uint64_t modifier = gbm_bo_get_modifier(s.bo);
				int planes = gbm_bo_get_plane_count(s.bo);
				uint32_t stride = gbm_bo_get_stride_for_plane(s.bo, 0);
				uint32_t off = gbm_bo_get_offset(s.bo, 0);
				int fd = gbm_bo_get_fd_for_plane(s.bo, 0);
				if (fd < 0) { err = std::string("gbm_bo_get_fd_for_plane: ") + strerror(errno); return false; }
				if (format != GBM_FORMAT_XRGB8888 || modifier != DRM_FORMAT_MOD_LINEAR || planes != 1)
				{
					close(fd);
					err = "the GBM BO is not one-plane XR24 LINEAR (format=" + std::to_string(format) +
					      " modifier=0x" + std::to_string((unsigned long long)modifier) + " planes=" + std::to_string(planes) + ")";
					return false;
				}
				EGLint attrs[] = {
					EGL_WIDTH, sw, EGL_HEIGHT, sh,
					EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
					EGL_DMA_BUF_PLANE0_FD_EXT, fd,
					EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)off,
					EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
					EGL_NONE
				};
				s.image = createImage(edpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
				if (s.image == EGL_NO_IMAGE_KHR) { close(fd); err = "scanout EGLImage failed (0x" + std::to_string(eglGetError()) + ")"; return false; }
				glGenTextures(1, &s.tex);
				glBindTexture(GL_TEXTURE_2D, s.tex);
				targetTexture(GL_TEXTURE_2D, s.image);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				glGenFramebuffers(1, &s.fbo);
				glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.tex, 0);
				if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
				{
					glBindFramebuffer(GL_FRAMEBUFFER, 0);
					close(fd);
					err = "scanout FBO is incomplete";
					return false;
				}
				glBindFramebuffer(GL_FRAMEBUFFER, 0);
				// Register the KMS framebuffer only after the EGL import (Stage 16 C2-E): the explicit
				// PRIME before the EGL image is the configuration that hangs. This PRIME is a hit on the
				// handle Mesa's import created, so it is not ours to close.
				uint32_t handle = 0;
				if (drmPrimeFDToHandle(d->drmFd(), fd, &handle)) { close(fd); err = std::string("drmPrimeFDToHandle: ") + strerror(errno); return false; }
				uint32_t handles[4] = { handle }, pitches[4] = { stride }, offsets[4] = { off };
				uint64_t m[4] = { modifier };
				int ret = drmModeAddFB2WithModifiers(d->drmFd(), (uint32_t)sw, (uint32_t)sh, format, handles, pitches, offsets, m, &s.fb, DRM_MODE_FB_MODIFIERS);
				if (ret) { s.fb = 0; close(fd); err = std::string("AddFB2WithModifiers: ") + strerror(errno); return false; }
				close(fd);
				s.state = Free;
				logf("slot: %dx%d XR24 LINEAR stride=%u offset=%u fb=%u", sw, sh, stride, off, s.fb);
				tracef("slot: fb=%u stride=%u", s.fb, stride);
			}
			return true;
		}

		// diagnostic: /run/banana/3c-dump-now (re-armable) writes the client's source and the rotated slot as raw
		// GL-order PPMs, /data/3c-src.ppm and /data/3c-dst.ppm, so a host diff can prove the pass is lossless
		static bool readRaw(GLuint fbo, int w, int h, const char* path)
		{
			std::vector<uint8_t> px((size_t)w * h * 4);
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
			if (glGetError() != GL_NO_ERROR) { logf("raw readback failed: %s", path); return false; }
			FILE* f = fopen(path, "wb");
			if (!f) return false;
			fprintf(f, "P6\n%d %d\n255\n", w, h);
			for (size_t i = 0; i < (size_t)w * h; i++) fwrite(&px[i * 4], 1, 3, f);
			fclose(f);
			return true;
		}
		void dumpPair(Slot& s, GLuint srcTex, int w, int h)
		{
			if (access("/run/banana/3c-dump-now", F_OK) != 0) return;
			unlink("/run/banana/3c-dump-now");
			GLuint fbo = 0;
			glGenFramebuffers(1, &fbo);
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcTex, 0);
			bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && readRaw(fbo, w, h, "/data/3c-src.ppm");
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glDeleteFramebuffers(1, &fbo);
			ok = readRaw(s.fbo, sw, sh, "/data/3c-dst.ppm") && ok;
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			logf("dump pair: %s (src %dx%d)", ok ? "written" : "FAILED", w, h);
		}

		void dump(Slot& s)
		{
			if (dumpDone || dumpPath.empty()) return;
			dumpDone = true;
			std::vector<uint8_t> px((size_t)sw * sh * 4);
			glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
			glPixelStorei(GL_PACK_ALIGNMENT, 1);
			glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
			if (glGetError() != GL_NO_ERROR) { logf("readback failed"); return; }
			FILE* f = fopen(dumpPath.c_str(), "wb");
			if (!f) { logf("readback: cannot write %s", dumpPath.c_str()); return; }
			fprintf(f, "P6\n%d %d\n255\n", sw, sh);
			for (int y = sh - 1; y >= 0; y--)	// GL rows are bottom-up; the panel's are top-down
				for (int x = 0; x < sw; x++)
				{
					const uint8_t* p = &px[((size_t)y * sw + x) * 4];
					fwrite(p, 1, 3, f);
				}
			fclose(f);
			logf("readback: %s (%dx%d)", dumpPath.c_str(), sw, sh);
		}
	};

	EglPresenter::EglPresenter(Display& d, DiagVariant variant) : diag(variant)
	{
		impl = new Impl();
		impl->d = &d;
		impl->diag = variant;
	}

	EglPresenter::~EglPresenter() { stop(); }

	bool EglPresenter::start(std::string& err)
	{
		Impl* im = impl;
		if (!im) { err = "no presenter"; return false; }
		im->sw = im->d->panelWidth();
		im->sh = im->d->panelHeight();
		if (im->sw != 1080 || im->sh != 1240) { err = "the panel is not 1080x1240"; return false; }
		const char* dump = getenv("BANANA_3C_DUMP");
		if (dump && *dump) im->dumpPath = dump;
		im->freshSource = access("/run/banana/3c-fresh-image", F_OK) == 0;
		logf("source mode: %s", im->freshSource ? "fresh EGLImage per commit (diagnostic)" : "cached EGLImage");
		tracef("start: source mode %s", im->freshSource ? "fresh" : "cached");
		if (im->diag != DiagVariant::None)
		{
			logf("diagnostic variant: %s (no KMS submission)", diagName(im->diag));
			tracef("diag: variant %s", diagName(im->diag));
		}

		// close-on-exec: this shares the DRM master's open file, so a daemon started from the menu that inherited it
		// would keep the old master alive and every later open of the card would get EACCES
		im->gbmFd = fcntl(im->d->drmFd(), F_DUPFD_CLOEXEC, 0);
		if (im->gbmFd < 0) { err = std::string("dup: ") + strerror(errno); return false; }
		im->gbm = gbm_create_device(im->gbmFd);
		if (!im->gbm) { err = "gbm_create_device failed"; return false; }
		auto getPlatform = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
		im->edpy = getPlatform ? getPlatform(EGL_PLATFORM_GBM_KHR, im->gbm, nullptr)
		                       : eglGetDisplay((EGLNativeDisplayType)im->gbm);
		if (im->edpy == EGL_NO_DISPLAY) { err = "eglGetDisplay failed"; return false; }
		EGLint maj = 0, min = 0;
		if (!eglInitialize(im->edpy, &maj, &min)) { err = "eglInitialize failed"; return false; }
		if (!eglBindAPI(EGL_OPENGL_ES_API)) { err = "eglBindAPI failed"; return false; }
		// The GBM platform's configs are window-capable; a pbuffer/surfaceless context only needs the ES2 bit.
		// Variant B renders into an ordinary EGL pbuffer, so it needs a pbuffer-capable config explicitly.
		const bool pbufCfg = im->diag == DiagVariant::Pbuffer;
		EGLint cfgAttrs[] = {
			EGL_SURFACE_TYPE, pbufCfg ? EGL_PBUFFER_BIT : (EGL_WINDOW_BIT | EGL_PBUFFER_BIT),
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
			EGL_NONE
		};
		EGLint n = 0;
		if (!eglChooseConfig(im->edpy, cfgAttrs, &im->cfg, 1, &n) || n < 1)
		{
			if (pbufCfg) { err = "no EGL config with EGL_PBUFFER_BIT"; return false; }
			EGLint total = 0;
			eglGetConfigs(im->edpy, nullptr, 0, &total);
			logf("eglChooseConfig failed (0x%x), %d configs total; trying ES2 only", eglGetError(), total);
			EGLint anyAttrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
			if (!eglChooseConfig(im->edpy, anyAttrs, &im->cfg, 1, &n) || n < 1)
			{ err = "eglChooseConfig failed"; return false; }
		}
		EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
		im->ctx = eglCreateContext(im->edpy, im->cfg, EGL_NO_CONTEXT, ctxAttrs);
		if (im->ctx == EGL_NO_CONTEXT) { err = "eglCreateContext failed"; return false; }
		const char* eglExt = eglQueryString(im->edpy, EGL_EXTENSIONS);
		bool surfaceless = hasExt(eglExt, "EGL_KHR_surfaceless_context");
		if (im->diag == DiagVariant::Pbuffer)
		{
			EGLint pb[] = { EGL_WIDTH, im->sw, EGL_HEIGHT, im->sh, EGL_NONE };
			im->diagPbuf = eglCreatePbufferSurface(im->edpy, im->cfg, pb);
			if (im->diagPbuf == EGL_NO_SURFACE) { err = "eglCreatePbufferSurface failed (0x" + std::to_string(eglGetError()) + ")"; return false; }
			if (!eglMakeCurrent(im->edpy, im->diagPbuf, im->diagPbuf, im->ctx))
			{ err = "eglMakeCurrent (diagnostic pbuffer) failed"; return false; }
		}
		else if (surfaceless)
		{
			if (!eglMakeCurrent(im->edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, im->ctx)) { err = "eglMakeCurrent (surfaceless) failed"; return false; }
		}
		else
		{
			EGLint pb[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
			im->pbuffer = eglCreatePbufferSurface(im->edpy, im->cfg, pb);
			if (im->pbuffer == EGL_NO_SURFACE || !eglMakeCurrent(im->edpy, im->pbuffer, im->pbuffer, im->ctx))
			{ err = "no surfaceless context and no pbuffer"; return false; }
		}
		im->createImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
		im->destroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
		im->targetTexture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
		im->createSync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
		im->destroySync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
		im->dupFence = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
		if (!im->createImage || !im->destroyImage || !im->targetTexture) { err = "EGLImage entry points missing"; return false; }
		const char* glExt = (const char*)glGetString(GL_EXTENSIONS);
		logf("%s / %s", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
		logf("extensions: dma_buf_import=%d modifiers=%d fence_sync=%d native_fence=%d surfaceless=%d OES_EGL_image=%d",
		     hasExt(eglExt, "EGL_EXT_image_dma_buf_import"), hasExt(eglExt, "EGL_EXT_image_dma_buf_import_modifiers"),
		     hasExt(eglExt, "EGL_KHR_fence_sync"), hasExt(eglExt, "EGL_ANDROID_native_fence_sync"),
		     surfaceless, hasExt(glExt, "GL_OES_EGL_image"));
		if (!hasExt(eglExt, "EGL_EXT_image_dma_buf_import")) { err = "EGL_EXT_image_dma_buf_import missing"; return false; }
		if (!hasExt(glExt, "GL_OES_EGL_image")) { err = "GL_OES_EGL_image missing"; return false; }
		im->fenceOk = hasExt(eglExt, "EGL_KHR_fence_sync") && hasExt(eglExt, "EGL_ANDROID_native_fence_sync") &&
		              im->createSync && im->destroySync && im->dupFence && im->d->fenceSupported();
		logf("3F: native fence %s", im->fenceOk ? "on" : "unavailable (glFinish)");

		std::string serr;
		GLuint vs = compile(GL_VERTEX_SHADER,
			"attribute vec2 aPos; attribute vec2 aUv; varying highp vec2 vUv;\n"
			"void main() { vUv = aUv; gl_Position = vec4(aPos, 0.0, 1.0); }\n", serr);
		GLuint fs = vs ? compile(GL_FRAGMENT_SHADER,
			// highp: a mediump (fp16 on Adreno) UV lands on the neighbour texel for ~1 column in 12 under
			// GL_NEAREST, which broke 1-px LCD grids and overlay lines (measured with /run/banana/3c-dump-now)
			"precision highp float; varying highp vec2 vUv; uniform sampler2D uTex;\n"
			"void main() { gl_FragColor = texture2D(uTex, vUv); }\n", serr) : 0;
		if (!vs || !fs) { err = serr.empty() ? "shader compile failed" : serr; return false; }
		im->program = glCreateProgram();
		glAttachShader(im->program, vs);
		glAttachShader(im->program, fs);
		glLinkProgram(im->program);
		glDeleteShader(vs);
		glDeleteShader(fs);
		GLint ok = 0;
		glGetProgramiv(im->program, GL_LINK_STATUS, &ok);
		if (!ok) { err = "shader link failed"; return false; }
		im->aPos = glGetAttribLocation(im->program, "aPos");
		im->aUv = glGetAttribLocation(im->program, "aUv");
		im->uTex = glGetUniformLocation(im->program, "uTex");
		// the quad: target corners -> source UVs for the 90-degree clockwise turn (ui/src/rotate.h)
		const float verts[] = {
			-1, -1, 0, 1,
			 1, -1, 0, 0,
			-1,  1, 1, 1,
			 1,  1, 1, 0,
		};
		glGenBuffers(1, &im->vbo);
		glBindBuffer(GL_ARRAY_BUFFER, im->vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
		glBindBuffer(GL_ARRAY_BUFFER, 0);

		// The production slots are needed by the normal path and the Stage 8 "clear" variant; the Stage 9 variants
		// allocate their own destination. This leaves the normal path (diag == None) unchanged.
		const bool needSlots = im->diag == DiagVariant::None || im->diag == DiagVariant::Clear;
		if (needSlots && !im->addSlot(err)) return false;
		if (im->diag == DiagVariant::PlainFbo && !im->addPlainFbo(err)) return false;
		if ((im->diag == DiagVariant::BoRender || im->diag == DiagVariant::BoScanout || im->diag == DiagVariant::BoScanoutFb ||
		     im->diag == DiagVariant::BoPrime || im->diag == DiagVariant::BoFbAfter) && !im->addDiagTargets(err)) return false;
		if (im->diag == DiagVariant::None)
			logf("ready: 3 slots, 1240x1080 -> %dx%d, %s", im->sw, im->sh, im->dumpPath.empty() ? "no readback" : "one-shot readback armed");
		else
			logf("ready: diagnostic variant %s, 1240x1080 -> %dx%d", diagName(im->diag), im->sw, im->sh);
		return true;
	}

	void EglPresenter::stop()
	{
		if (!impl) return;
		Impl* im = impl;
		impl = nullptr;
		if (im->fenceFd >= 0) { close(im->fenceFd); im->fenceFd = -1; }
		if (im->edpy != EGL_NO_DISPLAY && im->ctx != EGL_NO_CONTEXT)
			eglMakeCurrent(im->edpy, im->pbuffer == EGL_NO_SURFACE ? EGL_NO_SURFACE : im->pbuffer,
			               im->pbuffer == EGL_NO_SURFACE ? EGL_NO_SURFACE : im->pbuffer, im->ctx);
		for (auto& s : im->sources) if (im->edpy != EGL_NO_DISPLAY && im->destroyImage) im->destroySource(s);
		im->sources.clear();
		for (auto& s : im->slot)
		{
			if (s.fbo) glDeleteFramebuffers(1, &s.fbo);
			if (s.tex) glDeleteTextures(1, &s.tex);
			if (s.image != EGL_NO_IMAGE_KHR && im->edpy != EGL_NO_DISPLAY && im->destroyImage) im->destroyImage(im->edpy, s.image);
			if (s.fb && im->d->drmFd() >= 0 && (im->allowRmfb || s.state != Impl::Current))
				drmModeRmFB(im->d->drmFd(), s.fb);	// never while the plane may still scan it
			if (s.bo) gbm_bo_destroy(s.bo);
			s.fb = 0;
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		if (im->diagFbo) { glDeleteFramebuffers(1, &im->diagFbo); im->diagFbo = 0; }
		if (im->diagTex) { glDeleteTextures(1, &im->diagTex); im->diagTex = 0; }
		for (auto& t : im->diagTargets) im->destroyTarget(t);
		if (im->vbo) glDeleteBuffers(1, &im->vbo);
		if (im->program) glDeleteProgram(im->program);
		if (im->edpy != EGL_NO_DISPLAY)
		{
			eglMakeCurrent(im->edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			if (im->diagPbuf != EGL_NO_SURFACE) eglDestroySurface(im->edpy, im->diagPbuf);
			if (im->pbuffer != EGL_NO_SURFACE) eglDestroySurface(im->edpy, im->pbuffer);
			if (im->ctx != EGL_NO_CONTEXT) eglDestroyContext(im->edpy, im->ctx);
			eglTerminate(im->edpy);
		}
		if (im->gbm) gbm_device_destroy(im->gbm);
		if (im->gbmFd >= 0) close(im->gbmFd);
		delete im;
	}

	void EglPresenter::quiesce(bool scanoutCleared) { if (impl) impl->allowRmfb = scanoutCleared; }

	bool EglPresenter::scanning() const { return impl && impl->current; }

	bool EglPresenter::render(ClientBuffer& b, uint32_t& fb, std::string& err)
	{
		Impl* im = impl;
		if (!im) { err = "EGL presenter is stopped"; return false; }
		if (im->diag != DiagVariant::None)
		{
			if (b.w != (uint32_t)im->sh || b.h != (uint32_t)im->sw) { err = "the source is not 1240x1080"; return false; }
			return im->renderDiag(b, err);
		}
		Impl::Source fresh = {};
		Impl::Source* src = nullptr;
		if (im->freshSource)
		{
			if (!im->sourceFresh(b, fresh, err)) return false;
			src = &fresh;
		}
		else
		{
			src = im->source(b, err);
			if (!src) return false;
		}
		Impl::Slot* s = im->acquire();
		if (!s)
		{
			err = "no free scanout slot";
			if (im->freshSource) im->dropFresh(fresh);
			return false;
		}
		s->state = Impl::Rendering;
		glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
		glViewport(0, 0, im->sw, im->sh);
		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		// any source size: letterboxed into the 1240x1080 logical output, then turned (ui/src/rotate.h)
		uint32_t vx, vy, vw, vh;
		fitRotated(b.w, b.h, (uint32_t)im->sh, (uint32_t)im->sw, vx, vy, vw, vh);
		if (b.w != im->lastW || b.h != im->lastH)
		{
			logf("source %ux%u -> viewport %u,%u %ux%u", b.w, b.h, vx, vy, vw, vh);
			im->lastW = b.w;
			im->lastH = b.h;
		}
		glViewport((GLint)vx, (GLint)vy, (GLsizei)vw, (GLsizei)vh);
		glUseProgram(im->program);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, src->tex);
		glUniform1i(im->uTex, 0);
		glBindBuffer(GL_ARRAY_BUFFER, im->vbo);
		glEnableVertexAttribArray(im->aPos);
		glVertexAttribPointer(im->aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
		glEnableVertexAttribArray(im->aUv);
		glVertexAttribPointer(im->aUv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
		if (im->frames < kTraceFrames)
			tracef("frame %u: draw begin serial=%llu slot fb=%u", im->frames + 1,
			       (unsigned long long)b.serial, s->fb);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		// 3F: hand the GPU completion to KMS as a sync file instead of waiting here; the source and slot stay
		// referenced until the flip. glFinish remains the fallback and is required for the fresh-source diagnostic.
		bool fenced = false;
		if (im->fenceOk && !im->freshSource)
		{
			EGLSyncKHR sync = im->createSync(im->edpy, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
			if (sync != EGL_NO_SYNC_KHR)
			{
				glFlush();
				im->fenceFd = im->dupFence(im->edpy, sync);
				im->destroySync(im->edpy, sync);
				fenced = im->fenceFd >= 0;
			}
		}
		if (!fenced) glFinish();
		if (im->frames < kTraceFrames)
			tracef("frame %u: sync done serial=%llu fence=%d", im->frames + 1, (unsigned long long)b.serial, im->fenceFd);
		GLenum e = glGetError();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		if (im->freshSource) im->dropFresh(fresh);	// only after glFinish: the GPU is done reading it
		if (e != GL_NO_ERROR)
		{
			s->state = Impl::Free;	// no GPU work remains after glFinish
			if (im->fenceFd >= 0) { close(im->fenceFd); im->fenceFd = -1; }
			err = "GL error 0x" + std::to_string(e);
			return false;
		}
		s->state = Impl::Rendered;
		im->rendered = s;
		if (++im->frames <= 3 || im->frames % 60 == 0) logf("frame %u: slot fb=%u", im->frames, s->fb);
		im->dump(*s);
		if (!im->freshSource) im->dumpPair(*s, src->tex, (int)b.w, (int)b.h);	// a fresh source is already gone
		fb = s->fb;
		return true;
	}

	int EglPresenter::takeFenceFd()
	{
		if (!impl) return -1;
		int fd = impl->fenceFd;
		impl->fenceFd = -1;
		return fd;
	}

	void EglPresenter::presented()
	{
		if (!impl || !impl->rendered) return;
		Impl::Slot* s = impl->rendered;
		impl->rendered = nullptr;
		if (impl->current && impl->current != s) impl->current->state = Impl::Free;
		s->state = Impl::Current;
		s->flipPending = true;
		impl->current = s;
	}

	void EglPresenter::abandon()
	{
		if (!impl || !impl->rendered) return;
		impl->rendered->state = Impl::Free;
		impl->rendered = nullptr;
	}

	void EglPresenter::flipDone()
	{
		if (impl && impl->current) impl->current->flipPending = false;
	}

	void EglPresenter::detached()
	{
		if (!impl) return;
		if (impl->current) impl->current->state = Impl::Free;
		impl->current = nullptr;
	}

	void EglPresenter::forget(ClientBuffer& b)
	{
		if (!impl) return;
		for (size_t i = 0; i < impl->sources.size(); i++)
			if (impl->sources[i].b == &b)
			{
				if (b.serial <= 16)	// per-buffer lifecycle, not per-frame: at most the first 16 buffers
					tracef("source: cached destroy serial=%llu image=%p tex=%u",
					       (unsigned long long)b.serial, (void*)impl->sources[i].image, impl->sources[i].tex);
				impl->destroySource(impl->sources[i]);
				impl->sources.erase(impl->sources.begin() + i);
				break;
			}
		b.eglImage = nullptr;
		b.tex = 0;
	}
}
