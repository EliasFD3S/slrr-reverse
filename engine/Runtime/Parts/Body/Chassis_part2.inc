// Fragment 2/2 of Chassis.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


void java_game_parts_bodypart_Chassis_setTorque(InvObject* self, float t) {
  // PE @ 0x0043C930 size 0x16d (IDA Chassis_setTorque).
  // Unbox this + F. var_4 preset 1.0 (0x3F800000 bytes 00 00 80 3F) then
  // dest1. Native.ptr (dword_62E008)==0 → jz loc_43CA99 ret. NO Mighty.
  // Callees: JVM_UnboxArg @ 0x0045D910, JVM_vm_get_int_field @ 0x0042AB50,
  // sub_5447D0 @ 0x005447D0 (×2). Xref: Natives_Register_Partial @ 0x00442805.
  // inner=*(handle+0xC); 0 → ret. [inner+0x4C]!=1 → vtbl+0x14(0).
  // sub_5447D0(0xA0000000) test 80000000h sign → ret. vtbl+0xC(1.0f).
  // second=*(obj+0x44), edi=*(obj+0x4C). [second+0x4C]!=1 → vtbl+0x14(0).
  // sub_5447D0(0x20000000) sign → ret; 2nd vtbl+0xC null-check only.
  // hdr=*(edi+0x1FBC); *(hdr+0xD0)=*(hdr+0xD4)=var_4 (=t; ASM @
  // 0x43CA05/0x43CA0F mov from var_4 — Hex-Rays falsely folds 1.0).
  // wear=*(float*)(edi+0x1E38) fcom flt_5F08F0 (1.0 bytes 00 00 80 3F).
  // C0 (wear<1.0): k=1.0-wear*wear*flt_5F0F00 (0.6 bytes 9a 99 19 3f);
  // else k=flt_5F0EFC (0.1 bytes d0 cc cc 3d).
  // *(edi+0x1DD0)=k*[hdr+0xD0]; *(edi+0x1DD4)=k*[hdr+0xD4].
  // Contrast getTorque @ 0x0043C7F0: same walk then READS +0x1DD0/+0x1DD4
  // via lerp_xy_table0/1 (sub_43BE70/sub_43BEE0).
  // GAP: PE miss → silent ret; host TREE peaks (Java engine_torque /
  // engine_torque2 — no +0x1FBC hdr / wear scale / +0x1DD0 graph).
  // race123 PARTIAL: TREE peaks only; no invent +0x1FBC blob.
  // race125 PARTIAL: hdr_d0/d4=t; scales=k*t (+0x1DD0/+0x1DD4). No +0x1FBC.
  if (!self) return;
  tree_field_set_float(self, "engine_torque", t);
  tree_field_set_float(self, "engine_torque2", t);
  // PE setTorque: hdr[+0xD0/+0xD4]=t; wear@+0x1E38 → k → [+0x1DD0/+0x1DD4]=k*t.
  float wear = tree_field_get_float(self, "wear");
  if (wear == 0.f) wear = tree_field_get_float(self, "chassis_wear");
  const float k = (wear >= 1.f) ? 0.1f : (1.f - wear * wear * 0.6f);
  tree_field_set_float(self, "force_update_hdr_d0", t);  // PE hdr+0xD0
  tree_field_set_float(self, "force_update_hdr_d4", t);  // PE hdr+0xD4
  tree_field_set_float(self, "force_update_wear_k", k);
  tree_field_set_float(self, "force_update_scale0", k * t);
  tree_field_set_float(self, "force_update_scale1", k * t);
  if (uint8_t* hdr = chassis_hdr_ptr(self)) {
    hdr_put_f32(hdr, 0xD0, t);
    hdr_put_f32(hdr, 0xD4, t);
  }
}

void java_game_parts_bodypart_Chassis_setAckermann(InvObject* self, float a) {
  // PE @ 0x00440B70 size 0x80 (IDA Chassis.setAckermann).
  // Unbox this + F (var_4). Native.ptr (dword_62E008)==0 → jz loc_440BED
  // ret. NO Mighty. inner=*(handle+0xC); 0 → ret. [inner+0x4C]!=1 →
  // vtbl+0x14(0). ResHandle_PrepareLod(0xA0000000) test 80000000h sign →
  // ret. vtbl+0xC(1.0f=0x3F800000). ecx=*(obj+0x4C); edx=*(ecx+0x1FBC);
  // *(edx+0xA1C)=F (int_convert 2588). Twin setSteerWheelRadius @
  // 0x00440AF0 size 0x80: same walk, store +0xA14; setSteerWheel (FF) @
  // 0x00440A50 also writes +0xA18.
  // Host: TREE ackermann always; hdr+0xA1C when cloneHdr (+0x1FBC stand-in)
  // live — no invent private clone on miss (PE silent ret without hop).
  // forceUpdate syncs TREE→hdr when blob appears later.
  if (!self) return;
  tree_field_set_float(self, "ackermann", a);
  uint8_t* hdr = chassis_hdr_ptr(self);
  if (!hdr) {
    tree_field_set_int(self, "ackermann_hdr_pending", 1);
    return;
  }
  hdr_put_f32(hdr, 0xA1C, a);
  tree_field_set_int(self, "ackermann_hdr_pending", 0);
}

float java_game_parts_bodypart_Chassis_getMass(InvObject* self) {
  // PE @ 0x0043CAA0 size 0xD5 (IDA Chassis_getMass).
  // Unbox this. Native.ptr (dword_62E008)==0 → fld var_4 (0). NO Mighty.
  // inner=*(handle+0xC); 0 → 0.0. [inner+0x4C]!=1 → vtbl+0x14(0).
  // sub_5447D0(0xA0000000 bytes 00 00 00 a0) sign → 0.0.
  // vtbl+0xC(1.0f=0x3F800000). second=*(obj+0x44), edi=*(obj+0x4C).
  // sub_5447D0(0x20000000). return flt_5F08F0 (bytes 00 00 80 3F = 1.0)
  // fdiv dword [*(*(*(edi+0x13BC))+0x5C)+0x14] (inv_mass → kg).
  // Contrast Part.getMass @ 0x00469290: same +0x14 fdiv via sub_48AEA0
  // plus *(float*)(+0x14)>0.0 else 0. Chassis has no >0 check.
  // Units: CarInfo prints getMass() as kg (*2.2 lb). Host TREE mass is
  // kg (no 1/x — host has no +0x13BC physics graph).
  // GAP: PE miss → 0.0; host 1200 boot stand-in (CarInfo / smoke mass0).
  // race123 PARTIAL: TREE mass; no inv_mass fdiv via +0x13BC.
  // race125 PARTIAL — kg TREE; contrast forceUpdate phys mass*8000 (+0x1E58).
  if (!self) return 0.f;
  float m = tree_field_get_float(self, "mass");
  if (m <= 0.f) m = tree_field_get_float(self, "chassis_mass");
  return m > 0.f ? m : 1200.f;
}

// Phase 2.74: AABB + CM from wheel poses (CarInfo length/width); physics fallback.
static void chassis_bounds(InvObject* self, float mn[3], float mx[3],
                           float cm[3]) {
  mn[0] = mn[1] = mn[2] = 1.0e9f;
  mx[0] = mx[1] = mx[2] = -1.0e9f;
  float sx = 0.f, sy = 0.f, sz = 0.f;
  int n = 0;
  for (int32_t i = 0; i < 4; ++i) {
    float px = 0, py = 0, pz = 0;
    float r = 0.32f;
    InvObject* w = java_game_parts_bodypart_Chassis_getWheel(self, i);
    if (w) {
      if (InvObject* p = java_game_parts_WheelRef_getPos(w))
        vec3_get(p, &px, &py, &pz);
      r = java_game_parts_WheelRef_getRadius(w);
      if (r < 0.05f) r = 0.32f;
    } else if (!part_slot_get_pose(self, 101 + i, &px, &py, &pz, nullptr,
                                   nullptr, nullptr)) {
      continue;
    }
    if (px - r < mn[0]) mn[0] = px - r;
    if (py - r < mn[1]) mn[1] = py - r;
    if (pz - r < mn[2]) mn[2] = pz - r;
    if (px + r > mx[0]) mx[0] = px + r;
    if (py + r > mx[1]) mx[1] = py + r;
    if (pz + r > mx[2]) mx[2] = pz + r;
    sx += px;
    sy += py;
    sz += pz;
    ++n;
  }
  if (n == 0) {
    float hx = 1.f, hy = 0.5f, hz = 2.f;
    physics_extents(self, &hx, &hy, &hz);
    if (hx < 0.1f) hx = 1.f;
    if (hy < 0.1f) hy = 0.5f;
    if (hz < 0.1f) hz = 2.f;
    mn[0] = -hx;
    mn[1] = -hy;
    mn[2] = -hz;
    mx[0] = hx;
    mx[1] = hy;
    mx[2] = hz;
    cm[0] = cm[1] = cm[2] = 0.f;
  } else {
    const float inv = 1.f / static_cast<float>(n);
    cm[0] = sx * inv;
    cm[1] = sy * inv;
    cm[2] = sz * inv;
  }
  // Optional script overrides.
  if (tree_field_get_float(self, "cm_set") > 0.5f) {
    cm[0] = tree_field_get_float(self, "cm_x");
    cm[1] = tree_field_get_float(self, "cm_y");
    cm[2] = tree_field_get_float(self, "cm_z");
  }
}

InvObject* java_game_parts_bodypart_Chassis_getCM(InvObject* self) {
  // PE @ 0x0043CB80 size 0x1ef. Unbox this. Native.ptr (dword_62E008)==0 →
  // var_C/8/4 stay 0.0; still alloc Vector3. NO Mighty ERROR (jz
  // loc_43CCF2). inner=*(handle+0xC); 0 → zeros. Else
  // sub_419860(inner, 0xA0000000 bytes 00 00 00 a0). second=
  // *[eax+0x44], edi=*[eax+0x4C]; sub_419860(0x20000000). esi=
  // *[eax+0xC]. Slot CM: fld [esi+0x1EAC/0x1EB0/0x1EB4] fmul
  // flt_5F0C70 (bytes 00 00 80 bf = -1.0). Then sub_4A6F30 /
  // [ebx+0x13BC]+0x20C stride / fchs / fsub [+0x40]. Any jz →
  // Vector3(0,0,0). !self = handle 0. GAP: PE miss → (0,0,0);
  // host AABB / cm_set (CarInfo / smoke).
  float mn[3], mx[3], cm[3];
  if (!self) return vec3_new(0, 0, 0);
  chassis_bounds(self, mn, mx, cm);
  return vec3_new(cm[0], cm[1], cm[2]);
}

