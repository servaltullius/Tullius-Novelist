#include "World.h"
#include "Forms.h"
#include "Out.h"

namespace probe::world
{
	namespace
	{
		// An outfit swap is a burst of equip events; wait for it to settle before reading, but
		// never longer than the cap (a mod that keeps re-equipping would otherwise delay forever).
		constexpr double kEquipSettleMs = 1500.0;
		constexpr double kEquipMaxWaitMs = 5000.0;

		struct State
		{
			bool         timeValid{ false };
			std::int64_t hourKey{ 0 };  // calendar date and whole hour; changes when the clock passes an hour
			float        daysPassed{ 0.0f };

			bool       weatherValid{ false };
			RE::FormID weather{ 0 };

			bool       spaceValid{ false };
			bool       interior{ false };
			RE::FormID space{ 0 };  // the interior cell, or the worldspace when outside
		};

		struct Pending
		{
			double due{ 0.0 };
			double first{ 0.0 };  // when the actor first became pending; caps the settle delay
		};

		// Sample runs as an SKSE task and Reset at a game start. The stage 0 records show the task on
		// other threads than the one that delivers SKSE messages (1,391 of 1,419 poll lines), so the
		// state is locked rather than assumed to stay on one thread.
		std::mutex g_stateLock;
		State      g_state;

		// Equip notes come from event threads, so these are locked.
		std::mutex                                  g_lock;
		std::unordered_map<RE::FormID, Pending>     g_snapshotDue;   // actor -> when to read
		std::unordered_map<RE::FormID, std::string> g_lastSnapshot;  // actor -> last written content

		std::string_view Text(const char* a_text)
		{
			return a_text ? std::string_view(a_text) : std::string_view();
		}

		void SampleTime(State& a_state)
		{
			const auto* calendar = RE::Calendar::GetSingleton();
			if (!calendar) {
				return;
			}
			const float days = calendar->GetDaysPassed();
			const auto  year = static_cast<std::int64_t>(calendar->GetYear());
			const auto  month = static_cast<std::int64_t>(calendar->GetMonth());  // 0 = Morning Star
			const auto  day = static_cast<std::int64_t>(calendar->GetDay());
			const float rawHour = calendar->GetHour();
			const auto  hourKey = ((year * 12 + month) * 32 + day) * 24 + static_cast<std::int64_t>(std::floor(rawHour));
			if (a_state.timeValid && hourKey == a_state.hourKey) {
				return;
			}
			const double hour = std::round(static_cast<double>(rawHour) * 100.0) / 100.0;
			Line line("world.time");
			line.num("year", year)
				.num("month", month)
				.str("monthName", calendar->GetMonthName())
				.num("day", day)
				.num("hour", hour)
				.num("daysPassed", static_cast<double>(days))
				.num("timescale", static_cast<double>(calendar->GetTimescale()));
			// Large jumps mean fast travel or a carriage ride. Sleeping or waiting may show as
			// several lines a few hours apart (the clock runs while the menu is open); read them
			// together with sleep.stop / wait.stop.
			if (a_state.timeValid) {
				line.num("deltaHours", std::round((static_cast<double>(days) - a_state.daysPassed) * 2400.0) / 100.0);
			}
			line.flag("baseline", !a_state.timeValid).emit();
			a_state.timeValid = true;
			a_state.hourKey = hourKey;
			a_state.daysPassed = days;
		}

		std::string WeatherKinds(const RE::TESWeather* a_weather)
		{
			using F = RE::TESWeather::WeatherDataFlag;
			std::string kinds;
			const auto add = [&](F a_flag, std::string_view a_name) {
				if (a_weather->data.flags.any(a_flag)) {
					if (!kinds.empty()) {
						kinds += ',';
					}
					kinds += a_name;
				}
			};
			add(F::kPleasant, "pleasant");
			add(F::kCloudy, "cloudy");
			add(F::kRainy, "rainy");
			add(F::kSnow, "snow");
			add(F::kPermAurora, "aurora");
			return kinds;
		}

		const char* SkyModeName(RE::Sky::Mode a_mode)
		{
			switch (a_mode) {
			case RE::Sky::Mode::kNone:
				return "none";
			case RE::Sky::Mode::kInterior:
				return "interior";
			case RE::Sky::Mode::kSkyDomeOnly:
				return "skyDomeOnly";
			case RE::Sky::Mode::kFull:
				return "full";
			default:
				return "other";
			}
		}

