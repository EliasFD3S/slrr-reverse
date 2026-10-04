#include "host_objects.hpp"
#include "natives.hpp"
#include "runtime.hpp"
#include "rpak.hpp"
#include "render_d3d9.hpp"
#include "input_win32.hpp"
#include "tree_interp.hpp"
#include "audio_win32.hpp"
#include "System.h"
#include "Resources.h"
#include "Resources_internal.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace inv {
namespace {

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
// endpoints via RouteSpline_paramAtXZ @ 0x57EF20 → dword_6408D8/6408DC.
float g_last_route_u0 = 0.f;
float g_last_route_u1 = 0.f;



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

std::mutex g_mu;
std::unordered_map<InvObject*, LineState> g_lines;
std::unordered_map<InvObject*, ResState> g_res;
int32_t g_next_id = 1;
ResState& R(InvObject* self) { return g_res[self]; }


// ---- Native.ptr / ResHandle_getPayload (OOS unlock) ----
// PE handle: Engine_malloc(16) — +0/+4 links, +8 alive, +0xC node*.
// getPayload tag 0xA0000000: ResNode_vtbl+0xC → ResNode_getEmbeddedMid
// @ 0x0053EFA0 returns this+0xD8 (host: &mid). Mid layout (PE):
//   +0x0C leaf*, +0x40 type_id (53), +0x44 nested node*, +0x4C block*,
//   +0x84 fog child, +0xBC bone flags, +0xF8 stamp, +0x12C bone-id walk,
//   +0x13C id-list sentinel, +0x168/+0x178 own-list sentinel/head (≥0x17C).
// setParent type1 @ 0x48AD0A: nest hop → leaf.vtbl+0x20
//   = CameraCtrl_attachSetParent @ 0x00438590 | Chassis_attachSetParent
//   @ 0x0044C170. CameraCtrl: handleAttach(block) + setParent_inner(block+0x30)
//   — block = mid+0x4C ≥0x40 (attach), emb handle at +0x30 (+0x3C node*).
// Type53: parent_inner+0xCC → WT node; pay+0x40==53;
//   Type53_ensureHandleSlot(*(pay+0x4C)+0xB4, ...) @ 0x004B3EE0.
constexpr int32_t kResTagA0 = static_cast<int32_t>(0xA0000000u);
constexpr int32_t kPayloadType53 = 53;
constexpr int32_t kBoneParentLinked = 0x1800;
constexpr int32_t kWtBoneUnlink = 0x4000;
constexpr int32_t kRelinkLodFlag = 0x100;       // inner+0x54 → maybeRelinkLod
constexpr int32_t kRelinkLodCancel = 0x400;     // tryRelinkLod cancel (vtbl+0x2C)
constexpr int32_t kRelinkLodSlotted = 0x4000000;  // already in eng LOD slot list

struct HostFogParams {
  uint8_t pad0[0x1C]{};
  float near10 = 0.f;
  float far10 = 0.f;
  float enable = 1.f;
  int32_t color = 0;
};
static_assert(offsetof(HostFogParams, near10) == 0x1C, "fog+0x1C");
static_assert(offsetof(HostFogParams, far10) == 0x20, "fog+0x20");
static_assert(offsetof(HostFogParams, enable) == 0x24, "fog+0x24");
static_assert(offsetof(HostFogParams, color) == 0x28, "fog+0x28");

// PE mid (ResNode_getEmbeddedMid return): leaf / type53 / nest / block / fog /
// bone own-list (≥0x17C so +0x168/+0x178 sit on real getPayload mid).
struct HostMidPayload {
  uint8_t pad0[0x0C]{};
  void* leaf = nullptr;          // +0x0C → CameraCtrl / Chassis (vtbl+0x20)
  // PE GameType payload+0x10 Class* (LoadGameInit @ 0x53A8CE / Class_boxObject
  // @ 0x53A908). Host: Class* shell, or direct FQN C-string.
  void* script_class = nullptr;  // +0x10
  uint8_t pad_14_40[0x2C]{};     // +0x14 .. +0x3F
  int32_t payload_type = 0;      // +0x40; 53 = type53 blob
  void* nested = nullptr;        // +0x44 HostResNode* (setParent nest)
  uint8_t pad_48[4]{};           // +0x48 .. +0x4B (PE gap before block)
  void* block = nullptr;         // +0x4C handle/phys block arg to vtbl+0x20
  uint8_t pad_50_84[0x34]{};     // +0x50 .. +0x83
  void* child = nullptr;         // +0x84 → fog HostResNode*
  uint8_t pad_88_bc[0x34]{};     // +0x88 .. +0xBB
  int32_t bone_list_flags = 0;   // +0xBC (v8[47]) |= 0x1800
  uint8_t pad_c0_f8[0x38]{};     // +0xC0 .. +0xF7
  int32_t bone_stamp = 0;        // +0xF8 (v8[62]) ← dword_6200A4
  uint8_t pad_fc_12c[0x30]{};    // +0xFC .. +0x12B
  void* bone_tree = nullptr;     // +0x12C RenderPayload_findBoneById head
  uint8_t pad_130_13c[0x0C]{};   // +0x130 .. +0x13B
  uint8_t bone_id_sentinel[0x10]{};  // +0x13C PE this+79; +4 next, +8=head
  uint8_t pad_14c_168[0x1C]{};   // +0x14C .. +0x167
  uint8_t bone_own_sentinel[0x10]{}; // +0x168 own-list sentinel
  void* bone_own_head = nullptr; // +0x178 list HEAD

  HostMidPayload() {
    // findBoneById empty: *(+0x12C)+4 == 0 → return 0.
    bone_tree = bone_id_sentinel;
    // Own-list: head = &sentinel; *(sentinel+0x0C) = &sentinel.
    bone_own_head = bone_own_sentinel;
    *reinterpret_cast<void**>(bone_own_sentinel + 0x0C) = bone_own_sentinel;
  }
};
static_assert(offsetof(HostMidPayload, leaf) == 0x0C, "mid+0x0C");
static_assert(offsetof(HostMidPayload, script_class) == 0x10, "mid+0x10 Class*");
static_assert(offsetof(HostMidPayload, payload_type) == 0x40, "mid+0x40");
static_assert(offsetof(HostMidPayload, nested) == 0x44, "mid+0x44");
static_assert(offsetof(HostMidPayload, block) == 0x4C, "mid+0x4C");
static_assert(offsetof(HostMidPayload, child) == 0x84, "mid+0x84");
static_assert(offsetof(HostMidPayload, bone_list_flags) == 0xBC, "mid+0xBC");
static_assert(offsetof(HostMidPayload, bone_stamp) == 0xF8, "mid+0xF8");
static_assert(offsetof(HostMidPayload, bone_tree) == 0x12C, "mid+0x12C");
static_assert(offsetof(HostMidPayload, bone_id_sentinel) == 0x13C,
              "mid+0x13C");
static_assert(offsetof(HostMidPayload, bone_own_sentinel) == 0x168,
              "mid+0x168");
static_assert(offsetof(HostMidPayload, bone_own_head) == 0x178, "mid+0x178");
static_assert(sizeof(HostMidPayload) >= 0x17C, "mid≥0x17C");
static_assert(sizeof(HostMidPayload) == 0x17C, "mid==0x17C x86");

// PE bone node (Engine_malloc 244 / findOrInsertBone). Id @ +0x34; dllist
// +0x4/+0x8; own-list +0x0C/+0x10; flags +0x3C; matrix +0x54; pose +0xF0.
struct HostPeBoneNode {
  uint8_t raw[0xF4]{};
};

// PE Type53 object: mgr `this` for Type53_ensureHandleSlot is block+0xB4.
struct HostType53Block {
  uint8_t pad0[0xB4]{};
  uint8_t mgr[0xD4]{};  // +0xB4 ..; list heads used by ensureHandleSlot
};
static_assert(offsetof(HostType53Block, mgr) == 0xB4, "type53+0xB4");

// Leaf stand-in: PE CameraCtrl_vtbl / Chassis_vtbl; slot +0x20 attach.
// Layout: *[leaf]=vtbl, vtbl[+0x20]=CameraCtrl_attachSetParent @ 0x438590
// or Chassis_attachSetParent @ 0x44C170. Slot left null until GameRef calls.
struct HostLeafAttach {
  void** vtbl = nullptr;  // +0 → slots
  void* slots[9]{};       // [8] = +0x20
  HostLeafAttach() { vtbl = slots; }
};

// PE ResHandle_Rebind owner node (@ 0x00429060 / voidEvent add_light 0x45AFC9):
//   +0x44 ResHandle-head base; +0x48 list HEAD (a2[18]); +0x50 key → rh+8.
// pad0 covers +0x00..+0x4B (incl. +0x44/+0x48); pad_50_cc starts at +0x50.
struct HostResNode {
  uint8_t pad0[0x4C]{};
  int32_t type = 1;  // +0x4C INSTANCE_GAME
  uint8_t pad_50_cc[0x7C]{};  // +0x50 = Rebind key dword (rem_light +0x14)
  HostResNode* wt_cc = nullptr;  // +0xCC PE aux / type53 WT node*
  // Host-only fields below (getPayload returns &mid, not node+0xD8):
  int role = 0;  // 0=root, 1=fog, 2=nest, 3=type53 WT
  HostMidPayload mid{};
  HostFogParams fog{};
  HostResNode* fog_node = nullptr;
  HostResNode* nest_node = nullptr;
  HostResNode* type53_node = nullptr;
  HostLeafAttach* leaf = nullptr;
  HostType53Block* type53_block = nullptr;
  InvObject* owner = nullptr;
  InvObject* parent_link = nullptr;
  std::vector<HostPeBoneNode*> pe_bones;  // owned; findBoneById tree
};
static_assert(offsetof(HostResNode, type) == 0x4C, "node+0x4C");
static_assert(offsetof(HostResNode, pad_50_cc) == 0x50, "node+0x50 Rebind key");
static_assert(offsetof(HostResNode, wt_cc) == 0xCC, "node+0xCC");

// PE ResourceEngine type-buckets @ eng+i*0x90 (i=1..0x15) — GCSweep@537ED0.
// Touch head ptr @ bucket+0x150; scan head @ +0x17C; insertIndexedSlot base
// @ +0x16C. Node dllist +0xC/+0x10; stamp +0x74; GC flags +0x64 (0x10/20/40).
// Soft host: intrusive lists behind host_node_new / LookupById-created nodes.
constexpr int kGcTypeCount = 0x16;  // indices 0..0x15; loop uses 1..0x15
constexpr uint32_t kGcFlagTouch = 0x10u;
constexpr uint32_t kGcFlagAged = 0x20u;
constexpr uint32_t kGcFlagEvict = 0x40u;
constexpr uint32_t kGcFlagBucketMask = 0x50u;  // PE and 0xFFFFFFAF clears 0x10|0x40

// PE g_GCSweep_typeGate @ 0x618DC8 — stock .rdata: types 1,5,6,7 = 1.0f.
static const float kGcTypeGate[kGcTypeCount] = {
    0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f,
    0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f,
};

// PE GCSweep budget Hi/Lo @ 0x618D4C..68 (stock .rdata u32, IDA get_int).
// Pre-scan (a2==0): live < Hi && force==0 → skip; else force ^= 1.
// W33C post-evict (a2==0): live < Lo → force=0 + LABEL_74; else cap++ / stop.
constexpr uint32_t kGcBudgetType5Hi = 131072u;     // @ 0x618D4C
constexpr uint32_t kGcBudgetType5Lo = 32768u;      // @ 0x618D50
constexpr uint32_t kGcBudgetType7Hi = 134217728u;  // @ 0x618D54
constexpr uint32_t kGcBudgetType7Lo = 8388608u;    // @ 0x618D58
constexpr uint32_t kGcBudgetType1Hi = 2048u;       // @ 0x618D5C
constexpr uint32_t kGcBudgetType1Lo = 1024u;       // @ 0x618D60
constexpr uint32_t kGcBudgetType6Hi = 8388608u;    // @ 0x618D64
constexpr uint32_t kGcBudgetType6Lo = 6291456u;    // @ 0x618D68
constexpr uint32_t kGcEvictCapType5 = 16u;         // @ 0x618F80
constexpr uint32_t kGcEvictCapType7 = 16u;         // @ 0x618F84
constexpr uint32_t kGcEvictCapType1 = 16u;         // @ 0x618F88
constexpr uint32_t kGcEvictCapType6 = 16u;         // @ 0x618F8C

// PE dword_619D68[type] — stock .rdata (IDA get_int). Evict gate @ 0x53811C:
// typeGate!=0 && node[+0xD0]!=0 → skip evict.
static const uint32_t kGcTypeEvictGate[kGcTypeCount] = {
    0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

// Soft stand-in for PE g_Engine_frameStamp @ 0x6200A4 (System ++ OOS here).
static uint32_t g_res_gc_frame = 0;

// PE eng+0x106ED8..EE4 force gates (a2!=0 set 1 @ 0x537F0A..1C; clear
// @ 0x53833C). XOR toggle on budget enter @ 0x538029/04E/072/097.
static uint32_t g_gc_force_type7 = 0;  // eng+0x106ED8
static uint32_t g_gc_force_type5 = 0;  // eng+0x106EDC
static uint32_t g_gc_force_type1 = 0;  // eng+0x106EE0
static uint32_t g_gc_force_type6 = 0;  // eng+0x106EE4

// Soft live counters for budget gate (PE BSS). type1 = g_lgi_create_count
// @ 765F60. type5/6/7 producers OOS (soft 0 → under-budget skip until force).
static uint32_t g_gc_live_type5 = 0;  // PE @ 0x65BD44
static uint32_t g_gc_live_type7 = 0;  // PE @ 0x65C234
static uint32_t g_gc_live_type6 = 0;  // PE @ 0x765F3C
uint32_t g_lgi_create_count = 0;     // PE g_InstanceNodeCreateCount @ 0x765F60

struct HostGcTypeBucket {
  HostResNode touch_sent{};  // ≡ eng+i*0x90 soft; head walks via +0xC
  HostResNode* touch_head = nullptr;  // ≡ *[bucket+0x150] → &touch_sent
  HostResNode aged_sent{};            // soft insertIndexedSlot@+0x16C
  HostResNode* aged_head = nullptr;
  // Soft: aged list ≡ scan pool (PE +0x17C views insertIndexedSlot@+0x16C).
  HostResNode* scan_head = nullptr;   // alias → aged_head after ensure
  HostResNode evict_sent{};           // ≡ bucket+0x198 sentinel
  HostResNode* evict_head = nullptr;  // ≡ *[bucket+0x1C0]
  bool inited = false;
};

static HostGcTypeBucket g_gc_buckets[kGcTypeCount];

static HostResNode*& node_dll_next(HostResNode* n) {
  return *reinterpret_cast<HostResNode**>(reinterpret_cast<char*>(n) + 0x0C);
}
static HostResNode*& node_dll_prev(HostResNode* n) {
  return *reinterpret_cast<HostResNode**>(reinterpret_cast<char*>(n) + 0x10);
}
static uint32_t& node_gc_flags(HostResNode* n) {
  return *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(n) + 0x64);
}
static uint32_t& node_gc_stamp(HostResNode* n) {
  return *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(n) + 0x74);
}
// PE wantUnloadFrame stamp @ node+0x78 (≠ GC touch stamp +0x74).
static uint32_t& node_unload_frame(HostResNode* n) {
  return *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(n) + 0x78);
}

static void gc_bucket_ensure(int type) {
  if (type < 1 || type >= kGcTypeCount) return;
  HostGcTypeBucket& b = g_gc_buckets[type];
  if (b.inited) return;
  b.inited = true;
  // Empty: head=&sent, [sent+0xC]==0 (PE @ 0x537F3E..0x537F45).
  node_dll_next(&b.touch_sent) = nullptr;
  node_dll_prev(&b.touch_sent) = nullptr;
  b.touch_head = &b.touch_sent;
  node_dll_next(&b.aged_sent) = nullptr;
  node_dll_prev(&b.aged_sent) = nullptr;
  b.aged_head = &b.aged_sent;
  b.scan_head = b.aged_head;  // soft ≡ PE scan @ +0x17C → aged pool
  node_dll_next(&b.evict_sent) = nullptr;
  node_dll_prev(&b.evict_sent) = nullptr;
  b.evict_head = &b.evict_sent;
}

static void gc_list_fix_head(HostResNode*& head, HostResNode* sent,
                             HostResNode* removed, HostResNode* next) {
  if (head != removed) return;
  // Next real, else empty → &sent (PE head field).
  if (next && next != sent && node_dll_next(next))
    head = next;
  else if (next && next != sent)
    head = next;  // sole remaining; next→sent
  else
    head = sent;
}

// PE dllist unlink shape @ GCSweep 0x53814A..0x538163 / 0x5382A5..0x5382BE.
static void gc_node_dll_unlink(HostResNode* n) {
  if (!n) return;
  HostResNode* next = node_dll_next(n);
  HostResNode* prev = node_dll_prev(n);
  if (prev && next) {
    node_dll_prev(next) = prev;  // [next+0x10]=prev
    node_dll_next(prev) = next;  // [prev+0xC]=next
  } else if (prev) {
    node_dll_next(prev) = next;
  } else if (next) {
    node_dll_prev(next) = prev;
  }
  node_dll_next(n) = nullptr;
  node_dll_prev(n) = nullptr;
  for (int t = 1; t < kGcTypeCount; ++t) {
    HostGcTypeBucket& b = g_gc_buckets[t];
    if (!b.inited) continue;
    gc_list_fix_head(b.touch_head, &b.touch_sent, n, next);
    gc_list_fix_head(b.aged_head, &b.aged_sent, n, next);
    b.scan_head = b.aged_head;
    gc_list_fix_head(b.evict_head, &b.evict_sent, n, next);
  }
}

// Soft HEAD push: head=first real; last.next=&sent; [sent+0xC]==0.
static void gc_list_push_head(HostResNode*& head, HostResNode* sent,
                              HostResNode* n) {
  if (!n || !sent || n == sent) return;
  HostResNode* old = head ? head : sent;
  if (old == n) return;
  node_dll_next(n) = old;  // may be first real or &sent (empty)
  node_dll_prev(n) = nullptr;
  if (old != sent) node_dll_prev(old) = n;
  else node_dll_prev(sent) = n;
  head = n;
}

// Soft touch-list insert — producers into eng+0x150 OOS; host seeds on
// host_node_new (LookupById/Bind/native_ptr path). Seed-only (no re-touch).
static void gc_bucket_touch_insert(HostResNode* n) {
  if (!n) return;
  int type = n->type;
  if (type < 1 || type >= kGcTypeCount) type = 1;
  gc_bucket_ensure(type);
  HostGcTypeBucket& b = g_gc_buckets[type];
  if (node_dll_next(n) || node_dll_prev(n)) return;  // already linked
  gc_list_push_head(b.touch_head, &b.touch_sent, n);
  node_gc_stamp(n) = g_res_gc_frame;  // ≡ node[+0x74] ← frame
  node_gc_flags(n) =
      (node_gc_flags(n) & ~kGcFlagBucketMask) | kGcFlagTouch;  // soft |0x10
}

// PE ResourceEngine_TouchResNode @ 0x00537D10 size ~0x66.
// this=g_ResourceEngine (type-bucket base); a2=node. Host: type from +0x4C.
// Always stamp +0x74 ← g_res_gc_frame (≡ g_Engine_frameStamp@6200A4).
// If !(+0x64&0x10): unlink +0xC/+0x10, push touch list (eng+type*0x90+0x140
// soft), flags = (flags & ~0x70) | 0x10. If already touch: stamp-only.
static void resource_engine_touch_res_node(HostResNode* n) {
  if (!n) return;
  node_gc_stamp(n) = g_res_gc_frame;                      // @ 0x537D1A
  if ((node_gc_flags(n) & kGcFlagTouch) != 0) return;     // @ 0x537D21
  int type = n->type;                                     // +0x4C @ 0x537D4F
  if (type < 1 || type >= kGcTypeCount) type = 1;
  gc_bucket_ensure(type);
  HostGcTypeBucket& b = g_gc_buckets[type];
  gc_node_dll_unlink(n);  // @ 0x537D24..0x537D3F (+ clear +0xC/+0x10)
  gc_list_push_head(b.touch_head, &b.touch_sent, n);  // soft ≡ +0x150/+0x168
  // PE & 0xFFFFFF8F | 0x10 — clear 0x10|0x20|0x40, set touch.
  node_gc_flags(n) = (node_gc_flags(n) & ~0x70u) | kGcFlagTouch;  // @ 0x537D71
}

static HostResNode* gc_list_walk_start(HostResNode* head) {
  // PE @ 0x537F38: head=*[bucket+0x150]; empty iff [head+0xC]==0.
  if (!head || !node_dll_next(head)) return nullptr;
  return head;
}

static HostResNode* gc_list_walk_advance(HostResNode* cur) {
  // PE @ 0x537F63: next=[cur+0xC]; stop if next==0 || [next+0xC]==0.
  if (!cur) return nullptr;
  HostResNode* n = node_dll_next(cur);
  if (!n || !node_dll_next(n)) return nullptr;
  return n;
}

// Soft insertIndexedSlot(1) @ 0x537FAB → eng+type*0x90+0x16C aged list.
static void gc_bucket_age_insert(HostResNode* n, int type) {
  if (!n) return;
  if (type < 1 || type >= kGcTypeCount) type = 1;
  gc_bucket_ensure(type);
  HostGcTypeBucket& b = g_gc_buckets[type];
  gc_node_dll_unlink(n);
  gc_list_push_head(b.aged_head, &b.aged_sent, n);
  b.scan_head = b.aged_head;
  node_gc_flags(n) =
      (node_gc_flags(n) & ~kGcFlagBucketMask) | kGcFlagAged;  // |=0x20
}

// PE evict move @ 0x53814A..0x538198 → bucket+0x198/+0x1C0; flags|=0x40.
static void gc_bucket_evict_insert(HostResNode* n, int type) {
  if (!n) return;
  if (type < 1 || type >= kGcTypeCount) type = 1;
  gc_bucket_ensure(type);
  HostGcTypeBucket& b = g_gc_buckets[type];
  gc_node_dll_unlink(n);
  gc_list_push_head(b.evict_head, &b.evict_sent, n);
  b.scan_head = b.aged_head;
  // PE and 0xFFFFFFCF | 0x40 — clear 0x10|0x20, set 0x40.
  node_gc_flags(n) = (node_gc_flags(n) & ~0x30u) | kGcFlagEvict;
}

// Soft PE *(node+0x14)!=0 && *[payload+0x50]!=0 @ 0x5380D4..0x5380E4.
// Host: mid.block / leaf / script_class stand in for loaded payload.
static bool gc_node_has_scan_payload(HostResNode* n) {
  if (!n) return false;
  return n->mid.block != nullptr || n->mid.leaf != nullptr ||
         n->mid.script_class != nullptr;
}

// PE node[+0xD0] — host pad ends @ +0xCC; soft read 0 (passes typeGate).
static uint32_t node_gc_aux_d0(HostResNode* n) {
  if (!n) return 0;
  // +0xD0 sits past wt_cc@+0xCC; host-only fields follow — treat as 0.
  (void)n;
  return 0;
}

// PE Native.ptr = Engine_malloc(16) @ ResourceRef_newNative 0x47CEA0.
// CameraCtrl_attachSetParent @ 0x438590: handleAttach(mid+0x4C) then
// setParent_inner(block+0x30) — so mid+0x4C is a ≥0x40 attach/phys block
// (not the 16B box). Host: PE 16B head + embedded attach (≥0x40);
// mid.block → &attach (≠ handle*) so GameRef type1 gate fires.
struct HostAttachBlock {
  void* link0 = nullptr;             // +0 (handle-shaped head)
  void* link1 = nullptr;             // +4
  int32_t alive = 1;                 // +8
  HostResNode* node = nullptr;       // +0xC — handleAttachUnderParent
  uint8_t pad_10_30[0x20]{};         // +0x10 .. +0x2F
  void* emb_prev = nullptr;          // +0x30 — setParent_inner this
  void* emb_next = nullptr;          // +0x34
  int32_t emb_alive = 1;             // +0x38
  HostResNode* emb_node = nullptr;   // +0x3C ResHandle_getNode
};
static_assert(offsetof(HostAttachBlock, alive) == 8, "attach+8");
static_assert(offsetof(HostAttachBlock, node) == 0xC, "attach+0xC");
static_assert(offsetof(HostAttachBlock, emb_prev) == 0x30, "attach+0x30");
static_assert(offsetof(HostAttachBlock, emb_node) == 0x3C, "attach+0x3C");
static_assert(sizeof(HostAttachBlock) == 0x40, "attach==0x40");

struct HostNativeHandle {
  // PE Native.ptr 16B box (side-map key / Type53 a2 / alive+node):
  HostNativeHandle* prev = nullptr;
  HostNativeHandle* next = nullptr;
  int32_t alive = 1;
  HostResNode* node = nullptr;
  // Host-only: CameraCtrl mid+0x4C stand-in (≠ this).
  HostAttachBlock attach{};
};
static_assert(offsetof(HostNativeHandle, alive) == 8, "handle+8");
static_assert(offsetof(HostNativeHandle, node) == 0xC, "handle+0xC");
static_assert(offsetof(HostNativeHandle, attach) == 0x10, "handle+0x10 attach");
static_assert(sizeof(HostNativeHandle) == 0x50, "handle==0x50 x86");

static std::unordered_map<InvObject*, HostNativeHandle*> g_native_ptr;
// W14C: durable FQN when mid+0x10 stores C-string (not Class*). Keyed by
// HostResNode*; erased in host_node_free_tree.
static std::unordered_map<HostResNode*, std::string> g_mid_script_fqn_own;

// PE ResourceEngine_type_gametype @ 0x53A27D: mov [eax+10h], ecx
// (payload+0x10 = Class*). Host: Class* InvObject* or interned FQN cstr.
static void host_mid_seed_script_class(HostResNode* n, void* clazz,
                                       const char* fqn_fallback) {
  if (!n) return;
  n->type = 8;  // RESTYPE_GAME
  if (clazz) {
    n->mid.script_class = clazz;
    return;
  }
  if (!fqn_fallback || !fqn_fallback[0]) return;
  auto& s = g_mid_script_fqn_own[n];
  s = fqn_fallback;
  n->mid.script_class = const_cast<char*>(s.c_str());
}

// Wire attach block for CameraCtrl: +0xC = root node; emb +0x30 handle
// shape. emb_node = same root (type1) — host setParent_inner stops on
// type1 (no nest recurse); PE secondary emb target unknown.
// mid.block = &attach ≠ handle* so GameRef type1 gate calls emb path.
static void host_handle_wire_attach(HostNativeHandle* h) {
  if (!h || !h->node) return;
  h->attach.alive = h->alive;
  h->attach.node = h->node;
  h->attach.emb_alive = 1;
  h->attach.emb_node = h->node;
  // Upgrade legacy mid.block==handle (16B OOB for +0x30).
  if (!h->node->mid.block || h->node->mid.block == static_cast<void*>(h))
    h->node->mid.block = &h->attach;
}

