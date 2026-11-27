#include "Text.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <format>

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

namespace probe::text
{
	namespace
	{
		std::uintptr_t PageSize() noexcept
		{
			static const std::uintptr_t page = [] {
				SYSTEM_INFO system{};
				::GetSystemInfo(&system);
				return static_cast<std::uintptr_t>(system.dwPageSize);
			}();
			return page;
		}

		std::int64_t Ticks() noexcept
		{
			LARGE_INTEGER now{};
			::QueryPerformanceCounter(&now);
			return now.QuadPart;
		}

		double TicksToUs(std::int64_t a_ticks) noexcept
		{
			static const double ticksPerUs = [] {
				LARGE_INTEGER frequency{};
				::QueryPerformanceFrequency(&frequency);
				return static_cast<double>(frequency.QuadPart) / 1'000'000.0;
			}();
			return static_cast<double>(a_ticks) / ticksPerUs;
		}

		// Reads forward from an address that may not be readable. Each copy stays inside one page
		// (a cross-page ReadProcessMemory fails as a whole if the second page is unreadable) and
		// goes directly through ReadProcessMemory without VirtualQuery. On Windows 11 (26300,
		// re-checked at every build by probe_text_test), unreadable and guard pages fail without
		// clearing PAGE_GUARD and without raising a user-mode exception. With an observer, each
		// call is timed and reported; without one nothing is timed.
		class SafeReader
		{
		public:
			SafeReader(std::uintptr_t a_address, CallObserver* a_observer) noexcept :
				_address(a_address), _observer(a_observer)
			{}

			// Copies up to a_want bytes into a_buffer. Returns the bytes copied, 0 when the memory
			// at the current address cannot be read.
			std::size_t Next(char* a_buffer, std::size_t a_want)
			{
				const auto page = PageSize();
				const auto pageLeft = page - (_address % page);
				const auto chunk = static_cast<std::size_t>(std::min<std::uintptr_t>(a_want, pageLeft));
				SIZE_T     copied = 0;
				const auto start = _observer ? Ticks() : 0;
				const bool read = ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<const void*>(_address), a_buffer, chunk, &copied) != FALSE;
				const auto end = _observer ? Ticks() : 0;
				const bool ok = read && copied == chunk;
				if (_observer) {
					_observer->OnCall({ ok, TicksToUs(end - start), _address, chunk });
				}
				if (!ok) {
					return 0;
				}
				_address += chunk;
				return chunk;
			}

		private:
			std::uintptr_t _address;
			CallObserver*  _observer;
		};

		// What a lead byte of a well-formed UTF-8 sequence allows (Unicode 15, table 3-7): the
		// sequence length and the range of the second byte; later bytes are 80..BF.
		struct Lead
		{
			std::size_t length;
			unsigned    low;
			unsigned    high;
		};