// PE getMin@0x0043D600 / getMax@0x0043D8B0 size 0x2a9 twins.
// Origin @ payload+0x1EAC (NO fmul −1). Parent-walk iff (node+0x58)&4,
// sentinel *(owner+0x13BC), matrix@+0x1C (R rows +0x20/+0x30/+0x40 + T
// +4/+8/+0xC), next@+0x20. Mesh AABB *(*(FINAL_node+0x5C)+0x78) count+0x20
// verts+0x24 stride +0xC init 0 — FINAL after walk (Veh_ensureSceneBound
// @ 0x48AEA0 → scene @ +0x84). Pad flt_5F08E8 (0.1): min + / max −.
// Fail any jz → Vector3(0,0,0). NO Mighty.
// Host +0x1EAC: TREE phys_cm_* (forceUpdate sync) else cm_* else 0.
// Returns walk-final node (for mesh) or gate/self when no walk.
static InvObject* chassis_origin_parent_walk(InvObject* self, float out[3]) {
  out[0] = out[1] = out[2] = 0.f;
  if (!self) return nullptr;
  float ox = tree_field_get_float(self, "phys_cm_x");
  float oy = tree_field_get_float(self, "phys_cm_y");
  float oz = tree_field_get_float(self, "phys_cm_z");
  const bool have_phys_cm =
      tree_field_get_int(self, "phys_cm_set") != 0 || ox != 0.f || oy != 0.f ||
      oz != 0.f;
  if (!have_phys_cm && tree_field_get_float(self, "cm_set") > 0.5f) {
    ox = tree_field_get_float(self, "cm_x");
    oy = tree_field_get_float(self, "cm_y");
    oz = tree_field_get_float(self, "cm_z");
  }
  // Parent-walk: gate bit4@+0x58 → TREE scene_flags&4 on mesh_node /
  // visual_mesh (PE *(scene+0x84)) else self. Sentinel +0x13BC →
  // parent_walk_sentinel / phys_root / part_car_root. Matrix via slot pose.
  InvObject* gate_node = self;
  if (InvObject* mn = tree_field_get_obj(self, "mesh_node")) gate_node = mn;
  else if (InvObject* vm = tree_field_get_obj(self, "visual_mesh"))
    gate_node = vm;
  const int32_t gate_flags = tree_field_get_int(gate_node, "scene_flags");
  const int32_t self_flags = tree_field_get_int(self, "scene_flags");
  InvObject* walk = (gate_flags & 4) != 0 ? gate_node : self;
  if (((gate_flags | self_flags) & 4) != 0) {
    InvObject* sentinel = tree_field_get_obj(self, "parent_walk_sentinel");
    if (!sentinel) sentinel = tree_field_get_obj(self, "phys_root");
    if (!sentinel) sentinel = part_car_root(self);
    for (int depth = 0; depth < 16; ++depth) {
      if (sentinel && walk == sentinel) break;
      if ((tree_field_get_int(walk, "scene_flags") & 4) == 0) break;
      InvObject* parent = tree_field_get_obj(walk, "part_parent");
      if (!parent) break;
      const int32_t sid = tree_field_get_int(walk, "part_parent_slot");
      float px = 0.f, py = 0.f, pz = 0.f, oyaw = 0.f, op = 0.f, or_ = 0.f;
      if (part_slot_get_pose(parent, sid, &px, &py, &pz, &oyaw, &op, &or_)) {
        const float sy = std::sin(oyaw), cy = std::cos(oyaw);
        const float sp = std::sin(op), cp = std::cos(op);
        const float sr = std::sin(or_), cr = std::cos(or_);
        // Same composition as render_d3d9 / PE Ypr rows used in getMin walk.
        const float r00 = sr * sp * sy + cr * cy;
        const float r01 = cr * sp * sy - sr * cy;
        const float r02 = cp * sy;
        const float r10 = sr * cp;
        const float r11 = cr * cp;
        const float r12 = -sp;
        const float r20 = sr * sp * cy - cr * sy;
        const float r21 = cr * sp * cy + sr * sy;
        const float r22 = cp * cy;
        const float nx = ox * r00 + oy * r01 + oz * r02 + px;
        const float ny = ox * r10 + oy * r11 + oz * r12 + py;
        const float nz = ox * r20 + oy * r21 + oz * r22 + pz;
        ox = nx;
        oy = ny;
        oz = nz;
      }
      walk = parent;
    }
  }
  out[0] = ox;
  out[1] = oy;
  out[2] = oz;
  return walk;
}

static void chassis_aabb_origin_mesh(InvObject* self, bool want_max,
                                     float out[3]) {
  float origin[3];
  InvObject* walk_final = chassis_origin_parent_walk(self, origin);
  float ox = origin[0], oy = origin[1], oz = origin[2];
  // PE getMin@0x43d78a / getMax@0x43da3a: mesh ONLY from FINAL walk node
  // *(*(v9+0x5C)+0x78); count@+0x20 verts@+0x24 stride 0xC; init AABB 0.
  // race123: drop self/car mesh fallbacks (host invent) — miss → aabb 0,
  // result = origin ± flt_5F08E8 (0.1) only. NO physics_extents invent.
  float aabb_x = 0.f, aabb_y = 0.f, aabb_z = 0.f;
  bool have_verts = false;
  InvObject* mesh = nullptr;
  InvObject* mesh_owner = walk_final ? walk_final : self;
  if (mesh_owner) {
    mesh = tree_field_get_obj(mesh_owner, "visual_mesh");
    if (!mesh) mesh = tree_field_get_obj(mesh_owner, "mesh");
    if (!mesh) mesh = tree_field_get_obj(mesh_owner, "body_mesh");
  }
  // TREE cached AABB on FINAL node only (PE mesh owner) — init-0 else.
  if (!mesh && mesh_owner &&
      tree_field_get_int(mesh_owner, "aabb_mesh_set") != 0) {
    have_verts = true;
    if (want_max) {
      aabb_x = tree_field_get_float(mesh_owner, "aabb_max_x");
      aabb_y = tree_field_get_float(mesh_owner, "aabb_max_y");
      aabb_z = tree_field_get_float(mesh_owner, "aabb_max_z");
    } else {
      aabb_x = tree_field_get_float(mesh_owner, "aabb_min_x");
      aabb_y = tree_field_get_float(mesh_owner, "aabb_min_y");
      aabb_z = tree_field_get_float(mesh_owner, "aabb_min_z");
    }
  }
  if (!have_verts && mesh && render_d3d9_mesh_ready(mesh)) {
    float sample[3 * 256];
    const int32_t n = render_d3d9_mesh_copy_positions(mesh, sample, 256);
    for (int32_t i = 0; i < n; ++i) {
      const float vx = sample[i * 3 + 0];
      const float vy = sample[i * 3 + 1];
      const float vz = sample[i * 3 + 2];
      if (want_max) {
        if (vx >= aabb_x) aabb_x = vx;
        if (vy >= aabb_y) aabb_y = vy;
        if (vz >= aabb_z) aabb_z = vz;
      } else {
        if (vx <= aabb_x) aabb_x = vx;
        if (vy <= aabb_y) aabb_y = vy;
        if (vz <= aabb_z) aabb_z = vz;
      }
    }
    if (n > 0) have_verts = true;
  }
  (void)have_verts;  // PE: no verts → aabb remains 0
  constexpr float kPad = 0.1f;  // flt_5F08E8 @ 0x005F08E8
  if (want_max) {
    out[0] = ox + aabb_x - kPad;
    out[1] = oy + aabb_y - kPad;
    out[2] = oz + aabb_z - kPad;
  } else {
    out[0] = ox + aabb_x + kPad;
    out[1] = oy + aabb_y + kPad;
    out[2] = oz + aabb_z + kPad;
  }
}

InvObject* java_game_parts_bodypart_Chassis_getMin(InvObject* self) {
  // PE @ 0x0043D600 size 0x2a9. Host: shared chassis_aabb_origin_mesh(min,+0.1).
  // race125 PARTIAL: FINAL-node mesh only (ASM @ 0x43d78a *(*(v9+0x5C)+0x78));
  // AABB init 0; pad +flt_5F08E8(0.1). No Veh_ensureSceneBound(+0x84) /
  // dual Res hop / raw parent mat@+0x1C — TREE Ypr stand-in.
  if (!self) return vec3_new(0, 0, 0);
  float v[3];
  chassis_aabb_origin_mesh(self, /*want_max=*/false, v);
  return vec3_new(v[0], v[1], v[2]);
}

InvObject* java_game_parts_bodypart_Chassis_getMax(InvObject* self) {
  // PE @ 0x0043D8B0 twin getMin: max verts (>= / Z <= then take) then −0.1.
  // NOT wheels±r. race125 PARTIAL: same FINAL-node mesh path (ASM @ 0x43da3a).
  if (!self) return vec3_new(0, 0, 0);
  float v[3];
  chassis_aabb_origin_mesh(self, /*want_max=*/true, v);
  return vec3_new(v[0], v[1], v[2]);
}

InvObject* java_game_parts_bodypart_Chassis_getWheelPos(InvObject* self,
                                                       int32_t n) {
  // PE @ 0x0043CE30 size 0x244 (IDA Chassis_getWheelPos).
  // Unbox this+n (var_1C/var_20). xyz init 0. Native.ptr
  // (dword_62E008)==0 → still Engine_malloc Vector3(0,0,0). NO Mighty.
  // NO +0x1F40 count check (contrast getWheel @ 0x00440C80: id>=count
  // → null; id<0 → null). Dual Res via sub_419860(0xA0000000 then
  // 0x20000000). veh=[node+0x4C], data=[child+0xC]. Load CM @ data+0x1EAC;
  // if scene [+0x84] flags&4 walk parent mats until [veh+0x13BC]. Then +=
  // wheel[n] local @ *[veh+0x13E4]+n*0x2B4+0x78 (ASM LEA @ 0x43CFB9 —
  // Hex-Rays drops index; PE has NO bounds on n). ALWAYS Vector3 — never
  // null (fail → 0,0,0). Host: shared origin walk + phys slot +0x78; table
  // is 8 slots so n∉[0,8) → local 0 (PARTIAL vs PE unbounded LEA). race125:
  // ASM @ 0x43cfb9..0x43cff3: LEA n*0x2B4 then fadd [base+lea+0x78/+0x7C/+0x80];
  // NO cmp vs +0x1F40 (getWheel-only gate). Host clamps storage only —
  // no invent past table[8].
  if (!self) return vec3_new(0, 0, 0);
  float origin[3];
  (void)chassis_origin_parent_walk(self, origin);
  float lx = 0.f, ly = 0.f, lz = 0.f;
  if (n >= 0 && n < 8) {
    const auto& w = chassis_phys_wheel_slot(self, n);
    lx = w.px;
    ly = w.py;
    lz = w.pz;
  }
  return vec3_new(origin[0] + lx, origin[1] + ly, origin[2] + lz);
}

