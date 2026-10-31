#include "rag.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>

#include "utf8.h"

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------- helpers
struct Stmt {
  sqlite3_stmt* s = nullptr;
  Stmt(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) s = nullptr;
  }
  ~Stmt() {
    if (s) sqlite3_finalize(s);
  }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;
  explicit operator bool() const { return s != nullptr; }
  void text(int i, const std::string& v) { sqlite3_bind_text(s, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT); }
  void i64(int i, long long v) { sqlite3_bind_int64(s, i, v); }
  std::string col(int i) const {
    const unsigned char* t = sqlite3_column_text(s, i);
    return t ? std::string(reinterpret_cast<const char*>(t), static_cast<size_t>(sqlite3_column_bytes(s, i))) : "";
  }
};

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string lowerAscii(std::string s) {
  for (auto& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

// Valid UTF-8, no CR / control characters, tabs become spaces.
std::string sanitize(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  size_t i = 0;
  while (i < in.size()) {
    char32_t cp = u8::next(in, i);
    if (cp == '\r') continue;
    if (cp == '\t') cp = ' ';
    if (cp != '\n' && u8::isControl(cp)) continue;
    if (cp == 0xFFFD) continue;
    u8::append(out, cp);
  }
  return out;
}

// ---------------------------------------------------------------- chunking
struct Chunk {
  std::string heading;
  std::string text;
};

void splitLong(std::string p, size_t maxB, std::vector<std::string>& out) {
  while (p.size() > maxB) {
    size_t cut = std::string::npos;
    const size_t lo = maxB / 2;
    for (size_t i = maxB; i > lo; --i) {  // sentence / line end
      char c = p[i - 1];
      if (c == '\n' || ((c == ' ') && i >= 2 && (p[i - 2] == '.' || p[i - 2] == '!' || p[i - 2] == '?'))) {
        cut = i;
        break;
      }
    }
    if (cut == std::string::npos) {
      for (size_t i = maxB; i > lo; --i) {
        if (p[i - 1] == ' ') { cut = i; break; }
      }
    }
    if (cut == std::string::npos) {
      cut = maxB;
      while (cut > 0 && (static_cast<unsigned char>(p[cut]) & 0xC0) == 0x80) --cut;
      if (cut == 0) cut = maxB;
    }
    std::string piece = trim(p.substr(0, cut));
    if (!piece.empty()) out.push_back(piece);
    p = trim(p.substr(cut));
  }
  if (!p.empty()) out.push_back(p);
}

std::vector<Chunk> chunkText(const std::string& raw, size_t maxB) {
  const std::string text = sanitize(raw);
  std::vector<std::string> lines;
  {
    size_t pos = 0;
    while (pos <= text.size()) {
      size_t nl = text.find('\n', pos);
      if (nl == std::string::npos) nl = text.size();
      lines.push_back(text.substr(pos, nl - pos));
      pos = nl + 1;
    }
  }

  std::vector<Chunk> out;
  std::vector<std::string> stack;
  std::map<char, int> rstLevel;
  std::string cur, lastPiece, para;
  bool fence = false;

  auto headingPath = [&]() {
    std::string r;
    for (const auto& h : stack) {
      if (h.empty()) continue;
      if (!r.empty()) r += " > ";
      r += h;
    }
    return r;
  };
  auto flushChunk = [&]() {
    std::string t = trim(cur);
    if (!t.empty()) out.push_back({headingPath(), t});
    cur.clear();
  };
  auto addPiece = [&](const std::string& piece) {
    if (!cur.empty() && cur.size() + piece.size() + 2 > maxB) {
      flushChunk();
      if (!lastPiece.empty() && lastPiece.size() <= 250) cur = lastPiece;  // small overlap
    }
    if (!cur.empty()) cur += "\n\n";
    cur += piece;
    lastPiece = piece;
  };
  auto flushPara = [&]() {
    std::string p = trim(para);
    para.clear();
    if (p.empty()) return;
    if (p.size() > maxB) {
      std::vector<std::string> parts;
      splitLong(p, maxB, parts);
      for (const auto& x : parts) addPiece(x);
    } else {
      addPiece(p);
    }
  };
  auto setHeading = [&](size_t level, const std::string& title) {
    if (level < 1) level = 1;
    if (stack.size() > level - 1) stack.resize(level - 1);
    while (stack.size() < level - 1) stack.push_back("");
    stack.push_back(title);
  };

  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& ln = lines[i];
    const std::string t = trim(ln);
    if (startsWith(t, "```") || startsWith(t, "~~~")) {
      fence = !fence;
      para += ln + "\n";
      continue;
    }
    if (!fence) {
      size_t h = 0;
      while (h < ln.size() && ln[h] == '#') ++h;
      if (h >= 1 && h <= 6 && h < ln.size() && ln[h] == ' ') {  // markdown heading
        flushPara();
        flushChunk();
        lastPiece.clear();
        setHeading(h, trim(ln.substr(h)));
        continue;
      }
      if (!t.empty() && i + 1 < lines.size()) {  // reStructuredText heading
        const std::string u = trim(lines[i + 1]);
        bool uline = u.size() >= 3 && std::string("=-~^\"#*+").find(u[0]) != std::string::npos &&
                     u.find_first_not_of(u[0]) == std::string::npos;
        bool tline = t.find_first_not_of(t[0]) == std::string::npos && t.size() >= 3;
        if (uline && !tline && static_cast<int>(u.size()) >= u8::width(t)) {
          flushPara();
          flushChunk();
          lastPiece.clear();
          if (!rstLevel.count(u[0])) rstLevel[u[0]] = static_cast<int>(rstLevel.size()) + 1;
          setHeading(static_cast<size_t>(rstLevel[u[0]]), t);
          ++i;
          continue;
        }
      }
    }
    if (t.empty() && !fence) {
      flushPara();
      continue;
    }
    para += ln + "\n";
  }
  flushPara();
  flushChunk();
  return out;
}

bool wantedFile(const fs::path& p) {
  static const std::set<std::string> ok = {".md",  ".markdown", ".txt", ".rst", ".c",   ".h",   ".cc",
                                           ".cpp", ".hpp",      ".dts", ".dtsi", ".py", ".sh",  ".cfg",
                                           ".conf", ".ini",     ".yaml", ".yml", ".log"};
  return ok.count(lowerAscii(p.extension().string())) > 0;
}

bool hiddenName(const fs::path& p) {
  std::string n = p.filename().string();
  return !n.empty() && (n[0] == '.' || n[0] == '_');
}

std::string nameWords(const std::string& relPath) {
  std::string w = relPath;
  for (auto& c : w) {
    if (c == '/' || c == '.' || c == '-' || c == '_') c = ' ';
  }
  return w;
}

const std::unordered_set<std::string>& stopwords() {
  static const std::unordered_set<std::string> s = {
      "a",     "an",   "the",  "is",    "are",   "was",   "were", "be",    "been", "am",    "do",   "does",
      "did",   "how",  "what", "why",   "when",  "where", "which", "who",  "whom", "whose", "i",    "me",
      "my",    "we",   "our",  "you",   "your",  "it",    "its",  "of",    "in",   "on",    "at",   "to",
      "for",   "from", "by",   "with",  "about", "as",    "into", "than",  "that", "this",  "these", "those",
      "and",   "or",   "but",  "if",    "then",  "so",    "can",  "could", "should", "would", "will", "shall",
      "may",   "might", "must", "not",  "no",    "yes",   "tell", "explain", "describe", "give", "show", "please",
      "there", "here", "have", "has",   "had",   "get",   "make", "use",   "used", "using", "any",  "some",
      "us",    "he",   "she",  "they",  "them",  "his",   "her",  "their", "also", "just", "very"};
  return s;
}

bool nonAsciiSeparator(char32_t cp) {
  return (cp >= 0x2000 && cp <= 0x2BFF) || (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0xFF01 && cp <= 0xFF0F) ||
         (cp >= 0xFF1A && cp <= 0xFF20) || cp == 0xFFFD || cp == 0xA0;
}

}  // namespace

