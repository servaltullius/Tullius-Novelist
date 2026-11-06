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
		constexpr std::size_t    kMaxBacklog = 200'000;
		std::uint64_t            g_openFailures = 0;  // writer thread only
		std::uint64_t            g_droppedLines = 0;  // writer thread only

		std::int64_t Qpc()
		{
			LARGE_INTEGER v{};
			::QueryPerformanceCounter(&v);
			return v.QuadPart;
		}

		void WriterLoop()
		{
			std::vector<std::string> batch;
			for (;;) {
				for (int i = 0; i < 10 && !g_flushSoon.load(); ++i) {
					std::this_thread::sleep_for(50ms);
				}
				g_flushSoon.store(false);
				{
					std::scoped_lock lock(g_queueLock);
					batch.swap(g_queue);
				}
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
				if (g_openFailures > 0 || g_droppedLines > 0) {
					const auto note = std::format(R"({{"ev":"writer.recovered","ms":{:.3f},"openFailures":{},"droppedLines":{}}})",
						out::ElapsedMs(), g_openFailures, g_droppedLines);
					file.write(note.data(), static_cast<std::streamsize>(note.size()));
					file.put('\n');
					g_openFailures = 0;
					g_droppedLines = 0;
				}
				for (const auto& line : batch) {
					file.write(line.data(), static_cast<std::streamsize>(line.size()));
					file.put('\n');
				}
				batch.clear();
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

			auto dir = SKSE::log::log_directory();
			const auto base = dir ? *dir : std::filesystem::current_path();
			std::error_code ec;
			std::filesystem::create_directories(base, ec);
			const auto now = std::chrono::system_clock::now();
			const auto local = std::chrono::zoned_time{ std::chrono::current_zone(), now }.get_local_time();
			g_path = base / std::format("TulliusNovelistProbe-{:%Y%m%d-%H%M%S}.jsonl", std::chrono::floor<std::chrono::seconds>(local));

			std::thread(WriterLoop).detach();
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
