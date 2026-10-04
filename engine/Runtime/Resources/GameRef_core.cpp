#include "host_objects.hpp"
#include "natives.hpp"
#include "runtime.hpp"
#include "rpak.hpp"
#include "jvm.hpp"
#include "tree_interp.hpp"
#include "render_d3d9.hpp"
#include "input_win32.hpp"
#include "video_fmv.hpp"
#include "Resources.h"
#include "System.h"
#include "GameRef.h"
#include "GameRef_internal.hpp"
#include "../Parts/Body/Chassis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace inv {

InvObject* gameref_live_phys_key(InvObject* self) {
  if (!self) return nullptr;
  if (InvObject* chassis = tree_field_get_obj(self, "chassis")) {
    if (physics_shape(chassis) != 0) return chassis;
  }
  if (physics_shape(self) != 0) return self;
  return nullptr;
}

// Soft GetWorldPos ch=2 stand-in into floats. Prefer PhysicsRef on live
// phys key. Does NOT lock GameRef g_gr_mu and does NOT call GameRef_getPos
// (safe under sort_compact / tickPhys). Returns true when Soft phys
// path produced a sample. PhysicsRef locks Resources.cpp g_gr_mu only
// (≠ this TU's non-recursive GameRef g_gr_mu).
bool gameref_soft_phys_sample_pos(InvObject* self, float* ox, float* oy,
                                         float* oz) {
  if (ox) *ox = 0.f;
  if (oy) *oy = 0.f;
  if (oz) *oz = 0.f;
  InvObject* key = gameref_live_phys_key(self);
  if (!key) return false;
  InvObject* pp = java_util_resource_PhysicsRef_getPos(key);
  if (!pp) return false;
  float x = 0.f, y = 0.f, z = 0.f;
  vec3_get(pp, &x, &y, &z);
  if (ox) *ox = x;
  if (oy) *oy = y;
  if (oz) *oz = z;
  return true;
}

// Soft GII_VEL ch=3 stand-in into floats. Same lock rules as
// gameref_soft_phys_sample_pos — PhysicsRef only, never GameRef_getVel.
bool gameref_soft_phys_sample_vel(InvObject* self, float* ox, float* oy,
                                         float* oz) {
  if (ox) *ox = 0.f;
  if (oy) *oy = 0.f;
  if (oz) *oz = 0.f;
  InvObject* key = gameref_live_phys_key(self);
  if (!key) return false;
  InvObject* vv = java_util_resource_PhysicsRef_getVel(key);
  if (!vv) return false;
  float x = 0.f, y = 0.f, z = 0.f;
  vec3_get(vv, &x, &y, &z);
  if (ox) *ox = x;
  if (oy) *oy = y;
  if (oz) *oz = z;
  return true;
}

// Soft xyz when caller already holds GameRef g_gr_mu: PhysicsRef first,
// else GameRefState (PE GetWorldPos fail → inner+0x84). Never
// GameRef_getPos / getVel (non-recursive g_gr_mu).
bool gameref_soft_sample_pos_under_mu(InvObject* self, float* ox,
                                             float* oy, float* oz) {
  if (ox) *ox = 0.f;
  if (oy) *oy = 0.f;
  if (oz) *oz = 0.f;
  if (!self) return false;
  float px = 0.f, py = 0.f, pz = 0.f;
  if (gameref_soft_phys_sample_pos(self, &px, &py, &pz)) {
    if (ox) *ox = px;
    if (oy) *oy = py;
    if (oz) *oz = pz;
    auto it = g_refs.find(self);
    if (it != g_refs.end() && !it->second.empty) {
      it->second.px = px;
      it->second.py = py;
      it->second.pz = pz;
    }
    return true;
  }
  auto it = g_refs.find(self);
  if (it == g_refs.end() || it->second.empty) return false;
  if (ox) *ox = it->second.px;
  if (oy) *oy = it->second.py;
  if (oz) *oz = it->second.pz;
  return true;
}

namespace {

// Soft PE GameRef_applyWorldXform @ 0x0048B440 size 0xbb (setPos @
// 0x47E437 / setMatrix @ 0x47E5D9 / setState @ 0x47E86A):
//   handle[+8]==0 → 0; inner=*[handle+0xC]==0 → 0;
//   [inner+0x4C]!=1 → vtbl+0x14(1.0f); ResHandle_PrepareLod(A0000000);
//   mid=vtbl+0xC(1.0f); nested=*(mid+0x44); nest PrepareLod(80000000);
//   nest_pay=vtbl+0xC; leaf=*(nest_pay+0xC);
//   leaf → leaf.vtbl+0x1C(leaf, *(mid+0x4C), xformArg) → 1.
// Same mid→nest→leaf hop as setParent type1 @ 0x48ACC1 (vtbl+0x20 there).
// Host: wire Native.ptr; prove hop; soft vtbl+0x1C = PhysicsRef_setMatrix
// on gameref_live_phys_key (chassis / self shape). Track.lockCar /
// City bots: setParent(map) then setMatrix(pos,ori) — pose must hit phys.
// PrepareLod / leaf vtbl body OOS. Returns true when phys write ran.
bool gameref_soft_apply_world_xform(InvObject* self, InvObject* p,
                                    InvObject* o) {
  if (!self) return false;
  native_ptr_ensure(self);
  bool hop_ok = false;
  if (void* node = native_ptr_node(self)) {
    constexpr int32_t kTag = static_cast<int32_t>(0xA0000000u);
    if (void* mid = res_handle_get_payload(node, kTag)) {
      auto* mid_b = reinterpret_cast<unsigned char*>(mid);
      void* nested = *reinterpret_cast<void**>(mid_b + 0x44);
      void* mid_block = *reinterpret_cast<void**>(mid_b + 0x4C);
      if (nested) {
        if (void* nest_pay = res_handle_get_payload(nested, kTag)) {
          void* leaf = *reinterpret_cast<void**>(
              reinterpret_cast<unsigned char*>(nest_pay) + 0xC);
          // PE @ 0x48B4D3..0x48B4E7: leaf && mid_block → vtbl+0x1C.
          hop_ok = (leaf != nullptr && mid_block != nullptr);
        }
      }
    }
  }
  // Soft phys write even when mid hop not wired yet (host cars often have
  // tree "chassis" / self shape before Native.ptr nest is fully linked).
  if (InvObject* key = gameref_live_phys_key(self)) {
    java_util_resource_PhysicsRef_setMatrix(key, p, o);
    return true;
  }
  (void)hop_ok;
  return false;
}

}  // namespace

