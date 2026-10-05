#pragma once

namespace touch
{
	struct State
	{
		float X = 0.f;
		float Y = 0.f;
		bool Down = false;
		bool Pressed = false;
		bool Released = false;
	};

	bool Init(float ScreenW, float ScreenH);
	void SetDisplay(float ScreenW, float ScreenH, int Orient);
	State Snapshot();
	void Shutdown();
}
