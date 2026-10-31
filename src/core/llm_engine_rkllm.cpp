// Real engine: Rockchip RKLLM runtime (NPU).
#include <atomic>
#include <cstring>
#include <thread>

#include "llm_engine.h"
#include "rkllm.h"

struct LlmEngine::Impl {
  LLMHandle handle = nullptr;
  EngineConfig cfg;
  std::atomic<bool> busy{false};
  std::atomic<bool> aborted{false};
  std::atomic<bool> errored{false};
  std::atomic<bool> keepHistory{true};
  std::thread worker;
  EngineListener* listener = nullptr;
  std::string prompt;
  int keepOverride = -1;

  // Runs on the runtime's thread, inside rkllm_run().
  static int onResult(RKLLMResult* r, void* ud, LLMCallState state) {
    auto* self = static_cast<Impl*>(ud);
    if (!self) return 0;
    if (state == RKLLM_RUN_NORMAL) {
      if (r && r->text && self->listener) self->listener->onToken(r->text);
    } else if (state == RKLLM_RUN_ERROR) {
      self->errored = true;
    }
    // RKLLM_RUN_WAITING (partial UTF-8) and RKLLM_RUN_FINISH need no action here.
    return 0;
  }

  void job() {
    RKLLMInput in;
    std::memset(&in, 0, sizeof in);
    in.role = "user";
    in.input_type = RKLLM_INPUT_PROMPT;
    in.prompt_input = prompt.c_str();

    RKLLMInferParam ip;
    std::memset(&ip, 0, sizeof ip);
    ip.mode = RKLLM_INFER_GENERATE;
    ip.keep_history = keepOverride >= 0 ? keepOverride : (keepHistory ? 1 : 0);

    int ret = rkllm_run(handle, &in, &ip, this);

    FinishStatus st = FinishStatus::Ok;
    std::string err;
    if (aborted) {
      st = FinishStatus::Aborted;
    } else if (errored || ret != 0) {
      st = FinishStatus::Error;
      err = "rkllm_run returned " + std::to_string(ret);
    }
    EngineListener* l = listener;
    busy = false;
    if (l) l->onFinish(st, err);
  }
};

LlmEngine::LlmEngine() : impl_(new Impl) {}
LlmEngine::~LlmEngine() { shutdown(); }

bool LlmEngine::init(const EngineConfig& c, std::string& error) {
  impl_->cfg = c;
  impl_->keepHistory = c.keepHistory;

  RKLLMParam p = rkllm_createDefaultParam();
  p.model_path = impl_->cfg.modelPath.c_str();
  p.max_context_len = c.maxContext;
  p.max_new_tokens = c.maxNewTokens;
  p.top_k = c.topK;
  p.top_p = c.topP;
  p.temperature = c.temperature;
  p.repeat_penalty = c.repeatPenalty;
  p.frequency_penalty = 0.0f;
  p.presence_penalty = 0.0f;
  p.skip_special_token = true;
  p.extend_param.base_domain_id = 0;
  p.extend_param.embed_flash = 1;

  RKLLMCallback cbs = {};
  cbs.result_callback = &Impl::onResult;

  int ret = rkllm_init(&impl_->handle, &p, &cbs);
  if (ret != 0) {
    impl_->handle = nullptr;
    error = "rkllm_init failed (code " + std::to_string(ret) + ") - check model path / platform / sudo";
    return false;
  }
  return true;
}

bool LlmEngine::generate(const std::string& prompt, EngineListener* listener, int keepHistory) {
  if (!impl_->handle) return false;
  bool expected = false;
  if (!impl_->busy.compare_exchange_strong(expected, true)) return false;
  if (impl_->worker.joinable()) impl_->worker.join();
  impl_->listener = listener;
  impl_->prompt = prompt;
  impl_->keepOverride = keepHistory;
  impl_->aborted = false;
  impl_->errored = false;
  impl_->worker = std::thread([this] { impl_->job(); });
  return true;
}

void LlmEngine::abort() {
  if (impl_->handle && impl_->busy) {
    impl_->aborted = true;
    rkllm_abort(impl_->handle);
  }
}

bool LlmEngine::busy() const { return impl_->busy; }

bool LlmEngine::clearMemory() {
  if (!impl_->handle || impl_->busy) return false;
  return rkllm_clear_kv_cache(impl_->handle, 1, nullptr, nullptr) == 0;
}

void LlmEngine::setKeepHistory(bool on) { impl_->keepHistory = on; }
bool LlmEngine::keepHistory() const { return impl_->keepHistory; }

void LlmEngine::shutdown() {
  if (!impl_) return;
  if (impl_->handle && impl_->busy) {
    impl_->aborted = true;
    rkllm_abort(impl_->handle);
  }
  if (impl_->worker.joinable()) impl_->worker.join();
  if (impl_->handle) {
    rkllm_destroy(impl_->handle);
    impl_->handle = nullptr;
  }
}