// PE @ 0x0043E320 size 0xb6 (IDA Chassis_forceUpdate).
// Unbox this. Native.ptr==0 / node0 0 / dual walk fail → silent ret.
// NO Mighty. Hop0: *(handle+0xC), vtbl+0x14(0), sub_5447D0(0xA0000000),
// vtbl+0xC(1.0f). Hop1: *(mid+0x44), phys=*(mid+0x4C), sub_5447D0
// (0x20000000), vtbl+0xC → Chassis_forceUpdate_apply @ 0x00448430
// size 0x8d5 (this=*[hop1+0xC], a2=phys): clear +0x70 bit 0x800000,
// if [phys+0x1FBC]==[this+0x2E48] → Chassis_forceUpdate_cloneHdr
// @ 0x0043E3E0 size 0xa85 (malloc 3100=0xC1C) into +0x1FBC, gate
// suspend_update==0, wheel scratch (*[phys+0x13E4] stride 0x2B4) zeros
// +0x15C..+0x170 ONLY then Engine_memcpy pacejka 68B (IDA @ 0x005D6780)
// from this+0x2E50, CallScriptMethod "updatevariables", child walk via
// phys list *[a2+0xDC] → Chassis_forceUpdate_ingestPart @ 0x0043C520
// size 0x2a7 (types 5/6/7 → wheel +0x15C/+0x164/+0x16C pair writes;
// 8 → phys+0x1E38/1E3C; 9/10/12 → floats + ResHandle Link embeds
// +0x203C/+0x202C/+0x204C — W8C Link when *[part+0x0C] / native_ptr), JVM→hdr engine/gear/SFX/nitro,
// THEN ALWAYS Chassis_forceUpdate_tail @ 0x004479D0 (even if suspend
// skipped the body — apply returns tail @ 0x448cf3).
//
// IDA map phys+0x1FBC hdr (3100B) — apply writes (esi=hdr):
//   +0x00/+0x04/+0x08/+0x0C  torque lerp counts/ptrs (from hdr+0xC18 src)
//   +0x10                  curve meta; +0x14 maxRPM; +0x18 RPM_limit
//   +0x1C gears; +0x20..+0x3C ratio[F] (max 8); +0x40 rearend_ratio
//   +0xC8 ClutchF; +0xCC starter_torque; +0xD0/+0xD4 = 1.0f (peaks)
//   +0xE0 engine_inertia; +0xEC rpm_idle*π/30; +0xF0/+0xF4 friction_fwd/rev
//   +0xF8 / +0x340 / +0x588  SfxTable slots (stride 0x248 — getSfxTable)
//   +0x7D0 exhaust min vol; +0x7D4/+0x7E4/+0x7F4 ResHandle_Bind type6
//   +0x804/+0x808/+0x80C rpm_trans_fwd/rev, sfx_starter_rpm
//   +0x814 nitro SFX; +0x834.. horn SFX; +0xA14/+0xA18/+0xA1C steer
//   +0xA38 tank_nitro; +0xA44 consumption_nitro; +0xA48.. cooling
//   +0x8FC..+0x908 drag_center xyz + C_drag; +0xC18 engine-curve src ptr
// phys companion: +0x1DD0/+0x1DD4=k*hdr peaks; +0x1DCC=0; +0x1E38 wear;
//   +0x1E58 mass*8000; +0x1EF8 min(tank_nitro, prev).
//
// Aero consumer (IDA Chassis_physWheelTick @ 0x454500 size 0x334b):
//   Sole hdr+0x908 READ for drag force @ 0x456a83 / scale @ 0x456b05.
//   Physics_Step @ 0x4A5190 has ZERO +0x908 hits. Soft apply below
//   (chassis_phys_aero_soft_apply); forceUpdate only STOREs coeffs.
//   Formula @ 0x456a6b..0x456d2d (flt_zero@0x5E73CC=0, flt_half@0x5F09D0=0.5,
//   flt_quarter@0x5F0C80=0.25, float_1_0@0x5F08F0=1):
//     [phys+0x20F4]=0 @ 0x456a57; skip if (phys+0x70)&1 @ 0x456a61;
//     root = *[phys+0x13BC]; walk parent while (+0x58)&4;
//     skip if C_drag<=0 (hdr+0x908 vs flt_zero);
//     v = Phys_velAtLocalPoint(root, &hdr+0x8FC) @ 0x4A6FF0;
//     skip if |v|^2 <= 1.0; v_hat = v/|v|;
//     F = -C_drag * |v|^2 * v_hat;   // ≡ -C_drag * |v| * v
//     r = Phys_rotateLocalVec(root, drag_center) @ 0x4A7190 (torque arm);
//     clamp: J=F*dt; v_scaled=v*(1/(Iterm+inv_m)); Jb=J+0.5*v_scaled;
//            |Jb|^2 vs 0.25*|v_scaled|^2 then unblend → F;
//     |F| → phys+0x20F4 @ 0x456d27; Phys_accumForceAtLocalPoint @ 0x4A6520
//       → force accum + τ += (R*r)×F.
//   Other +0x908 hits: cloneHdr copy @ 0x43ebbf; console parse write
//   sub_444310 (C_drag*1.116); SFX thresh @ 0x4671f7 is OTHER object.
//
// Soft PE VA contract (TREE side-band — Resources integrate owns Δv):
//   aero_force_fx/fy/fz     ← F (Phys_accumForceAtLocalPoint a2)
//   aero_force_px/py/pz     ← drag_center local (a3 / hdr+0x8FC..+0x904)
//   aero_phys_20f4          ← |F| (phys+0x20F4)
//   aero_impulse_fx/fy/fz   ← soft J after PE half/quarter clamp
//   aero_v_px/py/pz         ← soft Phys_velAtLocalPoint result
//   aero_hat_x/y/z          ← v_hat
//   aero_r_wx/wy/wz         ← soft Phys_rotateLocalVec (Ypr R * drag_center)
//   aero_torque_tx/ty/tz    ← r_world × F (accumForce torque arm preview)
//   aero_speed / aero_dt    ← |v| / dt
//   aero_clamped            ← 1 when half/quarter clamp fired
//   aero_applied            ← 1 when F written this call
//   aero_root_hops          ← soft scene_flags&4 parent hops (PE +0x58&4)
// Host soft: Ypr R + ω×r_world; NO physics_set_velocity (Resources Cd).
// OOS: phys parent chain +0x20, inertia tensor I-term @ body+0x108,
//      Phys_accumForceAtLocalPoint body force/torque write.
// Host: real malloc(3100) + Sfx headers + null ResHandles + scalars;
//   W10B: prefer live phys+0xDC (gameref_phys_dc_head) else part_slots
//   rebuild + PE walk/ingest; DynoData→lerp when tables live (+0xC18 stays
//   0 — cloneHdr clear; side-band = DynoSim*).
// W8C: t9/10/12 ResHandle_Link when native_ptr_node(child) live (PE
//   *[part+0x0C]); else LABEL_25 zero. Post-Link key←*(inst+0x50).
// W15A: Sfx sample Rebind from g_sfxtables → hdr slots (cloneHdr loops
//   @ 0x43E6B5/70E/791); part+0x0C still needs Native.ptr for RH Link.
// W10B: camSlotFillFromHdr PE slice (hdr+0xA54 / +0xA58); camera create
//   ResourceEngine OOS. forceUpdate does not write +0x17A8 map.
// W16B: soft PrepareLod hop gate @ 0x4485DC (match_key/native_ptr);
//   Chassis_findPartNodeBySlotId stand-in + wheel RH splice @ 0x448b65
//   (embeds @ wheel+0x1D8).
// W17B: phys+0x78 side-map = Part_buildPhysSlotTable @ 0x46EAE0
//   (store @ 0x46f2e9); +0x115C = SimObjectList(+0x1154).head — host
//   part_slots order. Real RPK list ctor / allocCamBlob still OOS.
// W18D: Part_allocPhysBlob @ 0x46E930 malloc(0x11C)+ctor soft-ensure
//   (smaller than Chassis_allocCamBlob @ 0x44A250); then phys78.
// W19D: Chassis_camBlob_baseCtor @ 0x45E910 (via cam_tables_ensure_inner).
// W23D: Chassis_camEntryCtor @ 0x45E7D0 ×4 @ 0x44a295 (blob+0x11C×0x4A8).
// W34-07: allocCamBlob mid @ 0x44a2bb..0x44a2ff (+0x13BC..+0x14EC aux).
// W35-07: CircList×2 @ 0x44a304/+0x1DDC + 0x44a359/+0x1FA0 soft;
// W36-07: RH pads + listCtor×4 +0x20D8 + soft buildPhysSlotTable;
// W37-07: Rebind+0x13C4 / phys defaults @ 0x44a472 / wheelEntry_ctor×N soft
//   (forceUpdate/ingestPart TREE seeds); Veh_ensureSceneBound + Phys_*/
//   car_root / LOD still OOS — no invent body 0x16c2.

