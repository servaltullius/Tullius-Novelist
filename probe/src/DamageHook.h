#pragma once

namespace probe::damage
{
	// Installs the HandleHealthDamage hooks on Actor, Character and PlayerCharacter vtables.
	// Called once, late (first new game or load), so that plugins that hook at kDataLoaded
	// (for example CalamityAffixes, which skips its own hook when the slot is already patched)
	// are in place first. Always chains to whatever the slot held before.
	void InstallOnce();
}
