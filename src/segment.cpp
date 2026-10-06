#include "segment.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace indexdb {
namespace {

std::string read_line(std::istream& in, const std::string& path) {
  std::string line;
  if (!std::getline(in, line)) {
    throw std::runtime_error("truncated segment file: " + path);
  }
  if (!line.empty() && line.back() == '\r') line.pop_back();
  return line;
}

std::size_t read_count(std::istream& in, const std::string& path) {
  const std::string line = read_line(in, path);
  try {
    std::size_t used = 0;
    const unsigned long value = std::stoul(line, &used);
    if (used != line.size()) throw std::runtime_error("bad");
    return static_cast<std::size_t>(value);
  } catch (const std::exception&) {
    throw std::runtime_error("bad count in segment file: " + path);
  }
}

void write_field(std::ostream& out, const FieldIndex& field) {
  std::vector<std::string> terms;
  terms.reserve(field.term_count());
  for (const auto& entry : field.all()) terms.push_back(entry.first);
  std::sort(terms.begin(), terms.end());
  out << terms.size() << '\n';
  for (const auto& term : terms) {
    const auto& list = field.all().at(term);
    out << term << '\n' << list.size() << '\n';
    for (const Posting& posting : list) {
      out << posting.doc << ' ' << posting.tf << ' ' << posting.positions.size();
      for (std::uint16_t pos : posting.positions) out << ' ' << pos;
      out << '\n';
    }
  }
}

void read_field(std::istream& in, const std::string& path, FieldIndex& field, std::uint32_t doc_count) {
  const std::size_t terms = read_count(in, path);
  std::unordered_map<std::string, std::vector<Posting>> table;
  std::uint64_t tokens = 0;
  for (std::size_t t = 0; t < terms; ++t) {
    std::string term = read_line(in, path);
    const std::size_t postings = read_count(in, path);
    std::vector<Posting> list;
    list.reserve(postings);
    for (std::size_t p = 0; p < postings; ++p) {
      std::istringstream line(read_line(in, path));
      Posting posting;
      std::uint32_t npos = 0;
      std::uint32_t tf = 0;
      if (!(line >> posting.doc >> tf >> npos)) {
        throw std::runtime_error("bad posting in segment file: " + path);
      }
      if (posting.doc >= doc_count || tf > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("posting out of range in segment file: " + path);
      }
      posting.tf = static_cast<std::uint16_t>(tf);
      posting.positions.reserve(npos);
      for (std::uint32_t k = 0; k < npos; ++k) {
        std::uint32_t pos = 0;
        if (!(line >> pos) || pos > std::numeric_limits<std::uint16_t>::max()) {
          throw std::runtime_error("bad position in segment file: " + path);
        }
        posting.positions.push_back(static_cast<std::uint16_t>(pos));
      }
      tokens += posting.tf;
      list.push_back(std::move(posting));
    }
    table.emplace(std::move(term), std::move(list));
  }
  field.set_postings(std::move(table), tokens);
}

}  // namespace

std::string escape_text(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (unsigned char c : text) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out.push_back(static_cast<char>(c));
        break;
    }
  }
  return out;
}

std::string unescape_text(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\\' && i + 1 < text.size()) {
      const char next = text[++i];
      if (next == 'n') out.push_back('\n');
      else if (next == 'r') out.push_back('\r');
      else out.push_back(next);
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

void FieldIndex::add_document(std::uint32_t doc, const std::vector<std::string>& tokens) {
  std::unordered_map<std::string, Posting> local;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    Posting& posting = local[tokens[i]];
    posting.doc = doc;
    if (posting.tf < std::numeric_limits<std::uint16_t>::max()) ++posting.tf;
    if (i <= std::numeric_limits<std::uint16_t>::max() &&
        posting.positions.size() < std::numeric_limits<std::uint16_t>::max()) {
      posting.positions.push_back(static_cast<std::uint16_t>(i));
    }
  }
  for (auto& entry : local) postings_[entry.first].push_back(std::move(entry.second));
  total_tokens_ += tokens.size();
}

void FieldIndex::set_postings(std::unordered_map<std::string, std::vector<Posting>> postings,
                              std::uint64_t total_tokens) {
  for (auto& entry : postings) {
    std::sort(entry.second.begin(), entry.second.end(),
              [](const Posting& a, const Posting& b) { return a.doc < b.doc; });
  }
  postings_ = std::move(postings);
  total_tokens_ = total_tokens;
}

const std::vector<Posting>* FieldIndex::postings_for(const std::string& term) const {
  const auto it = postings_.find(term);
  if (it == postings_.end()) return nullptr;
  return &it->second;
}

std::uint32_t Segment::add(std::string id, std::string title, std::string body,
                           const std::vector<std::string>& title_tokens,
                           const std::vector<std::string>& body_tokens) {
  StoredDoc doc;
  doc.id = std::move(id);
  doc.title = std::move(title);
  doc.body = std::move(body);
  doc.title_len = static_cast<std::uint32_t>(title_tokens.size());
  doc.body_len = static_cast<std::uint32_t>(body_tokens.size());
  const auto local = static_cast<std::uint32_t>(docs_.size());
  title_.add_document(local, title_tokens);
  body_.add_document(local, body_tokens);
  docs_.push_back(std::move(doc));
  return local;
}

