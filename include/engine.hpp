#pragma once

#include "analyzer.hpp"
#include "query.hpp"
#include "segment.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace indexdb {

struct SearchHit {
  std::string id;
  std::string title;
  std::string body;
  double score = 0;
};

struct SearchResponse {
  std::vector<SearchHit> hits;
  std::size_t total = 0;
};

struct Stats {
  std::size_t sealed_segments = 0;
  std::size_t active_docs = 0;
  std::size_t live_docs = 0;
  std::size_t dead_docs = 0;
  std::size_t posting_lists = 0;
  std::uintmax_t bytes_on_disk = 0;
};

struct FlushInfo {
  std::string path;
  std::size_t docs = 0;
};

// A memory batch this full is written to a segment file on its own.
inline constexpr std::size_t kAutoFlushDocs = 8;

struct PutResult {
  std::size_t active_docs = 0;
  FlushInfo flushed;
};

struct MergeInfo {
  std::string path;
  bool compacted = false;
};

// Tiny search database: an in-memory active segment, sealed segment files,
// and a manifest that names the committed ones.
class Engine {
 public:
  explicit Engine(std::string data_dir);

  PutResult put(std::string id, std::string title, std::string body);
  void remove(const std::string& id);
  std::optional<StoredDoc> get(const std::string& id) const;
  std::vector<StoredDoc> list() const;

  SearchResponse search(const std::string& query, std::size_t limit = 10) const;
  FlushInfo flush();
  MergeInfo merge();
  Stats stats() const;

  const std::string& data_dir() const { return dir_; }

 private:
  struct Sealed {
    std::string path;
    Segment segment;
  };

  Segment* find_segment(const std::string& id, std::size_t* sealed_index);
  const Segment* find_segment(const std::string& id) const;
  std::vector<const Segment*> snapshot() const;
  std::string new_segment_path() const;
  void load_committed();
  void publish(const std::vector<std::string>& basenames) const;
  void remove_unreferenced() const;

  std::string dir_;
  Analyzer analyzer_;
  Segment active_;
  std::vector<Sealed> sealed_;
};

}  // namespace indexdb
