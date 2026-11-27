// Tests for probe/src/Text.cpp without the game. Build and run:
//   cmake --build --preset vs2022-release --target probe_text_test
//   G:/skyrim-build/TulliusNovelist/build/<preset dir>/probe_text_test.exe
// package_probe runs it before making the zip. Exit code 0 means every check passed.

#include "Text.h"

#include <algorithm>
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
		// on into the next page whatever its protection, as long as it can be read.
		DWORD before = 0;
		Check(::VirtualProtect(base + page, page, PAGE_READONLY, &before) != 0, "page 1 made read-only");
		MEMORY_BASIC_INFORMATION first{};
		::VirtualQuery(base, &first, sizeof(first));
		Check(first.RegionSize == page, "pages 0 and 1 are separate regions");
		Check(CopyCString(tail, 256, out) == Read::ok && out == "xxxxxxxxy", "string spanning two readable regions copied");

		// Execute-only memory: on this PC (Windows 11 26300), ReadProcessMemory reads PAGE_EXECUTE
		// pages, while hardware enforcing execute-only would refuse them. Either way it must not
		// crash and must return ok (read) or unterminated (stopped before page 1).
		DWORD old = 0;
		Check(::VirtualProtect(base + page, page, PAGE_EXECUTE, &old) != 0, "page 1 made execute-only");
		const auto execRead = CopyCString(tail, 256, out);
		Check((execRead == Read::ok && out == "xxxxxxxxy") || (execRead == Read::unterminated && out == "xxxxxxxx"),
			"execute-only page does not crash (ok if readable, unterminated if refused)");

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
		const auto  address = [](const void* a_ptr) { return reinterpret_cast<std::uintptr_t>(a_ptr); };
		std::string out;

		std::memcpy(base, "TrueHUD", 8);
		Recorder shortText;
		Check(CopyCString(base, 256, out, &shortText) == Read::ok && out == "TrueHUD", "string read with an observer is the same");
		Check(shortText.calls.size() == 1, "string inside one page: one copy");
		if (shortText.calls.size() == 1) {
			const auto& copy = shortText.calls[0];
			Check(copy.ok && copy.address == address(base) && copy.bytes == 256, "copy reports ok, address and chunk size");
		}
		Check(shortText.AllTimed(), "every call has a time");

		// Across the page boundary: one copy per page, never crossing the page end.
		char* tail = base + page - 8;
		std::memset(tail, 'x', 8);
		std::memcpy(base + page, "y", 2);
		Recorder twoPages;
		Check(CopyCString(tail, 256, out, &twoPages) == Read::ok && out == "xxxxxxxxy", "string across two pages read with an observer");
		Check(twoPages.calls.size() == 2 && twoPages.calls[0].ok && twoPages.calls[0].address == address(tail) &&
				  twoPages.calls[0].bytes == 8 && twoPages.calls[1].ok && twoPages.calls[1].address == address(base + page) &&
				  twoPages.calls[1].bytes == 248,
			"string across a page boundary: two copies");

		// Reserved memory: one failed copy at that address.
		Recorder reserved;
		Check(CopyCString(base + page * 2, 256, out, &reserved) == Read::unreadable, "reserved page unreadable with an observer");
		Check(reserved.calls.size() == 1 && !reserved.calls[0].ok && reserved.calls[0].address == address(base + page * 2) &&
				  reserved.calls[0].bytes == 256,
			"reserved page: one failed copy");

		Recorder bytes;
		Check(CopyBytes(base, 4, &bytes) == "True", "CopyBytes with an observer");
		Check(bytes.calls.size() == 1 && bytes.calls[0].ok && bytes.calls[0].address == address(base) && bytes.calls[0].bytes == 4,
			"CopyBytes: one copy of the bytes asked for");

		Recorder none;
		Check(CopyCString(nullptr, 256, out, &none) == Read::unreadable && none.calls.empty(), "null address makes no call");

		// Smaller copies: same text, more copy calls of at most the size asked for.
		const std::string longText(600, 'q');
		std::memcpy(base, longText.c_str(), longText.size() + 1);
		// A chunk below kMaxChunk is used as asked: 600 bytes and the zero after them take seven
		// 100-byte copies (the zero is the first byte of the seventh).
		Recorder shortCopies;
		Check(CopyCString(base, 4096, out, &shortCopies, 100) == Read::ok && out == longText, "text read in 100-byte copies is the same");
		bool hundredEach = shortCopies.calls.size() == 7;
		for (std::size_t i = 0; hundredEach && i < shortCopies.calls.size(); ++i) {
			hundredEach = shortCopies.calls[i].ok && shortCopies.calls[i].bytes == 100 && shortCopies.calls[i].address == address(base + i * 100);
		}
		Check(hundredEach, "600 bytes and a zero in 100-byte copies: seven copies of 100 bytes, one after another");
		Recorder defaultCopies;
		Check(CopyCString(base, 4096, out, &defaultCopies, 0) == Read::ok && out == longText && defaultCopies.calls.size() == 3 &&
				  defaultCopies.calls[0].bytes == kMaxChunk && defaultCopies.calls[1].bytes == kMaxChunk &&
				  defaultCopies.calls[2].bytes == kMaxChunk,
			"chunk 0 means the largest chunk");
		Recorder over;
		Check(CopyCString(base, 4096, out, &over, 100000) == Read::ok && over.calls.size() == 3 && over.calls[0].bytes == kMaxChunk,
			"a chunk above the largest is cut to it");
		Check(CopyCString(base, 4096, out, nullptr, 1) == Read::ok && out == longText, "one-byte copies still give the text");

		// The length limit shortens the last copy: 257 bytes in 256-byte copies are 256 and 1.
		Recorder cut;
		Check(CopyCString(base, 257, out, &cut, 256) == Read::tooLong && out == longText.substr(0, 257), "text cut at 257 bytes");
		Check(cut.calls.size() == 2 && cut.calls[0].ok && cut.calls[0].bytes == 256 && cut.calls[1].ok &&
				  cut.calls[1].address == address(base + 256) && cut.calls[1].bytes == 1,
			"limit of 257 in 256-byte copies: copies of 256 and 1");
		Recorder cutSmall;
		Check(CopyCString(base, 257, out, &cutSmall, 100) == Read::tooLong && out == longText.substr(0, 257), "text cut at 257 bytes in 100-byte copies");
		Check(cutSmall.calls.size() == 3 && cutSmall.calls[0].bytes == 100 && cutSmall.calls[1].bytes == 100 &&
				  cutSmall.calls[2].address == address(base + 200) && cutSmall.calls[2].bytes == 57,
			"limit of 257 in 100-byte copies: copies of 100, 100 and 57");
		Recorder nothing;
		Check(CopyCString(base, 0, out, &nothing) == Read::tooLong && out.empty() && nothing.calls.empty(), "limit 0 makes no call");

		Recorder manyBytes;
		Check(CopyBytes(base, 600, &manyBytes) == longText, "CopyBytes of 600 bytes with an observer");
		Check(manyBytes.calls.size() == 3 && manyBytes.calls[0].bytes == 256 && manyBytes.calls[1].bytes == 256 &&
				  manyBytes.calls[2].bytes == 88 && manyBytes.calls[0].address == address(base) &&
				  manyBytes.calls[1].address == address(base + 256) && manyBytes.calls[2].address == address(base + 512),
			"CopyBytes of 600 bytes: copies of 256, 256 and 88");
		Recorder nullBytes;
		Check(CopyBytes(nullptr, 16, &nullBytes).empty() && nullBytes.calls.empty(), "CopyBytes of a null address makes no call");

		// A string that runs into the reserved page: one copy, then a failed copy at the next page.
		std::memset(base + page * 2 - 8, 'r', 8);
		Recorder runsOut;
		Check(CopyCString(base + page * 2 - 8, 256, out, &runsOut) == Read::unterminated && out == "rrrrrrrr", "string into reserved memory with an observer");
		Check(runsOut.calls.size() == 2 && runsOut.calls[0].ok && runsOut.calls[0].address == address(base + page * 2 - 8) &&
				  runsOut.calls[0].bytes == 8 && !runsOut.calls[1].ok && runsOut.calls[1].address == address(base + page * 2) &&
				  runsOut.calls[1].bytes == 248,
			"string into reserved memory: one copy, then one failed copy");

		// Two regions in one allocation (page 1 read-only): two copies, one per page.
		DWORD before = 0;
		Check(::VirtualProtect(base + page, page, PAGE_READONLY, &before) != 0, "observer page 1 made read-only");
		Recorder regions;
		Check(CopyCString(tail, 256, out, &regions) == Read::ok && out == "xxxxxxxxy", "string across two regions read with an observer");
		Check(regions.calls.size() == 2 && regions.calls[0].ok && regions.calls[0].address == address(tail) &&
				  regions.calls[0].bytes == 8 && regions.calls[1].ok && regions.calls[1].address == address(base + page) &&
				  regions.calls[1].bytes == 248,
			"string across two regions: two copies, one per page");

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

		// Committed but not readable: a no-access page and a guard page. One failed copy each;
		// the guard page keeps PAGE_GUARD.
		auto* locked = static_cast<char*>(::VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
		DWORD unused = 0;
		if (!locked || ::VirtualProtect(locked + page, page, PAGE_READWRITE | PAGE_GUARD, &unused) == 0) {
			Check(false, "no-access and guard pages made");
		} else {
			Recorder noAccess;
			Check(CopyCString(locked, 256, out, &noAccess) == Read::unreadable && noAccess.calls.size() == 1 &&
					  !noAccess.calls[0].ok && noAccess.calls[0].address == address(locked) && noAccess.calls[0].bytes == 256,
				"no-access page: one failed copy");
			Recorder guarded;
			Check(CopyCString(locked + page, 256, out, &guarded) == Read::unreadable && guarded.calls.size() == 1 &&
					  !guarded.calls[0].ok && guarded.calls[0].address == address(locked + page) && guarded.calls[0].bytes == 256,
				"guard page: one failed copy");
			MEMORY_BASIC_INFORMATION guard{};
			Check(::VirtualQuery(locked + page, &guard, sizeof(guard)) == sizeof(guard) && (guard.Protect & PAGE_GUARD) != 0,
				"guard page still a guard page after the observed read");
		}
		if (locked) {
			::VirtualFree(locked, 0, MEM_RELEASE);
		}

		::VirtualFree(base, 0, MEM_RELEASE);

		// Freed memory: confirm MEM_FREE before reading, then one failed copy.
		MEMORY_BASIC_INFORMATION freedInfo{};
		Check(::VirtualQuery(base, &freedInfo, sizeof(freedInfo)) == sizeof(freedInfo) && freedInfo.State == MEM_FREE,
			"freed memory is MEM_FREE before observed read");
		Recorder freed;
		Check(CopyCString(base, 256, out, &freed) == Read::unreadable, "freed memory unreadable with an observer");
		Check(freed.calls.size() == 1 && !freed.calls[0].ok && freed.calls[0].address == address(base) && freed.calls[0].bytes == 256,
			"freed memory: one failed copy");

		// Kernel space address: one failed copy.
		const auto* kernel = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(0xFFFF'8000'0000'0000ull));
		Recorder    noAnswer;
		Check(CopyCString(kernel, 256, out, &noAnswer) == Read::unreadable, "kernel address unreadable");
		Check(noAnswer.calls.size() == 1 && !noAnswer.calls[0].ok && noAnswer.calls[0].address == address(kernel) &&
				  noAnswer.calls[0].bytes == 256,
			"kernel address: one failed copy");
	}

	std::size_t SystemPageSize() noexcept
	{
		SYSTEM_INFO info{};
		::GetSystemInfo(&info);
		return info.dwPageSize;
	}

	bool HasGuardBit(const void* a_ptr) noexcept
	{
		MEMORY_BASIC_INFORMATION mbi{};
		return ::VirtualQuery(a_ptr, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_GUARD) != 0;
	}

	void TestGuardPages()
	{
		constexpr std::size_t kMaxProbeRead = 4097;
		const std::size_t     page = SystemPageSize();
		const auto            address = [](const void* a_ptr) { return reinterpret_cast<std::uintptr_t>(a_ptr); };
		std::string           out;

		// Private allocation: [0] read-write, [1] PAGE_READWRITE | PAGE_GUARD, then PAGE_READONLY | PAGE_GUARD.
		auto* base = static_cast<char*>(::VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
		if (!base) {
			Check(false, "guard test pages allocated");
			return;
		}
		std::memset(base + page - 8, 'a', 8);
		DWORD old = 0;
		Check(::VirtualProtect(base + page, page, PAGE_READWRITE | PAGE_GUARD, &old) != 0, "private page 1 set to PAGE_READWRITE|PAGE_GUARD");

		Recorder rwDirect;
		Check(CopyCString(base + page, kMaxProbeRead, out, &rwDirect) == Read::unreadable && out.empty(),
			"RW guard page direct CopyCString (max chunk, limit 4097) is unreadable");
		Check(rwDirect.calls.size() == 1 && !rwDirect.calls[0].ok && rwDirect.calls[0].address == address(base + page) &&
				  rwDirect.calls[0].bytes == kMaxChunk,
			"RW guard page direct CopyCString attempted one kMaxChunk copy at guard address");
		Check(HasGuardBit(base + page), "RW guard page keeps PAGE_GUARD after direct CopyCString");

		Recorder rwCross;
		Check(CopyCString(base + page - 8, kMaxProbeRead, out, &rwCross) == Read::unterminated && out == "aaaaaaaa",
			"string entering RW guard page returns unterminated with prefix");
		Check(rwCross.calls.size() == 2 && rwCross.calls[0].ok && rwCross.calls[0].address == address(base + page - 8) &&
				  rwCross.calls[0].bytes == 8 && !rwCross.calls[1].ok && rwCross.calls[1].address == address(base + page) &&
				  rwCross.calls[1].bytes == kMaxChunk,
			"string entering RW guard page copies prefix then fails kMaxChunk copy at guard page");
		Check(HasGuardBit(base + page), "RW guard page keeps PAGE_GUARD after cross-page CopyCString");

		Recorder rwBytes;
		Check(CopyBytes(base + page, kMaxChunk, &rwBytes).empty(), "RW guard page CopyBytes returns empty");
		Check(rwBytes.calls.size() == 1 && !rwBytes.calls[0].ok && rwBytes.calls[0].address == address(base + page) &&
				  rwBytes.calls[0].bytes == kMaxChunk,
			"RW guard page CopyBytes attempted one kMaxChunk copy");
		Check(HasGuardBit(base + page), "RW guard page keeps PAGE_GUARD after CopyBytes");

		// Read-only guard page.
		Check(::VirtualProtect(base + page, page, PAGE_READONLY | PAGE_GUARD, &old) != 0, "private page 1 set to PAGE_READONLY|PAGE_GUARD");

		Recorder roDirect;
		Check(CopyCString(base + page, kMaxProbeRead, out, &roDirect) == Read::unreadable && out.empty(),
			"RO guard page direct CopyCString is unreadable");
		Check(roDirect.calls.size() == 1 && !roDirect.calls[0].ok && roDirect.calls[0].address == address(base + page) &&
				  roDirect.calls[0].bytes == kMaxChunk,
			"RO guard page direct CopyCString attempted one kMaxChunk copy");
		Check(HasGuardBit(base + page), "RO guard page keeps PAGE_GUARD after direct CopyCString");

		Recorder roCross;
		Check(CopyCString(base + page - 8, kMaxProbeRead, out, &roCross) == Read::unterminated && out == "aaaaaaaa",
			"string entering RO guard page returns unterminated with prefix");
		Check(roCross.calls.size() == 2 && roCross.calls[0].ok && roCross.calls[0].bytes == 8 && !roCross.calls[1].ok &&
				  roCross.calls[1].address == address(base + page) && roCross.calls[1].bytes == kMaxChunk,
			"string entering RO guard page fails kMaxChunk copy at guard page");
		Check(HasGuardBit(base + page), "RO guard page keeps PAGE_GUARD after cross-page CopyCString");

		Recorder roBytes;
		Check(CopyBytes(base + page, kMaxChunk, &roBytes).empty() && roBytes.calls.size() == 1 && !roBytes.calls[0].ok &&
				  roBytes.calls[0].bytes == kMaxChunk,
			"RO guard page CopyBytes returns empty after one kMaxChunk copy");
		Check(HasGuardBit(base + page), "RO guard page keeps PAGE_GUARD after CopyBytes");

		::VirtualFree(base, 0, MEM_RELEASE);

		// Paging-file-backed mapped view with a guard page.
		HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(page * 2), nullptr);
		Check(mapping != nullptr, "paging-file mapping created");
		if (mapping) {
			auto* view = static_cast<char*>(::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, page * 2));
			Check(view != nullptr, "paging-file mapping view mapped");
			if (view) {
				MEMORY_BASIC_INFORMATION mapInfo{};
				Check(::VirtualQuery(view, &mapInfo, sizeof(mapInfo)) == sizeof(mapInfo) && mapInfo.Type == MEM_MAPPED,
					"mapped view has MEM_MAPPED type");
				std::memset(view + page - 8, 'm', 8);
				Check(::VirtualProtect(view + page, page, PAGE_READWRITE | PAGE_GUARD, &old) != 0,
					"mapped page 1 set to PAGE_READWRITE|PAGE_GUARD");

				Recorder mapDirect;
				Check(CopyCString(view + page, kMaxProbeRead, out, &mapDirect) == Read::unreadable && out.empty(),
					"mapped guard page direct CopyCString is unreadable");
				Check(mapDirect.calls.size() == 1 && !mapDirect.calls[0].ok && mapDirect.calls[0].address == address(view + page) &&
						  mapDirect.calls[0].bytes == kMaxChunk,
					"mapped guard page direct CopyCString attempted one kMaxChunk copy");
				Check(HasGuardBit(view + page), "mapped guard page keeps PAGE_GUARD after direct CopyCString");

				Recorder mapCross;
				Check(CopyCString(view + page - 8, kMaxProbeRead, out, &mapCross) == Read::unterminated && out == "mmmmmmmm",
					"string entering mapped guard page returns unterminated with prefix");
				Check(mapCross.calls.size() == 2 && mapCross.calls[0].ok && mapCross.calls[0].bytes == 8 && !mapCross.calls[1].ok &&
						  mapCross.calls[1].address == address(view + page) && mapCross.calls[1].bytes == kMaxChunk,
					"string entering mapped guard page fails kMaxChunk copy at guard page");
				Check(HasGuardBit(view + page), "mapped guard page keeps PAGE_GUARD after cross-page CopyCString");

				Recorder mapBytes;
				Check(CopyBytes(view + page, kMaxChunk, &mapBytes).empty() && mapBytes.calls.size() == 1 && !mapBytes.calls[0].ok &&
						  mapBytes.calls[0].bytes == kMaxChunk,
					"mapped guard page CopyBytes returns empty after one kMaxChunk copy");
				Check(HasGuardBit(view + page), "mapped guard page keeps PAGE_GUARD after CopyBytes");

				::UnmapViewOfFile(view);
			}
			::CloseHandle(mapping);
		}
	}

	// Separate leaf functions without C++ objects needing unwinding (avoids C2712 under /EHsc).
	__declspec(noinline) bool GrowStackStep(std::uintptr_t a_targetLimit, int a_depthLeft, std::uintptr_t* a_finalLimit, volatile unsigned char* a_sink)
	{
		const auto* tib = reinterpret_cast<const NT_TIB*>(::NtCurrentTeb());
		const auto  limit = reinterpret_cast<std::uintptr_t>(tib->StackLimit);
		*a_finalLimit = limit;
		if (limit <= a_targetLimit) {
			return true;
		}
		if (a_depthLeft <= 0) {
			return false;
		}
		constexpr std::size_t  kFrameBytes = 2048;
		volatile unsigned char frame[kFrameBytes];
		frame[0] = static_cast<unsigned char>(a_depthLeft);
		frame[kFrameBytes - 1] = static_cast<unsigned char>(a_depthLeft ^ 0x5Au);
		*a_sink = static_cast<unsigned char>(frame[0] ^ frame[kFrameBytes - 1]);
		const bool reached = GrowStackStep(a_targetLimit, a_depthLeft - 1, a_finalLimit, a_sink);
		// Use frame after the recursive call so tail-call optimization cannot pop it early.
		*a_sink = static_cast<unsigned char>(*a_sink ^ frame[0] ^ frame[kFrameBytes - 1]);
		return reached;
	}

	bool GrowStackGuarded(std::uintptr_t a_targetLimit, int a_maxDepth, std::uintptr_t* a_finalLimit, DWORD* a_exceptionCode)
	{
		*a_exceptionCode = 0;
		volatile unsigned char sink = 0;
		__try {
			return GrowStackStep(a_targetLimit, a_maxDepth, a_finalLimit, &sink);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			*a_exceptionCode = ::GetExceptionCode();
			return false;
		}
	}

	struct StackGuardContext
	{
		std::size_t    page{ 0 };
		HANDLE         readyEvent{ nullptr };
		HANDLE         growEvent{ nullptr };
		std::uintptr_t guardAddr{ 0 };
		bool           foundGuard{ false };
		bool           growWaitOk{ false };
		bool           grewBelowTarget{ false };
		std::uintptr_t targetLimit{ 0 };
		std::uintptr_t finalLimit{ 0 };
		DWORD          exceptionCode{ 0 };
	};

	DWORD WINAPI StackGuardThreadProc(LPVOID a_param)
	{
		auto*       ctx = static_cast<StackGuardContext*>(a_param);
		const auto* tib = reinterpret_cast<const NT_TIB*>(::NtCurrentTeb());
		const auto  limit = reinterpret_cast<std::uintptr_t>(tib->StackLimit);
		if (limit >= ctx->page) {
			const auto guard = limit - ctx->page;
			if (HasGuardBit(reinterpret_cast<const void*>(guard))) {
				ctx->guardAddr = guard;
				ctx->foundGuard = true;
			}
		}
		::SetEvent(ctx->readyEvent);
		constexpr DWORD kWaitMs = 10'000;
		ctx->growWaitOk = ::WaitForSingleObject(ctx->growEvent, kWaitMs) == WAIT_OBJECT_0;
		if (!ctx->growWaitOk || !ctx->foundGuard || ctx->guardAddr <= ctx->page * 8) {
			return 0;
		}
		ctx->targetLimit = ctx->guardAddr - ctx->page * 8;
		constexpr int kMaxDepth = 128;
		ctx->grewBelowTarget = GrowStackGuarded(ctx->targetLimit, kMaxDepth, &ctx->finalLimit, &ctx->exceptionCode);
		return 0;
	}

	// A test thread that missed its deadline may still write to memory and handles this test is
	// about to release, and the tests after it would share that memory. So the run ends here with
	// the failure counted instead of going on.
	[[noreturn]] void StopTests(const char* a_why)
	{
		std::printf("FAILED (%d passed, %d failed, stopped early: %s)\n", g_passed, g_failed, a_why);
		std::fflush(stdout);
		::ExitProcess(1);
	}

	void TestThreadStackGuard()
	{
		constexpr std::size_t kStackReserve = 1024 * 1024;
		constexpr std::size_t kMaxProbeRead = 4097;
		constexpr DWORD       kWaitMs = 10'000;

		StackGuardContext ctx{};
		ctx.page = SystemPageSize();
		ctx.readyEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		ctx.growEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		Check(ctx.readyEvent != nullptr && ctx.growEvent != nullptr, "stack guard test events created");
		if (!ctx.readyEvent || !ctx.growEvent) {
			if (ctx.readyEvent) {
				::CloseHandle(ctx.readyEvent);
			}
			if (ctx.growEvent) {
				::CloseHandle(ctx.growEvent);
			}
			return;
		}

		HANDLE thread = ::CreateThread(nullptr, kStackReserve, StackGuardThreadProc, &ctx, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
		Check(thread != nullptr, "stack guard test thread created with 1 MB reservation");
		if (!thread) {
			::CloseHandle(ctx.readyEvent);
			::CloseHandle(ctx.growEvent);
			return;
		}

		const bool ready = ::WaitForSingleObject(ctx.readyEvent, kWaitMs) == WAIT_OBJECT_0;
		Check(ready, "stack guard thread signaled ready within 10 s");
		if (!ready) {
			StopTests("the stack guard thread may still be running");
		}
		Check(ctx.foundGuard, "thread stack guard page found right below NT_TIB::StackLimit");
		if (ctx.foundGuard) {
			const auto* guardPtr = reinterpret_cast<const char*>(ctx.guardAddr);
			// The thread used a little more stack after finding the page (to signal and wait); if
			// that reached the page, it is no longer the guard and the reads below say nothing.
			Check(HasGuardBit(guardPtr), "live thread stack guard page is still a guard page right before the reads");
			std::string out = "left over";
			Recorder    strRec;
			Check(CopyCString(guardPtr, kMaxProbeRead, out, &strRec) == Read::unreadable && out.empty(),
				"CopyCString on live thread stack guard page is unreadable");
			Check(strRec.calls.size() == 1 && !strRec.calls[0].ok && strRec.calls[0].address == ctx.guardAddr &&
					  strRec.calls[0].bytes == kMaxChunk,
				"CopyCString on thread stack guard page attempted one kMaxChunk copy");

			Recorder bytesRec;
			Check(CopyBytes(guardPtr, kMaxChunk, &bytesRec).empty() && bytesRec.calls.size() == 1 && !bytesRec.calls[0].ok &&
					  bytesRec.calls[0].address == ctx.guardAddr && bytesRec.calls[0].bytes == kMaxChunk,
				"CopyBytes on live thread stack guard page returns empty after one kMaxChunk copy");

			Check(HasGuardBit(guardPtr), "live thread stack guard page keeps PAGE_GUARD after reads");
		}

		::SetEvent(ctx.growEvent);
		const bool done = ::WaitForSingleObject(thread, kWaitMs) == WAIT_OBJECT_0;
		Check(done, "stack guard thread exited within 10 s");
		if (!done) {
			StopTests("the stack guard thread may still be running");
		}
		if (ctx.foundGuard) {
			Check(ctx.growWaitOk, "stack guard thread received grow signal within 10 s");
			Check(ctx.exceptionCode == 0, "growing thread stack past guard page raised no exception");
			Check(ctx.grewBelowTarget && ctx.finalLimit <= ctx.targetLimit && ctx.finalLimit < ctx.guardAddr,
				"thread StackLimit grew at least 8 pages below original guard page");
		}

		::CloseHandle(thread);
		::CloseHandle(ctx.readyEvent);
		::CloseHandle(ctx.growEvent);
	}

	volatile LONG g_vehExceptions = 0;

	LONG CALLBACK CountVectoredException(EXCEPTION_POINTERS*)
	{
		::InterlockedIncrement(&g_vehExceptions);
		return EXCEPTION_CONTINUE_SEARCH;
	}

	// Separate leaf function with no C++ objects needing unwinding (avoids C2712 under /EHsc).
	bool RaiseProbeException(DWORD a_code)
	{
		__try {
			::RaiseException(a_code, 0, 0, nullptr);
			return false;
		} __except (::GetExceptionCode() == a_code ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
			return true;
		}
	}

	void TestUnreadableAndNoExceptions()
	{
		constexpr std::size_t kMaxProbeRead = 4097;
		const std::size_t     page = SystemPageSize();

		// Allocate test pages before installing the vectored exception handler:
		// [0] PAGE_NOACCESS, [1] reserved only, [2] PAGE_READWRITE|PAGE_GUARD.
		auto* pages = static_cast<char*>(::VirtualAlloc(nullptr, page * 3, MEM_RESERVE, PAGE_NOACCESS));
		auto* toFree = static_cast<char*>(::VirtualAlloc(nullptr, page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
		if (!pages || !toFree) {
			Check(false, "unreadable-address test pages allocated");
			if (pages) {
				::VirtualFree(pages, 0, MEM_RELEASE);
			}
			if (toFree) {
				::VirtualFree(toFree, 0, MEM_RELEASE);
			}
			return;
		}
		const bool committed = ::VirtualAlloc(pages, page, MEM_COMMIT, PAGE_NOACCESS) != nullptr &&
		                       ::VirtualAlloc(pages + page * 2, page, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD) != nullptr;
		const bool freed = ::VirtualFree(toFree, 0, MEM_RELEASE) != 0;
		Check(committed && freed, "unreadable-address test pages prepared (no-access, reserved, guard, freed)");
		if (!committed || !freed) {
			::VirtualFree(pages, 0, MEM_RELEASE);
			return;
		}

		::InterlockedExchange(&g_vehExceptions, 0);
		void* veh = ::AddVectoredExceptionHandler(1, CountVectoredException);
		Check(veh != nullptr, "first-chance vectored exception handler installed");
		if (!veh) {
			::VirtualFree(pages, 0, MEM_RELEASE);
			return;
		}

		// Confirm the handler actually observes a raised exception before counting 0 on reads.
		constexpr DWORD kProbeException = 0xE000'0011u;
		const bool      caught = RaiseProbeException(kProbeException);
		const LONG      probeHits = ::InterlockedExchange(&g_vehExceptions, 0);
		Check(caught && probeHits == 1, "vectored exception handler catches RaiseException once");

		// Private read-only and read-write guard pages, a mapped guard page, and copies running into
		// them from the page before: all under the same handler.
		TestGuardPages();

		// Each read must really try the copy (one failed ReadProcessMemory at that address, of the
		// default chunk cut at the page end), so a check that skipped the copy would not pass.
		std::string out;
		const auto  checkUnreadable = [&](const void* a_ptr, const char* a_label) {
			const auto at = reinterpret_cast<std::uintptr_t>(a_ptr);
			const auto expected = std::min<std::uintptr_t>(kMaxChunk, page - (at % page));
			const auto oneFailedCopy = [&](const Recorder& a_rec) {
				return a_rec.calls.size() == 1 && !a_rec.calls[0].ok && a_rec.calls[0].address == at && a_rec.calls[0].bytes == expected;
			};
			out = "sentinel";
			Recorder   strRec;
			Recorder   bytesRec;
			const auto strOk = CopyCString(static_cast<const char*>(a_ptr), kMaxProbeRead, out, &strRec) == Read::unreadable && out.empty();
			const auto bytesOk = CopyBytes(a_ptr, kMaxChunk, &bytesRec).empty();
			Check(strOk && bytesOk && oneFailedCopy(strRec) && oneFailedCopy(bytesRec), a_label);
		};

		// 1. Committed no-access page.
		checkUnreadable(pages, "unreadable [1/7]: PAGE_NOACCESS gives unreadable/empty");
		// 2. Reserved-only page.
		checkUnreadable(pages + page, "unreadable [2/7]: MEM_RESERVE gives unreadable/empty");
		// 3. Freed page: confirm MEM_FREE immediately before reading.
		MEMORY_BASIC_INFORMATION freeInfo{};
		Check(::VirtualQuery(toFree, &freeInfo, sizeof(freeInfo)) == sizeof(freeInfo) && freeInfo.State == MEM_FREE,
			"unreadable [3/7]: freed address confirmed MEM_FREE right before read");
		checkUnreadable(toFree, "unreadable [3/7]: freed MEM_FREE page gives unreadable/empty");
		// 4. Low address 0x10.
		checkUnreadable(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x10)),
			"unreadable [4/7]: low address 0x10 gives unreadable/empty");
		// 5. Kernel-side address.
		checkUnreadable(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0xFFFF'8000'0000'0000ull)),
			"unreadable [5/7]: kernel address 0xFFFF800000000000 gives unreadable/empty");
		// 6. Above user-space address.
		checkUnreadable(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x0000'9000'0000'0000ull)),
			"unreadable [6/7]: non-canonical/above-user address 0x0000900000000000 gives unreadable/empty");
		// 7. Top of 64-bit address space (also checks page-remaining calculation without wraparound).
		const auto* topAddr = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(0xFFFF'FFFF'FFFF'FFF0ull));
		Recorder    topRec;
		out = "sentinel";
		Check(CopyCString(topAddr, kMaxProbeRead, out, &topRec) == Read::unreadable && out.empty() && CopyBytes(topAddr, kMaxChunk).empty(),
			"unreadable [7/7]: top address 0xFFFFFFFFFFFFFFF0 gives unreadable/empty");
		Check(topRec.calls.size() == 1 && !topRec.calls[0].ok && topRec.calls[0].bytes == 16,
			"top address 0xFFFFFFFFFFFFFFF0 clamps copy to 16 bytes without overflow");

		// Guard page under the same VEH.
		checkUnreadable(pages + page * 2, "guard page under VEH gives unreadable/empty");
		Check(HasGuardBit(pages + page * 2), "guard page keeps PAGE_GUARD under VEH");

		const LONG readExceptions = ::InterlockedExchange(&g_vehExceptions, 0);
		Check(::RemoveVectoredExceptionHandler(veh) != 0, "vectored exception handler removed");
		Check(readExceptions == 0, "guard-page reads and all seven unreadable addresses raised 0 exceptions");

		::VirtualFree(pages, 0, MEM_RELEASE);
	}

	void TestPageAndChunkBoundaries()
	{
		const std::size_t page = SystemPageSize();
		const auto        address = [](const void* a_ptr) { return reinterpret_cast<std::uintptr_t>(a_ptr); };
		const auto        staysInPage = [page](const CallInfo& a_call) {
			return a_call.bytes > 0 && a_call.bytes <= kMaxChunk && (a_call.address % page) + a_call.bytes <= page;
		};
		std::string out;

		Check(kMaxChunk == 256, "kMaxChunk is 256 bytes");

		auto* base = static_cast<char*>(::VirtualAlloc(nullptr, page * 2, MEM_RESERVE, PAGE_NOACCESS));
		if (!base) {
			Check(false, "boundary test pages reserved");
			return;
		}
		if (::VirtualAlloc(base, page, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
			Check(false, "boundary test page 0 committed");
			::VirtualFree(base, 0, MEM_RELEASE);
			return;
		}

		// String ending at the very last byte before an unreadable (reserved) page is ok.
		char* atEnd = base + page - 5;
		std::memcpy(atEnd, "abcd", 5);  // terminating zero is at base + page - 1
		Recorder endRec;
		Check(CopyCString(atEnd, 4097, out, &endRec) == Read::ok && out == "abcd",
			"string ending at the last byte before an unreadable page is ok");
		Check(endRec.calls.size() == 1 && endRec.calls[0].ok && endRec.calls[0].address == address(atEnd) &&
				  endRec.calls[0].bytes == 5 && staysInPage(endRec.calls[0]),
			"string before unreadable page stops at page end in one copy");

		// Page-sized string (page bytes, 4096 on Windows) at page start with a_max = page + 1 (4097):
		// when next page is unreadable, result is unterminated with page bytes.
		// Each of the three reads below copies the page in full chunks, then asks for exactly the one
		// byte the limit leaves at the start of the next page (never a whole chunk there).
		const auto pageThenOneByte = [&](const Recorder& a_rec, bool a_lastOk) {
			const std::size_t full = page / kMaxChunk;
			if (page % kMaxChunk != 0 || a_rec.calls.size() != full + 1) {
				return false;
			}
			for (std::size_t i = 0; i < full; ++i) {
				const auto& call = a_rec.calls[i];
				if (!call.ok || call.bytes != kMaxChunk || call.address != address(base + i * kMaxChunk)) {
					return false;
				}
			}
			const auto& last = a_rec.calls[full];
			return last.ok == a_lastOk && last.bytes == 1 && last.address == address(base + page);
		};
		std::memset(base, 'p', page);
		Recorder pageUnreadable;
		Check(CopyCString(base, page + 1, out, &pageUnreadable) == Read::unterminated && out.size() == page,
			"page-sized string at page start with limit page+1 is unterminated when next page is unreadable");
		Check(pageThenOneByte(pageUnreadable, false), "unreadable next page: full chunks, then one failed one-byte copy there");

		// Commit page 1 to test two-page strings and page+1 boundary cases.
		const bool secondPage = ::VirtualAlloc(base + page, page, MEM_COMMIT, PAGE_READWRITE) != nullptr;
		Check(secondPage, "boundary test page 1 committed");
		if (!secondPage) {
			::VirtualFree(base, 0, MEM_RELEASE);
			return;
		}

		// Next page starts with zero -> ok.
		base[page] = '\0';
		Recorder pageZero;
		Check(CopyCString(base, page + 1, out, &pageZero) == Read::ok && out.size() == page,
			"page-sized string at page start with limit page+1 is ok when next page starts with zero");
		Check(pageThenOneByte(pageZero, true), "zero at the next page: full chunks, then one one-byte copy there");

		// Next page starts with a character -> tooLong.
		base[page] = 'q';
		base[page + 1] = '\0';
		Recorder pageLong;
		Check(CopyCString(base, page + 1, out, &pageLong) == Read::tooLong && out.size() == page + 1,
			"page-sized string at page start with limit page+1 is tooLong when next page starts with a char");
		Check(pageThenOneByte(pageLong, true), "character at the next page: full chunks, then one one-byte copy and no more");

		// String spanning two readable pages: ok, two copies, neither crosses the page boundary.
		char* cross = base + page - 10;
		std::memset(cross, 'a', 10);
		std::memset(base + page, 'b', 10);
		base[page + 10] = '\0';
		Recorder crossRec;
		Check(CopyCString(cross, 4097, out, &crossRec) == Read::ok && out == "aaaaaaaaaabbbbbbbbbb",
			"string spanning two readable pages is ok");
		Check(crossRec.calls.size() == 2 && crossRec.calls[0].address == address(cross) && crossRec.calls[0].bytes == 10 &&
				  crossRec.calls[1].address == address(base + page) && crossRec.calls[1].bytes == kMaxChunk &&
				  staysInPage(crossRec.calls[0]) && staysInPage(crossRec.calls[1]),
			"string spanning two readable pages uses two copies that never cross a page boundary");

		// Default chunk never exceeds kMaxChunk on multi-chunk strings; CopyBytes 600 splits as 256+256+88.
		std::memset(base, 'c', 1000);
		base[1000] = '\0';
		Recorder defaultRec;
		Check(CopyCString(base, 4097, out, &defaultRec) == Read::ok && out.size() == 1000,
			"1000-byte string copied with default chunk");
		bool allBounded = defaultRec.calls.size() == 4;
		for (const auto& call : defaultRec.calls) {
			allBounded = allBounded && call.ok && staysInPage(call);
		}
		Check(allBounded, "default chunk copies never exceed kMaxChunk or cross a page boundary");

		Recorder bytes600;
		Check(CopyBytes(base, 600, &bytes600).size() == 600 && bytes600.calls.size() == 3 &&
				  bytes600.calls[0].bytes == 256 && bytes600.calls[1].bytes == 256 && bytes600.calls[2].bytes == 88,
			"CopyBytes of 600 bytes splits into 256, 256 and 88");

		// Chunk boundary: (kMaxChunk - 1)-byte string takes 1 copy; kMaxChunk-byte string takes 2 copies.
		std::memset(base, 'd', kMaxChunk - 1);
		base[kMaxChunk - 1] = '\0';
		Recorder chunkMinusOne;
		Check(CopyCString(base, 4097, out, &chunkMinusOne) == Read::ok && out.size() == kMaxChunk - 1,
			"255-byte string copied with default chunk");
		Check(chunkMinusOne.calls.size() == 1 && chunkMinusOne.calls[0].bytes == kMaxChunk,
			"(kMaxChunk - 1)-byte string takes one copy");

		std::memset(base, 'd', kMaxChunk);
		base[kMaxChunk] = '\0';
		Recorder chunkExact;
		Check(CopyCString(base, 4097, out, &chunkExact) == Read::ok && out.size() == kMaxChunk,
			"256-byte string copied with default chunk");
		Check(chunkExact.calls.size() == 2 && chunkExact.calls[0].bytes == kMaxChunk &&
				  chunkExact.calls[1].address == address(base + kMaxChunk) && chunkExact.calls[1].bytes == kMaxChunk,
			"kMaxChunk-byte string takes two copies to find the terminating zero");

		::VirtualFree(base, 0, MEM_RELEASE);
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
	TestThreadStackGuard();
	TestUnreadableAndNoExceptions();  // runs TestGuardPages inside its exception-counting window
	TestPageAndChunkBoundaries();
	std::printf("%s (%d passed, %d failed)\n", g_failed == 0 ? "all passed" : "FAILED", g_passed, g_failed);
	return g_failed == 0 ? 0 : 1;
}
