#include "engine.hpp"

#include <algorithm>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <unordered_set>
#include <utility>

namespace indexdb {
namespace {

namespace fs = std::filesystem;

bool ends_with(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string basename_of(const std::string& path) { return fs::path(path).filename().string(); }

int segment_number(const std::string& path) {
  const std::string name = basename_of(path);
  if (name.rfind("seg-", 0) != 0) return 0;
  try {
    return std::stoi(name.substr(4, 6));
  } catch (const std::exception&) {
    return 0;
  }
}

void apply_sidecar(Segment& segment, const std::string& dead_path) {
  std::ifstream in(dead_path);
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    segment.kill(unescape_text(line));
  }
}

std::uintmax_t file_bytes(const fs::path& path) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec)) return 0;
  return fs::file_size(path, ec);
}

}  // namespace

Engine::Engine(std::string data_dir) : dir_(std::move(data_dir)) {
  fs::create_directories(dir_);
  load_committed();
  remove_unreferenced();
  replay_translog();
}

void Engine::add_active(std::string id, std::string title, std::string body) {
  const auto title_tokens = analyzer_.analyze(title);
  const auto body_tokens = analyzer_.analyze(body);
  active_.add(std::move(id), std::move(title), std::move(body), title_tokens, body_tokens);
}

PutResult Engine::put(std::string id, std::string title, std::string body) {
  if (id.empty()) throw std::runtime_error("document id is empty");
  if (id.find('\n') != std::string::npos || id.find('\r') != std::string::npos) {
    throw std::runtime_error("document id cannot contain a newline");
  }
  if (find_segment(id) != nullptr) throw std::runtime_error("document already indexed: " + id);
  append_translog("PUT\n" + escape_text(id) + "\n" + escape_text(title) + "\n" + escape_text(body) +
                  "\n");
  add_active(std::move(id), std::move(title), std::move(body));
  PutResult result;
  result.active_docs = active_.live_count();
  if (result.active_docs >= kAutoFlushDocs) {
    result.flushed = flush();
    result.active_docs = active_.live_count();
  }
  return result;
}

void Engine::remove(const std::string& id) {
  std::size_t sealed_index = 0;
  Segment* segment = find_segment(id, &sealed_index);
  if (segment == nullptr) throw std::runtime_error("no such document: " + id);
  if (segment == &active_) {
    append_translog("DEL\n" + escape_text(id) + "\n");
    segment->kill(id);
    return;
  }
  segment->kill(id);
  std::ofstream out(sealed_[sealed_index].path + ".dead", std::ios::app);
  if (!out) throw std::runtime_error("cannot write tombstone for " + id);
  out << escape_text(id) << '\n';
  if (!out) throw std::runtime_error("cannot write tombstone for " + id);
}

std::optional<StoredDoc> Engine::get(const std::string& id) const {
  const Segment* segment = find_segment(id);
  if (segment == nullptr) return std::nullopt;
  const StoredDoc* doc = segment->find_live(id);
  if (doc == nullptr) return std::nullopt;
  return *doc;
}

std::vector<StoredDoc> Engine::list() const {
  std::vector<StoredDoc> docs;
  for (const Segment* segment : snapshot()) {
    for (const auto& doc : segment->docs()) {
      if (!doc.dead) docs.push_back(doc);
    }
  }
  return docs;
}

SearchResponse Engine::search(const std::string& query, std::size_t limit) const {
  QueryParser parser(analyzer_);
  const Node root = parser.parse(query);
  const auto segments = snapshot();

  Similarity sim;
  std::uint64_t title_tokens = 0;
  std::uint64_t body_tokens = 0;
  for (const Segment* segment : segments) {
    sim.documents += static_cast<std::uint32_t>(segment->live_count());
    title_tokens += segment->live_tokens("title");
    body_tokens += segment->live_tokens("body");
  }
  if (sim.documents > 0) {
    sim.avg_title = static_cast<double>(title_tokens) / static_cast<double>(sim.documents);
    sim.avg_body = static_cast<double>(body_tokens) / static_cast<double>(sim.documents);
  }

  std::vector<std::pair<std::string, std::string>> terms;
  collect_query_terms(root, terms);
  std::sort(terms.begin(), terms.end());
  terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
  for (const auto& term : terms) {
    const std::vector<std::string> fields =
        term.first.empty() ? std::vector<std::string>{"title", "body"}
                           : std::vector<std::string>{term.first};
    for (const auto& field : fields) {
      std::uint32_t df = 0;
      for (const Segment* segment : segments) df += segment->live_df(field, term.second);
      sim.set_df(field, term.second, df);
    }
  }

  auto collect = [&](const Segment* segment) {
    std::vector<SearchHit> local;
    for (const ScoredDoc& scored : execute(*segment, root, sim)) {
      const StoredDoc& doc = segment->docs()[scored.doc];
      local.push_back(SearchHit{doc.id, doc.title, doc.body, scored.score});
    }
    return local;
  };

  std::vector<SearchHit> hits;
  if (segments.size() <= 1) {
    for (const Segment* segment : segments) {
      auto local = collect(segment);
      hits.insert(hits.end(), local.begin(), local.end());
    }
  } else {
    std::vector<std::future<std::vector<SearchHit>>> jobs;
    jobs.reserve(segments.size());
    for (const Segment* segment : segments) {
      jobs.push_back(std::async(std::launch::async, collect, segment));
    }
    for (auto& job : jobs) {
      auto local = job.get();
      hits.insert(hits.end(), std::make_move_iterator(local.begin()),
                  std::make_move_iterator(local.end()));
    }
  }
  std::sort(hits.begin(), hits.end(), [](const SearchHit& a, const SearchHit& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.id < b.id;
  });

  SearchResponse response;
  response.total = hits.size();
  if (hits.size() > limit) hits.resize(limit);
  response.hits = std::move(hits);
  return response;
}

