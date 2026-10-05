#include "touch.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <vector>

namespace touch
{
	namespace
	{
		struct Finger
		{
			float X = 0.f;
			float Y = 0.f;
			bool Down = false;
		};

		struct Dev
		{
			int Fd = -1;
			input_absinfo AbsX{};
			input_absinfo AbsY{};
			Finger Slot[10]{};
			int Cur = 0;
		};

		std::mutex g_Mu;
		std::vector<Dev> g_Devs;
		std::atomic<bool> g_Run{false};
		std::atomic<int> g_Orient{0};
		float g_W = 1080.f;
		float g_H = 1920.f;
		float g_X = 0.f;
		float g_Y = 0.f;
		bool g_Down = false;
		bool g_Prev = false;
		pthread_t g_Th{};
		bool g_HaveTh = false;
		bool g_DigPortrait = true;

		float clampf(float V, float A, float B)
		{
			return V < A ? A : (V > B ? B : V);
		}

		float Norm(float V, const input_absinfo& A)
		{
			const float Lo = static_cast<float>(A.minimum);
			const float Hi = static_cast<float>(A.maximum);
			const float Span = Hi - Lo;
			if (Span <= 1.f)
				return 0.f;
			return clampf((V - Lo) / Span, 0.f, 1.f);
		}

		void Map(float Nx, float Ny, float& Ox, float& Oy)
		{
			int O = g_Orient.load(std::memory_order_relaxed) & 3;
			const bool BufLand = g_W > g_H;
			if (g_DigPortrait && BufLand && (O == 0 || O == 2))
				O = (O == 0) ? 1 : 3;
			if (g_DigPortrait && !BufLand && (O == 1 || O == 3))
				O = (O == 1) ? 0 : 2;
			if (!g_DigPortrait && BufLand && (O == 1 || O == 3))
				O = (O == 1) ? 0 : 2;
			if (!g_DigPortrait && !BufLand && (O == 0 || O == 2))
				O = (O == 0) ? 3 : 1;

			switch (O)
			{
			case 1:
				Ox = Ny * g_W;
				Oy = (1.f - Nx) * g_H;
				break;
			case 2:
				Ox = (1.f - Nx) * g_W;
				Oy = (1.f - Ny) * g_H;
				break;
			case 3:
				Ox = (1.f - Ny) * g_W;
				Oy = Nx * g_H;
				break;
			default:
				Ox = Nx * g_W;
				Oy = Ny * g_H;
				break;
			}
			Ox = clampf(Ox, 0.f, g_W);
			Oy = clampf(Oy, 0.f, g_H);
		}

		bool IsTouch(int Fd)
		{
			unsigned long Bits[(ABS_MAX / 64) + 2]{};
			if (ioctl(Fd, EVIOCGBIT(EV_ABS, sizeof(Bits)), Bits) < 0)
				return false;
			auto Has = [&](int Code) {
				return (Bits[Code / 64] >> (Code % 64)) & 1UL;
			};
			return Has(ABS_MT_POSITION_X) && Has(ABS_MT_POSITION_Y);
		}

		void PublishLocked(Dev& D)
		{
			bool Any = false;
			for (auto& F : D.Slot)
			{
				if (!F.Down)
					continue;
				float Ox = 0.f, Oy = 0.f;
				Map(Norm(F.X, D.AbsX), Norm(F.Y, D.AbsY), Ox, Oy);
				g_X = Ox;
				g_Y = Oy;
				g_Down = true;
				Any = true;
				break;
			}
			if (!Any)
				g_Down = false;
		}

