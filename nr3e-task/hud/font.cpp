#include "font.hpp"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>

extern "C"
{
	extern const unsigned char _binary_cjk_ttf_start[];
	extern const unsigned char _binary_cjk_ttf_end[];
}

namespace font
{
	namespace
	{
		constexpr float kPx = 22.f;

		ImFont* FromFile(ImGuiIO& Io, const char* Path, const ImFontConfig& Cfg,
			const ImWchar* Ranges)
		{
			if (access(Path, R_OK) != 0)
				return nullptr;
			ImFontConfig Local = Cfg;
			Local.FontDataOwnedByAtlas = true;
			ImFont* F = Io.Fonts->AddFontFromFileTTF(Path, kPx, &Local, Ranges);
			if (F)
				std::fprintf(stderr, "[font] file %s size=%.0f\n", Path, kPx);
			return F;
		}
	}

	void Load(ImGuiIO& Io)
	{
		ImFontConfig Cfg{};
		Cfg.OversampleH = 2;
		Cfg.OversampleV = 2;
		Cfg.PixelSnapH = true;
		const ImWchar* Ranges = Io.Fonts->GetGlyphRangesChineseSimplifiedCommon();

		ImFont* Font = nullptr;

		const unsigned char* Begin = _binary_cjk_ttf_start;
		const unsigned char* End = _binary_cjk_ttf_end;
		const int Bytes = static_cast<int>(End - Begin);
		if (Bytes > 100)
		{
			static std::vector<unsigned char> Keep;
			Keep.assign(Begin, End);
			Cfg.FontDataOwnedByAtlas = false;
			Font = Io.Fonts->AddFontFromMemoryTTF(Keep.data(), static_cast<int>(Keep.size()),
				kPx, &Cfg, Ranges);
			if (Font)
				std::fprintf(stderr, "[font] embedded CJK %d bytes\n", Bytes);
		}

		if (!Font)
		{
			const char* Paths[] = {
				"/data/local/tmp/nr3e_cjk.ttf",
				"/system/fonts/ZUKChinese.ttf",
				"/system/fonts/DroidSansFallback.ttf",
				"/system/fonts/Roboto-Regular.ttf",
			};
			for (const char* P : Paths)
			{
				Font = FromFile(Io, P, Cfg, Ranges);
				if (Font)
					break;
			}
		}

		if (!Font)
		{
			std::fprintf(stderr, "[font] fallback default (no CJK)\n");
			Io.Fonts->AddFontDefault();
		}
	}
}
