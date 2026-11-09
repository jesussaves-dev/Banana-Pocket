#include "egl_presenter.h"
#include "client_buffer.h"
#include "kms.h"
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
		void logf(const char* fmt, ...)
		{
			va_list ap;
			va_start(ap, fmt);
			fputs("egl: ", stderr);
			vfprintf(stderr, fmt, ap);
			fputc('\n', stderr);
			va_end(ap);
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
		GLuint program = 0, vbo = 0;
		GLint aPos = -1, aUv = -1, uTex = -1;
		Slot slot[3];
		Slot* current = nullptr;
		Slot* rendered = nullptr;
		std::vector<Source> sources;
		std::string dumpPath;
		bool dumpDone = false;
		bool allowRmfb = false;		// set by quiesce() when the primary no longer scans a slot
		unsigned frames = 0;
		int sw = 0, sh = 0;	// the panel: 1080x1240

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
		Source* source(ClientBuffer& b, std::string& err)
		{
			for (Source& s : sources)
				if (s.b == &b) return &s;
			if (b.fd < 0) { err = "the source fd is gone"; return nullptr; }
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
				return nullptr;
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
				glDeleteTextures(1, &tex);
				destroyImage(edpy, img);
				err = "binding the source EGLImage failed";
				return nullptr;
			}
			close(b.fd);
			b.fd = -1;
			b.eglImage = img;
			b.tex = tex;
			sources.push_back({ &b, img, tex });
			logf("source: %ux%u fmt=%.4s mod=0x%016llx image ok", b.w, b.h, (const char*)&b.format, (unsigned long long)b.modifier);
			tracef("source: image ok %ux%u", b.w, b.h);
			return &sources.back();
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
				uint32_t handle = 0;
				if (drmPrimeFDToHandle(d->drmFd(), fd, &handle)) { close(fd); err = std::string("drmPrimeFDToHandle: ") + strerror(errno); return false; }
				uint32_t handles[4] = { handle }, pitches[4] = { stride }, offsets[4] = { off };
				uint64_t m[4] = { modifier };
				int ret = drmModeAddFB2WithModifiers(d->drmFd(), (uint32_t)sw, (uint32_t)sh, format, handles, pitches, offsets, m, &s.fb, DRM_MODE_FB_MODIFIERS);
				drmCloseBufferHandle(d->drmFd(), handle);
				if (ret) { close(fd); err = std::string("AddFB2WithModifiers: ") + strerror(errno); return false; }
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
				close(fd);
				s.state = Free;
				logf("slot: %dx%d XR24 LINEAR stride=%u offset=%u fb=%u", sw, sh, stride, off, s.fb);
				tracef("slot: fb=%u stride=%u", s.fb, stride);
			}
			return true;
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

	EglPresenter::EglPresenter(Display& d)
	{
		impl = new Impl();
		impl->d = &d;
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

		im->gbmFd = dup(im->d->drmFd());
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
		EGLint cfgAttrs[] = {
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
			EGL_NONE
		};
		EGLint n = 0;
		if (!eglChooseConfig(im->edpy, cfgAttrs, &im->cfg, 1, &n) || n < 1)
		{
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
		if (surfaceless)
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
		if (!im->createImage || !im->destroyImage || !im->targetTexture) { err = "EGLImage entry points missing"; return false; }
		const char* glExt = (const char*)glGetString(GL_EXTENSIONS);
		logf("%s / %s", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
		logf("extensions: dma_buf_import=%d modifiers=%d fence_sync=%d native_fence=%d surfaceless=%d OES_EGL_image=%d",
		     hasExt(eglExt, "EGL_EXT_image_dma_buf_import"), hasExt(eglExt, "EGL_EXT_image_dma_buf_import_modifiers"),
		     hasExt(eglExt, "EGL_KHR_fence_sync"), hasExt(eglExt, "EGL_ANDROID_native_fence_sync"),
		     surfaceless, hasExt(glExt, "GL_OES_EGL_image"));
		if (!hasExt(eglExt, "EGL_EXT_image_dma_buf_import")) { err = "EGL_EXT_image_dma_buf_import missing"; return false; }
		if (!hasExt(glExt, "GL_OES_EGL_image")) { err = "GL_OES_EGL_image missing"; return false; }

		std::string serr;
		GLuint vs = compile(GL_VERTEX_SHADER,
			"attribute vec2 aPos; attribute vec2 aUv; varying vec2 vUv;\n"
			"void main() { vUv = aUv; gl_Position = vec4(aPos, 0.0, 1.0); }\n", serr);
		GLuint fs = vs ? compile(GL_FRAGMENT_SHADER,
			"precision mediump float; varying vec2 vUv; uniform sampler2D uTex;\n"
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

		if (!im->addSlot(err)) return false;
		logf("ready: 3 slots, 1240x1080 -> %dx%d, %s", im->sw, im->sh, im->dumpPath.empty() ? "no readback" : "one-shot readback armed");
		return true;
	}

	void EglPresenter::stop()
	{
		if (!impl) return;
		Impl* im = impl;
		impl = nullptr;
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
		if (im->vbo) glDeleteBuffers(1, &im->vbo);
		if (im->program) glDeleteProgram(im->program);
		if (im->edpy != EGL_NO_DISPLAY)
		{
			eglMakeCurrent(im->edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			if (im->pbuffer != EGL_NO_SURFACE) eglDestroySurface(im->edpy, im->pbuffer);
			if (im->ctx != EGL_NO_CONTEXT) eglDestroyContext(im->edpy, im->ctx);
			eglTerminate(im->edpy);
		}
		if (im->gbm) gbm_device_destroy(im->gbm);
		if (im->gbmFd >= 0) close(im->gbmFd);
		delete im;
	}

	void EglPresenter::quiesce(bool scanoutCleared) { if (impl) impl->allowRmfb = scanoutCleared; }

	bool EglPresenter::render(ClientBuffer& b, uint32_t& fb, std::string& err)
	{
		Impl* im = impl;
		if (!im) { err = "EGL presenter is stopped"; return false; }
		if (b.w != (uint32_t)im->sh || b.h != (uint32_t)im->sw)
		{ err = "the source is not 1240x1080"; return false; }
		Impl::Source* src = im->source(b, err);
		if (!src) return false;
		Impl::Slot* s = im->acquire();
		if (!s) { err = "no free scanout slot"; return false; }
		s->state = Impl::Rendering;
		glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
		glViewport(0, 0, im->sw, im->sh);
		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glUseProgram(im->program);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, src->tex);
		glUniform1i(im->uTex, 0);
		glBindBuffer(GL_ARRAY_BUFFER, im->vbo);
		glEnableVertexAttribArray(im->aPos);
		glVertexAttribPointer(im->aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
		glEnableVertexAttribArray(im->aUv);
		glVertexAttribPointer(im->aUv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
		tracef("frame %u: draw begin slot fb=%u", im->frames + 1, s->fb);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glFinish();	// the conservative 3C rule: the source and the slot are complete before anything else
		tracef("frame %u: glFinish done", im->frames + 1);
		GLenum e = glGetError();
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		if (e != GL_NO_ERROR)
		{
			s->state = Impl::Free;	// no GPU work remains after glFinish
			err = "GL error 0x" + std::to_string(e);
			return false;
		}
		s->state = Impl::Rendered;
		im->rendered = s;
		if (++im->frames <= 3 || im->frames % 60 == 0) logf("frame %u: slot fb=%u", im->frames, s->fb);
		im->dump(*s);
		fb = s->fb;
		return true;
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
				impl->destroySource(impl->sources[i]);
				impl->sources.erase(impl->sources.begin() + i);
				break;
			}
		b.eglImage = nullptr;
		b.tex = 0;
	}
}
