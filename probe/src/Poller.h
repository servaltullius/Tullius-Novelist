#pragma once

namespace probe::poller
{
	// Starts a background clock that schedules a main-thread sample every 500 ms
	// and dumps perf/counter summaries every 30 s.
	void Start();
	// Resets remembered state so the next sample is a fresh baseline (after a load).
	void ResetBaseline();
}
