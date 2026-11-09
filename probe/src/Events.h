#pragma once

namespace probe::events
{
	// Registers every event sink that does not need the player (call at kDataLoaded).
	void RegisterGlobal();
	// Marks the player animation graph sink for (re)attachment. Call at each game start.
	void AttachPlayerGraph();
	// From the poll task (the OS thread varies): attaches the sink when marked, after a failed attempt,
	// after a race switch (werewolf, vampire lord), and every 30 s to survive 3D/graph reloads.
	void EnsurePlayerGraph();
	// Poll task: attaches a graph sink to actors queued by combat events against the player side
	// (at most 32 at once) and detaches those that died, unloaded or left combat.
	void EnsureEnemyGraphs();
	// Forgets every tracked opponent. Call at each game start.
	void ResetEnemyGraphs();
	// Dumps aggregated animation tag (player and opponents) and ModEvent name counts.
	void DumpCounters();
}