// =========================================================================
Rag::Rag() = default;

Rag::~Rag() {
  if (db_) sqlite3_close(db_);
}

bool Rag::isOpen() const { return db_ != nullptr; }

bool Rag::open(const std::string& dbPath, std::string& error) {
  std::lock_guard<std::mutex> lk(m_);
  if (db_) return true;
  std::error_code ec;
  fs::path parent = fs::path(dbPath).parent_path();
  if (!parent.empty()) fs::create_directories(parent, ec);
  if (sqlite3_open_v2(dbPath.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    error = std::string("cannot open index ") + dbPath + ": " + (db_ ? sqlite3_errmsg(db_) : "out of memory") +
            " (permissions? see README)";
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
    return false;
  }
  sqlite3_busy_timeout(db_, 3000);
  sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
  const char* schema =
      "CREATE TABLE IF NOT EXISTS files(path TEXT PRIMARY KEY, mtime INTEGER, size INTEGER, chunks INTEGER);"
      "CREATE VIRTUAL TABLE IF NOT EXISTS fts USING fts5(text, heading, name, path UNINDEXED, "
      "tokenize = 'unicode61 tokenchars ''_''');";
  char* err = nullptr;
  if (sqlite3_exec(db_, schema, nullptr, nullptr, &err) != SQLITE_OK) {
    error = std::string("SQLite FTS5 unavailable or index unwritable: ") + (err ? err : "?");
    sqlite3_free(err);
    sqlite3_close(db_);
    db_ = nullptr;
    return false;
  }
  return true;
}

int Rag::scalar(const char* sql) {
  Stmt st(db_, sql);
  if (st && sqlite3_step(st.s) == SQLITE_ROW) return sqlite3_column_int(st.s, 0);
  return 0;
}

int Rag::chunkCount() {
  std::lock_guard<std::mutex> lk(m_);
  return db_ ? scalar("SELECT COALESCE(SUM(chunks),0) FROM files") : 0;
}

int Rag::fileCount() {
  std::lock_guard<std::mutex> lk(m_);
  return db_ ? scalar("SELECT COUNT(*) FROM files") : 0;
}

std::vector<DocStat> Rag::categories() {
  std::lock_guard<std::mutex> lk(m_);
  std::map<std::string, DocStat> m;
  if (db_) {
    Stmt st(db_, "SELECT path, chunks FROM files");
    while (st && sqlite3_step(st.s) == SQLITE_ROW) {
      std::string p = st.col(0);
      size_t sl = p.find('/');
      std::string cat = sl == std::string::npos ? "(top level)" : p.substr(0, sl);
      DocStat& d = m[cat];
      d.category = cat;
      d.files += 1;
      d.chunks += sqlite3_column_int(st.s, 1);
    }
  }
  std::vector<DocStat> v;
  for (auto& kv : m) v.push_back(kv.second);
  return v;
}

std::vector<std::pair<std::string, int>> Rag::files() {
  std::lock_guard<std::mutex> lk(m_);
  std::vector<std::pair<std::string, int>> v;
  if (!db_) return v;
  Stmt st(db_, "SELECT path, chunks FROM files ORDER BY path");
  while (st && sqlite3_step(st.s) == SQLITE_ROW) v.emplace_back(st.col(0), sqlite3_column_int(st.s, 1));
  return v;
}

IngestStats Rag::ingest(const std::string& docsDir) {
  IngestStats s;
  std::lock_guard<std::mutex> lk(m_);
  if (!db_) {
    s.error = "index is not open";
    return s;
  }
  std::error_code ec;
  if (!fs::is_directory(docsDir, ec)) {
    s.error = "docs folder not found: " + docsDir;
    return s;
  }

  struct Known { long long mtime, size; };
  std::map<std::string, Known> known;
  {
    Stmt st(db_, "SELECT path, mtime, size FROM files");
    while (st && sqlite3_step(st.s) == SQLITE_ROW)
      known[st.col(0)] = {sqlite3_column_int64(st.s, 1), sqlite3_column_int64(st.s, 2)};
  }

  std::set<std::string> seen;
  const fs::path root = fs::path(docsDir);
  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
       it != end && !ec; it.increment(ec)) {
    const fs::path& p = it->path();
    if (hiddenName(p)) {
      if (it->is_directory(ec)) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec) || !wantedFile(p)) continue;
    std::error_code e2;
    auto sz = static_cast<long long>(fs::file_size(p, e2));
    if (e2 || sz > 8LL * 1024 * 1024) { ++s.skipped; continue; }
    long long mt = std::chrono::duration_cast<std::chrono::seconds>(fs::last_write_time(p, e2).time_since_epoch()).count();
    const std::string rel = fs::relative(p, root, e2).generic_string();
    seen.insert(rel);

    auto k = known.find(rel);
    if (k != known.end() && k->second.mtime == mt && k->second.size == sz) {
      ++s.unchanged;
      continue;
    }

    std::ifstream f(p, std::ios::binary);
    if (!f) { ++s.skipped; continue; }
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<Chunk> chunks = chunkText(ss.str(), 1000);

    sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
    {
      Stmt del(db_, "DELETE FROM fts WHERE path = ?");
      if (del) { del.text(1, rel); sqlite3_step(del.s); }
      Stmt ins(db_, "INSERT INTO fts(text, heading, name, path) VALUES(?,?,?,?)");
      const std::string nm = nameWords(rel);
      for (const auto& c : chunks) {
        if (!ins) break;
        sqlite3_reset(ins.s);
        ins.text(1, c.text);
        ins.text(2, c.heading);
        ins.text(3, nm);
        ins.text(4, rel);
        sqlite3_step(ins.s);
      }
      Stmt up(db_, "INSERT OR REPLACE INTO files(path, mtime, size, chunks) VALUES(?,?,?,?)");
      if (up) {
        up.text(1, rel);
        up.i64(2, mt);
        up.i64(3, sz);
        up.i64(4, static_cast<long long>(chunks.size()));
        sqlite3_step(up.s);
      }
    }
    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    if (k == known.end()) ++s.added; else ++s.updated;
  }

  for (const auto& kv : known) {
    if (seen.count(kv.first)) continue;
    Stmt d1(db_, "DELETE FROM fts WHERE path = ?");
    Stmt d2(db_, "DELETE FROM files WHERE path = ?");
    if (d1 && d2) {
      d1.text(1, kv.first); sqlite3_step(d1.s);
      d2.text(1, kv.first); sqlite3_step(d2.s);
    }
    ++s.removed;
  }
  s.files = scalar("SELECT COUNT(*) FROM files");
  s.chunks = scalar("SELECT COALESCE(SUM(chunks),0) FROM files");
  return s;
}

