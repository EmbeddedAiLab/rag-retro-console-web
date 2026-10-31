#include "chat_log.h"

#include <algorithm>

#include "utf8.h"

std::string ChatLog::label(Role r) const {
  switch (r) {
    case Role::Operator: return "OPERATOR > ";
    case Role::Bot: return botName_ + " > ";
    case Role::System: return "SYS > ";
  }
  return "";
}

int ChatLog::labelWidth(Role r) const { return u8::width(label(r)); }

void ChatLog::add(Role r, const std::string& text) {
  std::lock_guard<std::mutex> lk(m_);
  msgs_.push_back({r, text, false});
  if (msgs_.size() > 400) msgs_.erase(msgs_.begin(), msgs_.begin() + 100);
  ++version_;
}

void ChatLog::beginBot() {
  std::lock_guard<std::mutex> lk(m_);
  msgs_.push_back({Role::Bot, "", true});
  ++version_;
}

void ChatLog::append(const std::string& chunk) {
  std::lock_guard<std::mutex> lk(m_);
  if (msgs_.empty() || msgs_.back().role != Role::Bot) msgs_.push_back({Role::Bot, "", true});
  msgs_.back().text += chunk;
  ++version_;
}

void ChatLog::endBot() {
  std::lock_guard<std::mutex> lk(m_);
  for (auto& m : msgs_) m.streaming = false;
  ++version_;
}

void ChatLog::clear() {
  std::lock_guard<std::mutex> lk(m_);
  msgs_.clear();
  ++version_;
}

static std::string trimBlank(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == '\n' || s[a] == '\r' || s[a] == ' ')) ++a;
  while (b > a && (s[b - 1] == '\n' || s[b - 1] == '\r' || s[b - 1] == ' ')) --b;
  return s.substr(a, b - a);
}

void ChatLog::rebuild(int cols) {
  cache_.clear();
  for (const auto& m : msgs_) {
    const int lw = labelWidth(m.role);
    const int bodyW = std::max(8, cols - lw - 1);
    const std::string text = trimBlank(m.text);
    const size_t startIdx = cache_.size();

    bool inCode = false;
    size_t pos = 0;
    while (pos <= text.size()) {
      size_t nl = text.find('\n', pos);
      std::string logical = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

      bool code = false;
      if (m.role == Role::Bot) {
        size_t k = logical.find_first_not_of(' ');
        bool fence = k != std::string::npos && logical.compare(k, 3, "```") == 0;
        if (fence) {
          code = true;
          inCode = !inCode;
        } else {
          code = inCode;
        }
      }
      std::vector<std::string> parts;
      u8::wrapLine(logical, bodyW, !code, parts);
      for (auto& p : parts) {
        DisplayLine dl;
        dl.role = m.role;
        dl.code = code;
        dl.body = std::move(p);
        cache_.push_back(std::move(dl));
      }
    }
    if (cache_.size() == startIdx) cache_.push_back(DisplayLine{m.role, false, false, false, ""});
    cache_[startIdx].first = true;
    if (m.streaming) cache_.back().tail = true;
    cache_.push_back(DisplayLine{Role::System, false, false, false, ""});  // spacer
  }
  cacheCols_ = cols;
  cacheVer_ = version_.load();
}

int ChatLog::totalLines(int cols) {
  std::lock_guard<std::mutex> lk(m_);
  if (cacheCols_ != cols || cacheVer_ != version_.load()) rebuild(cols);
  return static_cast<int>(cache_.size());
}

void ChatLog::slice(int cols, int start, int count, std::vector<DisplayLine>& out) {
  std::lock_guard<std::mutex> lk(m_);
  if (cacheCols_ != cols || cacheVer_ != version_.load()) rebuild(cols);
  out.clear();
  int n = static_cast<int>(cache_.size());
  for (int i = std::max(0, start); i < n && i < start + count; ++i) out.push_back(cache_[i]);
}
