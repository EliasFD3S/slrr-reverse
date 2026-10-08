// Fragment 1/6 of GameRef.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


namespace inv {
namespace {


// PE add_light list @ 0x45B034: [tail+4]=node; node+4=&+0x206C; node+8=tail;
// +0x2074=node. Host: heap sentinel stands in for &GameRef+0x206C.
static GameRefState::PeLightNode* pe_light_ensure_sent(GameRefState& r) {
  if (!r.light_sent) {
    r.light_sent = std::make_unique<GameRefState::PeLightNode>();
    r.light_tail = r.light_sent.get();
  }
  if (!r.light_tail) r.light_tail = r.light_sent.get();
  return r.light_sent.get();
}

static void pe_light_insert_tail(GameRefState& r,
                                 GameRefState::PeLightNode* node) {
  auto* sent = pe_light_ensure_sent(r);
  auto* old_tail = r.light_tail ? r.light_tail : sent;
  old_tail->next = node;
  node->next = sent;
  node->prev = old_tail;
  r.light_tail = node;
}

static void pe_light_unlink(GameRefState& r, GameRefState::PeLightNode* node) {
  if (!node || !r.light_sent) return;
  auto* sent = r.light_sent.get();
  if (node->prev) node->prev->next = node->next;
  if (node->next && node->next != sent) node->next->prev = node->prev;
  if (r.light_tail == node) r.light_tail = node->prev ? node->prev : sent;
  node->next = nullptr;
  node->prev = nullptr;
}

static GameRefState::PeWingNode* pe_wing_ensure_sent(GameRefState& r) {
  if (!r.wing_sent) {
    r.wing_sent = std::make_unique<GameRefState::PeWingNode>();
    r.wing_tail = r.wing_sent.get();
  }
  if (!r.wing_tail) r.wing_tail = r.wing_sent.get();
  return r.wing_sent.get();
}

static void pe_wing_insert_tail(GameRefState& r,
                                GameRefState::PeWingNode* node) {
  auto* sent = pe_wing_ensure_sent(r);
  auto* old_tail = r.wing_tail ? r.wing_tail : sent;
  old_tail->next = node;
  node->next = sent;
  node->prev = old_tail;
  r.wing_tail = node;
}

static void pe_wing_unlink(GameRefState& r, GameRefState::PeWingNode* node) {
  if (!node || !r.wing_sent) return;
  auto* sent = r.wing_sent.get();
  if (node->prev) node->prev->next = node->next;
  if (node->next && node->next != sent) node->next->prev = node->prev;
  if (r.wing_tail == node) r.wing_tail = node->prev ? node->prev : sent;
  node->next = nullptr;
  node->prev = nullptr;
}

static GameRefState::PeMslotNode* pe_mslot_ensure_sent(GameRefState& r) {
  if (!r.mslot_sent) {
    r.mslot_sent = std::make_unique<GameRefState::PeMslotNode>();
    r.mslot_tail = r.mslot_sent.get();
  }
  if (!r.mslot_tail) r.mslot_tail = r.mslot_sent.get();
  return r.mslot_sent.get();
}

static void pe_mslot_insert_tail(GameRefState& r,
                                 GameRefState::PeMslotNode* node) {
  auto* sent = pe_mslot_ensure_sent(r);
  auto* old_tail = r.mslot_tail ? r.mslot_tail : sent;
  old_tail->next = node;
  node->next = sent;
  node->prev = old_tail;
  r.mslot_tail = node;
}

// PE GameRef_physDcList_insertTail @ 0x45FAF0 (this=phys+0xD4):
//   tail=*(this+0x18); [tail+4]=node; node+4=this+0x10; node+8=tail;
//   *(this+0x18)=node. Host: heap sentinel == PE +0xE4.
static GameRefState::PePhysDcNode* pe_phys_dc_ensure_sent(GameRefState& r) {
  if (!r.phys_dc_sent) {
    r.phys_dc_sent = std::make_unique<GameRefState::PePhysDcNode>();
    r.phys_dc_tail = r.phys_dc_sent.get();
  }
  if (!r.phys_dc_tail) r.phys_dc_tail = r.phys_dc_sent.get();
  return r.phys_dc_sent.get();
}

static void pe_phys_dc_insert_tail(GameRefState& r,
                                   GameRefState::PePhysDcNode* node) {
  auto* sent = pe_phys_dc_ensure_sent(r);
  auto* old_tail = r.phys_dc_tail ? r.phys_dc_tail : sent;
  old_tail->next = node;
  node->next = sent;
  node->prev = old_tail;
  r.phys_dc_tail = node;
}

static void pe_phys_dc_unlink(GameRefState& r,
                              GameRefState::PePhysDcNode* node) {
  if (!node || !r.phys_dc_sent) return;
  auto* sent = r.phys_dc_sent.get();
  if (node->prev) node->prev->next = node->next;
  if (node->next && node->next != sent) node->next->prev = node->prev;
  if (r.phys_dc_tail == node) r.phys_dc_tail = node->prev ? node->prev : sent;
  node->next = nullptr;
  node->prev = nullptr;
}

// PE GameRef_cmdAddPart @ 0x447B3D..0x447B88: malloc 0x1C →
// Engine_SimCallbackNode_ctor @ 0x429130 → ResHandle clear @ +0xC →
// ResHandle_Rebind(node+0xC, *(part+0xC)) @ 0x447B74 →
// GameRef_physDcList_insertTail(phys+0xD4, node) @ 0x447B82.
static void gameref_phys_dc_add_part(InvObject* self, InvObject* part) {
  if (!self || !part) return;
  native_ptr_ensure(part);
  void* inst = native_ptr_node(part);
  auto node = std::make_unique<GameRefState::PePhysDcNode>();
  node->part = part;
  if (inst) {
    res_handle_rebind(node->rh, inst);
    node->match_key =
        *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(node->rh) + 8);
  }
  if (node->match_key == 0)
    node->match_key = java_util_resource_ResourceRef_id(part);
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& st = ref(self);
    GameRefState::PePhysDcNode* raw = node.get();
    pe_phys_dc_insert_tail(st, raw);
    st.phys_dc_nodes.push_back(std::move(node));
  }
  tree_field_set_int(self, "phys_dc_count",
                     tree_field_get_int(self, "phys_dc_count") + 1);
  tree_field_set_obj(self, "last_phys_dc_part", part);
}

// PE GameRef_cmdRemPart @ 0x44803E..0x4480A1: Engine_SimObjectListEmpty
// (phys+0xD4); walk head=*[phys+0xDC] match *(node+0x14)==*(part+8);
// unlink next/prev; vtbl dtor(1).
static void gameref_phys_dc_rem_part(InvObject* self, InvObject* part) {
  if (!self || !part) return;
  const int32_t want = java_util_resource_ResourceRef_id(part);
  bool removed = false;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& st = ref(self);
    GameRefState::PePhysDcNode* hit = nullptr;
    for (auto& up : st.phys_dc_nodes) {
      if (!up) continue;
      if (up->part == part || (want != 0 && up->match_key == want)) {
        hit = up.get();
        break;
      }
    }
    if (!hit) return;
    pe_phys_dc_unlink(st, hit);
    void_event_res_handle_unbind_owner(hit->rh);
    for (auto it = st.phys_dc_nodes.begin(); it != st.phys_dc_nodes.end();
         ++it) {
      if (it->get() == hit) {
        st.phys_dc_nodes.erase(it);
        removed = true;
        break;
      }
    }
  }
  if (removed) {
    const int32_t n = tree_field_get_int(self, "phys_dc_count");
    if (n > 0) tree_field_set_int(self, "phys_dc_count", n - 1);
  }
}

// PE ResHandle_Bind @ 0x546070: this=rh, a2=id, a3=type, a4=0.
// → ResourceEngine_LookupById @ 0x536820 (was sub_536820) →
// ResourceEngine_ResolveParent @ 0x537000 (type filter a3 vs [inner+0x4C]).
// On hit: link rh into instance+0x44 list; rh+8=id; rh+0xC=instance.
// Host: resref_find_by_id + type gate + res_handle_rebind (same +0x44/+0x48
// list as Bind) then overwrite rh+8 with requested id (PE Bind ≠ Rebind key).
// Miss / no Native.ptr: soft store id at +8 only (no invent ResourceEngine).
// Returns 1 when rh+0xC linked.
static int32_t res_handle_bind_resolve(void* rh, int32_t handle_id,
                                       int32_t type) {
  if (!rh) return 0;
  auto* words = reinterpret_cast<int32_t*>(rh);
  // PE @ 0x54607d: same id → no-op (keep prior link).
  if (handle_id != 0 && words[2] == handle_id &&
      *reinterpret_cast<void**>(reinterpret_cast<char*>(rh) + 0xC) != nullptr) {
    return 1;
  }
  // Unlink prior owner slice (Bind @ 0x546083..0x5460bd ≡ Rebind(rh,0)).
  res_handle_rebind(rh, nullptr);
  std::memset(rh, 0, 16);
  if (handle_id == 0) return 0;
  words[2] = handle_id;

  InvObject* obj = resref_find_by_id(handle_id);
  if (!obj) return 0;
  // PE ResolveParent @ 0x5370cd: a3!=0 → require [inner+0x4C]==a3.
  // RESOURCE_VIEWPORT=0x12 (ResourceRef.java). Host type 0 = unset → allow.
  if (type != 0) {
    const int32_t ot = java_util_resource_ResourceRef_type(obj);
    if (ot != 0 && ot != type) return 0;
  }
  native_ptr_ensure(obj);
  void* node = native_ptr_node(obj);
  if (!node) return 0;
  // Seed owner+0x50 when empty so Rebind-shaped list key matches id.
  {
    auto* key = reinterpret_cast<int32_t*>(reinterpret_cast<char*>(node) + 0x50);
    if (*key == 0) *key = handle_id;
  }
  res_handle_rebind(rh, node);
  words[2] = handle_id;  // PE Bind stores a2, not *(owner+0x50)
  return *reinterpret_cast<void**>(reinterpret_cast<char*>(rh) + 0xC) != nullptr
             ? 1
             : 0;
}

static void pe_mslot_unlink(GameRefState& r, GameRefState::PeMslotNode* node) {
  if (!node || !r.mslot_sent) return;
  auto* sent = r.mslot_sent.get();
  if (node->prev) node->prev->next = node->next;
  if (node->next && node->next != sent) node->next->prev = node->prev;
  if (r.mslot_tail == node) r.mslot_tail = node->prev ? node->prev : sent;
  node->next = nullptr;
  node->prev = nullptr;
}

