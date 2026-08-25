#include "host_objects.hpp"
#include "natives.hpp"
#include "runtime.hpp"
#include "rpak.hpp"
#include "render_d3d9.hpp"
#include "input_win32.hpp"
#include "tree_interp.hpp"
#include "audio_win32.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace inv {
namespace {

std::mutex g_mu;
float g_ground_y = 0.f;
constexpr float kGravity = 25.f;
constexpr float kDriveAccel = 18.f;
constexpr float kBrakeDecel = 28.f;
constexpr float kHandbrakeDecel = 45.f;  // Phase 2.32
constexpr float kEngineBrake = 11.f;     // Phase 2.92 — coast in gear
constexpr float kNitroBoost = 0.85f;     // extra accel multiplier
constexpr float kRefDriveTorqueNm = 200.f;  // Phase 2.81 — scale vs getTorque
constexpr float kRoadYawAssist = 2.2f;   // Phase 2.93 — 1/s toward road tangent
constexpr float kRoadPitchAssist = 3.0f; // Phase 2.95 — 1/s toward road slope
constexpr float kAirborneClearance = 0.45f;  // Phase 2.93 — above support = air
constexpr float kDrag = 0.8f;
constexpr float kSteerRate = 1.8f;  // rad/s at full steer + speed factor
constexpr float kMaxSpeed = 60.f;
constexpr float kLateralGrip = 8.f;  // Phase 2.22 — arcade side-slip kill
constexpr float kHandbrakeGrip = 1.5f;  // weak grip → drift when handbrake

struct RoadSeg {
  float x0 = 0, y0 = 0, z0 = 0;
  float x1 = 0, y1 = 0, z1 = 0;
  // PE path+0xC4=196 occupancy byte (GroundMap_markPathOccupied).
  uint8_t occupied = 0;
};
std::vector<RoadSeg> g_roads;
int32_t g_collide_events = 0;
constexpr float kCollideRestitution = 0.15f;

struct RoutePt {
  float x = 0, y = 0, z = 0;
};
std::vector<RoutePt> g_last_route;
float g_last_route_len = 0.f;
// PE GroundRef_cachedRoute @ dword_6408D0 writer 0x483750: arc params at route
// endpoints via sub_57EF20 → dword_6408D8 (start) / dword_6408DC (end).
float g_last_route_u0 = 0.f;
float g_last_route_u1 = 0.f;

struct LineVert {
  float x = 0, y = 0, z = 0;
  float nx = 0, ny = 1, nz = 0;
  int32_t color = 0;
  float width = 1.f;
};
struct LineState {
  bool active = false;
  InvObject* parent = nullptr;
  int32_t type_id = 0;
  int32_t color = 0;
  float sx = 0.01f, sy = 0.f, sz = 0.01f;
  std::vector<LineVert> verts;
  // Welder host
  float progress = 0.f;
  float target_len = 1.f;
  std::vector<LineVert> weld_pts;
};
std::unordered_map<InvObject*, LineState> g_lines;

struct ResState {
  int32_t id = 0;
  int32_t type = 0;
  int32_t parent_id = 0;
  InvObject* parent = nullptr;
  InvObject* first_child = nullptr;
  InvObject* next_child = nullptr;
  bool loaded = false;
  float sx = 1, sy = 1, sz = 1;
  // render/physics pose
  float px = 0, py = 0, pz = 0;
  float oy = 0, op = 0, or_ = 0;
  // PhysicsRef body (Phase 2.18)
  float vx = 0, vy = 0, vz = 0;
  float wx = 0, wy = 0, wz = 0;  // ang vel
  float hx = 0, hy = 0, hz = 0;  // box half-extents / sphere radius in hx
  int32_t shape = 0;             // 0 none, 1 box, 2 sphere
  int32_t is_static = 0;
  int32_t asleep = 0;
  int32_t pose_set = 0;  // PhysicsRef.setMatrix always writes pose (null → origin)
  int32_t collide = 0;  // Phase 2.25 — setActiveCollision
  // Phase 2.93 — last drive tick: wheels off support.
  int32_t airborne = 0;
  // Phase 2.33 — arcade gearbox (-1=R, 0=N, 1..5).
  int32_t gear = 1;
  float gear_axis_prev = 0.f;
  // Phase 2.66 — WheelRef aggregate (steer / drive / radius) for arcade drive.
  bool has_wheel_params = false;
  float wheel_steer = 0.f;
  float wheel_drive = 1.f;
  float wheel_radius = 0.32f;
  // Phase 2.67 — contact-ish: friction/sliction → grip; brake/hbrake; roll drag.
  float wheel_friction = 1.f;
  float wheel_sliction = 1.f;
  float wheel_brake = 0.f;    // 0..1 effective
  float wheel_hbrake = 0.f;   // 0..1 effective
  float wheel_roll_res = 0.f;
  // Phase 2.68 — Pacejka B/C/D (Wheel.java idx 4/2/0); stock defaults.
  float wheel_pk_b = 15.2f;
  float wheel_pk_c = 1.49f;
  float wheel_pk_d = 1.4f;
  // Phase 2.69 — spring/shock + arm length (suspension).
  float wheel_spring = 0.f;     // N/m
  float wheel_damp = 0.f;       // N/(m/s) bound
  float wheel_rest_len = 0.39f; // m (Spring.java default)
  float wheel_arm_len = 0.244f; // m (stock wishbone)
  // Phase 2.81 — DynoData/Chassis torque (Nm) + estimated engine RPM.
  float drive_torque_nm = 0.f;  // 0 = legacy accel (no torque scale)
  float engine_rpm = 900.f;
  int32_t color = 0;
  int32_t type_id = 0;
  // Phase 2.105 — RenderRef.setLight / setFlare / changeResource.
  int32_t light_diffuse = 0;
  int32_t light_ambient = 0;
  int32_t light_specular = 0;
  int32_t flare_color = 0;
  float flare_min = 0.f;
  float flare_max = 0.f;
  int32_t flare_count = 0;
  int32_t flare_rays = 0;
  int32_t flare_tex_id = 0;
  int32_t swapped_tex_id = 0;
  InvObject* swapped_tex = nullptr;
  int32_t cached = 0;
  int32_t flags = 0;  // inner+0x54 (GameRef_getFlags / ground_precache 0x80000)
  std::string entry_path;
  std::string alias;
  uint32_t blob_size = 0;
};

std::unordered_map<InvObject*, ResState> g_res;
int32_t g_next_id = 1;

ResState& R(InvObject* self) { return g_res[self]; }

std::vector<std::string> parse_sourcefile_lines(const std::vector<uint8_t>& blob);
bool path_ends_ci(const std::string& s, const char* suf);

int32_t map_rpak_kind(int32_t kind) {
  switch (kind & 0xFFFF) {
    case 0x8:
      return 7;  // RESOURCE_TEXTURE
    case 0x93:
      return 14;  // RESTYPE_RENDER_OBJECT
    case 0x3:
      // 0x40003 — skydome/instance type recipes ("mesh 0x.. / texture 0x..").
      return 14;
    default:
      break;
  }
  if (kind == 0x10008 || kind == 0x10004) return 5;
  if (kind == 0) return 0;
  return kind & 0xFF;
}

bool parse_u32_token(const char* s, uint32_t* out) {
  if (!s || !out) return false;
  while (*s == ' ' || *s == '\t') ++s;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    unsigned v = 0;
    if (std::sscanf(s, "%x", &v) != 1) return false;
    *out = v;
    return true;
  }
  unsigned v = 0;
  if (std::sscanf(s, "%u", &v) != 1) return false;
  *out = v;
  return true;
}

// Skydome / RenderRef type blobs: "mesh 0x3\r\nflags …\r\ntexture 0x6\r\n".
// Forward: city visual path remap (defined below with road/wall egyedi).
std::string resolve_city_visual_scx(const std::string& src_in);

bool parse_mesh_recipe(const std::vector<uint8_t>& blob, uint32_t* mesh_local,
                       uint32_t* tex_local) {
  if (mesh_local) *mesh_local = 0;
  if (tex_local) *tex_local = 0;
  if (blob.size() < 6) return false;
  if (std::memcmp(blob.data(), "mesh ", 5) != 0) return false;
  size_t i = 0;
  while (i < blob.size()) {
    size_t line_end = i;
    while (line_end < blob.size() && blob[line_end] != '\n' &&
           blob[line_end] != '\r')
      ++line_end;
    std::string line(reinterpret_cast<const char*>(blob.data() + i),
                     line_end - i);
    if (line.size() >= 5 && std::strncmp(line.c_str(), "mesh ", 5) == 0) {
      uint32_t v = 0;
      if (parse_u32_token(line.c_str() + 5, &v) && mesh_local) *mesh_local = v;
    } else if (line.size() >= 8 &&
               std::strncmp(line.c_str(), "texture ", 8) == 0) {
      uint32_t v = 0;
      if (parse_u32_token(line.c_str() + 8, &v) && tex_local) *tex_local = v;
    }
    i = line_end;
    while (i < blob.size() && (blob[i] == '\n' || blob[i] == '\r')) ++i;
  }
  return mesh_local && *mesh_local != 0;
}

bool load_mesh_from_res_id(InvObject* self, int32_t mesh_id) {
  if (!self || !mesh_id) return false;
  std::vector<uint8_t> mblob;
  if (!rpak_read_entry(mesh_id, &mblob)) return false;
  if (!mblob.empty() && mblob.size() >= 4 &&
      std::memcmp(mblob.data(), "INVO", 4) == 0) {
    return render_d3d9_mesh_create_from_memory(self, mblob.data(), mblob.size(),
                                               nullptr);
  }
  for (const std::string& src : parse_sourcefile_lines(mblob)) {
    if (!path_ends_ci(src, ".scx") && !path_ends_ci(src, ".SCX")) continue;
    // Phase 2.52 — city area/hotel paths often need egyedi remap.
    std::string resolved = resolve_city_visual_scx(src);
    if (resolved.empty()) resolved = rpak_resolve_path(src.c_str());
    if (resolved.empty()) resolved = src;
    if (render_d3d9_mesh_create_from_file(self, resolved.c_str())) return true;
  }
  return false;
}

// Host stand-in for load's sub_5447D0(inner,0x80000001,0,0) then vtbl+0x0C(1.0f).
// Returns false when PE would skip vt+0x0C (sub_5447D0 sign bit / no payload).
InvObject* make_bound_ref(int32_t res_id);

bool resource_ref_bind_payload(InvObject* self, int32_t id, int32_t type,
                               const std::string& entry_path) {
  if (!self || id == 0) return false;
  std::vector<uint8_t> blob;
  std::string entry_name;
  if (!rpak_read_entry(id, &blob)) return false;
  const RpakEntry* ent = rpak_find_entry(id);
  if (ent) entry_name = ent->name;
  const bool is_sourcefile = !blob.empty() && blob.size() >= 10 &&
                             std::memcmp(blob.data(), "sourcefile", 10) == 0;
  bool sourcefile_is_mesh = false;
  if (is_sourcefile) {
    for (const std::string& src : parse_sourcefile_lines(blob)) {
      if (path_ends_ci(src, ".scx") || path_ends_ci(src, ".SCX")) {
        sourcefile_is_mesh = true;
        break;
      }
    }
  }
  if (sourcefile_is_mesh) {
    return load_mesh_from_res_id(self, id);
  }
  if (type == 7 /* RESOURCE_TEXTURE */ || is_sourcefile ||
      (!entry_path.empty() && entry_path.size() >= 4 &&
       (entry_path.find(".dds") != std::string::npos ||
        entry_path.find(".DDS") != std::string::npos))) {
    return render_d3d9_texture_create_from_rpak(self, blob.data(), blob.size(),
                                                  entry_name.c_str(),
                                                  entry_path.c_str());
  }
  if (!blob.empty() && blob.size() >= 4 && blob[0] == 'D' && blob[1] == 'D' &&
      blob[2] == 'S') {
    return render_d3d9_texture_create_from_memory(self, blob.data(), blob.size(),
                                                  entry_path.c_str());
  }
  if ((!blob.empty() && blob.size() >= 4 &&
       std::memcmp(blob.data(), "INVO", 4) == 0) ||
      (!entry_path.empty() &&
       (entry_path.find(".scx") != std::string::npos ||
        entry_path.find(".SCX") != std::string::npos))) {
    if (!blob.empty() && blob.size() >= 4 &&
        std::memcmp(blob.data(), "INVO", 4) == 0) {
      return render_d3d9_mesh_create_from_memory(self, blob.data(), blob.size(),
                                                 entry_path.c_str());
    }
    if (!entry_path.empty()) {
      return render_d3d9_mesh_create_from_file(self, entry_path.c_str());
    }
  }
  if (!blob.empty() && blob.size() >= 5 &&
      std::memcmp(blob.data(), "mesh ", 5) == 0) {
    uint32_t mesh_local = 0, tex_local = 0;
    if (parse_mesh_recipe(blob, &mesh_local, &tex_local) && mesh_local) {
      const int32_t mid =
          rpak_make_id(rpak_id_pack(id), static_cast<uint16_t>(mesh_local));
      if (!load_mesh_from_res_id(self, mid)) return false;
      if (tex_local) {
        const int32_t tid =
            rpak_make_id(rpak_id_pack(id), static_cast<uint16_t>(tex_local));
        InvObject* tex = make_bound_ref(tid);
        if (!tex) return false;
        java_util_resource_ResourceRef_load(tex);
        if (render_d3d9_texture_ready(tex))
          render_d3d9_mesh_set_texture(self, tex);
      }
      return true;  // mesh recipe bound even if nested tex still loading
    }
  }
  return false;
}

void bind_res_id(ResState& st, int32_t ID) {
  st.id = ID;
  st.entry_path.clear();
  st.blob_size = 0;
  st.type = 0;
  st.parent_id = 0;
  st.parent = nullptr;
  st.first_child = nullptr;
  st.next_child = nullptr;
  if (ID == 0) return;
  const RpakEntry* e = rpak_find_entry(ID);
  if (!e) return;
  st.type = map_rpak_kind(e->kind);
  st.entry_path = e->path;
  st.blob_size = e->size;
  st.parent_id = rpak_parent_id(ID);
}

InvObject* make_bound_ref(int32_t res_id) {
  if (res_id == 0) return nullptr;
  InvObject* o = resref_new();
  {
    std::lock_guard<std::mutex> lock(g_mu);
    bind_res_id(R(o), res_id);
  }
  gameref_on_res_bound(o);
  return o;
}

}  // namespace

void resref_ensure(InvObject* self) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_res.find(self) == g_res.end()) g_res[self] = ResState{};
}

InvObject* resref_find_by_id(int32_t id) {
  if (!id) return nullptr;
  std::lock_guard<std::mutex> lock(g_mu);
  for (auto& kv : g_res) {
    if (kv.second.id == id) return kv.first;
  }
  return nullptr;
}

void resref_set_parent(InvObject* self, InvObject* parent) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& st = R(self);
  st.parent = parent;
  st.parent_id = parent ? R(parent).id : 0;
}

InvObject* resref_new() {
  auto* o = reinterpret_cast<InvObject*>(new InvString{nullptr});
  std::lock_guard<std::mutex> lock(g_mu);
  g_res[o] = ResState{};
  return o;
}

