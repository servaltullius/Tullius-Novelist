#pragma once

namespace probe::damage
{
	// Installs the HandleHealthDamage hooks on Actor, Character and PlayerCharacter vtables.
	// Called once, late (first game start: new game, load, or coc), so that plugins that hook at kDataLoaded
	// (for example CalamityAffixes, which skips its own hook when the slot is already patched)
	// are in place first. Always chains to whatever the slot held before.
	void InstallOnce();

	// Poll task: for each player-side target that took attacker-less damage under 0.5 in the last
	// window, reports the tick count, summed damage and the health-touching active effects
	// (at most once per 5 s per target). The hook itself only counts.
	void FlushDotSources();
	// Clears the per-target counters. Call at each game start.
	void ResetDotSources();
}
