// Fragment 1/2 of Chassis.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


namespace inv {

// WheelRef.cpp — phys table stand-in for PE *[veh+0x13E4]+id*0x2B4.
WheelRefState& chassis_phys_wheel_slot(InvObject* chassis, int32_t id);
void chassis_phys_wheel_scratch_reset(InvObject* chassis, int32_t id);
void chassis_phys_wheel_ingest_scratch(InvObject* chassis, int32_t type,
                                       int32_t wheel_idx, float a, float b);
void chassis_force_update_tail(InvObject* chassis, int32_t nw);
bool chassis_phys_wheel_dmg_active(InvObject* chassis, int32_t id);
void chassis_phys_wheel_dmg_clear(InvObject* chassis, int32_t id);
void chassis_phys_wheel_dmg_apply(InvObject* chassis, int32_t id, float px,
                                  float py, float pz, float yaw, float pitch,
                                  float roll, float f6, float f9);
bool chassis_phys_wheel_dmg_format(InvObject* chassis, int32_t id, char* buf,
                                   size_t buflen);

// PE Chassis_forceUpdate_cloneHdr @ 0x0043E3E0 — Engine_malloc(3100=0xC1C).
// thiscall ECX = chassis+0x2E48 template (ASM @ 0x448451). Deep-copy into
// private blob; ResHandle_Rebind/Link null-safe; +0xC18 cleared. Host owns
// real 3100B + side-band lerp; Sfx sample Rebind from g_sfxtables
// (PE loops @ 0x43E6B5/70E/791); no invent RPK sample payloads.
namespace {
constexpr size_t kChassisHdrBytes = 3100;  // 0xC1C
constexpr int32_t kSharedHdrToken = 0x2E48;  // PE this+0x2E48 template
constexpr size_t kSfxTableStride = 0x248;    // getSfxTable id0/1/2
constexpr size_t kResHandleBytes = 16;       // ResHandle_ctor_zero @ 0x42CEC0

struct ChassisHdrState {
  std::vector<uint8_t> blob;
  std::vector<uint8_t> lerp0;  // pairs @ hdr+0x04 (stride 8)
  std::vector<uint8_t> lerp1;  // pairs @ hdr+0x0C
  bool private_clone = false;
};
std::unordered_map<InvObject*, ChassisHdrState> g_chassis_hdr;

// PE phys+0xDC part dllist (apply walk @ 0x4485c9 / setBuck gate @ 0x43E1C0):
// head=*[phys+0xDC]; start = *(head+4)!=0 ? head : 0; next=*(node+4);
// id@+0x14; child@+0x18 → PrepareLod/vtbl+0xC → part=*(mid+0x4C)
// @ 0x448622 → ingestPart (part+0xB0 type, +0x0C ResHandle inst).
// Stop after advance when next==0 || *(next+4)==0 (terminator next=0).
// W10B: prefer live GameRef list (PePhysDcNode / gameref_phys_dc_head) when
// non-empty; else rebuild from part_slots (producer still GameRef attach).
// Host: logical nodes (32-bit PE ptr size not mirrored on x64) + terminator.
struct ChassisPhysDcNode {
  ChassisPhysDcNode* next = nullptr;  // PE +0x04
  int32_t part_id = 0;                // PE +0x14
  InvObject* child = nullptr;         // PE +0x18
};

// PE ResHandle embed @ phys+0x202C/203C/204C (16B Win32). Host side-band
// widens ptr fields; layout matches Link/Unlink list node:
//   +0 prev, +4 next, +8 key←*(inst+0x50), +0xC instance back-ref.
struct ChassisPhysRhEmbed {
  ChassisPhysRhEmbed* prev = nullptr;
  ChassisPhysRhEmbed* next = nullptr;
  uint32_t key = 0;
  void* instance = nullptr;
};

struct ChassisPhysDcState {
  std::vector<ChassisPhysDcNode> nodes;
  ChassisPhysDcNode terminator;  // next==0 end sentinel
  ChassisPhysDcNode* head = nullptr;
  // PE ingestPart phys ResHandle embeds:
  // type9 @ dword 2063 → +0x203C; type10 @ 2059 → +0x202C;
  // type12 @ 2067 → +0x204C. Back-ref compare @ +0x2048/+0x2038/+0x2058
  // vs *[part+0x0C] before Link @ 0x4290F0 / Unlink @ 0x429010.
  ChassisPhysRhEmbed rh_t9{};
  ChassisPhysRhEmbed rh_t10{};
  ChassisPhysRhEmbed rh_t12{};
};
std::unordered_map<InvObject*, ChassisPhysDcState> g_chassis_phys_dc;

// W16B/W17B — PE Chassis_findPartNodeBySlotId @ 0x4707D0:
// walk *(chassis+0x115C) match id@+0x48==slot; ret *(phys+0x78)+n*0x68.
// apply @ 0x448b65 reads ret+8 (key) / ret+0xC (instance) then splices
// wheel embed @ *[phys+0x13E4]+i*0x2B4+0x1D8 (dwords 118..121).
//
// Producers (IDA, no invent):
//   +0x115C = *(SimObjectList at +0x1154 + 8) — embedded list head field.
//     lea @ Chassis_attachPartSlot 0x43f2e6; no direct [x+115Ch] stores.
//     Slot-node fill = Part/Chassis RPK load (OOS Resources); host TREE
//     part_slots stands in for the ordered +0x115C walk.
//   phys+0x78×0x68 = Part_buildPhysSlotTable @ 0x46EAE0 (ex-sub_46EAE0):
//     count=*(this+0x1150); malloc(n*0x68+4)+4 → *(a4+0x78) @ 0x46f2e9;
//     parallel fill walking *(this+0x115C). Callers: Chassis_allocCamBlob
//     @ 0x44a415 (inside 0x44A250), Part_allocPhysBlob @ 0x46E930.
// Host: ordered side-band table (index ≡ PE n*0x68); RH from occupied.
struct ChassisPhys78Entry {
  ChassisPhysRhEmbed rh;  // PE phys+0x78 entry +0..+0xC
  int32_t slot_id = 0;
};
struct ChassisPhys78State {
  std::vector<ChassisPhys78Entry> table;  // PE *(phys+0x78) × 0x68
  bool built = false;
};
std::unordered_map<InvObject*, ChassisPhys78State> g_chassis_phys78;

// W18D — PE Part_allocPhysBlob @ 0x46E930 (size 0x1a3; camBlob 0x16c2):
//   Engine_malloc(284=0x11C) @ 0x46e93e
//   PhysBlob_baseZero7 @ 0x4369E0 — dwords 0..6 = 0
//   Engine_ResHandleSlot_clear @ +0x1C; ResHandle_ctor_zero @
//     +0x2C,+0x3C,+0x60,+0x7C,+0x8C,+0x9C,+0xC0
//   GameRef_physDcList_ctor @ +0xD4 (head field @ +0xDC); zero +0xF4/+0xF8
//   then Part_buildPhysSlotTable @ 0x46e9cb (W17B).
// W19D — PE Chassis_camBlob_baseCtor @ 0x45E910 (size 0xc7):
//   Caller Chassis_allocCamBlob @ 0x44a28a after Engine_malloc(8492=0x212C).
//   Engine_ResHandleSlot_clear(this+0); dword zeros covering RH embeds;
//   Engine_circList_ctor(this+0xD4) then *(+0xD4)=off_5F1060
//   (≡ GameRef_physDcList_ctor @ 0x4774A0); zero +0xF4/+0xF8.
//   Shared 0x11C end-state with W18D.
// W23D — PE Chassis_camEntryCtor @ 0x45E7D0 ×4 @ 0x44a295 (size 0x6e):
//   After baseCtor; cam[i] @ blob+0x11C + i*0x4A8. Zeros dwords [1..26]
//   (+64×4 @ +0x70); match id @ cam+0x128 (GameRef_findCamIndexByMatchId
//   @ 0x448D10). Does not touch [0]/[27].
// W34-07 — PE post-entry mid @ 0x44a2bb..0x44a2ff (after camEntryCtor×4):
//   dword0 +0x13BC/+0x13C0; ResHandle_ctor_zero +0x13C4/+0x13D4;
//   Engine_arrayCtor +0x13E8 ×16 stride 16 (ResHandle_ctor_zero);
//   Engine_arrayCtor +0x14EC ×16 stride 0x8C (Chassis_camAuxRow_zero4
//   @ 0x45E9E0 — zeros row+0x30..+0x3C only).
// W35-07 — PE Engine_CircList_ctor @ 0x428fd0 ×2 @ 0x44a304/+0x1DDC and
//   0x44a359/+0x1FA0 (size 0x1C empty circular; root off_5F09A8).
// W36-07 — PE @ 0x44a30f..0x44a3f4 after CircList A:
//   RH_ctor_zero +0x1E98/+0x1EA8/+0x1EB8/+0x1EE0; dword0 +0x1F90..+0x1F9C;
//   CircList B; RH +0x1FC4..+0x2004; dword0 +0x2014/+0x2018;
//   RH +0x202C/+0x203C/+0x204C (ingestPart t10/t9/t12 embeds);
//   listCtor_205C/2078/2094/20B0 ×4 (empty intrusive, size 0x1C);
//   RH_ctor_zero +0x20D8; then Part_buildPhysSlotTable @ 0x44a410 (W17B).
// W37-07 — PE @ 0x44a415..0x44a7de after buildPhysSlotTable (soft only):
//   ResHandle_Rebind(+0x13C4, *[+0x28]=RH+0x1C.instance) @ 0x44a425;
//   Veh_ensureSceneBound(+0x1C) @ 0x44a42d OOS → host fail ≡ jz loc_44A472;
//   phys dword/float defaults @ 0x44a472.. (mass/type8..12 pairs/+0x70);
//   Chassis_wheelEntry_ctor×N @ 0x45E840 stride 0x2B4 → +0x13E4 (zeros);
//   cloneHdr/camSlotFill already soft on forceUpdate path.
// Rest OOS (no invent body 0x16c2): Phys_*/car_root/LOD/bind/wake.
// Host soft side-band only.
constexpr int32_t kPhysBlobSize = 284;   // 0x11C
constexpr int32_t kCamBlobSize = 8492;   // 0x212C — PE malloc @ 0x44a271
constexpr int32_t kCamEntryStride = 0x4A8;     // 1192 — PE v6 += 298
constexpr int32_t kCamEntryMatchIdOff = 0x128;  // findCam match dword
constexpr int32_t kCamAuxRowStride = 0x8C;      // 140 — PE arrayCtor @ 0x44a2ff
constexpr int32_t kCamAuxRowZeroOff = 0x30;     // camAuxRow_zero4 start
constexpr int32_t kCamCircListOffA = 0x1DDC;    // Engine_CircList_ctor @ 0x44a30a
constexpr int32_t kCamCircListOffB = 0x1FA0;    // Engine_CircList_ctor @ 0x44a359
constexpr int32_t kCamCircListSize = 0x1C;      // PE empty dllist body
constexpr int32_t kCamListCtorOff0 = 0x205C;    // listCtor_205C @ 0x44a3c8
constexpr int32_t kCamListCtorOff1 = 0x2078;    // listCtor_2078 @ 0x44a3d3
constexpr int32_t kCamListCtorOff2 = 0x2094;    // listCtor_2094 @ 0x44a3de
constexpr int32_t kCamListCtorOff3 = 0x20B0;    // listCtor_20B0 @ 0x44a3e9
constexpr int32_t kCamListCtorSize = 0x1C;      // same empty end-state as CircList
constexpr int32_t kCamRhPad20D8 = 0x20D8;       // final RH_ctor_zero @ 0x44a3f4
// Soft stand-in for one PE cam entry (full 0x4A8 body / Bind OOS).
struct ChassisCamEntrySoft {
  int32_t match_id = 0;  // PE cam+0x128
};
// Soft stand-in for PE cam aux row (full 0x8C body OOS; only +0x30..+0x3C
// proven written by Chassis_camAuxRow_zero4).
struct ChassisCamAuxRowSoft {
  uint32_t z30 = 0;  // PE row+0x30
  uint32_t z34 = 0;  // PE row+0x34
  uint32_t z38 = 0;  // PE row+0x38
  uint32_t z3c = 0;  // PE row+0x3C
};
// Soft stand-in for PE Engine_CircList_ctor / listCtor_* empty end-state
// (0x1C): *this = root vtbl; nodes this+4 / this+0x10; circular links.
// Host: empty only (no PE vtbl / Win32 ptr size).
struct ChassisCircListSoft {
  bool empty = true;
};
struct ChassisPhysBlobState {
  bool allocated = false;
  bool cam_blob_base_cted = false;  // W19D: PE @ 0x45E910 soft
  bool cam_entries_cted = false;    // W23D: PE @ 0x45E7D0 ×4 soft
  bool cam_mid_ctor_cted = false;   // W34-07: PE @ 0x44a2bb..0x44a2ff soft
  bool cam_circlist_cted = false;   // W35-07: PE CircList ×2 soft
  bool cam_listctor_cted = false;   // W36-07: RH pads + listCtor×4 soft
  bool cam_phys_defaults_cted = false;  // W37-07: PE @ 0x44a472 soft
  bool cam_wheel_entries_cted = false;  // W37-07: PE wheelEntry_ctor×N soft
  std::array<ChassisCamEntrySoft, 4> cam_entries{};  // PE +0x11C ×4
  // W34-07 mid RH embeds (PE ResHandle_ctor_zero before +0x13E8 array).
  ChassisPhysRhEmbed rh_13c4{};  // +0x13C4
  ChassisPhysRhEmbed rh_13d4{};  // +0x13D4
  uint32_t mid_13bc = 0;         // +0x13BC
  uint32_t mid_13c0 = 0;         // +0x13C0
  ChassisCircListSoft circ_1ddc{};  // +0x1DDC
  ChassisCircListSoft circ_1fa0{};  // +0x1FA0
  // W36-07 intervening RH / dword pads (allocCamBlob @ 0x44a30f..0x44a3bd).
  ChassisPhysRhEmbed rh_1e98{};  // +0x1E98
  ChassisPhysRhEmbed rh_1ea8{};  // +0x1EA8
  ChassisPhysRhEmbed rh_1eb8{};  // +0x1EB8
  ChassisPhysRhEmbed rh_1ee0{};  // +0x1EE0
  uint32_t z_1f90 = 0;
  uint32_t z_1f94 = 0;
  uint32_t z_1f98 = 0;
  uint32_t z_1f9c = 0;
  ChassisPhysRhEmbed rh_1fc4{};  // +0x1FC4
  ChassisPhysRhEmbed rh_1fd4{};  // +0x1FD4
  ChassisPhysRhEmbed rh_1fe4{};  // +0x1FE4
  ChassisPhysRhEmbed rh_1ff4{};  // +0x1FF4
  ChassisPhysRhEmbed rh_2004{};  // +0x2004
  uint32_t z_2014 = 0;
  uint32_t z_2018 = 0;
  ChassisPhysRhEmbed rh_202c{};  // +0x202C — ingestPart type10
  ChassisPhysRhEmbed rh_203c{};  // +0x203C — ingestPart type9
  ChassisPhysRhEmbed rh_204c{};  // +0x204C — ingestPart type12
  ChassisCircListSoft list_205c{};  // +0x205C listCtor_205C
  ChassisCircListSoft list_2078{};  // +0x2078
  ChassisCircListSoft list_2094{};  // +0x2094
  ChassisCircListSoft list_20b0{};  // +0x20B0
  ChassisPhysRhEmbed rh_20d8{};     // +0x20D8
  ChassisPhysRhEmbed rh_1c{};  // +0x1C
  ChassisPhysRhEmbed rh_2c{};  // +0x2C
  ChassisPhysRhEmbed rh_3c{};  // +0x3C
  ChassisPhysRhEmbed rh_60{};  // +0x60
  ChassisPhysRhEmbed rh_7c{};  // +0x7C (buildPhysSlotTable Link)
  ChassisPhysRhEmbed rh_8c{};  // +0x8C
  ChassisPhysRhEmbed rh_9c{};  // +0x9C
  ChassisPhysRhEmbed rh_c0{};  // +0xC0
  uint32_t f4 = 0;             // +0xF4
  uint32_t f8 = 0;             // +0xF8
};
std::unordered_map<InvObject*, ChassisPhysBlobState> g_chassis_phys_blob;
// PE wheel[+0x1D8] ResHandle embeds (apply splice @ 0x448b86 = v40+118).
std::unordered_map<InvObject*, std::array<ChassisPhysRhEmbed, 8>>
    g_chassis_wheel_rh;

// W9C/W14A — PE chassis cam / slot tables (GameRef vehicle blob offsets; host
// side-band keyed by Chassis InvObject*). Proven consumers (not forceUpdate):
//   queueEvent "render" @ 0x458d6c / 0x458d7d (Rebind cam+0x34)
//   Chassis_camCopyRebind @ 0x44a15d / 0x44a16b / ApplyBound @ 0x44a23f
//   tick path @ 0x453c76 / 0x453cb6 (sub_4518C0)
//   Chassis_camSlotCleanup @ 0x44bcff (walk +0x13F4 × hdr+0xA54)
//   Chassis_camApplyBoundSlot @ 0x449680 (activate 0x4591c7/0x4592a2,
//     osd 0x459537; ClearSlotThenApply 0x449eb6)
// Layout (Chassis_allocCamBlob @ 0x44A250, malloc 0x212C):
//   +0x11C  cam[4] stride 0x4A8; match id @ cam+0x128 (findCam @ 0x448D10)
//           W23D: Chassis_camEntryCtor×4 @ 0x44a295 soft-ensure
//   +0x13E4 wheel-table *ptr (separate from slots)
//   +0x13E8 16× ResHandle_ctor_zero embeds (Engine_arrayCtor @ 0x44a2e7)
//           slot+0xC instance/node = *[base + i*16 + 0x13F4]
//           W34-07: mid RH +0x13C4/+0x13D4 before this array
//   +0x14EC 16×0x8C aux rows (Engine_arrayCtor @ 0x44a2ff +
//           Chassis_camAuxRow_zero4 @ 0x45E9E0 — zeros +0x30..+0x3C)
//   +0x1DDC / +0x1FA0 Engine_CircList_ctor @ 0x428fd0 (W35-07 soft;
//           empty 0x1C circular; root off_5F09A8)
//   +0x177C cam live count; +0x1780 active cam idx (init -1 @ 0x44694e)
//   +0x17A8 int32 map[camIdx*36 + camNum] → slot_idx; <0 → skip Rebind
//   +0x1784 cam embed table stride 0x90 (144); ApplyBound slot params
//   +0x1F40 gear-digit label cap (min 4) for text loops @ 0x449aab
// Live slot fill: Chassis_allocCamBlob @ 0x44a788 × hdr+0xA54 →
//   Chassis_camSlotFillFromHdr @ 0x48B940 (def @ hdr+0xA58 stride 0x1C;
//   a5=*(def+0x18), a6=1, this=slot +0x13E8+i*16). W10B host slice:
//   count/def floats PE-backed; ResourceEngine camera create still OOS.
// Map writers also OOS. Bind type 0x12 @ 0x458d42 = GameRef_queueEvent_parse
//   ONLY — do NOT invent in Chassis.
constexpr int32_t kCamTableMax = 4;        // ctor loop count @ 0x44a295
constexpr int32_t kCamSlotNodeCap = 16;    // vector count @ 0x44a2dc
constexpr int32_t kCamSlotMapStride = 36;  // index = camIdx*36+camNum
constexpr int32_t kCamApplyBoneEmbeds = 64;  // clear loop @ 0x44978c
constexpr int32_t kCamApplyGearDigitMax = 4;  // clamp @ 0x449ab4
static_assert(kCamTableMax == 4, "PE camEntryCtor×4");
static_assert(kCamCircListSize == 0x1C, "PE Engine_CircList_ctor body");
// PE OSD/gauge labels when active (strings @ 0x610310..): gauge, A..E,T,L,O
// + gear digit tables off_60E458="d/c/b/a" off_60E468="1/2/3/4"
//   off_60E478="7/8/5/6".
static constexpr const char* kCamApplyOsdLabels[] = {
    "gauge", "A", "B", "C", "D", "E", "T", "L", "O"};
static constexpr const char* kCamApplyGearDcba[] = {"d", "c", "b", "a"};
static constexpr const char* kCamApplyGear1234[] = {"1", "2", "3", "4"};
static constexpr const char* kCamApplyGear7856[] = {"7", "8", "5", "6"};
static_assert(sizeof(void*) >= 4, "host ptr");

struct ChassisCamSlotEmbed {
  ChassisCamSlotEmbed* prev = nullptr;  // PE +0x0
  ChassisCamSlotEmbed* next = nullptr;  // PE +0x4
  uint32_t key = 0;                     // PE +0x8
  void* instance = nullptr;             // PE +0xC — Rebind node
};

struct ChassisCamTablesState {
  int32_t cam_count = 0;    // PE +0x177C
  int32_t active_cam = -1;  // PE +0x1780
  // PE +0x17A8 — default -1 so jl @ 0x458d75 skips empty rows.
  std::array<int32_t, kCamTableMax * kCamSlotMapStride> slot_map{};
  std::array<ChassisCamSlotEmbed, kCamSlotNodeCap> slots{};  // PE +0x13E8
  // W34-07: PE +0x14EC ×16; only +0x30..+0x3C proven by camAuxRow_zero4.
  std::array<ChassisCamAuxRowSoft, kCamSlotNodeCap> aux_rows{};
  // W14A ApplyBoundSlot soft gates (PE @ 0x449680).
  int32_t apply_cam_idx = -1;
  int32_t apply_osd_live = 0;
  int32_t apply_bones = 0;   // Chassis_camBindBones @ 0x448f20 soft
  int32_t apply_gauges = 0;  // gauge/text path when cam==active
  int32_t apply_count = 0;
  int32_t gear_digit_cap = kCamApplyGearDigitMax;  // PE +0x1F40 soft
  bool inited = false;
};
std::unordered_map<InvObject*, ChassisCamTablesState> g_chassis_cam_tables;

void chassis_cam_tables_reset(ChassisCamTablesState& st) {
  st.cam_count = 0;
  st.active_cam = -1;
  st.slot_map.fill(-1);
  for (auto& s : st.slots) s = ChassisCamSlotEmbed{};
  for (auto& a : st.aux_rows) a = ChassisCamAuxRowSoft{};
  st.apply_cam_idx = -1;
  st.apply_osd_live = 0;
  st.apply_bones = 0;
  st.apply_gauges = 0;
  st.apply_count = 0;
  st.gear_digit_cap = kCamApplyGearDigitMax;
  st.inited = true;
}

ChassisCamTablesState& chassis_cam_tables_ref(InvObject* self) {
  ChassisCamTablesState& st = g_chassis_cam_tables[self];
  if (!st.inited) chassis_cam_tables_reset(st);
  return st;
}

// Soft ensure: PE Chassis_allocCamBlob @ 0x44A250 constructs tables; host
// zeros embeds + map=-1. cam_count from TREE camera_count. Slot fill from
// hdr+0xA54 is chassis_cam_slot_fill_from_hdr (after hdr clone / ensure).
// W19D: PE camBlob_baseCtor @ 0x45E910 first (via chassis_cam_blob_base_ensure).
// W23D: PE camEntryCtor×4 @ 0x45E7D0 next (via chassis_cam_entries_ensure).
// W34-07: PE mid @ 0x44a2bb..0x44a2ff (via chassis_cam_mid_ctor_ensure).
// W35-07: PE CircList×2 @ 0x44a304/+0x1DDC + 0x44a359/+0x1FA0
//   (via chassis_cam_circlist_ensure).
// W36-07: RH pads + listCtor×4 +0x20D8 (via chassis_cam_listctor_ensure)
//   then soft Part_buildPhysSlotTable (phys78_rebuild).
// W37-07: Rebind+0x13C4 / phys defaults @ 0x44a472 / wheelEntry_ctor×N soft;
//   Veh_ensureSceneBound + Phys_*/LOD still OOS (no invent body 0x16c2).
void chassis_cam_blob_base_ensure(InvObject* self);
void chassis_cam_entries_ensure(InvObject* self);
void chassis_cam_mid_ctor_ensure(InvObject* self);
void chassis_cam_circlist_ensure(InvObject* self);
void chassis_cam_listctor_ensure(InvObject* self);
void chassis_cam_phys_defaults_ensure(InvObject* self);
void chassis_cam_wheel_entries_ensure(InvObject* self);
void chassis_cam_tables_ensure_inner(InvObject* self) {
  if (!self) return;
  chassis_cam_blob_base_ensure(self);
  chassis_cam_entries_ensure(self);
  chassis_cam_mid_ctor_ensure(self);
  chassis_cam_circlist_ensure(self);
  chassis_cam_listctor_ensure(self);
  chassis_cam_phys_defaults_ensure(self);
  chassis_cam_wheel_entries_ensure(self);
  ChassisCamTablesState& st = chassis_cam_tables_ref(self);
  const int32_t cc = tree_field_get_int(self, "camera_count");
  if (cc > 0) {
    st.cam_count = cc > kCamTableMax ? kCamTableMax : cc;
    tree_field_set_int(self, "cam_table_count", st.cam_count);
  }
  tree_field_set_int(self, "cam_slot_node_cap", kCamSlotNodeCap);
  tree_field_set_int(self, "cam_slot_map_stride", kCamSlotMapStride);
}

// PE ResHandle_Unlink @ 0x429010: thiscall this=inst+0x44, a2=embed.
// ASM @ 0x43C66E (t9): push embed; lea ecx,[old+0x44]; call.
void chassis_phys_rh_unlink(ChassisPhysRhEmbed& emb) {
  void* old = emb.instance;
  if (emb.prev)
    emb.prev->next = emb.next;
  else if (old)
    *reinterpret_cast<void**>(static_cast<char*>(old) + 0x48) = emb.next;
  if (emb.next) emb.next->prev = emb.prev;
  emb.prev = nullptr;
  emb.next = nullptr;
  emb.key = 0;
  emb.instance = nullptr;
}

// PE ResHandle_Link @ 0x4290F0: this=inst+0x44, a2=embed.
// Post-Link @ 0x43C688/702/7A0: embed+8 = *(inst+0x50).
void chassis_phys_rh_link(void* inst, ChassisPhysRhEmbed& emb) {
  if (!inst) {
    emb = ChassisPhysRhEmbed{};
    return;
  }
  auto* head =
      reinterpret_cast<void**>(static_cast<char*>(inst) + 0x44);  // +0x48 HEAD
  auto* cur = reinterpret_cast<ChassisPhysRhEmbed*>(head[1]);
  if (cur) cur->prev = &emb;
  emb.prev = nullptr;
  emb.next = cur;
  head[1] = &emb;
  emb.instance = inst;
  emb.key = *reinterpret_cast<uint32_t*>(static_cast<char*>(inst) + 0x50);
}

// PE LABEL_25 @ 0x43C70C / ctor_zero @ 0x42CEC0 — clear embed (no list splice).
void chassis_phys_dc_rh_zero(ChassisPhysRhEmbed& emb) {
  emb = ChassisPhysRhEmbed{};
}

// W8C: PE *[part+0x0C] = ResHandle instance. Host stand-in = native_ptr_node
// when Native.ptr already allocated (no ensure — do not invent). Part blob
// itself = *(vtbl+0xC(child_node)+0x4C) @ 0x448622; +0x0C writer is
// handle/attach bind (ResourceRef_newNative @ 0x47CEA0 zeros; GameRef/
// createNative Link paths e.g. 0x47D900) — not a Chassis TREE field.
void* chassis_part_res_instance(InvObject* child) {
  if (!child || !native_ptr_get(child)) return nullptr;
  return native_ptr_node(child);
}

// PE ingestPart t9/10/12: if embed.instance != *[part+0x0C] → Unlink old,
// Link new or LABEL_25 zero. Post-Link writes key @ embed+8.
void chassis_phys_rh_relink(ChassisPhysRhEmbed& emb, void* inst) {
  if (emb.instance == inst) return;
  if (emb.instance) chassis_phys_rh_unlink(emb);
  if (inst)
    chassis_phys_rh_link(inst, emb);
  else
    chassis_phys_dc_rh_zero(emb);
}

// Host stand-in for *[phys+0xDC] producer: occupied part_slots → nodes
// + terminator (next==0). Used when GameRef live list empty / absent.
void chassis_phys_dc_rebuild(InvObject* self) {
  if (!self) return;
  ChassisPhysDcState& st = g_chassis_phys_dc[self];
  st.nodes.clear();
  st.terminator = ChassisPhysDcNode{};
  st.head = nullptr;
  const int32_t nslots = part_slot_count(self);
  for (int32_t si = 0; si < nslots; ++si) {
    const int32_t sid = part_slot_id_at(self, si);
    if (sid <= 0) continue;
    InvObject* child = part_on_slot(self, sid);
    if (!child) continue;
    ChassisPhysDcNode n;
    n.part_id = sid;
    n.child = child;
    st.nodes.push_back(n);
  }
  if (st.nodes.empty()) {
    tree_field_set_int(self, "force_update_phys_dc_nodes", 0);
    return;
  }
  for (size_t i = 0; i + 1 < st.nodes.size(); ++i)
    st.nodes[i].next = &st.nodes[i + 1];
  st.nodes.back().next = &st.terminator;
  st.head = &st.nodes[0];
  tree_field_set_int(self, "force_update_phys_dc_nodes",
                     static_cast<int32_t>(st.nodes.size()));
}

// W10A gameref_phys_dc_head — try Chassis self, then the_car / part_parent.
void* chassis_try_live_phys_dc_head(InvObject* self) {
  if (!self) return nullptr;
  if (void* h = gameref_phys_dc_head(self)) return h;
  if (InvObject* car = tree_field_get_obj(self, "the_car")) {
    if (void* h = gameref_phys_dc_head(car)) return h;
  }
  if (InvObject* parent = tree_field_get_obj(self, "part_parent")) {
    if (void* h = gameref_phys_dc_head(parent)) return h;
  }
  return nullptr;
}

// True when live export returned a first node (PE start gate).
bool chassis_phys_dc_live_nonempty(void* head) { return head != nullptr; }

// PE Chassis_forceUpdate_ingestPart @ 0x43C520: LOWORD(part+0xB0)=type,
// HIWORD=wheel idx; +0xB8/+0xBC float pair. Types 5/6/7 → wheel scratch;
// 8 → phys+0x1E38/1E3C; 9 → +0x1E40/44 + RH +0x203C; 10 → +0x1E50/54 +
// RH +0x202C; 12 → +0x1E48/4C + RH +0x204C.
// W8C: t9/10/12 Link(inst+0x44, embed) when *[part+0x0C] changes
// (ASM 0x43C651/6AE/737); post-Link embed+8=*(inst+0x50). Host: TREE
// floats; inst = native_ptr_node(child) if Native.ptr live, else LABEL_25.
// W37-07: allocCamBlob soft defaults seed TREE t8..t12 pairs before apply
// walk; ingestPart overwrites when packed phys_b0 present.
void chassis_force_update_ingest_part(InvObject* self, InvObject* child,
                                      int32_t nw, ChassisPhysDcState& st) {
  if (!self || !child) return;
  const int32_t packed = tree_field_get_int(child, "phys_b0");
  if (packed == 0) return;
  const int32_t type = packed & 0xFFFF;
  const int32_t widx = (packed >> 16) & 0xFFFF;
  const float a = tree_field_get_float(child, "phys_b8");
  const float b = tree_field_get_float(child, "phys_bc");
  if (type >= 5 && type <= 7) {
    if (widx < nw)
      chassis_phys_wheel_ingest_scratch(self, type, widx, a, b);
    return;
  }
  if (type != 8 && type != 9 && type != 10 && type != 12) return;
  char k0[40], k1[40];
  std::snprintf(k0, sizeof(k0), "force_update_phys_t%d_a", type);
  std::snprintf(k1, sizeof(k1), "force_update_phys_t%d_b", type);
  tree_field_set_float(self, k0, a);
  tree_field_set_float(self, k1, b);
  // type 8: float pair only (no ResHandle @ ingestPart case 8).
  if (type == 8) return;
  void* inst = chassis_part_res_instance(child);
  if (type == 9)
    chassis_phys_rh_relink(st.rh_t9, inst);
  else if (type == 10)
    chassis_phys_rh_relink(st.rh_t10, inst);
  else
    chassis_phys_rh_relink(st.rh_t12, inst);
}

void hdr_put_u32(uint8_t* b, size_t off, uint32_t v) {
  if (!b || off + 4 > kChassisHdrBytes) return;
  std::memcpy(b + off, &v, 4);
}
void hdr_put_f32(uint8_t* b, size_t off, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, 4);
  hdr_put_u32(b, off, bits);
}
void hdr_put_ptr32(uint8_t* b, size_t off, const void* p) {
  // PE dword slots; Win32 host stores ptr; x64 leaves 0 (side-band owns).
  if (sizeof(void*) == 4)
    hdr_put_u32(b, off,
                static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)));
  else
    hdr_put_u32(b, off, 0);
}
uint32_t hdr_get_u32(const uint8_t* b, size_t off) {
  if (!b || off + 4 > kChassisHdrBytes) return 0;
  uint32_t v = 0;
  std::memcpy(&v, b + off, 4);
  return v;
}
float hdr_get_f32(const uint8_t* b, size_t off) {
  uint32_t bits = hdr_get_u32(b, off);
  float f = 0.f;
  std::memcpy(&f, &bits, 4);
  return f;
}

