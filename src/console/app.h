// The retro console: boot screen, panels, input handling and the main loop.
#pragma once
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#include "chat_log.h"
#include "llm_engine.h"
#include "rag.h"
#include "telemetry.h"
#include "terminal.h"

struct Options {
  EngineConfig engine;
  std::string board = "RADXA ROCK 4D";
  std::string soc = "RK3576";
  std::string botName;     // derived from the model file name when empty
  std::string modelLabel;  // derived from the model file name when empty
  std::string theme = "green";
  std::string logPath = "/tmp/rkllm_console.log";
  bool mouse = false;
  bool noBg = false;
  bool fastBoot = false;

  // RAG (document search)
  std::string docsDir = RAG_DEFAULT_DOCS;
  std::string dbPath = RAG_DEFAULT_DB;
  bool ragEnabled = true;
  int ragK = 3;              // chunks sent to the model
  size_t ragBytes = 3200;    // max bytes of document text in one prompt
};

class App : public EngineListener {
 public:
  explicit App(Options o);
  int run();

  // EngineListener (worker thread)
  void onToken(const std::string& text) override;
  void onFinish(FinishStatus status, const std::string& error) override;

 private:
  struct Styles {
    std::string base, frame, title, sub, accent, oper, bot, sys, dim, code, warn, crit, bold;
  };
  using Clock = std::chrono::steady_clock;

  void applyTheme(const std::string& name);
  void cycleTheme();
  bool boot();
  void render();
  void handleKey(const Key& k);
  void submit();
  void command(const std::string& line);
  void clearAll();
  void refreshRagCounts();
  void startIngest();
  std::string describeIngest(const IngestStats& s) const;
  void scrollBy(int lines);

  Options opt_;
  Terminal term_;
  ChatLog chat_;
  TelemetrySampler sampler_;
  Telemetry telem_;
  LlmEngine engine_;
  std::string initError_;

  // RAG
  Rag rag_;
  bool ragOn_ = false;
  int ragFiles_ = 0;
  int ragChunks_ = 0;
  std::string ragError_;
  std::future<IngestStats> ingestFut_;
  std::string pendingSources_;  // guarded by statM_

  Styles st_;
  int themeIdx_ = 0;

  // input line
  std::u32string input_;
  size_t cursor_ = 0;
  size_t inStart_ = 0;
  std::vector<std::u32string> history_;
  int histIdx_ = -1;

  // chat viewport
  int scrollOff_ = 0;  // lines scrolled up from the bottom (0 = follow)
  int lastTotal_ = 0;
  int lastCols_ = 0;
  int viewH_ = 5;
  int totalLines_ = 0;

  bool dirty_ = true;
  bool needClear_ = true;
  bool quit_ = false;

  // generation statistics (written by the worker thread)
  std::mutex statM_;
  int tokens_ = 0;
  Clock::time_point tStart_, tFirst_, tLast_;
  double latencyMs_ = -1;
  bool generating_ = false;
};
