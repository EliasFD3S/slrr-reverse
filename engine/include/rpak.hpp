#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace inv {

struct RpakEntry {
  bool is_dir = false;
  // Parent resource local id (or filetype when high bits set, e.g. 0x10008).
  int32_t kind = 0;
  // Resource local id (u32 after kind in the entry header). Catalog RIDs use this.
  int32_t type_id = 0;
  std::string name;
  std::string path;
  uint32_t offset = 0;
  uint32_t size = 0;
  // Tree links store resource local ids (type_id), not table indices.
  int32_t parent_local = -1;
  int32_t first_child_local = -1;
  int32_t next_sibling_local = -1;
};

struct RpakPack {
  // Small integer pack id (1, 2, …). Full resource ids are (pack_id<<16)|local.
  // Matches Catalog: (parts.id() >> 16) == System.openLib(...).
  int32_t pack_id = 0;
  std::string path;
  std::string name;
  uint32_t version = 0;
  std::vector<std::string> deps;
  std::vector<RpakEntry> entries;
  bool is_registry = false;
  bool parsed_entries = false;
};

void rpak_set_game_root(const char* root);

// Resolve lib/asset path against game root (and CWD fallback).
std::string rpak_resolve_path(const char* path);

// Load/open a .rpk. Returns pack_id (>0) or 0 on failure. Idempotent.
int32_t rpak_open(const char* lib_path);

const RpakPack* rpak_get(int32_t pack_id);
const RpakPack* rpak_find_by_name(const char* basename);  // "frontend.rpk" or "frontend"
size_t rpak_count();

// Decode resource id = (pack_id << 16) | type_id (entry.type_id).
const RpakEntry* rpak_find_entry(int32_t res_id);
const RpakPack* rpak_find_pack_for_res(int32_t res_id);

// Parent key for hierarchy: scripted car nodes use kind=0x2xxxx → parent is low 16 bits
// (e.g. Baiern_VT kind=0x21000 → parent cars:0x1000).
inline int32_t rpak_parent_key(int32_t kind) {
  if ((static_cast<uint32_t>(kind) >> 16) == 0x2u)
    return kind & 0xFFFF;
  return kind;
}

// Cross-pack world-tree walk (VehicleType roots live in racer packs under cars:0x1000).
int32_t rpak_first_child_id(int32_t parent_res_id);
int32_t rpak_next_sibling_id(int32_t child_res_id);
int32_t rpak_parent_id(int32_t child_res_id);

// Read raw entry bytes (re-opens pack file). Returns false if missing/dir/empty.
bool rpak_read_entry(int32_t res_id, std::vector<uint8_t>* out);

inline int32_t rpak_make_id(int32_t pack_id, uint16_t local) {
  return (pack_id << 16) | static_cast<int32_t>(local);
}
inline int32_t rpak_id_pack(int32_t res_id) { return (res_id >> 16) & 0xFFFF; }
inline uint16_t rpak_id_local(int32_t res_id) {
  return static_cast<uint16_t>(res_id & 0xFFFF);
}

}  // namespace inv
