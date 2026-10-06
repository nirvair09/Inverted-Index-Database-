#include "query.hpp"

#include "analyzer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace indexdb {
namespace {

constexpr double k1 = 1.2;
constexpr double b = 0.75;
constexpr double kPhraseBoost = 1.25;

enum class TokKind { End, Word, Phrase, Or, And, Not, LParen, RParen, Minus };

struct Tok {
  TokKind kind = TokKind::End;
  std::string text;
  std::string field;
};

bool is_word_char(unsigned char c) { return std::isalnum(c) != 0; }

std::string lower_copy(std::string word) {
  for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return word;
}

class Lexer {
 public:
  explicit Lexer(std::string input) : in_(std::move(input)) {}

  Tok next() {
    skip();
    if (i_ >= in_.size()) return {};
    const char c = in_[i_];
    if (c == '(') {
      ++i_;
      return Tok{TokKind::LParen, "", ""};
    }
    if (c == ')') {
      ++i_;
      return Tok{TokKind::RParen, "", ""};
    }
    if (c == '"') return Tok{TokKind::Phrase, read_quoted(), ""};
    if (c == '-') {
      ++i_;
      return Tok{TokKind::Minus, "", ""};
    }
    if (!is_word_char(static_cast<unsigned char>(c))) {
      throw std::runtime_error(std::string("unexpected character '") + c + "'");
    }
    const std::size_t begin = i_;
    while (i_ < in_.size() && is_word_char(static_cast<unsigned char>(in_[i_]))) ++i_;
    std::string word = in_.substr(begin, i_ - begin);
    if (i_ < in_.size() && in_[i_] == ':') {
      ++i_;
      const std::string field = lower_copy(word);
      if (i_ < in_.size() && in_[i_] == '"') {
        return Tok{TokKind::Phrase, read_quoted(), field};
      }
      const std::size_t term_begin = i_;
      while (i_ < in_.size() && is_word_char(static_cast<unsigned char>(in_[i_]))) ++i_;
      if (term_begin == i_) throw std::runtime_error("expected a term after '" + field + ":'");
      return Tok{TokKind::Word, in_.substr(term_begin, i_ - term_begin), field};
    }
    const std::string op = lower_copy(word);
    if (op == "or") return Tok{TokKind::Or, "", ""};
    if (op == "and") return Tok{TokKind::And, "", ""};
    if (op == "not") return Tok{TokKind::Not, "", ""};
    return Tok{TokKind::Word, std::move(word), ""};
  }

 private:
  void skip() {
    while (i_ < in_.size() && std::isspace(static_cast<unsigned char>(in_[i_]))) ++i_;
  }

  std::string read_quoted() {
    ++i_;  // opening quote
    std::string text;
    while (i_ < in_.size() && in_[i_] != '"') {
      if (in_[i_] == '\\' && i_ + 1 < in_.size()) {
        text.push_back(in_[i_ + 1]);
        i_ += 2;
      } else {
        text.push_back(in_[i_++]);
      }
    }
    if (i_ >= in_.size() || in_[i_] != '"') throw std::runtime_error("unterminated quote");
    ++i_;
    return text;
  }

  std::string in_;
  std::size_t i_ = 0;
};

std::string describe(const Tok& tok) {
  switch (tok.kind) {
    case TokKind::End:
      return "end of query";
    case TokKind::Or:
      return "OR";
    case TokKind::And:
      return "AND";
    case TokKind::Not:
      return "NOT";
    case TokKind::LParen:
      return "'('";
    case TokKind::RParen:
      return "')'";
    case TokKind::Minus:
      return "'-'";
    case TokKind::Word:
    case TokKind::Phrase:
      return "'" + tok.text + "'";
  }
  return "token";
}

class Parser {
 public:
  Parser(const Analyzer& analyzer, const std::string& query)
      : analyzer_(analyzer), lexer_(query) {
    cur_ = lexer_.next();
  }

  std::optional<Node> parse_or() {
    auto left = parse_and();
    if (cur_.kind != TokKind::Or) return left;
    std::vector<Node> kids;
    if (left) kids.push_back(std::move(*left));
    while (cur_.kind == TokKind::Or) {
      advance();
      auto right = parse_and();
      if (right) kids.push_back(std::move(*right));
    }
    if (kids.empty()) return std::nullopt;
    if (kids.size() == 1) return kids[0];
    Node node;
    node.kind = Node::Kind::Or;
    node.children = std::move(kids);
    return node;
  }

