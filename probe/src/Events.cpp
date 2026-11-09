#include "Events.h"
#include "Forms.h"
#include "Out.h"
#include "World.h"

namespace probe::events
{
	namespace
	{
		using Ctrl = RE::BSEventNotifyControl;

		// One sink type per event: times the handler and forwards the event.
		template <class E>
		class Sink final : public RE::BSTEventSink<E>
		{
		public:
			using Handler = void (*)(const E&);

			Sink(const char* a_name, Handler a_handler) :
				_name(a_name), _handler(a_handler)
			{}

			Ctrl ProcessEvent(const E* a_event, RE::BSTEventSource<E>*) override
			{
				if (a_event) {
					perf::Scope scope(_name);
					_handler(*a_event);
				}
				return Ctrl::kContinue;
			}

		private:
			const char* _name;
			Handler     _handler;
		};

		template <class E>
		Sink<E>* Make(const char* a_name, typename Sink<E>::Handler a_handler)
		{
			// Sinks live for the whole process; the game keeps raw pointers to them.
			return new Sink<E>(a_name, a_handler);
		}

		template <class E>
		void AddToHolder(const char* a_name, typename Sink<E>::Handler a_handler)
		{
			if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
				holder->AddEventSink<E>(Make<E>(a_name, a_handler));
				Line("probe.sink").str("name", a_name).str("via", "holder").emit();
			} else {
				Line("probe.sink.fail").str("name", a_name).emit();
			}
		}

		template <class E, class Src>
		void AddToSource(const char* a_name, Src* a_source, typename Sink<E>::Handler a_handler)
		{
			if (a_source) {
				a_source->AddEventSink(Make<E>(a_name, a_handler));
				Line("probe.sink").str("name", a_name).str("via", "source").emit();
			} else {
				Line("probe.sink.fail").str("name", a_name).emit();
			}
		}

		const char* QuestName(RE::FormID a_id)
		{
			const auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(a_id);
			const char* name = quest ? quest->GetName() : nullptr;
			return name ? name : "";
		}