uint8_t* chassis_hdr_ptr(InvObject* self) {
  if (!self) return nullptr;
  auto it = g_chassis_hdr.find(self);
  if (it == g_chassis_hdr.end() || it->second.blob.size() != kChassisHdrBytes)
    return nullptr;
  return it->second.blob.data();
}

// PE Chassis_allocCamBlob @ 0x44a788 / Chassis_camSlotFillFromHdr @ 0x48B940:
// for i in 0..*[hdr+0xA54]: this=chassis+0x13E8+i*16; def=hdr+0xA58+i*0x1C;
// args (veh+0x2C, def, def+0xC, *(float*)(def+0x18), 1).
// Host PE-backed: sync count + def+0x18 float; no invent camera via
// ResourceEngine_type_rendertype_camera (slot.instance stays until Rebind).
void chassis_cam_slot_fill_from_hdr_inner(InvObject* self) {
  if (!self) return;
  ChassisCamTablesState& st = chassis_cam_tables_ref(self);
  uint8_t* hdr = chassis_hdr_ptr(self);
  int32_t n = 0;
  if (hdr) n = static_cast<int32_t>(hdr_get_u32(hdr, 0xA54));
  if (n <= 0) {
    n = tree_field_get_int(self, "camera_count");
    // PE allocCamBlob reads *[hdr+0xA54]; seed from TREE when blob empty.
    if (hdr && n > 0) hdr_put_u32(hdr, 0xA54, static_cast<uint32_t>(n));
  }
  if (n < 0) n = 0;
  if (n > kCamSlotNodeCap) n = kCamSlotNodeCap;
  int32_t filled = 0;
  for (int32_t i = 0; i < n; ++i) {
    float a5 = 0.f;
    if (hdr) {
      const size_t def = 0xA58 + static_cast<size_t>(i) * 0x1Cu;
      if (def + 0x1Cu > kChassisHdrBytes) break;
      a5 = hdr_get_f32(hdr, def + 0x18);  // PE push [hdr+0xA70+i*0x1C]
    }
    char key[40];
    std::snprintf(key, sizeof(key), "cam_slot_%d_a70", i);
    tree_field_set_float(self, key, a5);
    // Slot embed +0x13E8+i*16 already zeroed on ensure; instance fill =
    // camera create @ 0x48b9c8 — OOS (no invent ResourceEngine camera).
    (void)st.slots[static_cast<size_t>(i)];
    ++filled;
  }
  tree_field_set_int(self, "cam_slot_fill_count", filled);
  tree_field_set_int(self, "cam_slot_hdr_a54", n);
}