// ---- ResourceRef ----
void java_util_resource_ResourceRef_newNative(InvObject* self) {
  // PE @ 0x0047CEA0 size 0x5c (92): Unbox this (JVM_UnboxArg @ 0x0045D910).
  // Native.ptr via dword_62E008 (JVM_vm_get_int_field @
  // 0x0042AB50). ptr!=0 → ret (idempotent). Else Engine_malloc(0x10) @
  // 0x0054F560; zero 4 dwords: [0]/[4] list links, [8] id, [0xC] inner;
  // JVM_vm_set_int_field @ 0x0042A9E0. Host: g_res entry = handle present.
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_res.find(self) != g_res.end()) return;
  g_res[self] = ResState{};
}
void java_util_resource_ResourceRef_deleteNative(InvObject* self) {
  // PE @ 0x0047CF00 ResourceRef.deleteNative()V size 0x7f (127).
  // Unbox this (JVM_UnboxArg @ 0x0045D910). Native.ptr =
  //   JVM_vm_get_int_field(this, dword_62E008 @ 0x0062E008) @ 0x0042AB50.
  // Xref data: Natives_RegisterAll @ 0x00489302 push 0x47CF00.
  // handle==0 → skip free (NO Mighty ERROR). Else:
  //   inner=[handle+0xC]; if inner!=0: unlink DLL [handle+0] prev /
  //   [handle+4] next (prev==0 → head at inner+0x48); zero 4 dwords
  //   handle[0..0xC]. Else only [handle+8]=0 (id). Then Engine_free
  //   @ 0x0054F5B0 (0x10 blob from newNative malloc). Always
  //   JVM_vm_set_int_field @ 0x0042A9E0 (0) — even handle==0 (contrast
  //   newNative @ 0x0047CEA0 idempotent early-out if ptr!=0). No
  //   unload/destroy/GPU/queue. Java finalize() → deleteNative only.
  // Contrast destroy @ 0x0047D1E0: keeps Native.ptr, queues sub_48A8D0.
  // Host: g_res entry = Native.ptr / 0x10 handle; g_lines = LineState
  //   sidecar. GAPS: no DLL unlink @ inner+0x48; no Engine_free blob.
  if (!self) return;  // GAP: PE UnboxArg; host guard
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_res.find(self) != g_res.end()) {
    g_lines.erase(self);  // host LineState sidecar (not PE handle layout)
    g_res.erase(self);    // PE Engine_free @ 0x0054F5B0 (0x10)
  }
  // PE: always JVM_vm_set_int_field(Native.ptr, 0) even when handle==0.
}
void java_util_resource_ResourceRef_load(InvObject* self) {
  // PE @ 0x0047D0A0 size 0xc8 (200): java.util.resource.ResourceRef.load()V.
  // Unbox this (JVM_UnboxArg @ 0x0045D910). Handle = JVM_vm_get_int_field(this,
  // dword_62E008 @ 0x0062E008) @ 0x0042AB50. Handle 0 → CRT_strcat_n_thunk("!"
  // @ 0x612D44 + "Mighty ERROR" @ 0x612D48) into Engine_ErrorLogBuf @
  // 0x62E018, Engine_ErrorLogPrintf @ 0x5513B0, clear buf (host: ret, log not
  // mirrored). Config_GetInt("ground_precache" @ 0x612D34) @ 0x426170 > 0 →
  // inner=[handle+0xC]; inner!=0 → [inner+0x54] |= 0x80000 (unique writer;
  // reader sub_53DEE0 @ 0x53DF8E eager-loads children). Java Config.ground_precache
  // default 0. inner=[handle+0xC]==0 → ret. [inner+0x4C]!=INSTANCE_GAME(1) →
  // thiscall inner.vtbl+0x14(1.0f=0x3F800000) LOD request (not mirrored).
  // thiscall sub_5447D0(inner, 0x80000001, 0.0, 0.0) @ 0x5447D0 — NOT cache
  // (0xA0000001=0x80000001|0x20000000 @ 0x48A920) or precache. test eax,
  // 0x80000000: fail if sign (inner+0x64 bit0 clear) skips vtbl+0x0C. Success:
  // thiscall inner.vtbl+0x0C(1.0f) bind payload; return discarded. Not
  // ResourceEngine_Init. Java comment "recursive load talajra" = ground_precache
  // flag, not recursion in this native.
  // Host: g_res entry = Native.ptr handle; ResState.flags ↔ [inner+0x54];
  // resource_ref_bind_payload = sub_5447D0+vtbl+0x0C stand-in.
  if (!self) return;
  int32_t id = 0;
  int32_t type = 0;
  std::string entry_path;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    const auto it = g_res.find(self);
    if (it == g_res.end()) return;  // Native.ptr==0 → Mighty ERROR
    auto& st = it->second;
    id = st.id;
    type = st.type;
    entry_path = st.entry_path;
    InvObject* cfg = system_config_host();
    if (cfg && tree_field_get_int(cfg, "ground_precache") > 0 && id != 0)
      st.flags |= 0x80000u;  // [inner+0x54] ground_precache eager child load
  }
  if (id == 0) return;  // inner=[handle+0xC]==0
  // [inner+0x4C]!=1 → vtbl+0x14(1.0f) LOD — not mirrored (type==1 skip).
  const bool bound = resource_ref_bind_payload(self, id, type, entry_path);
  if (!bound) return;  // sub_5447D0 sign → skip vtbl+0x0C
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& st = R(self);
    st.loaded = true;  // vtbl+0x0C(1.0f) payload bound
    if (id != 0) {
      std::vector<uint8_t> blob;
      if (rpak_read_entry(id, &blob))
        st.blob_size = static_cast<uint32_t>(blob.size());
    }
  }
}
void java_util_resource_ResourceRef_unload(InvObject* self) {
  // PE @ 0x0047D170 size 0x70: Unbox this. Handle via dword_62E008.
  // Handle 0 → Mighty ERROR ("!" @ 0x612D58 + "Mighty ERROR" @ 0x612D5C)
  // (host: ret). Else thiscall jmp 0x0048A8F0 (xref unique from here).
  // 0x48A8F0: [handle+8]==0 or inner=[handle+0xC]==0 → ret. inner+0x4C==1
  // INSTANCE_GAME: sub_427720(ecx=0x636338, handle) — xref unique; 0x1C
  // list node, engine+0xD0++, no vtable+0x1C, no sub_5447D0. Else
  // inner.vtable+0x1C(1). Not load @ 0x0047D0A0 (sub_5447D0 0x80000001 +
  // optional ground_precache inner+0x54 |= 0x80000). Not cache @
  // 0x0047E8D0 (sub_48A920 / 0xA0000001 / bit 0x20000000). Not destroy @
  // 0x0047D1E0 (handle 0 silent ret, jmp 0x48A8D0 → sub_427620). No
  // cached / 0x20000000 / 0x80000 test in this native.
  // Java: map.unload Track/Garage/CarMarket; Catalog/Painter decal
  // textures; GameLogic.erase unload then destroy.
  // Host: GPU tex/mesh destroy + loaded=false (vtable+0x1C stand-in).
  if (!self) return;
  render_d3d9_texture_destroy(self);
  render_d3d9_mesh_destroy(self);
  std::lock_guard<std::mutex> lock(g_mu);
  R(self).loaded = false;
}
void java_util_resource_ResourceRef_cache(InvObject* self) {
  // PE @ 0x0047E8D0 size 0x78: Unbox this. Handle via dword_62E008.
  // Handle 0 → Mighty ERROR (host: ret). Else thiscall
  // sub_48A920(handle, 0.0f, 1.0f) then fstp. Twin of precache @
  // 0x0047E950 (only a2=1.0f). Not load @ 0x0047D0A0 (that is
  // sub_5447D0(inner, 0x80000001, 0, 0) + optional ground_precache
  // inner+0x54 |= 0x80000). Wrapper: type!=14 → vtable+0x14(1.0) if
  // type!=1, sub_5447D0(0xA0000001, 0, a2) — 0xA0000001 =
  // 0x80000001|0x20000000; bit 0x20000000 skips sub_537790 (list
  // relink). a4==0 lets sub_537240 run even if inner+0x54 has
  // 0x2000000. Type 14: sub_419860 children. No JVM field, no
  // unload skip, no refcount in this native. Java: Track spark/
  // smoke/skid; GameLogic.preCacheGametypes actually calls cache()
  // on children; SfxRef.play → cache then nplay.
  // Host: load + cached=1 keep-resident marker.
  if (!self) return;
  java_util_resource_ResourceRef_load(self);
  std::lock_guard<std::mutex> lock(g_mu);
  R(self).cached = 1;
  tree_field_set_int(self, "cached", 1);
}
void java_util_resource_ResourceRef_precache(InvObject* self) {
  // PE @ 0x0047E950 size 0x7b: identical to cache except
  // sub_48A920(handle, 1.0f, 1.0f). Handle 0 → Mighty ERROR (host:
  // ret). a4=1.0 skips sub_537240 when inner+0x54 has 0x2000000.
  // Java: Track air/tyre SfxRef, City siren, Dialog menu SFX.
  // Host: load only (no cached).
  if (!self) return;
  java_util_resource_ResourceRef_load(self);
}
void java_util_resource_ResourceRef_destroy(InvObject* self) {
  // PE @ 0x0047D1E0 size 0x2f (47): Unbox this (JVM_UnboxArg @ 0x0045D910).
  // Handle via dword_62E008 (JVM_vm_get_int_field @ 0x0042AB50). Handle 0 →
  // silent ret (NO Mighty ERROR). Else thiscall jmp sub_48A8D0 (ecx=handle):
  //   [handle+8]==0 or inner=[handle+0xC]==0 → ret. Else push handle;
  //   ecx=g_EngineState @ 0x636338; call sub_427620 (Do NOT rename).
  // sub_427620 size 0x100 (256): INSTANCE_GAME (inner+0x4C==1) may
  //   sub_5447D0(inner, 0x80000000, 0, 0) + vt+0x0C(1.0f) — note flag
  //   0x80000000 not load's 0x80000001; then Engine_malloc(0x1C) node,
  //   link inner+0x44, queue on g_EngineState destroy list (+counter).
  // Contrast deleteNative @ 0x0047CF00 size 0x7f (127): unlink list,
  //   Engine_free handle, always JVM_vm_set_int_field(Native.ptr=0). No
  //   queue / no GPU. Java finalize() → deleteNative only — not destroy.
  // Contrast load @ 0x0047D0A0 size 0xc8 (200): handle 0 → Mighty ERROR;
  //   ground_precache inner+0x54|=0x80000; vt+0x14; sub_5447D0(0x80000001);
  //   vt+0x0C. No destroy queue. Contrast unload @ 0x0047D170: Mighty
  //   ERROR on handle 0; sub_48A8F0 → INSTANCE_GAME sub_427720 else
  //   vt+0x1C(1). Destroy never vt+0x1C / never free handle.
  // Java: Part.addPart else xa.destroy(); GameLogic.erase unload then
  // destroy; Track/CarMarket/Bot car+cam teardown.
  // Host: GPU tex/mesh destroy + gameref_on_destroy (deferred queue
  // stand-in). Keeps g_res — PE leaves Native.ptr until deleteNative.
  if (!self) return;
  render_d3d9_texture_destroy(self);
  render_d3d9_mesh_destroy(self);
  gameref_on_destroy(self);
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_res.find(self);
  if (it != g_res.end()) it->second.loaded = false;
}
void java_util_resource_ResourceRef_set(InvObject* self, int32_t ID) {
  // PE @ 0x0047CF80 size 0x3a (58): Unbox this+I (JVM_UnboxArg @
  // 0x0045D910). Handle via dword_62E008 (JVM_vm_get_int_field @
  // 0x0042AB50). Handle 0 → silent return (NO Mighty ERROR — unlike
  // type @ 0x0047D210 / id @ 0x0047D290). Else thiscall sub_545FC0
  // (ecx=handle, arg0=ID): same [handle+8]==ID → no-op; else unlink
  // list ([0]/[4], head at inner+0x48), zero box; ID!=0 →
  // sub_536820(g_ResourceEngine, ID, 0, 0) lookup, relink at
  // resource+0x48, store ID at [handle+8] / inner at [handle+0xC].
  // Contrast set(ResourceRef) @ 0x0047CFC0: Unbox this+other jobject;
  // other==null → sub_545FC0(handle, 0) clear; else share other's
  // [+0xC] via inline list relink (NO ID lookup / sub_536820).
  // Host: bind_res_id (rpak stand-in for sub_545FC0). !self = handle 0.
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  bind_res_id(R(self), ID);
  gameref_on_res_bound(self);
}
void java_util_resource_ResourceRef_set_1(InvObject* self, InvObject* ref) {
  // PE @ 0x0047CFC0 — contrast set(I) @ 0x0047CF80 (ID lookup path).
  // Share other Native.ptr[+0xC]; null other clears via sub_545FC0(,0).
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  if (ref)
    R(self) = R(ref);
  else
    bind_res_id(R(self), 0);
  gameref_on_res_bound(self);
}
int32_t java_util_resource_ResourceRef_type(InvObject* self) {
  // PE @ 0x0047D210 size 0x80: Unbox this (JVM_UnboxArg @ 0x0045D910).
  // Handle via dword_62E008 (JVM_vm_get_int_field @ 0x0042AB50).
  // Handle 0 → CRT_strcat_n_thunk("!" @ 0x612D6C + "Mighty ERROR" @
  // 0x612D70) into Engine_ErrorLogBuf @ 0x62E018, Engine_ErrorLogPrintf,
  // clear buf, return 0. inner=[handle+0xC]==0 → return 0 (NO Mighty).
  // Else dword [inner+0x4C] RESTYPE (ResourceRef.java RESTYPE_INVALID=0
  // … INSTANCE_GAME=1 INSTANCE_PHYSICS=2 INSTANCE_RENDER=3 …
  // RESTYPE_GAME=8 … RESTYPE_MAX=22). Same +0x4C as unload / setParent.
  // Prologue inlined (not shared helper): id @ 0x0047D290 = [handle+8];
  // getParentID @ 0x0047D3E0 = thiscall sub_48AB80.
  // Host: R(self).type ↔ [inner+0x4C]. !self = handle 0 (log not mirrored).
  if (!self) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  return R(self).type;
}
int32_t java_util_resource_ResourceRef_id(InvObject* self) {
  // PE @ 0x0047D290 size 0x6d (109): Unbox this (JVM_UnboxArg @ 0x0045D910).
  // Handle via dword_62E008 (JVM_vm_get_int_field @ 0x0042AB50).
  // Handle 0 → CRT_strcat_n_thunk("!" @ 0x612D80 + "Mighty ERROR" @
  // 0x612D84) into Engine_ErrorLogBuf @ 0x62E018, Engine_ErrorLogPrintf,
  // clear buf, return 0. No inner/[handle+0xC] check (unlike type @
  // 0x0047D210). Success: dword [handle+8] resource id — not [inner+0x4C]
  // RESTYPE and not thiscall sub_48AB80 (getParentID @ 0x0047D3E0).
  // Unbox+field inlined. Host: R(self).id ↔ [handle+8]. !self = handle 0
  // (log not mirrored).
  if (!self) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  return R(self).id;
}
int32_t java_util_resource_ResourceRef_getParentID(InvObject* self) {
  // PE @ 0x0047D3E0 size 0xcd: Unbox this. Handle via dword_62E008.
  // Handle 0 → Mighty ERROR at Natives.cpp line 1137 (0x471) then 0.
  // Else thiscall sub_48AB80 (size 0x11, 1 xref): inner=[handle+0xC];
  // inner==0 → 0 (NO Mighty); else [[inner+0x14]+0x50] parent Native.ptr.
  // Host: parent_id in id() space. !self = handle 0.
  if (!self) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  return R(self).parent_id;
}
InvObject* java_util_resource_ResourceRef_getParent(InvObject* self) {
  int32_t pid = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    pid = R(self).parent_id;
  }
  return make_bound_ref(pid);
}
InvObject* java_util_resource_ResourceRef_getFirstChild(InvObject* self) {
  int32_t id = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    id = R(self).id;
  }
  return make_bound_ref(rpak_first_child_id(id));
}
InvObject* java_util_resource_ResourceRef_getNextChild(InvObject* self) {
  // PE @ 0x0047D6F0 size 0xba (186): twin of getFirstChild @ 0x0047D630
  // (same size / Unbox / dword_62E008 / Engine_malloc(16) zeroed scratch /
  // ResourceRef_boxFromHandle @ 0x0047D300 on success / Engine_free else).
  // Sole delta: thiscall sub_48D300 @ 0x0048D300 size 0xcd (vs getFirstChild
  // sub_48D230 @ 0x0048D230 size 0xc6). Both: ecx=handle, push scratch;
  // inner=[handle+0xC]. getFirstChild target=[[inner]+0x20] first-child
  // node; getNextChild target=[[inner]+4] next-sibling node. Both require
  // node!=0 and [node+4]!=0, else return [scratch+8] (often 0). Link
  // scratch into node list @ +0x48; [scratch+8]=[node+0x50] Native.ptr;
  // non-0 → box NEW ResourceRef, else free scratch → null. Java
  // (ResourceRef.countChildNodes / getChildNodes): loop =
  // getFirstChild(); loop=loop.getNextChild(). Host: next sibling in
  // id() space via rpak_next_sibling_id (contrast getFirstChild →
  // rpak_first_child_id).
  int32_t id = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    id = R(self).id;
  }
  return make_bound_ref(rpak_next_sibling_id(id));
}
InvObject* java_util_resource_ResourceRef_getWTRoot(InvObject* self) {
  return self;
}
void java_util_resource_ResourceRef_makeTexture(InvObject* self, InvObject* parent,
                                               InvObject* filename) {
  // PE @ 0x0047FFE0 size 0x11c (284). Unbox this+parent+String (JVM_UnboxArg
  // @ 0x0045D910). Handle via dword_62E008 (JVM_vm_get_int_field @
  // 0x0042AB50). Guard: handle==0 OR parent handle==0 OR filename==null →
  // silent ret (NO Mighty ERROR). Else Engine_malloc(strlen+0x1D) +
  // Util_Sprintf "sourcefile %s\r\nflags %d\r\n" (flags=0 literal) →
  // ResourceEngine_type_texture @ 0x539170 (parent, blob, size): AllocLocalRid,
  // RESTYPE=7, alias "_texture", [res+0x54]|=0x200, then same Native.ptr
  // unlink/relink as set(I) (inner at [handle+0xC], id at [handle+8], list
  // head resource+0x48). Contrast makeSound @ 0x00480100 size 0x11c: same
  // unbox/guards/sprintf/bind, but factory sub_539230 (RESTYPE=6, "_sfx",
  // 1 xref) — not ResourceEngine_type_texture. Host: type=7 + D3D upload
  // stand-in for factory; !self/!parent/!fn = PE silent guards.
  if (!self || !parent) return;
  const char* fn = string_cstr(filename);
  if (!fn) return;
  std::string resolved;
  if (fn[0]) {
    resolved = rpak_resolve_path(fn);
    if (resolved.empty()) resolved = fn;
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& r = R(self);
    r.id = g_next_id++;
    r.type = 7;  // RESTYPE_TEXTURE (PE ResourceEngine_type_texture v9[0]=7)
    r.parent = parent;
    r.parent_id = R(parent).id;
    r.entry_path = fn;
    r.loaded = true;
  }
  if (!resolved.empty()) {
    render_d3d9_texture_create_from_file(self, resolved.c_str());
  }
}
void java_util_resource_ResourceRef_makeSound(InvObject* self, InvObject* parent,
                                              InvObject* filename) {
  // PE @ 0x00480100 size 0x11c — twin of makeTexture @ 0x0047FFE0. Same
  // UnboxArg/guards/sprintf blob; factory ResourceEngine_type_sfx @
  // 0x539230 (RESTYPE=6, alias "_sfx"). Silent if handle/parent/fn null.
  if (!self || !parent) return;
  const char* fn = string_cstr(filename);
  if (!fn) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.id = g_next_id++;
  r.type = 6;  // RESTYPE_SOUND (PE ResourceEngine_type_sfx v9[0]=6)
  r.parent = parent;
  r.parent_id = R(parent).id;
  r.entry_path = fn;
  r.loaded = true;
}
void java_util_resource_ResourceRef_scaleMesh(InvObject* self, float x, float y,
                                              float z) {
  // PE @ 0x00480390: Unbox this+FFF. Handle else Mighty ERROR (not mirrored).
  // ResourceRef_applyScaleMesh @ 0x0048E7F0 (unique xref): native+0xC, LOD
  // vtable+0x14(1.0f) if +0x4C!=1, sub_5447D0(0x80000000) not mirrored,
  // geom vtable+0x4C(scale xyz) bakes vertices. Not instance MeshXform.
  // RectangleTemplate: duplicate → scaleMesh(w,h,1) → changeResource.
  if (!self) return;
  if (!render_d3d9_mesh_ready(self)) return;
  render_d3d9_mesh_scale_vertices(self, x, y, z);
}
void java_util_resource_ResourceRef_duplicate(InvObject* self, InvObject* src) {
  // PE @ 0x004802C0 → ResourceHandle_bindOrClone @ 0x0048D860.
  // Types 5/7/13/14 (mesh/tex/light/render-obj): Resource_cloneNative then bind.
  // Else share like set(ResourceRef). Null this/src = no-op.
  if (!self || !src) return;
  int32_t typ = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    R(self) = R(src);
    typ = R(src).type;
  }
  const bool clone_pe = (typ == 5 || typ == 7 || typ == 13 || typ == 14);
  int32_t cloned = 0;
  if (render_d3d9_mesh_ready(src)) {
    cloned = render_d3d9_mesh_clone(self, src) ? 1 : 0;
  } else if (clone_pe) {
    java_util_resource_ResourceRef_load(self);
    cloned = 1;
  }
  tree_field_set_int(self, "dup_cloned", cloned);
  tree_field_set_int(self, "dup_src_id", java_util_resource_ResourceRef_id(src));
  gameref_on_res_bound(self);
}

