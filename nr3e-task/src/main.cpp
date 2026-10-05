#include "nr3e/hide.hpp"
#include "nr3e/ipc.hpp"
#include "nr3e/kpm.h"
#include "nr3e/log.hpp"
#include "nr3e/ue.hpp"

#include <dlfcn.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <chrono>
#include <thread>

namespace
{
	void* Boot(void*)
	{
		prctl(PR_SET_NAME, "Runtime worker", 0, 0, 0);
		nr3e_xcall(NR3E_CMD_BIND, 0, 0, 0, 0);
		nr3e::hide::HideCurrentTid();
		nr3e::hide::CoverCurrentStack();
		for (int i = 0; i < 80; ++i)
		{
			if (nr3e::ue::Init())
			{
				nr3e::ue::SetGamePump(&nr3e::ipc::OnGameTick);
				nr3e::ue::HookProcessEvent();
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		}
		nr3e::ipc::ServeForever();
		return nullptr;
	}
}

__attribute__((constructor))
static void Nr3eCtor()
{
	Dl_info Inf{};
	if (dladdr(reinterpret_cast<void*>(&Nr3eCtor), &Inf) && Inf.dli_fname)
		dlopen(Inf.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
	pthread_t Th{};
	pthread_attr_t Attr{};
	pthread_attr_init(&Attr);
	pthread_attr_setdetachstate(&Attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&Attr, 4u * 1024u * 1024u);
	if (pthread_create(&Th, &Attr, Boot, nullptr) != 0)
		NRLOGE("pthread_create failed");
	pthread_attr_destroy(&Attr);
}
