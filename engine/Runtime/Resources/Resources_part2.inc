// Fragment 2/4 of Resources.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


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
// W38 / asyncload_hosted: MainLoop Soft HasWork@505CD0 size 0xd0 →
//   PumpOne@505DB0 → FinishSlice@505DA0 lives in System.cpp; Resources
//   side is this callee. AsyncLoad_DrainWorkFail_GetPackSlot@505EA0 size
//   0x84 (0 code xrefs — orphan) walks Work@65BD60 / Fail@65C1E8 type0..5
//   and calls GetPackSlot(eng, edx=job+0x38 unused, id=job+0x40). Soft
//   id path = this function; list walk stays System Soft (orphan PE).
// Residual OOS (same class as pump_load hosted): PostLoop@4FFFE0 D3D
//   VB/IB create/bind + Type2 JPEG mip upload — not GetPackSlot.
void* host_resource_engine_get_pack_slot(uint32_t id) {
  host_pack_eng_sync_from_rpak();
  const uint32_t hi = id >> 16;  // @ 0x5383F4
  if (hi == 0xFFFFu) return g_pack_eng.ffff.raw;  // @ 0x538430
  if (hi >= g_pack_eng.pack_count) return nullptr;  // @ 0x538404
  if (hi >= kMaxPackSlots) return nullptr;
  void* slot = g_pack_eng.slots[hi].raw;
  // PE: EnsureIndex!=0 ? slot : 0  (@ 0x53841C..0x538425)
  // DrainWorkFail@505EA0 Work site also loads edx=job+0x38 before call —
  // discarded here (PE GetPackSlot never reads edx).
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