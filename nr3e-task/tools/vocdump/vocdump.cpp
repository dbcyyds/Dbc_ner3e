// One-shot dump of NR3E SideID -> Chinese vocation name.
//
// The names are not stored as a plaintext table in the pak. They are loaded
// into the game process with the wolf-mode UI. Run this while 元梦之星 is open
// and the werewolf lobby or a match has been entered at least once:
//
//   su -c /data/local/tmp/nr3e-voc
//
// Writes /data/local/tmp/nr3e-side-names.tsv
// and, when the id is not sitting next to the string,
// /data/local/tmp/nr3e-side-loc.tsv (localization key -> Chinese).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
	using u8 = std::uint8_t;
	using u32 = std::uint32_t;
	using u64 = std::uint64_t;
	using i32 = std::int32_t;

	struct Map
	{
		u64 Start = 0;
		u64 End = 0;
		bool FileBacked = false;
	};

	struct NameHit
	{
		u64 Addr = 0;
		int Chars = 0;
		std::string Zh;
	};

	struct LocHit
	{
		std::string Key;
		std::string Zh;
	};

	struct IdHit
	{
		int Id = 0;
		int Camp = 0;
		std::string Zh;
	};

	class Mem
	{
	public:
		explicit Mem(int Fd) : Fd_(Fd) {}
		~Mem()
		{
			if (Fd_ >= 0)
				close(Fd_);
		}
		Mem(const Mem&) = delete;
		Mem& operator=(const Mem&) = delete;

		[[nodiscard]] bool Read(u64 Addr, void* Dst, std::size_t N) const
		{
			auto* Out = static_cast<u8*>(Dst);
			std::size_t Got = 0;
			while (Got < N)
			{
				const ssize_t R = pread(Fd_, Out + Got, N - Got, static_cast<off_t>(Addr + Got));
				if (R <= 0)
					return false;
				Got += static_cast<std::size_t>(R);
			}
			return true;
		}

		[[nodiscard]] std::vector<u8> ReadSome(u64 Addr, std::size_t N) const
		{
			std::vector<u8> Buf(N);
			std::size_t Got = 0;
			while (Got < N)
			{
				const ssize_t R = pread(Fd_, Buf.data() + Got, N - Got, static_cast<off_t>(Addr + Got));
				if (R <= 0)
					break;
				Got += static_cast<std::size_t>(R);
			}
			Buf.resize(Got);
			return Buf;
		}

	private:
		int Fd_ = -1;
	};

	[[nodiscard]] bool IsCjk(char32_t C)
	{
		return C >= 0x4E00 && C <= 0x9FFF;
	}

	[[nodiscard]] int Utf8Cjk(const u8* P, const u8* End, char32_t& Out)
	{
		if (P >= End)
			return 0;
		const u8 B = P[0];
		if (B < 0xE4 || B > 0xE9 || P + 3 > End)
			return 0;
		const char32_t Cp = (char32_t(B & 0x0F) << 12) | (char32_t(P[1] & 0x3F) << 6) | char32_t(P[2] & 0x3F);
		if ((P[1] & 0xC0) != 0x80 || (P[2] & 0xC0) != 0x80 || !IsCjk(Cp))
			return 0;
		Out = Cp;
		return 3;
	}

	[[nodiscard]] std::optional<std::string> CjkRun(const u8* P, const u8* End, int& Bytes, int Min, int Max)
	{
		std::string S;
		int N = 0;
		const u8* Q = P;
		while (Q < End && N < Max)
		{
			char32_t Cp = 0;
			const int K = Utf8Cjk(Q, End, Cp);
			if (K == 0)
				break;
			S.append(reinterpret_cast<const char*>(Q), static_cast<std::size_t>(K));
			Q += K;
			++N;
		}
		Bytes = static_cast<int>(Q - P);
		if (N < Min || N > Max)
			return std::nullopt;
		return S;
	}

	[[nodiscard]] bool KeyChar(u8 C)
	{
		return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '_';
	}

	[[nodiscard]] bool InterestingKey(std::string_view K)
	{
		return K.find("NR3E") != std::string_view::npos || K.find("Vocation") != std::string_view::npos;
	}

	[[nodiscard]] bool ParseInt(const u8* P, const u8* End, int& Out, int& Used)
	{
		if (P >= End || P[0] < '0' || P[0] > '9')
			return false;
		int V = 0;
		int N = 0;
		while (P + N < End && P[N] >= '0' && P[N] <= '9' && N < 5)
		{
			V = V * 10 + (P[N] - '0');
			++N;
		}
		if (N == 0 || V > 1300)
			return false;
		Out = V;
		Used = N;
		return true;
	}

	void HarvestRows(const u8* Buf, std::size_t N, std::vector<IdHit>& Rows)
	{
		const u8* End = Buf + N;
		for (std::size_t i = 0; i + 8 < N; ++i)
		{
			int Id = 0, IdN = 0;
			if (!ParseInt(Buf + i, End, Id, IdN))
				continue;
			if (i > 0 && Buf[i - 1] >= '0' && Buf[i - 1] <= '9')
				continue;
			const u8 Sep = Buf[i + static_cast<std::size_t>(IdN)];
			if (Sep != '\t' && Sep != ',' && Sep != '|')
				continue;
			int Camp = 0, CampN = 0;
			const u8* CampP = Buf + i + static_cast<std::size_t>(IdN) + 1;
			if (!ParseInt(CampP, End, Camp, CampN) || Camp > 3)
				continue;
			if (CampP[CampN] != Sep)
				continue;
			int Bytes = 0;
			auto Zh = CjkRun(CampP + CampN + 1, End, Bytes, 2, 6);
			if (!Zh)
				continue;
			Rows.push_back(IdHit{Id, Camp, *Zh});
			i += static_cast<std::size_t>(IdN + 1 + CampN + 1 + Bytes);
		}
	}

	void Harvest(const u8* Buf, std::size_t N, u64 Base, std::vector<NameHit>& Names, std::vector<LocHit>& Locs, std::vector<IdHit>& TextRows)
	{
		HarvestRows(Buf, N, TextRows);
		const u8* End = Buf + N;
		for (std::size_t i = 0; i + 6 < N;)
		{
			int Bytes = 0;
			auto Zh = CjkRun(Buf + i, End, Bytes, 2, 8);
			if (!Zh)
			{
				++i;
				continue;
			}
			char32_t More = 0;
			const bool CutOff = Utf8Cjk(Buf + i + static_cast<std::size_t>(Bytes), End, More) != 0;
			const bool Bounded = (i == 0 || Buf[i - 1] == 0 || Buf[i - 1] == '\t' || Buf[i - 1] == ',' || Buf[i - 1] == '"');
			if (!CutOff && Bounded && Zh->size() <= 18)
			{
				int Chars = 0;
				for (std::size_t k = 0; k < Zh->size();)
				{
					char32_t Cp = 0;
					const int C = Utf8Cjk(reinterpret_cast<const u8*>(Zh->data()) + k, reinterpret_cast<const u8*>(Zh->data()) + Zh->size(), Cp);
					if (C == 0)
						break;
					k += static_cast<std::size_t>(C);
					++Chars;
				}
				if (Chars >= 2 && Chars <= 6)
					Names.push_back(NameHit{Base + i, Chars, *Zh});
			}
			if (i >= 8)
			{
				std::size_t KeyEnd = i;
				while (KeyEnd > 0 && (Buf[KeyEnd - 1] == 0 || Buf[KeyEnd - 1] == '"' || Buf[KeyEnd - 1] == '*' || Buf[KeyEnd - 1] == '#'))
					--KeyEnd;
				std::size_t Gap = i - KeyEnd;
				if (Gap <= 24 && KeyEnd > 0)
				{
					std::size_t KeyStart = KeyEnd;
					while (KeyStart > 0 && KeyChar(Buf[KeyStart - 1]) && KeyEnd - KeyStart < 80)
						--KeyStart;
					if (KeyEnd - KeyStart >= 8)
					{
						std::string_view Key(reinterpret_cast<const char*>(Buf + KeyStart), KeyEnd - KeyStart);
						if (InterestingKey(Key) && Zh->size() >= 6 && Zh->size() <= 18)
							Locs.push_back(LocHit{std::string(Key), *Zh});
					}
				}
			}
			i += static_cast<std::size_t>(Bytes == 0 ? 1 : Bytes);
		}

		// UTF-16LE short names. Record the address of the first wchar.
		for (std::size_t i = 0; i + 8 < N; i += 2)
		{
			auto At = [&](std::size_t O) -> char32_t {
				if (O + 1 >= N)
					return 0;
				return char32_t(Buf[O] | (Buf[O + 1] << 8));
			};
			if (!IsCjk(At(i)))
				continue;
			if (i >= 2 && At(i - 2) != 0)
				continue;
			std::string Zh;
			int Chars = 0;
			std::size_t O = i;
			while (O + 1 < N && Chars < 8)
			{
				const char32_t Cp = At(O);
				if (!IsCjk(Cp))
					break;
				u8 Enc[4];
				Enc[0] = u8(0xE0 | (Cp >> 12));
				Enc[1] = u8(0x80 | ((Cp >> 6) & 0x3F));
				Enc[2] = u8(0x80 | (Cp & 0x3F));
				Zh.append(reinterpret_cast<char*>(Enc), 3);
				O += 2;
				++Chars;
			}
			if (Chars < 2 || Chars > 6 || At(O) != 0)
				continue;
			Names.push_back(NameHit{Base + i, Chars, Zh});
			i = O;
		}
	}

	[[nodiscard]] std::vector<Map> ReadMaps(int Pid)
	{
		char Path[64];
		std::snprintf(Path, sizeof(Path), "/proc/%d/maps", Pid);
		FILE* F = std::fopen(Path, "r");
		std::vector<Map> Out;
		if (!F)
			return Out;
		char Line[512];
		while (std::fgets(Line, sizeof(Line), F))
		{
			unsigned long long A = 0, B = 0;
			char Perm[8] = {};
			if (std::sscanf(Line, "%llx-%llx %7s", &A, &B, Perm) != 3)
				continue;
			if (Perm[0] != 'r' || B <= A)
				continue;
			const u64 Size = B - A;
			if (Size < 0x1000 || Size > 96ull * 1024 * 1024)
				continue;
			const bool File = std::strchr(Line, '/') != nullptr;
			if (File && (std::strstr(Line, "libart") || std::strstr(Line, ".dex") || std::strstr(Line, ".apk") || std::strstr(Line, "oat") || std::strstr(Line, "libc.so")))
				continue;
			if (File && Size > 32ull * 1024 * 1024)
				continue;
			Out.push_back(Map{A, B, File});
		}
		std::fclose(F);
		return Out;
	}

	[[nodiscard]] int FindPid()
	{
		int Best = 0;
		for (int Pid = 1; Pid < 65536; ++Pid)
		{
			char Path[64];
			std::snprintf(Path, sizeof(Path), "/proc/%d/cmdline", Pid);
			FILE* F = std::fopen(Path, "r");
			if (!F)
				continue;
			char Cmd[128] = {};
			const std::size_t N = std::fread(Cmd, 1, sizeof(Cmd) - 1, F);
			std::fclose(F);
			if (N == 0)
				continue;
			if (std::strcmp(Cmd, "com.tencent.letsgo") != 0)
				continue;
			std::snprintf(Path, sizeof(Path), "/proc/%d/maps", Pid);
			FILE* M = std::fopen(Path, "r");
			if (!M)
				continue;
			char Line[256];
			bool HasUe = false;
			while (std::fgets(Line, sizeof(Line), M))
			{
				if (std::strstr(Line, "libUE4.so"))
				{
					HasUe = true;
					break;
				}
			}
			std::fclose(M);
			if (HasUe)
				return Pid;
			Best = Pid;
		}
		return Best;
	}

	struct Vote
	{
		int Camp = 0;
		int Count = 0;
	};

	void ConsiderId(std::unordered_map<int, std::unordered_map<std::string, Vote>>& Votes, int Id, int Camp, const std::string& Zh)
	{
		if (Id < 1 || Id > 1300 || Zh.empty())
			return;
		Vote& V = Votes[Id][Zh];
		++V.Count;
		if (Camp >= 1 && Camp <= 3)
			V.Camp = Camp;
	}

	void ScanFStrings(const Mem& M, const std::vector<Map>& Maps, const std::vector<NameHit>& Names,
		std::unordered_map<int, std::unordered_map<std::string, Vote>>& Votes)
	{
		std::unordered_map<u64, const NameHit*> ByAddr;
		ByAddr.reserve(Names.size());
		for (const NameHit& H : Names)
			ByAddr.emplace(H.Addr, &H);

		constexpr std::size_t Chunk = 4 * 1024 * 1024;
		std::vector<u8> Buf(Chunk + 16);
		for (const Map& R : Maps)
		{
			for (u64 Addr = R.Start; Addr < R.End;)
			{
				const std::size_t N = static_cast<std::size_t>(std::min<u64>(Chunk, R.End - Addr));
				if (!M.Read(Addr, Buf.data(), N))
				{
					Addr += N;
					continue;
				}
				for (std::size_t i = 0; i + 16 <= N; i += 8)
				{
					u64 Ptr = 0;
					std::memcpy(&Ptr, Buf.data() + i, 8);
					const auto It = ByAddr.find(Ptr);
					if (It == ByAddr.end())
						continue;
					i32 Num = 0, Max = 0;
					std::memcpy(&Num, Buf.data() + i + 8, 4);
					std::memcpy(&Max, Buf.data() + i + 12, 4);
					const int Ch = It->second->Chars;
					const bool NumOk = Num == Ch || Num == Ch + 1;
					if (!NumOk || Max < Num || Max > 24)
						continue;
					i32 Id = 0, Camp = 0;
					if (i >= 8)
					{
						std::memcpy(&Camp, Buf.data() + i - 4, 4);
						std::memcpy(&Id, Buf.data() + i - 8, 4);
					}
					if (Id >= 1 && Id <= 1300)
						ConsiderId(Votes, Id, (Camp >= 1 && Camp <= 3) ? Camp : 0, It->second->Zh);
					else if (i >= 8 && Camp >= 1 && Camp <= 1300 && Id >= 1 && Id <= 3)
						ConsiderId(Votes, Camp, Id, It->second->Zh);
				}
				Addr += N;
			}
		}
	}
}

