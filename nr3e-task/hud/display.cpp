#include "display.hpp"
#include "a_native_window_creator.h"

#include <algorithm>
#include <cstdio>

namespace disp
{
	Info Query()
	{
		Info Out{};
		const auto Di = android::ANativeWindowCreator::GetDisplayInfo();
		Out.W = Di.width;
		Out.H = Di.height;
		int T = Di.theta % 360;
		if (T < 0)
			T += 360;
		if (T >= 45 && T < 135)
			Out.Orient = 1;
		else if (T >= 135 && T < 225)
			Out.Orient = 2;
		else if (T >= 225 && T < 315)
			Out.Orient = 3;
		else
			Out.Orient = 0;
		return Out;
	}
}
