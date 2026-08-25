#include "rpak.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace inv {
namespace {

std::mutex g_mu;
std::string g_root;
std::vector<RpakPack> g_packs;
std::unordered_map<std::string, int32_t> g_by_path;

std::string norm_path(std::string p) {
  for (char& c : p) {
    if (c == '\\') c = '/';
  }
  return p;
}

std::string basename_of(const std::string& p) {
  const auto slash = p.find_last_of('/');
  if (slash == std::string::npos) return p;
  return p.substr(slash + 1);
}

bool path_exists(const std::string& path) {
#ifdef _WIN32
  const DWORD attr = GetFileAttributesA(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES;
#else
  std::ifstream in(path, std::ios::binary);
  return static_cast<bool>(in);
#endif
}

bool file_exists(const std::string& path) { return path_exists(path); }

std::string resolve_path(const char* lib) {
  if (!lib || !lib[0]) return {};
  std::string p = norm_path(lib);
  if (p.size() >= 2 && p[1] == ':') return p;
  if (!p.empty() && p[0] == '/') return p;

  // Wildcards: resolve directory prefix only.
  const auto star = p.find('*');
  std::string dir = p;
  std::string leaf;
  if (star != std::string::npos) {
    const auto slash = p.find_last_of('/');
    if (slash == std::string::npos) {
      dir = ".";
      leaf = p;
    } else {
      dir = p.substr(0, slash);
      leaf = p.substr(slash + 1);
    }
  }

  std::string under_root;
  if (!g_root.empty()) {
    std::string root = g_root;
    if (!root.empty() && root.back() == '/') root.pop_back();
    under_root = root + "/" + dir;
    if (path_exists(under_root)) {
      return leaf.empty() ? under_root : (under_root + "/" + leaf);
    }
  }
  if (path_exists(dir)) {
    return leaf.empty() ? dir : (dir + "/" + leaf);
  }
  if (leaf.empty()) {
    return under_root.empty() ? p : under_root;
  }
  return under_root.empty() ? p : (under_root + "/" + leaf);
}

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const auto n = in.tellg();
  if (n <= 0) return false;
  in.seekg(0, std::ios::beg);
  out->resize(static_cast<size_t>(n));
  in.read(reinterpret_cast<char*>(out->data()), n);
  return static_cast<bool>(in) || in.eof();
}

bool read_name(const std::vector<uint8_t>& data, size_t* off, std::string* name) {
  if (*off >= data.size()) return false;
  const uint8_t nsz = data[*off];
  *off += 1;
  if (nsz < 1 || *off + nsz > data.size()) return false;
  const char* raw = reinterpret_cast<const char*>(data.data() + *off);
  *off += nsz;
  size_t len = 0;
  while (len < nsz && raw[len] != '\0') {
    const unsigned char c = static_cast<unsigned char>(raw[len]);
    // Type-tree packs use odd printable names (e.g. `14"`); allow all printables.
    if (c < 32 || c >= 127) return false;
    ++len;
  }
  if (len == 0) return false;
  name->assign(raw, len);
  return true;
}

enum class ParseMode { Strict, Map, Frontend };

bool parse_entries(const std::vector<uint8_t>& data, size_t off, uint32_t nentries,
                   ParseMode mode, std::vector<RpakEntry>* out) {
  out->clear();
  std::string cur_dir;
  try {
    for (uint32_t idx = 0; idx < nentries; ++idx) {
      if (off + 4 > data.size()) return false;
      uint32_t kind = 0;
      std::memcpy(&kind, data.data() + off, 4);

      bool is_dir = false;
      size_t transform = 0;
      if (kind == 0) {
        is_dir = true;
      } else if (mode == ParseMode::Map && kind == 1 && idx == 0) {
        is_dir = true;
        transform = 48;
      } else if (mode == ParseMode::Frontend && (kind == 1 || kind == 2) &&
                 idx == 0) {
        is_dir = true;
        transform = (kind == 1) ? 48 : 0;
      }

      if (is_dir) {
        size_t p = off + 4;
        p += 4 + 10 + 4 + 8;
        std::string name;
        if (!read_name(data, &p, &name)) return false;
        p += transform;
        if (p > data.size()) return false;
        cur_dir = name;
        RpakEntry e;
        e.is_dir = true;
        e.kind = static_cast<int32_t>(kind);
        e.name = name;
        e.path = name;
        out->push_back(std::move(e));
        off = p;
        continue;
      }

      // kind(4) + type_id(4) + flags(2) + unk(4) + offset(4) + size(4) + name
      if (off + 4 + 4 + 2 + 4 + 8 > data.size()) return false;
      uint32_t type_id = 0;
      std::memcpy(&type_id, data.data() + off + 4, 4);
      size_t p = off + 4 + 4 + 2 + 4;
      uint32_t file_off = 0, file_sz = 0;
      std::memcpy(&file_off, data.data() + p, 4);
      std::memcpy(&file_sz, data.data() + p + 4, 4);
      p += 8;
      std::string name;
      if (!read_name(data, &p, &name)) return false;
      if (kind == 0x10004) p += 48;
      // Type-tree stubs often have bogus offset/size — keep name/ids.
      if (file_off > data.size() ||
          static_cast<uint64_t>(file_off) + file_sz > data.size()) {
        file_off = 0;
        file_sz = 0;
      }
      RpakEntry e;
      e.is_dir = false;
      e.kind = static_cast<int32_t>(kind);
      e.type_id = static_cast<int32_t>(type_id);
      e.name = name;
      e.path = cur_dir.empty() ? name : (cur_dir + "/" + name);
      e.offset = file_off;
      e.size = file_sz;
      out->push_back(std::move(e));
      off = p;
    }
  } catch (...) {
    return false;
  }
  return true;
}

float entry_quality(const std::vector<RpakEntry>& ents,
                    const std::vector<uint8_t>& data) {
  size_t files = 0, good = 0;
  for (const auto& e : ents) {
    if (e.is_dir) continue;
    ++files;
    if (e.name.empty()) continue;
    // Named node counts even if blob is empty (type-tree stubs).
    if (e.size == 0 ||
        (e.offset <= data.size() &&
         static_cast<uint64_t>(e.offset) + e.size <= data.size())) {
      ++good;
    }
  }
  if (files == 0) return -1.f;
  return static_cast<float>(good) / static_cast<float>(files);
}

bool load_pack_file(const std::string& path, RpakPack* pack) {
  std::vector<uint8_t> data;
  if (!read_file(path, &data) || data.size() < 16) return false;
  if (std::memcmp(data.data(), "RPAK", 4) != 0) return false;

  uint32_t ver = 0, packs = 0, zero = 0;
  std::memcpy(&ver, data.data() + 4, 4);
  std::memcpy(&packs, data.data() + 8, 4);
  std::memcpy(&zero, data.data() + 12, 4);
  (void)zero;
  pack->version = ver;

  size_t off = 16;
  pack->deps.clear();
  for (uint32_t i = 0; i < packs; ++i) {
    if (off + 4 + 0x3C > data.size()) return false;
    off += 4;  // id/slot
    char namebuf[0x3C];
    std::memcpy(namebuf, data.data() + off, 0x3C);
    off += 0x3C;
    namebuf[0x3B] = '\0';
    pack->deps.emplace_back(namebuf);
  }

  if (off + 8 > data.size()) {
    // Truncated after deps — still a valid open for registry-ish packs.
    pack->is_registry = true;
    return true;
  }

  uint32_t info_size = 0, nentries = 0;
  std::memcpy(&info_size, data.data() + off, 4);
  std::memcpy(&nentries, data.data() + off + 4, 4);
  (void)info_size;
  off += 8;

  // system.rpk: packs==0 and entry parse usually fails → registry.
  if (packs == 0 && nentries == 0) {
    pack->is_registry = true;
    return true;
  }

  std::vector<RpakEntry> best;
  float best_q = -1.f;
  for (ParseMode mode :
       {ParseMode::Strict, ParseMode::Map, ParseMode::Frontend}) {
    std::vector<RpakEntry> ents;
    if (!parse_entries(data, off, nentries, mode, &ents)) continue;
    const float q = entry_quality(ents, data);
    if (q > best_q) {
      best_q = q;
      best = std::move(ents);
    }
  }

  if (best_q >= 0.5f) {
    pack->entries = std::move(best);
    pack->parsed_entries = true;
    pack->is_registry = false;
    // Hierarchy: children of R are entries with parent_key(kind) == R.type_id.
    // Scripted nodes (kind hi=0x2) parent via kind&0xFFFF (Baiern_VT → 0x1000).
    std::unordered_map<int32_t, size_t> by_type;
    std::unordered_map<int32_t, std::vector<size_t>> kids;
    for (size_t i = 0; i < pack->entries.size(); ++i) {
      auto& e = pack->entries[i];
      e.parent_local = -1;
      e.first_child_local = -1;
      e.next_sibling_local = -1;
      if (e.is_dir) continue;
      by_type[e.type_id] = i;
      kids[rpak_parent_key(e.kind)].push_back(i);
    }
    for (size_t i = 0; i < pack->entries.size(); ++i) {
      auto& e = pack->entries[i];
      if (e.is_dir) continue;
      const int32_t pk = rpak_parent_key(e.kind);
      auto pit = by_type.find(pk);
      if (pit != by_type.end()) e.parent_local = pk;
      else if ((static_cast<uint32_t>(e.kind) >> 16) == 0x2u)
        e.parent_local = pk;  // cross-pack parent (cars:0x1000)
    }
    for (auto& kv : kids) {
      const int32_t parent_tid = kv.first;
      auto& idxs = kv.second;
      if (idxs.empty()) continue;
      auto pit = by_type.find(parent_tid);
      if (pit != by_type.end()) {
        pack->entries[pit->second].first_child_local =
            pack->entries[idxs[0]].type_id;
      }
      for (size_t s = 0; s + 1 < idxs.size(); ++s) {
        pack->entries[idxs[s]].next_sibling_local =
            pack->entries[idxs[s + 1]].type_id;
      }
    }
  } else {
    // Header OK but no file index (type trees / system registry).
    pack->is_registry = true;
    pack->parsed_entries = false;
  }
  return true;
}

}  // namespace

