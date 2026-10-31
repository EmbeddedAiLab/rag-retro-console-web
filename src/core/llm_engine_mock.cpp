// Fake engine so the UI can be previewed on any Linux PC (cmake -DRKLLM_MOCK=ON).
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "llm_engine.h"

struct LlmEngine::Impl {
  EngineConfig cfg;
  bool ready = false;
  std::atomic<bool> busy{false};
  std::atomic<bool> aborted{false};
  std::atomic<bool> keepHistory{true};
  std::thread worker;
};

LlmEngine::LlmEngine() : impl_(new Impl) {}
LlmEngine::~LlmEngine() { shutdown(); }

bool LlmEngine::init(const EngineConfig& c, std::string&) {
  impl_->cfg = c;
  std::this_thread::sleep_for(std::chrono::milliseconds(1800));  // pretend to load weights
  impl_->ready = true;
  return true;
}

bool LlmEngine::generate(const std::string& prompt, EngineListener* l, int) {
  bool expected = false;
  if (!impl_->ready || !impl_->busy.compare_exchange_strong(expected, true)) return false;
  if (impl_->worker.joinable()) impl_->worker.join();
  impl_->aborted = false;
  impl_->worker = std::thread([this, prompt, l] {
    std::string reply =
        "MOCK ENGINE ONLINE. Prompt size: " + std::to_string(prompt.size()) +
        " bytes. This is a long streamed sentence used to test word wrapping, scrolling and the "
        "live token counter of the retro console. 你好，世界！这是一段中文测试文本。\n"
        "Here is a code block:\n```python\nfor i in range(3):\n    print('hello from the NPU', i)\n```\n"
        "- bullet one\n- bullet two with enough words to wrap around the edge of the conversation panel\n"
        "That is all.";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // fake prefill
    size_t i = 0;
    while (i < reply.size() && !impl_->aborted) {
      size_t j = reply.find_first_of(" \n", i);
      size_t end = (j == std::string::npos) ? reply.size() : j + 1;
      l->onToken(reply.substr(i, end - i));
      i = end;
      std::this_thread::sleep_for(std::chrono::milliseconds(45));
    }
    FinishStatus st = impl_->aborted ? FinishStatus::Aborted : FinishStatus::Ok;
    impl_->busy = false;
    l->onFinish(st, "");
  });
  return true;
}

void LlmEngine::abort() { impl_->aborted = true; }
bool LlmEngine::busy() const { return impl_->busy; }
bool LlmEngine::clearMemory() { return !impl_->busy; }
void LlmEngine::setKeepHistory(bool on) { impl_->keepHistory = on; }
bool LlmEngine::keepHistory() const { return impl_->keepHistory; }

void LlmEngine::shutdown() {
  if (!impl_) return;
  impl_->aborted = true;
  if (impl_->worker.joinable()) impl_->worker.join();
}
