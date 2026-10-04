#include "host_objects.hpp"
#include "runtime.hpp"
#include "rpak.hpp"
#include "tree_interp.hpp"
#include "input_win32.hpp"
#include "game_script.hpp"
#include "jvm.hpp"
#include "render_d3d9.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace inv {
namespace {

std::recursive_mutex g_mu;

// Soft PE File blob remount tables (stock 0x14C blob):
//   remount ≈ dllist @ blob+0x14 — pack-local write/readResID + SDAT trailer
//   unsaved ≈ dllist @ blob+0x34 — HIWORD id == 0xFFFF path
struct FileRemountPack {
  int32_t ordinal = 0;   // node[+0xC]; wire HIWORD on pack-local write
  int32_t pack_key = 0;  // node[+0x10]; GetPackSlot+8 / LoadPack slot idx
  std::string path;      // node[+0x14]; pack path for SDAT trailer remount
};
struct FileUnsavedRes {
  int32_t ordinal = 0;   // node[+0xC]; wire dword on unsaved write
  int32_t abs_id = 0;    // node[+0x1C]; readResID absolute restore
  int32_t restype = 0;   // node[+0x10]; type gate residue (MsgBox OOS)
};
struct FileState {
  std::string path;
  FILE* fp = nullptr;
  int mode = 0;
  std::vector<FileRemountPack> remount;
  std::vector<FileUnsavedRes> unsaved;
};

struct ThreadState {
  std::string name;
  int priority = 0;
  int daemon = 0;
  bool alive = false;
  bool suspended = false;
  // PE Native.ptr @ dword_62E008 → VMThread blob (malloc 56 / vmthread_init).
  // Soft: host VmThread* for flag/prio/daemon + green-list link; Runnable.run
  // still OS-detaches (LoadingScreen/HotkeyWatcher block — pump would deadlock).
  VmThread* thr = nullptr;
};

// Soft PE Native.ptr store: truncate host VmThread* into TREE int (stock 32-bit).
int32_t thread_ptr_tag(VmThread* thr) {
  if (!thr) return 0;
  return static_cast<int32_t>(reinterpret_cast<std::uintptr_t>(thr));
}

void thread_drop_vm(ThreadState* st, InvObject* self) {
  if (!st || !st->thr) return;
  vmthread_request_stop(st->thr);
  st->thr->pe.java_backref = 0;
  vmthread_destroy(st->thr);
  st->thr = nullptr;
  if (self) tree_field_set_int(self, "ptr", 0);
}

// Soft ensure PE-shaped VMThread for start() when TREE skipped Thread.init
// (LoadingScreen.show arms start without ctor init).
VmThread* thread_ensure_vm(ThreadState* st, InvObject* self) {
  if (!st) return nullptr;
  if (st->thr) return st->thr;
  VmThread* thr =
      vmthread_init(jvm_active(), /*priority=*/0, /*sync_flags=*/2,
                    st->name.empty() ? nullptr : st->name.c_str());
  if (!thr) return nullptr;
  thr->pe.java_backref =
      static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(self));
  st->thr = thr;
  st->priority = thr->pe.priority;
  if (self) tree_field_set_int(self, "ptr", thread_ptr_tag(thr));
  return thr;
}

struct FindState {
  int flags = 0;
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
  WIN32_FIND_DATAA data{};
  bool has = false;
#else
  DIR* dir = nullptr;
  std::string pattern;
#endif
};

std::unordered_map<InvObject*, FileState> g_files;
std::unordered_map<InvObject*, ThreadState> g_threads;
std::unordered_map<InvObject*, FindState> g_finds;

// device -> axis -> value (physical: DIK on dev0, mouse axes 0..4 on dev1)
std::unordered_map<int32_t, std::unordered_map<int32_t, float>> g_axes;
// Previous sample for Input_activeAxis @ 0x00557AA0 analog-slam edge.
std::unordered_map<int32_t, std::unordered_map<int32_t, float>> g_axis_prev;
// Soft PE Input_mouseSensScale @ 0x00777438 (default 0.5 = 0x3F000000).
// Written by Input_readPhysicalAxis mouse branch when
// (unsigned)axis > 0xFFFFFF9A (-102): scale = (-1-axis)*0.01.
// OptionsDialog/Track: getAxis(1, -1-(Config.mouseSensitivity*100)).
// PE pollDevices multiplies relative mouse accum by this; Soft stores here
// (input_win32 g_input_mouse_axis_scale is file-local — not linked).
float g_mouse_sens_scale = 0.5f;
// Soft Input.cursor() singleton — MouseCursor.enable / getAxis sens bridge.
InvObject* g_input_cursor = nullptr;
int32_t g_last_key = 0;
int32_t g_held_dik = 0;
std::deque<int32_t> g_key_queue;
// PE Input_cheatRing @ 0x00640924, size 0x10; ptr @ 0x00612C68, end @ 0x00640934.
constexpr int kCheatRing = 16;
char g_cheat_ring[kCheatRing]{};
int g_cheat_wp = 0;
std::string g_cheat_buf;  // linearized snapshot for input_cheat_buffer()

char dik_to_lower(int32_t dik) {
  switch (dik) {
    case 0x1e: return 'a';
    case 0x30: return 'b';
    case 0x2e: return 'c';
    case 0x20: return 'd';
    case 0x12: return 'e';
    case 0x21: return 'f';
    case 0x22: return 'g';
    case 0x23: return 'h';
    case 0x17: return 'i';
    case 0x24: return 'j';
    case 0x25: return 'k';
    case 0x26: return 'l';
    case 0x32: return 'm';
    case 0x31: return 'n';
    case 0x18: return 'o';
    case 0x19: return 'p';
    case 0x10: return 'q';
    case 0x13: return 'r';
    case 0x1f: return 's';
    case 0x14: return 't';
    case 0x16: return 'u';
    case 0x2f: return 'v';
    case 0x11: return 'w';
    case 0x2d: return 'x';
    case 0x15: return 'y';
    case 0x2c: return 'z';
    default: return 0;
  }
}

void cheat_append_ascii(char c) {
  if (!c) return;
  g_cheat_ring[g_cheat_wp] = c;
  g_cheat_wp = (g_cheat_wp + 1) % kCheatRing;
}

struct AxisMap {
  InvObject* inst = nullptr;
  int32_t vaxis = 0;
  int32_t device = 0;
  int32_t paxis = 0;
  float i_from = 0.f;
  float i_to = 1.f;
  float l_from = 0.f;
  float l_to = 1.f;
};
// PE Input_mapAxis_add @ 0x0054D650: 152 slots, empty when vaxis==0 (AXIS_NULL).
constexpr int32_t kAxisMapSlotCap = 152;
std::vector<AxisMap> g_axis_maps;

struct AxisForce {
  InvObject* inst = nullptr;
  int32_t vaxis = 0;
  float value = 0.f;
};
std::vector<AxisForce> g_axis_forces;

// Phase 2.107 — VirtualAxisSmoothProperties → rate-limited logical filter.
struct AxisSmooth {
  InvObject* inst = nullptr;
  int32_t vaxis = 0;
  float center_range = 0.1f;
  float factor_center = 1.f;
  float factor_opposite = 1.f;
  float factor_same = 1.f;
  float power = 1.f;
  float speed_mul = 1.f;  // Phase 2.108 — user_SetAxisSpeed
  float filtered = 0.f;
  bool has_t = false;
  std::chrono::steady_clock::time_point last_t{};
};
std::vector<AxisSmooth> g_axis_smooth;

float remap_axis(float v, float i0, float i1, float l0, float l1) {
  const float den = i1 - i0;
  if (den > -1e-8f && den < 1e-8f) return l0;
  float t = (v - i0) / den;
  if (t < 0.f) t = 0.f;
  if (t > 1.f) t = 1.f;
  return l0 + t * (l1 - l0);
}

float axis_raw_unlocked(int32_t device, int32_t axis) {
  auto dit = g_axes.find(device);
  if (dit == g_axes.end()) return 0.f;
  auto ait = dit->second.find(axis);
  return ait == dit->second.end() ? 0.f : ait->second;
}

FileState* file_state(InvObject* self) {
  auto it = g_files.find(self);
  return it == g_files.end() ? nullptr : &it->second;
}

std::string file_path_of(InvObject* f) {
  if (!f) return {};
  if (FileState* st = file_state(f)) {
    if (!st->path.empty()) return st->path;
  }
  // PE File.open reads Java field File.name (java.lang.String) then Native.ptr.
  InvObject* name = tree_field_get_obj(f, "name");
  const char* ns = string_cstr(name);
  if (ns && ns[0]) return std::string(ns);
  const char* s = string_cstr(f);
  return (s && s[0]) ? std::string(s) : std::string{};
}

std::string file_norm_slashes(std::string path) {
  for (char& c : path) {
    if (c == '/') c = '\\';
  }
  return path;
}

// Soft stand-in for File_FlushHandlesForPath + open-slot close under File_SlotCs
// (File_copyPaths @ 0x0054C550 / File_movePaths @ 0x0054C650 / File_PathDelete).
void file_soft_flush_open_path(const std::string& norm) {
  if (norm.empty()) return;
  for (auto& kv : g_files) {
    FileState& st = kv.second;
    if (!st.fp || st.path.empty()) continue;
    if (file_norm_slashes(st.path) == norm) {
      std::fclose(st.fp);
      st.fp = nullptr;
    }
  }
}

bool file_write_u32_le(FILE* fp, uint32_t v) {
  if (!fp) return false;
  unsigned char b[4] = {
      static_cast<unsigned char>(v & 0xFF),
      static_cast<unsigned char>((v >> 8) & 0xFF),
      static_cast<unsigned char>((v >> 16) & 0xFF),
      static_cast<unsigned char>((v >> 24) & 0xFF),
  };
  return std::fwrite(b, 1, 4, fp) == 4;
}

// Soft File_SlotWrite analogue — PE returns byte count / -1; blob/slot0 → 0.
int32_t file_soft_slot_write(FileState* st, const void* buf, size_t n) {
  if (!st || !st->fp) return 0;
  if (std::fwrite(buf, 1, n, st->fp) != n) return -1;
  return static_cast<int32_t>(n);
}

#ifdef _WIN32
// PE File_OpenCreate @ 0x0054CAF0 / File_CopyFileWithMkdir @ 0x00558A50 /
// File_Win32Move @ 0x00558BA0: CreateDirectoryA on each '\\'/'/' prefix;
// ERROR_ALREADY_EXISTS=183 ignored. Soft: no FormatMessage / MsgBox.
void file_mkdir_parents(const char* path) {
  if (!path || !path[0]) return;
  char buf[260];
  size_t n = 0;
  for (const char* p = path; *p && n + 1 < sizeof(buf); ++p) {
    buf[n++] = *p;
    if (*p == '\\' || *p == '/') {
      buf[n] = 0;
      // PE ignores ERROR_ALREADY_EXISTS=183; Soft: ignore all CreateDir fails.
      CreateDirectoryA(buf, nullptr);
    }
  }
}

// Soft stand-in for File_CopyFileWithMkdir @ 0x00558A50 / File_Win32Move
// @ 0x00558BA0 ERROR_PATH_NOT_FOUND=3 branch: mkdir parents then retry.
bool file_mkdir_parents_on_path_not_found(const char* dst) {
  if (GetLastError() != ERROR_PATH_NOT_FOUND) return false;
  file_mkdir_parents(dst);
  return true;
}
#endif

bool file_read_u32_le(FILE* fp, uint32_t* out) {
  if (!fp || !out) return false;
  unsigned char b[4] = {};
  if (std::fread(b, 1, 4, fp) != 4) return false;
  *out = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
  return true;
}

// Soft PackFile_PathExists @ 0x005542D0 — stock UseTree@7686E0 is never set
// (live path = Win32). Soft: basename already open via rpak (openLib/LoadPack).
bool soft_packfile_path_exists(const char* path) {
  if (!path || !path[0]) return false;
  const char* base = path;
  for (const char* p = path; *p; ++p) {
    if (*p == '\\' || *p == '/') base = p + 1;
  }
  if (!base[0]) return false;
  return rpak_find_by_name(base) != nullptr;
}

// Soft skip File_SdatType2_RemountTextures @ 0x00485190 (texture link OOS).
// Per entry: u32 id, u32 kind, u32 size; kind==7 && size>0 → size payload bytes.
bool file_sdat_soft_skip_type2_textures(FILE* fp, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t id = 0, kind = 0, size = 0;
    if (!file_read_u32_le(fp, &id) || !file_read_u32_le(fp, &kind) ||
        !file_read_u32_le(fp, &size)) {
      return false;
    }
    (void)id;
    if (kind == 7 && size > 0) {
      if (size > 0x1000000u) return false;
      if (std::fseek(fp, static_cast<long>(size), SEEK_CUR) != 0) return false;
    }
  }
  return true;
}

// Soft File_Sdat_RemountPackList @ 0x00484F00 / inline @ File.open type1/2:
// records {u32 ordinal@node+0xC, u32 path_len, path[path_len]} →
// ResourceEngine_LoadPack @ 0x00538380 → node[+0x10]=packIdx.
// LoadPack is the SAME callee System.openLib @ 0x00487BE0 uses first
// (openLib then GetPackSlot + ResHandle_Relink local:1; File.open stops
// at LoadPack index into SimCallbackNode[+0x10]).
// PATH-TO-WORLD: SDAT remount is the sync pack-stream hook that feeds
// world resource IDs (readResID HIWORD←pack_key) before Java cursor @+8.
// Soft: rpak_open ≈ LoadPack+EnsureIndex; path-eq early-out via
// rpak_find_by_name (LoadPack PathEq @ 0x543FE0) before open.
void file_sdat_soft_remount_packs(FileState* st, FILE* fp, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t ordinal = 0, path_len = 0;
    if (!file_read_u32_le(fp, &ordinal) || !file_read_u32_le(fp, &path_len))
      return;
    if (path_len == 0 || path_len > 255) return;
    char path[256] = {};
    if (std::fread(path, 1, path_len, fp) != path_len) return;
    path[path_len < 255 ? path_len : 255] = '\0';
    int32_t pack_key = 0;
    if (path[0]) {
      // Soft LoadPack path-eq: basename already open → reuse pack_id.
      const char* base = path;
      for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') base = p + 1;
      }
      if (const RpakPack* open = rpak_find_by_name(base)) {
        pack_key = open->pack_id;
      } else {
        // Soft ≈ LoadPack @ 0x538380 (PathEq miss → InitSlot + count++).
        pack_key = rpak_open(path);
      }
    }
    if (st) {
      FileRemountPack node;
      node.ordinal = static_cast<int32_t>(ordinal);
      node.pack_key = pack_key;  // PE node[+0x10]; readResID HIWORD remap
      node.path = path;
      st->remount.push_back(std::move(node));
    }
  }
}