static void host_node_free_tree(HostResNode* n) {
  if (!n) return;
  gc_node_dll_unlink(n);  // W29C: drop from GCSweep type-bucket lists
  // fog/nest/type53 share no owned leaf/block with siblings; root owns
  // leaf + type53_block. Clear aliases before recursing.
  if (n->fog_node) {
    host_node_free_tree(n->fog_node);
    n->fog_node = nullptr;
  }
  if (n->nest_node) {
    n->nest_node->leaf = nullptr;
    host_node_free_tree(n->nest_node);
    n->nest_node = nullptr;
  }
  if (n->type53_node) {
    n->type53_node->type53_block = nullptr;
    host_node_free_tree(n->type53_node);
    n->type53_node = nullptr;
  }
  for (HostPeBoneNode* b : n->pe_bones) delete b;
  n->pe_bones.clear();
  delete n->leaf;
  n->leaf = nullptr;
  delete n->type53_block;
  n->type53_block = nullptr;
  g_mid_script_fqn_own.erase(n);
  delete n;
}

// PE RenderPayload_findBoneById @ 0x005413C0: walk mid+0x12C via bone+4;
// match bone+0x34 == id. Empty: *(head+4)==0 → 0.
static HostPeBoneNode* render_payload_find_bone_by_id(HostMidPayload* mid,
                                                     int32_t bone_id) {
  if (!mid) return nullptr;
  auto* result = reinterpret_cast<uint8_t*>(mid->bone_tree);
  if (!result) return nullptr;
  if (*reinterpret_cast<void**>(result + 4) == nullptr) return nullptr;
  for (;;) {
    const int32_t diff =
        *reinterpret_cast<int32_t*>(result + 0x34) - bone_id;
    if (diff == 0) return reinterpret_cast<HostPeBoneNode*>(result);
    if (diff > 0) return nullptr;
    result = *reinterpret_cast<uint8_t**>(result + 4);
    if (!result || *reinterpret_cast<void**>(result + 4) == nullptr)
      return nullptr;
  }
}

// Host stand-in for RenderInst_findOrInsertBone id path: ensure a node with
// +0x34==id on mid+0x12C sorted list (ascending), terminated at +0x13C sentinel.
static HostPeBoneNode* mid_ensure_bone_by_id(HostResNode* node, int32_t bone_id) {
  if (!node) return nullptr;
  HostMidPayload& mid = node->mid;
  if (HostPeBoneNode* hit = render_payload_find_bone_by_id(&mid, bone_id))
    return hit;
  auto* bone = new HostPeBoneNode{};
  *reinterpret_cast<int32_t*>(bone->raw + 0x34) = bone_id;
  // PE ctor identity bits on matrix diag @ +0x54/+0x7C/+0xA4 (partial).
  *reinterpret_cast<float*>(bone->raw + 0x54) = 1.f;
  *reinterpret_cast<float*>(bone->raw + 0x68) = 1.f;
  *reinterpret_cast<float*>(bone->raw + 0x7C) = 1.f;
  *reinterpret_cast<int32_t*>(bone->raw + 0x3C) = 2;  // identity bit
  node->pe_bones.push_back(bone);

  void* sent = mid.bone_id_sentinel;
  auto* cur = reinterpret_cast<uint8_t*>(mid.bone_tree);
  uint8_t* prev = nullptr;
  // Empty: tree → sentinel with +4==0.
  if (!cur || *reinterpret_cast<void**>(cur + 4) == nullptr) {
    *reinterpret_cast<void**>(bone->raw + 4) = sent;
    *reinterpret_cast<void**>(bone->raw + 8) = nullptr;
    mid.bone_tree = bone;
    // PE this+81 (+0x144) = dllist head slot inside sentinel+8.
    *reinterpret_cast<void**>(mid.bone_id_sentinel + 8) = bone;
    return bone;
  }
  while (cur && *reinterpret_cast<void**>(cur + 4) != nullptr) {
    const int32_t cid = *reinterpret_cast<int32_t*>(cur + 0x34);
    if (cid > bone_id) break;
    prev = cur;
    cur = *reinterpret_cast<uint8_t**>(cur + 4);
  }
  *reinterpret_cast<void**>(bone->raw + 4) = cur ? cur : sent;
  *reinterpret_cast<void**>(bone->raw + 8) = prev;
  if (prev)
    *reinterpret_cast<void**>(prev + 4) = bone;
  else
    mid.bone_tree = bone;
  return bone;
}

// PE SetBoneMatrix HEAD @ 0x48C094: insert bone at payload+0x178 /
// sentinel +0x168.
static void mid_bone_own_list_insert_head(HostMidPayload* mid,
                                          HostPeBoneNode* bone) {
  if (!mid || !bone) return;
  void* sentinel = mid->bone_own_sentinel;
  if (!mid->bone_own_head) {
    mid->bone_own_head = sentinel;
    *reinterpret_cast<void**>(mid->bone_own_sentinel + 0x0C) = sentinel;
  }
  void* old_head = mid->bone_own_head;
  *reinterpret_cast<void**>(reinterpret_cast<char*>(old_head) + 0x0C) = bone;
  *reinterpret_cast<void**>(bone->raw + 0x0C) = sentinel;
  *reinterpret_cast<void**>(bone->raw + 0x10) = old_head;
  mid->bone_own_head = bone;
}

// PE ResNode_setLodCopy6C @ 0x0053EFF0 (ResNode_vtbl+0x14):
//   *(+0x6C) = a2*(+0xBC)+(+0xB8); stamp vs dword_6200A4 OOS → always write.
static void res_node_set_lod_copy_6c(void* node, float a2) {
  if (!node) return;
  auto* p = reinterpret_cast<uint8_t*>(node);
  const float bias = *reinterpret_cast<float*>(p + 0xB8);
  const float scale = *reinterpret_cast<float*>(p + 0xBC);
  *reinterpret_cast<float*>(p + 0x6C) = a2 * scale + bias;
}

namespace {
// Soft PrepareLod@5447D0 / setLodReadyBit@544B90 — defined with LoadGameInit.
int32_t host_lgi_prepare_lod(void* node, int32_t flags, float a3, float a4);
void host_lgi_set_lod_ready_bit(void* node, uint8_t ready);
}  // namespace

// PE ResNode_tryUnload @ 0x0053EC20 (ResNode_vtbl+0x1C) size 0x37B.
// Callers: GCSweep@53819B(a2=0); ResNode_dtor@53E72C(a2=1); vtbl.
// W34 soft: protect gate + SimObjectList children recurse + lod-ready tail
//   (setLodReadyBit(0) / --g_InstanceNodeCreateCount / clear +0x54&0x200).
// W35-03: TouchResNode@537D10 soft at protect @53EC55 + child-block @53ECAD.
// W37: GT Update soft in mid_UnloadUpdate; mid+0x4C create store.
// OOS: sub_545CB0 prologue; GT Destructor vtbl+0x10 @ 0x429AC0 via tryUnload
//   @ 0x53ED24 (getPayload / Link/Unlink / unregister / child dtor / free);
//   ResHandle Link/Unlink; child vtbl dtor@53EEB9; Engine_free.
static int res_node_try_unload(HostResNode* node, int a2) {
  if (!node) return 0;
  auto* p = reinterpret_cast<uint8_t*>(node);
  auto* flags54 = reinterpret_cast<uint32_t*>(p + 0x54);
  // @ 0x53EC30..0x53EC65: !(a2&1) && (+0x54&0x200) → Touch + return 1.
  if ((a2 & 1) == 0 && (*flags54 & 0x200u) != 0) {
    // @ 0x53EC4C..0x53EC55: stamp+0x74 ≠ frame → TouchResNode (keep protect).
    if (node_gc_stamp(node) != g_res_gc_frame)
      resource_engine_touch_res_node(node);
    return 1;
  }
  // Children @ +0x18 SimObjectListEmpty@429390; head = *[+0x20] @ 0x53EC78.
  // Host: +0x20==0 → empty (PE Node base always wires &+0x30).
  int blocked = 0;  // ebx @ 0x53EC6D
  void* head = *reinterpret_cast<void**>(p + 0x20);
  if (head &&
      *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(head) + 4) !=
          nullptr) {
    auto* cur = reinterpret_cast<HostResNode*>(head);
    while (cur) {
      if (cur->type == 1) {  // +0x4C @ 0x53EC83
        if (res_node_try_unload(cur, a2 | 2) != 0) {  // vtbl+0x1C @ 0x53EC94
          // @ 0x53ECA4..0x53ECAD: parent stamp≠frame → TouchResNode.
          if (node_gc_stamp(node) != g_res_gc_frame)
            resource_engine_touch_res_node(node);
          ++blocked;  // @ 0x53ECB2
        }
      }
      auto* next =
          *reinterpret_cast<HostResNode**>(reinterpret_cast<char*>(cur) + 4);
      if (!next ||
          *reinterpret_cast<void**>(reinterpret_cast<char*>(next) + 4) ==
              nullptr)
        next = nullptr;  // @ 0x53ECB6..0x53ECC1
      cur = next;
    }
    if (blocked != 0) return 1;  // @ 0x53ECC7
  }
  // @ 0x53ECD9: !(+0x64&1) → return 0 (skip unload body).
  if ((node_gc_flags(node) & 1u) == 0) return 0;

  // GT Destructor path @ 0x53ECE3..0x53EF71 OOS (getPayload / vtbl+0x10
  //   GameTypeCtor_destroyInstance@429AC0 / Unlink / child dtor / Engine_free).
  // W37 does NOT invent this slice — needs RH@+0x11C + inst@+0x124 live.
  host_lgi_set_lod_ready_bit(node, 0);  // @ 0x53EF75
  if (g_lgi_create_count > 0) --g_lgi_create_count;  // @ 0x53EF7A
  if ((*flags54 & 0x200u) != 0)                      // @ 0x53EF80
    *flags54 &= ~0x200u;                             // @ 0x53EF8D
  return 0;                                          // @ 0x53EF92
}

// PE GameTypeCtor_vtbl14_callUpdate @ 0x429BC0 (GameTypeCtor_vtbl+0x14).
// stdcall(block, float) — ctor this unused on base. owner=*(block+0xC);
// type!=1 → setLodCopy6C(0); PrepareLod(0xA0000000); getEmbeddedMid;
// mid+0x50≠0 → CallNamedMethod("update",0,79) OOS (no va host in Resources).
static int host_gametype_vtbl14_call_update(void* block, float /*a2*/) {
  if (!block) return 0;
  auto* bb = reinterpret_cast<uint8_t*>(block);
  void* owner = *reinterpret_cast<void**>(bb + 0xC);  // @ 0x429BC5
  if (!owner) return 0;
  auto* ob = reinterpret_cast<uint8_t*>(owner);
  if (*reinterpret_cast<int32_t*>(ob + 0x4C) != 1)     // @ 0x429BCC
    res_node_set_lod_copy_6c(owner, 0.f);              // @ 0x429BD8
  constexpr int32_t kTagA0 = static_cast<int32_t>(0xA0000000u);
  if (host_lgi_prepare_lod(owner, kTagA0, 0.f, 0.f) < 0)  // @ 0x429BE6
    return static_cast<int32_t>(0x80000001u);
  // PE vtbl+0xC → getEmbeddedMid = InstanceNode+0xD8 (@ 0x429BFB).
  uint8_t* emb = ob + 0xD8;
  void* script = *reinterpret_cast<void**>(emb + 0x50);  // @ 0x429C02
  if (!script) return 0;
  // CallNamedMethod(emb, "update", 0, 79) @ 0x429C13 OOS.
  (void)script;
  return 0;
}

// PE GameTypeCtor_vtbl18_UpdateRangeNull @ 0x429F60 — xor eax,eax; retn 10h.
static int host_gametype_vtbl18_update_range_null(void* /*gt*/, void* /*block*/,
                                                  float* /*lo*/, float* /*hi*/,
                                                  float /*a2*/) {
  return 0;
}

// PE ResNode_mid_gameTypeUnloadUpdate @ 0x0053E620 size ~0xB0.
// this = embedded mid (wantUnloadFrame: node+0xD8); a2 = *(node+0x70).
// W31C soft: mid+0x44 nested → type!=1 setLodCopy6C(1.0) + PrepareLod(0,0,0);
//   getEmbeddedMid ≡ &nested->mid. W35-03: PrepareLod now Touches nested.
// W37: GameTypeCtor vtbl+0x14/@429BC0 soft (PrepareLod gate; CallNamedMethod
//   update OOS) + vtbl+0x18/@429F60 null. Gate: only when mid.block is a
//   GameTypeCtor_create inst (engine_load_game_init_native(owner)==block) —
//   avoids HostResNode attach leaf/block false positives. Specialized GT
//   vtbl overrides beyond base OOS.
static void res_node_mid_game_type_unload_update(HostMidPayload* mid,
                                                 float a2) {
  if (!mid) return;
  auto* nested = static_cast<HostResNode*>(mid->nested);  // +0x44 @ 0x53E625
  if (!nested) return;                                    // @ 0x53E62A
  if (nested->type != 1)                                  // +0x4C @ 0x53E634
    res_node_set_lod_copy_6c(nested, 1.0f);               // vtbl+0x14 @ 0x53E63F
  if (host_lgi_prepare_lod(nested, 0, 0.f, 0.f) < 0) return;  // @ 0x53E654
  // PE vtbl+0xC(1.0) → getEmbeddedMid → nested+0xD8 (host: &mid).
  HostMidPayload* nest_mid = &nested->mid;
  void* gt = nest_mid->leaf;  // *(mid+0xC) @ 0x53E668
  if (!gt) {
    // PE *(mid+0x10)==0 → sub_5512D0("unload: Undefined GameType Update…")
    (void)nest_mid->script_class;
    return;
  }
  void* block = mid->block;  // this+0x4C @ 0x53E66F
  if (!block) return;
  // @ 0x53E67E: GameTypeCtor vtbl+0x14(gt, block, a2). Soft base @ 0x429BC0
  // when block is the create inst for *(block+0xC); else skip (no vtbl*).
  void* owner =
      *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(block) + 0xC);
  if (owner && engine_load_game_init_native(owner) == block)
    (void)host_gametype_vtbl14_call_update(block, a2);
  auto* mb = reinterpret_cast<uint8_t*>(mid);
  const float lo = *reinterpret_cast<float*>(mb + 0x74);  // @ 0x53E68A
  const float hi = *reinterpret_cast<float*>(mb + 0x78);  // @ 0x53E694
  if (lo > a2 || hi < a2) {
    // @ 0x53E6AF: GameTypeCtor vtbl+0x18 — base @ 0x429F60 always 0.
    (void)host_gametype_vtbl18_update_range_null(gt, block,
                                                 const_cast<float*>(&lo),
                                                 const_cast<float*>(&hi), a2);
  }
}

// PE ResNode_wantUnloadFrame @ 0x0053EFB0 (ResNode_vtbl+0x2C) size 0x35.
// Sole .rdata site (InstanceNode_ctor installs ResNode_vtbl). Args a2/a3
// ignored — PumpUnload passes (owner+0x6C, &SimList); tryRelink cancel
// passes (scale, 0). Body: if +0x78==g_Engine_frameStamp@6200A4 → 0;
// else ResNode_mid_gameTypeUnloadUpdate@53E620(this+0xD8, *(float*)(+0x70));
// stamp +0x78; return 1. Does NOT push SimList (drain stays empty).
static int res_node_want_unload_frame(HostResNode* node) {
  if (!node) return 1;
  if (node_unload_frame(node) == g_res_gc_frame) return 0;  // @ 0x53EFBC
  const float scale =
      *reinterpret_cast<float*>(reinterpret_cast<char*>(node) + 0x70);
  res_node_mid_game_type_unload_update(&node->mid, scale);  // @ 0x53EFCE
  node_unload_frame(node) = g_res_gc_frame;                 // @ 0x53EFD9
  return 1;                                                 // @ 0x53EFDC
}

// PE ResNode_tryRelinkLod @ 0x0053F090 (ResNode_vtbl+0x10) size 0xA8.
// Called only via RelinkLodSlot's [eax+10h]. Args: scale, a3, a4.
// Host: HostResNode mid ≡ getEmbeddedMid (PE this+0xD8).
static int res_node_try_relink_lod(HostResNode* node, float scale, float a3,
                                   float a4) {
  if (!node) return 0;
  auto* flags = reinterpret_cast<int32_t*>(reinterpret_cast<char*>(node) + 0x54);
  if ((*flags & kRelinkLodCancel) != 0 || (a3 == 0.f && a4 == 0.f)) {
    // PE vtbl+0x2C(this, scale, 0) @ 0x53F12E → wantUnloadFrame soft.
    (void)res_node_want_unload_frame(node);
    return 0;
  }
  if ((*flags & kRelinkLodSlotted) != 0) return 0;

  // PE vtbl+0xC(scale) = ResNode_getEmbeddedMid @ 0x53EFA0 → this+0xD8.
  // HostResNode: getPayload returns &mid (host-only), not +0xD8.
  auto* mid = reinterpret_cast<uint8_t*>(&node->mid);
  const int32_t mid_flags = *reinterpret_cast<int32_t*>(mid + 0x0C);
  const float lo = *reinterpret_cast<float*>(mid + 0x74);
  const float hi = *reinterpret_cast<float*>(mid + 0x78);
  if ((mid_flags & 0x4000) == 0 && lo <= scale && hi >= scale) return 0;

  if (scale > 0.f) res_node_set_lod_copy_6c(node, scale);
  return 1;
}

// PE LodSlot (≥0x2C): dllist +4/+8; a3/a4 @+0xC/+0x10; ResHandle link @+0x18
// (prev/next/key/owner); flags @+0x28 (PumpUnloadQueue |=2).
struct HostLodSlot {
  uint32_t pad0 = 0;
  HostLodSlot* next = nullptr;       // +0x04
  HostLodSlot* prev = nullptr;       // +0x08
  float param_a3 = 0.f;              // +0x0C ← RelinkLodSlot a4
  float param_a4 = 0.f;              // +0x10 ← RelinkLodSlot a5
  uint32_t pad14 = 0;
  void* link_prev = nullptr;         // +0x18
  void* link_next = nullptr;         // +0x1C
  void* link_key = nullptr;          // +0x20 ← node+0x50
  HostResNode* link_owner = nullptr; // +0x24
  int32_t slot_flags = 0;            // +0x28
};
static_assert(offsetof(HostLodSlot, next) == 0x04, "lod+4");
static_assert(offsetof(HostLodSlot, param_a3) == 0x0C, "lod+0xC");
static_assert(offsetof(HostLodSlot, link_prev) == 0x18, "lod+0x18");
static_assert(offsetof(HostLodSlot, link_owner) == 0x24, "lod+0x24");
static_assert(offsetof(HostLodSlot, slot_flags) == 0x28, "lod+0x28");
static_assert(sizeof(HostLodSlot) == 0x2C, "lod==0x2C");

// PE eng freelist/active (g_ResourceEngine@618D48):
//   +0xDF8 active sentinel, +0xE00 active head (= sentinel.prev overlap),
//   +0xE08 freelist sentinel, +0xE0C freelist head (= sentinel.next overlap).
//   +0xDF0 active walk root ptr — sole .text readers @ PumpUnload 0x537B55/
//   0x537BE1; no .text write found → host keeps root=&active_sentinel
//   (PE layout implies DF0→&DF8 after eng ctor / BSS).
// Refill sole site: ResourceEngine_PumpUnloadQueue @ 0x537B40 (MainLoop /
// forceRendering / flush). No .text create immediates for slots.
struct HostLodEngLists {
  HostLodSlot freelist_sentinel{};
  HostLodSlot* freelist_head = nullptr;  // ≡ *eng+0xE0C
  HostLodSlot active_sentinel{};
  HostLodSlot* active_head = nullptr;    // ≡ *eng+0xE00
  HostLodSlot* active_walk_root = nullptr;  // ≡ *eng+0xDF0 → &DF8
  HostLodSlot pool[16]{};
  bool inited = false;
};

static HostLodEngLists g_lod_eng;

// Host seed ≡ cold freelist (PE: head→sentinel, [head+4]==0 → RelinkLodSlot
// skips). Runtime refill: PumpUnloadQueue freelist push @ 0x537C51..0x537C66.
static void lod_eng_ensure_inited() {
  if (g_lod_eng.inited) return;
  g_lod_eng.inited = true;
  g_lod_eng.freelist_sentinel.next = nullptr;
  g_lod_eng.freelist_sentinel.prev = nullptr;
  g_lod_eng.freelist_head = &g_lod_eng.freelist_sentinel;
  g_lod_eng.active_sentinel.next = nullptr;
  g_lod_eng.active_sentinel.prev = nullptr;
  g_lod_eng.active_head = &g_lod_eng.active_sentinel;
  // PE *eng+0xDF0 walk root (PumpUnload); host ≡ &sentinel @ DF8.
  g_lod_eng.active_walk_root = &g_lod_eng.active_sentinel;
  for (auto& s : g_lod_eng.pool) {
    HostLodSlot* old = g_lod_eng.freelist_head;
    old->prev = &s;
    s.next = old;
    s.prev = &g_lod_eng.freelist_sentinel;
    g_lod_eng.freelist_head = &s;
  }
}

// PE freelist pop @ 0x5377C4..0x5377F6: head=*E0C; empty iff [head+4]==0.
// Splice out; PE advances *E0C via [prev+4]=next when prev==&E08 (E0C overlaps
// sentinel.next). Host sets freelist_head explicitly after splice.
static HostLodSlot* lod_freelist_pop() {
  lod_eng_ensure_inited();
  HostLodSlot* head = g_lod_eng.freelist_head;
  if (!head || head == &g_lod_eng.freelist_sentinel) return nullptr;
  HostLodSlot* next = head->next;
  if (!next) return nullptr;  // PE jz @ 0x5377D1
  HostLodSlot* prev = head->prev;
  if (prev) {
    next->prev = prev;
    prev->next = next;
  }
  head->next = nullptr;
  head->prev = nullptr;
  if (next == &g_lod_eng.freelist_sentinel) {
    g_lod_eng.freelist_sentinel.next = nullptr;
    g_lod_eng.freelist_sentinel.prev = nullptr;
    g_lod_eng.freelist_head = &g_lod_eng.freelist_sentinel;
  } else {
    g_lod_eng.freelist_head = next;
  }
  return head;
}

static void lod_slot_link_to_node(HostLodSlot* slot, HostResNode* node);

// PE freelist push @ 0x537C51..0x537C66 (sole refill site, PumpUnloadQueue):
//   old=*E0C; old->prev=slot; slot->next=old; slot->prev=&E08; *E0C=slot.
static void lod_freelist_push(HostLodSlot* slot) {
  if (!slot) return;
  lod_eng_ensure_inited();
  HostLodSlot* old = g_lod_eng.freelist_head;
  old->prev = slot;
  slot->next = old;
  slot->prev = &g_lod_eng.freelist_sentinel;
  g_lod_eng.freelist_head = slot;
}

// PE active unlink @ 0x537C1E..0x537C35 then head fix via sentinel.prev≡*E00.
static void lod_active_unlink(HostLodSlot* slot) {
  if (!slot) return;
  HostLodSlot* next = slot->next;
  HostLodSlot* prev = slot->prev;
  if (prev && next) {
    next->prev = prev;  // PE [next+8]=prev @ 0x537C2C
    prev->next = next;  // PE [prev+4]=next @ 0x537C2F
  }
  slot->next = nullptr;
  slot->prev = nullptr;
  if (g_lod_eng.active_head == slot) {
    // PE *E00 overlaps sentinel.prev when next==&DF8; host mirrors.
    g_lod_eng.active_head =
        (prev && prev != &g_lod_eng.active_sentinel) ? prev
                                                    : &g_lod_eng.active_sentinel;
  }
}

// PE recycle one flagged slot @ 0x537C0D..0x537C66.
static void lod_active_recycle_flagged(HostLodSlot* slot) {
  if (!slot || (slot->slot_flags & 2) == 0) return;
  HostResNode* owner = slot->link_owner;
  if (owner) {
    auto* flags =
        reinterpret_cast<int32_t*>(reinterpret_cast<char*>(owner) + 0x54);
    *flags &= ~kRelinkLodSlotted;  // PE and ~0x4000000 @ 0x537C17
  }
  lod_active_unlink(slot);
  if (owner) {
    // PE ResHandle_Unlink(owner+0x44, slot+0x18) @ 0x537C43.
    lod_slot_link_to_node(slot, nullptr);
  } else {
    slot->link_key = nullptr;  // PE [slot+0x20]=0 @ 0x537C4A
  }
  lod_freelist_push(slot);
}

// Active walk: PE @ 0x537B55 loads *eng+0xDF0 (sole .text readers; host
// sets root=&active_sentinel ≡ &DF8). Empty iff [root+4]==0. Soft stop at
// sentinel identity (circular DF8) so the sentinel itself is never recycled.
static HostLodSlot* lod_active_walk_start() {
  lod_eng_ensure_inited();
  HostLodSlot* root = g_lod_eng.active_walk_root
                          ? g_lod_eng.active_walk_root
                          : &g_lod_eng.active_sentinel;
  // PE: [root+4]!=0 ? root : 0 — then first slot_cur=root. Host skips the
  // sentinel identity: start at root->next (first real slot).
  HostLodSlot* first = root->next;
  if (!first || first == &g_lod_eng.active_sentinel) return nullptr;
  return first;
}

static HostLodSlot* lod_active_walk_advance(HostLodSlot* cur) {
  if (!cur) return nullptr;
  HostLodSlot* n = cur->next;
  if (!n || n == &g_lod_eng.active_sentinel) return nullptr;
  return n;
}

// PE Engine_SimObjectList @ 0x429350 / Empty @ 0x429390 — stack collector in
// PumpUnloadQueue. empty when *(head+4)==0; head=*(this+2)=&this[4].
struct HostSimObjectList {
  void* vtbl = nullptr;       // +0 → off_5F09B0 after ctor
  void* pad04 = nullptr;      // +4 inner vtbl stand-in
  void* head = nullptr;       // +8 → &tail_sent
  void* pad0c = nullptr;      // +0xC
  void* tail_sent = nullptr;  // +0x10 sentinel
  void* pad14 = nullptr;      // +0x14
  void* tail = nullptr;       // +0x18 → &pad04
  void* cur = nullptr;        // host: drain cursor ≡ PE v20 (list node*)
};

static void host_sim_object_list_ctor(HostSimObjectList* L) {
  if (!L) return;
  // PE @ 0x429350: head=&[4], tail=&[1], empty *(head+4)==0.
  L->vtbl = nullptr;
  L->pad04 = nullptr;
  L->pad0c = nullptr;
  L->tail_sent = nullptr;
  L->pad14 = nullptr;
  L->head = &L->tail_sent;
  L->tail = &L->pad04;
  L->cur = nullptr;
}

