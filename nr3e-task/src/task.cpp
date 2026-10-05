#include "nr3e/task.hpp"
#include "nr3e/log.hpp"
#include "nr3e/obf.hpp"
#include "nr3e/ue.hpp"

#include <android/log.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nr3e::task
{
	namespace
	{
		using nr3e::ue::i32;
		using nr3e::ue::i64;
		using nr3e::ue::u8;
		using nr3e::ue::TArrayHdr;
		using nr3e::ue::UObject;

		constexpr i32 kTaskSize = 0xE0;
		constexpr i32 kSubSize = 0x14;
		constexpr i32 kOffId = 0x04;
		constexpr i32 kOffSub = 0x18;
		constexpr i32 kOffDone = 0x28;
		constexpr i32 kOffUrgent = 0x29;
		constexpr i32 kOffRun = 0x50;
		constexpr i32 kOffType = 0x85;
		constexpr i32 kOffCompTasks = 0x120;
		constexpr i32 kOffCompUrgent = 0x140;
		constexpr i32 kOffCharTask = 0xC28;
		constexpr i32 kOffGsMgr = 0x2B0;
		constexpr i32 kOffMeetMgr = 0x2B8;
		constexpr i32 kOffMgrTasks = 0x320;
		constexpr i32 kOffMgrUrgent = 0x330;
		constexpr i32 kOffCompOwner = 0x100;

		std::atomic<int> g_Enable{0};
		std::atomic<int> g_CancelEvt{0};
		std::atomic_flag g_Meeting{};
		std::string g_Why;
		std::mutex g_MutMu;
		std::chrono::steady_clock::time_point g_LastAuto{};
		std::chrono::steady_clock::time_point g_LastIdReq{};

		void Dbg(const char* Fmt, ...)
		{
#ifdef NR3E_SILENT
			(void)Fmt;
#else
			char Buf[512];
			va_list Ap;
			va_start(Ap, Fmt);
			std::vsnprintf(Buf, sizeof(Buf), Fmt, Ap);
			va_end(Ap);
			FILE* Fp = fopen("/data/data/com.tencent.letsgo/files/nr3e_dbg.txt", "a");
			if (!Fp)
				Fp = fopen("/data/local/tmp/nr3e_dbg.txt", "a");
			if (Fp)
			{
				std::fprintf(Fp, "%s\n", Buf);
				fclose(Fp);
			}
#endif
		}

		bool Plausible(const void* P)
		{
			const auto U = reinterpret_cast<uintptr_t>(P);
			return U > 0x10000 && U < 0x0000800000000000ULL;
		}

		bool LooksObj(UObject* O)
		{
			if (!Plausible(O))
				return false;
			return Plausible(O->VTable) && Plausible(O->Class);
		}

		UObject* PtrAt(UObject* O, i32 Off)
		{
			if (!LooksObj(O))
				return nullptr;
			UObject* P = *reinterpret_cast<UObject**>(reinterpret_cast<u8*>(O) + Off);
			return LooksObj(P) ? P : nullptr;
		}

		bool Invoke(UObject* Self, UObject* Fn, void* Params, i32 Bytes)
		{
			if (!Self || !Fn)
				return false;
			const i32 Need = std::max(ue::FnParmsSize(Fn), Bytes);
			alignas(16) u8 Stack[256];
			u8* P = Stack;
			std::unique_ptr<u8[]> Heap;
			if (Need > static_cast<i32>(sizeof(Stack)))
			{
				Heap = std::make_unique<u8[]>(static_cast<size_t>(Need));
				P = Heap.get();
			}
			std::memset(P, 0, static_cast<size_t>(Need));
			if (Params && Bytes > 0)
				std::memcpy(P, Params, static_cast<size_t>(std::min(Bytes, Need)));
			ue::ProcessEvent(Self, Fn, P);
			if (Params && Bytes > 0)
				std::memcpy(Params, P, static_cast<size_t>(std::min(Bytes, Need)));
			return true;
		}

		bool CallFn(UObject* Self, const char* Name, void* Params, i32 Bytes)
		{
			UObject* Fn = ue::FindFn(Self, Name);
			return Fn && Invoke(Self, Fn, Params, Bytes);
		}

		UObject* CallRetObj(UObject* Self, const char* Fn)
		{
			void* Ret = nullptr;
			if (!CallFn(Self, Fn, &Ret, 8))
				return nullptr;
			return LooksObj(static_cast<UObject*>(Ret)) ? static_cast<UObject*>(Ret) : nullptr;
		}

		i64 CallRetI64(UObject* Self, const char* Fn)
		{
			i64 Ret = 0;
			CallFn(Self, Fn, &Ret, 8);
			return Ret;
		}

		i32 CallRetI32(UObject* Self, const char* Fn)
		{
			i64 Ret = 0;
			if (!CallFn(Self, Fn, &Ret, 8))
				return 0;
			return static_cast<i32>(Ret);
		}

		UObject* StaticCallRet(const char* ClsName, const char* Fn)
		{
			UObject* Cls = ue::FindClass(ClsName);
			if (!Cls)
				return nullptr;
			UObject* F = ue::FindFn(Cls, Fn);
			if (!F)
				return nullptr;
			UObject* Self = ue::CdoOf(Cls);
			if (!Self)
				Self = Cls;
			void* Ret = nullptr;
			if (!Invoke(Self, F, &Ret, 8))
				return nullptr;
			return LooksObj(static_cast<UObject*>(Ret)) ? static_cast<UObject*>(Ret) : nullptr;
		}

		bool NameHas(UObject* O, const char* Sub)
		{
			if (!LooksObj(O) || !Sub)
				return false;
			return ue::ClassNameOf(O).find(Sub) != std::string::npos
				|| ue::NameOf(O).find(Sub) != std::string::npos;
		}

		bool IsDummyPawn(UObject* P)
		{
			if (!LooksObj(P))
				return true;
			const std::string N = ue::NameOf(P);
			const std::string C = ue::ClassNameOf(P);
			return N.find("DefaultPawn") != std::string::npos
				|| N.find("Spectator") != std::string::npos
				|| N.find("StartUp") != std::string::npos
				|| C.find("DefaultPawn") != std::string::npos;
		}

		UObject* PawnFromPc(UObject* Pc)
		{
			if (UObject* P = PtrAt(Pc, 0x2B0))
				return P;
			if (UObject* P = PtrAt(Pc, 0x300))
				return P;
			return PtrAt(Pc, 0x2C0);
		}

		UObject* GsPawn(UObject* World, const char* FnName)
		{
			UObject* Gs = ue::FindClass(XS("GameplayStatics"));
			if (!Gs || !World)
				return nullptr;
			UObject* Fn = ue::FindFn(Gs, FnName);
			UObject* Self = ue::CdoOf(Gs);
			if (!Self)
				Self = Gs;
			if (!Fn || !Self)
				return nullptr;
			struct
			{
				UObject* Ctx;
				i32 Idx;
				i32 Pad;
				UObject* Ret;
			} P{World, 0, 0, nullptr};
			Invoke(Self, Fn, &P, sizeof(P));
			return LooksObj(P.Ret) ? P.Ret : nullptr;
		}

		UObject* FirstPawnInActors(UObject* Level, bool LocalOnly)
		{
			if (!LooksObj(Level))
				return nullptr;
			TArrayHdr Arr{};
			std::memcpy(&Arr, reinterpret_cast<u8*>(Level) + 0x98, sizeof(Arr));
			if (!ue::ArrayOk(Arr, 8, 4096) || !Arr.Data)
				return nullptr;
			auto** Acts = reinterpret_cast<UObject**>(Arr.Data);
			UObject* AnyPawn = nullptr;
			for (i32 i = 0; i < Arr.Num; ++i)
			{
				UObject* A = Acts[i];
				if (!LooksObj(A))
					continue;
				if (NameHas(A, XS("PlayerController")))
				{
					if (UObject* P = PawnFromPc(A))
					{
						if (!IsDummyPawn(P))
							return P;
					}
				}
				if (NameHas(A, XS("NR3E3GameCharacter")) || NameHas(A, XS("Pawn"))
					|| NameHas(A, XS("Character")))
				{
					if (!IsDummyPawn(A))
					{
						if (!LocalOnly)
							return A;
						if (!AnyPawn)
							AnyPawn = A;
					}
				}
			}
			return LocalOnly ? nullptr : AnyPawn;
		}

		UObject* PawnInWorld(UObject* World)
		{
			if (!LooksObj(World))
				return nullptr;
			if (UObject* P = GsPawn(World, XS("GetPlayerPawn")))
				if (!IsDummyPawn(P))
					return P;
			if (UObject* P = GsPawn(World, XS("GetPlayerCharacter")))
				if (!IsDummyPawn(P))
					return P;
			if (UObject* Pc = GsPawn(World, XS("GetPlayerController")))
			{
				if (UObject* P = PawnFromPc(Pc))
					if (!IsDummyPawn(P))
						return P;
			}
			UObject* Gi = PtrAt(World, 0x1F0);
			if (Gi)
			{
				TArrayHdr Arr{};
				std::memcpy(&Arr, reinterpret_cast<u8*>(Gi) + 0x38, sizeof(Arr));
				if (ue::ArrayOk(Arr, 8, 8) && Arr.Data)
				{
					auto** Els = reinterpret_cast<UObject**>(Arr.Data);
					for (i32 i = 0; i < Arr.Num; ++i)
					{
						UObject* P = PawnFromPc(PtrAt(Els[i], 0x30));
						if (P && !IsDummyPawn(P))
							return P;
					}
				}
			}
			if (UObject* P = FirstPawnInActors(PtrAt(World, 0x30), true))
				if (!IsDummyPawn(P))
					return P;
			return nullptr;
		}

		UObject* E3GameState()
		{
			if (UObject* Gs = StaticCallRet(XS("MoeNR3ECharComponentLibrary"), XS("GetE3GameState")))
				return Gs;
			if (UObject* Gs = StaticCallRet(XS("MoeNR3E3HelperLibrary"), XS("GetE3GameStateActor")))
				return Gs;
			if (UObject* Cls = ue::FindClass(XS("MoeNR3E3GameStateBase")))
			{
				if (UObject* Inst = ue::FindInstanceOf(Cls))
					return Inst;
			}
			UObject* W = ue::World();
			if (UObject* Gs = PtrAt(W, 0x140))
			{
				if (ue::ClassNameOf(Gs).find(XS("NR3E3")) != std::string::npos)
					return Gs;
			}
			return nullptr;
		}

		UObject* MeetingMgr()
		{
			if (UObject* Gs = E3GameState())
			{
				if (UObject* M = PtrAt(Gs, kOffMeetMgr))
					return M;
			}
			if (UObject* Cls = ue::FindClass(XS("MoeNR3E3MeetingManagerBase")))
			{
				if (UObject* Inst = ue::FindInstanceOf(Cls))
					return Inst;
			}
			return nullptr;
		}

		UObject* TaskMgr()
		{
			if (UObject* Gs = E3GameState())
			{
				if (UObject* M = PtrAt(Gs, kOffGsMgr))
					return M;
			}
			if (UObject* Cls = ue::FindClass(XS("MoeNR3E3TaskManagerBase")))
				return ue::FindInstanceOf(Cls);
			return nullptr;
		}

		UObject* LocalCharacter()
		{
			g_Why.clear();
			if (UObject* C = StaticCallRet(XS("MoeGameLibrary"), XS("GetLocalCharacter")))
				if (!IsDummyPawn(C))
					return C;

			if (UObject* Cls = ue::FindClass(XS("MoeNR3E3GameCharacter")))
			{
				for (UObject* O : ue::InstancesOf(Cls, 16))
				{
					if (!IsDummyPawn(O))
						return O;
				}
			}

			UObject* World = ue::World();
			if (UObject* P = PawnInWorld(World))
				if (!IsDummyPawn(P))
					return P;

			if (UObject* CompCls = ue::FindClass(XS("MoeNR3E3TaskComponentBase")))
			{
				for (UObject* Comp : ue::InstancesOf(CompCls, 16))
				{
					UObject* Ch = PtrAt(Comp, kOffCompOwner);
					if (Ch && !IsDummyPawn(Ch))
						return Ch;
				}
			}

			if (World)
				g_Why = "无Pawn w=" + ue::NameOf(World);
			else
				g_Why = "无World";
			return nullptr;
		}

		UObject* CharBase(UObject* Character)
		{
			if (!Character)
				return nullptr;
			UObject* Helper = ue::FindClass(XS("MoeNR3E3HelperLibrary"));
			if (Helper)
			{
				UObject* Fn = ue::FindFn(Helper, XS("GetNR3E3SCharComponentByCharacter"));
				UObject* Self = ue::CdoOf(Helper);
				if (!Self)
					Self = Helper;
				if (Fn && Self)
				{
					struct
					{
						UObject* Actor;
						UObject* Ret;
					} P{Character, nullptr};
					Invoke(Self, Fn, &P, sizeof(P));
					if (LooksObj(P.Ret))
						return P.Ret;
				}
			}
			UObject* CompClass = ue::FindClass(XS("MoeNR3E3CharBaseComponent"));
			UObject* GetComp = ue::FindFn(Character, XS("GetComponentByClass"));
			if (CompClass && GetComp)
			{
				struct
				{
					UObject* Cls;
					UObject* Ret;
				} P{CompClass, nullptr};
				Invoke(Character, GetComp, &P, sizeof(P));
				if (LooksObj(P.Ret))
					return P.Ret;
			}
			UObject* BpClass = ue::FindClass(XS("BP_CharCompNR3E3Base_C"));
			if (BpClass && GetComp)
			{
				struct
				{
					UObject* Cls;
					UObject* Ret;
				} P{BpClass, nullptr};
				Invoke(Character, GetComp, &P, sizeof(P));
				if (LooksObj(P.Ret))
					return P.Ret;
			}
			return nullptr;
		}

		UObject* TaskComp(UObject* Character, UObject* Base)
		{
			if (Character)
			{
				if (UObject* C = CallRetObj(Character, XS("GetTaskComponent")))
					return C;
				if (UObject* C = PtrAt(Character, kOffCharTask))
					return C;
			}
			if (Base)
			{
				if (UObject* C = CallRetObj(Base, XS("GetTaskComponent")))
					return C;
			}
			if (UObject* Cls = ue::FindClass(XS("MoeNR3E3TaskComponentBase")))
			{
				for (UObject* C : ue::InstancesOf(Cls, 16))
				{
					UObject* Owner = PtrAt(C, kOffCompOwner);
					if (Character && Owner == Character)
						return C;
					if (!Character && LooksObj(C))
						return C;
				}
			}
			return nullptr;
		}

		i64 LocalUid(UObject* Base)
		{
			if (!Base)
				return 0;
			if (UObject* Owner = CallRetObj(Base, XS("GetSafeCharacterOwner")))
			{
				if (const i64 U = CallRetI64(Owner, XS("GetUID")))
					return U;
			}
			return CallRetI64(Base, XS("GetLocalUID"));
		}

		TArrayHdr ReadArrAt(UObject* O, i32 Off)
		{
			TArrayHdr H{};
			if (!LooksObj(O))
				return H;
			std::memcpy(&H, reinterpret_cast<u8*>(O) + Off, sizeof(H));
			return H;
		}

		TArrayHdr ReadTasks(UObject* Comp, bool Urgent)
		{
			TArrayHdr H{};
			if (!Comp)
				return H;
			const char* Getter = Urgent ? XS("GetUrgentTaskArray") : XS("GetTaskArray");
			if (UObject* Fn = ue::FindFn(Comp, Getter))
			{
				Invoke(Comp, Fn, &H, sizeof(H));
				if (ue::ArrayOk(H, kTaskSize))
					return H;
			}
			return ReadArrAt(Comp, Urgent ? kOffCompUrgent : kOffCompTasks);
		}

		bool CompleteOne(UObject* Comp, UObject* Base, UObject* Mgr, i32 Id, i32 Sub)
		{
			(void)Base;
			if (Comp)
			{
				struct
				{
					i32 TaskID;
					i32 SubTaskId;
				} Gm{Id, Sub};
				if (CallFn(Comp, XS("CompleteTaskByGM"), &Gm, sizeof(Gm)))
					return true;
			}
			if (Mgr)
			{
				struct
				{
					i32 TaskID;
					float Extra;
					i32 NextByGM;
				} Mp{Id, 1.f, 1};
				if (CallFn(Mgr, XS("CompleteTask"), &Mp, sizeof(Mp)))
					return true;
			}
			return false;
		}

		i32 CountOpen(const TArrayHdr& Arr, bool UrgentOnly)
		{
			if (!ue::ArrayOk(Arr, kTaskSize))
				return 0;
			i32 N = 0;
			for (i32 i = 0; i < Arr.Num; ++i)
			{
				u8* T = Arr.Data + static_cast<size_t>(i) * kTaskSize;
				const bool Complete = T[kOffDone] != 0;
				const bool Urgent = T[kOffUrgent] != 0;
				const bool Running = T[kOffRun] != 0;
				if (UrgentOnly)
				{
					if (Urgent && Running && !Complete)
						++N;
				}
				else if (!Urgent && !Complete)
					++N;
			}
			return N;
		}

		i32 CompleteKind(bool UrgentOnly)
		{
			UObject* Ch = LocalCharacter();
			UObject* Base = Ch ? CharBase(Ch) : nullptr;
			UObject* Comp = TaskComp(Ch, Base);
			UObject* Mgr = TaskMgr();
			Dbg("CompleteKind ch=%s mgr=%p comp=%p base=%p",
				Ch ? ue::NameOf(Ch).c_str() : "-", Mgr, Comp, Base);

			TArrayHdr Arr{};
			if (Comp)
				Arr = ReadTasks(Comp, UrgentOnly);
			if ((!ue::ArrayOk(Arr, kTaskSize) || Arr.Num == 0) && Mgr)
				Arr = ReadArrAt(Mgr, UrgentOnly ? kOffMgrUrgent : kOffMgrTasks);
			if (!ue::ArrayOk(Arr, kTaskSize))
				return (Mgr || Comp) ? -6 : -4;

			i32 Done = 0;
			for (i32 i = 0; i < Arr.Num; ++i)
			{
				u8* T = Arr.Data + static_cast<size_t>(i) * kTaskSize;
				const bool Complete = T[kOffDone] != 0;
				const bool Urgent = T[kOffUrgent] != 0;
				const bool Running = T[kOffRun] != 0;
				if (UrgentOnly)
				{
					if (!(Urgent && Running && !Complete))
						continue;
				}
				else if (Urgent || Complete)
					continue;
				const i32 Id = *reinterpret_cast<i32*>(T + kOffId);
				Dbg("complete id=%d urgent=%d", Id, (int)Urgent);
				TArrayHdr Subs{};
				std::memcpy(&Subs, T + kOffSub, sizeof(Subs));
				if (!ue::ArrayOk(Subs, kSubSize) || Subs.Num == 0)
				{
					if (CompleteOne(Comp, Base, Mgr, Id, 0))
						++Done;
					continue;
				}
				for (i32 s = 0; s < Subs.Num; ++s)
				{
					u8* Sub = Subs.Data + static_cast<size_t>(s) * kSubSize;
					if (Sub[0x10])
						continue;
					const i32 SubId = *reinterpret_cast<i32*>(Sub);
					if (CompleteOne(Comp, Base, Mgr, Id, SubId))
						++Done;
				}
			}
			if (Done == 0 && Arr.Num == 0)
				return -6;
			return Done;
		}

		i32 TriggerUrgent(i32 Type)
		{
			UObject* Ch = LocalCharacter();
			UObject* Base = Ch ? CharBase(Ch) : nullptr;
			UObject* Comp = TaskComp(Ch, Base);
			UObject* Mgr = TaskMgr();
			TArrayHdr Arr{};
			if (Mgr)
				Arr = ReadArrAt(Mgr, kOffMgrUrgent);
			if ((!ue::ArrayOk(Arr, kTaskSize) || Arr.Num == 0) && Comp)
				Arr = ReadTasks(Comp, true);
			Dbg("TriggerUrgent type=%d n=%d mgr=%p comp=%p", Type, Arr.Num, Mgr, Comp);
			if (!ue::ArrayOk(Arr, kTaskSize))
				return Mgr || Comp ? -6 : -4;
			i32 Done = 0;
			for (i32 i = 0; i < Arr.Num; ++i)
			{
				u8* T = Arr.Data + static_cast<size_t>(i) * kTaskSize;
				const i32 TaskType = static_cast<i32>(T[kOffType]);
				if (T[kOffRun])
					continue;
				if (Type != 0 && TaskType != Type)
					continue;
				i32 Id = *reinterpret_cast<i32*>(T + kOffId);
				Dbg("trigger urgent id=%d type=%d", Id, TaskType);
				bool Ok = false;
				if (Mgr)
				{
					struct
					{
						i32 TaskID;
						i32 Pad;
						UObject* Character;
					} P{Id, 0, Ch};
					Ok = CallFn(Mgr, XS("TriggerUrgentTaskGM"), &P, sizeof(P)) || Ok;
				}
				if (Comp)
				{
					Ok = CallFn(Comp, XS("TriggerUrgentTaskOnServer"), &Id, 4) || Ok;
					Ok = CallFn(Comp, XS("TriggerUrgentTask"), &Id, 4) || Ok;
				}
				if (Ok)
					++Done;
			}
			return Done;
		}

		bool FsmConfig(std::string_view Config)
		{
			UObject* Helper = ue::FindClass(XS("MoeNR3E3HelperLibrary"));
			UObject* Fn = Helper ? ue::FindFn(Helper, XS("ChangeE3FSMGameStateByConfigName")) : nullptr;
			if (!Fn || !Helper)
				return false;
			auto Cfg = ue::MakeFString(Config);
			UObject* Self = ue::CdoOf(Helper);
			if (!Self)
				Self = Helper;
			return Invoke(Self, Fn, &Cfg.View, sizeof(Cfg.View));
		}

		UObject* E2Base(UObject* Ch)
		{
			if (!Ch)
				return nullptr;
			UObject* Cls = ue::FindClass(XS("MoeNR3E2CharBaseComponent"));
			UObject* GetComp = ue::FindFn(Ch, XS("GetComponentByClass"));
			if (!Cls || !GetComp)
				return nullptr;
			struct
			{
				UObject* Cls;
				UObject* Ret;
			} P{Cls, nullptr};
			Invoke(Ch, GetComp, &P, sizeof(P));
			return LooksObj(P.Ret) ? P.Ret : nullptr;
		}

		bool TryReqEvent(UObject* Self, const char* Fn, void* Params, i32 Bytes, const char* Tag)
		{
			if (!Self || !CallFn(Self, Fn, Params, Bytes))
				return false;
			Dbg("ReqEvent hit %s %s", Tag, Fn);
			return true;
		}

		bool FireBus(UObject* Ch, UObject* Base, i64 Uid, std::string_view Event, std::string_view Json)
		{
			auto NameStr = ue::MakeFString(Event);
			auto DataStr = ue::MakeFString(Json);
			struct
			{
				ue::FString EventName;
				i64 Uid;
				ue::FString Data;
			} P{NameStr.View, Uid, DataStr.View};

			if (TryReqEvent(Base, XS("ReqEventLuaCall"), &P, sizeof(P), "base"))
				return true;
			if (TryReqEvent(Base, XS("ReqEvent"), &P, sizeof(P), "base"))
				return true;
			if (TryReqEvent(Ch, XS("ReqEventLuaCall"), &P, sizeof(P), "ch"))
				return true;
			if (TryReqEvent(Ch, XS("ReqEvent"), &P, sizeof(P), "ch"))
				return true;
			if (UObject* E2 = E2Base(Ch))
			{
				if (TryReqEvent(E2, XS("ReqEvent"), &P, sizeof(P), "e2"))
					return true;
			}

			const std::string_view kEnter{XS("Enter_NR3E3_GFSMS_")};
			const std::string_view kExit{XS("Exit_NR3E3_GFSMS_")};
			if (Event.size() > kEnter.size() && Event.substr(0, kEnter.size()) == kEnter)
			{
				const bool Ok = FsmConfig(Event.substr(kEnter.size()));
				Dbg("FsmConfig enter %.*s ok=%d", (int)Event.size(), Event.data(), (int)Ok);
				return Ok;
			}
			if (Event.size() > kExit.size() && Event.substr(0, kExit.size()) == kExit)
			{
				const bool Ok = FsmConfig(Event.substr(kExit.size()));
				Dbg("FsmConfig exit %.*s ok=%d", (int)Event.size(), Event.data(), (int)Ok);
				return Ok;
			}

			Dbg("FireBus miss %.*s", (int)Event.size(), Event.data());
			return false;
		}

		bool FireNamed(UObject* Ch, UObject* Base, i64 Uid, std::string_view Event, std::string_view Json)
		{
			return FireBus(Ch, Base, Uid, Event, Json);
		}

		void FireMeeting(UObject* Ch, UObject* Base, i64 Uid)
		{
			if (g_Meeting.test_and_set(std::memory_order_acquire))
				return;
			FireNamed(Ch, Base, Uid, XS("Enter_NR3E3_GFSMS_MeetingReady"), "[]");
			std::thread([Ch, Base, Uid] {
				prctl(PR_SET_NAME, XS("Profile Saver"), 0, 0, 0);
				std::this_thread::sleep_for(std::chrono::milliseconds(3800));
				if (g_CancelEvt.load(std::memory_order_acquire) == 0 && LooksObj(Ch))
					FireNamed(Ch, Base, Uid, XS("Enter_NR3E3_GFSMS_MeetingDiscuss"), "[]");
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));
				if (g_CancelEvt.load(std::memory_order_acquire) == 0 && LooksObj(Ch))
					FireNamed(Ch, Base, Uid, XS("Enter_NR3E3_GFSMS_MeetingVote"), "[]");
				g_Meeting.clear(std::memory_order_release);
			}).detach();
		}

		struct VotePlayer
		{
			UObject* Ch = nullptr;
			UObject* Base = nullptr;
			i64 Uid = 0;
			i32 Seat = 0;
			char Name[24]{};
		};

		i32 SeatOf(UObject* Base)
		{
			if (!LooksObj(Base))
				return 0;
			return *reinterpret_cast<i32*>(reinterpret_cast<u8*>(Base) + 0x14C);
		}

		i32 MeetPlayerNum(UObject* Meet)
		{
			if (!LooksObj(Meet))
				return 0;
			return *reinterpret_cast<i32*>(reinterpret_cast<u8*>(Meet) + 0x304);
		}

		i32 SnapLobby(i32 N)
		{
			if (N <= 8)
				return 8;
			if (N <= 10)
				return 10;
			return 15;
		}

		i32 g_UidOff = -1;

		i64 UidFromBase(UObject* Base, i64 KnownLocal)
		{
			if (!LooksObj(Base))
				return 0;
			auto* P = reinterpret_cast<u8*>(Base);
			if (g_UidOff < 0 && KnownLocal > 0)
			{
				for (i32 o = 0x100; o <= 0x280; o += 8)
				{
					if (*reinterpret_cast<i64*>(P + o) == KnownLocal)
					{
						g_UidOff = o;
						break;
					}
				}
			}
			if (g_UidOff >= 0)
				return *reinterpret_cast<i64*>(P + g_UidOff);
			return 0;
		}

		void PushEntry(VotePlayer* Out, i32& N, i32 Max, i64 Uid, i32 Seat, UObject* Ch, UObject* Base)
		{
			if (!Out || N >= Max || Uid <= 0)
				return;
			for (i32 i = 0; i < N; ++i)
			{
				if (Out[i].Uid == Uid)
				{
					if (Out[i].Seat <= 0 && Seat > 0)
						Out[i].Seat = Seat;
					if (!Out[i].Ch && LooksObj(Ch))
						Out[i].Ch = Ch;
					if (!Out[i].Base && LooksObj(Base))
						Out[i].Base = Base;
					return;
				}
			}
			Out[N].Ch = Ch;
			Out[N].Base = Base;
			Out[N].Uid = Uid;
			Out[N].Seat = Seat;
			Out[N].Name[0] = 0;
			++N;
		}

		i64 UidOf(UObject* Ch, UObject* Base)
		{
			if (LooksObj(Ch))
			{
				if (const i64 U = CallRetI64(Ch, XS("GetUID")))
					return U;
			}
			if (LooksObj(Base))
			{
				if (UObject* Owner = CallRetObj(Base, XS("GetSafeCharacterOwner")))
				{
					if (const i64 U = CallRetI64(Owner, XS("GetUID")))
						return U;
				}
				if (const i64 U = CallRetI64(Base, XS("GetUID")))
					return U;
			}
			return 0;
		}

		void PushFromComp(VotePlayer* Out, i32& N, i32 Max, UObject* Base, i64 KnownLocal)
		{
			if (!LooksObj(Base) || N >= Max)
				return;
			const i32 Seat = SeatOf(Base);
			i64 Uid = UidFromBase(Base, KnownLocal);
			UObject* Ch = CallRetObj(Base, XS("GetSafeCharacterOwner"));
			if (Uid <= 0)
				Uid = UidOf(Ch, Base);
			PushEntry(Out, N, Max, Uid, Seat, Ch, Base);
		}

		void PushVotePlayer(VotePlayer* Out, i32& N, i32 Max, UObject* Ch, i64 UidHint = 0)
		{
			if (!Out || !LooksObj(Ch) || N >= Max || IsDummyPawn(Ch))
				return;
			UObject* Base = CharBase(Ch);
			i64 Uid = UidHint;
			if (Uid <= 0)
				Uid = UidFromBase(Base, 0);
			if (Uid <= 0)
				Uid = UidOf(Ch, Base);
			if (Uid <= 0)
				return;
			PushEntry(Out, N, Max, Uid, SeatOf(Base), Ch, Base);
		}

		bool LooksUid(i64 K)
		{
			return K > 1000000000LL && K < 900000000000000000LL;
		}

		void CollectFromPlayerMap(VotePlayer* Out, i32& N, i32 Max)
		{
			UObject* Gs = E3GameState();
			if (!LooksObj(Gs) || !Out || Max <= 0)
				return;
			const i32 Offs[] = {0x2C0, 0x2C8, 0x2D0, 0x2D8, 0x2E0, 0x2E8, 0x2F0, 0x2F8, 0x300, 0x308};
			for (i32 Off : Offs)
			{
				TArrayHdr Arr{};
				std::memcpy(&Arr, reinterpret_cast<u8*>(Gs) + Off, sizeof(Arr));
				i32 Stride = 24;
				if (!ue::ArrayOk(Arr, 24, 16))
				{
					Stride = 16;
					if (!ue::ArrayOk(Arr, 16, 16))
						continue;
				}
				for (i32 i = 0; i < Arr.Num && N < Max; ++i)
				{
					u8* E = Arr.Data + static_cast<size_t>(i) * static_cast<size_t>(Stride);
					const i64 Key = *reinterpret_cast<i64*>(E);
					UObject* Ch = *reinterpret_cast<UObject**>(E + 8);
					if (!LooksUid(Key) || !LooksObj(Ch))
						continue;
					if (!NameHas(Ch, XS("Character")) && !NameHas(Ch, XS("Pawn")))
						continue;
					PushVotePlayer(Out, N, Max, Ch, Key);
				}
			}
		}

		void CollectMeetUids(VotePlayer* Out, i32& N, i32 Max)
		{
			UObject* M = MeetingMgr();
			if (!LooksObj(M) || !Out)
				return;
			for (i32 Off = 0x2A0; Off <= 0x3C0 && N < Max; Off += 8)
			{
				TArrayHdr A{};
				std::memcpy(&A, reinterpret_cast<u8*>(M) + Off, sizeof(A));
				if (!ue::ArrayOk(A, 8, 16) || !A.Data || A.Num < 2)
					continue;
				auto* Ks = reinterpret_cast<i64*>(A.Data);
				i32 Hit = 0;
				for (i32 i = 0; i < A.Num; ++i)
				{
					if (LooksUid(Ks[i]))
						++Hit;
				}
				if (Hit < 2 || Hit < A.Num - 1)
					continue;
				for (i32 i = 0; i < A.Num && N < Max; ++i)
				{
					if (!LooksUid(Ks[i]))
						continue;
					PushEntry(Out, N, Max, Ks[i], i + 1, nullptr, nullptr);
				}
			}
		}

		i32 CollectVotePlayers(VotePlayer* Out, i32 Max)
		{
			if (!Out || Max <= 0)
				return 0;
			i32 N = 0;
			UObject* Self = LocalCharacter();
			UObject* SelfBase = Self ? CharBase(Self) : nullptr;
			const i64 Local = LocalUid(SelfBase);
			if (LooksObj(SelfBase) && Local > 0)
				(void)UidFromBase(SelfBase, Local);
			PushVotePlayer(Out, N, Max, Self, Local);
			CollectFromPlayerMap(Out, N, Max);
			CollectMeetUids(Out, N, Max);
			if (UObject* Cls = ue::FindClass(XS("MoeNR3E3CharBaseComponent")))
			{
				for (UObject* C : ue::InstancesOf(Cls, 16))
					PushFromComp(Out, N, Max, C, Local);
			}
			else if (UObject* Cls = ue::FindClass(XS("BP_CharCompNR3E3Base_C")))
			{
				for (UObject* C : ue::InstancesOf(Cls, 16))
					PushFromComp(Out, N, Max, C, Local);
			}
			if (LooksObj(SelfBase))
			{
				TArrayHdr Near{};
				std::memcpy(&Near, reinterpret_cast<u8*>(SelfBase) + 0x158, sizeof(Near));
				if (ue::ArrayOk(Near, 8, 16) && Near.Data)
				{
					auto** Ps = reinterpret_cast<UObject**>(Near.Data);
					for (i32 i = 0; i < Near.Num && N < Max; ++i)
						PushVotePlayer(Out, N, Max, Ps[i]);
				}
			}
			UObject* Level = PtrAt(ue::World(), 0x30);
			if (LooksObj(Level) && N < Max)
			{
				TArrayHdr Acts{};
				std::memcpy(&Acts, reinterpret_cast<u8*>(Level) + 0x98, sizeof(Acts));
				if (ue::ArrayOk(Acts, 8, 256) && Acts.Data)
				{
					auto** As = reinterpret_cast<UObject**>(Acts.Data);
					const i32 Cap = std::min(Acts.Num, 256);
					for (i32 i = 0; i < Cap && N < Max; ++i)
					{
						UObject* A = As[i];
						if (!LooksObj(A))
							continue;
						if (NameHas(A, XS("NR3E3GameCharacter")) || NameHas(A, XS("NR3E0GameCharacter")))
							PushVotePlayer(Out, N, Max, A);
					}
				}
			}
			return N;
		}

		bool FindUidByPlayerIndex(const VotePlayer* Pl, i32 Pn, i32 Index, i64& OutUid)
		{
			if (!Pl || Pn <= 0 || Index <= 0)
				return false;
			for (i32 i = 0; i < Pn; ++i)
			{
				if (Pl[i].Seat == Index)
				{
					OutUid = Pl[i].Uid;
					return true;
				}
			}
			return false;
		}

		bool PcReq(UObject* Pc, const char* Ev, const char* Param)
		{
			if (!LooksObj(Pc) || !Ev || !Param)
				return false;
			auto Name = ue::MakeFString(Ev);
			auto Arg = ue::MakeFString(Param);
			struct
			{
				ue::FString EventName;
				ue::FString Param;
			} P{Name.View, Arg.View};
			return CallFn(Pc, XS("ServerReqEvent"), &P, sizeof(P))
				|| CallFn(Pc, XS("ReqEvent"), &P, sizeof(P));
		}

		UObject* GetComp(UObject* Actor, const char* ClsName)
		{
			if (!LooksObj(Actor) || !ClsName)
				return nullptr;
			UObject* Cls = ue::FindClass(ClsName);
			UObject* Get = ue::FindFn(Actor, XS("GetComponentByClass"));
			if (!Cls || !Get)
				return nullptr;
			struct
			{
				UObject* Cls;
				UObject* Ret;
			} P{Cls, nullptr};
			Invoke(Actor, Get, &P, sizeof(P));
			return LooksObj(P.Ret) ? P.Ret : nullptr;
		}

		void DumpFns(FILE* Fp, UObject* Obj, const char* Tag)
		{
			if (!Fp || !LooksObj(Obj) || !Obj->Class)
				return;
			fprintf(Fp, "[%s] %s\n", Tag, ue::ClassNameOf(Obj).c_str());
			int N = 0;
			for (ue::UStruct* Cur = reinterpret_cast<ue::UStruct*>(Obj->Class); Cur && N < 80;
				 Cur = Cur->Super)
			{
				for (ue::UField* F = Cur->Children; F; F = F->Next)
				{
					const std::string Nm = ue::NameOf(F);
					if (Nm.find("Vote") == std::string::npos && Nm.find("ReqEvent") == std::string::npos
						&& Nm.find("DoSomething") == std::string::npos
						&& Nm.find("ServerRPC") == std::string::npos)
						continue;
					uint32_t Fl = 0;
					std::memcpy(&Fl, reinterpret_cast<u8*>(F) + 0xB0, 4);
					fprintf(Fp, "  %s flags=0x%x net=%d srv=%d\n", Nm.c_str(), Fl,
						(Fl & 0x40) != 0, (Fl & 0x200000) != 0);
					if (++N >= 40)
						return;
				}
			}
		}

		bool CharReq(UObject* Self, i64 TargetUid, const char* Ev, const char* Data)
		{
			if (!LooksObj(Self) || !Ev)
				return false;
			auto Name = ue::MakeFString(Ev);
			auto Ds = ue::MakeFString(Data ? Data : "[]");
			struct
			{
				ue::FString EventName;
				i64 Uid;
				ue::FString Data;
			} P{Name.View, TargetUid, Ds.View};
			return CallFn(Self, XS("ReqEvent"), &P, sizeof(P));
		}

		void VoteLog(const char* Fmt, ...)
		{
			FILE* Fp = fopen("/data/data/com.tencent.letsgo/files/nr3e_vote.txt", "a");
			if (!Fp)
				Fp = fopen("/data/local/tmp/nr3e_vote.txt", "a");
			if (!Fp)
				return;
			va_list Ap;
			va_start(Ap, Fmt);
			std::vfprintf(Fp, Fmt, Ap);
			va_end(Ap);
			std::fputc('\n', Fp);
			fclose(Fp);
		}

		bool ReqEventLuaCall(UObject* CharBase, const char* Event, i64 Uid, const char* Json)
		{
			if (!LooksObj(CharBase) || !Event || !Json)
				return false;
			UObject* CacheComp = nullptr;
			UObject* CacheFn = nullptr;
			ue::CachedLuaCall(&CacheComp, &CacheFn);
			UObject* Self = LooksObj(CacheComp) ? CacheComp : CharBase;
			UObject* Fn = nullptr;
			if (LooksObj(CacheFn) && ue::NameOf(CacheFn) == XS("ReqEventLuaCall"))
				Fn = CacheFn;
			if (!Fn)
				Fn = ue::FindFn(Self, XS("ReqEventLuaCall"));
			if (!Fn)
				Fn = ue::FindFn(CharBase, XS("ReqEventLuaCall"));
			VoteLog("ReqEventLuaCall self=%s fn=%p cache=%p event=%s uid=%lld",
				ue::ClassNameOf(Self).c_str(), Fn, CacheFn, Event, (long long)Uid);
			if (!Fn)
				return false;
			auto Name = ue::MakeFString(Event);
			auto Data = ue::MakeFString(Json);
			struct
			{
				ue::FString EventName;
				i64 UID;
				ue::FString Data;
			} P{Name.View, Uid, Data.View};
			ue::ProcessEvent(Self, Fn, &P);
			return true;
		}

		i32 FireStackedVote(i32 TargetIndex)
		{
			UObject* Ch = LocalCharacter();
			UObject* Comp = Ch ? CharBase(Ch) : nullptr;
			const i64 Local = LocalUid(Comp);
			FILE* Wipe = fopen("/data/data/com.tencent.letsgo/files/nr3e_vote.txt", "w");
			if (Wipe)
				fclose(Wipe);
			VoteLog("FireStackedVote index=%d local=%lld pump=%d comp=%s", TargetIndex,
				(long long)Local, ue::IsGamePump() ? 1 : 0,
				LooksObj(Comp) ? ue::ClassNameOf(Comp).c_str() : "-");
			if (Local == 0 || !LooksObj(Comp))
				return -1;
			VotePlayer Pl[16]{};
			const i32 Pn = CollectVotePlayers(Pl, 16);
			for (i32 i = 0; i < Pn; ++i)
			{
				VoteLog("  ply i=%d uid=%lld index=%d cls=%s", i, (long long)Pl[i].Uid, Pl[i].Seat,
					LooksObj(Pl[i].Ch) ? ue::ClassNameOf(Pl[i].Ch).c_str() : "-");
			}
			i64 TargetUid = Local;
			if (!FindUidByPlayerIndex(Pl, Pn, TargetIndex, TargetUid))
			{
				VoteLog("FindUidByPlayerIndex %d not found n=%d", TargetIndex, Pn);
				return -8;
			}
			char Json[512];
			int Used = std::snprintf(Json, sizeof(Json), "[%lld,%d,[",
				static_cast<long long>(TargetUid), TargetIndex);
			for (int i = 0; i < 50 && Used > 0 && Used < static_cast<int>(sizeof(Json) - 8); ++i)
			{
				Used += std::snprintf(Json + Used, sizeof(Json) - static_cast<size_t>(Used), "%s%d",
					i ? "," : "", i == 49 ? 1 : 2);
			}
			std::snprintf(Json + Used, sizeof(Json) - static_cast<size_t>(Used), "]]");
			VoteLog("self=%lld target=%lld index=%d json=%s", (long long)Local,
				(long long)TargetUid, TargetIndex, Json);
			if (!ReqEventLuaCall(Comp, XS("E3VotePlayerRequest"), Local, Json))
				return -9;
			return 50;
		}

		i32 CastMeetingVotes(i32 Seat, i32 Count)
		{
			(void)Count;
			return FireStackedVote(Seat);
		}

		i32 FireEvent(i32 Action)
		{
			UObject* Ch = LocalCharacter();
			UObject* Base = Ch ? CharBase(Ch) : nullptr;
			UObject* Gs = E3GameState();
			const i64 Uid = LocalUid(Base);
			Dbg("FireEvent act=%d ch=%s gs=%p uid=%lld", Action,
				Ch ? ue::NameOf(Ch).c_str() : "-", Gs, (long long)Uid);
			switch (Action)
			{
			case 1:
				FireMeeting(Ch, Base, Uid);
				return 1;
			case 2:
				return FireNamed(Ch, Base, Uid, XS("Enter_NR3E3_GFSMS_Night"), "[]") ? 1 : 0;
			case 4:
				return FireBus(Ch, Base, Uid,
						   XS("ON_NR3E3_FOGBOUNDNAVIGATOR_PROGRESS_ACTIVE_WIN"), "[]")
					? 1
					: 0;
			case 5:
				g_Enable.store(0, std::memory_order_release);
				g_CancelEvt.store(1, std::memory_order_release);
				return FireBus(Ch, Base, Uid, XS("Exit_NR3E3_GFSMS_GameFinish_ForceExit"), "[]")
					? 1
					: 0;
			case 6:
			{
				g_Enable.store(0, std::memory_order_release);
				g_CancelEvt.store(1, std::memory_order_release);
				const std::string Json = "[" + std::to_string(Uid) + "]";
				return FireBus(Ch, Base, Uid, XS("ON_NR3E3_NEUTRAL_CAMP_WIN"), Json) ? 1 : 0;
			}
			default:
				return -5;
			}
		}

		const char* ErrOf(i32 C)
		{
			switch (C)
			{
			case -1:
				return g_Why.empty() ? "没有本地角色（先进对局）" : g_Why.c_str();
			case -2: return "没有 CharBase";
			case -3: return "没有 Task 组件";
			case -4: return "任务数组读失败";
			case -5: return "未知事件";
			case -6: return "任务列表空（对局里还没刷任务）";
			case -7: return "没找到可投票玩家";
			case -8: return "投票目标序号不对（先点刷新）";
			case -9: return "没有 ReqEventLuaCall（组件上找不到）";
			case -10: return "没有 MeetingManager（还没开会）";
			default: return "";
			}
		}

		Reply Status()
		{
			Reply R;
			R.Enabled = g_Enable.load(std::memory_order_acquire) != 0;
			if (!ue::Ready() && !ue::Init())
			{
				R.Error = "引擎未就绪";
				return R;
			}
			UObject* Ch = LocalCharacter();
			UObject* Base = Ch ? CharBase(Ch) : nullptr;
			UObject* Comp = TaskComp(Ch, Base);
			UObject* Mgr = TaskMgr();
			const i64 Uid = LocalUid(Base);
			i32 Normal = 0, Urgent = 0, MgrN = 0;
			if (Comp)
			{
				const TArrayHdr A = ReadTasks(Comp, false);
				Normal = CountOpen(A, false);
				Urgent = CountOpen(ReadTasks(Comp, true), true);
			}
			if (Mgr)
			{
				const TArrayHdr A = ReadArrAt(Mgr, kOffMgrTasks);
				if (ue::ArrayOk(A, kTaskSize))
					MgrN = A.Num;
			}
			UObject* Meet = MeetingMgr();
			const i32 Lobby = MeetPlayerNum(Meet);
			const i32 SelfSeat = SeatOf(Base);
			char Line[220];
			std::snprintf(Line, sizeof(Line),
				"%s uid=%lld 未完成=%d 紧急=%d hook=%d 人数=%d 自己=%d号",
				R.Enabled ? "自动完成开" : "手动",
				static_cast<long long>(Uid), Normal, Urgent,
				ue::PeHooked() ? 1 : 0, Lobby, SelfSeat);
			R.Message = Line;
			R.Touched = Lobby;
			return R;
		}

		Reply VoteInfo()
		{
			Reply R;
			R.Enabled = g_Enable.load(std::memory_order_acquire) != 0;
			if (!ue::Ready() && !ue::Init())
			{
				R.Error = "引擎未就绪";
				return R;
			}
			UObject* Ch = LocalCharacter();
			UObject* Base = Ch ? CharBase(Ch) : nullptr;
			const i64 SelfUid = LocalUid(Base);
			const i32 Self = SeatOf(Base);
			VotePlayer Pl[16]{};
			const i32 Pn = CollectVotePlayers(Pl, 16);
			i32 Lobby = MeetPlayerNum(MeetingMgr());
			if (Lobby < Self)
				Lobby = Self;
			if (Lobby < Pn)
				Lobby = Pn;
			char Line[160];
			int Used = std::snprintf(Line, sizeof(Line), "V|%d|%d|", Lobby, Self);
			i32 Shown = 0;
			bool Have[16]{};
			for (i32 i = 0; i < Pn && Used > 0 && Used < static_cast<int>(sizeof(Line) - 8); ++i)
			{
				if (Pl[i].Uid == SelfUid)
					continue;
				i32 Seat = Pl[i].Seat;
				if (Seat <= 0 || Seat >= 16)
					continue;
				if (Have[Seat])
					continue;
				Have[Seat] = true;
				Used += std::snprintf(Line + Used, sizeof(Line) - static_cast<size_t>(Used),
					"%s%d", Shown ? "," : "", Seat);
				++Shown;
			}
			if (Shown == 0 && Lobby >= 2)
			{
				for (i32 S = 1; S <= Lobby && Used > 0 && Used < static_cast<int>(sizeof(Line) - 8);
					 ++S)
				{
					if (Self > 0 && S == Self)
						continue;
					Used += std::snprintf(Line + Used, sizeof(Line) - static_cast<size_t>(Used),
						"%s%d", Shown ? "," : "", S);
					++Shown;
				}
			}
			R.Message = Line;
			R.Touched = (Self << 8) | (Shown & 0xFF);
			if (Shown <= 0)
				R.Error = "没读到对局玩家";
			return R;
		}

		// FMoeNR3EVocationDefinition: UID, VocationId, VocationCamp. SDK size 0x48.
		// Owner 块是原身份；当前块可能被易容改掉。
		constexpr i32 kVocCur = 0x178;
		constexpr i32 kVocOwn = 0x1C0;
		constexpr i32 kInfoIndex = 0x5D0;
		constexpr i32 kInfoEncrypt = 0x648;
		constexpr i32 kInfoOwnerSide = 0x64C;
		constexpr i32 kInfoOwnerCamp = 0x650;
		constexpr i32 kDeckOff = 0x620;

		bool SideOk(i32 S)
		{
			return S > 0 && S <= 1200;
		}

		bool CampOk(i32 C)
		{
			return C >= 1 && C <= 3;
		}

		i32 ReadI32(UObject* O, i32 Off)
		{
			if (!LooksObj(O))
				return 0;
			return *reinterpret_cast<i32*>(reinterpret_cast<u8*>(O) + Off);
		}

		struct IdSeat
		{
			i32 Seat = 0;
			i32 Side = 0;
			i32 Camp = 0;
			i64 Uid = 0;
		};

		void PutId(IdSeat* A, i32& N, i32 Max, i32 Seat, i32 Side, i32 Camp, i64 Uid)
		{
			if (!A || Max <= 0)
				return;
			if (!SideOk(Side))
				Side = 0;
			if (!CampOk(Camp))
				Camp = 0;
			if (Seat < 0 || Seat > 16)
				Seat = 0;
			if (Uid > 0 && !LooksUid(Uid))
				Uid = 0;
			for (i32 i = 0; i < N; ++i)
			{
				const bool SameUid = Uid > 0 && A[i].Uid == Uid;
				const bool SameSeat = Seat > 0 && A[i].Seat == Seat;
				if (!SameUid && !SameSeat)
					continue;
				if (A[i].Seat <= 0 && Seat > 0)
					A[i].Seat = Seat;
				if (A[i].Uid <= 0 && Uid > 0)
					A[i].Uid = Uid;
				if (!SideOk(A[i].Side) && SideOk(Side))
				{
					A[i].Side = Side;
					if (CampOk(Camp))
						A[i].Camp = Camp;
				}
				if (!CampOk(A[i].Camp) && CampOk(Camp) && (!SideOk(Side) || A[i].Side == Side))
					A[i].Camp = Camp;
				return;
			}
			if (N >= Max)
				return;
			if (Seat <= 0 && !SideOk(Side))
				return;
			A[N].Seat = Seat;
			A[N].Side = Side;
			A[N].Camp = Camp;
			A[N].Uid = Uid;
			++N;
		}

		void ReadVocPack(u8* P, i64& Uid, i32& Side, i32& Camp)
		{
			Uid = 0;
			Side = 0;
			Camp = 0;
			if (!P)
				return;
			std::memcpy(&Uid, P, 8);
			std::memcpy(&Side, P + 8, 4);
			std::memcpy(&Camp, P + 12, 4);
		}

		struct FeatKey
		{
			i64 Uid = 0;
			i32 CampK = 0;
			i32 SideK = 0;
		};

		// 训练营「显示身份」把全员密钥写进自己的 EncryptFeatureKeyList。明文 = 密文 - 密钥。
		i32 LoadFeatKeys(FeatKey* Out, i32 Max, i64 Prefer)
		{
			if (!Out || Max <= 0)
				return 0;
			UObject* Cls = ue::FindClass(XS("NR3E3GamePlayerInfo"));
			if (!Cls)
				Cls = ue::FindClass(XS("MoeNR3E3GamePlayerInfo"));
			if (!Cls)
				return 0;
			UObject* Pick = nullptr;
			i32 PickN = -1;
			for (UObject* Info : ue::InstancesOf(Cls, 16))
			{
				if (!LooksObj(Info))
					continue;
				const i64 Uid = *reinterpret_cast<i64*>(reinterpret_cast<u8*>(Info) + 0x298);
				const TArrayHdr A = ReadArrAt(Info, 0x658);
				if (!ue::ArrayOk(A, 0x10, 16) || !A.Data || A.Num <= 0)
					continue;
				if (Prefer > 0 && Uid == Prefer)
				{
					Pick = Info;
					break;
				}
				if (A.Num > PickN)
				{
					PickN = A.Num;
					Pick = Info;
				}
			}
			if (!Pick)
				return 0;
			const TArrayHdr A = ReadArrAt(Pick, 0x658);
			if (!ue::ArrayOk(A, 0x10, 16) || !A.Data)
				return 0;
			i32 N = 0;
			for (i32 i = 0; i < A.Num && N < Max; ++i)
			{
				FeatKey K;
				std::memcpy(&K.Uid, A.Data + static_cast<size_t>(i) * 0x10, 8);
				std::memcpy(&K.CampK, A.Data + static_cast<size_t>(i) * 0x10 + 8, 4);
				std::memcpy(&K.SideK, A.Data + static_cast<size_t>(i) * 0x10 + 12, 4);
				if (!LooksUid(K.Uid))
					continue;
				Out[N++] = K;
			}
			return N;
		}

		void ApplyFeat(i64 Uid, i32& Side, i32& Camp, const FeatKey* Keys, i32 KeyN)
		{
			if (!Keys || KeyN <= 0 || !LooksUid(Uid))
				return;
			if (SideOk(Side) && CampOk(Camp))
				return;
			for (i32 i = 0; i < KeyN; ++i)
			{
				if (Keys[i].Uid != Uid)
					continue;
				if (!SideOk(Side) && Keys[i].SideK != 0)
				{
					const i32 P = Side - Keys[i].SideK;
					if (SideOk(P))
						Side = P;
				}
				if (!CampOk(Camp) && Keys[i].CampK != 0)
				{
					const i32 P = Camp - Keys[i].CampK;
					if (CampOk(P))
						Camp = P;
				}
				return;
			}
		}

		void CollectVocation(IdSeat* A, i32& N, i32 Max, const FeatKey* Keys, i32 KeyN)
		{
			UObject* Cls = ue::FindClass(XS("MoeNR3EVocationComponent"));
			if (!Cls)
				return;
			for (UObject* C : ue::InstancesOf(Cls, 16))
			{
				if (!LooksObj(C))
					continue;
				u8* B = reinterpret_cast<u8*>(C);
				i64 OwnUid = 0, CurUid = 0;
				i32 OwnSide = 0, OwnCamp = 0, CurSide = 0, CurCamp = 0;
				ReadVocPack(B + kVocOwn, OwnUid, OwnSide, OwnCamp);
				ReadVocPack(B + kVocCur, CurUid, CurSide, CurCamp);
				i32 Side = 0, Camp = 0;
				i64 Uid = 0;
				if (SideOk(OwnSide))
				{
					Side = OwnSide;
					Camp = OwnCamp;
					Uid = LooksUid(OwnUid) ? OwnUid : CurUid;
				}
				else
				{
					Side = CurSide;
					Camp = CurCamp;
					Uid = LooksUid(CurUid) ? CurUid : OwnUid;
				}
				ApplyFeat(Uid, Side, Camp, Keys, KeyN);
				UObject* Ch = nullptr;
				if (LooksObj(C->Outer)
					&& (NameHas(C->Outer, XS("Character")) || NameHas(C->Outer, XS("Pawn"))))
					Ch = C->Outer;
				if (!Ch)
					Ch = CallRetObj(C, XS("GetOwner"));
				if (!LooksUid(Uid) && LooksObj(Ch))
					Uid = CallRetI64(Ch, XS("GetUID"));
				ApplyFeat(Uid, Side, Camp, Keys, KeyN);
				i32 Seat = 0;
				if (LooksObj(Ch))
					Seat = SeatOf(CharBase(Ch));
				PutId(A, N, Max, Seat, Side, Camp, Uid);
			}
		}

		void CollectPlayerInfo(IdSeat* A, i32& N, i32 Max)
		{
			UObject* Cls = ue::FindClass(XS("NR3E3GamePlayerInfo"));
			if (!Cls)
				Cls = ue::FindClass(XS("MoeNR3E3GamePlayerInfo"));
			if (!Cls)
				return;
			for (UObject* Info : ue::InstancesOf(Cls, 16))
			{
				if (!LooksObj(Info))
					continue;
				if (CallRetI32(Info, XS("GetIsSpectator")) != 0)
					continue;
				const i64 Uid = *reinterpret_cast<i64*>(reinterpret_cast<u8*>(Info) + 0x298);
				if (!LooksUid(Uid))
					continue;
				i32 Seat = CallRetI32(Info, XS("GetPlayerIndex"));
				if (Seat <= 0 || Seat > 16)
					Seat = ReadI32(Info, kInfoIndex);
				if (Seat < 0 || Seat > 16)
					Seat = 0;
				i32 Side = CallRetI32(Info, XS("ClientGetSideId"));
				i32 Camp = CallRetI32(Info, XS("ClientGetCamp"));
				const i32 OwnSide = CallRetI32(Info, XS("GetOwnerSide"));
				const i32 OwnCamp = CallRetI32(Info, XS("GetOwnerCamp"));
				if (SideOk(OwnSide))
				{
					Side = OwnSide;
					if (CampOk(OwnCamp))
						Camp = OwnCamp;
				}
				if (!SideOk(Side) && ReadI32(Info, kInfoEncrypt) == 0)
				{
					const i32 RawSide = ReadI32(Info, kInfoOwnerSide);
					const i32 RawCamp = ReadI32(Info, kInfoOwnerCamp);
					if (SideOk(RawSide))
					{
						Side = RawSide;
						if (CampOk(RawCamp))
							Camp = RawCamp;
					}
				}
				PutId(A, N, Max, Seat, Side, Camp, Uid);
			}
		}

		// 身份池只有在 SortID 能对上已有座位、且那些座位还没有身份时才填。
		// 对不上就不当成某人的身份，避免把角色池按顺序安到错误的座位。
		void FillDeck(IdSeat* A, i32 N)
		{
			if (!A || N <= 0)
				return;
			i32 Blank = 0;
			for (i32 i = 0; i < N; ++i)
			{
				if (!SideOk(A[i].Side))
					++Blank;
			}
			if (Blank == 0)
				return;
			UObject* Cls = ue::FindClass(XS("NR3E3GameInfo"));
			if (!Cls)
				return;
			for (UObject* G : ue::InstancesOf(Cls, 4))
			{
				const TArrayHdr Deck = ReadArrAt(G, kDeckOff);
				if (!ue::ArrayOk(Deck, 0x10, 16) || !Deck.Data || Deck.Num < 1)
					continue;
				for (i32 i = 0; i < Deck.Num; ++i)
				{
					u8* E = Deck.Data + static_cast<size_t>(i) * 0x10;
					const i32 Camp = *reinterpret_cast<i32*>(E);
					const i32 Side = *reinterpret_cast<i32*>(E + 4);
					const i32 Sort = *reinterpret_cast<i32*>(E + 8);
					if (!SideOk(Side) || Sort <= 0 || Sort > 16)
						continue;
					for (i32 k = 0; k < N; ++k)
					{
						if (A[k].Seat != Sort || SideOk(A[k].Side))
							continue;
						A[k].Side = Side;
						if (!CampOk(A[k].Camp) && CampOk(Camp))
							A[k].Camp = Camp;
					}
				}
			}
		}

		bool CallStatic(const char* ClsName, const char* Fn, void* Params, i32 Bytes)
		{
			UObject* Cls = ue::FindClass(ClsName);
			if (!Cls)
				return false;
			UObject* F = ue::FindFn(Cls, Fn);
			if (!F)
				return false;
			UObject* Self = ue::CdoOf(Cls);
			if (!Self)
				Self = Cls;
			return Invoke(Self, F, Params, Bytes);
		}

		// 死亡后客户端才会发的那条可靠 Server RPC。Type=1 是 ENR3E3EncryptRequestType::AllPlayer。
		// 活着时本地 Role 不是 Authority，库函数会直接返回；必须打在自己的 Encrypt 组件上才会发包。
		UObject* LocalEncrypt(UObject* Ch, i64 Uid)
		{
			if (LooksObj(Ch))
			{
				if (UObject* C = GetComp(Ch, XS("NR3E3EncryptComponent")))
					return C;
			}
			UObject* Cls = ue::FindClass(XS("NR3E3EncryptComponent"));
			if (!Cls)
				return nullptr;
			UObject* Only = nullptr;
			i32 Count = 0;
			for (UObject* C : ue::InstancesOf(Cls, 24))
			{
				if (!LooksObj(C))
					continue;
				++Count;
				Only = C;
				const i64 OwnerUid = CallRetI64(C, XS("GetOwnerUID"));
				if (Uid > 0 && OwnerUid == Uid)
					return C;
				if (UObject* Owner = CallRetObj(C, XS("GetOwner")))
				{
					if (Ch && Owner == Ch)
						return C;
					if (const i64 U = CallRetI64(Owner, XS("GetUID")))
					{
						if (Uid > 0 && U == Uid)
							return C;
					}
				}
			}
			return Count == 1 ? Only : nullptr;
		}

		i32 RequestHiddenSides(UObject* Ch, i64 SelfUid, const IdSeat* Rows, i32 N)
		{
			bool Missing = N <= 0;
			for (i32 i = 0; i < N; ++i)
			{
				if (!SideOk(Rows[i].Side))
					Missing = true;
			}
			if (!Missing || SelfUid <= 0)
				return 0;
			UObject* Enc = LocalEncrypt(Ch, SelfUid);
			const auto Now = std::chrono::steady_clock::now();
			const bool Cooling = g_LastIdReq.time_since_epoch().count() != 0
				&& Now - g_LastIdReq < std::chrono::seconds(4);
			if (Cooling)
				return LooksObj(Enc) ? 0 : -2;
			g_LastIdReq = Now;
			i32 Sent = 0;
			if (LooksObj(Enc))
			{
				i32 Type = 1;
				if (CallFn(Enc, XS("RequestAllPlayerEncryptFeatureKey"), &Type, 4))
					++Sent;
				for (i32 Camp = 1; Camp <= 3; ++Camp)
				{
					if (CallFn(Enc, XS("RequestCampEncryptFeatureKey"), &Camp, 4))
						++Sent;
				}
				for (i32 i = 0; i < N; ++i)
				{
					if (!LooksUid(Rows[i].Uid) || Rows[i].Uid == SelfUid)
						continue;
					i64 Id = Rows[i].Uid;
					if (CallFn(Enc, XS("RequestPlayerEncryptFeatureKey"), &Id, 8))
						++Sent;
				}
			}
			if (UObject* World = ue::World())
			{
				struct
				{
					UObject* World;
					i64 Uid;
				} All{World, SelfUid};
				if (CallStatic(XS("MoeNR3EGameLibrary"), XS("RequestAllPlayerVocationInfo"), &All,
						sizeof(All)))
					++Sent;
				for (i32 Camp = 1; Camp <= 3; ++Camp)
				{
					struct
					{
						UObject* World;
						i64 Uid;
						i32 Camp;
						i32 Pad;
					} ByCamp{World, SelfUid, Camp, 0};
					if (CallStatic(XS("MoeNR3EGameLibrary"), XS("RequestVocationInfoByCamp"), &ByCamp,
							sizeof(ByCamp)))
						++Sent;
				}
				for (i32 i = 0; i < N; ++i)
				{
					if (!LooksUid(Rows[i].Uid))
						continue;
					struct
					{
						UObject* World;
						i64 Ask;
						i64 Target;
					} One{World, SelfUid, Rows[i].Uid};
					if (CallStatic(XS("MoeNR3EGameLibrary"), XS("RequestVocationInfoSpecificPlayer"),
							&One, sizeof(One)))
						++Sent;
				}
			}
			Dbg("RequestHidden sent=%d self=%lld enc=%d", Sent, (long long)SelfUid,
				LooksObj(Enc) ? 1 : 0);
			if (!LooksObj(Enc))
				return -2;
			return Sent;
		}

		Reply Identity()
		{
			Reply R;
			R.Enabled = g_Enable.load(std::memory_order_acquire) != 0;
			if (!ue::Ready() && !ue::Init())
			{
				R.Error = "引擎未就绪";
				R.Message = "I|0|";
				return R;
			}
			UObject* Self = LocalCharacter();
			const i32 SelfSeat = SeatOf(Self ? CharBase(Self) : nullptr);
			const i64 SelfUid = LocalUid(Self ? CharBase(Self) : nullptr);
			FeatKey Keys[16]{};
			const i32 KeyN = LoadFeatKeys(Keys, 16, SelfUid);
			IdSeat Rows[16]{};
			i32 N = 0;
			CollectPlayerInfo(Rows, N, 16);
			CollectVocation(Rows, N, 16, Keys, KeyN);
			FillDeck(Rows, N);
			const i32 Asked = RequestHiddenSides(Self, SelfUid, Rows, N);
			if (Asked > 0)
			{
				N = 0;
				CollectPlayerInfo(Rows, N, 16);
				CollectVocation(Rows, N, 16, Keys, KeyN);
				FillDeck(Rows, N);
			}
			std::sort(Rows, Rows + N, [](const IdSeat& A, const IdSeat& B) {
				if ((A.Seat <= 0) != (B.Seat <= 0))
					return A.Seat > 0;
				if (A.Seat != B.Seat)
					return A.Seat < B.Seat;
				return A.Uid < B.Uid;
			});
			std::uint64_t Mix = 1469598103934665603ull;
			for (i32 i = 0; i < N; ++i)
			{
				if (!LooksUid(Rows[i].Uid))
					continue;
				Mix ^= static_cast<std::uint64_t>(Rows[i].Uid);
				Mix *= 1099511628211ull;
			}
			const std::uint32_t Hash = static_cast<std::uint32_t>(Mix ^ (Mix >> 32));
			char Line[176];
			int Used = std::snprintf(Line, sizeof(Line), "I|%d|%08x|", SelfSeat > 0 ? SelfSeat : 0, Hash);
			i32 Known = 0;
			for (i32 i = 0; i < N && Used > 0 && Used < 168; ++i)
			{
				const int Add = std::snprintf(Line + Used, sizeof(Line) - static_cast<size_t>(Used),
					"%d:%d:%d|", Rows[i].Seat, Rows[i].Side, Rows[i].Camp);
				if (Add < 0 || Used + Add >= static_cast<int>(sizeof(Line)))
					break;
				Used += Add;
				if (SideOk(Rows[i].Side))
					++Known;
			}
			R.Message = Line;
			R.Touched = Known;
			if (N <= 0)
				R.Error = "没读到身份（先进对局）";
			else if (Asked == -2 && Known < N)
				R.Error = "没有加密组件，身份请求发不出去";
			Dbg("Identity self=%d n=%d known=%d ask=%d %s", SelfSeat, N, Known, Asked, Line);
			return R;
		}
	}

	Reply DoMutate(int32_t Command, int32_t Extra)
	{
		std::lock_guard<std::mutex> Mut(g_MutMu);
		Reply R;
		R.Enabled = g_Enable.load(std::memory_order_acquire) != 0;
		i32 Touched = 0;
		const auto Op = static_cast<Cmd>(Command);
		switch (Op)
		{
		case Cmd::CompleteNormal: Touched = CompleteKind(false); break;
		case Cmd::CompleteUrgent:
		{
			Touched = CompleteKind(true);
			if (Touched <= 0)
			{
				(void)TriggerUrgent(0);
				Touched = CompleteKind(true);
			}
			if (Touched <= 0)
				Touched = TriggerUrgent(0);
			break;
		}
		case Cmd::TriggerUrgent: Touched = TriggerUrgent(Extra); break;
		case Cmd::FireEvent: Touched = FireEvent(Extra); break;
		case Cmd::MeetingVote:
		{
			const i32 Seat = Extra & 0xFF;
			i32 Cnt = (Extra >> 8) & 0xFF;
			if (Cnt == 0)
				Cnt = 50;
			Touched = CastMeetingVotes(Seat, Cnt);
			if (Touched >= 0)
			{
				char Line[64];
				std::snprintf(Line, sizeof(Line), "已投序号 %d x%d", Seat, Cnt);
				R.Touched = Touched;
				R.Message = Line;
				return R;
			}
			break;
		}
		default:
			R.Error = "未知命令";
			return R;
		}
		R.Touched = Touched;
		if (Touched < 0)
		{
			R.Error = ErrOf(Touched);
			return R;
		}
		char Line[64];
		std::snprintf(Line, sizeof(Line), "完成 %d", Touched);
		R.Message = Line;
		return R;
	}

	void Pump()
	{
		if (g_Enable.load(std::memory_order_acquire) == 0)
			return;
		const auto Now = std::chrono::steady_clock::now();
		if (g_LastAuto.time_since_epoch().count() != 0
			&& Now - g_LastAuto < std::chrono::milliseconds(450))
			return;
		g_LastAuto = Now;
		std::lock_guard<std::mutex> Mut(g_MutMu);
		CompleteKind(false);
	}

	Reply Run(std::int32_t Command, std::int32_t Extra)
	{
		Dbg("Run cmd=%d extra=%d", Command, Extra);
		Reply R;
		R.Enabled = g_Enable.load(std::memory_order_acquire) != 0;
		const auto Op = static_cast<Cmd>(Command);
		if (Op == Cmd::SetEnable)
		{
			g_Enable.store(Extra != 0 ? 1 : 0, std::memory_order_release);
			R.Enabled = Extra != 0;
			if (!ue::Ready())
				ue::Init();
			if (!R.Enabled)
			{
				R.Message = "已关闭自动完成";
				return R;
			}
			i32 Touched = 0;
			{
				std::lock_guard<std::mutex> Mut(g_MutMu);
				Touched = CompleteKind(false);
			}
			char Line[160];
			std::snprintf(Line, sizeof(Line), "自动完成已开 本次完成=%d",
				Touched < 0 ? 0 : Touched);
			R.Touched = Touched < 0 ? 0 : Touched;
			R.Message = Line;
			if (Touched < 0)
				R.Error = ErrOf(Touched);
			return R;
		}
		if (Op == Cmd::Status)
			return Status();
		if (Op == Cmd::VoteInfo)
			return VoteInfo();
		if (Op == Cmd::Identity)
			return Identity();
		if (!ue::Ready() && !ue::Init())
		{
			R.Error = "引擎未就绪";
			return R;
		}
		Dbg("mutate cmd=%d extra=%d", Command, Extra);
		return DoMutate(Command, Extra);
	}
}