void rpak_set_game_root(const char* root) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_root = root ? norm_path(root) : std::string();
}

std::string rpak_resolve_path(const char* path) {
  std::lock_guard<std::mutex> lock(g_mu);
  return resolve_path(path);
}

int32_t rpak_open(const char* lib_path) {
  const std::string resolved = resolve_path(lib_path);
  if (resolved.empty()) return 0;

  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_by_path.find(resolved);
  if (it != g_by_path.end()) return it->second;

  RpakPack pack;
  pack.path = resolved;
  pack.name = basename_of(resolved);
  if (!load_pack_file(resolved, &pack)) {
    std::fprintf(stderr, "[rpak] open failed: %s\n", resolved.c_str());
    return 0;
  }

  // Catalog compares (res.id() >> 16) == openLib(...).
  const int32_t id = static_cast<int32_t>(g_packs.size()) + 1;
  pack.pack_id = id;
  g_packs.push_back(std::move(pack));
  g_by_path[resolved] = id;
  return id;
}

const RpakPack* rpak_get(int32_t pack_id) {
  std::lock_guard<std::mutex> lock(g_mu);
  for (const auto& p : g_packs) {
    if (p.pack_id == pack_id) return &p;
  }
  return nullptr;
}

const RpakPack* rpak_find_by_name(const char* basename) {
  if (!basename || !basename[0]) return nullptr;
  std::string want = norm_path(basename);
  // Accept "frontend" or "frontend.rpk"
  std::string want_rpk = want;
  if (want_rpk.size() < 4 ||
      want_rpk.substr(want_rpk.size() - 4) != ".rpk") {
    want_rpk += ".rpk";
  }
  std::lock_guard<std::mutex> lock(g_mu);
  for (const auto& p : g_packs) {
    if (p.name == want || p.name == want_rpk) return &p;
    // also match stem
    if (p.name.size() > 4 && p.name.substr(p.name.size() - 4) == ".rpk") {
      if (p.name.substr(0, p.name.size() - 4) == want) return &p;
    }
  }
  return nullptr;
}

