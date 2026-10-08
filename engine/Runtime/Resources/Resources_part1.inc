// Fragment 1/4 of Resources.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


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

// PE ResourceEngine_PumpUnloadQueue @ 0x537B40 size 0x189 — gameloop hosted.
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

// PE ResourceEngine_PumpLoadQueue @ 0x5378D0 size 0x270 — gameloop hosted.
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
// W38: asyncload_hosted — GetPackSlot@5383F0 Soft is DrainWorkFail@505EA0
//   Resources callee (orphan); MainLoop HasWork@505CD0/PumpOne/FinishSlice
//   Soft in System; PostLoop D3D VB/IB/JPEG residual OOS.
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