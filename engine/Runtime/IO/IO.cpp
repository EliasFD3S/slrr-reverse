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

struct FileState {
  std::string path;
  FILE* fp = nullptr;
  int mode = 0;
};

struct ThreadState {
  std::string name;
  int priority = 0;
  int daemon = 0;
  bool alive = false;
  bool suspended = false;
};

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

#ifdef _WIN32
// PE File_OpenCreate @ 0x0054CAF0 → sub_558E40 CreateDirectoryA on ERROR_PATH_NOT_FOUND.
void file_mkdir_parents(const char* path) {
  if (!path || !path[0]) return;
  char buf[260];
  size_t n = 0;
  for (const char* p = path; *p && n + 1 < sizeof(buf); ++p) {
    buf[n++] = *p;
    if (*p == '\\' || *p == '/') {
      buf[n] = 0;
      CreateDirectoryA(buf, nullptr);
    }
  }
}
#endif

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
  auto* obj = reinterpret_cast<InvObject*>(new InvString{nullptr});
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  ThreadState st;
  st.name = name ? name : "thread";
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
  if (!inst) return;
  if (speed < 0.01f) speed = 0.01f;
  if (speed > 20.f) speed = 20.f;
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
  // PE @ 0x00485440 size 0x423. Unbox this+I. Path = File.name String
  // Native.ptr. Native.ptr (dword_62E008)==0 → alloc 0x14C blob. Close
  // existing slot. Store mode [+0x48], path [+0x4C] cap 0x100.
  // mode==0 File_OpenRead (slash '/'→'\\', CreateFile GENERIC_READ /
  // OPEN_EXISTING); mode==1 File_OpenCreate + write 8 aSdat
  // (53 44 41 54 00 01 02 00); else Mighty ERROR
  // "vm_file_open: invalid file open mode". Open fail → free blob,
  // Native.ptr=0, return 0. Success return 1 (BOOL). Never -1.
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
  std::string path = file_path_of(self);
  for (char& c : path) {
    if (c == '/') c = '\\';
  }
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
    // PE aSdat @ 0x00612C70, WriteFile 8 bytes (not just the C string).
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
  // PE reads 8-byte header then, for SDAT type 1/2, LoadPack (not ported)
  // and FILE_BEGIN+8 for the Java payload. Non-SDAT (ControlSet etc.) stay
  // at 0 — blind skip-8 would drop the first 8 payload bytes.
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
  } else if (std::fseek(st->fp, 0, SEEK_SET) != 0) {
    std::fclose(st->fp);
    st->fp = nullptr;
    return 0;
  }
  return 1;
}

void java_io_File_close(InvObject* self) {
  // PE @ 0x00485870 size 0x2ab. Unbox this. Native.ptr==0 → ret (no free).
  // *blob==0 → skip CloseHandle, still HeapFree + Native.ptr=0.
  // Host fopen analogue: fclose if fp, always drop FileState.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  FileState* st = file_state(self);
  if (st && st->fp) {
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
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  FileState* st = file_state(self);
  if (!st || !st->fp) return 0;
  return std::fwrite(&value, 1, 4, st->fp) == 4 ? 1 : 0;
}

int32_t java_io_File_write_2(InvObject* self, InvObject* value) {
  // Stock FUN_00485c60: serialize ResourceRef/GameRef as its packed id
  // (Part.save / VehicleDescriptor / GameLogic save → readResID()).
  const int32_t id =
      value ? java_util_resource_ResourceRef_id(value) : 0;
  return java_io_File_write(self, id);
}

int32_t java_io_File_write_3(InvObject* self, InvObject* value) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  FileState* st = file_state(self);
  if (!st || !st->fp) return 0;
  const char* s = string_cstr(value);
  int32_t len = static_cast<int32_t>(std::strlen(s) + 1);
  if (std::fwrite(&len, 1, 4, st->fp) != 4) return 0;
  if (len > 0 && std::fwrite(s, 1, static_cast<size_t>(len), st->fp) !=
                     static_cast<size_t>(len)) {
    return 0;
  }
  return 1;
}

int32_t java_io_File_readInt(InvObject* self) {
  // PE @ 0x00485B70 size 0x46. Unbox this. blob=vm_get_int_field(dword_62E008).
  // blob==0 → 0. slot=*blob; slot!=0 → File_SlotRead(slot,&v,4) = ReadFile
  // 4 raw LE bytes (no bswap). Ignores bytecount. slot==0: leftover stack.
  // Host: !fp / short read → 0 (blob==0 analogue). SDAT skip is File.open.
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
  return java_io_File_readInt(self);
}

