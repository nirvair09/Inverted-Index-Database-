#pragma once

#include "segment.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace indexdb {

class Analyzer;

struct Node {
  enum class Kind { Term, Phrase, And, Or, Not };
  Kind kind = Kind::Term;
  std::string field;  // empty means title and body
  std::vector<std::string> terms;
  std::vector<Node> children;
};

// Corpus-wide BM25 stats so scores compare across segments.
struct Similarity {
  std::uint32_t documents = 0;
  double avg_title = 1.0;
  double avg_body = 1.0;

  void set_df(const std::string& field, const std::string& term, std::uint32_t df);
  std::uint32_t df_of(const std::string& field, const std::string& term) const;

 private:
  std::unordered_map<std::string, std::uint32_t> df_;
};

struct ScoredDoc {
  std::uint32_t doc = 0;
  double score = 0;
};

class QueryParser {
 public:
  explicit QueryParser(const Analyzer& analyzer);
  Node parse(const std::string& query) const;

 private:
  const Analyzer& analyzer_;
};

void collect_query_terms(const Node& node, std::vector<std::pair<std::string, std::string>>& out);

std::vector<ScoredDoc> execute(const Segment& segment, const Node& query, const Similarity& sim);

}  // namespace indexdb
