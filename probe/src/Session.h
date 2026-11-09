#pragma once

// Tracks whether a game is running, so the per-game state is reset and the damage hook installed
// once per game start, also when no SKSE message announces the start (coc from the main menu).
// SKSE messages, co-save callbacks and the poll task arrive on different threads; every function
// here is safe from any of them.
namespace probe::session
{
	// kPreLoadGame: a load is in progress until OnGameStart or OnLoadFailed.
	void OnPreLoad();
	// kNewGame, or kPostLoadGame with success. a_by names the signal ("newGame", "load").
	void OnGameStart(std::string_view a_by);
	// kPostLoadGame without success.
	void OnLoadFailed();
	// Co-save revert: the running game is being torn down (load, new game, or exit to the main menu).
	void OnRevert();
	// Poll task, player in the world: starts the session as "world" if no message did.
	// Returns at once while a session is running or a load is in progress.
	void EnsureStarted();
}