FlushInfo Engine::flush() {
  FlushInfo info;
  if (active_.docs().empty()) return info;
  Segment cleaned = Segment::merge_all({&active_});
  active_ = Segment{};
  if (cleaned.docs().empty()) {
    clear_translog();
    return info;
  }
  info.docs = cleaned.docs().size();
  info.path = new_segment_path();
  cleaned.save(info.path);
  sealed_.push_back(Sealed{info.path, std::move(cleaned)});
  std::vector<std::string> names;
  names.reserve(sealed_.size());
  for (const auto& sealed : sealed_) names.push_back(basename_of(sealed.path));
  publish(names);
  clear_translog();
  return info;
}

std::vector<std::size_t> Engine::pick_merge_group() const {
  struct Item {
    std::size_t index = 0;
    std::size_t live = 0;
    bool dead = false;
  };
  std::vector<Item> items;
  items.reserve(sealed_.size());
  for (std::size_t i = 0; i < sealed_.size(); ++i) {
    items.push_back(Item{i, sealed_[i].segment.live_count(), sealed_[i].segment.has_dead()});
  }
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    if (a.live != b.live) return a.live < b.live;
    return a.index < b.index;
  });

  std::vector<std::size_t> tier;
  const std::size_t smallest = items.front().live;
  const std::size_t limit = smallest > std::numeric_limits<std::size_t>::max() / kMergeSizeFactor
                                ? std::numeric_limits<std::size_t>::max()
                                : smallest * kMergeSizeFactor;
  for (const Item& item : items) {
    if (item.live > limit) break;
    tier.push_back(item.index);
  }
  if (tier.size() >= 2) return tier;

  for (const Item& item : items) {
    if (item.dead) return {item.index};
  }
  return {};
}

MergeInfo Engine::merge() {
  flush();
  MergeInfo info;
  info.segments = sealed_.size();
  if (sealed_.empty()) return info;

  const std::vector<std::size_t> group = pick_merge_group();
  if (group.empty()) {
    if (sealed_.size() == 1) info.path = sealed_[0].path;
    return info;
  }

  std::unordered_set<std::size_t> chosen(group.begin(), group.end());
  std::vector<const Segment*> parts;
  parts.reserve(group.size());
  for (std::size_t index : group) parts.push_back(&sealed_[index].segment);

  Segment merged = Segment::merge_all(parts);
  info.compacted = true;
  if (!merged.docs().empty()) {
    info.path = new_segment_path();
    merged.save(info.path);
  }
  std::vector<Sealed> kept;
  kept.reserve(sealed_.size() - group.size() + 1);
  for (std::size_t i = 0; i < sealed_.size(); ++i) {
    if (!chosen.count(i)) kept.push_back(std::move(sealed_[i]));
  }
  if (!info.path.empty()) kept.push_back(Sealed{info.path, std::move(merged)});
  sealed_ = std::move(kept);
  std::vector<std::string> names;
  names.reserve(sealed_.size());
  for (const auto& sealed : sealed_) names.push_back(basename_of(sealed.path));
  publish(names);
  remove_unreferenced();
  info.segments = sealed_.size();
  return info;
}

Stats Engine::stats() const {
  Stats stats;
  stats.sealed_segments = sealed_.size();
  stats.active_docs = active_.live_count();
  stats.bytes_on_disk = file_bytes(fs::path(dir_) / "manifest");
  stats.bytes_on_disk += file_bytes(fs::path(dir_) / "translog");
  auto take = [&](const Segment& segment, bool on_disk, const std::string& path) {
    stats.live_docs += segment.live_count();
    stats.dead_docs += segment.docs().size() - segment.live_count();
    stats.posting_lists += segment.term_count();
    if (!on_disk) return;
    stats.bytes_on_disk += file_bytes(path);
    stats.bytes_on_disk += file_bytes(path + ".dead");
  };
  for (const auto& sealed : sealed_) take(sealed.segment, true, sealed.path);
  take(active_, false, "");
  return stats;
}

