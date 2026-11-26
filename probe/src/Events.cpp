#include "Events.h"
#include "Forms.h"
#include "Out.h"
#include "Text.h"
#include "World.h"

#include <set>
#include <tuple>

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

		// Mod events with a broken name: logged up to this many per run, the rest only counted.
		constexpr std::uint64_t    kMaxBadModEventLines = 100;
		std::atomic<std::uint64_t> g_badModEventLines{ 0 };
		std::atomic<std::uint64_t> g_badModEventsSkipped{ 0 };

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
				// Tag and payload are read in place, not copied like EventText: they are the
				// behavior graph's own strings, held by the graph while it runs, and this runs for
				// every animation event (player and enemy graphs), where a checked copy per string
				// would cost more than the rest of the callback.
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
				// Read in place, as in PlayerGraphSink.
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

		// For pointers taken from event structs whose layout has not been seen working on this runtime.
		// CommonLib's definition is not proof: a SpellsLearned event carried 0x46 where it declares a
		// SpellItem*, and reading that as a form crashed the game (0.0.6, 2026-11-10). Later the same
		// field held the player right after a load (0.0.7, 2026-11-18), a live form of the wrong kind.
		// So the value must be a live form of the expected type before it is used. Each pointer is
		// checked once per event, and every decision about it (skip, write) uses that one result.
		struct Checked
		{
			const void*        raw;   // the value as the event carried it
			const RE::TESForm* live;  // live form at that address, of any type, or nullptr
			const RE::TESForm* form;  // `live` if it is of the expected type, else nullptr
		};

		Checked Check(const void* a_ptr, RE::FormType a_type)
		{
			const auto* live = forms::VerifiedForm(a_ptr);
			return { a_ptr, live, live && live->Is(a_type) ? live : nullptr };
		}

		// Writes the object only if it is a live form of the expected type. The raw value is always
		// written under "<key>Raw", so a wrong definition shows up in the record. A live form of
		// another type is written as "<key>WrongType" and "<key>WrongKey" (never under "<key>", so it
		// cannot be read as the thing the field names).
		void PutChecked(Line& a_line, std::string_view a_key, const Checked& a_value)
		{
			const auto* ref = a_value.form ? a_value.form->AsReference() : nullptr;
			a_line.raw(a_key, ref ? forms::Json(ref) : forms::Json(a_value.form));
			a_line.hex(std::string(a_key) + "Raw", reinterpret_cast<std::uintptr_t>(a_value.raw));
			if (a_value.live && !a_value.form) {
				a_line.str(std::string(a_key) + "WrongType", RE::FormTypeToString(a_value.live->GetFormType()));
				a_line.str(std::string(a_key) + "WrongKey", forms::Key(a_value.live));
			}
		}

		// For a value checked as an actor.
		bool IsCheckedPlayerSide(const Checked& a_actor)
		{
			return a_actor.form && forms::IsPlayerSide(a_actor.form->AsReference());
		}

		// --- strings carried by events ----------------------------------------------------------

		// Strings that other plugins or scripts hand to an event are not trusted to be strings.
		// On 2026-11-23 a menu event carried bytes that looked like a heap address instead of a
		// name (probably a name that had already been released, not confirmed). About 2 s later
		// the same thread crashed in another plugin on a one-byte read at the start of a page,
		// which looks like a string read (also not confirmed). Such text is copied only from
		// readable memory and up to a limit (text::CopyCString), never read in place.

		// (0.0.10, measurement) The safe copy made the menu and mod event sinks about four times
		// slower in 0.0.9 (menu 21 us against 4.9 us in the same first 210 s of a run). This build
		// records what the parts of a read cost, as "perf" names:
		// - for each place a string is read from ("<site>"): the whole read of a non-null address,
		//   by the copy size it used ("text.read.<site>.512" / ".256", one sample per string);
		//   each VirtualQuery ("text.query.<site>") and each ReadProcessMemory ("text.copy.<site>"),
		//   one sample per call;
		// - over all sites, by what might drive the cost: each VirtualQuery that found readable
		//   memory, by the size it reported from the read address's page to the region end
		//   ("text.queryRegion.<size>"), and each ReadProcessMemory by the bytes it asked for
		//   ("text.copyBytes.upTo511" / ".from512", copies that succeeded; older Windows kernels are
		//   said to copy more than 511 bytes by a slower path, not confirmed);
		// - what the measuring itself takes in each call ("text.observer.query" / ".copy").
		// Each read picks 512 or 256 bytes per copy at random (a hash of a per-launch seed, the site
		// and a per-site counter), so both sizes are measured on the same strings in the same run.
		// Taking turns would not do: menus open and close in the same order at every load, so a
		// given menu got the same size every time (seen by replaying the 0.0.9 records).
		enum class Site : std::uint8_t
		{
			menu,
			modName,
			modStr,
			stat,
			objective,
			worldspace,
		};
		constexpr std::size_t kSiteCount = static_cast<std::size_t>(Site::worldspace) + 1;

		struct SiteNames
		{
			const char* site;
			const char* read512;
			const char* read256;
			const char* query;
			const char* copy;
		};

		// In the order of Site.
		constexpr std::array<SiteNames, kSiteCount> kSiteNames{ {
			{ "menu", "text.read.menu.512", "text.read.menu.256", "text.query.menu", "text.copy.menu" },
			{ "modName", "text.read.modName.512", "text.read.modName.256", "text.query.modName", "text.copy.modName" },
			{ "modStr", "text.read.modStr.512", "text.read.modStr.256", "text.query.modStr", "text.copy.modStr" },
			{ "stat", "text.read.stat.512", "text.read.stat.256", "text.query.stat", "text.copy.stat" },
			{ "objective", "text.read.objective.512", "text.read.objective.256", "text.query.objective", "text.copy.objective" },
			{ "worldspace", "text.read.worldspace.512", "text.read.worldspace.256", "text.query.worldspace", "text.copy.worldspace" },
		} };

		const SiteNames& NamesOf(Site a_site) { return kSiteNames[static_cast<std::size_t>(a_site)]; }

		const char* QueryRegionName(std::size_t a_regionSize)
		{
			constexpr std::size_t kKiB = 1024;
			constexpr std::size_t kMiB = kKiB * 1024;
			if (a_regionSize <= 64 * kKiB) {
				return "text.queryRegion.upTo64K";
			}
			if (a_regionSize <= kMiB) {
				return "text.queryRegion.upTo1M";
			}
			if (a_regionSize <= 16 * kMiB) {
				return "text.queryRegion.upTo16M";
			}
			if (a_regionSize <= 64 * kMiB) {
				return "text.queryRegion.upTo64M";
			}
			if (a_regionSize <= 256 * kMiB) {
				return "text.queryRegion.upTo256M";
			}
			if (a_regionSize <= 1024 * kMiB) {
				return "text.queryRegion.upTo1G";
			}
			return "text.queryRegion.over1G";
		}

		constexpr std::size_t                              kSmallChunk = 256;
		std::array<std::atomic<std::uint64_t>, kSiteCount> g_textReads{};

		// splitmix64's output step: consecutive inputs give unrelated bits.
		constexpr std::uint64_t Mix(std::uint64_t a_value)
		{
			a_value += 0x9E37'79B9'7F4A'7C15ull;
			a_value = (a_value ^ (a_value >> 30)) * 0xBF58'476D'1CE4'E5B9ull;
			a_value = (a_value ^ (a_value >> 27)) * 0x94D0'49BB'1331'11EBull;
			return a_value ^ (a_value >> 31);
		}

		// Chosen once per launch (when the DLL loads, before any event), so a string read at the
		// same place in every run, such as the first menu, does not get the same copy size in
		// every run. Written in the "text.chunk" line.
		const std::uint64_t g_chunkSeed = Mix(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
											  (static_cast<std::uint64_t>(::GetCurrentProcessId()) << 32));

		// Picks the copy size for one read: a site's reads are numbered, and each number, the site
		// and the seed are hashed together, so sizes do not follow the order in which strings come.
		std::size_t NextChunk(Site a_site)
		{
			const auto site = static_cast<std::size_t>(a_site);
			const auto n = g_textReads[site].fetch_add(1, std::memory_order_relaxed);
			return (Mix(g_chunkSeed + n * kSiteCount + site) & 1) == 0 ? text::kMaxChunk : kSmallChunk;
		}

		// Which memory the strings sit in: one "text.region" line the first time a site reads from
		// a region, with what VirtualQuery reported. A region here is the allocation
		// (AllocationBase), where the run of pages VirtualQuery reported ends, its state, and its
		// protection when committed. Windows leaves AllocationBase, Protect and Type undefined for
		// memory that is not allocated (MEM_FREE) and Protect undefined for reserved memory, so
		// those are left out of the key (the line still shows what came back). A run of pages
		// that grows gets a new end, so the count of regions can be higher than the count of
		// allocations. If each site uses only a few regions, a later version could remember
		// checked regions instead of asking VirtualQuery for every string.
		// Each site writes at most kMaxRegionLinesPerSite lines; regions it meets after that are
		// counted per site since the start ("textRegionsSkipped" in "counts"). Each site remembers
		// at most kMaxRegionKeysPerSite regions; after that it writes and counts no new ones, and
		// "textRegionsFull" in "counts" says so for that site.
		// The line is written after the read that met the region, once that read's timing has
		// ended: writing it is not part of "text.read.*" or "text.observer.*", but it is part of
		// the sink's time ("menu" etc.), and is timed on its own as "text.regionLines".
		constexpr std::size_t kMaxRegionLinesPerSite = 50;
		constexpr std::size_t kMaxRegionKeysPerSite = 1024;
		using RegionKey = std::tuple<Site, std::uintptr_t, std::uintptr_t, unsigned long, unsigned long>;

		std::mutex                            g_regionLock;
		std::set<RegionKey>                   g_regionsSeen;       // guarded by g_regionLock
		std::array<std::size_t, kSiteCount>   g_regionKeys{};      // guarded by g_regionLock
		std::array<std::uint64_t, kSiteCount> g_regionLines{};     // guarded by g_regionLock
		std::array<std::uint64_t, kSiteCount> g_regionsSkipped{};  // guarded by g_regionLock
		std::array<bool, kSiteCount>          g_regionsFull{};     // guarded by g_regionLock

		// Records a query's region for its site. True if this is a region the site has not met and
		// it may still write a line for it.
		bool NoteRegion(Site a_site, const text::CallInfo& a_call)
		{
			const auto      index = static_cast<std::size_t>(a_site);
			const auto      regionEnd = a_call.regionBase + a_call.regionSize;
			const bool      unallocated = a_call.state == MEM_FREE;
			const RegionKey key{ a_site, unallocated ? 0 : a_call.allocationBase, regionEnd, a_call.state,
				a_call.state == MEM_COMMIT ? a_call.protect : 0 };
			std::scoped_lock lock(g_regionLock);
			if (g_regionsSeen.contains(key)) {
				return false;
			}
			if (g_regionKeys[index] >= kMaxRegionKeysPerSite) {
				g_regionsFull[index] = true;
				return false;
			}
			g_regionsSeen.insert(key);
			++g_regionKeys[index];
			if (g_regionLines[index] >= kMaxRegionLinesPerSite) {
				++g_regionsSkipped[index];
				return false;
			}
			++g_regionLines[index];
			return true;
		}

		void WriteRegionLine(Site a_site, const text::CallInfo& a_call)
		{
			Line("text.region").str("site", NamesOf(a_site).site).flag("ok", a_call.ok).num("us", a_call.us)
				.hex("address", a_call.address).hex("allocationBase", a_call.allocationBase).hex("regionBase", a_call.regionBase)
				.hex("regionEnd", a_call.regionBase + a_call.regionSize).num("regionSize", static_cast<std::uint64_t>(a_call.regionSize))
				.hex("state", a_call.state).hex("protect", a_call.protect).hex("type", a_call.type).emit();
		}

		// "textRegionsSkipped" and "textRegionsFull" for the counts line, each an object by site.
		std::pair<std::string, std::string> RegionCountsJson()
		{
			std::scoped_lock lock(g_regionLock);
			std::string      skipped = "{";
			std::string      full = "{";
			for (std::size_t i = 0; i < kSiteCount; ++i) {
				skipped += std::format("{}\"{}\":{}", i ? "," : "", kSiteNames[i].site, g_regionsSkipped[i]);
				full += std::format("{}\"{}\":{}", i ? "," : "", kSiteNames[i].site, g_regionsFull[i] ? "true" : "false");
			}
			return { skipped + "}", full + "}" };
		}

		class SiteTiming final : public text::CallObserver
		{
		public:
			explicit SiteTiming(Site a_site) :
				_site(a_site)
			{}

			// The time spent here goes to "text.observer.query" or "text.observer.copy", on the
			// same clock as the calls. It leaves out that Add itself, which costs about as much as
			// one of the Adds here.
			void OnCall(const text::CallInfo& a_call) override
			{
				const auto& names = NamesOf(_site);
				if (a_call.call == text::Call::query) {
					perf::Scope scope("text.observer.query");
					perf::Add(names.query, a_call.us);
					if (a_call.ok) {
						perf::Add(QueryRegionName(a_call.regionSize), a_call.us);
					}
					if (NoteRegion(_site, a_call)) {
						if (_newRegionCount < _newRegions.size()) {
							_newRegions[_newRegionCount++] = a_call;
						} else {
							WriteRegionLine(_site, a_call);  // not expected (see kMaxNewRegions); written now rather than lost
						}
					}
				} else {
					perf::Scope scope("text.observer.copy");
					perf::Add(names.copy, a_call.us);
					if (a_call.ok) {
						perf::Add(a_call.bytes <= 511 ? "text.copyBytes.upTo511" : "text.copyBytes.from512", a_call.us);
					}
				}
			}

			// The "text.region" lines for the regions this read met first. Call after the read's
			// timing has ended and before the record's Line is made.
			void WriteRegionLines() const
			{
				if (_newRegionCount == 0) {
					return;
				}
				perf::Scope scope("text.regionLines");
				for (std::size_t i = 0; i < _newRegionCount; ++i) {
					WriteRegionLine(_site, _newRegions[i]);
				}
			}

		private:
			// A read queries at most once per page, and stops at the first page it cannot read:
			// the text (at most 4097 bytes) spans at most two 4 KiB pages, and the 16 bytes shown
			// when it is bad at most two more. Kept in place, so noting a region allocates nothing.
			static constexpr std::size_t kMaxNewRegions = 4;

			Site                                       _site;
			std::array<text::CallInfo, kMaxNewRegions> _newRegions{};
			std::size_t                                _newRegionCount{ 0 };
		};

		struct EventText
		{
			std::string    text;   // the bytes copied, without the terminating zero
			text::Read     read;   // how the copy ended
			const char*    bad;    // nullptr, or a text::Classify reason, or "null"
			std::uintptr_t ptr;    // where the string was
			std::string    bytes;  // when bad: the first bytes at ptr, zeros included, read with the copy
		};

		// The address a BSFixedString holds, taken without going through its accessors:
		// BSFixedString::data() looks at the pool entry in front of the text in builds with
		// assertions, which is the memory that is not trusted here. The class is standard-layout
		// with the pointer as its only member, so the object and that member share an address.
		const char* RawData(const RE::BSFixedString& a_string)
		{
			static_assert(std::is_standard_layout_v<RE::BSFixedString>);
			static_assert(sizeof(RE::BSFixedString) == sizeof(const char*));
			return *reinterpret_cast<const char* const*>(&a_string);
		}

		// a_emptyIsBad: whether a missing or empty string is worth marking for this source. A menu
		// event without a name is unexpected (whether it ever happens for a real menu is not
		// known: the kept 0.0.8 records had 340 menu lines with an empty name, from four runs on
		// 11-23 and 11-24, at regular places such as right after "Mist Menu" opened, and did not
		// tell a null name from an empty one). Mod event strings are often empty on purpose, so
		// for them it is not.
		// Read the text before making the record's Line: a first read from a new region writes a
		// "text.region" line (after the read's timing), and a Line made earlier would carry an
		// earlier time yet be written after it.
		EventText ReadEventText(const char* a_data, bool a_emptyIsBad, Site a_site)
		{
			// Long enough for the longest strings seen (a mod event string of 3,663 bytes in the 0.0.7
			// records). Text longer than this is cut and marked "<key>Cut", not treated as broken.
			constexpr std::size_t kMaxText = 4096;
			constexpr std::size_t kShownBytes = 16;
			EventText             result{ {}, text::Read::ok, nullptr, reinterpret_cast<std::uintptr_t>(a_data), {} };
			if (!a_data) {
				result.bad = a_emptyIsBad ? "null" : nullptr;
				return result;
			}
			const auto chunk = NextChunk(a_site);
			SiteTiming timing(a_site);
			{
				perf::Scope scope(chunk == kSmallChunk ? NamesOf(a_site).read256 : NamesOf(a_site).read512);
				// One byte past the limit tells a text of exactly kMaxText bytes from a longer one.
				result.read = text::CopyCString(a_data, kMaxText + 1, result.text, &timing, chunk);
				if (result.read == text::Read::tooLong) {
					result.text.resize(kMaxText);
				}
				result.bad = text::Classify(result.read, result.text, a_emptyIsBad);
				if (result.bad) {
					result.bytes = text::CopyBytes(a_data, kShownBytes, &timing);
				}
			}
			timing.WriteRegionLines();
			return result;
		}

		EventText ReadEventText(const RE::BSFixedString& a_string, bool a_emptyIsBad, Site a_site)
		{
			return ReadEventText(RawData(a_string), a_emptyIsBad, a_site);
		}

		// Writes the text under a_key, cut at the limit if it was longer ("<key>Cut").
		// Text that is not clean is not written there; the key is null, as PutChecked does for a
		// form of the wrong type, and "<key>Text" holds what was copied (escaped), so nothing is
		// lost. A string that is merely missing or empty keeps "" under a_key. Either way
		// "<key>Bad" says why, "<key>Ptr" gives the address and "<key>Bytes" the first 16 bytes at
		// that address (zeros included), read together with the copy.
		void PutText(Line& a_line, std::string_view a_key, const EventText& a_text)
		{
			constexpr std::size_t kShownBytes = 16;
			const bool            cut = a_text.read == text::Read::tooLong;
			const auto            shown = cut ? text::WithoutCutSequence(a_text.text) : std::string_view(a_text.text);
			const bool            missing = a_text.bad && (std::strcmp(a_text.bad, "null") == 0 || std::strcmp(a_text.bad, "empty") == 0);
			if (a_text.bad && !missing) {
				a_line.raw(a_key, "null");
				a_line.str(std::string(a_key) + "Text", shown);
			} else {
				a_line.str(a_key, shown);
			}
			if (cut) {
				a_line.flag(std::string(a_key) + "Cut", true);
			}
			if (a_text.bad) {
				a_line.str(std::string(a_key) + "Bad", a_text.bad);
				a_line.hex(std::string(a_key) + "Ptr", a_text.ptr);
				if (a_text.ptr) {
					a_line.str(std::string(a_key) + "Bytes", text::HexBytes(a_text.bytes, kShownBytes));
				}
			}
		}


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
			const auto  display = obj ? ReadEventText(obj->displayText, false, Site::objective) : EventText{};
			Line        l("quest.objective");
			if (obj) {
				const auto* quest = obj->ownerQuest;
				const char* qname = quest ? quest->GetName() : nullptr;
				l.str("quest", forms::Key(quest)).str("name", qname ? qname : "")
					.num("index", static_cast<std::uint64_t>(obj->index));
				PutText(l, "text", display);
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
			// The worldspace ID is a plain C string from the event; copied like other event text.
			const auto worldspace = ReadEventText(e.worldspaceID, false, Site::worldspace);
			Line       line("location.discovered");
			line.str("name", name ? name : "");
			PutText(line, "worldspace", worldspace);
			line.emit();
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
			const auto name = ReadEventText(e.stat, false, Site::stat);
			Line       line("stat");
			PutText(line, "name", name);
			line.num("value", static_cast<std::int64_t>(e.value)).emit();
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

		void OnSpellLearned(const RE::SpellsLearned::Event& e)
		{
			// The first field has held 0x46 (spell tome read) and the player (right after a load), never
			// a spell so far (see PutChecked). The learned spell is taken from the tome read in the
			// same ms; this line only marks the moment.
			Line line("spell.learned");
			PutChecked(line, "spell", Check(e.spell, RE::FormType::Spell));
			line.emit();
		}

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
			// Never seen in the stage 0 records, so the pointers are checked before use.
			Line line("soul.trapped");
			PutChecked(line, "trapper", Check(e.trapper, RE::FormType::ActorCharacter));
			PutChecked(line, "target", Check(e.target, RE::FormType::ActorCharacter));
			line.emit();
		}

		void OnCritical(const RE::CriticalHit::Event& e)
		{
			// Seen working on 1.6.1170 (0.0.7, three times), still checked. An aggressor that does not
			// check out as an actor is written too: that is exactly the case worth knowing about.
			const auto aggressor = Check(e.aggressor, RE::FormType::ActorCharacter);
			if (aggressor.form && !IsCheckedPlayerSide(aggressor)) {
				return;
			}
			Line line("crit");
			PutChecked(line, "aggressor", aggressor);
			PutChecked(line, "weapon", Check(e.weapon, RE::FormType::Weapon));
			line.flag("sneak", e.sneakHit).emit();
		}

		void OnDisarm(const RE::DisarmedEvent::Event& e)
		{
			// Never seen in the stage 0 records, so the pointers are checked before use.
			const auto source = Check(e.source, RE::FormType::ActorCharacter);
			const auto target = Check(e.target, RE::FormType::ActorCharacter);
			if (source.form && target.form && !IsCheckedPlayerSide(source) && !IsCheckedPlayerSide(target)) {
				return;
			}
			Line line("disarm");
			PutChecked(line, "source", source);
			PutChecked(line, "target", target);
			line.emit();
		}

		void OnShout(const RE::ShoutAttack::Event& e) { Line("shout").raw("shout", forms::Json(e.shout)).emit(); }

		void OnActorKill(const RE::ActorKill::Event& e)
		{
			Line("actor.kill").raw("killer", forms::Json(e.killer)).raw("victim", forms::Json(e.victim))
				.flag("victimKillMove", e.victim && e.victim->IsInKillMove()).emit();
		}

		void OnMenu(const RE::MenuOpenCloseEvent& e)
		{
			const auto name = ReadEventText(e.menuName, true, Site::menu);
			Line       line("menu");
			PutText(line, "name", name);
			line.flag("opening", e.opening).emit();
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
			// Names and strings come from scripts and other plugins (see EventText).
			const auto name = ReadEventText(e.eventName, false, Site::modName);
			bool       first = false;
			if (name.bad) {
				// A broken name is not counted (each one may differ, and the map would only grow).
				// It is logged, since that is the case worth seeing, but only so many times in case
				// some plugin sends them all the time; the rest are counted in "counts".
				if (g_badModEventLines.fetch_add(1) >= kMaxBadModEventLines) {
					g_badModEventsSkipped.fetch_add(1);
					return;
				}
			} else {
				std::scoped_lock lock(g_countLock);
				first = Count(g_modEventCounts, name.text);
			}
			const auto* senderRef = e.sender ? e.sender->As<RE::TESObjectREFR>() : nullptr;
			// Always log the first of each name; later ones only when the player side is involved.
			if (!name.bad && !first && !forms::IsPlayerSide(senderRef)) {
				return;
			}
			const auto str = ReadEventText(e.strArg, false, Site::modStr);
			Line       line("modevent");
			PutText(line, "name", name);
			PutText(line, "str", str);
			line.num("num", static_cast<double>(e.numArg)).raw("sender", forms::Json(e.sender));
			if (!name.bad) {
				line.flag("first", first);  // "first" is about counted names only
			}
			line.emit();
		}
	}

	void RegisterGlobal()
	{
		// Before any string is read: the seed and sizes behind "text.read.<site>.512" and ".256".
		Line("text.chunk").hex("seed", g_chunkSeed).num("large", static_cast<std::uint64_t>(text::kMaxChunk))
			.num("small", static_cast<std::uint64_t>(kSmallChunk)).emit();
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
				// Names go through KeyText first: two names that differ only in bytes that are not
				// UTF-8 would both become U+FFFD and give the object the same key twice.
				json += std::format("{}\"{}\":{}", json.size() > 1 ? "," : "", Escape(text::KeyText(name)), n);
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
		const auto [regionsSkipped, regionsFull] = RegionCountsJson();
		Line("counts").raw("animTags", anim).raw("enemyAnimTags", enemyAnim).raw("modEvents", mods)
			.num("effectApplySuppressed", suppressed)
			.num("badModEventsSkipped", g_badModEventsSkipped.exchange(0))
			.raw("textRegionsSkipped", regionsSkipped).raw("textRegionsFull", regionsFull).emit();
	}
}