		bool LeadOf(unsigned a_byte, Lead& a_lead) noexcept
		{
			if (a_byte >= 0xC2 && a_byte <= 0xDF) {
				a_lead = { 2, 0x80, 0xBF };
			} else if (a_byte == 0xE0) {
				a_lead = { 3, 0xA0, 0xBF };
			} else if ((a_byte >= 0xE1 && a_byte <= 0xEC) || a_byte == 0xEE || a_byte == 0xEF) {
				a_lead = { 3, 0x80, 0xBF };
			} else if (a_byte == 0xED) {
				a_lead = { 3, 0x80, 0x9F };
			} else if (a_byte == 0xF0) {
				a_lead = { 4, 0x90, 0xBF };
			} else if (a_byte >= 0xF1 && a_byte <= 0xF3) {
				a_lead = { 4, 0x80, 0xBF };
			} else if (a_byte == 0xF4) {
				a_lead = { 4, 0x80, 0x8F };
			} else {
				return false;
			}
			return true;
		}
	}

	std::string_view WithoutCutSequence(std::string_view a_text) noexcept
	{
		// Drop the tail only if it is the start of a well-formed sequence (a valid lead byte and
		// continuation bytes in range for it); anything else stays and is judged as it is.
		for (std::size_t back = 1; back <= 3 && back <= a_text.size(); ++back) {
			const auto pos = a_text.size() - back;
			const auto c = static_cast<unsigned char>(a_text[pos]);
			if (c < 0x80) {
				return a_text;
			}
			if (c < 0xC0) {
				continue;  // continuation byte: the lead is further back
			}
			Lead lead{};
			if (!LeadOf(c, lead) || lead.length <= back) {
				return a_text;
			}
			for (std::size_t i = 1; i < back; ++i) {
				const unsigned next = static_cast<unsigned char>(a_text[pos + i]);
				const unsigned low = i == 1 ? lead.low : 0x80;
				const unsigned high = i == 1 ? lead.high : 0xBF;
				if (next < low || next > high) {
					return a_text;
				}
			}
			return a_text.substr(0, pos);
		}
		return a_text;
	}

	std::size_t Utf8SequenceLength(std::string_view a_text, std::size_t a_pos) noexcept
	{
		if (a_pos >= a_text.size()) {
			return 0;
		}
		const auto byte = [&](std::size_t a_index) { return static_cast<unsigned char>(a_text[a_index]); };
		const unsigned first = byte(a_pos);
		if (first < 0x80) {
			return 1;
		}
		Lead lead{};
		if (!LeadOf(first, lead) || a_text.size() - a_pos < lead.length) {
			return 0;
		}
		const unsigned second = byte(a_pos + 1);
		if (second < lead.low || second > lead.high) {
			return 0;
		}
		for (std::size_t i = 2; i < lead.length; ++i) {
			const unsigned next = byte(a_pos + i);
			if (next < 0x80 || next > 0xBF) {
				return 0;
			}
		}
		return lead.length;
	}

	bool IsUtf8(std::string_view a_text) noexcept
	{
		for (std::size_t pos = 0; pos < a_text.size();) {
			const auto length = Utf8SequenceLength(a_text, pos);
			if (length == 0) {
				return false;
			}
			pos += length;
		}
		return true;
	}

	std::string JsonEscape(std::string_view a_text)
	{
		std::string result;
		result.reserve(a_text.size() + 8);
		for (std::size_t pos = 0; pos < a_text.size();) {
			const auto c = static_cast<unsigned char>(a_text[pos]);
			if (c >= 0x80) {
				const auto length = Utf8SequenceLength(a_text, pos);
				if (length == 0) {
					result += "\\ufffd";
					++pos;
					continue;
				}
				const auto second = static_cast<unsigned char>(a_text[pos + 1]);
				if (length == 2 && c == 0xC2 && second < 0xA0) {
					result += std::format("\\u{:04x}", static_cast<unsigned>(second));  // C1 control
				} else if (length == 3 && c == 0xE2 && second == 0x80 &&
						   (static_cast<unsigned char>(a_text[pos + 2]) == 0xA8 || static_cast<unsigned char>(a_text[pos + 2]) == 0xA9)) {
					result += static_cast<unsigned char>(a_text[pos + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
				} else {
					result.append(a_text.substr(pos, length));
				}
				pos += length;
				continue;
			}
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
				if (c < 0x20 || c == 0x7F) {
					result += std::format("\\u{:04x}", static_cast<unsigned>(c));
				} else {
					result += static_cast<char>(c);
				}
			}
			++pos;
		}
		return result;
	}

	bool IsCleanText(std::string_view a_text) noexcept
	{
		for (std::size_t pos = 0; pos < a_text.size();) {
			const auto c = static_cast<unsigned char>(a_text[pos]);
			if (c < 0x80) {
				if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0x7F) {
					return false;
				}
				++pos;
				continue;
			}
			const auto length = Utf8SequenceLength(a_text, pos);
			if (length == 0) {
				return false;
			}
			if (length == 2 && c == 0xC2 && static_cast<unsigned char>(a_text[pos + 1]) < 0xA0) {
				return false;  // C1 control
			}
			pos += length;
		}
		return true;
	}

	std::string KeyText(std::string_view a_text)
	{
		const bool  utf8 = IsUtf8(a_text);
		std::string result;
		result.reserve(a_text.size());
		for (const char ch : a_text) {
			const auto c = static_cast<unsigned char>(ch);
			if (c == '%' || (!utf8 && c >= 0x80)) {
				result += std::format("%{:02X}", static_cast<unsigned>(c));
			} else {
				result += ch;
			}
		}
		return result;
	}

	std::string HexBytes(std::string_view a_bytes, std::size_t a_max)
	{
		std::string result;
		const auto  count = std::min(a_bytes.size(), a_max);
		result.reserve(count * 3);
		for (std::size_t i = 0; i < count; ++i) {
			if (i != 0) {
				result += ' ';
			}
			result += std::format("{:02X}", static_cast<unsigned>(static_cast<unsigned char>(a_bytes[i])));
		}
		return result;
	}

	Read CopyCString(const char* a_ptr, std::size_t a_max, std::string& a_out, CallObserver* a_observer, std::size_t a_chunk)
	{
		a_out.clear();
		if (!a_ptr) {
			return Read::unreadable;
		}
		SafeReader  reader(reinterpret_cast<std::uintptr_t>(a_ptr), a_observer);
		char        buffer[kMaxChunk];
		const auto  chunk = a_chunk == 0 ? sizeof(buffer) : std::min(a_chunk, sizeof(buffer));
		std::size_t left = a_max;
		while (left > 0) {
			const auto copied = reader.Next(buffer, std::min(left, chunk));
			if (copied == 0) {
				return a_out.empty() ? Read::unreadable : Read::unterminated;
			}
			if (const auto* zero = static_cast<const char*>(std::memchr(buffer, '\0', copied))) {
				a_out.append(buffer, static_cast<std::size_t>(zero - buffer));
				return Read::ok;
			}
			a_out.append(buffer, copied);
			left -= copied;
		}
		return Read::tooLong;
	}

	std::string CopyBytes(const void* a_ptr, std::size_t a_count, CallObserver* a_observer)
	{
		std::string result;
		if (!a_ptr) {
			return result;
		}
		SafeReader reader(reinterpret_cast<std::uintptr_t>(a_ptr), a_observer);
		char       buffer[kMaxChunk];
		while (result.size() < a_count) {
			const auto copied = reader.Next(buffer, std::min(a_count - result.size(), sizeof(buffer)));
			if (copied == 0) {
				break;
			}
			result.append(buffer, copied);
		}
		return result;
	}

	bool CopyExact(const void* a_ptr, void* a_out, std::size_t a_count, CallObserver* a_observer)
	{
		if (!a_ptr) {
			return false;
		}
		if (a_count == 0) {
			return true;
		}
		if (!a_out) {
			return false;
		}
		SafeReader  reader(reinterpret_cast<std::uintptr_t>(a_ptr), a_observer);
		auto*       dest = static_cast<char*>(a_out);
		std::size_t done = 0;
		while (done < a_count) {
			const auto copied = reader.Next(dest + done, std::min(a_count - done, kMaxChunk));
			if (copied == 0) {
				return false;
			}
			done += copied;
		}
		return true;
	}

	const char* Classify(Read a_read, std::string_view a_text, bool a_emptyIsBad) noexcept
	{
		switch (a_read) {
		case Read::unreadable:
			return "unreadable";
		case Read::unterminated:
			return "unterminated";
		case Read::ok:
		case Read::tooLong:
			break;
		}
		const auto text = a_read == Read::tooLong ? WithoutCutSequence(a_text) : a_text;
		if (!IsCleanText(text)) {
			return "notText";
		}
		if (a_emptyIsBad && a_read == Read::ok && text.empty()) {
			return "empty";
		}
		return nullptr;
	}
}