static bool host_sim_object_list_empty(HostSimObjectList* L) {
  if (!L || !L->head) return true;
  // PE: *(*(this+2)+4)==0
  return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(L->head) + 4) ==
         nullptr;
}

// PE ResourceEngine_GCSweep @ 0x537ED0 size 0x48E — called PumpUnload
// @ 0x537C77 with a2=0, and flush@47C319 with a2!=0.
// W28C: clear counters @ 765F10/14/18.
// W29C soft type-buckets: i=1..0x15 stride 0x90; touch walk @ +0x150;
//   age vs g_res_gc_frame (≡ g_Engine_frameStamp@6200A4); thresh a2?1:8;
//   soft insertIndexedSlot → aged @ +0x16C + flags|=0x20; ++touchCount.
//   g_GCSweep_typeGate@618DC8 (stock 1/5/6/7=1.0) gates scan @ +0x17C;
// W32C: force gates eng+0x106ED8..EE4 set/clear; pre-scan budget switch
//   Hi@618D4C/54/5C/64 + live type1=g_lgi_create_count / soft 5/6/7.
// W33C: touch vtbl+0x14→setLodCopy6C(0); scan=aged pool; evict move
//   +0x198/+0x1C0 + flags|=0x40; post-evict Lo@618D50.. + caps@618F80..;
// W34: ResNode_tryUnload@53EC20 soft (gate/children/lod-ready); GT dtor OOS.
uint32_t g_gcsweep_touch_count = 0;  // PE @ 0x765F10
uint32_t g_gcsweep_scan_count = 0;   // PE @ 0x765F14
uint32_t g_gcsweep_evict_count = 0;  // PE @ 0x765F18

// PE pre-scan budget @ 0x538007..0x53809E (a2==0 only). Returns true →
// skip scan (LABEL_74). Cases 1/5/6/7; default enter.
static bool gc_budget_skip_scan(int type) {
  switch (type) {
    case 7:  // @ 0x53800E
      if (g_gc_live_type7 < kGcBudgetType7Hi && g_gc_force_type7 == 0)
        return true;               // @ 0x538023 → LABEL_74
      g_gc_force_type7 ^= 1u;      // @ 0x538029
      break;
    case 5:  // @ 0x538032
      if (g_gc_live_type5 < kGcBudgetType5Hi && g_gc_force_type5 == 0)
        return true;               // @ 0x538048
      g_gc_force_type5 ^= 1u;      // @ 0x53804E
      break;
    case 1:  // @ 0x538057
      if (g_lgi_create_count < kGcBudgetType1Hi && g_gc_force_type1 == 0)
        return true;               // @ 0x53806C
      g_gc_force_type1 ^= 1u;      // @ 0x538072
      break;
    case 6:  // @ 0x53807B
      if (g_gc_live_type6 < kGcBudgetType6Hi && g_gc_force_type6 == 0)
        return true;               // @ 0x538091
      g_gc_force_type6 ^= 1u;      // @ 0x538097
      break;
    default:
      break;
  }
  return false;
}

// PE post-evict Lo/caps @ 0x5381CA..0x53828C (a2==0). true → LABEL_74.
static bool gc_budget_post_evict(int type, uint32_t& c7, uint32_t& c5,
                                 uint32_t& c1, uint32_t& c6) {
  switch (type) {
    case 7:  // @ 0x5381CA
      if (g_gc_live_type7 < kGcBudgetType7Lo) {
        g_gc_force_type7 = 0;  // @ 0x538302
        return true;
      }
      if (c7 >= kGcEvictCapType7) return true;  // @ 0x5381E8
      ++c7;                                    // @ 0x5381EF
      break;
    case 5:  // @ 0x5381F8
      if (g_gc_live_type5 < kGcBudgetType5Lo) {
        g_gc_force_type5 = 0;  // @ 0x53830A
        return true;
      }
      if (c5 >= kGcEvictCapType5) return true;  // @ 0x538219
      ++c5;                                    // @ 0x538222
      break;
    case 1:  // @ 0x53822B
      if (g_lgi_create_count < kGcBudgetType1Lo) {
        g_gc_force_type1 = 0;  // @ 0x538312
        return true;
      }
      if (c1 >= kGcEvictCapType1) return true;  // @ 0x53824C
      ++c1;                                    // @ 0x538255
      break;
    case 6:  // @ 0x53825E
      if (g_gc_live_type6 < kGcBudgetType6Lo) {
        g_gc_force_type6 = 0;  // @ 0x53831A
        return true;
      }
      if (c6 >= kGcEvictCapType6) return true;  // @ 0x538283
      ++c6;                                    // @ 0x53828C
      break;
    default:
      break;
  }
  return false;
}

int host_resource_engine_gcsweep(int a2) {
  g_gcsweep_touch_count = 0;  // @ 0x537EE1
  g_gcsweep_scan_count = 0;   // @ 0x537EE7
  g_gcsweep_evict_count = 0;  // @ 0x537EED
  // PE v25..v28 per-type evict caps @ 0x537EF3..0x537EFF.
  uint32_t evict_cap7 = 0;
  uint32_t evict_cap5 = 0;
  uint32_t evict_cap1 = 0;
  uint32_t evict_cap6 = 0;
  if (a2 != 0) {
    // PE eng+0x106EE0/EDC/ED8/EE4 = 1 @ 0x537F0A..0x537F1C.
    g_gc_force_type1 = 1;  // eng+0x106EE0
    g_gc_force_type5 = 1;  // eng+0x106EDC
    g_gc_force_type7 = 1;  // eng+0x106ED8
    g_gc_force_type6 = 1;  // eng+0x106EE4
  }
  const uint32_t age_thresh = (a2 != 0) ? 1u : 8u;  // @ 0x537F59..0x537F60
  const uint32_t scan_age_thresh =
      (a2 != 0) ? 1u : 32u;  // @ 0x5380FD..0x538100

  // Type loop i=1..0x15 @ 0x537F22..0x538320.
  for (int i = 1; i < kGcTypeCount; ++i) {
    gc_bucket_ensure(i);
    HostGcTypeBucket& b = g_gc_buckets[i];

    // Touch pass @ 0x537F38..0x537FCF — list head @ bucket+0x150.
    for (HostResNode* cur = gc_list_walk_start(b.touch_head); cur;) {
      HostResNode* next = gc_list_walk_advance(cur);
      const uint32_t age = g_res_gc_frame - node_gc_stamp(cur);  // @ 0x537F73
      if (age > age_thresh) {
        // PE ResNode_vtbl+0x14(0) @ 0x537F86 = setLodCopy6C@53EFF0.
        res_node_set_lod_copy_6c(cur, 0.f);
        if ((node_gc_flags(cur) & kGcFlagAged) == 0) {  // @ 0x537F89
          // unlinkIndexedSlot(1) @ 0x537F93 + insertIndexedSlot @ 0x537FAB
          // → soft aged list (eng+i*0x90+0x16C); flags |= 0x20.
          gc_bucket_age_insert(cur, i);
        }
      }
      ++g_gcsweep_touch_count;  // @ 0x537FBC
      cur = next;
    }

    // Scan/evict gated by g_GCSweep_typeGate[i] @ 0x537FD7 (≠0.0).
    if (kGcTypeGate[i] == 0.f) continue;  // @ 0x537FE9 → next type
    if (a2 == 0 && gc_budget_skip_scan(i)) continue;  // @ 0x537FF5..53809E

    // Scan pool ≡ aged @ +0x16C (PE head view @ +0x17C) @ 0x53809E..
    bool stop_type = false;
    bool restart = true;
    while (restart && !stop_type) {
      restart = false;
      for (HostResNode* cur = gc_list_walk_start(b.aged_head); cur;) {
        // PE advances next before body @ 0x5380B7 (safe across unlink).
        HostResNode* next = gc_list_walk_advance(cur);
        ++g_gcsweep_scan_count;  // @ 0x5380CE
        if (gc_node_has_scan_payload(cur)) {
          const uint32_t age = g_res_gc_frame - node_gc_stamp(cur);
          if (age > scan_age_thresh) {  // @ 0x538105
            auto* flags =
                reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(cur) + 0x54);
            const uint32_t f = *flags;
            // @ 0x53810E..0x538138: !0x2000000, typeGate/aux, !0x200.
            const int tidx = (cur->type >= 0 && cur->type < kGcTypeCount)
                                 ? cur->type
                                 : 0;
            if ((f & 0x2000000u) == 0 &&
                (kGcTypeEvictGate[tidx] == 0 || node_gc_aux_d0(cur) == 0) &&
                (f & 0x200u) == 0) {
              ++g_gcsweep_evict_count;  // @ 0x53813E
              if ((node_gc_flags(cur) & kGcFlagEvict) == 0)  // @ 0x538144
                gc_bucket_evict_insert(cur, i);  // +0x198/+0x1C0 @ 0x53814A
              // PE vtbl+0x1C(0) = ResNode_tryUnload@53EC20 @ 0x53819B.
              (void)res_node_try_unload(cur, 0);
              if (a2 == 0 &&
                  gc_budget_post_evict(i, evict_cap7, evict_cap5, evict_cap1,
                                      evict_cap6)) {
                stop_type = true;  // LABEL_74
                break;
              }
              // PE reloads scan head after tryUnload @ 0x5381A2.
              restart = true;
              break;
            }
          }
        } else if (node_gc_stamp(cur) != g_res_gc_frame) {  // @ 0x538292
          node_gc_stamp(cur) = g_res_gc_frame;              // @ 0x53829C
          if ((node_gc_flags(cur) & kGcFlagTouch) == 0) {   // @ 0x53829F
            // PE move scan→touch @ +0x140/+0x168; flags|=0x10.
            gc_node_dll_unlink(cur);
            gc_list_push_head(b.touch_head, &b.touch_sent, cur);
            b.scan_head = b.aged_head;
            node_gc_flags(cur) =
                (node_gc_flags(cur) & ~0x60u) | kGcFlagTouch;  // @ 0x5382ED
          }
        }
        cur = next;
      }
    }
  }

  if (a2 != 0) {
    // PE clears eng+0x106EE0/EDC/ED8/EE4 @ 0x53833C..0x53834E.
    g_gc_force_type1 = 0;
    g_gc_force_type5 = 0;
    g_gc_force_type7 = 0;
    g_gc_force_type6 = 0;
  }
  return 0;  // @ 0x538354
}

// PE ResourceEngine_PumpUnloadQueue @ 0x537B40 size 0x189.
// Callers: Engine_MainLoop@428CD4, forceRendering@47C1E7, flush@47C2E7.
// W18B: mark+recycle freelist eng+0xE0C.
// W28C: SimObjectList ctor/empty shell + GCSweep(0) soft counters.
// W29C: GCSweep type-bucket touch/age + typeGate scan shell.
// W30C: mark via ResNode_wantUnloadFrame@53EFB0 (vtbl+0x2C stamp +0x78);
//   drain vtbl+0x30=ResNode_vtbl30_null@545220 (base never pushes SimList).
// W31C: wantUnload → mid_gameTypeUnloadUpdate@53E620 PrepareLod soft
//   (nested mid+0x44); GameType vtbl+0x14/+0x18 OOS.
// W32C: GCSweep force gates eng+0x106ED8..EE4 + pre-scan budget Hi.
// W33C: GCSweep touch setLodCopy6C(0); aged-scan evict move + Lo/caps soft.
// W34: ResNode_tryUnload@53EC20 soft gate/children/lod-ready tail.
// W35-03: TouchResNode@537D10 soft (tryUnload protect/child + PrepareLod +
//   PumpLoad complete). W37: GT Update@429BC0/429F60 soft + loadrate@618D6C.
// Still OOS: GT Destructor@429AC0 tryUnload body; CallNamedMethod("update");
//   dllist teardown; Engine_free buffers; child vtbl dtor.
int resource_engine_pump_unload_queue() {
  lod_eng_ensure_inited();
  ++g_res_gc_frame;  // soft ≡ MainLoop ++g_Engine_frameStamp@6200A4

  HostSimObjectList sim{};
  host_sim_object_list_ctor(&sim);  // @ 0x537B50

  // Mark pass ≡ 0x537B72..0x537BAB.
  for (HostLodSlot* cur = lod_active_walk_start(); cur;
       cur = lod_active_walk_advance(cur)) {
    HostResNode* owner = cur->link_owner;  // PE [slot+0x24] @ 0x537B82
    if (!owner) {
      cur->slot_flags |= 2;  // PE null-owner @ 0x537B85 → 0x537BA3
    } else if (res_node_want_unload_frame(owner) != 0) {
      // PE vtbl+0x2C(owner, owner+0x6C, &sim) @ 0x537B9C; !=0 → mark.
      // Base ignores &sim (no collector push on ResNode_vtbl).
      (void)sim;
      cur->slot_flags |= 2;
    }
  }

  // SimList drain @ 0x537BB3..0x537BDD — empty (wantUnloadFrame never pushes).
  while (!host_sim_object_list_empty(&sim) && sim.cur != nullptr) {
    // PE: obj=cur[+0x18]; ResNode_vtbl30_null@545220; +0x54&=~0x20000000;
    // (*cur)(1) list-node dtor.
    void* obj = *reinterpret_cast<void**>(reinterpret_cast<char*>(sim.cur) +
                                          0x18);
    if (obj) {
      auto* f = reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(obj) + 0x54);
      *f &= ~0x20000000u;  // @ 0x537BD0
    }
    sim.cur = nullptr;
    break;
  }

  // Recycle pass ≡ 0x537BF4..0x537C70 — advance before mutate.
  for (HostLodSlot* cur = lod_active_walk_start(); cur;) {
    HostLodSlot* next = lod_active_walk_advance(cur);
    if ((cur->slot_flags & 2) != 0) lod_active_recycle_flagged(cur);
    cur = next;
  }

  (void)host_resource_engine_gcsweep(0);  // @ 0x537C77 a2=0

  // Teardown @ 0x537C85..0x537CBB — Empty → skip unlink loop.
  while (!host_sim_object_list_empty(&sim) && sim.cur != nullptr) {
    sim.cur = nullptr;
    break;
  }
  return 0;  // PE returns 0 @ 0x537CC0
}

// PE slot+0x18 → node+0x48 dllist @ 0x5377F9..0x537874 (ResHandle shape).
static void lod_slot_link_to_node(HostLodSlot* slot, HostResNode* node) {
  auto* link = reinterpret_cast<void**>(reinterpret_cast<char*>(slot) + 0x18);
  HostResNode* old_owner = slot->link_owner;
  if (old_owner != node) {
    if (old_owner) {
      // Unlink from old owner (PE 0x537805..0x53782C).
      void* lp = link[0];
      void* ln = link[1];
      if (lp)
        *reinterpret_cast<void**>(reinterpret_cast<char*>(lp) + 4) = ln;
      else
        *reinterpret_cast<void**>(reinterpret_cast<char*>(old_owner) + 0x48) =
            ln;
      if (ln) *reinterpret_cast<void**>(ln) = lp;
      link[0] = nullptr;
      link[1] = nullptr;
      link[2] = nullptr;
      slot->link_owner = nullptr;
    }
    if (node) {
      auto* node_u8 = reinterpret_cast<uint8_t*>(node);
      void** head = reinterpret_cast<void**>(node_u8 + 0x48);
      void* cur = *head;
      if (cur) *reinterpret_cast<void**>(cur) = link;
      link[0] = nullptr;
      link[1] = cur;
      *head = link;
      slot->link_owner = node;
      link[2] = *reinterpret_cast<void**>(node_u8 + 0x50);
    } else {
      slot->link_owner = nullptr;
      link[0] = link[1] = link[2] = nullptr;
    }
  }
}

// PE push active @ 0x537889..0x53789E: [E00+4]=slot; slot.next=&DF8;
// slot.prev=*E00; *E00=slot.
static void lod_active_push(HostLodSlot* slot) {
  HostLodSlot* old = g_lod_eng.active_head;
  old->next = slot;
  slot->next = &g_lod_eng.active_sentinel;
  slot->prev = old;
  g_lod_eng.active_head = slot;
}

// PE ResNode_relinkLodSlot @ 0x00537790 — ResourceEngine thiscall
// (g_ResourceEngine@618D48, node, scale, a3, a4). Size 0x11B.
// 1) tryRelinkLod (vtbl+0x10) @ 0x5377AC.
// 2) If ok && !(node+0x54 & 0x4000000): pop freelist eng+0xE0C,
//    unlink/relink slot+0x18 into node+0x48, store a3/a4 at slot+0xC/+0x10,
//    OR 0x4000000, push active list eng+0xE00 / sentinel eng+0xDF8.
// W17A: freelist pop + dllist + OR hosted; W18B: PumpUnloadQueue recycle
// refill @ 0x537C51 replaces seed-only stand-in.
static void res_node_relink_lod_slot(HostResNode* node, float scale, float a3,
                                     float a4) {
  if (!node) return;
  const int ok = res_node_try_relink_lod(node, scale, a3, a4);
  if (ok == 0) return;
  auto* flags = reinterpret_cast<int32_t*>(reinterpret_cast<char*>(node) + 0x54);
  if ((*flags & kRelinkLodSlotted) != 0) return;

  HostLodSlot* slot = lod_freelist_pop();
  if (!slot) return;  // PE [head+4]==0 → skip

  lod_slot_link_to_node(slot, node);
  slot->param_a3 = a3;  // PE [ecx+0Ch]=a4 @ 0x53787C
  slot->param_a4 = a4;  // PE [ecx+10h]=a5 @ 0x53787F
  *flags |= kRelinkLodSlotted;  // PE or [edi+54h],4000000h @ 0x537882
  lod_active_push(slot);
}

