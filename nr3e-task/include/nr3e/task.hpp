#pragma once

#include <cstdint>
#include <string>

namespace nr3e::task
{
	enum class Cmd : std::int32_t
	{
		Status = 0,
		SetEnable = 1,
		CompleteNormal = 2,
		CompleteUrgent = 3,
		TriggerUrgent = 4,
		FireEvent = 5,
		MeetingVote = 6,
		VoteInfo = 7,
		Identity = 8,
	};

	struct Reply
	{
		std::string Error;
		std::string Message;
		bool Enabled = false;
		std::int32_t Touched = 0;
	};

	Reply Run(std::int32_t Command, std::int32_t Extra);
	void Pump();
}
