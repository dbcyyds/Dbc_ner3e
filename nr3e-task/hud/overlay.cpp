#include "client.hpp"
#include "display.hpp"
#include "egl_ctx.hpp"
#include "font.hpp"
#include "role_name.hpp"
#include "touch.hpp"

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include <GLES3/gl3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>

namespace overlay
{
	namespace
	{
		struct Ui
		{
			bool Open = true;
			bool Running = true;
			ImVec2 Ball{0.f, 0.f};
			bool Drag = false;
			bool Enabled = false;
			int Urgent = 0;
			char Status[192]{"进对局后点自动完成，或直接点功能。"};
			float ToastT = 0.f;
			char Toast[160]{};
			bool Dock = true;
			bool BallArm = false;
			ImVec2 Grab{0.f, 0.f};
			float Scale = 1.2f;
			bool VoteOpen = false;
			int VoteN = 8;
			int VoteSelf = 0;
			int VoteSeats[16]{};
			int VoteSeatN = 0;
			struct IdRow
			{
				int Seat = 0;
				int Side = 0;
				int Camp = 0;
			};
			IdRow IdRows[16]{};
			int IdN = 0;
			int IdSelf = 0;
			char IdMatch[16]{};
			float IdPoll = 10.f;
			bool IdArm = false;
			char IdHint[80]{};
		};

		constexpr float kBallR = 36.f;

		struct QItem
		{
			int Cmd = 0;
			int Extra = 0;
			char Fb[32]{};
		};

		std::mutex g_QMu;
		std::deque<QItem> g_Q;
		client::Reply g_Rep;
		std::atomic<int> g_Have{0};
		std::atomic<int> g_Busy{0};
		std::atomic<int> g_Stop{0};

		void Toast(Ui& U, const char* M);
		void Apply(Ui& U, const client::Reply& R, const char* Fb);

