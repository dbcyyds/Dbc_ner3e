#include "a_native_window_creator.h"
#include "display.hpp"
#include "egl_ctx.hpp"
#include "nr3e/kpm.h"
#include "touch.hpp"

#include <android/native_window.h>
#include <cstdio>
#include <cstring>
#include <sys/prctl.h>
#include <unistd.h>

namespace overlay
{
	bool Run();
}

int main(int argc, char** argv)
{
	prctl(PR_SET_NAME, "update_verifier", 0, 0, 0);
	nr3e_xcall(NR3E_CMD_HIDEPID, (unsigned long)getpid(), 0, 0, 0);
	nr3e_xcall(NR3E_CMD_HIDETID, (unsigned long)gettid(), 0, 0, 0);
	if (argc > 0 && argv[0])
	{
		const size_t N = std::strlen(argv[0]);
		std::memset(argv[0], 0, N);
		const char* Fake = "update_verifier";
		const size_t M = std::strlen(Fake);
		std::memcpy(argv[0], Fake, M < N ? M : N);
	}
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
	android::ANativeWindowCreator::CreateOptions Opt{};
	Opt.name = "BackgroundSticky";
	Opt.width = 0;
	Opt.height = 0;
	Opt.skipScreenshot = true;
	ANativeWindow* Win = android::ANativeWindowCreator::Create(Opt);
	if (!Win)
	{
		std::fprintf(stderr, "native window create failed\n");
		return 1;
	}
	if (!eglctx::Init(Win))
	{
		std::fprintf(stderr, "egl init failed\n");
		return 1;
	}
	const int W = eglctx::Width();
	const int H = eglctx::Height();
	const auto D = disp::Query();
	int Orient = D.Orient;
	if (W > H && Orient == 0)
		Orient = 1;
	if (W > H && Orient == 2)
		Orient = 3;
	std::fprintf(stderr, "overlay %dx%d sf=%dx%d theta_orient=%d use=%d\n",
		W, H, D.W, D.H, D.Orient, Orient);
	if (!touch::Init(static_cast<float>(W), static_cast<float>(H)))
		std::fprintf(stderr, "touch init failed (overlay still draws)\n");
	touch::SetDisplay(static_cast<float>(W), static_cast<float>(H), Orient);
	overlay::Run();
	touch::Shutdown();
	eglctx::Shutdown();
	return 0;
}
