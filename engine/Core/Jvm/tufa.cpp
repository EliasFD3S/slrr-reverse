#include "tufa.hpp"

#include "tree_op_flags.inc"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <variant>
#include <vector>

namespace inv {
namespace {

using ConstEntry = std::variant<std::string, int32_t, std::array<uint32_t, 3>>;
// array = {kind, a, b} kind: 5=mref 6=fref 7=nat

struct ConstPool {
  std::vector<ConstEntry> entries;

  const ConstEntry* get0(uint32_t i) const {
    if (i >= entries.size()) return nullptr;
    return &entries[i];
  }

  std::string get_str(uint32_t i) const {
    const ConstEntry* e = get0(i);
    if (!e) return {};
    if (auto* s = std::get_if<std::string>(e)) return *s;
    if (auto* n = std::get_if<int32_t>(e)) {
      const ConstEntry* t = get0(static_cast<uint32_t>(*n));
      if (t) {
        if (auto* s2 = std::get_if<std::string>(t)) return *s2;
      }
    }
    return {};
  }
};

bool looks_ascii(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (p[i] < 32 || p[i] >= 127) return false;
  }
  return true;
}

bool is_string_at(const uint8_t* payload, size_t size, size_t pos, uint32_t length) {
  if (length > 0x10000 || pos + length > size) return false;
  if (!looks_ascii(payload + pos, length)) return false;
  if (pos + length < size) return payload[pos + length] == 0;
  return true;
}

bool is_class_ref(const uint8_t* payload, size_t size, size_t pos) {
  if (pos + 8 > size) return false;
  uint32_t idx = 0;
  std::memcpy(&idx, payload + pos, 4);
  if (idx > 0x100000) return false;
  if (is_string_at(payload, size, pos, 4)) {
    if (looks_ascii(payload + pos, 4) &&
        (pos + 4 >= size || payload[pos + 4] == 0)) {
      if (pos + 8 <= size) {
        uint32_t pad = 0;
        std::memcpy(&pad, payload + pos + 4, 4);
        if (pad == 0 && idx < 512) return true;
      }
      return false;
    }
  }
  return true;
}

bool is_ref_not_string(const uint8_t* payload, size_t size, size_t pos,
                       uint32_t length_or_tag) {
  if (is_string_at(payload, size, pos, length_or_tag)) {
    if (pos + 8 <= size) {
      uint32_t a = 0, b = 0;
      std::memcpy(&a, payload + pos, 4);
      std::memcpy(&b, payload + pos + 4, 4);
      if (a < 0x10000 && b < 0x10000) {
        if (looks_ascii(payload + pos, length_or_tag)) return false;
        return true;
      }
    }
    return false;
  }
  return pos + 8 <= size;
}

std::string read_string(const uint8_t* payload, size_t size, size_t& pos,
                        uint32_t ln) {
  if (pos + ln > size) {
    pos = size;
    return {};
  }
  std::string s(reinterpret_cast<const char*>(payload + pos), ln);
  pos += ln;
  if (pos < size && payload[pos] == 0) ++pos;
  return s;
}

ConstPool parse_cons_slrr(const uint8_t* payload, size_t size) {
  ConstPool pool;
  if (size < 4) return pool;
  size_t pos = 0;
  uint32_t count = 0;
  std::memcpy(&count, payload + pos, 4);
  pos += 4;
  pool.entries.reserve(count);

  for (uint32_t i = 0; i < count; ++i) {
    if (pos + 4 > size) break;
    uint32_t peek = 0;
    std::memcpy(&peek, payload + pos, 4);

    if (peek == 0) {
      pos += 4;
      if (pos + 4 > size) break;
      uint32_t ln = 0;
      std::memcpy(&ln, payload + pos, 4);
      pos += 4;
      if (ln > 0x100000 || pos + ln > size) break;
      pool.entries.emplace_back(read_string(payload, size, pos, ln));
      continue;
    }

    if (peek == 1) {
      if (pos + 6 <= size && payload[pos + 5] == 0 &&
          looks_ascii(payload + pos + 4, 1)) {
        if (pos + 10 <= size) {
          uint32_t val32 = 0;
          uint16_t pad16 = 0;
          std::memcpy(&val32, payload + pos + 4, 4);
          std::memcpy(&pad16, payload + pos + 8, 2);
          if (pad16 == 0 && (val32 >> 8) == 0) {
            pos += 10;
            const uint32_t ch = val32 & 0xFF;
            if (ch >= 32 && ch < 127) {
              pool.entries.emplace_back(std::string(1, static_cast<char>(ch)));
            } else {
              pool.entries.emplace_back(static_cast<int32_t>(val32));
            }
            continue;
          }
        }
        pos += 4;
        pool.entries.emplace_back(read_string(payload, size, pos, 1));
        continue;
      }
      pos += 4;
      uint32_t val = 0;
      std::memcpy(&val, payload + pos, 4);
      pos += 4;
      if (pos + 2 <= size) {
        uint16_t pad = 0;
        std::memcpy(&pad, payload + pos, 2);
        if (pad == 0) pos += 2;
      }
      const uint32_t ch = val & 0xFF;
      if ((val >> 8) == 0 && ch >= 32 && ch < 127) {
        pool.entries.emplace_back(std::string(1, static_cast<char>(ch)));
      } else {
        pool.entries.emplace_back(static_cast<int32_t>(val));
      }
      continue;
    }

    // RID: kind=3, pack_str_idx, local_id, pad — or Utf8 length=3 ("log", …).
    if (peek == 3) {
      if (is_string_at(payload, size, pos + 4, 3)) {
        pos += 4;
        pool.entries.emplace_back(read_string(payload, size, pos, 3));
        continue;
      }
      pos += 4;
      if (pos + 12 > size) break;
      uint32_t pack_idx = 0, local = 0, pad = 0;
      std::memcpy(&pack_idx, payload + pos, 4);
      std::memcpy(&local, payload + pos + 4, 4);
      std::memcpy(&pad, payload + pos + 8, 4);
      pos += 12;
      (void)pad;
      // Encode as ref-like array {3, pack_idx, local} for export.
      pool.entries.emplace_back(std::array<uint32_t, 3>{3u, pack_idx, local});
      continue;
    }

    if (peek == 4) {
      pos += 4;
      if (is_class_ref(payload, size, pos)) {
        uint32_t idx = 0;
        std::memcpy(&idx, payload + pos, 4);
        pos += 4;
        if (pos + 4 <= size) {
          uint32_t pad = 0;
          std::memcpy(&pad, payload + pos, 4);
          if (pad == 0) pos += 4;
        }
        pool.entries.emplace_back(static_cast<int32_t>(idx));
      } else {
        pool.entries.emplace_back(read_string(payload, size, pos, 4));
      }
      continue;
    }

    if (peek == 5 || peek == 6 || peek == 7) {
      if (is_ref_not_string(payload, size, pos + 4, peek)) {
        pos += 4;
        if (pos + 8 > size) break;
        uint32_t a = 0, b = 0;
        std::memcpy(&a, payload + pos, 4);
        std::memcpy(&b, payload + pos + 4, 4);
        pos += 8;
        pool.entries.emplace_back(std::array<uint32_t, 3>{peek, a, b});
      } else {
        pos += 4;
        pool.entries.emplace_back(read_string(payload, size, pos, peek));
      }
      continue;
    }

    // Bare length-prefixed string — or INT when length is not a plausible
    // ASCII string (City CONS: raw u32 236 + zero pad before next Utf8).
    const uint32_t ln = peek;
    if (ln > 7 && !is_string_at(payload, size, pos + 4, ln)) {
      pos += 4;
      pool.entries.emplace_back(static_cast<int32_t>(ln));
      if (pos + 4 <= size) {
        uint32_t pad = 0;
        std::memcpy(&pad, payload + pos, 4);
        if (pad == 0) pos += 4;
      }
      continue;
    }
    pos += 4;
    if (ln > 0x100000 || pos + ln > size) break;
    pool.entries.emplace_back(read_string(payload, size, pos, ln));
  }

  return pool;
}

std::string resolve_name(const ConstPool& const_pool, uint32_t idx) {
  if (idx == 0xFFFFFFFFu) return "?";
  return const_pool.get_str(idx);
}

bool is_mname(const std::string& s) {
  if (s.empty()) return false;
  if (s == "<init>") return true;
  if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
  for (char c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
  }
  return true;
}

bool is_msig(const std::string& s) {
  if (s.empty()) return false;
  if (s[0] == '(') return true;
  static const char* kPrims[] = {"I", "F", "V", "Z", "B", "C", "S", "J", "D"};
  for (const char* p : kPrims) {
    if (s == p) return true;
  }
  return false;
}

}  // namespace