// PE GameRef_setParent_inner @ 0x0048ABA0 type 2–3: GameRef_dllist_unlink
// @ 0x004A5D00 + list_add_tail under parent +0x30 sentinel / +0x38 tail.
// Type-1 @ 0x48ACC1: getPayload → leaf.vtbl+0x20 @ 0x48AD0A →
// CameraCtrl_attachSetParent @ 0x00438590: handleAttachUnderParent @
// 0x00429C80 then setParent_inner(block+0x30). Chassis @ 0x44C170 OOS.
// Type53_ensureHandleSlot @ 0x004B3EE0: lists A/B/C + malloc(200);
// found A/C retarget + C promote; W21C: insertSpatialBounds @ 0x4B3120;
// W22C: linkNearbySlots @ 0x4B36B0 malloc20+dllist;
// W23C: Type53_promoteSlotOnLink @ 0x4B3AC0 (RE @ 0x537240 OOS).
// W24C: insertSpatialBounds READS owner+0x84/+0x90 only (a3=cursor);
// half-extent fill = EXTP@53E108 / unionChildAabb@4988A0 (Resources OOS
// on HostResNode). Degenerate AABB skip + walk_guard 4096 kept.
// Host: sib_* == parent ↔ PE &parent+0x30 sentinel. Hold g_gr_mu.

// PE Type53_ensureHandleSlot @ 0x004B3EE0 size 0x367 (mgr = type53_block+0xB4).
// Walks three SimObject lists then Engine_malloc(200) @ 0x4B403A.
// Mgr dword layout (IDA): listA this+11 → head [13]; listB this+4 → head [6];
// listC sentinel [20]; insert sentinel &mgr[15] (+0x3C) / tail [17] (+0x44);
// counts [47]/[48]; gen [52]. Slot: +4 next / +8 prev / +0x14 key / +0x18
// Rebind owner; [49]=a3 binder. Found A/C: a3 retarget @ 0x4B3F74 / 0x4B410C;
// found C: promote to insert-list @ 0x4B4181 (or flag|8 if [47]!=0).
// Malloc tail @ 0x4B422F: Type53_insertSpatialBounds(mgr,slot,a3) @ 0x4B3120
// + Type53_linkNearbySlots(mgr,slot) @ 0x4B36B0.
// Key: PE result[5]==handle[+8]; host also stores handle addr at slot[+0x14]
// (HostNativeHandle+8 is alive=1 for all).
// Callers a3=0: setParent @ 0x48AD45, CreateGameInstanceNative @ 0x53A57B.
std::vector<void*> g_type53_owned_slots;  // malloc(200) lifetimes
std::vector<void*> g_type53_owned_neighbor_links;  // malloc(20) @ 0x4B378C

// PE spatial axis lists @ mgr+0x64 stride 0x38 ×3 (disasm edi=mgr+0x6C).
// Host HostType53Block.mgr[0xD4] ends at axis2 base — side blob mirrors
// PE +0x64..+0x10C so splice never OOB. Counts stay at mgr[+0xC4].
struct Type53SpatialLists {
  std::uint32_t raw[0x38u * 3u / 4u]{};
  // PE mgr[+0xC4]/[+0xCC]/[+0xD4]; host mgr[0xD4] only fits first two.
  std::uint32_t counts[3]{};
};
std::unordered_map<std::uint32_t*, Type53SpatialLists> g_type53_spatial;

enum class Type53FoundList : std::uint8_t { None, A, B, C };

static bool type53_sim_list_empty(std::uint32_t* list_obj) {
  // Engine_SimObjectListEmpty @ 0x00429390: *(*(this+2)+4)==0
  if (!list_obj) return true;
  auto* head_holder = reinterpret_cast<std::uint32_t**>(list_obj + 2);
  std::uint32_t* head = *head_holder;
  if (!head) return true;
  return head[1] == 0;
}

static std::uint32_t* type53_sim_list_head(std::uint32_t* list_obj) {
  if (!list_obj || type53_sim_list_empty(list_obj)) return nullptr;
  return reinterpret_cast<std::uint32_t*>(list_obj[2]);
}

static void type53_mgr_lazy_init(std::uint32_t* mgr) {
  // HostType53Block.mgr starts zeroed — PE constructs sentinels before use.
  // Insert list: tail [17] = &sentinel[15] when empty (see insert @ 0x4B420C).
  if (mgr[17] == 0) {
    mgr[15] = 0;
    mgr[16] = 0;
    mgr[17] = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(&mgr[15]));
  }
  // List A/B heads → insert sentinel (next==0 ⇒ empty).
  if (mgr[13] == 0)
    mgr[13] = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(&mgr[15]));
  if (mgr[6] == 0)
    mgr[6] = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(&mgr[15]));
  // List C: [20] is node*; empty when *[20]+4 == 0.
  if (mgr[20] == 0) {
    mgr[20] = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(&mgr[20]));
    mgr[21] = 0;
    mgr[22] = 0;
  }
}

static std::uint32_t* type53_slot_walk(std::uint32_t* head, std::uint32_t key,
                                      void* handle_node) {
  std::uint32_t* result = head;
  while (result) {
    // PE @ 0x4B3F4A: result[5] == *(handle+8)
    bool key_ok = (result[5] == key);
    // Host: key may be handle addr (alive collision); also match Rebind node.
    if (!key_ok && handle_node &&
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(result[6])) ==
            handle_node)
      key_ok = true;
    if (key_ok) return result;
    result = reinterpret_cast<std::uint32_t*>(
        static_cast<std::uintptr_t>(result[1]));
    if (!result || result[1] == 0) return nullptr;
  }
  return nullptr;
}

// PE @ 0x4B3F74 (list A) / 0x4B410C (list C): retarget result[49] binder.
static void type53_slot_retarget_a3(std::uint32_t* result, void* a3) {
  if (!result) return;
  std::uint32_t cur = result[49];
  if (cur == 0) return;
  auto a3u = reinterpret_cast<std::uintptr_t>(a3);
  if (a3 && a3u != static_cast<std::uintptr_t>(cur)) {
    --(*reinterpret_cast<std::uint32_t*>(
        static_cast<std::uintptr_t>(cur) + 188));
    ++(*reinterpret_cast<std::uint32_t*>(
        reinterpret_cast<unsigned char*>(a3) + 188));
    result[49] = static_cast<std::uint32_t>(a3u);
  }
  // Clear binder if owner node parent id mismatch @ 0x4B3F9F / 0x4B4133.
  std::uint32_t owner = result[6];
  if (owner == 0) return;
  std::uint32_t binder = result[49];
  if (binder == 0) return;
  auto* own_b = reinterpret_cast<unsigned char*>(
      static_cast<std::uintptr_t>(owner));
  void* mid14 = *reinterpret_cast<void**>(own_b + 0x14);
  if (!mid14) return;
  std::uint32_t parent_id =
      *reinterpret_cast<std::uint32_t*>(
          reinterpret_cast<unsigned char*>(mid14) + 0x50);
  std::uint32_t binder_id =
      *reinterpret_cast<std::uint32_t*>(
          static_cast<std::uintptr_t>(binder) + 0x14);
  if (parent_id != binder_id) {
    --(*reinterpret_cast<std::uint32_t*>(
        static_cast<std::uintptr_t>(binder) + 188));
    result[49] = 0;
  }
}

// PE splice into insert-list: flag&=~0x1A; unlink +4/+8; tail→slot→sentinel;
// mgr[47]++ mgr[48]--. Shared by list-C promote @ 0x4B4181 and
// Type53_promoteSlotOnLink @ 0x4B3BDF.
static void type53_slot_splice_to_insert_list(std::uint32_t* mgr,
                                             std::uint32_t* slot) {
  if (!mgr || !slot) return;
  slot[7] &= 0xFFFFFFE5u;
  std::uint32_t prev = slot[2];
  if (prev != 0) {
    std::uint32_t next = slot[1];
    if (next != 0) {
      *reinterpret_cast<std::uint32_t*>(
          static_cast<std::uintptr_t>(next) + 8) = prev;
      *reinterpret_cast<std::uint32_t*>(
          static_cast<std::uintptr_t>(prev) + 4) = next;
    }
  }
  slot[1] = 0;
  slot[2] = 0;
  auto* old_tail = reinterpret_cast<std::uint32_t*>(
      static_cast<std::uintptr_t>(mgr[17]));
  if (!old_tail) old_tail = &mgr[15];
  old_tail[1] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(slot));
  slot[2] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(old_tail));
  slot[1] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(&mgr[15]));
  mgr[17] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(slot));
  ++mgr[47];
  --mgr[48];
}

// PE @ 0x4B415E–0x4B41B6: list-C hit → flag|8 if [47]!=0, else splice into
// insert-list (sentinel &mgr[15] / tail mgr[17]); counts [47]++ [48]--.
static void type53_list_c_promote(std::uint32_t* mgr, std::uint32_t* result) {
  if (!mgr || !result) return;
  if (result[47] != 0) {
    result[7] |= 8u;
    return;
  }
  type53_slot_splice_to_insert_list(mgr, result);
}

// PE Type53_promoteSlotOnLink @ 0x004B3AC0: walk list A (mgr[13]) then list C
// (mgr[20]) for deps with [49]==slot; unlink dep Rebind list via [3]/[4] +
// owner[+0x48]; --slot[+0xBC]; clear binder. Then splice slot → insert-list.
// Call site linkNearby @ 0x4B37FC when flag&8 && flag&0x10.
static std::uint32_t* type53_sim_walk_next(std::uint32_t* cur) {
  if (!cur || cur[1] == 0) return nullptr;
  auto* n = reinterpret_cast<std::uint32_t*>(
      static_cast<std::uintptr_t>(cur[1]));
  if (!n || n[1] == 0) return nullptr;
  return n;
}

static void type53_clear_dependent_binder(std::uint32_t* dep,
                                         std::uint32_t* binder) {
  // PE @ 0x4B3AF7 / 0x4B3B87: owner=dep[6]; prev/next = dep[3]/[4].
  std::uint32_t owner = dep[6];
  if (owner != 0) {
    std::uint32_t prev = dep[3];
    std::uint32_t next = dep[4];
    if (prev != 0) {
      *reinterpret_cast<std::uint32_t*>(
          static_cast<std::uintptr_t>(prev) + 4) = next;
    } else {
      *reinterpret_cast<std::uint32_t*>(
          static_cast<std::uintptr_t>(owner) + 0x48) = next;
    }
    if (next != 0) {
      *reinterpret_cast<std::uint32_t*>(
          static_cast<std::uintptr_t>(next)) = prev;
    }
    dep[6] = 0;
    dep[5] = 0;
    dep[3] = 0;
    dep[4] = 0;
  } else {
    dep[5] = 0;
  }
  --binder[47];  // binder+0xBC
  dep[49] = 0;
}