// ---- RenderRef ----
void java_util_resource_RenderRef_create(InvObject* self, InvObject* parent,
                                         InvObject* type, InvObject* alias) {
  // PE @ 0x00480EE0: Unbox this+parent+type+alias. Handle/parent/type else
  // Mighty ERROR; parent+0xC==0 silent ret. ResourceEngine_type_renderinst
  // factory type=3 INSTANCE_RENDER; alias default "_renderinst"; bindBone
  // "bone00". Factory / sub_4290F0 not mirrored.
  // Host Camera.create calls this with type=null — keep that shim.
  if (!self) return;
  // PE Mighty ERROR if type && !parent. Camera.create shims type=null.
  if (type && !parent) return;
  const char* alias_s = alias ? string_cstr(alias) : nullptr;
  if (type && (!alias_s || !alias_s[0])) alias_s = "_renderinst";
  int32_t type_id = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& r = R(self);
    r.parent = parent;
    if (parent) r.parent_id = R(parent).id;
    if (type) {
      type_id = R(type).id;
      r.id = type_id;
      r.type = 3;  // INSTANCE_RENDER (factory v40[0]=3)
      r.type_id = type_id;
      r.entry_path = R(type).entry_path;
      r.blob_size = R(type).blob_size;
    } else {
      r.id = g_next_id++;
      r.type = 3;
    }
    if (alias_s) r.alias = alias_s;
    r.loaded = true;
  }
  if (type && render_d3d9_mesh_ready(type))
    render_d3d9_mesh_clone(self, type);
  render_d3d9_mesh_set_parent(self, parent);
  if (parent) {
    const int32_t bone00 = render_d3d9_mesh_get_bone_id(parent, "bone00");
    render_d3d9_mesh_set_attach_bone(self, bone00);
  }
  if (alias_s) tree_field_set_obj(self, "alias", string_new(alias_s));
  tree_field_set_int(self, "create_type", 3);
  gameref_on_res_bound(self);
}
int32_t java_util_resource_RenderRef_getBoneId(InvObject* self, InvObject* alias) {
  // PE @ 0x00481020 size 0x82 (130). JVM_UnboxArg @ 0x0045D910: this +
  // alias char* (JNI (Ljava.lang.String;)I, L → DWORD box+8). Handle via
  // JVM_vm_get_int_field @ 0x0042AB50 (dword_62E008). xor esi,esi then:
  // handle 0 → Mighty ERROR ("!" @ 0x6132D4 + "Mighty ERROR" @ 0x6132D8 via
  // Engine_ErrorLogBuf @ 0x62E018 / CRT_strcat_n_thunk @ 0x551140 n=0x100 /
  // Engine_ErrorLogPrintf @ 0x5513B0), eax=esi=0. Never -1.
  // Else thiscall RenderRef_bindBone @ 0x0048BC40(handle, alias): [this+0xC]
  // scene; null / sub_5447D0 fail / vtbl+0xC==0 → 0. Else
  // RenderInst_findOrInsertBone @ 0x5411B0 (sub_5D7190 case-insens; match →
  // id@node+0x34; miss → midpoint low=1 high=0xFFFF or 0 alloc fail). Never
  // -1. sub_5447D0 / sub_5D7190 not renamed. Xref Natives_RegisterAll
  // @ 0x00489B00.
  // Host: !self → 0 (no Mighty). Null alias → nullptr (not string_cstr
  // "<null>"); render_d3d9_mesh_get_bone_id stand-in (bone00/root/empty→0;
  // sequential ids from 1; not PE BST midpoint). No bindBone/scene APIs.
  if (!self) return 0;
  const char* name = alias ? string_cstr(alias) : nullptr;
  return render_d3d9_mesh_get_bone_id(self, name);
}
void java_util_resource_RenderRef_setMatrix(InvObject* self, int32_t bone_id,
                                            InvObject* bone_ref, InvObject* pos,
                                            InvObject* ori) {
  // PE @ 0x004810B0 size 0x188. Unbox this, bone_id, bone_ref, pos, ori
  // (box+8; Hex-Rays dests lie). Handle 0 → Mighty ERROR (host: ret).
  // Null pos/ori → 0,0,0 (test before get_float_field). sub_551C70 stores YPR.
  // No RenderRef_bindBone. sub_48BF50(handle, bone_id, bone_ref, pos*, ypr*):
  // bone_id looked up on THIS slot (sub_5413C0 ecx=slot); bone_ref is unboxed
  // L (object/0), no get_int_field. bone_ref==0 → sub_48BE10 unlink (own list).
  // PE has no bone_ref==self compare. Contrast 2-arg @ 0x00481240: bindBone
  // ("bone00") + bone_ref=0. Do not call setMatrix_1.
  if (!self) return;
  float x = 0, y = 0, z = 0;
  vec3_get(pos, &x, &y, &z);
  float yaw = 0, pitch = 0, roll = 0;
  ypr_get(ori, &yaw, &pitch, &roll);
  if (!bone_ref || bone_ref == self) {
    // Host: pose this's bone_id. PE always writes this's bone (lookup fail →
    // skip). bone_ref==self is host-only; smoke parent.setMatrix(id, self).
    render_d3d9_mesh_set_bone_local(self, bone_id, x, y, z, yaw, pitch, roll);
    return;
  }
  // bone_ref is another instance: PE parent-links this's bone to bone_ref.
  // Host: instance local pose + attach under bone_ref at bone_id.
  float sx, sy, sz;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& r = R(self);
    r.px = x;
    r.py = y;
    r.pz = z;
    r.oy = yaw;
    r.op = pitch;
    r.or_ = roll;
    sx = r.sx;
    sy = r.sy;
    sz = r.sz;
  }
  render_d3d9_mesh_set_transform(self, x, y, z, yaw, pitch, roll, sx, sy, sz);
  render_d3d9_set_flare_world(self, x, y, z);
  render_d3d9_mesh_set_parent(self, bone_ref);
  render_d3d9_mesh_set_attach_bone(self, bone_id);
}
void java_util_resource_RenderRef_setMatrix_1(InvObject* self, InvObject* pos,
                                              InvObject* ori) {
  // PE @ 0x00481240: Unbox this, pos, ori. Handle 0 → Mighty ERROR (host: ret).
  // Null pos/ori → 0,0,0 (test before get_float_field). sub_551C70 copies YPR.
  // RenderRef_bindBone("bone00") then sub_48BF50(handle, boneId, 0, pos*, ypr*).
  // Contrast 4-arg @ 0x004810B0 — not this ticket.
  if (!self) return;
  float x = 0, y = 0, z = 0;
  vec3_get(pos, &x, &y, &z);
  float yaw = 0, pitch = 0, roll = 0;
  ypr_get(ori, &yaw, &pitch, &roll);
  float sx, sy, sz;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& r = R(self);
    r.px = x;
    r.py = y;
    r.pz = z;
    r.oy = yaw;
    r.op = pitch;
    r.or_ = roll;
    sx = r.sx;
    sy = r.sy;
    sz = r.sz;
  }
  render_d3d9_mesh_set_transform(self, x, y, z, yaw, pitch, roll, sx, sy, sz);
  render_d3d9_set_flare_world(self, x, y, z);
  // PE RenderRef_bindBone("bone00") + sub_48BF50(handle, boneId, 0, pos, ypr)
  // poses this instance's root bone (bone_ref=0). Host attach_bone is the
  // PARENT bone index (create() already binds parent's bone00). Do not
  // overwrite it with self's bone00 id.
}
InvObject* java_util_resource_RenderRef_getPos(InvObject* self) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  return vec3_new(r.px, r.py, r.pz);
}
void java_util_resource_RenderRef_changeResource(InvObject* self,
                                                 InvObject* oldtexture,
                                                 InvObject* newtexture) {
  // PE @ 0x00480220: Unbox this+old+new (defaults 0). Handle + native+0xC.
  // native+0x4C!=1 → vtable+0x14(1.0). sub_5447D0(0x80000001,0,0) LOD gate
  // not mirrored. slot=vtable+0x0C(1.0); slot.vtable+8(old,new) replace by
  // native identity (tex OR mesh). Painter/Navigator/RectangleTemplate.
  if (!self) return;
  const int32_t old_id =
      oldtexture ? java_util_resource_ResourceRef_id(oldtexture) : 0;
  const int32_t new_id =
      newtexture ? java_util_resource_ResourceRef_id(newtexture) : 0;
  const bool new_is_mesh =
      newtexture && render_d3d9_mesh_ready(newtexture);
  const bool old_is_mesh =
      oldtexture && render_d3d9_mesh_ready(oldtexture);

  if (new_is_mesh || old_is_mesh) {
    if (new_is_mesh) render_d3d9_mesh_clone(self, newtexture);
    InvObject* keep = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      keep = R(self).swapped_tex;
    }
    if (keep && render_d3d9_texture_ready(keep))
      render_d3d9_mesh_set_texture(self, keep);
  } else {
    if (newtexture && !render_d3d9_texture_ready(newtexture) && new_id != 0)
      java_util_resource_ResourceRef_load(newtexture);
    const int32_t nsub = render_d3d9_mesh_submesh_count(self);
    int32_t replaced = 0;
    for (int32_t i = 0; i < nsub; ++i) {
      void* cur = render_d3d9_mesh_get_texture(self, i);
      int32_t cid = 0;
      if (cur) {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_res.find(static_cast<InvObject*>(cur));
        if (it != g_res.end()) cid = it->second.id;
      }
      const bool match =
          (cur == oldtexture) || (old_id != 0 && cid == old_id) ||
          (!oldtexture && !cur);
      if (!match) continue;
      render_d3d9_mesh_set_texture_at(self, i, newtexture);
      ++replaced;
    }
    // Painter: `new ResourceRef(misc.garage:0x0103r)` vs bound/owned tex.
    if (!replaced && nsub > 0)
      render_d3d9_mesh_set_texture(self, newtexture);
    {
      std::lock_guard<std::mutex> lock(g_mu);
      R(self).swapped_tex = newtexture;
    }
    if (newtexture) tree_field_set_obj(self, "swapped_tex", newtexture);
  }

  {
    std::lock_guard<std::mutex> lock(g_mu);
    R(self).swapped_tex_id = new_id;
  }
  tree_field_set_int(self, "swapped_tex_id", new_id);
  if (oldtexture) tree_field_set_int(self, "swap_old_tex_id", old_id);
}
void java_util_resource_RenderRef_setColor(InvObject* self, int32_t color) {
  // PE @ 0x00480310: Unbox this+I. Handle else Mighty ERROR (not mirrored).
  // sub_48C8C0 (multi-xref, not renamed): slot+0xCC = color as-is. No
  // Light_byteToUnit. Mesh vtable / sub_5447D0 not mirrored. D3D material
  // uses the stored DWORD (/255 like existing submesh.diffuse).
  if (!self) return;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    R(self).color = color;
  }
  tree_field_set_int(self, "color", color);
  render_d3d9_mesh_set_color(self, color);
}
void java_util_resource_RenderRef_setLight(InvObject* self, int32_t diffuse,
                                           int32_t ambient, int32_t specular) {
  // PE @ 0x00486AB0: Unbox this+III (defaults 0xFFFFFF/0x404040/0xFFFFFF);
  // handle dword_62E008; RenderRef_applyLight @ 0x0048C9D0 (xref unique).
  // Mesh vtable / sub_419860 / dir a5 not mirrored — D3D bind uses * 1/256.
  if (self) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& r = R(self);
    r.light_diffuse = diffuse;
    r.light_ambient = ambient;
    r.light_specular = specular;
    tree_field_set_int(self, "light_diffuse", diffuse);
    tree_field_set_int(self, "light_ambient", ambient);
    tree_field_set_int(self, "light_specular", specular);
  }
  render_d3d9_set_light(diffuse, ambient, specular);
}
void java_util_resource_RenderRef_setFlare(InvObject* self, InvObject* glowtexture,
                                           int32_t glowColor, float glowMinSize,
                                           float glowMaxSize, int32_t flareCount,
                                           int32_t rayCount) {
  // PE @ 0x00486B20: Unbox this+tex+color+min+max+count+rays; handle
  // dword_62E008; RenderRef_applyFlare @ 0x0048CB40 (xref unique).
  // Stores as-is (+0xD4 min, +0xD8 max, +0xE0 color, +0xE4 rays, +0x100 count).
  // Mesh vtable / sub_419860 / sub_429060 handle list not mirrored.
  if (!self) return;
  const int32_t tex_id =
      glowtexture ? java_util_resource_ResourceRef_id(glowtexture) : 0;
  if (glowtexture && !render_d3d9_texture_ready(glowtexture))
    java_util_resource_ResourceRef_load(glowtexture);
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.flare_tex_id = tex_id;
  r.flare_color = glowColor;
  r.flare_min = glowMinSize;
  r.flare_max = glowMaxSize;
  r.flare_count = flareCount;
  r.flare_rays = rayCount;
  tree_field_set_int(self, "flare_tex_id", tex_id);
  tree_field_set_int(self, "flare_color", glowColor);
  tree_field_set_float(self, "flare_min", glowMinSize);
  tree_field_set_float(self, "flare_max", glowMaxSize);
  tree_field_set_int(self, "flare_count", flareCount);
  tree_field_set_int(self, "flare_rays", rayCount);
  if (glowtexture) tree_field_set_obj(self, "flare_tex", glowtexture);
  const float wx = r.px, wy = r.py, wz = r.pz;
  render_d3d9_set_flare(self, glowtexture, glowColor, glowMinSize, glowMaxSize,
                        flareCount, rayCount);
  render_d3d9_set_flare_world(self, wx, wy, wz);
}
void java_util_resource_RenderRef_setType(InvObject* self, InvObject* type) {
  // PE @ 0x00486BA0: Unbox this+type. RenderRef_applyType @ 0x0048C970
  // (xref unique): native+0xC, LOD, slot=vtable+0x0C; sub_5439E0 binds type
  // native (also factory — not mirrored). Host: type_id=id(type), tag=3.
  if (!self) return;
  const int32_t tid = type ? java_util_resource_ResourceRef_id(type) : 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& r = R(self);
    r.type_id = tid;
    if (type) r.type = 3;  // INSTANCE_RENDER
  }
  tree_field_set_int(self, "type_id", tid);
}
int32_t java_util_resource_RenderRef_getTypeID(InvObject* self) {
  // PE @ 0x00486BE0 size 0x97 (151). Unbox this. Handle dword_62E008.
  // Handle 0 → Mighty ERROR ("!" @ 0x61397C + "Mighty ERROR" @ 0x613980)
  // then 0. Contrast ResourceRef.id @ 0x0047D290 size 0x6d: [handle+8],
  // no inner; !self → 0. Here: inner=[handle+0xC]; inner==0 or
  // [inner+0x4C]!=3 INSTANCE_RENDER → 0 (NO Mighty). Else
  // sub_419860(inner, 0xA0000001, 1.0, 0.0, 10.0) slot; slot==0 → 0;
  // else [slot+0x80] packed type id (MouseCursor particles:0x0024).
  // sub_419860 (198 xrefs) / sub_551140 / sub_5513B0 not renamed.
  // Host: R(self).type_id; !self = handle 0. sub_419860 not mirrored.
  if (!self) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  const auto& r = R(self);
  if (r.type != 3) return 0;
  return r.type_id;
}
int32_t java_util_resource_RenderRef_lineCreate(InvObject* self, InvObject* parent,
                                                InvObject* type) {
  // PE @ 0x0047FE70: Unbox this+parent+type. Handle && parent && type else 0.
  // sub_48A670(parent,type,0,0,"line") factory (also plotRoute / sub_4518C0,
  // not renamed). Return 1. RaceSetup uses plotRoute, not this Java site.
  if (!self || !parent || !type) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  if (!r.id) r.id = g_next_id++;
  r.type = 3;
  r.parent = parent;
  r.parent_id = R(parent).id;
  r.type_id = R(type).id;
  r.loaded = true;
  LineState& ln = g_lines[self];
  ln = LineState{};
  ln.active = true;
  ln.parent = parent;
  ln.type_id = r.type_id;
  return 1;
}

void java_util_resource_RenderRef_lineAdd(InvObject* self, InvObject* pos,
                                          InvObject* normal, int32_t color,
                                          float width) {
  // PE @ 0x0047FEE0: color default -1, width default 1.0f. Handle+8==0 no-op
  // (no auto-create). RenderRef_applyLineAdd @ 0x0048A860.
  if (!self) return;
  float x = 0, y = 0, z = 0, nx = 0, ny = 1, nz = 0;
  if (pos) vec3_get(pos, &x, &y, &z);
  if (normal) vec3_get(normal, &nx, &ny, &nz);
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_lines.find(self);
  if (it == g_lines.end() || !it->second.active) return;
  LineVert v;
  v.x = x;
  v.y = y;
  v.z = z;
  v.nx = nx;
  v.ny = ny;
  v.nz = nz;
  v.color = color;
  v.width = width;
  it->second.verts.push_back(v);
}

int32_t java_util_resource_RenderRef_plotRoute(InvObject* self, InvObject* parent,
                                               InvObject* type, int32_t color,
                                               float step, InvObject* scale) {
  // PE @ 0x00483960: no getRouteLength spline (dword_6408D0) → 0. Unbox
  // this+parent+type+color(-1)+step(1.0)+scale. Factory like lineCreate.
  // Scale x/z (y discarded). t=0..1 dt=step/span. Y:=0xBA03126F (-0.0005).
  // Return 1. RaceSetup: localroot + particles:0x17. Host keeps world XZ
  // (OSD stand-in already *0.01 vs player); PE bakes xz under scaled root.
  if (!self || !parent || !type) return 0;
  float sx = 1.f, sy = 0.f, sz = 1.f;
  if (scale) vec3_get(scale, &sx, &sy, &sz);
  (void)sy;
  const float sample = step > 0.f ? step : 1.f;

  std::vector<RoutePt> route;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    route = g_last_route;
  }
  if (route.size() < 2) return 0;
  if (java_util_resource_RenderRef_lineCreate(self, parent, type) == 0)
    return 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    LineState& ln = g_lines[self];
    ln.color = color;
    ln.sx = sx;
    ln.sy = 0.f;
    ln.sz = sz;
  }

  constexpr float kPlotRouteY = -0.0005f;  // 0xBA03126F
  auto emit = [&](float x, float z) {
    java_util_resource_RenderRef_lineAdd(self, vec3_new(x, kPlotRouteY, z),
                                         vec3_new(0.f, 1.f, 0.f), color,
                                         sx > 1e-4f ? sx : 0.01f);
  };

  float total = 0.f;
  std::vector<float> cum(route.size(), 0.f);
  for (size_t i = 1; i < route.size(); ++i) {
    const float dx = route[i].x - route[i - 1].x;
    const float dy = route[i].y - route[i - 1].y;
    const float dz = route[i].z - route[i - 1].z;
    total += std::sqrt(dx * dx + dy * dy + dz * dz);
    cum[i] = total;
  }
  emit(route[0].x, route[0].z);
  if (total > 1e-3f) {
    for (float d = sample; d < total - 1e-3f; d += sample) {
      size_t i = 1;
      while (i < cum.size() && cum[i] < d) ++i;
      if (i >= cum.size()) break;
      const float d0 = cum[i - 1], d1 = cum[i];
      const float u = (d1 > d0 + 1e-6f) ? (d - d0) / (d1 - d0) : 0.f;
      emit(route[i - 1].x + (route[i].x - route[i - 1].x) * u,
           route[i - 1].z + (route[i].z - route[i - 1].z) * u);
    }
  }
  emit(route.back().x, route.back().z);
  return 1;
}