// PE ResourceEngine_PumpLoadQueue @ 0x5378D0 size 0x270.
// Callers: Engine_MainLoop@428CC9, forceRendering@47C1DC, flush@47C2DC.
// Layout (IDA _DWORD* this → byte offs via int_convert):
//   eng+0xDB8  load-queue head (walk start; [head+4]==0 → null)
//   eng+0xDD0  freelist sentinel; eng+0xDD4 freelist head (recycle @ 0x537A65)
//   eng+0x106ED4 isLoading (System.isLoading @ 0x47C3B0 sole reader)
// Entry (esi): +4/+8 dllist; +0x18 ResHandle; +0x24=RH.owner≡ResNode*;
//   +0x28 flags; +0x2C; +0x30 buf; +0x34 cleared on recycle.
// W19A: empty-queue + LABEL_37 LoadRing / isLoading / type7 peak.
// W20B: LABEL_28 recycle @ 0x537A13..0x537A7F —
//   FileAsync_CancelByUserdata@5556F0 stub (Active==0 → -1; ret ignored),
//   ResHandle_Relink(entry+0x18,0)@545FC0 unlink-only (a2==0 skips Lookup),
//   freelist push eng+0xDD0/0xDD4 @ 0x537A65..0x537A7F, flags&=0x7FFFFFFE.
// W21A: vtbl+0x28 ResNode_GameType_vtbl28_LoadLod@53E860 LABEL_68 slice
//   (lod-ready + +0x124==0 + +0x118==0 → +0x68=1.0); pump clear +0x54
//   &= ~0x2000000; buf==0 → LABEL_28.
// W22A: parseLodBuf@53DEE0 prologue+empty-tail; LoadLod cold wires it.
// W23A: parseLodBuf body — cursor/BE32 children size-skip + PHYS/POLY mid
//   decode; RSD/text/CreateNodeUnder still OOS.
// W24A: EXTP PrefetchExtPackSlot@544370 dword loop + expand AABB half
//   (node+0x84/+0x90, state=1 @ +0x80).
// W25C: GetPackSlot@5383F0 + EnsureIndex@544170 early/rpak bridge; Prefetch
//   wires GetPackSlot (no longer discards pack_id when pack_slot+table).
// W26C: CreateNodeUnder@536900 shell + ParseChildRecord wire (RemapLocalId
//   FFFF/hi0; CreateNodeByDesc type1+TREE); children no longer size-skip only.
// W27C: parseLodBuf LABEL_19 text/RSD — Util_ReadLine + gametype|params +
//   RemapLocalId + ResHandle_Bind(mid+0x38,type8) via LookupById.
// W28C: PumpUnload SimList+GCSweep soft; Bind→LookupById@536820 extracted.
// W29C: GCSweep type-bucket touch@+0x150 / age→+0x16C / typeGate scan shell.
// W30C: wantUnloadFrame@53EFB0 stamp +0x78; drain vtbl+0x30 null (no push).
// W31C: mid_gameTypeUnloadUpdate@53E620 PrepareLod soft (mid+0x44 nested).
// W32C: GCSweep force eng+0x106ED8..EE4 + budget Hi@618D4C.. (pre-scan).
// W33C: touch setLodCopy6C(0); aged-scan evict + Lo/caps.
// W34: tryUnload@53EC20 soft (gate/children/lod-ready); GT dtor OOS.
// W35-03: TouchResNode@537D10 soft (stamp/+0x10 touch-bucket relink).
// W36: ResourceEngine_RemapFromPath@538440 Soft + cloneHdr lazy Bind
//   (sl.rpk/0x6E type6; particles.rpk/0x106 type0xE ×2).
// W37: loadrate@618D6C=16 soft break; GT Update vtbl+0x14/@429BC0 PrepareLod
//   gate + vtbl+0x18/@429F60 null; mid+0x4C store after create @53A8C9.
// Still OOS: CreateNodeByDesc cases≠1; RemapLocalId TOC@+0x5C File_SlotRead;
//   Bind Resolve miss; LoadPack@538380; producers into eng+0xDB8;
//   GT Destructor vtbl+0x10@429AC0 (tryUnload getPayload/Link/Unlink/free);
//   CallNamedMethod("update") inside vtbl+0x14; specialized GT vtbl overrides;
//   ApplyConfigGfx write to loadrate.
namespace {
constexpr int kLoadRingSlots = 32;  // PE & 0x1F wrap @ 0x537AC8
uint32_t g_load_ring_slots[kLoadRingSlots]{};  // PE @ 0x765E8C
uint32_t g_load_ring_index = 0;                // PE @ 0x765F20
uint32_t g_load_ring_sum = 0;                  // PE @ 0x765F24
uint32_t g_pump_load_type7 = 0;                // PE @ 0x765F30
uint32_t g_pump_load_type7_peak = 0;           // PE @ 0x765F34
int32_t g_re_is_loading = 0;                   // PE eng+0x106ED4
// PE Engine_resource_loadrate @ 0x618D6C — IDA default dword=16; PumpLoad
// breaks when loaded_n >= rate (@ 0x537937). ApplyConfigGfx@427BF0 may write.
uint32_t g_engine_resource_loadrate = 16;
// g_lgi_create_count — PE @ 0x765F60; defined with GCSweep soft lives (W32C).

// PE FourCC tags in parseLodBuf chunk loop @ 0x53E0BA..0x53E404.
constexpr uint32_t kLodChunkPhys = 0x53594850u;  // 'PHYS'
constexpr uint32_t kLodChunkPoly = 0x594C4F50u;  // 'POLY'
constexpr uint32_t kLodChunkExtp = 0x50545845u;  // 'EXTP'
constexpr uint32_t kLodChunkRsd = 0x00445352u;   // 'RSD\0' imm @ 0x53E0CB

// PE pack region (g_ResourceEngine@618D48): +0xFFE20 count, +0xFFE24 FFFF
// slot, +0xFFE94 indexed slots stride 0x70. Host logical stand-in (no 1MB
// eng blob). Init@535EF4: ResourcePack_InitSlot(FFFF, unk_765F38); count=0.
constexpr size_t kPackSlotStride = 0x70;
constexpr size_t kMaxPackSlots = 64;  // host cap (PE grows via LoadPack)
struct HostPackSlot {
  uint8_t raw[kPackSlotStride]{};
};
static_assert(sizeof(HostPackSlot) == 0x70, "pack slot stride 0x70");
struct HostPackEng {
  uint32_t pack_count = 0;  // ≡ eng+0xFFE20
  HostPackSlot ffff{};      // ≡ eng+0xFFE24
  HostPackSlot slots[kMaxPackSlots]{};
  bool ffff_inited = false;
};
HostPackEng g_pack_eng;

// PE ResourcePack_InitSlot @ 0x543EB0 — +0x50=0, +0x68=0, +0x6C=0xFFFF,
// +8=packIdx, strncpy(+0x0C, path, 64).
void host_resource_pack_init_slot(void* slot, uint32_t pack_idx,
                                  const char* path) {
  if (!slot) return;
  auto* p = reinterpret_cast<uint8_t*>(slot);
  *reinterpret_cast<uint32_t*>(p + 0x50) = 0;
  *reinterpret_cast<uint32_t*>(p + 0x68) = 0;
  *reinterpret_cast<uint32_t*>(p + 0x6C) = 0xFFFFu;
  *reinterpret_cast<uint32_t*>(p + 8) = pack_idx;
  char* dest = reinterpret_cast<char*>(p + 0x0C);
  dest[0] = '\0';
  if (path && path[0]) {
    std::strncpy(dest, path, 63);
    dest[63] = '\0';
  }
}

void host_pack_eng_ensure_ffff() {
  if (g_pack_eng.ffff_inited) return;
  g_pack_eng.ffff_inited = true;
  // PE Init@535EE7: InitSlot(eng+0xFFE24, 0xFFFF, unk_765F38).
  host_resource_pack_init_slot(g_pack_eng.ffff.raw, 0xFFFFu, nullptr);
  g_pack_eng.pack_count = 0;  // @ 0x535EF9
}

// Sync pack_count so GetPackSlot(hi) can see openLib/rpak ids (1-based).
void host_pack_eng_sync_from_rpak() {
  host_pack_eng_ensure_ffff();
  const size_t n = rpak_count();
  uint32_t need = static_cast<uint32_t>(n) + 1u;  // hi==n < count
  if (need > kMaxPackSlots) need = static_cast<uint32_t>(kMaxPackSlots);
  if (g_pack_eng.pack_count < need) g_pack_eng.pack_count = need;
}

// PE ResourcePack_EnsureIndex @ 0x544170 — +0x50 state 0/1/2.
// Early: state!=0 → return. Cold: PathExists(+0x0C); else return 0.
// Host bridge: already-open rpak (openLib ≈ LoadPack+EnsureIndex) → state=2.
// OOS: File_SlotRead RPAK TOC → +0x5C table (RemapLocalId ext-pack path).
int host_resource_pack_ensure_index(void* slot) {
  if (!slot) return 0;
  auto* p = reinterpret_cast<uint8_t*>(slot);
  int state = *reinterpret_cast<int*>(p + 0x50);  // @ 0x544176
  if (state != 0) return state;                   // @ 0x54417C

  // Derive PE pack index from slot pointer (eng+0xFFE94 + 0x70*idx).
  uint32_t pe_idx = *reinterpret_cast<uint32_t*>(p + 8);
  const auto* base = g_pack_eng.slots[0].raw;
  if (p >= base && p < base + kMaxPackSlots * kPackSlotStride) {
    const size_t off = static_cast<size_t>(p - base);
    if ((off % kPackSlotStride) == 0)
      pe_idx = static_cast<uint32_t>(off / kPackSlotStride);
  }

  // Host: rpak pack_id matches openLib HIWORD (1-based).
  if (pe_idx != 0 && pe_idx != 0xFFFFu) {
    if (const RpakPack* rp = rpak_get(static_cast<int32_t>(pe_idx))) {
      if (rp->parsed_entries || rp->is_registry) {
        *reinterpret_cast<uint32_t*>(p + 8) = pe_idx;
        *reinterpret_cast<int*>(p + 0x50) = 2;  // @ 0x544366
        return 2;
      }
    }
  }

  const char* path = reinterpret_cast<const char*>(p + 0x0C);
  if (path[0] != '\0') {
    if (const RpakPack* by = rpak_find_by_name(path)) {
      if (by->parsed_entries || by->is_registry) {
        *reinterpret_cast<int*>(p + 0x50) = 2;
        return 2;
      }
    }
    // File_PathExists@54C170 + RPAK TOC body @ 0x54419B..0x544366 OOS.
  }
  return *reinterpret_cast<int*>(p + 0x50);  // @ 0x544192 — still 0
}

// PE ResourceEngine_GetPackSlot @ 0x5383F0 size 0x49 —
// __fastcall(eng, edx_unused, id). HIWORD FFFF → eng+0xFFE24 (no EnsureIndex);
// else if hi >= count → 0; else EnsureIndex(slot) ? slot : 0.
void* host_resource_engine_get_pack_slot(uint32_t id) {
  host_pack_eng_sync_from_rpak();
  const uint32_t hi = id >> 16;  // @ 0x5383F4
  if (hi == 0xFFFFu) return g_pack_eng.ffff.raw;  // @ 0x538430
  if (hi >= g_pack_eng.pack_count) return nullptr;  // @ 0x538404
  if (hi >= kMaxPackSlots) return nullptr;
  void* slot = g_pack_eng.slots[hi].raw;
  // PE: EnsureIndex!=0 ? slot : 0  (@ 0x53841C..0x538425)
  if (host_resource_pack_ensure_index(slot) != 0) return slot;
  return nullptr;
}

// PE ReadBE32 @ 0x559690 via thunk @ 0x54F680 — byte assemble (LE load).
uint32_t host_read_be32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// PE ResPack_ParseChildRecord @ 0x544010 — size from +22 namelen + opt 12f
// if +9&1. W26C: CreateNodeUnder@536900 wired (was size-skip only).
int host_lgi_child_record_size(const uint8_t* rec) {
  const unsigned name_len = rec[22];
  const uint8_t* end = rec + 23 + name_len;
  if ((rec[9] & 1) != 0) end += 12 * sizeof(float);
  return static_cast<int>(end - rec);
}

// PE ResPack_RemapLocalId @ 0x544590 — this=pack_slot.
// packIdx(+8)==FFFF → identity; lo==0 → 0; hi>=count → 0; hi==0 →
// lo|(packIdx<<16); hi!=0 → TOC table@+0x5C (Soft: rpak_remap_toc).
uint32_t host_res_pack_remap_local_id(void* pack_slot, uint32_t local_id) {
  if (!pack_slot) return 0;
  auto* p = reinterpret_cast<uint8_t*>(pack_slot);
  const uint32_t pack_idx = *reinterpret_cast<uint32_t*>(p + 8);  // this+2
  if (pack_idx == 0xFFFFu) return local_id;                       // @ 0x54459D
  const uint32_t lo = local_id & 0xFFFFu;                         // @ 0x5445A5
  const uint32_t hi = local_id >> 16;                             // @ 0x5445AB
  if (lo == 0) return 0;                                          // @ 0x5445B0
  host_pack_eng_sync_from_rpak();
  if (hi >= g_pack_eng.pack_count) return 0;                      // @ 0x5445C6
  if (hi != 0) {
    // Ext-pack remap @ 0x5445CA..0x5445EF. Soft: prefer raw +0x5C if
    // wired; else rpak_remap_local_id Soft TOC (EnsureIndex stride 66).
    const uint32_t lim = *reinterpret_cast<uint32_t*>(p + 0x54) +
                         *reinterpret_cast<uint32_t*>(p + 0x58);
    auto* table = *reinterpret_cast<uint8_t**>(p + 0x5C);  // this+23
    if (table && hi <= lim) {
      const uint32_t ext_hi =
          *reinterpret_cast<const uint16_t*>(table + 66 * hi + 2);
      return lo | (ext_hi << 16);
    }
    if (pack_idx != 0 && pack_idx != 0xFFFFu)
      return rpak_remap_local_id(static_cast<int32_t>(pack_idx), local_id);
    return 0;
  }
  return lo | (pack_idx << 16);  // @ 0x5445B2
}

// PE ResourceEngine_RemapFromPath @ 0x538440 size ~0xD0 — thiscall
// g_ResourceEngine, stdcall (path, local) retn 8. Walk PathEq@+0x0C
// (stride 0x70); miss → InitSlot(count, path) ++count; EnsureIndex;
// ResPack_RemapLocalId(local). Callers leave Bind(type,flags) on stack.
uint32_t host_resource_engine_remap_from_path(const char* path,
                                              uint32_t local) {
  if (!path || !path[0]) return 0;
  // Soft EnsureIndex cold: open pack so PathEq / rpak_get can succeed.
  if (!rpak_find_by_name(path)) (void)rpak_open(path);
  const RpakPack* rp = rpak_find_by_name(path);

  host_pack_eng_sync_from_rpak();

  // PathEq walk @ 0x538456..0x538474 — Util_stricmp(slot+0x0C, path).
  for (uint32_t i = 0; i < g_pack_eng.pack_count && i < kMaxPackSlots; ++i) {
    auto* slot = g_pack_eng.slots[i].raw;
    const char* sp = reinterpret_cast<const char*>(slot + 0x0C);
    if (sp[0] == '\0') continue;
    if (_stricmp(sp, path) != 0) continue;
    if (host_resource_pack_ensure_index(slot) == 0) return 0;  // @ 0x5384F0
    return host_res_pack_remap_local_id(slot, local);           // @ 0x53850A
  }

  // LABEL_7 @ 0x538480: InitSlot at pack_count (PE index = count).
  // Soft: prefer rpak pack_id (1-based openLib HIWORD) when known so
  // RemapLocalId hi0 → lo|(pack_id<<16) matches Catalog / LookupById.
  uint32_t idx = g_pack_eng.pack_count;
  if (rp && rp->pack_id > 0 &&
      static_cast<uint32_t>(rp->pack_id) < kMaxPackSlots) {
    idx = static_cast<uint32_t>(rp->pack_id);
  }
  if (idx >= kMaxPackSlots) return 0;
  host_resource_pack_init_slot(g_pack_eng.slots[idx].raw, idx, path);
  if (idx >= g_pack_eng.pack_count) g_pack_eng.pack_count = idx + 1;
  if (host_resource_pack_ensure_index(g_pack_eng.slots[idx].raw) == 0)
    return 0;
  return host_res_pack_remap_local_id(g_pack_eng.slots[idx].raw, local);
}

// Soft stand-ins for BSS ResHandles cloneHdr binds (@ 0x43EDF1..0x43EE56).
// Layout ≡ ResHandle 16B: +0/+4 list, +8 id, +0xC owner (Bind @ 0x546070).
struct SoftGlobalRh {
  void* link0 = nullptr;
  void* link1 = nullptr;
  void* id = nullptr;     // +8 — PE dword_63C688 / 63C6A8 / 63C790 gate
  void* owner = nullptr;  // +0xC
};
SoftGlobalRh g_voidEvent_LoadGameInit_typeRH_soft;  // PE @ 0x63C680
SoftGlobalRh g_RH_particles_rpk_A_soft;             // PE @ 0x63C6A0
SoftGlobalRh g_RH_particles_rpk_B_soft;             // PE @ 0x63C788

void* host_res_handle_bind(void* rh, uint32_t id, int type, char flags);

void host_clone_hdr_lazy_remap_binds() {
  // PE cloneHdr @ 0x43EDD7: if *(RH+8)==0 → RemapFromPath + ResHandle_Bind.
  auto lazy = [](SoftGlobalRh& rh, const char* path, uint32_t local, int type) {
    if (rh.id != nullptr) return;  // already bound
    const uint32_t rid = host_resource_engine_remap_from_path(path, local);
    if (rid == 0) return;
    (void)host_res_handle_bind(&rh, rid, type, /*flags=*/0);
    // Soft deepen when Bind Resolve miss (PE leaves id unset): still record
    // remapped id at +8 so voidEvent/LoadGameInit type_rh+8 gate sees it.
    if (rh.id == nullptr)
      rh.id = reinterpret_cast<void*>(static_cast<uintptr_t>(rid));
  };
  lazy(g_voidEvent_LoadGameInit_typeRH_soft, "sl.rpk", 0x6Eu, /*type=*/6);
  lazy(g_RH_particles_rpk_A_soft, "particles.rpk", 0x106u, /*type=*/0xE);
  lazy(g_RH_particles_rpk_B_soft, "particles.rpk", 0x106u, /*type=*/0xE);
}

// Forward — body near InstanceNode ResolveParent RID cache.
void* host_resolve_parent(int32_t rid, int32_t type, char flags);

// PE Util_ReadLine @ 0x5511C0 → sub_554D60: copy thru \n or \0; ret bytes.
// Host: cap at max_n (LABEL_19 remaining) so sized RSD/text never OOB.
int host_util_read_line(const char* src, char* dst, int max_n) {
  if (!src || max_n <= 0) return 0;
  int n = 0;
  if (dst) {
    while (n < max_n) {
      const char c = src[n];
      dst[n] = c;
      ++n;
      if (c == '\n') {
        if (n < max_n) dst[n] = '\0';  // @ 0x554D98 after NL
        else dst[max_n - 1] = '\0';
        return n;
      }
      if (c == '\0') return n;  // @ 0x554D90 — NUL already in dst
    }
    dst[max_n - 1] = '\0';
    return max_n;
  }
  // dst==null: count only (@ 0x554D6B)
  while (n < max_n) {
    const char c = src[n++];
    if (c == '\n' || c == '\0') break;
  }
  return n;
}

// PE ResourceEngine_LookupById @ 0x536820 — EnsureIndex prelude + ResolveParent.
// ResourcePack_GetSlotState@544000 ≡ *(slot+0x50). Bind / LABEL_19 mid+0x38.
void* host_resource_engine_lookup_by_id(uint32_t id, int type, char flags) {
  if (id == 0) return nullptr;  // @ 0x53682A
  host_pack_eng_sync_from_rpak();
  const uint32_t hi = id >> 16;  // @ 0x536835
  if (hi != 0xFFFFu && hi < g_pack_eng.pack_count && hi < kMaxPackSlots) {
    uint8_t* slot = g_pack_eng.slots[hi].raw;  // eng+0xFFE94+0x70*hi
    // PE ResourcePack_GetSlotState@544000 == 0 → EnsureIndex@544170.
    if (*reinterpret_cast<int*>(slot + 0x50) == 0)
      (void)host_resource_pack_ensure_index(slot);  // @ 0x536868
  }
  return host_resolve_parent(static_cast<int32_t>(id), type, flags);  // @ 0x53687C
}

// PE ResHandle_Bind @ 0x546070 — this=rh(16B), a2=id, a3=type, a4.
// Unlink if owner; LookupById; link into owner+0x48. Miss → id not stored
// (stock @ 0x5460E0). W28C: LookupById extracted (was inline EnsureIndex).
void* host_res_handle_bind(void* rh, uint32_t id, int type, char flags) {
  if (!rh) return nullptr;
  auto** link = reinterpret_cast<void**>(rh);
  if (reinterpret_cast<uintptr_t>(link[2]) == id) return link[2];  // @ 0x54607D
  void* owner = link[3];
  if (owner) {
    // Unlink @ 0x546083..0x5460BD — same shape as Relink@545FC0.
    void* lp = link[0];
    void* ln = link[1];
    if (lp)
      *reinterpret_cast<void**>(reinterpret_cast<char*>(lp) + 4) = ln;
    else
      *reinterpret_cast<void**>(reinterpret_cast<char*>(owner) + 0x48) = ln;
    if (ln) *reinterpret_cast<void**>(ln) = lp;
    link[0] = nullptr;
    link[1] = nullptr;
    link[2] = nullptr;
    link[3] = nullptr;
  }
  if (id == 0) return nullptr;  // @ 0x5460C6
  void* node = host_resource_engine_lookup_by_id(id, type, flags);  // @ 0x5460D9
  if (!node) return nullptr;  // @ 0x5460E0 — id not written
  // Link @ 0x5460E2..0x546108 — head at node+0x48 (result+17 → +0x44,+[1]).
  link[2] = reinterpret_cast<void*>(static_cast<uintptr_t>(id));
  void* old_head =
      *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x48);
  if (old_head) *reinterpret_cast<void**>(old_head) = rh;
  link[0] = nullptr;
  link[1] = old_head;
  *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x48) = rh;
  link[3] = node;
  return node;
}

// PE LABEL_19 @ 0x53E019 — text after tag==0/size==0 or RSD FourCC.
// Util_ReadLine; "gametype" → RemapLocalId + Bind(mid+0x38, type=8);
// "params" → malloc(2049) @ mid+0x48 + sscanf %*s %2048s.
void host_parse_lod_text_label19(uint8_t* mid, void* pack_slot,
                                 const uint8_t*& cur, int nbytes) {
  if (!mid || nbytes <= 0) return;
  char line[2048];
  char tok[64];
  while (nbytes > 0) {
    const int got = host_util_read_line(reinterpret_cast<const char*>(cur),
                                        line, nbytes);  // @ 0x53E031
    if (got <= 0) break;
    cur += got;
    nbytes -= got;
    if (line[0] == '\0') continue;  // @ 0x53E04B
    tok[0] = '\0';
    (void)std::sscanf(line, "%63s", tok);  // @ 0x53E064 "%s" → v85[64]
    if (_stricmp(tok, "gametype") == 0) {  // @ 0x53E07F aGametype
      int local = 0;
      (void)std::sscanf(line, "%*s %d", &local);  // @ 0x53E095 "%*s %d"
      const uint32_t remapped = host_res_pack_remap_local_id(
          pack_slot, static_cast<uint32_t>(local));  // @ 0x53E0A7
      (void)host_res_handle_bind(mid + 0x38, remapped, /*type=*/8,
                                 /*flags=*/0);  // @ 0x53E0B0 v7+14
    } else if (_stricmp(tok, "params") == 0) {  // @ 0x53E2A5 aParams
      char** params_slot = reinterpret_cast<char**>(mid + 0x48);  // v7[18]
      if (*params_slot == nullptr)
        *params_slot = static_cast<char*>(std::malloc(2049));  // @ 0x53E2B9
      if (*params_slot)
        (void)std::sscanf(line, "%*s %2048s", *params_slot);  // @ 0x53E2CC
    }
  }
}

// W26C forward — body after InstanceNode pool (CreateNodeByDesc type1).
void* host_resource_engine_create_node_under(uint32_t remapped_id, uint32_t a3,
                                            const int32_t desc[5], void* parent,
                                            const uint32_t* xform7_or_null,
                                            int a7);

// PE ResPack_ParseChildRecord @ 0x544010 — RemapLocalId + CreateNodeUnder
// @536900; returns byte advance (PE pointer diff). Name malloc type==8
// freed unused (stock). PrepareLod / vtbl post-create stays at caller.
int host_res_pack_parse_child_record(void* pack_slot, uint8_t* rec,
                                     uint32_t pack_id, void** out_node,
                                     void* parent) {
  if (out_node) *out_node = nullptr;
  if (!rec || !pack_slot) return 0;
  const int adv = host_lgi_child_record_size(rec);
  if (adv <= 0) return 0;

  uint32_t local = 0;
  std::memcpy(&local, rec + 4, 4);  // @ 0x54407E input
  const uint32_t remapped = host_res_pack_remap_local_id(pack_slot, local);
  int32_t desc[5]{};
  desc[0] = static_cast<int32_t>(rec[8]);   // type  @ 0x544085
  desc[1] = static_cast<int32_t>(remapped);  // @ 0x544088
  desc[2] = static_cast<int32_t>(rec[9]);   // flags @ 0x54409E
  std::memcpy(&desc[3], rec + 14, 4);       // @ 0x5440A4
  std::memcpy(&desc[4], rec + 18, 4);       // @ 0x5440AA

  const uint32_t* xform = nullptr;
  uint32_t xform_buf[7]{};
  const uint8_t* payload = rec + 23 + rec[22];
  if ((rec[9] & 1) != 0) {
    // PE v15[0]=1 + 6 LE floats @ 0x5440AD..0x544111
    xform_buf[0] = 1u;
    std::memcpy(&xform_buf[1], payload, 24);
    xform = xform_buf;
  }
  void* node = host_resource_engine_create_node_under(
      remapped, pack_id, desc, parent, xform, /*a7=*/0);  // @ 0x544118/13D
  if (out_node) *out_node = node;
  return adv;  // @ 0x54414F
}

// PE ResNode_PrefetchExtPackSlot @ 0x544370 size 0x27 — thiscall
// (pack_slot, ext_idx). table=*(pack+0x5C); u16 at table+66*idx+2 <<16 →
// GetPackSlot(g_RE, 33*idx unused, pack_id). Ret discarded @ EXTP loop.
unsigned host_res_node_prefetch_ext_pack_slot(void* pack_slot, int ext_idx) {
  if (!pack_slot) return 0;
  auto* table = *reinterpret_cast<uint8_t**>(
      reinterpret_cast<uint8_t*>(pack_slot) + 0x5C);  // @ 0x54437B
  if (!table) return 0;
  const uint32_t pack_id =
      static_cast<uint32_t>(*reinterpret_cast<const uint16_t*>(
          table + 66 * ext_idx + 2))
      << 16;  // @ 0x544380..0x544385
  (void)(33 * ext_idx);  // PE edx arg unused by GetPackSlot body
  // @ 0x544394 — side-effect EnsureIndex on target pack.
  return static_cast<unsigned>(reinterpret_cast<uintptr_t>(
      host_resource_engine_get_pack_slot(pack_id)));
}

// PE EXTP AABB @ 0x53E108..0x53E280 — NOT unionChildAabb@4988A0.
// Keep node center (+0x84); expand half (+0x90) to cover each kid AABB;
// walk FindByRid chain [+0x20] via +4 (no type==2 filter); always
// writeback + aabb_state(+0x80)=1 (even if no kids).
void host_extp_expand_node_aabb(void* node) {
  if (!node) return;
  auto* np = reinterpret_cast<uint8_t*>(node);
  void* head = *reinterpret_cast<void**>(np + 0x20);  // @ 0x53E108
  void* cur = nullptr;
  if (head &&
      *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(head) + 4) !=
          nullptr) {
    cur = head;  // @ 0x53E10B..0x53E114
  }
  float cx = *reinterpret_cast<float*>(np + 0x84);
  float cy = *reinterpret_cast<float*>(np + 0x88);
  float cz = *reinterpret_cast<float*>(np + 0x8C);
  float hx = *reinterpret_cast<float*>(np + 0x90);
  float hy = *reinterpret_cast<float*>(np + 0x94);
  float hz = *reinterpret_cast<float*>(np + 0x98);
  while (cur) {
    auto* c = reinterpret_cast<uint8_t*>(cur);
    const float ccx = *reinterpret_cast<float*>(c + 0x84);
    const float ccy = *reinterpret_cast<float*>(c + 0x88);
    const float ccz = *reinterpret_cast<float*>(c + 0x8C);
    const float chx = *reinterpret_cast<float*>(c + 0x90);
    const float chy = *reinterpret_cast<float*>(c + 0x94);
    const float chz = *reinterpret_cast<float*>(c + 0x98);
    const float lo_x = ccx - chx;
    if (cx - hx > lo_x) hx = cx - lo_x;  // @ 0x53E18D..0x53E194
    const float hi_x = ccx + chx;
    if (hx + cx < hi_x) hx = hi_x - cx;  // @ 0x53E1AE..0x53E1B3
    const float lo_y = ccy - chy;
    if (cy - hy > lo_y) hy = cy - lo_y;
    const float hi_y = ccy + chy;
    if (hy + cy < hi_y) hy = hi_y - cy;
    const float lo_z = ccz - chz;
    if (cz - hz > lo_z) hz = cz - lo_z;
    const float hi_z = ccz + chz;
    if (hz + cz < hi_z) hz = hi_z - cz;
    cur = *reinterpret_cast<void**>(c + 4);  // @ 0x53E23A
    if (!cur ||
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cur) + 4) ==
            nullptr) {
      cur = nullptr;
    }
  }
  *reinterpret_cast<float*>(np + 0x84) = cx;  // @ 0x53E25B
  *reinterpret_cast<float*>(np + 0x88) = cy;
  *reinterpret_cast<float*>(np + 0x8C) = cz;
  *reinterpret_cast<float*>(np + 0x90) = hx;
  *reinterpret_cast<float*>(np + 0x94) = hy;
  *reinterpret_cast<float*>(np + 0x98) = hz;
  *reinterpret_cast<int32_t*>(np + 0x80) = 1;  // @ 0x53E280
}

// PE ResNode_setLodReadyBit @ 0x544B90: set/clear bit0 of this+0x64.
// a2=1 → ready. Callees sub_537D80 / sub_537DE0 OOS.
void host_lgi_set_lod_ready_bit(void* node, uint8_t ready) {
  if (!node) return;
  auto* st = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(node) + 0x64);
  const int32_t cur = *st;
  // PE: cur ^ ((a2 ^ (uint8)cur) & 1) — forces bit0 to a2, keeps high bits.
  *st = cur ^ ((ready ^ static_cast<uint8_t>(cur)) & 1);
}

// PE load-queue entry (≥0x38; +0x38 read @ 0x537B09 FreeAndNull path).
// dllist shape matches LodSlot +4/+8; RH at +0x18 ≡ ResHandle 16B.
struct HostLoadEntry {
  uint32_t pad0 = 0;
  HostLoadEntry* next = nullptr;       // +0x04
  HostLoadEntry* prev = nullptr;       // +0x08
  uint32_t pad0C[2]{};                 // +0x0C .. +0x13
  uint32_t pad14 = 0;                  // +0x14
  void* rh_prev = nullptr;             // +0x18 ResHandle.prev
  void* rh_next = nullptr;             // +0x1C
  void* rh_id = nullptr;               // +0x20
  void* node = nullptr;                // +0x24 RH.owner ≡ ResNode*/GI*
  int32_t flags = 0;                   // +0x28
  int32_t field_2C = 0;                // +0x2C
  void* buf = nullptr;                 // +0x30
  int32_t field_34 = 0;                // +0x34 cleared @ 0x537A4C
  uint32_t pad38 = 0;                  // +0x38
};
static_assert(offsetof(HostLoadEntry, next) == 0x04, "load+4");
static_assert(offsetof(HostLoadEntry, rh_prev) == 0x18, "load+0x18 RH");
static_assert(offsetof(HostLoadEntry, node) == 0x24, "load+0x24");
static_assert(offsetof(HostLoadEntry, flags) == 0x28, "load+0x28");
static_assert(offsetof(HostLoadEntry, buf) == 0x30, "load+0x30");
static_assert(sizeof(HostLoadEntry) == 0x3C, "load==0x3C");

// PE eng load lists: +0xDB8 queue head, +0xDD0 freelist sentinel, +0xDD4 head.
struct HostLoadEngLists {
  HostLoadEntry freelist_sentinel{};
  HostLoadEntry* freelist_head = nullptr;  // ≡ *eng+0xDD4
  HostLoadEntry queue_sentinel{};
  HostLoadEntry* queue_head = nullptr;      // ≡ *eng+0xDB8
  HostLoadEntry pool[16]{};
  bool inited = false;
};
HostLoadEngLists g_load_eng;

void load_eng_ensure_inited() {
  if (g_load_eng.inited) return;
  g_load_eng.inited = true;
  g_load_eng.freelist_sentinel.next = nullptr;
  g_load_eng.freelist_sentinel.prev = nullptr;
  g_load_eng.freelist_head = &g_load_eng.freelist_sentinel;
  g_load_eng.queue_sentinel.next = nullptr;
  g_load_eng.queue_sentinel.prev = nullptr;
  g_load_eng.queue_head = &g_load_eng.queue_sentinel;
  for (auto& e : g_load_eng.pool) {
    HostLoadEntry* old = g_load_eng.freelist_head;
    old->prev = &e;
    e.next = old;
    e.prev = &g_load_eng.freelist_sentinel;
    g_load_eng.freelist_head = &e;
  }
}

// PE FileAsync_CancelByUserdata @ 0x5556F0: Active==0 → -1; else ring
// cancel. PumpLoad sole caller @ 0x537A1B; return ignored.
int file_async_cancel_by_userdata(void* /*userdata*/) {
  // Host: no FileAsync subsystem ≡ FileAsync_Active==0.
  return -1;
}

// PE ResHandle_Relink(this=entry+0x18, a2=0) @ 0x545FC0 — unlink-only.
// cmp [rh+8], a2: equal → ret. Else if [rh+0xC]!=0: unlink node+0x48
// list, zero 4 dwords. a2==0 skips LookupById link half.
void load_entry_relink_clear(HostLoadEntry* entry) {
  if (!entry) return;
  auto* link = reinterpret_cast<void**>(reinterpret_cast<char*>(entry) + 0x18);
  if (link[2] == nullptr) return;  // id==a2==0 @ 0x545FCD
  void* owner = entry->node;  // [rh+0xC]
  if (!owner) return;         // PE skip unlink; a2==0 → done
  void* lp = link[0];
  void* ln = link[1];
  if (lp)
    *reinterpret_cast<void**>(reinterpret_cast<char*>(lp) + 4) = ln;
  else
    *reinterpret_cast<void**>(reinterpret_cast<char*>(owner) + 0x48) = ln;
  if (ln) *reinterpret_cast<void**>(ln) = lp;
  link[0] = nullptr;
  link[1] = nullptr;
  link[2] = nullptr;
  entry->node = nullptr;
}

// PE freelist push @ 0x537A65..0x537A7F (LABEL_28 tail):
//   old=*DD4; [old+8]=entry; [entry+4]=old; [entry+8]=&DD0; *DD4=entry.
void load_freelist_push(HostLoadEntry* entry) {
  if (!entry) return;
  load_eng_ensure_inited();
  HostLoadEntry* old = g_load_eng.freelist_head;
  old->prev = entry;                         // [old+8]=entry @ 0x537A6B
  entry->next = old;                         // *v18=old @ 0x537A6E
  entry->prev = &g_load_eng.freelist_sentinel;  // [esi+8]=&DD0 @ 0x537A76
  g_load_eng.freelist_head = entry;          // *DD4=entry @ 0x537A79
}