		// Tags written in full every time. Everything else is only counted.
		bool IsInterestingTag(std::string_view a_tag)
		{
			static constexpr std::array keys{
				"dodge"sv, "parry"sv, "stagger"sv, "block"sv, "guard"sv, "kill"sv, "potion"sv, "drink"sv,
				"tkd"sv, "msl_"sv, "cmf"sv, "maxsu"sv, "iframe"sv, "bash"sv, "recoil"sv, "bleed"sv,
				"poise"sv, "execution"sv, "paired"sv, "eat"sv, "attackstart"sv, "weaponswing"sv, "hitframe"sv,
				"bowdraw"sv, "bowrelease"sv, "arrowrelease"sv, "spellfire"sv, "shout"sv, "jump"sv, "fall"sv
			};
			std::string lower(a_tag);
			for (auto& c : lower) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			for (const auto key : keys) {
				if (lower.find(key) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		// Heterogeneous lookup: count by string_view without allocating a std::string per event.
		struct SvHash
		{
			using is_transparent = void;
			std::size_t operator()(std::string_view a_text) const noexcept { return std::hash<std::string_view>{}(a_text); }
		};
		using CountMap = std::unordered_map<std::string, std::uint64_t, SvHash, std::equal_to<>>;

		// Returns true when this is the first time the name is seen.
		bool Count(CountMap& a_map, std::string_view a_name)
		{
			if (auto it = a_map.find(a_name); it != a_map.end()) {
				++it->second;
				return false;
			}
			a_map.emplace(std::string(a_name), 1);
			return true;
		}

		std::mutex g_countLock;
		CountMap   g_animCounts;
		CountMap   g_modEventCounts;

		std::atomic_bool g_graphWanted{ false };
		std::atomic_bool g_graphAttached{ false };
		// EnsurePlayerGraph comes from the poll task and from a game start, on different threads.
		std::mutex g_graphLock;
		int        g_graphRefreshTicks = 0;  // guarded by g_graphLock

		// effect.apply de-duplication: same target+effect within 2 s is counted, not logged.
		std::mutex                                       g_effectLock;
		std::unordered_map<std::string, double>          g_effectLastMs;
		std::uint64_t                                    g_effectSuppressed = 0;

		class PlayerGraphSink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			Ctrl ProcessEvent(const RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
			{
				if (!a_event) {
					return Ctrl::kContinue;
				}
				perf::Scope scope("anim.player");
				const std::string_view tag = a_event->tag.c_str() ? a_event->tag.c_str() : "";
				bool first = false;
				{
					std::scoped_lock lock(g_countLock);
					first = Count(g_animCounts, tag);
				}
				if (first || IsInterestingTag(tag)) {
					Line(first ? "anim.first" : "anim")
						.str("tag", tag)
						.str("payload", a_event->payload.c_str() ? a_event->payload.c_str() : "")
						.emit();
				}
				return Ctrl::kContinue;
			}
		};

		PlayerGraphSink g_playerGraphSink;

		// Graphs of actors fighting the player side. A successful parry (Parry for All) cancels the
		// attacker's hit and staggers the attacker, so it shows on the attacker's graph, not ours.
		constexpr std::size_t kMaxEnemyGraphs = 32;
		CountMap              g_enemyAnimCounts;  // guarded by g_countLock

		std::mutex                     g_enemyLock;
		std::unordered_set<RE::FormID> g_enemyPending;   // queued by combat events (any thread)
		std::unordered_set<RE::FormID> g_enemyAttached;  // poll task and game start, under g_enemyLock
		bool                           g_enemyFullLogged = false;

		class EnemyGraphSink final : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			Ctrl ProcessEvent(const RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
			{
				if (!a_event) {
					return Ctrl::kContinue;
				}
				perf::Scope scope("anim.enemy");
				const std::string_view tag = a_event->tag.c_str() ? a_event->tag.c_str() : "";
				bool first = false;
				{
					std::scoped_lock lock(g_countLock);
					first = Count(g_enemyAnimCounts, tag);
				}
				if (first || IsInterestingTag(tag)) {
					// Runs on animation threads: record only the form ID (no name or extra-data reads).
					// The probe.enemyGraph attach line carries the full identity for the same ID.
					const auto id = a_event->holder ? a_event->holder->GetFormID() : 0u;
					Line(first ? "anim.enemy.first" : "anim.enemy")
						.str("actor", std::format("{:08X}", id))
						.str("tag", tag)
						.str("payload", a_event->payload.c_str() ? a_event->payload.c_str() : "")
						.emit();
				}
				return Ctrl::kContinue;
			}
		};

		EnemyGraphSink g_enemyGraphSink;

		// Parry for All marks a parry by placing ParryAll.esp explosions (Normal / Perfect, shield or
		// weapon, player or NPC variants) and a ParryMarker activator. Explosions in general are kept
		// too, so other hit-effect mods show up. Only references near the player are logged.
		bool IsWatchedPlacement(const RE::TESObjectREFR* a_ref)
		{
			const auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
			if (!base) {
				return false;
			}
			if (base->GetFormType() == RE::FormType::Explosion) {
				return true;
			}
			const auto* file = base->GetFile(0);
			if (!file) {
				return false;
			}
			// Called for every reference that loads; compare without allocating.
			constexpr std::string_view key = "parry";
			const std::string_view     name = file->GetFilename();
			const auto                 lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
			return std::ranges::search(name, key, {}, lower, {}).begin() != name.end();
		}

		constexpr float kPlacementRadius = 3000.0f;

		// --- handlers -------------------------------------------------------------------------

		void OnQuestStartStop(const RE::TESQuestStartStopEvent& e)
		{
			Line("quest.startstop").str("quest", forms::Key(e.formID)).str("name", QuestName(e.formID))
				.flag("started", e.started).flag("failed", e.failed).emit();
		}

		void OnQuestStatus(const RE::QuestStatus::Event& e)
		{
			const auto* name = e.quest ? e.quest->GetName() : nullptr;
			Line("quest.status").str("quest", forms::Key(e.quest)).str("name", name ? name : "")
				.num("status", static_cast<std::int64_t>(e.status)).emit();
		}

		void OnQuestStage(const RE::TESQuestStageEvent& e)
		{
			Line("quest.stage").str("quest", forms::Key(e.formID)).str("name", QuestName(e.formID))
				.num("stage", static_cast<std::uint64_t>(e.stage)).num("item", static_cast<std::uint64_t>(e.itemIndex)).emit();
		}

		void OnObjective(const RE::ObjectiveState::Event& e)
		{
			const auto* obj = e.objective;
			Line l("quest.objective");
			if (obj) {
				const auto* quest = obj->ownerQuest;
				const char* qname = quest ? quest->GetName() : nullptr;
				l.str("quest", forms::Key(quest)).str("name", qname ? qname : "")
					.num("index", static_cast<std::uint64_t>(obj->index))
					.str("text", obj->displayText.c_str() ? obj->displayText.c_str() : "");
			}
			l.num("old", static_cast<std::int64_t>(e.oldState)).num("new", static_cast<std::int64_t>(e.newState)).emit();
		}

		void OnLocationChange(const RE::TESActorLocationChangeEvent& e)
		{
			if (!forms::IsPlayer(e.actor.get())) {
				return;
			}
			Line("location.change").raw("old", forms::Json(e.oldLoc)).raw("new", forms::Json(e.newLoc)).emit();
		}

		void OnDiscovery(const RE::LocationDiscovery::Event& e)
		{
			const char* name = e.mapMarkerData ? e.mapMarkerData->locationName.GetFullName() : nullptr;
			Line("location.discovered").str("name", name ? name : "").str("worldspace", e.worldspaceID ? e.worldspaceID : "").emit();
		}

		void OnCleared(const RE::LocationCleared::Event&)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			Line("location.cleared").raw("playerLocation", forms::Json(player ? player->GetCurrentLocation() : nullptr)).emit();
		}

		void OnCombat(const RE::TESCombatEvent& e)
		{
			if (!forms::IsPlayerSide(e.actor.get()) && !forms::IsPlayerSide(e.targetActor.get())) {
				return;
			}
			Line("combat.state").raw("actor", forms::Json(e.actor.get())).raw("target", forms::Json(e.targetActor.get()))
				.num("state", static_cast<std::int64_t>(e.newState.get())).emit();

			// An opponent of the player side: watch its graph (attached later by the poll task).
			const auto* actor = e.actor.get();
			if (actor && e.newState.get() != RE::ACTOR_COMBAT_STATE::kNone &&
				!forms::IsPlayerSide(actor) && forms::IsPlayerSide(e.targetActor.get())) {
				std::scoped_lock lock(g_enemyLock);
				g_enemyPending.insert(actor->GetFormID());
			}
		}

		void LogPlacement(const char* a_event, const RE::TESObjectREFR* a_ref, bool a_flag, const char* a_flagName)
		{
			if (!IsWatchedPlacement(a_ref)) {
				return;
			}
			const float dist = forms::DistanceToPlayer(a_ref);
			if (dist < 0.0f || dist > kPlacementRadius) {
				return;
			}
			Line(a_event).raw("ref", forms::Json(a_ref)).flag(a_flagName, a_flag).num("dist", static_cast<double>(dist)).emit();
		}

		void OnObjectLoaded(const RE::TESObjectLoadedEvent& e)
		{
			LogPlacement("object.loaded", RE::TESForm::LookupByID<RE::TESObjectREFR>(e.formID), e.loaded, "loaded");
		}

		void OnMoveAttach(const RE::TESMoveAttachDetachEvent& e)
		{
			LogPlacement("object.attach", e.movedRef.get(), e.isCellAttached, "attached");
		}

		void OnDeath(const RE::TESDeathEvent& e)
		{
			const auto* dying = e.actorDying.get();
			const auto* actor = dying ? dying->As<RE::Actor>() : nullptr;
			const auto* npc = actor ? actor->GetActorBase() : nullptr;
			Line("death").raw("dying", forms::Json(dying)).raw("killer", forms::Json(e.actorKiller.get()))
				.flag("dead", e.dead).flag("unique", npc && npc->IsUnique())
				.flag("killMove", actor && actor->IsInKillMove())
				.num("dist", static_cast<double>(forms::DistanceToPlayer(dying))).emit();
		}

		void OnBleedout(const RE::TESEnterBleedoutEvent& e)
		{
			if (!forms::IsPlayerSide(e.actor.get())) {
				return;
			}
			Line("bleedout.enter").raw("actor", forms::Json(e.actor.get())).emit();
		}

		void OnHit(const RE::TESHitEvent& e)
		{
			if (!forms::IsPlayerSide(e.target.get()) && !forms::IsPlayerSide(e.cause.get())) {
				return;
			}
			using F = RE::TESHitEvent::Flag;
			Line("hit").raw("target", forms::Json(e.target.get())).raw("cause", forms::Json(e.cause.get()))
				.str("source", forms::Key(e.source)).str("projectile", forms::Key(e.projectile))
				.flag("power", e.flags.any(F::kPowerAttack)).flag("sneak", e.flags.any(F::kSneakAttack))
				.flag("bash", e.flags.any(F::kBashAttack)).flag("blocked", e.flags.any(F::kHitBlocked)).emit();
		}

		void OnSpellCast(const RE::TESSpellCastEvent& e)
		{
			if (!forms::IsPlayerSide(e.object.get())) {
				return;
			}
			Line("spell.cast").raw("caster", forms::Json(e.object.get())).str("spell", forms::Key(e.spell)).emit();
		}

		void OnEffectApply(const RE::TESMagicEffectApplyEvent& e)
		{
			if (!forms::IsPlayer(e.target.get()) && !forms::IsPlayer(e.caster.get())) {
				return;
			}
			{
				// Cloak and aura spells reapply the same effect every second; log one per 2 s per pair.
				const auto key = std::format("{:X}/{:X}", e.target ? e.target->GetFormID() : 0u, e.magicEffect);
				const double now = out::ElapsedMs();
				std::scoped_lock lock(g_effectLock);
				auto [it, inserted] = g_effectLastMs.try_emplace(key, now);
				if (!inserted) {
					if (now - it->second < 2000.0) {
						++g_effectSuppressed;
						return;
					}
					it->second = now;
				}
				if (g_effectLastMs.size() > 4096) {
					g_effectLastMs.clear();
				}
			}
			Line("effect.apply").raw("target", forms::Json(e.target.get())).raw("caster", forms::Json(e.caster.get()))
				.str("effect", forms::Key(e.magicEffect)).emit();
		}

		void OnEquip(const RE::TESEquipEvent& e)
		{
			if (!forms::IsPlayerSide(e.actor.get())) {
				return;
			}
			world::NoteEquipChange(e.actor ? e.actor->GetFormID() : 0);
			const auto* form = RE::TESForm::LookupByID(e.baseObject);
			Line("equip").raw("actor", forms::Json(e.actor.get())).raw("item", forms::Json(form))
				.num("type", static_cast<std::uint64_t>(form ? form->GetFormType() : RE::FormType::None))
				.flag("equipped", e.equipped).emit();
		}

		void OnContainer(const RE::TESContainerChangedEvent& e)
		{
			const auto playerId = RE::PlayerCharacter::GetSingleton() ? RE::PlayerCharacter::GetSingleton()->GetFormID() : 0;
			if (e.oldContainer != playerId && e.newContainer != playerId) {
				return;
			}
			const auto* item = RE::TESForm::LookupByID(e.baseObj);
			Line("container").str("from", forms::Key(e.oldContainer)).str("to", forms::Key(e.newContainer))
				.raw("item", forms::Json(item)).num("count", static_cast<std::int64_t>(e.itemCount)).emit();
		}

		void OnActivate(const RE::TESActivateEvent& e)
		{
			if (!forms::IsPlayer(e.actionRef.get())) {
				return;
			}
			Line("activate").raw("object", forms::Json(e.objectActivated.get())).emit();
		}

		void OnFurniture(const RE::TESFurnitureEvent& e)
		{
			if (!forms::IsPlayer(e.actor.get())) {
				return;
			}
			Line("furniture").raw("furniture", forms::Json(e.targetFurniture.get()))
				.num("type", static_cast<std::int64_t>(e.type.get())).emit();
		}

		void OnLock(const RE::TESLockChangedEvent& e)
		{
			Line("lock.changed").raw("object", forms::Json(e.lockedObject.get()))
				.num("dist", static_cast<double>(forms::DistanceToPlayer(e.lockedObject.get()))).emit();
		}

		void OnSleepStop(const RE::TESSleepStopEvent& e) { Line("sleep.stop").flag("interrupted", e.interrupted).emit(); }
		void OnWaitStop(const RE::TESWaitStopEvent& e) { Line("wait.stop").flag("interrupted", e.interrupted).emit(); }

		void OnTrackedStat(const RE::TESTrackedStatsEvent& e)
		{
			Line("stat").str("name", e.stat.c_str() ? e.stat.c_str() : "").num("value", static_cast<std::int64_t>(e.value)).emit();
		}

		void OnTopicInfo(const RE::TESTopicInfoEvent& e)
		{
			const float dist = forms::DistanceToPlayer(e.speakerRef.get());
			if (dist < 0.0f || dist > 2000.0f) {
				return;
			}
			Line("topic").raw("speaker", forms::Json(e.speakerRef.get())).str("info", forms::Key(e.topicInfoFormID))
				.num("type", static_cast<std::int64_t>(e.type.get())).num("dist", static_cast<double>(dist)).emit();
		}

		void OnSwitchRace(const RE::TESSwitchRaceCompleteEvent& e)
		{
			if (!forms::IsPlayerSide(e.subject.get())) {
				return;
			}
			const auto* actor = e.subject ? e.subject->As<RE::Actor>() : nullptr;
			Line("race.switch").raw("actor", forms::Json(e.subject.get())).raw("race", forms::Json(actor ? actor->GetRace() : nullptr)).emit();
			if (forms::IsPlayer(e.subject.get())) {
				// Transformation swaps the player's behavior graph; the old sink no longer receives events.
				g_graphWanted.store(true);
			}
		}

		void OnBowShot(const RE::TESPlayerBowShotEvent& e)
		{
			Line("bow.shot").str("weapon", forms::Key(e.weapon)).str("ammo", forms::Key(e.ammo))
				.num("power", static_cast<double>(e.shotPower)).emit();
		}

		void OnLevel(const RE::LevelIncrease::Event& e) { Line("level.up").num("level", static_cast<std::uint64_t>(e.newLevel)).emit(); }

		void OnSkill(const RE::SkillIncrease::Event& e)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			const float value = player ? player->AsActorValueOwner()->GetBaseActorValue(e.actorValue) : -1.0f;
			Line("skill.up").num("av", static_cast<std::int64_t>(e.actorValue)).num("base", static_cast<double>(value)).emit();
		}

		void OnBooksRead(const RE::BooksRead::Event& e) { Line("book.read").raw("book", forms::Json(e.book)).flag("skillBook", e.skillBook).emit(); }

		void OnBookRead(const RE::TESBookReadEvent& e)
		{
			Line("book.readRef").raw("ref", forms::Json(e.ref.get())).str("base", forms::Key(e.baseFormID)).emit();
		}

		void OnSouls(const RE::DragonSoulsGained::Event& e) { Line("dragon.souls").num("souls", static_cast<double>(e.souls)).emit(); }
		void OnSpellLearned(const RE::SpellsLearned::Event& e) { Line("spell.learned").raw("spell", forms::Json(e.spell)).emit(); }

		void OnCrafted(const RE::ItemCrafted::Event& e)
		{
			Line("crafted").raw("item", forms::Json(e.item)).flag("f08", e.unk08).flag("f09", e.unk09).flag("f0A", e.unk0A).emit();
		}

		void OnHarvest(const RE::TESHarvestedEvent::ItemHarvested& e)
		{
			if (!forms::IsPlayer(e.harvester)) {
				return;
			}
			Line("harvest").raw("item", forms::Json(e.produceItem)).emit();
		}

		void OnPickpocket(const RE::ItemsPickpocketed::Event& e) { Line("pickpocket").num("items", static_cast<std::int64_t>(e.numItems)).emit(); }
		void OnChestLooted(const RE::ChestsLooted::Event&) { Line("chest.looted").emit(); }
		void OnLocksPicked(const RE::LocksPicked::Event&) { Line("lock.picked").emit(); }

		void OnSoulTrap(const RE::SoulsTrapped::Event& e)
		{
			Line("soul.trapped").raw("trapper", forms::Json(e.trapper)).raw("target", forms::Json(e.target)).emit();
		}

		void OnCritical(const RE::CriticalHit::Event& e)
		{
			if (!forms::IsPlayerSide(e.aggressor)) {
				return;
			}
			Line("crit").raw("aggressor", forms::Json(e.aggressor)).raw("weapon", forms::Json(e.weapon)).flag("sneak", e.sneakHit).emit();
		}

		void OnDisarm(const RE::DisarmedEvent::Event& e)
		{
			if (!forms::IsPlayerSide(e.source) && !forms::IsPlayerSide(e.target)) {
				return;
			}
			Line("disarm").raw("source", forms::Json(e.source)).raw("target", forms::Json(e.target)).emit();
		}

		void OnShout(const RE::ShoutAttack::Event& e) { Line("shout").raw("shout", forms::Json(e.shout)).emit(); }

		void OnActorKill(const RE::ActorKill::Event& e)
		{
			Line("actor.kill").raw("killer", forms::Json(e.killer)).raw("victim", forms::Json(e.victim))
				.flag("victimKillMove", e.victim && e.victim->IsInKillMove()).emit();
		}

		void OnMenu(const RE::MenuOpenCloseEvent& e)
		{
			Line("menu").str("name", e.menuName.c_str() ? e.menuName.c_str() : "").flag("opening", e.opening).emit();
		}

		void OnAction(const SKSE::ActionEvent& e)
		{
			if (!forms::IsPlayerSide(e.actor)) {
				return;
			}
			Line("action").raw("actor", forms::Json(e.actor)).num("type", static_cast<std::int64_t>(e.type.get()))
				.num("slot", static_cast<std::int64_t>(e.slot.get())).raw("source", forms::Json(e.sourceForm)).emit();
		}

		void OnModEvent(const SKSE::ModCallbackEvent& e)
		{
			const std::string_view name = e.eventName.c_str() ? e.eventName.c_str() : "";
			bool first = false;
			{
				std::scoped_lock lock(g_countLock);
				first = Count(g_modEventCounts, name);
			}
			const auto* senderRef = e.sender ? e.sender->As<RE::TESObjectREFR>() : nullptr;
			// Always log the first of each name; later ones only when the player side is involved.
			if (!first && !forms::IsPlayerSide(senderRef)) {
				return;
			}
			Line("modevent").str("name", name).str("str", e.strArg.c_str() ? e.strArg.c_str() : "")
				.num("num", static_cast<double>(e.numArg)).raw("sender", forms::Json(e.sender)).flag("first", first).emit();
		}
	}