void java_util_resource_RenderRef_addPoints(InvObject* self, InvObject* v,
                                            float width, InvObject* normal) {
  if (!self || !v) return;
  float nx = 0, ny = 1, nz = 0;
  if (normal) vec3_get(normal, &nx, &ny, &nz);
  const int32_t n = tree_vector_size(v);
  std::lock_guard<std::mutex> lock(g_mu);
  LineState& ln = g_lines[self];
  ln.active = true;
  for (int32_t i = 0; i < n; ++i) {
    InvObject* p = tree_vector_element_at(v, i);
    float x = 0, y = 0, z = 0;
    if (p) vec3_get(p, &x, &y, &z);
    LineVert vert;
    vert.x = x;
    vert.y = y;
    vert.z = z;
    vert.nx = nx;
    vert.ny = ny;
    vert.nz = nz;
    vert.width = width;
    ln.weld_pts.push_back(vert);
    ln.verts.push_back(vert);
  }
  if (n >= 2) {
    float len = 0.f;
    for (int32_t i = 1; i < n; ++i) {
      InvObject* a = tree_vector_element_at(v, i - 1);
      InvObject* b = tree_vector_element_at(v, i);
      float ax = 0, ay = 0, az = 0, bx = 0, by = 0, bz = 0;
      if (a) vec3_get(a, &ax, &ay, &az);
      if (b) vec3_get(b, &bx, &by, &bz);
      const float dx = bx - ax, dy = by - ay, dz = bz - az;
      len += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    if (len > ln.target_len) ln.target_len = len;
  }
}

int32_t java_util_resource_RenderRef_weld(InvObject* self, InvObject* point,
                                          InvObject* line, float power) {
  if (!self) return 0;
  float px = 0, py = 0, pz = 0, lx = 0, ly = 0, lz = 0;
  if (point) vec3_get(point, &px, &py, &pz);
  if (line) vec3_get(line, &lx, &ly, &lz);
  const float seg = std::sqrt(lx * lx + ly * ly + lz * lz);
  std::lock_guard<std::mutex> lock(g_mu);
  LineState& ln = g_lines[self];
  ln.active = true;
  if (seg > ln.target_len) ln.target_len = seg > 1e-3f ? seg : 1.f;
  LineVert v;
  v.x = px;
  v.y = py;
  v.z = pz;
  v.width = 0.02f;
  ln.weld_pts.push_back(v);
  ln.verts.push_back(v);
  const float denom = ln.target_len > 1e-3f ? ln.target_len : 1.f;
  ln.progress += (power > 0.f ? power : 0.f) / denom;
  if (ln.progress > 1.f) ln.progress = 1.f;
  return ln.progress >= 1.f - 1e-4f ? 1 : 0;
}

float java_util_resource_RenderRef_progress(InvObject* self) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_lines.find(self);
  if (it == g_lines.end()) return 0.f;
  return it->second.progress;
}

// ---- PhysicsRef ----
void java_util_resource_PhysicsRef_create(InvObject* self, InvObject* parent,
                                          int32_t typeRid, InvObject*) {
  // PE @ 0x004807F0 size 0x12c: Unbox this+parent+typeRid+alias.
  // Handle 0 OR parent==0 → Mighty ERROR ("!" @ 0x6131EC + "Mighty ERROR"
  // @ 0x6131F0). Host: !self ret; !parent still creates (smokes/TREE pass
  // nullptr; PE would log Mighty). typeRid: sub_545FC0 → sub_536820;
  // sub_49A990; body+0x9C |= 1; sub_48A1E0 → INSTANCE_PHYSICS=2.
  // Not createBox/createSphere (Physics_createPrimitive @ 0x0049AB60).
  if (!self) return;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& r = R(self);
    r.parent = parent;
    r.parent_id = parent ? R(parent).id : 0;
    r.type = 2;  // INSTANCE_PHYSICS
    r.type_id = typeRid;
    r.id = g_next_id++;
    r.shape = 0;
    r.asleep = 0;
    r.is_static = 0;
    r.vx = r.vy = r.vz = 0;
    r.wx = r.wy = r.wz = 0;
  }
  gameref_on_res_bound(self);
}
void java_util_resource_PhysicsRef_createBox(InvObject* self, InvObject* parent,
                                             float x, float y, float z,
                                             InvObject* alias) {
  (void)alias;
  java_util_resource_PhysicsRef_create(self, parent, 1, nullptr);
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.shape = 1;
  // PE 0x004805B0: Java x,y,z are full sizes; Physics_createPrimitive
  // @ 0x0049AB60 type=5 stores half-extents (x*0.5, y*0.5, z*0.5).
  r.hx = x * 0.5f;
  r.hy = y * 0.5f;
  r.hz = z * 0.5f;
}
void java_util_resource_PhysicsRef_createSphere(InvObject* self, InvObject* parent,
                                                float radius, InvObject* alias) {
  // PE @ 0x004806F0 size 0xFF. Unbox this/parent/r/alias. Handle 0 OR parent
  // 0 → Mighty ERROR. Physics_createPrimitive type=1, radius RAW (no
  // flt_5F09D0 ×0.5). Contrast createBox @ 0x004805B0 type=5 half-extents.
  // *(body+0x9C)|=1 then sub_48A1E0. Host: create() typeRid=2
  // (INSTANCE_PHYSICS), not primitive type 1. Keep parent-null (probes).
  (void)alias;
  java_util_resource_PhysicsRef_create(self, parent, 2, nullptr);
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.shape = 2;
  r.hx = radius;
  r.hy = r.hz = 0;
}
void java_util_resource_PhysicsRef_setMatrix(InvObject* self, InvObject* pos,
                                             InvObject* ori) {
  // PE @ 0x00480920: Unbox this+Vector3+Ypr. Handle 0 → Mighty ERROR.
  // Null Vector3 → pos 0,0,0 (stores before jz). sub_551C70(0,0,0) zeros
  // YPR; Ypr fields overwrite if non-null. sub_48AEA0(handle); pos into
  // both ping-pong slots at base+212*index+100; sub_54ECD0(ypr);
  // sub_54FF20; sub_4986F0. No freeze-in-place skip. Null,null = origin.
  // PE does not set an asleep flag here — do not invent asleep=1.
  if (!self) return;
  float x = 0.f, y = 0.f, z = 0.f;
  if (pos) vec3_get(pos, &x, &y, &z);
  float yaw = 0.f, pitch = 0.f, roll = 0.f;
  if (ori) ypr_get(ori, &yaw, &pitch, &roll);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& r = R(self);
    r.px = x;
    r.py = y;
    r.pz = z;
    r.oy = yaw;
    r.op = pitch;
    r.or_ = roll;
    r.vx = r.vy = r.vz = 0;
    r.wx = r.wy = r.wz = 0;
    r.pose_set = 1;
  }
  render_d3d9_mesh_set_transform(self, x, y, z, yaw, pitch, roll, 1.f, 1.f, 1.f);
}
void java_util_resource_PhysicsRef_setStatic(InvObject* self, int32_t mode) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.is_static = mode ? 1 : 0;
  if (r.is_static) {
    r.vx = r.vy = r.vz = 0;
    r.wx = r.wy = r.wz = 0;
  }
}
InvObject* java_util_resource_PhysicsRef_getPos(InvObject* self) {
  // PE @ 0x00480B00: Unbox this only. Handle via dword_62E008.
  // handle==0 → Mighty ERROR, return nullptr. No *(handle+8) skip
  // (unlike GameRef.getPos @ 0x0047DAD0). Always alloc Vector3 0x1C
  // when handle≠0: sub_48AEA0 (101 xrefs, not renamed); xyz from
  // *(body+132)+212*slot+100. Fallback reads unboxed this as xyz.
  // Host: !self → nullptr. Existing object (even unposed / pose_set==0)
  // → Vector3 from cached px/py/pz (zeros if never setMatrix).
  if (!self) return nullptr;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  return vec3_new(r.px, r.py, r.pz);
}
InvObject* java_util_resource_PhysicsRef_getVel(InvObject* self) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  return vec3_new(r.vx, r.vy, r.vz);
}
InvObject* java_util_resource_PhysicsRef_getVel_1(InvObject* self, InvObject* pos) {
  // Point velocity: v + ω × (p - origin). ω ≈ (wx, wy, wz).
  float px, py, pz;
  vec3_get(pos, &px, &py, &pz);
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  const float rx = px - r.px, ry = py - r.py, rz = pz - r.pz;
  const float vx = r.vx + (r.wy * rz - r.wz * ry);
  const float vy = r.vy + (r.wz * rx - r.wx * rz);
  const float vz = r.vz + (r.wx * ry - r.wy * rx);
  return vec3_new(vx, vy, vz);
}
InvObject* java_util_resource_PhysicsRef_getAngVel(InvObject* self) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  return vec3_new(r.wx, r.wy, r.wz);
}
InvObject* java_util_resource_PhysicsRef_getOri(InvObject* self) {
  // PE @ 0x00480C10: Unbox this only. Handle 0 → Mighty ERROR + nullptr.
  // No handle+8 skip. Always alloc Ypr 0x1C when handle≠0.
  // sub_48AEA0; if body: Ypr_fromMatrix @ 0x00551C90 (matrix at +164).
  // If helper==0, PE still news Ypr from uninit stack.
  // Host: !self → nullptr. Else Ypr from cached oy/op/or_ (zeros if unposed).
  if (!self) return nullptr;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  return ypr_new(r.oy, r.op, r.or_);
}

void physics_set_velocity(InvObject* self, float vx, float vy, float vz) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.vx = vx;
  r.vy = vy;
  r.vz = vz;
  if (vx != 0.f || vy != 0.f || vz != 0.f) r.asleep = 0;
}

void physics_set_asleep(InvObject* self, int32_t asleep) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  R(self).asleep = asleep ? 1 : 0;
}

int32_t physics_is_asleep(InvObject* self) {
  if (!self) return 1;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_res.find(self);
  return it == g_res.end() ? 1 : it->second.asleep;
}

void physics_set_ang_vel(InvObject* self, float wx, float wy, float wz) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.wx = wx;
  r.wy = wy;
  r.wz = wz;
  if (wx != 0.f || wy != 0.f || wz != 0.f) r.asleep = 0;
}

void physics_set_ground_y(float y) { g_ground_y = y; }

float physics_ground_y() { return g_ground_y; }

void physics_road_clear() {
  std::lock_guard<std::mutex> lock(g_mu);
  g_roads.clear();
  g_last_route.clear();
  g_last_route_len = 0.f;
  g_last_route_u0 = 0.f;
  g_last_route_u1 = 0.f;
}

void physics_road_add_segment(float x0, float y0, float z0, float x1, float y1,
                              float z1) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_roads.push_back(RoadSeg{x0, y0, z0, x1, y1, z1});
}

int32_t physics_road_count() {
  std::lock_guard<std::mutex> lock(g_mu);
  return static_cast<int32_t>(g_roads.size());
}

void physics_road_clear_occupied() {
  std::lock_guard<std::mutex> lock(g_mu);
  for (RoadSeg& s : g_roads) s.occupied = 0;
}

void physics_road_mark_occupied_at(float x, float y, float z) {
  (void)y;
  // PE GroundMap_markPathOccupied: path+196=1 on every path of the cross.
  // Host: mark segments whose XZ projection is within 1 m of the junction.
  std::lock_guard<std::mutex> lock(g_mu);
  constexpr float kR2 = 1.f;
  for (RoadSeg& s : g_roads) {
    const float ax = s.x1 - s.x0;
    const float az = s.z1 - s.z0;
    const float len2 = ax * ax + az * az;
    float t = 0.f;
    if (len2 > 1e-8f) {
      t = ((x - s.x0) * ax + (z - s.z0) * az) / len2;
      if (t < 0.f) t = 0.f;
      if (t > 1.f) t = 1.f;
    }
    const float qx = s.x0 + t * ax;
    const float qz = s.z0 + t * az;
    const float dx = x - qx;
    const float dz = z - qz;
    if (dx * dx + dz * dz <= kR2) s.occupied = 1;
  }
}

int32_t physics_road_occupied_count() {
  std::lock_guard<std::mutex> lock(g_mu);
  int32_t n = 0;
  for (const RoadSeg& s : g_roads)
    if (s.occupied) ++n;
  return n;
}

bool physics_road_random_spawn(float* out_x, float* out_y, float* out_z,
                               float* out_yaw) {
  // PE Traffic_trySpawnOnRandomPath @ 0x0057B420: count empty paths
  // (byte +196==0), CRT_rand % n, walk list skip occupied, sample pos.
  std::lock_guard<std::mutex> lock(g_mu);
  std::vector<int> empty;
  empty.reserve(g_roads.size());
  for (int i = 0; i < static_cast<int>(g_roads.size()); ++i) {
    if (!g_roads[static_cast<size_t>(i)].occupied) empty.push_back(i);
  }
  if (empty.empty()) return false;
  const int n = static_cast<int>(empty.size());
  const int idx = empty[static_cast<size_t>((std::rand() & 0x7FFF) % n)];
  const RoadSeg& s = g_roads[static_cast<size_t>(idx)];
  const float u =
      static_cast<float>(std::rand() & 0x7FFF) * (1.f / 32768.f);
  const float t = 0.1f + u * 0.8f;
  const float x = s.x0 + t * (s.x1 - s.x0);
  const float y = s.y0 + t * (s.y1 - s.y0);
  const float z = s.z0 + t * (s.z1 - s.z0);
  if (out_x) *out_x = x;
  if (out_y) *out_y = y;
  if (out_z) *out_z = z;
  if (out_yaw) *out_yaw = std::atan2(s.x1 - s.x0, s.z1 - s.z0);
  return true;
}

bool physics_road_project(float x, float z, float* out_x, float* out_y,
                          float* out_z, float* out_dx, float* out_dy,
                          float* out_dz) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_roads.empty()) return false;
  float best_d2 = 1e30f;
  float bx = x, by = g_ground_y, bz = z;
  float bdx = 0.f, bdy = 0.f, bdz = 1.f;
  for (const RoadSeg& s : g_roads) {
    const float ax = s.x1 - s.x0;
    const float ay = s.y1 - s.y0;
    const float az = s.z1 - s.z0;
    const float len2 = ax * ax + az * az;
    float t = 0.f;
    if (len2 > 1e-8f) {
      t = ((x - s.x0) * ax + (z - s.z0) * az) / len2;
      if (t < 0.f) t = 0.f;
      if (t > 1.f) t = 1.f;
    }
    const float qx = s.x0 + t * ax;
    const float qy = s.y0 + t * ay;
    const float qz = s.z0 + t * az;
    const float dx = x - qx;
    const float dz = z - qz;
    const float d2 = dx * dx + dz * dz;
    if (d2 < best_d2) {
      best_d2 = d2;
      bx = qx;
      by = qy;
      bz = qz;
      const float llen = std::sqrt(len2);
      if (llen > 1e-6f) {
        // Horizontal unit tangent + rise per meter of XZ travel (pitch = atan(bdy)).
        bdx = ax / llen;
        bdy = ay / llen;
        bdz = az / llen;
      } else {
        bdx = 0.f;
        bdy = 0.f;
        bdz = 1.f;
      }
    }
  }
  if (out_x) *out_x = bx;
  if (out_y) *out_y = by;
  if (out_z) *out_z = bz;
  if (out_dx) *out_dx = bdx;
  if (out_dy) *out_dy = bdy;
  if (out_dz) *out_dz = bdz;
  return true;
}

struct RoadNode {
  float x = 0, y = 0, z = 0;
};
struct RoadEdge {
  int a = 0, b = 0;
  float len = 0;
};

constexpr float kRoadMergeEps = 0.75f;

int find_or_add_node(std::vector<RoadNode>& nodes, float x, float y, float z) {
  for (size_t i = 0; i < nodes.size(); ++i) {
    const float dx = nodes[i].x - x;
    const float dz = nodes[i].z - z;
    if (dx * dx + dz * dz <= kRoadMergeEps * kRoadMergeEps) return static_cast<int>(i);
  }
  nodes.push_back(RoadNode{x, y, z});
  return static_cast<int>(nodes.size() - 1);
}

void build_road_graph(std::vector<RoadNode>* nodes, std::vector<RoadEdge>* edges) {
  nodes->clear();
  edges->clear();
  for (const RoadSeg& s : g_roads) {
    const int a = find_or_add_node(*nodes, s.x0, s.y0, s.z0);
    const int b = find_or_add_node(*nodes, s.x1, s.y1, s.z1);
    if (a == b) continue;
    const float dx = s.x1 - s.x0;
    const float dy = s.y1 - s.y0;
    const float dz = s.z1 - s.z0;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    edges->push_back(RoadEdge{a, b, len > 1e-4f ? len : 1e-4f});
  }
}

int nearest_node(const std::vector<RoadNode>& nodes, float x, float z) {
  int best = -1;
  float best_d2 = 1e30f;
  for (size_t i = 0; i < nodes.size(); ++i) {
    const float dx = nodes[i].x - x;
    const float dz = nodes[i].z - z;
    const float d2 = dx * dx + dz * dz;
    if (d2 < best_d2) {
      best_d2 = d2;
      best = static_cast<int>(i);
    }
  }
  return best;
}

float dijkstra_len(const std::vector<RoadNode>& nodes,
                   const std::vector<RoadEdge>& edges, int src, int dst,
                   std::vector<int>* out_path) {
  if (out_path) out_path->clear();
  if (src < 0 || dst < 0) return 1e30f;
  if (src == dst) {
    if (out_path) out_path->push_back(src);
    return 0.f;
  }
  const int n = static_cast<int>(nodes.size());
  std::vector<float> dist(static_cast<size_t>(n), 1e30f);
  std::vector<int> prev(static_cast<size_t>(n), -1);
  std::vector<char> done(static_cast<size_t>(n), 0);
  dist[static_cast<size_t>(src)] = 0.f;
  for (int iter = 0; iter < n; ++iter) {
    int u = -1;
    float best = 1e30f;
    for (int i = 0; i < n; ++i) {
      if (!done[static_cast<size_t>(i)] && dist[static_cast<size_t>(i)] < best) {
        best = dist[static_cast<size_t>(i)];
        u = i;
      }
    }
    if (u < 0 || u == dst) break;
    done[static_cast<size_t>(u)] = 1;
    for (const RoadEdge& e : edges) {
      int v = -1;
      if (e.a == u) v = e.b;
      else if (e.b == u) v = e.a;
      else continue;
      const float nd = dist[static_cast<size_t>(u)] + e.len;
      if (nd < dist[static_cast<size_t>(v)]) {
        dist[static_cast<size_t>(v)] = nd;
        prev[static_cast<size_t>(v)] = u;
      }
    }
  }
  const float path_len = dist[static_cast<size_t>(dst)];
  if (out_path && path_len < 1e29f) {
    std::vector<int> rev;
    for (int cur = dst; cur >= 0; cur = prev[static_cast<size_t>(cur)]) {
      rev.push_back(cur);
      if (cur == src) break;
    }
    if (!rev.empty() && rev.back() == src) {
      out_path->assign(rev.rbegin(), rev.rend());
    }
  }
  return path_len;
}

InvObject* physics_road_nearest_cross(float ax, float ay, float az,
                                      float min_dist) {
  // Stock GroundMap_findNearestCross @ 0x00581A00: minimize
  // |len(node-approx) - targetDistance| (not "first beyond radius").
  // RaceSetup.enter: getNearestCross(pStart, 500+rnd*300).
  std::lock_guard<std::mutex> lock(g_mu);
  std::vector<RoadNode> nodes;
  std::vector<RoadEdge> edges;
  build_road_graph(&nodes, &edges);
  if (nodes.empty()) {
    return vec3_new(ax, g_ground_y, az);
  }
  int best = -1;
  float best_score = 1e30f;
  for (size_t i = 0; i < nodes.size(); ++i) {
    const float dx = nodes[i].x - ax;
    const float dy = nodes[i].y - ay;
    const float dz = nodes[i].z - az;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float score = std::fabs(d - min_dist);
    if (score < best_score) {
      best_score = score;
      best = static_cast<int>(i);
    }
  }
  if (best < 0) return vec3_new(ax, g_ground_y, az);
  const RoadNode& n = nodes[static_cast<size_t>(best)];
  return vec3_new(n.x, n.y, n.z);
}

