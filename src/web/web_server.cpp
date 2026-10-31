// Web front end for the RKLLM assistant: tiny HTTP server (no extra libraries).
//   GET  /                  the page (web/index.html)
//   GET  /api/status        telemetry + state (JSON)
//   GET  /api/docs          indexed files (JSON)
//   GET  /api/doc?path=...  one document, plain text (only inside the docs folder)
//   POST /api/ask?rag=1     body = question, answer streamed as Server-Sent Events
//   POST /api/stop          stop the running answer
//   POST /api/clear         forget the chat memory of the model
//   POST /api/ingest        rescan the docs folder
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "llm_engine.h"
#include "rag.h"
#include "telemetry.h"

namespace fs = std::filesystem;

namespace {

// ------------------------------------------------------------------ config
struct Config {
  EngineConfig engine;
  std::string host = "0.0.0.0";
  int port = 8080;
  std::string docsDir = RAG_DEFAULT_DOCS;
  std::string dbPath = RAG_DEFAULT_DB;
  std::string webDir = WEB_DEFAULT_ROOT;
  std::string modelLabel;
  bool ragEnabled = true;
  int ragK = 3;
  size_t ragBytes = 3200;
};

Config g_cfg;
LlmEngine g_engine;
Rag g_rag;
std::atomic<bool> g_quit{false};
std::atomic<bool> g_inUse{false};  // one generation at a time (single NPU)
std::atomic<int> g_conns{0};

// ------------------------------------------------------------------ telemetry
std::mutex g_telM;
Telemetry g_tel;
bool g_telReady = false;

// ------------------------------------------------------------------ small helpers
std::string jsonEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else {
          o += static_cast<char>(c);
        }
    }
  }
  return o;
}
std::string jstr(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string urlDecode(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      o += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else if (s[i] == '+') {
      o += ' ';
    } else {
      o += s[i];
    }
  }
  return o;
}

// ------------------------------------------------------------------ HTTP
struct Request {
  std::string method, path;
  std::map<std::string, std::string> query, headers;
  std::string body;
};

bool sendAll(int fd, const std::string& d) {
  size_t off = 0;
  while (off < d.size()) {
    ssize_t n = ::send(fd, d.data() + off, d.size() - off, MSG_NOSIGNAL);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

const char* statusText(int c) {
  switch (c) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 503: return "Service Unavailable";
    default: return "Error";
  }
}

void respond(int fd, int code, const std::string& type, const std::string& body) {
  std::string h = "HTTP/1.1 " + std::to_string(code) + " " + statusText(code) + "\r\nContent-Type: " + type +
                  "\r\nContent-Length: " + std::to_string(body.size()) +
                  "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
  sendAll(fd, h + body);
}
void respondJson(int fd, int code, const std::string& json) { respond(fd, code, "application/json; charset=utf-8", json); }
void respondError(int fd, int code, const std::string& msg) { respondJson(fd, code, "{\"error\":" + jstr(msg) + "}"); }

bool readRequest(int fd, Request& rq, int& errCode) {
  errCode = 400;
  std::string data;
  char buf[4096];
  size_t hdrEnd;
  while ((hdrEnd = data.find("\r\n\r\n")) == std::string::npos) {
    if (data.size() > 16384) return false;
    ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return false;
    data.append(buf, static_cast<size_t>(n));
  }
  std::istringstream hs(data.substr(0, hdrEnd));
  std::string line;
  if (!std::getline(hs, line)) return false;
  {
    std::istringstream rl(line);
    std::string target, ver;
    rl >> rq.method >> target >> ver;
    if (rq.method.empty() || target.empty()) return false;
    size_t q = target.find('?');
    rq.path = urlDecode(target.substr(0, q));
    if (q != std::string::npos) {
      std::istringstream qs(target.substr(q + 1));
      std::string kv;
      while (std::getline(qs, kv, '&')) {
        size_t e = kv.find('=');
        std::string k = urlDecode(kv.substr(0, e));
        std::string v = e == std::string::npos ? "" : urlDecode(kv.substr(e + 1));
        rq.query[k] = v;
      }
    }
  }
  while (std::getline(hs, line)) {
    size_t c = line.find(':');
    if (c == std::string::npos) continue;
    std::string k = line.substr(0, c);
    for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    rq.headers[k] = trim(line.substr(c + 1));
  }
  size_t clen = 0;
  if (rq.headers.count("content-length")) clen = static_cast<size_t>(std::strtoul(rq.headers["content-length"].c_str(), nullptr, 10));
  if (clen > 65536) { errCode = 413; return false; }
  rq.body = data.substr(hdrEnd + 4);
  while (rq.body.size() < clen) {
    ssize_t n = ::recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return false;
    rq.body.append(buf, static_cast<size_t>(n));
  }
  rq.body.resize(clen);
  return true;
}

// ------------------------------------------------------------------ generation session
struct Event {
  int type = 0;  // 0 = token, 1 = finish
  std::string text;
  std::string status;  // finish only
};

class Session : public EngineListener {
 public:
  void reset() {
    std::lock_guard<std::mutex> lk(m_);
    q_.clear();
  }
  void onToken(const std::string& t) override {
    std::lock_guard<std::mutex> lk(m_);
    q_.push_back({0, t, ""});
    cv_.notify_one();
  }
  void onFinish(FinishStatus s, const std::string& err) override {
    std::lock_guard<std::mutex> lk(m_);
    q_.push_back({1, err, s == FinishStatus::Ok ? "ok" : (s == FinishStatus::Aborted ? "aborted" : "error")});
    cv_.notify_one();
  }
  bool pop(Event& e, int timeoutMs) {
    std::unique_lock<std::mutex> lk(m_);
    if (!cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return !q_.empty(); })) return false;
    e = q_.front();
    q_.pop_front();
    return true;
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<Event> q_;
};
Session& session() {
  static Session* s = new Session();  // never destroyed: the engine thread may still be inside onFinish
  return *s;
}