// PE ResHandle_Bind @ 0x546070 (apply @ 0x448aa9): this=hdr+off, a2=rid,
// a3=6, a4=0. rid==0 → unlink zero 16B. rid!=0 without RPK loader → store
// id at +8, list/instance stay 0 (no invent samples).
void hdr_res_bind_nullsafe(uint8_t* b, size_t off, int32_t rid) {
  if (!b || off + kResHandleBytes > kChassisHdrBytes) return;
  std::memset(b + off, 0, kResHandleBytes);
  if (rid != 0) hdr_put_u32(b, off + 8, static_cast<uint32_t>(rid));
}

// PE cloneHdr SfxTable×3 @ +0xF8/+0x340/+0x588 (stride 0x248): count @
// base+0x240, vol @ base+0x244. Empty deep-copy = count 0, vol 1.0f.
// Sample slot stride 0x24 (Chassis_SfxTable_ctor @ 0x45DC30 ×16):
//   +0x00..+0x0C ResHandle; +0x10 pitch(1/p); +0x14..+0x20 pmin..vmax.
constexpr size_t kSfxSampleStride = 0x24;
constexpr int32_t kSfxSampleCap = 16;  // addItem jge @ 0x4423E0

void chassis_hdr_init_sfx_tables(uint8_t* b) {
  if (!b) return;
  for (size_t i = 0; i < 3; ++i) {
    const size_t base = 0xF8 + i * kSfxTableStride;
    hdr_put_u32(b, base + 0x240, 0);
    hdr_put_f32(b, base + 0x244, 1.f);
  }
}