InvObject* physics_road_start_direction(float fx, float fy, float fz, float tx,
                                        float ty, float tz) {
  (void)fy;
  (void)ty;
  const float want_x = tx - fx;
  const float want_z = tz - fz;
  const float want_len = std::sqrt(want_x * want_x + want_z * want_z);
  if (want_len < 0.05f) return nullptr;

  const bool have_roads = physics_road_count() > 0;
  if (!have_roads)
    return vec3_new(want_x / want_len, 0.f, want_z / want_len);

  float ox = fx, oy = 0.f, oz = fz, dx = 0.f, dy = 0.f, dz = 1.f;
  if (!physics_road_project(fx, fz, &ox, &oy, &oz, &dx, &dy, &dz))
    return vec3_new(want_x / want_len, 0.f, want_z / want_len);
  // Orient tangent toward destination.
  if (dx * want_x + dz * want_z < 0.f) {
    dx = -dx;
    dz = -dz;
  }
  return vec3_new(dx, 0.f, dz);
}

static float route_arc_param_xz_locked(float x, float z) {
  if (g_last_route.size() < 2) return 0.f;
  float best_d2 = 1e30f;
  float best_u = 0.f;
  float acc = 0.f;
  for (size_t i = 1; i < g_last_route.size(); ++i) {
    const RoutePt& a = g_last_route[i - 1];
    const RoutePt& b = g_last_route[i];
    const float dx = b.x - a.x, dz = b.z - a.z;
    const float len2 = dx * dx + dz * dz;
    float u_seg = 0.f;
    if (len2 > 1e-8f) u_seg = ((x - a.x) * dx + (z - a.z) * dz) / len2;
    if (u_seg < 0.f) u_seg = 0.f;
    if (u_seg > 1.f) u_seg = 1.f;
    const float px = a.x + u_seg * dx, pz = a.z + u_seg * dz;
    const float d2 = (x - px) * (x - px) + (z - pz) * (z - pz);
    const float seg = std::sqrt(len2);
    if (d2 < best_d2) {
      best_d2 = d2;
      best_u = acc + u_seg * seg;
    }
    acc += seg;
  }
  return best_u;
}

float physics_road_route_length(float x0, float y0, float z0, float x1,
                                float y1, float z1, bool* pe_ok) {
  (void)y0;
  (void)y1;
  const float eu =
      std::sqrt((x1 - x0) * (x1 - x0) + (z1 - z0) * (z1 - z0));
  (void)eu;
  std::lock_guard<std::mutex> lock(g_mu);
  std::vector<RoutePt> saved_route;
  float saved_len = 0.f;
  float saved_u0 = 0.f;
  float saved_u1 = 0.f;
  if (pe_ok) {
    saved_route = g_last_route;
    saved_len = g_last_route_len;
    saved_u0 = g_last_route_u0;
    saved_u1 = g_last_route_u1;
  }
  g_last_route.clear();
  g_last_route_len = 0.f;
  g_last_route_u0 = 0.f;
  g_last_route_u1 = 0.f;
  auto restore_cache = [&]() {
    if (!pe_ok) return;
    g_last_route = std::move(saved_route);
    g_last_route_len = saved_len;
    g_last_route_u0 = saved_u0;
    g_last_route_u1 = saved_u1;
    *pe_ok = false;
  };
  auto finish = [&](float xs0, float zs0, float xs1, float zs1) {
    const float u0 = route_arc_param_xz_locked(xs0, zs0);
    const float u1 = route_arc_param_xz_locked(xs1, zs1);
    g_last_route_u0 = u0;
    g_last_route_u1 = u1;
    g_last_route_len = std::fabs(u1 - u0);
    if (pe_ok) *pe_ok = true;
    return g_last_route_len;
  };
  auto fail = [&]() -> float {
    if (pe_ok) {
      restore_cache();
      return -1.f;
    }
    g_last_route.push_back(RoutePt{x0, g_ground_y, z0});
    g_last_route.push_back(RoutePt{x1, g_ground_y, z1});
    return finish(x0, z0, x1, z1);
  };
  if (g_roads.empty()) {
    if (pe_ok) {
      restore_cache();
      return -1.f;
    }
    g_last_route.push_back(RoutePt{x0, g_ground_y, z0});
    g_last_route.push_back(RoutePt{x1, g_ground_y, z1});
    return finish(x0, z0, x1, z1);
  }

  // Same-segment shortcut.
  for (const RoadSeg& s : g_roads) {
    const float ax = s.x1 - s.x0;
    const float az = s.z1 - s.z0;
    const float len2 = ax * ax + az * az;
    if (len2 < 1e-8f) continue;
    const float t0 = ((x0 - s.x0) * ax + (z0 - s.z0) * az) / len2;
    const float t1 = ((x1 - s.x0) * ax + (z1 - s.z0) * az) / len2;
    if (t0 >= -0.05f && t0 <= 1.05f && t1 >= -0.05f && t1 <= 1.05f) {
      const float ct0 = t0 < 0.f ? 0.f : (t0 > 1.f ? 1.f : t0);
      const float ct1 = t1 < 0.f ? 0.f : (t1 > 1.f ? 1.f : t1);
      const float qx0 = s.x0 + ct0 * ax, qz0 = s.z0 + ct0 * az;
      const float qx1 = s.x0 + ct1 * ax, qz1 = s.z0 + ct1 * az;
      const float d0 = std::sqrt((x0 - qx0) * (x0 - qx0) + (z0 - qz0) * (z0 - qz0));
      const float d1 = std::sqrt((x1 - qx1) * (x1 - qx1) + (z1 - qz1) * (z1 - qz1));
      if (d0 < 5.f && d1 < 5.f) {
        const float yq0 = s.y0 + ct0 * (s.y1 - s.y0);
        const float yq1 = s.y0 + ct1 * (s.y1 - s.y0);
        g_last_route.push_back(RoutePt{x0, y0, z0});
        g_last_route.push_back(RoutePt{qx0, yq0, qz0});
        g_last_route.push_back(RoutePt{qx1, yq1, qz1});
        g_last_route.push_back(RoutePt{x1, y1, z1});
        return finish(x0, z0, x1, z1);
      }
    }
  }

  std::vector<RoadNode> nodes;
  std::vector<RoadEdge> edges;
  build_road_graph(&nodes, &edges);
  if (nodes.empty()) return fail();
  const int n0 = nearest_node(nodes, x0, z0);
  const int n1 = nearest_node(nodes, x1, z1);
  if (n0 < 0 || n1 < 0) return fail();
  const float stub0 = std::sqrt((nodes[static_cast<size_t>(n0)].x - x0) *
                                    (nodes[static_cast<size_t>(n0)].x - x0) +
                                (nodes[static_cast<size_t>(n0)].z - z0) *
                                    (nodes[static_cast<size_t>(n0)].z - z0));
  const float stub1 = std::sqrt((nodes[static_cast<size_t>(n1)].x - x1) *
                                    (nodes[static_cast<size_t>(n1)].x - x1) +
                                (nodes[static_cast<size_t>(n1)].z - z1) *
                                    (nodes[static_cast<size_t>(n1)].z - z1));
  std::vector<int> path_idx;
  const float path = dijkstra_len(nodes, edges, n0, n1, &path_idx);
  if (path >= 1e29f) return fail();
  g_last_route.push_back(RoutePt{x0, y0, z0});
  for (int idx : path_idx) {
    const RoadNode& n = nodes[static_cast<size_t>(idx)];
    g_last_route.push_back(RoutePt{n.x, n.y, n.z});
  }
  g_last_route.push_back(RoutePt{x1, y1, z1});
  (void)stub0;
  (void)stub1;
  (void)path;
  return finish(x0, z0, x1, z1);
}

int32_t physics_road_last_route_count() {
  std::lock_guard<std::mutex> lock(g_mu);
  return static_cast<int32_t>(g_last_route.size());
}

bool physics_road_last_route_point(int32_t i, float* x, float* y, float* z) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (i < 0 || static_cast<size_t>(i) >= g_last_route.size()) return false;
  const RoutePt& p = g_last_route[static_cast<size_t>(i)];
  if (x) *x = p.x;
  if (y) *y = p.y;
  if (z) *z = p.z;
  return true;
}

float physics_road_last_route_length() {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_last_route.size() < 2) return 0.f;
  return g_last_route_len;
}

static bool route_sample_arc_dist_locked(float target, float* x, float* y,
                                         float* z) {
  if (g_last_route.size() < 2) return false;
  float acc = 0.f;
  for (size_t i = 1; i < g_last_route.size(); ++i) {
    const RoutePt& a = g_last_route[i - 1];
    const RoutePt& b = g_last_route[i];
    const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    const float seg = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (acc + seg >= target || i + 1 == g_last_route.size()) {
      const float u = seg > 1e-6f ? (target - acc) / seg : 0.f;
      if (x) *x = a.x + u * dx;
      if (y) *y = a.y + u * dy;
      if (z) *z = a.z + u * dz;
      return true;
    }
    acc += seg;
  }
  const RoutePt& p = g_last_route.back();
  if (x) *x = p.x;
  if (y) *y = p.y;
  if (z) *z = p.z;
  return true;
}

bool physics_road_route_sample(float t, float* x, float* y, float* z) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_last_route.size() < 2 || g_last_route_len <= 0.f) return false;
  // PE getRoutePos/plotRoute: u=(1-t)*6408D8+t*6408DC; host polyline analogue.
  const float u = (1.f - t) * g_last_route_u0 + t * g_last_route_u1;
  return route_sample_arc_dist_locked(u, x, y, z);
}

float physics_road_route_param(float x, float y, float z) {
  (void)y;
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_last_route.size() < 2 || g_last_route_len <= 0.f) return 0.f;
  float best_d2 = 1e30f;
  float best_t = 0.f;
  float acc = 0.f;
  const size_t n = g_last_route.size();
  for (size_t i = 1; i < n; ++i) {
    const RoutePt& a = g_last_route[i - 1];
    const RoutePt& b = g_last_route[i];
    const float dx = b.x - a.x, dz = b.z - a.z;
    const float len2 = dx * dx + dz * dz;
    float u_raw = 0.f;
    if (len2 > 1e-8f) u_raw = ((x - a.x) * dx + (z - a.z) * dz) / len2;
    float u = u_raw;
    if (u < 0.f) u = 0.f;
    if (u > 1.f) u = 1.f;
    const float px = a.x + u * dx, pz = a.z + u * dz;
    const float d2 = (x - px) * (x - px) + (z - pz) * (z - pz);
    const float seg = std::sqrt(len2);
    if (d2 < best_d2) {
      best_d2 = d2;
      float u_t = u;
      if (i == 1 && u_raw < 0.f) u_t = u_raw;
      if (i + 1 == n && u_raw > 1.f) u_t = u_raw;
      const float span = g_last_route_u1 - g_last_route_u0;
      best_t = span > 1e-6f ? (acc + u_t * seg - g_last_route_u0) / span : 0.f;
    }
    acc += seg;
  }
  return best_t;
}

int32_t render_line_point_count(InvObject* self) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_lines.find(self);
  if (it == g_lines.end()) return 0;
  return static_cast<int32_t>(it->second.verts.size());
}

bool render_line_point_at(InvObject* self, int32_t i, float* x, float* y,
                          float* z) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_lines.find(self);
  if (it == g_lines.end() || i < 0 ||
      static_cast<size_t>(i) >= it->second.verts.size())
    return false;
  const LineVert& v = it->second.verts[static_cast<size_t>(i)];
  if (x) *x = v.x;
  if (y) *y = v.y;
  if (z) *z = v.z;
  return true;
}

int32_t render_line_color(InvObject* self) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_lines.find(self);
  if (it == g_lines.end()) return 0;
  return it->second.color;
}