		void Worker()
		{
			while (g_Stop.load() == 0)
			{
				QItem It{};
				bool Got = false;
				{
					std::lock_guard<std::mutex> L(g_QMu);
					if (!g_Q.empty())
					{
						It = g_Q.front();
						g_Q.pop_front();
						Got = true;
						g_Busy.store(1);
					}
				}
				if (!Got)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(12));
					continue;
				}
				const auto R = client::Call(It.Cmd, It.Extra);
				{
					std::lock_guard<std::mutex> L(g_QMu);
					g_Rep = R;
					g_Have.store(1);
					g_Busy.store(0);
				}
			}
		}

		void Send(Ui& U, int Cmd, int Extra, const char* Fb)
		{
			QItem It;
			It.Cmd = Cmd;
			It.Extra = Extra;
			std::snprintf(It.Fb, sizeof(It.Fb), "%s", Fb ? Fb : "");
			{
				std::lock_guard<std::mutex> L(g_QMu);
				if (g_Q.size() >= 12)
				{
					Toast(U, "点太快了，等一条完成");
					return;
				}
				g_Q.push_back(It);
			}
			char Line[96];
			std::snprintf(Line, sizeof(Line), "已发送 %s", Fb ? Fb : "");
			Toast(U, Line);
		}

		void SendQuiet(int Cmd, int Extra)
		{
			std::lock_guard<std::mutex> L(g_QMu);
			for (const QItem& Q : g_Q)
			{
				if (Q.Cmd == Cmd)
					return;
			}
			if (g_Q.size() >= 12)
				return;
			QItem It;
			It.Cmd = Cmd;
			It.Extra = Extra;
			g_Q.push_back(It);
		}

		int ReadNum(const char*& P)
		{
			int V = 0;
			bool Any = false;
			while (*P >= '0' && *P <= '9')
			{
				Any = true;
				V = V * 10 + (*P - '0');
				++P;
			}
			return Any ? V : -1;
		}

		void ParseIdentity(Ui& U, const char* P)
		{
			U.IdN = 0;
			U.IdSelf = 0;
			U.IdMatch[0] = 0;
			if (!P || P[0] != 'I' || P[1] != '|')
				return;
			P += 2;
			const int Self = ReadNum(P);
			U.IdSelf = Self > 0 ? Self : 0;
			if (*P == '|')
				++P;
			bool HasColon = false;
			for (const char* Q = P; *Q && *Q != '|'; ++Q)
			{
				if (*Q == ':')
					HasColon = true;
			}
			if (!HasColon)
			{
				int H = 0;
				while (*P && *P != '|' && H < 15)
					U.IdMatch[H++] = *P++;
				U.IdMatch[H] = 0;
				if (*P == '|')
					++P;
			}
			while (*P && U.IdN < 16)
			{
				const int Seat = ReadNum(P);
				if (*P != ':')
					break;
				++P;
				const int Side = ReadNum(P);
				if (*P != ':')
					break;
				++P;
				const int Camp = ReadNum(P);
				if (Seat < 0)
					break;
				Ui::IdRow& Row = U.IdRows[U.IdN++];
				Row.Seat = Seat;
				Row.Side = Side > 0 ? Side : 0;
				Row.Camp = Camp > 0 ? Camp : 0;
				if (Row.Camp == 0)
					Row.Camp = role::CampOf(Row.Side);
				if (*P == '|')
					++P;
				else
					break;
			}
		}

		const char* RoleLabel(int Side, char* Fallback, int Cap)
		{
			const char* Zh = role::Name(Side);
			if (Zh)
				return Zh;
			std::snprintf(Fallback, static_cast<size_t>(Cap), "%d", Side);
			return Fallback;
		}

		ImVec4 CampColor(int Camp)
		{
			switch (Camp)
			{
			case 1: return ImVec4(0.72f, 0.28f, 0.24f, 1.f);
			case 2: return ImVec4(0.18f, 0.40f, 0.56f, 1.f);
			case 3: return ImVec4(0.55f, 0.40f, 0.14f, 1.f);
			default: return ImVec4(0.45f, 0.47f, 0.50f, 0.90f);
			}
		}

		const char* CampLabel(int Camp)
		{
			switch (Camp)
			{
			case 1: return "狼人";
			case 2: return "平民";
			case 3: return "中立";
			default: return "未知";
			}
		}

		constexpr const char* kMatchPath = "/data/local/tmp/nr3e-match.txt";
		constexpr const char* kHistPath = "/data/local/tmp/nr3e-id-history.txt";

		struct IdDisk
		{
			char Hash[16]{};
			bool Archived = false;
			int Known = 0;
			char Body[4096]{};
		};

		IdDisk g_Disk;

		void WriteText(const char* Path, const char* Body, bool Append)
		{
			FILE* F = std::fopen(Path, Append ? "a" : "w");
			if (!F)
				return;
			std::fputs(Body, F);
			std::fflush(F);
			std::fclose(F);
		}

		void PublishMatch(const Ui& U)
		{
			if (U.IdN <= 0 || U.IdMatch[0] == 0 || std::strcmp(U.IdMatch, "00000000") == 0)
				return;
			if (g_Disk.Hash[0] && std::strcmp(g_Disk.Hash, U.IdMatch) != 0)
			{
				if (!g_Disk.Archived && g_Disk.Known > 0 && g_Disk.Body[0])
					WriteText(kHistPath, g_Disk.Body, true);
				g_Disk.Archived = false;
				g_Disk.Known = 0;
				g_Disk.Body[0] = 0;
			}
			std::snprintf(g_Disk.Hash, sizeof(g_Disk.Hash), "%s", U.IdMatch);
			char Body[4096];
			int Used = std::snprintf(Body, sizeof(Body), "对局 %s\n座\t编号\t身份\t阵营\n", U.IdMatch);
			int Known = 0;
			for (int i = 0; i < U.IdN && Used > 0 && Used < static_cast<int>(sizeof(Body)) - 80; ++i)
			{
				const Ui::IdRow& Row = U.IdRows[i];
				char Num[16];
				const char* Zh = RoleLabel(Row.Side, Num, sizeof(Num));
				const int Add = std::snprintf(Body + Used, sizeof(Body) - static_cast<size_t>(Used),
					"%d\t%d\t%s\t%s\n", Row.Seat, Row.Side, Zh, CampLabel(Row.Camp));
				if (Add < 0)
					break;
				Used += Add;
				if (Row.Side > 0)
					++Known;
			}
			WriteText(kMatchPath, Body, false);
			if (Known >= g_Disk.Known)
			{
				std::snprintf(g_Disk.Body, sizeof(g_Disk.Body), "%s", Body);
				g_Disk.Known = Known;
			}
			if (!g_Disk.Archived && Known > 0 && Known == U.IdN)
			{
				char Stamp[64];
				const std::time_t Now = std::time(nullptr);
				std::tm Tm{};
				localtime_r(&Now, &Tm);
				std::strftime(Stamp, sizeof(Stamp), "%Y-%m-%d %H:%M:%S", &Tm);
				char Block[4600];
				std::snprintf(Block, sizeof(Block), "===== %s =====\n%s\n", Stamp, Body);
				WriteText(kHistPath, Block, true);
				g_Disk.Archived = true;
			}
		}

		void DrainReply(Ui& U)
		{
			client::Reply R;
			bool Got = false;
			{
				std::lock_guard<std::mutex> L(g_QMu);
				if (g_Have.load())
				{
					R = g_Rep;
					g_Have.store(0);
					Got = true;
				}
			}
			if (Got)
			{
				if (!R.Message.empty() && R.Message[0] == 'I' && R.Message[1] == '|')
				{
					U.Enabled = R.Enabled != 0;
					ParseIdentity(U, R.Message.c_str());
					PublishMatch(U);
					if (!R.Error.empty())
						std::snprintf(U.IdHint, sizeof(U.IdHint), "%s", R.Error.c_str());
					else
						U.IdHint[0] = 0;
					if (U.IdArm)
					{
						char Line[64];
						int Known = 0;
						for (int i = 0; i < U.IdN; ++i)
						{
							if (U.IdRows[i].Side > 0)
								++Known;
						}
						if (!R.Error.empty() && U.IdN == 0)
							std::snprintf(Line, sizeof(Line), "%s", R.Error.c_str());
						else
							std::snprintf(Line, sizeof(Line), "身份 %d/%d", Known, U.IdN);
						Toast(U, Line);
						U.IdArm = false;
					}
				}
				else if (!R.Message.empty() && R.Message[0] == 'V' && R.Message[1] == '|')
				{
					int N = 0, Self = 0;
					const char* P = R.Message.c_str();
					if (std::sscanf(P, "V|%d|%d|", &N, &Self) >= 1)
					{
						U.VoteSelf = Self;
						U.VoteN = N;
						U.VoteSeatN = 0;
						const char* Bar = std::strrchr(P, '|');
						if (Bar && Bar[1])
						{
							int V = 0;
							for (const char* S = Bar + 1; *S && U.VoteSeatN < 16; ++S)
							{
								if (*S >= '0' && *S <= '9')
								{
									V = V * 10 + (*S - '0');
								}
								else if (*S == ',')
								{
									if (V > 0)
										U.VoteSeats[U.VoteSeatN++] = V;
									V = 0;
								}
							}
							if (V > 0 && U.VoteSeatN < 16)
								U.VoteSeats[U.VoteSeatN++] = V;
						}
					}
				}
				else if (R.Touched >= 256)
				{
					U.VoteSelf = (R.Touched >> 8) & 0xFF;
					int N = R.Touched & 0xFF;
					if (N >= 2 && N <= 15)
						U.VoteN = N;
				}
				if (R.Message.empty() || R.Message[0] != 'I' || R.Message.size() < 2 || R.Message[1] != '|')
					Apply(U, R, "");
			}
		}

		void Toast(Ui& U, const char* M)
		{
			std::snprintf(U.Toast, sizeof(U.Toast), "%s", M);
			U.ToastT = 2.2f;
		}

		void Apply(Ui& U, const client::Reply& R, const char* Fb)
		{
			U.Enabled = R.Enabled != 0;
			if (!R.Error.empty() && !R.Message.empty())
				std::snprintf(U.Status, sizeof(U.Status), "%s | %s", R.Message.c_str(),
					R.Error.c_str());
			else if (!R.Error.empty())
				std::snprintf(U.Status, sizeof(U.Status), "%s", R.Error.c_str());
			else
				std::snprintf(U.Status, sizeof(U.Status), "%s",
					R.Message.empty() ? Fb : R.Message.c_str());
			Toast(U, U.Status);
		}

		void Style()
		{
			ImGui::StyleColorsLight();
			ImGuiStyle& S = ImGui::GetStyle();
			S.WindowRounding = 16.f;
			S.FrameRounding = 12.f;
			S.GrabRounding = 8.f;
			S.FramePadding = ImVec2(14.f, 10.f);
			S.ItemSpacing = ImVec2(10.f, 10.f);
			S.WindowPadding = ImVec2(16.f, 14.f);
			S.WindowBorderSize = 1.15f;
			S.ScrollbarSize = 10.f;
			S.GrabMinSize = 14.f;
			ImVec4* C = S.Colors;
			C[ImGuiCol_WindowBg] = ImVec4(0.98f, 0.99f, 1.f, 0.38f);
			C[ImGuiCol_Border] = ImVec4(1.f, 1.f, 1.f, 0.70f);
			C[ImGuiCol_Text] = ImVec4(0.22f, 0.24f, 0.28f, 0.92f);
			C[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.54f, 0.58f, 0.75f);
			C[ImGuiCol_TitleBg] = ImVec4(1.f, 1.f, 1.f, 0.0f);
			C[ImGuiCol_TitleBgActive] = ImVec4(1.f, 1.f, 1.f, 0.0f);
			C[ImGuiCol_Button] = ImVec4(1.f, 1.f, 1.f, 0.35f);
			C[ImGuiCol_ButtonHovered] = ImVec4(1.f, 1.f, 1.f, 0.55f);
			C[ImGuiCol_ButtonActive] = ImVec4(0.95f, 0.96f, 0.98f, 0.85f);
			C[ImGuiCol_Separator] = ImVec4(0.80f, 0.82f, 0.86f, 0.45f);
			C[ImGuiCol_ResizeGrip] = ImVec4(0.70f, 0.74f, 0.80f, 0.45f);
			C[ImGuiCol_ResizeGripHovered] = ImVec4(0.45f, 0.55f, 0.70f, 0.70f);
			C[ImGuiCol_ResizeGripActive] = ImVec4(0.30f, 0.45f, 0.70f, 0.85f);
			C[ImGuiCol_ScrollbarBg] = ImVec4(1.f, 1.f, 1.f, 0.15f);
			C[ImGuiCol_ScrollbarGrab] = ImVec4(0.75f, 0.78f, 0.82f, 0.70f);
		}

		bool GlassBtn(const char* Id, const char* Lab, ImVec2 Sz, bool On = false)
		{
			const ImVec2 P0 = ImGui::GetCursorScreenPos();
			ImGui::PushID(Id);
			const bool Hit = ImGui::InvisibleButton("##g", Sz);
			const bool Hov = ImGui::IsItemHovered();
			const bool Act = ImGui::IsItemActive();
			ImGui::PopID();
			const ImVec2 P1(P0.x + Sz.x, P0.y + Sz.y);
			ImDrawList* Dl = ImGui::GetWindowDrawList();
			const float R = 12.f;
			Dl->AddRectFilled(ImVec2(P0.x + 1.f, P0.y + 4.f), ImVec2(P1.x + 1.f, P1.y + 4.f),
				IM_COL32(80, 90, 110, 22), R);
			ImU32 Fill = On ? IM_COL32(230, 248, 255, 150)
				: Act ? IM_COL32(255, 255, 255, 175)
				: Hov ? IM_COL32(255, 255, 255, 145)
					  : IM_COL32(255, 255, 255, 95);
			Dl->AddRectFilled(P0, P1, Fill, R);
			Dl->AddRectFilledMultiColor(P0, ImVec2(P1.x, P0.y + Sz.y * 0.55f),
				IM_COL32(255, 255, 255, 150), IM_COL32(255, 255, 255, 150),
				IM_COL32(255, 255, 255, 8), IM_COL32(255, 255, 255, 8));
			Dl->AddRect(P0, P1, IM_COL32(255, 255, 255, 210), R, 0, 1.35f);
			Dl->AddLine(ImVec2(P0.x + 12.f, P0.y + 1.4f), ImVec2(P1.x - 12.f, P0.y + 1.4f),
				IM_COL32(255, 255, 255, 140), 1.2f);
			const ImVec2 Ts = ImGui::CalcTextSize(Lab);
			Dl->AddText(ImVec2(P0.x + (Sz.x - Ts.x) * 0.5f, P0.y + (Sz.y - Ts.y) * 0.5f - 1.f),
				IM_COL32(48, 52, 60, 230), Lab);
			return Hit;
		}

		void AddHeart(ImDrawList* Dl, ImVec2 C, float R, ImU32 Fill)
		{
			const ImVec2 L(C.x - R * 0.34f, C.y - R * 0.22f);
			const ImVec2 Ri(C.x + R * 0.34f, C.y - R * 0.22f);
			Dl->AddCircleFilled(L, R * 0.50f, Fill, 24);
			Dl->AddCircleFilled(Ri, R * 0.50f, Fill, 24);
			Dl->AddTriangleFilled(ImVec2(C.x, C.y + R * 0.82f),
				ImVec2(C.x - R * 0.78f, C.y - R * 0.02f),
				ImVec2(C.x + R * 0.78f, C.y - R * 0.02f), Fill);
		}

		void DrawBall(Ui& U, float W, float H, const touch::State& T)
		{
			if (U.VoteOpen)
				return;
			U.Ball.x = std::clamp(U.Ball.x, kBallR + 8.f, W - kBallR - 8.f);
			U.Ball.y = std::clamp(U.Ball.y, kBallR + 8.f, H - kBallR - 8.f);
			ImGui::SetNextWindowPos(ImVec2(U.Ball.x - kBallR, U.Ball.y - kBallR), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(kBallR * 2.f, kBallR * 2.f), ImGuiCond_Always);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
			ImGui::Begin("##ball", nullptr,
				ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar
					| ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground
					| ImGuiWindowFlags_NoInputs);
			ImDrawList* Dl = ImGui::GetForegroundDrawList();
			AddHeart(Dl, U.Ball, kBallR - 1.f, IM_COL32(236, 92, 108, 230));
			ImGui::End();
			ImGui::PopStyleColor();
			ImGui::PopStyleVar(2);

			const float Hit = kBallR + 14.f;
			const float Dx = T.X - U.Ball.x;
			const float Dy = T.Y - U.Ball.y;
			if (T.Pressed && Dx * Dx + Dy * Dy <= Hit * Hit)
			{
				U.BallArm = true;
				U.Drag = false;
				U.Grab = ImVec2(T.X, T.Y);
			}
			if (U.BallArm && T.Down)
			{
				if (std::fabs(T.X - U.Grab.x) + std::fabs(T.Y - U.Grab.y) > 10.f)
					U.Drag = true;
				if (U.Drag)
				{
					U.Ball.x = T.X;
					U.Ball.y = T.Y;
				}
			}
			if (T.Released && U.BallArm)
			{
				if (!U.Drag)
				{
					U.Open = !U.Open;
					if (U.Open)
						U.IdPoll = 10.f;
				}
				U.BallArm = false;
				U.Drag = false;
			}
		}

		void DrawIdentity(Ui& U, float S)
		{
			const float Av = ImGui::GetContentRegionAvail().x;
			ImGui::Dummy(ImVec2(0, 4.f * S));
			ImGui::TextDisabled("身份");
			ImGui::SameLine(0.f, 12.f);
			if (GlassBtn("id", "读身份", ImVec2(108.f * S, 40.f * S)))
			{
				U.IdArm = true;
				U.IdPoll = 0.f;
				Send(U, 8, 0, "身份");
			}
			int Known = 0;
			for (int i = 0; i < U.IdN; ++i)
			{
				if (U.IdRows[i].Side > 0)
					++Known;
			}
			if (U.IdN > 0)
			{
				ImGui::SameLine(0.f, 10.f);
				ImGui::TextDisabled("%d/%d", Known, U.IdN);
			}

			ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.f, 1.f, 1.f, 0.22f));
			ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.55f));
			ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f * S, 8.f * S));
			ImGui::BeginChild("##ids", ImVec2(Av, 196.f * S), ImGuiChildFlags_Borders, 0);
			if (U.IdHint[0])
				ImGui::TextDisabled("%s", U.IdHint);
			if (U.IdN <= 0)
			{
				if (!U.IdHint[0])
					ImGui::TextDisabled("进对局后显示每人的身份和阵营");
			}
			else
			{
				ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, ImVec4(1.f, 1.f, 1.f, 0.28f));
				ImGui::PushStyleColor(ImGuiCol_TableRowBg, ImVec4(1.f, 1.f, 1.f, 0.05f));
				ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(1.f, 1.f, 1.f, 0.14f));
				ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImVec4(1.f, 1.f, 1.f, 0.28f));
				ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, ImVec4(1.f, 1.f, 1.f, 0.40f));
				const ImGuiTableFlags Flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp
					| ImGuiTableFlags_PadOuterX;
				if (ImGui::BeginTable("idtab", 3, Flags))
				{
					ImGui::TableSetupColumn("号", ImGuiTableColumnFlags_WidthFixed, 52.f * S);
					ImGui::TableSetupColumn("身份", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("阵营", ImGuiTableColumnFlags_WidthFixed, 72.f * S);
					ImGui::TableHeadersRow();
					for (int i = 0; i < U.IdN; ++i)
					{
						const Ui::IdRow& Row = U.IdRows[i];
						const bool Mine = U.IdSelf > 0 && Row.Seat == U.IdSelf;
						ImGui::TableNextRow();
						if (Mine)
							ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
								IM_COL32(255, 255, 255, 70));
						ImGui::TableSetColumnIndex(0);
						if (Mine)
							ImGui::Text("我%d", Row.Seat);
						else if (Row.Seat > 0)
							ImGui::Text("%d", Row.Seat);
						else
							ImGui::TextUnformatted("—");
						ImGui::TableSetColumnIndex(1);
						char Num[16];
						ImGui::TextUnformatted(RoleLabel(Row.Side, Num, sizeof(Num)));
						ImGui::TableSetColumnIndex(2);
						ImGui::TextColored(CampColor(Row.Camp), "%s", CampLabel(Row.Camp));
					}
					ImGui::EndTable();
				}
				ImGui::PopStyleColor(5);
			}
			ImGui::EndChild();
			ImGui::PopStyleVar(2);
			ImGui::PopStyleColor(2);
		}

		void DrawPanel(Ui& U, float Dt, float W, float H)
		{
			U.IdPoll += Dt;
			int Known = 0;
			for (int i = 0; i < U.IdN; ++i)
			{
				if (U.IdRows[i].Side > 0)
					++Known;
			}
			const float Wait = (U.IdN > 0 && Known == U.IdN) ? 8.f : 2.2f;
			if (U.IdPoll >= Wait)
			{
				U.IdPoll = 0.f;
				SendQuiet(8, 0);
			}
			DrainReply(U);
			if (!U.Open)
				return;
			const float S = std::clamp(U.Scale, 0.85f, 1.7f);
			const bool Land = W > H;
			const float Pw = Land ? std::min(W - 32.f, 960.f * S) : std::min(W - 24.f, 500.f * S);
			const float Ph = Land ? std::min(H - 24.f, 480.f * S) : std::min(H - 24.f, 680.f * S);
			if (U.Dock)
			{
				const bool Right = U.Ball.x >= W * 0.5f;
				float Px = Right ? 16.f : std::min(W - Pw - 16.f, U.Ball.x + kBallR + 12.f);
				Px = std::clamp(Px, 12.f, W - Pw - 12.f);
				ImGui::SetNextWindowPos(ImVec2(Px, 16.f), ImGuiCond_Always);
				ImGui::SetNextWindowSize(ImVec2(Pw, Ph), ImGuiCond_Always);
				U.Dock = false;
			}
			ImGui::SetNextWindowSizeConstraints(ImVec2(300.f * S, 260.f * S), ImVec2(W - 12.f, H - 12.f));
			ImGui::Begin("##panel", &U.Open,
				ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
			DrainReply(U);

			const float DragW = ImGui::GetContentRegionAvail().x - 130.f * S;
			ImGui::InvisibleButton("##drag", ImVec2(std::max(80.f, DragW), 34.f * S));
			if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 1.f))
			{
				const ImVec2 P = ImGui::GetWindowPos();
				ImGui::SetWindowPos(ImVec2(P.x + ImGui::GetIO().MouseDelta.x,
					P.y + ImGui::GetIO().MouseDelta.y));
			}
			{
				const ImVec2 A = ImGui::GetItemRectMin();
				const ImVec2 B = ImGui::GetItemRectMax();
				ImDrawList* Dl = ImGui::GetWindowDrawList();
				const float Mid = (A.x + B.x) * 0.5f;
				Dl->AddRectFilled(ImVec2(Mid - 22.f, A.y + 12.f), ImVec2(Mid + 22.f, A.y + 16.f),
					IM_COL32(170, 176, 188, 170), 2.f);
			}
			ImGui::SameLine(0.f, 6.f);
			if (GlassBtn("zm", "A-", ImVec2(54.f * S, 38.f * S)))
			{
				U.Scale = std::clamp(U.Scale - 0.1f, 0.85f, 1.7f);
				U.Dock = true;
			}
			ImGui::SameLine(0.f, 6.f);
			if (GlassBtn("zp", "A+", ImVec2(54.f * S, 38.f * S)))
			{
				U.Scale = std::clamp(U.Scale + 0.1f, 0.85f, 1.7f);
				U.Dock = true;
			}

			const float Av = ImGui::GetContentRegionAvail().x;
			const float Bh = 58.f * S;
			{
				const ImVec2 P0 = ImGui::GetCursorScreenPos();
				const ImVec2 Sz(Av, 40.f * S);
				ImDrawList* Dl = ImGui::GetWindowDrawList();
				Dl->AddRectFilled(P0, ImVec2(P0.x + Sz.x, P0.y + Sz.y),
					IM_COL32(255, 255, 255, 70), 10.f);
				Dl->AddRect(P0, ImVec2(P0.x + Sz.x, P0.y + Sz.y),
					IM_COL32(255, 255, 255, 140), 10.f, 0, 1.1f);
				ImGui::Dummy(Sz);
				ImGui::SetCursorScreenPos(ImVec2(P0.x + 12.f * S, P0.y + 8.f * S));
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.32f, 0.34f, 0.38f, 0.88f));
				ImGui::PushTextWrapPos(P0.x + Sz.x - 12.f * S);
				ImGui::TextUnformatted(U.Status);
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
				ImGui::SetCursorScreenPos(ImVec2(P0.x, P0.y + Sz.y + 8.f * S));
			}

			DrawIdentity(U, S);

			ImGui::Dummy(ImVec2(0, 6.f * S));
			ImGui::TextDisabled("任务");
			{
				const float Bw = (Av - 12.f) * 0.5f;
				if (GlassBtn("n", "普通任务", ImVec2(Bw, Bh)))
					Send(U, 2, 0, "普通任务");
				ImGui::SameLine(0.f, 12.f);
				if (GlassBtn("e", "紧急任务", ImVec2(Bw, Bh)))
					Send(U, 3, 0, "紧急任务");
			}
			static const char* kUrg[] = {"默认", "灭火", "清雾", "关门", "开饭", "治病"};
			const float Chip = std::max(58.f * S, (Av - 92.f * S - 6.f * 6.f) / 6.f);
			for (int i = 0; i < 6; ++i)
			{
				if (i)
					ImGui::SameLine(0.f, 6.f);
				char Id[8];
				std::snprintf(Id, sizeof(Id), "u%d", i);
				if (GlassBtn(Id, kUrg[i], ImVec2(Chip, 42.f * S), U.Urgent == i))
					U.Urgent = i;
			}
			ImGui::SameLine(0.f, 8.f);
			if (GlassBtn("tr", "触发", ImVec2(84.f * S, 42.f * S)))
				Send(U, 4, U.Urgent, "触发紧急");

			ImGui::Dummy(ImVec2(0, 6.f * S));
			ImGui::TextDisabled("开会");
			if (GlassBtn("vote", "投票", ImVec2(Av, Bh)))
			{
				Send(U, 7, 0, "人数");
				U.VoteOpen = true;
			}

			ImGui::Dummy(ImVec2(0, 6.f * S));
			ImGui::TextDisabled("阶段");
			{
				const char* Labs[] = {"开会", "入夜", "好人胜", "猎人胜", "中立胜"};
				const int Ex[] = {1, 2, 4, 5, 6};
				const float Bw = (Av - 10.f * 4.f) / 5.f;
				for (int i = 0; i < 5; ++i)
				{
					if (i)
						ImGui::SameLine(0.f, 10.f);
					char Id[8];
					std::snprintf(Id, sizeof(Id), "p%d", i);
					if (GlassBtn(Id, Labs[i], ImVec2(Bw, 54.f * S)))
						Send(U, 5, Ex[i], Labs[i]);
				}
			}

			if (U.ToastT > 0.f)
			{
				U.ToastT -= Dt;
				ImGui::TextColored(ImVec4(0.75f, 0.35f, 0.28f, 1.f), "%s", U.Toast);
			}
			if (GlassBtn("rf", "刷新", ImVec2(96.f * S, 42.f * S)))
				Send(U, 0, 0, "刷新");
			ImGui::SameLine();
			if (GlassBtn("ex", "退出", ImVec2(96.f * S, 42.f * S)))
				U.Running = false;
			ImGui::End();
		}

		void DrawVoteWheel(Ui& U, float W, float H)
		{
			if (!U.VoteOpen)
				return;
			const float S = std::clamp(U.Scale, 0.85f, 1.7f);
			int Seats[16];
			int M = 0;
			if (U.VoteSeatN > 0)
			{
				for (int i = 0; i < U.VoteSeatN && M < 16; ++i)
					Seats[M++] = U.VoteSeats[i];
			}
			else
			{
				const int Total = (U.VoteN >= 2 && U.VoteN <= 15) ? U.VoteN : 0;
				for (int i = 1; i <= Total && M < 16; ++i)
				{
					if (U.VoteSelf > 0 && i == U.VoteSelf)
						continue;
					Seats[M++] = i;
				}
			}
			const float Box = 248.f * S;
			ImGui::SetNextWindowPos(ImVec2((W - Box) * 0.5f, (H - Box) * 0.5f), ImGuiCond_Always);
			ImGui::SetNextWindowSize(ImVec2(Box, Box), ImGuiCond_Always);
			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.f, 1.f, 1.f, 0.42f));
			ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 1.f, 1.f, 0.78f));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 22.f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.1f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
			ImGui::Begin("##votewheel", nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
					| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse
					| ImGuiWindowFlags_NoSavedSettings);
			ImDrawList* Dl = ImGui::GetWindowDrawList();
			const ImVec2 Wp = ImGui::GetWindowPos();
			const ImVec2 C(Wp.x + Box * 0.5f, Wp.y + Box * 0.5f);
			const float Ring = 78.f * S;
			const float Br = (M >= 12) ? 16.f * S : 18.f * S;
			Dl->AddCircle(C, Ring, IM_COL32(255, 255, 255, 120), 48, 1.15f);

			if (M <= 0)
			{
				ImGui::SetCursorScreenPos(ImVec2(C.x - 48.f * S, C.y - 10.f * S));
				ImGui::TextUnformatted("没有可投的人");
			}
			else
			{
				const float TwoPi = 6.2831853f;
				for (int i = 0; i < M; ++i)
				{
					const float Ang = -1.5707963f
						+ TwoPi * (static_cast<float>(i) / static_cast<float>(M));
					const ImVec2 P(C.x + std::cos(Ang) * Ring, C.y + std::sin(Ang) * Ring);
					char Id[12], Lab[8];
					std::snprintf(Id, sizeof(Id), "seat%d", Seats[i]);
					std::snprintf(Lab, sizeof(Lab), "%d", Seats[i]);
					ImGui::SetCursorScreenPos(ImVec2(P.x - Br, P.y - Br));
					ImGui::PushID(Id);
					const bool Hit = ImGui::InvisibleButton("s", ImVec2(Br * 2.f, Br * 2.f));
					const bool Hov = ImGui::IsItemHovered();
					ImGui::PopID();
					Dl->AddCircleFilled(P, Br,
						Hov ? IM_COL32(255, 255, 255, 230) : IM_COL32(255, 255, 255, 175), 24);
					Dl->AddCircle(P, Br, IM_COL32(255, 255, 255, 240), 24, 1.05f);
					const ImVec2 Ts = ImGui::CalcTextSize(Lab);
					Dl->AddText(ImVec2(P.x - Ts.x * 0.5f, P.y - Ts.y * 0.5f - 0.5f),
						IM_COL32(46, 50, 58, 235), Lab);
					if (Hit)
					{
						char Fb[24];
						std::snprintf(Fb, sizeof(Fb), "投%d号", Seats[i]);
						Send(U, 6, Seats[i] | (50 << 8), Fb);
						U.VoteOpen = false;
					}
				}
			}

			const float Cr = 22.f * S;
			ImGui::SetCursorScreenPos(ImVec2(C.x - Cr, C.y - Cr));
			ImGui::PushID("vclose");
			const bool Close = ImGui::InvisibleButton("c", ImVec2(Cr * 2.f, Cr * 2.f));
			const bool Chov = ImGui::IsItemHovered();
			ImGui::PopID();
			Dl->AddCircleFilled(C, Cr,
				Chov ? IM_COL32(236, 92, 108, 210) : IM_COL32(236, 92, 108, 170), 24);
			{
				const ImVec2 Ts = ImGui::CalcTextSize("关");
				Dl->AddText(ImVec2(C.x - Ts.x * 0.5f, C.y - Ts.y * 0.5f - 0.5f),
					IM_COL32(255, 255, 255, 240), "关");
			}
			if (Close)
				U.VoteOpen = false;
			ImGui::End();
			ImGui::PopStyleVar(3);
			ImGui::PopStyleColor(2);
		}
	}

	bool Run()
	{
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		Style();
		ImGuiIO& Io = ImGui::GetIO();
		Io.IniFilename = nullptr;
		Io.FontGlobalScale = 1.f;
		font::Load(Io);
		ImGui_ImplOpenGL3_Init("#version 300 es");

		Ui U;
		U.Ball = ImVec2(static_cast<float>(eglctx::Width()) - kBallR - 20.f,
			static_cast<float>(eglctx::Height()) * 0.35f);
		g_Stop.store(0);
		client::Warmup();
		std::thread Work(Worker);
		auto T0 = std::chrono::steady_clock::now();
		auto LastDisp = T0;
		int LastKey = -1;
		for (;;)
		{
			if (!U.Running)
				break;
			const auto T1 = std::chrono::steady_clock::now();
			Io.DeltaTime = std::chrono::duration<float>(T1 - T0).count();
			if (Io.DeltaTime <= 0.f)
				Io.DeltaTime = 1.f / 60.f;
			T0 = T1;
			const float Dt = Io.DeltaTime;

			if (T1 - LastDisp > std::chrono::milliseconds(100))
			{
				LastDisp = T1;
				const auto D = disp::Query();
				int Dw = D.W > 64 ? D.W : eglctx::Width();
				int Dh = D.H > 64 ? D.H : eglctx::Height();
				int Orient = D.Orient & 3;
				const bool Land = Dw > Dh;
				if (Land && (Orient == 0 || Orient == 2))
					Orient = (Orient == 0) ? 1 : 3;
				if (!Land && (Orient == 1 || Orient == 3))
					Orient = (Orient == 1) ? 0 : 2;
				eglctx::Relayout(Dw, Dh);
				const int Ew = eglctx::Width();
				const int Eh = eglctx::Height();
				touch::SetDisplay(static_cast<float>(Ew), static_cast<float>(Eh), Orient);
				const int Key = (Ew << 16) ^ Eh ^ (Orient << 28);
				if (Key != LastKey)
				{
					LastKey = Key;
					U.Dock = true;
					U.Ball.x = static_cast<float>(Ew) - kBallR - 16.f;
					U.Ball.y = static_cast<float>(Eh) * 0.32f;
				}
			}
			const float W = static_cast<float>(eglctx::Width());
			const float H = static_cast<float>(eglctx::Height());

			const auto T = touch::Snapshot();
			if (!U.BallArm)
			{
				Io.AddMousePosEvent(T.X, T.Y);
				if (T.Pressed)
					Io.AddMouseButtonEvent(0, true);
				if (T.Released)
					Io.AddMouseButtonEvent(0, false);
			}

			Io.DisplaySize = ImVec2(W, H);
			Io.FontGlobalScale = U.Scale;
			ImGui_ImplOpenGL3_NewFrame();
			ImGui::NewFrame();
			DrawBall(U, W, H, T);
			DrawPanel(U, Dt, W, H);
			DrawVoteWheel(U, W, H);
			ImGui::Render();
			glViewport(0, 0, eglctx::Width(), eglctx::Height());
			glClearColor(0, 0, 0, 0);
			glClear(GL_COLOR_BUFFER_BIT);
			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
			eglctx::Swap();
		}
		g_Stop.store(1);
		if (Work.joinable())
			Work.join();
		ImGui_ImplOpenGL3_Shutdown();
		ImGui::DestroyContext();
		return true;
	}
}