// ---------------------------------------------------------------- search
std::vector<std::string> Rag::queryTerms(const std::string& q) {
  std::vector<std::string> terms;
  std::set<std::string> seen;
  std::string w;
  bool ascii = true;
  auto flush = [&]() {
    if (w.empty()) return;
    bool keep = ascii ? (w.size() >= 2 && !stopwords().count(w)) : true;
    if (keep && !seen.count(w)) {
      seen.insert(w);
      terms.push_back(w);
    }
    w.clear();
    ascii = true;
  };
  size_t i = 0;
  while (i < q.size()) {
    char32_t cp = u8::next(q, i);
    if (cp < 0x80) {
      if (std::isalnum(static_cast<int>(cp)) || cp == '_') {
        w += static_cast<char>(std::tolower(static_cast<int>(cp)));
      } else {
        flush();
      }
    } else if (nonAsciiSeparator(cp)) {
      flush();
    } else {
      u8::append(w, cp);
      ascii = false;
    }
  }
  flush();
  if (terms.size() > 12) terms.resize(12);
  return terms;
}

std::vector<RagHit> Rag::search(const std::string& question, int k) {
  std::vector<RagHit> hits;
  const std::vector<std::string> terms = queryTerms(question);
  if (terms.empty() || k <= 0) return hits;

  std::string match;
  for (const auto& t : terms) {
    if (!match.empty()) match += " OR ";
    bool asciiOnly = std::all_of(t.begin(), t.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    match += "\"" + t + "\"";
    if (asciiOnly && t.size() >= 4) match += "*";
  }

  struct Cand { RagHit hit; int found; };
  std::vector<Cand> cands;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!db_) return hits;
    Stmt st(db_,
            "SELECT path, heading, text, bm25(fts, 1.0, 4.0, 2.0, 0.0) AS s FROM fts "
            "WHERE fts MATCH ? ORDER BY s LIMIT 16");
    if (!st) return hits;
    st.text(1, match);
    while (sqlite3_step(st.s) == SQLITE_ROW) {
      RagHit h;
      h.path = st.col(0);
      h.heading = st.col(1);
      h.text = st.col(2);
      h.score = -sqlite3_column_double(st.s, 3);
      cands.push_back({h, 0});
    }
  }

  // Keep only chunks that contain a reasonable share of the question's keywords.
  const size_t need = std::max<size_t>(1, static_cast<size_t>(std::ceil(terms.size() * 0.4)));
  for (auto& c : cands) {
    std::string hay = lowerAscii(c.hit.text + " " + c.hit.heading + " " + nameWords(c.hit.path));
    for (const auto& t : terms)
      if (hay.find(t) != std::string::npos) ++c.found;
  }
  // Also drop passages that score far below the best one (they only add noise for a small model).
  double top = 0;
  for (auto& c : cands) {
    if (static_cast<size_t>(c.found) >= need) { top = c.hit.score; break; }
  }
  for (auto& c : cands) {
    if (static_cast<size_t>(c.found) < need) continue;
    if (top > 0.5 && c.hit.score < 0.25 * top) continue;
    hits.push_back(c.hit);
    if (static_cast<int>(hits.size()) >= k) break;
  }
  return hits;
}

