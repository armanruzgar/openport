#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace openport::md {

/// The length of the well-formed UTF-8 sequence (RFC 3629: no overlong forms, no
/// surrogates, nothing past U+10FFFF) that starts at `text[at]`, or 0 when none does.
[[nodiscard]] constexpr std::size_t utf8_sequence(std::string_view text, std::size_t at) noexcept {
  const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
  const auto lead = byte(at);
  if (lead < 0x80) return 1;
  std::size_t length = 0;
  unsigned char low = 0x80, high = 0xBF;  // the range of the second byte
  if (lead >= 0xC2 && lead <= 0xDF) length = 2;
  else if (lead == 0xE0) { length = 3; low = 0xA0; }
  else if (lead >= 0xE1 && lead <= 0xEC) length = 3;
  else if (lead == 0xED) { length = 3; high = 0x9F; }
  else if (lead >= 0xEE && lead <= 0xEF) length = 3;
  else if (lead == 0xF0) { length = 4; low = 0x90; }
  else if (lead >= 0xF1 && lead <= 0xF3) length = 4;
  else if (lead == 0xF4) { length = 4; high = 0x8F; }
  else return 0;
  if (at + length > text.size()) return 0;
  if (byte(at + 1) < low || byte(at + 1) > high) return 0;
  for (std::size_t i = 2; i < length; ++i)
    if (byte(at + i) < 0x80 || byte(at + i) > 0xBF) return 0;
  return length;
}

/// Whether every byte of `text` belongs to well-formed UTF-8, which JSON output needs.
[[nodiscard]] constexpr bool valid_utf8(std::string_view text) noexcept {
  for (std::size_t at = 0; at < text.size();) {
    const auto length = utf8_sequence(text, at);
    if (length == 0) return false;
    at += length;
  }
  return true;
}

/// `text` with U+FFFD in place of each byte that is not part of well-formed UTF-8:
/// for text from files or parsers that is shown or printed as JSON.
[[nodiscard]] inline std::string utf8_replaced(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t at = 0; at < text.size();) {
    if (const auto length = utf8_sequence(text, at)) {
      out.append(text.substr(at, length));
      at += length;
    } else {
      out.append("\xEF\xBF\xBD");
      ++at;
    }
  }
  return out;
}

/// The longest prefix of `text` at most `bytes` long that does not end inside a
/// UTF-8 sequence.
[[nodiscard]] constexpr std::string_view utf8_prefix(std::string_view text, std::size_t bytes) noexcept {
  if (text.size() <= bytes) return text;
  std::size_t cut = bytes;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
  return text.substr(0, cut);
}

}  // namespace openport::md