static void type53_promote_slot_on_link(std::uint32_t* mgr,
                                       std::uint32_t* slot) {
  if (!mgr || !slot) return;
  // List A: start at mgr[13] if head.next != 0 — PE @ 0x4B3AC1.
  std::uint32_t* cur = nullptr;
  if (mgr[13] != 0) {
    auto* head = reinterpret_cast<std::uint32_t*>(
        static_cast<std::uintptr_t>(mgr[13]));
    if (head[1] != 0) cur = head;
  }
  bool early = false;
  while (cur) {
    std::uint32_t* next = type53_sim_walk_next(cur);
    if (reinterpret_cast<std::uint32_t*>(
            static_cast<std::uintptr_t>(cur[49])) == slot) {
      type53_clear_dependent_binder(cur, slot);
      if (slot[47] == 0) {
        early = true;
        break;
      }
    }
    cur = next;
  }
  // LABEL_16 @ 0x4B3B53: if binder count still >0, walk list C.
  if (!early && slot[47] != 0) {
    cur = nullptr;
    if (mgr[20] != 0) {
      auto* head = reinterpret_cast<std::uint32_t*>(
          static_cast<std::uintptr_t>(mgr[20]));
      if (head[1] != 0) cur = head;
    }
    while (cur) {
      std::uint32_t* next = type53_sim_walk_next(cur);
      if (reinterpret_cast<std::uint32_t*>(
              static_cast<std::uintptr_t>(cur[49])) == slot) {
        type53_clear_dependent_binder(cur, slot);
        if (slot[47] == 0) break;
      }
      cur = next;
    }
  }
  // PE @ 0x4B3BDF — always splice after binder walks.
  type53_slot_splice_to_insert_list(mgr, slot);
}

// PE spatial list @ mgr+0x64+axis*0x38: head[2], sentinel[4] (+0x10),
// tail[6] (+0x18). physDc-style — empty when head.next==0.
static std::uint32_t* type53_spatial_axis_list(std::uint32_t* mgr, int axis) {
  auto& blob = g_type53_spatial[mgr];
  return blob.raw + static_cast<std::size_t>(axis) * (0x38u / 4u);
}

static void type53_spatial_list_lazy(std::uint32_t* list) {
  if (!list || list[2] != 0) return;
  list[4] = 0;
  list[5] = 0;
  list[2] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(&list[4]));
  list[6] = list[2];
}

// PE @ 0x4B3273: push min-link as new head.
static void type53_spatial_insert_head(std::uint32_t* list,
                                      std::uint32_t* node) {
  auto* head = reinterpret_cast<std::uint32_t*>(
      static_cast<std::uintptr_t>(list[2]));
  if (!head) head = &list[4];
  head[2] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(node));
  node[1] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(head));
  node[2] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(&list[1]));
  list[2] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(node));
}

// PE @ 0x4B3354: append max-link as new tail (next → sentinel @ list+0x10).
static void type53_spatial_insert_tail(std::uint32_t* list,
                                      std::uint32_t* node) {
  auto* tail = reinterpret_cast<std::uint32_t*>(
      static_cast<std::uintptr_t>(list[6]));
  if (!tail) tail = &list[4];
  tail[1] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(node));
  node[1] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(&list[4]));
  node[2] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(tail));
  list[6] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(node));
}

// PE @ 0x4B3242: splice node before `at`.
static void type53_spatial_splice_before(std::uint32_t* node,
                                        std::uint32_t* at) {
  auto* prev = reinterpret_cast<std::uint32_t*>(
      static_cast<std::uintptr_t>(at[2]));
  node[1] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(at));
  node[2] = at[2];
  if (prev)
    prev[1] = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(node));
  at[2] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(node));
}

// PE @ 0x4B329F: splice node after `at` (between at and at.next).
static void type53_spatial_splice_after(std::uint32_t* node,
                                       std::uint32_t* at) {
  auto* next = reinterpret_cast<std::uint32_t*>(
      static_cast<std::uintptr_t>(at[1]));
  node[1] = at[1];
  node[2] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(at));
  at[1] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(node));
  if (next)
    next[2] = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(node));
}

// PE Type53_insertSpatialBounds @ 0x004B3120: READS owner AABB then embeds.
// W24C IDA: result=*(slot+0x18); result[33..38] = +0x84 center / +0x90 half
// → mins/maxs → slot axis floats (+0x30/+0x6C …); a3 is a *cursor slot*
// for sorted splice (a3+0x20/+0x5C), NOT a bounds source. PE does NOT write
// owner+0x84..+0x98 here. Half-extent fill is Resources:
//   EXTP expand @ 0x53E108 (host_extp_expand_node_aabb) /
//   ResNode_unionChildAabb @ 0x4988A0. Host Native.ptr HostResNode pad at
//   +0x84 stays 0 until that path lands on the same node blob.
static void type53_insert_spatial_bounds(std::uint32_t* mgr,
                                        std::uint32_t* slot, void* a3) {
  if (!mgr || !slot) return;
  auto* node = reinterpret_cast<float*>(
      static_cast<std::uintptr_t>(slot[6]));  // +0x18
  if (!node) return;

  // PE @ 0x4B313E..0x4B3162: consume only (no invent / no a3→owner copy).
  const float cx = node[0x84 / 4];
  const float cy = node[0x88 / 4];
  const float cz = node[0x8C / 4];
  const float hx = node[0x90 / 4];
  const float hy = node[0x94 / 4];
  const float hz = node[0x98 / 4];
  const float mins[3] = {cx - hx, cy - hy, cz - hz};
  const float maxs[3] = {cx + hx, cy + hy, cz + hz};

  auto* a3b = reinterpret_cast<unsigned char*>(a3);
  // PE loop: ebp 0,8,16 cmp 0xC — three axes; slot stride 0x28; list +0x38.
  for (int axis = 0; axis < 3; ++axis) {
    auto* list = type53_spatial_axis_list(mgr, axis);
    type53_spatial_list_lazy(list);

    auto* slot_b = reinterpret_cast<unsigned char*>(slot);
    auto* min_link = reinterpret_cast<std::uint32_t*>(
        slot_b + 0x20 + axis * 0x28);
    auto* max_link = reinterpret_cast<std::uint32_t*>(
        slot_b + 0x5C + axis * 0x28);
    *reinterpret_cast<float*>(slot_b + 0x30 + axis * 0x28) = mins[axis];
    *reinterpret_cast<float*>(slot_b + 0x6C + axis * 0x28) = maxs[axis];

    // --- min sorted insert (PE @ 0x4B31CE) ---
    std::uint32_t* cur = nullptr;
    if (a3b) {
      cur = reinterpret_cast<std::uint32_t*>(a3b + 0x20 + axis * 0x28);
    } else if (!type53_sim_list_empty(list)) {
      cur = reinterpret_cast<std::uint32_t*>(
          static_cast<std::uintptr_t>(list[2]));
    }
    if (!cur) {
      type53_spatial_insert_head(list, min_link);
    } else {
      const float our = mins[axis];
      float cf = *reinterpret_cast<float*>(
          reinterpret_cast<unsigned char*>(cur) + 0x10);
      if (cf > our) {
        // Walk prev (+8) while float >= our — PE @ 0x4B3253.
        while (cur) {
          cf = *reinterpret_cast<float*>(
              reinterpret_cast<unsigned char*>(cur) + 0x10);
          if (cf < our) break;
          auto* prev = reinterpret_cast<std::uint32_t*>(
              static_cast<std::uintptr_t>(cur[2]));
          if (!prev || prev[2] == 0) {
            cur = nullptr;
            break;
          }
          cur = prev;
        }
        if (!cur)
          type53_spatial_insert_head(list, min_link);
        else
          type53_spatial_splice_after(min_link, cur);
      } else {
        // Walk next (+4) while float < our — PE @ 0x4B320A.
        while (cur) {
          cf = *reinterpret_cast<float*>(
              reinterpret_cast<unsigned char*>(cur) + 0x10);
          if (!(cf < our)) break;
          auto* next = reinterpret_cast<std::uint32_t*>(
              static_cast<std::uintptr_t>(cur[1]));
          if (!next || next[1] == 0) {
            cur = nullptr;
            break;
          }
          cur = next;
        }
        if (!cur)
          type53_spatial_insert_tail(list, min_link);  // PE @ 0x4B322A
        else
          type53_spatial_splice_before(min_link, cur);
      }
    }

    // --- max sorted insert (PE @ 0x4B3283) ---
    cur = nullptr;
    if (a3b) {
      cur = reinterpret_cast<std::uint32_t*>(a3b + 0x5C + axis * 0x28);
    } else if (list[2] != 0 &&
               reinterpret_cast<std::uint32_t*>(
                   static_cast<std::uintptr_t>(list[2]))[1] != 0) {
      cur = reinterpret_cast<std::uint32_t*>(
          static_cast<std::uintptr_t>(list[6]));
    }
    if (!cur) {
      type53_spatial_insert_tail(list, max_link);
    } else {
      const float our = maxs[axis];
      float cf = *reinterpret_cast<float*>(
          reinterpret_cast<unsigned char*>(cur) + 0x10);
      if (our >= cf) {
        // Walk next while float < our — PE @ 0x4B331A.
        while (cur) {
          cf = *reinterpret_cast<float*>(
              reinterpret_cast<unsigned char*>(cur) + 0x10);
          if (!(cf < our)) break;
          auto* next = reinterpret_cast<std::uint32_t*>(
              static_cast<std::uintptr_t>(cur[1]));
          if (!next || next[1] == 0) {
            cur = nullptr;
            break;
          }
          cur = next;
        }
        if (!cur)
          type53_spatial_insert_tail(list, max_link);
        else
          type53_spatial_splice_before(max_link, cur);
      } else {
        // Walk prev while float >= our — PE @ 0x4B32D2.
        while (cur) {
          cf = *reinterpret_cast<float*>(
              reinterpret_cast<unsigned char*>(cur) + 0x10);
          if (cf < our) break;
          auto* prev = reinterpret_cast<std::uint32_t*>(
              static_cast<std::uintptr_t>(cur[2]));
          if (!prev || prev[2] == 0) {
            cur = nullptr;
            break;
          }
          cur = prev;
        }
        if (!cur)
          type53_spatial_insert_head(list, max_link);  // PE @ 0x4B32F3
        else
          type53_spatial_splice_after(max_link, cur);
      }
    }

    // PE @ 0x4B3366: *(mgr+0xC4+axis*8) += 2. Axis2 @ +0xD4 OOB on
    // host mgr[0xD4] — side counts[]; mirror 0/1 into mgr.
    auto& spat = g_type53_spatial[mgr];
    spat.counts[axis] += 2;
    if (axis < 2) mgr[49 + axis * 2] += 2;
  }
}

// PE slot neighbor dllist @ +0xA8 sentinel / +0xB0 tail / +0xB4 count.
static void type53_neighbor_list_lazy(std::uint32_t* slot) {
  if (!slot || slot[44] != 0) return;
  slot[42] = 0;
  slot[43] = 0;
  slot[44] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(&slot[42]));
}

