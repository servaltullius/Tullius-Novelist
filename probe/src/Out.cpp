#include "Out.h"

namespace probe
{
	namespace
	{
		std::mutex               g_queueLock;
		std::vector<std::string> g_queue;
		std::atomic_bool         g_flushSoon{ false };
		std::atomic_uint32_t     g_generation{ 0 };
		std::int64_t             g_qpcFreq = 1;
		std::int64_t             g_qpcStart = 0;
		std::filesystem::path    g_path;
		std::filesystem::path    g_startupPath;
		std::atomic_bool         g_armed{ false };
		bool                     g_prepared = false;  // writer thread only
		std::size_t              g_mirrored = 0;      // writer thread only: queued lines already in the startup file
		bool                     g_startupOpened = false;  // writer thread only
		constexpr std::size_t    kMaxBacklog = 200'000;
		// Lines held in memory while no game has started (main menu). Past this the file is opened anyway.
		constexpr std::size_t    kMaxPending = 20'000;
		// Old record files: keep the newest kKeepFiles (including this run's), but never delete one written
		// within kKeepRecent, so a burst of short launches cannot push out a play session not yet handed over.
		constexpr std::size_t    kKeepFiles = 50;
		constexpr auto           kKeepRecent = std::chrono::hours{ 72 };
		constexpr std::wstring_view kFilePrefix = L"TulliusNovelistProbe-";
		std::uint64_t            g_openFailures = 0;  // writer thread only
		std::uint64_t            g_droppedLines = 0;  // writer thread only

		std::int64_t Qpc()
		{
			LARGE_INTEGER v{};
			::QueryPerformanceCounter(&v);
			return v.QuadPart;
		}

		bool IsRecordFile(const std::filesystem::path& a_path)
		{
			return a_path.extension() == L".jsonl" && a_path.filename().wstring().starts_with(kFilePrefix);
		}

		// Runs once on the writer thread, right before this run's file is first written.
		// Only touches files of this probe inside its own folder; subfolders are left alone.
		void PrepareFolder()
		{
			const auto dir = g_path.parent_path();
			std::error_code dirEc;
			std::filesystem::create_directories(dir, dirEc);

			std::vector<std::filesystem::path> old;
			std::error_code                    listEc;
			for (std::filesystem::directory_iterator it(dir, listEc), end; !listEc && it != end; it.increment(listEc)) {
				std::error_code fe;
				if (it->is_regular_file(fe) && IsRecordFile(it->path()) && it->path() != g_path) {
					old.push_back(it->path());
				}
			}
			// File names carry the launch time, so name order is age order.
			std::sort(old.begin(), old.end(), [](const auto& a, const auto& b) { return a.filename() > b.filename(); });

			const auto    now = std::filesystem::file_time_type::clock::now();
			std::uint64_t removed = 0;
			std::uint64_t failed = 0;
			for (std::size_t i = kKeepFiles - 1; i < old.size(); ++i) {
				std::error_code fe;
				const auto      written = std::filesystem::last_write_time(old[i], fe);
				if (fe || now - written < kKeepRecent) {
					continue;
				}
				if (std::filesystem::remove(old[i], fe) && !fe) {
					++removed;
				} else {
					++failed;
				}
			}
			Line("probe.files")
				.flag("folderOk", !dirEc)
				.flag("listOk", !listEc)
				.num("previousFiles", static_cast<std::uint64_t>(old.size()))
				.num("removed", removed)
				.num("removeFailed", failed)
				.emit();
		}

		// Runs on the writer thread while no game has started. The lines held in memory are also kept
		// in one fixed file that each launch overwrites, so a launch that dies before reaching a game
		// (a crash while loading plugins or at the main menu) still shows how far it got. The file does
		// not carry the record prefix, so the clean-up above never counts or removes it.
		void MirrorPending()
		{
			std::vector<std::string> fresh;
			{
				std::scoped_lock lock(g_queueLock);
				if (g_queue.size() <= g_mirrored) {
					return;
				}
				fresh.assign(g_queue.begin() + static_cast<std::ptrdiff_t>(g_mirrored), g_queue.end());
			}
			if (!g_startupOpened) {
				std::error_code ec;
				std::filesystem::create_directories(g_startupPath.parent_path(), ec);
			}
			std::ofstream file(g_startupPath, std::ios::binary | (g_startupOpened ? std::ios::app : std::ios::trunc));
			if (!file) {
				return;  // the same lines are tried again on the next pass
			}
			g_startupOpened = true;
			for (const auto& line : fresh) {
				file.write(line.data(), static_cast<std::streamsize>(line.size()));
				file.put('\n');
			}
			g_mirrored += fresh.size();
		}

