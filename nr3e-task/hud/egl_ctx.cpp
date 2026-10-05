#include "egl_ctx.hpp"
#include "a_native_window_creator.h"

#include <android/native_window.h>
#include <cstdlib>

namespace eglctx
{
	namespace
	{
		EGLDisplay g_Dpy = EGL_NO_DISPLAY;
		EGLSurface g_Surf = EGL_NO_SURFACE;
		EGLContext g_Ctx = EGL_NO_CONTEXT;
		EGLConfig g_Cfg{};
		ANativeWindow* g_Win = nullptr;
		int g_W = 0, g_H = 0;

		bool BindWindow(ANativeWindow* Window)
		{
			if (!Window)
				return false;
			g_Win = Window;
			if (g_Surf != EGL_NO_SURFACE)
			{
				eglMakeCurrent(g_Dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
				eglDestroySurface(g_Dpy, g_Surf);
				g_Surf = EGL_NO_SURFACE;
			}
			g_Surf = eglCreateWindowSurface(g_Dpy, g_Cfg,
				reinterpret_cast<EGLNativeWindowType>(Window), nullptr);
			if (g_Surf == EGL_NO_SURFACE)
				return false;
			if (!eglMakeCurrent(g_Dpy, g_Surf, g_Surf, g_Ctx))
				return false;
			eglQuerySurface(g_Dpy, g_Surf, EGL_WIDTH, &g_W);
			eglQuerySurface(g_Dpy, g_Surf, EGL_HEIGHT, &g_H);
			return g_W > 1 && g_H > 1;
		}
	}

	bool Init(ANativeWindow* Window)
	{
		g_Dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		if (g_Dpy == EGL_NO_DISPLAY)
			return false;
		if (!eglInitialize(g_Dpy, nullptr, nullptr))
			return false;
		const EGLint Attr[] = {
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
			EGL_NONE};
		EGLint N = 0;
		if (!eglChooseConfig(g_Dpy, Attr, &g_Cfg, 1, &N) || N < 1)
			return false;
		const EGLint CtxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
		g_Ctx = eglCreateContext(g_Dpy, g_Cfg, EGL_NO_CONTEXT, CtxAttr);
		if (g_Ctx == EGL_NO_CONTEXT)
			return false;
		return BindWindow(Window);
	}

	bool Relayout(int W, int H)
	{
		if (W < 64 || H < 64 || g_Dpy == EGL_NO_DISPLAY)
			return false;
		if (std::abs(g_W - W) < 12 && std::abs(g_H - H) < 12)
			return true;
		if (g_Win)
		{
			android::ANativeWindowCreator::Destroy(g_Win);
			g_Win = nullptr;
		}
		android::ANativeWindowCreator::CreateOptions Opt{};
		Opt.name = "BackgroundSticky";
		Opt.width = W;
		Opt.height = H;
		Opt.skipScreenshot = true;
		ANativeWindow* Nw = android::ANativeWindowCreator::Create(Opt);
		return BindWindow(Nw);
	}

	void MakeCurrent()
	{
		eglMakeCurrent(g_Dpy, g_Surf, g_Surf, g_Ctx);
	}

	void Swap()
	{
		eglSwapBuffers(g_Dpy, g_Surf);
	}

	void Shutdown()
	{
		if (g_Dpy != EGL_NO_DISPLAY)
		{
			eglMakeCurrent(g_Dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			if (g_Surf != EGL_NO_SURFACE)
				eglDestroySurface(g_Dpy, g_Surf);
			if (g_Ctx != EGL_NO_CONTEXT)
				eglDestroyContext(g_Dpy, g_Ctx);
			eglTerminate(g_Dpy);
		}
		if (g_Win)
		{
			android::ANativeWindowCreator::Destroy(g_Win);
			g_Win = nullptr;
		}
		g_Dpy = EGL_NO_DISPLAY;
		g_Surf = EGL_NO_SURFACE;
		g_Ctx = EGL_NO_CONTEXT;
	}

	int Width() { return g_W; }
	int Height() { return g_H; }
}