// ------------------------------------------------------------------ handlers
std::string readFileText(const std::string& path, size_t maxBytes, bool& ok) {
  std::ifstream f(path, std::ios::binary);
  ok = static_cast<bool>(f);
  if (!ok) return "";
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (s.size() > maxBytes) s.resize(maxBytes);
  return s;
}

void handleStatus(int fd) {
  Telemetry t;
  bool ready;
  {
    std::lock_guard<std::mutex> lk(g_telM);
    t = g_tel;
    ready = g_telReady;
  }
  char b[160];
  std::string j = "{";
  j += "\"model\":" + jstr(g_cfg.modelLabel);
  j += std::string(",\"busy\":") + (g_inUse ? "true" : "false");
  std::snprintf(b, sizeof b, ",\"npu\":{\"ok\":%s,\"pct\":%.0f}", (ready && t.npuOk) ? "true" : "false", t.npuPct);
  j += b;
  std::snprintf(b, sizeof b, ",\"cpu\":{\"ok\":%s,\"pct\":%.0f}", (ready && t.cpuOk) ? "true" : "false", t.cpuPct);
  j += b;
  std::snprintf(b, sizeof b, ",\"ram\":{\"ok\":%s,\"used_mb\":%.0f,\"total_mb\":%.0f}", (ready && t.ramOk) ? "true" : "false",
                t.ramUsedMb, t.ramTotalMb);
  j += b;
  std::snprintf(b, sizeof b, ",\"temp\":{\"ok\":%s,\"c\":%.1f}", (ready && t.tempOk) ? "true" : "false", t.tempC);
  j += b;
  std::snprintf(b, sizeof b, ",\"rag\":{\"available\":%s,\"files\":%d,\"chunks\":%d}", g_rag.isOpen() ? "true" : "false",
                g_rag.fileCount(), g_rag.chunkCount());
  j += b;
  j += "}";
  respondJson(fd, 200, j);
}

void handleDocs(int fd) {
  std::string j = "{\"files\":[";
  bool first = true;
  for (auto& f : g_rag.files()) {
    if (!first) j += ",";
    first = false;
    j += "{\"path\":" + jstr(f.first) + ",\"chunks\":" + std::to_string(f.second) + "}";
  }
  j += "]}";
  respondJson(fd, 200, j);
}