// PE LABEL_28 @ 0x537A13..0x537A7F — recycle completed/orphan load entry.
void load_entry_recycle(HostLoadEntry* entry) {
  if (!entry) return;
  load_eng_ensure_inited();
  // PE @ 0x537A13: if !(flags&1) → Cancel + free buf.
  if ((entry->flags & 1) == 0) {
    (void)file_async_cancel_by_userdata(entry);  // @ 0x537A1B
    if (entry->buf) {
      // PE Engine_free(buf) @ 0x537A2A; host: drop ptr (no PE heap).
      entry->buf = nullptr;
    }
  }
  load_entry_relink_clear(entry);  // Relink(entry+0x18, 0) @ 0x537A3D
  entry->field_34 = 0;             // @ 0x537A4C

  // Unlink from load-queue dllist @ 0x537A42..0x537A62.
  HostLoadEntry* next = entry->next;
  HostLoadEntry* prev = entry->prev;
  if (prev) {
    if (next) {
      next->prev = prev;  // [next+8]=prev @ 0x537A57
      prev->next = next;  // [prev+4]=next @ 0x537A5A
    }
  }
  entry->next = nullptr;
  entry->prev = nullptr;
  if (g_load_eng.queue_head == entry) {
    g_load_eng.queue_head =
        (next && next != &g_load_eng.queue_sentinel) ? next
                                                    : &g_load_eng.queue_sentinel;
  }

  load_freelist_push(entry);
  entry->flags &= 0x7FFFFFFE;  // @ 0x537A7F — clear bit0 + sign
}
}  // namespace

// PE ResNode_GameType_parseLodBuf @ 0x53DEE0 size 0x717.
// thiscall ecx=node+0xD8 mid; stack (node, buf, size=+0x5C, scale).
// W22A: prologue + empty success tail (GetPackSlot / setLodReadyBit / ++count).
// W23A body after setLodReadyBit @ 0x53DF57:
//   cursor skip dword0 + 4*dword0; ReadBE32 childCount (@ 0x53DF2A..0x53DF4A)
//   children: OR +9|=8; ParseChildRecord@544010 (@ 0x53DF61..0x53DFD9)
//   chunk loop (@ 0x53DFDE): PHYS→mid+0x10..0x1C; POLY→mid+0x30/0x34;
//     EXTP→Prefetch@544370 + expand AABB; RSD/text LABEL_19
// W25C: PackSlot = GetPackSlot(node+0x50); null → 0 (no FFFF-only gate).
// W26C: children → ParseChildRecord → CreateNodeUnder@536900 (type1 TREE);
//   PrepareLod/vtbl@53DF95..53DFD3 OOS.
// W27C: text/RSD LABEL_19 @ 0x53E019 — ReadLine + gametype Bind(mid+0x38,8)
//   + params@mid+0x48; RemapLocalId TOC miss / Bind Resolve miss OOS.
// W28C: Bind→LookupById@536820 (GetSlotState@544000+EnsureIndex+Resolve).
//   ++g_InstanceNodeCreateCount @ 0x53E5E0; return 1.0
float res_node_gametype_parse_lod_buf(void* mid, void* node, void* buf,
                                      int size, float /*scale*/) {
  if (!buf) return 0.f;  // @ 0x53DEFB
  if (!node) return 0.f;
  auto* np = reinterpret_cast<uint8_t*>(node);
  // PE GetPackSlot@5383F0 @ 0x53DF17 — var_28 PackSlot; null → return 0.
  // FFFF → eng+0xFFE24; else EnsureIndex(eng+0xFFE94+0x70*hi).
  const uint32_t pack_id =
      *reinterpret_cast<uint32_t*>(np + 0x50);  // @ 0x53DF06
  void* pack_slot = host_resource_engine_get_pack_slot(pack_id);
  if (!pack_slot) return 0.f;                   // @ 0x53DF1A
  host_lgi_set_lod_ready_bit(node, 1);          // @ 0x53DF57

  auto* mid_b = reinterpret_cast<uint8_t*>(mid ? mid : np + 0xD8);
  const auto* base = reinterpret_cast<const uint8_t*>(buf);
  const auto* end = base + (size > 0 ? size : 0);
  const auto* cur = base + 4;  // @ 0x53DF30
  {
    uint32_t hdr0 = 0;
    std::memcpy(&hdr0, base, 4);
    if (hdr0 != 0) cur += 4u * hdr0;  // @ 0x53DF35
  }
  if (cur + 4 > end) {
    ++g_lgi_create_count;
    return 1.0f;
  }
  uint32_t child_n = host_read_be32(cur);  // @ 0x53DF4A
  cur += 4;

  // Children @ 0x53DF61 — ParseChildRecord@544010 → CreateNodeUnder@536900.
  for (; child_n != 0; --child_n) {
    if (cur + 23 > end) break;
    auto* rec = const_cast<uint8_t*>(cur);
    rec[9] |= 8u;  // @ 0x53DF78
    void* child = nullptr;
    const int adv = host_res_pack_parse_child_record(
        pack_slot, rec, pack_id, &child, node);  // @ 0x53DF85
    if (adv <= 0 || cur + adv > end) break;
    cur += adv;
    // PE @ 0x53DF95: if child && (node+0x54 & 0x80000) → OR child+0x54,
    //   vtbl+0x14, PrepareLod@5447D0, vtbl+0xC — OOS (no ResNode_vtbl*).
    (void)child;
  }

  // Chunk loop @ 0x53DFDE: while (cur - buf < size).
  // W24: chunk_sz==0 + tag!=0 would spin (cur+=0) — break (corrupt/misaligned
  // cursor from cold LoadLod). Caps on PHYS/POLY counts by remaining bytes.
  while (cur < end) {
    if (cur + 8 > end) break;
    uint32_t tag = 0;
    uint32_t chunk_sz = 0;
    std::memcpy(&tag, cur, 4);           // @ 0x53DFF0
    std::memcpy(&chunk_sz, cur + 4, 4);  // @ 0x53DFF2
    cur += 8;                            // @ 0x53DFF8

    if (tag == 0) {
      if (chunk_sz != 0) {
        if (cur + chunk_sz > end) break;
        cur += chunk_sz;  // @ 0x53E406
        continue;
      }
      // tag==0 size==0 @ 0x53E00B: rest-of-buf text → LABEL_19 @ 0x53E019
      cur += 4;  // @ 0x53E011
      const int rem = static_cast<int>(end - cur);  // @ 0x53E016
      host_parse_lod_text_label19(mid_b, pack_slot, cur, rem);
      continue;
    }

    if (tag > kLodChunkPhys) {
      if (tag == kLodChunkPoly) {
        // POLY @ 0x53E40D → mid+0x30 count / +0x34 blob
        if (cur + 8 > end) break;
        (void)host_read_be32(cur);  // discarded BE32 @ 0x53E40E
        cur += 4;
        const uint32_t poly_n = host_read_be32(cur);  // @ 0x53E417
        cur += 4;
        *reinterpret_cast<uint32_t*>(mid_b + 0x30) = poly_n;  // @ 0x53E424
        if (poly_n == 0) continue;

        // Bound: min poly record advance is 12 bytes (a=b=0).
        const size_t rem = static_cast<size_t>(end - cur);
        if (poly_n > rem / 12u) {
          cur = end;
          break;
        }

        // Size pass @ 0x53E43B: PE malloc((int*)null + Σ(14a+3b+3))
        size_t words = 0;
        const uint8_t* scan = cur;
        for (uint32_t i = 0; i < poly_n; ++i) {
          if (scan + 8 > end) {
            words = 0;
            break;
          }
          scan += 4;  // skip group dword0
          const uint32_t a = host_read_be32(scan);
          scan += 4;
          const uint32_t b = host_read_be32(scan);
          words += static_cast<size_t>(14u * a + 3u * b + 3u);
          scan += 4 + 56u * a + 12u * b;
          if (scan > end) {
            words = 0;
            break;
          }
        }
        if (words == 0) {
          cur = end;
          break;
        }
        auto* blob = static_cast<uint32_t*>(std::malloc(words * 4));
        *reinterpret_cast<void**>(mid_b + 0x34) = blob;
        if (!blob) {
          *reinterpret_cast<uint32_t*>(mid_b + 0x30) = 0;
          cur = scan;
          continue;
        }
        // Fill pass @ 0x53E4B4..0x53E5C8
        uint32_t* out = blob;
        for (uint32_t i = 0; i < poly_n; ++i) {
          if (cur + 12 > end) break;
          *out++ = host_read_be32(cur);
          cur += 4;
          const uint32_t a = host_read_be32(cur);
          cur += 4;
          *out++ = a;
          const uint32_t b = host_read_be32(cur);
          cur += 4;
          *out++ = b;
          for (uint32_t j = 0; j < a; ++j) {
            // 6× LE + 2× BE32 + 6× LE = 14 dwords @ 0x53E4F4
            if (cur + 56 > end) break;
            for (int k = 0; k < 6; ++k) {
              uint32_t v = 0;
              std::memcpy(&v, cur, 4);
              cur += 4;
              *out++ = v;
            }
            *out++ = host_read_be32(cur);
            cur += 4;
            *out++ = host_read_be32(cur);
            cur += 4;
            for (int k = 0; k < 6; ++k) {
              uint32_t v = 0;
              std::memcpy(&v, cur, 4);
              cur += 4;
              *out++ = v;
            }
          }
          for (uint32_t j = 0; j < b; ++j) {
            if (cur + 12 > end) break;
            *out++ = host_read_be32(cur);
            cur += 4;
            *out++ = host_read_be32(cur);
            cur += 4;
            *out++ = host_read_be32(cur);
            cur += 4;
          }
        }
        continue;
      }
      if (chunk_sz == 0) break;  // W24: avoid cur+=0 spin
      if (cur + chunk_sz > end) break;
      cur += chunk_sz;  // unknown > PHYS @ 0x53E406
      continue;
    }

    if (tag == kLodChunkPhys) {
      // PHYS @ 0x53E2E2 → mid+0x18/+0x1C recs; mid+0x10/+0x14 xyz
      if (cur + 4 > end) break;
      uint32_t nrec = host_read_be32(cur);
      cur += 4;
      {
        const size_t rem = static_cast<size_t>(end - cur);
        if (nrec > rem / 28u) nrec = static_cast<uint32_t>(rem / 28u);
      }
      *reinterpret_cast<uint32_t*>(mid_b + 0x18) = nrec;  // @ 0x53E2F0
      if (nrec == 0) continue;
      auto* recs = static_cast<uint8_t*>(std::malloc(32u * nrec));
      *reinterpret_cast<void**>(mid_b + 0x1C) = recs;
      if (!recs) {
        *reinterpret_cast<uint32_t*>(mid_b + 0x18) = 0;
      } else {
        for (uint32_t i = 0; i < nrec; ++i) {
          if (cur + 28 > end) break;
          auto* slot = recs + 32u * i;
          *reinterpret_cast<uint32_t*>(slot) = host_read_be32(cur);
          cur += 4;
          for (int k = 0; k < 3; ++k) {
            *reinterpret_cast<uint32_t*>(slot + 4 + 4 * k) =
                host_read_be32(cur);
            cur += 4;
          }
          // 3× LE float @ +0x10; +0x1C = 0 (@ 0x53E361..0x53E384)
          std::memcpy(slot + 16, cur, 12);
          cur += 12;
          *reinterpret_cast<uint32_t*>(slot + 28) = 0;
        }
      }
      if (cur + 4 > end) break;
      uint32_t nxyz = host_read_be32(cur);
      cur += 4;
      {
        const size_t rem = static_cast<size_t>(end - cur);
        if (nxyz > rem / 12u) nxyz = static_cast<uint32_t>(rem / 12u);
      }
      *reinterpret_cast<uint32_t*>(mid_b + 0x10) = nxyz;  // @ 0x53E3AB
      auto* xyz = static_cast<uint8_t*>(std::malloc(12u * nxyz));
      *reinterpret_cast<void**>(mid_b + 0x14) = xyz;
      if (!xyz) {
        *reinterpret_cast<uint32_t*>(mid_b + 0x10) = 0;
      } else if (nxyz != 0) {
        if (cur + 12u * nxyz > end) {
          std::free(xyz);
          *reinterpret_cast<void**>(mid_b + 0x14) = nullptr;
          *reinterpret_cast<uint32_t*>(mid_b + 0x10) = 0;
          break;
        }
        std::memcpy(xyz, cur, 12u * nxyz);  // LE floats @ 0x53E3CF
        cur += 12u * nxyz;
      }
      continue;
    }

    if (tag == kLodChunkRsd) {
      // RSD @ 0x53E0CB → goto LABEL_19 with v14=chunk_sz (@ 0x53E0D0)
      if (cur + chunk_sz > end) break;
      host_parse_lod_text_label19(mid_b, pack_slot, cur,
                                  static_cast<int>(chunk_sz));
      continue;
    }

    if (tag == kLodChunkExtp) {
      // EXTP @ 0x53E0D6: PrefetchExtPackSlot@544370 dword loop (this=var_28
      // pack_slot) then expand AABB half @ 0x53E108..0x53E280.
      if (chunk_sz == 0) break;  // W24: avoid cur+=0 spin
      if (cur + chunk_sz > end) break;
      // Prefetch loop @ 0x53E0EB..0x53E103 — LE idx dwords; rem-=4.
      // PE assumes chunk_sz%4==0; host stops at rem<4 (no over-read).
      {
        const uint8_t* p = cur;
        int rem = static_cast<int>(chunk_sz);
        while (rem >= 4) {
          int ext_idx = 0;
          std::memcpy(&ext_idx, p, 4);  // @ 0x53E0EE
          p += 4;
          rem -= 4;
          (void)host_res_node_prefetch_ext_pack_slot(pack_slot, ext_idx);
        }
      }
      cur += chunk_sz;  // @ 0x53E10E
      host_extp_expand_node_aabb(node);  // @ 0x53E108..0x53E280
      continue;
    }

    if (chunk_sz == 0) break;  // W24: avoid cur+=0 spin on unknown tag
    if (cur + chunk_sz > end) break;
    cur += chunk_sz;  // default @ 0x53E406
  }

  ++g_lgi_create_count;  // @ 0x53E5E0
  return 1.0f;
}

// PE ResNode_GameType_vtbl28_LoadLod @ 0x53E860 size 0x3BE.
// Pump @ 0x5379B5: thiscall (node, buf=entry+0x30, scale=*(float*)(node+0x6C)).
// W21A: LABEL_13 early LABEL_68 (+0x64&1,+0x124==0,+0x118==0 → +0x68=1.0).
// W22A: cold !(+0x64&1) → parseLodBuf body @ 0x53E8C2 + setLodReadyBit
//   @ 0x53E8CD → fall LABEL_13. W23A: parse body (PHYS/POLY/child skip).
// OOS: parent PrepareLod@53E86B; Rebind@53E8F9; getPayload ctor@53E920..
float res_node_gametype_vtbl28_load_lod(void* node, void* buf, float scale) {
  if (!node) return 0.f;
  auto* p = reinterpret_cast<uint8_t*>(node);
  if ((*reinterpret_cast<int32_t*>(p + 0x64) & 1) == 0) {
    // Cold @ 0x53E8AF..0x53E8CD — parse then setLodReadyBit (PE always).
    const int32_t sz = *reinterpret_cast<int32_t*>(p + 0x5C);
    (void)res_node_gametype_parse_lod_buf(p + 0xD8, node, buf, sz, scale);
    host_lgi_set_lod_ready_bit(node, 1);  // @ 0x53E8CD
    // Rebind mid+0xF8 @ 0x53E8F9 OOS
  }
  // LABEL_13 @ 0x53E908
  if (*reinterpret_cast<int32_t*>(p + 0x124) != 0)
    return *reinterpret_cast<float*>(p + 0x68);
  if (*reinterpret_cast<int32_t*>(p + 0x118) != 0)
    return *reinterpret_cast<float*>(p + 0x68);  // ctor path OOS
  // LABEL_68 @ 0x53EBFB
  *reinterpret_cast<float*>(p + 0x68) = 1.0f;
  return 1.0f;
}

int resource_engine_pump_load_queue() {
  // PE @ 0x5378D9: Type7Count=0 before walk.
  g_pump_load_type7 = 0;
  load_eng_ensure_inited();

  // PE walk start @ 0x5378EB..0x5378FC: head=*eng+0xDB8; null if [head+4]==0.
// W20B: null-node orphan → LABEL_28. W21A–W23A: non-null + (lod-ready|flags&1)
// → vtbl+0x28 (cold parse body + LABEL_68); +0x54&=~0x2000000; buf==0 → LABEL_28.
// W37: rate@618D6C soft (default 16). OOS: EnsureIndex RPAK TOC File_SlotRead;
//   CreateNodeByDesc≠1; RemapLocalId TOC@+0x5C; Bind Resolve miss; LoadPack.
  uint32_t processed = 0;  // PE v2 / v19
  HostLoadEntry* cur = g_load_eng.queue_head;
  if (!cur || cur == &g_load_eng.queue_sentinel || !cur->next) {
    cur = nullptr;
  }
  while (cur) {
    HostLoadEntry* nxt = cur->next;
    if (!nxt || !nxt->next || nxt == &g_load_eng.queue_sentinel) nxt = nullptr;

    if (!cur->node) {
      // PE @ 0x537AEB: null node → recycle iff +0x2C==0 && flags&1.
      if (cur->field_2C == 0 && (cur->flags & 1) != 0) {
        load_entry_recycle(cur);  // LABEL_28
      }
    } else {
      // PE @ 0x537921..0x537937: ++loaded_n; >= Engine_resource_loadrate →
      // LABEL_37 (leave remaining slots for next pump).
      ++processed;
      if (processed >= g_engine_resource_loadrate) {
        // Keep loaded_save ≡ processed for ring; stop walk.
        cur = nullptr;
        break;
      }
      int32_t flag_bit0 = 0;
      if (cur->field_2C == 0) {
        // PE @ 0x537942..0x537962: flags&1;&4 + buf → free buf.
        flag_bit0 = cur->flags & 1;
        if (flag_bit0 != 0 && (cur->flags & 4) != 0 && cur->buf != nullptr) {
          cur->buf = nullptr;  // Engine_free OOS — drop ptr
        }
      }
      auto* np = reinterpret_cast<uint8_t*>(cur->node);
      const int32_t lod_st = *reinterpret_cast<int32_t*>(np + 0x64);
      // PE @ 0x537971: (+0x64&1) || flags&1 → complete.
      if ((lod_st & 1) != 0 || flag_bit0 != 0) {
        // @ 0x537977..0x537989: stamp+0x74 ≠ frame → TouchResNode.
        auto* touch_n = static_cast<HostResNode*>(cur->node);
        if (node_gc_stamp(touch_n) != g_res_gc_frame)
          resource_engine_touch_res_node(touch_n);
        if (*reinterpret_cast<int32_t*>(np + 0x4C) == 7)
          ++g_pump_load_type7;  // @ 0x537998
        const float scale = *reinterpret_cast<float*>(np + 0x6C);
        (void)res_node_gametype_vtbl28_load_lod(cur->node, cur->buf, scale);
        // PE @ 0x5379BA: node+0x54 &= ~0x2000000.
        *reinterpret_cast<uint32_t*>(np + 0x54) &= ~0x2000000u;
        if (cur->buf == nullptr) {
          load_entry_recycle(cur);  // LABEL_28 @ 0x5379C4
        }
        // else: EnsureIndex RPAK TOC OOS — leave on queue.
      }
    }

    cur = nxt;
  }

  // LABEL_37 @ 0x537A96..0x537B35 — rolling 32-slot ring + isLoading gate.
  const uint32_t idx = g_load_ring_index & 0x1Fu;
  const uint32_t sum =
      processed - g_load_ring_slots[idx] + g_load_ring_sum;  // @ 0x537AAC
  g_load_ring_slots[idx] = processed;                       // @ 0x537AB7
  g_load_ring_index = (idx + 1u) & 0x1Fu;                   // @ 0x537AC8
  g_load_ring_sum = sum;                                    // @ 0x537ACD
  // PE: (double)sum * 0.03125 > 4.0 → eng+0x106ED4 (flt_5F3564 / flt_5F0CC8).
  g_re_is_loading = (static_cast<double>(sum) * 0.03125 > 4.0) ? 1 : 0;
  if (g_pump_load_type7_peak < g_pump_load_type7)
    g_pump_load_type7_peak = g_pump_load_type7;  // @ 0x537B33
  return 0;  // PE @ 0x537B30
}

// PE ResHandle_maybeRelinkLod @ 0x00425150 size 0x2C:
// *(handle+0xC); if inner && (inner+0x54 & 0x100) →
// RelinkLodSlot(g_ResourceEngine, inner, 0.1f, 0, 1.0f).
void res_handle_maybe_relink_lod_node(void* host_res_node) {
  if (!host_res_node) return;
  auto* node = reinterpret_cast<HostResNode*>(host_res_node);
  auto* flags =
      reinterpret_cast<int32_t*>(reinterpret_cast<char*>(node) + 0x54);
  if ((*flags & kRelinkLodFlag) == 0) return;
  // PE immediates @ 0x42516C..0x425176: 0x3DCCCCCD, 0, 0x3F800000.
  res_node_relink_lod_slot(node, 0.1f, 0.f, 1.0f);
}

static void res_handle_maybe_relink_lod(HostNativeHandle* h) {
  if (!h || !h->node) return;
  res_handle_maybe_relink_lod_node(h->node);
}

static int32_t* host_node_flags(HostResNode* n) {
  if (!n) return nullptr;
  return reinterpret_cast<int32_t*>(reinterpret_cast<char*>(n) + 0x54);
}

static HostResNode* host_node_new(InvObject* owner, int role) {
  auto* n = new HostResNode{};
  n->owner = owner;
  n->role = role;
  n->type = 1;
  // PE InstanceNode_initExt @ 0x544D40: +0xB8 bias=0, +0xBC scale=1.0
  // (setLodCopy6C/70 formula). pad_50_cc covers +0x50..+0xCB.
  *reinterpret_cast<float*>(reinterpret_cast<char*>(n) + 0xB8) = 0.f;
  *reinterpret_cast<float*>(reinterpret_cast<char*>(n) + 0xBC) = 1.0f;
  // W29C: soft eng+type*0x90+0x150 touch list (LookupById/Bind consumers).
  gc_bucket_touch_insert(n);
  if (role == 0) {
    // Camera.setFog dual hop: mid+0x84 → fog node → fog params.
    n->fog_node = host_node_new(owner, 1);
    n->mid.child = n->fog_node;
    // setParent type1: mid+0x44 nest → nest mid+0xC leaf (CameraCtrl /
    // Chassis). mid+0x4C = attach block (≥0x40) wired in native_ptr_ensure
    // — not the PE 16B Native.ptr box.
    n->nest_node = host_node_new(owner, 2);
    n->mid.nested = n->nest_node;
    n->leaf = new HostLeafAttach{};
    n->nest_node->leaf = n->leaf;
    n->nest_node->mid.leaf = n->leaf;
    // parent_inner+0xCC type53 WT: getPayload → pay+0x40==53, +0x4C block.
    n->type53_node = host_node_new(owner, 3);
    n->wt_cc = n->type53_node;
    n->type53_block = new HostType53Block{};
    n->type53_node->type53_block = n->type53_block;
    n->type53_node->mid.payload_type = kPayloadType53;
    n->type53_node->mid.block = n->type53_block;
  }
  return n;
}

HostNativeHandle* native_ptr_get(InvObject* self) {
  if (!self) return nullptr;
  auto it = g_native_ptr.find(self);
  return it == g_native_ptr.end() ? nullptr : it->second;
}

HostNativeHandle* native_ptr_ensure(InvObject* self) {
  if (!self) return nullptr;
  HostNativeHandle* h = native_ptr_get(self);
  if (h) {
    h->alive = 1;
    if (!h->node) h->node = host_node_new(self, 0);
    // PE mid+0x4C: ≥0x40 attach/phys block → leaf.vtbl+0x20 @ 0x48AD0A;
    // CameraCtrl setParent_inner(block+0x30) @ 0x4385A2.
    host_handle_wire_attach(h);
  } else {
    h = new HostNativeHandle{};
    h->alive = 1;
    h->node = host_node_new(self, 0);
    // mid+0x4C → &attach (+0xC node, +0x30 emb) — GameRef_handleAttachUnderParent
    // @ 0x00429C80 reads a1+0xC; CameraCtrl lea ecx,[edi+30h].
    host_handle_wire_attach(h);
    g_native_ptr[self] = h;
  }
  // W14C: RESTYPE_GAME=8 load path — sync node+0x4C and seed mid+0x10 when
  // ResState.type==8 and script_class still null (PE type_gametype write).
  if (h->node && !h->node->mid.script_class) {
    int32_t rtype = 0;
    {
      auto it = g_res.find(self);
      if (it != g_res.end()) rtype = it->second.type;
    }
    if (rtype == 8) {
      h->node->type = 8;
      const char* fqn = tree_host_class(self);
      if (fqn && fqn[0])
        host_mid_seed_script_class(h->node, /*clazz=*/nullptr, fqn);
    }
  }
  return h;
}

void* native_ptr_attach_block(InvObject* self) {
  HostNativeHandle* h = native_ptr_get(self);
  if (!h || !h->alive) return nullptr;
  host_handle_wire_attach(h);
  return &h->attach;
}

void native_ptr_clear(InvObject* self) {
  if (!self) return;
  auto it = g_native_ptr.find(self);
  if (it == g_native_ptr.end()) return;
  HostNativeHandle* h = it->second;
  g_native_ptr.erase(it);
  if (h) {
    host_node_free_tree(h->node);
    h->node = nullptr;
    delete h;
  }
}

void* native_ptr_node(InvObject* self) {
  HostNativeHandle* h = native_ptr_get(self);
  if (!h || !h->alive) return nullptr;
  return h->node;
}

void* res_handle_get_payload(void* node_raw, int32_t tag) {
  // PE @ 0x00419860 → ResNode_getEmbeddedMid @ 0x0053EFA0 (this+0xD8).
  // Host: tag 0xA0000000 only (fog / setParent type1 / type53 WT).
  auto* node = reinterpret_cast<HostResNode*>(node_raw);
  if (!node || tag != kResTagA0) return nullptr;
  if (node->type != 1) return nullptr;
  if (node->role == 1) return &node->fog;  // fog params +0x1C..
  // role 0 root mid, 2 nest mid (leaf at +0xC), 3 type53 (+0x40==53).
  return &node->mid;
}