size_t rpak_count() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_packs.size();
}

const RpakPack* rpak_find_pack_for_res(int32_t res_id) {
  return rpak_get(rpak_id_pack(res_id));
}

const RpakEntry* rpak_find_entry(int32_t res_id) {
  const RpakPack* pack = rpak_get(rpak_id_pack(res_id));
  if (!pack || !pack->parsed_entries) return nullptr;
  const int32_t local = static_cast<int32_t>(rpak_id_local(res_id));
  for (const auto& e : pack->entries) {
    if (!e.is_dir && e.type_id == local) return &e;
  }
  return nullptr;
}

bool rpak_read_entry(int32_t res_id, std::vector<uint8_t>* out) {
  if (!out) return false;
  out->clear();
  const RpakPack* pack = rpak_get(rpak_id_pack(res_id));
  const RpakEntry* ent = rpak_find_entry(res_id);
  if (!pack || !ent || ent->is_dir || ent->size == 0) return false;

  std::ifstream in(pack->path, std::ios::binary);
  if (!in) return false;
  out->resize(ent->size);
  in.seekg(static_cast<std::streamoff>(ent->offset));
  in.read(reinterpret_cast<char*>(out->data()),
          static_cast<std::streamsize>(ent->size));
  return static_cast<bool>(in) || in.gcount() == static_cast<std::streamsize>(ent->size);
}