void handleDoc(int fd, const Request& rq) {
  auto it = rq.query.find("path");
  if (it == rq.query.end() || it->second.empty() || it->second.find('\0') != std::string::npos) {
    respondError(fd, 400, "missing path");
    return;
  }
  std::error_code ec;
  fs::path root = fs::weakly_canonical(g_cfg.docsDir, ec);
  fs::path p = fs::weakly_canonical(root / it->second, ec);
  const std::string rs = root.string(), ps = p.string();
  if (ec || ps.size() <= rs.size() + 1 || ps.compare(0, rs.size(), rs) != 0 || ps[rs.size()] != '/') {
    respondError(fd, 403, "outside the documents folder");
    return;
  }
  std::string name = p.filename().string();
  if (name.empty() || name[0] == '.' || name[0] == '_' || !fs::is_regular_file(p, ec)) {
    respondError(fd, 404, "file not found");
    return;
  }
  bool ok;
  std::string text = readFileText(ps, 2 * 1024 * 1024, ok);
  if (!ok) { respondError(fd, 404, "cannot read file"); return; }
  respond(fd, 200, "text/plain; charset=utf-8", text);
}

bool sse(int fd, const std::string& event, const std::string& data) {
  return sendAll(fd, "event: " + event + "\ndata: " + data + "\n\n");
}

void handleAsk(int fd, const Request& rq) {
  std::string q = trim(rq.body);
  if (q.empty()) { respondError(fd, 400, "empty question"); return; }
  if (q.size() > 4000) { respondError(fd, 413, "question too long"); return; }

  auto it = rq.query.find("rag");
  const bool wantRag = it != rq.query.end() && it->second == "1" && g_rag.isOpen() && g_rag.chunkCount() > 0;

  if (g_inUse.exchange(true)) {
    respondError(fd, 409, "The NPU is busy with another answer. Try again in a moment.");
    return;
  }
  struct Release { ~Release() { g_inUse = false; } } release;

  std::vector<RagHit> hits;
  std::string prompt = q;
  int keep = -1, used = 0;
  if (wantRag) {
    hits = g_rag.search(q, g_cfg.ragK);
    if (!hits.empty()) {
      prompt = Rag::buildPrompt(q, hits, g_cfg.ragBytes, used);
      keep = 0;
    }
  }

  const std::string hdr =
      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n"
      "X-Accel-Buffering: no\r\n\r\n";
  if (!sendAll(fd, hdr)) return;

  if (wantRag && hits.empty()) {
    sse(fd, "nodocs", "true");
    sse(fd, "done", "{\"status\":\"nodocs\",\"error\":\"\"}");
    return;
  }
  if (wantRag) {
    std::string j = "[";
    for (int i = 0; i < used; ++i) {
      const RagHit& h = hits[static_cast<size_t>(i)];
      if (i) j += ",";
      j += "{\"n\":" + std::to_string(i + 1) + ",\"path\":" + jstr(h.path) + ",\"heading\":" + jstr(h.heading) +
           ",\"text\":" + jstr(h.text) + "}";
    }
    j += "]";
    if (!sse(fd, "sources", j)) return;
  }

  Session& s = session();
  s.reset();
  if (!g_engine.generate(prompt, &s, keep)) {
    sse(fd, "done", "{\"status\":\"error\",\"error\":\"could not start inference\"}");
    return;
  }

  bool alive = true;
  for (;;) {
    Event e;
    if (!s.pop(e, 10000)) {  // keep-alive while the model is still prefilling
      if (alive && !sendAll(fd, ": ping\n\n")) { alive = false; g_engine.abort(); }
      continue;
    }
    if (e.type == 0) {
      if (alive && !sse(fd, "token", jstr(e.text))) { alive = false; g_engine.abort(); }
    } else {
      if (alive) sse(fd, "done", "{\"status\":" + jstr(e.status) + ",\"error\":" + jstr(e.text) + "}");
      break;
    }
  }
}

