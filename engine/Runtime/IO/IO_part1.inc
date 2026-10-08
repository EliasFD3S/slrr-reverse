// Fragment 1/2 of IO.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


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
// returns null when i is outside Input_diDeviceCount @ 0x777434.
// EnumDevicesCb @ 0x005565A0 may grow count while ++count < 16 (type=3 joy
// names from DIDEVICEINSTANCE+40) — Soft joy enum OOS; count stays 2.
constexpr int32_t kInputDeviceCap = 16;  // PE EnumDevicesCb stop @ 16
int32_t g_input_di_device_count = 2;     // Soft stand-in Input_diDeviceCount
const char* kInputDeviceName[kInputDeviceCap] = {"SysKeyboard", "SysMouse"};

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