InvObject* java_util_resource_GameRef_create(InvObject* self, InvObject* parent,
                                             InvObject* type, InvObject* params,
                                             InvObject* alias) {
  // PE @ 0x0047D7B0 size 0x147 (327). JNI
  // (LGameRef;LGameRef;Ljava/lang/String;Ljava/lang/String;)LGameType;.
  // UnboxArg @ 0x0045D910: dest0=this, dest1=parent (overwrites CallInfo),
  // dest2=type, dest3=params, dest4=alias. Native.ptr via dword_62E008
  // (JVM_vm_get_int_field @ 0x0042AB50). handle==0 → "!"+"Mighty ERROR"
  // (CRT_strcat_n_thunk + Engine_ErrorLogPrintf) return 0.
  // parent==null → g_WorldTreeRoot @ 0x636460; require parent[+0xC]!=0 else
  // silent return 0. Factory Engine_CreateGameInstance @ 0x53A5E0
  // (parent, type, params, alias) — NOT create_native's
  // Engine_CreateGameInstanceNative @ 0x53A2A0. Factory RESTYPE: if type
  // RID (handle+8)!=0 and (inner==0 || [inner+0x4C]!=RESTYPE_GAME=8) →
  // Fatal "create: Wrong GameType!". Alias null → strncpy "_gameinst" (31).
  // Params passed to GameType ctor (vtbl+0xC), not parsed in this native.
  // Relink: if handle[+0xC]!=new_inst → ResHandle_Unlink old; inline
  // ResHandle_Link at inst+0x44/+0x48; handle+8=inst+0x50. handle[+0xC]==0
  // → 0. Else ResHandle_getPayload(inst, 0x80000000, 1.0f=0x3F800000, 0, 0);
  // null→0; return *[eax+0x50] Java GameType (null→0).
  // Host: !self = handle 0. Soft RESTYPE only when R(type).type known
  // non-zero and !=8 (PE would Fatal). Null parent = world-root stand-in.
  // W17D: Engine_LoadGameInit @ 0x47d824 + Link/Rebind Native.ptr→GI;
  // getPayload GameType via engine_load_game_init_script when attached.
  if (!self) return nullptr;

  const int32_t type_id =
      type ? java_util_resource_ResourceRef_id(type) : 0;
  // PE Fatal "create: Wrong GameType!" when type RID bound and
  // [inner+0x4C]!=RESTYPE_GAME=8. Host ResourceRef_type is often a non-8
  // stand-in for cars/traffic — never soft-null on that (breaks addTrafficP).

  const RpakEntry* ent = type_id ? rpak_find_entry(type_id) : nullptr;
  // PE null alias → "_gameinst"; host keeps empty for VehicleType detect.
  const char* alias_cstr = alias ? string_cstr(alias) : "";
  const std::string fqn = script_fqn_for_entry(ent);
  const bool want_vt =
      (alias_cstr && std::strstr(alias_cstr, "VehicleType")) ||
      (!fqn.empty() && fqn.size() >= 3 &&
       fqn.compare(fqn.size() - 3, 3, "_VT") == 0);

  float pose[6] = {};
  const bool have_pose =
      parse_instance_params(params ? string_cstr(params) : nullptr, pose);

  auto finish_create = [&](InvObject* inst) {
    // Phase 2.59: world-tree parent → getParentID (Part.addPart install check).
    // PE null parent already resolved to g_WorldTreeRoot before factory.
    if (parent) {
      java_util_resource_GameRef_setParent(inst, parent);
      if (self != inst) java_util_resource_GameRef_setParent(self, parent);
    }
    if (have_pose) apply_instance_pose(inst, pose);
  };

  // W17D — PE @ 0x47d824 Engine_LoadGameInit(parent|g_WorldTreeRoot@636460,
  // type, params, alias) then @ 0x47d833.. ResHandle_Link Native.ptr→GI
  // (+0x48/+0x50) then getPayload→*[+0x50] GameType. Contrasts create_native
  // @ 0x47d975 → Engine_CreateGameInstanceNative @ 0x53A2A0 (still OOS).
  // IDA: voidEvent/setParent do NOT produce chassis+0x115C / phys+0x78×0x68
  // (those = Chassis_attachPartSlot walk + Part_buildPhysSlotTable @ 0x46EAE0).
  {
    alignas(4) uint8_t parent_rh[16]{};
    alignas(4) uint8_t type_rh[16]{};
    InvObject* lgi_parent = parent;
    if (!lgi_parent)
      lgi_parent = java_util_resource_ResourceRef_getWTRoot(self);
    const int32_t parent_key =
        lgi_parent ? java_util_resource_ResourceRef_id(lgi_parent) : 0;
    *reinterpret_cast<int32_t*>(parent_rh + 8) = parent_key;
    if (type_id != 0) *reinterpret_cast<int32_t*>(type_rh + 8) = type_id;
    const char* params_cstr = params ? string_cstr(params) : nullptr;
    const char* lgi_alias =
        (alias_cstr && alias_cstr[0]) ? alias_cstr : "_gameinst";
    void* gi = nullptr;
    if (parent_key != 0) {
      gi = engine_load_game_init(
          parent_rh, type_id != 0 ? type_rh : nullptr,
          (params_cstr && params_cstr[0]) ? params_cstr : "", lgi_alias);
    }
    if (gi) {
      const int32_t gikey =
          *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(gi) + 0x50);
      tree_field_set_int(self, "loadgameinit_gi_key", gikey);
      // PE inline Link @ 0x47d846..0x47d86a on Native.ptr (dword_62E008).
      if (HostNativeHandle* self_h = native_ptr_ensure(self))
        void_event_loadgameinit_rebind(self_h, gi);
      // Non-VT: prefer LoadGameInit THRD-CREATE script (PE return GameType*).
      if (!want_vt) {
        if (InvObject* scr = engine_load_game_init_script(gi)) {
          java_util_resource_ResourceRef_set(self, type_id);
          if (type) java_util_resource_RenderRef_setType(self, type);
          {
            std::lock_guard<std::mutex> lock(g_gr_mu);
            bind_gameref(scr, parent, fqn, alias_cstr);
            bind_gameref(self, parent, fqn, alias_cstr);
            ref(self).script = scr;
          }
          finish_create(scr);
          return scr;
        }
      }
    }
  }

  InvObject* inst = nullptr;
  if (want_vt && !fqn.empty()) {
    // Stand-in for factory GameType ctor + return *[sub_419860+0x50].
    inst = make_vt_host(fqn.c_str());
    java_util_resource_ResourceRef_set(inst, type_id);
    java_util_resource_ResourceRef_set(self, type_id);
    if (type) {
      java_util_resource_RenderRef_setType(inst, type);
      if (self != inst) java_util_resource_RenderRef_setType(self, type);
    }
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      bind_gameref(inst, parent, fqn, alias_cstr);
      bind_gameref(self, parent, fqn, alias_cstr);
      ref(self).script = inst;  // PE handle+8 = inst+0x50 GameType*
    }
    if (Jvm* j = jvm_active()) {
      if (!j->find_class(fqn.c_str())) j->load_class(fqn.c_str());
      j->invoke(fqn.c_str(), "<init>", "(I)V",
                {JvmValue::make_obj(inst), JvmValue::make_int(type_id)}, false);
    }
    finish_create(inst);
    return inst;
  }

  // Generic GameRef bind (non-scripted / unknown alias).
  // PE still returns GameType* at +0x50; host returns bound GameRef as
  // script stand-in (no separate THRD-CREATE object).
  inst = gameref_new();
  java_util_resource_ResourceRef_set(inst, type_id);
  java_util_resource_ResourceRef_set(self, type_id);
  // Keep type_id distinct from instance id for GII_TYPE (Phase 2.96).
  if (type) {
    java_util_resource_RenderRef_setType(inst, type);
    if (self != inst) java_util_resource_RenderRef_setType(self, type);
  }

  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    bind_gameref(inst, parent, fqn, alias_cstr);
    bind_gameref(self, parent, fqn, alias_cstr);
    ref(self).script = inst;
  }
  finish_create(inst);
  return inst;
}

void java_util_resource_GameRef_create_native(InvObject* self, InvObject* parent,
                                              InvObject* type, InvObject* params,
                                              InvObject* alias) {
  // PE @ 0x0047D900 size 0x129. JNI
  // (LGameRef;LGameRef;Ljava/lang/String;Ljava/lang/String;)V.
  // Unbox this+parent+type+params+alias; handle via dword_62E008
  // (0x62E008). handle==0 → Mighty ERROR ("!"+"Mighty ERROR").
  // parent null → g_WorldTreeRoot @ 0x636460; require parent[+0xC]!=0 else
  // silent ret. Factory Engine_CreateGameInstanceNative @ 0x53A2A0
  // (parent, type, script=0, params, alias) — NOT create's
  // Engine_CreateGameInstance @ 0x53A5E0. Relink: inline unlink old,
  // ResHandle_Link(inst+0x44), handle+8=inst+0x50. VOID: no sub_419860,
  // no Java GameType (THRD-CREATE/sub_404E20 skipped). Sibling
  // GameType.createNativeInstance @ 0x481A70.
  // World-enter prep: GameLogic create_native(player, RID, "0,0,0,…",
  // "dummycar") then Track.lockCar setParent(map)+setMatrix — no script
  // at +0x50 (factory a3=0). W18A: engine_create_game_instance_native +
  // Native.ptr Rebind.
  if (!self) return;
  const int32_t type_id =
      type ? java_util_resource_ResourceRef_id(type) : 0;
  // PE factory requires type RID (handle+8)!=0; else returns null / clears.
  if (!type_id) return;

  const char* alias_cstr = alias ? string_cstr(alias) : "";
  const char* params_cstr = params ? string_cstr(params) : nullptr;
  float pose[6] = {};
  const bool have_pose = parse_instance_params(params_cstr, pose);

  java_util_resource_ResourceRef_set(self, type_id);
  if (type) java_util_resource_RenderRef_setType(self, type);

  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& rs = ref(self);
    rs.empty = false;
    rs.parent = parent;
    rs.script = nullptr;  // factory a3=0 → body+0x50 / handle+8
    rs.script_class.clear();
    rs.script_alias = alias_cstr ? alias_cstr : "";
  }

  // PE @ 0x47D975 Engine_CreateGameInstanceNative(parent|wtroot, type,
  // script=0, params, alias) then Link Native.ptr→GI (no THRD-CREATE).
  {
    alignas(4) uint8_t parent_rh[16]{};
    alignas(4) uint8_t type_rh[16]{};
    InvObject* cgi_parent = parent;
    if (!cgi_parent)
      cgi_parent = java_util_resource_ResourceRef_getWTRoot(self);
    const int32_t parent_key =
        cgi_parent ? java_util_resource_ResourceRef_id(cgi_parent) : 0;
    *reinterpret_cast<int32_t*>(parent_rh + 8) = parent_key;
    *reinterpret_cast<int32_t*>(type_rh + 8) = type_id;
    const char* cgi_alias =
        (alias_cstr && alias_cstr[0]) ? alias_cstr : "_gameinst";
    void* gi = nullptr;
    if (parent_key != 0) {
      gi = engine_create_game_instance_native(
          parent_rh, type_rh, /*script=*/nullptr,
          (params_cstr && params_cstr[0]) ? params_cstr : "", cgi_alias);
    }
    if (gi) {
      const int32_t gikey =
          *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(gi) + 0x50);
      tree_field_set_int(self, "create_native_gi_key", gikey);
      if (HostNativeHandle* self_h = native_ptr_ensure(self))
        void_event_loadgameinit_rebind(self_h, gi);
    }
  }

  if (have_pose) apply_instance_pose(self, pose);
  if (parent) java_util_resource_GameRef_setParent(self, parent);
}