// PE File.open type@hdr[6]==1|2 trailer remount before BEGIN+8 Java cursor.
// type1 @ 0x4856D7: END-8 → {remount_bytes, count}; seek END-8-bytes; packs.
// type2 @ 0x4855BD: END-4 → trailer_len; seek END-4-len; soft-skip textures
// @ 485190; then pack count + remount. Always restores caller to +8 after.
// Soft: rebuild FileState.remount from trailer (readResID pack remap).
void file_sdat_soft_loadpack_remount(FileState* st, FILE* fp, unsigned type) {
  if (!fp || (type != 1 && type != 2)) return;
  if (st) {
    st->remount.clear();
    st->unsaved.clear();
  }
  if (type == 1) {
    if (std::fseek(fp, -8, SEEK_END) != 0) return;
    uint32_t remount_bytes = 0, count = 0;
    if (!file_read_u32_le(fp, &remount_bytes) ||
        !file_read_u32_le(fp, &count)) {
      return;
    }
    if (count == 0) return;
    if (std::fseek(fp, -8 - static_cast<long>(remount_bytes), SEEK_END) != 0)
      return;
    file_sdat_soft_remount_packs(st, fp, count);
    return;
  }
  // type 2
  if (std::fseek(fp, -4, SEEK_END) != 0) return;
  uint32_t trailer = 0;
  if (!file_read_u32_le(fp, &trailer) || trailer < 8) return;
  if (std::fseek(fp, -4 - static_cast<long>(trailer), SEEK_END) != 0) return;
  uint32_t tex_count = 0;
  if (!file_read_u32_le(fp, &tex_count)) return;
  if (!file_sdat_soft_skip_type2_textures(fp, tex_count)) return;
  uint32_t pack_count = 0;
  if (!file_read_u32_le(fp, &pack_count) || pack_count == 0) return;
  file_sdat_soft_remount_packs(st, fp, pack_count);
}

// Soft File.close mode==1 + aSdat[6]==2 @ 0x00485870: append type2 trailer
// {unsaved_count, File_SdatType2_WriteUnsavedList@484FA0 (Soft size0 rows),
//  pack_count, remount{ordinal,path_len,path}, trailer_len} then SlotClose.
// type1 Soft: remount bytes+count only. MsgBox / invalid-len OOS.
bool file_sdat_soft_flush_trailer(FileState* st) {
  if (!st || !st->fp) return false;
  // Stock create writes aSdat type byte[6]=2 @ 0x00612C76 — Soft always type2.
  uint32_t trailer = 0;
  const uint32_t unsaved_count =
      static_cast<uint32_t>(st->unsaved.size());
  if (!file_write_u32_le(st->fp, unsaved_count)) return false;
  trailer += 4;
  // Soft File_SdatType2_WriteUnsavedList: {ordinal, restype, size=0} per node
  // (kind==7 payload OOS — size0 keeps open skip path linear).
  for (const FileUnsavedRes& u : st->unsaved) {
    if (!file_write_u32_le(st->fp, static_cast<uint32_t>(u.ordinal)) ||
        !file_write_u32_le(st->fp, static_cast<uint32_t>(u.restype)) ||
        !file_write_u32_le(st->fp, 0u)) {
      return false;
    }
    trailer += 12;
  }
  const uint32_t pack_count = static_cast<uint32_t>(st->remount.size());
  if (!file_write_u32_le(st->fp, pack_count)) return false;
  trailer += 4;
  for (const FileRemountPack& r : st->remount) {
    const uint32_t plen =
        r.path.empty() ? 0u
                       : static_cast<uint32_t>(r.path.size() + 1);
    if (!file_write_u32_le(st->fp, static_cast<uint32_t>(r.ordinal)) ||
        !file_write_u32_le(st->fp, plen)) {
      return false;
    }
    if (plen > 0) {
      if (std::fwrite(r.path.c_str(), 1, plen, st->fp) != plen) return false;
    }
    trailer += 8 + plen;
  }
  if (!file_write_u32_le(st->fp, trailer)) return false;
  return trailer >= 8;
}

}  // namespace

InvObject* file_new(const char* path) {
  auto* obj = reinterpret_cast<InvObject*>(new InvString{nullptr});
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  g_files[obj] = FileState{path ? path : "", nullptr, 0};
  return obj;
}

InvObject* findfile_new() {
  auto* obj = reinterpret_cast<InvObject*>(new InvString{nullptr});
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  g_finds[obj] = FindState{};
  return obj;
}

InvObject* thread_new(const char* name) {
  // Soft host helper (not a PE native). Mirrors Thread.init: side-table +
  // vmthread_init(prio=0, flags=2) so start() finds an armed Native.ptr.
  auto* obj = reinterpret_cast<InvObject*>(new InvString{nullptr});
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  ThreadState st;
  st.name = name ? name : "thread";
  st.priority = 0;
  VmThread* thr =
      vmthread_init(jvm_active(), /*priority=*/0, /*sync_flags=*/2,
                    st.name.c_str());
  if (thr) {
    thr->pe.java_backref =
        static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(obj));
    st.thr = thr;
    tree_field_set_int(obj, "ptr", thread_ptr_tag(thr));
  }
  g_threads[obj] = std::move(st);
  return obj;
}

void input_set_axis(int32_t device, int32_t axis, float value) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  g_axes[device][axis] = value;
}

void input_set_last_key(int32_t key, bool edge_enqueue) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  // Edge-trigger into queue so smoke input_set_last_key sequences drain via lastKey.
  if (edge_enqueue && key != 0 && key != g_held_dik) g_key_queue.push_back(key);
  g_held_dik = key;
  g_last_key = key;
}

void input_cheat_clear() {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  std::memset(g_cheat_ring, 0, sizeof(g_cheat_ring));
  g_cheat_wp = 0;
  g_cheat_buf.clear();
}

const char* input_cheat_buffer() {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  g_cheat_buf.assign(g_cheat_ring, g_cheat_ring + kCheatRing);
  return g_cheat_buf.c_str();
}

int32_t input_cheat_try_match_encoded(const char* enc) {
  // PE 0x0047CB50: encoded[c]-1 vs ring walking backward from write ptr.
  if (!enc) return 0;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  const int len = static_cast<int>(std::strlen(enc));
  int idx = g_cheat_wp;
  int left = len;
  const char* p = enc + len;
  if (len != 0) {
    do {
      idx -= 1;
      if (idx < 0) idx += kCheatRing;
      --p;
      if (static_cast<unsigned char>(*p - 1) !=
          static_cast<unsigned char>(g_cheat_ring[idx]))
        break;
      --left;
    } while (left != 0);
  }
  if (left != 0) return 0;
  int last = g_cheat_wp - 1;
  if (last < 0) last += kCheatRing;
  g_cheat_ring[last] = 0;
  return 1;
}

int32_t input_dik_from_letter(char letter) {
  const char c = (letter >= 'A' && letter <= 'Z')
                     ? static_cast<char>(letter - 'A' + 'a')
                     : letter;
  for (int32_t dik = 0x10; dik <= 0x32; ++dik) {
    if (dik_to_lower(dik) == c) return dik;
  }
  return 0;
}

// PE Input_mapAxis_add @ 0x0054D650 return codes (also Controller.user_Add).
static int32_t input_map_add_pe(InvObject* inst, int32_t vaxis, int32_t device,
                                int32_t paxis, float i_from, float i_to,
                                float l_from, float l_to) {
  // PE Input_mapAxis_add @ 0x0054D650 (thiscall ecx=Input*+0x1C map table):
  // device/paxis<0 → ret -1; scan 152 slots @ this+913×4 stride 8 dwords for
  // dword[vaxis]==0 (AXIS_NULL); table full → ret 0; else store vaxis/device/
  // paxis, snapshot phys @ slot+3 via Input_readPhysicalAxis @ 0x00557430,
  // i_from..l_to, analogFlag=Input_physAxisIsAnalog @ 0x00557990 on vaxis
  // prop this+12*vaxis+2; ++*this (count); ret slot+1. No upsert — Del @
  // 0x0054D700 zeros first (vaxis,device,paxis) match only.
  if (device < 0 || paxis < 0) return -1;
  if (!inst) return 0;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  int32_t n = 0;
  for (const AxisMap& m : g_axis_maps) {
    if (m.inst == inst) ++n;
  }
  if (n >= kAxisMapSlotCap) return 0;
  g_axis_maps.push_back(
      AxisMap{inst, vaxis, device, paxis, i_from, i_to, l_from, l_to});
  return n + 1;
}

void input_map_add(InvObject* inst, int32_t vaxis, int32_t device, int32_t paxis,
                   float i_from, float i_to, float l_from, float l_to) {
  (void)input_map_add_pe(inst, vaxis, device, paxis, i_from, i_to, l_from,
                         l_to);
}

// PE Input_mapAxis_del @ 0x0054D700 (also Controller.user_Del tail).
static int32_t input_map_del_pe(InvObject* inst, int32_t vaxis, int32_t device,
                                int32_t paxis) {
  // thiscall ecx=Input*+0x1C: scan 152 slots @ this+913×4 stride 8 dwords;
  // first (vaxis,device,paxis) match → dword[vaxis]=0 (AXIS_NULL), --*count;
  // no further slots. No match → no-op.
  if (!inst) return 0;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  for (size_t i = 0; i < g_axis_maps.size(); ++i) {
    const AxisMap& m = g_axis_maps[i];
    if (m.inst == inst && m.vaxis == vaxis && m.device == device &&
        m.paxis == paxis) {
      g_axis_maps.erase(g_axis_maps.begin() + static_cast<std::ptrdiff_t>(i));
      return 1;
    }
  }
  return 0;
}

int32_t input_map_del(InvObject* inst, int32_t vaxis, int32_t device,
                      int32_t paxis) {
  return input_map_del_pe(inst, vaxis, device, paxis);
}

