#include "Poller.h"
#include "DamageHook.h"
#include "Events.h"
#include "Forms.h"
#include "Out.h"
#include "Session.h"
#include "World.h"

namespace probe::poller
{
	namespace
	{
		struct PlayerState
		{
			bool valid{ false };
			bool sneaking{ false };
			bool mounted{ false };
			bool swimming{ false };
			bool killMove{ false };
			bool godMode{ false };
			bool console{ false };
			int  hpBand{ -1 };  // 0: <10%, 1: <25%, 2: <50%, 3: >=50%
		};

		std::mutex                                g_stateLock;
		PlayerState                               g_last;
		std::unordered_map<RE::FormID, bool>      g_teamLast;
		std::atomic_bool                          g_samplePending{ false };

		int Band(float a_ratio)
		{
			if (a_ratio < 0.10f) {
				return 0;
			}
			if (a_ratio < 0.25f) {
				return 1;
			}
			if (a_ratio < 0.50f) {
				return 2;
			}
			return 3;
		}

		template <class T>
		void Changed(const char* a_what, T a_old, T a_new, bool a_valid)
		{
			if (!a_valid || a_old != a_new) {
				Line("poll.change").str("what", a_what).num("value", static_cast<std::int64_t>(a_new)).flag("baseline", !a_valid).emit();
			}
		}

		// Runs as an SKSE task. One sample at a time, but not on one fixed thread: in the stage 0
		// records the task ran on other threads than the one that delivers SKSE messages.
		void SampleTask()
		{
			g_samplePending.store(false);
			perf::Scope scope("poll.sample");
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !player->Is3DLoaded()) {
				return;
			}
			// A game started from the main menu with coc sends no SKSE message; the player in the world is the signal.
			out::Arm();
			session::EnsureStarted();
			auto* av = player->AsActorValueOwner();
			const float hp = av->GetActorValue(RE::ActorValue::kHealth);
			const float hpMax = av->GetPermanentActorValue(RE::ActorValue::kHealth);

			PlayerState now;
			now.valid = true;
			now.sneaking = player->IsSneaking();
			now.mounted = player->IsOnMount();
			now.swimming = player->AsActorState()->IsSwimming();
			now.killMove = player->IsInKillMove();
			now.godMode = RE::PlayerCharacter::IsGodMode();
			if (auto* ui = RE::UI::GetSingleton()) {
				now.console = ui->IsMenuOpen(RE::Console::MENU_NAME);
			}
			now.hpBand = hpMax > 0.0f ? Band(hp / hpMax) : -1;

			PlayerState last;
			{
				std::scoped_lock lock(g_stateLock);
				last = g_last;
				g_last = now;
			}
			Changed("sneaking", last.sneaking, now.sneaking, last.valid);
			Changed("mounted", last.mounted, now.mounted, last.valid);
			Changed("swimming", last.swimming, now.swimming, last.valid);
			Changed("killMove", last.killMove, now.killMove, last.valid);
			Changed("godMode", last.godMode, now.godMode, last.valid);
			Changed("console", last.console, now.console, last.valid);
			Changed("hpBand", last.hpBand, now.hpBand, last.valid);

			world::Sample(player);
			forms::PublishTeammatePointers();
			events::EnsurePlayerGraph();
			events::EnsureEnemyGraphs();
			damage::FlushDotSources();

			for (const auto id : forms::KnownTeammates()) {
				auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
				auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
				if (!actor) {
					continue;
				}
				const bool loaded = actor->Is3DLoaded();
				const bool teammate = actor->IsPlayerTeammate();
				bool changed = false;
				{
					std::scoped_lock lock(g_stateLock);
					auto it = g_teamLast.find(id);
					changed = (it == g_teamLast.end() || it->second != teammate);
					g_teamLast[id] = teammate;
				}
				if (teammate && loaded) {
					world::EnsureBaseline(id);
				}
				if (changed) {
					Line("poll.teammate").raw("actor", forms::Json(actor)).flag("teammate", teammate).flag("loaded", loaded).emit();
				}
			}
		}

		void ClockLoop()
		{
			int ticks = 0;
			for (;;) {
				std::this_thread::sleep_for(500ms);
				if (!g_samplePending.exchange(true)) {
					if (auto* task = SKSE::GetTaskInterface()) {
						task->AddTask([] { SampleTask(); });
					} else {
						g_samplePending.store(false);
					}
				}
				if (++ticks % 60 == 0) {  // every 30 s
					perf::Dump();
					events::DumpCounters();
				}
			}
		}
	}

	void Start()
	{
		std::thread(ClockLoop).detach();
	}

	void ResetBaseline()
	{
		std::scoped_lock lock(g_stateLock);
		g_last = PlayerState{};
		g_teamLast.clear();
	}
}
