// Fragment 3/4 of Resources.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.

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
  // Native.ptr → RenderRef_applyLight @ 0x0048C9D0 (dir a5=null).
  // Soft: packed ResState + apply_light_soft TREE units + D3D bind.
  // OOS: getPayload tag 0x80000001 mid poke / dir vector +0x4C.
  if (self) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& r = R(self);
    r.light_diffuse = diffuse;
    r.light_ambient = ambient;
    r.light_specular = specular;
  }
  if (self) render_ref_apply_light_soft(self, diffuse, ambient, specular);
  render_d3d9_set_light(diffuse, ambient, specular);
}
void java_util_resource_RenderRef_setFlare(InvObject* self, InvObject* glowtexture,
                                           int32_t glowColor, float glowMinSize,
                                           float glowMaxSize, int32_t flareCount,
                                           int32_t rayCount) {
  // PE @ 0x00486B20: Unbox → ResHandle_ctorFromObj(glow) →
  // RenderRef_applyFlare @ 0x0048CB40 (+0xD4 min/+0xD8 max/+0xE0 color/
  // +0xE4 rays/+0x100 count; Rebind payload+0xC4).
  // Soft: ResState + apply_flare_soft TREE + D3D. OOS: ctorFromObj/Rebind.
  if (!self) return;
  const int32_t tex_id =
      glowtexture ? java_util_resource_ResourceRef_id(glowtexture) : 0;
  if (glowtexture && !render_d3d9_texture_ready(glowtexture))
    java_util_resource_ResourceRef_load(glowtexture);
  float wx = 0.f, wy = 0.f, wz = 0.f;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& r = R(self);
    r.flare_tex_id = tex_id;
    r.flare_color = glowColor;
    r.flare_min = glowMinSize;
    r.flare_max = glowMaxSize;
    r.flare_count = flareCount;
    r.flare_rays = rayCount;
    wx = r.px;
    wy = r.py;
    wz = r.pz;
  }
  render_ref_apply_flare_soft(self, glowtexture, glowColor, glowMinSize,
                              glowMaxSize, flareCount, rayCount);
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