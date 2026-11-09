#pragma once

// JSONL output for the probe. One line per observation, written by a background thread.
// Every line carries: ev (kind), ms (steady clock since plugin load), tid (OS thread id), gen (load generation).
namespace probe
{
	class Line
	{
	public:
		explicit Line(std::string_view a_event);

		Line& str(std::string_view a_key, std::string_view a_value);
		Line& num(std::string_view a_key, double a_value);
		Line& num(std::string_view a_key, std::int64_t a_value);
		Line& num(std::string_view a_key, std::uint64_t a_value);
		Line& num(std::string_view a_key, std::uint32_t a_value) { return num(a_key, static_cast<std::uint64_t>(a_value)); }
		Line& num(std::string_view a_key, std::int32_t a_value) { return num(a_key, static_cast<std::int64_t>(a_value)); }
		Line& flag(std::string_view a_key, bool a_value);
		Line& hex(std::string_view a_key, std::uint64_t a_value);
		// Inserts an already-formatted JSON value (object or array) as-is.
		Line& raw(std::string_view a_key, std::string_view a_json);

		// Finishes the line and hands it to the writer. Safe from any thread.
		void emit();

	private:
		void key(std::string_view a_key);
		std::string _buf;
		bool        _done{ false };
	};

	std::string Escape(std::string_view a_text);

	namespace out
	{
		// Picks this run's file name. Nothing is written until Arm().
		void Start();
		// A game is starting (load or new game): create the file, write everything held so far, and keep writing.
		void Arm();
		void FlushSoon();
		double ElapsedMs();
		std::uint32_t Generation();
		void BumpGeneration();
	}

	// Per-callback timing. Aggregated in memory, dumped by the poller as "perf" lines.
	namespace perf
	{
		struct Scope
		{
			explicit Scope(const char* a_name);
			~Scope();
			Scope(const Scope&) = delete;
			Scope& operator=(const Scope&) = delete;

		private:
			const char*   _name;
			std::int64_t  _start;
		};

		void Dump();
	}
}
