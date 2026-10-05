#pragma once

#include <EGL/egl.h>
#include <android/native_window.h>

namespace eglctx
{
	bool Init(ANativeWindow* Window);
	bool Relayout(int W, int H);
	void MakeCurrent();
	void Swap();
	void Shutdown();
	int Width();
	int Height();
}
