#pragma once

#include "Text.h"

namespace probe::forms
{
	// "<plugin>:<local id hex>", "dyn:<formid hex>" for runtime forms, "" for null. The plugin file
	// name goes through text::KeyText: lossless, with %XX for bytes of a name that is not UTF-8.
	std::string Key(const RE::TESForm* a_form);
	std::string Key(RE::FormID a_formID);

	// JSON object {"key":..,"name":..} for a form, or null.
	std::string Json(const RE::TESForm* a_form);
	// JSON object for a reference: {"key":ref key,"base":base key,"name":..}, or null.
	std::string Json(const RE::TESObjectREFR* a_ref);

	enum class ObjectCheck
	{
		ok,
		null,
		unaligned,
		unreadable,
		notGameVtable,
	};

	const char* ObjectCheckName(ObjectCheck a_check) noexcept;

	// Holds only the static-lifetime perf name literal and feeds each ReadProcessMemory copy
	// duration into perf::Add.
	class CopyTiming final : public text::CallObserver
	{
	public:
		explicit CopyTiming(const char* a_perfName) noexcept :
			_perfName(a_perfName)
		{}

		void OnCall(const text::CallInfo& a_call) noexcept override;

	private:
		const char* _perfName;
	};

	// Checks whether an address is 8-byte aligned and its first 8 bytes, copied through
	// ReadProcessMemory without VirtualQuery, hold an 8-byte-aligned vtable address inside the game
	// executable's .rdata. Never faults, and throws only if a_observer does (CopyTiming does not),
	// so the check itself is safe for stale or garbage pointers. ok does not prove a live object:
	// a released object whose first 8 bytes still hold its vtable passes too, and later direct
	// reads of the object are not covered. Because VirtualQuery is not used, PAGE_EXECUTE pages
	// can also be read on this Windows build.
	ObjectCheck CheckGameObject(const void* a_ptr, text::CallObserver* a_observer = nullptr);
	// The form at this address, or nullptr unless it is a live form: an 8-byte aligned address whose
	// copied TESForm header starts with a game vtable and whose form ID resolves back to the same
	// address. For pointers taken from event structs whose layout has not been seen working on this
	// runtime (a SpellsLearned event carried 0x46 where CommonLib declares a SpellItem*, and reading
	// it as one crashed the game). Checks that it is a form, not which type: the caller still has to
	// check the type it expects.
	const RE::TESForm* VerifiedForm(const void* a_ptr, text::CallObserver* a_observer = nullptr);

	// Runs once per process and emits a "form.selfTest" line exercising CheckGameObject (player,
	// low unaligned/aligned, local buffer, decommitted page) and VerifiedForm (player, copied
	// TESForm header).
	void RunSelfTestOnce();

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