int32_t game_logic_init_vehicle_types() {
  const RpakPack* cars = rpak_find_by_name("cars");
  if (!cars) {
    const int32_t opened = java_lang_System_openLib(string_new("cars.rpk"));
    if (!opened) return 0;
  }
  const RpakPack* cars2 = rpak_find_by_name("cars");
  if (!cars2) return 0;
  const int32_t root_id = rpak_make_id(cars2->pack_id, 0x1000);

  InvObject* root = resref_new();
  java_util_resource_ResourceRef_set(root, root_id);

  std::vector<InvObject*> kids;
  for (InvObject* c = java_util_resource_ResourceRef_getFirstChild(root); c;
       c = java_util_resource_ResourceRef_getNextChild(c)) {
    kids.push_back(c);
  }

  InvObject* vts = tree_vector_new();
  Jvm* j = jvm_active();
  // Match Java: for (i = ct.length-1; i >= 0; i--)
  for (int i = static_cast<int>(kids.size()) - 1; i >= 0; --i) {
    InvObject* xa = gameref_new();
    InvObject* vt = java_util_resource_GameRef_create(
        xa, nullptr, kids[static_cast<size_t>(i)], nullptr,
        string_new("VehicleType"));
    if (!vt) continue;
    if (j) {
      const char* cn = tree_host_class(vt);
      if (!cn || !cn[0]) cn = "java.game.VehicleType";
      j->invoke(cn, "init", "()V", {JvmValue::make_obj(vt)}, false);
    }
    tree_vector_add(vts, vt);
  }

  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    g_vehicle_types = vts;
  }
  return tree_vector_size(vts);
}

InvObject* game_logic_vehicle_types() {
  std::lock_guard<std::mutex> lock(g_gr_mu);
  return g_vehicle_types;
}

namespace {

InvObject* pick_weighted(InvObject* vec, int32_t set, bool models) {
  if (!vec) return nullptr;
  const int n = tree_vector_size(vec);
  float gross = 0.f;
  for (int i = n - 1; i >= 0; --i) {
    InvObject* e = tree_vector_element_at(vec, i);
    if (!e) continue;
    const int32_t mask = tree_field_get_int(e, "vehicleSetMask");
    if (!(set & mask)) continue;
    gross += tree_field_get_float(e, "prevalence");
  }
  if (gross <= 0.f) return nullptr;
  float target = gross * java_lang_Math_random();
  float acc = 0.f;
  InvObject* pick = nullptr;
  for (int i = n - 1; i >= 0; --i) {
    InvObject* e = tree_vector_element_at(vec, i);
    if (!e) continue;
    const int32_t mask = tree_field_get_int(e, "vehicleSetMask");
    if (!(set & mask)) continue;
    acc += tree_field_get_float(e, "prevalence");
    if (acc > target) {
      pick = e;
      break;
    }
  }
  (void)models;
  return pick;
}

}  // namespace

InvObject* game_logic_get_vehicle_type(int32_t set) {
  return pick_weighted(game_logic_vehicle_types(), set, false);
}

InvObject* vehicle_type_get_vehicle_descriptor(InvObject* vt, int32_t set,
                                               float param) {
  if (!vt) return nullptr;
  InvObject* vtdarr = tree_field_get_obj(vt, "vtdarr");
  InvObject* vtd = pick_weighted(vtdarr, set, true);
  if (!vtd) return nullptr;

  InvObject* vd = tree_host_new("java.game.VehicleDescriptor");
  const int32_t mid = tree_field_get_int(vtd, "id");
  tree_field_set_int(vd, "id", mid);
  tree_field_set_float(vd, "stockPrestige",
                       tree_field_get_float(vtd, "stockPrestige"));
  tree_field_set_float(vd, "fullPrestige",
                       tree_field_get_float(vtd, "fullPrestige"));
  tree_field_set_float(vd, "stockQM", tree_field_get_float(vtd, "stockQM"));
  tree_field_set_float(vd, "fullQM", tree_field_get_float(vtd, "fullQM"));
  const char* ns = "";
  if (InvObject* name = tree_field_get_obj(vtd, "vehicleName")) {
    ns = string_cstr(name);
    tree_field_set_obj(vd, "vehicleName", string_new(ns ? ns : ""));
  } else {
    tree_field_set_obj(vd, "vehicleName", string_new("unknown"));
  }

  InvObject* model_colors = tree_field_get_obj(vtd, "preferredColorIndexes");
  InvObject* type_colors = tree_field_get_obj(vt, "preferredColorIndexes");
  const int m = tree_vector_size(model_colors);
  const int t = tree_vector_size(type_colors);
  InvObject* colorIndexes = nullptr;
  if (tree_field_get_int(vtd, "exclusiveColors") && m > 0) {
    colorIndexes = model_colors;
  } else if (m > 0 || t > 0) {
    if ((m + t) * java_lang_Math_random() < static_cast<float>(m))
      colorIndexes = model_colors;
    else
      colorIndexes = type_colors;
  }
  if (colorIndexes && tree_vector_size(colorIndexes) > 0) {
    const int idx = static_cast<int>(
        java_lang_Math_random() *
        static_cast<float>(tree_vector_size(colorIndexes)));
    InvObject* boxed = tree_vector_element_at(colorIndexes, idx);
    tree_field_set_int(vd, "colorIndex", tree_field_get_int(boxed, "value"));
  }

  const float minP = tree_field_get_float(vtd, "minPower");
  const float maxP = tree_field_get_float(vtd, "maxPower");
  const float minO = tree_field_get_float(vtd, "minOptical");
  const float maxO = tree_field_get_float(vtd, "maxOptical");
  const float minT = tree_field_get_float(vtd, "minTear");
  const float maxT = tree_field_get_float(vtd, "maxTear");
  const float minW = tree_field_get_float(vtd, "minWear");
  const float maxW = tree_field_get_float(vtd, "maxWear");

  if (param < 0.f) {
    tree_field_set_float(vd, "power",
                         minP + java_lang_Math_random() * (maxP - minP));
    tree_field_set_float(vd, "optical",
                         minO + java_lang_Math_random() * (maxO - minO));
    tree_field_set_float(vd, "tear",
                         minT + java_lang_Math_random() * (maxT - minT));
    tree_field_set_float(vd, "wear",
                         minW + java_lang_Math_random() * (maxW - minW));
  } else {
    if (param > 1.f) param = 1.f;
    tree_field_set_float(vd, "power", minP + param * (maxP - minP));
    tree_field_set_float(vd, "optical", minO + param * (maxO - minO));
    tree_field_set_float(vd, "tear", minT + param * (maxT - minT));
    tree_field_set_float(vd, "wear", minW + param * (maxW - minW));
  }
  return vd;
}

InvObject* game_logic_get_vehicle_descriptor(int32_t set, float param) {
  InvObject* vt = game_logic_get_vehicle_type(set);
  if (!vt) return nullptr;
  return vehicle_type_get_vehicle_descriptor(vt, set, param);
}

