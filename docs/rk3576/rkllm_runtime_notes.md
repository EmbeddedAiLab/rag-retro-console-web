# RKLLM runtime notes

## Loading a model
Call rkllm_createDefaultParam, set model_path, max_context_len and max_new_tokens, then call rkllm_init with a result callback.
rkllm_init returns 0 on success.

## Running a prompt
Fill an RKLLMInput with input_type RKLLM_INPUT_PROMPT and call rkllm_run. The result callback receives the generated text piece by piece.
The call states are RKLLM_RUN_NORMAL, RKLLM_RUN_WAITING (incomplete UTF-8 character), RKLLM_RUN_FINISH and RKLLM_RUN_ERROR.

## Chat history
Set keep_history to 1 in RKLLMInferParam to keep earlier turns in the KV cache, or 0 to forget them.
rkllm_clear_kv_cache clears the cache. rkllm_abort stops a running generation. rkllm_destroy releases the model.

## NPU load
Run as root and read /sys/kernel/debug/rknpu/load to see the load of each NPU core.
For stable speed, the SDK ships a script scripts/fix_freq_rk3576.sh that fixes the clock frequencies.
