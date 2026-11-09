#include "CoSave.h"
#include "Out.h"
#include "Session.h"

namespace probe::cosave
{
	namespace
	{
		constexpr std::uint32_t kUniqueId = 'TNPB';
		constexpr std::uint32_t kRecord = 'PRB1';
		constexpr std::uint32_t kVersion = 1;

		struct Record
		{
			std::uint32_t saveCounter;
			std::uint32_t generation;
			std::uint64_t saveId;
		};

		std::atomic_uint32_t g_saveCounter{ 0 };
		// Lineage head: the save the current play continues from (last successful save or last load).
		std::atomic_uint64_t g_headId{ 0 };

		std::uint64_t NewId()
		{
			const auto t = static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
			return t ^ (static_cast<std::uint64_t>(::GetCurrentThreadId()) << 48);
		}

		void OnSave(SKSE::SerializationInterface* a_intfc)
		{
			const Record rec{ g_saveCounter.fetch_add(1) + 1, out::Generation(), NewId() };
			const bool ok = a_intfc->WriteRecord(kRecord, kVersion, rec);
			Line("cosave.save").num("counter", rec.saveCounter).hex("saveId", rec.saveId)
				.hex("parentId", g_headId.load()).flag("writeOk", ok).emit();
			if (ok) {
				// Play continues from this save, so the next save's parent is this one.
				g_headId.store(rec.saveId);
			}
			out::FlushSoon();
		}

		void OnLoad(SKSE::SerializationInterface* a_intfc)
		{
			std::uint32_t type = 0;
			std::uint32_t version = 0;
			std::uint32_t length = 0;
			bool found = false;
			while (a_intfc->GetNextRecordInfo(type, version, length)) {
				if (type == kRecord && version == kVersion && length == sizeof(Record)) {
					Record rec{};
					if (a_intfc->ReadRecordData(rec)) {
						found = true;
						g_headId.store(rec.saveId);
						Line("cosave.load").num("counter", rec.saveCounter).hex("saveId", rec.saveId)
							.num("savedGen", rec.generation).emit();
					}
				}
			}
			if (!found) {
				g_headId.store(0);
				Line("cosave.load").flag("noRecord", true).emit();
			}
		}

		void OnRevert(SKSE::SerializationInterface*)
		{
			// The game being torn down is no longer what the next save continues from. A load sets the
			// head again in OnLoad; a new game gets no load callback and must start without a parent.
			g_headId.store(0);
			Line("cosave.revert").emit();
			session::OnRevert();
		}
	}

	void Register()
	{
		auto* intfc = SKSE::GetSerializationInterface();
		if (!intfc) {
			Line("probe.cosave.fail").emit();
			return;
		}
		intfc->SetUniqueID(kUniqueId);
		intfc->SetSaveCallback(OnSave);
		intfc->SetLoadCallback(OnLoad);
		intfc->SetRevertCallback(OnRevert);
		Line("probe.cosave").emit();
	}
}
