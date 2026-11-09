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
		// Written once at install, read by the thunks on whatever thread the game calls them from.
		std::atomic<Fn*> g_origActor{ nullptr };
		std::atomic<Fn*> g_origCharacter{ nullptr };
		std::atomic<Fn*> g_origPlayer{ nullptr };

		struct ModuleRef
		{
			std::string   name{ "?" };
			std::uint64_t offset{ 0 };  // address minus module base (RVA)
		};

		ModuleRef ModuleOf(std::uintptr_t a_address)
		{
			ModuleRef ref;
			HMODULE   module = nullptr;
			if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(a_address), &module) ||
				!module) {
				return ref;
			}
			ref.offset = a_address - reinterpret_cast<std::uintptr_t>(module);
			wchar_t path[MAX_PATH]{};
			const auto len = ::GetModuleFileNameW(module, path, MAX_PATH);
			if (len != 0) {
				ref.name = std::filesystem::path(std::wstring(path, len)).filename().string();
			}
			return ref;
		}

		// Some callers pass stale or garbage actor pointers (CalamityAffixes logs and drops them).
		// An actor is accepted only if it is readable and its vtable lies inside the game executable.
		RE::Actor* Sanitize(RE::Actor* a_actor)
		{
			return forms::IsGameObject(a_actor) ? a_actor : nullptr;
		}

		// Attacker-less damage below this size arrives every frame (B run: 6,677 of 7,545 calls).
		// Each call is still written as a damage line. On top of that the hook counts them per target,
		// and the poll task lists the target's health effects once per window.
		constexpr float  kDotThreshold = 0.5f;
		constexpr double kDotWindowMs = 5000.0;

		struct DotState
		{
			std::uint64_t ticks{ 0 };       // since the last report
			double        total{ 0.0 };     // summed damage since the last report (negative)
			double        lastReportMs{ -kDotWindowMs };
		};

		std::mutex                               g_dotLock;
		std::unordered_map<RE::FormID, DotState> g_dot;

		void NoteDot(RE::FormID a_target, float a_damage)
		{
			std::scoped_lock lock(g_dotLock);
			auto& state = g_dot[a_target];
			++state.ticks;
			state.total += static_cast<double>(a_damage);
		}

		// Same policy as Line::num: NaN/inf become null so the line stays valid JSON.
		std::string Number(float a_value)
		{
			return std::isfinite(a_value) ? std::format("{}", a_value) : std::string("null");
		}

		// Walks the active effects through the game's own visitor, which takes the magic target's
		// lock; actor updates run on several job threads, so the raw list is not iterated directly.
		class HealthEffectVisitor final : public RE::MagicTarget::ForEachActiveEffectVisitor
		{
		public:
			RE::BSContainer::ForEachResult Accept(RE::ActiveEffect* a_effect) override
			{
				const auto* base = a_effect ? a_effect->GetBaseObject() : nullptr;
				if (base && (base->data.primaryAV == RE::ActorValue::kHealth || base->data.secondaryAV == RE::ActorValue::kHealth)) {
					json += std::format(
						R"({}{{"effect":"{}","spell":"{}","archetype":{},"magnitude":{},"detrimental":{},"inactive":{},"elapsed":{},"duration":{}}})",
						json.size() > 1 ? "," : "", Escape(forms::Key(base)), Escape(forms::Key(a_effect->spell)),
						static_cast<int>(base->GetArchetype()), Number(a_effect->magnitude), base->IsDetrimental() ? "true" : "false",
						a_effect->flags.any(RE::ActiveEffect::Flag::kInactive) ? "true" : "false", Number(a_effect->elapsedSeconds),
						Number(a_effect->duration));
				}
				return RE::BSContainer::ForEachResult::kContinue;
			}

			std::string json{ "[" };
		};

		std::string HealthEffectsJson(RE::Actor* a_actor)
		{
			auto* magicTarget = a_actor ? a_actor->AsMagicTarget() : nullptr;
			if (!magicTarget) {
				return "[]";
			}
			HealthEffectVisitor visitor;
			magicTarget->VisitEffects(visitor);
			return visitor.json + "]";
		}

		float Health(RE::Actor* a_actor)
		{
			return a_actor ? a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) : -1.0f;
		}

		float MaxHealth(RE::Actor* a_actor)
		{
			return a_actor ? a_actor->AsActorValueOwner()->GetPermanentActorValue(RE::ActorValue::kHealth) : -1.0f;
		}

		void ThunkActor(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage);
		void ThunkCharacter(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage);
		void ThunkPlayer(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage);

		// a_caller: return address into whoever called the HandleHealthDamage slot. While our thunk is the
		// outermost hook this is the real call site (game code or another plugin's DLL).
		void Observe(const char* a_label, Fn* a_original, std::uintptr_t a_caller, RE::Actor* a_this, RE::Actor* a_attacker, float a_damage)
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
				const auto caller = ModuleOf(a_caller);
				Line("damage.badTarget").str("vtbl", a_label).hex("target", reinterpret_cast<std::uintptr_t>(a_this))
					.str("callerModule", caller.name).hex("callerOffset", caller.offset).emit();
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
			if (!attacker && a_damage < 0.0f && a_damage > -kDotThreshold) {
				NoteDot(target->GetFormID(), a_damage);
			}
			const float after = Health(target);
			const auto  caller = ModuleOf(a_caller);
			// If another plugin hooked the slot after us, its DLL is what we see as the caller.
			// Report whether our thunk is still the one in the target's vtable slot.
			const auto* vtbl = *reinterpret_cast<const std::uintptr_t* const*>(target);
			const bool  outermost = vtbl && (vtbl[kSlot] == reinterpret_cast<std::uintptr_t>(&ThunkActor) ||
			                                    vtbl[kSlot] == reinterpret_cast<std::uintptr_t>(&ThunkCharacter) ||
			                                    vtbl[kSlot] == reinterpret_cast<std::uintptr_t>(&ThunkPlayer));
			Line("damage")
				.str("vtbl", a_label)
				.str("callerModule", caller.name)
				.hex("callerOffset", caller.offset)
				.flag("probeOutermost", outermost)
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

		// noinline keeps _ReturnAddress() pointing at the caller of the vtable slot.
		__declspec(noinline) void ThunkActor(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage)
		{
			Observe("Actor", g_origActor.load(), reinterpret_cast<std::uintptr_t>(_ReturnAddress()), a_this, a_attacker, a_damage);
		}
		__declspec(noinline) void ThunkCharacter(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage)
		{
			Observe("Character", g_origCharacter.load(), reinterpret_cast<std::uintptr_t>(_ReturnAddress()), a_this, a_attacker, a_damage);
		}
		__declspec(noinline) void ThunkPlayer(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage)
		{
			Observe("PlayerCharacter", g_origPlayer.load(), reinterpret_cast<std::uintptr_t>(_ReturnAddress()), a_this, a_attacker, a_damage);
		}

		void Install(const char* a_label, REL::Relocation<std::uintptr_t> a_vtbl, Fn* a_thunk, std::atomic<Fn*>& a_original)
		{
			// Chain to whatever is in the slot now, vanilla or another plugin's hook. Never skip.
			// The chain target is stored before the slot points at our thunk: actor updates run on job
			// threads, and a call arriving in between would find nothing to pass the damage on to.
			const auto* entries = reinterpret_cast<const std::uintptr_t*>(a_vtbl.address());
			const std::uintptr_t seen = entries[kSlot];
			a_original.store(reinterpret_cast<Fn*>(seen));
			const std::uintptr_t previous = a_vtbl.write_vfunc(kSlot, a_thunk);
			if (previous != seen) {
				// Another plugin replaced the slot between the read and the write; chain to what was really there.
				a_original.store(reinterpret_cast<Fn*>(previous));
			}
			const auto prev = ModuleOf(previous);
			Line("probe.damageHook")
				.str("vtbl", a_label)
				.hex("previous", previous)
				.str("previousModule", prev.name)
				.hex("previousOffset", prev.offset)
				.flag("chained", previous != 0)
				.flag("slotChangedDuringInstall", previous != seen)
				.emit();
		}
	}

	void FlushDotSources()
	{
		struct Report
		{
			RE::FormID    id;
			std::uint64_t ticks;
			double        total;
		};
		std::vector<Report> due;
		const double        now = out::ElapsedMs();
		{
			std::scoped_lock lock(g_dotLock);
			for (auto& [id, state] : g_dot) {
				if (state.ticks == 0 || now - state.lastReportMs < kDotWindowMs) {
					continue;
				}
				due.push_back({ id, state.ticks, state.total });
				state.ticks = 0;
				state.total = 0.0;
				state.lastReportMs = now;
			}
		}
		for (const auto& report : due) {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(report.id);
			Line("damage.dotSources")
				.raw("target", forms::Json(actor))
				.str("id", forms::Key(report.id))
				.num("ticks", report.ticks)
				.num("total", report.total)
				.raw("healthEffects", HealthEffectsJson(actor))
				.emit();
		}
	}

	void ResetDotSources()
	{
		std::scoped_lock lock(g_dotLock);
		g_dot.clear();
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