namespace {

bool path_readable(const std::string& path) {
  if (path.empty()) return false;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fclose(f);
  return true;
}

void slash_norm(std::string* s) {
  for (char& c : *s) {
    if (c == '\\') c = '/';
  }
}

bool path_has_ci(const std::string& s, const char* needle) {
  if (!needle || !needle[0]) return true;
  const size_t n = std::strlen(needle);
  if (n > s.size()) return false;
  for (size_t i = 0; i + n <= s.size(); ++i) {
    bool ok = true;
    for (size_t j = 0; j < n; ++j) {
      char a = s[i + j];
      char b = needle[j];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
      if (a != b) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
  }
  return false;
}

bool path_ends_ci(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  if (n > s.size()) return false;
  for (size_t i = 0; i < n; ++i) {
    char a = s[s.size() - n + i];
    char b = suf[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

std::vector<std::string> parse_sourcefile_lines(const std::vector<uint8_t>& blob) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < blob.size()) {
    size_t line_end = i;
    while (line_end < blob.size() && blob[line_end] != '\n' &&
           blob[line_end] != '\r')
      ++line_end;
    std::string line(reinterpret_cast<const char*>(blob.data() + i),
                     line_end - i);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
      ++start;
    line = line.substr(start);
    if (line.size() >= 10 && std::strncmp(line.c_str(), "sourcefile", 10) == 0) {
      size_t p = 10;
      if (p < line.size() && line[p] == '=') ++p;
      while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
      if (p < line.size()) {
        std::string path = line.substr(p);
        slash_norm(&path);
        out.push_back(path);
      }
    }
    i = line_end;
    while (i < blob.size() && (blob[i] == '\n' || blob[i] == '\r')) ++i;
  }
  return out;
}

// Stock city.rpk points at maps/city/meshes/roadtestNN.scx (often absent);
// Invictus installs may ship visual/phys variants under objects/meshes.
std::string resolve_city_road_scx(const std::string& src_in) {
  std::string src = src_in;
  slash_norm(&src);
  if (src.empty() || !path_ends_ci(src, ".scx") || !path_has_ci(src, "road"))
    return {};
  if (path_has_ci(src, "rakpart")) return {};

  std::string resolved = rpak_resolve_path(src.c_str());
  if (path_readable(resolved)) return resolved;
  if (path_readable(src)) return src;

  // Basename stem: .../roadtest20.scx → roadtest20
  size_t slash = src.find_last_of('/');
  std::string leaf = (slash == std::string::npos) ? src : src.substr(slash + 1);
  if (leaf.size() < 5 || !path_ends_ci(leaf, ".scx")) return {};
  const std::string stem = leaf.substr(0, leaf.size() - 4);
  if (stem.empty()) return {};

  const std::string egyedi =
      "objects/meshes/" + stem + "_egyedi/" + stem + "_egyedi.scx";
  resolved = rpak_resolve_path(egyedi.c_str());
  if (path_readable(resolved)) return resolved;
  if (path_readable(egyedi)) return egyedi;
  return {};
}

// Phase 2.41 — broader visual remap (roads + area/hotel egyedi folders).
std::string resolve_city_visual_scx(const std::string& src_in) {
  std::string src = src_in;
  slash_norm(&src);
  if (src.empty() || !path_ends_ci(src, ".scx")) return {};
  if (path_has_ci(src, "rakpart") || path_has_ci(src, "phys_")) return {};

  std::string resolved = rpak_resolve_path(src.c_str());
  if (path_readable(resolved)) return resolved;
  if (path_readable(src)) return src;

  size_t slash = src.find_last_of('/');
  std::string leaf = (slash == std::string::npos) ? src : src.substr(slash + 1);
  if (leaf.size() < 5 || !path_ends_ci(leaf, ".scx")) return {};
  std::string stem = leaf.substr(0, leaf.size() - 4);
  if (stem.empty()) return {};

  auto try_path = [&](const std::string& rel) -> std::string {
    std::string r = rpak_resolve_path(rel.c_str());
    if (path_readable(r)) return r;
    if (path_readable(rel)) return rel;
    return {};
  };

  // objects/meshes/<stem>_egyedi/<stem>_egyedi.scx (roads, areas, hotels)
  std::string hit =
      try_path("objects/meshes/" + stem + "_egyedi/" + stem + "_egyedi.scx");
  if (!hit.empty()) return hit;
  hit = try_path("objects/meshes/" + stem + "_egyedi/" + stem + ".scx");
  if (!hit.empty()) return hit;
  hit = try_path("objects/meshes/" + stem + "/" + stem + ".scx");
  if (!hit.empty()) return hit;

  // Phase 2.45: Wall_10.scx → objects/meshes/walls/wall10/Wall_10.scx
  if (stem.size() > 5) {
    std::string low = stem;
    for (char& c : low) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (low.compare(0, 5, "wall_") == 0) {
      const std::string num = stem.substr(5);
      hit = try_path("objects/meshes/walls/wall" + num + "/" + stem + ".scx");
      if (!hit.empty()) return hit;
      hit = try_path("objects/meshes/walls/wall" + num + "/Wall_" + num +
                     ".scx");
      if (!hit.empty()) return hit;
    }
  }

  // hotel_010_epulet → hotel_010_egyedi
  const size_t us = stem.rfind('_');
  if (us != std::string::npos && us > 0) {
    const std::string base = stem.substr(0, us);
    hit = try_path("objects/meshes/" + base + "_egyedi/" + base +
                   "_egyedi.scx");
    if (!hit.empty()) return hit;
  }

  // Prefer road-specific resolve last (same egyedi pattern).
  return resolve_city_road_scx(src);
}

}  // namespace

namespace {
std::vector<InvObject*> g_city_visual_meshes;
int32_t g_city_instance_count = 0;
int32_t g_city_instance_drawn = 0;
}  // namespace

int32_t city_mesh_count() {
  return static_cast<int32_t>(g_city_visual_meshes.size());
}

int32_t city_mesh_vertex_total() {
  int32_t n = 0;
  for (InvObject* m : g_city_visual_meshes)
    n += render_d3d9_mesh_vertex_count(m);
  return n;
}

int32_t city_instance_count() { return g_city_instance_count; }

int32_t city_instance_drawn() { return g_city_instance_drawn; }

void city_mesh_clear() {
  for (InvObject* m : g_city_visual_meshes) {
    if (!m) continue;
    render_d3d9_mesh_destroy(m);
  }
  g_city_visual_meshes.clear();
  g_city_instance_count = 0;
  g_city_instance_drawn = 0;
}

int32_t city_mesh_seed_from_rpak(const char* pack_name, int32_t max_meshes) {
  if (!pack_name || !pack_name[0]) return 0;
  // Phase 2.58: default covers all city.rpk instance recipes (~142).
  if (max_meshes <= 0) max_meshes = 142;

  const RpakPack* pack = rpak_find_by_name(pack_name);
  if (!pack) {
    std::string try_path = pack_name;
    if (!path_has_ci(try_path, "/") && !path_has_ci(try_path, "\\"))
      try_path = std::string("maps/") + pack_name;
    rpak_open(try_path.c_str());
    pack = rpak_find_by_name(pack_name);
  }
  if (!pack || !pack->parsed_entries) return 0;

  std::unordered_map<std::string, bool> seen;
  std::unordered_map<uint32_t, bool> seen_mesh_local;
  std::unordered_map<uint32_t, bool> drawn_mesh_local;
  int32_t added = 0;

  auto try_add_mesh = [&](InvObject* mesh, const std::string& dedupe_key) -> bool {
    if (!mesh) return false;
    if (!dedupe_key.empty() && seen[dedupe_key]) {
      render_d3d9_mesh_destroy(mesh);
      return false;
    }
    if (!render_d3d9_mesh_ready(mesh) ||
        render_d3d9_mesh_vertex_count(mesh) < 3) {
      render_d3d9_mesh_destroy(mesh);
      return false;
    }
    render_d3d9_mesh_set_transform(mesh, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 1.f,
                                   1.f);
    render_d3d9_mesh_queue_add(mesh);
    g_city_visual_meshes.push_back(mesh);
    if (!dedupe_key.empty()) seen[dedupe_key] = true;
    ++added;
    return true;
  };

  auto mark_mesh_paths = [&](uint32_t mesh_local) {
    const int32_t mid =
        rpak_make_id(pack->pack_id, static_cast<uint16_t>(mesh_local & 0xFFFF));
    std::vector<uint8_t> mblob;
    if (!rpak_read_entry(mid, &mblob)) return;
    for (const std::string& src : parse_sourcefile_lines(mblob)) {
      const std::string path = resolve_city_visual_scx(src);
      if (!path.empty()) seen[path] = true;
    }
  };

  auto bind_recipe_tex = [&](InvObject* mesh, uint32_t tex_local) {
    if (!mesh || !tex_local) return;
    int32_t tid = 0;
    if (tex_local > 0xFFFFu) {
      const uint16_t local = static_cast<uint16_t>(tex_local & 0xFFFF);
      if (const RpakPack* texpack = rpak_find_by_name("textures"))
        tid = rpak_make_id(texpack->pack_id, local);
      if (!tid) tid = rpak_make_id(pack->pack_id, local);
    } else {
      tid = rpak_make_id(pack->pack_id,
                         static_cast<uint16_t>(tex_local & 0xFFFF));
    }
    if (!tid) return;
    InvObject* tex = resref_new();
    java_util_resource_ResourceRef_set(tex, tid);
    java_util_resource_ResourceRef_load(tex);
    if (render_d3d9_texture_ready(tex)) render_d3d9_mesh_set_texture(mesh, tex);
  };

  // Pass 1 — Phase 2.57: ground-map instance recipes draw first (budget).
  for (const RpakEntry& e : pack->entries) {
    if (e.is_dir || e.size == 0) continue;
    const int32_t res_id =
        rpak_make_id(pack->pack_id, static_cast<uint16_t>(e.type_id & 0xFFFF));
    std::vector<uint8_t> blob;
    if (!rpak_read_entry(res_id, &blob) || blob.size() < 8) continue;
    if (std::memcmp(blob.data(), "mesh ", 5) != 0) continue;
    uint32_t mesh_local = 0, tex_local = 0;
    if (!parse_mesh_recipe(blob, &mesh_local, &tex_local) || !mesh_local)
      continue;
    ++g_city_instance_count;
    if (seen_mesh_local[mesh_local]) {
      // Shared mesh already handled — count as drawn only if it was queued.
      if (drawn_mesh_local[mesh_local]) ++g_city_instance_drawn;
      continue;
    }
    seen_mesh_local[mesh_local] = true;
    if (added >= max_meshes) continue;

    InvObject* mesh = resref_new();
    const int32_t mid =
        rpak_make_id(pack->pack_id, static_cast<uint16_t>(mesh_local & 0xFFFF));
    if (!load_mesh_from_res_id(mesh, mid)) {
      render_d3d9_mesh_destroy(mesh);
      continue;
    }
    char key[32];
    std::snprintf(key, sizeof(key), "mesh:%u", mesh_local);
    if (!try_add_mesh(mesh, key)) continue;
    mark_mesh_paths(mesh_local);
    bind_recipe_tex(mesh, tex_local);
    drawn_mesh_local[mesh_local] = true;
    ++g_city_instance_drawn;
  }

  // Pass 2 — leftover sourcefile SCX not already covered by instance meshes.
  for (const RpakEntry& e : pack->entries) {
    if (added >= max_meshes) break;
    if (e.is_dir || e.size == 0) continue;
    const int32_t res_id =
        rpak_make_id(pack->pack_id, static_cast<uint16_t>(e.type_id & 0xFFFF));
    std::vector<uint8_t> blob;
    if (!rpak_read_entry(res_id, &blob) || blob.size() < 12) continue;
    if (std::memcmp(blob.data(), "sourcefile", 10) != 0) continue;
    for (const std::string& src : parse_sourcefile_lines(blob)) {
      if (added >= max_meshes) break;
      const std::string path = resolve_city_visual_scx(src);
      if (path.empty() || seen[path]) continue;
      FILE* f = std::fopen(path.c_str(), "rb");
      if (!f) {
        std::string resolved = rpak_resolve_path(path.c_str());
        if (!resolved.empty()) f = std::fopen(resolved.c_str(), "rb");
      }
      long sz = 0;
      if (f) {
        std::fseek(f, 0, SEEK_END);
        sz = std::ftell(f);
        std::fclose(f);
      }
      if (sz < 1024) continue;

      InvObject* mesh = resref_new();
      const char* load_path = path.c_str();
      std::string resolved = rpak_resolve_path(path.c_str());
      if (!resolved.empty()) load_path = resolved.c_str();
      if (!render_d3d9_mesh_create_from_file(mesh, load_path)) {
        render_d3d9_mesh_destroy(mesh);
        continue;
      }
      try_add_mesh(mesh, path);
    }
  }

  return added;
}

int32_t physics_road_seed_from_rpak(const char* pack_name, int32_t* out_meshes) {
  if (out_meshes) *out_meshes = 0;
  if (!pack_name || !pack_name[0]) return 0;

  const RpakPack* pack = rpak_find_by_name(pack_name);
  if (!pack) {
    // Accept basename or maps/<name>.
    std::string try_path = pack_name;
    if (!path_has_ci(try_path, "/") && !path_has_ci(try_path, "\\"))
      try_path = std::string("maps/") + pack_name;
    rpak_open(try_path.c_str());
    pack = rpak_find_by_name(pack_name);
  }
  if (!pack || !pack->parsed_entries) return 0;

  const int32_t before = physics_road_count();
  int32_t meshes = 0;
  for (const RpakEntry& e : pack->entries) {
    if (e.is_dir || e.size == 0) continue;
    const int32_t res_id =
        rpak_make_id(pack->pack_id, static_cast<uint16_t>(e.type_id & 0xFFFF));
    std::vector<uint8_t> blob;
    if (!rpak_read_entry(res_id, &blob) || blob.size() < 12) continue;
    if (std::memcmp(blob.data(), "sourcefile", 10) != 0) continue;
    for (const std::string& src : parse_sourcefile_lines(blob)) {
      const std::string path = resolve_city_road_scx(src);
      if (path.empty()) continue;
      const int32_t added = physics_road_add_from_scx(path.c_str());
      if (added > 0) ++meshes;
    }
  }
  if (out_meshes) *out_meshes = meshes;
  return physics_road_count() - before;
}

int32_t physics_road_seed_valocity() {
  // Club garage poses from Valocity.java + race seed near (0,500).
  // Connected spine so getNearestCross / getRouteLength work for City races.
  physics_road_clear();
  auto seg = [](float x0, float y0, float z0, float x1, float y1, float z1) {
    physics_road_add_segment(x0, y0, z0, x1, y1, z1);
  };
  // G0 club0 → mid → race hub
  seg(-278.518f, 9.8f, 1033.002f, -278.518f, 5.0f, 500.0f);
  seg(-278.518f, 5.0f, 500.0f, 0.0f, 2.0f, 500.0f);
  // Hub south + G2
  seg(0.0f, 2.0f, 500.0f, 0.0f, 2.0f, 0.0f);
  seg(0.0f, 2.0f, 0.0f, -531.138f, 5.05f, -149.357f);
  // Hub → G1 club1
  seg(0.0f, 2.0f, 500.0f, 355.381f, 1.6f, 418.244f);
  // East–west connector G0 mid → G1
  seg(-278.518f, 5.0f, 500.0f, 355.381f, 1.6f, 418.244f);
  // Phase 2.30: append centerlines from city.rpk road sourcefiles (egyedi remap).
  physics_road_seed_from_rpak("city.rpk", nullptr);
  return physics_road_count();
}

int32_t physics_road_add_from_scx(const char* path) {
  if (!path || !path[0]) return 0;
  std::string resolved = rpak_resolve_path(path);
  if (resolved.empty()) resolved = path;
  void* key = reinterpret_cast<void*>(
      static_cast<uintptr_t>(0x56A000u + static_cast<unsigned>(physics_road_count())));
  if (!render_d3d9_mesh_create_from_file(key, resolved.c_str()) ||
      !render_d3d9_mesh_ready(key)) {
    render_d3d9_mesh_destroy(key);
    return 0;
  }
  constexpr int32_t kMaxSamp = 4096;
  std::vector<float> xyz(static_cast<size_t>(kMaxSamp) * 3u);
  const int32_t n =
      render_d3d9_mesh_copy_positions(key, xyz.data(), kMaxSamp);
  render_d3d9_mesh_destroy(key);
  if (n < 2) return 0;

  // PCA on XZ → principal axis; endpoints at min/max projection.
  double mx = 0, mz = 0, my = 0;
  for (int32_t i = 0; i < n; ++i) {
    mx += xyz[static_cast<size_t>(i) * 3u + 0];
    my += xyz[static_cast<size_t>(i) * 3u + 1];
    mz += xyz[static_cast<size_t>(i) * 3u + 2];
  }
  mx /= n;
  my /= n;
  mz /= n;
  double cxx = 0, czz = 0, cxz = 0;
  for (int32_t i = 0; i < n; ++i) {
    const double dx = xyz[static_cast<size_t>(i) * 3u + 0] - mx;
    const double dz = xyz[static_cast<size_t>(i) * 3u + 2] - mz;
    cxx += dx * dx;
    czz += dz * dz;
    cxz += dx * dz;
  }
  // Largest eigenvector of [[cxx,cxz],[cxz,czz]].
  double ax = 1, az = 0;
  if (cxx + czz > 1e-8) {
    const double tr = cxx + czz;
    const double det = cxx * czz - cxz * cxz;
    const double disc_arg = tr * tr * 0.25 - det;
    const double disc = std::sqrt(disc_arg > 0.0 ? disc_arg : 0.0);
    const double l1 = tr * 0.5 + disc;
    ax = cxz;
    az = l1 - cxx;
    if (std::fabs(ax) + std::fabs(az) < 1e-10) {
      ax = l1 - czz;
      az = cxz;
    }
    const double len = std::sqrt(ax * ax + az * az);
    if (len > 1e-10) {
      ax /= len;
      az /= len;
    } else {
      ax = 1;
      az = 0;
    }
  }
  double tmin = 1e30, tmax = -1e30;
  for (int32_t i = 0; i < n; ++i) {
    const double dx = xyz[static_cast<size_t>(i) * 3u + 0] - mx;
    const double dz = xyz[static_cast<size_t>(i) * 3u + 2] - mz;
    const double t = dx * ax + dz * az;
    if (t < tmin) tmin = t;
    if (t > tmax) tmax = t;
  }
  if (tmax - tmin < 0.5) {
    // Degenerate PCA — AABB major axis from samples.
    float xmin = xyz[0], xmax = xyz[0], zmin = xyz[2], zmax = xyz[2];
    for (int32_t i = 1; i < n; ++i) {
      const float x = xyz[static_cast<size_t>(i) * 3u + 0];
      const float z = xyz[static_cast<size_t>(i) * 3u + 2];
      if (x < xmin) xmin = x;
      if (x > xmax) xmax = x;
      if (z < zmin) zmin = z;
      if (z > zmax) zmax = z;
    }
    const float y = static_cast<float>(my);
    const int32_t before = physics_road_count();
    if ((xmax - xmin) >= (zmax - zmin))
      physics_road_add_segment(xmin, y, 0.5f * (zmin + zmax), xmax, y,
                               0.5f * (zmin + zmax));
    else
      physics_road_add_segment(0.5f * (xmin + xmax), y, zmin,
                               0.5f * (xmin + xmax), y, zmax);
    return physics_road_count() - before;
  }

  const float y = static_cast<float>(my);
  const float x0 = static_cast<float>(mx + ax * tmin);
  const float z0 = static_cast<float>(mz + az * tmin);
  const float x1 = static_cast<float>(mx + ax * tmax);
  const float z1 = static_cast<float>(mz + az * tmax);
  // Split long centerlines into a few segments for denser junctions.
  const float len = std::sqrt((x1 - x0) * (x1 - x0) + (z1 - z0) * (z1 - z0));
  const int parts = len > 40.f ? 3 : (len > 15.f ? 2 : 1);
  const int32_t before = physics_road_count();
  for (int p = 0; p < parts; ++p) {
    const float t0 = static_cast<float>(p) / static_cast<float>(parts);
    const float t1 = static_cast<float>(p + 1) / static_cast<float>(parts);
    physics_road_add_segment(x0 + (x1 - x0) * t0, y, z0 + (z1 - z0) * t0,
                             x0 + (x1 - x0) * t1, y, z0 + (z1 - z0) * t1);
  }
  return physics_road_count() - before;
}

float contact_half_height(const ResState& r) {
  if (r.shape == 1) return r.hy > 0.f ? r.hy : 0.5f;  // box
  if (r.shape == 2) return r.hx > 0.f ? r.hx : 0.5f;  // sphere radius
  return 0.5f;
}

void body_half_extents(const ResState& r, float* hx, float* hy, float* hz) {
  if (r.shape == 2) {
    const float rad = r.hx > 0.f ? r.hx : 0.5f;
    *hx = *hy = *hz = rad;
  } else {
    *hx = r.hx > 0.f ? r.hx : 0.5f;
    *hy = r.hy > 0.f ? r.hy : 0.5f;
    *hz = r.hz > 0.f ? r.hz : 0.5f;
  }
}

void physics_set_collide_active(InvObject* self, int32_t on) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  R(self).collide = on ? 1 : 0;
}

int32_t physics_collide_active(InvObject* self) {
  if (!self) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_res.find(self);
  return it == g_res.end() ? 0 : it->second.collide;
}

int32_t physics_collide_events() { return g_collide_events; }

// Phase 2.31 — full OBB from mesh YPR (Ry*Rx*Rz), same basis as render_d3d9.
struct Obb3 {
  float cx = 0, cy = 0, cz = 0;
  float ax[3] = {1, 0, 0};  // local +X (right)
  float ay[3] = {0, 1, 0};  // local +Y (up)
  float az[3] = {0, 0, 1};  // local +Z (forward)
  float hx = 0.5f, hy = 0.5f, hz = 0.5f;
};

void ypr_basis(float yaw, float pitch, float roll, float* right, float* up,
               float* fwd) {
  const float cy = std::cos(yaw), sy = std::sin(yaw);
  const float cp = std::cos(pitch), sp = std::sin(pitch);
  const float cr = std::cos(roll), sr = std::sin(roll);
  // R = Ry(yaw) * Rx(pitch) * Rz(roll) — rows = local axes in world.
  right[0] = cy * cr + sy * sp * sr;
  right[1] = cp * sr;
  right[2] = -sy * cr + cy * sp * sr;
  up[0] = -cy * sr + sy * sp * cr;
  up[1] = cp * cr;
  up[2] = sy * sr + cy * sp * cr;
  fwd[0] = sy * cp;
  fwd[1] = -sp;
  fwd[2] = cy * cp;
}

void make_obb3(const ResState& r, float hx, float hy, float hz, Obb3* o) {
  o->cx = r.px;
  o->cy = r.py;
  o->cz = r.pz;
  o->hx = hx;
  o->hy = hy;
  o->hz = hz;
  if (r.shape == 2) {
    // Sphere → isotropic box (axis-aligned).
    o->ax[0] = 1.f;
    o->ax[1] = 0.f;
    o->ax[2] = 0.f;
    o->ay[0] = 0.f;
    o->ay[1] = 1.f;
    o->ay[2] = 0.f;
    o->az[0] = 0.f;
    o->az[1] = 0.f;
    o->az[2] = 1.f;
    return;
  }
  ypr_basis(r.oy, r.op, r.or_, o->ax, o->ay, o->az);
}

float obb_radius_on_axis3(const Obb3& o, float nx, float ny, float nz) {
  return o.hx * std::fabs(o.ax[0] * nx + o.ax[1] * ny + o.ax[2] * nz) +
         o.hy * std::fabs(o.ay[0] * nx + o.ay[1] * ny + o.ay[2] * nz) +
         o.hz * std::fabs(o.az[0] * nx + o.az[1] * ny + o.az[2] * nz);
}

// 3D SAT; MTV (mx,my,mz) pushes B out of A.
bool sat_obb3(const Obb3& a, const Obb3& b, float* mx, float* my, float* mz) {
  const float dx = b.cx - a.cx;
  const float dy = b.cy - a.cy;
  const float dz = b.cz - a.cz;
  float axes[15][3];
  int naxes = 0;
  auto push_axis = [&](float x, float y, float z) {
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len < 1e-6f) return;
    axes[naxes][0] = x / len;
    axes[naxes][1] = y / len;
    axes[naxes][2] = z / len;
    ++naxes;
  };
  push_axis(a.ax[0], a.ax[1], a.ax[2]);
  push_axis(a.ay[0], a.ay[1], a.ay[2]);
  push_axis(a.az[0], a.az[1], a.az[2]);
  push_axis(b.ax[0], b.ax[1], b.ax[2]);
  push_axis(b.ay[0], b.ay[1], b.ay[2]);
  push_axis(b.az[0], b.az[1], b.az[2]);
  const float* aa[3] = {a.ax, a.ay, a.az};
  const float* ba[3] = {b.ax, b.ay, b.az};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      push_axis(aa[i][1] * ba[j][2] - aa[i][2] * ba[j][1],
                aa[i][2] * ba[j][0] - aa[i][0] * ba[j][2],
                aa[i][0] * ba[j][1] - aa[i][1] * ba[j][0]);
    }
  }

  float best_pen = 1e30f;
  float best_align = -1.f;
  float best_nx = 1.f, best_ny = 0.f, best_nz = 0.f;
  for (int i = 0; i < naxes; ++i) {
    const float nx = axes[i][0];
    const float ny = axes[i][1];
    const float nz = axes[i][2];
    const float dist = dx * nx + dy * ny + dz * nz;
    const float ra = obb_radius_on_axis3(a, nx, ny, nz);
    const float rb = obb_radius_on_axis3(b, nx, ny, nz);
    const float pen = ra + rb - std::fabs(dist);
    if (pen <= 0.f) return false;
    const float align = std::fabs(dist);
    // Tie-break: prefer axis with larger center separation so face-face
    // contacts (dist≈0 on a side axis) don't steal MTV from the real stack.
    if (pen < best_pen - 1e-5f ||
        (pen <= best_pen + 1e-5f && align > best_align)) {
      best_pen = pen;
      best_align = align;
      const float s = (dist < 0.f) ? -1.f : 1.f;
      best_nx = nx * s;
      best_ny = ny * s;
      best_nz = nz * s;
    }
  }
  if (mx) *mx = best_nx * best_pen;
  if (my) *my = best_ny * best_pen;
  if (mz) *mz = best_nz * best_pen;
  return true;
}

