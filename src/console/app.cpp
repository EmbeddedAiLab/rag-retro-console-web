#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

#include "utf8.h"

namespace {

std::string fg(int n) { return "\x1b[38;5;" + std::to_string(n) + "m"; }
std::string bgc(int n) { return "\x1b[48;5;" + std::to_string(n) + "m"; }
std::string repeat(const std::string& s, int n) {
  std::string r;
  for (int i = 0; i < n; ++i) r += s;
  return r;
}
std::string at(int row, int col = 1) {
  return "\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H";
}

struct ThemeDef {
  const char* name;
  int frame, title, sub, accent, oper, bot, sys, dim, code, warn, crit, bg;
};
const ThemeDef kThemes[] = {
    {"green", 34, 46, 28, 51, 214, 46, 178, 28, 79, 220, 196, 16},
    {"amber", 172, 214, 130, 220, 223, 214, 137, 94, 180, 226, 196, 16},
    {"ice", 31, 51, 24, 117, 229, 51, 245, 24, 153, 220, 203, 16},
};
const int kThemeCount = sizeof(kThemes) / sizeof(kThemes[0]);

// A run of styled text with a known display width.
struct Cells {
  std::string s;
  int w = 0;
  void put(const std::string& text, const std::string& style, const std::string& base) {
    s += style;
    s += text;
    s += base;
    w += u8::width(text);
  }
  void space(int n) {
    if (n > 0) {
      s.append(static_cast<size_t>(n), ' ');
      w += n;
    }
  }
};

std::string fmtGb(double mb) {
  char b[16];
  std::snprintf(b, sizeof b, "%.1f", mb / 1024.0);
  return b;
}

std::string tailOfFile(const std::string& path, int n) {
  std::ifstream f(path);
  std::vector<std::string> lines;
  std::string l;
  while (std::getline(f, l)) {
    if (!l.empty()) lines.push_back(l);
  }
  std::string out;
  for (size_t i = lines.size() > static_cast<size_t>(n) ? lines.size() - n : 0; i < lines.size(); ++i)
    out += lines[i] + "\n";
  return out;
}

}  // namespace

App::App(Options o) : opt_(std::move(o)) {
  chat_.setBotName(opt_.botName.empty() ? "AI" : opt_.botName);
  engine_.setKeepHistory(opt_.engine.keepHistory);
}

void App::applyTheme(const std::string& name) {
  themeIdx_ = 0;
  for (int i = 0; i < kThemeCount; ++i)
    if (name == kThemes[i].name) themeIdx_ = i;
  const ThemeDef& t = kThemes[themeIdx_];
  st_.base = "\x1b[0m" + (opt_.noBg ? std::string() : bgc(t.bg));
  st_.bold = "\x1b[1m";
  st_.frame = fg(t.frame);
  st_.title = "\x1b[1m" + fg(t.title);
  st_.sub = fg(t.sub);
  st_.accent = "\x1b[1m" + fg(t.accent);
  st_.oper = fg(t.oper);
  st_.bot = fg(t.bot);
  st_.sys = fg(t.sys);
  st_.dim = fg(t.dim);
  st_.code = fg(t.code);
  st_.warn = fg(t.warn);
  st_.crit = fg(t.crit);
  needClear_ = true;
  dirty_ = true;
}

void App::cycleTheme() {
  applyTheme(kThemes[(themeIdx_ + 1) % kThemeCount].name);
}

// ---------------------------------------------------------------------------
// Engine callbacks (worker thread)
// ---------------------------------------------------------------------------
void App::onToken(const std::string& text) {
  {
    std::lock_guard<std::mutex> lk(statM_);
    auto now = Clock::now();
    if (tokens_ == 0) {
      tFirst_ = now;
      latencyMs_ = std::chrono::duration<double, std::milli>(now - tStart_).count();
    }
    ++tokens_;
    tLast_ = now;
  }
  chat_.append(text);
}

void App::onFinish(FinishStatus status, const std::string& error) {
  std::string src;
  {
    std::lock_guard<std::mutex> lk(statM_);
    generating_ = false;
    src.swap(pendingSources_);
  }
  chat_.endBot();
  if (status == FinishStatus::Ok && !src.empty()) {
    chat_.add(Role::System, src);
  } else if (status == FinishStatus::Aborted) {
    chat_.add(Role::System, "GENERATION ABORTED BY OPERATOR.");
  } else if (status == FinishStatus::Error) {
    chat_.add(Role::System,
              "RUNTIME ERROR: " + error + ". CONTEXT FULL? TRY /clear OR /memory off.");
  }
}