// PE cloneHdr sample Rebind loops @ 0x43E6B5 / 0x43E70E / 0x43E791:
//   count=*[tmpl+base+0x240]; for i: push *[src+0xC]; lea ecx,dest;
//   ResHandle_Rebind @ 0x429060; copy +0x10..+0x20; stride 0x24.
// Host: g_sfxtables rows (SfxTable.addItem) → hdr slots. Instance =
// native_ptr_node(sfx) when Native.ptr live (same as ingestPart t9/10/12);
// else zero RH (null Rebind a2=0). No invent sample payloads / RPK.
void chassis_hdr_sfx_sample_rebind(uint8_t* b, InvObject* self) {
  if (!b || !self) return;
  int32_t total = 0;
  for (int32_t tid = 0; tid < 3; ++tid) {
    const size_t base =
        0xF8 + static_cast<size_t>(tid) * kSfxTableStride;
    char key[32];
    std::snprintf(key, sizeof(key), "sfx_table_%d", static_cast<int>(tid));
    InvObject* tab = tree_field_get_obj(self, key);
    const std::vector<SfxItem>* rows = nullptr;
    if (tab) {
      auto it = g_sfxtables.find(tab);
      if (it != g_sfxtables.end()) rows = &it->second;
    }
    int32_t n = rows ? static_cast<int32_t>(rows->size()) : 0;
    if (n > kSfxSampleCap) n = kSfxSampleCap;
    hdr_put_u32(b, base + 0x240, static_cast<uint32_t>(n));
    // Vol: keep prior if set; else ctor default 1.0f (0x45DC56).
    if (hdr_get_f32(b, base + 0x244) == 0.f)
      hdr_put_f32(b, base + 0x244, 1.f);
    for (int32_t i = 0; i < kSfxSampleCap; ++i) {
      const size_t slot = base + static_cast<size_t>(i) * kSfxSampleStride;
      if (slot + kSfxSampleStride > kChassisHdrBytes) break;
      std::memset(b + slot, 0, kResHandleBytes);  // RH +0..+0xC
      if (i >= n || !rows) {
        // Clear stale floats past new count (PE clear leaves floats;
        // host zeros on shrink for deterministic clone).
        hdr_put_f32(b, slot + 0x10, 0.f);
        hdr_put_f32(b, slot + 0x14, 0.f);
        hdr_put_f32(b, slot + 0x18, 0.f);
        hdr_put_f32(b, slot + 0x1C, 0.f);
        hdr_put_f32(b, slot + 0x20, 0.f);
        continue;
      }
      const SfxItem& it = (*rows)[static_cast<size_t>(i)];
      // ResHandle_Rebind(dest, src_inst): null-safe store instance+key.
      void* inst = nullptr;
      if (it.sfx && native_ptr_get(it.sfx))
        inst = native_ptr_node(it.sfx);
      if (inst) {
        hdr_put_ptr32(b, slot + 0xC, inst);
        hdr_put_u32(
            b, slot + 0x8,
            *reinterpret_cast<uint32_t*>(static_cast<char*>(inst) + 0x50));
      }
      hdr_put_f32(b, slot + 0x10, it.pitch);
      hdr_put_f32(b, slot + 0x14, it.pmin);
      hdr_put_f32(b, slot + 0x18, it.pmax);
      hdr_put_f32(b, slot + 0x1C, it.vmin);
      hdr_put_f32(b, slot + 0x20, it.vmax);
      ++total;
    }
    char ck[40];
    std::snprintf(ck, sizeof(ck), "force_update_sfx_tab%d_n",
                  static_cast<int>(tid));
    tree_field_set_int(self, ck, n);
  }
  tree_field_set_int(self, "force_update_sfx_samples", total);
}

// PE cloneHdr ResHandle ctors / null Link path — slots that Link/Unlink
// when src instance dword == 0 (ASM after Sfx deep-copy).
void chassis_hdr_null_reshandles(uint8_t* b) {
  if (!b) return;
  static const size_t kSlots[] = {
      0x7D4, 0x7E4, 0x7F4, 0x814, 0x824, 0x834, 0x844, 0x854, 0x864,
      0x874, 0x884, 0x894, 0x8A4, 0x8B4, 0x8C4, 0x8DC, 0x8EC};
  for (size_t off : kSlots) {
    if (off + kResHandleBytes <= kChassisHdrBytes)
      std::memset(b + off, 0, kResHandleBytes);
  }
}

// PE apply null-curve stub @ 0x4488f0: count0=count1=1, malloc(8)×2, zeros.
void chassis_hdr_stub_lerp(ChassisHdrState& st) {
  st.lerp0.assign(8, 0);
  st.lerp1.assign(8, 0);
  uint8_t* b = st.blob.data();
  hdr_put_u32(b, 0x00, 1);  // count0
  hdr_put_u32(b, 0x08, 1);  // count1
  hdr_put_ptr32(b, 0x04, st.lerp0.data());
  hdr_put_ptr32(b, 0x0C, st.lerp1.data());
  hdr_put_u32(b, 0x10, 0);  // curve meta
  hdr_put_u32(b, 0xC18, 0);  // engine-curve src (cloneHdr clears)
}

