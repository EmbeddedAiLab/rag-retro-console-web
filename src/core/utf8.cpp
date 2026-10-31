#include "utf8.h"

namespace u8 {

char32_t next(const std::string& s, size_t& i) {
  unsigned char c = static_cast<unsigned char>(s[i]);
  if (c < 0x80) {
    ++i;
    return c;
  }
  int n = 0;
  char32_t cp = 0;
  if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
  else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
  else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
  else { ++i; return 0xFFFD; }
  if (i + static_cast<size_t>(n) >= s.size()) { ++i; return 0xFFFD; }
  for (int k = 1; k <= n; ++k) {
    unsigned char b = static_cast<unsigned char>(s[i + k]);
    if ((b & 0xC0) != 0x80) { ++i; return 0xFFFD; }
    cp = (cp << 6) | (b & 0x3F);
  }
  i += n + 1;
  return cp;
}

void append(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

bool isControl(char32_t cp) { return cp < 0x20 || (cp >= 0x7F && cp < 0xA0); }

int cpWidth(char32_t cp) {
  if (isControl(cp)) return 0;
  if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x200B && cp <= 0x200F) ||
      (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F))
    return 0;
  if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) ||
      (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
      (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
      (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
      (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
      (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
      (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD))
    return 2;
  return 1;
}

int width(const std::string& s) {
  int w = 0;
  size_t i = 0;
  while (i < s.size()) w += cpWidth(next(s, i));
  return w;
}

int width(const std::u32string& s, size_t from, size_t to) {
  int w = 0;
  if (to > s.size()) to = s.size();
  for (size_t i = from; i < to; ++i) w += cpWidth(s[i]);
  return w;
}

std::u32string toU32(const std::string& s) {
  std::u32string r;
  size_t i = 0;
  while (i < s.size()) r.push_back(next(s, i));
  return r;
}

std::string toU8(const std::u32string& s) {
  std::string r;
  for (char32_t c : s) append(r, c);
  return r;
}

std::string clip(const std::string& s, int cols) {
  std::string out;
  int used = 0;
  size_t i = 0;
  while (i < s.size()) {
    char32_t cp = next(s, i);
    if (isControl(cp)) continue;
    int w = cpWidth(cp);
    if (used + w > cols) break;
    append(out, cp);
    used += w;
  }
  return out;
}

void wrapLine(const std::string& s, int cols, bool wordWrap, std::vector<std::string>& out) {
  if (cols < 1) cols = 1;
  const size_t base = out.size();
  std::string line;
  int lw = 0;
  int lastSpaceByte = -1;
  int lastSpaceW = 0;

  auto flush = [&]() {
    out.push_back(line);
    line.clear();
    lw = 0;
    lastSpaceByte = -1;
  };

  size_t i = 0;
  while (i < s.size()) {
    char32_t cp = next(s, i);
    if (cp == '\t') cp = ' ';
    if (isControl(cp)) continue;
    int w = cpWidth(cp);

    if (wordWrap && cp == ' ' && lw == 0 && out.size() > base) continue;  // no leading blanks after a wrap

    if (lw + w > cols) {
      if (cp == ' ') {
        flush();
        continue;
      }
      if (wordWrap && lastSpaceByte >= 0) {
        std::string head = line.substr(0, lastSpaceByte);
        std::string tail = line.substr(lastSpaceByte + 1);
        int tailW = lw - lastSpaceW - 1;
        out.push_back(head);
        line = tail;
        lw = tailW;
        lastSpaceByte = -1;
        if (lw + w > cols) flush();
      } else {
        flush();
      }
    }
    if (wordWrap && cp == ' ') {
      lastSpaceByte = static_cast<int>(line.size());
      lastSpaceW = lw;
    }
    append(line, cp);
    lw += w;
  }
  out.push_back(line);
}

}  // namespace u8