void input_map_reset(InvObject* inst) {
  // PE Input_mapAxis_reset @ 0x0054D750 (ecx = Input object +0x1C):
  // zero 152 map slots (this+913×4, stride 8 dwords), *count=0, then
  // reinit 76 vaxis property records (defaults: cr=0.1, factors=1.0,
  // force fields 0). Host: drop maps/forces/smooth for this Controller.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  for (size_t i = 0; i < g_axis_maps.size();) {
    if (g_axis_maps[i].inst == inst)
      g_axis_maps.erase(g_axis_maps.begin() + static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
  for (size_t i = 0; i < g_axis_forces.size();) {
    if (g_axis_forces[i].inst == inst)
      g_axis_forces.erase(g_axis_forces.begin() +
                          static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
  for (size_t i = 0; i < g_axis_smooth.size();) {
    if (g_axis_smooth[i].inst == inst)
      g_axis_smooth.erase(g_axis_smooth.begin() +
                          static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
}

void input_map_set_force(InvObject* inst, int32_t vaxis, float value) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  for (AxisForce& f : g_axis_forces) {
    if (f.inst == inst && f.vaxis == vaxis) {
      f.value = value;
      return;
    }
  }
  g_axis_forces.push_back(AxisForce{inst, vaxis, value});
}

void input_map_clear_force(InvObject* inst, int32_t vaxis) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  for (size_t i = 0; i < g_axis_forces.size();) {
    if (g_axis_forces[i].inst == inst && g_axis_forces[i].vaxis == vaxis)
      g_axis_forces.erase(g_axis_forces.begin() +
                          static_cast<std::ptrdiff_t>(i));
    else
      ++i;
  }
}

void input_map_set_smooth(InvObject* inst, int32_t vaxis, float center_range,
                          float factor_center, float factor_opposite,
                          float factor_same, float power) {
  if (!inst) return;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  for (AxisSmooth& s : g_axis_smooth) {
    if (s.inst == inst && s.vaxis == vaxis) {
      s.center_range = center_range;
      s.factor_center = factor_center;
      s.factor_opposite = factor_opposite;
      s.factor_same = factor_same;
      s.power = (power > 0.01f) ? power : 1.f;
      s.filtered = 0.f;
      s.has_t = false;
      return;
    }
  }
  AxisSmooth s;
  s.inst = inst;
  s.vaxis = vaxis;
  s.center_range = center_range;
  s.factor_center = factor_center;
  s.factor_opposite = factor_opposite;
  s.factor_same = factor_same;
  s.power = (power > 0.01f) ? power : 1.f;
  g_axis_smooth.push_back(s);
}

void input_map_set_speed(InvObject* inst, int32_t vaxis, float speed) {
  // PE Input_setLogicalAxisSpeed @ 0x0054D810: store raw float at
  // map+48*vaxis+0x20 — no clamp. Host speed_mul feeds readLogical.
  if (!inst) return;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  for (AxisSmooth& s : g_axis_smooth) {
    if (s.inst == inst && s.vaxis == vaxis) {
      s.speed_mul = speed;
      return;
    }
  }
  // No smooth yet — create a default filter so speed still applies.
  AxisSmooth s;
  s.inst = inst;
  s.vaxis = vaxis;
  s.factor_center = 2.f;
  s.factor_opposite = 4.f;
  s.factor_same = 2.f;
  s.speed_mul = speed;
  g_axis_smooth.push_back(s);
}

float input_map_get_logical(InvObject* inst, int32_t vaxis) {
  // PE Controller.user_GetAxisVal @ 0x00477A80 → Input_readLogicalAxis
  // @ 0x0054D830: read mapped axis only. Does NOT pump Cursor_tick.
  // Host used to call input_live_poll() here; that re-entered tickSysCursor
  // from physics_drive (Valocity simulate) and aborted the process.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  for (const AxisForce& f : g_axis_forces) {
    if (f.inst == inst && f.vaxis == vaxis) return f.value;
  }
  float sum = 0.f;
  for (const AxisMap& m : g_axis_maps) {
    if (m.inst != inst || m.vaxis != vaxis) continue;
    const float raw = axis_raw_unlocked(m.device, m.paxis);
    sum += remap_axis(raw, m.i_from, m.i_to, m.l_from, m.l_to);
  }
  AxisSmooth* sm = nullptr;
  for (AxisSmooth& s : g_axis_smooth) {
    if (s.inst == inst && s.vaxis == vaxis) {
      sm = &s;
      break;
    }
  }
  if (!sm) return sum;

  float target = sum;
  if (sm->power > 0.01f && std::fabs(sm->power - 1.f) > 0.01f) {
    const float sign = target < 0.f ? -1.f : 1.f;
    float mag = std::fabs(target);
    if (mag > 1.f) mag = 1.f;
    target = sign * std::pow(mag, sm->power);
  }

  const auto now = std::chrono::steady_clock::now();
  float dt = 0.05f;
  if (sm->has_t) {
    dt = std::chrono::duration<float>(now - sm->last_t).count();
    if (dt < 0.f) dt = 0.f;
    if (dt > 0.25f) dt = 0.25f;
  }
  sm->last_t = now;
  sm->has_t = true;

  const float delta = target - sm->filtered;
  float rate = sm->factor_same;
  const float af = sm->filtered < 0.f ? -sm->filtered : sm->filtered;
  const bool toward_center =
      (sm->filtered > 0.f && delta < 0.f) || (sm->filtered < 0.f && delta > 0.f);
  if (af < sm->center_range && toward_center) {
    rate = sm->factor_center;
  } else if ((sm->filtered > 0.01f && target < -0.01f) ||
             (sm->filtered < -0.01f && target > 0.01f)) {
    rate = sm->factor_opposite;
  }
  if (rate < 0.f) rate = 0.f;
  rate *= sm->speed_mul;
  const float step = rate * dt;
  if (delta > step)
    sm->filtered += step;
  else if (delta < -step)
    sm->filtered -= step;
  else
    sm->filtered = target;
  return sm->filtered;
}

int32_t input_map_count(InvObject* inst) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  int32_t n = 0;
  for (const AxisMap& m : g_axis_maps) {
    if (m.inst == inst) ++n;
  }
  return n;
}

// ---- File ----

int32_t java_io_File_open(InvObject* self, int32_t mode) {
  // PE @ 0x00485440 size 0x423. Unbox this+I. Path = File.name
  // (java.lang.String Native.ptr). Native.ptr (dword_62E008)==0 →
  // Engine_malloc 0x14C blob + two SimObjectLists. Close prior slot
  // (File_SlotClose cluster). Store mode blob[+0x48], path [+0x4C]
  // cap 0x100 via Util_strncpy_n.
  // mode==0 → File_OpenRead @ 0x0054C880 ('/'→'\\', CreateFile read).
  // mode==1 → File_OpenCreate @ 0x0054CAF0 + File_SlotWrite 8 aSdat
  // @ 0x00612C70 = {53 44 41 54 00 01 02 00} (type byte[6]=2).
  // else Engine_ErrorLogMsgBox "vm_file_open: invalid file open mode".
  // Open-read: File_SlotRead 8 hdr; type=byte[6] (v24). type==1|2 →
  // trailer remount → ResourceEngine_LoadPack @ 0x00538380 per path
  // (same LoadPack as System.openLib @ 0x00487BE0; Soft: rpak_open /
  // path-eq early-out in file_sdat_soft_remount_packs; SimCallbackNode
  // dllist OOS).
  // Always File_SlotSeek @ 0x0054CE40(slot, origin0, +8) → Java at +8.
  // Fail → free blob, Native.ptr=0, return 0. Success BOOL 1. Never -1.
  // PATH-TO-WORLD Soft: CRT fopen; SDAT type1/2 soft remount via
  // file_sdat_soft_loadpack_remount → rpak_open (LoadPack@538380); SDAT→+8;
  // non-SDAT rewind 0 (fixtures without SDAT — stock path is SDAT).
  // AsyncLoad/FileAsync ring live in System.cpp (MainLoop) — not this native.
  if (!self) return 0;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  FileState* st = file_state(self);
  if (!st) {
    g_files[self] = FileState{};
    st = file_state(self);
    if (!st) return 0;
  }
  if (st->fp) {
    std::fclose(st->fp);
    st->fp = nullptr;
  }
  st->mode = mode;
  st->remount.clear();
  st->unsaved.clear();
  std::string path = file_path_of(self);
  path = file_norm_slashes(std::move(path));
  const std::string resolved = rpak_resolve_path(path.c_str());
  const char* use = !resolved.empty() ? resolved.c_str() : path.c_str();
  if (!path.empty()) st->path = path;

  if (mode != 0 && mode != 1) return 0;

  if (mode == 1) {
#ifdef _WIN32
    file_mkdir_parents(use);
#endif
    st->fp = std::fopen(use, "wb");
    if (!st->fp) return 0;
    // PE aSdat @ 0x00612C70 — File_SlotWrite 8 raw bytes (not C strlen).
    static const unsigned char kSdat[8] = {0x53, 0x44, 0x41, 0x54, 0, 1, 2, 0};
    if (std::fwrite(kSdat, 1, 8, st->fp) != 8) {
      std::fclose(st->fp);
      st->fp = nullptr;
      return 0;
    }
    return 1;
  }

  st->fp = std::fopen(use, "rb");
  if (!st->fp) return 0;
  // PE File_SlotRead 8 → type@hdr[6]; type1/2 remount LoadPack; BEGIN+8.
  unsigned char hdr[8] = {};
  const size_t n = std::fread(hdr, 1, 8, st->fp);
  const bool sdat = n >= 4 && hdr[0] == 0x53 && hdr[1] == 0x44 &&
                    hdr[2] == 0x41 && hdr[3] == 0x54;
  if (sdat) {
    if (n != 8) {
      std::fclose(st->fp);
      st->fp = nullptr;
      return 0;
    }
    // Soft: type@hdr[6] ∈ {1,2} → trailer remount → rpak_open + remount table.
    const unsigned type = hdr[6];
    if (type == 1 || type == 2) file_sdat_soft_loadpack_remount(st, st->fp, type);
    if (std::fseek(st->fp, 8, SEEK_SET) != 0) {
      std::fclose(st->fp);
      st->fp = nullptr;
      return 0;
    }
  } else if (std::fseek(st->fp, 0, SEEK_SET) != 0) {
    std::fclose(st->fp);
    st->fp = nullptr;
    return 0;
  }
  return 1;
}

void java_io_File_close(InvObject* self) {
  // PE @ 0x00485870 size 0x2ab. Unbox this. Native.ptr==0 → ret (no free).
  // mode@blob[+0x48]==1 → flush SDAT trailer then File_SlotClose @ 0x0054CCA0:
  //   aSdat[6]==1: remount packs + {bytes,count}
  //   aSdat[6]==2: unsaved_count + File_SdatType2_WriteUnsavedList @ 0x00484FA0
  //                + pack_count + remount + trailer_len (fail if trailer<8)
  // mode!=1 / *blob==0 → skip write, still free remount lists + Engine_free
  // + Native.ptr=0. Soft: mode==1 CRT flush type2 trailer; fclose; drop state.
  // Texture payload / MsgBox / SimObjectList dtor OOS.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  FileState* st = file_state(self);
  if (st && st->fp) {
    if (st->mode == 1) {
      (void)file_sdat_soft_flush_trailer(st);
    }
    std::fclose(st->fp);
    st->fp = nullptr;
  }
  g_files.erase(self);
}

int32_t java_io_File_write(InvObject* self, int32_t value) {
  // PE @ 0x00485B20 size 0x4f write(I)I. Unbox this+int LE. Native.ptr
  // (dword_62E008)==0 or slot==0 → 0. Else File_SlotWrite 4 raw LE bytes
  // via WriteFile (not fwrite). Success → NumberOfBytesWritten (4);
  // slot∉1..31 / HANDLE null / WriteFile fail → -1. Host CRT analogue:
  // return byte count, not boolean 1/0. write(F)/write(String) other natives.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  FileState* st = file_state(self);
  if (!st || !st->fp) return 0;
  const size_t n = std::fwrite(&value, 1, 4, st->fp);
  if (n == 4) return 4;
  return -1;
}

int32_t java_io_File_write_1(InvObject* self, float value) {
  // PE @ 0x00485BC0 size 0x4f write(F)I. Same skeleton as write(I)
  // @ 0x00485B20: Unbox this+float LE. Native.ptr (dword_62E008)==0 or
  // slot==0 → 0. Else File_SlotWrite @ 0x0054CF00(*blob,&f,4) → byte
  // count 4 / -1. Ticket race115 VA 0x00485FD0 is write(String); this
  // is the (F)I sibling. Host CRT: return polarity PE 4/-1/0.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  FileState* st = file_state(self);
  if (!st || !st->fp) return 0;
  const size_t n = std::fwrite(&value, 1, 4, st->fp);
  if (n == 4) return 4;
  return -1;
}

int32_t java_io_File_write_2(InvObject* self, InvObject* value) {
  // PE @ 0x00485C60 size 0x240 (int_convert 576). write(ResourceRef)I.
  // UnboxArg dest0=&var_8 this, dest1=&var_C ref (overwrites). blob =
  // JVM_vm_get_int_field(this, dword_62E008); blob==0 || *blob==0 → 0.
  // id = ref ? *(ref+8) : 0. id==0 → File_SlotWrite(*blob,&id,4).
  // HIWORD(id)==0xFFFF (unsaved): type gate on ref+0xC → +0x4C; if
  // type-5 ∈ {0,2} walk remount list blob+0x34 (v2[13]) match id at
  // node+0x1C else "!"+"ERROR! Unsaveable resource!" +
  // Engine_ErrorLogMsgBox → 0; hit/create → write ordinal.
  // Else (pack-local): GetPackSlot(id) @ 0x5383F0; walk/create node on
  // blob+0x14 list (276 B alloc), store local ordinal at node+0xC;
  // write (ordinal<<16)|LOWORD(id) via File_SlotWrite @ 0x54CF00.
  // Soft: FileState.remount/unsaved side-tables; GetPackSlot ≈
  // rpak_find_pack_for_res (pack_key=pack_id, path=pack.path); MsgBox OOS.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  FileState* st = file_state(self);
  if (!st || !st->fp) return 0;
  const int32_t id =
      value ? java_util_resource_ResourceRef_id(value) : 0;
  if (id == 0) {
    return file_soft_slot_write(st, &id, 4);
  }
  const uint32_t uid = static_cast<uint32_t>(id);
  const uint32_t hi = uid & 0xFFFF0000u;
  if (hi == 0xFFFF0000u) {
    // Unsaved: RESTYPE-5 ∈ {0,2} → type ∈ {5,7}.
    const int32_t restype =
        value ? java_util_resource_ResourceRef_type(value) : 0;
    const int32_t gate = restype - 5;
    if (gate != 0 && gate != 2) return 0;
    for (const FileUnsavedRes& u : st->unsaved) {
      if (u.abs_id == id) {
        return file_soft_slot_write(st, &u.ordinal, 4);
      }
    }
    FileUnsavedRes node;
    node.ordinal = static_cast<int32_t>(st->unsaved.size() + 1);
    node.abs_id = id;
    node.restype = restype;
    const int32_t ord = node.ordinal;
    st->unsaved.push_back(node);
    return file_soft_slot_write(st, &ord, 4);
  }
  // Pack-local: Soft GetPackSlot → rpak pack; match remount by pack_key.
  int32_t pack_key = static_cast<int32_t>((uid >> 16) & 0xFFFF);
  std::string pack_path;
  if (const RpakPack* pk = rpak_find_pack_for_res(id)) {
    pack_key = pk->pack_id;
    pack_path = pk->path;
  }
  for (const FileRemountPack& r : st->remount) {
    if (r.pack_key == pack_key) {
      const int32_t wired =
          (r.ordinal << 16) | static_cast<int32_t>(uid & 0xFFFF);
      return file_soft_slot_write(st, &wired, 4);
    }
  }
  FileRemountPack node;
  node.ordinal = static_cast<int32_t>(st->remount.size() + 1);
  node.pack_key = pack_key;
  node.path = std::move(pack_path);
  const int32_t wired =
      (node.ordinal << 16) | static_cast<int32_t>(uid & 0xFFFF);
  st->remount.push_back(std::move(node));
  return file_soft_slot_write(st, &wired, 4);
}

int32_t java_io_File_write_3(InvObject* self, InvObject* value) {
  // PE @ 0x00485FD0 size 0x90 (144). write(String)I. Unbox this+String
  // (JVM_UnboxArg @ 0x0045D910). blob=JVM_vm_get_int_field(this,
  // dword_62E008); blob==0 → 0. slot=*blob; slot==0 → 0. Else
  // len = cstr ? strlen(cstr)+1 : 0; File_SlotWrite(slot,&len,4);
  // if len>0 return File_SlotWrite(slot,cstr,len) (byte count / -1);
  // else return 0. Contrast write(I) @ 0x00485B20 / write(F)
  // @ 0x00485BC0 (raw 4 bytes). Host CRT stand-in: polarity PE.
  // PE @ 0x00485FD0
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  FileState* st = file_state(self);
  if (!st || !st->fp) return 0;
  const char* s = value ? string_cstr(value) : nullptr;
  int32_t len = s ? static_cast<int32_t>(std::strlen(s) + 1) : 0;
  if (std::fwrite(&len, 1, 4, st->fp) != 4) return -1;
  if (len <= 0) return 0;
  if (std::fwrite(s, 1, static_cast<size_t>(len), st->fp) !=
      static_cast<size_t>(len)) {
    return -1;
  }
  return len;
}

int32_t java_io_File_readInt(InvObject* self) {
  // PE @ 0x00485B70 size 0x46. Unbox this. blob=vm_get_int_field(dword_62E008).
  // blob==0 → 0. slot=*blob; slot!=0 → File_SlotRead @ 0x0054CEC0 →
  // FilePool_Read 4 raw LE (no bswap); ignores bytecount. slot==0:
  // leftover stack. Soft: !fp / short → 0. SDAT/+8 seek is File.open.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  FileState* st = file_state(self);
  if (!st || !st->fp) return 0;
  int32_t v = 0;
  if (std::fread(&v, 1, 4, st->fp) != 4) return 0;
  return v;
}

float java_io_File_readFloat(InvObject* self) {
  // PE @ 0x00485C10 size 0x4c. Same skeleton as readInt @ 0x00485B70:
  // Unbox this. blob=vm_get_int_field(dword_62E008).
  // blob==0 → fld flt_5E73CC (00 00 00 00) = 0.0, no crash.
  // slot=*blob; slot!=0 → File_SlotRead(slot,&v,4) = ReadFile 4 raw LE
  // IEEE754 float32 (no bswap). Ignores bytecount. slot==0: leftover stack.
  // Host: !fp → 0.f (blob==0 analogue). Short fread keeps leftover 0.f.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  FileState* st = file_state(self);
  if (!st || !st->fp) return 0.f;
  float v = 0.f;
  std::fread(&v, 1, 4, st->fp);
  return v;
}

int32_t java_io_File_readResID(InvObject* self) {
  // PE @ 0x00485EA0 size 0x12b (int_convert 299). Unbox this
  // (JVM_UnboxArg @ 0x0045D910). blob=JVM_vm_get_int_field(this,
  // dword_62E008); blob==0 || *blob==0 → 0. Else File_SlotRead
  // @ 0x0054CEC0(*blob,&id,4). id==0 → 0. hi=(id&0xFFFF0000):
  // if hi!=0 && hi!=0xFFFF walk remount list at blob+0x14 (v2[5]),
  // match HIWORD at node+0xC → return LOWORD|(node[+0x10]<<16);
  // miss → 0. Else (unsaved hi 0/0xFFFF) walk list at blob+0x34
  // (v2[13]), match LOWORD at node+0xC → return *[node+0x1C]; miss
  // → Engine_strcat_cap + "ERROR! Unsaved resource!" +
  // Engine_ErrorLogMsgBox → 0. Soft: FileState.remount/unsaved
  // (filled by write(ResourceRef) / open type1|2 trailer); MsgBox OOS.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  FileState* st = file_state(self);
  if (!st || !st->fp) return 0;
  int32_t wired = 0;
  if (std::fread(&wired, 1, 4, st->fp) != 4) return 0;
  if (wired == 0) return 0;
  const uint32_t uw = static_cast<uint32_t>(wired);
  const uint32_t hi = uw & 0xFFFF0000u;
  if (hi != 0 && hi != 0xFFFF0000u) {
    const int32_t ord = static_cast<int32_t>(hi >> 16);
    for (const FileRemountPack& r : st->remount) {
      if (r.ordinal == ord) {
        return static_cast<int32_t>(uw & 0xFFFF) | (r.pack_key << 16);
      }
    }
    return 0;
  }
  const int32_t ord = static_cast<int32_t>(uw & 0xFFFF);
  for (const FileUnsavedRes& u : st->unsaved) {
    if (u.ordinal == ord) return u.abs_id;
  }
  return 0;
}

InvObject* java_io_File_readString(InvObject* self) {
  // PE @ 0x00486060 size 0x8c (140). Unbox this. blob=vm_get_int_field
  // (dword_62E008); blob==0 || *blob==0 → null. Else File_SlotRead
  // @ 0x0054CEC0(*blob,&len,4); len==0 → null (not empty String).
  // Engine_malloc(len) @ 0x0054F560; File_SlotRead(*blob,buf,len);
  // JVM_String_from_cstr @ 0x004174A0(buf); Engine_free @ 0x0054F5B0.
  // write(String) @ 0x00485FD0 stores strlen+1 (NUL included). Soft:
  // !fp / short / len<=0 → null; force trailing NUL for safety.
  // PE @ 0x00486060
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  FileState* st = file_state(self);
  if (!st || !st->fp) return nullptr;
  int32_t len = 0;
  if (std::fread(&len, 1, 4, st->fp) != 4 || len <= 0) return nullptr;
  std::vector<char> buf(static_cast<size_t>(len));
  if (std::fread(buf.data(), 1, buf.size(), st->fp) != buf.size()) {
    return nullptr;
  }
  buf.back() = '\0';
  return string_new(buf.data());
}

int32_t java_io_File_delete(InvObject* f) {
  // PE @ 0x00486150 size 0x1f. Static delete(String): JVM_UnboxArg
  // (this dest nullptr) then File_PathDelete @ 0x0054C7C0 size 0xb9.
  // File_PathDelete: Engine_strcpy @ 0x00554860 into 0x100 stack,
  // 0x2f '/' → 0x5c '\\'. thiscall sub_5504C0(File_SlotCs @ 0x00766458,
  // timeout -1); sub_555550(path); scan File_OpenSlots @ 0x0076656C ..
  // File_OpenSlotsEnd @ 0x007685E0 stride 0x10C (31 slots) — if handle>=0
  // && byte@+0x108==0 && File_PathCmp @ 0x00554A10(slot+4, path, 0)==0 →
  // File_Win32Close @ 0x00559090 + mark -1; sub_5504F0(File_SlotCs);
  // return File_Win32Delete @ 0x00558CF0 size 0x8c (DeleteFileA ||
  // RemoveDirectoryA → 0; else GetLastError 2/3 silent -1, else
  // FormatMessageA 0x1300 + Engine_ErrorLogMsgBox -1).
  // Soft: flush matching FILE* then Win32 delete; polarity PE 0/-1.
  std::string path;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    path = file_norm_slashes(file_path_of(f));
    if (path.empty()) return -1;
    file_soft_flush_open_path(path);
  }
#ifdef _WIN32
  if (DeleteFileA(path.c_str()) || RemoveDirectoryA(path.c_str())) return 0;
  return -1;
#else
  return std::remove(path.c_str()) == 0 ? 0 : -1;
#endif
}

