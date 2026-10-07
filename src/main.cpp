#include "engine.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string trim(const std::string& text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

std::vector<std::string> split_args(const std::string& text) {
  std::vector<std::string> args;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i >= text.size()) break;
    if (text[i] == '"') {
      ++i;
      std::string current;
      while (i < text.size() && text[i] != '"') {
        if (text[i] == '\\' && i + 1 < text.size()) {
          current.push_back(text[i + 1]);
          i += 2;
        } else {
          current.push_back(text[i++]);
        }
      }
      if (i >= text.size() || text[i] != '"') throw std::runtime_error("unterminated quote");
      ++i;
      args.push_back(std::move(current));
    } else {
      const std::size_t begin = i;
      while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
      args.push_back(text.substr(begin, i - begin));
    }
  }
  return args;
}

std::string snippet(const std::string& body) {
  std::string out;
  for (char c : body) {
    out.push_back(c == '\n' || c == '\t' ? ' ' : c);
    if (out.size() == 72) return out + "...";
  }
  return out;
}

void print_hits(const indexdb::SearchResponse& response) {
  if (response.total == 0) {
    std::cout << "no hits\n";
    return;
  }
  std::cout << response.total << (response.total == 1 ? " hit\n" : " hits\n");
  for (std::size_t i = 0; i < response.hits.size(); ++i) {
    const auto& hit = response.hits[i];
    std::cout << "  " << (i + 1) << ". " << std::fixed;
    std::cout.setf(std::ios::fixed);
    std::cout.precision(3);
    std::cout << hit.score << "  " << hit.id << "  " << hit.title << "\n     " << snippet(hit.body)
              << "\n";
  }
  if (response.hits.size() < response.total) {
    std::cout << "  ... " << (response.total - response.hits.size()) << " more\n";
  }
}

void print_help() {
  std::cout
      << "commands\n"
      << "  put <id> \"<title>\" \"<body>\"   index a document (saves a segment every "
      << indexdb::kAutoFlushDocs << ")\n"
      << "  get <id>                        print one document\n"
      << "  del <id>                        tombstone a document\n"
      << "  list                            live documents\n"
      << "  search <query>                  rank with BM25\n"
      << "  flush                           seal the active segment to disk\n"
      << "  merge                           combine similar-sized segments, drop tombstones\n"
      << "  stats\n"
      << "  help\n"
      << "  quit\n"
      << "query\n"
      << "  fox dog              both terms (AND)\n"
      << "  fox OR dog           either term\n"
      << "  fox -dog             fox, but not dog\n"
      << "  \"quick brown\"        phrase, in that order\n"
      << "  title:fox            only the title field (title counts double)\n"
      << "  (fox OR dog) AND bear\n"
      << "words are lowercased, stopwords dropped, and light-stemmed (jumps -> jump).\n";
}

void print_stats(const indexdb::Stats& stats) {
  std::cout << "sealed segments   " << stats.sealed_segments << "\n"
            << "active docs       " << stats.active_docs << "\n"
            << "live docs         " << stats.live_docs << "\n"
            << "tombstones        " << stats.dead_docs << "\n"
            << "posting lists     " << stats.posting_lists << "\n"
            << "disk              " << stats.bytes_on_disk << " bytes\n";
}