void handleConnection(int fd) {
  timeval tv{15, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

  Request rq;
  int err = 400;
  if (!readRequest(fd, rq, err)) {
    respondError(fd, err, "bad request");
    ::close(fd);
    return;
  }

  if (rq.method == "GET" && (rq.path == "/" || rq.path == "/index.html")) {
    bool ok;
    std::string html = readFileText(g_cfg.webDir + "/index.html", 2 * 1024 * 1024, ok);
    if (ok) respond(fd, 200, "text/html; charset=utf-8", html);
    else respondError(fd, 404, "web/index.html not found (use --web DIR)");
  } else if (rq.method == "GET" && rq.path == "/api/status") {
    handleStatus(fd);
  } else if (rq.method == "GET" && rq.path == "/api/docs") {
    handleDocs(fd);
  } else if (rq.method == "GET" && rq.path == "/api/doc") {
    handleDoc(fd, rq);
  } else if (rq.method == "POST" && rq.path == "/api/ask") {
    handleAsk(fd, rq);
  } else if (rq.method == "POST" && rq.path == "/api/stop") {
    g_engine.abort();
    respondJson(fd, 200, "{\"ok\":true}");
  } else if (rq.method == "POST" && rq.path == "/api/clear") {
    if (g_inUse) respondError(fd, 409, "busy");
    else respondJson(fd, 200, std::string("{\"ok\":") + (g_engine.clearMemory() ? "true" : "false") + "}");
  } else if (rq.method == "POST" && rq.path == "/api/ingest") {
    if (!g_rag.isOpen()) { respondError(fd, 503, "document search is not available"); }
    else {
      IngestStats s = g_rag.ingest(g_cfg.docsDir);
      if (!s.error.empty()) { respondError(fd, 400, s.error); }
      else {
        char b[200];
        std::snprintf(b, sizeof b,
                      "{\"added\":%d,\"updated\":%d,\"removed\":%d,\"unchanged\":%d,\"files\":%d,\"chunks\":%d}", s.added,
                      s.updated, s.removed, s.unchanged, s.files, s.chunks);
        respondJson(fd, 200, b);
      }
    }
  } else {
    respondError(fd, 404, "not found");
  }
  ::shutdown(fd, SHUT_RDWR);
  ::close(fd);
}

void onSignal(int) { g_quit = true; }

void printAddresses(int port) {
  ifaddrs* ifa = nullptr;
  if (getifaddrs(&ifa) != 0) return;
  for (ifaddrs* p = ifa; p; p = p->ifa_next) {
    if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || (p->ifa_flags & IFF_LOOPBACK)) continue;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr, ip, sizeof ip);
    std::printf("    http://%s:%d   (%s)\n", ip, port, p->ifa_name);
  }
  freeifaddrs(ifa);
}

