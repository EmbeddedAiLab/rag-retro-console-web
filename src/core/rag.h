// Offline retrieval for RAG: documents -> chunks -> SQLite FTS5 (BM25) -> prompt.
#pragma once
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct sqlite3;

struct RagHit {
  std::string path;     // relative to the docs folder, e.g. "stories/lighthouse.md"
  std::string heading;  // "Chapter 1 > The Storm" (may be empty)
  std::string text;
  double score = 0;     // higher = better
  std::string label() const { return heading.empty() ? path : path + " > " + heading; }
};

struct IngestStats {
  int added = 0, updated = 0, unchanged = 0, removed = 0, skipped = 0;
  int files = 0, chunks = 0;  // totals in the index afterwards
  std::string error;
};

struct DocStat {
  std::string category;  // first sub-folder of the docs folder
  int files = 0;
  int chunks = 0;
};

class Rag {
 public:
  Rag();
  ~Rag();
  Rag(const Rag&) = delete;
  Rag& operator=(const Rag&) = delete;

  bool open(const std::string& dbPath, std::string& error);
  bool isOpen() const;

  // Incremental: only new / changed / deleted files are touched.
  IngestStats ingest(const std::string& docsDir);

  std::vector<RagHit> search(const std::string& question, int k);
  int chunkCount();
  int fileCount();
  std::vector<DocStat> categories();
  std::vector<std::pair<std::string, int>> files();  // path, chunks

  // Keywords that will be searched for (exposed for /search and debugging).
  static std::vector<std::string> queryTerms(const std::string& question);
  // Builds the text that is sent to the LLM. `used` = number of hits that fit.
  static std::string buildPrompt(const std::string& question, const std::vector<RagHit>& hits,
                                 size_t maxBytes, int& used);
  static std::string formatSources(const std::vector<RagHit>& hits, int used);

 private:
  int scalar(const char* sql);

  sqlite3* db_ = nullptr;
  std::mutex m_;
};