int32_t java_util_resource_GameRef_getFlags(InvObject* self) {
  // PE @ 0x0047DF40 size 0x36 (54): ()I. GameRef_getFlags.
  // Unbox this (JVM_UnboxArg @ 0x0045D910). handle =
  // JVM_vm_get_int_field(this, dword_62E008 @ 0x0042AB50). No handle==0 test
  // (stock derefs [handle+0xC] unconditionally). NO Mighty ERROR (unlike
  // getPos @ 0x0047DAD0). inner=*(handle+0xC) offset 12; inner==0 → 0 @
  // loc_47DF72 (xor esi,esi). else return *(inner+0x54) offset 84. Sole
  // callees: UnboxArg, vm_get_int_field (xref Natives_RegisterAll @ 0x4895AC).
  // Host: GameRefState.flags; g_refs miss = inner 0; !self = unbox null.
  if (!self) return 0;
  std::lock_guard<std::mutex> lock(g_gr_mu);
  const auto it = g_refs.find(self);
  if (it == g_refs.end()) return 0;  // inner==0 @ loc_47DF72
  return it->second.flags;           // mov eax,[eax+54h] @ 0x47df6d
}

void java_util_resource_GameRef_setFlags(InvObject* self, int32_t flags) {
  // PE @ 0x00486C80 size 0x45 (int_convert 69). UnboxArg (I)V: dest0=this
  // (var_4), dest1=flags (arg_0 in-place). Native.ptr dword_62E008;
  // inner=*(handle+0xC). inner==0 → return (no Mighty). Else
  // *(inner+0x54) |= flags (offset 84). flags & 0x10 (GameRef.WORLDTREEROOT)
  // → thiscall GameRef_worldTreeLink @ 0x00544F40 (ecx=inner, push handle):
  // WT node at inner+0xC0 spliced into *(handle+0xC)+0x48 / aux +0x50
  // (Dummy/Osd/MouseCursor/Group setFlags(WORLDTREEROOT) — MainMenu /
  // world-root bootstrap before map enter). Host: |= flags;
  // WORLDTREEROOT → gameref_world_tree_link (owner list head + wt_list_aux).
  if (!self) return;
  constexpr int32_t kWorldTreeRoot = 0x10;
  std::lock_guard<std::mutex> lock(g_gr_mu);
  auto& r = ref(self);
  r.flags |= flags;
  if ((flags & kWorldTreeRoot) != 0) {
    gameref_world_tree_link(self);
    tree_field_set_int(self, "worldtree_root", 1);
  }
}

void java_util_resource_GameRef_clearFlags(InvObject* self, int32_t flags) {
  // PE @ 0x00486CD0 size 0x3f: Unbox this+I. Same walk as getFlags.
  // inner==0 → return. else *(inner+0x54) &= ~flags. No WORLDTREEROOT call.
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_gr_mu);
  ref(self).flags &= ~flags;
}

InvObject* java_util_resource_GameRef_getPos(InvObject* self) {
  // PE @ 0x0047DAD0 size 0x106: Unbox this only. Handle via dword_62E008.
  // handle==0 → Mighty ERROR, return nullptr.
  // *(handle+8)==0 → return nullptr (not Vector3 0,0,0).
  // else GameRef_GetWorldPos @ 0x0048B280 size 0x4e: query channel 2
  // (GII_POS) via Engine_queryGameRefChannel @ 0x00426470; fail → copy
  // xyz from *(handle+0xC)+0x84 (inner+132, int_convert). World-enter
  // readback after setMatrix/setParent(map) uses this path.
  // Host: empty ↔ handle+8==0. Soft ch=2 = PhysicsRef live pose when
  // physics_shape; else GameRefState (inner+0x84 cache). Write-through
  // phys→GameRefState so traffic_car_sample_pos under g_gr_mu stays fresh
  // without re-entering getPos.
  if (!self) return nullptr;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    if (ref(self).empty) return nullptr;
  }
  if (InvObject* key = gameref_live_phys_key(self)) {
    if (InvObject* pp = java_util_resource_PhysicsRef_getPos(key)) {
      float x = 0.f, y = 0.f, z = 0.f;
      vec3_get(pp, &x, &y, &z);
      {
        std::lock_guard<std::mutex> lock(g_gr_mu);
        auto& r = ref(self);
        if (!r.empty) {
          r.px = x;
          r.py = y;
          r.pz = z;
        }
      }
      return pp;
    }
  }
  std::lock_guard<std::mutex> lock(g_gr_mu);
  auto& r = ref(self);
  if (r.empty) return nullptr;
  return vec3_new(r.px, r.py, r.pz);
}

InvObject* java_util_resource_GameRef_getOri(InvObject* self) {
  // PE @ 0x0047DBE0: Unbox this. Handle 0 → Mighty ERROR + nullptr.
  // NO handle+8 skip (unlike getPos @ 0x0047DAD0). Always alloc Ypr
  // (0x1C) if handle≠0. GameRef_GetWorldYpr @ 0x0048B300 (was
  // GameRef_readOri): query ch=1 then Ypr_fromMatrix @ 0x00551C90.
  // Host: !self → nullptr. Soft ch=1 = PhysicsRef ori when shape;
  // else GameRefState. Empty still Ypr(0,0,0).
  if (!self) return nullptr;
  if (InvObject* key = gameref_live_phys_key(self)) {
    if (InvObject* oo = java_util_resource_PhysicsRef_getOri(key)) {
      float yaw = 0.f, pitch = 0.f, roll = 0.f;
      ypr_get(oo, &yaw, &pitch, &roll);
      {
        std::lock_guard<std::mutex> lock(g_gr_mu);
        auto& r = ref(self);
        r.oy = yaw;
        r.op = pitch;
        r.or_ = roll;
      }
      return oo;
    }
  }
  std::lock_guard<std::mutex> lock(g_gr_mu);
  auto& r = ref(self);
  return ypr_new(r.oy, r.op, r.or_);
}

InvObject* java_util_resource_GameRef_getVel(InvObject* self) {
  // PE @ 0x0047DCE0 size 0x101 (257) end ~0x47DDE0.
  // GameRef.getVel()Ljava.lang.Vector3;
  // Callees: JVM_UnboxArg @ 0x0045D910, JVM_vm_get_int_field @ 0x0042AB50
  // (dword_62E008 Native.ptr), Engine_queryGameRefChannel @ 0x00426470
  // (thiscall ecx=g_EngineState @ 0x636338; args handle, channel=3, out
  // float[3]), Engine_malloc @ 0x0054F560 (0x1C), JVM_getClass /
  // JVM_Instance_initialize, JVM_vm_set_float_field x/y/z, Mighty path
  // CRT_strcat_n_thunk + Engine_ErrorLogPrintf.
  // Flow: Unbox this. handle==0 → "!Mighty ERROR" + return nullptr (only
  // null). NO [handle+8] early-out (getPos @ 0x0047DAD0 has one). Else
  // always query channel 3 then always alloc java.lang.Vector3 — even if
  // helper returns 0 without writing out ([handle+8]==0 @ 0x426479).
  // Channel 3 = velocity (same push 3 as Vehicle.getSpeedSquare @
  // 0x00480500). Do not rename Engine_queryGameRefChannel (164 xrefs).
  // Host: !self → nullptr (handle-0 analogue, silent — no Mighty log).
  // Soft ch=3 = PhysicsRef vel when physics_shape (same prefer as
  // Vehicle.getSpeedSquare / gameref_soft_phys_sample_vel). Else
  // GameRefState vx (setMatrix zeros; setState restores). City / Track:
  // empty must not be null Vector3. Write-through under g_gr_mu — never
  // nest getPos/getVel while already holding it.
  if (!self) return nullptr;
  if (InvObject* key = gameref_live_phys_key(self)) {
    if (InvObject* vv = java_util_resource_PhysicsRef_getVel(key)) {
      float vx = 0.f, vy = 0.f, vz = 0.f;
      vec3_get(vv, &vx, &vy, &vz);
      {
        std::lock_guard<std::mutex> lock(g_gr_mu);
        auto& r = ref(self);
        r.vx = vx;
        r.vy = vy;
        r.vz = vz;
      }
      return vv;
    }
  }
  std::lock_guard<std::mutex> lock(g_gr_mu);
  auto& r = ref(self);
  return vec3_new(r.vx, r.vy, r.vz);
}

void java_util_resource_GameRef_setPos(InvObject* self, InvObject* v) {
  // PE @ 0x0047E350. UnboxArg (L)V: dest0=this (var_1C), dest1=Vector3
  // (var_24). Native.ptr==0 → "!Mighty ERROR". No Vector3 null test —
  // reads float fields "x"/"y"/"z" via sub_42A560. Vec3_store @ 0x00551C70
  // zeros local YPR (0,0,0). Pack "ffffff" (pos+ypr) via sub_551FC0 then
  // GameRef_applyWorldXform @ 0x0048B440 (same pose write as setMatrix).
  // Garage.lockCar: player.car.setPos(defCarPos). Host: setMatrix(self, v,
  // nullptr) zeros ori; null Vector3 → pos 0,0,0 (host-safe; PE would
  // crash on null field read).
  java_util_resource_GameRef_setMatrix(self, v, nullptr);
}

