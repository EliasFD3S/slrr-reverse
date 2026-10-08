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

// PE ResPack_ParseChildRecord @ 0x544010 — namelen @ +22; name @ +23;
// if (+9 & 1) skip 12 floats (48B) after name. Returns record byte length.
bool read_pe_name(const std::vector<uint8_t>& data, size_t name_off,
                  uint8_t nsz, std::string* name) {
  if (nsz < 1 || name_off + nsz > data.size()) return false;
  const char* raw = reinterpret_cast<const char*>(data.data() + name_off);
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

// Soft PE makeTexture @ 0x0047FFE0 / File_SdatType2_RemountTextures @ 0x485190
// / Resource_loadSourcefilePayload @ 0x538840: texture TOC payload is often
// text "sourcefile <path>\r\nflags <n>\r\n" (space or '=' after keyword).
// Returns first path; normalizes '\\' → '/'.
bool extract_sourcefile_path(const uint8_t* blob, size_t n, std::string* path) {
  if (!blob || n < 10 || !path) return false;
  size_t i = 0;
  while (i < n) {
    size_t line_end = i;
    while (line_end < n && blob[line_end] != '\n' && blob[line_end] != '\r')
      ++line_end;
    std::string line(reinterpret_cast<const char*>(blob + i), line_end - i);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
      ++start;
    line = line.substr(start);
    if (line.size() >= 10 &&
        std::strncmp(line.c_str(), "sourcefile", 10) == 0) {
      size_t p = 10;
      if (p < line.size() && line[p] == '=') ++p;
      while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
      if (p < line.size()) {
        path->assign(line.substr(p));
        for (char& c : *path) {
          if (c == '\\') c = '/';
        }
        return !path->empty();
      }
    }
    i = line_end;
    while (i < n && (blob[i] == '\n' || blob[i] == '\r')) ++i;
  }
  return false;
}

// Soft PE TOC walk — EnsureIndex @ 0x54431C..0x544341 + ParseChildRecord size.
// Layout per record (a2 in ParseChildRecord):
//   +0  kind/parent u32   +4  type_id u32
//   +8  restype u8        +9  flags u8 (bit0 → 12f transform)
//   +10 pad u16+u32       +14 file_off u32  +18 file_sz u32
//   +22 namelen u8        +23 name[namelen]  [+48 if flags&1]
// PE ParseChildRecord @ 0x544085: desc[0]=rec[8] restype (7=texture).
bool parse_entries_pe(const std::vector<uint8_t>& data, size_t off,
                      uint32_t nentries, size_t toc_end,
                      std::vector<RpakEntry>* out) {
  out->clear();
  try {
    for (uint32_t nleft = nentries; nleft > 0; --nleft) {
      if (off + 23 > data.size() || off + 23 > toc_end) return false;
      uint32_t kind = 0, type_id = 0;
      std::memcpy(&kind, data.data() + off, 4);
      std::memcpy(&type_id, data.data() + off + 4, 4);
      const uint8_t restype = data[off + 8];  // @ 0x544085 desc[0]
      const uint8_t flags = data[off + 9];
      uint32_t file_off = 0, file_sz = 0;
      std::memcpy(&file_off, data.data() + off + 14, 4);
      std::memcpy(&file_sz, data.data() + off + 18, 4);
      const uint8_t nsz = data[off + 22];
      std::string name;
      if (!read_pe_name(data, off + 23, nsz, &name)) return false;
      size_t end = off + 23 + nsz;
      if ((flags & 1u) != 0) end += 12 * sizeof(float);  // @ 0x5440BB
      if (end > data.size() || end > toc_end) return false;
      // Type-tree stubs often have bogus offset/size — keep name/ids.
      if (file_off > data.size() ||
          static_cast<uint64_t>(file_off) + file_sz > data.size()) {
        file_off = 0;
        file_sz = 0;
      }
      RpakEntry e;
      e.is_dir = false;  // PE CreateNodeUnder — kind is parent, not a dir bit
      e.kind = static_cast<int32_t>(kind);
      e.type_id = static_cast<int32_t>(type_id);
      e.name = name;
      e.path = name;
      // Soft PE makeTexture@47FFE0 / RemountTextures@485190 restype==7:
      // prefer sourcefile path as entry_path for DDS resolve/upload.
      if (file_off != 0 && file_sz != 0 &&
          (restype == 7u ||
           (file_sz >= 10 &&
            std::memcmp(data.data() + file_off, "sourcefile", 10) == 0))) {
        std::string src;
        if (extract_sourcefile_path(data.data() + file_off, file_sz, &src))
          e.path = std::move(src);
      }
      e.offset = file_off;
      e.size = file_sz;
      out->push_back(std::move(e));
      off = end;
    }
  } catch (...) {
    return false;
  }
  return off == toc_end;
}

void link_entry_hierarchy(RpakPack* pack) {
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
}

// Soft PE ResourcePack_EnsureIndex @ 0x544170 — PathExists + SlotRead RPAK
// magic/ver + dep table (0x40) + TOC header + ParseChildRecord blob.
// Remap TOC@+0x5C produced in rpak_open_unlocked after LoadPack deps.
bool load_pack_file(const std::string& path, RpakPack* pack) {
  std::vector<uint8_t> data;
  if (!read_file(path, &data) || data.size() < 16) return false;
  if (std::memcmp(data.data(), "RPAK", 4) != 0) return false;

  uint32_t ver = 0;
  std::memcpy(&ver, data.data() + 4, 4);
  // PE SlotRead → pack+0x54 / +0x58 @ 0x5441F3..0x5441FF (file +8/+0xC).
  std::memcpy(&pack->toc_lim_a, data.data() + 8, 4);
  std::memcpy(&pack->toc_lim_b, data.data() + 12, 4);
  pack->version = ver;

  // PE malloc count = (+0x54)+(+0x58)+1; dep loop = count-1 @ 0x54420A..0x544255.
  const uint32_t ndeps = pack->toc_lim_a + pack->toc_lim_b;
  size_t off = 16;
  pack->deps.clear();
  pack->deps.reserve(ndeps);
  for (uint32_t i = 0; i < ndeps; ++i) {
    // PE @ 0x54425E: SlotRead 0x40 = id u32 + name[56] + trailer u32.
    if (off + 0x40 > data.size()) return false;
    uint32_t id_dword = 0;
    std::memcpy(&id_dword, data.data() + off, 4);
    off += 4;
    char namebuf[56];
    std::memcpy(namebuf, data.data() + off, 56);
    off += 56;
    uint32_t trailer = 0;
    std::memcpy(&trailer, data.data() + off, 4);
    off += 4;
    namebuf[55] = '\0';
    RpakDep dep;
    dep.id_hi = static_cast<uint16_t>(id_dword >> 16);  // @ 0x54426A
    dep.trailer = trailer;
    dep.name.assign(namebuf);
    pack->deps.push_back(std::move(dep));
  }

  if (off + 8 > data.size()) {
    // Truncated after deps — still a valid open for registry-ish packs.
    pack->is_registry = true;
    return true;
  }

  // PE @ 0x5442C2..0x5442E9: info_size, nentries, then two unused u32
  // (stack v15/v16 — discarded; must skip before TOC blob).
  uint32_t info_size = 0, nentries = 0;
  std::memcpy(&info_size, data.data() + off, 4);
  std::memcpy(&nentries, data.data() + off + 4, 4);
  off += 8;
  if (off + 8 > data.size()) {
    pack->is_registry = true;
    return true;
  }
  off += 8;  // skip PE unused dword pair (W40)

  if (ndeps == 0 && nentries == 0) {
    pack->is_registry = true;
    return true;
  }

  if (info_size == 0 || nentries == 0) {
    pack->is_registry = true;
    pack->parsed_entries = false;
    return true;
  }
  if (off + info_size > data.size()) {
    pack->is_registry = true;
    pack->parsed_entries = false;
    return true;
  }

  const size_t toc_end = off + info_size;
  std::vector<RpakEntry> ents;
  if (parse_entries_pe(data, off, nentries, toc_end, &ents)) {
    pack->entries = std::move(ents);
    pack->parsed_entries = true;
    pack->is_registry = false;
    link_entry_hierarchy(pack);
  } else {
    // Header OK but TOC walk failed (corrupt / unknown variant).
    pack->is_registry = true;
    pack->parsed_entries = false;
  }
  return true;
}

// Soft PE ResourceEngine_LoadPack @ 0x538380 + EnsureIndex dep loop
// @ 0x5442AF — register path, parse TOC, then LoadPack each dep name.
// Assumes g_mu held. Idempotent via g_by_path.
int32_t rpak_open_unlocked(const char* lib_path) {
  const std::string resolved = resolve_path(lib_path);
  if (resolved.empty()) return 0;

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
  const size_t self_idx = g_packs.size() - 1;

  // Soft PE EnsureIndex TOC@+0x5C @ 0x544221..0x5442AF:
  // malloc((+0x54)+(+0x58)+1)*66; slot0 pack_hi=own; deps LoadPack → +2.
  // Re-index after each dep open — recursive push may reallocate g_packs.
  {
    RpakPack& self0 = g_packs[self_idx];
    const uint32_t lim = self0.toc_lim_a + self0.toc_lim_b;
    const size_t ndeps = self0.deps.size();
    self0.remap_toc.assign(static_cast<size_t>(lim) + 1u, RpakRemapSlot{});
    self0.remap_toc[0].pack_hi = static_cast<uint16_t>(id);  // @ 0x544231
    for (uint32_t i = 0; i < ndeps && i < lim; ++i) {
      const uint16_t id_hi = g_packs[self_idx].deps[i].id_hi;
      const uint32_t trailer = g_packs[self_idx].deps[i].trailer;
      const std::string dep_name = g_packs[self_idx].deps[i].name;
      int32_t dep_id = 0;
      if (!dep_name.empty())
        dep_id = rpak_open_unlocked(dep_name.c_str());  // @ 0x5442A7
      RpakRemapSlot& slot =
          g_packs[self_idx].remap_toc[static_cast<size_t>(i) + 1u];
      slot.id_hi = id_hi;
      slot.trailer = trailer;
      slot.pack_hi = static_cast<uint16_t>(dep_id);  // @ 0x5442AF
    }
  }
  return id;
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
  std::lock_guard<std::mutex> lock(g_mu);
  return rpak_open_unlocked(lib_path);
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

// Soft PE ResPack_RemapLocalId @ 0x544590 (ASM stride 66 @ 0x5445DB..E2).
uint32_t rpak_remap_local_id(int32_t pack_id, uint32_t local_id) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (static_cast<uint32_t>(pack_id) == 0xFFFFu) return local_id;  // @ 0x54459D
  const uint32_t lo = local_id & 0xFFFFu;                          // @ 0x5445A5
  const uint32_t hi = local_id >> 16;                              // @ 0x5445AB
  if (lo == 0) return 0;                                           // @ 0x5445B0
  // Soft stand-in for eng+0xFFE20 pack_count (@ 0x5445C0).
  const uint32_t eng_count = static_cast<uint32_t>(g_packs.size()) + 1u;
  if (hi >= eng_count) return 0;                                   // @ 0x5445C6
  if (hi == 0) return lo | (static_cast<uint32_t>(pack_id) << 16); // @ 0x5445F6

  const RpakPack* pack = nullptr;
  for (const auto& p : g_packs) {
    if (p.pack_id == pack_id) {
      pack = &p;
      break;
    }
  }
  if (!pack) return 0;
  const uint32_t lim = pack->toc_lim_a + pack->toc_lim_b;  // @ 0x5445CC
  if (hi > lim) return 0;                                 // @ 0x5445D4 ja
  if (hi >= pack->remap_toc.size()) return 0;
  const uint32_t ext_hi = pack->remap_toc[hi].pack_hi;     // table+66*hi+2
  return lo | (ext_hi << 16);                             // @ 0x5445EA
}

// Soft PE sourcefile load path: entry blob is often text
// "sourcefile <path>\r\n..." (EnsureIndex TOC off/size → File_SlotRead of
// pack; consumers Resource_loadSourcefilePayload @ 0x538840 /
// ResourceRef.load / makeTexture@47FFE0 upload parse the lines).
// Host returns the raw TOC payload — offsets now PE-accurate.
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
  if (!(static_cast<bool>(in) ||
        in.gcount() == static_cast<std::streamsize>(ent->size))) {
    out->clear();
    return false;
  }
  return true;
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
      const std::string want = stem(dep.name);
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