// PE apply @ 0x448773.. when *[hdr+0xC18] (DynoSim*): count=+0x1D4,
// step=+0x1D0, y0=+0x1DC, y1=+0x1E0, meta=+0x1C0 → hdr lerp pairs.
// Host: DynoData g_dyno.nm side-band (cloneHdr clears +0xC18).
void chassis_hdr_rebuild_lerp_from_dyno(ChassisHdrState& st, InvObject* dyno) {
  uint8_t* b = st.blob.data();
  hdr_put_u32(b, 0xC18, 0);  // PE cloneHdr / host: no raw DynoSim*
  if (!dyno) {
    chassis_hdr_stub_lerp(st);
    return;
  }
  auto it = g_dyno.find(dyno);
  if (it == g_dyno.end() || it->second.nm.empty()) {
    chassis_hdr_stub_lerp(st);
    return;
  }
  const auto& nm = it->second.nm;
  const int32_t n = static_cast<int32_t>(nm.size());
  if (n <= 0) {
    chassis_hdr_stub_lerp(st);
    return;
  }
  float step = tree_field_get_float(dyno, "table_stepsize");
  if (step <= 0.f && it->second.steps > 1 && it->second.max_rpm > 0.f)
    step = it->second.max_rpm / static_cast<float>(it->second.steps);
  if (step <= 0.f) step = 1.f;
  st.lerp0.assign(static_cast<size_t>(n) * 8u, 0);
  st.lerp1.assign(static_cast<size_t>(n) * 8u, 0);
  for (int32_t i = 0; i < n; ++i) {
    const float x = static_cast<float>(i) * step;
    const float y = nm[static_cast<size_t>(i)];
    std::memcpy(st.lerp0.data() + static_cast<size_t>(i) * 8u, &x, 4);
    std::memcpy(st.lerp0.data() + static_cast<size_t>(i) * 8u + 4, &y, 4);
    // PE +0x1E0 null → duplicate y0 into both tables (0x448844).
    std::memcpy(st.lerp1.data() + static_cast<size_t>(i) * 8u, &x, 4);
    std::memcpy(st.lerp1.data() + static_cast<size_t>(i) * 8u + 4, &y, 4);
  }
  hdr_put_u32(b, 0x00, static_cast<uint32_t>(n));
  hdr_put_u32(b, 0x08, static_cast<uint32_t>(n));
  hdr_put_ptr32(b, 0x04, st.lerp0.data());
  hdr_put_ptr32(b, 0x0C, st.lerp1.data());
  hdr_put_u32(b, 0x10, 0);  // DynoSim+0x1C0 meta unknown on host
}

// PE Chassis_hdr_lerp_xy_table0 @ 0x43BE70: count[+0] pairs[+4] stride 8.
float chassis_hdr_lerp_xy(const uint8_t* pairs, int32_t count, float rpm) {
  if (!pairs || count <= 1) return 0.f;
  float x0 = 0.f, y0 = 0.f;
  std::memcpy(&x0, pairs, 4);
  std::memcpy(&y0, pairs + 4, 4);
  if (rpm < x0) return 0.f;
  for (int32_t i = 1; i < count; ++i) {
    float x1 = 0.f, y1 = 0.f;
    std::memcpy(&x1, pairs + static_cast<size_t>(i) * 8u, 4);
    std::memcpy(&y1, pairs + static_cast<size_t>(i) * 8u + 4, 4);
    if (rpm <= x1) {
      const float span = x1 - x0;
      if (span == 0.f) return y0;
      return y0 + (rpm - x0) * (y1 - y0) / span;
    }
    x0 = x1;
    y0 = y1;
  }
  return 0.f;  // past last
}

// W18D — PE Part_allocPhysBlob @ 0x46E930 preamble (before buildPhysSlotTable):
// malloc(0x11C) + PhysBlob_baseZero7 + RH/physDcList ctor + zero +0xF4/+0xF8.
// Host soft side-band; real Engine_malloc / scene / LOD tail OOS.
// Then W17B phys78_rebuild ≡ Part_buildPhysSlotTable store @ 0x46f2e9.
void chassis_phys78_rebuild(InvObject* self);
void chassis_phys_blob_ensure(InvObject* self) {
  if (!self) return;
  ChassisPhysBlobState& st = g_chassis_phys_blob[self];
  if (st.allocated) {
    if (!g_chassis_phys78[self].built) chassis_phys78_rebuild(self);
    return;
  }
  // PE PhysBlob_baseZero7 @ 0x4369E0 — dwords 0..6 cleared (no raw blob).
  // Same RH/+0xF4/+0xF8 end-state as Chassis_camBlob_baseCtor @ 0x45E910.
  chassis_phys_dc_rh_zero(st.rh_1c);  // +0x1C Engine_ResHandleSlot_clear
  chassis_phys_dc_rh_zero(st.rh_2c);  // +0x2C ResHandle_ctor_zero
  chassis_phys_dc_rh_zero(st.rh_3c);  // +0x3C
  chassis_phys_dc_rh_zero(st.rh_60);  // +0x60
  chassis_phys_dc_rh_zero(st.rh_7c);  // +0x7C
  chassis_phys_dc_rh_zero(st.rh_8c);  // +0x8C
  chassis_phys_dc_rh_zero(st.rh_9c);  // +0x9C
  chassis_phys_dc_rh_zero(st.rh_c0);  // +0xC0
  st.f4 = 0;  // +0xF4
  st.f8 = 0;  // +0xF8
  // PE GameRef_physDcList_ctor @ +0xD4 — empty circular list; host walk
  // uses g_chassis_phys_dc / gameref_phys_dc_head (filled on apply).
  st.allocated = true;
  tree_field_set_int(self, "phys_blob_size", kPhysBlobSize);
  tree_field_set_int(self, "phys_blob_alloc", 1);
  chassis_phys78_rebuild(self);
}

// W19D — PE @ Chassis_camBlob_baseCtor @ 0x45E910 (size 0xc7):
// allocCamBlob @ 0x44a28a after malloc(0x212C). Shared 0x11C prefix with
// Part_allocPhysBlob (W18D); camEntryCtor×4 (W23D) / mid+aux (W34-07) /
// CircList×2 (W35-07) / listCtor / phys tail OOS.
void chassis_cam_blob_base_ensure(InvObject* self) {
  if (!self) return;
  ChassisPhysBlobState& st = g_chassis_phys_blob[self];
  if (st.cam_blob_base_cted) {
    if (!st.allocated) chassis_phys_blob_ensure(self);
    return;
  }
  // PE @ 0x45E910 end-state ≡ W18D 0x11C RH/list/F4/F8 zeros.
  chassis_phys_blob_ensure(self);
  st.cam_blob_base_cted = true;
  tree_field_set_int(self, "cam_blob_size", kCamBlobSize);
  tree_field_set_int(self, "cam_blob_base_cted", 1);
}

// W23D — PE Chassis_camEntryCtor @ 0x45E7D0 ×4 @ allocCamBlob 0x44a295:
// cam[i] @ blob+0x11C + i*0x4A8; zeros PE fields; match_id @ +0x128 = 0.
// Host soft side-band only (no Bind / ResourceEngine camera create).
void chassis_cam_entries_ensure(InvObject* self) {
  if (!self) return;
  ChassisPhysBlobState& st = g_chassis_phys_blob[self];
  if (st.cam_entries_cted) {
    if (!st.cam_blob_base_cted) chassis_cam_blob_base_ensure(self);
    return;
  }
  chassis_cam_blob_base_ensure(self);
  for (auto& e : st.cam_entries) e = ChassisCamEntrySoft{};
  st.cam_entries_cted = true;
  tree_field_set_int(self, "cam_entry_stride", kCamEntryStride);
  tree_field_set_int(self, "cam_entry_match_off", kCamEntryMatchIdOff);
  tree_field_set_int(self, "cam_entries_cted", 1);
  tree_field_set_int(self, "cam_entries_count", kCamTableMax);
}

// W34-07 — PE Chassis_allocCamBlob @ 0x44a2bb..0x44a2ff after camEntryCtor×4:
//   *[blob+0x13BC]=0; *[blob+0x13C0]=0;
//   ResHandle_ctor_zero(blob+0x13C4); ResHandle_ctor_zero(blob+0x13D4);
//   Engine_arrayCtor(blob+0x13E8, stride=16, n=16, ResHandle_ctor_zero);
//   Engine_arrayCtor(blob+0x14EC, stride=0x8C, n=16, Chassis_camAuxRow_zero4
//     @ 0x45E9E0) — zeros each row +0x30..+0x3C only.
// Host: soft RH/dwords + re-zero +0x13E8 slots + aux +0x30..+0x3C.
// Next: CircList×2 (W35-07) then RH pads/listCtor (W36-07).
void chassis_cam_mid_ctor_ensure(InvObject* self) {
  if (!self) return;
  ChassisPhysBlobState& st = g_chassis_phys_blob[self];
  if (st.cam_mid_ctor_cted) {
    if (!st.cam_entries_cted) chassis_cam_entries_ensure(self);
    return;
  }
  chassis_cam_entries_ensure(self);
  st.mid_13bc = 0;
  st.mid_13c0 = 0;
  chassis_phys_dc_rh_zero(st.rh_13c4);
  chassis_phys_dc_rh_zero(st.rh_13d4);
  // PE Engine_arrayCtor +0x13E8 ×16 ResHandle_ctor_zero — host slots side-band.
  ChassisCamTablesState& cam = chassis_cam_tables_ref(self);
  for (auto& s : cam.slots) s = ChassisCamSlotEmbed{};
  // PE Chassis_camAuxRow_zero4 ×16 @ +0x14EC — only +0x30..+0x3C.
  for (auto& a : cam.aux_rows) a = ChassisCamAuxRowSoft{};
  st.cam_mid_ctor_cted = true;
  tree_field_set_int(self, "cam_aux_row_stride", kCamAuxRowStride);
  tree_field_set_int(self, "cam_aux_row_zero_off", kCamAuxRowZeroOff);
  tree_field_set_int(self, "cam_aux_rows_count", kCamSlotNodeCap);
  tree_field_set_int(self, "cam_mid_ctor_cted", 1);
}

// W35-07 — PE Chassis_allocCamBlob @ 0x44a304..0x44a359 after mid/aux:
//   Engine_CircList_ctor(blob+0x1DDC);  // @ 0x428fd0, size 0x1C empty
//   Engine_CircList_ctor(blob+0x1FA0);  // same; root * = off_5F09A8
// Host: soft empty circ lists only. Pads / listCtor = W36-07.
void chassis_cam_circlist_ensure(InvObject* self) {
  if (!self) return;
  ChassisPhysBlobState& st = g_chassis_phys_blob[self];
  if (st.cam_circlist_cted) {
    if (!st.cam_mid_ctor_cted) chassis_cam_mid_ctor_ensure(self);
    return;
  }
  chassis_cam_mid_ctor_ensure(self);
  st.circ_1ddc = ChassisCircListSoft{};
  st.circ_1fa0 = ChassisCircListSoft{};
  st.cam_circlist_cted = true;
  tree_field_set_int(self, "cam_circlist_off_a", kCamCircListOffA);
  tree_field_set_int(self, "cam_circlist_off_b", kCamCircListOffB);
  tree_field_set_int(self, "cam_circlist_size", kCamCircListSize);
  tree_field_set_int(self, "cam_circlist_cted", 1);
}

