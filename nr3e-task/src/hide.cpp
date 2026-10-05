#include "nr3e/hide.hpp"
#include "nr3e/log.hpp"
#include "nr3e/obf.hpp"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace nr3e::hide
{
	namespace
	{
		constexpr int PR_HIDEMAPS_PING = 0x484D0001;
		constexpr int PR_HIDEMAPS_ADD_KW = 0x484D0002;
		constexpr int PR_HIDEMAPS_ADD_RANGE = 0x484D0003;
		constexpr int PR_HIDEMAPS_ADD_TID = 0x484D0005;
		constexpr int HIDEMAPS_MAGIC = 0x484D;
		constexpr uintptr_t kPage = 0x1000;

		struct Vma
		{
			uintptr_t S = 0;
			uintptr_t E = 0;
			char Path[256]{};
		};

		uintptr_t PageDown(uintptr_t V) { return V & ~(kPage - 1); }
		uintptr_t PageUp(uintptr_t V) { return (V + kPage - 1) & ~(kPage - 1); }

		bool KernelOk()
		{
			return prctl(PR_HIDEMAPS_PING, 0, 0, 0, 0) == HIDEMAPS_MAGIC;
		}

		void AddKw(const char* S)
		{
			if (S && S[0])
				prctl(PR_HIDEMAPS_ADD_KW, reinterpret_cast<unsigned long>(S), 0, 0, 0);
		}

		void AddRange(uintptr_t S, uintptr_t E)
		{
			S = PageDown(S);
			E = PageUp(E);
			if (E <= S)
				return;
			prctl(PR_HIDEMAPS_ADD_RANGE, S, E, 0, 0);
		}

		std::vector<Vma> ReadMaps()
		{
			std::vector<Vma> Out;
			FILE* Fp = fopen("/proc/self/maps", "re");
			if (!Fp)
				return Out;
			char Line[512];
			while (fgets(Line, sizeof(Line), Fp))
			{
				Vma V;
				unsigned long S = 0, E = 0;
				if (sscanf(Line, "%lx-%lx", &S, &E) != 2 || E <= S)
					continue;
				V.S = S;
				V.E = E;
				const char* P = strchr(Line, '/');
				if (!P)
				{
					P = strstr(Line, "[");
				}
				if (P)
				{
					std::snprintf(V.Path, sizeof(V.Path), "%s", P);
					char* Nl = strchr(V.Path, '\n');
					if (Nl)
						*Nl = 0;
				}
				Out.push_back(V);
			}
			fclose(Fp);
			return Out;
		}

		bool Readable(const std::vector<Vma>& Maps, uintptr_t P)
		{
			for (const auto& V : Maps)
			{
				if (P >= V.S && P < V.E)
					return true;
			}
			return false;
		}

		uintptr_t SelfBase(std::string* PathOut)
		{
			Dl_info Inf{};
			if (!dladdr(reinterpret_cast<void*>(&Apply), &Inf) || !Inf.dli_fbase)
				return 0;
			if (PathOut && Inf.dli_fname)
				*PathOut = Inf.dli_fname;
			return reinterpret_cast<uintptr_t>(Inf.dli_fbase);
		}

		void CoverElf(uintptr_t Base)
		{
			if (!Base)
				return;
			auto* Eh = reinterpret_cast<Elf64_Ehdr*>(Base);
			if (std::memcmp(Eh->e_ident, ELFMAG, SELFMAG) != 0)
				return;
			auto* Ph = reinterpret_cast<Elf64_Phdr*>(Base + Eh->e_phoff);
			uintptr_t MinV = ~uintptr_t{0};
			for (int i = 0; i < Eh->e_phnum; ++i)
			{
				if (Ph[i].p_type != PT_LOAD)
					continue;
				if (Ph[i].p_vaddr < MinV)
					MinV = Ph[i].p_vaddr;
			}
			const uintptr_t Bias = Base - (MinV == ~uintptr_t{0} ? 0 : MinV);
			for (int i = 0; i < Eh->e_phnum; ++i)
			{
				if (Ph[i].p_type != PT_LOAD)
					continue;
				const uintptr_t S = Bias + Ph[i].p_vaddr;
				const uintptr_t E = S + Ph[i].p_memsz;
				AddRange(S, E);
			}
		}

		void CoverMaps(uintptr_t Base, const std::string& SelfPath)
		{
			const auto Maps = ReadMaps();
			const char* BaseName = nullptr;
			if (!SelfPath.empty())
			{
				const auto Slash = SelfPath.rfind('/');
				BaseName = Slash == std::string::npos ? SelfPath.c_str()
													  : SelfPath.c_str() + Slash + 1;
			}
			for (const auto& V : Maps)
			{
				bool Hit = false;
				if (Base && V.S <= Base && Base < V.E)
					Hit = true;
				if (BaseName && BaseName[0] && std::strstr(V.Path, BaseName))
					Hit = true;
				if (std::strstr(V.Path, XS("libnr3e")))
					Hit = true;
				if (std::strstr(V.Path, "(deleted)") && Base && V.S <= Base && Base < V.E)
					Hit = true;
				if (Hit)
					AddRange(V.S, V.E);
			}
		}

		uintptr_t LinkerBase(std::string* PathOut)
		{
			for (const auto& V : ReadMaps())
			{
				if (!std::strstr(V.Path, "/linker64") && !std::strstr(V.Path, "/linker"))
					continue;
				if (std::strstr(V.Path, ".so"))
					continue;
				if (PathOut)
				{
					*PathOut = V.Path;
					if (auto* Sp = std::strchr(PathOut->data(), ' '))
						*Sp = 0;
				}
				return V.S;
			}
			return 0;
		}

		uintptr_t FileSym(const char* Path, const char* Name, uintptr_t LoadBias)
		{
			int Fd = open(Path, O_RDONLY | O_CLOEXEC);
			if (Fd < 0)
				return 0;
			struct stat St{};
			if (fstat(Fd, &St) != 0 || St.st_size < 64)
			{
				close(Fd);
				return 0;
			}
			void* Map = mmap(nullptr, static_cast<size_t>(St.st_size), PROT_READ, MAP_PRIVATE, Fd, 0);
			close(Fd);
			if (Map == MAP_FAILED)
				return 0;
			auto* Eh = static_cast<Elf64_Ehdr*>(Map);
			if (std::memcmp(Eh->e_ident, ELFMAG, SELFMAG) != 0)
			{
				munmap(Map, static_cast<size_t>(St.st_size));
				return 0;
			}
			auto* Sh = reinterpret_cast<Elf64_Shdr*>(static_cast<uint8_t*>(Map) + Eh->e_shoff);
			uintptr_t Found = 0;
			for (int i = 0; i < Eh->e_shnum; ++i)
			{
				if (Sh[i].sh_type != SHT_SYMTAB && Sh[i].sh_type != SHT_DYNSYM)
					continue;
				auto* Sym = reinterpret_cast<Elf64_Sym*>(static_cast<uint8_t*>(Map) + Sh[i].sh_offset);
				const char* Str = reinterpret_cast<char*>(Map) + Sh[Sh[i].sh_link].sh_offset;
				const size_t N = Sh[i].sh_entsize ? Sh[i].sh_size / Sh[i].sh_entsize : 0;
				for (size_t k = 0; k < N; ++k)
				{
					if (!Sym[k].st_name)
						continue;
					const char* S = Str + Sym[k].st_name;
					const size_t Nl = std::strlen(Name);
					if (std::strncmp(S, Name, Nl) == 0 && (S[Nl] == 0 || S[Nl] == '.'))
					{
						Found = LoadBias + Sym[k].st_value;
						break;
					}
				}
				if (Found)
					break;
			}
			munmap(Map, static_cast<size_t>(St.st_size));
			return Found;
		}

		int GuessNextOff(void* Head, const std::vector<Vma>& Maps)
		{
			if (!Head)
				return 0;
			for (int Off = 8; Off <= 0x80; Off += 8)
			{
				int N = 0;
				void* P = Head;
				void* Seen[8]{};
				bool Cycle = false;
				for (; P && N < 48; ++N)
				{
					if (!Readable(Maps, reinterpret_cast<uintptr_t>(P)))
						break;
					for (int i = 0; i < 8 && i < N; ++i)
					{
						if (Seen[i] == P)
						{
							Cycle = true;
							break;
						}
					}
					if (Cycle)
						break;
					if (N < 8)
						Seen[N] = P;
					void* Next = *reinterpret_cast<void**>(static_cast<uint8_t*>(P) + Off);
					if (Next && !Readable(Maps, reinterpret_cast<uintptr_t>(Next)))
						break;
					P = Next;
				}
				if (N >= 6)
					return Off;
			}
			return 0;
		}

		bool HasBase(void* Si, uintptr_t Base, const std::vector<Vma>& Maps)
		{
			if (!Si || !Base)
				return false;
			auto* B = static_cast<uint8_t*>(Si);
			for (int Off = 0; Off < 0x280; Off += 8)
			{
				if (!Readable(Maps, reinterpret_cast<uintptr_t>(B + Off)))
					return false;
				if (*reinterpret_cast<uintptr_t*>(B + Off) == Base)
					return true;
			}
			return false;
		}

		uintptr_t LibcBase()
		{
			Dl_info Inf{};
			void* P = dlsym(RTLD_DEFAULT, "malloc");
			if (!P || !dladdr(P, &Inf) || !Inf.dli_fbase)
				return 0;
			return reinterpret_cast<uintptr_t>(Inf.dli_fbase);
		}

		void ScrubSoinfoNames(uintptr_t Self)
		{
			if (!Self)
				return;
			std::string Lpath;
			const uintptr_t Lbase = LinkerBase(&Lpath);
			if (!Lbase || Lpath.empty())
				return;
			static const char* kNames[] = {
				"__dl__ZL6solist",
				"__dl__ZL8solinker",
			};
			void** ListLoc = nullptr;
			for (const char* N : kNames)
			{
				const uintptr_t A = FileSym(Lpath.c_str(), N, Lbase);
				if (!A)
					continue;
				ListLoc = reinterpret_cast<void**>(A);
				break;
			}
			if (!ListLoc)
				return;
			const auto Maps = ReadMaps();
			if (!Readable(Maps, reinterpret_cast<uintptr_t>(ListLoc)))
				return;
			void* Head = *ListLoc;
			const int NextOff = GuessNextOff(Head, Maps);
			if (!NextOff)
				return;
			const uintptr_t Libc = LibcBase();
			bool SawLibc = false;
			bool SawSelf = false;
			void* Cur = Head;
			int Guard = 0;
			while (Cur && Guard++ < 4096)
			{
				if (!Readable(Maps, reinterpret_cast<uintptr_t>(Cur)))
					break;
				if (Libc && HasBase(Cur, Libc, Maps))
					SawLibc = true;
				if (HasBase(Cur, Self, Maps))
					SawSelf = true;
				Cur = *reinterpret_cast<void**>(static_cast<uint8_t*>(Cur) + NextOff);
			}
			if (!SawLibc || !SawSelf)
				return;
			Cur = Head;
			Guard = 0;
			while (Cur && Guard++ < 4096)
			{
				if (!Readable(Maps, reinterpret_cast<uintptr_t>(Cur)))
					break;
				if (HasBase(Cur, Self, Maps))
				{
					int Mem = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
					auto* B = static_cast<uint8_t*>(Cur);
					for (int Off = 0; Off < 0x300; Off += 8)
					{
						if (!Readable(Maps, reinterpret_cast<uintptr_t>(B + Off)))
							break;
						const uintptr_t P = *reinterpret_cast<uintptr_t*>(B + Off);
						if (P < 0x10000 || P > 0x0000800000000000ULL)
							continue;
						if (!Readable(Maps, P))
							continue;
						auto* S = reinterpret_cast<char*>(P);
						if (!std::strstr(S, XS("libnr3e")) && !std::strstr(S, XS("code_cache")))
							continue;
						if (Mem >= 0)
						{
							char Z = 0;
							pwrite(Mem, &Z, 1, static_cast<off_t>(P));
						}
					}
					if (Mem >= 0)
						close(Mem);
					return;
				}
				Cur = *reinterpret_cast<void**>(static_cast<uint8_t*>(Cur) + NextOff);
			}
		}

		void WipeNeedles(const std::string& SelfPath)
		{
			int Fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
			if (Fd < 0)
				return;
			const char* Needles[6]{};
			int Nn = 0;
			Needles[Nn++] = XS("libnr3e.so");
			Needles[Nn++] = XS("/data/local/tmp/libnr3e");
			Needles[Nn++] = XS("/data/data/com.tencent.letsgo/code_cache/");
			if (!SelfPath.empty())
				Needles[Nn++] = SelfPath.c_str();
			constexpr size_t kChunk = 4096;
			constexpr size_t kOv = 128;
			uint8_t Buf[kChunk + kOv];
			for (const auto& V : ReadMaps())
			{
				if (std::strstr(V.Path, "[stack") || std::strstr(V.Path, "[vdso"))
					continue;
				if (V.E <= V.S || V.E - V.S > (512ull << 20))
					continue;
				uintptr_t Off = V.S;
				size_t Keep = 0;
				while (Off < V.E)
				{
					const size_t Want = static_cast<size_t>(
						std::min<uint64_t>(kChunk, V.E - Off));
					const ssize_t N = pread(Fd, Buf + Keep, Want, static_cast<off_t>(Off));
					if (N <= 0)
					{
						Off += kChunk;
						Keep = 0;
						continue;
					}
					const size_t Total = Keep + static_cast<size_t>(N);
					for (int i = 0; i < Nn; ++i)
					{
						const size_t Nl = std::strlen(Needles[i]);
						if (!Nl || Nl > Total)
							continue;
						uint8_t* P = Buf;
						uint8_t* End = Buf + Total;
						while (P + Nl <= End)
						{
							auto* H = static_cast<uint8_t*>(memmem(P, static_cast<size_t>(End - P),
								Needles[i], Nl));
							if (!H)
								break;
							const uintptr_t At = Off - Keep + static_cast<uintptr_t>(H - Buf);
							uint8_t Z[256]{};
							const size_t Zl = std::min(Nl, sizeof(Z));
							pwrite(Fd, Z, Zl, static_cast<off_t>(At));
							P = H + Nl;
						}
					}
					if (Total >= kOv)
					{
						std::memmove(Buf, Buf + Total - kOv, kOv);
						Keep = kOv;
					}
					else
						Keep = Total;
					Off += static_cast<size_t>(N);
				}
			}
			close(Fd);
		}
	}

	void CoverCurrentStack()
	{
		if (!KernelOk())
			return;
		pthread_attr_t Attr{};
		if (pthread_getattr_np(pthread_self(), &Attr) != 0)
			return;
		void* Stack = nullptr;
		size_t Sz = 0;
		if (pthread_attr_getstack(&Attr, &Stack, &Sz) == 0 && Stack && Sz >= 0x10000)
			AddRange(reinterpret_cast<uintptr_t>(Stack),
				reinterpret_cast<uintptr_t>(Stack) + Sz);
		pthread_attr_destroy(&Attr);
	}

	void Apply(bool WipeHeap)
	{
		(void)WipeHeap;
		if (!KernelOk())
			return;
		AddKw(XS("libnr3e.so"));
		AddKw(XS("libnr3e"));
		AddKw(XS("c7f2a91b"));
		std::string Path;
		(void)SelfBase(&Path);
		if (!Path.empty())
		{
			const auto Slash = Path.rfind('/');
			const char* BaseName = Slash == std::string::npos ? Path.c_str()
															  : Path.c_str() + Slash + 1;
			if (BaseName && std::strlen(BaseName) >= 6)
				AddKw(BaseName);
		}
	}

	void HideCurrentTid()
	{
		if (!KernelOk())
			return;
		(void)prctl(PR_HIDEMAPS_ADD_TID, (unsigned long)gettid(), 0, 0, 0);
	}
}