// Soft PE Chassis_physWheelTick aero slice @ 0x456a6b..0x456d2d.
// Called from setMileage (GameRef vehicle tick hits mileage each frame).
// Side-band preview only — Resources::physics_apply_chassis_aero owns Δv.
void chassis_phys_aero_soft_apply(InvObject* chassis, float dt) {
  if (!chassis || dt <= 0.f) return;
  // PE @ 0x456a57: [phys+0x20F4]=0 then gate (phys+0x70)&1 @ 0x456a61.
  auto clear_aero_band = [&]() {
    tree_field_set_float(chassis, "aero_phys_20f4", 0.f);
    tree_field_set_int(chassis, "aero_applied", 0);
    tree_field_set_int(chassis, "aero_clamped", 0);
    tree_field_set_int(chassis, "aero_root_hops", 0);
    tree_field_set_float(chassis, "aero_speed", 0.f);
    tree_field_set_float(chassis, "aero_dt", dt);
  };
  clear_aero_band();
  const int32_t flags70 =
      tree_field_get_int(chassis, "force_update_flags70");
  if ((flags70 & 1) != 0) return;

  // C_drag / drag_center: prefer forceUpdate side-band (hdr mirror), else
  // TREE/hdr. force_update_drag_set=1 means apply wrote hdr+0x8FC..+0x908.
  float cd = tree_field_get_float(chassis, "force_update_drag_c");
  float dx = tree_field_get_float(chassis, "force_update_drag_x");
  float dy = tree_field_get_float(chassis, "force_update_drag_y");
  float dz = tree_field_get_float(chassis, "force_update_drag_z");
  const bool drag_seeded =
      tree_field_get_int(chassis, "force_update_drag_set") != 0;
  if (!drag_seeded && cd == 0.f && dx == 0.f && dy == 0.f && dz == 0.f) {
    cd = tree_field_get_float(chassis, "C_drag");
    if (InvObject* dc = tree_field_get_obj(chassis, "drag_center"))
      vec3_get(dc, &dx, &dy, &dz);
    if (uint8_t* hdr = chassis_hdr_ptr(chassis)) {
      if (cd == 0.f) cd = hdr_get_f32(hdr, 0x908);
      if (dx == 0.f && dy == 0.f && dz == 0.f) {
        dx = hdr_get_f32(hdr, 0x8FC);
        dy = hdr_get_f32(hdr, 0x900);
        dz = hdr_get_f32(hdr, 0x904);
      }
    }
  }
  // PE @ 0x456a83: fcomp flt_zero (0x5E73CC=0) — skip if C_drag<=0.
  if (!(cd > 0.f)) return;

  // Phys body key: Vehicle.set copies Native.ptr onto chassis (GameRef).
  InvObject* body = chassis;
  if (physics_shape(body) == 0) {
    if (InvObject* car = tree_field_get_obj(chassis, "the_car")) {
      if (physics_shape(car) != 0) body = car;
    }
  }
  if (physics_shape(body) == 0) return;

  // Soft root walk stand-in for PE @ 0x456a6b..0x456a80:
  //   root=*[phys+0x13BC]; while (root+0x58)&4: root=*[root+0x20].
  // Host: TREE scene_flags&4 via part_parent (same bit as getMin walk);
  // prefer parent PhysicsRef when shaped. Real phys +0x20 chain OOS.
  int32_t root_hops = 0;
  InvObject* root = body;
  {
    InvObject* walk = body;
    for (int depth = 0; depth < 8; ++depth) {
      if ((tree_field_get_int(walk, "scene_flags") & 4) == 0) break;
      InvObject* parent = tree_field_get_obj(walk, "part_parent");
      if (!parent) parent = tree_field_get_obj(walk, "phys_root");
      if (!parent || parent == walk) break;
      ++root_hops;
      walk = parent;
      if (physics_shape(walk) != 0) root = walk;
    }
  }
  tree_field_set_int(chassis, "aero_root_hops", root_hops);

  // Soft Phys_rotateLocalVec @ 0x4A7190: Ypr_toMatrix rows × drag_center
  // (same composition as chassis_origin_parent_walk / WheelRef @ 0x54ECD0).
  float rx = dx, ry = dy, rz = dz;
  float oy = 0.f, op = 0.f, or_ = 0.f;
  if (InvObject* ori = java_util_resource_PhysicsRef_getOri(root))
    ypr_get(ori, &oy, &op, &or_);
  if (oy != 0.f || op != 0.f || or_ != 0.f) {
    const float sy = std::sin(oy), cy = std::cos(oy);
    const float sp = std::sin(op), cp = std::cos(op);
    const float sr = std::sin(or_), cr = std::cos(or_);
    const float r00 = sr * sp * sy + cr * cy;
    const float r01 = cr * sp * sy - sr * cy;
    const float r02 = cp * sy;
    const float r10 = sr * cp;
    const float r11 = cr * cp;
    const float r12 = -sp;
    const float r20 = sr * sp * cy - cr * sy;
    const float r21 = cr * sp * cy + sr * sy;
    const float r22 = cp * cy;
    rx = dx * r00 + dy * r01 + dz * r02;
    ry = dx * r10 + dy * r11 + dz * r12;
    rz = dx * r20 + dy * r21 + dz * r22;
  }
  tree_field_set_float(chassis, "aero_r_wx", rx);
  tree_field_set_float(chassis, "aero_r_wy", ry);
  tree_field_set_float(chassis, "aero_r_wz", rz);

  // Soft Phys_velAtLocalPoint leaf @ 0x4A6FF0: v_lin + ω×r_world
  // (parent recursion when (+0x58)&4 still OOS beyond hop census above).
  float vx = 0.f, vy = 0.f, vz = 0.f;
  if (InvObject* vel = java_util_resource_PhysicsRef_getVel(root))
    vec3_get(vel, &vx, &vy, &vz);
  float wx = 0.f, wy = 0.f, wz = 0.f;
  if (InvObject* av = java_util_resource_PhysicsRef_getAngVel(root))
    vec3_get(av, &wx, &wy, &wz);
  vx += wy * rz - wz * ry;
  vy += wz * rx - wx * rz;
  vz += wx * ry - wy * rx;
  tree_field_set_float(chassis, "aero_v_px", vx);
  tree_field_set_float(chassis, "aero_v_py", vy);
  tree_field_set_float(chassis, "aero_v_pz", vz);

  const float v2 = vx * vx + vy * vy + vz * vz;
  // PE @ 0x456ac8: skip if |v|^2 <= float_1_0 (1.0 @ 0x5F08F0).
  if (v2 <= 1.f) return;
  const float speed = std::sqrt(v2);
  tree_field_set_float(chassis, "aero_speed", speed);
  const float inv_speed = 1.f / speed;
  const float hx = vx * inv_speed;
  const float hy = vy * inv_speed;
  const float hz = vz * inv_speed;
  tree_field_set_float(chassis, "aero_hat_x", hx);
  tree_field_set_float(chassis, "aero_hat_y", hy);
  tree_field_set_float(chassis, "aero_hat_z", hz);
  // PE @ 0x456b05: F = -C_drag * |v|^2 * v_hat.
  float fx = -cd * v2 * hx;
  float fy = -cd * v2 * hy;
  float fz = -cd * v2 * hz;

  // Soft PE impulse clamp @ 0x456b2a..0x456d00 (flt_half@0x5F09D0=0.5,
  // flt_quarter@0x5F0C80=0.25). Angular I-term (body+0x108) OOS → 0;
  // PE inv_eff = 1 / (Iterm + *(mass+0x14)); soft *(+0x14)=1/m → factor=m.
  //   v_scaled = v * m;  J = F*dt;  Jb = J + 0.5*v_scaled;
  //   if |Jb|^2 > 0.25*|v_scaled|^2: scale Jb, J = Jb - 0.5*v_scaled, F=J/dt.
  constexpr float kHalf = 0.5f;     // flt_5F09D0
  constexpr float kQuarter = 0.25f;  // flt_5F0C80
  const float mass = java_game_parts_bodypart_Chassis_getMass(chassis);
  float jx = fx * dt, jy = fy * dt, jz = fz * dt;
  int32_t clamped = 0;
  if (mass > 1e-3f) {
    const float vsx = vx * mass, vsy = vy * mass, vsz = vz * mass;
    const float half_vsx = kHalf * vsx, half_vsy = kHalf * vsy,
                half_vsz = kHalf * vsz;
    float jbx = jx + half_vsx, jby = jy + half_vsy, jbz = jz + half_vsz;
    const float jb2 = jbx * jbx + jby * jby + jbz * jbz;
    const float vs2 = vsx * vsx + vsy * vsy + vsz * vsz;
    const float max2 = vs2 * kQuarter;
    if (jb2 > max2 && jb2 > 0.f && max2 > 0.f) {
      const float s = std::sqrt(max2 / jb2);
      jbx *= s;
      jby *= s;
      jbz *= s;
      jx = jbx - half_vsx;
      jy = jby - half_vsy;
      jz = jbz - half_vsz;
      const float inv_dt = 1.f / dt;
      fx = jx * inv_dt;
      fy = jy * inv_dt;
      fz = jz * inv_dt;
      clamped = 1;
    }
  }
  // Soft τ = r_world × F (Phys_accumForceAtLocalPoint torque arm @ 0x4A6520).
  const float tx = ry * fz - rz * fy;
  const float ty = rz * fx - rx * fz;
  const float tz = rx * fy - ry * fx;
  const float fmag = std::sqrt(fx * fx + fy * fy + fz * fz);
  // PE @ 0x456d27: |F| → phys+0x20F4; then Phys_accumForceAtLocalPoint.
  tree_field_set_float(chassis, "aero_phys_20f4", fmag);
  tree_field_set_float(chassis, "aero_force_fx", fx);
  tree_field_set_float(chassis, "aero_force_fy", fy);
  tree_field_set_float(chassis, "aero_force_fz", fz);
  tree_field_set_float(chassis, "aero_force_px", dx);
  tree_field_set_float(chassis, "aero_force_py", dy);
  tree_field_set_float(chassis, "aero_force_pz", dz);
  tree_field_set_float(chassis, "aero_impulse_fx", jx);
  tree_field_set_float(chassis, "aero_impulse_fy", jy);
  tree_field_set_float(chassis, "aero_impulse_fz", jz);
  tree_field_set_float(chassis, "aero_torque_tx", tx);
  tree_field_set_float(chassis, "aero_torque_ty", ty);
  tree_field_set_float(chassis, "aero_torque_tz", tz);
  tree_field_set_int(chassis, "aero_clamped", clamped);
  tree_field_set_int(chassis, "aero_applied", 1);
  // Δv: Resources::physics_apply_chassis_aero only — no physics_set_velocity.
}