void java_util_resource_GameRef_setMatrix(InvObject* self, InvObject* p, InvObject* o) {
  // PE @ 0x0047E490 size 0x1a0: Unbox this+Vector3+Ypr. Handle 0 →
  // Mighty ERROR. Null Vector3 → pos 0,0,0. Vec3_store @ 0x551C70 zeros
  // YPR; Ypr fields overwrite if non-null. Pack "ffffff" via
  // GameRef_xformArg_* @ 0x551FA0/FC0 then GameRef_applyWorldXform @
  // 0x0048B440 (mid→nest→leaf → phys vtbl+0x1C). No freeze-in-place skip.
  // Vehicle.create: setMatrix(null,null) after chassis.forceUpdate —
  // "ezzel leallitjuk a fizikajat". Track.lockCar / City: setParent(map)
  // then setMatrix(posStart,oriStart) — world-enter pose.
  // Host: GameRefState (+0x84 stand-in) + render; soft applyWorldXform
  // dual-hop + PhysicsRef_setMatrix (zeros phys lin/ang vel).
  if (!self) return;
  float x = 0.f, y = 0.f, z = 0.f;
  if (p) vec3_get(p, &x, &y, &z);
  float yaw = 0.f, pitch = 0.f, roll = 0.f;
  if (o) ypr_get(o, &yaw, &pitch, &roll);
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& r = ref(self);
    r.px = x;
    r.py = y;
    r.pz = z;
    r.oy = yaw;
    r.op = pitch;
    r.or_ = roll;
    r.vx = r.vy = r.vz = 0;
    r.empty = false;
  }
  render_d3d9_mesh_set_transform(self, x, y, z, yaw, pitch, roll, 1.f, 1.f, 1.f);
  // Soft PE applyWorldXform @ 0x48B440 — see gameref_soft_apply_world_xform.
  (void)gameref_soft_apply_world_xform(self, p, o);
}

void java_util_resource_GameRef_setParent(InvObject* self, InvObject* newparent) {
  // PE @ 0x0047E2D0 size 0x7b: UnboxArg this+parent. Native.ptr
  // (dword_62E008)==0 → Mighty ERROR, return (no splice). Else thiscall
  // GameRef_setParent_inner @ 0x0048ABA0 (was sub_48ABA0, 16 xrefs).
  // Host: !self / id==0 = handle-0 (silent; PE logs Mighty).
  // PE GameRef_setParent_inner size 0x1bf (447): parent Java null crashes
  // [a2+8]. Boxed parent Native.ptr 0 → jz return 0, no detach. Already
  // parented (ResHandle_getNode→[+0x14]+0x50 == parent ptr) → 1 @ 0x48ABCF
  // BEFORE WT dirty (host: ref.parent==newparent early return).
  // Gate @ 0x48ABDD: child_inner && (parent_inner==0 ||
  //   *[child+0xC8] != *[parent+0xC8] (wt_aux, int_convert 200)) →
  //   ResNode_InvalidateOnClassChange @ 0x545D20 (free +0xD4, vtbl+0x18,
  //   frameStamp, recurse children — OOS on host).
  // Type [inner+0x4C]: 1 INSTANCE → path @ 0x48ACC1 (no dllist); 2–3
  // PHYSICS/RENDER: zero +0x14, GameRef_dllist_unlink @ 0x004A5D00,
  // restore +0x14, append under parent +0x30/+0x38 (host sib_*==parent
  // sentinel); parent WT |= 0x40000 @ 0x48AC19 BEFORE type split
  // (int_convert 262144); WT RelinkWtNode @ 0x544FE0 / PropagateWtRelink
  // @ 0x545120 when (parent+0xC0)!=(child+0xC0) — not ported.
  // World-enter: Track.lockCar setParent(map); releaseCar setParent(player);
  // City/CarMarket/ROCTrack same map↔owner splice.
  // Host type 0 (untyped traffic) keeps dllist stand-in.
  // Null parent / parent id 0 → no-op (PE no detach). Soft traffic spawn
  // setParent(eng+0x311C default) OOS — host uses live parent GameRefs.
  if (!self || java_util_resource_ResourceRef_id(self) == 0) return;
  if (!newparent || java_util_resource_ResourceRef_id(newparent) == 0) return;
  const int32_t restype = java_util_resource_ResourceRef_type(self);
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    // Same parent → PE early return 1 (no re-splice).
    if (ref(self).parent == newparent) return;
    // PE @ 0x48AC0F..0x48AC19: BEFORE type1/2/3 split, when parent has
    // WT at +0xCC, *(wt+0x54) |= 0x40000. Host: parent GameRefState.flags
    // (gameref_list_link also ORs — idempotent).
    constexpr int32_t kWtParentDirty = 0x40000;
    ref(newparent).flags |= kWtParentDirty;
    // Soft Invalidate gate: wt_aux (+0xC8) mismatch → mark dirty only;
    // ResNode_InvalidateOnClassChange body OOS.
    if (ref(self).wt_aux != ref(newparent).wt_aux)
      ref(self).flags |= kWtParentDirty;
    if (restype == 1) {
      // Type-1 INSTANCE @ 0x48ACC1 (IDA):
      //   mid = ResHandle_getPayload(child_inner, 0xA0000000, 1.0, 0, 0)
      //   if mid && (nested=*(mid+0x44)): nest = getPayload(nested, same);
      //     if nest && (leaf=*(nest+0xC)): leaf->vtbl[+0x20](leaf,
      //       *(mid+0x4C), parent_jobject) @ 0x48AD0A
      //       → CameraCtrl_attachSetParent @ 0x00438590 |
      //         Chassis_attachSetParent @ 0x0044C170
      //       → GameRef_handleAttachUnderParent(mid_block, parent) @
      //         0x00429C80 (+ CameraCtrl: setParent_inner(block+0x30);
      //         Chassis: part-handle loop — host OOS)
      //   then if (wt=[parent_inner+0xCC]): pay=getPayload(wt, A0, a3=0);
      //     if pay && [pay+0x40]==53 && *(pay+0x4C) && != -180:
      //       Type53_ensureHandleSlot(*(pay+0x4C)+0xB4, child_handle, 0)
      //         @ 0x004B3EE0 (a2 = child Native.ptr / setParent_inner this)
      // Host: no invented vtbl — CameraCtrl/Chassis body inlined from IDA.
      // type53_ensure_handle_slot = PE lists+malloc+found promote +
      // insertSpatialBounds @ 0x4B3120 + linkNearby @ 0x4B36B0 +
      // promoteSlotOnLink @ 0x4B3AC0 (RE @ 0x537240 OOS).
      native_ptr_ensure(self);
      native_ptr_ensure(newparent);
      HostNativeHandle* child_handle = native_ptr_get(self);
      if (void* node = native_ptr_node(self)) {
        constexpr int32_t kTag = static_cast<int32_t>(0xA0000000u);
        void* mid = res_handle_get_payload(node, kTag);
        if (mid) {
          auto* mid_b = reinterpret_cast<unsigned char*>(mid);
          void* nested = *reinterpret_cast<void**>(mid_b + 0x44);
          void* mid_block = *reinterpret_cast<void**>(mid_b + 0x4C);
          if (nested) {
            void* nest_pay = res_handle_get_payload(nested, kTag);
            if (nest_pay) {
              void* leaf =
                  *reinterpret_cast<void**>(
                      reinterpret_cast<unsigned char*>(nest_pay) + 0xC);
              // PE @ 0x48AD0A leaf.vtbl+0x20 → CameraCtrl_attachSetParent
              // @ 0x00438590: handleAttach(mid_block,parent) then
              // setParent_inner(mid_block+0x30,parent). Chassis part-handle
              // loop @ 0x44C170 still OOS.
              if (leaf && mid_block) {
                gameref_handle_attach_under_parent(mid_block, newparent);
                gameref_list_link(self, newparent);  // InvObject mirror
                // PE lea ecx,[block+0x30]. Host Native.ptr is 16B
                // (mid.block==handle) — only deref emb when block is larger.
                if (mid_block != static_cast<void*>(child_handle)) {
                  gameref_set_parent_inner_handle(
                      reinterpret_cast<unsigned char*>(mid_block) + 0x30,
                      newparent);
                }
              }
            }
          }
          // Type53 after attach (PE runs even if nest/leaf missing).
          if (void* parent_node = native_ptr_node(newparent)) {
            void* wt = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(parent_node) + 0xCC);
            if (wt) {
              void* pay = res_handle_get_payload(wt, kTag);
              if (pay) {
                auto* pay_b = reinterpret_cast<unsigned char*>(pay);
                int32_t ptype = *reinterpret_cast<int32_t*>(pay_b + 0x40);
                void* block = *reinterpret_cast<void**>(pay_b + 0x4C);
                // PE: v24 != 0 && v24 != -180
                const auto block_i = reinterpret_cast<intptr_t>(block);
                if (ptype == 53 && block && block_i != -180) {
                  void* mgr = reinterpret_cast<unsigned char*>(block) + 0xB4;
                  type53_ensure_handle_slot(mgr, child_handle, 0);
                }
              }
            }
          }
        }
      }
      // Parent ptr if attach did not already link (no leaf / mid).
      // Soft: keep parent for Track.lockCar when mid hop not wired yet
      // (PE type1 with Payload==0 falls through return 0).
      if (ref(self).parent != newparent) ref(self).parent = newparent;
    } else if (restype == 2 || restype == 3 || restype == 0) {
      gameref_list_link(self, newparent);
    } else {
      return;  // PE fall-through return 0 (no splice)
    }
  }
  resref_set_parent(self, newparent);
  render_d3d9_mesh_set_parent(self, newparent);
}