void handle(indexdb::Engine& engine, const std::string& line) {
  const std::string trimmed = trim(line);
  if (trimmed.empty()) return;
  const auto gap = trimmed.find_first_of(" \t");
  const std::string cmd = trimmed.substr(0, gap);
  const std::string rest = gap == std::string::npos ? "" : trim(trimmed.substr(gap));

  if (cmd == "help") {
    print_help();
  } else if (cmd == "put") {
    const auto args = split_args(rest);
    if (args.size() != 3) {
      throw std::runtime_error("usage: put <id> \"<title>\" \"<body>\"");
    }
    const auto stored = engine.put(args[0], args[1], args[2]);
    if (!stored.flushed.path.empty()) {
      std::cout << "ok  " << args[0] << "  (auto-flushed "
                << std::filesystem::path(stored.flushed.path).filename().string() << ", "
                << stored.flushed.docs << " docs)\n";
    } else {
      std::cout << "ok  " << args[0] << "  (" << stored.active_docs
                << " in the active segment)\n";
    }
  } else if (cmd == "del") {
    const auto args = split_args(rest);
    if (args.size() != 1) throw std::runtime_error("usage: del <id>");
    engine.remove(args[0]);
    std::cout << "tombstoned " << args[0] << "\n";
  } else if (cmd == "get") {
    const auto args = split_args(rest);
    if (args.size() != 1) throw std::runtime_error("usage: get <id>");
    const auto doc = engine.get(args[0]);
    if (!doc) {
      std::cout << "no such document\n";
      return;
    }
    std::cout << doc->id << "\n" << doc->title << "\n" << doc->body << "\n";
  } else if (cmd == "list") {
    const auto docs = engine.list();
    if (docs.empty()) {
      std::cout << "no documents\n";
      return;
    }
    for (const auto& doc : docs) std::cout << doc.id << "  " << doc.title << "\n";
  } else if (cmd == "search") {
    print_hits(engine.search(rest));
  } else if (cmd == "flush") {
    const auto info = engine.flush();
    if (info.path.empty()) std::cout << "nothing to flush\n";
    else {
      std::cout << "sealed " << std::filesystem::path(info.path).filename().string() << " ("
                << info.docs << " docs)\n";
    }
  } else if (cmd == "merge") {
    const auto info = engine.merge();
    if (info.compacted && info.path.empty()) std::cout << "merged to an empty index\n";
    else if (info.compacted) {
      std::cout << "merged into " << std::filesystem::path(info.path).filename().string() << "\n";
    } else if (info.segments == 0) std::cout << "no segments on disk\n";
    else if (info.segments == 1) std::cout << "already one segment\n";
    else std::cout << "left the larger segment alone\n";
  } else if (cmd == "stats") {
    print_stats(engine.stats());
  } else if (cmd == "quit" || cmd == "exit") {
    throw std::runtime_error("quit");
  } else {
    throw std::runtime_error("unknown command '" + cmd + "' (try help)");
  }
}

int run_repl(indexdb::Engine& engine) {
  std::cout << "inverted index db\n"
            << "data  " << engine.data_dir() << "\n"
            << "type help\n";
  std::string line;
  while (true) {
    std::cout << "indexdb> " << std::flush;
    if (!std::getline(std::cin, line)) break;
    if (trim(line) == "quit" || trim(line) == "exit") break;
    try {
      handle(engine, line);
    } catch (const std::exception& ex) {
      std::cout << "error: " << ex.what() << "\n";
    }
  }
  const auto info = engine.flush();
  if (!info.path.empty()) {
    std::cout << "sealed " << std::filesystem::path(info.path).filename().string() << " ("
              << info.docs << " docs)\n";
  }
  return 0;
}

std::set<std::string> ids_of(const indexdb::SearchResponse& response) {
  std::set<std::string> ids;
  for (const auto& hit : response.hits) ids.insert(hit.id);
  return ids;
}

std::string join_ids(const indexdb::SearchResponse& response) {
  std::string out;
  for (std::size_t i = 0; i < response.hits.size(); ++i) {
    if (i) out += ", ";
    out += response.hits[i].id;
  }
  if (response.hits.size() < response.total) out += ", ...";
  if (out.empty()) out = "(none)";
  return out;
}