int32_t java_io_File_copy(InvObject* original, InvObject* copy) {
  // PE @ 0x004860F0 size 0x2a. Static UnboxArg 2 Strings → File_copyPaths
  // @ 0x0054C550 ('/'→'\\', FlushHandlesForPath+close open slots matching
  // dst ONLY) → File_CopyFileWithMkdir @ 0x00558A50: CopyFileA(overwrite);
  // ERROR_PATH_NOT_FOUND=3 → CreateDirectoryA parents + touch CreateFile
  // GENERIC_WRITE CREATE_ALWAYS + retry CopyFileA; else FormatMessage
  // + MsgBox → -1. Success → 0. Soft: flush dst FILE* + CopyFileA +
  // mkdir-retry; MsgBox OOS. Polarity PE 0/-1.
  std::string src;
  std::string dst;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    src = file_norm_slashes(file_path_of(original));
    dst = file_norm_slashes(file_path_of(copy));
    if (src.empty() || dst.empty()) return -1;
    // PE File_copyPaths flushes dst only (not src).
    file_soft_flush_open_path(dst);
  }
#ifdef _WIN32
  if (CopyFileA(src.c_str(), dst.c_str(), FALSE)) return 0;
  if (file_mkdir_parents_on_path_not_found(dst.c_str()) &&
      CopyFileA(src.c_str(), dst.c_str(), FALSE)) {
    return 0;
  }
  return -1;
#else
  FILE* in = std::fopen(src.c_str(), "rb");
  if (!in) return -1;
  FILE* out = std::fopen(dst.c_str(), "wb");
  if (!out) {
    std::fclose(in);
    return -1;
  }
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
    if (std::fwrite(buf, 1, n, out) != n) {
      std::fclose(in);
      std::fclose(out);
      return -1;
    }
  }
  std::fclose(in);
  std::fclose(out);
  return 0;
#endif
}

int32_t java_io_File_move(InvObject* original, InvObject* renamed) {
  // PE @ 0x00486120 size 0x2a (42). Static move(String,String)I:
  // JVM_UnboxArg dest0=nullptr → File_movePaths @ 0x0054C650 size 0x170.
  // File_movePaths: Path_CopyCstr both, '/'→'\\'; CS lock; flush open
  // slots matching src then dst (FilePool_Close + mark -1); then
  // File_Win32Move @ 0x00558BA0 (MoveFileA → 0; ERROR_PATH_NOT_FOUND=3
  // mkdir parents + CreateFile touch + retry MoveFileA; else
  // FormatMessage + MsgBox → -1). Soft: flush src+dst FILE*, MoveFileA
  // + mkdir-retry; MsgBox OOS. Polarity PE 0/-1.
  std::string src;
  std::string dst;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    src = file_norm_slashes(file_path_of(original));
    dst = file_norm_slashes(file_path_of(renamed));
    if (src.empty() || dst.empty()) return -1;
    file_soft_flush_open_path(src);
    file_soft_flush_open_path(dst);
  }
#ifdef _WIN32
  if (MoveFileA(src.c_str(), dst.c_str())) return 0;
  if (file_mkdir_parents_on_path_not_found(dst.c_str()) &&
      MoveFileA(src.c_str(), dst.c_str())) {
    return 0;
  }
  return -1;
#else
  return std::rename(src.c_str(), dst.c_str()) == 0 ? 0 : -1;
#endif
}

int32_t java_io_File_exists(InvObject* f) {
  // PE @ 0x00486170 size 0x1f. Static exists(String): JVM_UnboxArg
  // (this dest nullptr) then File_PathExists @ 0x0054C170 size 0x70.
  // File_PathExists: Path_CopyCstr @ 0x00554860 into 0x100 stack,
  // 0x2f '/' → 0x5c '\\'. If File_UseTree @ 0x007686E0 &&
  // PackFile_PathExists @ 0x005542D0 → 1; else File_Win32Exists
  // @ 0x005586B0 (CreateFileA GENERIC_READ / FILE_SHARE_READ /
  // OPEN_EXISTING / FILE_ATTRIBUTE_NORMAL; INVALID → 0 else
  // CloseHandle + 1). File_UseTree only written 0 (sub_54C0D0); tree
  // setter sub_5542C0 has 0 xrefs — live stock path is Win32.
  // Soft W35-12: rpak_resolve_path + Win32; OR soft PackFile_PathExists
  // (rpak_find_by_name) so LoadPack/openLib packs count as present.
  std::string path;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    path = file_path_of(f);
  }
  if (path.empty()) return 0;
  for (char& c : path) {
    if (c == '/') c = '\\';
  }
  if (soft_packfile_path_exists(path.c_str())) return 1;
  const std::string resolved = rpak_resolve_path(path.c_str());
  const char* use = !resolved.empty() ? resolved.c_str() : path.c_str();
  if (use != path.c_str() && soft_packfile_path_exists(use)) return 1;
#ifdef _WIN32
  HANDLE h = CreateFileA(use, GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return 0;
  CloseHandle(h);
  return 1;
#else
  FILE* fp = std::fopen(use, "rb");
  if (!fp) return 0;
  std::fclose(fp);
  return 1;
#endif
}

// ---- FindFile ----

#ifdef _WIN32
static bool accept_find(const WIN32_FIND_DATAA& d, int flags) {
  const bool is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  if (std::strcmp(d.cFileName, ".") == 0 || std::strcmp(d.cFileName, "..") == 0) {
    return false;
  }
  if (flags == 1) return !is_dir;  // FILES_ONLY
  if (flags == 2) return is_dir;   // DIRS_ONLY
  return true;
}
#endif

InvObject* java_io_FindFile_first(InvObject* self, InvObject* path, int32_t flags) {
  // PE @ 0x00487CB0 size 0xf0 (240). Unbox this+String+I. FindFile_SlotOpen
  // @ 0x0054CF50(path,&name,&isDir) → slot (>=0) or fail. JVM_vm_set_int
  // this "handle"=slot, "flags"=flags. Skip "."/".."; filter: isDir &&
  // flags!=FILES_ONLY(1) → accept; !isDir && flags<=1 → accept; else
  // FindFile_SlotNext @ 0x0054D1C0 until match or fail. Success →
  // JVM_String_from_cstr; fail → null. Soft load-path (rpkScan /
  // File.delete|copy|move masks): rpak_resolve_path + Win32 FindFirst;
  // tree handle/flags for close; null not "". Slot tables OOS.
  // PE @ 0x00487CB0
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  FindState& st = g_finds[self];
#ifdef _WIN32
  if (st.handle != INVALID_HANDLE_VALUE) {
    FindClose(st.handle);
    st.handle = INVALID_HANDLE_VALUE;
  }
  st.flags = flags;
  tree_field_set_int(self, "flags", flags);
  const std::string pat = rpak_resolve_path(string_cstr(path));
  st.handle = FindFirstFileA(pat.c_str(), &st.data);
  if (st.handle == INVALID_HANDLE_VALUE) {
    tree_field_set_int(self, "handle", -1);
    return nullptr;
  }
  tree_field_set_int(self, "handle", 1);  // PE slot≥0; host synthetic open
  st.has = true;
  while (st.has && !accept_find(st.data, flags)) {
    st.has = FindNextFileA(st.handle, &st.data) != 0;
  }
  if (!st.has) {
    FindClose(st.handle);
    st.handle = INVALID_HANDLE_VALUE;
    tree_field_set_int(self, "handle", -1);
    return nullptr;
  }
  return string_new(st.data.cFileName);
#else
  (void)flags;
  tree_field_set_int(self, "handle", -1);
  return nullptr;
#endif
}

InvObject* java_io_FindFile_next(InvObject* self) {
  // PE @ 0x00487DA0 size 0xc6 (198). Unbox this. handle/flags =
  // JVM_vm_get_int_field_by_name ("handle"/"flags"). handle<0 → null.
  // Loop FindFile_SlotNext @ 0x0054D1C0: skip "."/".."; same flags
  // filter as first @ 0x00487CB0 (isDir→flags!=1; file→flags<=1).
  // Match → JVM_String_from_cstr; exhausted → null.
  // PE @ 0x00487DA0
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto it = g_finds.find(self);
  if (it == g_finds.end()) return nullptr;
  FindState& st = it->second;
#ifdef _WIN32
  if (st.handle == INVALID_HANDLE_VALUE) return nullptr;
  do {
    st.has = FindNextFileA(st.handle, &st.data) != 0;
  } while (st.has && !accept_find(st.data, st.flags));
  if (!st.has) return nullptr;
  return string_new(st.data.cFileName);
#else
  return nullptr;
#endif
}

void java_io_FindFile_close(InvObject* self) {
  // PE @ 0x00487E70 size 0x48. Unbox this (JVM_UnboxArg @ 0x0045D910).
  // slot = JVM_vm_get_int_field_by_name(this, "handle") @ 0x0042A430.
  // JVM_vm_set_int_field(this, "handle", -1) @ 0x0042A170 (push 0xFFFFFFFF).
  // if (slot >= 0) FindFile_SlotClose @ 0x0054D240: slot >= 0x20 → -1;
  // else Engine_free(dword_7663D8[slot]), zero dword_7663D8[slot],
  // dword_768660[slot], File_OpenSlotsEnd[slot] (malloc'd enumerate buffer).
  // Host: tree_field handle -1; no dword_7663D8 — g_finds FindClose stand-in
  // (host first @ 0x00487CB0 not yet slot-backed; always release Win32 handle).
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  const int32_t slot = tree_field_get_int(self, "handle");
  tree_field_set_int(self, "handle", -1);
  if (slot >= 0) {
    // PE FindFile_SlotClose @ 0x0054D240 — host: g_finds FindClose below.
  }
  auto it = g_finds.find(self);
  if (it == g_finds.end()) return;
#ifdef _WIN32
  if (it->second.handle != INVALID_HANDLE_VALUE) {
    FindClose(it->second.handle);
    it->second.handle = INVALID_HANDLE_VALUE;
  }
#endif
}

// ---- Input ----

// PE Input_initDevices @ 0x00556150: device 0 name "SysKeyboard" type=1
// (256 DIK axes), device 1 "SysMouse" type=2. getDeviceName @ 0x0047C9C0
// returns null when i is outside Input_deviceCount.
constexpr int32_t kInputDeviceCount = 2;
const char* kInputDeviceName[kInputDeviceCount] = {"SysKeyboard", "SysMouse"};

// PE off_61AC44 — Input.axisName keyboard (type 1), 256 DIK slots.
const char* kKbAxisName[] = {
    "?",        "Escape",   "1",        "2",        "3",        "4",
    "5",        "6",        "7",        "8",        "9",        "0",
    "-",        "=",        "Backspace","Tab",      "Q",        "W",
    "E",        "R",        "T",        "Y",        "U",        "I",
    "O",        "P",        "[",        "]",        "Return",   "LCtrl",
    "A",        "S",        "D",        "F",        "G",        "H",
    "J",        "K",        "L",        ";",        "'",        "`",
    "LShift",   "\\",       "Z",        "X",        "C",        "V",
    "B",        "N",        "M",        ",",        ".",        "/",
    "RShift",   "Num*",     "LAlt",     "Space",    "CapsLock", "F1",
    "F2",       "F3",       "F4",       "F5",       "F6",       "F7",
    "F8",       "F9",       "F10",      "NumLock",  "ScrollLock","Num7",
    "Num8",     "Num9",     "Num-",     "Num4",     "Num5",     "Num6",
    "Num+",     "Num1",     "Num2",     "Num3",     "Num0",     "Num.",
    "?",        "?",        "?",        "F11",      "F12",
};
constexpr int32_t kKbAxisLo =
    static_cast<int32_t>(sizeof(kKbAxisName) / sizeof(kKbAxisName[0]));

