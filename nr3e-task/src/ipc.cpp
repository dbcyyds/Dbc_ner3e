#include "nr3e/ipc.hpp"
#include "nr3e/kpm.h"
#include "nr3e/task.hpp"
#include "nr3e/ue.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <time.h>

namespace nr3e::ipc
{
	namespace
	{
		std::atomic<int> g_Have{0};
		std::atomic<int> g_Done{0};
		nr3e_take g_Take{};
		char g_Err[192]{};
		char g_Msg[192]{};
		int32_t g_En = 0;
		int32_t g_Touched = 0;

		void ReplyNow(int32_t Cmd, const task::Reply& R)
		{
			std::snprintf(g_Err, sizeof(g_Err), "%d:%s", Cmd, R.Error.c_str());
			std::snprintf(g_Msg, sizeof(g_Msg), "%d:%s", Cmd, R.Message.c_str());
			nr3e_xcall(NR3E_CMD_REPLY, (unsigned long)(R.Enabled ? 1 : 0),
				(unsigned long)R.Touched, reinterpret_cast<unsigned long>(g_Err),
				reinterpret_cast<unsigned long>(g_Msg));
		}

		void ExecTake(const nr3e_take& Take)
		{
			if (Take.cmd == 99 && !ue::Ready())
				ue::Init();
			if (ue::Ready())
				ue::HookProcessEvent();
			const auto R = task::Run(Take.cmd, Take.extra);
			ReplyNow(Take.cmd, R);
		}
	}

	void OnGameTick()
	{
		if (g_Have.load(std::memory_order_acquire) == 0)
		{
			nr3e::task::Pump();
			return;
		}
		nr3e_take Take = g_Take;
		g_Have.store(0, std::memory_order_release);
		ExecTake(Take);
		g_Done.store(1, std::memory_order_release);
		nr3e::task::Pump();
	}

	void ServeForever()
	{
		nr3e_xcall(NR3E_CMD_BIND, 0, 0, 0, 0);
		nr3e_xcall(NR3E_CMD_HIDETID, 0, 0, 0, 0);
		for (;;)
		{
			if (ue::Ready())
				ue::HookProcessEvent();
			nr3e_take Take{};
			Take.cmd = -1;
			(void)nr3e_xcall(NR3E_CMD_TAKE, reinterpret_cast<unsigned long>(&Take), 0, 0, 0);
			if (Take.cmd >= 0)
			{
				if (ue::PeHooked())
				{
					g_Done.store(0, std::memory_order_release);
					g_Take = Take;
					g_Have.store(1, std::memory_order_release);
					bool Ok = false;
					for (int i = 0; i < 400; ++i)
					{
						if (g_Done.load(std::memory_order_acquire))
						{
							Ok = true;
							break;
						}
						timespec Ts{0, 10 * 1000 * 1000};
						nanosleep(&Ts, nullptr);
					}
					if (!Ok)
					{
						g_Have.store(0, std::memory_order_release);
						ExecTake(Take);
					}
				}
				else
					ExecTake(Take);
				continue;
			}
			timespec Ts{0, 8 * 1000 * 1000};
			nanosleep(&Ts, nullptr);
		}
	}
}
