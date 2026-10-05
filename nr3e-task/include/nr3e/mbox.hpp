#pragma once

#include <cstdint>

namespace nr3e::mbox
{
	constexpr uint32_t kMagic = 0x4533524E;

	struct Box
	{
		uint32_t Magic = kMagic;
		int32_t Cmd = -1; // -1 idle, >=0 request
		int32_t Extra = 0;
		int32_t Seq = 0;
		int32_t DoneSeq = 0;
		int32_t Enabled = 0;
		int32_t Touched = 0;
		char Error[192]{};
		char Message[192]{};
	};
}

extern "C" nr3e::mbox::Box nr3e_mbox;
