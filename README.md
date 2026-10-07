# Inverted Index DB

A small inverted-index database in C++. You put in documents, it stores an inverted index, and `search` ranks them with BM25.

```bash
make
./indexdb --demo    # builds a tiny corpus and checks itself
./indexdb           # interactive prompt, data in ./index_data
```

Needs g++ with C++17. The binary is `indexdb`.

## What it does

- **Analyzer pipeline.** Text is tokenized, lowercased, stripped of stopwords, then passed through a light suffix stemmer (`jumps` and `jumping` both become `jump`). Indexing and queries share that pipeline. `StopwordFilter` and `StemFilter` are virtual stages, so another filter is a subclass.
- **Positional postings.** A term maps to `(doc, term frequency, positions)`. Positions make `"quick brown"` match only when those stems sit next to each other. `"fox brown"` does not match.
- **Boolean execution by merging sorted lists.** `AND` is a two-pointer intersection, `OR` a union, `-dog` a subtraction. A bare `NOT` matches nothing, because there is no match-all.
- **BM25, with stats summed across segments.** Score uses `tf`, document length, and `log(1 + (N - df + 0.5) / (df + 0.5))`. Document frequency is counted over every open segment, so a hit in an old segment and a hit in the active one are comparable. The **title field is boosted ×2**. Shorter fields rank a bit higher, which is why `City Foxes` beats `Quick Brown Fox` on the query `fox`.
- **Immutable segments and a manifest commit.** `put` fills an in-memory active segment. `flush` writes `seg-000001.seg` and only then renames `manifest` onto it. A segment counts once its name is in the manifest. Search reads every committed segment plus the active one.
- **Tombstones, dropped on merge.** `del` of a sealed document appends its id to a `.dead` sidecar and does not rewrite postings. Search skips it immediately. `merge` combines segments of similar size, where the biggest is at most twice the smallest, into one new segment and leaves a much larger segment in place. Dead documents are not copied. The new manifest names the files that remain.

## Prompt

```
indexdb> put fox "Quick Brown Fox" "The quick brown fox jumps over the lazy dog."
indexdb> flush
indexdb> search "quick brown"
indexdb> search jump
indexdb> search fox -dog
indexdb> search title:fox
indexdb> del fox
indexdb> merge
```

| Query | Meaning |
| --- | --- |
| `fox dog` | both terms (a space is AND) |
| `fox OR dog` | either term |
| `fox -dog` or `fox NOT dog` | exclude `dog` |
| `"quick brown"` | phrase, in order |
| `title:fox` / `body:"lazy dog"` | one field |
| `(fox OR dog) AND bear` | parentheses |

`title` and `body` are the only fields. `quit` seals the active segment. Anything not flushed yet lives only in memory.

## In memory until flush

`put` keeps the new document in memory. After 8 documents the batch is saved on its own. `flush` writes the batch early. Quit and start `./indexdb` again: flushed documents are still searchable. A document you never flushed is gone when the program exits. `quit` flushes on the way out.

## Layout

```
include/    analyzer, segment, query, engine
src/        the same, plus the prompt in main.cpp
index_data/ segment files, sidecars, manifest
```

A segment file is plain text (`INDEXDB 1`, stored documents, then the title index and the body index) so you can open it and see the postings.