const char* kb_axis_name(int32_t axis) {
  if (axis < 0 || axis >= 256) return "";
  if (axis < kKbAxisLo) return kKbAxisName[axis];
  switch (axis) {
    case 0x9C: return "NumEnter";
    case 0x9D: return "RCtrl";
    case 0xB5: return "Num/";
    case 0xB7: return "SysRq";
    case 0xB8: return "RAlt";
    case 0xC5: return "Pause";
    case 0xC7: return "Home";
    case 0xC8: return "Up";
    case 0xC9: return "PageUp";
    case 0xCB: return "Left";
    case 0xCD: return "Right";
    case 0xCF: return "End";
    case 0xD0: return "Down";
    case 0xD1: return "PageDown";
    case 0xD2: return "Insert";
    case 0xD3: return "Delete";
    case 0xDB: return "LWin";
    case 0xDC: return "RWin";
    case 0xDD: return "AppMenu";
    default: return "?";
  }
}

const char* kMouseAxisName[] = {
    "Mouse X",    "Mouse Y",    "Mouse Z",    "M.Button 1", "M.Button 2",
    "M.Button 3", "M.Button 4", "Mouse X",    "Mouse Y",    "Mouse Z",
};
constexpr int32_t kMouseAxisCount =
    static_cast<int32_t>(sizeof(kMouseAxisName) / sizeof(kMouseAxisName[0]));
// PE Input_dev0_axisCount @ 0x76F9E8 = 256.
constexpr int32_t kKbPhysAxisCount = 256;

float java_io_Input_getAxis(int32_t device, int32_t axis) {
  // PE @ 0x0047C990 size 0x2a (42). Static getAxis(II)F: JVM_UnboxArg
  // dest0=nullptr → sole callee Input_readPhysicalAxis @ 0x00557430
  // (device, axis). Physical only — NOT Input_readLogicalAxis @ 0x54D830
  // (Controller.user_GetAxisVal / mapAxis remap + smooth).
  // Device table Input_diDevices @ 0x76F9B0 stride 490 dwords; type at
  // dword[+12] (1=key 2=mouse 3=joy); naxes at dword[+14].
  // No Input_pollDevices / Cursor_tick inside getAxis — MainLoop
  // Input_tick @ 0x54DDA0 samples devices. Host MUST NOT call
  // input_live_poll() here: that re-enters tickSysCursor (historical race
  // from physics_drive / Valocity simulate → abort).
  int32_t type = 0;
  int32_t naxes = 0;
  if (device == 0) {
    type = 1;
    naxes = kKbPhysAxisCount;
  } else if (device == 1) {
    type = 2;
    naxes = kMouseAxisCount;
  }
  // Type 3 (joystick) Soft OOS — PE returns 0 if type not 1/2/3.
  if (type == 0) return 0.f;

  std::lock_guard<std::recursive_mutex> lock(g_mu);

  // Mouse branch @ 0x55776E: (unsigned)axis > 0xFFFFFF9A (-102).
  // Input_mouseSensScale @ 0x777438 = (-1-axis)*0.01; zero float
  // accum at slot+17 for naxes; return 0.0. Keyboard type1 never
  // takes this path (negative axis fails a2>=0 → 0.0).
  if (type == 2 && static_cast<uint32_t>(axis) > 0xFFFFFF9Au) {
    g_mouse_sens_scale = static_cast<float>(-1 - axis) * 0.01f;
    auto dit = g_axes.find(device);
    if (dit != g_axes.end()) {
      for (auto& kv : dit->second) kv.second = 0.f;
    }
    // Soft bridge: mirror onto Input.cursor MouseCursor for host probes
    // (PE global only; Java MouseCursor.config queues "sens").
    if (g_input_cursor) {
      tree_field_set_float(g_input_cursor, "mouse_sensitivity",
                           g_mouse_sens_scale);
    }
    return 0.f;
  }

  if (axis < 0 || axis >= naxes) return 0.f;
  float v = 0.f;
  auto dit = g_axes.find(device);
  if (dit != g_axes.end()) {
    auto ait = dit->second.find(axis);
    if (ait != dit->second.end()) v = ait->second;
  }
  if (type == 1) {
    // Keyboard: DI buffer signed char < 0 → 1.0 else 0.0.
    return v > 0.f ? 1.f : 0.f;
  }
  // Mouse: button phys ids (initDevices 3..6) digital; analog clamp [-1,1].
  if (axis >= 3 && axis <= 6) return v > 0.f ? 1.f : 0.f;
  if (v > 1.f) return 1.f;
  if (v < -1.f) return -1.f;
  return v;
}

InvObject* java_io_Input_getDeviceName(int32_t i) {
  // PE @ 0x0047C9C0 size 0x29 (41). Static native: JVM_UnboxArg @ 0x0045D910
  // (dest0=nullptr) unboxes int i. Input_deviceNameThunk @ 0x0054DE20 →
  // Input_deviceName @ 0x00558100: if i<0 || i>=Input_deviceCount @ 0x777434
  // return nullptr; else C-string at Input_dev0_name @ 0x770104 + 1960*i
  // (device stride 490 dwords; name at +1876, 64 bytes). initDevices
  // @ 0x00556150 sets "SysKeyboard"/"SysMouse" for devices 0/1.
  // JVM_String_from_cstr @ 0x004174A0: null cstr → null String.
  if (i < 0 || i >= kInputDeviceCount) return nullptr;
  return string_new(kInputDeviceName[i]);
}

void java_io_Input_mapAxis(InvObject* inst, int32_t l_axis, int32_t device,
                           int32_t i_axis, float i_from, float i_to, float l_from,
                           float l_to) {
  // PE @ 0x0047C9F0 size 0x7B. Static native: JVM_UnboxArg @ 0x0045D910
  // (dest0=nullptr) unboxes GameRef inst + l_axis, device, i_axis, i_from,
  // i_to, l_from, l_to. thiscall sub_426470(g_EngineState @ 0x00636338,
  // channel 27, 0) resolves Controller Input* map table from GameRef inst
  // (same tail as setAxisSmooth @ 0x0047CA70). Contrast Controller.user_Add
  // @ 0x00477740: Unbox this → Native.ptr (dword_62E008) → *(h+0xC) gate
  // (v2[19]!=1 → vtbl+0x14(1.0f); sub_5447D0>=0 → vtbl+0xC(1.0f)) then
  // Input_mapAxis_add on *(result+76)+0x1C. Both call Input_mapAxis_add
  // @ 0x0054D650 (first free of 152; analogFlag=Input_physAxisIsAnalog).
  // Return void — slot code from add is discarded.
  input_map_add(inst, l_axis, device, i_axis, i_from, i_to, l_from, l_to);
}
void java_io_Input_setAxisSmooth(InvObject* inst, int32_t l_axis, float a,
                                 float b, float c, float d) {
  // PE @ 0x0047CA70 size 0x6c (108). STATIC (GameRef;IFFFF)V.
  // JVM_UnboxArg dest0=nullptr; dest1=GameRef; dest2..5=l_axis+a,b,c,d
  // (center_range, factor_center, factor_opposite, factor_same).
  // Engine_queryGameRefChannel @ 0x00426470(g_EngineState @ 0x00636338,
  // GameRef, channel 27, 0) → Input* map.
  // thiscall Input_setLogicalAxisSmooth @ 0x0054D7B0
  // (map, l_axis, a,b,c,d, power=1.0f=0x3F800000):
  //   slot = map + 48*l_axis + 4; store +3..+6,+8 (VirtualAxisSmooth).
  // Contrast Controller.user_SetAxisSmooth @ 0x004778B0 (power=e).
  // VOID — EAX discarded. No poll / no Cursor_tick.
  // Host: input_map_set_smooth(..., power=1.f).
  input_map_set_smooth(inst, l_axis, a, b, c, d, 1.f);
}

// ---- Controller (physical→logical maps; same table as Input.mapAxis) ----
// PE gate (Add/Del/Reset/SetAxisForce @ 0x00477740..0x00477B20): UnboxArg →
// Native.ptr (dword_62E008) → *(h+0xC); if v2[19]!=1 vtbl+0x14(1.0f);
// sub_5447D0(v2, 0xA0000000, 0, 0)>=0 → vtbl+0xC(1.0f) → Input*+0x1C.

int32_t java_io_Controller_user_Add(InvObject* self, int32_t vaxis,
                                    int32_t device, int32_t paxis, float a,
                                    float b, float c, float d) {
  // PE @ 0x00477740: UnboxArg(this,vaxis,device,paxis,a,b,c,d) →
  // Native.ptr (dword_62E008) → *(h+0xC); if v2[0x4C]!=1 vtbl+0x14(1.0f);
  // sub_5447D0(v2,0xA0000000,0,0) fail / vtbl+0xC(1.0f)==0 → ret 0;
  // else Input_mapAxis_add(*(res+0x4C)+0x1C, ...) @ 0x0054D650 (−1/0/slot+1).
  return input_map_add_pe(self, vaxis, device, paxis, a, b, c, d);
}

int32_t java_io_Controller_user_Del(InvObject* self, int32_t vaxis,
                                    int32_t device, int32_t paxis) {
  // PE @ 0x00477810 size 0x9A (154). Unbox this+I×3 (JVM_UnboxArg @ 0x0045D910).
  // handle = JVM_vm_get_int_field(this, dword_62E008) — Native.ptr.
  // handle==0 || inner=*(handle+0xC)==0 → loc_4778A4 xor eax,eax.
  // [inner+0x4C]!=1 (INSTANCE_GAME) → vtbl+0x14(1.0f=0x3F800000).
  // sub_5447D0(inner, 0xA0000000, 0.0, 0.0); eax&0x80000000 → skip del.
  // mid = vtbl+0xC(inner, 1.0f); mid==0 → skip del.
  // Input_mapAxis_del(*(mid+0x4C)+0x1C, vaxis, device, paxis) @ 0x0054D700:
  // first matching slot only (152 cap, stride 8 dwords, zero vaxis, --count).
  // Always return 0 (xor eax,eax) — contrast user_Add @ 0x00477740 (slot code).
  // Host stand-in: self as Controller inst (no Native.ptr / sub_5447D0 / vtbl
  // gate — same gap as user_Add). input_map_del_pe mirrors first-match del.
  // race123–125 deepen: Input_mapAxis_del; always ret 0; first-match only.
  if (!self) return 0;
  (void)input_map_del_pe(self, vaxis, device, paxis);
  return 0;
}

int32_t java_io_Controller_user_SetAxisSmooth(InvObject* self, int32_t vaxis,
                                              float a, float b, float c,
                                              float d, float e) {
  // PE @ 0x004778B0 size 0xbc (188). Unbox this+I+FFFFF. Same gate as
  // user_Add @ 0x00477740: Native.ptr → *(h+0xC); [inner+0x4C]!=1 →
  // vtbl+0x14(1.0f); sub_5447D0(0xA0000000) fail / vtbl+0xC==0 → skip.
  // Else thiscall Input_setLogicalAxisSmooth @ 0x0054D7B0
  // (*(mid+0x4C)+0x1C, vaxis, a,b,c,d,e) — slot+3..+6,+8 (power=e).
  // Always return 1. Contrast Input.setAxisSmooth @ 0x0047CA70 (power=1).
  // Host stand-in: self as Controller (no Native.ptr gate).
  // race123–125 deepen: Input_setLogicalAxisSmooth; always 1; power=e.
  input_map_set_smooth(self, vaxis, a, b, c, d, e);
  return 1;
}

void java_io_Controller_user_SetAxisSpeed(InvObject* self, int32_t vaxis,
                                          float a) {
  // PE @ 0x00477970 size 0x8f (143). Unbox this+I+F
  // (JVM_UnboxArg @ 0x0045D910). handle=JVM_vm_get_int_field(this,
  // dword_62E008); handle==0 || inner=*(handle+0xC)==0 → ret.
  // [inner+0x4C]!=1 → vtbl+0x14(1.0f). sub_5447D0(inner,
  // 0xA0000000,0,0) eax&0x80000000 → ret. mid=vtbl+0xC(inner,1.0f);
  // mid==0 → ret. thiscall Input_setLogicalAxisSpeed @ 0x0054D810
  // (*(mid+0x4C)+0x1C, vaxis, speed): store float at map+48*vaxis+0x20
  // — no clamp. Same gate family as user_Add @ 0x00477740 /
  // user_GetAxisVal @ 0x00477A80. Host stand-in: self as Controller
  // (no Native.ptr / sub_5447D0 / vtbl gate); input_map_set_speed.
  // race123–125 deepen: store @ map+48*vaxis+0x20 raw; void discard EAX.
  if (!self) return;
  input_map_set_speed(self, vaxis, a);
}

float java_io_Controller_user_GetAxisVal(InvObject* self, int32_t vaxis) {
  // PE @ 0x00477A80 size 0x95 (149). Unbox this+I; stack float zeroed.
  // handle=JVM_vm_get_int_field(this, dword_62E008);
  // handle==0 || inner=*(handle+0xC)==0 → fld 0. [inner+0x4C]!=1 →
  // vtbl+0x14(1.0f). sub_5447D0(inner,0xA0000000,0,0) eax&0x80000000
  // → fld 0. mid=vtbl+0xC(inner,1.0f); mid==0 → fld 0. Else thiscall
  // Input_readLogicalAxis @ 0x0054D830(*(mid+0x4C)+0x1C, vaxis) —
  // 152-map remap + smooth (does NOT pump Cursor_tick). Same gate as
  // user_SetAxisSpeed @ 0x00477970 / user_Add. Host stand-in: self as
  // Controller; input_map_get_logical (no live_poll — re-entrancy).
  // race123–125 deepen: gate fail → 0.0; no Cursor_tick; host logical.
  if (!self) return 0.f;
  return input_map_get_logical(self, vaxis);
}

void java_io_Controller_user_SetAxisForce(InvObject* self, int32_t vaxis,
                                          float c, float f) {
  // PE @ 0x00477B20 size 0x9e (158). Unbox this+I+FF. Same gate family
  // as user_Add @ 0x00477740 / user_Reset @ 0x00477A00. Then thiscall
  // Input_setLogicalAxisForce @ 0x0054D7F0(*(mid+0x4C)+0x1C, vaxis, c, f):
  // store c @ map+48*vaxis+4+36, f @ +40 (raw floats, no epsilon clear).
  // VOID (EAX=1 discarded). Host stand-in: f overrides logical; |f|<ε
  // clears (smoke SetAxisForce(...,0) — PE keeps zeros in slots).
  // race123–125 deepen: PE stores c@+36 f@+40 raw; host ε-clear for smoke.
  (void)c;
  if (!self) return;
  if (f > -1e-6f && f < 1e-6f)
    input_map_clear_force(self, vaxis);
  else
    input_map_set_force(self, vaxis, f);
}

void java_io_Controller_user_Reset(InvObject* self) {
  // PE @ 0x00477A00 size 0x7a (122). Unbox this. Same Native.ptr /
  // INSTANCE_GAME / sub_5447D0 / vtbl+0xC gate as user_Add @ 0x00477740.
  // Then Input_mapAxis_reset @ 0x0054D750(*(mid+0x4C)+0x1C) — full wipe
  // of 152 maps + 76 vaxis props (not a Del-all loop). EAX=1 discarded
  // (Java void). Contrast Add→mapAxis_add / Del→mapAxis_del.
  // Host stand-in: input_map_reset(self).
  // race123–125 deepen: Input_mapAxis_reset (152×8-dword + 76 props);
  // EAX=1 discarded; host input_map_reset.
  input_map_reset(self);
}