InvObject* java_io_File_readString(InvObject* self) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  FileState* st = file_state(self);
  if (!st || !st->fp) return string_new("");
  int32_t len = 0;
  if (std::fread(&len, 1, 4, st->fp) != 4 || len <= 0) return string_new("");
  std::vector<char> buf(static_cast<size_t>(len));
  if (std::fread(buf.data(), 1, buf.size(), st->fp) != buf.size()) {
    return string_new("");
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
  // Host: no PE CS/open-slot/resource-flush tables — stand-in closes
  // matching FILE* then Win32 delete; return polarity PE 0/-1.
  std::string path;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    path = file_path_of(f);
    if (path.empty()) return -1;
    std::string norm = path;
    for (char& c : norm) {
      if (c == '/') c = '\\';
    }
    for (auto& kv : g_files) {
      FileState& st = kv.second;
      if (!st.fp || st.path.empty()) continue;
      std::string open = st.path;
      for (char& c : open) {
        if (c == '/') c = '\\';
      }
      if (open == norm) {
        std::fclose(st.fp);
        st.fp = nullptr;
      }
    }
  }
  for (char& c : path) {
    if (c == '/') c = '\\';
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
  // @ 0x0054C550 → File_CopyFileWithMkdir @ 0x00558A50 (CopyFileA overwrite,
  // mkdir parents on ERROR_PATH_NOT_FOUND=3). Success → 0; fail → -1.
  // Host CRT stand-in: return polarity PE 0/-1 (was 1/0). Slot flush /
  // mkdir-on-fail not fully mirrored.
  std::string src;
  std::string dst;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    src = file_path_of(original);
    dst = file_path_of(copy);
  }
  if (src.empty() || dst.empty()) return -1;
  for (char& c : src) {
    if (c == '/') c = '\\';
  }
  for (char& c : dst) {
    if (c == '/') c = '\\';
  }
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
}

int32_t java_io_File_move(InvObject* original, InvObject* renamed) {
  std::string src;
  std::string dst;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    src = file_path_of(original);
    dst = file_path_of(renamed);
  }
  if (src.empty() || dst.empty()) return 0;
  return std::rename(src.c_str(), dst.c_str()) == 0 ? 1 : 0;
}

int32_t java_io_File_exists(InvObject* f) {
  // PE @ 0x00486170 size 0x1f. Static exists(String): JVM_UnboxArg
  // (this dest nullptr) then File_PathExists @ 0x0054C170 size 0x70.
  // File_PathExists: Engine_strcpy @ 0x00554860 into 0x100 stack,
  // 0x2f '/' → 0x5c '\\'. If File_UseTree @ 0x007686E0 &&
  // File_TreeFileExists @ 0x005542D0 → 1; else File_Win32Exists
  // @ 0x005586B0 (CreateFileA GENERIC_READ 0x80000000 / FILE_SHARE_READ 1
  // / OPEN_EXISTING 3 / FILE_ATTRIBUTE_NORMAL 0x80; -1 → 0 else
  // CloseHandle + 1). File_UseTree only written 0 (sub_54C0D0); tree
  // setter sub_5542C0 has 0 xrefs — live stock path is Win32.
  std::string path;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    path = file_path_of(f);
  }
  if (path.empty()) return 0;
  for (char& c : path) {
    if (c == '/') c = '\\';
  }
#ifdef _WIN32
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return 0;
  CloseHandle(h);
  return 1;
#else
  FILE* fp = std::fopen(path.c_str(), "rb");
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
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto it = g_finds.find(self);
  if (it == g_finds.end()) return string_new("");
  FindState& st = it->second;
#ifdef _WIN32
  if (st.handle != INVALID_HANDLE_VALUE) {
    FindClose(st.handle);
    st.handle = INVALID_HANDLE_VALUE;
  }
  st.flags = flags;
  const std::string pat = rpak_resolve_path(string_cstr(path));
  st.handle = FindFirstFileA(pat.c_str(), &st.data);
  if (st.handle == INVALID_HANDLE_VALUE) return string_new("");
  st.has = true;
  while (st.has && !accept_find(st.data, flags)) {
    st.has = FindNextFileA(st.handle, &st.data) != 0;
  }
  if (!st.has) return string_new("");
  return string_new(st.data.cFileName);
#else
  (void)flags;
  return string_new("");
#endif
}

InvObject* java_io_FindFile_next(InvObject* self) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto it = g_finds.find(self);
  if (it == g_finds.end()) return string_new("");
  FindState& st = it->second;