namespace {

std::vector<int32_t> collect_children_unlocked(int32_t parent_local) {
  std::vector<int32_t> out;
  for (const auto& pack : g_packs) {
    if (!pack.parsed_entries) continue;
    for (const auto& e : pack.entries) {
      if (e.is_dir) continue;
      if (rpak_parent_key(e.kind) == parent_local)
        out.push_back(rpak_make_id(pack.pack_id,
                                   static_cast<uint16_t>(e.type_id)));
    }
  }
  return out;
}

}  // namespace

int32_t rpak_first_child_id(int32_t parent_res_id) {
  if (parent_res_id == 0) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  const int32_t plocal = static_cast<int32_t>(rpak_id_local(parent_res_id));
  // Prefer same-pack linked list when present.
  for (const auto& pack : g_packs) {
    if (pack.pack_id != rpak_id_pack(parent_res_id) || !pack.parsed_entries)
      continue;
    for (const auto& e : pack.entries) {
      if (!e.is_dir && e.type_id == plocal && e.first_child_local >= 0)
        return rpak_make_id(pack.pack_id,
                            static_cast<uint16_t>(e.first_child_local));
    }
  }
  auto kids = collect_children_unlocked(plocal);
  return kids.empty() ? 0 : kids.front();
}

int32_t rpak_next_sibling_id(int32_t child_res_id) {
  if (child_res_id == 0) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  const RpakPack* pack = nullptr;
  const RpakEntry* ent = nullptr;
  for (const auto& p : g_packs) {
    if (p.pack_id != rpak_id_pack(child_res_id) || !p.parsed_entries) continue;
    for (const auto& e : p.entries) {
      if (!e.is_dir && e.type_id == static_cast<int32_t>(rpak_id_local(child_res_id))) {
        pack = &p;
        ent = &e;
        break;
      }
    }
  }
  if (!ent) return 0;
  if (ent->next_sibling_local >= 0)
    return rpak_make_id(pack->pack_id,
                        static_cast<uint16_t>(ent->next_sibling_local));
  // Cross-pack: next child of the same parent_key after this id.
  const int32_t pk = rpak_parent_key(ent->kind);
  auto kids = collect_children_unlocked(pk);
  for (size_t i = 0; i < kids.size(); ++i) {
    if (kids[i] == child_res_id)
      return (i + 1 < kids.size()) ? kids[i + 1] : 0;
  }
  return 0;
}

int32_t rpak_parent_id(int32_t child_res_id) {
  if (child_res_id == 0) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  const int32_t clocal = static_cast<int32_t>(rpak_id_local(child_res_id));
  const int32_t cpack = rpak_id_pack(child_res_id);
  const RpakPack* child_pack = nullptr;
  const RpakEntry* ent = nullptr;
  for (const auto& p : g_packs) {
    if (p.pack_id != cpack || !p.parsed_entries) continue;
    child_pack = &p;
    for (const auto& e : p.entries) {
      if (!e.is_dir && e.type_id == clocal) {
        ent = &e;
        break;
      }
    }
  }
  if (!ent || ent->parent_local < 0) return 0;
  const int32_t plocal = ent->parent_local;

  auto stem = [](std::string n) {
    n = norm_path(n);
    n = basename_of(n);
    if (n.size() > 4 && n.substr(n.size() - 4) == ".rpk")
      n = n.substr(0, n.size() - 4);
    return n;
  };

  auto find_in_pack = [&](const RpakPack& p) -> int32_t {
    if (!p.parsed_entries) return 0;
    for (const auto& e : p.entries) {
      if (!e.is_dir && e.type_id == plocal)
        return rpak_make_id(p.pack_id, static_cast<uint16_t>(plocal));
    }
    return 0;
  };

  if (child_pack) {
    if (int32_t id = find_in_pack(*child_pack)) return id;
    for (const auto& dep : child_pack->deps) {
      const std::string want = stem(dep);
      for (const auto& p : g_packs) {
        if (stem(p.name) != want) continue;
        if (int32_t id = find_in_pack(p)) return id;
      }
    }
  }
  for (const auto& p : g_packs) {
    if (int32_t id = find_in_pack(p)) return id;
  }
  return 0;
}

}  // namespace inv