Segment* Engine::find_segment(const std::string& id, std::size_t* sealed_index) {
  if (active_.find_live(id) != nullptr) {
    if (sealed_index != nullptr) *sealed_index = static_cast<std::size_t>(-1);
    return &active_;
  }
  for (std::size_t i = sealed_.size(); i-- > 0;) {
    if (sealed_[i].segment.find_live(id) != nullptr) {
      if (sealed_index != nullptr) *sealed_index = i;
      return &sealed_[i].segment;
    }
  }
  return nullptr;
}

const Segment* Engine::find_segment(const std::string& id) const {
  if (active_.find_live(id) != nullptr) return &active_;
  for (std::size_t i = sealed_.size(); i-- > 0;) {
    if (sealed_[i].segment.find_live(id) != nullptr) return &sealed_[i].segment;
  }
  return nullptr;
}

std::vector<const Segment*> Engine::snapshot() const {
  std::vector<const Segment*> segments;
  segments.reserve(sealed_.size() + 1);
  for (const auto& sealed : sealed_) segments.push_back(&sealed.segment);
  if (!active_.docs().empty()) segments.push_back(&active_);
  return segments;
}

std::string Engine::new_segment_path() const {
  int max_id = 0;
  for (const auto& sealed : sealed_) max_id = std::max(max_id, segment_number(sealed.path));
  std::ostringstream name;
  name << "seg-" << std::setw(6) << std::setfill('0') << (max_id + 1) << ".seg";
  return (fs::path(dir_) / name.str()).string();
}

void Engine::load_committed() {
  const fs::path manifest = fs::path(dir_) / "manifest";
  if (!fs::is_regular_file(manifest)) return;
  std::ifstream in(manifest);
  if (!in) throw std::runtime_error("cannot read manifest in " + dir_);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const fs::path path = fs::path(dir_) / line;
    if (!fs::is_regular_file(path)) {
      throw std::runtime_error("manifest names a missing segment: " + line);
    }
    Segment segment = Segment::load(path.string());
    apply_sidecar(segment, path.string() + ".dead");
    sealed_.push_back(Sealed{path.string(), std::move(segment)});
  }
}

void Engine::publish(const std::vector<std::string>& basenames) const {
  const fs::path tmp = fs::path(dir_) / "manifest.tmp";
  const fs::path final = fs::path(dir_) / "manifest";
  {
    std::ofstream out(tmp);
    if (!out) throw std::runtime_error("cannot write manifest in " + dir_);
    for (const auto& name : basenames) out << name << '\n';
    out.flush();
    if (!out) throw std::runtime_error("cannot write manifest in " + dir_);
  }
  fs::rename(tmp, final);
}

void Engine::append_translog(const std::string& record) {
  const std::string path = (fs::path(dir_) / "translog").string();
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) throw std::runtime_error("cannot write translog in " + dir_);
  std::size_t written = 0;
  while (written < record.size()) {
    const ssize_t n = ::write(fd, record.data() + written, record.size() - written);
    if (n < 0) {
      ::close(fd);
      throw std::runtime_error("cannot write translog in " + dir_);
    }
    written += static_cast<std::size_t>(n);
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    throw std::runtime_error("cannot sync translog in " + dir_);
  }
  ::close(fd);
}

void Engine::clear_translog() const {
  std::error_code ec;
  fs::remove(fs::path(dir_) / "translog", ec);
}

void Engine::replay_translog() {
  std::ifstream in(fs::path(dir_) / "translog");
  if (!in) return;
  auto read_line = [&](std::string& line) {
    if (!std::getline(in, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
  };
  std::string kind;
  while (read_line(kind)) {
    if (kind.empty()) continue;
    if (kind == "PUT") {
      std::string id;
      std::string title;
      std::string body;
      if (!read_line(id) || !read_line(title) || !read_line(body)) break;
      id = unescape_text(id);
      if (find_segment(id) != nullptr) continue;
      add_active(std::move(id), unescape_text(title), unescape_text(body));
    } else if (kind == "DEL") {
      std::string id;
      if (!read_line(id)) break;
      id = unescape_text(id);
      if (active_.find_live(id) != nullptr) active_.kill(id);
    } else {
      break;
    }
  }
}

void Engine::remove_unreferenced() const {
  std::unordered_set<std::string> keep;
  for (const auto& sealed : sealed_) keep.insert(basename_of(sealed.path));
  for (const auto& entry : fs::directory_iterator(dir_)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (ends_with(name, ".seg.dead")) {
      const std::string segment_name = name.substr(0, name.size() - 5);  // drop ".dead"
      if (!keep.count(segment_name)) fs::remove(entry.path());
    } else if (ends_with(name, ".seg")) {
      if (!keep.count(name)) fs::remove(entry.path());
    } else if (name == "manifest.tmp") {
      fs::remove(entry.path());
    }
  }
}

}  // namespace indexdb
