// Tests for probe/src/Text.cpp without the game. Build and run:
//   cmake --build --preset vs2022-release --target probe_text_test
//   G:/skyrim-build/TulliusNovelist/build/<preset dir>/probe_text_test.exe
// package_probe runs it before making the zip. Exit code 0 means every check passed.

#include "Text.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

namespace
{
	int g_failed = 0;
	int g_passed = 0;

	void Check(bool a_ok, const char* a_what)
	{
		std::printf("%s  %s\n", a_ok ? "ok  " : "FAIL", a_what);
		if (a_ok) {
			++g_passed;
		} else {
			++g_failed;
		}
	}

	using namespace probe::text;

	std::string Bytes(const char* a_text, std::size_t a_size) { return std::string(a_text, a_size); }

	// Percent-decoding of KeyText output, as a consumer would do it.
	std::string DecodeKey(std::string_view a_key)
	{
		const auto hex = [](char c) -> int {
			if (c >= '0' && c <= '9') {
				return c - '0';
			}
			if (c >= 'A' && c <= 'F') {
				return c - 'A' + 10;
			}
			return -1;
		};
		std::string out;
		for (std::size_t i = 0; i < a_key.size(); ++i) {
			if (a_key[i] == '%' && i + 2 < a_key.size() && hex(a_key[i + 1]) >= 0 && hex(a_key[i + 2]) >= 0) {
				out += static_cast<char>(hex(a_key[i + 1]) * 16 + hex(a_key[i + 2]));
				i += 2;
			} else {
				out += a_key[i];
			}
		}
		return out;
	}