int main(int Argc, char** Argv)
{
	const bool Self = Argc > 1 && std::strcmp(Argv[1], "--selftest") == 0;
	if (Self)
	{
		u8 Blob[256] = {};
		const char* Key = "UI_InLevel_NR3E3_WarriorTip";
		std::memcpy(Blob + 16, Key, std::strlen(Key));
		const u8 Yong[] = {0xE5, 0x8B, 0x87, 0xE5, 0xA3, 0xAB};
		std::memcpy(Blob + 16 + std::strlen(Key) + 3, Yong, sizeof(Yong));
		std::vector<NameHit> Names;
		std::vector<LocHit> Locs;
		std::vector<IdHit> TextRows;
		Harvest(Blob, sizeof(Blob), 0x1000, Names, Locs, TextRows);
		if (Locs.size() != 1 || Locs[0].Zh != "勇士" || Locs[0].Key != Key)
		{
			std::fprintf(stderr, "selftest loc failed %zu\n", Locs.size());
			return 1;
		}
		const u8 Row[] = {'1', '2', '\t', '3', '\t', 0xE8, 0xB5, 0x8C, 0xE5, 0xBE, 0x92};
		std::vector<IdHit> Rows;
		HarvestRows(Row, sizeof(Row), Rows);
		if (Rows.size() != 1 || Rows[0].Id != 12 || Rows[0].Camp != 3 || Rows[0].Zh != "赌徒")
		{
			std::fprintf(stderr, "selftest row failed %zu\n", Rows.size());
			return 1;
		}
		u8 Raw[128] = {};
		Raw[0] = 0x66;
		Raw[1] = 0x8B;
		Raw[2] = 0x7F;
		Raw[3] = 0x95;
		const u64 Str = 0x2000;
		i32 Sid = 5, Camp = 2, Num = 3, Max = 4;
		std::memcpy(Raw + 0x40, &Sid, 4);
		std::memcpy(Raw + 0x44, &Camp, 4);
		std::memcpy(Raw + 0x48, &Str, 8);
		std::memcpy(Raw + 0x50, &Num, 4);
		std::memcpy(Raw + 0x54, &Max, 4);
		u64 Ptr = 0;
		std::memcpy(&Ptr, Raw + 0x48, 8);
		i32 GotNum = 0, GotMax = 0, GotId = 0, GotCamp = 0;
		std::memcpy(&GotNum, Raw + 0x50, 4);
		std::memcpy(&GotMax, Raw + 0x54, 4);
		std::memcpy(&GotId, Raw + 0x40, 4);
		std::memcpy(&GotCamp, Raw + 0x44, 4);
		if (Ptr != Str || GotNum != 3 || GotMax != 4 || GotId != 5 || GotCamp != 2)
		{
			std::fprintf(stderr, "selftest fstring layout failed\n");
			return 1;
		}
		std::puts("selftest ok");
		return 0;
	}

	const int Pid = FindPid();
	if (Pid <= 0)
	{
		std::fprintf(stderr, "游戏没在跑。先打开元梦之星，进一次狼人杀，再执行 su -c /data/local/tmp/nr3e-voc\n");
		return 1;
	}
	char MemPath[64];
	std::snprintf(MemPath, sizeof(MemPath), "/proc/%d/mem", Pid);
	const int Fd = open(MemPath, O_RDONLY);
	if (Fd < 0)
	{
		std::fprintf(stderr, "读不到 /proc/%d/mem。用 root 跑: su -c /data/local/tmp/nr3e-voc\n", Pid);
		return 1;
	}
	Mem M(Fd);
	const std::vector<Map> Maps = ReadMaps(Pid);
	std::fprintf(stderr, "pid %d  maps %zu\n", Pid, Maps.size());

	std::vector<NameHit> Names;
	std::vector<LocHit> Locs;
	std::vector<IdHit> TextRows;
	constexpr std::size_t Chunk = 4 * 1024 * 1024;
	std::vector<u8> Buf(Chunk);
	for (const Map& R : Maps)
	{
		for (u64 Addr = R.Start; Addr < R.End;)
		{
			const std::size_t N = static_cast<std::size_t>(std::min<u64>(Chunk, R.End - Addr));
			if (!M.Read(Addr, Buf.data(), N))
			{
				Addr += N;
				continue;
			}
			Harvest(Buf.data(), N, Addr, Names, Locs, TextRows);
			Addr += N;
		}
	}
	std::fprintf(stderr, "short names %zu  loc %zu\n", Names.size(), Locs.size());

	std::unordered_map<int, std::unordered_map<std::string, Vote>> Votes;
	for (const IdHit& R : TextRows)
		ConsiderId(Votes, R.Id, R.Camp, R.Zh);
	ScanFStrings(M, Maps, Names, Votes);

	std::vector<IdHit> Rows;
	for (const auto& [Id, NamesForId] : Votes)
	{
		const std::string* BestZh = nullptr;
		Vote Best{};
		for (const auto& [Zh, V] : NamesForId)
		{
			if (!BestZh || V.Count > Best.Count)
			{
				BestZh = &Zh;
				Best = V;
			}
		}
		if (!BestZh)
			continue;
		if (Best.Count < 2 && Best.Camp == 0)
			continue;
		Rows.push_back(IdHit{Id, Best.Camp, *BestZh});
	}
	std::sort(Rows.begin(), Rows.end(), [](const IdHit& A, const IdHit& B) { return A.Id < B.Id; });

	std::unordered_map<std::string, std::string> LocBest;
	for (const LocHit& L : Locs)
	{
		auto It = LocBest.find(L.Key);
		if (It == LocBest.end() || L.Zh.size() < It->second.size())
			LocBest.insert_or_assign(L.Key, L.Zh);
	}

	const char* OutPath = "/data/local/tmp/nr3e-side-names.tsv";
	FILE* Out = std::fopen(OutPath, "w");
	if (!Out)
		Out = std::fopen("nr3e-side-names.tsv", "w");
	if (!Out)
	{
		std::perror("fopen");
		return 1;
	}
	std::fprintf(Out, "id\tcamp\tzh\n");
	for (const IdHit& R : Rows)
		std::fprintf(Out, "%d\t%d\t%s\n", R.Id, R.Camp, R.Zh.c_str());
	std::fclose(Out);

	const char* LocPath = "/data/local/tmp/nr3e-side-loc.tsv";
	FILE* Loc = std::fopen(LocPath, "w");
	if (!Loc)
		Loc = std::fopen("nr3e-side-loc.tsv", "w");
	if (Loc)
	{
		std::fprintf(Loc, "key\tzh\n");
		std::vector<std::pair<std::string, std::string>> Sorted(LocBest.begin(), LocBest.end());
		std::sort(Sorted.begin(), Sorted.end());
		for (const auto& [K, V] : Sorted)
			std::fprintf(Loc, "%s\t%s\n", K.c_str(), V.c_str());
		std::fclose(Loc);
	}

	if (Rows.empty())
	{
		std::fprintf(stderr,
			"没有扫到 SideID 旁边的中文名。进一次狼人杀（大厅或对局都行），等身份界面加载完再跑一次。\n"
			"本地化键已经写到 %s （%zu 条）。\n",
			LocPath, LocBest.size());
		return Rows.empty() && LocBest.empty() ? 2 : 0;
	}
	std::printf("id\tcamp\tzh\n");
	for (const IdHit& R : Rows)
		std::printf("%d\t%d\t%s\n", R.Id, R.Camp, R.Zh.c_str());
	std::fprintf(stderr, "写了 %zu 条到 %s\n", Rows.size(), OutPath);
	return 0;
}