void resolve_pair(ResState& a, ResState& b) {
  float ahx, ahy, ahz, bhx, bhy, bhz;
  body_half_extents(a, &ahx, &ahy, &ahz);
  body_half_extents(b, &bhx, &bhy, &bhz);

  Obb3 oa, ob;
  make_obb3(a, ahx, ahy, ahz, &oa);
  make_obb3(b, bhx, bhy, bhz, &ob);
  float mx = 0.f, my = 0.f, mz = 0.f;
  if (!sat_obb3(oa, ob, &mx, &my, &mz)) return;

  // Coincident centers: push against inbound relative velocity.
  if (std::fabs(a.px - b.px) < 1e-4f && std::fabs(a.py - b.py) < 1e-4f &&
      std::fabs(a.pz - b.pz) < 1e-4f) {
    const float rvx = b.vx - a.vx;
    const float rvy = b.vy - a.vy;
    const float rvz = b.vz - a.vz;
    const float rvl = std::sqrt(rvx * rvx + rvy * rvy + rvz * rvz);
    const float pen = std::sqrt(mx * mx + my * my + mz * mz);
    if (rvl > 1e-4f) {
      mx = (rvx / rvl) * pen;
      my = (rvy / rvl) * pen;
      mz = (rvz / rvl) * pen;
    } else {
      mx = pen;
      my = mz = 0.f;
    }
  }

  const bool a_move = !a.is_static && !a.asleep;
  const bool b_move = !b.is_static && !b.asleep;
  if (!a_move && !b_move) return;

  float wa = a_move ? 1.f : 0.f;
  float wb = b_move ? 1.f : 0.f;
  const float wsum = wa + wb;
  if (wsum < 1e-6f) return;
  wa /= wsum;
  wb /= wsum;

  a.px -= mx * wa;
  a.py -= my * wa;
  a.pz -= mz * wa;
  b.px += mx * wb;
  b.py += my * wb;
  b.pz += mz * wb;

  float nx = mx, ny = my, nz = mz;
  const float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
  if (nlen > 1e-6f) {
    nx /= nlen;
    ny /= nlen;
    nz /= nlen;
  } else {
    nx = 1.f;
    ny = nz = 0.f;
  }

  const float rv = (b.vx - a.vx) * nx + (b.vy - a.vy) * ny + (b.vz - a.vz) * nz;
  if (rv < 0.f) {
    const float j = -(1.f + kCollideRestitution) * rv;
    a.vx -= j * nx * wa;
    a.vy -= j * ny * wa;
    a.vz -= j * nz * wa;
    b.vx += j * nx * wb;
    b.vy += j * ny * wb;
    b.vz += j * nz * wb;
  }
  ++g_collide_events;
}

void physics_resolve_collisions() {
  std::vector<InvObject*> keys;
  keys.reserve(g_res.size());
  for (auto& kv : g_res) {
    if (kv.second.shape != 0 && kv.second.collide) keys.push_back(kv.first);
  }
  for (size_t i = 0; i < keys.size(); ++i) {
    for (size_t j = i + 1; j < keys.size(); ++j) {
      resolve_pair(g_res[keys[i]], g_res[keys[j]]);
    }
  }
}

void physics_integrate(float dt) {
  if (dt <= 0.f) return;
  g_collide_events = 0;
  // Substep to reduce tunneling through thin statics.
  constexpr int kSub = 4;
  const float sdt = dt / static_cast<float>(kSub);
  struct Step {
    InvObject* key;
    float px, py, pz, oy, op, or_, sx, sy, sz;
  };
  std::vector<Step> steps;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (int sub = 0; sub < kSub; ++sub) {
      // Resolve at current pose first so stacked YPR contacts (equal side/face
      // pens) separate on the approach axis before gravity deepens Y overlap.
      physics_resolve_collisions();
      for (auto& kv : g_res) {
        auto& r = kv.second;
        if (r.shape == 0) continue;
        if (r.is_static || r.asleep) continue;
        r.vy -= kGravity * sdt;
        r.px += r.vx * sdt;
        r.py += r.vy * sdt;
        r.pz += r.vz * sdt;
        r.oy += r.wy * sdt;
        r.op += r.wx * sdt;
        r.or_ += r.wz * sdt;
        const float half = contact_half_height(r);
        const float min_y = g_ground_y + half;
        if (r.py < min_y) {
          r.py = min_y;
          if (r.vy < 0.f) r.vy = 0.f;
        }
      }
      physics_resolve_collisions();
    }
    for (auto& kv : g_res) {
      auto& r = kv.second;
      if (r.shape == 0) continue;
      if (r.is_static || r.asleep) continue;
      steps.push_back(Step{kv.first, r.px, r.py, r.pz, r.oy, r.op, r.or_, r.sx,
                           r.sy, r.sz});
    }
  }
  for (const Step& s : steps)
    render_d3d9_mesh_set_transform(s.key, s.px, s.py, s.pz, s.oy, s.op, s.or_,
                                   s.sx, s.sy, s.sz);
}

void physics_set_wheel_params(InvObject* self, float steer, float drive,
                              float radius) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.has_wheel_params = true;
  r.wheel_steer = steer;
  r.wheel_drive = drive;
  r.wheel_radius = (radius > 0.05f) ? radius : 0.32f;
}

void physics_set_wheel_contact(InvObject* self, float friction, float sliction,
                               float brake, float hbrake, float roll_res) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.has_wheel_params = true;
  r.wheel_friction = friction;
  r.wheel_sliction = sliction;
  r.wheel_brake = brake;
  r.wheel_hbrake = hbrake;
  r.wheel_roll_res = roll_res;
}

void physics_set_wheel_pacejka(InvObject* self, float b, float c, float d) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.has_wheel_params = true;
  r.wheel_pk_b = b;
  r.wheel_pk_c = c;
  r.wheel_pk_d = d;
}

void physics_set_wheel_suspension(InvObject* self, float spring, float damp,
                                  float rest_len, float arm_len) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.has_wheel_params = true;
  r.wheel_spring = spring;
  r.wheel_damp = damp;
  r.wheel_rest_len = (rest_len > 0.01f) ? rest_len : 0.39f;
  r.wheel_arm_len = (arm_len > 0.05f) ? arm_len : 0.244f;
}

void physics_set_drive_torque(InvObject* self, float nm) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  if (nm < 0.f) nm = 0.f;
  r.drive_torque_nm = nm;
}

float physics_get_engine_rpm(InvObject* self) {
  if (!self) return 0.f;
  std::lock_guard<std::mutex> lock(g_mu);
  return R(self).engine_rpm;
}

void physics_drive(InvObject* self, InvObject* controller, float dt) {
  if (!self || dt <= 0.f) return;
  float throttle =
      controller ? input_map_get_logical(controller, kAxisThrottle) : 0.f;
  float brake =
      controller ? input_map_get_logical(controller, kAxisBrake) : 0.f;
  float steer =
      controller ? input_map_get_logical(controller, kAxisTurnLR) : 0.f;
  float handbrake =
      controller ? input_map_get_logical(controller, kAxisHandbrake) : 0.f;
  const float nitro =
      controller ? input_map_get_logical(controller, kAxisNitro) : 0.f;
  const float clutch =
      controller ? input_map_get_logical(controller, kAxisClutch) : 0.f;
  const float gear_axis =
      controller ? input_map_get_logical(controller, kAxisGearUpDown) : 0.f;

  // Phase 2.93: sample support before lock (road_project also locks).
  float sample_px = 0, sample_py = 0, sample_pz = 0, sample_half = 0.5f;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& r = R(self);
    if (r.shape == 0 || r.is_static || r.asleep) return;
    sample_px = r.px;
    sample_py = r.py;
    sample_pz = r.pz;
    sample_half = contact_half_height(r);
  }
  float sox = 0, soy = 0, soz = 0, srdx = 0, srdy = 0, srdz = 0;
  const bool sample_on_road = physics_road_project(
      sample_px, sample_pz, &sox, &soy, &soz, &srdx, &srdy, &srdz);
  const float support_y =
      sample_on_road ? (soy + sample_half) : (g_ground_y + sample_half);
  const bool airborne_now = sample_py > support_y + kAirborneClearance;

  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& r = R(self);
    if (r.shape == 0 || r.is_static || r.asleep) return;
    r.airborne = airborne_now ? 1 : 0;

    float drive_mul = 1.f;
    float radius_mul = 1.f;
    float grip_mul = 1.f;
    float roll_extra = 0.f;
    float pk_b = 15.2f, pk_c = 1.49f, pk_d = 1.4f;
    bool use_pacejka = false;
    float arm_steer_mul = 1.f;
    if (r.has_wheel_params) {
      steer = r.wheel_steer;
      drive_mul = r.wheel_drive;
      if (drive_mul < 0.f) drive_mul = 0.f;
      if (drive_mul > 2.f) drive_mul = 2.f;
      radius_mul = r.wheel_radius / 0.32f;
      if (radius_mul < 0.25f) radius_mul = 0.25f;
      if (radius_mul > 2.5f) radius_mul = 2.5f;
      grip_mul = r.wheel_friction * r.wheel_sliction;
      if (grip_mul < 0.05f) grip_mul = 0.05f;
      if (grip_mul > 3.f) grip_mul = 3.f;
      if (r.wheel_brake > brake) brake = r.wheel_brake;
      if (brake > 1.f) brake = 1.f;
      if (r.wheel_hbrake > handbrake) handbrake = r.wheel_hbrake;
      if (handbrake > 1.f) handbrake = 1.f;
      roll_extra = r.wheel_roll_res * 800.f;
      if (roll_extra < 0.f) roll_extra = 0.f;
      if (roll_extra > 8.f) roll_extra = 8.f;
      pk_b = r.wheel_pk_b;
      pk_c = r.wheel_pk_c;
      pk_d = r.wheel_pk_d;
      use_pacejka = true;
      if (r.wheel_arm_len > 0.05f) {
        arm_steer_mul = 0.244f / r.wheel_arm_len;
        if (arm_steer_mul < 0.5f) arm_steer_mul = 0.5f;
        if (arm_steer_mul > 2.f) arm_steer_mul = 2.f;
      }
    }
    if (airborne_now) {
      grip_mul *= 0.08f;
      steer *= 0.08f;
      arm_steer_mul *= 0.2f;
    }
    // Phase 2.103: ASR cuts excess throttle; ABS boosts grip under brake.
    {
      float asr_v = tree_field_get_float(self, "asr");
      float abs_v = tree_field_get_float(self, "abs");
      if (controller) {
        const float a1 = tree_field_get_float(controller, "asr");
        const float a2 = tree_field_get_float(controller, "abs");
        if (a1 > asr_v) asr_v = a1;
        if (a2 > abs_v) abs_v = a2;
      }
      if (asr_v > 0.f && throttle > 0.35f) {
        float cut = asr_v * 0.4f;
        if (cut > 0.65f) cut = 0.65f;
        throttle *= 1.f - cut * (throttle - 0.35f);
        if (throttle < 0.f) throttle = 0.f;
      }
      if (abs_v > 0.f && brake > 0.2f) {
        grip_mul *= 1.f + abs_v * 0.45f;
        if (grip_mul > 4.f) grip_mul = 4.f;
      }
    }

    // Gear shift on edge of AXIS_GEAR_UPDOWN.
    if (gear_axis > 0.5f && r.gear_axis_prev <= 0.5f) {
      if (r.gear < 5) ++r.gear;
    } else if (gear_axis < -0.5f && r.gear_axis_prev >= -0.5f) {
      if (r.gear > -1) --r.gear;
    }
    r.gear_axis_prev = gear_axis;

    const float yaw = r.oy;
    const float fx = std::sin(yaw);
    const float fz = std::cos(yaw);
    const float dx = (r.gear < 0) ? -fx : fx;
    const float dz = (r.gear < 0) ? -fz : fz;
    float spd = std::sqrt(r.vx * r.vx + r.vz * r.vz);

    float engage = 1.f - clutch;
    if (engage < 0.f) engage = 0.f;
    if (engage > 1.f) engage = 1.f;
    if (r.gear == 0) engage = 0.f;

    auto gear_accel = [](int32_t g) -> float {
      switch (g) {
        case -1:
          return 0.55f;
        case 1:
          return 1.00f;
        case 2:
          return 0.90f;
        case 3:
          return 0.75f;
        case 4:
          return 0.60f;
        case 5:
          return 0.48f;
        default:
          return 0.f;
      }
    };
    auto gear_vmax = [](int32_t g) -> float {
      switch (g) {
        case -1:
          return 0.40f;
        case 1:
          return 0.55f;
        case 2:
          return 0.70f;
        case 3:
          return 0.85f;
        case 4:
          return 1.00f;
        case 5:
          return 1.18f;
        default:
          return 0.f;
      }
    };

    const bool have_tq = r.drive_torque_nm > 1.f;
    float torque_scale = 1.f;
    if (have_tq) {
      torque_scale = r.drive_torque_nm / kRefDriveTorqueNm;
      if (torque_scale < 0.2f) torque_scale = 0.2f;
      if (torque_scale > 2.5f) torque_scale = 2.5f;
    }

    const float accel =
        kDriveAccel * gear_accel(r.gear) *
        (1.f + ((!have_tq && nitro > 0.01f) ? kNitroBoost * nitro : 0.f)) *
        engage * drive_mul * radius_mul * torque_scale;
    if (throttle > 0.01f && accel > 0.01f) {
      r.vx += dx * accel * throttle * dt;
      r.vz += dz * accel * throttle * dt;
    }
    // Phase 2.92: engine braking — engaged gear, throttle released (needs
    // a controller so free-coast test bodies without maps keep sliding).
    if (controller && throttle < 0.05f && brake < 0.01f && handbrake < 0.01f &&
        engage > 0.05f && r.gear != 0 && spd > 0.8f) {
      const float nx = r.vx / spd;
      const float nz = r.vz / spd;
      float eb = kEngineBrake * gear_accel(r.gear) * engage;
      if (have_tq) eb *= (0.65f + 0.35f * torque_scale);
      if (eb < 0.f) eb = 0.f;
      r.vx -= nx * eb * dt;
      r.vz -= nz * eb * dt;
      spd = std::sqrt(r.vx * r.vx + r.vz * r.vz);
    }
    if (brake > 0.01f) {
      if (spd > 0.5f) {
        const float dec = kBrakeDecel * brake * dt;
        const float nx = r.vx / spd, nz = r.vz / spd;
        r.vx -= nx * dec;
        r.vz -= nz * dec;
      } else if (r.gear >= 0) {
        r.vx -= fx * kDriveAccel * 0.35f * brake * dt;
        r.vz -= fz * kDriveAccel * 0.35f * brake * dt;
      }
    }
    if (handbrake > 0.01f && spd > 0.2f) {
      const float dec = kHandbrakeDecel * handbrake * dt;
      const float nx = r.vx / spd, nz = r.vz / spd;
      r.vx -= nx * dec;
      r.vz -= nz * dec;
    }
    const float drag = kDrag + roll_extra;
    r.vx *= (1.f - drag * dt);
    r.vz *= (1.f - drag * dt);
    {
      const float fwd_spd = r.vx * fx + r.vz * fz;
      float lat_x = r.vx - fx * fwd_spd;
      float lat_z = r.vz - fz * fwd_spd;
      float pace_mul = 1.f;
      if (use_pacejka) {
        // Simplified Magic Formula: Fy ~ D*sin(C*atan(B*alpha)).
        const float lat_spd = std::sqrt(lat_x * lat_x + lat_z * lat_z);
        const float alpha =
            std::atan2(lat_spd, std::fabs(fwd_spd) + 0.5f);
        const float mf =
            pk_d * std::sin(pk_c * std::atan(pk_b * alpha));
        constexpr float kStock = 1.4f * 1.49f * 15.2f;
        float stiff = (pk_d * pk_c * pk_b) / kStock;
        if (stiff < 0.05f) stiff = 0.05f;
        if (stiff > 4.f) stiff = 4.f;
        float shape = 1.f;
        if (std::fabs(pk_d) > 0.01f) {
          shape = std::fabs(mf) / std::fabs(pk_d);
          if (shape < 0.15f) shape = 0.15f;
          if (shape > 1.f) shape = 1.f;
        }
        pace_mul = stiff * (0.5f + 0.5f * shape);
      }
      const float grip =
          ((handbrake > 0.01f) ? kHandbrakeGrip : kLateralGrip) * grip_mul *
          pace_mul;
      const float damp = 1.f / (1.f + grip * dt);
      lat_x *= damp;
      lat_z *= damp;
      r.vx = fx * fwd_spd + lat_x;
      r.vz = fz * fwd_spd + lat_z;
    }
    spd = std::sqrt(r.vx * r.vx + r.vz * r.vz);
    const float max_spd =
        kMaxSpeed * gear_vmax(r.gear) *
        (1.f + (nitro > 0.01f ? 0.25f * nitro : 0.f)) * radius_mul;
    if (max_spd > 0.5f && spd > max_spd) {
      r.vx *= max_spd / spd;
      r.vz *= max_spd / spd;
      spd = max_spd;
    }
    const float steer_scale = spd / (spd + 5.f);
    r.wy = -steer * kSteerRate * steer_scale * arm_steer_mul;
    if (airborne_now) r.wy *= 0.15f;

    // Phase 2.81: rough engine RPM from road speed × gear (for getTorque).
    float gear_factor = 1.f;
    switch (r.gear) {
      case -1:
        gear_factor = 1.4f;
        break;
      case 0:
        gear_factor = 0.f;
        break;
      case 1:
        gear_factor = 1.6f;
        break;
      case 2:
        gear_factor = 1.25f;
        break;
      case 3:
        gear_factor = 1.0f;
        break;
      case 4:
        gear_factor = 0.82f;
        break;
      case 5:
        gear_factor = 0.68f;
        break;
      default:
        break;
    }
    float rpm = 900.f;
    if (r.gear != 0) rpm = 800.f + spd * 90.f * gear_factor;
    if (rpm < 800.f) rpm = 800.f;
    if (rpm > 9000.f) rpm = 9000.f;
    r.engine_rpm = rpm;
  }

  physics_integrate(dt);
  {
    float px = 0, pz = 0, half = 0.5f;
    float ride_bias = 0.f;
    float spring = 0.f, damp = 0.f;
    bool use_susp = false;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      auto& r = R(self);
      px = r.px;
      pz = r.pz;
      half = contact_half_height(r);
      if (r.has_wheel_params) {
        ride_bias = (r.wheel_rest_len - 0.39f) * 0.5f;
        if (ride_bias < -0.15f) ride_bias = -0.15f;
        if (ride_bias > 0.25f) ride_bias = 0.25f;
        spring = r.wheel_spring;
        damp = r.wheel_damp;
        use_susp = spring > 1.f || damp > 1.f || std::fabs(ride_bias) > 0.001f;
      }
    }
    float ox = 0, oy = 0, oz = 0, rdx = 0, rdy = 0, rdz = 0;
    const bool on_road =
        physics_road_project(px, pz, &ox, &oy, &oz, &rdx, &rdy, &rdz);
    {
      std::lock_guard<std::mutex> lock(g_mu);
      auto& r = R(self);
      if (on_road) {
        r.py = oy + half + ride_bias;
        if (r.vy < 0.f) r.vy = 0.f;
        // Phase 2.93: gentle yaw toward road tangent when grounded + low steer.
        if (controller && !r.airborne) {
          float th =
              input_map_get_logical(controller, kAxisThrottle);
          float st =
              r.has_wheel_params
                  ? r.wheel_steer
                  : input_map_get_logical(controller, kAxisTurnLR);
          const float spd_xz = std::sqrt(r.vx * r.vx + r.vz * r.vz);
          if (th < 0.35f && std::fabs(st) < 0.25f && spd_xz > 1.5f) {
            float tlen = std::sqrt(rdx * rdx + rdz * rdz);
            if (tlen > 1e-4f) {
              rdx /= tlen;
              rdz /= tlen;
              float want = std::atan2(rdx, rdz);
              // Prefer orientation matching velocity when reversing.
              const float fwd = r.vx * std::sin(r.oy) + r.vz * std::cos(r.oy);
              if (fwd < -0.5f) want += 3.14159265f;
              float dyaw = want - r.oy;
              while (dyaw > 3.14159265f) dyaw -= 6.2831853f;
              while (dyaw < -3.14159265f) dyaw += 6.2831853f;
              if (std::fabs(dyaw) < 0.85f) {
                // Phase 2.102: Vehicle.steerhelp scales road yaw assist.
                float help = tree_field_get_float(self, "steerhelp");
                if (controller) {
                  const float ch = tree_field_get_float(controller, "steerhelp");
                  if (ch > help) help = ch;
                }
                if (help < 0.f) help = 0.f;
                if (help > 2.f) help = 2.f;
                float w = kRoadYawAssist * (1.f + help) * dt *
                          (1.f - std::fabs(st));
                if (w > 1.f) w = 1.f;
                r.oy += dyaw * w;
              }
            }
          }
        }
        // Phase 2.95: blend body pitch to road slope (rdy = ΔY / ΔXZ).
        if (!r.airborne) {
          float want_p = std::atan(rdy);
          const float along = r.vx * rdx + r.vz * rdz;
          if (along < -0.5f) want_p = -want_p;
          float dp = want_p - r.op;
          float wp = kRoadPitchAssist * dt;
          if (wp > 1.f) wp = 1.f;
          r.op += dp * wp;
        }
      } else if (use_susp) {
        const float min_y = g_ground_y + half;
        float supported_y = min_y + ride_bias;
        if (supported_y < min_y) supported_y = min_y;
        // Near ground: sit at spring rest ride height (arcade, no fight vs g).
        if (r.py <= supported_y + 0.08f) {
          r.py = supported_y;
          if (r.vy < 0.f) r.vy = 0.f;
        } else {
          const float err = r.py - supported_y;
          float k = (spring / 20000.f) * 50.f;
          if (k < 0.f) k = 0.f;
          if (k > 120.f) k = 120.f;
          r.vy -= k * err * dt;
        }
        float dn = (damp / 2000.f) * 10.f;
        if (dn > 40.f) dn = 40.f;
        r.vy *= 1.f / (1.f + dn * dt);
        if (r.py < min_y) {
          r.py = min_y;
          if (r.vy < 0.f) r.vy = 0.f;
        }
      }
    }
  }
}

