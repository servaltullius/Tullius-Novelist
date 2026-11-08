// Tullius Novelist stage 0 probe.
// Logs the order and thread of SKSE signals, gameplay events, HandleHealthDamage calls and
// polled player state into Documents/My Games/<game>/SKSE/TulliusNovelistProbe-<time>.jsonl.
// It changes nothing in the game. See docs/design-draft.md, chapter 12 (stage 0).

#include "CoSave.h"
#include "DamageHook.h"
#include "Events.h"
#include "Forms.h"
#include "Out.h"
#include "Poller.h"
#include "World.h"

namespace
{
	const char* MessageName(std::uint32_t a_type)
	{
		using M = SKSE::MessagingInterface;
		switch (a_type) {
		case M::kPostLoad:
			return "PostLoad";
		case M::kPostPostLoad:
			return "PostPostLoad";
		case M::kPreLoadGame:
			return "PreLoadGame";
		case M::kPostLoadGame:
			return "PostLoadGame";
		case M::kSaveGame:
			return "SaveGame";
		case M::kDeleteGame:
			return "DeleteGame";
		case M::kInputLoaded:
			return "InputLoaded";
		case M::kNewGame:
			return "NewGame";
		case M::kDataLoaded:
			return "DataLoaded";
		default:
			return "Other";
		}
	}

	void AfterGameStart()
	{
		probe::damage::InstallOnce();
		probe::events::AttachPlayerGraph();
		probe::events::ResetEnemyGraphs();
		probe::damage::ResetDotSources();
		probe::poller::ResetBaseline();
		probe::forms::ResetTeammates();
		probe::world::Reset();
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		using M = SKSE::MessagingInterface;
		probe::Line line("skse.message");
		line.str("type", MessageName(a_msg->type)).num("typeId", a_msg->type).num("dataLen", a_msg->dataLen);

		switch (a_msg->type) {
		case M::kPreLoadGame:
		case M::kSaveGame:
		case M::kDeleteGame:
			if (a_msg->data && a_msg->dataLen > 0) {
				line.str("save", std::string_view(static_cast<const char*>(a_msg->data), ::strnlen(static_cast<const char*>(a_msg->data), a_msg->dataLen)));
			}
			break;
		case M::kPostLoadGame:
			// SKSE passes the success flag in the pointer value itself; do not dereference it.
			line.flag("success", a_msg->data != nullptr);
			break;
		default:
			break;
		}
		if (a_msg->type == M::kPreLoadGame) {
			probe::out::BumpGeneration();
		}
		line.emit();

		switch (a_msg->type) {
		case M::kDataLoaded:
			probe::events::RegisterGlobal();
			probe::poller::Start();
			break;
		case M::kNewGame:
			AfterGameStart();
			break;
		case M::kPostLoadGame:
			if (a_msg->data != nullptr) {
				AfterGameStart();
			}
			break;
		case M::kSaveGame:
			probe::out::FlushSoon();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	probe::out::Start();

	const auto ver = a_skse->RuntimeVersion();
	probe::Line("probe.start")
		.str("runtime", std::format("{}.{}.{}.{}", ver.major(), ver.minor(), ver.patch(), ver.build()))
		.str("probeVersion", TN_PROBE_VERSION)
		.emit();

	probe::cosave::Register();
	if (auto* messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener(OnMessage);
	}
	return true;
}
