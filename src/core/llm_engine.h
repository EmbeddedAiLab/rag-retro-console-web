// Thin wrapper around the RKLLM runtime (or a fake engine for UI previews).
#pragma once
#include <memory>
#include <string>

struct EngineConfig {
  std::string modelPath;
  int maxNewTokens = 512;
  int maxContext = 2048;
  int topK = 20;
  float topP = 0.9f;
  float temperature = 0.7f;
  float repeatPenalty = 1.1f;
  bool keepHistory = true;
};

enum class FinishStatus { Ok, Aborted, Error };

// Called from the engine's worker thread.
class EngineListener {
 public:
  virtual ~EngineListener() = default;
  virtual void onToken(const std::string& text) = 0;
  virtual void onFinish(FinishStatus status, const std::string& error) = 0;
};

class LlmEngine {
 public:
  LlmEngine();
  ~LlmEngine();
  LlmEngine(const LlmEngine&) = delete;
  LlmEngine& operator=(const LlmEngine&) = delete;

  // Blocking; loading a model takes a few seconds.
  bool init(const EngineConfig& cfg, std::string& error);

  // Starts generation on a worker thread. Returns false if one is running.
  // keepHistory: -1 = use the engine setting, 0 = forget this turn afterwards, 1 = remember.
  bool generate(const std::string& prompt, EngineListener* listener, int keepHistory = -1);
  void abort();
  bool busy() const;

  // Forgets the conversation (KV cache).
  bool clearMemory();
  void setKeepHistory(bool on);
  bool keepHistory() const;

  // Aborts, joins and releases the model. Safe to call twice.
  void shutdown();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