void java_game_parts_bodypart_Chassis_forceUpdate(InvObject* self) {
  if (!self) return;
  // W19D..W37-07: PE camBlob_baseCtor @ 0x45E910 → camEntryCtor×4 @
  // 0x44a295 → mid arrayCtor +0x13E8/+0x14EC @ 0x44a2bb..0x44a2ff →
  // CircList×2 @ 0x44a304/+0x1DDC + 0x44a359/+0x1FA0 → RH pads +
  // listCtor×4 +0x20D8 @ 0x44a30f..0x44a3f4 → soft phys78
  // (Part_buildPhysSlotTable) → soft Rebind+0x13C4 / phys defaults @
  // 0x44a472 / wheelEntry_ctor×N → shared 0x11C + W18D; then W9C cam
  // tables. Veh_ensureSceneBound / Phys_* / LOD still OOS.
  chassis_cam_tables_ensure_inner(self);
  // PE apply prologue: phys[+0x70] &= ~0x800000.
  const int32_t flags70 = tree_field_get_int(self, "force_update_flags70");
  tree_field_set_int(self, "force_update_flags70", flags70 & ~0x800000);
  // +0x1FBC cache: when hdr still aliases shared template (this+0x2E48),
  // Chassis_forceUpdate_cloneHdr malloc(3100) → private blob.
  int32_t hdr_tok = tree_field_get_int(self, "force_update_hdr");
  const int32_t tmpl = tree_field_get_int(self, "force_update_hdr_tmpl");
  const int32_t shared = tmpl != 0 ? tmpl : kSharedHdrToken;
  if (hdr_tok == 0) hdr_tok = shared;
  uint8_t* hdr = chassis_hdr_ptr(self);
  if (hdr_tok == shared || !hdr) {
    const int32_t gen = tree_field_get_int(self, "force_update_hdr_gen") + 1;
    tree_field_set_int(self, "force_update_hdr_gen", gen);
    hdr = chassis_force_update_clone_hdr(self);
  }
  tree_field_set_int(self, "force_update_hdr_tmpl", shared);
  // After private hdr exists: PE allocCamBlob fill path @ 0x44a788.
  chassis_cam_slot_fill_from_hdr_inner(self);
  // PE apply: body only when suspend_update==0 (Chassis.load sets 1 then 0
  // before forceUpdate). Host clears flag after gate check.
  const int32_t suspend = tree_field_get_int(self, "suspend_update");
  tree_field_set_int(self, "suspend_update", 0);
  const int32_t n = tree_field_get_int(self, "force_update_count");
  tree_field_set_int(self, "force_update_count", n + 1);
  const int32_t nw = java_game_parts_bodypart_Chassis_getWheels(self);
  if (suspend == 0) {
    // Apply scratch reset @ 0x4484e3: stride 0x2B4 × count this+0x1F40.
    for (int32_t i = 0; i < nw && i < 8; ++i)
      chassis_phys_wheel_scratch_reset(self, i);
    // PE CallNamedMethod "updatevariables" @ 0x44855d BEFORE child walk.
    // Chassis.java updatevariables: C_drag = fully_stripped_drag; then later
    // (same method) C_drag -= BodyPart.drag_reduction per occupied slot;
    // clamp C_drag >= 0. Native only needs coeffs on hdr — script does the
    // rest when TREE runs; host soft-mirrors so forceUpdate still seeds
    // hdr+0x908 when Java path is partial.
    {
      // Java always assigns (field default fully_stripped_drag=0.333).
      float cd = tree_field_get_float(self, "fully_stripped_drag");
      tree_field_set_float(self, "C_drag", cd);
      tree_field_set_float(self, "diff_lock", 0.f);
      for (int32_t i = 0; i < nw && i < 8; ++i) {
        auto& w = chassis_phys_wheel_slot(self, i);
        w.drive = 0.f;
        // InstantCenter sentinel 10000 (Chassis.updatevariables).
        for (int k = 0; k < 6; ++k) w.ic[k] = 10000.f;
      }
      // BodyPart.drag_reduction subtract — Chassis.java slot walk after
      // wheel/has[] logic. Host: every occupied part_slots child; missing
      // drag_reduction → 0 (non-BodyPart / unset).
      const int32_t nslots = part_slot_count(self);
      for (int32_t si = 0; si < nslots; ++si) {
        const int32_t sid = part_slot_id_at(self, si);
        if (sid <= 0) continue;
        InvObject* child = part_on_slot(self, sid);
        if (!child) continue;
        cd -= tree_field_get_float(child, "drag_reduction");
      }
      if (cd < 0.f) cd = 0.f;
      tree_field_set_float(self, "C_drag", cd);
    }
    // PE child walk @ phys+0xDC (0x4485c9): prefer live GameRef list,
    // else part_slots rebuild; Unlink type10 RH (+0x202C) @ 0x448562,
    // walk next=+4 / child=+0x18 → ingestPart @ 0x43C520.
    chassis_force_update_walk_children(self, nw);
    // JVM→hdr engine scalars @ 0x448647.. (Chassis.java field names).
    const float eng_inertia = tree_field_get_float(self, "engine_inertia");
    const float starter_tq = tree_field_get_float(self, "starter_torque");
    const float fr_fwd = tree_field_get_float(self, "engine_friction_fwd");
    const float fr_rev = tree_field_get_float(self, "engine_friction_rev");
    tree_field_set_float(self, "force_update_hdr_e0", eng_inertia);  // +0xE0
    tree_field_set_float(self, "force_update_hdr_cc", starter_tq);    // +0xCC
    tree_field_set_float(self, "force_update_hdr_f0", fr_fwd);        // +0xF0
    tree_field_set_float(self, "force_update_hdr_f4", fr_rev);        // +0xF4
    if (hdr) {
      hdr_put_f32(hdr, 0xE0, eng_inertia);
      hdr_put_f32(hdr, 0xCC, starter_tq);
      hdr_put_f32(hdr, 0xF0, fr_fwd);
      hdr_put_f32(hdr, 0xF4, fr_rev);
    }
    // Gear / ratio / RPM / clutch — PE @ 0x44894d.. into hdr(+0x1FBC).
    {
      int32_t nrat = 0;
      if (InvObject* ratio = tree_field_get_obj(self, "ratio"))
        nrat = tree_vector_size(ratio);
      if (nrat <= 0) nrat = 8;  // Chassis.java float[8]
      if (nrat > 8) nrat = 8;
      tree_field_set_int(self, "force_update_ratio_n", nrat);
      for (int32_t i = 0; i < nrat; ++i) {
        char src[32], dst[32];
        std::snprintf(src, sizeof(src), "ratio_%d", static_cast<int>(i));
        std::snprintf(dst, sizeof(dst), "force_update_ratio_%d",
                      static_cast<int>(i));
        const float r = tree_field_get_float(self, src);
        tree_field_set_float(self, dst, r);
        if (hdr) hdr_put_f32(hdr, 0x20 + static_cast<size_t>(i) * 4u, r);
      }
    }
    const int32_t gears = tree_field_get_int(self, "gears");
    const float rearend = tree_field_get_float(self, "rearend_ratio");
    const float maxRPM = tree_field_get_float(self, "maxRPM");
    const float rpm_lim = tree_field_get_float(self, "RPM_limit");
    const float clutch = tree_field_get_float(self, "ClutchF");
    tree_field_set_int(self, "force_update_gears", gears);            // +0x1C
    tree_field_set_float(self, "force_update_rearend", rearend);      // +0x40
    tree_field_set_float(self, "force_update_maxRPM", maxRPM);        // +0x14
    tree_field_set_float(self, "force_update_RPM_limit", rpm_lim);    // +0x18
    tree_field_set_float(self, "force_update_ClutchF", clutch);       // +0xC8
    if (hdr) {
      hdr_put_u32(hdr, 0x1C, static_cast<uint32_t>(gears));
      hdr_put_f32(hdr, 0x40, rearend);
      hdr_put_f32(hdr, 0x14, maxRPM);
      hdr_put_f32(hdr, 0x18, rpm_lim);
      hdr_put_f32(hdr, 0xC8, clutch);
    }
    // rpm_trans / starter @ hdr+0x804/+0x808/+0x80C (ASM 0x448a5f).
    const float rpm_tf = tree_field_get_float(self, "rpm_trans_fwd");
    const float rpm_tr = tree_field_get_float(self, "rpm_trans_rev");
    const float sfx_st = tree_field_get_float(self, "sfx_starter_rpm");
    tree_field_set_float(self, "force_update_hdr_804", rpm_tf);
    tree_field_set_float(self, "force_update_hdr_808", rpm_tr);
    tree_field_set_float(self, "force_update_hdr_80c", sfx_st);
    if (hdr) {
      hdr_put_f32(hdr, 0x804, rpm_tf);
      hdr_put_f32(hdr, 0x808, rpm_tr);
      hdr_put_f32(hdr, 0x80C, sfx_st);
    }
    // PE ResHandle_Bind(rid, 6, 0) ×3 @ hdr+0x7D4/+0x7E4/+0x7F4 (0x448aa9).
    // Null-safe: rid==0 → zero 16B; else id @ +8, no invent samples.
    auto bind_sfx = [&](const char* field, const char* bound_key,
                        const char* type_key, size_t hdr_off) {
      const int32_t id = tree_field_get_int(self, field);
      const int32_t prev = tree_field_get_int(self, bound_key);
      tree_field_set_int(self, field, id);
      if (id != 0) {
        tree_field_set_int(self, bound_key, id);
        tree_field_set_int(self, type_key, 6);  // PE a3=6
      } else if (prev != 0) {
        tree_field_set_int(self, bound_key, 0);
        tree_field_set_int(self, type_key, 0);
      }
      if (hdr) hdr_res_bind_nullsafe(hdr, hdr_off, id);
    };
    bind_sfx("SFX_trans_fwd", "force_update_sfx_fwd", "force_update_sfx_fwd_t",
             0x7D4);
    bind_sfx("SFX_trans_rev", "force_update_sfx_rev", "force_update_sfx_rev_t",
             0x7E4);
    bind_sfx("SFX_ignition", "force_update_sfx_ign", "force_update_sfx_ign_t",
             0x7F4);
    // W15A: re-sync SfxTable samples into private hdr (addItem may have
    // run after prior cloneHdr). PE clone copies tmpl→private once;
    // host logical tmpl = g_sfxtables @ 0x43E6B5/70E/791.
    if (hdr) chassis_hdr_sfx_sample_rebind(hdr, self);
    // phys+0x1DCC = 0 (horn slot index clear) @ 0x448ad8.
    tree_field_set_int(self, "force_update_horn_idx", 0);
    // tank_nitro → hdr+0xA38; consumption_nitro → +0xA44; min → phys+0x1EF8.
    {
      const float tank = tree_field_get_float(self, "tank_nitro");
      const float cons = tree_field_get_float(self, "consumption_nitro");
      tree_field_set_float(self, "force_update_hdr_a38", tank);
      tree_field_set_float(self, "force_update_hdr_a44", cons);
      if (hdr) {
        hdr_put_f32(hdr, 0xA38, tank);
        hdr_put_f32(hdr, 0xA44, cons);
      }
      float cap = tree_field_get_float(self, "force_update_nitro_cap");  // +0x1EF8
      if (cap == 0.f || tank <= cap) cap = tank;
      tree_field_set_float(self, "force_update_nitro_cap", cap);
    }
    // Torque curve: PE rebuilds lerp from *[hdr+0xC18] DynoSim (0x448773).
    // cloneHdr clears +0xC18; host side-band = engine.dynodata g_dyno.nm.
    InvObject* dyno_src = nullptr;
    if (InvObject* eng = tree_field_get_obj(self, "engine")) {
      const float t0 = tree_field_get_float(eng, "torque");
      const float t1 = tree_field_get_float(eng, "torque2");
      if (t0 != 0.f || t1 != 0.f) {
        tree_field_set_float(self, "engine_torque", t0);
        tree_field_set_float(self, "engine_torque2", t1 != 0.f ? t1 : t0);
      }
      dyno_src = tree_field_get_obj(eng, "dynodata");
    }
    if (!dyno_src) dyno_src = tree_field_get_obj(self, "dynodata");
    if (hdr) {
      auto it = g_chassis_hdr.find(self);
      if (it != g_chassis_hdr.end())
        chassis_hdr_rebuild_lerp_from_dyno(it->second, dyno_src);
      hdr = chassis_hdr_ptr(self);
    }
    // Drag center xyz + C_drag → hdr+0x8FC..+0x908 (ASM @ 0x448c9d..0x448ce0).
    // Side-band force_update_drag_* mirrors blob for soft aero apply
    // (chassis_phys_aero_soft_apply ← Chassis_physWheelTick @ 0x456a83).
    // force_update_drag_set=1 locks soft apply to this hdr mirror (no TREE
    // fallback once forceUpdate wrote coeffs — matches PE hdr-only read).
    {
      float dx = 0, dy = 0, dz = 0;
      if (InvObject* dc = tree_field_get_obj(self, "drag_center"))
        vec3_get(dc, &dx, &dy, &dz);
      const float cd = tree_field_get_float(self, "C_drag");
      tree_field_set_float(self, "force_update_drag_x", dx);  // hdr+0x8FC
      tree_field_set_float(self, "force_update_drag_y", dy);  // hdr+0x900
      tree_field_set_float(self, "force_update_drag_z", dz);  // hdr+0x904
      tree_field_set_float(self, "force_update_drag_c", cd);  // hdr+0x908
      tree_field_set_int(self, "force_update_drag_set", 1);
      // Refresh hdr ptr after lerp rebuild above may have re-seated blob.
      hdr = chassis_hdr_ptr(self);
      if (hdr) {
        hdr_put_f32(hdr, 0x8FC, dx);
        hdr_put_f32(hdr, 0x900, dy);
        hdr_put_f32(hdr, 0x904, dz);
        hdr_put_f32(hdr, 0x908, cd);
      }
    }
    // PE apply @ 0x448660: hdr[+0xD0/+0xD4]=1.0f, THEN wear → k → +0x1DD0.
    {
      float wear = tree_field_get_float(self, "wear");
      if (wear == 0.f) wear = tree_field_get_float(self, "chassis_wear");
      const float k =
          (wear >= 1.f) ? 0.1f : (1.f - wear * wear * 0.6f);
      tree_field_set_float(self, "force_update_hdr_d0", 1.f);
      tree_field_set_float(self, "force_update_hdr_d4", 1.f);
      tree_field_set_float(self, "force_update_wear_k", k);
      tree_field_set_float(self, "force_update_scale0", k);  // +0x1DD0
      tree_field_set_float(self, "force_update_scale1", k);  // +0x1DD4
      if (hdr) {
        hdr_put_f32(hdr, 0xD0, 1.f);
        hdr_put_f32(hdr, 0xD4, 1.f);
      }
    }
    // PE apply @ 0x4486cb: engine_mass*8000 → phys+0x1E58; rpm_idle*π/30 → +0xEC.
    {
      float eng_m = tree_field_get_float(self, "engine_mass");
      if (eng_m <= 0.f) {
        if (InvObject* eng = tree_field_get_obj(self, "engine"))
          eng_m = tree_field_get_float(eng, "mass");
      }
      if (eng_m <= 0.f) eng_m = tree_field_get_float(self, "mass");
      if (eng_m <= 0.f) eng_m = tree_field_get_float(self, "chassis_mass");
      if (eng_m <= 0.f) eng_m = 1200.f;
      constexpr float kMassScale = 8000.f;  // flt_5F0FBC @ 0x005F0FBC
      const float m_phys = eng_m * kMassScale;
      float inv = tree_field_get_float(self, "inv_mass");
      float m_prev = tree_field_get_float(self, "force_update_mass");
      if (m_prev <= 0.f) m_prev = m_phys;
      if (inv <= 0.f) inv = 1.f / m_prev;
      const float prod = m_prev * inv;
      tree_field_set_float(self, "force_update_mass", m_phys);
      tree_field_set_float(self, "force_update_inv_mass",
                           (m_phys > 0.f) ? (prod / m_phys) : inv);
      tree_field_set_float(self, "inv_mass",
                           (m_phys > 0.f) ? (prod / m_phys) : inv);
      float rpm_idle = tree_field_get_float(self, "engine_rpm_idle");
      if (rpm_idle == 0.f) {
        if (InvObject* eng = tree_field_get_obj(self, "engine"))
          rpm_idle = tree_field_get_float(eng, "rpm_idle");
      }
      constexpr float kRpmToRad = 0.10471976f;  // flt_5F0EF0 ≈ π/30
      const float omega = rpm_idle * kRpmToRad;
      tree_field_set_float(self, "force_update_rpm_omega", omega);
      if (hdr) hdr_put_f32(hdr, 0xEC, omega);
    }
    // Sync cooling / steer / exhaust min into hdr when TREE already set
    // (setCooling / setSteer* / setSfxExhaustMinVol write these offsets).
    if (hdr) {
      const float cmin = tree_field_get_float(self, "cooling_min");
      const float cmax = tree_field_get_float(self, "cooling_max");
      const float cspd = tree_field_get_float(self, "cooling_spd");
      if (cmin != 0.f || cmax != 0.f || cspd != 0.f) {
        hdr_put_f32(hdr, 0xA48, cmin);
        hdr_put_f32(hdr, 0xA4C, cmax);
        hdr_put_f32(hdr, 0xA50, cspd);
      }
      const float swr = tree_field_get_float(self, "steer_wheel_r");
      const float swz = tree_field_get_float(self, "steer_wheel_z");
      const float ack = tree_field_get_float(self, "ackermann");
      if (swr != 0.f) hdr_put_f32(hdr, 0xA14, swr);
      if (swz != 0.f) hdr_put_f32(hdr, 0xA18, swz);
      // setAckermann may land before cloneHdr — flush pending / non-zero.
      if (ack != 0.f ||
          tree_field_get_int(self, "ackermann_hdr_pending") != 0) {
        hdr_put_f32(hdr, 0xA1C, ack);
        tree_field_set_int(self, "ackermann_hdr_pending", 0);
      }
      const float exh = tree_field_get_float(self, "sfx_exhaust_min_vol");
      if (exh != 0.f) hdr_put_f32(hdr, 0x7D0, exh);
    }
    // Sync +0x1EAC CM stand-in for getMin/getWheelPos (phys_cm_*).
    {
      float cx = 0.f, cy = 0.f, cz = 0.f;
      if (tree_field_get_float(self, "cm_set") > 0.5f) {
        cx = tree_field_get_float(self, "cm_x");
        cy = tree_field_get_float(self, "cm_y");
        cz = tree_field_get_float(self, "cm_z");
      } else {
        float mn[3], mx[3], cm[3];
        chassis_bounds(self, mn, mx, cm);
        cx = cm[0];
        cy = cm[1];
        cz = cm[2];
      }
      tree_field_set_float(self, "phys_cm_x", cx);
      tree_field_set_float(self, "phys_cm_y", cy);
      tree_field_set_float(self, "phys_cm_z", cz);
      tree_field_set_int(self, "phys_cm_set", 1);
    }
    // W16B/W17B: PE Chassis_findPartNodeBySlotId @ 0x4707D0 + wheel RH
    // splice @ 0x448b65; phys78 table rebuild @ Part_buildPhysSlotTable
    // 0x46EAE0 / store 0x46f2e9 before walk.
    chassis_force_update_wheel_rh_splice(self, nw);
  }  // end suspend==0 body
  // PE apply @ 0x448cf3: ALWAYS Chassis_forceUpdate_tail after hop ok,
  // even when suspend_update skipped the body (race121 deepen).
  chassis_force_update_tail(self, nw);
  valocity_sync_wheel_visuals(self, 0.016f);
}