  const Tok& current() const { return cur_; }

 private:
  bool clause_start() const {
    return cur_.kind == TokKind::Word || cur_.kind == TokKind::Phrase || cur_.kind == TokKind::Not ||
           cur_.kind == TokKind::Minus || cur_.kind == TokKind::LParen;
  }

  void advance() { cur_ = lexer_.next(); }

  void check_field(const std::string& field) const {
    if (!field.empty() && field != "title" && field != "body") {
      throw std::runtime_error("unknown field \"" + field + "\" (use title or body)");
    }
  }

  std::optional<Node> parse_and() {
    std::vector<Node> kids;
    auto take = [&](std::optional<Node> node) {
      if (node) kids.push_back(std::move(*node));
    };
    take(parse_unary());
    while (cur_.kind == TokKind::And || clause_start()) {
      if (cur_.kind == TokKind::And) advance();
      take(parse_unary());
    }
    if (kids.empty()) return std::nullopt;
    if (kids.size() == 1) return kids[0];
    Node node;
    node.kind = Node::Kind::And;
    node.children = std::move(kids);
    return node;
  }

  std::optional<Node> parse_unary() {
    if (cur_.kind == TokKind::Not || cur_.kind == TokKind::Minus) {
      advance();
      auto child = parse_unary();
      if (!child) return std::nullopt;
      Node node;
      node.kind = Node::Kind::Not;
      node.children.push_back(std::move(*child));
      return node;
    }
    return parse_primary();
  }

  std::optional<Node> parse_primary() {
    if (cur_.kind == TokKind::LParen) {
      advance();
      auto inner = parse_or();
      if (cur_.kind != TokKind::RParen) throw std::runtime_error("expected ')'");
      advance();
      return inner;
    }
    if (cur_.kind == TokKind::Phrase || cur_.kind == TokKind::Word) {
      Tok tok = cur_;
      advance();
      check_field(tok.field);
      auto terms = analyzer_.analyze(tok.text);
      if (terms.empty()) return std::nullopt;
      Node node;
      node.kind = tok.kind == TokKind::Phrase ? Node::Kind::Phrase : Node::Kind::Term;
      node.field = std::move(tok.field);
      node.terms = std::move(terms);
      return node;
    }
    throw std::runtime_error("unexpected " + describe(cur_));
  }

  const Analyzer& analyzer_;
  Lexer lexer_;
  Tok cur_;
};

struct DocScores {
  std::vector<std::uint32_t> docs;
  std::vector<double> scores;
  bool negated = false;
};

DocScores intersect(const DocScores& a, const DocScores& b) {
  DocScores out;
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.docs.size() && j < b.docs.size()) {
    if (a.docs[i] == b.docs[j]) {
      out.docs.push_back(a.docs[i]);
      out.scores.push_back(a.scores[i] + b.scores[j]);
      ++i;
      ++j;
    } else if (a.docs[i] < b.docs[j]) {
      ++i;
    } else {
      ++j;
    }
  }
  return out;
}

DocScores unite(const DocScores& a, const DocScores& b) {
  DocScores out;
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < a.docs.size() || j < b.docs.size()) {
    const bool take_a = j == b.docs.size() || (i < a.docs.size() && a.docs[i] < b.docs[j]);
    const bool take_b = i == a.docs.size() || (j < b.docs.size() && b.docs[j] < a.docs[i]);
    if (take_a) {
      out.docs.push_back(a.docs[i]);
      out.scores.push_back(a.scores[i]);
      ++i;
    } else if (take_b) {
      out.docs.push_back(b.docs[j]);
      out.scores.push_back(b.scores[j]);
      ++j;
    } else {
      out.docs.push_back(a.docs[i]);
      out.scores.push_back(a.scores[i] + b.scores[j]);
      ++i;
      ++j;
    }
  }
  return out;
}