// PE Type53_linkNearbySlots @ 0x004B36B0: walk list B; same node[+0xC8]
// + XZ AABB (center +0x84/+0x8C, half +0x90/+0x98) → malloc(20) link into
// other[+0xA8]. W22C: ctor+dllist+flag&~8; W23C: promote @ 0x4B3AC0.
// Twin: Type53_linkNearbyFromListA @ 0x4B33A0 (list A → a2 neighbors;
// ensureActiveSlot @ 0x4B3C40). OOS: ResourceEngine sub_537240 @ 0x537240.
// W24C: no PE write of half-extents on this path — keep degenerate skip.
static void type53_link_nearby_slots(std::uint32_t* mgr,
                                    std::uint32_t* slot) {
  if (!mgr || !slot) return;
  auto* owner = reinterpret_cast<unsigned char*>(
      static_cast<std::uintptr_t>(slot[6]));  // +0x18
  if (!owner) return;

  auto* list_b = mgr + 4;  // +0x10 — PE @ 0x4B36CD
  if (type53_sim_list_empty(list_b)) return;
  std::uint32_t* cur = type53_sim_list_head(list_b);
  int walk_guard = 0;
  const float ox = *reinterpret_cast<float*>(owner + 0x84);
  const float oz = *reinterpret_cast<float*>(owner + 0x8C);
  const float hx = *reinterpret_cast<float*>(owner + 0x90);
  const float hz = *reinterpret_cast<float*>(owner + 0x98);
  const std::uint32_t tag =
      *reinterpret_cast<std::uint32_t*>(owner + 0xC8);

  while (cur) {
    // W24: host list-B can cycle (Valocity hang); PE walk is acyclic.
    if (++walk_guard > 4096) break;
    // PE walk: next = cur[1] if next[1]!=0 else null — @ 0x4B36EF
    std::uint32_t* next = nullptr;
    if (cur[1] != 0) {
      auto* cand = reinterpret_cast<std::uint32_t*>(
          static_cast<std::uintptr_t>(cur[1]));
      if (cand && cand != cur && cand[1] != 0) next = cand;
    }
    if (cur[5] != 0 && cur != slot) {
      auto* other = reinterpret_cast<unsigned char*>(
          static_cast<std::uintptr_t>(cur[6]));
        if (other &&
          *reinterpret_cast<std::uint32_t*>(other + 0xC8) == tag) {
        const float cx = *reinterpret_cast<float*>(other + 0x84);
        const float cz = *reinterpret_cast<float*>(other + 0x8C);
        const float ohx = *reinterpret_cast<float*>(other + 0x90);
        const float ohz = *reinterpret_cast<float*>(other + 0x98);
        // W24/W24C: host owner AABB often still 0 (HostResNode pad; fill is
        // EXTP/unionChildAabb in Resources, not insertSpatialBounds). PE
        // halfsum>=|Δ| with hx=ohx=0 matches every peer → O(N²) hang.
        // Keep skip until Resources writes real half-extents on this node.
        if (hx + ohx <= 0.f && hz + ohz <= 0.f) {
          cur = next;
          continue;
        }
        // PE @ 0x4B373E..0x4B3786: halfsum >= fabs(delta)
        if (hx + ohx >= std::fabs(cx - ox) &&
            hz + ohz >= std::fabs(cz - oz)) {
          // PE @ 0x4B378C Engine_malloc(20)
          auto* link =
              static_cast<std::uint32_t*>(std::calloc(1, 20));
          if (link) {
            g_type53_owned_neighbor_links.push_back(link);
            // Engine_SimCallbackNode_ctor @ 0x429130 ([1]=[2]=0);
            // PE then *link = Type53_NeighborLink_vtbl @ 0x5F14F8 —
            // host skips PE vtbl (no invent).
            link[1] = 0;
            link[2] = 0;
            link[3] = static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(slot));  // +0xC
            ++slot[46];                                   // +0xB8
            link[4] = mgr[52];  // gen @ mgr+0xD0 — PE @ 0x4B379F

            // Splice into other neighbor list — PE @ 0x4B37C8
            type53_neighbor_list_lazy(cur);
            auto* tail = reinterpret_cast<std::uint32_t*>(
                static_cast<std::uintptr_t>(cur[44]));
            if (!tail) tail = &cur[42];
            tail[1] = static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(link));
            link[2] = static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(tail));
            link[1] = static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(&cur[42]));
            cur[44] = static_cast<std::uint32_t>(
                reinterpret_cast<std::uintptr_t>(link));
            ++cur[45];  // +0xB4

            // PE @ 0x4B37EC: bit8+bit16 → promote @ 0x4B3AC0; else clear bit8
            const std::uint32_t fl = slot[7];
            if ((fl & 8u) != 0) {
              if ((fl & 0x10u) != 0)
                type53_promote_slot_on_link(mgr, slot);
              else
                slot[7] = fl & 0xFFFFFFF7u;
            }
            // ResourceEngine sub_537240 @ 0x537240 OOS
          }
        }
      }
    }
    cur = next;
  }
}

}  // namespace

std::mutex g_gr_mu;
std::unordered_map<InvObject*, GameRefState> g_refs;
InvObject* g_vehicle_types = nullptr;
std::unordered_map<InvObject*, GroundTrafficState> g_grounds;
GameRefState& ref(InvObject* self) { return g_refs[self]; }
GroundTrafficState& ground(InvObject* self) { return g_grounds[self]; }

void* type53_ensure_handle_slot(void* mgr_raw, void* child_handle,
                                int32_t a3) {
  if (!mgr_raw || !child_handle) return nullptr;
  auto* hb = reinterpret_cast<unsigned char*>(child_handle);
  // PE: *(handle+0xC) node; type at node+0x4C must be 1.
  void* node = *reinterpret_cast<void**>(hb + 0xC);
  if (!node) return nullptr;
  auto* nb = reinterpret_cast<unsigned char*>(node);
  if (*reinterpret_cast<int32_t*>(nb + 0x4C) != 1) return nullptr;
  // PE @ 0x4B3F0F: (node+0x64)&1 && node[+0xC8] != mgr[+8] → null.
  if ((nb[0x64] & 1) != 0 &&
      *reinterpret_cast<std::uint32_t*>(nb + 0xC8) !=
          *reinterpret_cast<std::uint32_t*>(reinterpret_cast<unsigned char*>(
                                               mgr_raw) +
                                           8))
    return nullptr;

  auto* mgr = reinterpret_cast<std::uint32_t*>(mgr_raw);
  type53_mgr_lazy_init(mgr);

  const std::uint32_t h8 = *reinterpret_cast<std::uint32_t*>(hb + 8);
  // Host uniqueness: PE key is handle[+8]; alive=1 collides — prefer handle addr.
  const std::uint32_t key =
      (h8 != 0 && h8 != 1)
          ? h8
          : static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(
                child_handle));

  void* a3p = reinterpret_cast<void*>(
      static_cast<std::uintptr_t>(static_cast<std::uint32_t>(a3)));

  std::uint32_t* result = nullptr;
  Type53FoundList found = Type53FoundList::None;
  // List A @ mgr+0x2C (this+11) — PE @ 0x4B3F30
  result = type53_slot_walk(type53_sim_list_head(mgr + 11), key, node);
  if (result) {
    found = Type53FoundList::A;
  } else {
    // List B @ mgr+0x10 (this+4) — PE @ 0x4B3FE2
    result = type53_slot_walk(type53_sim_list_head(mgr + 4), key, node);
    if (result) {
      found = Type53FoundList::B;
    } else {
      // List C: start at sentinel mgr[20] if its next != 0 — PE @ 0x4B4010
      auto* sent = reinterpret_cast<std::uint32_t*>(
          static_cast<std::uintptr_t>(mgr[20]));
      if (sent && sent[1] != 0)
        result = type53_slot_walk(sent, key, node);
      if (result) found = Type53FoundList::C;
    }
  }
  if (result) {
    // List A/C retarget; list B returns as-is (PE no post-walk).
    if (found == Type53FoundList::A || found == Type53FoundList::C)
      type53_slot_retarget_a3(result, a3p);
    if (found == Type53FoundList::C) type53_list_c_promote(mgr, result);
    return result;
  }

  // LABEL_38: Engine_malloc(200) @ 0x4B403A
  auto* slot = static_cast<std::uint32_t*>(std::calloc(1, 200));
  if (!slot) return nullptr;
  g_type53_owned_slots.push_back(slot);
  // Minimal ctor (vtbl/sub_4B5B20 loops omitted — no invent PE vtables).
  // ResHandle_Rebind stand-in @ v9+3: owner node at +0xC of ResHandle =
  // slot[+0x18]; key at slot[+0x14] (= Rebind copies node[+0x50]).
  *reinterpret_cast<std::uint32_t*>(nb + 0x50) = key;
  slot[5] = key;                                      // +0x14
  slot[6] = static_cast<std::uint32_t>(               // +0x18 owner node
      reinterpret_cast<std::uintptr_t>(node));
  slot[7] = 0;
  slot[11] = 4;
  slot[16] = 5;
  slot[21] = 6;
  slot[26] = 8;
  slot[31] = 9;
  slot[36] = 10;
  slot[45] = slot[46] = slot[47] = slot[49] = 0;
  if (a3p) {
    auto* a3b = reinterpret_cast<unsigned char*>(a3p);
    slot[49] = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(a3b));
    ++(*reinterpret_cast<std::uint32_t*>(a3b + 188));
    slot[7] |= *reinterpret_cast<std::uint32_t*>(a3b + 28) & 0x20u;
  }
  slot[48] = mgr[52] ? (mgr[52] - 1) : 0;
  // Insert at mgr insert-list: sentinel &mgr[15], tail mgr[17] @ 0x4B420C.
  auto* old_tail = reinterpret_cast<std::uint32_t*>(
      static_cast<std::uintptr_t>(mgr[17]));
  if (!old_tail) old_tail = &mgr[15];
  old_tail[1] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(slot));  // old_tail.next = slot
  slot[2] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(old_tail));  // slot.prev
  slot[1] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(&mgr[15]));  // slot.next = sentinel
  mgr[17] = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(slot));
  ++mgr[47];
  // PE @ 0x4B422F Type53_insertSpatialBounds(mgr,slot,a3) — consume AABB.
  // @ 0x4B4237 Type53_linkNearbySlots(mgr,slot) — W22C+W23C promote.
  // W24C: no PE owner AABB write before these calls (a3==0 on setParent).
  // EnsureActiveSlot@4B3C40 also insert→linkNearbyFromListA→linkNearby.
  type53_insert_spatial_bounds(mgr, slot, a3p);
  type53_link_nearby_slots(mgr, slot);
  return slot;
}



