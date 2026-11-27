#include "Forms.h"
#include "Out.h"
#include "Text.h"

#include <cstddef>
#include <cstring>

namespace probe::forms
{
	namespace
	{
		std::mutex                     g_teamLock;
		std::unordered_set<RE::FormID> g_team;

		using PtrList = std::vector<std::uintptr_t>;
		std::atomic<std::shared_ptr<const PtrList>> g_teamPtrs{ std::make_shared<const PtrList>() };
		std::atomic_bool                            g_selfTested{ false };

		bool IsAligned8(const void* a_ptr) noexcept
		{
			return (reinterpret_cast<std::uintptr_t>(a_ptr) & 7u) == 0;
		}

		bool IsGameVtable(std::uintptr_t a_vtbl)
		{
			if ((a_vtbl & 7u) != 0) {
				return false;
			}
			const auto rdata = REL::Module::get().segment(REL::Segment::rdata);
			const auto begin = rdata.address();
			return a_vtbl >= begin && a_vtbl < begin + rdata.size();
		}
	}

	const char* ObjectCheckName(ObjectCheck a_check) noexcept
	{
		switch (a_check) {
		case ObjectCheck::ok:
			return "ok";
		case ObjectCheck::null:
			return "null";
		case ObjectCheck::unaligned:
			return "unaligned";
		case ObjectCheck::unreadable:
			return "unreadable";
		case ObjectCheck::notGameVtable:
			return "notGameVtable";
		}
		return "unknown";
	}

	void CopyTiming::OnCall(const text::CallInfo& a_call) noexcept
	{
		perf::Add(_perfName, a_call.us);
	}

	ObjectCheck CheckGameObject(const void* a_ptr, text::CallObserver* a_observer)
	{
		if (!a_ptr) {
			return ObjectCheck::null;
		}
		if (!IsAligned8(a_ptr)) {
			return ObjectCheck::unaligned;
		}
		std::uintptr_t vtbl = 0;
		if (!text::CopyExact(a_ptr, &vtbl, sizeof(vtbl), a_observer)) {
			return ObjectCheck::unreadable;
		}
		return IsGameVtable(vtbl) ? ObjectCheck::ok : ObjectCheck::notGameVtable;
	}

	const RE::TESForm* VerifiedForm(const void* a_ptr, text::CallObserver* a_observer)
	{
		static_assert(sizeof(RE::TESForm) == 0x20);
		static_assert(offsetof(RE::TESForm, formID) == 0x14);
		static_assert(sizeof(RE::FormID) == 4);

		if (!a_ptr || !IsAligned8(a_ptr)) {
			return nullptr;
		}
		alignas(8) std::byte header[sizeof(RE::TESForm)]{};
		if (!text::CopyExact(a_ptr, header, sizeof(header), a_observer)) {
			return nullptr;
		}
		std::uintptr_t vtbl = 0;
		std::memcpy(&vtbl, header, sizeof(vtbl));
		if (!IsGameVtable(vtbl)) {
			return nullptr;
		}
		RE::FormID id = 0;
		std::memcpy(&id, header + offsetof(RE::TESForm, formID), sizeof(id));
		const auto* form = static_cast<const RE::TESForm*>(a_ptr);
		// A real form is the one the game returns for its own ID. Anything else that merely looks
		// like an object (a stale pointer, a field that is not a form at all) fails here.
		return id != 0 && RE::TESForm::LookupByID(id) == form ? form : nullptr;
	}