	void RegisterGlobal()
	{
		AddToHolder<RE::TESQuestStartStopEvent>("quest.startstop", OnQuestStartStop);
		AddToHolder<RE::TESQuestStageEvent>("quest.stage", OnQuestStage);
		AddToHolder<RE::TESActorLocationChangeEvent>("location.change", OnLocationChange);
		AddToHolder<RE::TESCombatEvent>("combat.state", OnCombat);
		AddToHolder<RE::TESDeathEvent>("death", OnDeath);
		AddToHolder<RE::TESEnterBleedoutEvent>("bleedout.enter", OnBleedout);
		AddToHolder<RE::TESHitEvent>("hit", OnHit);
		AddToHolder<RE::TESSpellCastEvent>("spell.cast", OnSpellCast);
		AddToHolder<RE::TESMagicEffectApplyEvent>("effect.apply", OnEffectApply);
		AddToHolder<RE::TESEquipEvent>("equip", OnEquip);
		AddToHolder<RE::TESContainerChangedEvent>("container", OnContainer);
		AddToHolder<RE::TESActivateEvent>("activate", OnActivate);
		AddToHolder<RE::TESFurnitureEvent>("furniture", OnFurniture);
		AddToHolder<RE::TESLockChangedEvent>("lock.changed", OnLock);
		AddToHolder<RE::TESSleepStopEvent>("sleep.stop", OnSleepStop);
		AddToHolder<RE::TESWaitStopEvent>("wait.stop", OnWaitStop);
		AddToHolder<RE::TESTrackedStatsEvent>("stat", OnTrackedStat);
		AddToHolder<RE::TESTopicInfoEvent>("topic", OnTopicInfo);
		AddToHolder<RE::TESSwitchRaceCompleteEvent>("race.switch", OnSwitchRace);
		AddToHolder<RE::TESPlayerBowShotEvent>("bow.shot", OnBowShot);
		AddToHolder<RE::TESBookReadEvent>("book.readRef", OnBookRead);
		AddToHolder<RE::TESObjectLoadedEvent>("object.loaded", OnObjectLoaded);
		AddToHolder<RE::TESMoveAttachDetachEvent>("object.attach", OnMoveAttach);

		AddToSource<RE::QuestStatus::Event>("quest.status", RE::QuestStatus::GetEventSource(), OnQuestStatus);
		AddToSource<RE::ObjectiveState::Event>("quest.objective", RE::ObjectiveState::GetEventSource(), OnObjective);
		AddToSource<RE::LocationDiscovery::Event>("location.discovered", RE::LocationDiscovery::GetEventSource(), OnDiscovery);
		AddToSource<RE::LocationCleared::Event>("location.cleared", RE::LocationCleared::GetEventSource(), OnCleared);
		AddToSource<RE::LevelIncrease::Event>("level.up", RE::LevelIncrease::GetEventSource(), OnLevel);
		AddToSource<RE::SkillIncrease::Event>("skill.up", RE::SkillIncrease::GetEventSource(), OnSkill);
		AddToSource<RE::BooksRead::Event>("book.read", RE::BooksRead::GetEventSource(), OnBooksRead);
		AddToSource<RE::DragonSoulsGained::Event>("dragon.souls", RE::DragonSoulsGained::GetEventSource(), OnSouls);
		AddToSource<RE::SpellsLearned::Event>("spell.learned", RE::SpellsLearned::GetEventSource(), OnSpellLearned);
		AddToSource<RE::ItemCrafted::Event>("crafted", RE::ItemCrafted::GetEventSource(), OnCrafted);
		AddToSource<RE::TESHarvestedEvent::ItemHarvested>("harvest", RE::TESHarvestedEvent::GetEventSource(), OnHarvest);
		AddToSource<RE::ItemsPickpocketed::Event>("pickpocket", RE::ItemsPickpocketed::GetEventSource(), OnPickpocket);
		AddToSource<RE::ChestsLooted::Event>("chest.looted", RE::ChestsLooted::GetEventSource(), OnChestLooted);
		AddToSource<RE::LocksPicked::Event>("lock.picked", RE::LocksPicked::QEventSource(), OnLocksPicked);
		AddToSource<RE::SoulsTrapped::Event>("soul.trapped", RE::SoulsTrapped::GetEventSource(), OnSoulTrap);
		AddToSource<RE::CriticalHit::Event>("crit", RE::CriticalHit::GetEventSource(), OnCritical);
		AddToSource<RE::DisarmedEvent::Event>("disarm", RE::DisarmedEvent::GetEventSource(), OnDisarm);
		AddToSource<RE::ShoutAttack::Event>("shout", RE::ShoutAttack::GetEventSource(), OnShout);
		AddToSource<RE::ActorKill::Event>("actor.kill", RE::ActorKill::GetEventSource(), OnActorKill);

		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(Make<RE::MenuOpenCloseEvent>("menu", OnMenu));
			Line("probe.sink").str("name", "menu").str("via", "ui").emit();
		}
		AddToSource<SKSE::ActionEvent>("action", SKSE::GetActionEventSource(), OnAction);
		AddToSource<SKSE::ModCallbackEvent>("modevent", SKSE::GetModCallbackEventSource(), OnModEvent);
	}

	void AttachPlayerGraph()
	{
		g_graphWanted.store(true);
		EnsurePlayerGraph();
	}

	void EnsurePlayerGraph()
	{
		std::scoped_lock lock(g_graphLock);
		// Periodic refresh covers graph rebuilds we get no event for (3D reload, some mods).
		const bool periodic = (++g_graphRefreshTicks % 60) == 0;
		if (!g_graphWanted.load() && g_graphAttached.load() && !periodic) {
			return;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->Is3DLoaded()) {
			g_graphAttached.store(false);
			return;  // retried on the next poll
		}
		// Remove first so the same sink is never registered twice.
		player->RemoveAnimationGraphEventSink(&g_playerGraphSink);
		const bool ok = player->AddAnimationGraphEventSink(&g_playerGraphSink);
		const bool wasWanted = g_graphWanted.exchange(false);
		const bool changed = g_graphAttached.exchange(ok) != ok;
		if (wasWanted || changed) {
			Line("probe.anim.attach").flag("ok", ok).flag("periodic", periodic && !wasWanted).emit();
		}
		if (!ok) {
			g_graphWanted.store(true);
		}
	}

	void EnsureEnemyGraphs()
	{
		std::scoped_lock lock(g_enemyLock);

		// Drop opponents that left the fight. A graph destroyed with its 3D takes our sink with it,
		// so removing from a reloaded graph is only a no-op, never a dangling call.
		for (auto it = g_enemyAttached.begin(); it != g_enemyAttached.end();) {
			auto*      actor = RE::TESForm::LookupByID<RE::Actor>(*it);
			const bool keep = actor && actor->Is3DLoaded() && !actor->IsDead() && actor->IsInCombat();
			if (keep) {
				++it;
				continue;
			}
			if (actor) {
				actor->RemoveAnimationGraphEventSink(&g_enemyGraphSink);
			}
			Line("probe.enemyGraph").str("op", "detach").str("id", std::format("{:08X}", *it)).raw("actor", forms::Json(actor)).emit();
			it = g_enemyAttached.erase(it);
		}

		for (const auto id : g_enemyPending) {
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
			if (!actor || !actor->Is3DLoaded() || actor->IsDead()) {
				continue;
			}
			const bool known = g_enemyAttached.contains(id);
			if (!known && g_enemyAttached.size() >= kMaxEnemyGraphs) {
				if (!g_enemyFullLogged) {
					Line("probe.enemyGraph").str("op", "full").num("max", static_cast<std::uint64_t>(kMaxEnemyGraphs)).emit();
					g_enemyFullLogged = true;
				}
				continue;
			}
			// Remove first: a repeated combat event (searching -> combat) must not register twice.
			actor->RemoveAnimationGraphEventSink(&g_enemyGraphSink);
			const bool ok = actor->AddAnimationGraphEventSink(&g_enemyGraphSink);
			if (ok) {
				g_enemyAttached.insert(id);
			}
			if (!known || !ok) {
				Line("probe.enemyGraph").str("op", "attach").str("id", std::format("{:08X}", id)).raw("actor", forms::Json(actor))
					.flag("ok", ok).emit();
			}
		}
		g_enemyPending.clear();
		if (g_enemyAttached.size() < kMaxEnemyGraphs) {
			g_enemyFullLogged = false;
		}
	}

	void ResetEnemyGraphs()
	{
		// After a load every old actor and graph is gone; the IDs alone could match new references.
		std::scoped_lock lock(g_enemyLock);
		g_enemyPending.clear();
		g_enemyAttached.clear();
		g_enemyFullLogged = false;
	}

	void DumpCounters()
	{
		std::uint64_t suppressed = 0;
		{
			std::scoped_lock lock(g_effectLock);
			suppressed = g_effectSuppressed;
			g_effectSuppressed = 0;
		}
		const auto toJson = [](const CountMap& a_map) {
			std::string json = "{";
			for (const auto& [name, n] : a_map) {
				json += std::format("{}\"{}\":{}", json.size() > 1 ? "," : "", Escape(name), n);
			}
			return json + "}";
		};
		std::string anim;
		std::string enemyAnim;
		std::string mods;
		{
			std::scoped_lock lock(g_countLock);
			anim = toJson(g_animCounts);
			enemyAnim = toJson(g_enemyAnimCounts);
			mods = toJson(g_modEventCounts);
		}
		Line("counts").raw("animTags", anim).raw("enemyAnimTags", enemyAnim).raw("modEvents", mods)
			.num("effectApplySuppressed", suppressed).emit();
	}
}
