// Command-line helper for the document index (no model needed).
//   rag_tool ingest DOCS_DIR DB_FILE
//   rag_tool search DB_FILE "question" [k]
//   rag_tool stats  DB_FILE
#include <cstdio>
#include <cstdlib>
#include <string>

#include "rag.h"

static int fail(const std::string& m) {
  std::fprintf(stderr, "error: %s\n", m.c_str());
  return 1;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage:\n  %s ingest DOCS_DIR DB_FILE\n  %s search DB_FILE \"question\" [k]\n  %s stats DB_FILE\n",
                argv[0], argv[0], argv[0]);
    return 2;
  }
  const std::string cmd = argv[1];
  Rag rag;
  std::string err;
  if (cmd == "ingest" && argc >= 4) {
    if (!rag.open(argv[3], err)) return fail(err);
    IngestStats s = rag.ingest(argv[2]);
    if (!s.error.empty()) return fail(s.error);
    std::printf("added %d, updated %d, unchanged %d, removed %d, skipped %d -> %d files, %d chunks\n", s.added,
                s.updated, s.unchanged, s.removed, s.skipped, s.files, s.chunks);
    return 0;
  }
  if (cmd == "search" && argc >= 4) {
    if (!rag.open(argv[2], err)) return fail(err);
    int k = argc >= 5 ? std::atoi(argv[4]) : 3;
    std::printf("keywords:");
    for (auto& t : Rag::queryTerms(argv[3])) std::printf(" %s", t.c_str());
    std::printf("\n");
    auto hits = rag.search(argv[3], k);
    if (hits.empty()) std::printf("no relevant documents found\n");
    int i = 0;
    for (auto& h : hits) {
      std::printf("\n[%d] %s  (score %.2f)\n%s\n", ++i, h.label().c_str(), h.score, h.text.c_str());
    }
    return 0;
  }
  if (cmd == "stats") {
    if (!rag.open(argv[2], err)) return fail(err);
    std::printf("%d files, %d chunks\n", rag.fileCount(), rag.chunkCount());
    for (auto& d : rag.categories()) std::printf("  %-14s %3d files %4d chunks\n", d.category.c_str(), d.files, d.chunks);
    return 0;
  }
  return fail("bad arguments");
}
