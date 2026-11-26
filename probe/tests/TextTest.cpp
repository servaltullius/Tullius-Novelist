// Tests for probe/src/Text.cpp without the game. Build and run:
//   cmake --build --preset vs2022-release --target probe_text_test
//   G:/skyrim-build/TulliusNovelist/build/<preset dir>/probe_text_test.exe
// package_probe runs it before making the zip. Exit code 0 means every check passed.

#include "Text.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

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
	std::printf("%s (%d passed, %d failed)\n", g_failed == 0 ? "all passed" : "FAILED", g_passed, g_failed);
	return g_failed == 0 ? 0 : 1;
}
