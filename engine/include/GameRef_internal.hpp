#pragma once
#include "host_objects.hpp"
#include "natives.hpp"
#include "rpak.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace inv {

struct GameRefState {
  int32_t flags = 0;
  float px = 0, py = 0, pz = 0;
  float oy = 0, op = 0, or_ = 0;
  float vx = 0, vy = 0, vz = 0;
  InvObject* parent = nullptr;
  // PE GameRef_setParent_inner @ 0x0048ABA0 type 2/3: intrusive dllist
  // (child +0x04 next / +0x08 prev; parent +0x30 sentinel / +0x38 tail).
  // GameRef_dllist_unlink @ 0x004A5D00 mirrored; WT dirty sub_544FE0 /
  // sub_545120. Type-1: CameraCtrl_attachSetParent @ 0x438590
  // (handleAttach + setParent_inner(block+0x30)) + Type53_ensureHandleSlot
  // @ 0x004B3EE0 (list walk + malloc 200). Host: sib_* == parent means
  // PE (&parent+0x30); both always set when linked.
  InvObject* sib_next = nullptr;       // PE inner+0x04 (→&parent+0x30 when last)
  InvObject* sib_prev = nullptr;       // PE inner+0x08 (→&parent+0x30 when first)
  InvObject* child_list_head = nullptr;  // PE parent sentinel+0x04 / +0x34 first
  InvObject* child_list_tail = nullptr;  // PE parent inner+0x38 (sentinel.prev)
  InvObject* script = nullptr;
  std::string script_class;
  std::string script_alias;
  bool empty = true;
  // PE setFlags WORLDTREEROOT=0x10 → GameRef_worldTreeLink @ 0x00544F40:
  // WT node at inner+0xC0 spliced into handle-inner list +0x48/+0x50.
  bool worldtree_root = false;
  InvObject* wt_next = nullptr;       // PE inner+0xC0 (node.next)
  InvObject* wt_prev = nullptr;       // PE inner+0xC4 (node.prev)
  InvObject* wt_aux = nullptr;        // PE inner+0xC8 ← *(owner+0x50)
  InvObject* wt_owner = nullptr;      // PE inner+0xCC (list owner inner*)
  InvObject* wt_list_head = nullptr;  // PE owner inner+0x48 (list head)
  InvObject* wt_list_aux = nullptr;   // PE owner inner+0x50 (aux slot copied to +0xC8)
  // Phase 2.94 — Vehicle horn (sethorn via EVENT_COMMAND).
  // PE sub_458C00 @ 0x45A5C3: store (scanf %d) % 4 at obj+0x1DCC.
  int32_t horn = 0;
  // Phase 2.101 — CarMarket start/stop (1 = held/grabbed).
  int32_t drive_held = 0;
  // queueEvent EVENT_COMMAND subset (sub_458C00 strcmp — not full 0x38fe).
  int32_t suspended = 0;
  int32_t hidden = 0;
  int32_t team = 0;
  float setpitch = 0.f;
  float addroll = 0.f;
  float brake = 0.f;
  // Phase 2.102 — Vehicle assist / gearbox commands.
  int32_t transmission = 0;
  float steerhelp = 0.f;
  float asr = 0.f;
  float abs_ = 0.f;
  float esp = 0.f;  // PE @ 0x459739 "esp" %f → obj+0x1FBC+0xA30
  float difflock = 0.f;
  int32_t cruise = 0;
  float damage_multiplier = 1.f;
  float setsteer = 0.f;
  // Phase 2.103 — Garage/Mechanic/Track "filter cat mode".
  int32_t filter_engine = 0;
  int32_t filter_body = 0;
  int32_t filter_rgear = 0;
  // race119 — more EVENT_COMMAND tokens (sub_458C00; not full 0x38fe).
  int32_t autopilot = 0;
  int32_t activated = 0;
  // race123 — PE @ 0x459109 activate scanf 3 ints (slot bind OOS).
  int32_t activate_a = 0;
  int32_t activate_b = 0;
  int32_t activate_c = 0;
  int32_t health = 0;
  int32_t ammo = 0;
  int32_t pickup = 0;
  // W13A — PE @ 0x4594B4 "osd": scanf arg_8=osd.id / arg_C=ctrl
  // (disasm 0x4594CA..). findCam(arg_C) @ 0x4594F5; Bind(cam+0x54,
  // arg_8=osd.id) @ 0x45952E when cam+0x5C != osd.id. No osd→+0x128
  // write — match stays ctrl from activate Bind(cam+0x4).
  int32_t osd_id = 0;
  int32_t osd_ctrl_id = 0;
  int32_t osd_cam_idx = -1;
  int32_t osd_bind_ok = 0;
  // W12A — PE cam[4] @ chassis+0x11C stride 0x4A8 (Chassis_camEntryCtor
  // @ 0x45E7D0). match_id @ cam_entry+0xC (=blob+0x128) = ResHandle key
  // after Bind(cam+0x4, id, type=1) @ activate 0x45917d / 0x459210.
  // Free row: match_id==0 (findCam(0) @ 0x4591E0). OSD key cam+0x5C.
  static constexpr int32_t kCamTableMax = 4;
  struct CamRow {
    int32_t match_id = 0;      // PE cam+0xC / blob+0x128 (ctrl / activate)
    int32_t osd_bound_id = 0;  // PE cam+0x5C = osd.id after Bind +0x54
    alignas(4) uint8_t rh_ctrl[16]{};  // PE cam+0x4  Bind type 1
    alignas(4) uint8_t rh_b[16]{};     // PE cam+0x14 Bind type 1
    alignas(4) uint8_t rh_c[16]{};     // PE cam+0x24 Relink
    alignas(4) uint8_t rh_osd[16]{};   // PE cam+0x54 Bind type 1 (osd.id)
  };
  std::array<CamRow, kCamTableMax> cam_rows{};
  // W8A/W11A — PE queueEvent "render" @ 0x458C51..0x458D88: scanf
  // "%*s %d %d %d" → arg_C=vp / var_20=con / arg_8=camNum (IDA push
  // order). findCam(con) @ 0x448D10; gate arg_C!=0; Bind(cam+0x44, vp,
  // RESOURCE_VIEWPORT=0x12) @ 0x458d42 → ResourceEngine_LookupById @
  // 0x536820; Rebind(cam+0x34, slot_node) @ 0x458D88 via chassis_cam_*.
  alignas(4) uint8_t render_cam_rh[16]{};       // PE cam+0x34 Rebind
  alignas(4) uint8_t render_cam_bind_rh[16]{};  // PE cam+0x44 Bind type 0x12
  int32_t render_vp_id = 0;
  int32_t render_con_id = 0;
  int32_t render_cam_num = 0;
  int32_t render_cam_idx = 0;
  int32_t render_bind_type = 0;
  int32_t render_bind_resolved = 0;  // 1 when LookupById linked rh+0xC
  int32_t render_cam_match_key = 0;
  // queueEvent physical cluster lists (sub_458C00). PE GameRef blob offsets;
  // host mirrors on ResState (no native +0x206C). Heap sentinels so node
  // link addresses stay valid across unordered_map rehash/move.
  // Light: +0x2064 head / +0x206C sentinel / +0x2074 tail; node malloc 0x2C.
  struct PeLightNode {
    PeLightNode* next = nullptr;  // PE +0x04
    PeLightNode* prev = nullptr;  // PE +0x08
    // PE ResHandle @ node+0xC (4 dwords Win32); Rebind @ 0x45AFC9.
    alignas(4) uint8_t rh[16]{};
    InvObject* type = nullptr;  // type ResourceRef (host)
    int32_t match_key = 0;      // PE +0x14 == *(rh+8) after Rebind
    int32_t flags = 0;          // PE +0x1C
    float f20 = 0.f;            // PE +0x20 intensity
    float f24 = 0.f;            // PE +0x24
    float f28 = 0.f;            // PE +0x28 (= f20 * 0.5)
    InvObject* light_obj = nullptr;
  };
  std::unique_ptr<PeLightNode> light_sent;
  PeLightNode* light_tail = nullptr;
  std::vector<std::unique_ptr<PeLightNode>> light_nodes;
  // Wing: +0x2080 head / +0x2088 sentinel / +0x2090 tail; node malloc 0x1C.
  struct PeWingNode {
    PeWingNode* next = nullptr;  // PE +0x04
    PeWingNode* prev = nullptr;  // PE +0x08
    int32_t key_b = 0;           // PE +0x0C (add 2nd / rem 2nd)
    int32_t key_a = 0;           // host id for PE +0x10 handle match
    InvObject* handle_a = nullptr;
  };
  std::unique_ptr<PeWingNode> wing_sent;
  PeWingNode* wing_tail = nullptr;
  std::vector<std::unique_ptr<PeWingNode>> wing_nodes;
  // Rollbar: PE *[obj+0x1FBC]+0x44 count (max 8), entries +0x48 stride 0x10.
  struct PeRollbarEntry {
    int32_t a = -1;
    int32_t b = -1;
    float f0 = 10000.f;
    float f1 = 500.f;
  };
  std::vector<PeRollbarEntry> rollbars;
  // Mslot: +0x209C head / +0x20A4 sentinel / +0x20AC tail; node malloc 0x54.
  // Mesh copy from handle+0x2C/+0x38 (rep movsd 0xC) OOS — keys only.
  struct PeMslotNode {
    PeMslotNode* next = nullptr;
    PeMslotNode* prev = nullptr;
    int32_t key_a = 0;  // PE +0x0C
    int32_t key_b = 0;  // PE +0x10
  };
  std::unique_ptr<PeMslotNode> mslot_sent;
  PeMslotNode* mslot_tail = nullptr;
  std::vector<std::unique_ptr<PeMslotNode>> mslot_nodes;
  // W9B/W10A — PE *[phys+0xDC] part dllist (cmdAddPart @ 0x447B82 →
  // GameRef_physDcList_insertTail @ 0x45FAF0; list ctor @ 0x4774A0).
  // Layout: list object at phys+0xD4; head field +0xDC; sentinel +0xE4;
  // tail +0xEC. Node malloc 0x1C: vtbl+0; next+4; prev+8; ResHandle+0xC
  // (key@+0x14 / inst@+0x18 after Rebind). Chassis forceUpdate_apply @
  // 0x4485C9 / setBuck @ 0x43E1C0 walk this list — producer is GameRef
  // attach (not Chassis). Host: heap sentinel + nodes; exported via
  // include/GameRef.h (gameref_phys_dc_*).
  struct PePhysDcNode {
    PePhysDcNode* next = nullptr;  // PE +0x04
    PePhysDcNode* prev = nullptr;  // PE +0x08
    alignas(4) uint8_t rh[16]{};   // PE ResHandle @ +0xC
    int32_t match_key = 0;         // PE +0x14 == *(rh+8) after Rebind
    InvObject* part = nullptr;     // host; PE child inst @ +0x18
  };
  std::unique_ptr<PePhysDcNode> phys_dc_sent;
  PePhysDcNode* phys_dc_tail = nullptr;
  std::vector<std::unique_ptr<PePhysDcNode>> phys_dc_nodes;
};