void java_util_resource_GameRef_setState(InvObject* self, InvObject* p, InvObject* o,
                                        InvObject* l, InvObject* a) {
  // PE @ 0x0047E630 size 0x291: Unbox this, Vector3 p, Ypr o, Vector3
  // linvel, Vector3 angvel. Handle 0 → Mighty ERROR. p: NO null-check
  // (reads x/y/z — host null→0,0,0). o: Vec3_store zeros YPR; non-null
  // reads y/p/r. linvel/angvel default 0,0,0 if null else x/y/z.
  // Packs 12 floats "ffffffffffff" via GameRef_xformArg_* then single
  // GameRef_applyWorldXform @ 0x0048B440 (same dual-hop as setMatrix).
  // Host: setMatrix → soft applyWorldXform (zeros vx — race80 analogue);
  // then restore linvel on GameRefState and live phys. Angular →
  // physics_set_ang_vel on phys key (Java GameRef has getVel only).
  if (!self) return;
  java_util_resource_GameRef_setMatrix(self, p, o);
  float lx = 0.f, ly = 0.f, lz = 0.f;
  if (l) vec3_get(l, &lx, &ly, &lz);
  float ax = 0.f, ay = 0.f, az = 0.f;
  if (a) vec3_get(a, &ax, &ay, &az);
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& r = ref(self);
    r.vx = lx;
    r.vy = ly;
    r.vz = lz;
  }
  InvObject* key = gameref_live_phys_key(self);
  if (!key) key = self;
  physics_set_velocity(key, lx, ly, lz);
  physics_set_ang_vel(key, ax, ay, az);
}

int32_t java_util_resource_GameRef_isEmpty(InvObject* self) {
  // PE @ 0x00486D10 size 0x8c (140) end 0x486D9B. GameRef.isEmpty()I —
  // Java: "ures gametype az illeto?" Empty flag gated on RESTYPE_GAME=8.
  // Callees: JVM_UnboxArg @ 0x0045D910, JVM_vm_get_int_field @ 0x0042AB50
  // (dword_62E008 Native.ptr), sub_5447D0 @ 0x005447D0. Unbox this;
  // edi=1 default empty @ 0x486d2d. Handle 0 → loc_486D97 return 1 —
  // NO Mighty ERROR (unlike getPos @ 0x0047DAD0). inner=*(handle+0xC)
  // @ 0x486d3e — NOT handle+8. inner==0 → 1. [inner+0x4C]==8 only
  // (ResourceRef.RESTYPE_GAME); type!=8 → 1. INSTANCE_GAME=1 has no
  // success path (unlike isScripted @ 0x00486DA0 / getScriptInstance
  // @ 0x00486F30). Dead cmp eax,edi (type==1) @ 0x486d4d — eax already
  // 8. vtbl+0x14(1.0f=0x3F800000) @ 0x486d53; sub_5447D0(inner,
  // push 0A0000000h, 0, 0) @ 0x486d61/68 — test sign 0x80000000 → 1.
  // vtbl+0xC(1.0f) payload @ 0x486d7d; payload==0 → 1. *(payload+0x10)
  // Class* OR *(payload+0xC) nonzero → loc_486D92 return 0; else 1.
  // (isEmpty-only +0xC vs getScriptInstance type-8 +0x10-only.) Host:
  // g_refs.empty + script_class / res_id mirror +0x10/+0xC on type-8;
  // no +0x50 script slot. pose/RID bind clears empty without GameType
  // (host stand-in when ResourceRef.type!=8 — PE would still return 1).
  // sub_5447D0 / vtbl+0x14 not mirrored. !self = handle 0.
  if (!self) return 1;

  bool empty = true;
  bool mapped = false;
  std::string script_class;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    const auto it = g_refs.find(self);
    if (it != g_refs.end()) {
      mapped = true;
      empty = it->second.empty;
      script_class = it->second.script_class;
    }
  }
  if (!mapped || empty) return 1;  // inner==0 @ loc_486D97

  constexpr int32_t kRestypeGame = 8;
  const int32_t restype = java_util_resource_ResourceRef_type(self);
  if (restype == kRestypeGame) {
    // PE type-8 path only (not isScripted type-1): payload+0x10 Class*,
    // then payload+0xC — no script-instance (+0x50) slot on this native.
    if (!script_class.empty()) return 0;
    if (java_util_resource_ResourceRef_id(self) != 0) return 0;  // +0xC stand-in
    return 1;
  }

  // Host stand-in: type!=8 but empty flag already cleared (traffic/cars/RID).
  // PE jnz loc_486D97 would return 1 here.
  return 0;
}

namespace {

// PE Class_isInheritedFrom @ 0x00404500: walk super at Class+0x1C8 until
// this==want. Host: exact FQN, then JvmClass::super_name (TREE classpath).
bool gameref_script_isa(const std::string& have, const char* want) {
  if (have.empty() || !want || !want[0]) return false;
  if (have == want) return true;
  Jvm* j = jvm_active();
  if (!j) return false;
  if (!j->find_class(have.c_str())) j->load_class(have.c_str());
  const JvmClass* cls = j->find_class(have.c_str());
  for (int depth = 0; cls && depth < 32; ++depth) {
    if (cls->name == want) return true;
    if (cls->super_name.empty()) break;
    if (!j->find_class(cls->super_name.c_str()))
      j->load_class(cls->super_name.c_str());
    cls = j->find_class(cls->super_name.c_str());
  }
  return false;
}

}  // namespace

int32_t java_util_resource_GameRef_isScripted(InvObject* self, InvObject* clazzname) {
  // PE @ 0x00486DA0 size 0x189 (int_convert 393). Unbox this (var_104) +
  // String clazzname (var_108, box+8 C str). clazzname!=0 → JNI `L`+fqn+`;`
  // (sub_551120 / sub_551140). Native.ptr (dword_62E008).
  // Handle 0: xor ebx,ebx @ 0x00486DC3; jz @ 0x00486E1E → loc_486F1E
  // mov eax,ebx (0). NO sub_5513B0 — unlike getPos @ 0x0047DAD0 jz
  // loc_47DB90 ("!" @ 0x612EA4 + "Mighty ERROR" @ 0x612EA8).
  // Contrast isEmpty @ 0x00486D10: jz loc_486D97 edi=1 (also no Mighty);
  // isEmpty requires [inner+0x4C]==RESTYPE_GAME=8 else empty. isScripted
  // accepts INSTANCE_GAME=1 or RESTYPE_GAME=8 else ebx=0.
  // inner=*(handle+0xC) (int_convert 12); 0 → 0. NOT handle+8.
  // [inner+0x4C] INSTANCE_GAME=1: sub_5447D0 sign → 0; vtbl+0xC(1.0f);
  //   *(payload+0x50)==0 → 0 (no script instance). clazzname==0 → 1.
  //   Class_isInheritedFrom_desc @ 0x004044E0 this=*(script+0xC).
  // [inner+0x4C] RESTYPE_GAME=8: vtbl+0x14(1.0f); same sub_5447D0;
  //   *(payload+0x10)==0 → 0 (no script class). clazzname==0 → 1.
  //   *(Class+0x10)!=0 → 0 (interface). isInheritedFrom this=Class.
  // else 0. Compare: script FQN is-a clazzname (superclass), NOT alias /
  // suffix / VehicleType heuristic. Catalog Part vs Set; Java null wrapper.
  // Host: !self / g_refs miss = handle 0 → 0 (no Mighty). Keep getPos.
  if (!self) return 0;
  std::string have;
  InvObject* script = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto it = g_refs.find(self);
    if (it == g_refs.end()) return 0;
    const GameRefState& r = it->second;
    if (!r.script && r.script_class.empty()) return 0;
    have = r.script_class;
    script = r.script;
  }
  if (!clazzname) return 1;
  const char* want = string_cstr(clazzname);
  // Empty C str: PE still wraps `L;` → lookup fail → 0. Only nullptr is "any".
  if (!want || !want[0]) return 0;
  if (have.empty() && script) {
    if (const char* hc = tree_host_class(script)) have = hc;
  }
  return gameref_script_isa(have, want) ? 1 : 0;
}

