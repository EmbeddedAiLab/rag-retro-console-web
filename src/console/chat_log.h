// Conversation storage + word-wrapped layout (thread-safe).
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

enum class Role { Operator, Bot, System };

struct DisplayLine {
  Role role = Role::System;
  bool first = false;  // first line of a message (gets the "OPERATOR >" label)
  bool code = false;   // inside a ``` block
  bool tail = false;   // last line of a message that is still streaming
  std::string body;
};

class ChatLog {
 public:
  void setBotName(const std::string& n) { botName_ = n; }
  std::string label(Role r) const;
  int labelWidth(Role r) const;

  void add(Role r, const std::string& text);
  void beginBot();
  void append(const std::string& chunk);
  void endBot();
  void clear();

  uint64_t version() const { return version_.load(); }

  // `cols` = columns available for label + text.
  int totalLines(int cols);
  void slice(int cols, int start, int count, std::vector<DisplayLine>& out);

 private:
  struct Message {
    Role role;
    std::string text;
    bool streaming;
  };
  void rebuild(int cols);

  std::mutex m_;
  std::vector<Message> msgs_;
  std::string botName_ = "AI";
  std::atomic<uint64_t> version_{1};
  std::vector<DisplayLine> cache_;
  int cacheCols_ = -1;
  uint64_t cacheVer_ = 0;
};