struct GroundTrafficState {
  int32_t traffic_count = 0;
  int32_t traffic_streams = 0;
  int32_t next_car_id = 1;
  float ped_density = 0.f;
  float ped_density_hi = 0.f;
  int32_t ped_types = 0;
  int32_t path_spawns = 0;
  // W28D — PE Traffic_activate_apply @ 0x579040 sideband:
  // g_TrafficPoolActive @ 0x798A18, g_TrafficActivateStats @ 0x798A1C
  // (lo=activate / hi=deactivate). car_rem_n ≡ queueEvent "car_rem" when
  // +0xDD on deactivate @ 0x5790fe (Traffic_setVisibleDC @ 0x576040 twin).
  int32_t pool_active = 0;
  int32_t activate_n = 0;
  int32_t deactivate_n = 0;
  int32_t car_rem_n = 0;
  // W29D — PE Traffic_pool_push @ 0x5810A0: eng+0x310C array /
  // +0x3110 count / +0x3114 cap; Traffic_pool_slot_write @ 0x575AC0
  // stores (*car, car*) in 16B slots. Soft: car ptrs only (*car dword
  // OOS). W30D — Traffic_pool_sort_compact @ 0x581100 soft compact
  // dead (slot_live). W31D — soft score (sample_pos stand-in) +
  // heap_gt sort + Traffic_pool_car_tick gates/budget; path follow /
  // LoadGameInit / path-cursor sample Resources OOS.
  // W32D — Engine_tickPhysTraffic @ 0x463AC0 soft: budget=1 walk
  // (sort_compact @ 0x463c72); ldHigh→budget=-1; cam scratch optional.
  // W33D — Traffic_eng_frame_update @ 0x580E70 soft: eng+0x84 dt
  // (host frame_dt). W35-04 — soft timer heap (eng+0 / +4):
  // Traffic_timer_heap_push @ 0x57FE80 / pop @ 0x57AB00 drain @
  // 0x580ecc; list ticks +0x90/94/88/8C + +0xC8 queueEvent OOS.
  // W35 Soft - PE EMA eng+0x90 @ 0x463c89 after PerfTimer3
  // (host tick_phys_ema; ped eng+0x8C moverBuf half OOS).
  float frame_dt = 0.f;
  float tick_phys_ema = 0.f;
  int32_t tick_phys_n = 0;
  // Soft absolute clock: host valocity dt accumulates; PE compares
  // heap due vs *Engine_physWorld (absolute @ 0x617650).
  float sim_clock = 0.f;
  struct TimerHeapEntry {
    float due = 0.f;           // PE heap[i]+0 float
    InvObject* car = nullptr;  // PE heap[i]+4 payload
  };
  std::vector<TimerHeapEntry> timer_heap;  // eng+0 array / +4 count
  int32_t timer_heap_pops = 0;
  std::vector<InvObject*> pool_slots;
  // W34 — PE pathnode @ eng+0x98 / freelist eng+0xAC (slab 24B /
  // 6 dwords from sub_584960). node[+0xC] live (RH+0x50); +0x10 RH;
  // +0x14 child-entry chain (56B entries freelist eng+0xC0). Soft:
  // owned nodes/entries; Resources RH Link/Unlink / slab alloc OOS.
  struct TrafficPathEntry {
    TrafficPathEntry* dll_prev = nullptr;    // PE +0x0
    TrafficPathEntry* dll_next = nullptr;    // PE +0x4
    TrafficPathEntry* chain_next = nullptr;  // PE +0x8 (node+0x14)
    TrafficPathEntry** parent_head = nullptr;  // soft ≡ entry[+0xC] path+4
  };
  struct TrafficPathNode {
    TrafficPathNode* list_next = nullptr;  // PE +0x0 live / freelist
    TrafficPathNode* sib_prev = nullptr;   // PE +0x4 (RH hook)
    TrafficPathNode* sib_next = nullptr;   // PE +0x8
    int32_t live = 0;                      // PE +0xC
    TrafficPathNode** parent_head = nullptr;  // soft ≡ parent+0x48
    TrafficPathEntry* children = nullptr;  // PE +0x14
  };
  std::vector<std::unique_ptr<TrafficPathNode>> pathnode_owned;
  std::vector<std::unique_ptr<TrafficPathEntry>> path_entry_owned;
  TrafficPathNode* pathnodes_live = nullptr;   // eng+0x98
  TrafficPathNode* pathnodes_free = nullptr;   // eng+0xAC
  TrafficPathEntry* path_entries_free = nullptr;  // eng+0xC0
  std::vector<int32_t> car_ids;
  std::unordered_map<int32_t, InvObject*> cars_by_id;
  // PE addTrafficN @ 0x00484050: LoadGameInit "traffic_car" x
  // min(n,ftol(dens*20)); spawn loop i<n → Traffic_trySpawnOnRandomPath
  // @ 0x0057B420 (host: GameRef per success in traffic_cars).
  std::vector<InvObject*> traffic_cars;
  // Phase 2.84 — water / halt / ped distance.
  float water_level = 0.f;
  float water_density = 0.f;
  float water_viscosity = 0.f;
  float water_px = 0, water_py = 0, water_pz = 0;
  float water_nx = 0, water_ny = 1.f, water_nz = 0;
  bool water_plane = false;
  // PE addWaterLimit @ 0x00486920 stack defaults before vm_get_float_field:
  // point=(0,-12,0) normal=(0,1,0) — same as setWater(FFF) @ 0x004866C0.
  struct WaterLimit {
    float px = 0, py = -12.f, pz = 0;
    float nx = 0, ny = 1.f, nz = 0;
  };
  std::vector<WaterLimit> water_limits;
  std::unordered_map<int32_t, int32_t> car_behaviour;
  struct HaltCross {
    float x = 0, y = 0, z = 0;
    float time = 0;
  };
  std::vector<HaltCross> halt_crosses;
  struct HaltPath {
    float x1 = 0, y1 = 0, z1 = 0;
    float x2 = 0, y2 = 0, z2 = 0;
  };
  std::vector<HaltPath> halt_paths;
  struct PedSample {
    int32_t type_id = 0;
    float x = 0, y = 0, z = 0;
  };
  std::vector<PedSample> ped_samples;
};