InvObject* java_util_resource_GameRef_getScriptInstance(InvObject* self) {
  // PE @ 0x00486F30 size 0xe1: Unbox this. Native.ptr (dword_62E008).
  // Handle 0 → null. NO Mighty ERROR (unlike getPos @ 0x0047DAD0;
  // same family as isEmpty @ 0x00486D10 / isScripted @ 0x00486DA0).
  // inner=*(handle+0xC) — NOT handle+8. inner==0 → null.
  // [inner+0x4C] INSTANCE_GAME=1: sub_5447D0(0x80000000) sign → null;
  //   vtbl+0xC(1.0f); *(payload+0x50) raw Java obj (no box).
  // [inner+0x4C] RESTYPE_GAME=8: vtbl+0x14(1.0f); same sub_5447D0;
  //   *(payload+0x10) Class* via Class_boxObject @ 0x00404E20 (fresh
  //   0x1C shell + Class vtbl off_5E7354 + JVM_Instance_initialize;
  //   NO Native.ptr store; returns Class wrapper — NOT script instance).
  // else null. Host: type-1 → GameRefState.script (+0x50). type-8 →
  // fresh tree_host_new(FQN) each call (script_class / tree_host_class)
  // as Class_boxObject stand-in — NOT a real Class shell (sub_403E80 /
  // Engine_malloc 0x1C / off_5E7354 / sub_407DA0 / Class_boxObject @
  // 0x00404E20 OOS — race125 still; needs JVM Class shell).
  // type 0 / unset → script (traffic smoke part_si).
  // !self = handle 0. sub_5447D0 / ResHandle_getPayload / vtbl hops not
  // mirrored.
  if (!self) return nullptr;
  constexpr int32_t kInstanceGame = 1;
  constexpr int32_t kRestypeGame = 8;
  InvObject* script = nullptr;
  std::string script_class;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto it = g_refs.find(self);
    if (it == g_refs.end()) return nullptr;  // inner==0
    script = it->second.script;
    script_class = it->second.script_class;
  }
  const int32_t t = java_util_resource_ResourceRef_type(self);
  if (t == kInstanceGame) {
    // PE: *(payload+0x50)==0 → null (raw jobject, no box).
    return script;
  }
  if (t == kRestypeGame) {
    // PE Class_boxObject @ 0x00404E20: Class* at payload+0x10 → fresh
    // 28-byte Class shell. clazz==0 → null. Host: class_box_object(FQN)
    // (java.lang.Class + name) — NOT live script instance.
    if (script_class.empty() && script) {
      if (const char* hc = tree_host_class(script)) script_class = hc;
    }
    if (script_class.empty()) return nullptr;
    return class_box_object(script_class.c_str());
  }
  // Host untyped (ResourceRef.type==0) after bind_gameref / create.
  return script;
}

int32_t java_util_resource_GameRef_getInfo(InvObject* self, int32_t query,
                                          int32_t subquery) {
  // PE @ 0x0047DDF0 size 0x150 (336). Shared with
  // getInfo(ILjava.lang.String;)I — same native (Natives_RegisterAll
  // data xrefs @ 0x00489666 + 0x00489685). Java getInfo(I) is non-native
  // wrapper → getInfo(q, 0). Unbox this+query+subquery (var_4=query INT
  // GII_*, var_8=subquery int|String*). Native.ptr (dword_62E008)==0 →
  // "!"+"Mighty ERROR"+ErrorLogMsgBox, ret 0. Else: *(handle+8)==0 → 0;
  // lock=*(handle+0xC); lock==0 → 0; [lock+0x4C]!=1 → vtbl+0x14(1.0f);
  // sub_5447D0(lock, 0x80000000, 0.f, 0.f) sign-bit fail → 0;
  // edi=vtbl+0xC(1.0f); edi==0||[edi+0x4C]==0 → 0; same lock walk on
  // [edi+0x44]; then thiscall vtbl+0x3C(ecx=[payload+0xC], [edi+0x4C],
  // query, subquery) → int. NOT MouseCursor path (no sub_426470, no
  // query 59, no float dest). Do NOT rename sub_5447D0 / sub_426470 /
  // JVM_UnboxArg / dword_62E008. Host: GII_* switch stand-in for
  // vtbl+0x3C (subquery used by GII_AXIS).
  if (!self) return 0;
  // Mirrored from GameType / GameInstance.h (Phase 2.96+).
  constexpr int32_t kGiiBone = 1;
  constexpr int32_t kGiiDir = 4;  // GameType.GII_DIR — int° heading
  constexpr int32_t kGiiId = 5;
  constexpr int32_t kGiiType = 6;
  constexpr int32_t kGiiCategory = 7;
  constexpr int32_t kGiiSize = 11;
  constexpr int32_t kGiiOwner = 24;
  constexpr int32_t kGiiAxis = 25;
  constexpr int32_t kGiiCamera = 34;
  constexpr int32_t kGiiRemoveOk = 41;  // == GII_GETOUT_OK
  constexpr int32_t kGiiRender = 48;    // internal camera count (Track)
  constexpr int32_t kGiiCarDrivetype = 52;
  constexpr int32_t kGiiPartCategory = 55;
  constexpr int32_t kGiiCarTrafficPtr = 56;
  constexpr int32_t kGirCatVehicle = 5;
  constexpr int32_t kGirCatPart = 9;
  switch (query) {
    case kGiiDir: {
      // PE: Engine_queryGameRefChannel(GII_DIR=4) / getInfo vtbl+0x3C →
      // int° (Cars dynamarker @ 0x48314B fild * flt_deg2rad @ 0x005F13BC).
      // Host stand-in: live getOri / ori_y → same MSVC trunc as Cars seed.
      float yaw_rad = 0.f, pitch = 0.f, roll = 0.f;
      if (InvObject* ori = java_util_resource_GameRef_getOri(self)) {
        ypr_get(ori, &yaw_rad, &pitch, &roll);
      } else {
        yaw_rad = tree_field_get_float(self, "ori_y");
      }
      (void)pitch;
      (void)roll;
      constexpr float kDeg2Rad = 0.017453292f;  // flt_deg2rad @ 0x005F13BC
      const int32_t deg = static_cast<int32_t>(yaw_rad / kDeg2Rad);
      if (deg != 0) return deg;
      return tree_field_get_int(self, "gii_dir");
    }
    case kGiiId:
      return java_util_resource_ResourceRef_id(self);
    case kGiiType: {
      int32_t tid = java_util_resource_RenderRef_getTypeID(self);
      if (!tid) tid = java_util_resource_ResourceRef_id(self);
      return tid;
    }
    case kGiiCategory: {
      if (tree_field_get_obj(self, "chassis")) return kGirCatVehicle;
      const char* hc = tree_host_class(self);
      if (hc && hc[0]) {
        if (std::strstr(hc, "VehicleType") ||
            std::strstr(hc, "VehicleDescriptor"))
          return 0;
        if (std::strstr(hc, "Vehicle")) return kGirCatVehicle;
        if (std::strstr(hc, "Part") || std::strstr(hc, ".parts."))
          return kGirCatPart;
      }
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto it = g_refs.find(self);
      if (it != g_refs.end()) {
        const std::string& sc = it->second.script_class;
        const std::string& al = it->second.script_alias;
        if (sc.find("VehicleType") == std::string::npos &&
            (sc.find("Vehicle") != std::string::npos ||
             al.find("Vehicle") != std::string::npos))
          return kGirCatVehicle;
        if (sc.find("parts") != std::string::npos ||
            al.find("Part") != std::string::npos)
          return kGirCatPart;
      }
      return 0;
    }
    case kGiiOwner:
      return java_util_resource_ResourceRef_getParentID(self);
    case kGiiRemoveOk: {
      // Mechanic/VisualInventory: reason!=-1 → removable; Garage drag: ==0.
      const char* hc = tree_host_class(self);
      if (hc && std::strstr(hc, "Chassis")) return -1;
      const int32_t n = part_slot_count(self);
      for (int32_t i = 0; i < n; ++i) {
        const int32_t sid = part_slot_id_at(self, i);
        if (sid <= 0) continue;
        if (part_on_slot(self, sid)) return -1;  // dependents still attached
      }
      return 0;
    }
    case kGiiPartCategory: {
      // Mechanic filters: 1=engine 2=body 3=rgear (0 = uncategorized).
      const int32_t stored = tree_field_get_int(self, "part_category");
      if (stored != 0) return stored;
      auto cat_from = [](const char* s) -> int32_t {
        if (!s || !s[0]) return 0;
        if (std::strstr(s, "enginepart") || std::strstr(s, "EnginePart") ||
            std::strstr(s, ".engines."))
          return 1;
        if (std::strstr(s, "rgearpart") || std::strstr(s, "RGear"))
          return 3;
        if (std::strstr(s, "bodypart") || std::strstr(s, "BodyPart"))
          return 2;
        return 0;
      };
      if (int32_t c = cat_from(tree_host_class(self))) return c;
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto it = g_refs.find(self);
      if (it != g_refs.end()) {
        if (int32_t c = cat_from(it->second.script_class.c_str())) return c;
        if (int32_t c = cat_from(it->second.script_alias.c_str())) return c;
      }
      return 0;
    }
    case kGiiCarDrivetype: {
      // Chassis bits DT_FWD=1 / DT_RWD=2 → CarInfo codes
      // 0=none 1=AWD 2=FWD 3=RWD 4=cross.
      InvObject* ch = tree_field_get_obj(self, "chassis");
      if (!ch) ch = self;
      const int32_t bits = tree_field_get_int(ch, "drive_type");
      const int fwd = bits & 1;
      const int rwd = bits & 2;
      if (!fwd && !rwd) return bits ? 4 : 0;
      if (fwd && rwd) return 1;
      if (fwd) return 2;
      return 3;
    }
    case kGiiBone: {
      // Track: new ResourceRef(getInfo(GII_BONE)) → look target id.
      const int32_t stored = tree_field_get_int(self, "gii_bone");
      if (stored != 0) return stored;
      return java_util_resource_ResourceRef_id(self);
    }
    case kGiiSize: {
      // InventoryPanel: createDefCamera(size/100.0) — centimetres.
      const int32_t stored = tree_field_get_int(self, "gii_size");
      if (stored > 0) return stored;
      InvObject* mesh = tree_field_get_obj(self, "visual_mesh");
      if (!mesh) mesh = self;
      float bmin[3] = {}, bmax[3] = {};
      if (render_d3d9_mesh_local_bounds(mesh, bmin, bmax)) {
        const float dx = bmax[0] - bmin[0];
        const float dy = bmax[1] - bmin[1];
        const float dz = bmax[2] - bmin[2];
        float extent = dx;
        if (dy > extent) extent = dy;
        if (dz > extent) extent = dz;
        int32_t cm = static_cast<int32_t>(extent + 0.5f);
        if (cm < 1) cm = 1;
        if (cm > 10000) cm = 10000;
        return cm;
      }
      return 100;  // 1.0 m default
    }
    case kGiiRender: {
      // Track.changeCamInternal: number of onboard cameras.
      const int32_t stored = tree_field_get_int(self, "camera_count");
      if (stored > 0) return stored;
      if (tree_field_get_obj(self, "chassis")) return 1;
      const char* hc = tree_host_class(self);
      if (hc && std::strstr(hc, "Vehicle") && !std::strstr(hc, "VehicleType"))
        return 1;
      return 0;
    }
    case kGiiCamera: {
      const int32_t stored = tree_field_get_int(self, "gii_camera");
      if (stored != 0) return stored;
      return java_util_resource_ResourceRef_id(self);
    }
    case kGiiAxis: {
      // Input.getInput → controller.getInfo(GII_AXIS, axis_id).
      // Milli-units of mapped logical axis (truthy when active).
      const float v = input_map_get_logical(self, subquery);
      if (std::fabs(v) < 0.001f) return 0;
      int32_t iv = static_cast<int32_t>(v * 1000.f);
      if (iv == 0) iv = (v > 0.f) ? 1 : -1;
      return iv;
    }
    case kGiiCarTrafficPtr: {
      // City traffic tracker: opaque ctCar pointer / host id.
      int32_t t = tree_field_get_int(self, "traffic_ptr");
      if (!t) t = tree_field_get_int(self, "gii_traffic");
      return t;
    }
    default:
      return 0;
  }
}  // PE @ 0x0047DDF0

