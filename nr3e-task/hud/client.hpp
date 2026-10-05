#pragma once

#include <cstdint>
#include <string>

namespace client
{
	struct Reply
	{
		std::string Error;
		std::string Message;
		int32_t Enabled = 0;
		int32_t Touched = 0;
		bool Ok = false;
	};

	Reply Call(int32_t Cmd, int32_t Extra);
	void Warmup();
}
