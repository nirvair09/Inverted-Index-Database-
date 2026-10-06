#include "analyzer.hpp"

#include <cctype>
#include <unordered_set>

namespace indexdb {
namespace {

bool ends_with(const std::string& word, const char* suffix) {
  const std::size_t n = std::char_traits<char>::length(suffix);
  return word.size() >= n && word.compare(word.size() - n, n, suffix) == 0;
}

const std::unordered_set<std::string>& stopwords() {
  static const std::unordered_set<std::string> words = {
      "a",  "an", "the", "is",  "are", "was", "were", "be",  "of", "and",
      "or", "not", "to", "in",  "on",  "for", "with", "at",  "by", "from",
      "as", "it", "that", "this"};
  return words;
}

std::vector<std::string> tokenize(const std::string& text) {
  std::vector<std::string> tokens;
  std::string current;
  auto flush = [&]() {
    if (current.empty()) return;
    tokens.push_back(std::move(current));
    current.clear();
  };
  for (unsigned char c : text) {
    if (std::isalnum(c)) {
      current.push_back(static_cast<char>(std::tolower(c)));
    } else {
      flush();
    }
  }
  flush();
  return tokens;
}

// Strip a trailing suffix only when the leftover stem is longer than 2 letters.
std::string stem_word(std::string word) {
  auto strip = [&](std::size_t n) {
    word.resize(word.size() - n);
  };
  if (word.size() > 5 && ends_with(word, "ing")) {
    strip(3);
    return word;
  }
  if (word.size() > 4 && ends_with(word, "ed")) {
    strip(2);
    return word;
  }
  if (word.size() > 4 && ends_with(word, "es")) {
    const std::string stem = word.substr(0, word.size() - 2);
    const bool plural_es = ends_with(stem, "ch") || ends_with(stem, "sh") ||
                           (!stem.empty() && (stem.back() == 's' || stem.back() == 'x' ||
                                              stem.back() == 'z'));
    if (plural_es) return stem;
  }
  if (word.size() > 3 && ends_with(word, "s") && !ends_with(word, "ss")) {
    word.pop_back();
  }
  return word;
}

}  // namespace

void StopwordFilter::apply(std::vector<std::string>& tokens) const {
  std::vector<std::string> kept;
  kept.reserve(tokens.size());
  for (auto& token : tokens) {
    if (!stopwords().count(token)) kept.push_back(std::move(token));
  }
  tokens.swap(kept);
}

void StemFilter::apply(std::vector<std::string>& tokens) const {
  for (auto& token : tokens) token = stem_word(std::move(token));
}

Analyzer::Analyzer() {
  filters_.push_back(std::make_unique<StopwordFilter>());
  filters_.push_back(std::make_unique<StemFilter>());
}

std::vector<std::string> Analyzer::analyze(const std::string& text) const {
  auto tokens = tokenize(text);
  for (const auto& filter : filters_) filter->apply(tokens);
  return tokens;
}

}  // namespace indexdb
