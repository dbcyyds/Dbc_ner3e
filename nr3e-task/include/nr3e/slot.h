#pragma once

#include "nr3e/obf.hpp"

#include <cstdint>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>

#define NR3E_SLOT_MAGIC 0x4E33510u

inline const char* nr3e_slot_dir(void)
{
	static char D[48];
	static bool Once = false;
	if (!Once)
	{
		static const unsigned char E[] = {0x75,0x3f,0x3d,0x29,0x3f,0x70,0x04,0x00,0x16,0x02,0x4b,0x06,0x09,0x0a,0x46,0x1d,0x0f,0x05,0x0f,0x08,0x00,0x1b,0x5e,0x1d,0x17,0x07,0x07,0x12,0x19,0x58,0x1e,0x10,0x36,0x3e,0x2f,0x72,0x33,0x32,0x0b,0x17};
		for (unsigned i = 0; i < sizeof(E); ++i)
			D[i] = static_cast<char>(E[i] ^ static_cast<char>(0x5A + (i & 0x1F)));
		D[sizeof(E)] = 0;
		Once = true;
	}
	return D;
}

inline const char* nr3e_slot_path(void)
{
	static char P[56];
	static bool Once = false;
	if (!Once)
	{
		static const unsigned char E[] = {0x75,0x3f,0x3d,0x29,0x3f,0x70,0x04,0x00,0x16,0x02,0x4b,0x06,0x09,0x0a,0x46,0x1d,0x0f,0x05,0x0f,0x08,0x00,0x1b,0x5e,0x1d,0x17,0x07,0x07,0x12,0x19,0x58,0x1e,0x10,0x36,0x3e,0x2f,0x72,0x33,0x32,0x0b,0x17,0x4d,0x4d,0x07,0x17,0x05};
		for (unsigned i = 0; i < sizeof(E); ++i)
			P[i] = static_cast<char>(E[i] ^ static_cast<char>(0x5A + (i & 0x1F)));
		P[sizeof(E)] = 0;
		Once = true;
	}
	return P;
}

struct Nr3eSlot
{
	uint32_t magic;
	uint32_t wseq;
	uint32_t rseq;
	int32_t cmd;
	int32_t extra;
	int32_t enabled;
	int32_t touched;
	char error[192];
	char message[192];
};

inline void nr3e_slot_unlink(void)
{
	unlink(nr3e_slot_path());
}

inline Nr3eSlot* nr3e_slot_map(bool Create)
{
	if (Create)
		mkdir(nr3e_slot_dir(), 0700);
	const int Fd = open(nr3e_slot_path(), Create ? (O_RDWR | O_CREAT) : O_RDWR, 0600);
	if (Fd < 0)
		return nullptr;
	if (Create)
	{
		fchmod(Fd, 0600);
		if (ftruncate(Fd, static_cast<off_t>(sizeof(Nr3eSlot))) != 0)
		{
			close(Fd);
			return nullptr;
		}
	}
	void* P = mmap(nullptr, sizeof(Nr3eSlot), PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0);
	close(Fd);
	if (P == MAP_FAILED)
		return nullptr;
	auto* S = static_cast<Nr3eSlot*>(P);
	if (Create && S->magic != NR3E_SLOT_MAGIC)
	{
		std::memset(S, 0, sizeof(*S));
		S->magic = NR3E_SLOT_MAGIC;
	}
	return S;
}
