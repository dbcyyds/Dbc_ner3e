#pragma once

namespace disp
{
	struct Info
	{
		int W = 0;
		int H = 0;
		int Orient = 0; // 0/1/2/3 = 0/90/180/270
	};

	Info Query();
}