		void SampleWeather(State& a_state)
		{
			const auto* sky = RE::Sky::GetSingleton();
			if (!sky) {
				return;
			}
			const auto*      weather = sky->currentWeather;
			const RE::FormID id = weather ? weather->GetFormID() : 0;
			if (a_state.weatherValid && id == a_state.weather) {
				return;
			}
			Line line("weather.change");
			line.str("weather", forms::Key(weather))
				.str("editorId", weather ? Text(weather->GetFormEditorID()) : std::string_view())
				.str("kinds", weather ? WeatherKinds(weather) : std::string())
				.str("skyMode", SkyModeName(sky->mode.get()));
			if (a_state.weatherValid) {
				line.str("previous", forms::Key(a_state.weather));
			}
			line.flag("baseline", !a_state.weatherValid).emit();
			a_state.weatherValid = true;
			a_state.weather = id;
		}

		void SampleSpace(State& a_state, RE::PlayerCharacter* a_player)
		{
			auto* cell = a_player->GetParentCell();
			if (!cell) {
				return;
			}
			const bool  interior = cell->IsInteriorCell();
			auto*       worldspace = a_player->GetWorldspace();
			const auto* space = interior ? static_cast<const RE::TESForm*>(cell) : static_cast<const RE::TESForm*>(worldspace);
			const RE::FormID id = space ? space->GetFormID() : 0;
			// Outside, only a worldspace change counts: exterior cells change every few steps
			// and rarely have names. Location changes are already logged as location.change.
			if (a_state.spaceValid && interior == a_state.interior && id == a_state.space) {
				return;
			}
			Line("cell.change")
				.flag("interior", interior)
				.raw("cell", forms::Json(cell))
				.str("cellEditorId", Text(cell->GetFormEditorID()))
				.raw("worldspace", forms::Json(worldspace))
				.raw("location", forms::Json(a_player->GetCurrentLocation()))
				.flag("baseline", !a_state.spaceValid)
				.emit();
			a_state.spaceValid = true;
			a_state.interior = interior;
			a_state.space = id;
		}

		// Worn items (armor, weapons, ammo, torch) as a JSON array, sorted by form key so the same
		// outfit always produces the same text.
		std::string WornJson(RE::Actor* a_actor)
		{
			// noInit: read what exists; never create inventory changes on an actor.
			auto inventory = a_actor->GetInventory([](RE::TESBoundObject& a_object) {
				return a_object.IsArmor() || a_object.IsWeapon() || a_object.IsAmmo() || a_object.Is(RE::FormType::Light);
			},
				true);

			std::vector<std::pair<std::string, std::string>> items;
			for (auto& [object, data] : inventory) {
				auto& [count, entry] = data;
				if (count <= 0 || !entry || !entry->extraLists) {
					continue;
				}
				// The worn instance is the extra list that carries the worn mark. Its name and
				// temper are read only if they already exist: InventoryEntryData::GetDisplayName
				// would add display data to the item, and the probe must not change the game.
				bool        right = false;
				bool        left = false;
				std::string shown;
				float       health = 1.0f;
				for (auto* xList : *entry->extraLists) {
					if (!xList) {
						continue;
					}
					const bool wornRight = xList->HasType<RE::ExtraWorn>();
					const bool wornLeft = xList->HasType<RE::ExtraWornLeft>();
					if (!wornRight && !wornLeft) {
						continue;
					}
					right = right || wornRight;
					left = left || wornLeft;
					if (shown.empty()) {
						if (const auto* text = xList->GetByType<RE::ExtraTextDisplayData>(); text && !text->displayName.empty()) {
							shown = text->displayName.c_str();
						}
					}
					if (const auto* xHealth = xList->GetByType<RE::ExtraHealth>()) {
						health = xHealth->health;
					}
				}
				if (!right && !left) {
					continue;
				}
				const auto  key = forms::Key(object);
				const char* baseName = object->GetName();
				std::string item = std::format(R"({{"key":"{}","name":"{}")", Escape(key), Escape(Text(baseName)));
				// Renamed, player-enchanted or tempered items carry their own name.
				if (!shown.empty() && shown != Text(baseName)) {
					item += std::format(R"(,"shown":"{}")", Escape(shown));
				}
				// Temper level as the game stores it (1.0 = none, 1.1 = fine, ...).
				if (std::fabs(health - 1.0f) >= 0.001f) {
					item += std::format(R"(,"health":{})", std::round(static_cast<double>(health) * 1000.0) / 1000.0);
				}
				item += std::format(R"(,"type":{})", static_cast<std::uint32_t>(object->GetFormType()));
				if (const auto* armor = object->As<RE::TESObjectARMO>()) {
					item += std::format(R"(,"slots":"{:08X}")", armor->GetSlotMask().underlying());
				}
				if (left) {
					item += right ? R"(,"hand":"both")" : R"(,"hand":"left")";
				}
				// Armor keeps its non-playable mark in the record flags (TESObjectARMO does not
				// override GetPlayable, and the base reading of that bit is not trustworthy); weapons
				// and ammo override GetPlayable. Other worn things (torches) are not marked.
				bool playable = true;
				if (object->Is(RE::FormType::Armor)) {
					playable = (object->GetFormFlags() & RE::TESObjectARMO::RecordFlags::kNonPlayable) == 0;
				} else if (object->IsWeapon() || object->IsAmmo()) {
					playable = object->GetPlayable();
				}
				if (!playable) {
					item += R"(,"playable":false)";
				}
				item += '}';
				items.emplace_back(key, std::move(item));
			}
			std::sort(items.begin(), items.end());

			std::string json = "[";
			for (std::size_t i = 0; i < items.size(); ++i) {
				if (i > 0) {
					json += ',';
				}
				json += items[i].second;
			}
			json += ']';
			return json;
		}