// ---------------------------------------------------------------------------
// Boot screen
// ---------------------------------------------------------------------------
bool App::boot() {
  const bool slow = !opt_.fastBoot;
  auto nap = [&](int ms) {
    if (slow) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  };
  int W, H;
  term_.size(W, H);
  term_.write(st_.base + "\x1b[2J");

  int row = 2;
  auto line = [&](const std::string& text, const std::string& style) {
    term_.write(at(row, 3) + style + u8::clip(text, W - 4) + st_.base);
    ++row;
  };
  auto status = [&](const std::string& label, const std::string& value, const std::string& tag,
                    const std::string& tagStyle) {
    std::string dots = " " + repeat(".", std::max(2, 30 - u8::width(label))) + " ";
    std::string l = label + dots + value;
    term_.write(at(row, 3) + st_.title + l + " " + st_.base + tagStyle + tag + st_.base);
    ++row;
  };

  line(opt_.soc + " // NEURAL COMPUTE TERMINAL // RETRO CONSOLE", st_.title);
  line("NEURAL-BIOS v1.0   //   " + opt_.board, st_.sub);
  ++row;
  nap(250);

  Telemetry t = sampler_.sample();
  status("CPU CORES", std::to_string(t.cpuCount > 0 ? t.cpuCount : 4) + " ONLINE", "[ OK ]", st_.accent);
  nap(140);
  status("MEMORY CHECK", t.ramOk ? fmtGb(t.ramTotalMb) + " GB" : "N/A", "[ OK ]", st_.accent);
  nap(140);
  bool npu = sampler_.npuAvailable();
  status("NPU", npu ? "DETECTED" : "NO TELEMETRY", npu ? "[ OK ]" : "[ -- ]", npu ? st_.accent : st_.warn);
  nap(140);
  {
    std::string val = "DISABLED", tag = "[ -- ]";
    std::string style = st_.warn;
    if (opt_.ragEnabled) {
      term_.write(at(row, 3) + st_.accent + "INDEXING DOCUMENTS ..." + st_.base);
      if (rag_.open(opt_.dbPath, ragError_)) {
        IngestStats s = rag_.ingest(opt_.docsDir);
        refreshRagCounts();
        ragOn_ = ragChunks_ > 0;
        if (!s.error.empty()) {
          ragError_ = s.error;
          val = "NO DOCS FOLDER";
        } else {
          val = std::to_string(ragFiles_) + " FILES / " + std::to_string(ragChunks_) + " CHUNKS";
          tag = "[ OK ]";
          style = st_.accent;
        }
      } else {
        val = "UNAVAILABLE";
      }
    }
    status("DOCUMENT INDEX", val, tag, style);
    nap(140);
  }
  std::string file = std::filesystem::path(opt_.engine.modelPath).filename().string();
  status("RKLLM RUNTIME", std::string("v") + RKLLM_VERSION_LABEL, "[ OK ]", st_.accent);
  nap(140);
  ++row;
  line("LOADING MODEL: " + file, st_.accent);
  line("CONTEXT " + std::to_string(opt_.engine.maxContext) + " TOKENS  //  MAX REPLY " +
           std::to_string(opt_.engine.maxNewTokens),
       st_.sub);
  ++row;
  const int barRow = row;
  ++row;

  auto fut = std::async(std::launch::async, [this] { return engine_.init(opt_.engine, initError_); });
  const int barW = std::min(40, W - 24);
  const char spin[] = "|/-\\";
  int tick = 0;
  auto start = Clock::now();
  while (fut.wait_for(std::chrono::milliseconds(70)) != std::future_status::ready) {
    int span = std::max(1, barW - 5);
    int p = tick % (2 * span);
    if (p > span) p = 2 * span - p;
    std::string bar;
    for (int i = 0; i < barW; ++i) bar += (i >= p && i < p + 5) ? "█" : "░";
    term_.write(at(barRow, 3) + st_.accent + "[" + st_.base + st_.title + bar + st_.base + st_.accent +
                "] " + st_.base + st_.warn + "LOADING WEIGHTS " + spin[tick % 4] + st_.base);
    ++tick;
  }
  bool ok = fut.get();
  // keep the bar visible for a moment in the demo-style boot
  if (slow) {
    auto el = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    if (el < 900) std::this_thread::sleep_for(std::chrono::milliseconds(900 - el));
  }

  if (ok) {
    term_.write(at(barRow, 3) + st_.accent + "[" + st_.base + st_.title + repeat("█", barW) + st_.base +
                st_.accent + "] " + st_.base + st_.title + "MODEL ONLINE       " + st_.base);
    row = barRow + 2;
    line("ALL SYSTEMS NOMINAL. ENTERING CONSOLE...", st_.title);
    nap(600);
    return true;
  }

  row = barRow + 2;
  line("*** MODEL LOAD FAILED ***", st_.crit);
  line(initError_, st_.warn);
  std::string tail = tailOfFile(opt_.logPath, 5);
  size_t pos = 0;
  while (pos < tail.size()) {
    size_t nl = tail.find('\n', pos);
    if (nl == std::string::npos) nl = tail.size();
    line(tail.substr(pos, nl - pos), st_.dim);
    pos = nl + 1;
  }
  ++row;
  line("PRESS ANY KEY TO EXIT", st_.accent);
  Key k;
  while (!Terminal::g_quit && !term_.pollKey(200, k)) {
  }
  return false;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void App::render() {
  int W, H;
  term_.size(W, H);
  const Styles& S = st_;
  std::string out;
  out.reserve(static_cast<size_t>(W) * H * 8);

  if (needClear_) {
    out += S.base + "\x1b[2J";
    needClear_ = false;
  }
  if (W < 60 || H < 20) {
    out += S.base + "\x1b[2J" + at(1) + S.warn + "TERMINAL TOO SMALL: NEED 60x20, HAVE " +
           std::to_string(W) + "x" + std::to_string(H) + S.base;
    term_.write(out);
    needClear_ = true;
    return;
  }

  const int IW = W - 4;  // text columns between "║ " and " ║"
  const auto now = Clock::now();
  const long long ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
  const bool busy = engine_.busy();
  const bool blink = (ms / 500) % 2 == 0;
  const char spinCh = "|/-\\"[(ms / 120) % 4];

  auto sep = [&](int r) {
    out += at(r) + S.frame + "╠" + repeat("═", W - 2) + "╣" + S.base;
  };
  auto row = [&](int r, const Cells& c, const std::string& right) {
    out += at(r) + S.frame + "║ " + S.base + c.s + std::string(static_cast<size_t>(std::max(0, IW - c.w)), ' ') +
           S.frame + " " + right + S.base;
  };
  const std::string plainRight = S.frame + "║";

  // ---- top banner -------------------------------------------------------
  out += at(1) + S.frame + "╔" + repeat("═", W - 2) + "╗" + S.base;

  std::string model = opt_.modelLabel.empty() ? "MODEL" : opt_.modelLabel;
  std::string t1 = opt_.soc + " // NEURAL COMPUTE TERMINAL // RETRO CONSOLE";
  std::string t2 = opt_.board + "  |  " + model + "  |  RKLLM " + RKLLM_VERSION_LABEL;
  t1 = u8::clip(t1, IW);
  t2 = u8::clip(t2, IW);
  {
    Cells c;
    c.space((IW - u8::width(t1)) / 2);
    c.put(t1, S.title, S.base);
    row(2, c, plainRight);
    Cells d;
    d.space((IW - u8::width(t2)) / 2);
    d.put(t2, S.sub, S.base);
    row(3, d, plainRight);
  }
  sep(4);

  // ---- telemetry --------------------------------------------------------
  {
    Cells c;
    c.put("SYSTEM TELEMETRY", S.accent, S.base);
    std::string live = telem_.npuOk ? "" : "";
    (void)live;
    row(5, c, plainRight);
  }
  const int segW = (IW - 2) / 3;
  const int sufW = segW >= 24 ? 9 : 5;
  auto bar = [&](Cells& c, const std::string& label, bool ok, double pct, const std::string& suffix) {
    int barW = std::max(2, segW - static_cast<int>(label.size()) - 1 - 1 - sufW);
    c.put(label + " ", S.accent, S.base);
    int filled = ok ? static_cast<int>(std::lround(std::min(100.0, std::max(0.0, pct)) / 100.0 * barW)) : 0;
    const std::string& col = pct < 60 ? S.title : (pct < 85 ? S.warn : S.crit);
    c.put(repeat("█", filled), ok ? col : S.dim, S.base);
    c.put(repeat("░", barW - filled), S.dim, S.base);
    std::string txt = ok ? suffix : "N/A";
    c.put(" " + std::string(static_cast<size_t>(std::max(0, sufW - u8::width(txt))), ' ') + txt, ok ? S.title : S.dim,
          S.base);
  };
  {
    Cells c;
    char b[32];
    std::snprintf(b, sizeof b, "%.0f%%", telem_.npuPct);
    bar(c, "NPU", telem_.npuOk, telem_.npuPct, b);
    c.space(1);
    std::snprintf(b, sizeof b, "%.0f%%", telem_.cpuPct);
    bar(c, "CPU", telem_.cpuOk, telem_.cpuPct, b);
    c.space(1);
    double rp = telem_.ramTotalMb > 0 ? 100.0 * telem_.ramUsedMb / telem_.ramTotalMb : 0;
    std::string rs;
    if (sufW >= 9) rs = fmtGb(telem_.ramUsedMb) + "/" + fmtGb(telem_.ramTotalMb) + "G";
    else { std::snprintf(b, sizeof b, "%.0f%%", rp); rs = b; }
    bar(c, "RAM", telem_.ramOk, rp, rs);
    row(6, c, plainRight);
  }
  {
    int tokens;
    double tokps = -1, lat;
    bool gen;
    {
      std::lock_guard<std::mutex> lk(statM_);
      tokens = tokens_;
      lat = latencyMs_;
      gen = generating_;
      if (tokens_ >= 2) {
        auto end = gen ? now : tLast_;
        double dt = std::chrono::duration<double>(end - tFirst_).count();
        if (dt > 0.01) tokps = (tokens_ - 1) / dt;
      }
    }
    Cells c;
    auto piece = [&](const std::string& label, const std::string& value, const std::string& vStyle) {
      int need = u8::width(label) + 1 + u8::width(value) + (c.w > 0 ? 3 : 0);
      if (c.w + need > IW) return;
      if (c.w > 0) c.space(3);
      c.put(label + " ", S.dim, S.base);
      c.put(value, vStyle, S.base);
    };
    char b[32];
    if (busy) piece("INFERENCE:", std::string(tokens == 0 ? "PREFILL " : "GENERATING ") + spinCh, S.warn);
    else piece("INFERENCE:", "IDLE", S.title);
    if (telem_.tempOk) { std::snprintf(b, sizeof b, "%.1fC", telem_.tempC); piece("TEMP", b, telem_.tempC > 80 ? S.crit : S.title); }
    else piece("TEMP", "N/A", S.dim);
    if (tokps >= 0) { std::snprintf(b, sizeof b, "%.1f", tokps); piece("TOK/S", b, S.title); }
    else piece("TOK/S", "--", S.dim);
    piece("TOKENS", std::to_string(tokens), S.title);
    if (lat >= 0) { std::snprintf(b, sizeof b, "%.2fs", lat / 1000.0); piece("LATENCY", b, S.title); }
    piece("MEMORY", engine_.keepHistory() ? "ON" : "OFF", engine_.keepHistory() ? S.title : S.dim);
    row(7, c, plainRight);
  }
  sep(8);

  // ---- conversation -----------------------------------------------------
  const int ch = H - 15;
  viewH_ = ch;
  int T = chat_.totalLines(IW);
  if (W == lastCols_ && scrollOff_ > 0 && T > lastTotal_) scrollOff_ += T - lastTotal_;
  lastCols_ = W;
  lastTotal_ = T;
  totalLines_ = T;
  const int maxOff = std::max(0, T - ch);
  scrollOff_ = std::min(std::max(0, scrollOff_), maxOff);

  {
    Cells c;
    c.put("AI CONVERSATION", S.accent, S.base);
    std::string hint = "[SCROLL: ↓ ↑]";
    std::string state = scrollOff_ == 0 ? " LIVE" : " PAUSED -" + std::to_string(scrollOff_);
    int rw = u8::width(hint) + u8::width(state);
    {
      std::string rt;
      std::string rs = S.dim;
      if (ingestFut_.valid()) { rt = "[RAG: INDEXING...]"; rs = S.warn; }
      else if (!rag_.isOpen()) rt = "[RAG: N/A]";
      else if (!ragOn_) rt = "[RAG: OFF]";
      else { rt = "[RAG: ON | " + std::to_string(ragChunks_) + " CHUNKS]"; rs = S.title; }
      if (c.w + 2 + u8::width(rt) + 1 + rw <= IW) {
        c.space(2);
        c.put(rt, rs, S.base);
      }
    }
    c.space(IW - c.w - rw);
    c.put(hint, S.dim, S.base);
    c.put(state, scrollOff_ == 0 ? S.title : S.warn, S.base);
    row(9, c, plainRight);
  }
  out += at(10) + S.frame + "║ " + S.base + S.dim + repeat("─", IW) + S.base + S.frame + " ║" + S.base;

  const int start = T > ch ? T - ch - scrollOff_ : 0;
  std::vector<DisplayLine> lines;
  chat_.slice(IW, start, ch, lines);

  int thumbSize = 0, thumbTop = 0;
  if (T > ch) {
    thumbSize = std::max(1, ch * ch / T);
    thumbTop = maxOff > 0 ? (maxOff - scrollOff_) * (ch - thumbSize) / maxOff : 0;
  }
  for (int i = 0; i < ch; ++i) {
    Cells c;
    if (i < static_cast<int>(lines.size())) {
      const DisplayLine& dl = lines[i];
      const bool spacer = dl.body.empty() && !dl.first && dl.role == Role::System;
      const int lw = spacer ? 0 : chat_.labelWidth(dl.role);
      const std::string& roleStyle = dl.role == Role::Operator ? S.oper : (dl.role == Role::Bot ? S.bot : S.sys);
      if (dl.first) c.put(chat_.label(dl.role), S.bold + roleStyle, S.base);
      else c.space(lw);
      c.put(u8::clip(dl.body, IW - lw), dl.code ? S.code : roleStyle, S.base);
      if (dl.tail && c.w < IW) c.put(blink ? "█" : " ", S.bot, S.base);
    }
    bool thumb = thumbSize > 0 && i >= thumbTop && i < thumbTop + thumbSize;
    row(11 + i, c, thumb ? S.title + "█" : plainRight);
  }

  // ---- prompt -------------------------------------------------------------
  sep(H - 4);
  const int promptRow = H - 3;
  const std::string prompt = "OPERATOR@" + opt_.soc + ":~$ ";
  const int promptW = u8::width(prompt);
  const int avail = std::max(4, IW - promptW - 1);
  if (cursor_ > input_.size()) cursor_ = input_.size();
  if (inStart_ > cursor_) inStart_ = cursor_;
  while (inStart_ < cursor_ && u8::width(input_, inStart_, cursor_) > avail - 1) ++inStart_;
  int cursorX;
  {
    Cells c;
    c.put(prompt, S.accent, S.base);
    std::string visible = u8::clip(u8::toU8(input_.substr(inStart_)), avail);
    c.put(visible, S.title, S.base);
    row(promptRow, c, plainRight);
    cursorX = 3 + promptW + u8::width(input_, inStart_, cursor_);
  }
  sep(H - 2);

  // ---- footer -------------------------------------------------------------
  {
    using KV = std::vector<std::pair<std::string, std::string>>;
    KV full = {{"[ENTER]", "SEND"}, {"[↑/↓]", "SCROLL"}, {"[PGUP/PGDN]", "PAGE"},
               {"[CTRL+L]", "CLEAR"}, {"[CTRL+C]", "STOP"}, {"[/exit]", "QUIT"}};
    KV mid = {{"[ENTER]", "SEND"}, {"[↑/↓]", "SCROLL"}, {"[^L]", "CLEAR"}, {"[^C]", "STOP"}, {"[/exit]", "QUIT"}};
    KV tiny = {{"[ENTER]", "SEND"}, {"[↑/↓]", "SCROLL"}, {"[^C]", "STOP"}, {"[/exit]", "QUIT"}};
    auto width = [](const KV& kv) {
      int w = 0;
      for (size_t i = 0; i < kv.size(); ++i)
        w += u8::width(kv[i].first) + 1 + u8::width(kv[i].second) + (i ? 2 : 0);
      return w;
    };
    const KV* use = &tiny;
    if (width(full) <= IW) use = &full;
    else if (width(mid) <= IW) use = &mid;
    Cells c;
    for (size_t i = 0; i < use->size(); ++i) {
      if (i) c.space(2);
      c.put((*use)[i].first, S.accent, S.base);
      c.put(" " + (*use)[i].second, S.sub, S.base);
    }
    row(H - 1, c, plainRight);
  }
  out += at(H) + S.frame + "╚" + repeat("═", W - 2) + "╝" + S.base;

  out += at(promptRow, cursorX) + "\x1b[?25h";
  term_.write(out);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
void App::scrollBy(int lines) {
  int maxOff = std::max(0, totalLines_ - viewH_);
  scrollOff_ = std::min(std::max(0, scrollOff_ + lines), maxOff);
  dirty_ = true;
}

void App::handleKey(const Key& k) {
  dirty_ = true;
  switch (k.type) {
    case Key::Char:
      if (input_.size() < 4000) {
        input_.insert(input_.begin() + static_cast<long>(cursor_), k.ch);
        ++cursor_;
      }
      histIdx_ = -1;
      break;
    case Key::Enter: submit(); break;
    case Key::Backspace:
      if (cursor_ > 0) { input_.erase(cursor_ - 1, 1); --cursor_; }
      break;
    case Key::Delete:
      if (cursor_ < input_.size()) input_.erase(cursor_, 1);
      break;
    case Key::CtrlD:
      if (input_.empty()) quit_ = true;
      else if (cursor_ < input_.size()) input_.erase(cursor_, 1);
      break;
    case Key::Left: if (cursor_ > 0) --cursor_; break;
    case Key::Right: if (cursor_ < input_.size()) ++cursor_; break;
    case Key::CtrlA: cursor_ = 0; break;
    case Key::CtrlE: cursor_ = input_.size(); break;
    case Key::CtrlU: input_.clear(); cursor_ = 0; inStart_ = 0; break;
    case Key::Up: scrollBy(1); break;
    case Key::Down: scrollBy(-1); break;
    case Key::WheelUp: scrollBy(3); break;
    case Key::WheelDown: scrollBy(-3); break;
    case Key::PageUp: scrollBy(std::max(1, viewH_ - 1)); break;
    case Key::PageDown: scrollBy(-std::max(1, viewH_ - 1)); break;
    case Key::Home: scrollBy(1 << 20); break;
    case Key::End: scrollOff_ = 0; break;
    case Key::CtrlC:
      if (engine_.busy()) engine_.abort();
      else if (!input_.empty()) { input_.clear(); cursor_ = 0; inStart_ = 0; }
      else quit_ = true;
      break;
    case Key::CtrlL:
      if (engine_.busy()) chat_.add(Role::System, "STOP THE GENERATION FIRST (CTRL+C).");
      else clearAll();
      break;
    case Key::CtrlT: cycleTheme(); break;
    case Key::CtrlP:
      if (!history_.empty()) {
        if (histIdx_ < 0) histIdx_ = static_cast<int>(history_.size());
        if (histIdx_ > 0) --histIdx_;
        input_ = history_[static_cast<size_t>(histIdx_)];
        cursor_ = input_.size();
      }
      break;
    case Key::CtrlN:
      if (histIdx_ >= 0) {
        ++histIdx_;
        if (histIdx_ >= static_cast<int>(history_.size())) { histIdx_ = -1; input_.clear(); }
        else input_ = history_[static_cast<size_t>(histIdx_)];
        cursor_ = input_.size();
      }
      break;
    default: break;
  }
}

void App::refreshRagCounts() {
  ragFiles_ = rag_.fileCount();
  ragChunks_ = rag_.chunkCount();
}

std::string App::describeIngest(const IngestStats& s) const {
  if (!s.error.empty()) return "INDEXING FAILED: " + s.error;
  return "INDEX UPDATED: +" + std::to_string(s.added) + " NEW, " + std::to_string(s.updated) + " CHANGED, -" +
         std::to_string(s.removed) + " REMOVED, " + std::to_string(s.unchanged) + " UNCHANGED. NOW " +
         std::to_string(s.files) + " FILES / " + std::to_string(s.chunks) + " CHUNKS.";
}

void App::startIngest() {
  if (ingestFut_.valid()) {
    chat_.add(Role::System, "ALREADY INDEXING. WAIT FOR IT TO FINISH.");
    return;
  }
  chat_.add(Role::System, "SCANNING " + opt_.docsDir + " ...");
  const std::string dir = opt_.docsDir;
  ingestFut_ = std::async(std::launch::async, [this, dir] { return rag_.ingest(dir); });
}

void App::clearAll() {
  chat_.clear();
  scrollOff_ = 0;
  lastTotal_ = 0;
  bool ok = engine_.clearMemory();
  chat_.add(Role::System, ok ? "SCREEN AND MODEL MEMORY CLEARED." : "SCREEN CLEARED (MODEL MEMORY RESET FAILED).");
}

void App::submit() {
  std::string line = u8::toU8(input_);
  size_t a = line.find_first_not_of(" \t");
  size_t b = line.find_last_not_of(" \t");
  line = (a == std::string::npos) ? "" : line.substr(a, b - a + 1);
  input_.clear();
  cursor_ = 0;
  inStart_ = 0;
  histIdx_ = -1;
  if (line.empty()) return;
  history_.push_back(u8::toU32(line));
  if (history_.size() > 100) history_.erase(history_.begin());

  if (line[0] == '/') {
    command(line);
    return;
  }
  if (engine_.busy()) {
    chat_.add(Role::System, "INFERENCE BUSY. WAIT, OR PRESS CTRL+C TO STOP.");
    return;
  }
  chat_.add(Role::Operator, line);
  scrollOff_ = 0;

  std::string prompt = line;
  std::string sources;
  int keep = -1;
  if (ragOn_ && rag_.isOpen() && ragChunks_ > 0 && !ingestFut_.valid()) {
    std::vector<RagHit> hits = rag_.search(line, opt_.ragK);
    if (hits.empty()) {
      chat_.add(Role::System,
                "NO RELEVANT DOCUMENTS FOUND FOR THIS QUESTION. TRY OTHER KEYWORDS, ADD FILES + /ingest, "
                "OR /rag off FOR PLAIN CHAT.");
      return;
    }
    int used = 0;
    prompt = Rag::buildPrompt(line, hits, opt_.ragBytes, used);
    sources = Rag::formatSources(hits, used);
    keep = 0;  // the documents are re-sent every time, so do not pile up history
  }

  chat_.beginBot();
  {
    std::lock_guard<std::mutex> lk(statM_);
    tokens_ = 0;
    latencyMs_ = -1;
    generating_ = true;
    tStart_ = Clock::now();
    pendingSources_ = sources;
  }
  if (!engine_.generate(prompt, this, keep)) {
    {
      std::lock_guard<std::mutex> lk(statM_);
      generating_ = false;
    }
    chat_.endBot();
    chat_.add(Role::System, "COULD NOT START INFERENCE.");
  }
}

void App::command(const std::string& line) {
  std::string cmd = line.substr(0, line.find(' '));
  std::string arg = line.size() > cmd.size() ? line.substr(cmd.size() + 1) : "";
  const std::string argRaw = arg;
  for (auto& ch : cmd) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  for (auto& ch : arg) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

  if (cmd == "/exit" || cmd == "/quit") {
    quit_ = true;
  } else if (cmd == "/clear") {
    if (engine_.busy()) chat_.add(Role::System, "STOP THE GENERATION FIRST (CTRL+C).");
    else clearAll();
  } else if (cmd == "/memory") {
    if (arg == "on" || arg == "off") {
      engine_.setKeepHistory(arg == "on");
      chat_.add(Role::System, arg == "on" ? "MEMORY ON: THE MODEL REMEMBERS THIS CONVERSATION."
                                           : "MEMORY OFF: EVERY MESSAGE IS ANSWERED FROM SCRATCH.");
    } else {
      chat_.add(Role::System, std::string("MEMORY IS ") + (engine_.keepHistory() ? "ON" : "OFF") +
                                  ". USE /memory on OR /memory off.");
    }
  } else if (cmd == "/theme") {
    if (arg.empty()) cycleTheme();
    else applyTheme(arg);
    chat_.add(Role::System, std::string("THEME: ") + kThemes[themeIdx_].name + "  (green | amber | ice)");
  } else if (cmd == "/rag") {
    if (!rag_.isOpen()) {
      chat_.add(Role::System, "RAG UNAVAILABLE: " + (ragError_.empty() ? std::string("DISABLED (--no-rag)") : ragError_));
    } else if (arg == "on" || arg == "off") {
      ragOn_ = arg == "on";
      if (ragOn_ && ragChunks_ == 0) {
        ragOn_ = false;
        chat_.add(Role::System, "INDEX IS EMPTY. PUT FILES IN " + opt_.docsDir + " AND TYPE /ingest.");
      } else {
        chat_.add(Role::System, ragOn_ ? "RAG ON: ANSWERS USE YOUR DOCUMENTS AND SHOW SOURCES."
                                       : "RAG OFF: PLAIN CHAT WITH THE MODEL.");
      }
    } else {
      chat_.add(Role::System, std::string("RAG IS ") + (ragOn_ ? "ON" : "OFF") + ". USE /rag on OR /rag off.");
    }
  } else if (cmd == "/ingest") {
    if (!rag_.isOpen()) chat_.add(Role::System, "RAG UNAVAILABLE: " + ragError_);
    else startIngest();
  } else if (cmd == "/docs") {
    if (!rag_.isOpen()) {
      chat_.add(Role::System, "RAG UNAVAILABLE: " + ragError_);
    } else {
      std::string t = "DOCUMENTS IN " + opt_.docsDir + "\n";
      for (const auto& d : rag_.categories())
        t += "  " + d.category + ": " + std::to_string(d.files) + " FILES, " + std::to_string(d.chunks) + " CHUNKS\n";
      if (ragFiles_ == 0) t += "  (EMPTY)\n";
      t += "ADD FILES, THEN TYPE /ingest.";
      chat_.add(Role::System, t);
    }
  } else if (cmd == "/search") {
    if (!rag_.isOpen() || ragChunks_ == 0) {
      chat_.add(Role::System, "NOTHING INDEXED YET. ADD FILES TO " + opt_.docsDir + " AND TYPE /ingest.");
    } else if (argRaw.empty()) {
      chat_.add(Role::System, "USAGE: /search your keywords   (SHOWS MATCHES WITHOUT ASKING THE MODEL)");
    } else {
      std::vector<RagHit> hits = rag_.search(argRaw, 5);
      if (hits.empty()) {
        chat_.add(Role::System, "NO MATCHES.");
      } else {
        std::string t = "MATCHES";
        int i = 0;
        for (const auto& h : hits) {
          std::string snip;
          for (char ch : h.text.substr(0, 200)) snip += (ch == '\n') ? ' ' : ch;
          t += "\n[" + std::to_string(++i) + "] " + h.label() + "\n    " + snip + (h.text.size() > 200 ? "..." : "");
        }
        chat_.add(Role::System, t);
      }
    }
  } else if (cmd == "/help") {
    chat_.add(Role::System,
              "/help - this list\n/clear - wipe screen + model memory (Ctrl+L)\n"
              "/rag on|off - answer from your documents (with sources) or plain chat\n"
              "/ingest - re-scan the docs folder (new / changed / deleted files)\n"
              "/docs - what is indexed\n/search words - show matching passages, no model\n"
              "/memory on|off - keep or drop chat history in the context\n"
              "/theme [green|amber|ice] - change colors (Ctrl+T)\n/exit - quit\n"
              "Ctrl+C stops a running answer. Ctrl+P / Ctrl+N recall earlier messages.");
  } else {
    chat_.add(Role::System, "UNKNOWN COMMAND: " + cmd + "  (TRY /help)");
  }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
int App::run() {
  if (!term_.open(opt_.logPath, opt_.mouse)) {
    std::fprintf(stderr, "rkllm_console needs an interactive terminal (stdin/stdout must be a TTY).\n");
    return 1;
  }
  applyTheme(opt_.theme);

  if (!boot()) {
    engine_.shutdown();
    term_.close();
    std::fprintf(stderr, "Model load failed: %s\nRuntime log: %s\n", initError_.c_str(), opt_.logPath.c_str());
    return 1;
  }

  chat_.add(Role::System, "NEURAL LINK ESTABLISHED. " + (opt_.modelLabel.empty() ? std::string("MODEL") : opt_.modelLabel) +
                              " ONLINE.");
  if (ragOn_)
    chat_.add(Role::System, "RAG ON: " + std::to_string(ragFiles_) + " DOCUMENTS / " + std::to_string(ragChunks_) +
                                " CHUNKS INDEXED. ANSWERS SHOW SOURCES. /rag off FOR PLAIN CHAT.");
  else if (opt_.ragEnabled && rag_.isOpen())
    chat_.add(Role::System, "NO DOCUMENTS INDEXED. PUT FILES IN " + opt_.docsDir + " AND TYPE /ingest.");
  else if (opt_.ragEnabled)
    chat_.add(Role::System, "RAG UNAVAILABLE: " + ragError_);
  chat_.add(Role::System, "TYPE A MESSAGE AND PRESS ENTER. /help LISTS ALL COMMANDS.");
  needClear_ = true;

  auto lastTelem = Clock::time_point{};
  auto lastFrame = Clock::time_point{};
  uint64_t lastVer = 0;

  while (!quit_ && !Terminal::g_quit) {
    Key k;
    bool got = term_.pollKey(40, k);
    while (got) {
      handleKey(k);
      got = term_.pollKey(0, k);
    }
    auto now = Clock::now();

    if (Terminal::g_resized) {
      Terminal::g_resized = 0;
      needClear_ = true;
      dirty_ = true;
    }
    if (ingestFut_.valid() && ingestFut_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      IngestStats s = ingestFut_.get();
      refreshRagCounts();
      if (ragChunks_ > 0 && s.error.empty() && (s.added || s.updated)) ragOn_ = true;
      if (ragChunks_ == 0) ragOn_ = false;
      chat_.add(Role::System, describeIngest(s));
      dirty_ = true;
    }
    if (now - lastTelem >= std::chrono::milliseconds(500)) {
      telem_ = sampler_.sample();
      lastTelem = now;
      dirty_ = true;
    }
    uint64_t ver = chat_.version();
    if (ver != lastVer) {
      lastVer = ver;
      dirty_ = true;
    }
    const bool busy = engine_.busy();
    auto since = now - lastFrame;
    if ((dirty_ && since >= std::chrono::milliseconds(30)) || (busy && since >= std::chrono::milliseconds(110))) {
      dirty_ = false;
      lastFrame = now;
      render();
    }
  }

  if (ingestFut_.valid()) ingestFut_.wait();
  engine_.shutdown();
  term_.close();
  return 0;
}