		void WriterLoop()
		{
			std::vector<std::string> batch;
			for (;;) {
				for (int i = 0; i < 10 && !g_flushSoon.load(); ++i) {
					std::this_thread::sleep_for(50ms);
				}
				g_flushSoon.store(false);
				if (!g_armed.load()) {
					// No game started yet: keep lines in memory so a launch that ends at the main menu
					// leaves no record file of its own.
					MirrorPending();
					std::scoped_lock lock(g_queueLock);
					if (g_queue.size() < kMaxPending) {
						continue;
					}
				}
				if (!g_prepared) {
					g_prepared = true;
					PrepareFolder();
				}
				{
					std::scoped_lock lock(g_queueLock);
					batch.swap(g_queue);
				}
				g_mirrored = 0;  // the queue starts over; only matters if it fills up again before a game starts
				if (batch.empty()) {
					continue;
				}
				std::ofstream file(g_path, std::ios::binary | std::ios::app);
				if (!file) {
					// The file can be locked briefly (antivirus, cloud sync). Keep the lines and retry,
					// but cap the backlog so a permanent failure cannot grow memory without bound.
					++g_openFailures;
					::OutputDebugStringA("TulliusNovelistProbe: cannot open log file, retrying\n");
					std::scoped_lock lock(g_queueLock);
					if (batch.size() + g_queue.size() <= kMaxBacklog) {
						batch.insert(batch.end(), std::make_move_iterator(g_queue.begin()), std::make_move_iterator(g_queue.end()));
						g_queue.swap(batch);
					} else {
						g_droppedLines += batch.size();
					}
					batch.clear();
					continue;
				}
				for (const auto& line : batch) {
					file.write(line.data(), static_cast<std::streamsize>(line.size()));
					file.put('\n');
				}
				batch.clear();
				if (g_openFailures > 0 || g_droppedLines > 0) {
					// Queued like any other line, so it lands after the held-back lines it reports on
					// and the file stays in time order.
					Line("writer.recovered").num("openFailures", g_openFailures).num("droppedLines", g_droppedLines).emit();
					g_openFailures = 0;
					g_droppedLines = 0;
				}
			}
		}
	}

	std::string Escape(std::string_view a_text)
	{
		std::string result;
		result.reserve(a_text.size() + 8);
		for (const unsigned char c : a_text) {
			switch (c) {
			case '"':
				result += "\\\"";
				break;
			case '\\':
				result += "\\\\";
				break;
			case '\n':
				result += "\\n";
				break;
			case '\r':
				result += "\\r";
				break;
			case '\t':
				result += "\\t";
				break;
			default:
				if (c < 0x20) {
					result += std::format("\\u{:04x}", static_cast<unsigned>(c));
				} else {
					result += static_cast<char>(c);
				}
			}
		}
		return result;
	}

	Line::Line(std::string_view a_event)
	{
		_buf.reserve(192);
		_buf += "{\"ev\":\"";
		_buf += Escape(a_event);
		_buf += std::format("\",\"ms\":{:.3f},\"tid\":{},\"gen\":{}", out::ElapsedMs(), ::GetCurrentThreadId(), out::Generation());
	}

	void Line::key(std::string_view a_key)
	{
		_buf += ",\"";
		_buf += a_key;
		_buf += "\":";
	}

	Line& Line::str(std::string_view a_key, std::string_view a_value)
	{
		key(a_key);
		_buf += '"';
		_buf += Escape(a_value);
		_buf += '"';
		return *this;
	}

	Line& Line::num(std::string_view a_key, double a_value)
	{
		key(a_key);
		// Shortest round-trip form keeps full precision for HP/damage comparisons. NaN/inf become null (valid JSON).
		if (std::isfinite(a_value)) {
			_buf += std::format("{}", a_value);
		} else {
			_buf += "null";
		}
		return *this;
	}