DocScores subtract(const DocScores& src, const DocScores& ban) {
  DocScores out;
  std::size_t j = 0;
  for (std::size_t i = 0; i < src.docs.size(); ++i) {
    while (j < ban.docs.size() && ban.docs[j] < src.docs[i]) ++j;
    if (j < ban.docs.size() && ban.docs[j] == src.docs[i]) continue;
    out.docs.push_back(src.docs[i]);
    out.scores.push_back(src.scores[i]);
  }
  return out;
}

double bm25_score(std::uint32_t tf, std::uint32_t dl, double avgdl, std::uint32_t documents,
                  std::uint32_t df) {
  if (documents == 0 || df == 0 || tf == 0) return 0.0;
  if (avgdl <= 0.0) avgdl = 1.0;
  // Lucene's BM25 idf: log(1 + (N - df + 0.5) / (df + 0.5))
  const double idf = std::log(1.0 + (static_cast<double>(documents) - static_cast<double>(df) + 0.5) /
                                       (static_cast<double>(df) + 0.5));
  const double norm = 1.0 - b + b * (static_cast<double>(dl) / avgdl);
  const double freq = (static_cast<double>(tf) * (k1 + 1.0)) /
                      (static_cast<double>(tf) + k1 * norm);
  return idf * freq;
}

bool positions_match(const std::vector<const Posting*>& chain) {
  const auto& starts = chain[0]->positions;
  for (std::uint16_t start : starts) {
    bool ok = true;
    for (std::size_t k = 1; k < chain.size(); ++k) {
      const auto need = static_cast<std::uint32_t>(start) + static_cast<std::uint32_t>(k);
      if (need > std::numeric_limits<std::uint16_t>::max()) {
        ok = false;
        break;
      }
      const auto& positions = chain[k]->positions;
      if (!std::binary_search(positions.begin(), positions.end(), static_cast<std::uint16_t>(need))) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
  }
  return false;
}

DocScores score_term_field(const Segment& segment, const Similarity& sim, const std::string& field,
                           const std::string& term) {
  DocScores out;
  const auto* list = segment.index_for(field).postings_for(term);
  if (!list) return out;
  const std::uint32_t df = sim.df_of(field, term);
  const double avg = field == "title" ? sim.avg_title : sim.avg_body;
  const double boost = Segment::boost_for(field);
  for (const Posting& posting : *list) {
    const StoredDoc& doc = segment.docs()[posting.doc];
    if (doc.dead) continue;
    const std::uint32_t dl = field == "title" ? doc.title_len : doc.body_len;
    out.docs.push_back(posting.doc);
    out.scores.push_back(bm25_score(posting.tf, dl, avg, sim.documents, df) * boost);
  }
  return out;
}

DocScores score_term(const Segment& segment, const Similarity& sim, const Node& node) {
  if (node.terms.empty()) return {};
  const std::string& term = node.terms[0];
  if (!node.field.empty()) return score_term_field(segment, sim, node.field, term);
  return unite(score_term_field(segment, sim, "title", term),
               score_term_field(segment, sim, "body", term));
}

DocScores score_phrase_field(const Segment& segment, const Similarity& sim, const std::string& field,
                             const std::vector<std::string>& terms) {
  DocScores out;
  std::vector<const std::vector<Posting>*> lists;
  lists.reserve(terms.size());
  for (const auto& term : terms) {
    const auto* list = segment.index_for(field).postings_for(term);
    if (!list || list->empty()) return out;
    lists.push_back(list);
  }
  std::vector<std::size_t> idx(lists.size(), 0);
  const double avg = field == "title" ? sim.avg_title : sim.avg_body;
  const double boost = Segment::boost_for(field);
  while (idx[0] < lists[0]->size()) {
    const std::uint32_t doc = (*lists[0])[idx[0]].doc;
    bool aligned = true;
    for (std::size_t k = 1; k < lists.size(); ++k) {
      const auto& list = *lists[k];
      while (idx[k] < list.size() && list[idx[k]].doc < doc) ++idx[k];
      if (idx[k] == list.size()) return out;
      if (list[idx[k]].doc != doc) {
        aligned = false;
        const std::uint32_t need = list[idx[k]].doc;
        while (idx[0] < lists[0]->size() && (*lists[0])[idx[0]].doc < need) ++idx[0];
        break;
      }
    }
    if (!aligned) continue;
    std::vector<const Posting*> chain;
    chain.reserve(lists.size());
    for (std::size_t k = 0; k < lists.size(); ++k) chain.push_back(&(*lists[k])[idx[k]]);
    const bool dead = segment.docs()[doc].dead;
    if (!dead && positions_match(chain)) {
      const StoredDoc& stored = segment.docs()[doc];
      const std::uint32_t dl = field == "title" ? stored.title_len : stored.body_len;
      double score = 0;
      for (std::size_t k = 0; k < terms.size(); ++k) {
        score += bm25_score(chain[k]->tf, dl, avg, sim.documents, sim.df_of(field, terms[k]));
      }
      out.docs.push_back(doc);
      out.scores.push_back(score * boost * kPhraseBoost);
    }
    ++idx[0];
  }
  return out;
}

DocScores score_phrase(const Segment& segment, const Similarity& sim, const Node& node) {
  if (node.terms.empty()) return {};
  if (!node.field.empty()) return score_phrase_field(segment, sim, node.field, node.terms);
  return unite(score_phrase_field(segment, sim, "title", node.terms),
               score_phrase_field(segment, sim, "body", node.terms));
}

DocScores eval_node(const Segment& segment, const Similarity& sim, const Node& node) {
  switch (node.kind) {
    case Node::Kind::Term:
      return score_term(segment, sim, node);
    case Node::Kind::Phrase:
      return score_phrase(segment, sim, node);
    case Node::Kind::Not: {
      auto child = eval_node(segment, sim, node.children.at(0));
      child.negated = !child.negated;
      return child;
    }
    case Node::Kind::And: {
      std::vector<DocScores> parts;
      std::vector<DocScores> bans;
      for (const auto& child : node.children) {
        auto scored = eval_node(segment, sim, child);
        if (scored.negated) bans.push_back(std::move(scored));
        else parts.push_back(std::move(scored));
      }
      if (parts.empty()) return {};
      DocScores acc = std::move(parts[0]);
      for (std::size_t i = 1; i < parts.size(); ++i) acc = intersect(acc, parts[i]);
      for (const auto& ban : bans) acc = subtract(acc, ban);
      return acc;
    }
    case Node::Kind::Or: {
      DocScores acc;
      bool any = false;
      for (const auto& child : node.children) {
        auto scored = eval_node(segment, sim, child);
        if (scored.negated) continue;
        acc = any ? unite(acc, scored) : std::move(scored);
        any = true;
      }
      return acc;
    }
  }
  return {};
}

bool blank(const std::string& query) {
  for (unsigned char c : query) {
    if (!std::isspace(c)) return false;
  }
  return true;
}

}  // namespace