// PE GameRef_handleAttachUnderParent @ 0x00429C80: splice *(block+0xC) under
// parent_inner +0x30/+0x38. mid_block is Native.ptr (host) or phys block.
void gameref_handle_attach_under_parent(void* mid_block, InvObject* parent) {
  if (!mid_block || !parent) return;
  auto* block = reinterpret_cast<unsigned char*>(mid_block);
  void* child_inner = *reinterpret_cast<void**>(block + 0xC);
  void* parent_inner = native_ptr_node(parent);
  HostNativeHandle* parent_h = native_ptr_get(parent);
  if (!child_inner || !parent_inner || !parent_h) return;
  if (*reinterpret_cast<int32_t*>(reinterpret_cast<unsigned char*>(parent_h) +
                                  8) == 0)
    return;
  auto* ci = reinterpret_cast<unsigned char*>(child_inner);
  auto* pi = reinterpret_cast<unsigned char*>(parent_inner);
  // Already under this parent: PE compares parent handle to *(+0x14)+0x50.
  void* cur = *reinterpret_cast<void**>(ci + 0x14);
  if (cur) {
    void* back = *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(cur) +
                                           0x50);
    if (back == parent_h) return;
  }
  // Unlink if both next/prev set (PE inline, same as dllist).
  void* next = *reinterpret_cast<void**>(ci + 0x4);
  void* prev = *reinterpret_cast<void**>(ci + 0x8);
  if (next && prev) {
    *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(next) + 0x8) =
        prev;
    *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(prev) + 0x4) =
        next;
  }
  *reinterpret_cast<void**>(ci + 0x4) = nullptr;
  *reinterpret_cast<void**>(ci + 0x8) = nullptr;
  *reinterpret_cast<void**>(ci + 0x14) = parent_inner;
  *reinterpret_cast<void**>(pi + 0x50) = parent_h;  // inner back-ref
  void* sent = pi + 0x30;
  void** tailp = reinterpret_cast<void**>(pi + 0x38);
  if (*tailp == nullptr) *tailp = sent;  // empty → sentinel
  void* old_tail = *tailp;
  *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(old_tail) + 0x4) =
      child_inner;
  *reinterpret_cast<void**>(ci + 0x4) = sent;
  *reinterpret_cast<void**>(ci + 0x8) = old_tail;
  *tailp = child_inner;
}

// PE GameRef_setParent_inner @ 0x0048ABA0 on a handle-shaped this.
// CameraCtrl @ 0x4385A2: this = mid_block+0x30. Types 2/3 → dllist via
// handleAttach shape (handle+0xC = node). Type 1 nest attach not recursed.
void gameref_set_parent_inner_handle(void* handle, InvObject* parent) {
  if (!handle || !parent) return;
  auto* hb = reinterpret_cast<unsigned char*>(handle);
  void* node = *reinterpret_cast<void**>(hb + 0xC);
  if (!node) return;
  const int32_t ty =
      *reinterpret_cast<int32_t*>(reinterpret_cast<unsigned char*>(node) + 0x4C);
  if (ty == 2 || ty == 3) {
    gameref_handle_attach_under_parent(handle, parent);
  }
  // type 1 / other: PE may nest further — host stops (no invent).
}

void gameref_list_unlink(InvObject* child) {
  if (!child) return;
  auto it = g_refs.find(child);
  if (it == g_refs.end()) return;
  GameRefState& cs = it->second;
  InvObject* p = cs.parent;
  InvObject* next = cs.sib_next;
  InvObject* prev = cs.sib_prev;
  // PE GameRef_dllist_unlink @ 0x004A5D00: if prev!=0 && next!=0:
  //   [next+8]=prev; [prev+4]=next; then clear this+4/+8.
  // Linked nodes always have both (sibling or &sentinel). Writing
  // sentinel+8 / sentinel+4 updates parent+0x38 / +0x34 (tail/head).
  if (p && next && prev) {
    const bool next_sent = (next == p);
    const bool prev_sent = (prev == p);
    auto pit = g_refs.find(p);
    if (!next_sent) {
      auto nit = g_refs.find(next);
      if (nit != g_refs.end()) nit->second.sib_prev = prev;
    } else if (pit != g_refs.end()) {
      pit->second.child_list_tail = prev_sent ? nullptr : prev;
    }
    if (!prev_sent) {
      auto qit = g_refs.find(prev);
      if (qit != g_refs.end()) qit->second.sib_next = next;
    } else if (pit != g_refs.end()) {
      pit->second.child_list_head = next_sent ? nullptr : next;
    }
  }
  cs.parent = nullptr;
  cs.sib_next = nullptr;
  cs.sib_prev = nullptr;
}

// Caller must hold g_gr_mu. Same-parent → no-op (PE early return 1 @ 0x48AC57).
void gameref_list_link(InvObject* child, InvObject* parent) {
  if (!child || !parent) return;
  auto& cs = ref(child);
  if (cs.parent == parent) return;
  gameref_list_unlink(child);
  auto& ps = ref(parent);
  InvObject* old_tail = ps.child_list_tail;
  // PE @ 0x48AC6A-0x48AC96: zero +0x14 → GameRef_dllist_unlink → restore
  // +0x14; [old_tail+4]=child; child+8=old_tail; child+4=&parent+0x30;
  // parent+0x38=child. Empty list: old_tail was sentinel → first.prev=sent.
  cs.parent = parent;
  cs.sib_next = parent;  // → &parent+0x30
  if (old_tail) {
    cs.sib_prev = old_tail;
    auto tit = g_refs.find(old_tail);
    if (tit != g_refs.end()) tit->second.sib_next = child;
  } else {
    cs.sib_prev = parent;  // first: prev also → sentinel
    ps.child_list_head = child;
  }
  ps.child_list_tail = child;
  // PE @ 0x48AC19: parent WT node *(+0xCC)+0x54 |= 0x40000 (dirty).
  constexpr int32_t kWtParentDirty = 0x40000;
  ps.flags |= kWtParentDirty;
}

// PE GameRef_worldTreeLink @ 0x00544F40 (thiscall ecx=inner, a2=handle):
// WT node at inner+0xC0 into list at *(handle+0xC)+0x48. setFlags: owner=self.
// Hold g_gr_mu.
void gameref_world_tree_link(InvObject* self) {
  if (!self) return;
  auto& r = ref(self);
  // setFlags path: a2=handle → owner=*(handle+0xC)=self.
  InvObject* owner = self;
  // PE: if *(inner+0xCC) == owner already linked → skip.
  if (r.wt_owner == owner) return;
  // Unlink from prior owner list (PE 0x544F5A-0x544F83).
  if (r.wt_owner) {
    auto oit = g_refs.find(r.wt_owner);
    if (oit != g_refs.end()) {
      GameRefState& os = oit->second;
      // PE: if node.next≠0 → next.prev=prev; else owner+0x48=prev (head).
      if (r.wt_next) {
        auto nit = g_refs.find(r.wt_next);
        if (nit != g_refs.end()) nit->second.wt_prev = r.wt_prev;
      } else if (os.wt_list_head == self) {
        os.wt_list_head = r.wt_prev;
      }
      if (r.wt_prev) {
        auto pit = g_refs.find(r.wt_prev);
        if (pit != g_refs.end()) pit->second.wt_next = r.wt_next;
      }
    }
    r.wt_next = nullptr;
    r.wt_prev = nullptr;
    r.wt_owner = nullptr;
    r.wt_aux = nullptr;
    r.worldtree_root = false;
  }
  // Insert at head (PE 0x544F8A-0x544FAF): next=0, prev=old_head,
  // old_head.next=new, owner+0x48=new, node+0xCC=owner,
  // node+0xC8=*(owner+0x50) (host: wt_list_aux).
  auto& os = ref(owner);
  InvObject* old_head = os.wt_list_head;
  if (old_head) {
    auto hit = g_refs.find(old_head);
    if (hit != g_refs.end()) hit->second.wt_next = self;
  }
  r.wt_next = nullptr;
  r.wt_prev = old_head;
  os.wt_list_head = self;
  r.wt_owner = owner;
  r.wt_aux = os.wt_list_aux;  // PE *(owner+0x50)
  r.worldtree_root = true;
}


// ground_sync_fields → GroundRef_traffic.cpp

std::string script_fqn_for_entry(const RpakEntry* e) {
  if (!e || e->name.empty()) return {};
  // Racer VT scripts live in package java.game.cars (Baiern_VT, …).
  if (e->name.size() >= 3 &&
      e->name.compare(e->name.size() - 3, 3, "_VT") == 0)
    return std::string("java.game.cars.") + e->name;
  return std::string("java.game.") + e->name;
}

InvObject* make_vt_host(const char* fqn) {
  InvObject* o = tree_host_new(fqn);
  tree_field_set_obj(o, "vtdarr", tree_vector_new());
  tree_field_set_obj(o, "preferredColorIndexes", tree_vector_new());
  tree_field_set_float(o, "prevalence", 1.f);
  tree_field_set_int(o, "vehicleSetMask", 0);
  resref_ensure(o);
  return o;
}

void bind_gameref(InvObject* o, InvObject* parent, const std::string& fqn,
                  const char* alias) {
  auto& rs = ref(o);
  rs.empty = false;
  rs.script = o;
  rs.script_class = fqn;
  rs.script_alias = alias ? alias : "";
  // Caller holds g_gr_mu (create / traffic smoke). Keep +0x30/+0x38 stand-in.
  if (parent)
    gameref_list_link(o, parent);
  else
    gameref_list_unlink(o);
}

// Stock create params: "px,py,pz,oy,op,or" (metres + YPR radians; spaces OK).
bool parse_instance_params(const char* s, float out[6]) {
  if (!s || !s[0] || !out) return false;
  int n = 0;
  const char* p = s;
  while (*p && n < 6) {
    while (*p == ' ' || *p == '\t' || *p == ',') ++p;
    if (!*p) break;
    char* end = nullptr;
    const float v = std::strtof(p, &end);
    if (end == p) return false;
    out[n++] = v;
    p = end;
  }
  return n == 6;
}

void apply_instance_pose(InvObject* o, const float pose[6]) {
  if (!o || !pose) return;
  java_util_resource_GameRef_setMatrix(
      o, vec3_new(pose[0], pose[1], pose[2]),
      ypr_new(pose[3], pose[4], pose[5]));
  InvObject* parent = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    parent = ref(o).parent;
  }
  if (parent) java_util_resource_GameRef_setParent(o, parent);
}


InvObject* gameref_new() {
  auto* o = reinterpret_cast<InvObject*>(new InvString{nullptr});
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    g_refs[o] = GameRefState{};
  }
  resref_ensure(o);
  return o;
}

// W10A — PE *[phys+0xDC] walk export (forceUpdate_apply @ 0x4485C9).
void* gameref_phys_dc_head(InvObject* gameref) {
  if (!gameref) return nullptr;
  std::lock_guard<std::mutex> lock(g_gr_mu);
  auto it = g_refs.find(gameref);
  if (it == g_refs.end()) return nullptr;
  auto& st = it->second;
  if (!st.phys_dc_sent) return nullptr;
  // PE: head=*[phys+0xDC]; start=*(head+4)!=0 ? head : 0.
  // Host circular sent→first; expose first live node (part!=null).
  auto* n = st.phys_dc_sent->next;
  if (!n || !n->part) return nullptr;
  return n;
}

