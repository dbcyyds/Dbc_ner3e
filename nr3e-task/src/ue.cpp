#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "nr3e/ue.hpp"
#include "nr3e/log.hpp"

#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nr3e::ue
{
	namespace
	{
		constexpr i32 kChunk = 0x10000;
		constexpr u32 kNameStride = 2;
		constexpr u32 kNameBlockBits = 16;

		struct FUObjectItem
		{
			UObject* Object;
			u8 Pad[0x10];
		};

		struct TUObjectArray
		{
			FUObjectItem** Objects;
			u8 Pad8[8];
			i32 MaxElements;
			i32 NumElements;
			i32 MaxChunks;
			i32 NumChunks;
		};

		struct FNameEntryHeader
		{
			u16 Packed;
			[[nodiscard]] bool Wide() const { return (Packed & 1u) != 0; }
			[[nodiscard]] u16 Len() const { return static_cast<u16>(Packed >> 6); }
		};

		struct FNamePool
		{
			u8 Pad[0x38];
			u32 CurrentBlock;
			u32 CurrentByteCursor;
			u8* Blocks[0x2000];
		};

		struct OffPair
		{
			uintptr_t GObjects;
			uintptr_t GNames;
			i32 PeIdx;
			const char* Tag;
		};

		// SDK Basic.hpp, then Dumpspace JSON (later dump).
		constexpr OffPair kOffs[] = {
			{0x125181E0, 0x124FD0C0, 0x44, "sdk"},
			{0x1251E360, 0x125032C0, 0x44, "dumpspace"},
		};
		constexpr uintptr_t kGWorld[] = {0x126938F0, 0x12699EF0};

		uintptr_t g_Base = 0;
		TUObjectArray* g_Objs = nullptr;
		FNamePool* g_Names = nullptr;
		i32 g_PeIdx = 0x44;
		std::mutex g_Mu;
		bool g_Ready = false;
		std::unordered_map<std::string, UObject*> g_ClassCache;
		std::unordered_map<std::string, UObject*> g_FnCache;

		uintptr_t FindUe4()
		{
			struct State
			{
				uintptr_t Named = 0;
				uintptr_t Largest = 0;
				size_t Exec = 0;
			} S{};

			dl_iterate_phdr(
				[](dl_phdr_info* Info, size_t, void* Data) -> int {
					auto* Out = static_cast<State*>(Data);
					const char* Path = Info->dlpi_name ? Info->dlpi_name : "";
					uintptr_t Min = ~uintptr_t{0};
					size_t Exec = 0;
					for (ElfW(Half) i = 0; i < Info->dlpi_phnum; ++i)
					{
						const auto& Ph = Info->dlpi_phdr[i];
						if (Ph.p_type != PT_LOAD)
							continue;
						const uintptr_t Start =
							static_cast<uintptr_t>(Info->dlpi_addr + Ph.p_vaddr);
						if (Start < Min)
							Min = Start;
						if (Ph.p_flags & PF_X)
							Exec += static_cast<size_t>(Ph.p_memsz);
					}
					if (Min == ~uintptr_t{0})
						return 0;
					if (std::strstr(Path, "libUE4.so") || std::strstr(Path, "libUnreal.so"))
					{
						Out->Named = Min;
						return 1;
					}
					if (Exec > Out->Exec)
					{
						Out->Exec = Exec;
						Out->Largest = Min;
					}
					return 0;
				},
				&S);
			return S.Named ? S.Named : S.Largest;
		}

		bool LooksArray(const TUObjectArray* A)
		{
			if (!A)
				return false;
			if (A->NumElements < 8000 || A->NumElements > 8000000)
				return false;
			if (A->NumChunks <= 0 || A->NumChunks > 512)
				return false;
			if (!A->Objects)
				return false;
			return static_cast<i64>(A->NumChunks) * kChunk >= A->NumElements;
		}

		bool LooksNames(const FNamePool* P)
		{
			if (!P)
				return false;
			if (P->CurrentBlock >= 0x2000)
				return false;
			return P->Blocks[0] != nullptr;
		}

		std::string EntryString(i32 Index)
		{
			if (!g_Names || Index < 0)
				return {};
			const i32 Chunk = Index >> kNameBlockBits;
			const i32 In = Index & ((1 << kNameBlockBits) - 1);
			if (Chunk < 0 || static_cast<u32>(Chunk) > g_Names->CurrentBlock)
				return {};
			u8* Block = g_Names->Blocks[Chunk];
			if (!Block)
				return {};
			auto* H = reinterpret_cast<FNameEntryHeader*>(Block + In * kNameStride);
			const u16 Len = H->Len();
			if (Len == 0 || Len > 1024)
				return {};
			const u8* Body = reinterpret_cast<u8*>(H + 1);
			std::string Out;
			Out.reserve(Len);
			if (H->Wide())
			{
				const auto* W = reinterpret_cast<const char16_t*>(Body);
				for (u16 i = 0; i < Len; ++i)
				{
					const char16_t C = W[i];
					Out.push_back(C < 0x80 ? static_cast<char>(C) : '?');
				}
			}
			else
			{
				Out.assign(reinterpret_cast<const char*>(Body), Len);
			}
			return Out;
		}

		UObject* Item(i32 Index)
		{
			if (!g_Objs || Index < 0 || Index >= g_Objs->NumElements)
				return nullptr;
			const i32 C = Index / kChunk;
			const i32 I = Index % kChunk;
			if (C < 0 || C >= g_Objs->NumChunks)
				return nullptr;
			FUObjectItem* Chunk = g_Objs->Objects[C];
			if (!Chunk)
				return nullptr;
			return Chunk[I].Object;
		}
	}

	bool Init()
	{
		std::lock_guard<std::mutex> Lock(g_Mu);
		if (g_Ready)
			return true;
		g_Base = FindUe4();
		if (!g_Base)
		{
			NRLOGE("libUE4.so not mapped");
			return false;
		}
		for (const auto& Off : kOffs)
		{
			auto* Objs = reinterpret_cast<TUObjectArray*>(g_Base + Off.GObjects);
			auto* Names = reinterpret_cast<FNamePool*>(g_Base + Off.GNames);
			if (!LooksArray(Objs) || !LooksNames(Names))
				continue;
			g_Objs = Objs;
			g_Names = Names;
			g_PeIdx = Off.PeIdx;
			UObject* Sample = Item(0);
			if (!Sample || !Sample->VTable)
				continue;
			g_Ready = true;
			NRLOGI("ue init %s base=%p objs=%d pe=%d", Off.Tag,
				reinterpret_cast<void*>(g_Base), g_Objs->NumElements, g_PeIdx);
			return true;
		}
		NRLOGE("ue offsets missed base=%p", reinterpret_cast<void*>(g_Base));
		return false;
	}

	bool Ready() { return g_Ready; }
	uintptr_t ImageBase() { return g_Base; }
	i32 ObjectCount() { return g_Objs ? g_Objs->NumElements : 0; }
	UObject* Get(i32 Index) { return Item(Index); }

	std::string NameOf(const UObject* Obj)
	{
		if (!Obj)
			return {};
		std::string S = EntryString(Obj->NameIndex);
		if (Obj->NameNumber > 0)
			S += "_" + std::to_string(Obj->NameNumber - 1);
		const auto Slash = S.rfind('/');
		if (Slash != std::string::npos)
			S = S.substr(Slash + 1);
		return S;
	}

	std::string ClassNameOf(const UObject* Obj)
	{
		return Obj && Obj->Class ? NameOf(Obj->Class) : std::string{};
	}

	UObject* Find(std::string_view Name)
	{
		if (!g_Objs)
			return nullptr;
		const i32 N = g_Objs->NumElements;
		for (i32 i = 0; i < N; ++i)
		{
			UObject* O = Item(i);
			if (O && NameOf(O) == Name)
				return O;
		}
		return nullptr;
	}

	UObject* FindClass(std::string_view Name)
	{
		if (!g_Objs)
			return nullptr;
		std::string Key(Name);
		auto It = g_ClassCache.find(Key);
		if (It != g_ClassCache.end())
			return It->second;
		const i32 N = g_Objs->NumElements;
		UObject* Hit = nullptr;
		for (i32 i = 0; i < N; ++i)
		{
			UObject* O = Item(i);
			if (!O || !O->Class)
				continue;
			const std::string Cn = ClassNameOf(O);
			if (Cn != "Class" && Cn != "BlueprintGeneratedClass")
				continue;
			if (NameOf(O) == Name)
			{
				Hit = O;
				break;
			}
		}
		if (Hit)
			g_ClassCache.emplace(std::move(Key), Hit);
		return Hit;
	}

	bool IsA(const UObject* Obj, const UObject* Cls)
	{
		if (!Obj || !Cls || !Obj->Class)
			return false;
		for (UStruct* C = reinterpret_cast<UStruct*>(Obj->Class); C; C = C->Super)
		{
			if (static_cast<const UObject*>(C) == Cls)
				return true;
		}
		return false;
	}

	UObject* FindInstanceOf(UObject* Cls)
	{
		const auto Hits = InstancesOf(Cls, 1);
		return Hits.empty() ? nullptr : Hits.front();
	}

	std::vector<UObject*> InstancesOf(UObject* Cls, i32 Max)
	{
		std::vector<UObject*> Out;
		if (!g_Objs || !Cls || Max <= 0)
			return Out;
		UObject* Cdo = CdoOf(Cls);
		const i32 N = g_Objs->NumElements;
		for (i32 i = 0; i < N && static_cast<i32>(Out.size()) < Max; ++i)
		{
			UObject* O = Item(i);
			if (!O || O == Cls || O == Cdo || !O->Class || !O->VTable)
				continue;
			if (IsA(O, Cls))
				Out.push_back(O);
		}
		return Out;
	}

	i32 FnParmsSize(UObject* Fn)
	{
		if (!Fn)
			return 16;
		const i32 N = reinterpret_cast<UStruct*>(Fn)->Size;
		if (N < 16)
			return 16;
		if (N > 0x2000)
			return 0x2000;
		return N;
	}

	UObject* FindFn(UObject* Obj, std::string_view Name)
	{
		if (!Obj)
			return nullptr;
		const std::string Kind = ClassNameOf(Obj);
		UStruct* Cls = nullptr;
		if (Kind == "Class" || Kind == "BlueprintGeneratedClass")
			Cls = reinterpret_cast<UStruct*>(Obj);
		else if (Obj->Class)
			Cls = reinterpret_cast<UStruct*>(Obj->Class);
		if (!Cls)
			return nullptr;
		char Key[96];
		std::snprintf(Key, sizeof(Key), "%p:%s", static_cast<void*>(Cls),
			std::string(Name).c_str());
		auto It = g_FnCache.find(Key);
		if (It != g_FnCache.end())
			return It->second;
		UObject* Hit = nullptr;
		for (UStruct* Cur = Cls; Cur && !Hit; Cur = Cur->Super)
		{
			for (UField* F = Cur->Children; F; F = F->Next)
			{
				if (NameOf(F) == Name)
				{
					Hit = F;
					break;
				}
			}
		}
		g_FnCache.emplace(Key, Hit);
		return Hit;
	}

	UObject* FindFunction(std::string_view Name)
	{
		if (!g_Objs)
			return nullptr;
		const i32 N = g_Objs->NumElements;
		for (i32 i = 0; i < N; ++i)
		{
			UObject* O = Item(i);
			if (!O)
				continue;
			if (ClassNameOf(O) != "Function")
				continue;
			if (NameOf(O) == Name)
				return O;
		}
		return nullptr;
	}

	UObject* CdoOf(UObject* Cls)
	{
		if (!Cls)
			return nullptr;
		return *reinterpret_cast<UObject**>(reinterpret_cast<u8*>(Cls) + 0x118);
	}

	UObject* World()
	{
		if (!g_Base)
			return nullptr;
		for (uintptr_t Off : kGWorld)
		{
			auto** Slot = reinterpret_cast<UObject**>(g_Base + Off);
			UObject* W = *Slot;
			if (!W || !W->VTable || !W->Class)
				continue;
			const std::string Cn = ClassNameOf(W);
			if (Cn.find("World") == std::string::npos)
				continue;
			return W;
		}
		return nullptr;
	}

	using PeFn = void (*)(void*, void*, void*);
	PeFn g_OrigPe = nullptr;
	void* g_PeSite = nullptr;
	void (*g_Pump)() = nullptr;
	bool g_PeHooked = false;
	thread_local int t_InPe = 0;
	uint8_t* g_Tramp = nullptr;
	UObject* g_LuaCallFn = nullptr;
	UObject* g_LuaCallComp = nullptr;

	void LogPe(void* Self, void* Fn, void* Params)
	{
		if (!Fn)
			return;
		const std::string Nm = NameOf(static_cast<UObject*>(Fn));
		if (Nm == "ReqEventLuaCall")
		{
			g_LuaCallFn = static_cast<UObject*>(Fn);
			if (Self)
				g_LuaCallComp = static_cast<UObject*>(Self);
		}
		if (Nm.find("ReqEvent") == std::string::npos && Nm != "E3VotePlayerRequest")
			return;
		static int s_N = 0;
		if (s_N > 200)
			return;
		++s_N;
		FILE* Fp = fopen("/data/data/com.tencent.letsgo/files/nr3e_pe.log", "a");
		if (!Fp)
			return;
		std::string Own = Self ? ClassNameOf(static_cast<UObject*>(Self)) : "-";
		char A[80]{};
		i64 U16 = 0, U24 = 0, U32 = 0;
		if (Params)
		{
			struct FS
			{
				char16_t* D;
				i32 N;
				i32 M;
			};
			auto* S = static_cast<FS*>(Params);
			if (S->D && S->N > 1 && S->N < 80 && S->M >= S->N)
			{
				const i32 L = S->N > 1 ? S->N - 1 : 0;
				for (i32 i = 0; i < L && i < 70; ++i)
				{
					const auto C = S->D[i];
					A[i] = (C >= 32 && C < 127) ? static_cast<char>(C) : '.';
				}
			}
			U16 = *reinterpret_cast<i64*>(static_cast<u8*>(Params) + 16);
			U24 = *reinterpret_cast<i64*>(static_cast<u8*>(Params) + 24);
			U32 = *reinterpret_cast<i64*>(static_cast<u8*>(Params) + 32);
		}
		fprintf(Fp, "pe %s on %s arg=%s u16=%lld u24=%lld u32=%lld fn=%p self=%p\n", Nm.c_str(),
			Own.c_str(), A, static_cast<long long>(U16), static_cast<long long>(U24),
			static_cast<long long>(U32), Fn, Self);
		fclose(Fp);
	}

	extern "C" void Nr3ePeHook(void* Self, void* Fn, void* Params)
	{
		LogPe(Self, Fn, Params);
		if (!t_InPe && g_Pump)
		{
			t_InPe = 1;
			g_Pump();
			t_InPe = 0;
		}
		if (g_OrigPe)
			g_OrigPe(Self, Fn, Params);
	}

	bool IsPcRel(uint32_t Insn)
	{
		const uint32_t Op = Insn >> 24;
		if ((Insn & 0x7C000000u) == 0x14000000u)
			return true;
		if ((Insn & 0x9F000000u) == 0x10000000u)
			return true;
		if ((Insn & 0xFF000000u) == 0x58000000u)
			return true;
		if (Op == 0x54 || Op == 0x94 || Op == 0x97)
			return true;
		return false;
	}

	bool InstallPeInline(void* Pe)
	{
		if (!Pe)
			return false;
		if (g_PeHooked && g_OrigPe)
			return true;
		auto* Src = static_cast<uint32_t*>(Pe);
		for (int i = 0; i < 4; ++i)
		{
			if (IsPcRel(Src[i]))
				return false;
		}
		const long Page = sysconf(_SC_PAGESIZE);
		if (Page <= 0)
			return false;
		if (!g_Tramp)
		{
			void* M = mmap(nullptr, static_cast<size_t>(Page), PROT_READ | PROT_WRITE | PROT_EXEC,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (M == MAP_FAILED)
			{
				M = mmap(nullptr, static_cast<size_t>(Page), PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
				if (M == MAP_FAILED)
					return false;
				if (mprotect(M, static_cast<size_t>(Page), PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
					return false;
			}
			g_Tramp = static_cast<uint8_t*>(M);
		}
		std::memcpy(g_Tramp, Pe, 16);
		auto* Tr = reinterpret_cast<uint32_t*>(g_Tramp + 16);
		Tr[0] = 0x58000050u;
		Tr[1] = 0xD61F0200u;
		const uint64_t Back = reinterpret_cast<uint64_t>(Src) + 16;
		std::memcpy(g_Tramp + 24, &Back, 8);
		__builtin___clear_cache(reinterpret_cast<char*>(g_Tramp),
			reinterpret_cast<char*>(g_Tramp) + 32);

		const auto PageAddr = reinterpret_cast<uintptr_t>(Pe) & ~static_cast<uintptr_t>(Page - 1);
		if (mprotect(reinterpret_cast<void*>(PageAddr), static_cast<size_t>(Page) * 2,
				PROT_READ | PROT_WRITE | PROT_EXEC)
			!= 0)
		{
			if (mprotect(reinterpret_cast<void*>(PageAddr), static_cast<size_t>(Page) * 2,
					PROT_READ | PROT_WRITE)
				!= 0)
				return false;
		}
		Src[0] = 0x58000050u;
		Src[1] = 0xD61F0200u;
		const uint64_t Hook = reinterpret_cast<uint64_t>(&Nr3ePeHook);
		std::memcpy(Src + 2, &Hook, 8);
		__builtin___clear_cache(reinterpret_cast<char*>(Pe), reinterpret_cast<char*>(Pe) + 16);
		mprotect(reinterpret_cast<void*>(PageAddr), static_cast<size_t>(Page) * 2,
			PROT_READ | PROT_EXEC);
		g_OrigPe = reinterpret_cast<PeFn>(g_Tramp);
		g_PeSite = Pe;
		g_PeHooked = true;
		return true;
	}

	void ProcessEvent(UObject* Self, UObject* Fn, void* Params)
	{
		if (!Self || !Fn || !Self->VTable)
			return;
		auto** Vt = static_cast<void**>(Self->VTable);
		auto Pe = reinterpret_cast<PeFn>(Vt[g_PeIdx]);
		if (!Pe)
			return;
		if (g_OrigPe && (Pe == reinterpret_cast<PeFn>(g_PeSite) || Pe == &Nr3ePeHook))
		{
			g_OrigPe(Self, Fn, Params);
			return;
		}
		Pe(Self, Fn, Params);
	}

	void SetGamePump(void (*Fn)()) { g_Pump = Fn; }

	bool PeHooked() { return g_PeHooked; }
	bool IsGamePump() { return t_InPe != 0; }
	void CachedLuaCall(UObject** Comp, UObject** Fn)
	{
		if (Comp)
			*Comp = g_LuaCallComp;
		if (Fn)
			*Fn = g_LuaCallFn;
	}

	bool HookObject(UObject* Obj)
	{
		if (!Obj || !Obj->VTable)
			return false;
		auto** Vt = static_cast<void**>(Obj->VTable);
		void* Pe = Vt[g_PeIdx];
		if (!Pe)
			return false;
		auto* Hook = reinterpret_cast<void*>(&Nr3ePeHook);
		if (Pe == Hook)
		{
			g_PeHooked = true;
			return true;
		}
		if (g_OrigPe && Pe != reinterpret_cast<void*>(g_OrigPe))
			return false;
		const long Page = sysconf(_SC_PAGESIZE);
		if (Page <= 0)
			return false;
		const auto PageAddr = reinterpret_cast<uintptr_t>(Vt) & ~static_cast<uintptr_t>(Page - 1);
		if (mprotect(reinterpret_cast<void*>(PageAddr), static_cast<size_t>(Page) * 2,
				PROT_READ | PROT_WRITE)
			!= 0)
			return false;
		if (!g_OrigPe)
			g_OrigPe = reinterpret_cast<PeFn>(Pe);
		Vt[g_PeIdx] = Hook;
		g_PeHooked = true;
		return true;
	}

	bool HookProcessEvent()
	{
		if (g_PeHooked && g_OrigPe)
			return true;

		void* Pe = nullptr;
		if (UObject* W = World(); W && W->VTable)
			Pe = static_cast<void**>(W->VTable)[g_PeIdx];
		if (!Pe)
		{
			for (i32 i = 0; i < 512 && !Pe; ++i)
			{
				UObject* O = Get(i);
				if (O && O->VTable)
					Pe = static_cast<void**>(O->VTable)[g_PeIdx];
			}
		}
		if (InstallPeInline(Pe))
			return true;

		bool Ok = HookObject(World());
		if (UObject* Cls = FindClass("MoeNR3E3GameCharacter"))
		{
			for (UObject* O : InstancesOf(Cls, 8))
				Ok = HookObject(O) || Ok;
		}
		if (UObject* Cls = FindClass("MoeNR3E3TaskManagerBase"))
		{
			for (UObject* O : InstancesOf(Cls, 4))
				Ok = HookObject(O) || Ok;
		}
		if (UObject* Cls = FindClass("PlayerController"))
		{
			for (UObject* O : InstancesOf(Cls, 4))
				Ok = HookObject(O) || Ok;
		}
		if (UObject* Cls = FindClass("GameStateBase"))
		{
			for (UObject* O : InstancesOf(Cls, 4))
				Ok = HookObject(O) || Ok;
		}
		if (!Ok)
		{
			UObject* O = World();
			if (!O)
			{
				for (i32 i = 0; i < 256 && !O; ++i)
				{
					UObject* C = Get(i);
					if (C && C->VTable)
						O = C;
				}
			}
			Ok = HookObject(O);
		}
		return Ok && g_OrigPe != nullptr;
	}

	bool ArrayOk(const TArrayHdr& A, i32 ElemSize, i32 MaxNum)
	{
		(void)ElemSize;
		if (A.Num < 0 || A.Num > MaxNum)
			return false;
		if (A.Num == 0)
			return true;
		return A.Data != nullptr && A.Max >= A.Num;
	}

	OwnedStr MakeFString(std::string_view Utf8)
	{
		OwnedStr O;
		O.Keep.reserve(Utf8.size() + 1);
		for (unsigned char C : Utf8)
			O.Keep.push_back(static_cast<char16_t>(C));
		O.Keep.push_back(0);
		O.View.Data = O.Keep.data();
		O.View.Num = static_cast<i32>(O.Keep.size());
		O.View.Max = static_cast<i32>(O.Keep.size());
		return O;
	}

	namespace
	{
		bool HeapOk(const void* P)
		{
			const auto U = reinterpret_cast<uintptr_t>(P);
			return U > 0x10000 && U < 0x0000800000000000ULL;
		}

		std::string FieldNameOf(u8* Field)
		{
			if (!HeapOk(Field))
				return {};
			const int kNameOff[] = {0x28, 0x24, 0x2C, 0x18, 0x20};
			for (int Off : kNameOff)
			{
				const i32 Idx = *reinterpret_cast<i32*>(Field + Off);
				std::string S = EntryString(Idx);
				if (S.size() < 2 || S.size() > 48)
					continue;
				if (S[0] < 'A' || S[0] > 'z')
					continue;
				bool Ok = true;
				for (char C : S)
				{
					if (!((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9')
							|| C == '_'))
					{
						Ok = false;
						break;
					}
				}
				if (Ok)
					return S;
			}
			return {};
		}

		u8* FieldNextOf(u8* Field)
		{
			if (!HeapOk(Field))
				return nullptr;
			for (int Off : {0x20, 0x18, 0x10})
			{
				u8* N = *reinterpret_cast<u8**>(Field + Off);
				if (HeapOk(N) && N != Field && N != Field + Off && !FieldNameOf(N).empty())
					return N;
			}
			return nullptr;
		}

		void WriteStrAt(u8* Params, i32 Off, i32 Cap, std::string_view S)
		{
			if (!Params || Off < 0 || Off + 16 > Cap)
				return;
			const i32 N = static_cast<i32>(S.size()) + 1;
			auto* W = static_cast<char16_t*>(std::malloc(static_cast<size_t>(N) * sizeof(char16_t)));
			if (!W)
				return;
			for (i32 i = 0; i < N - 1; ++i)
				W[i] = static_cast<char16_t>(static_cast<unsigned char>(S[static_cast<size_t>(i)]));
			W[N - 1] = 0;
			*reinterpret_cast<char16_t**>(Params + Off) = W;
			*reinterpret_cast<i32*>(Params + Off + 8) = N;
			*reinterpret_cast<i32*>(Params + Off + 12) = N;
		}

		void WriteI64At(u8* Params, i32 Off, i32 Cap, i64 V)
		{
			if (!Params || Off < 0 || Off + 8 > Cap)
				return;
			*reinterpret_cast<i64*>(Params + Off) = V;
		}

		bool NameIsEvent(const std::string& N)
		{
			return N == "EventName" || N == "Event" || N == "InEventName" || N == "EventKey";
		}

		bool NameIsUid(const std::string& N)
		{
			return N == "UID" || N == "Uid" || N == "InUID" || N == "PlayerUID" || N == "UserId"
				|| N == "LocalUID" || N == "InUid";
		}

		bool NameIsData(const std::string& N)
		{
			return N == "Data" || N == "Param" || N == "Args" || N == "Json" || N == "InData"
				|| N == "ParamString";
		}
	}

	void DumpFn(UObject* Fn, std::FILE* Fp)
	{
		if (!Fp || !Fn)
			return;
		auto* St = reinterpret_cast<UStruct*>(Fn);
		std::fprintf(Fp, "fn %s size=%d flags@b0=0x%x childP=%p children=%p super=%p\n",
			NameOf(Fn).c_str(), St->Size,
			*reinterpret_cast<u32*>(reinterpret_cast<u8*>(Fn) + 0xB0), St->ChildProperties,
			St->Children, static_cast<void*>(St->Super));
		u8* Head = nullptr;
		i32 HeadAt = -1;
		auto* Obj = reinterpret_cast<u8*>(Fn);
		for (i32 o = 0x30; o <= 0x78; o += 8)
		{
			u8* P = *reinterpret_cast<u8**>(Obj + o);
			if (!HeapOk(P))
				continue;
			const std::string Nm = FieldNameOf(P);
			if (Nm.empty())
				continue;
			if (NameIsEvent(Nm) || NameIsUid(Nm) || NameIsData(Nm) || Nm == "ReturnValue")
			{
				Head = P;
				HeadAt = o;
				break;
			}
			if (!Head)
			{
				Head = P;
				HeadAt = o;
			}
		}
		std::fprintf(Fp, "  propHead off=0x%x p=%p name=%s\n", HeadAt, Head,
			Head ? FieldNameOf(Head).c_str() : "-");
		i32 N = 0;
		for (u8* F = Head; F && N < 8; F = FieldNextOf(F), ++N)
		{
			const std::string Nm = FieldNameOf(F);
			i32 OffGuess = -1;
			for (int o = 0x3C; o <= 0x5C; o += 4)
			{
				const i32 V = *reinterpret_cast<i32*>(F + o);
				if (V >= 0 && V <= 0x100 && (V % 4) == 0)
				{
					OffGuess = V;
					std::fprintf(Fp, "  %s field=%p nameOffGuess@0x%x=%d\n", Nm.c_str(), F, o, V);
				}
			}
			(void)OffGuess;
		}
	}

	std::string FillLuaParms(UObject* Fn, void* Params, i32 Cap, std::string_view Event, i64 Uid,
		std::string_view Json)
	{
		auto* P = static_cast<u8*>(Params);
		if (!P || Cap < 40)
			return "badcap";
		std::memset(P, 0, static_cast<size_t>(Cap));

		struct Hit
		{
			std::string Name;
			i32 Off = -1;
			u8* Field = nullptr;
		};
		Hit Hits[8]{};
		i32 Hn = 0;
		u8* Head = nullptr;
		auto* Obj = Fn ? reinterpret_cast<u8*>(Fn) : nullptr;
		if (Obj)
		{
			for (i32 o = 0x30; o <= 0x78 && !Head; o += 8)
			{
				u8* C = *reinterpret_cast<u8**>(Obj + o);
				if (!HeapOk(C))
					continue;
				const std::string Nm = FieldNameOf(C);
				if (NameIsEvent(Nm) || NameIsData(Nm) || NameIsUid(Nm))
					Head = C;
			}
			if (!Head)
			{
				auto* St = reinterpret_cast<UStruct*>(Fn);
				if (HeapOk(St->ChildProperties))
					Head = reinterpret_cast<u8*>(St->ChildProperties);
			}
		}

		u8* Fields[8]{};
		std::string Names[8]{};
		i32 FnN = 0;
		for (u8* F = Head; F && FnN < 8; F = FieldNextOf(F))
		{
			const std::string Nm = FieldNameOf(F);
			if (Nm.empty() || Nm == "ReturnValue")
				continue;
			Fields[FnN] = F;
			Names[FnN] = Nm;
			++FnN;
		}
		const i32 Parms = Fn ? FnParmsSize(Fn) : Cap;
		int Slot = -1;
		int SlotScore = -1;
		for (int o = 0x44; o <= 0x5C; o += 4)
		{
			i32 Offs[8]{};
			bool Ok = true;
			int Zero = 0;
			for (i32 i = 0; i < FnN; ++i)
			{
				const i32 V = *reinterpret_cast<i32*>(Fields[i] + o);
				Offs[i] = V;
				if (V < 0 || V > Parms - 4 || (V % 4) != 0)
				{
					Ok = false;
					break;
				}
				if (V == 0)
					++Zero;
				for (i32 j = 0; j < i; ++j)
				{
					if (Offs[j] == V)
					{
						Ok = false;
						break;
					}
				}
				if (!Ok)
					break;
			}
			if (!Ok || FnN == 0)
				continue;
			int Score = Zero * 4;
			if (Offs[0] == 0)
				Score += 5;
			if (FnN >= 3 && Offs[0] == 0 && Offs[1] == 16 && (Offs[2] == 24 || Offs[2] == 32))
				Score += 10;
			if (Score > SlotScore)
			{
				SlotScore = Score;
				Slot = o;
			}
		}
		for (i32 i = 0; i < FnN && Hn < 8; ++i)
		{
			Hits[Hn].Name = Names[i];
			Hits[Hn].Off = Slot >= 0 ? *reinterpret_cast<i32*>(Fields[i] + Slot) : -1;
			Hits[Hn].Field = Fields[i];
			++Hn;
		}

		i32 EventOff = -1, UidOff = -1, DataOff = -1;
		for (i32 i = 0; i < Hn; ++i)
		{
			if (NameIsEvent(Hits[i].Name) && EventOff < 0)
				EventOff = Hits[i].Off;
			else if (NameIsUid(Hits[i].Name) && UidOff < 0)
				UidOff = Hits[i].Off;
			else if (NameIsData(Hits[i].Name) && DataOff < 0)
				DataOff = Hits[i].Off;
		}
		if (EventOff < 0 || UidOff < 0 || DataOff < 0)
		{
			i32 Strs[2]{-1, -1};
			i32 Sn = 0;
			for (i32 i = 0; i < Hn; ++i)
			{
				if (Hits[i].Off < 0)
					continue;
				if (NameIsUid(Hits[i].Name))
					continue;
				if (Sn < 2)
					Strs[Sn++] = Hits[i].Off;
			}
			if (EventOff < 0 && Sn > 0)
				EventOff = Strs[0];
			if (DataOff < 0 && Sn > 1)
				DataOff = Strs[1];
			if (UidOff < 0)
			{
				for (i32 i = 0; i < Hn; ++i)
				{
					if (Hits[i].Off != EventOff && Hits[i].Off != DataOff)
					{
						UidOff = Hits[i].Off;
						break;
					}
				}
			}
		}

		char Desc[192];
		if (EventOff >= 0 && UidOff >= 0 && DataOff >= 0)
		{
			WriteStrAt(P, EventOff, Cap, Event);
			WriteI64At(P, UidOff, Cap, Uid);
			WriteStrAt(P, DataOff, Cap, Json);
			std::snprintf(Desc, sizeof(Desc), "props n=%d ev=%d uid=%d data=%d", Hn, EventOff, UidOff,
				DataOff);
			return Desc;
		}

		WriteStrAt(P, 0, Cap, Event);
		WriteI64At(P, 16, Cap, Uid);
		WriteStrAt(P, 24, Cap, Json);
		std::snprintf(Desc, sizeof(Desc), "fallback40 n=%d ev=%d uid=%d data=%d", Hn, EventOff, UidOff,
			DataOff);
		return Desc;
	}
}