void Similarity::set_df(const std::string& field, const std::string& term, std::uint32_t df) {
  df_[field + "\n" + term] = df;
}

std::uint32_t Similarity::df_of(const std::string& field, const std::string& term) const {
  const auto it = df_.find(field + "\n" + term);
  if (it == df_.end()) return 0;
  return it->second;
}

QueryParser::QueryParser(const Analyzer& analyzer) : analyzer_(analyzer) {}

Node QueryParser::parse(const std::string& query) const {
  if (blank(query)) throw std::runtime_error("query has no searchable terms");
  Parser parser(analyzer_, query);
  auto node = parser.parse_or();
  if (parser.current().kind != TokKind::End) {
    throw std::runtime_error("unexpected " + describe(parser.current()));
  }
  if (!node) throw std::runtime_error("query has no searchable terms");
  return *node;
}

void collect_query_terms(const Node& node, std::vector<std::pair<std::string, std::string>>& out) {
  if (node.kind == Node::Kind::Term || node.kind == Node::Kind::Phrase) {
    for (const auto& term : node.terms) out.emplace_back(node.field, term);
    return;
  }
  for (const auto& child : node.children) collect_query_terms(child, out);
}

std::vector<ScoredDoc> execute(const Segment& segment, const Node& query, const Similarity& sim) {
  auto scored = eval_node(segment, sim, query);
  std::vector<ScoredDoc> hits;
  if (scored.negated) return hits;
  hits.reserve(scored.docs.size());
  for (std::size_t i = 0; i < scored.docs.size(); ++i) {
    if (segment.docs()[scored.docs[i]].dead) continue;
    hits.push_back(ScoredDoc{scored.docs[i], scored.scores[i]});
  }
  return hits;
}

}  // namespace indexdb
