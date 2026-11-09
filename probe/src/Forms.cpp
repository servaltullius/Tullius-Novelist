#include "Forms.h"
#include "Out.h"

namespace probe::forms
{
	namespace
	{
		std::mutex                    g_teamLock;
		std::unordered_set<RE::FormID> g_team;

		using PtrList = std::vector<std::uintptr_t>;
		std::atomic<std::shared_ptr<const PtrList>> g_teamPtrs{ std::make_shared<const PtrList>() };
	}

	namespace
	{
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

		bool HasGameVtable(const void* a_ptr)
		{
			const auto vtbl = *reinterpret_cast<const std::uintptr_t*>(a_ptr);
			const auto rdata = REL::Module::get().segment(REL::Segment::rdata);
			const auto begin = rdata.address();
			return vtbl >= begin && vtbl < begin + rdata.size();
		}
	}

	bool IsGameObject(const void* a_ptr)
	{
		return IsReadable(a_ptr, sizeof(void*)) && HasGameVtable(a_ptr);
	}

	const RE::TESForm* VerifiedForm(const void* a_ptr)
	{
		// The whole TESForm header has to be readable before the form ID is taken from it.
		if (!IsReadable(a_ptr, sizeof(RE::TESForm)) || !HasGameVtable(a_ptr)) {
			return nullptr;
		}
		const auto* form = static_cast<const RE::TESForm*>(a_ptr);
		const auto  id = form->GetFormID();
		// A real form is the one the game returns for its own ID. Anything else that merely looks
		// like an object (a stale pointer, a field that is not a form at all) fails here.
		return id != 0 && RE::TESForm::LookupByID(id) == form ? form : nullptr;
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
		return std::format("{}:{:X}", file->GetFilename(), a_form->GetLocalFormID());
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