#ifdef _WIN32
  if (st.handle == INVALID_HANDLE_VALUE) return string_new("");
  do {
    st.has = FindNextFileA(st.handle, &st.data) != 0;
  } while (st.has && !accept_find(st.data, st.flags));
  if (!st.has) return string_new("");
  return string_new(st.data.cFileName);
#else
  return string_new("");
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
  // PE 0x0047C990 → Input_readPhysicalAxis @ 0x00557430.
  // Device table stride 490 dwords; type at +12 (1=key 2=mouse 3=joy).
  input_live_poll();
  int32_t type = 0;
  int32_t naxes = 0;
  if (device == 0) {
    type = 1;
    naxes = kKbPhysAxisCount;
  } else if (device == 1) {
    type = 2;
    naxes = kMouseAxisCount;
  }
  // Type 3 (joystick) not enumerated yet — PE returns 0 if type not 1/2/3.
  if (type == 0) return 0.f;
  if (axis < 0 || axis >= naxes) return 0.f;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
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
  // Mouse: buttons (initDevices 3..6) digital; analog clamp [-1,1].
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
  // a=center_range, b=factor_center, c=factor_opposite, d=factor_same (power=1).
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
  if (!self) return 0;
  (void)input_map_del_pe(self, vaxis, device, paxis);
  return 0;
}

int32_t java_io_Controller_user_SetAxisSmooth(InvObject* self, int32_t vaxis,
                                              float a, float b, float c,
                                              float d, float e) {
  // VirtualAxisSmoothProperties: cr, fc, fo, fs, power.
  input_map_set_smooth(self, vaxis, a, b, c, d, e);
  return 1;
}

void java_io_Controller_user_SetAxisSpeed(InvObject* self, int32_t vaxis,
                                          float a) {
  input_map_set_speed(self, vaxis, a);
}

float java_io_Controller_user_GetAxisVal(InvObject* self, int32_t vaxis) {
  return input_map_get_logical(self, vaxis);
}

void java_io_Controller_user_SetAxisForce(InvObject* self, int32_t vaxis,
                                          float c, float f) {
  (void)c;
  // Host: f is the forced logical value; |f|<ε clears the override.
  if (!self) return;
  if (f > -1e-6f && f < 1e-6f)
    input_map_clear_force(self, vaxis);
  else
    input_map_set_force(self, vaxis, f);
}

void java_io_Controller_user_Reset(InvObject* self) {
  // PE Controller.user_Reset @ 0x00477A00 → Input_mapAxis_reset @ 0x0054D750.
  // Contrast: Add→mapAxis_add (one slot), Del→mapAxis_del (one slot),
  // Reset→full wipe of 152 maps + 76 vaxis props (not a Del-all loop).
  // Java setcontrol/reset rely on this before re-user_Add.
  input_map_reset(self);
}

int32_t java_io_Input_activeAxis(int32_t device) {
  // PE Input.activeAxis @ 0x0047CAE0 → Input_activeAxis @ 0x00557AA0:
  // walk device axes; count hits; return index iff count==1 else -1.
  // Bands (flt_5F0C80 / flt_5F3AD0 / flt_5F0C60): v>=0.25, or
  // v in [-0.75,-0.25], or v<=-0.75 with previous v>=-0.25 (edge).
  input_live_poll();
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
  if (!hk) return;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  // PE 0x0047CC50: table +20 = 4th arg (OSD / GameLogic), not hk.handler.
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
  if (!hk) return;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  g_hotkeys.erase(
      std::remove_if(g_hotkeys.begin(), g_hotkeys.end(),
                     [hk](const HotkeyReg& r) { return r.hk == hk; }),
      g_hotkeys.end());
  tree_field_set_int(hk, "active", 0);
}

void java_io_Input_checkHotkeys(InvObject* inst, InvObject* infocus) {
  // PE 0x0047CD40: walk Input_hotkeyTable; first edge only
  // Engine_queueEvent(handler+20, 0, EVENT_HOTKEY, hk, 0).
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
  static InvObject* g_cursor = nullptr;
  if (!g_cursor) {
    g_cursor = tree_host_new("java.io.MouseCursor");
    tree_field_set_int(g_cursor, "visible", 0);
    tree_field_set_int(g_cursor, "enabled", 0);
  }
  // Java MouseCursor ctor: cursor = new GameRef(..., RID_CURSOR, ...).
  // tickSysCursor / addHandler dest is this inner GameRef.
  if (!tree_field_get_obj(g_cursor, "cursor")) {
    InvObject* inner = gameref_new();
    tree_field_set_obj(g_cursor, "cursor", inner);
    tree_field_set_obj(inner, "cursor_owner", g_cursor);
  }
  return g_cursor;
}