void usage(const char* a0) {
  std::printf(
      "RK3576 web assistant\n\n"
      "Usage: %s MODEL.rkllm [max_new_tokens=512] [max_context_len=2048] [options]\n\n"
      "  --port N            default 8080\n"
      "  --host ADDR         default 0.0.0.0 (all interfaces); use 127.0.0.1 for this board only\n"
      "  --docs DIR          documents folder (default <project>/docs)\n"
      "  --db FILE           index file (default <project>/data/index.db)\n"
      "  --web DIR           folder with index.html (default <project>/web)\n"
      "  --rag-k N           passages per question (3)\n"
      "  --rag-bytes N       max document bytes per question (3200)\n"
      "  --no-rag            disable document search\n"
      "  --top-k N  --top-p X  --temp X  --repeat-penalty X\n"
      "  --model-label TEXT  name shown in the page\n",
      a0);
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> pos;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&](const char* n) -> std::string {
      if (i + 1 >= argc) { std::fprintf(stderr, "Missing value for %s\n", n); std::exit(2); }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
    else if (a == "--port") g_cfg.port = std::atoi(val("--port").c_str());
    else if (a == "--host") g_cfg.host = val("--host");
    else if (a == "--docs") g_cfg.docsDir = val("--docs");
    else if (a == "--db") g_cfg.dbPath = val("--db");
    else if (a == "--web") g_cfg.webDir = val("--web");
    else if (a == "--rag-k") g_cfg.ragK = std::max(1, std::atoi(val("--rag-k").c_str()));
    else if (a == "--rag-bytes") g_cfg.ragBytes = static_cast<size_t>(std::max(500, std::atoi(val("--rag-bytes").c_str())));
    else if (a == "--no-rag") g_cfg.ragEnabled = false;
    else if (a == "--top-k") g_cfg.engine.topK = std::atoi(val("--top-k").c_str());
    else if (a == "--top-p") g_cfg.engine.topP = static_cast<float>(std::atof(val("--top-p").c_str()));
    else if (a == "--temp") g_cfg.engine.temperature = static_cast<float>(std::atof(val("--temp").c_str()));
    else if (a == "--repeat-penalty") g_cfg.engine.repeatPenalty = static_cast<float>(std::atof(val("--repeat-penalty").c_str()));
    else if (a == "--model-label") g_cfg.modelLabel = val("--model-label");
    else if (a.rfind("--", 0) == 0) { std::fprintf(stderr, "Unknown option %s\n", a.c_str()); return 2; }
    else pos.push_back(a);
  }
  if (pos.empty()) { usage(argv[0]); return 2; }
  g_cfg.engine.modelPath = pos[0];
  if (pos.size() > 1) g_cfg.engine.maxNewTokens = std::max(1, std::atoi(pos[1].c_str()));
  if (pos.size() > 2) g_cfg.engine.maxContext = std::max(64, std::atoi(pos[2].c_str()));
  if (g_cfg.modelLabel.empty()) {
    std::string stem = fs::path(pos[0]).stem().string();
    size_t cut = stem.find("_RK");
    if (cut == std::string::npos) cut = stem.find("_rk");
    g_cfg.modelLabel = cut == std::string::npos ? stem : stem.substr(0, cut);
  }
  g_cfg.engine.keepHistory = true;

  std::signal(SIGPIPE, SIG_IGN);
  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sa_handler = onSignal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  // ---- documents
  if (g_cfg.ragEnabled) {
    std::string err;
    if (g_rag.open(g_cfg.dbPath, err)) {
      IngestStats s = g_rag.ingest(g_cfg.docsDir);
      if (!s.error.empty()) std::printf("[docs] %s\n", s.error.c_str());
      else std::printf("[docs] %d files / %d chunks indexed from %s\n", s.files, s.chunks, g_cfg.docsDir.c_str());
    } else {
      std::printf("[docs] document search unavailable: %s\n", err.c_str());
    }
  }

  // ---- model
  std::printf("[model] loading %s ...\n", g_cfg.engine.modelPath.c_str());
  std::fflush(stdout);
  std::string err;
  if (!g_engine.init(g_cfg.engine, err)) {
    std::fprintf(stderr, "[model] failed: %s\n", err.c_str());
    return 1;
  }
  std::printf("[model] ready\n");

  // ---- telemetry sampler
  std::thread tel([] {
    TelemetrySampler sampler;
    while (!g_quit) {
      Telemetry t = sampler.sample();
      {
        std::lock_guard<std::mutex> lk(g_telM);
        g_tel = t;
        g_telReady = true;
      }
      for (int i = 0; i < 10 && !g_quit; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });

  // ---- listen
  int ls = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(g_cfg.port));
  if (inet_pton(AF_INET, g_cfg.host.c_str(), &addr.sin_addr) != 1) {
    std::fprintf(stderr, "bad --host %s\n", g_cfg.host.c_str());
    return 2;
  }
  if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(ls, 16) != 0) {
    std::perror("bind/listen");
    g_quit = true;
    tel.join();
    g_engine.shutdown();
    return 1;
  }
  std::printf("\n  RK3576 web assistant is running. Open from any device on the network:\n");
  if (g_cfg.host == "0.0.0.0") printAddresses(g_cfg.port);
  else std::printf("    http://%s:%d\n", g_cfg.host.c_str(), g_cfg.port);
  std::printf("  Ctrl+C to stop. No password: use it only on a network you trust.\n\n");
  std::fflush(stdout);

  while (!g_quit) {
    pollfd p{ls, POLLIN, 0};
    if (::poll(&p, 1, 500) <= 0) continue;
    int c = ::accept(ls, nullptr, nullptr);
    if (c < 0) continue;
    if (g_conns >= 48) { ::close(c); continue; }
    ++g_conns;
    std::thread([c] {
      handleConnection(c);
      --g_conns;
    }).detach();
  }

  std::printf("\nstopping ...\n");
  g_engine.abort();
  ::close(ls);
  for (int i = 0; i < 50 && g_conns > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  tel.join();
  g_engine.shutdown();
  return 0;
}