InvObject* class_box_object(const char* fqn) {
  // PE @ 0x00404E20 Class_boxObject — fresh Class shell, not script instance.
  if (!fqn || !fqn[0]) return nullptr;
  InvObject* c = tree_host_new("java.lang.Class");
  if (!c) return nullptr;
  tree_field_set_obj(c, "name", string_new(fqn));
  tree_field_set_obj(c, "clazzname", string_new(fqn));
  return c;
}

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
  // race125: deepen_partial — ground_precache 0x80000 + bind stand-in;
  // sub_5447D0 / vtbl hops still OOS.
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
  // (host: ret). Else [handle+8]==0 or inner=[handle+0xC]==0 → ret.
  // [inner+0x4C]==1 INSTANCE_GAME → sub_427720(handle) (engine destroy
  // queue). Else thiscall inner.vtbl+0x1C(1). race123: deepen_partial —
  // IDA body is inline type split (not jmp 0x48A8F0). Not load @
  // 0x0047D0A0 / cache @ 0x0047E8D0 / destroy @ 0x0047D1E0.
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
  // PE @ 0x0047E950 size 0x7b (int_convert 123). UnboxArg ()V: this only.
  // Native.ptr dword_62E008; 0 → "!Mighty ERROR" on Engine_ErrorLogBuf @
  // 0x0062E018 then ret. Else thiscall sub_48A920(ecx=handle, a2=1.0f /
  // 0x3F800000, a4=1.0f). Twin of cache @ 0x0047E8D0 (only a2=0.0f).
  // a4=1.0 skips sub_537240 when inner+0x54 has 0x2000000. Not load @
  // 0x0047D0A0 (sub_5447D0 0x80000001 + optional ground_precache). Java:
  // Track air/tyre SfxRef, City siren, Dialog menu SFX. Host: !self =
  // handle 0 (silent); load only (no cached=1).
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
  // PE @ 0x0047CFC0 size 0xd5 (213). Unbox this+other (JVM_UnboxArg).
  // Handle via dword_62E008. handle==0 → silent ret (NO Mighty).
  // other==null (v6==0) → sub_545FC0(handle, 0) clear bind.
  // Else share other's [+0xC] via inline list unlink/relink at resource
  // +0x48/+0x50 (NO ID lookup / sub_536820 — contrast set(I) @ 0x0047CF80).
  // (log not mirrored). Race125 deepen_partial: same-inner early-out by
  // id+type (PE [this+0xC]==[other+0xC]); list unlink/relink at +0x48
  // still OOS (host copies ResState).
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  if (!ref) {
    bind_res_id(R(self), 0);
  } else {
    auto& dst = R(self);
    const auto& src = R(ref);
    // PE same-inner: no unlink/relink.
    if (dst.id == src.id && dst.type == src.type && dst.id != 0) return;
    dst = src;
  }
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
  // PE @ 0x0047D3E0 size 0xcd (int_convert 205). UnboxArg ()I: this.
  // Native.ptr dword_62E008; 0 → "!Mighty ERROR at " + Natives.cpp +
  // " line " + sprintf "%d" 1137 (0x471) then return 0. Else thiscall
  // ResourceRef_getParentID_inner @ 0x0048AB80 (size 0x11, unique xref):
  // inner=[handle+0xC]; inner==0 → 0 (NO Mighty); else
  // *(*(inner+0x14)+0x50) parent id DWORD. Host: R.parent_id in id()
  // space. !self = handle 0 (silent).
  if (!self) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  return R(self).parent_id;
}
InvObject* java_util_resource_ResourceRef_getParent(InvObject* self) {
  // PE @ 0x0047D4B0 size 0xba (186): twin of getFirstChild @ 0x0047D630 /
  // getWTRoot @ 0x0047D570 / getNextChild @ 0x0047D6F0. Unbox this;
  // dword_62E008; Engine_malloc(16) zeroed scratch; thiscall
  // ResourceRef_getParentNode @ 0x0048AAF0 (was sub_48AAF0):
  // inner=[handle+0xC]; parent_node=[[inner]+0x14]; scratch+8 = [node+0x50]
  // Native.ptr; non-0 → ResourceRef_boxFromHandle @ 0x0047D300, else free
  // → null. Contrast getParentID @ 0x0047D3E0: same node via sub_48AB80,
  // returns DWORD id only. Host: parent_id in id() space → make_bound_ref.
  int32_t pid = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    pid = R(self).parent_id;
  }
  return make_bound_ref(pid);
}
InvObject* java_util_resource_ResourceRef_getFirstChild(InvObject* self) {
  // PE @ 0x0047D630 size 0xba (186): twin of getNextChild @ 0x0047D6F0
  // (same size / Unbox / dword_62E008 / Engine_malloc(16) zeroed scratch /
  // ResourceRef_boxFromHandle @ 0x0047D300 on success / Engine_free else).
  // Sole delta: thiscall ResourceRef_getFirstChildNode @ 0x0048D230
  // (was sub_48D230) size 0xc6. ecx=handle, push scratch; inner=[handle+0xC];
  // target=[[inner]+0x20] first-child node; require node!=0 and [node+4]!=0,
  // else return [scratch+8] (often 0). Link scratch into node list @ +0x48;
  // [scratch+8]=[node+0x50] Native.ptr; non-0 → box NEW ResourceRef.
  // Host: first child in id() space via rpak_first_child_id.
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
  // ResourceRef_getFirstChildNode @ 0x0048D230 size 0xc6). Both: ecx=handle, push scratch;
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
  // PE @ 0x0047D570 size 0xba (186): twin of getParent @ 0x0047D4B0 /
  // getFirstChild @ 0x0047D630. Unbox this; dword_62E008; Engine_malloc(16)
  // zeroed scratch; thiscall ResourceRef_getWTRootNode @ 0x0048AA70
  // (was sub_48AA70): gate [handle+8]==0 or inner=[handle+0xC]==0 → 0;
  // WT root node at [inner+0xCC]; scratch+8 = [node+0x50] Native.ptr;
  // non-0 → ResourceRef_boxFromHandle @ 0x0047D300, else free → null.
  // Host: climb parent_id / rpak_parent_id to world-tree root id.
  if (!self) return nullptr;
  int32_t id = 0;
  int32_t pid = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    id = R(self).id;
    pid = R(self).parent_id;
  }
  if (id == 0) return nullptr;
  int guard = 0;
  while (pid != 0 && guard++ < 4096) {
    id = pid;
    pid = 0;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      for (auto& kv : g_res) {
        if (kv.second.id == id) {
          pid = kv.second.parent_id;
          break;
        }
      }
    }
    if (pid == 0) pid = rpak_parent_id(id);
  }
  return make_bound_ref(id);
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
  // race123: deepen_partial — still AllocLocalRid stand-in; PE
  // ResourceEngine_type_texture @ 0x539170 + list splice OOS.
  // race125: IDA renamed ResourceEngine_type_texture; host still
  // type=7 + D3D upload (no PE blob sprintf / list +0x48).
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
  // sequential ids from 1; not PE BST midpoint). Ensure HostPeBoneNode on
  // getPayload mid+0x12C for findBoneById / SetBoneMatrix HEAD.
  if (!self) return 0;
  const char* name = alias ? string_cstr(alias) : nullptr;
  const int32_t id = render_d3d9_mesh_get_bone_id(self, name);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (HostNativeHandle* h = native_ptr_ensure(self)) {
      if (h->node) mid_ensure_bone_by_id(h->node, id);
    }
  }
  return id;
}
void java_util_resource_RenderRef_setMatrix(InvObject* self, int32_t bone_id,
                                            InvObject* bone_ref, InvObject* pos,
                                            InvObject* ori) {
  // PE @ 0x004810B0 size 0x188. Unbox this, bone_id, bone_ref, pos, ori
  // (box+8; Hex-Rays dests lie). Handle 0 → Mighty ERROR (host: ret).
  // Null pos/ori → 0,0,0 (JNI still passes stack buffers → matrix write).
  // Vec3_store YPR. No RenderRef_bindBone. RenderRef_SetBoneMatrixParentLink
  // @ 0x0048BF50 size 0x19b: bone_id via RenderPayload_findBoneById
  // @ 0x005413C0 on THIS payload; bone_ref unboxed L — no get_int_field.
  // When pos buf nonzero: Ypr_toMatrix / Mat3x4_setIdentity + sub_54F4C0
  // translate; write bone+0x54 matrix; bone+0xF0=1; identity → bone+0x3C
  // |=2 else &= ~2; then HEAD insert at payload+0x178 (v8[94]) @ 0x48C094
  // (bone+0x0C=&+0x168 sentinel, bone+0x10=old_head); payload+0xBC
  // (v8[47]) |= 0x1800. Always RenderRef_LinkOrUnlinkBone @ 0x0048BE10
  // (a4=bone_ref): bone_ref==0 → unlink to own list; else reparent under
  // bone_ref. Stamp payload+0xF8 ← dword_6200A4. PE has no bone_ref==self
  // compare. Contrast 2-arg @ 0x00481240: bindBone("bone00") + bone_ref=0.
  // Host: mid≥0x17C own-list + findBoneById HEAD; RelinkLod gate; ResState
  // sib stand-in kept for InvObject parent graph.
  // LinkOrUnlinkBone @ 0x0048BE10 hostability (IDA):
  //   a4==0: unlink + reinsert own list mid+0x168/+0x178; ~0x4000 on
  //     handle-inner+0x54.
  //   a4!=0 type-match: ResHandle_maybeRelinkLod @ 0x425150 →
  //     RelinkLodSlot@537790 (tryRelinkLod + W17A freelist/OR hosted).
  //   a4!=0 type mismatch: createBoneParentHook still OOS (d3d9 side).
  if (!self) return;
  float x = 0, y = 0, z = 0;
  vec3_get(pos, &x, &y, &z);
  float yaw = 0, pitch = 0, roll = 0;
  ypr_get(ori, &yaw, &pitch, &roll);
  // PE @ 0x48C029-0x48C08C: after matrix write bone+0xF0=1; identity →
  // bone+0x3C |= 2 else &= ~2 (1.0=0x3F800000 on diag, zeros on translate).
  const bool identity =
      (x == 0.f && y == 0.f && z == 0.f && yaw == 0.f && pitch == 0.f &&
       roll == 0.f);
  auto bone_list_unlink_from = [](InvObject* child, InvObject* parent) {
    if (!child || !parent) return;
    auto cit = g_res.find(child);
    auto pit = g_res.find(parent);
    if (cit == g_res.end() || pit == g_res.end()) return;
    ResState& r = cit->second;
    ResState& ps = pit->second;
    InvObject* next = r.bone_sib_next;
    InvObject* prev = r.bone_sib_prev;
    // PE LinkOrUnlinkBone unlink: if next&&prev: next.prev=prev; prev.next=next.
    // Host null next/prev = sentinel stand-in (&payload+0x168).
    if (next) {
      auto nit = g_res.find(next);
      if (nit != g_res.end()) nit->second.bone_sib_prev = prev;
    } else if (ps.bone_child_head == child) {
      ps.bone_child_head = prev;
    }
    if (prev) {
      auto qit = g_res.find(prev);
      if (qit != g_res.end()) qit->second.bone_sib_next = next;
    }
    r.bone_sib_next = nullptr;
    r.bone_sib_prev = nullptr;
  };
  auto bone_list_insert_head = [](InvObject* child, InvObject* parent) {
    if (!child || !parent) return;
    auto& r = R(child);
    auto& pr = R(parent);
    // PE @ 0x48C094-0x48C0AD: insert at HEAD (v8[94]=payload+0x178).
    // new.next=&sentinel(+0x168); new.prev=old_head; old_head.next=new;
    // head=new. Host: null next = sentinel stand-in.
    InvObject* old_head = pr.bone_child_head;
    if (old_head) {
      auto hit = g_res.find(old_head);
      if (hit != g_res.end()) hit->second.bone_sib_next = child;
    }
    r.bone_sib_next = nullptr;
    r.bone_sib_prev = old_head;
    pr.bone_child_head = child;
  };
  // PE SetBoneMatrixParentLink: findBoneById → matrix/HEAD; always LinkOrUnlink.
  HostNativeHandle* self_h = nullptr;
  HostPeBoneNode* pe_bone = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    self_h = native_ptr_ensure(self);
    if (self_h && self_h->node) {
      mid_ensure_bone_by_id(self_h->node, bone_id);
      pe_bone =
          render_payload_find_bone_by_id(&self_h->node->mid, bone_id);
      if (pe_bone) {
        *reinterpret_cast<int32_t*>(pe_bone->raw + 0xF0) = 1;
        int32_t fl = *reinterpret_cast<int32_t*>(pe_bone->raw + 0x3C);
        if (identity)
          fl |= 2;
        else
          fl &= ~2;
        *reinterpret_cast<int32_t*>(pe_bone->raw + 0x3C) = fl;
        mid_bone_own_list_insert_head(&self_h->node->mid, pe_bone);
        self_h->node->mid.bone_list_flags |= kBoneParentLinked;
        self_h->node->mid.bone_stamp = 1;
      }
    }
  }
  if (!bone_ref) {
    // PE LinkOrUnlinkBone a4==0: unlink foreign, own-list @ mid+0x168/+0x178
    // (HEAD above when pe_bone). PE @ 0x48BEF2: inner+0x54 &= ~0x4000.
    render_d3d9_mesh_set_bone_local(self, bone_id, x, y, z, yaw, pitch, roll);
    {
      std::lock_guard<std::mutex> lock(g_mu);
      auto& r = R(self);
      r.px = x;
      r.py = y;
      r.pz = z;
      r.oy = yaw;
      r.op = pitch;
      r.or_ = roll;
      if (r.bone_link_parent) bone_list_unlink_from(self, r.bone_link_parent);
      r.bone_link_parent = nullptr;
      r.bone_link_id = bone_id;
      r.bone_sib_next = nullptr;
      r.bone_sib_prev = nullptr;
      r.bone_pose_set = 1;
      if (identity)
        r.bone_flag_bits |= 2;
      else
        r.bone_flag_bits &= ~2;
      r.bone_stamp = 1;
      r.flags &= ~kWtBoneUnlink;
      if (self_h && self_h->node) {
        if (int32_t* nf = host_node_flags(self_h->node))
          *nf &= ~kWtBoneUnlink;
      }
    }
    render_d3d9_mesh_set_parent(self, nullptr);
    return;
  }
  if (bone_ref == self) {
    // Host-only smoke: parent.setMatrix(id, self) — pose this's bone_id.
    // PE has no bone_ref==self compare (would still parent-link).
    render_d3d9_mesh_set_bone_local(self, bone_id, x, y, z, yaw, pitch, roll);
    return;
  }
  // bone_ref another instance: PE HEAD splice + |= 0x1800; LinkOrUnlinkBone
  // type-match → RelinkLod only; mismatch OOS hook (d3d9 side map).
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
    InvObject* prev_parent = r.bone_link_parent;
    if (prev_parent && prev_parent != bone_ref)
      bone_list_unlink_from(self, prev_parent);
    const bool already_linked = (prev_parent == bone_ref);
    r.bone_link_parent = bone_ref;
    r.bone_link_id = bone_id;
    r.bone_pose_set = 1;
    if (identity)
      r.bone_flag_bits |= 2;
    else
      r.bone_flag_bits &= ~2;
    auto& pr = R(bone_ref);
    pr.bone_link_flags |= kBoneParentLinked;
    pr.bone_stamp = 1;  // PE v8[62]=dword_6200A4 @ payload+0xF8
    if (!already_linked) bone_list_insert_head(self, bone_ref);
    if (HostNativeHandle* ph = native_ptr_ensure(bone_ref)) {
      if (ph->node) {
        ph->node->mid.bone_list_flags |= kBoneParentLinked;
        ph->node->mid.bone_stamp = 1;
      }
      res_handle_maybe_relink_lod(ph);
    }
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
  // PE @ 0x00481240 size 0x184: Unbox this, pos, ori. Handle 0 → Mighty
  // ERROR (host: ret). Null pos/ori → 0,0,0. Vec3_store YPR. Then
  // RenderRef_bindBone(handle, "bone00") @ 0x0048BC40 + 
  // RenderRef_SetBoneMatrixParentLink(handle, boneId, bone_ref=0, pos*, ypr*)
  // @ 0x0048BF50 — same body as 4-arg @ 0x004810B0 with a4=0.
  // race124: pass getBoneId("bone00") stand-in for bindBone return (not
  // hardcoded 0); LinkOrUnlinkBone a4==0 own-list via mid+0x168/+0x178.
  if (!self) return;
  const int32_t bone00 =
      java_util_resource_RenderRef_getBoneId(self, string_new("bone00"));
  java_util_resource_RenderRef_setMatrix(self, bone00, /*bone_ref=*/nullptr,
                                         pos, ori);
}
InvObject* java_util_resource_RenderRef_getPos(InvObject* self) {
  // Soft host-only: stock Natives_RegisterAll has NO RenderRef.getPos
  // (IDA find_regex: create/getBoneId/setMatrix×2/changeResource/
  // setColor/setLight/setFlare/setType/getTypeID/lineCreate/lineAdd/
  // plotRoute only). Soft mirrors PhysicsRef.getPos PE @ 0x00480B00
  // size 0x110 (272) null-handle→nullptr (no Mighty — no Soft PE body)
  // + pose written by setMatrix PE @ 0x00481240 size 0x184 (388)
  // (bone00 + RenderRef_SetBoneMatrixParentLink). Always alloc Vector3.
  if (!self) return nullptr;
  // PE @ 0x00480B00 Soft parallel (PhysicsRef.getPos); writer PE @ 0x00481240.
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  const float px = r.px, py = r.py, pz = r.pz;
  return vec3_new(px, py, pz);
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
  // race124: deepen_partial — still polyline stand-in; PE Y=-0.0005 bake
  // mirrored (0xBA03126F); span sample via host g_last_route (not
  // dword_6408D0 spline). race125: same; GroundRef_cachedRoute gate
  // still physics_road polyline.
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
  // Soft Welder.addPoints (Java private native) — no stock UnboxArg FQN /
  // impl in Natives_RegisterAll. Soft stand-in mirrors lineAdd PE @
  // 0x0047FEE0 size 0xf4 (244) → RenderRef_applyLineAdd PE @ 0x0048A860
  // → Skidmark_AppendPoint @ 0x00521620: append pos+normal+width into
  // host LineState (g_lines ≡ handle+8 line payload). Also seeds
  // target_len for weld/progress Soft pair.
  if (!self || !v) return;
  // PE @ 0x0047FEE0 Soft related (lineAdd); apply PE @ 0x0048A860.
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
  // Soft Welder.weld — no stock PE string/impl. Soft: append weld point
  // like lineAdd PE @ 0x0047FEE0 → RenderRef_applyLineAdd PE @ 0x0048A860
  // (Skidmark_AppendPoint @ 0x00521620); accumulate progress vs target_len
  // from addPoints / |line| segment. Return 1 when progress≥1.
  if (!self) return 0;
  // PE @ 0x0047FEE0 Soft related (lineAdd); apply PE @ 0x0048A860.
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
  // Soft Welder.progress — no stock PE. Soft reads LineState.progress
  // written by weld (sibling of lineAdd PE @ 0x0047FEE0 /
  // RenderRef_applyLineAdd PE @ 0x0048A860). Missing line → 0.
  if (!self) return 0.f;
  // PE @ 0x0047FEE0 Soft related (lineAdd LineState); apply PE @ 0x0048A860.
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_lines.find(self);
  if (it == g_lines.end() || !it->second.active) return 0.f;
  const float p = it->second.progress;
  return p > 1.f ? 1.f : (p < 0.f ? 0.f : p);
}

// ---- PhysicsRef ----

// PhysicsRef natives → PhysicsRef.cpp

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

// Soft PE Physics_Step @ 0x4A5190 (ecx=Engine_physWorld; caller
// Engine_SimulateFrame @ 0x42859A / 0x42892B). int_convert offsets:
//   +4 dt accum, +12 last step, +16/+20/+44 counters, +88=0x58, +92=0x5C,
//   +132=0x84 body list (head *[list+0x24], next +0x28),
//   +156=0x9C dllist, +184=0xB8 dllist, +280=0x118 reset wipe,
//   +492=0x1EC solver continue. Gate flt_5F09D4@0x5F09D4=1e-4 (0x38D1B717).
// Soft max substep 0.05 ≡ System kPhysMaxSubstepSoft (*(world+8) OOS).
// Body lists: soft census / gates only — no PE dllist nodes, no invent
// sub_4A3900/sub_4A2D10/sub_4A2540 solver. sub_426F60 animate OOS no-op.
//
// Chassis aero consumer (not Physics_Step — PE has ZERO +0x908 hits):
//   PE Chassis_physWheelTick @ 0x456a83 → Phys_accumForceAtLocalPoint
//   @ 0x4A6520 (F into body force accum; τ += (R*r)×F). Host Chassis
//   writes TREE side-band (aero_impulse_* / aero_applied / aero_force_p*);
//   THIS path is the sole live Δv consumer (Chassis.h). forceUpdate only
//   STOREs hdr+0x8FC..+0x908 → force_update_drag_* (fallback Cd path).
// Constants (get_bytes): flt_5E73CC=0, float_1_0=1, flt_5F09D0=0.5,
// flt_5F0C80=0.25. Torque/inertia tensor / parent-walk (+0x58)&4 OOS.
static int32_t g_phys_step_frame = 0;     // soft dword_6439C0 @ 0x4a520c
static int32_t g_phys_list132 = 0;        // soft +132 walk tally
static int32_t g_phys_list156 = 0;        // soft +156 cull candidates
static int32_t g_phys_list184 = 0;        // soft +184 live candidates
static int32_t g_phys_solver_iters = 0;   // soft *(world+16)
static int32_t g_phys_aero_hits = 0;      // soft Cd / impulse apply count
static float g_phys_world_clock = 0.f;    // soft *(physWorld+0) @ 0x4a548e

// Soft donor for Chassis aero TREE fields. Body key may be Vehicle
// (Native.ptr) while side-band lives on chassis (setMileage / forceUpdate).
static InvObject* physics_aero_sideband_owner(InvObject* key) {
  if (!key) return nullptr;
  if (tree_field_get_int(key, "aero_applied") != 0) return key;
  if (InvObject* ch = tree_field_get_obj(key, "chassis")) {
    if (tree_field_get_int(ch, "aero_applied") != 0) return ch;
  }
  if (tree_field_get_float(key, "force_update_drag_c") > 0.f) return key;
  if (InvObject* ch = tree_field_get_obj(key, "chassis")) {
    if (tree_field_get_float(ch, "force_update_drag_c") > 0.f) return ch;
  }
  return key;
}

static void physics_apply_chassis_aero(InvObject* key, ResState& r, float dt) {
  if (!key || dt <= 0.f) return;
  InvObject* src = physics_aero_sideband_owner(key);
  if (!src) return;

  // Path A — consume Chassis soft side-band (chassis_phys_aero_soft_apply).
  // One-shot: clear aero_applied so kSub≠double-apply (Chassis wrote J for
  // its setMileage dt once; PE accumulates force once per wheel-tick).
  if (tree_field_get_int(src, "aero_applied") != 0) {
    float jx = tree_field_get_float(src, "aero_impulse_fx");
    float jy = tree_field_get_float(src, "aero_impulse_fy");
    float jz = tree_field_get_float(src, "aero_impulse_fz");
    // Soft inv-mass stand-in for PE force-accum → solver. TREE mass /
    // chassis_mass; else 1200 ≡ Chassis_getMass soft default (so Chassis
    // mass-clamped J @ 0x456b2a.. yields |Δv|≲0.5*|v|).
    float mass = tree_field_get_float(src, "mass");
    if (mass <= 0.f) mass = tree_field_get_float(src, "chassis_mass");
    if (mass <= 1e-3f) mass = 1200.f;
    const float inv_m = 1.f / mass;
    float dvx = jx * inv_m;
    float dvy = jy * inv_m;
    float dvz = jz * inv_m;
    // Soft safety vs flt_5F0C80@0x5F0C80=0.25 (PE clamp @ 0x456c97..cab).
    const float v2 = r.vx * r.vx + r.vy * r.vy + r.vz * r.vz;
    constexpr float kQuarter = 0.25f;
    const float max2 = v2 * kQuarter;
    const float d2 = dvx * dvx + dvy * dvy + dvz * dvz;
    if (max2 > 1e-12f && d2 > max2) {
      const float s = std::sqrt(max2 / d2);
      dvx *= s;
      dvy *= s;
      dvz *= s;
    }
    r.vx += dvx;
    r.vy += dvy;
    r.vz += dvz;
    // Soft τ stand-in for Phys_accumForceAtLocalPoint a3=drag_center:
    // ω += r_local × Δv (unit I≈m OOS — no inertia tensor @ body+0x108).
    const float rx = tree_field_get_float(src, "aero_force_px");
    const float ry = tree_field_get_float(src, "aero_force_py");
    const float rz = tree_field_get_float(src, "aero_force_pz");
    r.wx += (ry * dvz - rz * dvy);
    r.wy += (rz * dvx - rx * dvz);
    r.wz += (rx * dvy - ry * dvx);
    tree_field_set_int(src, "aero_applied", 0);
    ++g_phys_aero_hits;
    (void)dt;
    return;
  }

  // Path B — fallback when Chassis tick hasn't written side-band this
  // frame: TREE force_update_drag_* (forceUpdate @ hdr+0x8FC..+0x908).
  // PE gate C_drag<=0 vs flt_5E73CC=0 @ 0x456a83..0x456a94.
  const float cd = tree_field_get_float(src, "force_update_drag_c");
  if (!(cd > 0.f)) return;
  // Soft: drag_center xyz side-band read — torque arm OOS on fallback
  // (Phys_rotateLocalVec @ 0x4A7190). Consumed only on Path A.
  (void)tree_field_get_float(src, "force_update_drag_x");
  (void)tree_field_get_float(src, "force_update_drag_y");
  (void)tree_field_get_float(src, "force_update_drag_z");
  const float v2 = r.vx * r.vx + r.vy * r.vy + r.vz * r.vz;
  // PE |v|^2 <= float_1_0@0x5F08F0 → skip @ 0x456ac8.
  if (v2 <= 1.f) return;
  const float speed = std::sqrt(v2);
  // F = -Cd * |v|^2 * v_hat ≡ -Cd * |v| * v  (@ 0x456b05 fchs).
  float fx = -cd * speed * r.vx;
  float fy = -cd * speed * r.vy;
  float fz = -cd * speed * r.vz;
  // Soft unit-mass impulse = F*dt (PE inv-mass / inertia path OOS).
  float ix = fx * dt;
  float iy = fy * dt;
  float iz = fz * dt;
  // Soft clamp @ 0x456c34..0x456cab: PE mixes F*dt with vel*flt_5F09D0(0.5)
  // then caps vs vel²*flt_5F0C80(0.25). Host: |imp|² ≤ 0.25*|v|² (vel*0.5
  // blend needs inv-mass — OOS).
  constexpr float kQuarter = 0.25f;  // flt_5F0C80 @ 0x5F0C80
  const float max2 = v2 * kQuarter;
  const float i2 = ix * ix + iy * iy + iz * iz;
  if (max2 > 1e-12f && i2 > max2) {
    const float s = std::sqrt(max2 / i2);
    ix *= s;
    iy *= s;
    iz *= s;
  }
  r.vx += ix;
  r.vy += iy;
  r.vz += iz;
  ++g_phys_aero_hits;
}