int32_t java_io_Input_activeAxis(int32_t device) {
  // PE @ 0x0047CAE0 size 0x1f (31). STATIC (I)I. UnboxArg dest0=nullptr
  // → device (overwrites CallInfo). Sole callee Input_activeAxis @
  // 0x00557AA0 (size 0x565). device∉[0,Input_deviceCount@0x777434) →
  // -1. Walk axes of device slot dword_76F9B0+490*device; skip if
  // type dword[+0x144+4*i] bit15 set. Type 1/2/3 sample → float;
  // hit bands (get_bytes): flt_5F0C80@0x5F0C80=0.25, flt_5F3AD0@
  // 0x5F3AD0=-0.25, flt_5F0C60@0x5F0C60=-0.75 — v>=0.25 OR
  // v∈[-0.75,-0.25] OR (v<=-0.75 && prev[+0x44+4*i]>=-0.25); store
  // prev; count hits; return index iff count==1 else -1.
  // No Input_pollDevices inside activeAxis (same as getAxis) — do NOT
  // call input_live_poll() (tickSysCursor re-entrancy race).
  if (device < 0 || device >= kInputDeviceCount) return -1;
  const int32_t naxes = (device == 0) ? 256 : kMouseAxisCount;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto& cur = g_axes[device];
  auto& prev = g_axis_prev[device];
  int32_t found = -1;
  int32_t n = 0;
  for (int32_t i = 0; i < naxes; ++i) {
    float v = 0.f;
    auto it = cur.find(i);
    if (it != cur.end()) v = it->second;
    float old = 0.f;
    auto pit = prev.find(i);
    if (pit != prev.end()) old = pit->second;
    bool hit = false;
    if (v >= 0.25f)
      hit = true;
    else if (v <= -0.25f && v >= -0.75f)
      hit = true;
    else if (v <= -0.75f && old >= -0.25f)
      hit = true;
    if (hit) {
      found = i;
      ++n;
    }
    prev[i] = v;
  }
  return n == 1 ? found : -1;
}

InvObject* java_io_Input_axisName(int32_t device, int32_t axis) {
  // PE @ 0x0047CB00 size 0x4b (75). Static axisName(II)String: UnboxArg
  // dest0=nullptr. Input_physAxisName @ 0x005579F0(device,axis,buf256):
  // device∉[0,Input_deviceCount) or axis∉[0,naxes) → buf[0]=0, ret -1;
  // type1 → strcpy(off_61AC44[axis]); type2/3 → device name table
  // +4*axis+145. Then JVM_String_from_cstr(buf) — empty buf → empty
  // String (not null; null only if cstr ptr null).
  // PE @ 0x0047CB00
  const char* s = "";
  if (device == 0)
    s = kb_axis_name(axis);
  else if (device == 1 && axis >= 0 && axis < kMouseAxisCount)
    s = kMouseAxisName[axis];
  return string_new(s);
}

int32_t java_io_Input_lastKey() {
  // PE @ 0x0047CC10 → Input_lastKeyEvent @ 0x00556E00: scan | (ascii<<16).
  // If (result & 0xFF0000): BYTE2 → 16-byte Input_cheatRing (wrap @ end).
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  int32_t result = input_last_key_event();
  if (result == 0 && !g_key_queue.empty()) {
    // Host smoke injection (input_set_last_key edges); not stock.
    const int32_t dik = g_key_queue.front() & 0xFF;
    g_key_queue.pop_front();
    const char ascii = dik_to_lower(dik);
    result = dik | (static_cast<int32_t>(static_cast<unsigned char>(ascii)) << 16);
  }
  if ((result & 0xFF0000) != 0)
    cheat_append_ascii(static_cast<char>((result >> 16) & 0xFF));
  return result;
}

namespace {

constexpr int32_t kHkVirtual = 1;
constexpr int32_t kHkKey = 2;
constexpr int32_t kEventKeyPress = 0x1;
constexpr int32_t kEventKeyRelease = 0x2;
// PE Input_hotkeyTable @ 0x0063C890: 512 slots × 32 bytes (Input_hotkeyTableClear).
constexpr int32_t kHotkeyCap = 512;
// PE checkHotkeys @ 0x0047CD40: down if LogicalAxis > 0.2 (dbl_5F0900).
constexpr float kHotkeyDown = 0.2f;

struct HotkeyReg {
  InvObject* hk = nullptr;
  int32_t axis = 0;
  int32_t flags = 0;
  int32_t event_filter = kEventKeyPress;
  InvObject* handler = nullptr;
  InvObject* owner = nullptr;
  int32_t prev_down = 0;
  int32_t armed = 0;  // PE +28; 0 after create/flush → sample only
};

std::vector<HotkeyReg> g_hotkeys;

// PE Engine_queueEvent @ 0x00426800 → Engine_queueEvent_dispatch @ 0x004265C0
// (eventMask +0x70 & type) → Engine_dispatchScriptEvent @ 0x00425C60.
// EVENT_HOTKEY 0x00100000 → handleEvent(Hotkey), not osdCommand.
constexpr int32_t kEventHotkey = 0x00100000;

bool class_has_handleEvent_hotkey(Jvm* j, const char* cn) {
  if (!j || !cn || !cn[0]) return false;
  const JvmClass* cls = j->find_class(cn);
  if (!cls) return false;
  for (const JvmMethod& m : cls->methods) {
    if (m.name == "handleEvent" &&
        m.signature.find("Hotkey") != std::string::npos)
      return true;
  }
  return false;
}

void hotkey_fire(InvObject* hk, InvObject* native_handler, int32_t cmd) {
  if (!native_handler) return;
  // PE: skip if !(eventMask & EVENT_HOTKEY).
  const int32_t mask = tree_field_get_int(native_handler, "event_mask");
  if ((mask & kEventHotkey) == 0) return;
  tree_field_set_int(native_handler, "last_event", kEventHotkey);

  // Osd.handleEvent else-branch telemetry: hk.handler.osdCommand(cmd).
  InvObject* dest = hk ? tree_field_get_obj(hk, "handler") : nullptr;
  if (!dest) dest = native_handler;
  tree_field_set_int(dest, "last_osd_cmd", cmd);
  tree_field_set_int(dest, "osd_cmd_count",
                     tree_field_get_int(dest, "osd_cmd_count") + 1);

  Jvm* j = jvm_active();
  const char* cn = tree_host_class(native_handler);
  if (j && cn && cn[0] && class_has_handleEvent_hotkey(j, cn)) {
    std::vector<JvmValue> args = {JvmValue::make_obj(native_handler),
                                  JvmValue::make_obj(hk)};
    j->invoke(cn, "handleEvent", "(Ljava.io.Hotkey;)V", args, false);
  }
}

float hotkey_axis_value(InvObject* inst, int32_t axis, int32_t flags) {
  if (flags & kHkVirtual) {
    return inst ? input_map_get_logical(inst, axis) : 0.f;
  }
  if (flags & kHkKey) {
    return java_io_Input_getAxis(0, axis);
  }
  // Default: treat as virtual logical axis.
  return inst ? input_map_get_logical(inst, axis) : 0.f;
}

}  // namespace

void java_io_Input_createHotkey(int32_t axis, int32_t flags, InvObject* hk,
                                InvObject* handler, InvObject* owner,
                                int32_t eventFilter) {
  // PE @ 0x0047CC50 size 0xa3 (163). STATIC
  // (IILjava.io.Hotkey;Ljava.util.resource.GameRef;Ljava.render.Osd;I)V.
  // UnboxArg dest0=nullptr; dest1=&var_14 axis; dest2=&arg_0 flags;
  // dest3=&var_10 hk; dest4=&var_C handler; dest5=&var_8 owner;
  // dest6=&var_4 eventFilter. Walk Input_hotkeyTable @ 0x63C890
  // (512×0x20) to first slot[+0]==0; full → ret. Else fill:
  // +0 flags (occupied), +4 axis, +8 Hotkey*, +12 owner Osd,
  // +16 prev_down=0, +20 handler GameRef, +24 eventFilter, +28 armed=0.
  // Java Input.VIRTUAL=1 / KEY bit2. No Hotkey field writes in PE.
  // Host: g_hotkeys side table + mirror key/flags/osd on hk for smoke.
  // PE @ 0x0047CC50
  if (!hk) return;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  tree_field_set_int(hk, "key", axis);
  tree_field_set_int(hk, "flags", flags);
  if (eventFilter != 0) tree_field_set_int(hk, "eventFilter", eventFilter);
  if (owner) tree_field_set_obj(hk, "osd", owner);
  tree_field_set_int(hk, "active", 1);
  tree_field_set_int(hk, "state", 0);

  for (HotkeyReg& r : g_hotkeys) {
    if (r.hk == hk) {
      r.axis = axis;
      r.flags = flags;
      r.event_filter = eventFilter ? eventFilter : kEventKeyPress;
      r.handler = handler;
      r.owner = owner;
      r.prev_down = 0;
      r.armed = 0;
      return;
    }
  }
  if (static_cast<int32_t>(g_hotkeys.size()) >= kHotkeyCap) return;
  HotkeyReg r;
  r.hk = hk;
  r.axis = axis;
  r.flags = flags;
  r.event_filter = eventFilter ? eventFilter : kEventKeyPress;
  r.handler = handler;
  r.owner = owner;
  r.prev_down = 0;
  r.armed = 0;
  g_hotkeys.push_back(r);
}

void java_io_Input_deleteHotkey(InvObject* hk) {
  // PE @ 0x0047CD00 size 0x3d (61). Static deleteHotkey(Hotkey)V:
  // JVM_UnboxArg dest0=nullptr. Walk Input_hotkeyTable @ 0x0063C890
  // (512×32 B) matching Hotkey* at slot+8 (dword_63C898); on hit zero
  // slot[0] (dword_63C890[8*i]=0) — no Hotkey field writes. Miss → ret.
  // Host: erase g_hotkeys entry; active=0 for smoke.
  // PE @ 0x0047CD00
  if (!hk) return;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  g_hotkeys.erase(
      std::remove_if(g_hotkeys.begin(), g_hotkeys.end(),
                     [hk](const HotkeyReg& r) { return r.hk == hk; }),
      g_hotkeys.end());
  tree_field_set_int(hk, "active", 0);
}

void java_io_Input_checkHotkeys(InvObject* inst, InvObject* infocus) {
  // PE @ 0x0047CD40 size 0x13e (318). STATIC (GameRef,Osd)V. UnboxArg
  // dest0=nullptr → inst, infocus. Engine_queryGameRefChannel(inst,27,0)
  // → Controller mid for Input_readLogicalAxis. Walk 512 slots @
  // 0x63C890: skip +0==0; owner(+12)==0 || owner==infocus; sample:
  // flags&1 → readLogicalAxis(mid, axis@+4); flags&2 → key sample
  // sub_556EC0(0,0,axis); down = sample > 0.2 (dbl_5F0900). armed@+28
  //==0 → set 1 (arm only); else edge vs prev@+16 & eventFilter@+24
  // bits F_KEY_PRESS/RELEASE → count; first edge index kept. Always
  // store prev@+16; JVM setIntField(hk,"state",down). Tail: if any
  // edge → Engine_queueEvent(handler@+20, 0, EVENT_HOTKEY 0x100000,
  // hk@+8, 0). Host: g_hotkeys + hotkey_fire stand-in.
  // race119–121: Frontend polls checkHotkeys each frame — stand-in for
  // Engine_MainLoop Input_tick adjacency before Sfx_ListenerSetPose
  // @ 0x00428CB6 + Sfx_UpdateVoices @ 0x00428CBE + PollWindowQuit
  // @ 0x00428F5F (system_mainloop_sfx_listener_update).
  // PE @ 0x0047CD40
  system_mainloop_sfx_listener_update();
  struct PendingFire {
    InvObject* hk = nullptr;
    InvObject* handler = nullptr;
    int32_t cmd = 0;
  };
  PendingFire fire{};
  {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    bool fired = false;
    for (HotkeyReg& r : g_hotkeys) {
      if (!r.hk) continue;
      if (tree_field_get_int(r.hk, "active") == 0) continue;
      // PE: owner==0 (global) || owner==infocus. Null infocus skips OSD keys.
      if (r.owner && r.owner != infocus) continue;

      const int32_t axis = tree_field_get_int(r.hk, "key");
      const int32_t flags = tree_field_get_int(r.hk, "flags");
      int32_t ef = tree_field_get_int(r.hk, "eventFilter");
      if (ef == 0) ef = r.event_filter;
      const float v = hotkey_axis_value(inst, axis ? axis : r.axis,
                                        flags ? flags : r.flags);
      const int32_t down = (v > kHotkeyDown) ? 1 : 0;
      if (r.armed == 0) {
        r.armed = 1;
      } else if (down != r.prev_down && !fired) {
        const int32_t want = down ? kEventKeyPress : kEventKeyRelease;
        if (ef & want) {
          fire.hk = r.hk;
          fire.handler = r.handler;
          fire.cmd = tree_field_get_int(r.hk, "command");
          fired = true;
        }
      }
      r.prev_down = down;
      tree_field_set_int(r.hk, "state", down);
    }
  }
  if (fire.hk || fire.handler) hotkey_fire(fire.hk, fire.handler, fire.cmd);
}

void java_io_Input_flushHotkeys() {
  // PE @ 0x0047CE80 size 0x1d (29). Static ()V: no JVM_UnboxArg; no callees.
  // Walk Input_hotkeyArmed[0..512) @ +28, stride 0x20: if slot.flags(+0)!=0 → armed=0.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  for (HotkeyReg& r : g_hotkeys) {
    if (r.flags != 0) r.armed = 0;
  }
}

InvObject* java_io_Input_cursor() {
  // Soft stand-in for Java Input.cursor() accessor (not a PE native —
  // registry has MouseCursor.getPos/getPickedPos only @ 0x487970/0x487870).
  // Returns the singleton MouseCursor GameType; ctor allocates inner
  // GameRef RID_CURSOR. tickSysCursor / enable queueEvent dest = inner.
  if (!g_input_cursor) {
    g_input_cursor = tree_host_new("java.io.MouseCursor");
    tree_field_set_int(g_input_cursor, "visible", 0);
    tree_field_set_int(g_input_cursor, "enabled", 0);
    tree_field_set_float(g_input_cursor, "mouse_sensitivity",
                         g_mouse_sens_scale);
  }
  // Java MouseCursor ctor: cursor = new GameRef(..., RID_CURSOR, ...).
  // tickSysCursor / addHandler dest is this inner GameRef.
  if (!tree_field_get_obj(g_input_cursor, "cursor")) {
    InvObject* inner = gameref_new();
    tree_field_set_obj(g_input_cursor, "cursor", inner);
    tree_field_set_obj(inner, "cursor_owner", g_input_cursor);
  }
  return g_input_cursor;
}