// W36-07 — PE Chassis_allocCamBlob @ 0x44a30f..0x44a3f4 after CircList A:
//   RH_ctor_zero +0x1E98/+0x1EA8/+0x1EB8/+0x1EE0;
//   dword0 +0x1F90/+0x1F94/+0x1F98/+0x1F9C; CircList B (W35);
//   RH_ctor_zero +0x1FC4/+0x1FD4/+0x1FE4/+0x1FF4/+0x2004;
//   dword0 +0x2014/+0x2018;
//   RH_ctor_zero +0x202C/+0x203C/+0x204C (ingestPart t10/t9/t12);
//   listCtor_205C/2078/2094/20B0 ×4 empty (size 0x1C, roots off_5F1064..);
//   RH_ctor_zero +0x20D8; then Part_buildPhysSlotTable @ 0x44a410.
// Host: soft zero RH/dwords + empty lists; phys78_rebuild ≡ buildPhysSlotTable.
// Next: W37-07 soft defaults / wheelEntry; Veh_ensureSceneBound @ 0x44a42d OOS.
void chassis_cam_listctor_ensure(InvObject* self) {
  if (!self) return;
  ChassisPhysBlobState& st = g_chassis_phys_blob[self];
  if (st.cam_listctor_cted) {
    if (!st.cam_circlist_cted) chassis_cam_circlist_ensure(self);
    return;
  }
  chassis_cam_circlist_ensure(self);
  // Pads between CircList A and B (disasm @ 0x44a30f..0x44a353).
  chassis_phys_dc_rh_zero(st.rh_1e98);
  chassis_phys_dc_rh_zero(st.rh_1ea8);
  chassis_phys_dc_rh_zero(st.rh_1eb8);
  chassis_phys_dc_rh_zero(st.rh_1ee0);
  st.z_1f90 = 0;
  st.z_1f94 = 0;
  st.z_1f98 = 0;
  st.z_1f9c = 0;
  // Post CircList B (disasm @ 0x44a35e..0x44a3bd).
  chassis_phys_dc_rh_zero(st.rh_1fc4);
  chassis_phys_dc_rh_zero(st.rh_1fd4);
  chassis_phys_dc_rh_zero(st.rh_1fe4);
  chassis_phys_dc_rh_zero(st.rh_1ff4);
  chassis_phys_dc_rh_zero(st.rh_2004);
  st.z_2014 = 0;
  st.z_2018 = 0;
  chassis_phys_dc_rh_zero(st.rh_202c);
  chassis_phys_dc_rh_zero(st.rh_203c);
  chassis_phys_dc_rh_zero(st.rh_204c);
  // listCtor×4 empty intrusive (same soft end-state as CircList).
  st.list_205c = ChassisCircListSoft{};
  st.list_2078 = ChassisCircListSoft{};
  st.list_2094 = ChassisCircListSoft{};
  st.list_20b0 = ChassisCircListSoft{};
  chassis_phys_dc_rh_zero(st.rh_20d8);
  st.cam_listctor_cted = true;
  tree_field_set_int(self, "cam_listctor_off0", kCamListCtorOff0);
  tree_field_set_int(self, "cam_listctor_off1", kCamListCtorOff1);
  tree_field_set_int(self, "cam_listctor_off2", kCamListCtorOff2);
  tree_field_set_int(self, "cam_listctor_off3", kCamListCtorOff3);
  tree_field_set_int(self, "cam_listctor_size", kCamListCtorSize);
  tree_field_set_int(self, "cam_rh_pad_20d8", kCamRhPad20D8);
  tree_field_set_int(self, "cam_listctor_cted", 1);
  // PE @ 0x44a410: Part_buildPhysSlotTable after listCtor tail.
  chassis_phys78_rebuild(self);
}

// W37-07 — PE Chassis_allocCamBlob @ 0x44a415..0x44a7de after buildPhysSlotTable:
//   ResHandle_Rebind(blob+0x13C4, *[blob+0x28]) @ 0x44a425 — +0x28 is
//     instance dword of RH embed @ +0x1C (ResHandleSlot_clear body).
//   Veh_ensureSceneBound(blob+0x1C) @ 0x44a42d — PrepareLod/scene OOS;
//     host fail ≡ jz loc_44A472 (PE merge when eax==0).
//   loc_44A472 phys defaults (int_convert IEEE): +0x14E8=0;
//     +0x1E58=320000.f; +0x1E64≈9.375e-5; +0x1E5C/+0x1E60=20.f;
//     t8 +0x1E38=0 / +0x1E3C≈5e-8; t9 +0x1E40=0 / +0x1E44≈5e-8;
//     t10 +0x1E50=0 / +0x1E54≈5e-8; t12 +0x1E48=0 / +0x1E4C≈5e-6;
//     +0x1DCC=0; +0x70 = (+0x70 & ~0x4000) | 0x60.
//   Host TREE side-band for forceUpdate/ingestPart consumers; also zero
//   phys_dc t9/10/12 embeds (same PE +0x203C/+0x202C/+0x204C as blob).
//   No invent Veh_ensureSceneBound / Phys_* / LOD (rest of body 0x16c2 OOS).
void chassis_cam_phys_defaults_ensure(InvObject* self) {
  if (!self) return;
  ChassisPhysBlobState& st = g_chassis_phys_blob[self];
  if (st.cam_phys_defaults_cted) {
    if (!st.cam_listctor_cted) chassis_cam_listctor_ensure(self);
    return;
  }
  chassis_cam_listctor_ensure(self);
  // PE @ 0x44a425: Rebind(+0x13C4, *[+0x28]=rh_1c.instance); null-safe.
  chassis_phys_rh_relink(st.rh_13c4, st.rh_1c.instance);
  // PE Veh_ensureSceneBound fail → skip +0x13BC list link; mid stays 0.
  st.mid_13bc = 0;
  st.mid_13c0 = 0;
  // PE loc_44A472 consumer-facing defaults (forceUpdate overwrites later).
  constexpr float kMassDefault = 320000.f;     // 0x489C4000 @ +0x1E58
  constexpr float kEps5e8 = 5.e-8f;            // 0x3356BF95
  constexpr float kEps5e6 = 5.e-6f;            // 0x36A7C5AC
  constexpr float kEps9e5 = 9.375e-5f;         // 0x38C49BA6 @ +0x1E64
  tree_field_set_float(self, "force_update_mass", kMassDefault);
  tree_field_set_float(self, "force_update_phys_t8_a", 0.f);
  tree_field_set_float(self, "force_update_phys_t8_b", kEps5e8);
  tree_field_set_float(self, "force_update_phys_t9_a", 0.f);
  tree_field_set_float(self, "force_update_phys_t9_b", kEps5e8);
  tree_field_set_float(self, "force_update_phys_t10_a", 0.f);
  tree_field_set_float(self, "force_update_phys_t10_b", kEps5e8);
  tree_field_set_float(self, "force_update_phys_t12_a", 0.f);
  tree_field_set_float(self, "force_update_phys_t12_b", kEps5e6);
  tree_field_set_float(self, "cam_phys_1e64", kEps9e5);
  tree_field_set_float(self, "cam_phys_1e5c", 20.f);
  tree_field_set_float(self, "cam_phys_1e60", 20.f);
  tree_field_set_int(self, "force_update_horn_idx", 0);  // +0x1DCC
  tree_field_set_int(self, "cam_phys_14e8", 0);
  // PE +0x70: and ~0x4000 | 0x60 (after zero). Preserve host ~0x800000 clear
  // contract from forceUpdate by only OR-ing PE init bits when unset.
  {
    int32_t f70 = tree_field_get_int(self, "force_update_flags70");
    f70 = (f70 & ~0x4000) | 0x60;
    tree_field_set_int(self, "force_update_flags70", f70);
  }
  // PE ctor zeros ingestPart RH embeds; host phys_dc mirrors blob+0x202C..
  ChassisPhysDcState& dc = g_chassis_phys_dc[self];
  if (!dc.rh_t10.instance) chassis_phys_dc_rh_zero(dc.rh_t10);
  if (!dc.rh_t9.instance) chassis_phys_dc_rh_zero(dc.rh_t9);
  if (!dc.rh_t12.instance) chassis_phys_dc_rh_zero(dc.rh_t12);
  chassis_phys_dc_rh_zero(st.rh_202c);
  chassis_phys_dc_rh_zero(st.rh_203c);
  chassis_phys_dc_rh_zero(st.rh_204c);
  st.cam_phys_defaults_cted = true;
  tree_field_set_int(self, "cam_phys_defaults_cted", 1);
}

// W37-07 — PE Chassis_wheelEntry_ctor @ 0x45E840 ×N @ allocCamBlob 0x44a826:
//   Engine_ResHandleSlot_clear ×3 (+0/+0x10/+0x20); dword zeros; empty
//   intrusive list pair; stride 0x2B4 → store ptr @ blob+0x13E4.
// Host: soft mark + clear wheel RH embeds (g_chassis_wheel_rh); WheelRef
//   scratch already owns per-slot state. No invent Engine_malloc(692*N) /
//   Physics_createPrimitive / car_root (rest of 0x16c2 OOS).
void chassis_cam_wheel_entries_ensure(InvObject* self) {
  if (!self) return;
  ChassisPhysBlobState& st = g_chassis_phys_blob[self];
  if (st.cam_wheel_entries_cted) {
    if (!st.cam_phys_defaults_cted) chassis_cam_phys_defaults_ensure(self);
    return;
  }
  chassis_cam_phys_defaults_ensure(self);
  // PE count = *[chassis+0x1F40] (dword 2000); host TREE wheels / phys_wheel_count.
  int32_t nw = tree_field_get_int(self, "wheels");
  if (nw <= 0) nw = tree_field_get_int(self, "phys_wheel_count");
  if (nw < 0) nw = 0;
  if (nw > 8) nw = 8;
  auto& embeds = g_chassis_wheel_rh[self];
  for (int32_t i = 0; i < 8; ++i) {
    // PE wheelEntry_ctor: 3× ResHandleSlot_clear + zero wheel+0x1D8 region.
    // Scratch +0x15C.. reset is forceUpdate apply @ 0x4484e3 — not here.
    embeds[static_cast<size_t>(i)] = ChassisPhysRhEmbed{};
  }
  (void)nw;
  st.cam_wheel_entries_cted = true;
  tree_field_set_int(self, "cam_wheel_entry_stride", 0x2B4);
  tree_field_set_int(self, "cam_wheel_entries_count", nw);
  tree_field_set_int(self, "cam_wheel_entries_cted", 1);
}

// W17B — PE Part_buildPhysSlotTable @ 0x46EAE0 / store *(phys+0x78) @
// 0x46f2e9: count=*(chassis+0x1150); table[i] ↔ +0x115C node i (id@+0x48).
// Host: part_slot_count / part_slot_id_at order; RH when part_on_slot live.
void chassis_phys78_rebuild(InvObject* self) {
  if (!self) return;
  ChassisPhys78State& st = g_chassis_phys78[self];
  st.table.clear();
  const int32_t nslots = part_slot_count(self);
  st.table.reserve(nslots > 0 ? static_cast<size_t>(nslots) : 0);
  for (int32_t si = 0; si < nslots; ++si) {
    ChassisPhys78Entry e;
    e.slot_id = part_slot_id_at(self, si);
    InvObject* child =
        e.slot_id > 0 ? part_on_slot(self, e.slot_id) : nullptr;
    void* inst = child ? chassis_part_res_instance(child) : nullptr;
    if (inst) {
      e.rh.instance = inst;
      e.rh.key =
          *reinterpret_cast<uint32_t*>(static_cast<char*>(inst) + 0x50);
      e.rh.prev = nullptr;
      e.rh.next = nullptr;
    }
    st.table.push_back(e);
  }
  st.built = true;
  tree_field_set_int(self, "force_update_phys78_count",
                     static_cast<int32_t>(st.table.size()));
}