void* gameref_phys_dc_next(void* node) {
  if (!node) return nullptr;
  auto* n = static_cast<GameRefState::PePhysDcNode*>(node);
  auto* nxt = n->next;
  // PE stop: next==0 || *(next+4)==0 (sentinel next stays 0 after insert).
  // Host sentinel: part==nullptr.
  if (!nxt || !nxt->part) return nullptr;
  return nxt;
}

int32_t gameref_phys_dc_part_id(void* node) {
  if (!node) return 0;
  return static_cast<GameRefState::PePhysDcNode*>(node)->match_key;
}

InvObject* gameref_phys_dc_child(void* node) {
  if (!node) return nullptr;
  return static_cast<GameRefState::PePhysDcNode*>(node)->part;
}

int32_t gameref_phys_dc_count(InvObject* gameref) {
  if (!gameref) return 0;
  std::lock_guard<std::mutex> lock(g_gr_mu);
  auto it = g_refs.find(gameref);
  if (it == g_refs.end()) return 0;
  return static_cast<int32_t>(it->second.phys_dc_nodes.size());
}

// PE GameRef_findCamIndexByMatchId @ 0x448D10 (W12A).
// this+0x177C = count; walk chassis_blob+0x128+idx*0x4A8 == match_id;
// wrap with step; full lap → -1. Host: GameRefState.cam_rows[].match_id;
// count = chassis_cam_count (+0x177C stand-in).
static int32_t find_cam_index_by_match_id_unlocked(GameRefState& r, int32_t count,
                                                  int32_t match_id,
                                                  int32_t start_idx,
                                                  int32_t step) {
  if (count <= 0) return -1;
  if (count > GameRefState::kCamTableMax) count = GameRefState::kCamTableMax;
  int32_t result = start_idx;
  if (start_idx < count) {
    if (start_idx < 0) result = count - 1;
  } else {
    result = 0;
  }
  const int32_t begin = result;
  for (;;) {
    if (r.cam_rows[static_cast<size_t>(result)].match_id == match_id)
      return result;
    result += step;
    if (result < count) {
      if (result < 0) result = count - 1;
    } else {
      result = 0;
    }
    if (result == begin) return -1;
  }
}

static int32_t cam_table_count_for(InvObject* self, InvObject* chassis) {
  InvObject* ch = chassis ? chassis : self;
  if (!ch && !self) return 0;
  int32_t n = ch ? chassis_cam_count(ch) : 0;
  if (n <= 0 && ch) n = tree_field_get_int(ch, "camera_count");
  if (n <= 0 && self) n = tree_field_get_int(self, "camera_count");
  // PE Chassis_allocCamBlob always constructs 4 cam entries (@ 0x44a295);
  // +0x177C live count inits 0 @ 0x446948. Host: walk all 4 when live
  // count unset so findCam(0) free-slot (activate @ 0x4591E0) works.
  if (n <= 0) n = GameRefState::kCamTableMax;
  if (n > GameRefState::kCamTableMax) n = GameRefState::kCamTableMax;
  return n;
}

static void cam_rows_tree_sync(InvObject* self, GameRefState& r, int32_t count) {
  if (!self) return;
  tree_field_set_int(self, "cam_table_count", count);
  for (int32_t i = 0; i < GameRefState::kCamTableMax; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "cam_%d_match_id", i);
    tree_field_set_int(self, key, r.cam_rows[static_cast<size_t>(i)].match_id);
    std::snprintf(key, sizeof(key), "cam_%d_osd_bound", i);
    tree_field_set_int(self, key,
                       r.cam_rows[static_cast<size_t>(i)].osd_bound_id);
  }
}

int32_t gameref_find_cam_index_by_match_id(InvObject* gameref, int32_t match_id,
                                           int32_t start_idx, int32_t step) {
  if (!gameref) return -1;
  InvObject* chassis = tree_field_get_obj(gameref, "chassis");
  if (!chassis) chassis = gameref;
  chassis_cam_tables_ensure(chassis);
  const int32_t count = cam_table_count_for(gameref, chassis);
  std::lock_guard<std::mutex> lock(g_gr_mu);
  return find_cam_index_by_match_id_unlocked(ref(gameref), count, match_id,
                                             start_idx, step);
}

void gameref_on_res_bound(InvObject* self) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_gr_mu);
  auto it = g_refs.find(self);
  if (it != g_refs.end()) it->second.empty = false;
}

void gameref_on_destroy(InvObject* self) {
  if (!self) return;
  // Detach install-graph edge (Part.addPart failure path never installed).
  if (InvObject* parent = tree_field_get_obj(self, "part_parent")) {
    const int32_t slot_id = tree_field_get_int(self, "part_parent_slot");
    if (slot_id && part_on_slot(parent, slot_id) == self) {
      InvObject* slots = tree_field_get_obj(parent, "part_slots");
      const int32_t n = slots ? tree_vector_size(slots) : 0;
      for (int32_t i = 0; i < n; ++i) {
        InvObject* s = tree_vector_element_at(slots, i);
        if (!s || tree_field_get_int(s, "slot_id") != slot_id) continue;
        tree_field_set_obj(s, "child", nullptr);
        tree_field_set_int(s, "child_slot_id", 0);
        if (tree_field_get_obj(s, "visual") == self)
          tree_field_set_obj(s, "visual", nullptr);
        break;
      }
    }
  }
  tree_field_set_obj(self, "part_parent", nullptr);
  tree_field_set_int(self, "part_parent_slot", 0);
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    // Drop from parent +0x30/+0x38 dllist before erase (PE unlink).
    gameref_list_unlink(self);
    // Orphan children via +0x38 tail walk (PE ResHandle teardown elsewhere).
    auto it = g_refs.find(self);
    if (it != g_refs.end()) {
      InvObject* c = it->second.child_list_tail;
      while (c && c != self) {
        auto cit = g_refs.find(c);
        if (cit == g_refs.end()) break;
        InvObject* prev = cit->second.sib_prev;
        cit->second.parent = nullptr;
        cit->second.sib_next = nullptr;
        cit->second.sib_prev = nullptr;
        // PE sentinel stand-in: prev==self means end of +0x38 walk.
        c = (prev == self) ? nullptr : prev;
      }
      it->second.child_list_tail = nullptr;
      it->second.child_list_head = nullptr;
    }
    g_refs.erase(self);
  }
}


// → GameRef_core.cpp (GameRef_core create..clearFlags)


// → GameRef_core.cpp (GameRef_core soft+fwd)