int32_t java_io_MouseCursor_enable(InvObject* self, int32_t state) {
  // Soft stand-in for Java MouseCursor.enable(I)I (sources MouseCursor.java
  // — NOT a PE native). Stock: prev=visible; if (visible^state) then
  // queueEvent EVENT_COMMAND "enable"/"disable" on inner cursor GameRef,
  // vp activate/deactivate, visible=state; return prev.
  // Host: xor toggle + Win32 ShowCursor; soft-queue enable/disable on
  // inner GameRef (vp/camera OOS). getPos/getPickedPos live in Cars.cpp.
  if (!self) return 0;
  const int32_t prev = tree_field_get_int(self, "visible");
  const int32_t on = state ? 1 : 0;
  if ((prev ^ on) != 0) {
    tree_field_set_int(self, "visible", on);
    tree_field_set_int(self, "enabled", on);
    InvObject* inner = tree_field_get_obj(self, "cursor");
    if (inner) {
      // EVENT_COMMAND = 0x10 — Java enable() path.
      java_util_resource_GameRef_queueEvent(
          inner, nullptr, 0x10,
          string_new(on ? "enable" : "disable"));
    }
    render_d3d9_set_cursor_visible(on);
  }
  return prev;
}

// ---- Thread ----

void java_lang_Thread_init(InvObject* self, InvObject* name) {
  // PE @ 0x0047C510 size 0x59 (89). Unbox this+String (JVM_UnboxArg @
  // 0x0045D910): dest0=this, dest1=name cstr. Engine_malloc(56 / 0x38) →
  // VMThread_init @ 0x0041F340 (thiscall on blob):
  //   JVM*=*CallInfo, priority=0 (NORM), flags=2 (arm +0x2C bit1), name.
  // *(handle+0x18)=this Java back-ref; JVM_vm_set_int_field(this,
  // dword_62E008, handle) stores Native.ptr. VMThread_init links handle into
  // cooperative scheduler dllist (host: vm_sched_link) — no CreateThread.
  // start @ 0x0047C570 clears bit1 then queues target.run (race109).
  // malloc-fail path zeros EAX then still writes [eax+18h] (latent PE crash;
  // not mirrored). xref: Natives_RegisterAll @ 0x004883F8.
  // PATH-TO-WORLD Soft: vmthread_init(prio=0, flags=2) + TREE ptr tag;
  // Runnable.run still OS-detaches on start (blocking LoadingScreen).
  if (!self) return;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  ThreadState& st = g_threads[self];
  thread_drop_vm(&st, self);
  st.name = string_cstr(name);
  st.priority = 0;  // PE VMThread_init a3=0 — NORM_PRIORITY
  st.daemon = 0;
  st.alive = false;
  st.suspended = false;
  VmThread* thr =
      vmthread_init(jvm_active(), /*priority=*/0, /*sync_flags=*/2,
                    st.name.empty() ? nullptr : st.name.c_str());
  if (thr) {
    thr->pe.java_backref =
        static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(self));
    st.thr = thr;
    tree_field_set_int(self, "ptr", thread_ptr_tag(thr));
  }
}

void java_lang_Thread_start(InvObject* self) {
  // PE @ 0x0047C570 size 0x90 — Thread.start()V.
  // Unbox this; Native.ptr via dword_62E008. ptr==0 → ret.
  // Bytes @ 0x47C59A: mov eax,[esi+2Ch]; or ecx,2; jz (dead — or|2 never
  // ZF); and eax,~2; store — clear armed bit1 from VMThread_init a4=2.
  // push 0; thiscall VMThread_pushCallFrame(ecx=handle, a2=null) @ 0x41F9F0.
  // get_instance_field("target","java.lang.Runnable");
  // Object_callMethod(ecx=target, push handle, push "run" @ off_612D18) →
  // Thread_callMethod @ 0x4207C0. Nonzero → Thread_requestStop +
  // [handle+0x18]=0 + set_int_field(ptr,0). Cooperative — NO CreateThread.
  // PATH-TO-WORLD Soft: ensure VmThread + clear bit1 + pushCallFrame(empty);
  // do NOT green-queue run() (pending+PumpFrame would block MainLoop on
  // Object.wait). Detached std::thread invokes target.run()V — LoadingScreen /
  // HotkeyWatcher / GameType.enterAsyncMode_Script path.
  if (!self) return;
  InvObject* target = tree_field_get_obj(self, "target");
  // Soft: TREE may omit Thread(Runnable) target=this; PE ctor always sets it.
  if (!target) target = self;

  {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    ThreadState& st = g_threads[self];
    if (st.alive) return;
    VmThread* thr = thread_ensure_vm(&st, self);
    if (thr) {
      // PE @ 0x47C5A4: flags &= ~2 (armed clear).
      thr->pe.flags &= ~2;
      thr->pack_vec.clear();
      // PE push 0 → a2=null → pushCallFrame still boxes count=0 onto operand.
      vmthread_push_call_frame(thr);
    }
    st.alive = true;
    st.suspended = false;
    if (thr) thr->pe.flags &= ~0x20;  // Soft: clear suspend bit on (re)start
  }

  // Detached OS thread — stock stop() is cooperative via STOP bit / alive.
  std::thread([self, target]() {
    tree_field_set_int(self, "engine_run_entered", 1);
    Jvm* j = jvm_active();
    if (j && target) {
      const char* hc = tree_host_class(target);
      if (!hc || !hc[0]) hc = "java.lang.Thread";
      for (;;) {
        bool sus = false;
        bool alive = false;
        {
          std::lock_guard<std::recursive_mutex> lock(g_mu);
          auto it = g_threads.find(self);
          if (it == g_threads.end()) break;
          alive = it->second.alive;
          sus = it->second.suspended;
          if (it->second.thr &&
              (it->second.thr->pe.flags & 0x20) != 0) {
            sus = true;
          }
        }
        if (!alive) break;
        if (!sus) break;
        java_lang_Thread_sleep(10.f);
      }
      {
        std::lock_guard<std::recursive_mutex> lock(g_mu);
        auto it = g_threads.find(self);
        if (it == g_threads.end() || !it->second.alive) {
          tree_field_set_int(self, "engine_run_done", 1);
          return;
        }
      }
      // Soft ≡ Object_callMethod(target, handle, "run") → TREE run()V.
      j->invoke(hc, "run", "()V", {JvmValue::make_obj(target)}, false);
    }
    tree_field_set_int(self, "engine_run_done", 1);
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    auto it = g_threads.find(self);
    if (it != g_threads.end()) {
      it->second.alive = false;
      // Soft: mark DONE on VM blob (PE op16 |=0x40); leave for stop/dtor.
      if (it->second.thr) it->second.thr->pe.flags |= kVmThreadFlagDone;
    }
  }).detach();
}

void java_lang_Thread_stop(InvObject* self) {
  // PE @ 0x0047C600 size 0x50 (80). Unbox this. handle=JVM_vm_get_int_field
  // (this, dword_62E008); handle==0 → ret. Else Thread_requestStop
  // @ 0x0041F780: if flags+0x2C & 0x10 → Object_MonitorDequeue; then
  // flags |= 0x80 (STOP). Clear *(handle+0x18)=0; JVM_vm_set_int_field
  // (this, dword_62E008, 0) — destroys Native.ptr. Contrast suspend
  // @ 0x0047C680 (bit 0x20 + suspend queue, keeps handle).
  // PATH-TO-WORLD Soft: alive=false + vmthread_request_stop + destroy +
  // TREE ptr=0 (PE destroys handle; host unlinks green-list).
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto it = g_threads.find(self);
  if (it == g_threads.end()) return;
  it->second.alive = false;
  it->second.suspended = false;
  thread_drop_vm(&it->second, self);
}

int32_t java_lang_Thread_isAlive(InvObject* self) {
  // PE @ 0x0047C730 size 0x26 (38). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // JVM_vm_get_int_field(this, dword_62E008) — Native.ptr; EAX discarded.
  // Then xor eax,eax / retn — stock ALWAYS returns 0 (no alive bit read).
  // Host stand-in: g_threads.alive set by start/cleared by stop+run end
  // (Phase 2.117 LoadingScreen / HotkeyWatcher waits need a real flag).
  // race123–125 deepen: PE always 0; host keeps alive≠PE (smoke waits).
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto it = g_threads.find(self);
  return (it != g_threads.end() && it->second.alive) ? 1 : 0;
}

int32_t java_lang_Thread_getPriority(InvObject* self) {
  // PE @ 0x0047C760 size 0x2e (46). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // handle = JVM_vm_get_int_field(this, dword_62E008) — Native.ptr.
  // handle==0 → xor eax,eax return 0. Else return *(DWORD*)(handle+0x28).
  // Contrast Thread.init @ 0x0047C510: Engine_malloc(56) → VMThread_init
  // @ 0x0041F340 seeds *(handle+0x28)=0 NORM. setPriority @ 0x0047C790 stores.
  // PATH-TO-WORLD Soft: thr→pe.priority (+0x28); missing thr → 0.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto it = g_threads.find(self);
  if (it == g_threads.end()) return 0;
  if (it->second.thr) return it->second.thr->pe.priority;
  return it->second.priority;
}

void java_lang_Thread_setPriority(InvObject* self, int32_t newPriority) {
  // PE @ 0x0047C790 size 0x35 (53). Unbox this+I (JVM_UnboxArg @ 0x0045D910):
  // dest0=this, dest1=newPriority (var_4). handle = JVM_vm_get_int_field(this,
  // dword_62E008). handle==0 → no store. Else *(DWORD*)(handle+0x28)=newPriority.
  // Contrast getPriority @ 0x0047C760: same Unbox+dword_62E008+0x28 slot, but load.
  // PATH-TO-WORLD Soft: thr→pe.priority; null thr no-op (PE polarity).
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto it = g_threads.find(self);
  if (it == g_threads.end()) return;
  it->second.priority = newPriority;
  if (it->second.thr) it->second.thr->pe.priority = newPriority;
}

void java_lang_Thread_setDaemon(InvObject* self, int32_t daemon) {
  // PE @ 0x0047C6E0 size 0x4d (77). Unbox this+I
  // (JVM_UnboxArg @ 0x0045D910): dest0=this, dest1=daemon (var_4).
  // handle=JVM_vm_get_int_field(this, dword_62E008); handle==0 → no
  // store. Else flags=*(handle+0x2C): daemon!=0 → OR 0x100; else AND
  // 0xFFFFFEFF. Contrast setPriority @ 0x0047C790 (store at handle+0x28).
  // PATH-TO-WORLD Soft: ThreadState.daemon + thr flags bit 0x100.
  if (!self) return;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  ThreadState& st = g_threads[self];
  st.daemon = daemon;
  if (!st.thr) return;
  if (daemon)
    st.thr->pe.flags |= 0x100;
  else
    st.thr->pe.flags &= ~0x100;
}

void java_lang_Thread_sleep(float millisec) {
  // PE @ 0x0047C650 size 0x29 (41). Unbox F (JVM_UnboxArg @ 0x0045D910):
  // dest0=&var_4 (static: no this write), dest1=&arg_0 := millisec float.
  // ECX = *(CallInfo+4) — current VM thread ctx. thiscall
  // Thread_setSleepDeadline @ 0x0041F630 (race125 rename, size 0x1C):
  // now=Engine_GetTimeMs @ 0x005516C0; *(float*)(this+0x30)=now+ms;
  // *(DWORD*)(this+0x2C)|=8 (sleep bit). Scheduler @ 0x00416940: skip run
  // while bit8; clear when Engine_GetTimeMs() > *(float*)(this+0x30).
  // Stock is cooperative, not OS Sleep. PATH-TO-WORLD Soft: Win32 Sleep /
  // usleep — LoadingScreen / SoftTimer need a real block on OS thread.
  // (No CallInfo+4 current-VM ctx on host OS workers.)
#ifdef _WIN32
  Sleep(static_cast<DWORD>(millisec < 0 ? 0 : millisec));
#else
  usleep(static_cast<useconds_t>(millisec * 1000.f));
#endif
}

void java_lang_Thread_suspend(InvObject* self) {
  // PE @ 0x0047C680 size 0x2f (47). Unbox this. handle=JVM_vm_get_int_field
  // (this, dword_62E008). handle==0 → ret. Else JMP loc_41F570 (ECX=handle):
  // flags=*(DWORD*)(handle+0x2C); if (flags&0x20) already suspended → ret.
  // Else flags|=0x20; store +0x2C. if (flags&0x10)!=0 → skip queue relink
  // (WAITING). Else unlink +4/+8, push onto suspend queue via
  // *(*(handle+0x10)+0x1C) head at +0x1C. Contrast resume @ 0x0047C6B0
  // (clear bit20, run queue +0x18). PATH-TO-WORLD Soft: suspended=true +
  // thr flags |= 0x20 (start() polls before run).
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  ThreadState& st = g_threads[self];
  st.suspended = true;
  if (st.thr && (st.thr->pe.flags & 0x20) == 0) st.thr->pe.flags |= 0x20;
}

void java_lang_Thread_resume(InvObject* self) {
  // PE @ 0x0047C6B0 size 0x2f (47). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // handle = JVM_vm_get_int_field(this, dword_62E008). handle==0 → ret.
  // Else JMP loc_41F5D0 (ECX=handle): flags=*(DWORD*)(handle+0x2C);
  // if (flags&0x20)==0 → already runnable, ret. Else flags&=~0x20; store +0x2C.
  // if (flags&0x10)!=0 → skip queue relink. Else unlink +4/+8 list, then
  // push onto run queue via *( *(handle+0x10) +0x18 ) head at +0x18.
  // Contrast suspend @ 0x0047C680: JMP loc_41F570 sets bit 0x20.
  // Contrast stop @ 0x0047C600: requestStop + clear +0x18 + destroy ptr.
  // PATH-TO-WORLD Soft: suspended=false + thr flags &= ~0x20.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  ThreadState& st = g_threads[self];
  st.suspended = false;
  if (st.thr) st.thr->pe.flags &= ~0x20;
}

// ---- Object ----

namespace {

struct ObjMonitor {
  std::mutex mu;
  std::condition_variable cv;
  int pending = 0;  // notify tokens (sticky until consumed by wait)
  int waiters = 0;
  int wake_count = 0;
  int notify_count = 0;
  int gc_disabled = 0;
};

std::mutex g_mon_map_mu;
std::unordered_map<InvObject*, std::shared_ptr<ObjMonitor>> g_monitors;

std::shared_ptr<ObjMonitor> monitor_of(InvObject* self) {
  std::lock_guard<std::mutex> lock(g_mon_map_mu);
  auto& p = g_monitors[self];
  if (!p) p = std::make_shared<ObjMonitor>();
  return p;
}

}  // namespace