		void Snapshot(RE::Actor* a_actor)
		{
			// Hands: spells, weapons, shield or torch as the game holds them; power: selected
			// shout or power.
			const auto* right = a_actor->GetEquippedObject(false);
			const auto* left = a_actor->GetEquippedObject(true);
			const auto* power = a_actor->GetActorRuntimeData().selectedPower;
			std::string content = std::format(R"("rightHand":{},"leftHand":{},"power":{},"worn":{})",
				forms::Json(right), forms::Json(left), forms::Json(power), WornJson(a_actor));

			bool baseline = false;
			{
				std::scoped_lock lock(g_lock);
				auto it = g_lastSnapshot.find(a_actor->GetFormID());
				if (it != g_lastSnapshot.end() && it->second == content) {
					return;
				}
				baseline = (it == g_lastSnapshot.end());
				g_lastSnapshot[a_actor->GetFormID()] = content;
			}
			Line("equip.snapshot")
				.raw("actor", forms::Json(static_cast<const RE::TESObjectREFR*>(a_actor)))
				.flag("baseline", baseline)
				.raw("state", "{" + content + "}")
				.emit();
		}

		void SampleSnapshots()
		{
			const double            now = out::ElapsedMs();
			std::vector<RE::FormID> due;
			{
				std::scoped_lock lock(g_lock);
				for (auto it = g_snapshotDue.begin(); it != g_snapshotDue.end();) {
					if (it->second.due <= now) {
						due.push_back(it->first);
						it = g_snapshotDue.erase(it);
					} else {
						++it;
					}
				}
			}
			for (const auto id : due) {
				auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
				// Only actors in the scene: an unloaded follower's gear is not part of the chapter.
				if (actor && actor->Is3DLoaded()) {
					Snapshot(actor);
				}
			}
		}
	}

	void Sample(RE::PlayerCharacter* a_player)
	{
		if (!a_player) {
			return;
		}
		perf::Scope scope("world.sample");
		{
			std::scoped_lock lock(g_stateLock);
			SampleTime(g_state);
			SampleWeather(g_state);
			SampleSpace(g_state, a_player);
		}
		// The player's outfit is the chapter's starting point. Counting the settle time from the
		// first loaded sample (not from the load message) skips the re-equip burst that follows
		// the loading screen.
		EnsureBaseline(a_player->GetFormID());
		SampleSnapshots();
	}

	void NoteEquipChange(RE::FormID a_actor)
	{
		if (a_actor == 0) {
			return;
		}
		const double now = out::ElapsedMs();
		std::scoped_lock lock(g_lock);
		auto [it, added] = g_snapshotDue.try_emplace(a_actor, Pending{ now, now });
		it->second.due = std::min(now + kEquipSettleMs, it->second.first + kEquipMaxWaitMs);
	}

	void EnsureBaseline(RE::FormID a_actor)
	{
		if (a_actor == 0) {
			return;
		}
		const double now = out::ElapsedMs();
		std::scoped_lock lock(g_lock);
		if (g_lastSnapshot.contains(a_actor)) {
			return;
		}
		// An equip burst that is already settling keeps its own due time.
		g_snapshotDue.try_emplace(a_actor, Pending{ now + kEquipSettleMs, now });
	}

	void Reset()
	{
		{
			std::scoped_lock lock(g_stateLock);
			g_state = State{};
		}
		std::scoped_lock lock(g_lock);
		g_snapshotDue.clear();
		g_lastSnapshot.clear();
	}
}