		void* Loop(void*)
		{
			while (g_Run.load(std::memory_order_relaxed))
			{
				std::vector<pollfd> Pf;
				{
					std::lock_guard<std::mutex> L(g_Mu);
					for (auto& D : g_Devs)
					{
						if (D.Fd >= 0)
							Pf.push_back(pollfd{D.Fd, POLLIN, 0});
					}
				}
				if (Pf.empty())
					break;
				if (poll(Pf.data(), Pf.size(), 200) <= 0)
					continue;
				for (size_t i = 0; i < Pf.size(); ++i)
				{
					if (!(Pf[i].revents & POLLIN))
						continue;
					input_event Ev[64]{};
					const ssize_t N = read(Pf[i].fd, Ev, sizeof(Ev));
					if (N <= 0 || (N % static_cast<ssize_t>(sizeof(input_event))) != 0)
						continue;
					const size_t C = static_cast<size_t>(N) / sizeof(input_event);
					std::lock_guard<std::mutex> L(g_Mu);
					if (i >= g_Devs.size())
						continue;
					Dev& D = g_Devs[i];
					for (size_t k = 0; k < C; ++k)
					{
						const auto& E = Ev[k];
						if (E.type == EV_ABS)
						{
							if (E.code == ABS_MT_SLOT)
							{
								D.Cur = std::clamp(E.value, 0, 9);
								continue;
							}
							if (E.code == ABS_MT_TRACKING_ID)
							{
								D.Slot[D.Cur].Down = E.value != -1;
								continue;
							}
							if (E.code == ABS_MT_POSITION_X || E.code == ABS_X)
							{
								D.Slot[D.Cur].X = static_cast<float>(E.value);
								continue;
							}
							if (E.code == ABS_MT_POSITION_Y || E.code == ABS_Y)
							{
								D.Slot[D.Cur].Y = static_cast<float>(E.value);
								continue;
							}
						}
						else if (E.type == EV_KEY && E.code == BTN_TOUCH)
						{
							if (E.value == 0)
								D.Slot[D.Cur].Down = false;
							else
								D.Slot[D.Cur].Down = true;
						}
						else if (E.type == EV_SYN && E.code == SYN_REPORT)
						{
							PublishLocked(D);
						}
					}
				}
			}
			return nullptr;
		}
	}

	bool Init(float ScreenW, float ScreenH)
	{
		Shutdown();
		SetDisplay(ScreenW, ScreenH, 0);
		DIR* Dir = opendir("/dev/input");
		if (!Dir)
			return false;
		while (dirent* E = readdir(Dir))
		{
			if (std::strncmp(E->d_name, "event", 5) != 0)
				continue;
			char Path[64];
			std::snprintf(Path, sizeof(Path), "/dev/input/%s", E->d_name);
			int Fd = open(Path, O_RDONLY | O_NONBLOCK);
			if (Fd < 0)
				Fd = open(Path, O_RDWR | O_NONBLOCK);
			if (Fd < 0)
				continue;
			if (!IsTouch(Fd))
			{
				close(Fd);
				continue;
			}
			Dev D{};
			D.Fd = Fd;
			if (ioctl(Fd, EVIOCGABS(ABS_MT_POSITION_X), &D.AbsX) != 0
				|| ioctl(Fd, EVIOCGABS(ABS_MT_POSITION_Y), &D.AbsY) != 0)
			{
				close(Fd);
				continue;
			}
			g_Devs.push_back(D);
		}
		closedir(Dir);
		if (g_Devs.empty())
			return false;
		const auto& A = g_Devs[0];
		g_DigPortrait = (A.AbsX.maximum - A.AbsX.minimum)
			< (A.AbsY.maximum - A.AbsY.minimum);
		std::fprintf(stderr, "[touch] devices=%zu dig=%s absX=%d..%d absY=%d..%d\n",
			g_Devs.size(), g_DigPortrait ? "portrait" : "landscape",
			A.AbsX.minimum, A.AbsX.maximum, A.AbsY.minimum, A.AbsY.maximum);
		g_Run.store(true);
		g_HaveTh = pthread_create(&g_Th, nullptr, Loop, nullptr) == 0;
		return true;
	}

	void SetDisplay(float ScreenW, float ScreenH, int Orient)
	{
		std::lock_guard<std::mutex> L(g_Mu);
		if (ScreenW > 1.f)
			g_W = ScreenW;
		if (ScreenH > 1.f)
			g_H = ScreenH;
		g_Orient.store(Orient & 3, std::memory_order_relaxed);
	}

	State Snapshot()
	{
		State S;
		std::lock_guard<std::mutex> L(g_Mu);
		S.X = g_X;
		S.Y = g_Y;
		S.Down = g_Down;
		S.Pressed = g_Down && !g_Prev;
		S.Released = !g_Down && g_Prev;
		g_Prev = g_Down;
		return S;
	}

	void Shutdown()
	{
		g_Run.store(false);
		{
			std::lock_guard<std::mutex> L(g_Mu);
			for (auto& D : g_Devs)
			{
				if (D.Fd >= 0)
				{
					close(D.Fd);
					D.Fd = -1;
				}
			}
		}
		if (g_HaveTh)
		{
			pthread_join(g_Th, nullptr);
			g_HaveTh = false;
			g_Th = {};
		}
		std::lock_guard<std::mutex> L(g_Mu);
		g_Devs.clear();
		g_Down = false;
	}
}
