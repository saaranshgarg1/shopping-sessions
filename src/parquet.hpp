// parquet.hpp — dependency-free Parquet writer, single header, C++20.
//
// Writes uncompressed Parquet: PLAIN-encoded DataPage v1, Thrift compact footer,
// one row group per call. Flat schema only (no nesting, no dictionary, no
// compression, no statistics). Upgrades to add if ever needed: compression,
// dictionary pages, statistics.
//
// API:
//   namespace pq {
//     struct Column {
//       std::string name;
//       enum Kind { STRING, INT64, DATE };
//       Kind kind = STRING;
//       std::vector<std::string> strings;   // STRING: UTF-8 values
//       std::vector<int64_t>     integers;  // INT64 (physical INT64);
//                                           // DATE (physical INT32, days since epoch)
//       std::vector<uint8_t>     valid;     // null mask: empty = all present
//                                           // (top_category remains optional),
//                                           // else size must equal
//                                           // the value count, 1 = present, 0 = null
//       std::vector<std::string_view> string_views; // Alternative STRING input; bytes live through write().
//     };
//     void write(const std::string& path, const std::vector<Column>& columns);
//   }
//
// write() always creates the file, including for a 0-row table (schema-only
// footer, no row groups). Values are streamed to disk page by page (~4 MiB
// flush), so peak extra memory is one page. Throws std::runtime_error on
// invalid input (mismatched lengths, empty name, date outside int32 days).

#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pq {

struct Column {
  std::string name;
  enum Kind { STRING, INT64, DATE };
  Kind kind = STRING;
  std::vector<std::string> strings;
  std::vector<int64_t> integers;
  std::vector<uint8_t> valid;
  std::vector<std::string_view> string_views; // Alternative to strings; backing bytes must live through write().
};

namespace detail {

// ---- Thrift compact protocol writer ---------------------------------------
enum : uint8_t {
  T_STOP = 0, T_TRUE = 1, T_FALSE = 2, T_BYTE = 3, T_I16 = 4, T_I32 = 5,
  T_I64 = 6, T_DOUBLE = 7, T_BINARY = 8, T_LIST = 9, T_STRUCT = 12
};

struct TCompact {
  std::vector<uint8_t> b;
  int last = 0;
  std::vector<int> stack;

  void varint(uint64_t v) {
    while (v >= 0x80) { b.push_back(uint8_t(v) | 0x80); v >>= 7; }
    b.push_back(uint8_t(v));
  }
  void zig32(int32_t v) {
    varint(uint64_t(uint32_t(v) << 1) ^ uint32_t(v >> 31));
  }
  void zig64(int64_t v) {
    varint((uint64_t(v) << 1) ^ uint64_t(v >> 63));
  }
  void fh(int id, uint8_t t) {
    b.push_back(uint8_t((id - last) << 4) | t);
    last = id;
  }
  void i32(int id, int32_t v) { fh(id, T_I32); zig32(v); }
  void i64(int id, int64_t v) { fh(id, T_I64); zig64(v); }
  void str(int id, const std::string& s) {
    fh(id, T_BINARY);
    varint(s.size());
    b.insert(b.end(), s.begin(), s.end());
  }
  // Struct-typed field: field header, then fields, then send().
  void sb(int id) { fh(id, T_STRUCT); stack.push_back(last); last = 0; }
  // Anonymous struct (list element): no field header.
  void sbegin() { stack.push_back(last); last = 0; }
  void send() { b.push_back(T_STOP); last = stack.back(); stack.pop_back(); }
  void lbegin(size_t n, uint8_t et) {
    if (n < 15) b.push_back(uint8_t(uint8_t(n) << 4) | et);
    else { b.push_back(0xF0 | et); varint(n); }
  }
  // List-typed field.
  void ls(int id, size_t n, uint8_t et) { fh(id, T_LIST); lbegin(n, et); }
};

// parquet enums
constexpr int32_t TYPE_INT32 = 1, TYPE_INT64 = 2, TYPE_BYTE_ARRAY = 6;
constexpr int32_t REP_REQUIRED = 0, REP_OPTIONAL = 1;
constexpr int32_t CT_UTF8 = 0, CT_DATE = 6;
constexpr int32_t ENC_PLAIN = 0, ENC_RLE = 3;
constexpr int32_t CODEC_UNCOMPRESSED = 0;
constexpr int32_t PTYPE_DATA = 0;

inline void put_u32(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(uint8_t(v));
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v >> 16));
  b.push_back(uint8_t(v >> 24));
}

inline void put_u64(std::vector<uint8_t>& b, uint64_t v) {
  for (int i = 0; i < 8; i++) b.push_back(uint8_t(v >> (8 * i)));
}

}  // namespace detail

