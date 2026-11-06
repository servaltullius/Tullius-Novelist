#include "DamageHook.h"
#include "Forms.h"
#include "Out.h"

namespace probe::damage
{
	namespace
	{
		using Fn = void(RE::Actor*, RE::Actor*, float);

		// HandleHealthDamage slot (Actor.h): 0x104 on SE/AE. This build disables VR.
		constexpr std::size_t kSlot = 0x104;

		std::atomic_bool g_installed{ false };
		Fn*              g_origActor = nullptr;
		Fn*              g_origCharacter = nullptr;
		Fn*              g_origPlayer = nullptr;

		std::string ModuleOf(std::uintptr_t a_address)
		{
			HMODULE module = nullptr;
			if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(a_address), &module) ||
				!module) {
				return "?";
			}
			wchar_t path[MAX_PATH]{};
			const auto len = ::GetModuleFileNameW(module, path, MAX_PATH);
			if (len == 0) {
				return "?";
			}
			return std::filesystem::path(std::wstring(path, len)).filename().string();
		}

		bool IsReadable(const void* a_ptr, std::size_t a_size)
		{
			if (!a_ptr) {
				return false;
			}
			MEMORY_BASIC_INFORMATION mbi{};
			if (::VirtualQuery(a_ptr, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT) {
				return false;
			}
			if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
				return false;
			}
			const auto start = reinterpret_cast<std::uintptr_t>(a_ptr);
			const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
			return start + a_size <= regionEnd;
		}

		// Some callers pass stale or garbage actor pointers (CalamityAffixes logs and drops them).
		// An actor is accepted only if it is readable and its vtable lies inside the game executable.
		RE::Actor* Sanitize(RE::Actor* a_actor)
		{
			if (!a_actor || !IsReadable(a_actor, sizeof(void*))) {
				return nullptr;
			}
			const auto vtbl = *reinterpret_cast<const std::uintptr_t*>(a_actor);
			const auto rdata = REL::Module::get().segment(REL::Segment::rdata);
			const auto begin = rdata.address();
			const auto end = begin + rdata.size();
			if (vtbl < begin || vtbl >= end) {
				return nullptr;
			}
			return a_actor;
		}

		float Health(RE::Actor* a_actor)
		{
			return a_actor ? a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) : -1.0f;
		}

		float MaxHealth(RE::Actor* a_actor)
		{
			return a_actor ? a_actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth) : -1.0f;
		}

		void Observe(const char* a_label, Fn* a_original, RE::Actor* a_this, RE::Actor* a_attacker, float a_damage)
		{
			// The probe only observes: the original arguments always go down the chain unchanged.
			// Cheap address-only relevance check first, so the per-call cost for NPC-vs-NPC damage
			// stays at a few comparisons; pointers are validated only when we are going to read them.
			const void* player = RE::PlayerCharacter::GetSingleton();
			const bool  maybeRelevant = (player && (a_this == player || a_attacker == player)) ||
			                           forms::IsKnownTeammatePtr(a_this) || forms::IsKnownTeammatePtr(a_attacker);
			if (!maybeRelevant) {
				if (a_original) {
					a_original(a_this, a_attacker, a_damage);
				}
				return;
			}

			RE::Actor* target = nullptr;
			RE::Actor* attacker = nullptr;
			{
				perf::Scope scope("damage.filter");
				target = Sanitize(a_this);
				attacker = Sanitize(a_attacker);
			}
			if (!target) {
				Line("damage.badTarget").str("vtbl", a_label).hex("target", reinterpret_cast<std::uintptr_t>(a_this)).emit();
				if (a_original) {
					a_original(a_this, a_attacker, a_damage);
				}
				return;
			}

			float before = 0.0f;
			float maxHp = 0.0f;
			{
				perf::Scope scope("damage.pre");
				before = Health(target);
				maxHp = MaxHealth(target);
			}
			if (a_original) {
				a_original(a_this, a_attacker, a_damage);
			}
			perf::Scope scope("damage.post");
			const float after = Health(target);
			Line("damage")
				.str("vtbl", a_label)
				.raw("target", forms::Json(target))
				.raw("attacker", forms::Json(attacker))
				.flag("attackerSanitized", a_attacker != nullptr && attacker == nullptr)
				.num("reported", static_cast<double>(a_damage))
				.num("hpBefore", static_cast<double>(before))
				.num("hpAfter", static_cast<double>(after))
				.num("hpMax", static_cast<double>(maxHp))
				.flag("targetKillMove", target->IsInKillMove())
				.emit();
		}

		void ThunkActor(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage) { Observe("Actor", g_origActor, a_this, a_attacker, a_damage); }
		void ThunkCharacter(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage) { Observe("Character", g_origCharacter, a_this, a_attacker, a_damage); }
		void ThunkPlayer(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage) { Observe("PlayerCharacter", g_origPlayer, a_this, a_attacker, a_damage); }

		void Install(const char* a_label, REL::Relocation<std::uintptr_t> a_vtbl, Fn* a_thunk, Fn*& a_original)
		{
			auto* entries = reinterpret_cast<std::uintptr_t*>(a_vtbl.address());
			const std::uintptr_t previous = entries ? entries[kSlot] : 0;
			// Chain to whatever is in the slot now, vanilla or another plugin's hook. Never skip.
			a_original = reinterpret_cast<Fn*>(a_vtbl.write_vfunc(kSlot, a_thunk));
			Line("probe.damageHook")
				.str("vtbl", a_label)
				.hex("previous", previous)
				.str("previousModule", ModuleOf(previous))
				.flag("chained", a_original != nullptr)
				.emit();
		}
	}

	void InstallOnce()
	{
		if (g_installed.exchange(true)) {
			return;
		}
		Install("Actor", REL::Relocation<std::uintptr_t>{ RE::VTABLE_Actor[0] }, ThunkActor, g_origActor);
		Install("Character", REL::Relocation<std::uintptr_t>{ RE::VTABLE_Character[0] }, ThunkCharacter, g_origCharacter);
		Install("PlayerCharacter", REL::Relocation<std::uintptr_t>{ RE::VTABLE_PlayerCharacter[0] }, ThunkPlayer, g_origPlayer);
	}
}
