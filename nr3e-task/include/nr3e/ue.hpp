#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace nr3e::ue
{
	using u8 = std::uint8_t;
	using i16 = std::int16_t;
	using u16 = std::uint16_t;
	using i32 = std::int32_t;
	using u32 = std::uint32_t;
	using i64 = std::int64_t;
	using u64 = std::uint64_t;

	struct TArrayHdr
	{
		u8* Data = nullptr;
		i32 Num = 0;
		i32 Max = 0;
	};

	struct FString
	{
		char16_t* Data = nullptr;
		i32 Num = 0;
		i32 Max = 0;
	};

	struct UObject
	{
		void* VTable;
		i32 Flags;
		i32 Index;
		UObject* Class; // UClass*
		i32 NameIndex;
		u32 NameNumber;
		UObject* Outer;
	};

	struct UField : UObject
	{
		UField* Next;
	};

	struct UStruct : UField
	{
		void* ChainArray;
		i32 ChainCount;
		i32 PadChain;
		UStruct* Super;
		UField* Children;
		void* ChildProperties;
		i32 Size;
		i16 MinAlign;
	};

	struct UFunction : UStruct
	{
	};

	struct UClass : UStruct
	{
	};

	[[nodiscard]] bool Init();
	[[nodiscard]] bool Ready();
	[[nodiscard]] uintptr_t ImageBase();
	[[nodiscard]] i32 ObjectCount();

	[[nodiscard]] UObject* Get(i32 Index);
	[[nodiscard]] std::string NameOf(const UObject* Obj);
	[[nodiscard]] std::string ClassNameOf(const UObject* Obj);

	[[nodiscard]] UObject* Find(std::string_view Name);
	[[nodiscard]] UObject* FindClass(std::string_view Name);
	[[nodiscard]] UObject* FindInstanceOf(UObject* Cls);
	[[nodiscard]] std::vector<UObject*> InstancesOf(UObject* Cls, i32 Max = 24);
	[[nodiscard]] bool IsA(const UObject* Obj, const UObject* Cls);
	[[nodiscard]] UObject* FindFn(UObject* Obj, std::string_view Name);
	[[nodiscard]] UObject* FindFunction(std::string_view Name);
	[[nodiscard]] UObject* CdoOf(UObject* Cls);
	[[nodiscard]] UObject* World();
	[[nodiscard]] i32 FnParmsSize(UObject* Fn);

	void ProcessEvent(UObject* Self, UObject* Fn, void* Params);
	void SetGamePump(void (*Fn)());
	bool HookProcessEvent();
	bool HookObject(UObject* Obj);
	[[nodiscard]] bool PeHooked();
	[[nodiscard]] bool IsGamePump();
	void CachedLuaCall(UObject** Comp, UObject** Fn);
	void DumpFn(UObject* Fn, std::FILE* Fp);
	std::string FillLuaParms(UObject* Fn, void* Params, i32 Cap, std::string_view Event, i64 Uid,
		std::string_view Json);

	[[nodiscard]] bool ArrayOk(const TArrayHdr& A, i32 ElemSize, i32 MaxNum = 512);

	struct OwnedStr
	{
		std::u16string Keep;
		FString View{};
	};

	[[nodiscard]] OwnedStr MakeFString(std::string_view Utf8);
}