	Line& Line::num(std::string_view a_key, std::int64_t a_value)
	{
		key(a_key);
		_buf += std::to_string(a_value);
		return *this;
	}

	Line& Line::num(std::string_view a_key, std::uint64_t a_value)
	{
		key(a_key);
		_buf += std::to_string(a_value);
		return *this;
	}

	Line& Line::flag(std::string_view a_key, bool a_value)
	{
		key(a_key);
		_buf += a_value ? "true" : "false";
		return *this;
	}

	Line& Line::hex(std::string_view a_key, std::uint64_t a_value)
	{
		key(a_key);
		_buf += std::format("\"0x{:X}\"", a_value);
		return *this;
	}

	Line& Line::raw(std::string_view a_key, std::string_view a_json)
	{
		key(a_key);
		_buf += a_json;
		return *this;
	}

	void Line::emit()
	{
		if (_done) {
			return;
		}
		_done = true;
		_buf += '}';
		std::scoped_lock lock(g_queueLock);
		g_queue.push_back(std::move(_buf));
	}

	namespace out
	{
		void Start()
		{
			LARGE_INTEGER f{};
			::QueryPerformanceFrequency(&f);
			g_qpcFreq = f.QuadPart > 0 ? f.QuadPart : 1;
			g_qpcStart = Qpc();

			// Own folder under SKSE's log folder, created on the first write.
			auto       dir = SKSE::log::log_directory();
			const auto base = (dir ? *dir : std::filesystem::current_path()) / L"TulliusNovelist";
			// Local launch time through the C runtime: std::chrono::current_zone() throws where the
			// system has no time zone database, and this runs while the game loads the plugin.
			const std::time_t now = std::time(nullptr);
			std::tm           local{};
			char              stamp[32] = "00000000-000000";
			if (::localtime_s(&local, &now) == 0) {
				std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
			}
			g_path = base / std::format("TulliusNovelistProbe-{}.jsonl", stamp);
			g_startupPath = base / L"startup-last.jsonl";

			std::thread(WriterLoop).detach();
		}

		void Arm()
		{
			if (!g_armed.exchange(true)) {
				g_flushSoon.store(true);
			}
		}

		void FlushSoon() { g_flushSoon.store(true); }

		double ElapsedMs()
		{
			return static_cast<double>(Qpc() - g_qpcStart) * 1000.0 / static_cast<double>(g_qpcFreq);
		}

		std::uint32_t Generation() { return g_generation.load(); }

		void BumpGeneration() { g_generation.fetch_add(1); }
	}

	namespace perf
	{
		namespace
		{
			struct Stat
			{
				std::uint64_t count{ 0 };
				double        totalUs{ 0 };
				double        maxUs{ 0 };
				std::vector<float> samples;  // bounded reservoir for percentiles
			};

			std::mutex                                   g_perfLock;
			std::unordered_map<std::string_view, Stat>   g_stats;
		}

		Scope::Scope(const char* a_name) :
			_name(a_name), _start(Qpc())
		{}

		Scope::~Scope()
		{
			const double us = static_cast<double>(Qpc() - _start) * 1'000'000.0 / static_cast<double>(g_qpcFreq);
			std::scoped_lock lock(g_perfLock);
			auto& s = g_stats[_name];
			++s.count;
			s.totalUs += us;
			if (us > s.maxUs) {
				s.maxUs = us;
			}
			if (s.samples.size() < 4096) {
				s.samples.push_back(static_cast<float>(us));
			} else {
				s.samples[s.count % 4096] = static_cast<float>(us);
			}
		}

		void Dump()
		{
			std::unordered_map<std::string_view, Stat> copy;
			{
				std::scoped_lock lock(g_perfLock);
				copy.swap(g_stats);
			}
			for (auto& [name, s] : copy) {
				auto pct = [&](double p) -> double {
					if (s.samples.empty()) {
						return 0.0;
					}
					auto v = s.samples;
					const auto idx = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
					std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(idx), v.end());
					return v[idx];
				};
				Line("perf")
					.str("name", name)
					.num("count", s.count)
					.num("avgUs", s.count ? s.totalUs / static_cast<double>(s.count) : 0.0)
					.num("p50Us", pct(0.50))
					.num("p95Us", pct(0.95))
					.num("p99Us", pct(0.99))
					.num("maxUs", s.maxUs)
					.emit();
			}
		}
	}
}
