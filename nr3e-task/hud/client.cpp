#include "client.hpp"
#include "nr3e/kpm.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace client
{
	namespace
	{
		bool PollRaw(nr3e_kreply& Rep, long& Rc)
		{
			std::memset(&Rep, 0, sizeof(Rep));
			Rc = nr3e_xcall(NR3E_CMD_POLL, reinterpret_cast<unsigned long>(&Rep), 0, 0, 0);
			return Rc == 2 || Rep.error[0] || Rep.message[0];
		}

		void Drain()
		{
			for (int i = 0; i < 8; ++i)
			{
				nr3e_kreply Rep {};
				long Rc = 0;
				if (!PollRaw(Rep, Rc))
					break;
			}
		}

		int TaggedCmd(const char* S)
		{
			if (!S || !S[0])
				return -1;
			int Cmd = -1;
			char Colon = 0;
			if (std::sscanf(S, "%d%c", &Cmd, &Colon) != 2 || Colon != ':')
				return -1;
			return Cmd;
		}

		const char* AfterTag(const char* S)
		{
			const char* P = std::strchr(S, ':');
			return P ? P + 1 : S;
		}

		bool MatchCmd(const nr3e_kreply& Rep, int32_t Cmd)
		{
			return TaggedCmd(Rep.message) == Cmd || TaggedCmd(Rep.error) == Cmd;
		}
	}

	void Warmup()
	{
		(void)nr3e_kping();
		Drain();
	}

	Reply Call(int32_t Cmd, int32_t Extra)
	{
		Reply R;
		if (nr3e_kping() != NR3E_MAGIC)
		{
			R.Error = "内核邮箱未就绪";
			return R;
		}
		Drain();
		long Post = nr3e_xcall(NR3E_CMD_POST, (unsigned long)Cmd, (unsigned long)Extra, 0, 0);
		for (int t = 0; t < 4 && Post < 0; ++t)
		{
			Drain();
			Post = nr3e_xcall(NR3E_CMD_POST, (unsigned long)Cmd, (unsigned long)Extra, 0, 0);
		}
		if (Post == -22)
		{
			R.Error = "游戏未BIND（SO未起来）";
			return R;
		}
		if (Post == -16)
		{
			R.Error = "上一命令还在跑";
			return R;
		}
		if (Post < 0)
		{
			char B[48];
			std::snprintf(B, sizeof(B), "POST失败 %ld", Post);
			R.Error = B;
			return R;
		}
		for (int i = 0; i < 400; ++i)
		{
			nr3e_kreply Rep {};
			long Rc = 0;
			if (PollRaw(Rep, Rc) && MatchCmd(Rep, Cmd))
			{
				R.Error = AfterTag(Rep.error);
				R.Message = AfterTag(Rep.message);
				R.Enabled = Rep.enabled;
				R.Touched = Rep.touched;
				R.Ok = R.Error.empty();
				return R;
			}
			usleep(20 * 1000);
		}
		R.Error = "等待超时（游戏未接命令）";
		return R;
	}
}