InvObject* java_game_parts_bodypart_Chassis_getWheelDamage(InvObject* self,
                                                          int32_t index) {
  // PE @ 0x0043D080 size 0x1F2 (498) (IDA Chassis_getWheelDamage).
  // Twin setWheelDamage @ 0x0043D280 size 0x37C (sscanf/apply). Java save/load:
  // while(wheels--) write(getWheelDamage(wheels)).
  // JVM_UnboxArg(this+I). dst[256]: [0]=byte_63C7B0 (0), memset rest.
  // index < 0 (test/jge @ 0x43D0CB) → null (not "").
  // Bounds ASM @ 0x43D18D: cmp index,[node+0x1F40]; jle ok else null
  // (index > count → null; allows index==count — contrast getWheel jge).
  // Slot = *[veh+0x13E4]+index*0x2B4 (LEA @ 0x43D19D; Hex-Rays drops
  // index). ([slot+0x100]&0x20)==0 → "". Else Sprintf "%.3f×10":
  // +0x78..+0x80, Ypr_fromMatrix(+0x90), +0xF4, 1, 1, +0xF8.
  // Host: phys extra bit0x20 → format via Ypr_fromMatrix(mat90); else
  // g_wheel_dmg map (setWheelDamage round-trip — PE has no secondary map).
  // race125 PARTIAL: ASM @ 0x43d0cb jl index<0 → null; @ 0x43d18d jle vs
  // +0x1F40 (allows ==count); LEA id*0x2B4 + bit0x20 @+0x100; format via
  // Ypr_fromMatrix(+0x90) + literals 1,1. No raw *[veh+0x13E4] blob.
  // Hop fail → "" (not null).
  if (index < 0) return nullptr;
  if (!self) return string_new("");
  const int32_t wheel_limit = java_game_parts_bodypart_Chassis_getWheels(self);
  if (index > wheel_limit) return nullptr;
  if (index >= 8) return nullptr;  // host table clamp (PE unbounded LEA)
  // Touch phys slot so +0x22C / base are live (same LEA as getWheel).
  (void)chassis_phys_wheel_slot(self, index);
  if (chassis_phys_wheel_dmg_active(self, index)) {
    char buf[256];
    if (chassis_phys_wheel_dmg_format(self, index, buf, sizeof(buf)))
      return string_new(buf);
  }
  if (index <= 3) {
    const auto& slot = g_wheel_dmg[self][static_cast<size_t>(index)];
    if (!slot.empty()) return string_new(slot.c_str());
  }
  return string_new("");
}

