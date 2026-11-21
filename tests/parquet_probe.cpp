// probe: writes parquet files with pq::write, asserts round-trip shape.
// files land outside git (in argv[1] dir); decode checked by python script.
#include "parquet.hpp"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : ".";

  // nonempty: all three kinds, nulls in the nullable string, 3000 rows
  // (one page per column — small enough to never hit the flush threshold).
  {
    pq::Column cat;
    cat.name = "top_category";
    cat.kind = pq::Column::STRING;
    pq::Column day;
    day.name = "day";
    day.kind = pq::Column::DATE;
    pq::Column sessions;
    sessions.name = "sessions";
    sessions.kind = pq::Column::INT64;
    for (int64_t i = 0; i < 3000; i++) {
      if (i % 7 == 0) {
        cat.strings.push_back("");
        cat.valid.push_back(0);
      } else {
        cat.strings.push_back("cat_" + std::to_string(i % 23));
        cat.valid.push_back(1);
      }
      day.integers.push_back(19000 + i);            // days since epoch
      sessions.integers.push_back(i * 3);
    }
    pq::write(dir + "/nonempty.parquet", {cat, day, sessions});
    std::puts("ok nonempty.parquet");
  }

  // empty: schema-only file, zero rows
  {
    pq::Column u;
    u.name = "user_id";
    u.kind = pq::Column::STRING;
    u.strings = {};
    pq::write(dir + "/empty.parquet", {u});
    std::puts("ok empty.parquet");
  }

  // required-only column (no valid[]): required STRING
  {
    pq::Column u;
    u.name = "user_id";
    u.kind = pq::Column::STRING;
    u.strings = {"u1", "u2"};
    pq::Column rev;
    rev.name = "revenue_cents";
    rev.kind = pq::Column::INT64;
    rev.integers = {0, 12345};
    pq::write(dir + "/required.parquet", {u, rev});
    std::puts("ok required.parquet");
  }

  // Definition-level padding, all-null pages, and nullable schema with no mask.
  for (int count = 0; count <= 17; ++count) {
    for (int mode = 0; mode < 3; ++mode) {
      pq::Column cat{"top_category", pq::Column::STRING, {}, {}, {}};
      for (int i = 0; i < count; ++i) {
        cat.strings.push_back(i % 2 ? "" : "unicode_\xc3\xa9");
        if (mode) cat.valid.push_back(mode == 2 && i % 3 != 0);
      }
      pq::write(dir + "/null_" + std::to_string(count) + "_" + std::to_string(mode) + ".parquet", {cat});
    }
  }

  // Cross the page threshold in required and optional columns.
  {
    pq::Column value{"value", pq::Column::STRING, {}, {}, {}};
    pq::Column cat{"top_category", pq::Column::STRING, {}, {}, {}};
    for (int i = 0; i < 5001; ++i) {
      value.strings.push_back(std::string(1000, 'x') + std::to_string(i));
      cat.strings.push_back(value.strings.back());
      cat.valid.push_back(i % 7 != 0);
    }
    pq::write(dir + "/multipage.parquet", {value, cat});
  }

  // error paths
  {
    pq::Column a, b;
    a.name = "x"; a.kind = pq::Column::INT64; a.integers = {1, 2};
    b.name = "y"; b.kind = pq::Column::INT64; b.integers = {1};
    bool threw = false;
    try { pq::write(dir + "/bad.parquet", {a, b}); } catch (const std::exception&) { threw = true; }
    assert(threw);
    a.name = "";
    threw = false;
    try { pq::write(dir + "/bad.parquet", {a}); } catch (const std::exception&) { threw = true; }
    assert(threw);
    a.name = "d"; a.kind = pq::Column::DATE; a.integers = {int64_t(1) << 40};
    threw = false;
    try { pq::write(dir + "/bad.parquet", {a}); } catch (const std::exception&) { threw = true; }
    assert(threw);
    std::puts("ok error paths");
  }
  return 0;
}