bool Segment::kill(const std::string& id) {
  for (auto& doc : docs_) {
    if (!doc.dead && doc.id == id) {
      doc.dead = true;
      return true;
    }
  }
  return false;
}

const StoredDoc* Segment::find_live(const std::string& id) const {
  for (const auto& doc : docs_) {
    if (!doc.dead && doc.id == id) return &doc;
  }
  return nullptr;
}

bool Segment::has_dead() const {
  for (const auto& doc : docs_) {
    if (doc.dead) return true;
  }
  return false;
}

void Segment::save(const std::string& path) const {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot write segment: " + path);
  out << "INDEXDB 1\n" << docs_.size() << '\n';
  for (const auto& doc : docs_) {
    out << escape_text(doc.id) << '\n'
        << escape_text(doc.title) << '\n'
        << escape_text(doc.body) << '\n'
        << doc.title_len << ' ' << doc.body_len << '\n';
  }
  out << "title\n";
  write_field(out, title_);
  out << "body\n";
  write_field(out, body_);
  if (!out) throw std::runtime_error("failed while writing segment: " + path);
}

Segment Segment::load(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot read segment: " + path);
  if (read_line(in, path) != "INDEXDB 1") {
    throw std::runtime_error("not an indexdb segment: " + path);
  }
  Segment segment;
  const std::size_t docs = read_count(in, path);
  segment.docs_.reserve(docs);
  for (std::size_t i = 0; i < docs; ++i) {
    StoredDoc doc;
    doc.id = unescape_text(read_line(in, path));
    doc.title = unescape_text(read_line(in, path));
    doc.body = unescape_text(read_line(in, path));
    std::istringstream lens(read_line(in, path));
    if (!(lens >> doc.title_len >> doc.body_len)) {
      throw std::runtime_error("bad document length in segment file: " + path);
    }
    segment.docs_.push_back(std::move(doc));
  }
  if (read_line(in, path) != "title") {
    throw std::runtime_error("missing title index in segment file: " + path);
  }
  read_field(in, path, segment.title_, static_cast<std::uint32_t>(segment.docs_.size()));
  if (read_line(in, path) != "body") {
    throw std::runtime_error("missing body index in segment file: " + path);
  }
  read_field(in, path, segment.body_, static_cast<std::uint32_t>(segment.docs_.size()));
  return segment;
}

Segment Segment::merge_all(const std::vector<const Segment*>& parts) {
  struct Remap {
    const Segment* segment = nullptr;
    std::vector<int> to;
  };
  std::vector<Remap> maps;
  Segment out;
  for (const Segment* segment : parts) {
    Remap map;
    map.segment = segment;
    map.to.assign(segment->docs_.size(), -1);
    for (std::uint32_t i = 0; i < segment->docs_.size(); ++i) {
      if (segment->docs_[i].dead) continue;
      map.to[i] = static_cast<int>(out.docs_.size());
      StoredDoc copy = segment->docs_[i];
      copy.dead = false;
      out.docs_.push_back(std::move(copy));
    }
    maps.push_back(std::move(map));
  }

  auto collect = [&](bool title_field) {
    std::unordered_map<std::string, std::vector<Posting>> acc;
    std::uint64_t tokens = 0;
    for (const auto& map : maps) {
      const FieldIndex& source = title_field ? map.segment->title_ : map.segment->body_;
      for (const auto& entry : source.all()) {
        for (const Posting& posting : entry.second) {
          const int next = map.to[posting.doc];
          if (next < 0) continue;
          Posting rewritten = posting;
          rewritten.doc = static_cast<std::uint32_t>(next);
          acc[entry.first].push_back(std::move(rewritten));
          tokens += posting.tf;
        }
      }
    }
    return std::pair<std::unordered_map<std::string, std::vector<Posting>>, std::uint64_t>{
        std::move(acc), tokens};
  };

  auto title = collect(true);
  auto body = collect(false);
  out.title_.set_postings(std::move(title.first), title.second);
  out.body_.set_postings(std::move(body.first), body.second);
  return out;
}

const FieldIndex& Segment::index_for(const std::string& field) const {
  if (field == "title") return title_;
  if (field == "body") return body_;
  throw std::runtime_error("unknown field \"" + field + "\" (use title or body)");
}

double Segment::boost_for(const std::string& field) {
  if (field == "title") return 2.0;
  return 1.0;
}

std::size_t Segment::live_count() const {
  std::size_t n = 0;
  for (const auto& doc : docs_) {
    if (!doc.dead) ++n;
  }
  return n;
}

std::uint64_t Segment::live_tokens(const std::string& field) const {
  std::uint64_t n = 0;
  for (const auto& doc : docs_) {
    if (doc.dead) continue;
    n += field == "title" ? doc.title_len : doc.body_len;
  }
  return n;
}

std::uint32_t Segment::live_df(const std::string& field, const std::string& term) const {
  const auto* list = index_for(field).postings_for(term);
  if (!list) return 0;
  std::uint32_t n = 0;
  for (const Posting& posting : *list) {
    if (!docs_[posting.doc].dead) ++n;
  }
  return n;
}

std::size_t Segment::term_count() const { return title_.term_count() + body_.term_count(); }

}  // namespace indexdb