	void RunSelfTestOnce()
	{
		if (g_selfTested.exchange(true)) {
			return;
		}

		const auto* const        player = RE::PlayerCharacter::GetSingleton();
		const RE::TESForm* const playerBase = player;
		bool                     pass = true;
		std::uint64_t            skipped = 0;

		const char* playerRes = "skipped";
		if (player) {
			const auto r = CheckGameObject(player);
			playerRes = ObjectCheckName(r);
			pass = pass && r == ObjectCheck::ok;
		} else {
			++skipped;
		}

		const auto  lowCheck = CheckGameObject(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x46)));
		const char* lowRes = ObjectCheckName(lowCheck);
		pass = pass && lowCheck == ObjectCheck::unaligned;

		const auto  lowAlignedCheck = CheckGameObject(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x40)));
		const char* lowAlignedRes = ObjectCheckName(lowAlignedCheck);
		pass = pass && lowAlignedCheck == ObjectCheck::unreadable;

		alignas(8) std::uint64_t zero = 0;
		const auto               localCheck = CheckGameObject(&zero);
		const char*              localRes = ObjectCheckName(localCheck);
		pass = pass && localCheck == ObjectCheck::notGameVtable;

		const char* decommittedRes = "skipped";
		SYSTEM_INFO sys{};
		::GetSystemInfo(&sys);
		void* page = ::VirtualAlloc(nullptr, sys.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (page) {
			if (::VirtualFree(page, sys.dwPageSize, MEM_DECOMMIT) != 0) {
				const auto r = CheckGameObject(page);
				decommittedRes = ObjectCheckName(r);
				pass = pass && r == ObjectCheck::unreadable;
			} else {
				++skipped;
			}
			::VirtualFree(page, 0, MEM_RELEASE);
		} else {
			++skipped;
		}

		const auto formResultName = [playerBase](const RE::TESForm* a_form) -> const char* {
			if (!a_form) {
				return "null";
			}
			return a_form == playerBase ? "player" : "other";
		};

		const char* playerFormRes = "skipped";
		const char* headerCopyRes = "skipped";
		if (player) {
			const auto* pf = VerifiedForm(player);
			playerFormRes = formResultName(pf);
			pass = pass && pf == playerBase;

			alignas(8) std::byte copiedHeader[sizeof(RE::TESForm)]{};
			if (text::CopyExact(player, copiedHeader, sizeof(copiedHeader))) {
				const auto* copyForm = VerifiedForm(copiedHeader);
				headerCopyRes = formResultName(copyForm);
				pass = pass && copyForm == nullptr;
			} else {
				++skipped;
			}
		} else {
			skipped += 2;
		}

		pass = pass && skipped == 0;

		Line("form.selfTest")
			.flag("pass", pass)
			.num("skipped", skipped)
			.str("player", playerRes)
			.str("low", lowRes)
			.str("lowAligned", lowAlignedRes)
			.str("localBuffer", localRes)
			.str("decommittedPage", decommittedRes)
			.str("playerForm", playerFormRes)
			.str("headerCopy", headerCopyRes)
			.emit();
	}

	void PublishTeammatePointers()
	{
		auto list = std::make_shared<PtrList>();
		for (const auto id : KnownTeammates()) {
			if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id)) {
				list->push_back(reinterpret_cast<std::uintptr_t>(ref));
			}
		}
		g_teamPtrs.store(std::move(list));
	}

	bool IsKnownTeammatePtr(const void* a_ptr)
	{
		if (!a_ptr) {
			return false;
		}
		const auto list = g_teamPtrs.load();
		const auto value = reinterpret_cast<std::uintptr_t>(a_ptr);
		return std::find(list->begin(), list->end(), value) != list->end();
	}

	std::string Key(RE::FormID a_formID)
	{
		if (a_formID == 0) {
			return {};
		}
		if (a_formID >= 0xFF000000) {
			return std::format("dyn:{:08X}", a_formID);
		}
		if (const auto* form = RE::TESForm::LookupByID(a_formID)) {
			return Key(form);
		}
		return std::format("id:{:08X}", a_formID);
	}

	std::string Key(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return {};
		}
		const auto id = a_form->GetFormID();
		if (a_form->IsDynamicForm()) {
			return std::format("dyn:{:08X}", id);
		}
		const auto* file = a_form->GetFile(0);
		if (!file) {
			return std::format("id:{:08X}", id);
		}
		// The file name is kept lossless: a name that is not UTF-8 keeps its bytes as %XX
		// (text::KeyText), so different plugins never share a key.
		return std::format("{}:{:X}", text::KeyText(file->GetFilename()), a_form->GetLocalFormID());
	}

	std::string Json(const RE::TESForm* a_form)
	{
		if (!a_form) {
			return "null";
		}
		const char* name = a_form->GetName();
		return std::format(R"({{"key":"{}","name":"{}"}})", Escape(Key(a_form)), Escape(name ? name : ""));
	}

	std::string Json(const RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return "null";
		}
		const auto* base = a_ref->GetBaseObject();
		const char* name = a_ref->GetName();
		return std::format(R"({{"key":"{}","base":"{}","name":"{}"}})",
			Escape(Key(a_ref)), Escape(Key(base)), Escape(name ? name : ""));
	}

	bool IsPlayer(const RE::TESObjectREFR* a_ref)
	{
		return a_ref && a_ref == RE::PlayerCharacter::GetSingleton();
	}

	bool IsPlayerSide(const RE::TESObjectREFR* a_ref)
	{
		if (!a_ref) {
			return false;
		}
		if (IsPlayer(a_ref)) {
			return true;
		}
		const auto* actor = a_ref->As<RE::Actor>();
		if (actor && actor->IsPlayerTeammate()) {
			NoteTeammate(a_ref);
			return true;
		}
		return false;
	}

	void NoteTeammate(const RE::TESObjectREFR* a_ref)
	{
		if (!a_ref || IsPlayer(a_ref)) {
			return;
		}
		bool added = false;
		{
			std::scoped_lock lock(g_teamLock);
			added = g_team.insert(a_ref->GetFormID()).second;
		}
		if (added) {
			Line("teammate.seen").raw("actor", Json(a_ref)).emit();
		}
	}

	std::vector<RE::FormID> KnownTeammates()
	{
		std::scoped_lock lock(g_teamLock);
		return { g_team.begin(), g_team.end() };
	}

	void ResetTeammates()
	{
		{
			std::scoped_lock lock(g_teamLock);
			g_team.clear();
		}
		g_teamPtrs.store(std::make_shared<const PtrList>());
	}

	float DistanceToPlayer(const RE::TESObjectREFR* a_ref)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!a_ref || !player) {
			return -1.0f;
		}
		// Coordinates are comparable only inside one space: the same interior cell, or the same
		// worldspace. Otherwise an unrelated cell whose local coordinates happen to be close
		// would count as "near the player".
		const auto* cell = a_ref->GetParentCell();
		const auto* playerCell = player->GetParentCell();
		if (!cell || !playerCell) {
			return -1.0f;
		}
		const bool interior = cell->IsInteriorCell();
		if (interior != playerCell->IsInteriorCell()) {
			return -1.0f;
		}
		if (interior ? cell != playerCell : a_ref->GetWorldspace() != player->GetWorldspace()) {
			return -1.0f;
		}
		return a_ref->GetPosition().GetDistance(player->GetPosition());
	}
}