// PE Chassis_findPartNodeBySlotId @ 0x4707D0 (IDA name).
// PE: start=*( *(chassis+0x115C)+4 ) ? *(+0x115C) : 0; match id@+0x48;
// advance next=+4 stop next==0||*(next+4)==0; ret *(phys+0x78)+n*0x68.
// Miss / empty list → 0 (leave wheel). Hit with key==0 → clear path.
// Host: ordered side-band from chassis_phys78_rebuild (W17B).
ChassisPhys78Entry* chassis_find_part_node_by_slot_id(InvObject* self,
                                                      int32_t slot_id) {
  if (!self || slot_id <= 0) return nullptr;
  ChassisPhys78State& st = g_chassis_phys78[self];
  if (!st.built) chassis_phys78_rebuild(self);
  for (auto& e : st.table) {
    if (e.slot_id == slot_id) return &e;
  }
  return nullptr;
}

// PE apply @ 0x448b3c..0x448c84: for i<this+0x1F40, wheel=base+i*0x2B4,
// findPartNode(101+i); if ret: key@+8≠0 → Link/Unlink splice embed
// wheel+0x1D8 onto *(ret+0xC); else Unlink/clear. ret==0 → no touch.
// W17B: rebuild phys+0x78 side-map first (Part_buildPhysSlotTable).
// W18D: ensure 0x11C phys-blob ctor side-band before table fill.
void chassis_force_update_wheel_rh_splice(InvObject* self, int32_t nw) {
  if (!self || nw <= 0) return;
  chassis_phys_blob_ensure(self);
  chassis_phys78_rebuild(self);
  auto& embeds = g_chassis_wheel_rh[self];
  int32_t spliced = 0;
  int32_t cleared = 0;
  int32_t missed = 0;
  for (int32_t i = 0; i < nw && i < 8; ++i) {
    ChassisPhys78Entry* node =
        chassis_find_part_node_by_slot_id(self, 101 + i);
    if (!node) {
      ++missed;
      continue;
    }
    ChassisPhysRhEmbed& emb = embeds[static_cast<size_t>(i)];
    // PE @ 0x448b75: cmp dword [node+8], 0
    if (node->rh.key != 0) {
      chassis_phys_rh_relink(emb, node->rh.instance);
      ++spliced;
    } else if (emb.instance) {
      // PE @ 0x448c07..0x448c53 Unlink wheel embed
      chassis_phys_rh_unlink(emb);
      ++cleared;
    } else {
      // PE @ 0x448c5b: only clear +0x1E0 (dword 120)
      emb.key = 0;
      ++cleared;
    }
  }
  tree_field_set_int(self, "force_update_wheel_rh_spliced", spliced);
  tree_field_set_int(self, "force_update_wheel_rh_cleared", cleared);
  tree_field_set_int(self, "force_update_wheel_rh_missed", missed);
}

// PE apply child walk @ phys+0xDC (0x4485c9): next=*(node+4),
// *(node+0x18)=RH.instance → PrepareLod(0x80000000)/vtbl+0xC →
// part=*(mid+0x4C) @ 0x448622 → ingestPart @ 0x43C520.
// W10B/W15A: prefer live GameRef PePhysDcNode list; else rebuild from
// part_slots. TREE phys_b0/b8/bc; RH Link via native_ptr_node.
// W16B soft PrepareLod hop: PE null *(node+0x18) skips; host live uses
// match_key / native_ptr as instance-ready gate (no invent ResHandle_PrepareLod).
// Full PrepareLod/vtbl + ResourceEngine still OOS.
void chassis_force_update_walk_children(InvObject* self, int32_t nw) {
  if (!self) return;
  ChassisPhysDcState& st = g_chassis_phys_dc[self];
  void* live = chassis_try_live_phys_dc_head(self);
  const bool use_live = chassis_phys_dc_live_nonempty(live);
  if (use_live) {
    int32_t nlive = 0;
    for (void* n = live; n; n = gameref_phys_dc_next(n)) ++nlive;
    tree_field_set_int(self, "force_update_phys_dc_nodes", nlive);
    tree_field_set_int(self, "force_update_phys_dc_live", 1);
  } else {
    chassis_phys_dc_rebuild(self);
    tree_field_set_int(self, "force_update_phys_dc_live", 0);
  }
  // PE apply @ 0x448562..0x4485c3: Unlink/clear type10 RH (+0x202C) before walk.
  if (st.rh_t10.instance)
    chassis_phys_rh_unlink(st.rh_t10);
  else
    chassis_phys_dc_rh_zero(st.rh_t10);
  int32_t walked = 0;
  int32_t ingested = 0;
  int32_t hop_skip = 0;
  int32_t linked = 0;
  if (use_live) {
    // PE @ 0x4485c9: start already gated by gameref_phys_dc_head;
    // next/stop via gameref_phys_dc_next (sentinel / *(next+4)==0).
    // id@+0x14 mirrored by gameref_phys_dc_part_id (match_key).
    for (void* node = live; node; node = gameref_phys_dc_next(node)) {
      InvObject* child = gameref_phys_dc_child(node);
      if (!child) continue;
      ++walked;
      // W16B @ 0x4485DC: PE v16=*(node+0x18); null → skip PrepareLod.
      // Host: match_key (rh+8) or native_ptr_node ≡ instance-ready.
      const int32_t mk = gameref_phys_dc_part_id(node);
      const bool hop_ok =
          mk != 0 || chassis_part_res_instance(child) != nullptr;
      if (!hop_ok) {
        ++hop_skip;
        continue;
      }
      // PrepareLod/vtbl fail → PE skip; host ingest no-ops phys_b0==0.
      chassis_force_update_ingest_part(self, child, nw, st);
      ++ingested;
    }
  } else {
    // PE start gate: head && *(head+4) — terminator next==0 ends walk.
    ChassisPhysDcNode* node = st.head;
    if (!node || !node->next) node = nullptr;
    while (node) {
      if (node->child) {
        ++walked;
        // Rebuild path has no RH @ +0x18 — soft hop via native_ptr only.
        if (!chassis_part_res_instance(node->child) &&
            tree_field_get_int(node->child, "phys_b0") == 0) {
          ++hop_skip;
        } else {
          chassis_force_update_ingest_part(self, node->child, nw, st);
          ++ingested;
        }
      }
      node = node->next;
      if (!node || !node->next) node = nullptr;
    }
  }
  if (st.rh_t9.instance) ++linked;
  if (st.rh_t10.instance) ++linked;
  if (st.rh_t12.instance) ++linked;
  tree_field_set_int(self, "force_update_phys_dc_walked", walked);
  tree_field_set_int(self, "force_update_phys_dc_ingested", ingested);
  tree_field_set_int(self, "force_update_prepare_lod_skip", hop_skip);
  tree_field_set_int(self, "force_update_phys_rh_linked", linked);
}

// PE cloneHdr: malloc 3100, ctor zero ResHandle/Sfx regions, copy scalars
// from template, deep-copy lerp tables, Rebind/Link ResHandles, +0xC18=0.
// Then lazy RemapFromPath+Bind @ 0x43EDD7 (sl.rpk/particles.rpk → BSS RHs).
// Host: fresh zeroed 3100B; Sfx headers + sample Rebind from g_sfxtables;
// null ResHandles; stub lerp; Soft RemapFromPath lazy binds.
uint8_t* chassis_force_update_clone_hdr(InvObject* self) {
  if (!self) return nullptr;
  ChassisHdrState& st = g_chassis_hdr[self];
  st.blob.assign(kChassisHdrBytes, 0);
  uint8_t* b = st.blob.data();
  // PE: SfxTable×3 ctor @ 0x43E42D then sample Rebind @ 0x43E6B5..
  chassis_hdr_init_sfx_tables(b);
  chassis_hdr_sfx_sample_rebind(b, self);
  // PE: ResHandle ctors + null Link path leave usable zero slots.
  chassis_hdr_null_reshandles(b);
  chassis_hdr_stub_lerp(st);
  // PE @ 0x43EDD7..0x43EE56: RemapFromPath + ResHandle_Bind BSS RHs.
  resource_engine_clone_hdr_lazy_binds();
  tree_field_set_int(self, "force_update_remap_sl",
                     static_cast<int32_t>(resource_engine_void_event_type_rh_id()));
  tree_field_set_int(self, "force_update_remap_ptx_a",
                     static_cast<int32_t>(resource_engine_particles_rh_a_id()));
  tree_field_set_int(self, "force_update_remap_ptx_b",
                     static_cast<int32_t>(resource_engine_particles_rh_b_id()));
  st.private_clone = true;
  tree_field_set_int(self, "force_update_hdr_bytes",
                     static_cast<int32_t>(kChassisHdrBytes));
  tree_field_set_int(self, "force_update_hdr", 1);  // private ≠ shared token
  return st.blob.data();
}
}  // namespace

// W9C — PE @ Chassis_allocCamBlob / queueEvent render table accessors.
// External linkage for GameRef (list producer / render consumer). Layout
// owned here; Bind type 0x12 stays in GameRef_queueEvent_parse @ 0x458d42.
void chassis_cam_tables_ensure(InvObject* self) {
  chassis_cam_tables_ensure_inner(self);
  chassis_cam_slot_fill_from_hdr(self);
}

// PE @ 0x44a788 × hdr+0xA54 → Chassis_camSlotFillFromHdr @ 0x48B940.
void chassis_cam_slot_fill_from_hdr(InvObject* self) {
  chassis_cam_slot_fill_from_hdr_inner(self);
}

int32_t chassis_cam_count(InvObject* self) {
  if (!self) return 0;
  return chassis_cam_tables_ref(self).cam_count;
}

int32_t chassis_cam_active(InvObject* self) {
  if (!self) return -1;
  return chassis_cam_tables_ref(self).active_cam;
}

void chassis_cam_set_active(InvObject* self, int32_t idx) {
  if (!self) return;
  chassis_cam_tables_ref(self).active_cam = idx;
}

// PE @ 0x458d6c: map[camIdx*36+camNum]; <0 → skip.
int32_t chassis_cam_slot_map_get(InvObject* self, int32_t cam_idx,
                                 int32_t cam_num) {
  if (!self || cam_idx < 0 || cam_idx >= kCamTableMax || cam_num < 0 ||
      cam_num >= kCamSlotMapStride)
    return -1;
  auto& st = chassis_cam_tables_ref(self);
  return st.slot_map[static_cast<size_t>(cam_idx * kCamSlotMapStride +
                                         cam_num)];
}

void chassis_cam_slot_map_set(InvObject* self, int32_t cam_idx, int32_t cam_num,
                              int32_t slot_idx) {
  if (!self || cam_idx < 0 || cam_idx >= kCamTableMax || cam_num < 0 ||
      cam_num >= kCamSlotMapStride)
    return;
  auto& st = chassis_cam_tables_ref(self);
  st.slot_map[static_cast<size_t>(cam_idx * kCamSlotMapStride + cam_num)] =
      slot_idx;
}