bool tufa_parse(const uint8_t* data, size_t size, JvmClass* out, std::string* err) {
  if (!out) {
    if (err) *err = "null out";
    return false;
  }
  out->name.clear();
  out->super_name.clear();
  out->file.clear();
  out->methods.clear();
  out->trees.clear();
  out->field_inits.clear();

  if (size < 12 || std::memcmp(data, "TUFA", 4) != 0) {
    if (err) *err = "not TUFA";
    return false;
  }

  struct Section {
    char tag[5]{};
    const uint8_t* payload = nullptr;
    size_t size = 0;
  };
  std::vector<Section> sections;
  size_t pos = 12;
  while (pos + 8 <= size) {
    Section s;
    std::memcpy(s.tag, data + pos, 4);
    // Invictus packs several public classes into one .class file as
    // concatenated TUFA blobs — stop before the next magic.
    if (std::memcmp(s.tag, "TUFA", 4) == 0) break;
    uint32_t sz = 0;
    std::memcpy(&sz, data + pos + 4, 4);
    pos += 8;
    if (pos + sz > size) {
      if (err) *err = "truncated section";
      return false;
    }
    s.payload = data + pos;
    s.size = sz;
    sections.push_back(s);
    pos += sz;
  }

  auto find_sec = [&](const char* tag) -> const Section* {
    for (auto& s : sections) {
      if (std::strcmp(s.tag, tag) == 0) return &s;
    }
    return nullptr;
  };

  ConstPool pool;
  if (const Section* cons = find_sec("CONS")) {
    pool = parse_cons_slrr(cons->payload, cons->size);
  }

  // Export const pool strings + resolve mref → field/method name via nat.
  out->const_strings.assign(pool.entries.size(), {});
  out->const_mref_name.assign(pool.entries.size(), {});
  out->const_ints.assign(pool.entries.size(), 0);
  out->const_int_valid.assign(pool.entries.size(), 0);
  out->const_rid_pack.assign(pool.entries.size(), {});
  for (size_t i = 0; i < pool.entries.size(); ++i) {
    if (auto* s = std::get_if<std::string>(&pool.entries[i])) {
      out->const_strings[i] = *s;
      // Single-byte "string" often an INT/RID byte (e.g. local id 0x06).
      if (s->size() == 1) {
        out->const_ints[i] = static_cast<unsigned char>((*s)[0]);
        out->const_int_valid[i] = 1;
      }
    } else if (auto* n = std::get_if<int32_t>(&pool.entries[i])) {
      out->const_ints[i] = *n;
      out->const_int_valid[i] = 1;
    }
  }
  for (size_t i = 0; i < pool.entries.size(); ++i) {
    auto* ref = std::get_if<std::array<uint32_t, 3>>(&pool.entries[i]);
    if (!ref) continue;
    const uint32_t kind = (*ref)[0];
    const uint32_t a = (*ref)[1];
    const uint32_t b = (*ref)[2];
    if (kind == 3) {  // RID: pack_str_idx, local_id
      out->const_ints[i] = static_cast<int32_t>(b);
      out->const_int_valid[i] = 1;
      out->const_rid_pack[i] = pool.get_str(a);
    } else if (kind == 7) {  // nat: name_idx, sig_idx
      out->const_mref_name[i] = pool.get_str(a);
    } else if (kind == 5 || kind == 6) {  // mref/fref → nat
      if (a < pool.entries.size()) {
        // try b as nat index first (SLRR mref is (class, nat))
        if (b < pool.entries.size()) {
          if (auto* natb =
                  std::get_if<std::array<uint32_t, 3>>(&pool.entries[b])) {
            if ((*natb)[0] == 7) out->const_mref_name[i] = pool.get_str((*natb)[1]);
          } else {
            std::string s = pool.get_str(b);
            if (!s.empty()) out->const_mref_name[i] = s;
          }
        }
      }
    }
  }

  if (const Section* clss = find_sec("CLSS")) {
    if (clss->size >= 16) {
      uint32_t words[5]{};
      const size_t n = (std::min)(clss->size / 4, size_t{5});
      for (size_t i = 0; i < n; ++i) {
        std::memcpy(&words[i], clss->payload + i * 4, 4);
      }
      out->name = resolve_name(pool, words[2]);
      if (words[3] && words[3] != 0xFFFFFFFFu) {
        out->super_name = resolve_name(pool, words[3]);
      }
    }
  }

  if (out->name.empty() || out->name == "?") {
    for (const auto& e : pool.entries) {
      if (auto* s = std::get_if<std::string>(&e)) {
        if (s->find('.') != std::string::npos) {
          out->name = *s;
          break;
        }
      }
    }
  }

  if (const Section* tree = find_sec("TREE")) {
    // SLRR: u32 tree_count; per tree: u32 node_count; then node_count nodes.
    // Node is 3 bytes (op:u8, slot:u16) or 7 bytes (+ imm:u32) when
    // (kTreeOpFlags[op] >> 12) in {1,2,4} — matches TREE_readNode/TREE_nodeBytes.
    auto node_bytes = [](uint8_t op) -> size_t {
      if (op >= 48) return 3;
      const uint16_t f = kTreeOpFlags[op];
      const uint16_t nib = static_cast<uint16_t>((f >> 12) & 0xF);
      return (nib == 1 || nib == 2 || nib == 4) ? 7u : 3u;
    };
    if (tree->size >= 4) {
      uint32_t n = 0;
      std::memcpy(&n, tree->payload, 4);
      size_t off = 4;
      for (uint32_t i = 0; i < n; ++i) {
        if (off + 4 > tree->size) {
          if (err) *err = "TREE truncated header";
          return false;
        }
        uint32_t nnodes = 0;
        std::memcpy(&nnodes, tree->payload + off, 4);
        off += 4;
        TreeBody body;
        body.nodes.reserve(nnodes);
        for (uint32_t ni = 0; ni < nnodes; ++ni) {
          if (off >= tree->size) {
            if (err) *err = "TREE truncated node";
            return false;
          }
          const uint8_t op = tree->payload[off];
          const size_t nb = node_bytes(op);
          if (off + nb > tree->size) {
            if (err) *err = "TREE truncated node bytes";
            return false;
          }
          TreeNode node;
          node.op = op;
          std::memcpy(&node.slot, tree->payload + off + 1, 2);
          if (nb == 7) {
            std::memcpy(&node.imm, tree->payload + off + 3, 4);
            node.has_imm = true;
          }
          body.nodes.push_back(node);
          off += nb;
        }
        out->trees.push_back(std::move(body));
      }
      if (off != tree->size) {
        // Tolerate trailing padding only; hard mismatch is a format bug.
        if (off > tree->size) {
          if (err) *err = "TREE parse overrun";
          return false;
        }
      }
    }
  }

  if (const Section* mthd = find_sec("MTHD")) {
    const uint8_t* p = mthd->payload;
    const size_t psz = mthd->size;
    if (psz >= 8) {
      uint32_t n_name_first = 0;
      std::memcpy(&n_name_first, p, 4);
      const uint32_t n_total = static_cast<uint32_t>((psz - 8) / 20);
      if (n_name_first > n_total) n_name_first = 0;
      if (n_total > 0 && (psz - 8) % 20 == 0) {
        size_t off = 8;
        for (uint32_t mi = 0; mi < n_total; ++mi) {
          uint32_t w0, w1, w2, w3, w4;
          std::memcpy(&w0, p + off, 4);
          std::memcpy(&w1, p + off + 4, 4);
          std::memcpy(&w2, p + off + 8, 4);
          std::memcpy(&w3, p + off + 12, 4);
          std::memcpy(&w4, p + off + 16, 4);
          (void)w4;
          off += 20;

          std::string name, sig;
          uint32_t tree_i = 0;
          uint32_t flags = 0;
          if (mi < n_name_first) {
            name = pool.get_str(w0);
            sig = pool.get_str(w1);
            tree_i = w2;
            flags = (w3 == 0xFFFFFFFFu) ? 0 : w3;
            if (!is_mname(name) && is_mname(pool.get_str(w1))) {
              name = pool.get_str(w1);
              sig = pool.get_str(w2);
              tree_i = w3;
              flags = w0;
            }
          } else {
            flags = w0;
            name = pool.get_str(w1);
            sig = pool.get_str(w2);
            tree_i = w3;
            if (is_msig(name) && is_mname(sig)) std::swap(name, sig);
          }
          if (!is_mname(name)) continue;
          if (sig.empty()) sig = "()V";
          if (name == "<init>" && sig == "()") sig = "()V";
          else if (name == "<init>" && !sig.empty() && sig.back() == ')') sig += 'V';

          JvmMethod m;
          m.name = std::move(name);
          m.signature = std::move(sig);
          m.tree_index = (tree_i == 0xFFFFFFFFu) ? -1 : static_cast<int>(tree_i);
          m.flags = flags;
          const bool empty_tree =
              m.tree_index < 0 ||
              static_cast<size_t>(m.tree_index) >= out->trees.size() ||
              out->trees[static_cast<size_t>(m.tree_index)].nodes.empty();
          m.is_native = empty_tree || (flags & 0x100) != 0;
          out->methods.push_back(std::move(m));
        }
      }
    }
  }

  // FILD: instance field initializers as dedicated TREEs.
  // Records are 16 bytes. Static/named rows have w0!=0; instance inits use
  // w0==0, w1=name, w2=type, w3=tree_index (0xFFFFFFFF = no init tree).
  if (const Section* fild = find_sec("FILD")) {
    const uint8_t* p = fild->payload;
    const size_t psz = fild->size;
    size_t off = 0;
    if (psz >= 8) {
      // Skip leading count words when present (City: n, n).
      uint32_t a = 0, b = 0;
      std::memcpy(&a, p, 4);
      std::memcpy(&b, p + 4, 4);
      if (a > 0 && a < 0x10000 && (b == a || b < 0x10000)) off = 8;
    }
    while (off + 16 <= psz) {
      uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
      std::memcpy(&w0, p + off, 4);
      std::memcpy(&w1, p + off + 4, 4);
      std::memcpy(&w2, p + off + 8, 4);
      std::memcpy(&w3, p + off + 12, 4);
      off += 16;
      if (w0 != 0 || w3 == 0xFFFFFFFFu) continue;
      if (static_cast<size_t>(w3) >= out->trees.size()) continue;
      std::string fname = pool.get_str(w1);
      if (fname.empty()) fname = pool.get_str(w2);
      if (fname.empty() || !is_mname(fname)) continue;
      JvmFieldInit fi;
      fi.name = std::move(fname);
      fi.tree_index = static_cast<int>(w3);
      out->field_inits.push_back(std::move(fi));
    }
  }

  if (out->name.empty()) {
    if (err) *err = "missing class name";
    return false;
  }
  return true;
}