int32_t java_io_MouseCursor_enable(InvObject* self, int32_t state) {
  if (!self) return 0;
  const int32_t prev = tree_field_get_int(self, "visible");
  const int32_t on = state ? 1 : 0;
  if ((prev ^ on) != 0) {
    tree_field_set_int(self, "visible", on);
    tree_field_set_int(self, "enabled", on);
    // Phase 2.123/2.124: Win32 stock cursor show/hide.
    render_d3d9_set_cursor_visible(on);
  }
  return prev;
}

// ---- Thread ----

void java_lang_Thread_init(InvObject* self, InvObject* name) {
  // PE @ 0x0047C510 size 0x59 (89). Unbox this+String (JVM_UnboxArg @
  // 0x0045D910): dest0=this, dest1=name cstr. Engine_malloc(56 / 0x38) →
  // VMThread_init @ 0x0041F340 (was sub_41F340; thiscall on blob):
  //   JVM*=*CallInfo, priority=0 (NORM), flags=2 (arm +0x2C bit1), name.
  // *(handle+0x18)=this Java back-ref; JVM_vm_set_int_field(this,
  // dword_62E008, handle) stores Native.ptr. VMThread_init links handle into
  // cooperative scheduler pool dword_62DDF0 via sub_407B10 — no CreateThread.
  // start @ 0x0047C570 clears bit1 then queues target.run (race109
  // _ida_race109_Thread_start.json). malloc-fail path zeros EAX then still
  // writes [eax+18h] (latent PE crash; not mirrored).
  // xref: Natives_RegisterAll @ 0x004883F8 push impl → JVM_RegisterNative.
  // Host stand-in: g_threads[self] side table (name + priority=0). No 56 B
  // VMThread, no Native.ptr / dword_62E008, no +0x2C start-armed, no
  // dword_62DDF0 link — full green-thread port is out of scope (no invented
  // scheduler APIs). start() still uses detached std::thread (documented gap).
  if (!self) return;
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  ThreadState st;
  st.name = string_cstr(name);
  st.priority = 0;  // PE VMThread_init a3=0 — NORM_PRIORITY
  g_threads[self] = std::move(st);
}

void java_lang_Thread_start(InvObject* self) {
  if (!self) return;
  InvObject* target = tree_field_get_obj(self, "target");
  if (!target) target = self;

  {
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    ThreadState& st = g_threads[self];
    if (st.alive) return;
    st.alive = true;
    st.suspended = false;
  }

  // Detached OS thread — stock stop() is cooperative via alive flag.
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
      j->invoke(hc, "run", "()V", {JvmValue::make_obj(target)}, false);
    }
    tree_field_set_int(self, "engine_run_done", 1);
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    auto it = g_threads.find(self);
    if (it != g_threads.end()) it->second.alive = false;
  }).detach();
}

void java_lang_Thread_stop(InvObject* self) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto it = g_threads.find(self);
  if (it == g_threads.end()) return;
  it->second.alive = false;
  it->second.suspended = false;
}

int32_t java_lang_Thread_isAlive(InvObject* self) {
  // PE @ 0x0047C730 size 0x26 (38). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // JVM_vm_get_int_field(this, dword_62E008) — Native.ptr handle; EAX discarded.
  // Then xor eax,eax / retn — stock always returns 0 (no alive bit read).
  // Host stand-in: g_threads.alive set by start/cleared by stop+run end
  // (Phase 2.117 LoadingScreen / HotkeyWatcher waits need a real flag).
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto it = g_threads.find(self);
  return (it != g_threads.end() && it->second.alive) ? 1 : 0;
}

int32_t java_lang_Thread_getPriority(InvObject* self) {
  // PE @ 0x0047C760 size 0x2e (46). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // handle = JVM_vm_get_int_field(this, dword_62E008) — Native.ptr.
  // handle==0 → xor eax,eax return 0. Else return *(DWORD*)(handle+0x28).
  // Contrast Thread.init @ 0x0047C510 (race111): Unbox this+String;
  // Engine_malloc(56/0x38) → VMThread_init @ 0x0041F340: *(this+10)=a3
  // (=handle+0x28) with a3=0 NORM, then JVM_vm_set_int_field(this,
  // dword_62E008, handle). getPriority loads that same slot; setPriority @
  // 0x0047C790 stores it. dword_62E008 not renamed (high xref).
  // Host stand-in: ThreadState.priority (init seeds 0; missing → 0 like
  // handle==0). No Native.ptr / 56 B VMThread blob.
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto it = g_threads.find(self);
  return it == g_threads.end() ? 0 : it->second.priority;
}