	void TestUtf8()
	{
		Check(IsUtf8(""), "empty text is UTF-8");
		Check(IsUtf8("TrueHUD"), "ASCII is UTF-8");
		Check(IsUtf8("\xEB\x93\x9C\xEB\x9E\x98\xEA\xB3\xA4"), "Korean (3 syllables) is UTF-8");
		const char* valid[] = { "\xC2\x80", "\xDF\xBF", "\xE0\xA0\x80", "\xED\x9F\xBF", "\xEE\x80\x80",
			"\xEF\xBF\xBF", "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF" };
		bool allValid = true;
		for (const auto* v : valid) {
			allValid = allValid && IsUtf8(v);
		}
		Check(allValid, "boundary sequences (C2 80 .. F4 8F BF BF) are UTF-8");
		const char* invalid[] = { "\xA5", "\xC0\x80", "\xC1\xBF", "\xE0\x9F\xBF", "\xED\xA0\x80",
			"\xF0\x8F\xBF\xBF", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xFF", "\xEB\x93" };
		bool allInvalid = true;
		for (const auto* v : invalid) {
			allInvalid = allInvalid && !IsUtf8(v);
		}
		Check(allInvalid, "stray, overlong, surrogate, out-of-range and cut sequences are not UTF-8");
		Check(Utf8SequenceLength("", 0) == 0 && Utf8SequenceLength("a", 5) == 0, "position outside the text gives 0");
	}

	void TestEscape()
	{
		Check(JsonEscape("a\"b\\c") == "a\\\"b\\\\c", "quote and backslash escaped");
		Check(JsonEscape("\n\r\t\x01") == "\\n\\r\\t\\u0001", "C0 controls escaped");
		Check(JsonEscape(Bytes("a\0b", 3)) == "a\\u0000b", "embedded zero escaped");
		Check(JsonEscape("\x7F") == "\\u007f", "DEL escaped");
		Check(JsonEscape("\xC2\x85") == "\\u0085", "C1 control (NEL) escaped");
		Check(JsonEscape("\xE2\x80\xA8\xE2\x80\xA9") == "\\u2028\\u2029", "line and paragraph separators escaped");
		Check(JsonEscape("\xC2\xA0\xEB\x93\x9C\xF0\x9F\x98\x80") == "\xC2\xA0\xEB\x93\x9C\xF0\x9F\x98\x80", "other UTF-8 kept as is");
		// The menu name written by 0.0.8 on 2026-11-23 (20:32 record): 20 67 A5 5D 9B 01.
		const auto broken = Bytes("\x20\x67\xA5\x5D\x9B\x01", 6);
		const auto escaped = JsonEscape(broken);
		Check(escaped == " g\\ufffd]\\ufffd\\u0001", "broken menu name becomes valid JSON text");
		Check(IsUtf8(escaped), "escaped broken name is UTF-8");
		Check(JsonEscape("\xEB\x93") == "\\ufffd\\ufffd", "each byte of a cut-off sequence replaced");
		Check(HexBytes(broken, 16) == "20 67 A5 5D 9B 01", "hex bytes");
		Check(HexBytes(broken, 2) == "20 67", "hex bytes limited");
	}

	void TestCleanText()
	{
		Check(IsCleanText("TrueHUD") && IsCleanText("") && IsCleanText("\xEB\x93\x9C"), "names and Korean are clean");
		Check(IsCleanText("a\tb\nc\rd"), "tab, line feed and carriage return are clean");
		Check(IsCleanText("\xC2\xA0"), "U+00A0 is clean");
		Check(!IsCleanText("a\x01"), "C0 control is not clean");
		Check(!IsCleanText("a\x7F"), "DEL is not clean");
		Check(!IsCleanText("\xC2\x85"), "C1 control is not clean");
		Check(!IsCleanText("\xA5"), "invalid UTF-8 is not clean");
	}

	void TestKeyText()
	{
		Check(KeyText("Skyrim.esm") == "Skyrim.esm", "ASCII key text unchanged");
		Check(KeyText("\xEB\x93\x9C.esp") == "\xEB\x93\x9C.esp", "UTF-8 key text unchanged");
		Check(KeyText("100%.esp") == "100%25.esp", "percent sign in key text escaped");
		// CP949 file names. \xC7\xB0 alone is also well-formed UTF-8 (U+01F0); in a name that is
		// not UTF-8 as a whole it must still be encoded, not shown as that letter.
		Check(KeyText("\xC7\xD1.esp") == "%C7%D1.esp", "non-UTF-8 name bytes kept as %XX");
		Check(KeyText("\xC7\xB0\xC7\xD1.esp") == "%C7%B0%C7%D1.esp", "every high byte of a non-UTF-8 name encoded");
		Check(KeyText("\xC7\xD1.esp") != KeyText("\xB1\xDB.esp"), "different non-UTF-8 names give different key text");
		Check(KeyText("%C7%D1.esp") != KeyText("\xC7\xD1.esp"), "literal %XX in a name never matches encoded bytes");
		const std::string samples[] = { "Skyrim.esm", "100%.esp", "\xEB\x93\x9C.esp", "\xC7\xD1.esp",
			"\xC7\xB0\xC7\xD1.esp", "%C7%D1.esp", Bytes("a%\xEB\x93\x9C\xC7z", 7) };
		bool roundTrip = true;
		bool validJson = true;
		for (const auto& sample : samples) {
			const auto key = KeyText(sample);
			roundTrip = roundTrip && DecodeKey(key) == sample;
			validJson = validJson && IsUtf8(key);
		}
		Check(roundTrip, "decoding %XX gives back the original bytes");
		Check(validJson, "key text is always UTF-8");
	}

	void TestClassify()
	{
		Check(Classify(Read::ok, "TrueHUD", true) == nullptr, "clean text is not bad");
		Check(std::strcmp(Classify(Read::ok, "", true), "empty") == 0, "empty text is bad where empty is unexpected");
		Check(Classify(Read::ok, "", false) == nullptr, "empty text is fine where it is expected");
		Check(std::strcmp(Classify(Read::ok, "a\x7F", false), "notText") == 0, "DEL makes text notText");
		Check(std::strcmp(Classify(Read::ok, Bytes("\x20\x67\xA5\x5D\x9B\x01", 6), true), "notText") == 0, "broken menu name is notText");
		Check(std::strcmp(Classify(Read::unreadable, "", false), "unreadable") == 0, "unreadable");
		Check(std::strcmp(Classify(Read::unterminated, "abc", false), "unterminated") == 0, "unterminated");
		Check(Classify(Read::tooLong, "ab\xEB\x93", false) == nullptr, "text cut inside a sequence at the limit is not bad");
		Check(WithoutCutSequence("ab\xEB\x93") == "ab", "cut sequence dropped");
		Check(WithoutCutSequence("ab\xEB\x93\x9C") == "ab\xEB\x93\x9C" && WithoutCutSequence("abc") == "abc", "complete text kept");
		Check(WithoutCutSequence("a\xF0") == "a" && WithoutCutSequence("a\xF0\x9F") == "a" && WithoutCutSequence("a\xF0\x9F\x98") == "a",
			"4-byte sequence cut after 1, 2 or 3 bytes dropped");
		Check(WithoutCutSequence("a\xFF") == "a\xFF" && WithoutCutSequence("a\xC0") == "a\xC0" && WithoutCutSequence("a\xE0\x80") == "a\xE0\x80",
			"bytes that cannot start a sequence are kept");
		Check(std::strcmp(Classify(Read::tooLong, "ab\xFF", false), "notText") == 0, "cut text ending in an invalid byte is notText");
		Check(std::strcmp(Classify(Read::tooLong, "a\x01\xEB\x93", false), "notText") == 0, "cut text with a control character is notText");
	}

	void TestProtection()
	{
		Check(IsReadableProtection(PAGE_READWRITE) && IsReadableProtection(PAGE_READONLY) && IsReadableProtection(PAGE_EXECUTE_READ),
			"readable protections");
		Check(!IsReadableProtection(PAGE_READWRITE | PAGE_GUARD) && !IsReadableProtection(PAGE_NOACCESS) &&
				  !IsReadableProtection(PAGE_EXECUTE) && !IsReadableProtection(0),
			"guard, no-access, execute-only and none are not readable");
	}

	void TestCopy()
	{
		std::string out = "left over";
		Check(CopyCString(nullptr, 256, out) == Read::unreadable && out.empty(), "null address is unreadable and clears the output");
		Check(CopyCString("", 256, out) == Read::ok && out.empty(), "empty string copied");
		Check(CopyCString("TrueHUD", 256, out) == Read::ok && out == "TrueHUD", "normal string copied");
		Check(CopyCString("abcd", 4, out) == Read::tooLong && out == "abcd", "string as long as the limit is tooLong");
		Check(CopyCString("abcd", 5, out) == Read::ok && out == "abcd", "string one shorter than the limit is ok");
		Check(CopyCString("abcd", 0, out) == Read::tooLong && out.empty(), "limit 0 reads nothing");
		const std::string longText(3000, 'z');
		Check(CopyCString(longText.c_str(), 4096, out) == Read::ok && out == longText, "long string copied in several chunks");
		Check(CopyBytes("ab\0cd", 5) == Bytes("ab\0cd", 5), "CopyBytes keeps zeros");
		Check(CopyBytes(nullptr, 5).empty(), "CopyBytes of null is empty");

		// Pages: [0] read-write, [1] reserved only, then [2] read-write, [3] guard, [4] no-access.
		SYSTEM_INFO info{};
		::GetSystemInfo(&info);
		const std::size_t page = info.dwPageSize;
		auto* base = static_cast<char*>(::VirtualAlloc(nullptr, page * 5, MEM_RESERVE, PAGE_NOACCESS));
		if (!base) {
			Check(false, "test pages reserved");
			return;
		}
		const bool committed = ::VirtualAlloc(base, page, MEM_COMMIT, PAGE_READWRITE) != nullptr &&
		                       ::VirtualAlloc(base + page * 2, page, MEM_COMMIT, PAGE_READWRITE) != nullptr &&
		                       ::VirtualAlloc(base + page * 3, page, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD) != nullptr &&
		                       ::VirtualAlloc(base + page * 4, page, MEM_COMMIT, PAGE_NOACCESS) != nullptr;
		Check(committed, "test pages committed");
		if (!committed) {
			::VirtualFree(base, 0, MEM_RELEASE);
			return;
		}

		// A string filling the end of page 0 without a zero stops at the reserved page 1.
		char* tail = base + page - 8;
		std::memset(tail, 'x', 8);
		Check(CopyCString(tail, 256, out) == Read::unterminated && out == "xxxxxxxx", "string running into an unreadable page stops at the page end");
		Check(CopyBytes(tail, 32) == "xxxxxxxx", "CopyBytes stops at the unreadable page");
		Check(CopyCString(base + page, 256, out) == Read::unreadable && out.empty(), "reserved page is unreadable");

		// The guard page must be refused without being touched (touching it would clear the guard).
		char* beforeGuard = base + page * 3 - 4;
		std::memset(beforeGuard, 'g', 4);
		Check(CopyCString(beforeGuard, 256, out) == Read::unterminated && out == "gggg", "string running into a guard page stops before it");
		MEMORY_BASIC_INFORMATION guard{};
		::VirtualQuery(base + page * 3, &guard, sizeof(guard));
		Check((guard.Protect & PAGE_GUARD) != 0, "guard page left untouched");
		Check(CopyCString(base + page * 4, 256, out) == Read::unreadable, "committed no-access page is unreadable");

		// Same string across two readable pages: make page 1 readable, the zero right at its start.
		Check(::VirtualAlloc(base + page, page, MEM_COMMIT, PAGE_READWRITE) != nullptr, "page 1 committed");
		base[page] = '\0';
		Check(CopyCString(tail, 256, out) == Read::ok && out == "xxxxxxxx", "zero at the start of the next page ends the string");
		base[page] = 'y';
		base[page + 1] = '\0';
		Check(CopyCString(tail, 256, out) == Read::ok && out == "xxxxxxxxy", "string spanning two readable pages copied");

		// Two readable regions in a row (page 1 read-only after read-write page 0): the copy goes
		// on into the next region after the cached region end.
		DWORD before = 0;
		Check(::VirtualProtect(base + page, page, PAGE_READONLY, &before) != 0, "page 1 made read-only");
		MEMORY_BASIC_INFORMATION first{};
		::VirtualQuery(base, &first, sizeof(first));
		Check(first.RegionSize == page, "pages 0 and 1 are separate regions");
		Check(CopyCString(tail, 256, out) == Read::ok && out == "xxxxxxxxy", "string spanning two readable regions copied");

		// Execute-only memory is committed but not readable.
		DWORD old = 0;
		Check(::VirtualProtect(base + page, page, PAGE_EXECUTE, &old) != 0, "page 1 made execute-only");
		Check(CopyCString(tail, 256, out) == Read::unterminated && out == "xxxxxxxx", "execute-only page is not read");

		::VirtualFree(base, 0, MEM_RELEASE);
	}

	struct Recorder final : CallObserver
	{
		std::vector<CallInfo> calls;
		void OnCall(const CallInfo& a_call) override { calls.push_back(a_call); }

		bool AllTimed() const
		{
			for (const auto& call : calls) {
				if (!(call.us >= 0.0)) {
					return false;
				}
			}
			return true;
		}
	};

	void TestObserver()
	{
		// Pages: [0] and [1] read-write (one region), [2] reserved only.
		SYSTEM_INFO info{};
		::GetSystemInfo(&info);
		const std::size_t page = info.dwPageSize;
		auto* base = static_cast<char*>(::VirtualAlloc(nullptr, page * 3, MEM_RESERVE, PAGE_NOACCESS));
		if (!base) {
			Check(false, "observer test pages reserved");
			return;
		}
		if (::VirtualAlloc(base, page * 2, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
			Check(false, "observer test pages committed");
			::VirtualFree(base, 0, MEM_RELEASE);
			return;
		}
		const auto address = [](const void* a_ptr) { return reinterpret_cast<std::uintptr_t>(a_ptr); };
		std::string out;

		std::memcpy(base, "TrueHUD", 8);
		Recorder shortText;
		Check(CopyCString(base, 256, out, &shortText) == Read::ok && out == "TrueHUD", "string read with an observer is the same");
		Check(shortText.calls.size() == 2 && shortText.calls[0].call == Call::query && shortText.calls[1].call == Call::copy,
			"string inside one page: one query, one copy");
		if (shortText.calls.size() == 2) {
			const auto& query = shortText.calls[0];
			const auto& copy = shortText.calls[1];
			Check(query.ok && query.address == address(base) && query.allocationBase == address(base) &&
					  query.regionBase == address(base) && query.regionSize == page * 2 && query.state == MEM_COMMIT &&
					  query.protect == PAGE_READWRITE && query.type == MEM_PRIVATE && query.bytes == 0,
				"query reports the region VirtualQuery gave");
			Check(copy.ok && copy.address == address(base) && copy.bytes == 256 && copy.allocationBase == 0 && copy.regionSize == 0,
				"copy reports its chunk and no region");
		}
		Check(shortText.AllTimed(), "every call has a time");

		// Across the page boundary inside the region: still one query, then a copy per page.
		char* tail = base + page - 8;
		std::memset(tail, 'x', 8);
		std::memcpy(base + page, "y", 2);
		Recorder twoPages;
		Check(CopyCString(tail, 256, out, &twoPages) == Read::ok && out == "xxxxxxxxy", "string across two pages read with an observer");
		Check(twoPages.calls.size() == 3 && twoPages.calls[0].call == Call::query && twoPages.calls[1].call == Call::copy &&
				  twoPages.calls[1].bytes == 8 && twoPages.calls[2].call == Call::copy && twoPages.calls[2].bytes == 248 &&
				  twoPages.calls[2].address == address(base + page),
			"string across a page boundary: one query, two copies");

		// Reserved memory: the query fails and nothing is copied.
		Recorder reserved;
		Check(CopyCString(base + page * 2, 256, out, &reserved) == Read::unreadable, "reserved page unreadable with an observer");
		Check(reserved.calls.size() == 1 && reserved.calls[0].call == Call::query && !reserved.calls[0].ok &&
				  reserved.calls[0].state == MEM_RESERVE && reserved.calls[0].allocationBase == address(base),
			"reserved page: one failed query that still says which allocation it is");

		Recorder bytes;
		Check(CopyBytes(base, 4, &bytes) == "True", "CopyBytes with an observer");
		Check(bytes.calls.size() == 2 && bytes.calls[0].call == Call::query && bytes.calls[1].call == Call::copy && bytes.calls[1].bytes == 4,
			"CopyBytes: one query, one copy of the bytes asked for");

		Recorder none;
		Check(CopyCString(nullptr, 256, out, &none) == Read::unreadable && none.calls.empty(), "null address makes no call");

		// Smaller copies: same text, more copy calls of at most the size asked for.
		const std::string longText(600, 'q');
		std::memcpy(base, longText.c_str(), longText.size() + 1);
		Recorder shortCopies;
		Check(CopyCString(base, 4096, out, &shortCopies, 256) == Read::ok && out == longText, "text read in 256-byte copies is the same");
		Check(shortCopies.calls.size() == 4 && shortCopies.calls[1].bytes == 256 && shortCopies.calls[2].bytes == 256 &&
				  shortCopies.calls[3].bytes == 256,
			"600 bytes and a zero in 256-byte copies: one query, three copies");
		Recorder defaultCopies;
		Check(CopyCString(base, 4096, out, &defaultCopies, 0) == Read::ok && out == longText && defaultCopies.calls.size() == 3 &&
				  defaultCopies.calls[1].bytes == kMaxChunk,
			"chunk 0 means the largest chunk");
		Recorder over;
		Check(CopyCString(base, 4096, out, &over, 100000) == Read::ok && over.calls.size() == 3 && over.calls[1].bytes == kMaxChunk,
			"a chunk above the largest is cut to it");
		Check(CopyCString(base, 4096, out, nullptr, 1) == Read::ok && out == longText, "one-byte copies still give the text");

		// The length limit shortens the last copy: 257 bytes in 256-byte copies are 256 and 1.
		Recorder cut;
		Check(CopyCString(base, 257, out, &cut, 256) == Read::tooLong && out == longText.substr(0, 257), "text cut at 257 bytes");
		Check(cut.calls.size() == 3 && cut.calls[0].call == Call::query && cut.calls[1].bytes == 256 && cut.calls[2].call == Call::copy &&
				  cut.calls[2].bytes == 1,
			"limit of 257 in 256-byte copies: one query, copies of 256 and 1");
		Recorder nothing;
		Check(CopyCString(base, 0, out, &nothing) == Read::tooLong && out.empty() && nothing.calls.empty(), "limit 0 makes no call");

		Recorder manyBytes;
		Check(CopyBytes(base, 600, &manyBytes) == longText, "CopyBytes of 600 bytes with an observer");
		Check(manyBytes.calls.size() == 3 && manyBytes.calls[1].bytes == kMaxChunk && manyBytes.calls[2].bytes == 600 - kMaxChunk,
			"CopyBytes of 600 bytes: one query, copies of 512 and 88");
		Recorder nullBytes;
		Check(CopyBytes(nullptr, 16, &nullBytes).empty() && nullBytes.calls.empty(), "CopyBytes of a null address makes no call");

		// A string that runs into the reserved page: query, copy, then a failed query.
		std::memset(base + page * 2 - 8, 'r', 8);
		Recorder runsOut;
		Check(CopyCString(base + page * 2 - 8, 256, out, &runsOut) == Read::unterminated && out == "rrrrrrrr", "string into reserved memory with an observer");
		Check(runsOut.calls.size() == 3 && runsOut.calls[0].call == Call::query && runsOut.calls[1].call == Call::copy &&
				  runsOut.calls[1].bytes == 8 && runsOut.calls[2].call == Call::query && !runsOut.calls[2].ok,
			"string into reserved memory: query, copy, failed query");

		// Two regions in one allocation (page 1 read-only): two queries, same allocation, different region ends.
		DWORD before = 0;
		Check(::VirtualProtect(base + page, page, PAGE_READONLY, &before) != 0, "observer page 1 made read-only");
		Recorder regions;
		Check(CopyCString(tail, 256, out, &regions) == Read::ok && out == "xxxxxxxxy", "string across two regions read with an observer");
		Check(regions.calls.size() == 4 && regions.calls[0].call == Call::query && regions.calls[2].call == Call::query &&
				  regions.calls[0].allocationBase == regions.calls[2].allocationBase &&
				  regions.calls[0].regionBase + regions.calls[0].regionSize == address(base + page) &&
				  regions.calls[2].regionBase == address(base + page) && regions.calls[2].protect == PAGE_READONLY,
			"string across two regions: a query per region, same allocation");

		// Timing is real: many calls together take some time.
		Recorder many;
		for (int i = 0; i < 200; ++i) {
			CopyCString(base, 4096, out, &many);
		}
		double total = 0.0;
		for (const auto& call : many.calls) {
			total += call.us;
		}
		Check(many.AllTimed() && total > 0.0, "calls are timed (200 reads take more than zero time)");

		// Committed but not readable: a no-access page and a guard page. One failed query each,
		// state MEM_COMMIT and the protection as it is; the guard page is not touched.
		auto* locked = static_cast<char*>(::VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
		DWORD unused = 0;
		if (!locked || ::VirtualProtect(locked + page, page, PAGE_READWRITE | PAGE_GUARD, &unused) == 0) {
			Check(false, "no-access and guard pages made");
		} else {
			Recorder noAccess;
			Check(CopyCString(locked, 256, out, &noAccess) == Read::unreadable && noAccess.calls.size() == 1 &&
					  !noAccess.calls[0].ok && noAccess.calls[0].state == MEM_COMMIT && noAccess.calls[0].protect == PAGE_NOACCESS,
				"no-access page: one failed query, committed, PAGE_NOACCESS");
			Recorder guarded;
			Check(CopyCString(locked + page, 256, out, &guarded) == Read::unreadable && guarded.calls.size() == 1 &&
					  !guarded.calls[0].ok && guarded.calls[0].state == MEM_COMMIT && (guarded.calls[0].protect & PAGE_GUARD) != 0,
				"guard page: one failed query, committed, PAGE_GUARD kept in protect");
			MEMORY_BASIC_INFORMATION guard{};
			Check(::VirtualQuery(locked + page, &guard, sizeof(guard)) == sizeof(guard) && (guard.Protect & PAGE_GUARD) != 0,
				"guard page still a guard page after the observed read");
		}
		if (locked) {
			::VirtualFree(locked, 0, MEM_RELEASE);
		}

		::VirtualFree(base, 0, MEM_RELEASE);

		// Freed memory: the query answers MEM_FREE (its other fields are undefined) and nothing is copied.
		Recorder freed;
		Check(CopyCString(base, 256, out, &freed) == Read::unreadable, "freed memory unreadable with an observer");
		Check(freed.calls.size() == 1 && freed.calls[0].call == Call::query && !freed.calls[0].ok && freed.calls[0].state == MEM_FREE,
			"freed memory: one failed query with state MEM_FREE");

		// An address VirtualQuery does not answer for (kernel space): one failed query, all fields 0.
		const auto* kernel = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(0xFFFF'8000'0000'0000ull));
		Recorder noAnswer;
		Check(CopyCString(kernel, 256, out, &noAnswer) == Read::unreadable, "kernel address unreadable");
		Check(noAnswer.calls.size() == 1 && !noAnswer.calls[0].ok && noAnswer.calls[0].state == 0 && noAnswer.calls[0].allocationBase == 0 &&
				  noAnswer.calls[0].regionBase == 0 && noAnswer.calls[0].regionSize == 0 && noAnswer.calls[0].protect == 0 &&
				  noAnswer.calls[0].type == 0,
			"unanswered query: one call, every region field 0");
	}
}

int main()
{
	TestUtf8();
	TestEscape();
	TestCleanText();
	TestKeyText();
	TestClassify();
	TestProtection();
	TestCopy();
	TestObserver();
	std::printf("%s (%d passed, %d failed)\n", g_failed == 0 ? "all passed" : "FAILED", g_passed, g_failed);
	return g_failed == 0 ? 0 : 1;
}
