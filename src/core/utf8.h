// UTF-8 helpers: decoding, display width, clipping and word wrapping.
#pragma once
#include <string>
#include <vector>

namespace u8 {

// Decode the next code point at s[i] and advance i. Invalid bytes -> U+FFFD.
char32_t next(const std::string& s, size_t& i);
void append(std::string& out, char32_t cp);

// True for C0/C1 control characters (never printed to the terminal).
bool isControl(char32_t cp);

// Terminal columns occupied by a code point (0, 1 or 2).
int cpWidth(char32_t cp);
int width(const std::string& s);
int width(const std::u32string& s, size_t from, size_t to);

std::u32string toU32(const std::string& s);
std::string toU8(const std::u32string& s);

// Cut a string so that it fits in `cols` columns (control characters removed).
std::string clip(const std::string& s, int cols);

// Wrap one logical line into display lines of at most `cols` columns.
// wordWrap=false hard-wraps (used for code blocks).
void wrapLine(const std::string& s, int cols, bool wordWrap, std::vector<std::string>& out);

}  // namespace u8
