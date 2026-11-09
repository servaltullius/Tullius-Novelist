#include "Session.h"
#include "DamageHook.h"
#include "Events.h"
#include "Forms.h"
#include "Out.h"
#include "Poller.h"
#include "World.h"

namespace probe::session
{
	namespace
	{
		// Serializes starts: a message and the poll task may both decide to start.
		std::mutex       g_startLock;
		std::atomic_bool g_started{ false };
		std::atomic_bool g_loading{ false };

		// Call with g_startLock held.
		void Start(std::string_view a_by)
		{
			g_started.store(true);
			Line("session.start").str("by", a_by).emit();
			damage::InstallOnce();
			events::AttachPlayerGraph();
			events::ResetEnemyGraphs();
			damage::ResetDotSources();
			poller::ResetBaseline();
			forms::ResetTeammates();
			world::Reset();
		}
	}

	void OnPreLoad()
	{
		g_loading.store(true);
	}

	void OnGameStart(std::string_view a_by)
	{
		std::scoped_lock lock(g_startLock);
		g_loading.store(false);
		Start(a_by);
	}

	void OnLoadFailed()
	{
		g_loading.store(false);
	}

	void OnRevert()
	{
		// No lock: this runs inside the game's revert, which must not wait on a start in progress.
		g_started.store(false);
	}

	void EnsureStarted()
	{
		if (g_started.load() || g_loading.load()) {
			return;
		}
		// The player can stay in the world behind the main menu or a loading screen; that is not a game start.
		if (auto* ui = RE::UI::GetSingleton()) {
			if (ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
				return;
			}
		}
		std::scoped_lock lock(g_startLock);
		if (g_started.load() || g_loading.load()) {
			return;  // a message started the game, or a load began, while we were checking
		}
		// No kPreLoadGame or kNewGame came for this game, so nothing has marked the new generation yet.
		out::BumpGeneration();
		Start("world");
	}
}
