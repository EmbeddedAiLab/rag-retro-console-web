// Raw terminal handling: alternate screen, raw keyboard input, ANSI output.
#pragma once
#include <csignal>
#include <string>
#include <termios.h>

struct Key {
  enum Type {
    None, Char, Enter, Backspace, Delete, Up, Down, Left, Right,
    PageUp, PageDown, Home, End, WheelUp, WheelDown,
    CtrlC, CtrlD, CtrlL, CtrlU, CtrlA, CtrlE, CtrlP, CtrlN, CtrlT, Tab, Escape
  };
  Type type = None;
  char32_t ch = 0;
};

class Terminal {
 public:
  static inline volatile std::sig_atomic_t g_quit = 0;
  static inline volatile std::sig_atomic_t g_resized = 0;

  Terminal() = default;
  ~Terminal();
  Terminal(const Terminal&) = delete;
  Terminal& operator=(const Terminal&) = delete;

  // Enters raw mode + alternate screen. Everything the process (or the rkllm
  // runtime) prints to stdout/stderr afterwards is redirected into `logPath`
  // so it cannot corrupt the UI; the UI itself writes to a duplicate of the
  // original stdout.
  bool open(const std::string& logPath, bool mouse);
  void close();

  void size(int& cols, int& rows) const;
  void write(const std::string& data) const;

  // Waits up to timeoutMs for one key. Returns false on timeout.
  bool pollKey(int timeoutMs, Key& out);

 private:
  enum ParseResult { kOk, kIncomplete, kEmpty };
  ParseResult parse(Key& out);

  termios orig_{};
  bool open_ = false;
  bool mouse_ = false;
  int uiFd_ = -1;
  int saveOut_ = -1;
  int saveErr_ = -1;
  std::string buf_;
};