void physics_integrate(float dt) {
  // PE @ 0x4a5197..0x4a51ae: v3=a2+*(world+4); if v3 < 1e-4 return 0.
  // Soft: world+4 accum always cleared after a taken step → gate ≡ dt
  // (flt_5F09D4@0x5F09D4 bytes 17 b7 d1 38 ≈ 1e-4).
  if (dt < 1e-4f) return;
  g_collide_events = 0;
  ++g_phys_step_frame;       // PE ++dword_6439C0 @ 0x4a520c
  g_phys_solver_iters = 0;   // PE *(world+16)=0 @ 0x4a5214
  g_phys_aero_hits = 0;

  // Soft body-list census (PE walks @ 0x4a5230 / 0x4a525f / 0x4a52f2).
  // +132: dynamic shaped bodies (prep sub_4A3F90 + integrate sub_4A3BC0).
  // +156: asleep/static with shape — cull candidates (vtbl+12 < 0 OOS).
  // +184: collide-enabled dynamics — live-check candidates (vtbl+24 OOS).
  // sub_426F60 @ 0x4a52d0 (g_EngineState animate) — OOS no-op.
  // Physics_stepResetPending wipe @ world+280 OOS (no host dllist).
  int n132 = 0, n156 = 0, n184 = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& kv : g_res) {
      const auto& r = kv.second;
      if (r.shape == 0) continue;
      if (r.is_static || r.asleep) {
        ++n156;
        continue;
      }
      ++n132;
      if (r.collide) ++n184;
    }
  }
  g_phys_list132 = n132;
  g_phys_list156 = n156;
  g_phys_list184 = n184;

  // Soft substep: PE one integrate pass per Physics_Step call; outer
  // SimulateFrame already slices by *(world+8)≈0.05. Internal kSub=4 was
  // over-subdividing — use 1 when dt already ≤ soft max, else ceil.
  constexpr float kPeMaxSub = 0.05f;
  int kSub = 1;
  if (dt > kPeMaxSub) {
    kSub = static_cast<int>(dt / kPeMaxSub + 0.999f);
    if (kSub < 2) kSub = 2;
    if (kSub > 4) kSub = 4;  // soft cap (was fixed 4)
  }
  const float sdt = dt / static_cast<float>(kSub);
  // Soft *(world+16) iter tally — PE solver loop max 20 @ 0x4a5455 OOS
  // (sub_4A3900 / sub_4A2D10 / sub_4A2540). Host: collide+Euler only.
  struct Step {
    InvObject* key;
    float px, py, pz, oy, op, or_, sx, sy, sz;
  };
  std::vector<Step> steps;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (int sub = 0; sub < kSub; ++sub) {
      // Soft +132 prep (sub_4A3F90 @ 0x4a5235) OOS — resolve-before-move.
      physics_resolve_collisions();
      ++g_phys_solver_iters;
      for (auto& kv : g_res) {
        auto& r = kv.second;
        if (r.shape == 0) continue;
        if (r.is_static || r.asleep) continue;
        // Soft aero before Euler ≡ PE wheel-tick force accum → Step
        // integrate. Path A consumes Chassis aero_impulse_* once (clears
        // aero_applied); Path B Cd fallback uses sdt each substep.
        physics_apply_chassis_aero(kv.first, r, sdt);
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
      // Soft +132 integrate re-walk (sub_4A3BC0 @ 0x4a52e6 / 0x4a543d) —
      // post-move collide stands in; no PE constraint solver.
      physics_resolve_collisions();
      ++g_phys_solver_iters;
    }
    for (auto& kv : g_res) {
      auto& r = kv.second;
      if (r.shape == 0) continue;
      if (r.is_static || r.asleep) continue;
      steps.push_back(Step{kv.first, r.px, r.py, r.pz, r.oy, r.op, r.or_, r.sx,
                           r.sy, r.sz});
    }
  }
  // Soft +184 live-check / +132 finalize (sub_4A4ED0) OOS — pose sync only.
  for (const Step& s : steps)
    render_d3d9_mesh_set_transform(s.key, s.px, s.py, s.pz, s.oy, s.op, s.or_,
                                   s.sx, s.sy, s.sz);
  // PE *(physWorld+0) += v22 @ 0x4a548e; flt_6439C4 EMA OOS.
  g_phys_world_clock += dt;
  (void)g_phys_step_frame;
  (void)g_phys_list132;
  (void)g_phys_list156;
  (void)g_phys_list184;
  (void)g_phys_solver_iters;
  (void)g_phys_aero_hits;
  (void)g_phys_world_clock;
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


// GroundRef route natives → GroundRef_route.cpp

// ---- SfxRef ----
// nplay @ 0x00480D40 — body in Audio/Sound.cpp (Sfx_3DListenerCull).

// PE @ 0x00480E60 size 0x7b (int_convert 123). UnboxArg (I)V: dest0=this
// (arg_0), dest1=instance (var_4). Native.ptr dword_62E008; 0 → "!Mighty
// ERROR" then ret (void). Else thiscall SfxRef_stopInstance @ 0x0048D180
// (ecx=handle, push instance): inner=[handle+0xC]; type!=1 → vtable+0x14
// (1.0f); sub_5447D0(inner, 0x80000000, 0, 0); sign → skip; else
// vtable+0xC(1.0f) → payload; voice=*(payload+0xC); voice &&
// *(voice+0x48)>=0 → sub_550870(instance, *[handle+8] rid). Gate is
// Native.ptr — NOT id ([handle+8]). Host: !self / no g_res = handle 0
// (silent); else audio_sfx_stop(instance) stand-in (no load walk /
// voice+0x48 mixer field — race124 still OOS, no invent).
// race125: IDA renamed Sfx_stopVoiceInstance @ 0x550870; host still
// audio_sfx_stop stand-in (no voice+0x48 gate).
void java_util_resource_SfxRef_stop(InvObject* self, int32_t instance) {
  // PE @ 0x00480E60 size 0x7b — Unbox this+I; Native.ptr → SfxRef_stopInstance
  // @ 0x0048D180 → Sfx_stopVoiceInstance @ 0x00550870.
  if (!self) return;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_res.find(self) == g_res.end()) return;  // Native.ptr==0
  }
  audio_sfx_stop(instance);
}

// ---------------------------------------------------------------------------
// W9A / W11B / W12B — PE Engine_LoadGameInit @ 0x0053A5E0 size 0x51f.
// cdecl (a1=wtroot_rh, a2=type_rh, a3=params, a4=alias) → GI*|0.
// Callers: voidEvent case2 @ 0x45C3BD, GameRef_create @ 0x47D824,
// Engine_MainLoop @ 0x4289E5, GroundRef_addTrafficN @ 0x4841C4, …
//
// PE factory chain (IDA W11B/W12B):
//   ResourceEngine_AllocLocalRid(g_ResourceEngine@618D48) @ 0x536ED0
//   create desc {type=1, rid, 33, 0, 0, alias*} @ 0x53A68E
//   wtroot+0xC!=0 → CreateNodeUnder @ 0x536900 (a6=parent)
//   wtroot+0xC==0 → CreateNode @ 0x536890 → CreateNodeByDesc a6=0
//     → ResourceEngine_ResolveParent(wt_key, type=1, 0) @ 0x537000
//     → InstanceNode_PoolAlloc @ 0x538720 + ctor @ 0x53E6E0
//     → TREE splice @ 0x536DFF (parent+0x38 / sentinel parent+0x30)
//   parent LOD @ 0x53A706; GI+0x54 |= 0x200 @ 0x53A76D
//
// Host: PoolAlloc + TREE + ResolveParent RID slices (cache / FindByRid).
// W13B: eng+0x11C SimObject root + Class*→FQN → attach_gametype.
// W15C: parent LOD @ 0x53A706 — PrepareLod gate + setLodReadyBit stand-in.
// Gaps: GameType ctor vtbl+0xC @ 0x53A8B2; sub_537240 non-early / PE vtbl
//   dispatch (host GI has no ResNode_vtbl*).
// ---------------------------------------------------------------------------

namespace {

// PE InstanceNode slot 348 (0x15C) — PoolAlloc @ 0x538720.
// ResHandle_Rebind owner (@ 0x00429060 a2):
//   +0x4/+0x8 dllist; +0x14 parent; +0x20→&+0x30 (FindByRid walk);
//   +0x30 sentinel / +0x38 head (TREE); +0x48 RH HEAD; +0x4C type;
//   +0x50 AllocLocalRid key; +0x54 flags; +0x60 alias*; +0x68 lod.
struct HostLoadGameInitGi {
  void* vtbl = nullptr;             // +0x0
  void* list_prev = nullptr;        // +0x4  → parent+0x30 sentinel
  void* list_next = nullptr;        // +0x8  old child head
  uint8_t pad_0c[0x8]{};            // +0xC .. +0x13
  void* parent = nullptr;           // +0x14 PE v9[5]
  uint8_t pad_18[0x8]{};            // +0x18 .. +0x1F
  void* find_by_rid_head = nullptr; // +0x20 PE [this+0x20] → &+0x30
  uint8_t pad_24[0xC]{};            // +0x24 .. +0x2F
  void* child_sentinel = nullptr;   // +0x30 PE list sentinel (addr taken)
  void* pad_34 = nullptr;           // +0x34
  void* child_head = nullptr;       // +0x38 PE v10[14] / init → &+0x1C
  uint8_t pad_3c[0xC]{};            // +0x3C .. +0x47
  void* rh_list_head = nullptr;     // +0x48
  int32_t type = 1;                 // +0x4C INSTANCE
  int32_t rebind_key = 0;           // +0x50 → rh+8 after Rebind
  int32_t flags = 0;                // +0x54
  int32_t desc_pad_c = 0;           // +0x58
  int32_t desc_pad_d = 0;           // +0x5C
  char* alias = nullptr;            // +0x60
  int32_t lod_state = 0;            // +0x64 PE byte&1 PrepareLod gate
  float lod_scale = 0.f;            // +0x68 PE v13[26]
  float lod_copy_6c = 0.f;          // +0x6C PE parent+108
  float lod_copy_70 = 0.f;          // +0x70 PE parent+112
  uint8_t pad_74_80[0xC]{};         // +0x74 .. +0x7F
  // W21D PE ResNode_unionChildAabb @ 0x4988A0: state/center/half.
  int32_t aabb_state = 0;           // +0x80; 1=ready; 2=skip contrib
  float aabb_cx = 0.f;              // +0x84
  float aabb_cy = 0.f;              // +0x88
  float aabb_cz = 0.f;              // +0x8C
  float aabb_hx = 0.f;              // +0x90 half-extent
  float aabb_hy = 0.f;              // +0x94
  float aabb_hz = 0.f;              // +0x98
  // +0x9C .. +0xD7: initExt +0xB8 bias / +0xBC scale (before embed mid).
  uint8_t pad_9c_d8[0xD8 - 0x9C]{};
  // PE ResNode_getEmbeddedMid @ 0x53EFA0 → this+0xD8 (slot mid size 0x84).
  // W19C: mid+0x38 ResHandle_Rebind type owner.
  // W20A: mid+0x50 script (a3) @ 0x53A515.
  uint8_t mid_embed[0x84]{};
};
static_assert(offsetof(HostLoadGameInitGi, list_prev) == 0x4, "GI+0x4");
static_assert(offsetof(HostLoadGameInitGi, parent) == 0x14, "GI+0x14");
static_assert(offsetof(HostLoadGameInitGi, find_by_rid_head) == 0x20,
              "GI+0x20 FindByRid");
static_assert(offsetof(HostLoadGameInitGi, child_sentinel) == 0x30, "GI+0x30");
static_assert(offsetof(HostLoadGameInitGi, child_head) == 0x38, "GI+0x38");
static_assert(offsetof(HostLoadGameInitGi, rh_list_head) == 0x48, "GI+0x48");
static_assert(offsetof(HostLoadGameInitGi, type) == 0x4C, "GI+0x4C");
static_assert(offsetof(HostLoadGameInitGi, rebind_key) == 0x50, "GI+0x50");
static_assert(offsetof(HostLoadGameInitGi, flags) == 0x54, "GI+0x54");
static_assert(offsetof(HostLoadGameInitGi, alias) == 0x60, "GI+0x60");
static_assert(offsetof(HostLoadGameInitGi, lod_scale) == 0x68, "GI+0x68");
static_assert(offsetof(HostLoadGameInitGi, aabb_state) == 0x80, "GI+0x80 AABB");
static_assert(offsetof(HostLoadGameInitGi, aabb_cx) == 0x84, "GI+0x84");
static_assert(offsetof(HostLoadGameInitGi, aabb_hx) == 0x90, "GI+0x90");
static_assert(offsetof(HostLoadGameInitGi, mid_embed) == 0xD8, "GI+0xD8 mid");
static_assert(sizeof(HostLoadGameInitGi) == 348, "InstanceNode slot 348");

std::vector<HostLoadGameInitGi*> g_loadgameinit_gis;
// PE ResourceEngine_AllocLocalRid @ 0x536ED0: u16 freelist @ eng+0xDA0,
// refill 128, return rid|0xFFFF0000 (empty → 0xFFFF0000). Host seq stand-in.
uint16_t g_lgi_rid_seq = 1;
// g_lgi_create_count — PE @ 0x765F60; defined with GCSweep soft lives (W32C).

// PE ResolveParent RID cache @ eng+0x106E9C (active) / +0x106E94 (MRU).
// Entry: +0x14 rid, +0x18 node*. Host: flat rid→node map (hold g_mu).
std::unordered_map<int32_t, void*> g_resolve_parent_rid;
// PE pending miss RIDs @ eng+0x106ECC count / +0x106ED0 ptr (a4&1==0).
std::vector<int32_t> g_resolve_parent_pending;

// PE InstanceNode_PoolAlloc @ 0x538720 (g_InstanceNodePool @ 0x765F70):
// freelist @ pool+4; else slab index, stride 348, batch 1024 (malloc 0x57000).
struct HostInstanceNodePool {
  void* freelist = nullptr;
  std::vector<std::vector<uint8_t>> slabs;
  uint32_t used = 0;
  uint32_t capacity = 0;
};
HostInstanceNodePool g_inode_pool;

uint32_t host_alloc_local_rid() {
  uint16_t rid = g_lgi_rid_seq++;
  if (rid == 0) rid = g_lgi_rid_seq++;
  return 0xFFFF0000u | static_cast<uint32_t>(rid);
}

HostLoadGameInitGi* host_instance_node_pool_alloc() {
  constexpr uint32_t kBatch = 1024;
  constexpr size_t kSlot = 348;
  HostLoadGameInitGi* gi = nullptr;
  if (g_inode_pool.freelist) {
    auto* slot = reinterpret_cast<HostLoadGameInitGi*>(g_inode_pool.freelist);
    g_inode_pool.freelist = *reinterpret_cast<void**>(slot);
    gi = new (slot) HostLoadGameInitGi{};
  } else {
    if (g_inode_pool.used == g_inode_pool.capacity) {
      g_inode_pool.slabs.emplace_back(kBatch * kSlot);
      g_inode_pool.capacity += kBatch;
    }
    const uint32_t idx = g_inode_pool.used++;
    const uint32_t slab_i = idx / kBatch;
    const uint32_t slot_i = idx % kBatch;
    uint8_t* raw = g_inode_pool.slabs[slab_i].data() + slot_i * kSlot;
    gi = new (raw) HostLoadGameInitGi{};
  }
  if (gi) {
    // PE InstanceNode_initExt @ 0x544D40: +0xB8 bias=0, +0xBC scale=1.0
    // (ResNode_setLodCopy6C/70 formula). pad_74_d8 / mid_embed otherwise 0.
    auto* p = reinterpret_cast<uint8_t*>(gi);
    *reinterpret_cast<float*>(p + 0xB8) = 0.f;
    *reinterpret_cast<float*>(p + 0xBC) = 1.0f;
  }
  return gi;
}

// PE sub_551750 Node base: [+0x20]=&+0x30; [+0x38]=&+0x1C (FindByRid/TREE).
void host_node_ensure_child_list_hdr(uint8_t* p) {
  if (!p) return;
  void** p38 = reinterpret_cast<void**>(p + 0x38);
  if (*p38) return;
  *reinterpret_cast<void**>(p + 0x20) = p + 0x30;
  *reinterpret_cast<void**>(p + 0x30) = nullptr;
  *reinterpret_cast<void**>(p + 0x34) = nullptr;
  *p38 = p + 0x1C;
}

// PE ResourceEngine_CreateNodeByDesc TREE register @ 0x536DFF:
//   node[+0x14]=parent; old=parent[+0x38]; old[+4]=node;
//   node[+4]=&parent[+0x30]; node[+8]=old; parent[+0x38]=node.
void host_create_node_by_desc_tree_register(HostLoadGameInitGi* node,
                                           void* parent) {
  if (!node || !parent) return;
  auto* n = reinterpret_cast<uint8_t*>(node);
  auto* p = reinterpret_cast<uint8_t*>(parent);
  host_node_ensure_child_list_hdr(p);
  host_node_ensure_child_list_hdr(n);
  *reinterpret_cast<void**>(n + 0x14) = parent;
  void* old = *reinterpret_cast<void**>(p + 0x38);
  if (old) {
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(old) + 4) = node;
  }
  *reinterpret_cast<void**>(n + 4) = p + 0x30;
  *reinterpret_cast<void**>(n + 8) = old;
  *reinterpret_cast<void**>(p + 0x38) = node;
  node->parent = parent;
}

// W21D PE ResNode_unionChildAabb @ 0x4988A0 size 0x267 — no callees.
// Walk FindByRid chain: start=*(node+0x20) if *(start+4)!=0; next via +4
// until next==0 or *(next+4)==0. Contrib when type(+0x4C)==2 &&
// aabb_state(+0x80)!=2: center±half @ +0x84/+0x90 → union min/max.
// If any contrib: state=1; write center=(min+max)/2, half=(max-min)/2.
void host_res_node_union_child_aabb(void* node) {
  if (!node) return;
  auto* a1 = reinterpret_cast<uint8_t*>(node);
  void* head = *reinterpret_cast<void**>(a1 + 0x20);
  if (!head) return;
  void* cur =
      (*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(head) + 4) !=
       nullptr)
          ? head
          : nullptr;
  bool have = false;
  float mn_x = 0.f, mn_y = 0.f, mn_z = 0.f;
  float mx_x = 0.f, mx_y = 0.f, mx_z = 0.f;
  while (cur) {
    auto* c = reinterpret_cast<uint8_t*>(cur);
    if (*reinterpret_cast<int32_t*>(c + 0x4C) == 2 &&
        *reinterpret_cast<int32_t*>(c + 0x80) != 2) {
      const float cx = *reinterpret_cast<float*>(c + 0x84);
      const float cy = *reinterpret_cast<float*>(c + 0x88);
      const float cz = *reinterpret_cast<float*>(c + 0x8C);
      const float hx = *reinterpret_cast<float*>(c + 0x90);
      const float hy = *reinterpret_cast<float*>(c + 0x94);
      const float hz = *reinterpret_cast<float*>(c + 0x98);
      const float lo_x = cx - hx;
      const float lo_y = cy - hy;
      const float lo_z = cz - hz;
      const float hi_x = cx + hx;
      const float hi_y = cy + hy;
      const float hi_z = cz + hz;
      if (!have) {
        mn_x = lo_x;
        mn_y = lo_y;
        mn_z = lo_z;
        mx_x = hi_x;
        mx_y = hi_y;
        mx_z = hi_z;
        have = true;
      } else {
        if (lo_x < mn_x) mn_x = lo_x;
        if (hi_x > mx_x) mx_x = hi_x;
        if (lo_y < mn_y) mn_y = lo_y;
        if (hi_y > mx_y) mx_y = hi_y;
        if (lo_z < mn_z) mn_z = lo_z;
        if (hi_z > mx_z) mx_z = hi_z;
      }
    }
    cur = *reinterpret_cast<void**>(c + 4);
    if (!cur ||
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cur) + 4) ==
            nullptr) {
      cur = nullptr;
    }
  }
  if (!have) return;
  *reinterpret_cast<int32_t*>(a1 + 0x80) = 1;
  const float hx = (mx_x - mn_x) * 0.5f;
  const float hy = (mx_y - mn_y) * 0.5f;
  const float hz = (mx_z - mn_z) * 0.5f;
  *reinterpret_cast<float*>(a1 + 0x84) = mn_x + hx;
  *reinterpret_cast<float*>(a1 + 0x88) = mn_y + hy;
  *reinterpret_cast<float*>(a1 + 0x8C) = mn_z + hz;
  *reinterpret_cast<float*>(a1 + 0x90) = hx;
  *reinterpret_cast<float*>(a1 + 0x94) = hy;
  *reinterpret_cast<float*>(a1 + 0x98) = hz;
}

// W22D PE ResNode_drainSpatial @ 0x498B10 size 0x260 — cdecl(node) → 0|1.
// CreateGI while @ 0x53A54C / LoadGameInit @ 0x53AA40 until 0.
// Slice: aabb_state(+0x80)==2 → 0; parent climb +0x14 stop on
//   !grandparent | flags(+0x54)&0x30 | either aabb==2 (v5=14);
//   AABB sub_54B850/54BA20 OOS → no further climb. RID(+0x50) same → 0.
// Gaps: GameRef_dllist_unlink@4A5D00 + sub_4A5D20 reparent;
//   Type53_ensureHandleSlot@4B3EE0; sibling SimObjectList@parent+0x18;
//   AllocLocalRid@536ED0 + sub_5447B0.
int32_t host_res_node_drain_spatial(void* node) {
  if (!node) return 0;
  auto* a1 = reinterpret_cast<uint8_t*>(node);
  // PE @ 0x498b27: aabb_state==2 → return 0.
  if (*reinterpret_cast<int32_t*>(a1 + 0x80) == 2) return 0;

  void* v2 = *reinterpret_cast<void**>(a1 + 0x14);  // parent
  if (!v2) return 0;  // stock assumes parent; host safe-exit

  // PE climb @ 0x498b32..0x498ba1.
  for (;;) {
    auto* p = reinterpret_cast<uint8_t*>(v2);
    void* grand = *reinterpret_cast<void**>(p + 0x14);
    if (!grand || (*reinterpret_cast<int32_t*>(p + 0x54) & 0x30) != 0)
      break;
    // PE: parent|self aabb_state==2 → v5=14 (bit8) → break.
    const int32_t p_aabb = *reinterpret_cast<int32_t*>(p + 0x80);
    const int32_t s_aabb = *reinterpret_cast<int32_t*>(a1 + 0x80);
    if (p_aabb == 2 || s_aabb == 2) break;
    // AABB helpers OOS — stop without climbing (as if contained / bit8).
    break;
  }

  void* cur_parent = *reinterpret_cast<void**>(a1 + 0x14);
  if (!cur_parent) return 0;
  // PE @ 0x498bae: walked RID != current parent RID → reparent path.
  if (*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(v2) + 0x50) !=
      *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(cur_parent) +
                                  0x50)) {
    // reparent + Type53_ensureHandleSlot OOS
    return 0;
  }
  // Sibling SimObjectList / AllocLocalRid path OOS → return 0 (v16).
  return 0;
}

// host_lgi_set_lod_ready_bit — PE @ 0x544B90; defined near PumpLoad (W22A).

// PE ResHandle_PrepareLod @ 0x5447D0 — LoadGameInit call site a2=0,a3=0,a4=0.
// W16A/W17A: RelinkLodSlot (tryRelinkLod + freelist/OR) for maybeRelinkLod;
//   PrepareLod→RelinkLodSlot still OOS here (a2 bit29 / +0x54&0x100 gate).
//   W18B: PumpUnloadQueue mark+recycle freelist hosted;
//   W28C: SimObjectList ctor/empty + GCSweep(0) soft counters;
//   W29C: type-bucket touch@+0x150 / age→aged@+0x16C / typeGate scan shell;
//   W30C: wantUnloadFrame@53EFB0 (+0x78 stamp) / vtbl+0x30 null drain.
//   W31C: mid_gameTypeUnloadUpdate@53E620 → this PrepareLod soft.
//   W32C: GCSweep force eng+0x106ED8..EE4 + pre-scan budget Hi.
//   W33C: GCSweep touch setLodCopy6C(0) + evict move/Lo/caps soft.
//   W34: ResNode_tryUnload@53EC20 soft gate/children/lod-ready.
//   W35-03: TouchResNode@537D10 soft on success (stamp≠frame, !(flags&0x40M)).
//   W19A: PumpLoadQueue empty+LoadRing/isLoading; W20B LABEL_28 recycle;
//   W21A/W22A/W23A vtbl+0x28@53E860 LABEL_68 + parseLodBuf body slice.
// Full body (scale table dword_619DC0) OOS. Stand-in:
//   sub_537240 early @ 0x53725a when this+0x5C==0 → setLodReadyBit(1)+lod=1.0;
//   return 0 if (this+0x64)&1 else 0x80000001; Touch on success.
int32_t host_lgi_prepare_lod(void* node, int32_t flags, float a3, float a4) {
  (void)a3;
  (void)a4;
  if (!node) return static_cast<int32_t>(0x80000001u);
  auto* p = reinterpret_cast<uint8_t*>(node);
  int32_t* lod_st = reinterpret_cast<int32_t*>(p + 0x64);
  if ((flags & 0x40000000) == 0) {
    const bool need =
        (*lod_st & 1) == 0;  // PE also re-enters on scale mismatch — OOS
    if (need) {
      // PE sub_537240 early path when dword[23] (+0x5C) == 0.
      if (*reinterpret_cast<int32_t*>(p + 0x5C) == 0) {
        host_lgi_set_lod_ready_bit(node, 1);
        *reinterpret_cast<float*>(p + 0x68) = 1.0f;
      }
      // else: full LOD apply OOS — leave bit0 clear → fail below
    }
  }
  // @ 0x5449C8: !(+0x64&1) → 0x80000001.
  if ((*lod_st & 1) == 0) return static_cast<int32_t>(0x80000001u);
  // @ 0x5449DD..0x5449F7: !(flags&0x40000000) && stamp≠frame → Touch.
  if ((flags & 0x40000000) == 0 &&
      node_gc_stamp(static_cast<HostResNode*>(node)) != g_res_gc_frame)
    resource_engine_touch_res_node(static_cast<HostResNode*>(node));
  return 0;
}