int32_t java_util_resource_GameRef_getInfo_1(InvObject* self, int32_t query,
                                            InvObject* subquery) {
  // PE @ 0x0047DDF0 size 0x150 — same entry as getInfo(II)I. UnboxArg
  // writes String* into subquery dword; vtbl+0x3C receives it unchanged.
  // Contrast getInfo(II): int subquery vs String*; PE walk identical.
  // Host: Catalog GII_INSTALL_OK / GII_COMPATIBLE parse dest id string;
  // other queries forward getInfo(q, 0) (PE would still pass String*).
  constexpr int32_t kGiiInstallOk = 71;
  constexpr int32_t kGiiCompatible = 72;
  if (query != kGiiInstallOk && query != kGiiCompatible)
    return java_util_resource_GameRef_getInfo(self, query, 0);
  if (!self) return 0;
  const char* s = string_cstr(subquery);
  if (!s || !s[0]) return 0;
  while (*s == ' ' || *s == '\t') ++s;
  char* end = nullptr;
  const long dest_id_l = std::strtol(s, &end, 0);
  if (end == s || dest_id_l == 0) return 0;
  const int32_t dest_id = static_cast<int32_t>(dest_id_l);
  InvObject* dest = resref_find_by_id(dest_id);
  if (!dest || dest == self) return 0;

  auto type_or_id = [](InvObject* o) -> int32_t {
    int32_t tid = java_util_resource_RenderRef_getTypeID(o);
    if (!tid) tid = java_util_resource_ResourceRef_id(o);
    return tid;
  };
  auto is_vehicle_like = [](InvObject* o) -> bool {
    if (!o) return false;
    if (tree_field_get_obj(o, "chassis")) return true;
    const char* hc = tree_host_class(o);
    return hc && std::strstr(hc, "Vehicle") &&
           !std::strstr(hc, "VehicleType") &&
           !std::strstr(hc, "VehicleDescriptor");
  };
  auto install_target = [](InvObject* o) -> InvObject* {
    if (!o) return nullptr;
    if (InvObject* ch = tree_field_get_obj(o, "chassis")) return ch;
    return o;
  };
  auto has_free_slot = [&](InvObject* o) -> bool {
    InvObject* root = install_target(o);
    if (!root) return false;
    const int32_t n = part_slot_count(root);
    if (n <= 0) {
      // No slot table yet: Catalog allows 1-step onto a vehicle/chassis.
      return is_vehicle_like(o) || root != o;
    }
    for (int32_t i = 0; i < n; ++i) {
      const int32_t sid = part_slot_id_at(root, i);
      if (sid <= 0) continue;
      if (part_slot_is_disabled(root, sid)) continue;
      if (!part_on_slot(root, sid)) return true;
    }
    return false;
  };

  if (query == kGiiCompatible) {
    const int32_t st = type_or_id(self);
    const int32_t dt = type_or_id(dest);
    if ((st >> 16) != 0 && (st >> 16) == (dt >> 16)) return 1;
    if (is_vehicle_like(dest)) return 1;
    return 0;
  }

  // GII_INSTALL_OK — free install target on dest (or its chassis).
  if (has_free_slot(dest)) return 1;
  // Inventory-to-part: empty mate on the other part itself (not a vehicle).
  if (!is_vehicle_like(dest)) {
    const int32_t n = part_slot_count(dest);
    for (int32_t i = 0; i < n; ++i) {
      const int32_t sid = part_slot_id_at(dest, i);
      if (sid <= 0) continue;
      if (part_slot_is_disabled(dest, sid)) continue;
      if (!part_on_slot(dest, sid)) return 1;
    }
    // Bare part with no slot table can still accept a mate.
    if (n == 0) return 1;
  }
  return 0;
}  // PE @ 0x0047DDF0 (getInfo String overload)


}  // namespace inv