void java_lang_Thread_setPriority(InvObject* self, int32_t newPriority) {
  // PE @ 0x0047C790 size 0x35 (53). Unbox this+I (JVM_UnboxArg @ 0x0045D910):
  // dest0=this, dest1=newPriority (var_4). handle = JVM_vm_get_int_field(this,
  // dword_62E008). handle==0 → no store. Else *(DWORD*)(handle+0x28)=newPriority.
  // Contrast getPriority @ 0x0047C760: same Unbox+dword_62E008+0x28 slot, but load.
  // dword_62E008 not renamed (high xref). Host stand-in: ThreadState.priority
  // (getPriority reads the same slot; NORM_PRIORITY=0 in Thread.java).
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  auto it = g_threads.find(self);
  if (it == g_threads.end()) return;
  it->second.priority = newPriority;
}

void java_lang_Thread_setDaemon(InvObject* self, int32_t daemon) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  g_threads[self].daemon = daemon;
}

void java_lang_Thread_sleep(float millisec) {
  // PE @ 0x0047C650 size 0x29 (41). Unbox F (JVM_UnboxArg @ 0x0045D910):
  // dest0=&var_4 (static: no this write), dest1=&arg_0 := millisec float.
  // ECX = *(CallInfo+4) — current VM thread ctx. thiscall sub_41F630 @
  // 0x0041F630 size 0x1C (28): now=sub_5516C0() (QPC ms @ 0x005516C0);
  // *(float*)(this+0x30)=now+ms; *(DWORD*)(this+0x2C)|=8 (sleep bit).
  // Scheduler sub_416940 @ 0x00416940: skip run while bit8; clear when
  // sub_5516C0() > *(float*)(this+0x30). Stock is cooperative, not OS Sleep.
  // sub_41F630 / sub_5516C0 / +0x2C not renamed (VM-thread layout / high xref).
  // Host stand-in: Win32 Sleep / usleep — no CallInfo+4 VM ctx on host;
  // LoadingScreen / Frontend / GameRef waits need a real block.
#ifdef _WIN32
  Sleep(static_cast<DWORD>(millisec < 0 ? 0 : millisec));
#else
  usleep(static_cast<useconds_t>(millisec * 1000.f));
#endif
}

void java_lang_Thread_suspend(InvObject* self) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  g_threads[self].suspended = true;
}

void java_lang_Thread_resume(InvObject* self) {
  // PE @ 0x0047C6B0 size 0x2f (47). Unbox this (JVM_UnboxArg @ 0x0045D910).
  // handle = JVM_vm_get_int_field(this, dword_62E008). handle==0 → ret.
  // Else JMP loc_41F5D0 (ECX=handle): flags=*(DWORD*)(handle+0x2C);
  // if (flags&0x20)==0 → already runnable, ret. Else flags&=~0x20; store +0x2C.
  // if (flags&0x10)!=0 → skip queue relink. Else unlink +4/+8 list, then
  // push onto run queue via *( *(handle+0x10) +0x18 ) head at +0x18.
  // Contrast suspend @ 0x0047C680 (same size/Unbox/dword_62E008): JMP
  // loc_41F570 sets bit 0x20 and relinks via +0x1C suspend queue (not +0x18).
  // Contrast stop @ 0x0047C600 size 0x50: sub_41F780 + clear *(handle+0x18)
  // + JVM_vm_set_int_field(this, dword_62E008, 0) — destroys handle, no bit20.
  // dword_62E008 / loc_41F5D0 / +0x2C not renamed (high xref / shared tail).
  // Host stand-in: ThreadState.suspended=false (start() polls this flag).
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  g_threads[self].suspended = false;
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
  if (!self) return;
  auto m = monitor_of(self);
  std::lock_guard<std::mutex> lk(m->mu);
  if (m->gc_disabled > 0) --m->gc_disabled;
  tree_field_set_int(self, "gc_disabled", m->gc_disabled > 0 ? 1 : 0);
}

void java_lang_Object_disableGC(InvObject* self) {
  if (!self) return;
  auto m = monitor_of(self);
  std::lock_guard<std::mutex> lk(m->mu);
  ++m->gc_disabled;
  tree_field_set_int(self, "gc_disabled", 1);
}

int32_t java_lang_Object_hashCode(InvObject* self) {
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