// PE @ 0x53A706..0x53A764: gi vtbl+0x18/+0x14 parent floats; +0x68 copy;
// if !(parent+0x64&1): type!=1 → parent vtbl+0x14(1.0); PrepareLod(0,0,0);
// ≥0 → parent vtbl+0xC(1.0).
// ResNode_vtbl: +0x18=setLodCopy70@53F040, +0x14=setLodCopy6C@53EFF0,
//   +0xC=getEmbeddedMid@53EFA0. Formula a2*(+0xBC)+(+0xB8); initExt seeds
//   +0xB8=0,+0xBC=1.0 → direct float write ≡ vtbl hop.
void host_lgi_parent_lod_copies(HostLoadGameInitGi* gi, void* parent) {
  if (!gi || !parent) return;
  auto* pb = reinterpret_cast<uint8_t*>(parent);
  // PE gi vtbl+0x18(parent+0x70) / vtbl+0x14(parent+0x6C); +0x68 direct.
  gi->lod_copy_70 = *reinterpret_cast<float*>(pb + 0x70);
  gi->lod_copy_6c = *reinterpret_cast<float*>(pb + 0x6C);
  gi->lod_scale = *reinterpret_cast<float*>(pb + 0x68);

  int32_t* pst = reinterpret_cast<int32_t*>(pb + 0x64);
  if ((*pst & 1) != 0) return;  // PE @ 0x53A72F already ready → skip

  // PE @ 0x53A735: parent+0x4C!=1 → parent vtbl+0x14(1.0f).
  if (*reinterpret_cast<int32_t*>(pb + 0x4C) != 1) {
    const float bias = *reinterpret_cast<float*>(pb + 0xB8);
    const float scale = *reinterpret_cast<float*>(pb + 0xBC);
    *reinterpret_cast<float*>(pb + 0x6C) = 1.0f * scale + bias;
  }

  // PE @ 0x53A74F PrepareLod(parent,0,0,0); ≥0 → vtbl+0xC(1.0f).
  if (host_lgi_prepare_lod(parent, 0, 0.f, 0.f) >= 0) {
    // INSTANCE type1: getEmbeddedMid no-op side effect. Non-INSTANCE PE
    // vtbl+0xC payload hop OOS (host parent may lack ResNode_vtbl).
    (void)pb;
  }
}

static int32_t host_node_type_at(void* node) {
  if (!node) return 0;
  return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(node) + 0x4C);
}

static int32_t host_node_rid_at(void* node) {
  if (!node) return 0;
  return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(node) + 0x50);
}

// PE InstanceNode_FindByRid @ 0x544AC0 / FindByRidType @ 0x544B10:
// match [node+0x50]==rid; type filter [node+0x4C]; children via [+0x20]/+4.
// Host: recursive walk when +0x20 wired; else flat scan of known nodes.
void* host_find_by_rid_walk(void* node, int32_t rid, int32_t type, int depth) {
  if (!node || rid == 0 || depth > 4096) return nullptr;
  if (host_node_rid_at(node) == rid) {
    if (type != 0 && host_node_type_at(node) != type) return nullptr;
    return node;
  }
  auto* p = reinterpret_cast<uint8_t*>(node);
  void* cur = *reinterpret_cast<void**>(p + 0x20);
  if (!cur) return nullptr;
  // PE: require [cur+4]!=0 before walking (sentinel empty gate).
  if (*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cur) + 4) == nullptr)
    return nullptr;
  for (int guard = 0; cur && guard < 4096; ++guard) {
    void* next = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cur) + 4);
    if (!next) break;
    if (void* hit = host_find_by_rid_walk(cur, rid, type, depth + 1))
      return hit;
    cur = next;
    if (*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cur) + 4) ==
        nullptr)
      break;
  }
  return nullptr;
}

// Hold g_mu. Flat fallback when eng+0x11C root walk misses.
void* host_find_by_rid_scan(int32_t rid, int32_t type) {
  if (!rid) return nullptr;
  for (HostLoadGameInitGi* gi : g_loadgameinit_gis) {
    if (!gi || gi->rebind_key != rid) continue;
    if (type != 0 && gi->type != type) continue;
    return gi;
  }
  for (auto& kv : g_native_ptr) {
    HostNativeHandle* h = kv.second;
    if (!h || !h->node) continue;
    if (host_node_rid_at(h->node) != rid) continue;
    if (type != 0 && host_node_type_at(h->node) != type) continue;
    return h->node;
  }
  return nullptr;
}

// PE g_ResourceEngine+0x11C SimObject root (ResolveParent miss @ 0x53708d:
// !SimObjectListEmpty(eng+0x114) && *(eng+0x11C) → FindByRid @ 0x544AC0).
// Host: single InstanceNode-shaped root; orphans TREE-spliced under it.
HostLoadGameInitGi* g_eng_sim_object_root = nullptr;

HostLoadGameInitGi* host_eng_sim_object_root() {
  if (g_eng_sim_object_root) return g_eng_sim_object_root;
  g_eng_sim_object_root = host_instance_node_pool_alloc();
  if (!g_eng_sim_object_root) return nullptr;
  host_node_ensure_child_list_hdr(
      reinterpret_cast<uint8_t*>(g_eng_sim_object_root));
  g_eng_sim_object_root->type = 0;  // root sentinel (not INSTANCE)
  g_eng_sim_object_root->rebind_key = 0;
  return g_eng_sim_object_root;
}

// Attach node under eng+0x11C when it has no parent (top-level forest).
void host_sim_object_root_adopt(HostLoadGameInitGi* node) {
  if (!node || node->parent) return;
  HostLoadGameInitGi* root = host_eng_sim_object_root();
  if (!root || node == root) return;
  host_create_node_by_desc_tree_register(node, root);
}

void host_rid_register(int32_t rid, void* node) {
  if (!rid || !node) return;
  g_resolve_parent_rid[rid] = node;
}

// PE Class_getNameCstr @ 0x404EA0:
//   mov eax,[ecx+8] / mov eax,[eax] / ret  — FQN C-string via Class+0x8.
// Host Class shell (class_box_object): tree fields "name" / "clazzname".
const char* host_class_get_name_cstr(void* clazz) {
  if (!clazz) return nullptr;
  auto* obj = reinterpret_cast<InvObject*>(clazz);
  // Host java.lang.Class shell (side-map) before PE +0x8 deref.
  if (const char* hc = tree_host_class(obj)) {
    if (hc[0] && std::strcmp(hc, "java.lang.Class") == 0) {
      if (InvObject* name = tree_field_get_obj(obj, "name")) {
        if (const char* s = string_cstr(name)) {
          if (s[0]) return s;
        }
      }
      if (InvObject* cn = tree_field_get_obj(obj, "clazzname")) {
        if (const char* s = string_cstr(cn)) {
          if (s[0]) return s;
        }
      }
      return nullptr;
    }
  }
  // PE Class*: [Class+0x8] → char*.
  void* name_slot =
      *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(clazz) + 8);
  if (!name_slot) return nullptr;
  const char* pe = *reinterpret_cast<const char**>(name_slot);
  return (pe && pe[0]) ? pe : nullptr;
}

// PE LoadGameInit @ 0x53A7BD..0x53A908:
//   type_rh+8!=0 → Rebind embed to *(type_rh+0xC) → getPayload(tag 0x80000000)
//   → Class* at payload+0x10 → Class_boxObject; host needs FQN for attach.
// Host: HostResNode.mid.script_class (+0x10) = Class* or FQN C-string.
const char* host_lgi_fqn_from_type_rh(void* type_rh) {
  if (!type_rh) return nullptr;
  auto* rh = reinterpret_cast<uint8_t*>(type_rh);
  if (*reinterpret_cast<int32_t*>(rh + 8) == 0) return nullptr;
  void* inner = *reinterpret_cast<void**>(rh + 0xC);
  if (!inner) return nullptr;

  auto* hn = reinterpret_cast<HostResNode*>(inner);
  void* at10 = hn->mid.script_class;  // PE GameType payload+0x10
  if (!at10) return nullptr;

  if (const char* fqn = host_class_get_name_cstr(at10)) return fqn;
  // Host may store FQN C-string directly at mid+0x10.
  auto* direct = reinterpret_cast<const char*>(at10);
  if (direct[0] >= 0x20 && direct[0] < 0x7F) return direct;
  return nullptr;
}

// PE LoadGameInit @ 0x53A8B2: ecx = *(type_payload+0xC) = mid.leaf (ctor*).
void* host_lgi_mid_from_type_rh(void* type_rh) {
  if (!type_rh) return nullptr;
  auto* rh = reinterpret_cast<uint8_t*>(type_rh);
  if (*reinterpret_cast<int32_t*>(rh + 8) == 0) return nullptr;
  void* inner = *reinterpret_cast<void**>(rh + 0xC);
  if (!inner) return nullptr;
  return &reinterpret_cast<HostResNode*>(inner)->mid;
}

// PE ResourceEngine_ResolveParent @ 0x537000 thiscall(eng, rid, type, flags).
// 1) Walk eng+0x106E9C cache: entry[+0x14]==rid; type→[node+0x4C]; return
//    entry[+0x18]. 2) Miss: FindByRid(eng+0x11C) @ 0x544AC0 / Type @ 0x544B10.
// 3) Miss+(flags&1)==0: push rid to eng+0x106ECC pending. Hold g_mu.
void* host_resolve_parent(int32_t rid, int32_t type, char flags) {
  if (!rid) return nullptr;

  auto it = g_resolve_parent_rid.find(rid);
  if (it != g_resolve_parent_rid.end() && it->second) {
    void* node = it->second;
    // PE @ 0x5370cd: a3!=0 → require [inner+0x4C]==a3.
    if (type != 0 && host_node_type_at(node) != type) return nullptr;
    return node;
  }

  // PE miss @ 0x53708d: !SimObjectListEmpty(eng+0x114) && *(eng+0x11C).
  void* found = nullptr;
  if (HostLoadGameInitGi* root = host_eng_sim_object_root()) {
    // PE FindByRid(this=*(eng+0x11C), rid) — walk forest from root.
    found = host_find_by_rid_walk(root, rid, type, 0);
  }
  if (!found) {
    for (HostLoadGameInitGi* gi : g_loadgameinit_gis) {
      if (!gi) continue;
      found = host_find_by_rid_walk(gi, rid, type, 0);
      if (found) break;
    }
  }
  if (!found) {
    for (auto& kv : g_native_ptr) {
      if (!kv.second || !kv.second->node) continue;
      found = host_find_by_rid_walk(kv.second->node, rid, type, 0);
      if (found) break;
    }
  }
  if (!found) found = host_find_by_rid_scan(rid, type);

  if (found) {
    g_resolve_parent_rid[rid] = found;
    return found;
  }

  // PE @ 0x5371f7: (a4 & 1)==0 → append rid to pending array.
  if ((flags & 1) == 0) g_resolve_parent_pending.push_back(rid);
  return nullptr;
}

// PE CreateNodeByDesc @ 0x536A10 — W26C host slice: case1 INSTANCE only
// when parent (a6) non-null. Cases 2..21 / ResolveParent(a6==0) OOS.
// CreateNodeUnder passes (a3=packId unused here, desc, xform, a7, parent).
void* host_create_node_by_desc_under_parent(const int32_t desc[5],
                                            const uint32_t* xform7_or_null,
                                            int /*a7*/, void* parent) {
  if (!desc || !parent) return nullptr;
  if (desc[0] != 1) return nullptr;  // non-INSTANCE ctors OOS
  HostLoadGameInitGi* gi = host_instance_node_pool_alloc();
  if (!gi) return nullptr;
  host_node_ensure_child_list_hdr(reinterpret_cast<uint8_t*>(gi));
  g_loadgameinit_gis.push_back(gi);
  // Node_initFromCreateDesc @ 0x54464D — memcpy 0x14 → +0x4C..+0x5C.
  gi->type = desc[0];
  gi->rebind_key = desc[1];
  gi->flags = desc[2];
  gi->desc_pad_c = desc[3];
  gi->desc_pad_d = desc[4];
  // InstanceNode_initExt @ 0x544DD7 — optional 0x1C xform → +0x80.
  if (xform7_or_null) {
    std::memcpy(reinterpret_cast<uint8_t*>(gi) + 0x80, xform7_or_null, 0x1C);
  }
  host_create_node_by_desc_tree_register(gi, parent);  // @ 0x536DFF
  if (desc[1] != 0) host_rid_register(desc[1], gi);
  return gi;
}

// PE ResourceEngine_CreateNodeUnder @ 0x536900 size 0x66 —
// __thiscall(eng, a2=remapped, a3, a4=desc*, a5=parent, a6=xform*|0, a7).
// HIWORD(a2)!=FFFF && <count → EnsureIndex(eng+0xFFE94+0x70*hi);
// CreateNodeByDesc(eng, a3, desc, xform, a7, parent); setLodReadyBit(node,0).
void* host_resource_engine_create_node_under(uint32_t remapped_id, uint32_t a3,
                                            const int32_t desc[5], void* parent,
                                            const uint32_t* xform7_or_null,
                                            int a7) {
  (void)a3;  // PE forwards to CreateNodeByDesc a2 (ResolveParent when !parent)
  host_pack_eng_sync_from_rpak();
  const uint32_t hi = remapped_id >> 16;  // @ 0x536904
  if (hi != 0xFFFFu && hi < g_pack_eng.pack_count && hi < kMaxPackSlots) {
    // @ 0x536919..0x53692C — eng+0xFFE94 + 0x70*hi
    (void)host_resource_pack_ensure_index(g_pack_eng.slots[hi].raw);
  }
  void* node = host_create_node_by_desc_under_parent(desc, xform7_or_null, a7,
                                                     parent);  // @ 0x53694C
  if (node) host_lgi_set_lod_ready_bit(node, 0);               // @ 0x53695B
  return node;
}

}  // namespace

// PE ResourceEngine_RemapFromPath @ 0x538440 — public Soft entry.
uint32_t resource_engine_remap_from_path(const char* path, uint32_t local) {
  return host_resource_engine_remap_from_path(path, local);
}

// PE Chassis_forceUpdate_cloneHdr lazy Remap+Bind @ 0x43EDD7..0x43EE56.
void resource_engine_clone_hdr_lazy_binds() {
  host_clone_hdr_lazy_remap_binds();
}

void* resource_engine_void_event_type_rh() {
  return &g_voidEvent_LoadGameInit_typeRH_soft;
}

uint32_t resource_engine_void_event_type_rh_id() {
  return static_cast<uint32_t>(
      reinterpret_cast<uintptr_t>(g_voidEvent_LoadGameInit_typeRH_soft.id));
}

uint32_t resource_engine_particles_rh_a_id() {
  return static_cast<uint32_t>(
      reinterpret_cast<uintptr_t>(g_RH_particles_rpk_A_soft.id));
}

uint32_t resource_engine_particles_rh_b_id() {
  return static_cast<uint32_t>(
      reinterpret_cast<uintptr_t>(g_RH_particles_rpk_B_soft.id));
}

void* engine_load_game_init(void* wtroot_rh, void* type_rh,
                            const char* params, const char* alias) {
  (void)params;
  // PE @ 0x53A640: if *(wtroot+8)==0 → return null (no create).
  if (!wtroot_rh) return nullptr;
  const auto* wt =
      reinterpret_cast<const uint8_t*>(wtroot_rh);
  const int32_t wt_key = *reinterpret_cast<const int32_t*>(wt + 8);
  if (wt_key == 0) return nullptr;

  // PE @ 0x53A5F8: type_rh+8!=0 → payload+0x4C RESTYPE==8 (GameType) or
  // log "Wrong GameType!" — warn-only, still creates. type_rh may be
  // g_voidEvent_LoadGameInit_typeRH @ 0x63C680 (caller-owned ResHandle).

  // PE create desc @ 0x53A68E: type=1, AllocLocalRid, +8=33, alias* @ +0x14.
  const uint32_t local_rid = host_alloc_local_rid();
  if (local_rid == 0) return nullptr;  // PE treats 0 as alloc fail

  // PE CreateNodeUnder when *(wtroot+0xC)!=0 (a6=parent); else CreateNode →
  // CreateNodeByDesc a6=0 → ResolveParent(wt_key, *desc=1, 0) @ 0x537000.
  void* parent = *reinterpret_cast<void* const*>(wt + 0xC);

  // PE case1 @ 0x536A38: InstanceNode_PoolAlloc + InstanceNode_ctor.
  HostLoadGameInitGi* gi = nullptr;
  const char* script_fqn = nullptr;
  void* type_mid = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    (void)host_eng_sim_object_root();  // seed eng+0x11C stand-in
    if (!parent) {
      // PE CreateNodeByDesc @ 0x536ad1: ResolveParent(a2=wt_key, a3=1, a4=0).
      parent = host_resolve_parent(wt_key, /*type=*/1, /*flags=*/0);
    }
    gi = host_instance_node_pool_alloc();
    if (!gi) return nullptr;
    host_node_ensure_child_list_hdr(reinterpret_cast<uint8_t*>(gi));
    g_loadgameinit_gis.push_back(gi);

    gi->type = 1;
    gi->rebind_key = static_cast<int32_t>(local_rid);
    gi->flags = 33;  // create desc[2] → Node_initFromCreateDesc @ +0x54
    gi->rh_list_head = nullptr;

    // Alias: PE Engine_malloc(32)+Util_strncpy_n(31) → node+0x60; else
    // "_gameinst".
    {
      const char* src = (alias && alias[0]) ? alias : "_gameinst";
      char* buf = new char[32]{};
      std::strncpy(buf, src, 31);
      buf[31] = '\0';
      gi->alias = buf;
    }

    // TREE register (CreateNodeByDesc @ 0x536DFF) when parent known.
    if (parent) {
      host_create_node_by_desc_tree_register(gi, parent);
      host_lgi_parent_lod_copies(gi, parent);
    } else {
      // No ResolveParent hit — adopt under eng+0x11C so FindByRid can see RID.
      host_sim_object_root_adopt(gi);
    }

    host_rid_register(static_cast<int32_t>(local_rid), gi);

    // PE @ 0x53A8CE gate: *(GameType payload+0x10) Class* != 0.
    script_fqn = host_lgi_fqn_from_type_rh(type_rh);
    type_mid = host_lgi_mid_from_type_rh(type_rh);
  }

  ++g_lgi_create_count;           // PE ++g_InstanceNodeCreateCount @ 0x53A767
  gi->flags |= 0x200;             // PE @ 0x53A76D
  // PE @ 0x53A778: ResNode_setLodReadyBit(gi,1) then +0x68=1.0 @ 0x53A77D.
  host_lgi_set_lod_ready_bit(gi, 1);
  gi->lod_scale = 1.0f;
  // PE @ 0x53A78D: gi vtbl+0xC(1.0f) → ResNode_getEmbeddedMid → mid;
  //   Rebind mid+0x38 (same as CreateGameInstanceNative W19C @ 0x53A435).
  //   LoadGameInit Rebind still OOS here — void_event_loadgameinit_rebind
  //   owns the caller RH wire.

  // PE @ 0x53A8B2 ctor hop then @ 0x53A8FD THRD-CREATE.
  // W37: PE mov [GI_mid+0x4C],eax @ 0x53A8C9 — distinct from node+0x4C RESTYPE.
  if (void* nat =
          engine_load_game_init_run_gametype_ctor(gi, type_mid, params)) {
    *reinterpret_cast<void**>(gi->mid_embed + 0x4C) = nat;
  }
  (void)engine_load_game_init_attach_gametype(gi, script_fqn, gi->rebind_key);

  // W21D PE @ 0x53AA3A: ResNode_unionChildAabb(gi).
  // W22D PE @ 0x53AA40: while(ResNode_drainSpatial(gi)).
  host_res_node_union_child_aabb(gi);
  while (host_res_node_drain_spatial(gi) != 0) {
  }

  // Gaps: sub_537240 non-early; PE vtbl* dispatch; mid.leaf until seed;
  //   drain reparent/sibling; Type53_ensureHandleSlot@4B3EE0.
  return gi;
}

// ---------------------------------------------------------------------------
// W18A/W19C/W20A/W21D/W22D — PE Engine_CreateGameInstanceNative @ 0x0053A2A0
// size 0x332. cdecl (a1=parent_rh, a2=type_rh, a3=script, a4=params,
// a5=alias) → GI*|0.
//
// Call sites (IDA):
//   GameRef.create_native @ 0x47D975 — script=0, then ResHandle_Link(GI+0x44).
//   GameType.createNativeInstance @ 0x481B52 — script=this (mid+0x50).
//
// vs Engine_LoadGameInit @ 0x53A5E0:
//   Gate type_rh+8!=0 (LoadGameInit gates parent+8). Same create desc
//   {type=1, AllocLocalRid, flags=33} + CreateNodeUnder/CreateNode + parent
//   LOD + |0x200 + setLodReadyBit. Then Rebind mid+0x38←type owner;
//   PrepareLod(type,0x80000000)+GameTypeCtor(params); store a3 at mid+0x50.
//   NO Class_boxObject / THRD-CREATE / attach_gametype.
//
// Host: PoolAlloc/TREE/LOD + run_gametype_ctor; skip attach_gametype.
// W19C: ResHandle_Rebind(GI mid+0x38, *(type_rh+0xC)) @ 0x53A435 —
//   getEmbeddedMid ≡ GI+0xD8 (PE @ 0x53EFA0).
// W20A: mid+0x50=script(a3) @ 0x53A515 (create_native a3=0).
// W21D: ResNode_unionChildAabb @ 0x4988A0 / call @ 0x53A546.
// W22D: ResNode_drainSpatial @ 0x498B10 / while @ 0x53A54C (slice).
// Gaps: drain reparent/sibling; Type53_ensureHandleSlot@4B3EE0.
// ---------------------------------------------------------------------------
void* engine_create_game_instance_native(void* parent_rh, void* type_rh,
                                         void* script, const char* params,
                                         const char* alias) {
  // PE @ 0x53A2FA: *(type_rh+8)==0 → return null (before AllocLocalRid).
  if (!type_rh) return nullptr;
  const auto* trh = reinterpret_cast<const uint8_t*>(type_rh);
  if (*reinterpret_cast<const int32_t*>(trh + 8) == 0) return nullptr;

  // PE @ 0x53A2B9: type_rh+8!=0 → payload+0x4C==8 or log Wrong GameType!
  // (warn-only). Host: no Fatal.

  if (!parent_rh) return nullptr;
  const auto* wt = reinterpret_cast<const uint8_t*>(parent_rh);
  const int32_t wt_key = *reinterpret_cast<const int32_t*>(wt + 8);

  const uint32_t local_rid = host_alloc_local_rid();
  if (local_rid == 0) return nullptr;  // PE @ 0x53A30B

  void* parent = *reinterpret_cast<void* const*>(wt + 0xC);
  HostLoadGameInitGi* gi = nullptr;
  void* type_mid = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    (void)host_eng_sim_object_root();
    if (!parent && wt_key != 0) {
      // PE CreateNode when parent+0xC==0 → ResolveParent(wt_key, type=1, 0).
      parent = host_resolve_parent(wt_key, /*type=*/1, /*flags=*/0);
    }
    gi = host_instance_node_pool_alloc();
    if (!gi) return nullptr;
    host_node_ensure_child_list_hdr(reinterpret_cast<uint8_t*>(gi));
    g_loadgameinit_gis.push_back(gi);

    gi->type = 1;
    gi->rebind_key = static_cast<int32_t>(local_rid);
    gi->flags = 33;
    gi->rh_list_head = nullptr;

    {
      const char* src = (alias && alias[0]) ? alias : "_gameinst";
      char* buf = new char[32]{};
      std::strncpy(buf, src, 31);
      buf[31] = '\0';
      gi->alias = buf;
    }

    if (parent) {
      host_create_node_by_desc_tree_register(gi, parent);
      host_lgi_parent_lod_copies(gi, parent);
    } else {
      host_sim_object_root_adopt(gi);
    }

    host_rid_register(static_cast<int32_t>(local_rid), gi);
    type_mid = host_lgi_mid_from_type_rh(type_rh);
  }

  ++g_lgi_create_count;  // PE @ 0x53A3FA
  gi->flags |= 0x200;    // PE @ 0x53A40C
  host_lgi_set_lod_ready_bit(gi, 1);
  gi->lod_scale = 1.0f;

  // W19C PE @ 0x53A427..0x53A435: vtbl+0xC → getEmbeddedMid (GI+0xD8);
  // ResHandle_Rebind(mid+0x38, *(type_rh+0xC)). Owner = GameType node
  // (+0x48 list / +0x50 key) — same as voidEvent Rebind @ 0x429060.
  void* type_owner = *reinterpret_cast<void* const*>(trh + 0xC);
  res_handle_rebind(gi->mid_embed + 0x38, type_owner);

  // PE @ 0x53A4F8..0x53A50C: GameTypeCtor vtbl+0xC(ctor, tmpRH, params).
  // No THRD-CREATE (contrast LoadGameInit @ 0x53A8FD).
  // W37: store create result at mid+0x4C (PE @ 0x53A8C9 twin).
  if (void* nat =
          engine_load_game_init_run_gametype_ctor(gi, type_mid, params)) {
    *reinterpret_cast<void**>(gi->mid_embed + 0x4C) = nat;
  }

  // W20A PE @ 0x53A515: mov [ebx+50h], ecx — mid+0x50 = a3 (script).
  // create_native passes a3=0; createNativeInstance passes this.
  *reinterpret_cast<void**>(gi->mid_embed + 0x50) = script;

  // W21D PE @ 0x53A546: ResNode_unionChildAabb(gi) — AABB from type==2 kids.
  // W22D PE @ 0x53A54C: while(ResNode_drainSpatial(gi)); Type53 OOS.
  host_res_node_union_child_aabb(gi);
  while (host_res_node_drain_spatial(gi) != 0) {
  }

  // Gaps: drain reparent/sibling; Type53_ensureHandleSlot@4B3EE0.
  return gi;
}

// PE ResourceEngine_type_gametype @ 0x53A1A0 size 0xf8.
// cdecl (parent_rh, Class*, name_cstr) -> node*|0.
// createNativeInstance @ 0x481AD6: type_gametype(&dword_62F260, class,
//   Class_getNameCstr) -> store [node+0x50] at class+0x1D4.
// Create desc: type=8 RESTYPE_GAME, flags=2, alias name|"_gametype".
// @ 0x53A27D: *(vtbl+0xC(node,1.0f)+0x10) = Class* — host mid.script_class.
void* resource_engine_type_gametype(void* parent_rh, void* clazz,
                                    const char* name) {
  (void)parent_rh;  // CreateNodeUnder / CreateNode parent splice OOS
  // Class shell alloc before g_mu (tree_host_new must not nest under lock).
  void* seed = clazz;
  if (!seed && name && name[0]) seed = class_box_object(name);

  std::lock_guard<std::mutex> lock(g_mu);
  const uint32_t local_rid = host_alloc_local_rid();
  if (local_rid == 0) return nullptr;

  HostResNode* node = host_node_new(/*owner=*/nullptr, 0);
  if (!node) return nullptr;
  node->type = 8;  // RESTYPE_GAME
  if (int32_t* fl = host_node_flags(node)) *fl = 2;  // create desc[2]
  *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(node) + 0x50) =
      static_cast<int32_t>(local_rid);

  // PE @ 0x53A27D: payload+0x10 = a2 Class*. Prefer Class*; else FQN cstr.
  host_mid_seed_script_class(node, seed, name);

  host_rid_register(static_cast<int32_t>(local_rid), node);
  return node;
}

}  // namespace inv