// ---------------------------------------------------------------- prompt
std::string Rag::buildPrompt(const std::string& question, const std::vector<RagHit>& hits, size_t maxBytes,
                             int& used) {
  std::string p =
      "Use the context below to answer the question in one or two short sentences. "
      "Take the facts from the context. Only if the context has nothing relevant, "
      "say \"I don't know based on the documents.\"\n\n"
      "Question: " + question + "\n\nContext:\n";
  used = 0;
  size_t budget = maxBytes;
  for (size_t i = 0; i < hits.size(); ++i) {
    std::string block = "[" + std::to_string(i + 1) + "] (" + hits[i].label() + ")\n" + hits[i].text + "\n\n";
    if (block.size() > budget) {
      if (used > 0) break;
      size_t cut = budget;  // always keep at least the best hit, trimmed
      while (cut > 0 && (static_cast<unsigned char>(block[cut]) & 0xC0) == 0x80) --cut;
      block = block.substr(0, cut) + "\n\n";
    }
    budget = budget > block.size() ? budget - block.size() : 0;
    p += block;
    ++used;
  }
  p += "Question: " + question + "\nAnswer:";
  return p;
}

std::string Rag::formatSources(const std::vector<RagHit>& hits, int used) {
  std::string s = "SOURCES";
  for (int i = 0; i < used && i < static_cast<int>(hits.size()); ++i)
    s += "\n[" + std::to_string(i + 1) + "] " + hits[static_cast<size_t>(i)].label();
  return s;
}
