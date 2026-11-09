#pragma once

namespace probe::forms
{
	// "<plugin>:<local id hex>", "dyn:<formid hex>" for runtime forms, "" for null.
	std::string Key(const RE::TESForm* a_form);
	std::string Key(RE::FormID a_formID);

	// JSON object {"key":..,"name":..} for a form, or null.
	std::string Json(const RE::TESForm* a_form);
	// JSON object for a reference: {"key":ref key,"base":base key,"name":..}, or null.
	std::string Json(const RE::TESObjectREFR* a_ref);

	// True if the address is readable and starts with a vtable inside the game executable.
	// Never throws or faults, so it is safe for stale or garbage pointers.
	bool IsGameObject(const void* a_ptr);
	// The form at this address, or nullptr unless it is a live form: a game object that the game
	// returns for its own form ID. For pointers taken from event structs whose layout has not been
	// seen working on this runtime (a SpellsLearned event carried 0x46 where CommonLib declares a
	// SpellItem*, and reading it as one crashed the game).
	const RE::TESForm* VerifiedForm(const void* a_ptr);

	bool IsPlayer(const RE::TESObjectREFR* a_ref);
	// Player or a current player teammate.
	bool IsPlayerSide(const RE::TESObjectREFR* a_ref);

	// Teammates seen so far (form IDs), for polling.
	void NoteTeammate(const RE::TESObjectREFR* a_ref);
	std::vector<RE::FormID> KnownTeammates();
	void ResetTeammates();

	// Distance in game units, or -1 when unknown or not in the player's space
	// (different interior cell, interior vs exterior, or another worldspace).
	float DistanceToPlayer(const RE::TESObjectREFR* a_ref);

	// Poll task: resolves known teammate form IDs to object addresses for the damage hook.
	void PublishTeammatePointers();
	// Any thread: true if the address is a known teammate. Compares addresses against the last
	// published list only and never dereferences, so it is safe for unvalidated pointers.
	bool IsKnownTeammatePtr(const void* a_ptr);
}