// PE @ 0x458d7d: *[chassis + slot_idx*16 + 0x13F4] = embed[+0xC].
void* chassis_cam_slot_node(InvObject* self, int32_t slot_idx) {
  if (!self || slot_idx < 0 || slot_idx >= kCamSlotNodeCap) return nullptr;
  return chassis_cam_tables_ref(self).slots[static_cast<size_t>(slot_idx)]
      .instance;
}

void chassis_cam_slot_set_node(InvObject* self, int32_t slot_idx, void* node) {
  if (!self || slot_idx < 0 || slot_idx >= kCamSlotNodeCap) return;
  auto& emb =
      chassis_cam_tables_ref(self).slots[static_cast<size_t>(slot_idx)];
  emb.instance = node;
  if (!node) {
    emb.prev = nullptr;
    emb.next = nullptr;
    emb.key = 0;
  }
}

// PE Chassis_camApplyBoundSlot @ 0x449680:
//   Cam_queueLookMoveZoom(veh+0x13C4) — follow (veh+0x1F30) or slot+0x1784
//   Chassis_camBindBones @ 0x448f20
//   clear 64 bone embeds @ cam+0x190 (dword 100)
//   if OSD RH live (cam+0x178 / dword 94) && cam_idx == +0x1780:
//     Text_createRText2Inst gauge + A..E T L O + gear digits (cap +0x1F40)
//     Text_setColorARGB(0xFF00FF00)
//   else clear 64 embeds @ cam+0x18C
//   channel21 Engine_queryGameRefChannel @ veh+0x1EE0 — OOS
// Host soft: TREE gates; no invent look/move/zoom / Text / RenderRef.
void chassis_cam_apply_bound_slot(InvObject* self, int32_t cam_idx,
                                  int32_t osd_live) {
  if (!self) return;
  chassis_cam_tables_ensure_inner(self);
  ChassisCamTablesState& st = chassis_cam_tables_ref(self);
  if (cam_idx < 0 || cam_idx >= kCamTableMax) {
    tree_field_set_int(self, "cam_apply_bound_ok", 0);
    tree_field_set_int(self, "cam_apply_bound_cam", cam_idx);
    return;
  }
  st.apply_cam_idx = cam_idx;
  st.apply_osd_live = osd_live != 0 ? 1 : 0;
  st.apply_bones = 1;  // PE always calls Chassis_camBindBones @ 0x449784
  // PE @ 0x4497e3..0x4497fa: gauges only if OSD RH live AND cam==active.
  const int32_t active = st.active_cam;
  const bool gauges =
      st.apply_osd_live != 0 && cam_idx == active && active >= 0;
  st.apply_gauges = gauges ? 1 : 0;
  int32_t rtext2_ok = 0;
  if (gauges) {
    int32_t cap = st.gear_digit_cap;
    if (cap > kCamApplyGearDigitMax) cap = kCamApplyGearDigitMax;
    if (cap < 0) cap = 0;
    tree_field_set_int(self, "cam_apply_gauge", 1);
    // Soft PE @ 0x449835: Text_createRText2Inst gauge (buf=256, align=2,
    // scale=1.0f) when cam OSD slot empty. Soft: one owned r_text2 Text.
    InvObject* gauge_txt = tree_field_get_obj(self, "cam_apply_rtext2");
    if (!gauge_txt) {
      gauge_txt = tree_host_new("java.render.Text");
      tree_field_set_obj(self, "cam_apply_rtext2", gauge_txt);
    }
    InvObject* charset = frontend_medium_font();
    if (text_create_rtext2_inst_soft(gauge_txt, /*parent*/ nullptr, charset,
                                     /*x*/ 0.f, /*z*/ 0.f,
                                     /*color*/ static_cast<int32_t>(0xFFFFFFFFu),
                                     /*align*/ 2, /*scale*/ 1.f)) {
      rtext2_ok = 1;
      tree_field_set_obj(gauge_txt, "text", string_new(""));
      java_render_Text_update(gauge_txt);
    }
    for (const char* lab : kCamApplyOsdLabels) {
      char key[40];
      std::snprintf(key, sizeof(key), "cam_apply_label_%s", lab);
      tree_field_set_int(self, key, 1);
    }
    for (int32_t i = 0; i < cap; ++i) {
      char key[40];
      std::snprintf(key, sizeof(key), "cam_apply_gear_dcba_%s",
                    kCamApplyGearDcba[static_cast<size_t>(i)]);
      tree_field_set_int(self, key, 1);
      std::snprintf(key, sizeof(key), "cam_apply_gear_1234_%s",
                    kCamApplyGear1234[static_cast<size_t>(i)]);
      tree_field_set_int(self, key, 1);
      std::snprintf(key, sizeof(key), "cam_apply_gear_7856_%s",
                    kCamApplyGear7856[static_cast<size_t>(i)]);
      tree_field_set_int(self, key, 1);
    }
    tree_field_set_int(self, "cam_apply_gear_digit_n", cap);
  } else {
    tree_field_set_int(self, "cam_apply_gauge", 0);
    tree_field_set_int(self, "cam_apply_gear_digit_n", 0);
  }
  // Soft stand-in for 64× ResHandle_clearIfBound @ 0x44978c (bone embeds).
  tree_field_set_int(self, "cam_apply_bone_embeds_cleared",
                     kCamApplyBoneEmbeds);
  ++st.apply_count;
  tree_field_set_int(self, "cam_apply_bound_ok", 1);
  tree_field_set_int(self, "cam_apply_bound_cam", cam_idx);
  tree_field_set_int(self, "cam_apply_bound_osd", st.apply_osd_live);
  tree_field_set_int(self, "cam_apply_bound_bones", st.apply_bones);
  tree_field_set_int(self, "cam_apply_bound_gauges", st.apply_gauges);
  tree_field_set_int(self, "cam_apply_bound_count", st.apply_count);
  tree_field_set_int(self, "cam_apply_bound_active", active);
  tree_field_set_int(self, "cam_apply_rtext2_ok", rtext2_ok);
  // PE Cam_queueLookMoveZoom / channel21 / A..O Text_createRenderInst OOS.
  tree_field_set_int(self, "cam_apply_look_move_zoom_oos", 1);
  tree_field_set_int(self, "cam_apply_text_inst_oos",
                     gauges && rtext2_ok == 0 ? 1 : 0);
  tree_field_set_int(self, "cam_apply_channel21_oos", 1);
}

float java_game_parts_bodypart_Chassis_getTorque(InvObject* self, float RPM,
                                                 float boost) {
  // PE @ 0x0043C7F0 size 0x138 (IDA Chassis_getTorque).
  // Unbox this+RPM+boost (acc0/rpm/boost preset 0). Native.ptr
  // (dword_62E008)==0 → fld acc0 (0). NO Mighty. Walk=setTorque @
  // 0x0043C930: inner=*(handle+0xC); 0 → 0.0. [inner+0x4C]!=1 →
  // vtbl+0x14(0). sub_5447D0(0xA0000000 bytes 00 00 00 a0) test
  // 80000000h sign → 0.0. vtbl+0xC(1.0f=0x3F800000). second=*(obj+0x44),
  // edi=*(obj+0x4C). sub_5447D0(0x20000000) sign → 0.0; 2nd vtbl+0xC
  // null-check only. hdr=*(edi+0x1FBC) (int_convert 8124).
  // t0=lerp_xy_table0(RPM,hdr) @ 0x0043BE70 size 0x70: count[+0]
  // pairs[+4] stride 8; x<first/count<=1/past last → 0.
  // t1=lerp_xy_table1(RPM,hdr) @ 0x0043BEE0 size 0x71: count[+8]
  // pairs[+0xC]. fld flt_5F08F0 (1.0) fsub boost (unclamped).
  // return t0*(1-boost)*[edi+0x1DD0]+t1*boost*[edi+0x1DD4]
  // (int_convert 7632/7636; scales WRITE by setTorque wear).
  // Miss → fld acc0=0. Callees: JVM_UnboxArg @ 0x0045D910,
  // JVM_vm_get_int_field @ 0x0042AB50, sub_5447D0 @ 0x005447D0,
  // lerp_xy_table0/1. Xref: Natives_Register_Partial @ 0x004427E6.
  // NOT DynoData.getTorque @ 0x0046B0F0 (other FQN).
  // GAP: PE miss → 0.0; host TREE peaks (engine_torque/engine_torque2 —
  // setTorque twin) with curve≡1 (no +0x1FBC / lerp_xy / wear +0x1DD0).
  // DynoData only if peaks unset (host-only; not PE Chassis).
  // race123 PARTIAL: TREE peaks; no +0x1FBC lerp tables / wear scales.
  // race125 PARTIAL: peaks × force_update_wear_k (stand-in for lerp ×
  // +0x1DD0); no invent hdr lerp blob. scale0/1 = +0x1DD0 raw (k after
  // forceUpdate, k*t after setTorque) — do NOT multiply peaks×scale
  // (would double-count setTorque's t).
  if (!self) return 0.f;
  // PE: lerp_xy_table0/1(RPM, hdr) × wear scales. Host: real hdr tables
  // when forceUpdate rebuilt from DynoData (C18 side-band).
  if (uint8_t* hdr = chassis_hdr_ptr(self)) {
    auto it = g_chassis_hdr.find(self);
    if (it != g_chassis_hdr.end()) {
      const int32_t c0 = static_cast<int32_t>(hdr_get_u32(hdr, 0x00));
      const int32_t c1 = static_cast<int32_t>(hdr_get_u32(hdr, 0x08));
      if (c0 > 1 && !it->second.lerp0.empty()) {
        const float t0 =
            chassis_hdr_lerp_xy(it->second.lerp0.data(), c0, RPM);
        const float t1 = (c1 > 1 && !it->second.lerp1.empty())
                             ? chassis_hdr_lerp_xy(it->second.lerp1.data(),
                                                   c1, RPM)
                             : t0;
        float k = tree_field_get_float(self, "force_update_wear_k");
        if (k <= 0.f) k = 1.f;
        return ((1.f - boost) * t0 + boost * t1) * k;
      }
    }
  }
  const float s0 = tree_field_get_float(self, "engine_torque");
  const float s1 = tree_field_get_float(self, "engine_torque2");
  if (s0 != 0.f || s1 != 0.f) {
    (void)RPM;  // no hdr curve → peaks stand-in (curve≡1)
    float k = tree_field_get_float(self, "force_update_wear_k");
    if (k <= 0.f) k = 1.f;
    const float peak1 = (s1 != 0.f) ? s1 : s0;
    return ((1.f - boost) * s0 + boost * peak1) * k;
  }
  InvObject* engine = tree_field_get_obj(self, "engine");
  InvObject* dyno =
      engine ? tree_field_get_obj(engine, "dynodata") : nullptr;
  if (!dyno) dyno = tree_field_get_obj(self, "dynodata");
  if (dyno) return java_game_parts_DynoData_getTorque(dyno, RPM, boost);
  (void)RPM;
  return 0.f;
}