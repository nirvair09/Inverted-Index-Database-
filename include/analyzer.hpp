#pragma once

#include <memory>
#include <string>
#include <vector>

namespace indexdb {

// One stage in the analyzer pipeline. Subclass this to add a filter.
class TokenFilter {
 public:
  virtual ~TokenFilter() = default;
  virtual void apply(std::vector<std::string>& tokens) const = 0;
};

// Drops a small stopword list ("the", "of", ...).
class StopwordFilter : public TokenFilter {
 public:
  void apply(std::vector<std::string>& tokens) const override;
};

// Light suffix stemmer: jumps/jumping -> jump, foxes -> fox.
class StemFilter : public TokenFilter {
 public:
  void apply(std::vector<std::string>& tokens) const override;
};

// Tokenizer (lowercase, split on non-alphanumerics) then the filters above.
// Indexing and queries share one analyzer, so both sides stem the same way.
class Analyzer {
 public:
  Analyzer();
  std::vector<std::string> analyze(const std::string& text) const;

 private:
  std::vector<std::unique_ptr<TokenFilter>> filters_;
};

}  // namespace indexdb
