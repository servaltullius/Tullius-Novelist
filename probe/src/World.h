#pragma once

// Scene context for the novel: in-game time, weather, the player's cell, and what the player
// side wears. Written only when something changes, so a chapter can be placed in time and space.
namespace probe::world
{
	// Main thread (poll task, every 500 ms, player 3D loaded): game time, weather, cell, and
	// equipment snapshots that are due.
	void Sample(RE::PlayerCharacter* a_player);

	// Any thread: this actor equipped or unequipped something. A snapshot follows once no change
	// came for 1.5 s (capped at 5 s), and only if the content differs from the last one, so a
	// quick outfit swap gives one line.
	void NoteEquipChange(RE::FormID a_actor);

	// Any thread, cheap to call every poll: if this actor has no snapshot since the last load and
	// none is pending, take one after the settle time (player after load, teammate in the scene).
	void EnsureBaseline(RE::FormID a_actor);

	// After a load or new game: forget remembered state so the next sample writes a baseline.
	void Reset();
}