void java_lang_Object_wait(InvObject* self) {
  // PE @ 0x0047C920 size 0x22 (34). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // Object_MonitorEnqueue @ 0x00408B20(monitor, CallInfo+4 Thread*):
  //   Thread_markWaiting @ 0x0041F650 — OR flags+0x2C bit0x10 (WAITING),
  //   store monitor @ thread+0x34, unlink run-queue DLL (+4/+8), link wait-
  //   queue; then push Thread* onto T_Container @ InvObject+0x14 (lazy
  //   Engine_malloc 12 B: capacity/data/count). Native returns immediately —
  //   stock does NOT OS-block; cooperative JVM yields until notify pops LIFO
  //   and Thread_notify @ 0x0041F6F0 clears WAITING / re-queues (+0x18) unless
  //   SUSPENDED bit0x20. No sticky tokens; empty notify-before-wait is no-op.
  // xref: Natives_RegisterAll @ 0x00488534. Pair: notify @ 0x0047C950 /
  // notifyAll @ 0x0047C970 (_ida_race107_object_wait / _ida_race108_object_notify).
  // Host stand-in: ObjMonitor side-table CV (not InvObject+0x14). Blocks the
  // calling OS thread until sticky pending>0 — usable for smoke; full PE port
  // needs VM scheduler (Thread_markWaiting / Thread_notify / T_Container), out
  // of scope here (no invented green-thread APIs). LoadingScreen render waits
  // still bypassed via polling (D3D9 thread safety).
  if (!self) return;
  auto m = monitor_of(self);
  std::unique_lock<std::mutex> lk(m->mu);
  ++m->waiters;
  tree_field_set_int(self, "waiting", 1);
  m->cv.wait(lk, [&] { return m->pending > 0; });
  --m->pending;
  --m->waiters;
  ++m->wake_count;
  tree_field_set_int(self, "waiting", 0);
  tree_field_set_int(self, "wake_count", m->wake_count);
}

void java_lang_Object_notify(InvObject* self) {
  // PE @ 0x0047C950 size 0x1b (27). Unbox this → Object_MonitorNotify
  // body @ 0x00408BC0: T_Container @ this+0x14; if count==0 → no-op
  // (no sticky token). Else loop count times: pop LIFO waiter, clear
  // slot, Thread_notify @ 0x0041F6F0 (first non-null stops — notify
  // wakes ONE). Underflow → Engine_ErrorLogMsgBox "T_Container:
  // underflow". Contrast notifyAll @ 0x0047C970 (jmp 0x00408CE0, wake
  // all). xref Natives_RegisterAll @ 0x00488553.
  // Host stand-in: sticky pending+1 + cv.notify_one (pairs with wait
  // @ 0x0047C920 host CV — needed for smoke; full PE needs VM scheduler).
  // PE @ 0x0047C950
  if (!self) return;
  auto m = monitor_of(self);
  std::lock_guard<std::mutex> lk(m->mu);
  ++m->pending;
  ++m->notify_count;
  tree_field_set_int(self, "notify_count", m->notify_count);
  m->cv.notify_one();
}

void java_lang_Object_notifyAll(InvObject* self) {
  // PE @ 0x0047C970 size 0x1b (27). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // jmp Object_MonitorNotifyAll @ 0x00408CE0 — no inline body (contrast notify
  // @ 0x0047C950: same Unbox then monitor pop loop inlined ending @ 0x00408C3E).
  // Object_MonitorNotifyAll: T_Container @ this+0x14; loop initial count times:
  // pop waiter stack (index count-1), clear slot, Thread_notify @ 0x0041F6F0
  // each non-null; T_Container underflow → Engine_ErrorLogPrintf. Count==0 →
  // no-op (no sticky token for future waiters). xref: Natives_RegisterAll
  // @ 0x00488572.
  // Host stand-in: ObjMonitor CV — one pending token per current waiter, then
  // cv.notify_all() (LoadingScreen.termSig / Signal.notifyAll).
  if (!self) return;
  auto m = monitor_of(self);
  std::lock_guard<std::mutex> lk(m->mu);
  if (m->waiters <= 0) return;
  m->pending += m->waiters;
  ++m->notify_count;
  tree_field_set_int(self, "notify_count", m->notify_count);
  m->cv.notify_all();
}

void java_lang_Object_enableGC(InvObject* self) {
  // PE @ 0x0047C8E0 size 0x1b (27). Unbox this → Object_GC_enable
  // @ 0x00408780 size 0x58: if (flags+0x18 & 4)==0 → already enabled,
  // ret. Else unlink DLL +4/+8, link onto GC list via class+0xC → +0x10
  // head, flags &= ~4. Idempotent bit clear (not a nest counter).
  // Host stand-in: gc_disabled bit → 0 (no real GC lists).
  // PE @ 0x0047C8E0 — race123 deepen: IDA Object_GC_enable confirmed.
  // race124/125 deepen: Object_GC_enable @ 0x00408780 — bit4 clear +
  // link GC list via class+0xC→+0x10; idempotent; host gc_disabled=0.
  if (!self) return;
  auto m = monitor_of(self);
  std::lock_guard<std::mutex> lk(m->mu);
  m->gc_disabled = 0;
  tree_field_set_int(self, "gc_disabled", 0);
}

void java_lang_Object_disableGC(InvObject* self) {
  // PE @ 0x0047C8C0 size 0x1b (27). Unbox this → Object_GC_disable
  // @ 0x00408720 size 0x58: if (flags+0x18 & 4)!=0 → already pinned,
  // ret. Else unlink DLL, link onto no-GC list via class+0xC → +0x14
  // head, flags |= 4. Idempotent bit set (nested disable is no-op).
  // Host stand-in: gc_disabled=1 (no real GC lists).
  // PE @ 0x0047C8C0 — race123 deepen: IDA Object_GC_disable confirmed.
  // race124/125 deepen: Object_GC_disable @ 0x00408720 — bit4 set +
  // link no-GC list via class+0xC→+0x14; idempotent; host gc_disabled=1.
  if (!self) return;
  auto m = monitor_of(self);
  std::lock_guard<std::mutex> lk(m->mu);
  m->gc_disabled = 1;
  tree_field_set_int(self, "gc_disabled", 1);
}

int32_t java_lang_Object_hashCode(InvObject* self) {
  // PE @ 0x0047C900 size 0x17 (int_convert 23). Unbox this
  // (JVM_UnboxArg @ 0x0045D910): dest0=&arg_0 overwrites CallInfo with
  // object ptr. mov eax,[esp+arg_0]; ret — identity pointer as int,
  // NOT JDK scramble / System.identityHashCode mix. Sole callee
  // JVM_UnboxArg. xref: Natives_RegisterAll @ 0x004885EE. Host: InvObject*
  // is the unboxed handle → same cast.
  // PE @ 0x0047C900
  return static_cast<int32_t>(reinterpret_cast<uintptr_t>(self));
}

InvObject* java_lang_Object_toString(InvObject* self) {
  // PE @ 0x0047C870 size 0x50 (80). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // Class_getNameCstr @ 0x00404EA0(*(this+0xC)) — Class* FQN C string.
  // Util_Sprintf(dst256, "%s<%0x>" @ 0x00612D1C, cn, thisObj) →
  // JVM_String_from_cstr @ 0x004174A0. xref: Natives_RegisterAll @ 0x00488591.
  char buf[256];
  const char* cn = tree_host_class(self);
  if (!cn || !cn[0]) cn = "java.lang.Object";
  std::snprintf(buf, sizeof(buf), "%s<%0x>", cn,
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(self)));
  return string_new(buf);
}

// ---- Input controllers (GameLogic boot) ----

namespace {
constexpr int kMaxPlayers = 1;
InvObject* g_input_controllers[kMaxPlayers] = {};
}  // namespace

InvObject* input_init_controllers() {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  // Mirror Input.initControllers (old_controls=0): one Controller with id!=0.
  // Controller.RESOURCEID = system:0x32 — bind a non-zero ResourceRef id.
  if (!g_input_controllers[0]) {
    InvObject* c = gameref_new();
    java_util_resource_ResourceRef_set(c, 0x32);
    InvObject* css = tree_host_new("java.io.ControlSetState");
    for (int i = 0; i < 5; ++i) {
      char key[8];
      std::snprintf(key, sizeof(key), "g%d", i);
      tree_field_set_int(css, key, 0);
    }
    tree_field_set_obj(c, "css", css);
    g_input_controllers[0] = c;
  }
  return g_input_controllers[0];
}

int32_t input_is_player_active(int32_t n) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (n < 0 || n >= kMaxPlayers || !g_input_controllers[n]) return 0;
  return java_util_resource_ResourceRef_id(g_input_controllers[n]) != 0 ? 1
                                                                        : 0;
}

InvObject* input_get_controller(int32_t n) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (n < 0 || n >= kMaxPlayers) return nullptr;
  return g_input_controllers[n];
}

namespace {
constexpr int32_t kDefaultSet = 0;
InvObject* css_of(InvObject* ctrl) {
  if (!ctrl) return nullptr;
  InvObject* css = tree_field_get_obj(ctrl, "css");
  if (css) return css;
  css = tree_host_new("java.io.ControlSetState");
  for (int i = 0; i < 5; ++i) {
    char key[8];
    std::snprintf(key, sizeof(key), "g%d", i);
    tree_field_set_int(css, key, 0);
  }
  tree_field_set_obj(ctrl, "css", css);
  return css;
}
}  // namespace

void controller_activate_state(InvObject* ctrl, int32_t group,
                               int32_t new_state) {
  InvObject* css = css_of(ctrl);
  if (!css || group < 0 || group >= 5) return;
  char key[8];
  std::snprintf(key, sizeof(key), "g%d", group);
  if (tree_field_get_int(css, key) == new_state) return;
  tree_field_set_int(css, key, new_state);
  // ControlSet.load / user_Add skipped until control-file IO is wired.
}

InvObject* controller_reset(InvObject* ctrl) {
  InvObject* css = css_of(ctrl);
  if (!css) return nullptr;
  for (int i = 0; i < 5; ++i) {
    char key[8];
    std::snprintf(key, sizeof(key), "g%d", i);
    tree_field_set_int(css, key, 0);
  }
  // Mirror Controller.reset(null) → activateState(DEFAULTSET).
  controller_activate_state(ctrl, kDefaultSet, 1);
  return css;
}

int32_t controller_css_get(InvObject* ctrl, int32_t group) {
  InvObject* css = css_of(ctrl);
  if (!css || group < 0 || group >= 5) return 0;
  char key[8];
  std::snprintf(key, sizeof(key), "g%d", group);
  return tree_field_get_int(css, key);
}

// ---- ControlSet (CTRL binary) ----

namespace {
constexpr int32_t kCtrlFileId = 0x4c525443;
constexpr int32_t kCtrlFileVersion = 16;
constexpr int kNControls = 58 + 8;

struct CtrlSetData {
  int nDevices = 0;
  std::vector<std::string> deviceName;
  int group[kNControls]{};
  int vaxisID[kNControls]{};
  int deviceID[kNControls]{};
  int axisID[kNControls]{};
  float from_min[kNControls]{};
  float from_max[kNControls]{};
  float to_min[kNControls]{};
  float to_max[kNControls]{};
  float dead_zone[kNControls]{};
  int nControls = kNControls;
  bool loaded = false;
};

std::unordered_map<InvObject*, CtrlSetData> g_control_sets;

CtrlSetData& ctrl_data(InvObject* cs) { return g_control_sets[cs]; }
}  // namespace

InvObject* control_set_new() {
  InvObject* cs = tree_host_new("java.io.ControlSet");
  CtrlSetData& d = ctrl_data(cs);
  // Mirror ControlSet ctor: while (getDeviceName(n) != null) nDevices++.
  d.nDevices = 0;
  for (int i = 0; i < 16; ++i) {
    InvObject* name = java_io_Input_getDeviceName(i);
    if (!name) break;
    const char* s = string_cstr(name);
    if (!s || !s[0]) break;
    d.deviceName.emplace_back(s);
    ++d.nDevices;
  }
  d.nControls = kNControls;
  tree_field_set_int(cs, "nDevices", d.nDevices);
  tree_field_set_obj(cs, "vasp", tree_vector_new());
  return cs;
}

int32_t control_set_file_check(const char* path) {
  if (!path || !path[0]) return 0;
  InvObject* f = file_new(path);
  if (!java_io_File_open(f, 0)) return 0;
  const int32_t magic = java_io_File_readInt(f);
  const int32_t ver = java_io_File_readInt(f);
  java_io_File_close(f);
  return (magic == kCtrlFileId && ver == kCtrlFileVersion) ? 1 : 0;
}

int32_t control_set_load(InvObject* cs, const char* path) {
  if (!cs || !path || !path[0]) return 0;
  InvObject* f = file_new(path);
  if (!java_io_File_open(f, 0)) return 0;
  const int32_t magic = java_io_File_readInt(f);
  const int32_t ver = java_io_File_readInt(f);
  if (magic != kCtrlFileId || ver != kCtrlFileVersion) {
    java_io_File_close(f);
    return 0;
  }

  CtrlSetData& d = ctrl_data(cs);
  const int32_t nDev = java_io_File_readInt(f);
  d.deviceName.clear();
  if (nDev > 0) d.deviceName.reserve(static_cast<size_t>(nDev));
  for (int i = 0; i < nDev; ++i) {
    InvObject* sn = java_io_File_readString(f);
    const char* s = string_cstr(sn);
    d.deviceName.emplace_back(s ? s : "");
  }
  d.nDevices = nDev;

  const int32_t n = java_io_File_readInt(f);
  for (int i = 0; i < n; ++i) {
    const int32_t group = java_io_File_readInt(f);
    const int32_t vaxis = java_io_File_readInt(f);
    int32_t id = java_io_File_readInt(f);
    const int32_t axis = java_io_File_readInt(f);
    const float fmin = java_io_File_readFloat(f);
    const float fmax = java_io_File_readFloat(f);
    const float tmin = java_io_File_readFloat(f);
    const float tmax = java_io_File_readFloat(f);
    const float dz = java_io_File_readFloat(f);
    if (i < kNControls) {
      if (id < 0) id = -id - 1;
      d.group[i] = group;
      d.vaxisID[i] = vaxis;
      d.deviceID[i] = id;
      d.axisID[i] = axis;
      d.from_min[i] = fmin;
      d.from_max[i] = fmax;
      d.to_min[i] = tmin;
      d.to_max[i] = tmax;
      d.dead_zone[i] = dz;
    }
  }
  d.nControls = n < kNControls ? n : kNControls;

  const int32_t nVasp = java_io_File_readInt(f);
  InvObject* vasp = tree_vector_new();
  for (int i = 0; i < nVasp; ++i) {
    // VirtualAxisSmoothProperties(ctrlFile) — skip 6 floats if present later.
    (void)i;
  }
  tree_field_set_obj(cs, "vasp", vasp);
  tree_field_set_int(cs, "nDevices", d.nDevices);
  d.loaded = true;
  java_io_File_close(f);
  return 1;
}

int32_t control_set_nitems(InvObject* cs) {
  if (!cs) return 0;
  auto it = g_control_sets.find(cs);
  if (it == g_control_sets.end()) return kNControls;
  return it->second.nControls;
}

int32_t control_set_ndevices(InvObject* cs) {
  if (!cs) return 0;
  auto it = g_control_sets.find(cs);
  return it == g_control_sets.end() ? 0 : it->second.nDevices;
}

int32_t control_set_count_group(InvObject* cs, int32_t group) {
  if (!cs) return 0;
  auto it = g_control_sets.find(cs);
  if (it == g_control_sets.end()) return 0;
  int n = 0;
  for (int i = 0; i < it->second.nControls; ++i) {
    if (it->second.group[i] == group) ++n;
  }
  return n;
}

}  // namespace inv