int run_demo() {
  namespace fs = std::filesystem;
  const fs::path dir = "index_data_demo";
  fs::remove_all(dir);

  int failed = 0;
  auto expect = [&](bool ok, const std::string& name) {
    std::cout << (ok ? "ok   " : "FAIL ") << name << "\n";
    if (!ok) ++failed;
  };
  auto expect_ids = [&](const indexdb::SearchResponse& response, std::set<std::string> want,
                        const std::string& name) {
    const bool ok = ids_of(response) == want && response.total == want.size();
    expect(ok, name + (ok ? "" : "  got " + join_ids(response)));
  };

  std::cout << "inverted index db demo\n\n";
  {
    indexdb::Engine engine(dir.string());
    const struct {
      const char* id;
      const char* title;
      const char* body;
    } docs[] = {
        {"fox", "Quick Brown Fox",
         "The quick brown fox jumps over the lazy dog near the river."},
        {"dog", "Lazy Dog", "A lazy dog sleeps in the sun after chasing the fox."},
        {"city", "City Foxes", "Urban foxes hunt at night in the city and avoid dogs."},
        {"index", "Inverted Index",
         "An inverted index maps each term to the documents that contain it."},
        {"segments", "Search Segments",
         "Search engines flush immutable segments and merge them later."},
        {"bear", "Brown Bears", "The brown bear fishes in the river and ignores the fox."},
    };
    for (std::size_t i = 0; i < 6; ++i) {
      engine.put(docs[i].id, docs[i].title, docs[i].body);
      if (i == 2) {
        const auto flushed = engine.flush();
        std::cout << "flushed " << fs::path(flushed.path).filename().string() << " ("
                  << flushed.docs << " docs)\n";
      }
    }

    const auto mid = engine.stats();
    std::cout << "open segments: " << mid.sealed_segments << " sealed + " << mid.active_docs
              << " active docs\n\n";
    expect(mid.sealed_segments == 1 && mid.active_docs == 3, "flush left one sealed segment and three active docs");

    auto show = [&](const std::string& query) {
      std::cout << "search  " << query << "\n";
      const auto response = engine.search(query, 20);
      print_hits(response);
      std::cout << "\n";
      return response;
    };

    const auto fox = show("fox");
    expect(fox.total >= 2 && (fox.hits[0].id == "fox" || fox.hits[0].id == "city"),
           "a title hit ranks first for fox");
    double best_title = 0;
    double best_body = 0;
    for (const auto& hit : fox.hits) {
      if (hit.id == "fox" || hit.id == "city") best_title = std::max(best_title, hit.score);
      if (hit.id == "dog" || hit.id == "bear") best_body = std::max(best_body, hit.score);
    }
    expect(best_title > best_body, "title matches outrank body-only matches");

    expect_ids(show("\"quick brown\""), {"fox"}, "phrase quick brown");
    expect_ids(show("\"fox brown\""), {}, "reversed phrase misses");
    expect_ids(show("\"search segments\""), {"segments"}, "phrase uses positions, not a bag of words");
    expect_ids(show("jump"), {"fox"}, "stemming: jumps matches jump");
    expect_ids(show("engine"), {"segments"}, "stemming: engines matches engine");
    expect_ids(show("fish"), {"bear"}, "stemming: fishes matches fish");
    expect_ids(show("fox -dog"), {"bear"}, "NOT drops every doc that contains dog");
    expect_ids(show("title:fox"), {"city", "fox"}, "field query stays inside title");
    expect_ids(show("(fox OR dog) AND bear"), {"bear"}, "parentheses and OR");
    expect_ids(show("title:segments"), {"segments"}, "query reaches the unflushed segment");

    bool rejected = false;
    try {
      engine.put("fox", "again", "nope");
    } catch (const std::exception&) {
      rejected = true;
    }
    expect(rejected, "duplicate id is rejected");

    bool empty_not = false;
    try {
      auto response = engine.search("the");
      empty_not = false;
      (void)response;
    } catch (const std::runtime_error& ex) {
      empty_not = std::string(ex.what()).find("no searchable") != std::string::npos;
    }
    expect(empty_not, "a stopword-only query is refused");

    expect_ids(engine.search("NOT fox", 20), {}, "a bare NOT matches nothing");

    engine.remove("dog");
    expect(fs::is_regular_file(dir / "seg-000001.seg.dead"),
           "sealed delete appends a tombstone sidecar");
    expect(ids_of(engine.search("fox", 20)).count("dog") == 0,
           "sidecar tombstone hides dog while its postings are still on disk");
    expect(engine.stats().dead_docs == 1, "the tombstone is counted until merge");

    engine.remove("bear");
    expect_ids(engine.search("fish", 20), {}, "active-segment delete hides bear immediately");
    expect_ids(engine.search("fox -dog", 20), {}, "deleted bear is gone from NOT fox dog");

    const auto merged = engine.merge();
    std::cout << "merged into " << fs::path(merged.path).filename().string() << "\n";
    const auto after = engine.stats();
    bool sidecar_left = false;
    for (const auto& entry : fs::directory_iterator(dir)) {
      if (entry.path().filename().string().find(".dead") != std::string::npos) sidecar_left = true;
    }
    expect(merged.compacted && after.sealed_segments == 1 && after.dead_docs == 0 &&
               after.live_docs == 4 && after.active_docs == 0 && !sidecar_left,
           "merge rewrites live docs into one segment and drops tombstones");
    expect_ids(engine.search("jump", 20), {"fox"}, "jump still hits after merge");
    expect(engine.get("segments").has_value(), "get reads the stored document");
    expect(!engine.get("bear").has_value() && !engine.get("dog").has_value(),
           "get misses documents dropped by merge");
  }

  {
    indexdb::Engine again(dir.string());
    const auto stats = again.stats();
    expect(stats.sealed_segments == 1 && stats.live_docs == 4 && stats.dead_docs == 0,
           "reopen reads the manifest and the segment file");
    expect_ids(again.search("immutable", 20), {"segments"}, "a reloaded segment still searches");
    std::cout << "\n";
    print_stats(stats);
  }

  {
    indexdb::Engine batch((dir / "batch").string());
    for (std::size_t i = 0; i < indexdb::kAutoFlushDocs; ++i) {
      batch.put("d" + std::to_string(i), "Title " + std::to_string(i), "body fox number");
    }
    const auto stats = batch.stats();
    expect(stats.sealed_segments == 1 && stats.active_docs == 0 &&
               stats.live_docs == indexdb::kAutoFlushDocs,
           "the 8th put writes a segment without an extra flush");
    expect_ids(batch.search("fox", 20),
               {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"},
               "a search still sees the auto-flushed documents");
  }

  {
    indexdb::Engine tiers((dir / "tiers").string());
    tiers.put("a", "Small A", "alpha document");
    tiers.put("b", "Small B", "alpha document");
    tiers.flush();
    tiers.put("c", "Small C", "alpha document");
    tiers.put("d", "Small D", "alpha document");
    tiers.flush();
    for (int i = 0; i < 8; ++i) {
      tiers.put("big" + std::to_string(i), "Big", "beta document " + std::to_string(i));
    }
    const auto before = tiers.stats();
    const auto merged = tiers.merge();
    const auto after = tiers.stats();
    expect(before.sealed_segments == 3 && merged.compacted && after.sealed_segments == 2 &&
               after.live_docs == 12 && after.dead_docs == 0,
           "merge combines the two small segments and leaves the big one");
    expect(tiers.get("a").has_value() && tiers.get("big0").has_value(),
           "documents from both the merged file and the large file are still there");
  }

  {
    const std::string crash_dir = (dir / "translog").string();
    {
      indexdb::Engine first(crash_dir);
      first.put("keep", "Keep", "still here after a crash");
      first.put("drop", "Drop", "deleted before the crash");
      first.remove("drop");
    }
    indexdb::Engine second(crash_dir);
    const auto stats = second.stats();
    expect(second.get("keep").has_value() && !second.get("drop").has_value() &&
               stats.sealed_segments == 0 && stats.active_docs == 1,
           "a restart replays the translog and keeps an unflushed document");
  }

  std::cout << "\n" << (failed == 0 ? "all checks passed\n" : "checks failed\n");
  return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string data = "index_data";
  bool demo = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--demo") {
      demo = true;
    } else if (arg == "--data" && i + 1 < argc) {
      data = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "usage: indexdb [--data DIR] [--demo]\n";
      return 0;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return 2;
    }
  }
  if (demo) return run_demo();
  try {
    indexdb::Engine engine(data);
    return run_repl(engine);
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 1;
  }
}
