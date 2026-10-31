#include "terminal.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "utf8.h"

namespace {
void onTermSignal(int) { Terminal::g_quit = 1; }
void onWinch(int) { Terminal::g_resized = 1; }
}  // namespace

Terminal::~Terminal() { close(); }

bool Terminal::open(const std::string& logPath, bool mouse) {
  if (open_) return true;
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;

  uiFd_ = ::dup(STDOUT_FILENO);
  saveOut_ = ::dup(STDOUT_FILENO);
  saveErr_ = ::dup(STDERR_FILENO);
  if (uiFd_ < 0) return false;

  // Keep rkllm's own console chatter out of the UI.
  int lg = ::open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (lg < 0) lg = ::open("/dev/null", O_WRONLY);
  if (lg >= 0) {
    ::dup2(lg, STDOUT_FILENO);
    ::dup2(lg, STDERR_FILENO);
    ::close(lg);
  }

  if (tcgetattr(STDIN_FILENO, &orig_) != 0) return false;
  termios raw = orig_;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_cflag |= CS8;
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = onTermSignal;
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
  sa.sa_handler = onWinch;
  sigaction(SIGWINCH, &sa, nullptr);

  mouse_ = mouse;
  open_ = true;
  // alt screen, no autowrap, steady block cursor, optional SGR mouse wheel
  std::string init = "\x1b[?1049h\x1b[?7l\x1b[2 q\x1b[2J\x1b[H";
  if (mouse_) init += "\x1b[?1000h\x1b[?1006h";
  write(init);
  return true;
}

void Terminal::close() {
  if (!open_) return;
  open_ = false;
  std::string fin = "\x1b[0m";
  if (mouse_) fin += "\x1b[?1006l\x1b[?1000l";
  fin += "\x1b[?7h\x1b[?25h\x1b[0 q\x1b[?1049l";
  write(fin);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_);
  if (saveOut_ >= 0) { ::dup2(saveOut_, STDOUT_FILENO); ::close(saveOut_); saveOut_ = -1; }
  if (saveErr_ >= 0) { ::dup2(saveErr_, STDERR_FILENO); ::close(saveErr_); saveErr_ = -1; }
  if (uiFd_ >= 0) { ::close(uiFd_); uiFd_ = -1; }
}

void Terminal::size(int& cols, int& rows) const {
  winsize ws{};
  if (uiFd_ >= 0 && ioctl(uiFd_, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
    cols = ws.ws_col;
    rows = ws.ws_row;
  } else {
    cols = 80;
    rows = 24;
  }
}

void Terminal::write(const std::string& data) const {
  if (uiFd_ < 0) return;
  size_t off = 0;
  while (off < data.size()) {
    ssize_t n = ::write(uiFd_, data.data() + off, data.size() - off);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      return;
    }
    off += static_cast<size_t>(n);
  }
}

Terminal::ParseResult Terminal::parse(Key& out) {
  out = Key{};
  if (buf_.empty()) return kEmpty;
  unsigned char c = static_cast<unsigned char>(buf_[0]);

  if (c == 0x1b) {
    if (buf_.size() == 1) return kIncomplete;
    if (buf_[1] == '[' || buf_[1] == 'O') {
      size_t j = 2;
      while (j < buf_.size() && !(static_cast<unsigned char>(buf_[j]) >= 0x40 &&
                                  static_cast<unsigned char>(buf_[j]) <= 0x7E))
        ++j;
      if (j >= buf_.size()) return kIncomplete;
      char fin = buf_[j];
      std::string params = buf_.substr(2, j - 2);
      buf_.erase(0, j + 1);
      switch (fin) {
        case 'A': out.type = Key::Up; break;
        case 'B': out.type = Key::Down; break;
        case 'C': out.type = Key::Right; break;
        case 'D': out.type = Key::Left; break;
        case 'H': out.type = Key::Home; break;
        case 'F': out.type = Key::End; break;
        case '~':
          if (params == "1" || params == "7") out.type = Key::Home;
          else if (params == "4" || params == "8") out.type = Key::End;
          else if (params == "3") out.type = Key::Delete;
          else if (params == "5") out.type = Key::PageUp;
          else if (params == "6") out.type = Key::PageDown;
          break;
        case 'M':
        case 'm':  // SGR mouse: <btn;x;y
          if (!params.empty() && params[0] == '<' && fin == 'M') {
            int btn = std::atoi(params.c_str() + 1);
            if (btn == 64) out.type = Key::WheelUp;
            else if (btn == 65) out.type = Key::WheelDown;
          }
          break;
        default: break;
      }
      return kOk;
    }
    buf_.erase(0, 2);  // Alt+key: ignored
    return kOk;
  }

  switch (c) {
    case 0x0d: case 0x0a: buf_.erase(0, 1); out.type = Key::Enter; return kOk;
    case 0x7f: case 0x08: buf_.erase(0, 1); out.type = Key::Backspace; return kOk;
    case 0x03: buf_.erase(0, 1); out.type = Key::CtrlC; return kOk;
    case 0x04: buf_.erase(0, 1); out.type = Key::CtrlD; return kOk;
    case 0x0c: buf_.erase(0, 1); out.type = Key::CtrlL; return kOk;
    case 0x15: buf_.erase(0, 1); out.type = Key::CtrlU; return kOk;
    case 0x01: buf_.erase(0, 1); out.type = Key::CtrlA; return kOk;
    case 0x05: buf_.erase(0, 1); out.type = Key::CtrlE; return kOk;
    case 0x10: buf_.erase(0, 1); out.type = Key::CtrlP; return kOk;
    case 0x0e: buf_.erase(0, 1); out.type = Key::CtrlN; return kOk;
    case 0x14: buf_.erase(0, 1); out.type = Key::CtrlT; return kOk;
    case 0x09: buf_.erase(0, 1); out.type = Key::Tab; return kOk;
    default: break;
  }

  if (c >= 0x20) {
    size_t need = 1;
    if (c >= 0xF0) need = 4;
    else if (c >= 0xE0) need = 3;
    else if (c >= 0xC0) need = 2;
    if (buf_.size() < need) return kIncomplete;
    size_t i = 0;
    char32_t cp = u8::next(buf_, i);
    buf_.erase(0, i);
    if (cp != 0xFFFD && !u8::isControl(cp)) {
      out.type = Key::Char;
      out.ch = cp;
    }
    return kOk;
  }

  buf_.erase(0, 1);  // other control bytes
  return kOk;
}

bool Terminal::pollKey(int timeoutMs, Key& out) {
  for (;;) {
    ParseResult r = parse(out);
    if (r == kOk) {
      if (out.type == Key::None) continue;  // swallow ignored sequences
      return true;
    }
    int wait = (r == kIncomplete) ? 30 : timeoutMs;
    pollfd p{STDIN_FILENO, POLLIN, 0};
    int n = ::poll(&p, 1, wait);
    if (n > 0) {
      if (p.revents & POLLIN) {
        char tmp[256];
        ssize_t k = ::read(STDIN_FILENO, tmp, sizeof tmp);
        if (k > 0) {
          buf_.append(tmp, static_cast<size_t>(k));
          continue;
        }
        if (k == 0) { g_quit = 1; return false; }
        continue;
      }
      g_quit = 1;  // POLLHUP / POLLERR
      return false;
    }
    if (r == kIncomplete) {
      if (buf_.size() == 1 && buf_[0] == 0x1b) {
        buf_.clear();
        out = Key{};
        out.type = Key::Escape;
        return true;
      }
      buf_.erase(0, 1);  // never-completing garbage
      continue;
    }
    return false;
  }
}