void java_game_parts_bodypart_Chassis_setWheelDamage(InvObject* self,
                                                     int32_t index,
                                                     InvObject* data) {
  // PE @ 0x0043D280 (ILjava.lang.String;)V — NOT 0x43DF50 (mid setCooling).
  // Unbox this+index+cstr; bounds jle vs +0x1F40; wheel stride 0x2B4;
  // strlen<=1 → clear bit0x20 @+0x100; else sscanf 10f + |=0x20 + store
  // +0x78/+0x90/+0xF4/+0xF8. Host: string map + phys slot twin.
  if (!self || index < 0) return;
  const int32_t wheel_limit = java_game_parts_bodypart_Chassis_getWheels(self);
  if (index > wheel_limit || index >= 8) return;
  const char* s = data ? string_cstr(data) : "";
  if (index <= 3) g_wheel_dmg[self][static_cast<size_t>(index)] = s ? s : "";
  if (!s || std::strlen(s) <= 1) {
    chassis_phys_wheel_dmg_clear(self, index);
    if (index <= 3) g_wheel_dmg[self][static_cast<size_t>(index)].clear();
    return;
  }
  float f[10] = {};
  f[7] = 1.f;
  f[8] = 1.f;
  const int n = std::sscanf(s, "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &f[0], &f[1],
                            &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8],
                            &f[9]);
  if (n >= 6) {
    chassis_phys_wheel_dmg_apply(self, index, f[0], f[1], f[2], f[3], f[4],
                                 f[5], (n >= 7) ? f[6] : 0.f,
                                 (n >= 10) ? f[9] : 0.f);
  } else {
    // Keep opaque string for get round-trip when parse fails.
    chassis_phys_wheel_dmg_clear(self, index);
  }
}

void java_game_parts_bodypart_Chassis_setCooling(InvObject* self, float min,
                                                 float max, float spd) {
  // PE @ 0x0043DEB0 size 0x131 (IDA Chassis.setCooling).
  // Unbox this + FFF. Presets var_C/var_8/var_4 = 10/50/0.01
  // (0x41200000 / 0x42480000 / 0x3C23D70A) then dest1..3. Native.ptr
  // (dword_62E008)==0 → jz loc_43DFDC ret. NO Mighty. inner=*(handle+0xC);
  // 0 → ret. [inner+0x4C]!=1 → vtbl+0x14(0). sub_5447D0(0xA0000000
  // bytes 00 00 00 a0) test 80000000h sign → ret. vtbl+0xC(1.0f=
  // 0x3F800000). ecx=*(obj+0x4C); base=*(ecx+0x1FBC). Clamp vs
  // flt_5E73CC (0.0): min<0 → flt_5E7334 (10.0) else min → [base+0xA48];
  // max<0 → flt_5F0C3C (50.0) else max → [base+0xA4C]; spd<0 → 0.0
  // else spd → [base+0xA50]. Java: spd quadratic (Sala). GAP: PE miss →
  // silent ret; host TREE + dual-write into cloneHdr blob when present.
  if (!self) return;
  tree_field_set_float(self, "cooling_min", min);
  tree_field_set_float(self, "cooling_max", max);
  tree_field_set_float(self, "cooling_spd", spd);
  if (uint8_t* hdr = chassis_hdr_ptr(self)) {
    hdr_put_f32(hdr, 0xA48, min);
    hdr_put_f32(hdr, 0xA4C, max);
    hdr_put_f32(hdr, 0xA50, spd);
  }
}

InvObject* java_game_parts_bodypart_Chassis_getSfxTable(InvObject* self,
                                                       int32_t id) {
  // PE @ 0x00442210 size 0xFB (IDA Chassis_getSfxTable).
  // Unbox this + I (var_4 preset 0). id < 0 → null. Native.ptr
  // (dword_62E008)==0 → null. NO Mighty. inner=*(handle+0xC); 0 → null.
  // [inner+0x4C]!=1 → vtbl+0x14(0). sub_5447D0(0xA0000000 bytes 00 00 00 a0)
  // test 80000000h sign → null. vtbl+0xC(1.0f=0x3F800000). ecx=*(obj+0x4C);
  // base=*(ecx+0x1FBC). Switch id: 0 → base+0xF8; 1 → base+0x340;
  // 2 → base+0x588; else null. slot==0 → null. JVM_getClass
  // "java.game.parts.SfxTable" + sub_404E20 → fresh host; vm_set_int_field
  // (Native.ptr, slot). Engine blocks: 0=engine, 1=?, 2=exhaust.
  // GAP: PE always new wrapper (same slot ptr); host TREE cache for identity
  // (smoke tab0==tab0b). No +0x1FBC audio blob.
  if (!self || id < 0 || id > 2) return nullptr;
  char key[32];
  std::snprintf(key, sizeof(key), "sfx_table_%d", id);
  InvObject* tab = tree_field_get_obj(self, key);
  if (!tab) {
    tab = tree_host_new("java.game.parts.SfxTable");
    tree_field_set_obj(self, key, tab);
    g_sfxtables[tab];  // ensure empty vector
  }
  return tab;
}

void java_game_parts_bodypart_Chassis_setSfxExhaustMinVol(InvObject* self,
                                                         float f) {
  // PE @ 0x004409D0 size 0x80 (IDA Chassis.setSfxExhaustMinVol).
  // Unbox this + F (var_4). Native.ptr (dword_62E008)==0 → jz loc_440A4D
  // ret. NO Mighty. inner=*(handle+0xC); 0 → ret. [inner+0x4C]!=1 →
  // vtbl+0x14(0). sub_5447D0(0xA0000000 bytes 00 00 00 a0) test
  // 80000000h sign → ret. vtbl+0xC(1.0f=0x3F800000 bytes 00 00 80 3F).
  // mov [edx+7D0h], eax: edx=*(*(vtbl+0xC result)+0x4C)+0x1FBC);
  // *(edx+0x7D0)=unboxed F (exhaust min vol).
  // Twin setSteerWheelRadius @ 0x00440AF0 size 0x80: same walk, store +0xA14.
  // Engine blocks pass 0.6 / 0.9 (Baiern / Duhen). GAP: PE miss →
  // silent ret; host TREE + hdr+0x7D0 when blob cloned.
  if (!self) return;
  tree_field_set_float(self, "sfx_exhaust_min_vol", f);
  if (uint8_t* hdr = chassis_hdr_ptr(self)) hdr_put_f32(hdr, 0x7D0, f);
}

void java_game_parts_bodypart_Chassis_setSteerWheelRadius(InvObject* self, float f) {
  // PE @ 0x00440AF0
  // size 0x80 (IDA Chassis.setSteerWheelRadius). Unbox this + F (var_4).
  // Native.ptr (dword_62E008)==0 → jz loc_440B6D ret. NO Mighty.
  // inner=*(handle+0xC); 0 → ret. [inner+0x4C]!=1 → vtbl+0x14(0).
  // sub_5447D0(0xA0000000) test 80000000h sign → ret.
  // vtbl+0xC(1.0f=0x3F800000). ecx=*(obj+0x4C); edx=*(ecx+0x1FBC);
  // *(edx+0xA14)=F (int_convert 2580). Twin setAckermann @ 0x00440B70
  // same walk, store +0xA1C. Contrast setSteerWheel (FF) @ 0x00440A50:
  // +0xA14=r AND +0xA18=z (Radius overwrites R only).
  // Host: +0xA14 → steer_wheel_r + hdr blob when cloned.
  if (!self) return;
  tree_field_set_float(self, "steer_wheel_r", f);
  if (uint8_t* hdr = chassis_hdr_ptr(self)) hdr_put_f32(hdr, 0xA14, f);
}

void java_game_parts_bodypart_Chassis_setSteerWheel(InvObject* self, float r, float z) {
  // PE @ 0x00440A50 size 0x9A (IDA Chassis.setSteerWheel). Twin Radius
  // @ 0x00440AF0: same walk; here (FF) → [base+0xA14]=r, [base+0xA18]=z.
  // race122 filler SKIP — TREE primary stores already match PE slots.
  if (!self) return;
  tree_field_set_float(self, "steer_wheel_r", r);
  tree_field_set_float(self, "steer_wheel_z", z);
  if (uint8_t* hdr = chassis_hdr_ptr(self)) {
    hdr_put_f32(hdr, 0xA14, r);
    hdr_put_f32(hdr, 0xA18, z);
  }
}

void java_game_parts_bodypart_Chassis_setHornSFX(InvObject* self, InvObject* sfx,
                                                float pitch, int32_t index) {
  // PE @ 0x0043DC00 (IDA Chassis_setHornSFX). Unbox defaults: index=1,
  // sfx=0, pitch=1.0f. Bounds index [0,4) else silent. Hop
  // sub_5447D0(0x80000000). Slot Relink: hdr=*[veh+0x1FBC];
  // slot=hdr+0x834+index*0x10 (16B ResourceRef); sfx null → skip relink.
  // Pitch ALWAYS: fstp [hdr+0x874+index*4] (disasm @ 0x43dd61; int_convert
  // 2164). Host TREE stand-in (no +0x1FBC blob) — race125 PARTIAL.
  if (!self || index < 0 || index >= 4) return;
  char key[32];
  if (sfx) {
    std::snprintf(key, sizeof(key), "horn_sfx_%d", index);
    tree_field_set_obj(self, key, sfx);
  }
  std::snprintf(key, sizeof(key), "horn_pitch_%d", index);
  tree_field_set_float(self, key, pitch);
}