bool tufa_load_file(const char* path, JvmClass* out, std::string* err) {
  std::vector<JvmClass> all;
  if (!tufa_load_file_all(path, &all, err)) return false;
  if (all.empty()) {
    if (err) *err = "no TUFA blobs";
    return false;
  }
  *out = std::move(all[0]);
  return true;
}

bool tufa_load_file_all(const char* path, std::vector<JvmClass>* out,
                        std::string* err) {
  if (!out) {
    if (err) *err = "null out";
    return false;
  }
  out->clear();
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (err) *err = std::string("cannot open ") + path;
    return false;
  }
  in.seekg(0, std::ios::end);
  const auto len = in.tellg();
  in.seekg(0, std::ios::beg);
  if (len <= 0) {
    if (err) *err = "empty file";
    return false;
  }
  std::vector<uint8_t> buf(static_cast<size_t>(len));
  in.read(reinterpret_cast<char*>(buf.data()), len);
  if (!in) {
    if (err) *err = "read failed";
    return false;
  }

  size_t off = 0;
  while (off + 12 <= buf.size() &&
         std::memcmp(buf.data() + off, "TUFA", 4) == 0) {
    // Measure this blob: header + sections until next TUFA or EOF.
    size_t pos = off + 12;
    while (pos + 8 <= buf.size()) {
      char tag[4];
      std::memcpy(tag, buf.data() + pos, 4);
      if (std::memcmp(tag, "TUFA", 4) == 0) break;
      uint32_t sz = 0;
      std::memcpy(&sz, buf.data() + pos + 4, 4);
      if (pos + 8 + sz > buf.size()) {
        if (err) *err = "truncated multi-TUFA section";
        return false;
      }
      pos += 8 + sz;
    }
    JvmClass cls;
    std::string perr;
    if (!tufa_parse(buf.data() + off, pos - off, &cls, &perr)) {
      if (err) *err = perr.empty() ? "tufa_parse failed" : perr;
      return false;
    }
    cls.file = path;
    out->push_back(std::move(cls));
    off = pos;
  }
  if (out->empty()) {
    if (err) *err = "no TUFA blobs";
    return false;
  }
  return true;
}

}  // namespace inv