inline void write(const std::string& path, const std::vector<Column>& cols) {
  using namespace detail;
  if (cols.empty()) throw std::runtime_error("pq::write: no columns");
  auto rows_of = [](const Column& c) -> size_t {
    return c.kind == Column::STRING ? (c.string_views.empty() ? c.strings.size() : c.string_views.size()) : c.integers.size();
  };
  auto is_optional = [](const Column& c) {
    // Preserve the declared nullable output schema even with no rows/null mask.
    return !c.valid.empty() || c.name == "top_category";
  };
  const size_t n = rows_of(cols[0]);
  for (const Column& c : cols) {
    if (!c.string_views.empty() && (!c.strings.empty() || c.kind != Column::STRING))
      throw std::runtime_error("pq::write: column '" + c.name + "' has conflicting string inputs");
    if (c.name.empty())
      throw std::runtime_error("pq::write: empty column name");
    if (rows_of(c) != n)
      throw std::runtime_error("pq::write: column '" + c.name + "' length mismatch");
    if (!c.valid.empty() && c.valid.size() != n)
      throw std::runtime_error("pq::write: column '" + c.name + "' valid[] length mismatch");
  }

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) throw std::runtime_error("pq::write: cannot open '" + path + "'");
  f.write("PAR1", 4);

  struct Chunk {
    int32_t type;
    bool optional;
    const std::string* name;
    int64_t num_values, offset, size;
  };
  std::vector<Chunk> chunks;
  int64_t rg_size = 0;

  for (const Column& c : cols) {
    const bool optional = is_optional(c);
    Chunk ch{c.kind == Column::STRING ? TYPE_BYTE_ARRAY
             : c.kind == Column::INT64 ? TYPE_INT64 : TYPE_INT32,
             optional, &c.name, 0, 0, 0};
    ch.offset = int64_t(f.tellp());

    std::vector<uint8_t> vals;  // PLAIN values of the current page
    std::vector<uint8_t> defs;  // bit-packed def levels of the current page
    size_t page_rows = 0;
    uint8_t db = 0;
    int dbits = 0;

    auto flush_page = [&]() {
      std::vector<uint8_t> data;
      if (optional) {
        if (dbits > 0) { defs.push_back(db); db = 0; dbits = 0; }
        // Definition-level bit width comes from schema, not a payload prefix.
        // shortcut: one bit-packed run per page; split if pages exceed int32 slots.
        std::vector<uint8_t> blob;
        TCompact h;
        h.varint((uint64_t(defs.size()) << 1) | 1);
        blob.insert(blob.end(), h.b.begin(), h.b.end());
        blob.insert(blob.end(), defs.begin(), defs.end());
        put_u32(data, uint32_t(blob.size()));
        data.insert(data.end(), blob.begin(), blob.end());
        defs.clear();
      }
      // Write level bytes and PLAIN values separately, avoiding a full page copy.
      const size_t data_size = data.size() + vals.size();
      TCompact ph;
      ph.i32(1, PTYPE_DATA);
      ph.i32(2, int32_t(data_size));  // uncompressed_page_size
      ph.i32(3, int32_t(data_size));  // compressed_page_size (no compression)
      ph.sb(5);                         // DataPageHeader (v1)
      ph.i32(1, int32_t(page_rows));    // num_values, slots including nulls
      ph.i32(2, ENC_PLAIN);
      ph.i32(3, ENC_RLE);               // def levels (none when required)
      ph.i32(4, ENC_RLE);               // rep levels: none written (max_rep=0)
      ph.send();                        // close DataPageHeader
      ph.b.push_back(T_STOP);           // close PageHeader (top level)
      f.write(reinterpret_cast<const char*>(ph.b.data()),
              std::streamsize(ph.b.size()));
      if (!data.empty()) f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
      if (!vals.empty()) f.write(reinterpret_cast<const char*>(vals.data()), std::streamsize(vals.size()));
      vals.clear();
      ch.num_values += int64_t(page_rows);
      const int64_t sz = int64_t(ph.b.size() + data_size);
      ch.size += sz;
      rg_size += sz;
      page_rows = 0;
    };

    // Required INT64: PLAIN values are exactly the native little-endian bytes,
    // so copy whole pages instead of encoding one byte at a time.
    if (c.kind == Column::INT64 && !optional && std::endian::native == std::endian::little) {
      constexpr size_t kPageValues = (4u << 20) / sizeof(int64_t);
      for (size_t i = 0; i < n; i += kPageValues) {
        const size_t cnt = (n - i < kPageValues) ? n - i : kPageValues;
        vals.resize(cnt * sizeof(int64_t));
        std::memcpy(vals.data(), c.integers.data() + i, cnt * sizeof(int64_t));
        page_rows = cnt;
        flush_page();
      }
    } else {
      for (size_t i = 0; i < n; i++) {
        const bool present = c.valid.empty() || c.valid[i];
        if (optional) {
          db |= uint8_t(uint8_t(present) << dbits);
          if (++dbits == 8) { defs.push_back(db); db = 0; dbits = 0; }
        }
        if (present) {
          switch (c.kind) {
          case Column::STRING: {
            const std::string_view s = c.string_views.empty() ? std::string_view(c.strings[i]) : c.string_views[i];
            if (s.size() > UINT32_MAX) throw std::runtime_error("pq::write: string exceeds uint32 length");
            put_u32(vals, uint32_t(s.size()));
            if (!s.empty()) vals.insert(vals.end(), s.begin(), s.end());
            break;
          }
          case Column::INT64: {
            put_u64(vals, uint64_t(c.integers[i]));
            break;
          }
          case Column::DATE: {
            const int64_t v = c.integers[i];
            if (v < INT32_MIN || v > INT32_MAX)
              throw std::runtime_error("pq::write: date out of range in column '" + c.name + "'");
            put_u32(vals, uint32_t(int32_t(v)));
            break;
          }
          }
        }
        page_rows++;
        if (vals.size() >= (4u << 20)) flush_page();
      }
      if (page_rows > 0) flush_page();
    }
    if (n > 0) chunks.push_back(ch);
  }

  // ---- footer: FileMetaData ----
  TCompact m;
  m.i32(1, 1);  // version
  m.ls(2, cols.size() + 1, T_STRUCT);  // schema
  m.sbegin();  // root
  m.str(4, "schema");
  m.i32(5, int32_t(cols.size()));
  m.send();
  for (const Column& c : cols) {
    m.sbegin();
    m.i32(1, c.kind == Column::STRING ? TYPE_BYTE_ARRAY
             : c.kind == Column::INT64 ? TYPE_INT64 : TYPE_INT32);
    m.i32(3, is_optional(c) ? REP_OPTIONAL : REP_REQUIRED);
    m.str(4, c.name);
    if (c.kind == Column::STRING) {
      m.i32(6, CT_UTF8);
      m.sb(10);  // LogicalType
      m.sb(1);   // STRING
      m.send();
      m.send();
    } else if (c.kind == Column::DATE) {
      m.i32(6, CT_DATE);
      m.sb(10);  // LogicalType
      m.sb(6);   // DATE
      m.send();
      m.send();
    }
    m.send();
  }
  m.i64(3, int64_t(n));  // num_rows
  m.ls(4, n > 0 ? 1 : 0, T_STRUCT);  // required row_groups: list<RowGroup>
  if (n > 0) {
    m.sbegin();  // RowGroup element
    m.ls(1, chunks.size(), T_STRUCT);  // columns
    for (const Chunk& ch : chunks) {
      m.sbegin();  // ColumnChunk
      m.i64(2, ch.offset);  // file_offset
      m.sb(3);  // meta_data
      m.i32(1, ch.type);
      m.ls(2, ch.optional ? 2 : 1, T_I32);  // encodings
      m.zig32(ENC_PLAIN);
      if (ch.optional) m.zig32(ENC_RLE);
      m.ls(3, 1, T_BINARY);  // path_in_schema
      m.varint(ch.name->size());
      m.b.insert(m.b.end(), ch.name->begin(), ch.name->end());
      m.i32(4, CODEC_UNCOMPRESSED);
      m.i64(5, ch.num_values);
      m.i64(6, ch.size);  // total_uncompressed_size
      m.i64(7, ch.size);  // total_compressed_size
      m.i64(9, ch.offset);  // data_page_offset
      m.send();  // ColumnMetaData
      m.send();  // ColumnChunk
    }
    m.i64(2, rg_size);  // total_byte_size
    m.i64(3, int64_t(n));  // num_rows
    m.send();  // RowGroup element
  }
  m.b.push_back(T_STOP);  // close FileMetaData

  const uint32_t mlen = uint32_t(m.b.size());
  f.write(reinterpret_cast<const char*>(m.b.data()), std::streamsize(m.b.size()));
  std::vector<uint8_t> tail;
  put_u32(tail, mlen);
  tail.push_back('P'); tail.push_back('A'); tail.push_back('R'); tail.push_back('1');
  f.write(reinterpret_cast<const char*>(tail.data()), std::streamsize(tail.size()));
  f.close();
  if (!f) throw std::runtime_error("pq::write: failed writing '" + path + "'");
}

}  // namespace pq