void java_game_parts_bodypart_Chassis_setNitroSFX(InvObject* self, InvObject* sfx,
                                                 float pitch) {
  // PE @ 0x0043DD70 size 0x13A (IDA Chassis_setNitroSFX). Unbox defaults:
  // sfx=0, pitch=1.0f. Hop sub_5447D0(0x80000000). EARLY-OUT if sfx==null
  // (cmp @ 0x43de06) — pitch unboxed but NEVER written (contrast setHornSFX:
  // pitch always @ hdr+0x874+index*4). Slot=*(owner+0x1FBC)+0x814 (2068);
  // skip if [slot+0xC]==*(sfx+0xC); else unlink/relink. race125 PARTIAL:
  // TREE nitro_sfx; no +0x1FBC ResourceRef list @ hdr+0x814. Pitch never stored.
  (void)pitch;
  if (!self || !sfx) return;
  tree_field_set_obj(self, "nitro_sfx", sfx);
}

// PE @ 0x0043DFF0 size 0x84 — IDA Chassis_getMileage.
// Unbox this (JVM_UnboxArg @ 0x0045D910). Native.ptr (dword_62E008 via
// JVM_vm_get_int_field @ 0x0042AB50)==0 → fld var_4 (0). NO Mighty.
// inner=*(handle+0xC); 0 → 0.0. [inner+0x4C]!=1 → vtbl+0x14(1.0f=
// 0x3F800000). sub_5447D0(0x80000000,0,0); EAX&0x80000000 → 0.0.
// obj=vtbl+0xC(1.0f); 0 → 0.0. fld float [*(obj+0x4C)+0x20FC]
// (int_convert 8444==0x20FC). Twin setMileage @ 0x0043E080 writes same
// slot; tick fadd in sub_454500. Single-level walk (contrast getMass
// @ 0x0043CAA0: 0xA0000000+0x20000000). Host: no Native.ptr / vtbl /
// sub_5447D0 — TREE "mileage" (setMileage twin). dword_62E008 /
// sub_5447D0 not renamed (shared / 310+ xrefs).

float java_game_parts_bodypart_Chassis_getMileage(InvObject* self) {
  // PE @ 0x0043DFF0 size 0x84. Phys → [*(obj+0x4C)+0x20FC]; host TREE.
  // race125 deepen_partial SKIP — primary TREE twin of setMileage already
  // stand-in (no +0x20FC phys). Hop tag 0x80000000 (single-level).
  return self ? tree_field_get_float(self, "mileage") : 0.f;
}

void java_game_parts_bodypart_Chassis_setMileage(InvObject* self, float m) {
  // PE @ 0x0043E080 size 0x85 (IDA Chassis.setMileage).
  // Unbox this + F (var_4 preset 0 then dest1). Native.ptr
  // (dword_62E008)==0 → jz loc_43E102 ret. NO Mighty.
  // inner=*(handle+0xC); 0 → ret. [inner+0x4C]!=1 → vtbl+0x14(1.0f=
  // 0x3F800000). sub_5447D0(0x80000000,0,0) test 80000000h sign → ret.
  // vtbl+0xC(1.0f). ecx=*(obj+0x4C); [ecx+0x20FC]=var_4 (mileage).
  // Twin getMileage @ 0x0043DFF0: same walk, fld same slot. Accumulators
  // PE: sub_454500 fadd [ebx+0x20FC]; init 0 in sub_44A250. Hex-rays
  // folded store=0 (lost var_4) — disasm mov edx,var_4 / mov [ecx+20FCh].
  // No clamp (contrast setCooling). GAP: PE miss → silent ret; host TREE
  // "mileage" (no +0x20FC phys / sub_5447D0 gate).
  // Soft PE aero side-band: stock apply is Chassis_physWheelTick inside
  // phys — host Δv lives in Resources::physics_integrate only. setMileage
  // may refresh TREE impulse preview (no Δv) when dt recoverable.
  if (!self) return;
  const float prev = tree_field_get_float(self, "mileage");
  tree_field_set_float(self, "mileage", m);
  const float d_miles = m - prev;
  if (d_miles > 0.f && d_miles < 50.f) {
    InvObject* body = self;
    if (physics_shape(body) == 0) {
      if (InvObject* car = tree_field_get_obj(self, "the_car")) {
        if (physics_shape(car) != 0) body = car;
      }
    }
    const float spd2 = physics_speed_square(body);
    if (spd2 > 1.f) {
      const float spd = std::sqrt(spd2);
      const float dt = d_miles / spd;
      if (dt > 0.f && dt < 0.5f) chassis_phys_aero_soft_apply(self, dt);
    }
  }
}

void java_game_parts_bodypart_Chassis_setBuck(InvObject* self, int32_t partID,
                                              int32_t buckid, float freq,
                                              float prob, float rpmdep,
                                              float amp) {
  // PE @ 0x0043E110 (IIFFFF)I returns node*: hop sub_419860(0x80000000);
  // walks list @ veh+0xDC (match id@+0x14 — list consumer); malloc 40 +
  // vtbl off_5F0F04; links into veh+0x1DEC/+0x1DE4. Hex-Rays drops
  // unboxed IIFFFF — race125 PARTIAL.
  // Host void upsert stand-in — not 1:1 (no node* return / phys list).
  if (!self) return;
  auto& list = g_bucks[self];
  for (auto& e : list) {
    if (e.part_id == partID && e.buck_id == buckid) {
      e.freq = freq;
      e.prob = prob;
      e.rpmdep = rpmdep;
      e.amp = amp;
      return;
    }
  }
  BuckEntry e;
  e.part_id = partID;
  e.buck_id = buckid;
  e.freq = freq;
  e.prob = prob;
  e.rpmdep = rpmdep;
  e.amp = amp;
  list.push_back(e);
  tree_field_set_int(self, "buck_count", static_cast<int32_t>(list.size()));
}

int32_t java_game_parts_bodypart_Chassis_getWheels(InvObject* self) {
  // PE @ 0x0043CD70 size 0xb9 (IDA Chassis_getWheels).
  // Unbox this. Native.ptr==0 / dual walk fail → 0. NO Mighty.
  // Same hop tags as getWheel @ 0x00440C80 (0xA0000000 then 0x20000000).
  // Success: *[*(leaf+0xC)+0x1F40] phys wheel count (layout dword —
  // same as getWheel bounds / forceUpdate_apply this+0x1F40). NOT
  // occupied-slot count. int_convert 8000.
  // Host +0x1F40 stand-in: TREE `wheels` (Chassis.java seeds 4) mirrored
  // to phys_wheel_count. PE miss / unset → 0 (no invent default-4).
  if (!self) return 0;
  const int32_t field = tree_field_get_int(self, "wheels");
  if (field > 0) {
    tree_field_set_int(self, "phys_wheel_count", field);
    ChassisCamTablesState& cam = chassis_cam_tables_ref(self);
    // Same PE +0x1F40 dword also caps gear-digit text loops @ 0x449aab.
    cam.gear_digit_cap = field;
    return field;
  }
  const int32_t phys = tree_field_get_int(self, "phys_wheel_count");
  if (phys > 0) return phys;
  return 0;
}

InvObject* java_game_parts_bodypart_Chassis_getWheel(InvObject* self,
                                                    int32_t id) {
  // PE @ 0x00440C80 size 0x13b (IDA Chassis_getWheel).
  // Unbox this + I (var_4 preset 0). id < 0 → null (jl loc_440DB4).
  // Native.ptr (dword_62E008)==0 → null. NO Mighty. Dual Res walk
  // 0xA0000000 then 0x20000000. Bounds ASM @ 0x440d5f: cmp id,
  // [*(leaf+0xC)+0x1F40]; jge → null. wheel* = *[veh+0x13E4] + id*0x2B4
  // (LEA @ 0x440d67..0x440d7f — Hex-Rays drops index). Always NEW
  // WheelRef (Class_boxObject) + Native.ptr=wheel* (address, not deref).
  // Host: bounds via getWheels (+0x1F40). Always-new WheelRef; chassis
  // (+0x22C via extra.veh), wheel_id; native_ptr = &phys_slot; seed
  // +0x78 from slot 101+id when unset; contact30 for setCPatch gate.
  // race125 PARTIAL: ASM @ 0x440d5f jge vs +0x1F40; LEA id*0x2B4 @
  // 0x440d67..7f; Class_boxObject + Native.ptr=wheel* (address). No raw
  // *[veh+0x13E4] / dual Res hop. Host also nulls id<0 (PE would LEA).
  if (!self || id < 0) return nullptr;
  if (id >= java_game_parts_bodypart_Chassis_getWheels(self)) return nullptr;
  if (id >= 8) return nullptr;
  InvObject* wr = tree_host_new("java.game.parts.WheelRef");
  if (!wr) return nullptr;
  constexpr int32_t kStride = 0x2B4;  // PE LEA id*0x2B4 (int_convert 692)
  WheelRefState& slot = chassis_phys_wheel_slot(self, id);
  if (!slot.has_pos) {
    float px = 0.f, py = 0.f, pz = 0.f;
    if (part_slot_get_pose(self, 101 + id, &px, &py, &pz, nullptr, nullptr,
                           nullptr)) {
      slot.px = px;
      slot.py = py;
      slot.pz = pz;
      slot.has_pos = true;
    }
  }
  const auto wheel_ptr =
      static_cast<int32_t>(reinterpret_cast<uintptr_t>(&slot));
  tree_field_set_obj(wr, "chassis", self);  // PE handle+0x22C Java mirror
  tree_field_set_int(wr, "wheel_id", id);
  tree_field_set_int(wr, "native_ptr", wheel_ptr);
  tree_field_set_int(wr, "native_ptr_lea", id * kStride);
  // phys_wheels_base set inside chassis_phys_wheel_slot (*[veh+0x13E4]).
  tree_field_set_obj(wr, "part_parent", self);
  tree_field_set_int(wr, "part_parent_slot", 101 + id);
  (void)part_on_slot(self, 101 + id);  // touch install graph only
  return wr;
}

}  // namespace inv
