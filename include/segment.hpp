#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace indexdb {

struct Posting {
  std::uint32_t doc = 0;
  std::uint16_t tf = 0;
  std::vector<std::uint16_t> positions;
};

struct StoredDoc {
  std::string id;
  std::string title;
  std::string body;
  std::uint32_t title_len = 0;
  std::uint32_t body_len = 0;
  bool dead = false;
};

std::string escape_text(const std::string& text);
std::string unescape_text(const std::string& text);

// term -> postings sorted by local doc id.
class FieldIndex {
 public:
  void add_document(std::uint32_t doc, const std::vector<std::string>& tokens);
  void set_postings(std::unordered_map<std::string, std::vector<Posting>> postings,
                    std::uint64_t total_tokens);

  const std::vector<Posting>* postings_for(const std::string& term) const;
  std::uint64_t total_tokens() const { return total_tokens_; }
  std::size_t term_count() const { return postings_.size(); }
  const std::unordered_map<std::string, std::vector<Posting>>& all() const { return postings_; }

 private:
  std::unordered_map<std::string, std::vector<Posting>> postings_;
  std::uint64_t total_tokens_ = 0;
};

// One immutable batch of documents plus their inverted index.
// The dead flag is the only mutation after seal; the postings file is not rewritten.
class Segment {
 public:
  std::uint32_t add(std::string id, std::string title, std::string body,
                    const std::vector<std::string>& title_tokens,
                    const std::vector<std::string>& body_tokens);

  bool kill(const std::string& id);
  const StoredDoc* find_live(const std::string& id) const;
  bool has_dead() const;

  void save(const std::string& path) const;
  static Segment load(const std::string& path);
  static Segment merge_all(const std::vector<const Segment*>& parts);

  const std::vector<StoredDoc>& docs() const { return docs_; }
  const FieldIndex& index_for(const std::string& field) const;
  static double boost_for(const std::string& field);

  std::size_t live_count() const;
  std::uint64_t live_tokens(const std::string& field) const;
  std::uint32_t live_df(const std::string& field, const std::string& term) const;
  std::size_t term_count() const;

 private:
  std::vector<StoredDoc> docs_;
  FieldIndex title_;
  FieldIndex body_;
};

}  // namespace indexdb
