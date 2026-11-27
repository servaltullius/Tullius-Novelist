#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Text handling that does not depend on the game, so it can be tested on its own
// (probe/tests/TextTest.cpp).
namespace probe::text
{
	// Length of the well-formed UTF-8 sequence that starts at a_text[a_pos] (1 to 4), or 0 if
	// none starts there (stray continuation byte, overlong form, surrogate, beyond U+10FFFF,
	// cut off by the end of the text, or a_pos not inside the text).
	std::size_t Utf8SequenceLength(std::string_view a_text, std::size_t a_pos) noexcept;
	bool        IsUtf8(std::string_view a_text) noexcept;

	// Body of a JSON string, so a record line stays valid JSON whatever bytes a game string holds:
	// - quote and backslash escaped;
	// - C0 controls, DEL and C1 controls (U+0080-U+009F) as \uXXXX, and U+2028/U+2029 too, so
	//   tools that split on any line break do not break a record line;
	// - every byte that is not part of well-formed UTF-8 replaced by U+FFFD (one per byte).
	std::string JsonEscape(std::string_view a_text);

	// True if the text is well-formed UTF-8 without control characters other than tab, line
	// feed and carriage return (C0, DEL and C1 count as control characters).
	bool IsCleanText(std::string_view a_text) noexcept;

	// Text for use inside an identifier (a plugin file name in a form key, a save name), kept
	// lossless:
	// - a name that is all well-formed UTF-8 stays as it is, except that '%' becomes "%25";
	// - any other name is written in ASCII: '%' and every byte from 0x80 up become "%XX".
	// Decoding every %XX in the UTF-8 bytes of the result gives back the original bytes, and two
	// different names never give the same result. Plugin file names are expected to come in the
	// system code page (the game keeps them in a char array; not confirmed with a non-ASCII
	// name), so a non-ASCII name would normally take the second form. A code-page name that
	// happens to be well-formed UTF-8 takes the first form and shows as unrelated characters,
	// but stays lossless. The record does not say which code page it was.
	std::string KeyText(std::string_view a_text);

	// Up to a_max bytes as hex pairs separated by spaces, for showing bytes that are not text.
	std::string HexBytes(std::string_view a_bytes, std::size_t a_max);

	// True if pages with this protection (MEMORY_BASIC_INFORMATION::Protect) can be read directly
	// after a VirtualQuery check (used by probe::forms). CopyCString and CopyBytes do not call
	// this: they copy through ReadProcessMemory without querying first, which on this Windows
	// build can also read PAGE_EXECUTE pages.
	bool IsReadableProtection(unsigned long a_protect) noexcept;

	// Each ReadProcessMemory call behind CopyCString and CopyBytes, reported to an observer.
	struct CallInfo
	{
		bool           ok;       // every byte of the chunk copied
		double         us;       // time spent in ReadProcessMemory alone
		std::uintptr_t address;  // where the copy started
		std::size_t    bytes;    // chunk size asked for
	};

	class CallObserver
	{
	public:
		// Called right after each ReadProcessMemory call, on the reading thread, before the read goes on.
		virtual void OnCall(const CallInfo& a_call) = 0;

	protected:
		~CallObserver() = default;
	};

	enum class Read
	{
		ok,            // copied up to the terminating zero
		unreadable,    // no readable byte at the address (or a null address)
		unterminated,  // readable memory ran out before a zero
		tooLong,       // a_max bytes copied without a zero among them (the byte after is not read)
	};

	// Copies a zero-terminated string from an address that may not hold one: a string the game
	// has already released, or one that never ends. Reads at most a_max bytes through
	// ReadProcessMemory without VirtualQuery; on Windows 11 (26300, re-checked at every build by
	// probe_text_test), unreadable and guard pages fail without clearing PAGE_GUARD and without
	// raising a user-mode exception. Because VirtualQuery is not used, PAGE_EXECUTE pages can be
	// read on this Windows build and may be returned as text if their bytes happen to look like
	// text. a_out holds the bytes copied (without the zero), also when the result is not ok. With
	// a_max 0 nothing is read and the result is unreadable for a null address, else tooLong.
	// a_observer, if given, is told about every ReadProcessMemory call.
	// a_chunk: the most bytes one ReadProcessMemory call asks for; 0, or anything above
	// kMaxChunk, means kMaxChunk. A copy also never crosses a page end.
	inline constexpr std::size_t kMaxChunk = 256;
	Read CopyCString(const char* a_ptr, std::size_t a_max, std::string& a_out, CallObserver* a_observer = nullptr,
		std::size_t a_chunk = kMaxChunk);

	// Copies up to a_count bytes from a_ptr the same way, stopping at the first byte that cannot
	// be read (zeros included, unlike CopyCString). For showing what is really at an address.
	std::string CopyBytes(const void* a_ptr, std::size_t a_count, CallObserver* a_observer = nullptr);

	// The text without a UTF-8 sequence cut off at its end (a valid lead byte followed by fewer
	// continuation bytes than it needs, those in range), for text cut at a length limit. Bytes
	// that could not start a sequence stay, so they are still judged.
	std::string_view WithoutCutSequence(std::string_view a_text) noexcept;

	// Classifies a string copied by CopyCString. nullptr for clean text; otherwise "unreadable",
	// "unterminated", "notText", or "empty" (only when a_emptyIsBad: the address was not null
	// but held no text, which for some sources is itself a sign of a released string).
	// tooLong is not bad: the text is only cut.
	const char* Classify(Read a_read, std::string_view a_text, bool a_emptyIsBad) noexcept;
}
