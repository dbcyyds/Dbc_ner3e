#pragma once

#include <cstddef>
#include <string>

namespace nr3e::obf
{
	constexpr char kXor = 0x5A;

	template<std::size_t N>
	struct Lit
	{
		char Enc[N]{};
		constexpr Lit(const char (&S)[N])
		{
			for (std::size_t i = 0; i < N; ++i)
				Enc[i] = static_cast<char>(S[i] ^ static_cast<char>(kXor + (i & 0x1F)));
		}
		[[nodiscard]] std::string Dec() const
		{
			std::string O(N ? N - 1 : 0, '\0');
			for (std::size_t i = 0; i + 1 < N; ++i)
				O[i] = static_cast<char>(Enc[i] ^ static_cast<char>(kXor + (i & 0x1F)));
			return O;
		}
	};

	template<Lit L>
	const char* Get()
	{
		static const std::string S = L.Dec();
		return S.c_str();
	}

	inline const char* Blob(const unsigned char* Enc, std::size_t N)
	{
		static thread_local char Buf[128];
		if (N >= sizeof(Buf))
			N = sizeof(Buf) - 1;
		for (std::size_t i = 0; i < N; ++i)
			Buf[i] = static_cast<char>(Enc[i] ^ static_cast<char>(kXor + (i & 0x1F)));
		Buf[N] = 0;
		return Buf;
	}
}

#define XS(s) (::nr3e::obf::Get<::nr3e::obf::Lit(s)>())