// → GameRef_core.cpp (GameRef_core getPos..getInfo)
void java_util_resource_GameRef_queueEvent(InvObject* self, InvObject* ro,
                                          int32_t type, InvObject* param) {
  // PE @ 0x0047DA30 size 0x9c. JNI (LResourceRef;ILjava/lang/String;)V
  // (Natives_RegisterAll @ 0x004896A4). JVM_UnboxArg cdecl 5 stack
  // (Hex-Rays 2-arg WRONG): CallInfo*, &this, &ro, &type, &param.
  // handle = JVM_vm_get_int_field(this, dword_62E008). handle==0 →
  // Mighty ERROR ("!"+"Mighty ERROR"). ro/param NOT tested; type NOT
  // filtered (EVENT_COMMAND=0x10 is Java command() only). Else stdcall
  // Engine_queueEvent(handle, ro, type, param, 0) @ 0x00426800 →
  // Engine_queueEvent_dispatch @ 0x004265C0 (was sub_4265C0): payload
  // vtbl+0x38 = sub_458C00 size 0x38fe. DO NOT RENAME / FULL-PORT
  // sub_458C00. EVENT_COMMAND @ 0x458C3C: scanf "%s" then strcmp chain
  // (sub_5D7190) — render/hide/…/stop/start/suspend/wakeup/reset/…
  // /sethorn/setvel/team/…. Host string parse = strcmp subset only.
  (void)ro;
  if (!self) return;

  // W9B — PE GameRef_voidEvent_parse EVENT type 0x80 @ 0x45C15A →
  // jpt_45C300; case 2 @ 0x45C323..0x45C3C9: clear +0x70 bit 0x40000;
  // gates +0x1FC0==0, +0x1FCC!=0, +0x1FD0 && *[+0x4C]==8 (GII_CONTROL);
  // getWTRoot → LoadGameInit("1,0.25","bot") @ 0x45C3BD →
  // void_event_loadgameinit_rebind @ 0x45C3C9; then "controllable %d" +
  // "AI_GoToTrafficSlow" + Engine_addTimer(0x80000080). Host: subtype
  // from param int (engine a5); soft gates (no PE blob +0x1FC*).
  constexpr int32_t kEventType80 = 0x80;
  if (type == kEventType80) {
    int32_t sub = 0;
    if (param) {
      const char* ps = string_cstr(param);
      if (ps && ps[0]) {
        char* end = nullptr;
        sub = static_cast<int32_t>(std::strtol(ps, &end, 10));
      }
    }
    if (sub == 2) {
      {
        std::lock_guard<std::mutex> lock(g_gr_mu);
        ref(self).flags &= ~0x40000;
      }
      // PE stack ResHandle ch18 @ 0x45C34x; wtroot RH from getWTRootNode.
      alignas(4) uint8_t wt_rh[16]{};
      alignas(4) uint8_t stack_rh[16]{};
      int32_t wt_key = java_util_resource_ResourceRef_id(self);
      if (wt_key == 0) {
        if (InvObject* wt = java_util_resource_ResourceRef_getWTRoot(self))
          wt_key = java_util_resource_ResourceRef_id(wt);
      }
      *reinterpret_cast<int32_t*>(wt_rh + 8) = wt_key;
      if (wt_key != 0) {
        // PE @ 0x45C3BD Engine_LoadGameInit(wt, g_RH@63C680, "1,0.25", "bot")
        // → GI; Rebind @ 0x45C3C9 via void_event_loadgameinit_rebind.
        void* gi =
            engine_load_game_init(wt_rh, nullptr, "1,0.25", "bot");
        void_event_loadgameinit_rebind(stack_rh, gi);
        const int32_t gikey =
            gi ? *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(gi) + 0x50)
               : 0;
        tree_field_set_int(self, "loadgameinit_gi_key", gikey);
        tree_field_set_int(self, "cmd_event_0x80_case2",
                           tree_field_get_int(self, "cmd_event_0x80_case2") +
                               1);
        // Downstream PE @ 0x45C3FD — host counters only (timer OOS).
        tree_field_set_int(self, "cmd_controllable",
                           tree_field_get_int(self, "cmd_controllable") + 1);
        tree_field_set_int(self, "cmd_AI_GoToTrafficSlow",
                           tree_field_get_int(self, "cmd_AI_GoToTrafficSlow") +
                               1);
      }
    }
    return;
  }

  // GameRef.command → queueEvent(null, EVENT_COMMAND=0x10, param).
  constexpr int32_t kEventCommand = 0x10;
  if (type != kEventCommand) return;  // other types: PE vtbl; host no-op
  if (!param) return;
  const char* raw = string_cstr(param);
  if (!raw || !raw[0]) return;
  // PE: first token via "%s" into var_80, then exact strcmp (not prefix).
  char tok[96];
  {
    const char* s = raw;
    while (*s == ' ' || *s == '\t') ++s;
    size_t n = 0;
    while (s[n] && s[n] != ' ' && s[n] != '\t' && n + 1 < sizeof(tok)) {
      tok[n] = s[n];
      ++n;
    }
    tok[n] = '\0';
    if (n == 0) return;
  }
  const char* s = raw;
  while (*s == ' ' || *s == '\t') ++s;
  // Phase 2.94 — Bot.pressHorn / City challenge: "sethorn 0|1".
  // PE @ 0x45A589: strcmp "sethorn"; scanf "%*s %d"; store (v % 4) at
  // obj+0x1DCC (and ecx,80000003h signed-mod). getHorn reads that slot.
  if (std::strcmp(tok, "sethorn") == 0) {
    s += 7;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s) return;
    // PE signed modulo 4 (MSVC and 80000003h pattern).
    int32_t slot = static_cast<int32_t>(v) % 4;
    if (slot < 0) slot += 4;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      ref(self).horn = slot;
    }
    // TREE mirror for java_game_Vehicle_getHorn in Cars.cpp (PE +0x1DCC
    // slot stand-in; setnz read). Keep GameRefState.horn for legacy.
    tree_field_set_int(self, "horn", slot);
    return;
  }
  // Phase 2.101 — Vehicle.wakeUp / CarMarket start|stop|reset.
  // PE strcmp exact: "wakeup" @ 0x459ED1 → sub_4A7990; "start" @
  // 0x459E02 clears flag bits + physics vfuncs; "stop" @ 0x459B1E ors
  // +0x70 bit2 + halt; "reset" @ 0x459F2D clears pose vel slots.
  if (std::strcmp(tok, "wakeup") == 0) {
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      ref(self).suspended = 0;
    }
    physics_set_asleep(self, 0);
    if (InvObject* ch = tree_field_get_obj(self, "chassis"))
      physics_set_asleep(ch, 0);
    tree_field_set_int(self, "awake", 1);
    tree_field_set_int(self, "suspended", 0);
    return;
  }
  if (std::strcmp(tok, "start") == 0) {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    ref(self).drive_held = 0;
    tree_field_set_int(self, "drive_held", 0);
    return;
  }
  if (std::strcmp(tok, "stop") == 0) {
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      ref(self).drive_held = 1;
    }
    tree_field_set_int(self, "drive_held", 1);
    physics_set_velocity(self, 0.f, 0.f, 0.f);
    physics_set_ang_vel(self, 0.f, 0.f, 0.f);
    return;
  }
  if (std::strcmp(tok, "reset") == 0) {
    physics_set_velocity(self, 0.f, 0.f, 0.f);
    physics_set_ang_vel(self, 0.f, 0.f, 0.f);
    physics_set_asleep(self, 0);
    tree_field_set_int(self, "reset_count",
                       tree_field_get_int(self, "reset_count") + 1);
    return;
  }
  // PE strcmp @ 0x459E75 "suspend" / 0x459ED1 "wakeup" twin — asleep flag.
  if (std::strcmp(tok, "suspend") == 0) {
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      ref(self).suspended = 1;
    }
    physics_set_asleep(self, 1);
    if (InvObject* ch = tree_field_get_obj(self, "chassis"))
      physics_set_asleep(ch, 1);
    tree_field_set_int(self, "awake", 0);
    tree_field_set_int(self, "suspended", 1);
    return;
  }
  // PE @ 0x458EB7 "hide" — visibility gate stand-in.
  if (std::strcmp(tok, "hide") == 0) {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    ref(self).hidden = 1;
    tree_field_set_int(self, "hidden", 1);
    return;
  }
  // PE @ 0x458C51 "render": strcmp then scanf "%*s %d %d %d" (defaults 0).
  // IDA push order @ 0x458C67..: &arg_C=vp, &var_20=con, &arg_8=camNum.
  // Track: cameraTarget.command("render "+vp+" "+con+" "+cameraNum).
  // findCam(con) @ 0x448D10 → cam idx (match @ blob+0x128); gate arg_C!=0
  // @ 0x458CC0; Bind(cam+0x44, vp, RESOURCE_VIEWPORT=0x12) @ 0x458d42 →
  // LookupById @ 0x536820; clamp camNum @ 0x458D5C; Rebind(cam+0x34, slot)
  // @ 0x458D88 via +0x17A8/+0x13F4.
  if (std::strcmp(tok, "render") == 0) {
    s += 6;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long vp_l = *s ? std::strtol(s, &end, 10) : 0;
    if (*s && end == s) return;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long con_l = *s ? std::strtol(s, &end, 10) : 0;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long cam_l = *s ? std::strtol(s, &end, 10) : 0;
    const int32_t vp_id = static_cast<int32_t>(vp_l);
    const int32_t con_id = static_cast<int32_t>(con_l);
    int32_t cam_num = static_cast<int32_t>(cam_l);
    InvObject* chassis = tree_field_get_obj(self, "chassis");
    if (!chassis) chassis = self;
    chassis_cam_tables_ensure(chassis);
    // PE clamp @ 0x458D5C: camNum < *[chassis_blob+0xA54] else max-1.
    // Host: TREE camera_count, else chassis_cam_count (+0x177C stand-in).
    int32_t cam_cap = tree_field_get_int(self, "camera_count");
    if (cam_cap <= 0) cam_cap = chassis_cam_count(chassis);
    if (cam_cap > 0 && cam_num >= cam_cap) cam_num = cam_cap - 1;
    if (cam_num < 0) cam_num = 0;
    InvObject* vp = vp_id != 0 ? resref_find_by_id(vp_id) : nullptr;
    InvObject* con = con_id != 0 ? resref_find_by_id(con_id) : nullptr;
    // PE @ 0x458C92..0x458C99: findCam(con) start=0 step=1.
    const int32_t cam_count = cam_table_count_for(self, chassis);
    int32_t cam_idx = -1;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      cam_idx = find_cam_index_by_match_id_unlocked(ref(self), cam_count,
                                                    con_id, 0, 1);
    }
    // PE jl @ 0x458CA2 → abort; host keep prior soft 0 only when count==0.
    if (cam_idx < 0) {
      if (cam_count <= 0)
        cam_idx = 0;
      else
        return;
    }
    // PE @ 0x458D66: map[cam_idx*36+camNum] after Bind (vp gate), not con.
    const int32_t slot_idx =
        vp_id != 0 ? chassis_cam_slot_map_get(chassis, cam_idx, cam_num) : -1;
    void* slot_node =
        slot_idx >= 0 ? chassis_cam_slot_node(chassis, slot_idx) : nullptr;
    int32_t match_key = 0;
    int32_t bind_ok = 0;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& r = ref(self);
      r.hidden = 0;
      r.render_vp_id = vp_id;
      r.render_con_id = con_id;
      r.render_cam_num = cam_num;
      r.render_cam_idx = cam_idx;
      // PE arg_C==0 → skip Bind/Rebind (jz 0x458DD5).
      if (vp_id != 0) {
        // PE @ 0x458d42 ResHandle_Bind(cam+0x44, vp, type=0x12, 0).
        bind_ok = res_handle_bind_resolve(r.render_cam_bind_rh, vp_id, 0x12);
        r.render_bind_type = 0x12;
        r.render_bind_resolved = bind_ok;
        if (slot_node) {
          // PE @ 0x458D88 ResHandle_Rebind(cam+0x34, chassis_slot_node).
          res_handle_rebind(r.render_cam_rh, slot_node);
          match_key = *reinterpret_cast<int32_t*>(
              reinterpret_cast<char*>(r.render_cam_rh) + 8);
        } else {
          // Map empty / slot unset — prior soft Rebind to chassis|self.
          match_key = void_event_res_handle_rebind_owner(
              r.render_cam_rh, chassis, vp_id);
        }
        r.render_cam_match_key = match_key;
      } else {
        r.render_bind_type = 0;
        r.render_bind_resolved = 0;
      }
    }
    if (vp) tree_field_set_obj(self, "render_viewport", vp);
    if (con) tree_field_set_obj(self, "render_controller", con);
    tree_field_set_int(self, "hidden", 0);
    tree_field_set_int(self, "render_vp_id", vp_id);
    tree_field_set_int(self, "render_con_id", con_id);
    tree_field_set_int(self, "render_cam_num", cam_num);
    tree_field_set_int(self, "render_cam_idx", cam_idx);
    tree_field_set_int(self, "render_cam_slot", slot_idx);
    tree_field_set_int(self, "render_cam_bound", vp_id != 0 ? 1 : 0);
    tree_field_set_int(self, "render_cam_bind_type",
                       vp_id != 0 ? 0x12 : 0);
    tree_field_set_int(self, "render_cam_bind_resolved", bind_ok);
    tree_field_set_int(self, "render_cam_match_key", match_key);
    tree_field_set_int(self, "cmd_render",
                       tree_field_get_int(self, "cmd_render") + 1);
    return;
  }
  // PE @ 0x45A232 "reload" / 0x45A27C "idle" — counters only (no full PE).
  if (std::strcmp(tok, "reload") == 0) {
    tree_field_set_int(self, "reload_count",
                       tree_field_get_int(self, "reload_count") + 1);
    return;
  }
  if (std::strcmp(tok, "idle") == 0) {
    tree_field_set_int(self, "idle_count",
                       tree_field_get_int(self, "idle_count") + 1);
    return;
  }
  // PE @ 0x45A32E "setpitch" / 0x45A3C1 "addroll" / 0x459DD5 "brake".
  if (std::strcmp(tok, "setpitch") == 0) {
    s += 8;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& r = ref(self);
    r.setpitch = v;
    r.op = v;
    tree_field_set_float(self, "setpitch", v);
    return;
  }
  if (std::strcmp(tok, "addroll") == 0) {
    s += 7;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& r = ref(self);
    r.addroll = v;
    r.or_ += v;
    tree_field_set_float(self, "addroll", r.addroll);
    return;
  }
  if (std::strcmp(tok, "brake") == 0) {
    s += 5;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    ref(self).brake = v;
    tree_field_set_float(self, "brake", v);
    return;
  }
  // PE @ 0x45A47F "setvel" — scanf "%*s %f,%f,%f,%f,%f,%f" (lin xyz then
  // ang xyz) into physics pose slots at obj+0x70 / xor-1 twin + ang buffer
  // (engine double-buffer @ +0x20C — OOS layout). Host: lin + ang via
  // physics_* (same as GameRef.setState / stop/reset); not inventing +0x70.
  if (std::strcmp(tok, "setvel") == 0) {
    s += 6;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const float vx = std::strtof(s, &end);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t' || *s == ',') ++s;
    const float vy = std::strtof(s, &end);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t' || *s == ',') ++s;
    const float vz = std::strtof(s, &end);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t' || *s == ',') ++s;
    float wx = 0.f, wy = 0.f, wz = 0.f;
    if (*s) {
      wx = std::strtof(s, &end);
      if (end != s) {
        s = end;
        while (*s == ' ' || *s == '\t' || *s == ',') ++s;
        wy = std::strtof(s, &end);
        if (end != s) {
          s = end;
          while (*s == ' ' || *s == '\t' || *s == ',') ++s;
          wz = std::strtof(s, &end);
        }
      }
    }
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& r = ref(self);
      r.vx = vx;
      r.vy = vy;
      r.vz = vz;
    }
    physics_set_velocity(self, vx, vy, vz);
    physics_set_ang_vel(self, wx, wy, wz);
    return;
  }
  // PE @ 0x4599C3 "team" — int store stand-in.
  if (std::strcmp(tok, "team") == 0) {
    s += 4;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    ref(self).team = static_cast<int32_t>(v);
    tree_field_set_int(self, "team", static_cast<int32_t>(v));
    return;
  }
  // Phase 2.102 — transmission / steerhelp / asr / abs / difflock / cruise /
  // damage_multiplier / setsteer (Vehicle.java + CarMarket).
  // PE: exact first-token strcmp (sub_5D7190), same as wakeup/sethorn.
  auto sync_assist = [self](GameRefState& r) {
    tree_field_set_int(self, "transmission", r.transmission);
    tree_field_set_float(self, "steerhelp", r.steerhelp);
    tree_field_set_float(self, "asr", r.asr);
    tree_field_set_float(self, "abs", r.abs_);
    tree_field_set_float(self, "esp", r.esp);
    tree_field_set_float(self, "difflock", r.difflock);
    tree_field_set_int(self, "cruise", r.cruise);
    tree_field_set_float(self, "damage_multiplier", r.damage_multiplier);
    tree_field_set_float(self, "setsteer", r.setsteer);
    tree_field_set_int(self, "filter_1", r.filter_engine);
    tree_field_set_int(self, "filter_2", r.filter_body);
    tree_field_set_int(self, "filter_3", r.filter_rgear);
  };
  if (std::strcmp(tok, "transmission") == 0) {
    s += 12;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& r = ref(self);
    r.transmission = static_cast<int32_t>(v);
    sync_assist(r);
    return;
  }
  if (std::strcmp(tok, "steerhelp") == 0) {
    s += 9;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& r = ref(self);
    r.steerhelp = v;
    sync_assist(r);
    return;
  }
  if (std::strcmp(tok, "asr") == 0) {
    s += 3;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& r = ref(self);
    r.asr = v;
    sync_assist(r);
    return;
  }
  if (std::strcmp(tok, "abs") == 0) {
    s += 3;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& r = ref(self);
    r.abs_ = v;
    sync_assist(r);
    return;
  }
  // PE @ 0x459739 "esp" — twin of abs/asr: scanf "%*s %f"; >0 → flag
  // |0x4000000 + store float at *[obj+0x1FBC]+0xA30 (physics OOS).
  // Host: GameRefState.esp stand-in (Vehicle assist family; not Java site).
  if (std::strcmp(tok, "esp") == 0) {
    s += 3;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& r = ref(self);
    r.esp = v;
    sync_assist(r);
    return;
  }
  if (std::strcmp(tok, "difflock") == 0) {
    s += 8;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& r = ref(self);
    r.difflock = v;
    sync_assist(r);
    return;
  }
  if (std::strcmp(tok, "cruise") == 0) {
    s += 6;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& r = ref(self);
    r.cruise = static_cast<int32_t>(v);
    sync_assist(r);
    return;
  }
  if (std::strcmp(tok, "damage_multiplier") == 0) {
    s += 17;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& r = ref(self);
    r.damage_multiplier = v;
    sync_assist(r);
    return;
  }
  if (std::strcmp(tok, "setsteer") == 0) {
    s += 8;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& r = ref(self);
    r.setsteer = v;
    sync_assist(r);
    return;
  }
  // Phase 2.103 — "filter <cat> <mode>" (1=engine 2=body 3=rgear).
  if (std::strcmp(tok, "filter") == 0) {
    s += 6;
    while (*s == ' ' || *s == '\t') ++s;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long cat = std::strtol(s, &end, 10);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t') ++s;
    const long mode = std::strtol(s, &end, 10);
    if (end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto& r = ref(self);
    if (cat == 1)
      r.filter_engine = static_cast<int32_t>(mode);
    else if (cat == 2)
      r.filter_body = static_cast<int32_t>(mode);
    else if (cat == 3)
      r.filter_rgear = static_cast<int32_t>(mode);
    else
      return;
    sync_assist(r);
    return;
  }
  // race123 — more EVENT_COMMAND tokens from sub_458C00 (stand-in stores;
  // full physics/slot handlers OOS — not porting 0x38fe). IDA push-string
  // audit: "bot"/"controllable %d"/"ai_info_leavetraffic …" are sprintf /
  // LoadGameInit data — NOT first-token strcmp inputs.
  // PE @ 0x459109 "activate": scanf "%*s %d %d %d" → findCam(a0) @
  // 0x459151; miss → findCam(0) free row @ 0x4591E0; Bind(cam+0x4, a0,
  // type=1) writes match @ cam+0xC; Bind(cam+0x14, a1, type=1);
  // Relink(cam+0x24, a2). Host: cam_rows + res_handle_bind_resolve.
  if (std::strcmp(tok, "activate") == 0) {
    s += 8;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long a0 = *s ? std::strtol(s, &end, 10) : 0;
    if (*s && end == s) return;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long a1 = *s ? std::strtol(s, &end, 10) : 0;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long a2 = *s ? std::strtol(s, &end, 10) : 0;
    const int32_t ctrl_id = static_cast<int32_t>(a0);
    const int32_t b_id = static_cast<int32_t>(a1);
    const int32_t c_id = static_cast<int32_t>(a2);
    InvObject* chassis = tree_field_get_obj(self, "chassis");
    if (!chassis) chassis = self;
    chassis_cam_tables_ensure(chassis);
    const int32_t cam_count = cam_table_count_for(self, chassis);
    void* c_node = nullptr;
    if (c_id != 0) {
      if (InvObject* cobj = resref_find_by_id(c_id)) {
        native_ptr_ensure(cobj);
        c_node = native_ptr_node(cobj);
      }
    }
    int32_t cam_idx = -1;
    int32_t bind_a = 0;
    int32_t bind_b = 0;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& r = ref(self);
      // PE: find existing match for ctrl, else free row (match_id==0).
      cam_idx = find_cam_index_by_match_id_unlocked(r, cam_count, ctrl_id, 0,
                                                    1);
      if (cam_idx < 0)
        cam_idx =
            find_cam_index_by_match_id_unlocked(r, cam_count, 0, 0, 1);
      if (cam_idx >= 0 && cam_idx < GameRefState::kCamTableMax) {
        auto& row = r.cam_rows[static_cast<size_t>(cam_idx)];
        // PE Bind type=1 @ cam+0x4 / +0x14 (push 0; push 1; push id).
        bind_a = res_handle_bind_resolve(row.rh_ctrl, ctrl_id, 1);
        row.match_id = ctrl_id;  // PE rh+8 after Bind @ cam+0x4
        if (b_id != 0)
          bind_b = res_handle_bind_resolve(row.rh_b, b_id, 1);
        if (c_id != 0) {
          res_handle_rebind(row.rh_c, c_node);
          if (!c_node) {
            auto* w = reinterpret_cast<int32_t*>(row.rh_c);
            w[2] = c_id;
          }
        }
      }
      r.activated = 1;
      r.activate_a = ctrl_id;
      r.activate_b = b_id;
      r.activate_c = c_id;
      cam_rows_tree_sync(self, r, cam_count);
    }
    tree_field_set_int(self, "activated", 1);
    tree_field_set_int(self, "activate_a", ctrl_id);
    tree_field_set_int(self, "activate_b", b_id);
    tree_field_set_int(self, "activate_c", c_id);
    tree_field_set_int(self, "activate_cam_idx", cam_idx);
    tree_field_set_int(self, "activate_bind_ctrl", bind_a);
    tree_field_set_int(self, "activate_bind_b", bind_b);
    tree_field_set_int(self, "cmd_activate",
                       tree_field_get_int(self, "cmd_activate") + 1);
    // PE @ 0x4591c7 / 0x4592a2: Chassis_camApplyBoundSlot after Bind.
    if (cam_idx >= 0) {
      int32_t osd_live = 0;
      {
        std::lock_guard<std::mutex> lock(g_gr_mu);
        if (cam_idx < GameRefState::kCamTableMax)
          osd_live = ref(self)
                         .cam_rows[static_cast<size_t>(cam_idx)]
                         .osd_bound_id != 0
                         ? 1
                         : 0;
      }
      chassis_cam_apply_bound_slot(chassis, cam_idx, osd_live);
    }
    return;
  }
  if (std::strcmp(tok, "deactivate") == 0) {
    // PE hide/deactivate paths Unlink cam embeds + findCam clear; host:
    // zero match_id for activate_a row (free slot for next activate).
    int32_t cleared = 0;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& r = ref(self);
      const int32_t want = r.activate_a;
      if (want != 0) {
        for (auto& row : r.cam_rows) {
          if (row.match_id == want) {
            row.match_id = 0;
            row.osd_bound_id = 0;
            std::memset(row.rh_ctrl, 0, sizeof(row.rh_ctrl));
            std::memset(row.rh_b, 0, sizeof(row.rh_b));
            std::memset(row.rh_c, 0, sizeof(row.rh_c));
            std::memset(row.rh_osd, 0, sizeof(row.rh_osd));
            ++cleared;
          }
        }
      }
      r.activated = 0;
      cam_rows_tree_sync(self, r, GameRefState::kCamTableMax);
    }
    tree_field_set_int(self, "activated", 0);
    tree_field_set_int(self, "deactivate_cleared", cleared);
    return;
  }
  if (std::strcmp(tok, "next") == 0) {
    tree_field_set_int(self, "cmd_next",
                       tree_field_get_int(self, "cmd_next") + 1);
    return;
  }
  if (std::strcmp(tok, "prev") == 0) {
    tree_field_set_int(self, "cmd_prev",
                       tree_field_get_int(self, "cmd_prev") + 1);
    return;
  }
  if (std::strcmp(tok, "corpse") == 0) {
    tree_field_set_int(self, "corpse", 1);
    return;
  }
  // PE @ 0x459A07 "pickup": first `%*s %d %d` (defaults 0,1); if first
  // int≠0 → exit. Else `%*s %32s %d` — second token:
  //   "health" @ 0x459A6A: add scaled int to +0xB4 (cap 1.0f);
  //   "ammo"   @ 0x459ACE: match → immediate ret (no-op);
  //   "all"    @ 0x459AEA: set +0xB4=1.0f.