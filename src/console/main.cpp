#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "app.h"

static void usage(const char* argv0) {
  std::printf(
      "RK3576 retro neural console\n\n"
      "Usage: %s MODEL.rkllm [max_new_tokens] [max_context_len] [options]\n\n"
      "  max_new_tokens   default 512   (same meaning as llm_demo)\n"
      "  max_context_len  default 2048  (same meaning as llm_demo)\n\n"
      "Options:\n"
      "  --theme NAME         green (default) | amber | ice\n"
      "  --board TEXT         board name shown in the banner (default \"RADXA ROCK 4D\")\n"
      "  --soc TEXT           SoC name shown in the banner (default RK3576)\n"
      "  --bot-name TEXT      label for the model's messages (default: from file name)\n"
      "  --model-label TEXT   model name in the banner (default: from file name)\n"
      "  --top-k N            default 20\n"
      "  --top-p X            default 0.9\n"
      "  --temp X             default 0.7\n"
      "  --repeat-penalty X   default 1.1\n"
      "  --no-history         start with chat memory off (toggle later with /memory)\n"
      "  --mouse              enable mouse-wheel scrolling (Shift+drag to select text)\n"
      "  --no-bg              keep your terminal background instead of painting black\n"
      "  --fast-boot          skip the boot animation delays\n"
      "\nRAG (answers from your documents):\n"
      "  --docs DIR           documents folder (default: <project>/docs)\n"
      "  --db FILE            index file       (default: <project>/data/index.db)\n"
      "  --rag-k N            passages sent to the model, default 3\n"
      "  --rag-bytes N        max bytes of document text per question, default 3200\n"
      "  --no-rag             disable document search completely\n\n"
      "  --log FILE           where rkllm runtime messages go (default /tmp/rkllm_console.log)\n"
      "  -h, --help           this text\n",
      argv0);
}

static std::string upper(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

int main(int argc, char** argv) {
  Options o;
  std::vector<std::string> pos;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Missing value for %s\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
    else if (a == "--theme") o.theme = value("--theme");
    else if (a == "--board") o.board = upper(value("--board"));
    else if (a == "--soc") o.soc = upper(value("--soc"));
    else if (a == "--bot-name") o.botName = upper(value("--bot-name"));
    else if (a == "--model-label") o.modelLabel = upper(value("--model-label"));
    else if (a == "--top-k") o.engine.topK = std::atoi(value("--top-k").c_str());
    else if (a == "--top-p") o.engine.topP = static_cast<float>(std::atof(value("--top-p").c_str()));
    else if (a == "--temp") o.engine.temperature = static_cast<float>(std::atof(value("--temp").c_str()));
    else if (a == "--repeat-penalty") o.engine.repeatPenalty = static_cast<float>(std::atof(value("--repeat-penalty").c_str()));
    else if (a == "--log") o.logPath = value("--log");
    else if (a == "--no-history") o.engine.keepHistory = false;
    else if (a == "--mouse") o.mouse = true;
    else if (a == "--no-bg") o.noBg = true;
    else if (a == "--fast-boot") o.fastBoot = true;
    else if (a == "--docs") o.docsDir = value("--docs");
    else if (a == "--db") o.dbPath = value("--db");
    else if (a == "--rag-k") o.ragK = std::max(1, std::atoi(value("--rag-k").c_str()));
    else if (a == "--rag-bytes") o.ragBytes = static_cast<size_t>(std::max(500, std::atoi(value("--rag-bytes").c_str())));
    else if (a == "--no-rag") o.ragEnabled = false;
    else if (a.rfind("--", 0) == 0) {
      std::fprintf(stderr, "Unknown option %s (try --help)\n", a.c_str());
      return 2;
    } else pos.push_back(a);
  }

  if (pos.empty()) {
    usage(argv[0]);
    return 2;
  }
  o.engine.modelPath = pos[0];
  if (pos.size() > 1) o.engine.maxNewTokens = std::max(1, std::atoi(pos[1].c_str()));
  if (pos.size() > 2) o.engine.maxContext = std::max(64, std::atoi(pos[2].c_str()));

  // "Qwen2-0.5B-Instruct_RK3576_w4a16_g128.rkllm" -> "QWEN2-0.5B-INSTRUCT", bot "QWEN"
  std::string stem = std::filesystem::path(o.engine.modelPath).stem().string();
  size_t cut = stem.find("_RK");
  if (cut == std::string::npos) cut = stem.find("_rk");
  if (cut != std::string::npos) stem = stem.substr(0, cut);
  if (o.modelLabel.empty()) o.modelLabel = upper(stem);
  if (o.botName.empty()) {
    std::string lead;
    for (char c : stem) {
      if (!std::isalpha(static_cast<unsigned char>(c))) break;
      lead += c;
    }
    o.botName = lead.empty() ? "AI" : upper(lead);
  }

  App app(o);
  return app.run();
}