int32_t physics_get_gear(InvObject* self) {
  if (!self) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_res.find(self);
  return it == g_res.end() ? 0 : it->second.gear;
}

int32_t physics_is_airborne(InvObject* self) {
  if (!self) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_res.find(self);
  return it == g_res.end() ? 0 : it->second.airborne;
}

void physics_set_gear(InvObject* self, int32_t gear) {
  if (!self) return;
  if (gear < -1) gear = -1;
  if (gear > 5) gear = 5;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.gear = gear;
  r.gear_axis_prev = 0.f;
}

float physics_speed_square(InvObject* self) {
  if (!self) return 0.f;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_res.find(self);
  if (it == g_res.end()) return 0.f;
  const auto& r = it->second;
  return r.vx * r.vx + r.vy * r.vy + r.vz * r.vz;
}

int32_t physics_shape(InvObject* self) {
  if (!self) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_res.find(self);
  return it == g_res.end() ? 0 : it->second.shape;
}

void physics_extents(InvObject* self, float* a, float* b, float* c) {
  float hx = 0, hy = 0, hz = 0;
  if (self) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_res.find(self);
    if (it != g_res.end()) {
      hx = it->second.hx;
      hy = it->second.hy;
      hz = it->second.hz;
    }
  }
  if (a) *a = hx;
  if (b) *b = hy;
  if (c) *c = hz;
}

bool physics_pick_osd_gadget(float ndc_x, float ndc_y, InvObject** out_phy,
                             InvObject** out_group, float* out_px, float* out_py,
                             float* out_pz) {
  // Java Osd.createButton: phy.createBox(g, rWidth, rHeight, 0.001) then
  // setMatrix(convertTextCoordinates). Group.activate setEventMask(EVENT_CURSOR)
  // — physics instances send to parent (Group.java). Thin Z = OSD vs car boxes.
  struct Cand {
    InvObject* phy = nullptr;
    InvObject* parent = nullptr;
    float px = 0, py = 0, pz = 0, hx = 0, hy = 0, hz = 0;
  };
  std::vector<Cand> cands;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& kv : g_res) {
      const ResState& r = kv.second;
      if (r.shape != 1) continue;
      // half of 0.001 = 0.0005; car boxes are orders of magnitude larger.
      if (r.hz <= 0.f || r.hz >= 0.01f) continue;
      if (r.hx <= 0.f || r.hy <= 0.f) continue;
      // PE collides posed primitives; TREE createBox without setMatrix stays
      // at origin and would false-hit every NDC near 0.
      if (!r.pose_set) continue;
      if (!kv.first || !r.parent) continue;
      Cand c;
      c.phy = kv.first;
      c.parent = r.parent;
      c.px = r.px;
      c.py = r.py;
      c.pz = r.pz;
      c.hx = r.hx;
      c.hy = r.hy;
      c.hz = r.hz;
      cands.push_back(c);
    }
  }
  constexpr int32_t kEventCursor = 0x00010000;
  constexpr float kScale3d = 1.469f;  // Osd.SCALE_3D
  InvObject* state = game_logic_actual_state();
  InvObject* focus_osd = state ? tree_field_get_obj(state, "osd") : nullptr;
  InvObject* best_phy = nullptr;
  InvObject* best_group = nullptr;
  float best_px = 0, best_py = 0, best_pz = 0;
  float best_area = 0.f;
  bool have = false;
  for (const Cand& c : cands) {
    if ((tree_field_get_int(c.parent, "event_mask") & kEventCursor) == 0)
      continue;
    InvObject* osd = tree_field_get_obj(c.parent, "osd");
    if (!osd) continue;
    // No current-state OSD → do not guess (Garage chrome would steal Valocity).
    if (!focus_osd || osd != focus_osd) continue;
    InvObject* gh = tree_field_get_obj(osd, "globalHandler");
    if (!gh || gh != state) continue;
    float vw = tree_field_get_float(osd, "vpWidth");
    float vh = tree_field_get_float(osd, "vpHeight");
    float va = tree_field_get_float(osd, "vpAspect");
    if (vw <= 0.f) vw = 1.f;
    if (vh <= 0.f) vh = 1.f;
    if (va <= 0.f) va = 1.f;
    const float cx3 = ndc_x * kScale3d * va * vw;
    const float cy3 = -ndc_y * kScale3d * vh;
    if (std::fabs(cx3 - c.px) > c.hx) continue;
    if (std::fabs(cy3 - c.py) > c.hy) continue;
    const float area = c.hx * c.hy;
    if (!have || c.pz > best_pz + 1e-6f ||
        (std::fabs(c.pz - best_pz) <= 1e-6f && area < best_area)) {
      have = true;
      best_phy = c.phy;
      best_group = c.parent;
      best_px = c.px;
      best_py = c.py;
      best_pz = c.pz;
      best_area = area;
    }
  }
  if (out_phy) *out_phy = best_phy;
  if (out_group) *out_group = best_group;
  if (out_px) *out_px = best_px;
  if (out_py) *out_py = best_py;
  if (out_pz) *out_pz = best_pz;
  return have;
}

// ---- GroundRef ----
// PE @ 0x00483E60 size 0x1E9 (489). GroundRef.alignToRoad(Vector3)→Vector3[2].
// Unbox rp + this (JVM_UnboxArg @ 0x0045D910). rp==null → null. this
// Native.ptr (dword_62E008 @ 0x0042AB50)==0 → null. thiscall
// sub_426470(g_EngineState @ 0x00636338, handle, 0x39=57 GroundMap, 0)==0
// → null. Read rp z/y/x; thiscall GroundMap::sub_583A00 @ 0x00583A00(map,
// &pos, &tan): sub_581BF0 path lookup; RouteSpline_paramAtXZ; tangent via
// sub_57EB00 (negated if param≥*(path+96)*0.5); sub_57E960 snaps pos on
// spline. Return uncaught if sub_583A00==0. [0]=aligned pos, [1]=tangent.
// Engine_malloc 0x1C×2 + JVM_getClass Vector3 + sub_42B0A0 len-2 array.
// xref: Natives_RegisterAll data @ 0x004899CA only. City: [0].y height;
// normalize([1]) for lane spacing (halfStreetWidth rotate).
// Host: g_res entry = Native.ptr; sub_426470 type-57 = attached GroundRef;
// physics_road_project = sub_583A00 stand-in (no param-half tangent flip).
InvObject* java_util_resource_GroundRef_alignToRoad(InvObject* self,
                                                    InvObject* rp) {
  // PE @ 0x00483E60: rp/handle/GroundMap gate; physics_road_project align.
  if (!rp) return nullptr;
  if (!self) return nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_res.find(self) == g_res.end()) return nullptr;
  }
  float x = 0.f, y = 0.f, z = 0.f;
  vec3_get(rp, &x, &y, &z);
  float ox = x, oy = y, oz = z;
  float dx = 0.f, dy = 0.f, dz = 1.f;
  if (!physics_road_project(x, z, &ox, &oy, &oz, &dx, &dy, &dz))
    oy = physics_ground_y();
  InvObject* arr = tree_vector_new();
  tree_vector_add(arr, vec3_new(ox, oy, oz));
  tree_vector_add(arr, vec3_new(dx, dy, dz));
  return arr;
}

// Phase 2.23 — junction / path queries on host road graph.
InvObject* java_util_resource_GroundRef_getNearestCross(InvObject* self,
                                                        InvObject* approx,
                                                        float distance) {
  (void)self;
  float x = 0.f, y = 0.f, z = 0.f;
  if (approx) vec3_get(approx, &x, &y, &z);
  return physics_road_nearest_cross(x, y, z, distance);
}

InvObject* java_util_resource_GroundRef_getStartDirection(InvObject* self,
                                                          InvObject* from,
                                                          InvObject* to) {
  (void)self;
  float fx = 0, fy = 0, fz = 0, tx = 0, ty = 0, tz = 0;
  if (from) vec3_get(from, &fx, &fy, &fz);
  if (to) vec3_get(to, &tx, &ty, &tz);
  return physics_road_start_direction(fx, fy, fz, tx, ty, tz);
}

// PE @ 0x00483750 size 0x206 (518). GroundRef.findRoute / getRouteLength(V,V)F.
// Shared native body (registry findRoute @ 0x00616AD8, getRouteLength(V,V) @
// 0x00616A24). Default -1.0f. JVM_UnboxArg @ 0x0045D910 (this+p1); this/p2
// null → -1. Native.ptr dword_62E008 + sub_426470(handle,0x39=57,0) GroundMap
// gate → -1. Read p1/p2 xyz (JVM_vm_get_float_field). GroundMap_findRoute @
// 0x005826F0(a7=0,a8=0); null → -1, cache untouched. RouteSpline_paramAtXZ @
// 0x0057EF20 at each endpoint → GroundRef_routeParamStart/End (6408D8/6408DC);
// return fabs(u1-u0) → GroundRef_cachedRouteLength (6408D4). Prior
// GroundRef_cachedRoute (6408D0): sub_57DA40 + Engine_free. Cache spline ptr +
// endpoint coords 6408E0..6408F4. xrefs: Natives_RegisterAll 0x4899AB/0x4899E9.
// Host: g_res=sub_426470 type-57; physics_road_route_length(pe_ok) builds
// polyline; route_arc_param_xz_locked = RouteSpline_paramAtXZ on g_last_route.
float java_util_resource_GroundRef_findRoute(InvObject* self, InvObject* p1,
                                             InvObject* p2) {
  if (!self || !p1 || !p2) return -1.f;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_res.find(self) == g_res.end()) return -1.f;
  }
  float x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
  vec3_get(p1, &x0, &y0, &z0);
  vec3_get(p2, &x1, &y1, &z1);
  bool ok = false;
  const float len =
      physics_road_route_length(x0, y0, z0, x1, y1, z1, &ok);
  return ok ? len : -1.f;
}

// PE @ 0x00483C00 size 0x14 (20). GroundRef.getRouteLength()F. No this/unbox.
// Gate GroundRef_cachedRoute (6408D0); hit → GroundRef_cachedRouteLength
// (6408D4), else 0. Writer: findRoute/getRouteLength(V,V) @ 0x00483750.
float java_util_resource_GroundRef_getRouteLength(InvObject* self) {
  (void)self;
  return physics_road_last_route_length();
}

// PE @ 0x00483B30 size 0xD0 (208). GroundRef.getRoutePos(F)Vector3.
// Callees: JVM_UnboxArg @ 0x0045D910 (this + t). this unused after unbox.
// Gate dword_6408D0 (cached route spline from findRoute @ 0x00483750 /
// getRouteLength(V,V)F); span dword_6408D4. dword_6408D0==0 → null.
// t unclamped: u=(1-t)*dword_6408D8+t*dword_6408DC (sub_57EF20 arc params
// at route endpoints); sub_57EA00(spline,u,0) → xyz (16 xrefs — plotRoute
// @ 0x00483960 / traffic; not renamed). Y kept (plotRoute → 0xBA03126F).
// Engine_malloc 0x1C + JVM_getClass("java.lang.Vector3") + set float x/y/z.
// Host: g_last_route + u0/u1; physics_road_route_sample; miss → nullptr.
InvObject* java_util_resource_GroundRef_getRoutePos(InvObject* self, float t) {
  // PE @ 0x00483B30: dword_6408D0 spline sample; host g_last_route.
  (void)self;
  float x = 0.f, y = 0.f, z = 0.f;
  if (!physics_road_route_sample(t, &x, &y, &z)) return nullptr;
  return vec3_new(x, y, z);
}

// VA 0x00483C20 — no spline/span<=0 → 0. pos null → -1. Y discarded.
float java_util_resource_GroundRef_getRouteDist(InvObject* self, InvObject* pos) {
  (void)self;
  if (physics_road_last_route_length() <= 0.f) return 0.f;
  if (!pos) return -1.f;
  float x = 0, y = 0, z = 0;
  vec3_get(pos, &x, &y, &z);
  return physics_road_route_param(x, y, z);
}

// PE @ 0x00484E20 size 0x49 (73). GroundRef.removePedestrianType(GameRef)V
// — PE name string (Java source: remPedestrianType). Unbox dests swapped vs
// addPedestrianType @ 0x00484D90: rem dest0=&arg_0(=this) dest1=&var_4(=g);
// add dest0=&var_4(=this) dest1=&arg_0(=g). Gate both: get_int_field(this,
// dword_62E008) then sub_426470(handle, 0x3D=61, 0); fail → early out.
// Contrast add: also prep *[g+0xC] (vtable+0x14 if +0x4C!=1, sub_5447D0
// flags 0x80000000, vtable+0xC 1.0f) before Pedestrian_addType @ 0x00589940
// (32-slot, skip dup on g+8). rem: no mesh prep; Pedestrian_remType @
// 0x00589A20 (unique xref): scan 16-byte slots @ eng+13256 keyed [g+8],
// miss=no-op; hit unlink +0x48, count-- @+13244, compact last into hole.
// Stock Valocity never calls (only addPedestrianType ×6). Host: forward to
// remPedestrianType (erase ped_samples by ResourceRef_id; miss no-op).
void java_util_resource_GroundRef_removePedestrianType(InvObject* self,
                                                       InvObject* g) {
  // PE @ 0x00484E20: type-61 gate + Pedestrian_remType(g); host state in rem*.
  java_util_resource_GroundRef_remPedestrianType(self, g);
}

// ---- SfxRef ----
// VA 0x00480D40 size 0x120. UnboxArg: this, pos, radius, pitch, volume,
// flags, instance (I/F = DWORD at box+8). Handle via dword_62E008;
// 0 → Mighty ERROR return -1. Null Vector3: skip x/y/z; pass NULL into
// sub_48CFB0 (thiscall, 20+ xrefs — not renamed). Non-null: read x/y/z;
// neg/sbb → &stack_xyz. handle+8 type 0x26..0x2B is a nop (reload pos).
// volume *= flt_612C58 (0x3F800000 = 1.0). thiscall:
//   sub_48CFB0(handle, pitch, vol, flags, pos_or_0, instance, radius, 0, 0)
// pos+radius → sub_550560 listener dist cull (radius>0: dist²-r²;
// <16.0 → play; >62500.0 → -1). Null pos → flags|4 (2D). Success returns the instance
// arg (0 included); fail -1. 3D atten not ported (no mixer field).
// Host: !self / id==0 → -1. audio_sfx_play 0 (fail) → -1. pos/radius
// unused. Host still allocates voice id when instance==0 (PE echoes 0).
int32_t java_util_resource_SfxRef_nplay(InvObject* self, InvObject* pos,
                                        float radius, float pitch, float volume,
                                        int32_t flags, int32_t instance) {
  // PE @ 0x00480D40 size 0x120. Unbox this/pos/radius/pitch/volume/flags/
  // instance. Native.ptr==0 → Mighty then -1. pos==null: 2D flags|=4.
  // vol *= 1.0. thiscall sub_48CFB0; cull dist2 vs 62500 (sub_550560) NOT
  // ported. Success echoes instance including 0; Sfx_MixerPlay eax ignored.
  // Host: !self / id==0 → -1 (no Mighty). instance==0 still maps 0→-1
  // (PE echoes 0). Cluster sfx_3d_cull undone.
  (void)pos;
  (void)radius;
  if (!self) return -1;
  const int32_t rid = java_util_resource_ResourceRef_id(self);
  if (rid == 0) return -1;
  const int32_t id = audio_sfx_play(rid, pitch, volume, flags, instance);
  return id == 0 ? -1 : id;
}

// VA 0x00480E60 size 0x7b. UnboxArg: this, instance. Handle 0 → Mighty
// ERROR (void). Else thiscall sub_48D180(handle, instance) →
// sub_550870(instance, handle+8). Host: !self / id==0 → ret.
void java_util_resource_SfxRef_stop(InvObject* self, int32_t instance) {
  if (!self) return;
  if (java_util_resource_ResourceRef_id(self) == 0) return;
  audio_sfx_stop(instance);
}

}  // namespace inv