extern std::mutex g_gr_mu;
extern std::unordered_map<InvObject*, GameRefState> g_refs;
extern InvObject* g_vehicle_types;
extern std::unordered_map<InvObject*, GroundTrafficState> g_grounds;

GameRefState& ref(InvObject* self);
GroundTrafficState& ground(InvObject* self);

void ground_sync_fields(InvObject* self);
void* type53_ensure_handle_slot(void* mgr_raw, void* child_handle, int32_t a3);

void gameref_handle_attach_under_parent(void* mid_block, InvObject* parent);
void gameref_set_parent_inner_handle(void* handle, InvObject* parent);
void gameref_list_unlink(InvObject* child);
void gameref_list_link(InvObject* child, InvObject* parent);
void gameref_world_tree_link(InvObject* self);
std::string script_fqn_for_entry(const RpakEntry* e);
InvObject* make_vt_host(const char* fqn);
void bind_gameref(InvObject* o, InvObject* parent, const std::string& fqn,
                  const char* alias);
bool parse_instance_params(const char* s, float out[6]);
void apply_instance_pose(InvObject* o, const float pose[6]);

InvObject* gameref_live_phys_key(InvObject* self);
bool gameref_soft_phys_sample_pos(InvObject* self, float* ox, float* oy,
                                  float* oz);
bool gameref_soft_phys_sample_vel(InvObject* self, float* ox, float* oy,
                                  float* oz);
bool gameref_soft_sample_pos_under_mu(InvObject* self, float* ox, float* oy,
                                      float* oz);

void traffic_car_sample_pos(InvObject* car, float* ox, float* oy, float* oz,
                            float cursor_88 = 0.f, float lane_a8 = 0.f);
void engine_tick_phys_traffic(InvObject* map, float dt);

}  // namespace inv
