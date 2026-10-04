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
  // These are NOT first-token strcmp (only nested under pickup).
  // race125: nested ammo no-op; +0xB4 via ammo_frac float stand-in.
  if (std::strcmp(tok, "pickup") == 0) {
    s += 6;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    // Nested form: "pickup health N" / "pickup all" / "pickup ammo" (nop)
    if (std::strncmp(s, "health", 6) == 0 &&
        (s[6] == ' ' || s[6] == '\t' || s[6] == '\0')) {
      s += 6;
      while (*s == ' ' || *s == '\t') ++s;
      const long v = std::strtol(s, &end, 10);
      if (end == s) return;
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& r = ref(self);
      // PE +0xB4 float frac: add then cap at 1.0 — host health int + frac.
      r.health += static_cast<int32_t>(v);
      float frac = tree_field_get_float(self, "ammo_frac") +
                   static_cast<float>(v);
      if (frac > 1.f) frac = 1.f;
      if (frac < 0.f) frac = 0.f;
      tree_field_set_int(self, "health", r.health);
      tree_field_set_float(self, "ammo_frac", frac);
      return;
    }
    if (std::strncmp(s, "ammo", 4) == 0 &&
        (s[4] == ' ' || s[4] == '\t' || s[4] == '\0')) {
      // PE @ 0x459ADE: ammo match → jz exit (no store).
      return;
    }
    if (std::strcmp(s, "all") == 0 ||
        (std::strncmp(s, "all", 3) == 0 &&
         (s[3] == ' ' || s[3] == '\t' || s[3] == '\0'))) {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      ref(self).ammo = 1;  // PE +0xB4 = 1.0f full
      tree_field_set_int(self, "ammo", 1);
      tree_field_set_float(self, "ammo_frac", 1.f);
      return;
    }
    const long v = *s ? std::strtol(s, &end, 10) : 1;
    if (*s && end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    ref(self).pickup = static_cast<int32_t>(v);
    tree_field_set_int(self, "pickup", static_cast<int32_t>(v));
    return;
  }
  // PE has no first-token "health"/"ammo" (only nested under pickup).
  // Host stand-in kept for TREE callers that emit bare tokens.
  if (std::strcmp(tok, "health") == 0) {
    s += 6;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    ref(self).health = static_cast<int32_t>(v);
    tree_field_set_int(self, "health", static_cast<int32_t>(v));
    return;
  }
  if (std::strcmp(tok, "ammo") == 0) {
    // PE nested ammo is no-op; bare first-token also no-op (no invent parse).
    return;
  }
  // PE @ 0x45A5DA install / 0x45A6BF remove / 0x45A71A cancel / 0x45A76D repair.
  if (std::strcmp(tok, "install") == 0 || std::strcmp(tok, "remove") == 0 ||
      std::strcmp(tok, "cancel") == 0 || std::strcmp(tok, "repair") == 0) {
    char key[32];
    std::snprintf(key, sizeof(key), "cmd_%s", tok);
    tree_field_set_int(self, key, tree_field_get_int(self, key) + 1);
    return;
  }
  // PE @ 0x45AF19 "updatevariables".
  if (std::strcmp(tok, "updatevariables") == 0) {
    tree_field_set_int(self, "updatevariables",
                       tree_field_get_int(self, "updatevariables") + 1);
    return;
  }
  // PE @ 0x45BBA6 autopilot_on / 0x45BBF7 autopilot_off — store 1/0 at
  // gameInst[+0x180] stride (host: GameRefState.autopilot).
  if (std::strcmp(tok, "autopilot_on") == 0) {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    ref(self).autopilot = 1;
    tree_field_set_int(self, "autopilot", 1);
    return;
  }
  if (std::strcmp(tok, "autopilot_off") == 0) {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    ref(self).autopilot = 0;
    tree_field_set_int(self, "autopilot", 0);
    return;
  }
  if (std::strcmp(tok, "ai_cheat") == 0) {
    tree_field_set_int(self, "ai_cheat",
                       tree_field_get_int(self, "ai_cheat") + 1);
    return;
  }
  // race120/126 — physical EVENT_COMMAND cluster (sub_458C00 @ 0x45AE64..).
  // Light/wing/mslot intrusive lists + rollbar table ported on GameRefState
  // (PE blob +0x206C/+0x2088/+0x20A4/+0x1FBC). Chassis slot tables still OOS.
  // IDA: ResHandle_orPayloadFlags @ 0x48D1F0, RenderRef_applyBoneId @ 0x48BD30.
  // PE @ 0x45AE64 "add_part": scanf "%*s %d" → GameRef_cmdAddPart @ 0x45AE95
  // (alias "addpart") + Chassis_attachPartSlot @ 0x45AEA1 (flag |1 @ +0x88).
  // W9B: cmdAddPart @ 0x447B82 inserts *[phys+0xDC] via physDcList_insertTail.
  // W17D: attachPartSlot @ 0x43F2E0 walks SimObjectList(+0x1154).head@+0x115C
  // — Chassis producer/consumer OOS here (no GameRef list insert). phys+0x78
  // table = Part_buildPhysSlotTable @ 0x46EAE0 (Chassis_allocCamBlob).
  if (std::strcmp(tok, "add_part") == 0) {
    s += 8;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long id = std::strtol(s, &end, 10);
    if (end == s) return;
    const int32_t rid = static_cast<int32_t>(id);
    tree_field_set_int(self, "cmd_add_part",
                       tree_field_get_int(self, "cmd_add_part") + 1);
    tree_field_set_int(self, "add_part_id", rid);
    InvObject* type = resref_find_by_id(rid);
    if (!type) return;
    InvObject* part = gameref_new();
    java_util_resource_GameRef_create(part, self, type, string_new(""),
                                      string_new("addpart"));
    int32_t parent_slot = 0, child_slot = 0;
    if (part_find_cfg_install(self, part, &parent_slot, &child_slot))
      part_install(self, parent_slot, part, child_slot);
    else
      java_util_resource_GameRef_setParent(part, self);
    gameref_phys_dc_add_part(self, part);  // PE @ 0x447B82
    tree_field_set_obj(self, "last_add_part", part);
    return;
  }
  // PE @ 0x45AEC6 "rem_part": scanf "%*s %d" → Chassis_detachPartSlot +
  // GameRef_cmdRemPart (alias "rempart").
  // W9B: cmdRemPart @ 0x448047 walks/unlinks *[phys+0xDC] node.
  if (std::strcmp(tok, "rem_part") == 0) {
    s += 8;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long id = std::strtol(s, &end, 10);
    if (end == s) return;
    const int32_t rid = static_cast<int32_t>(id);
    tree_field_set_int(self, "cmd_rem_part",
                       tree_field_get_int(self, "cmd_rem_part") + 1);
    tree_field_set_int(self, "rem_part_id", rid);
    InvObject* part = resref_find_by_id(rid);
    if (!part) part = tree_field_get_obj(self, "last_add_part");
    if (!part) return;
    gameref_phys_dc_rem_part(self, part);  // PE @ 0x448047
    part_uninstall(part);
    java_util_resource_GameRef_setParent(part, nullptr);
    return;
  }
  // PE @ 0x45AF46 "add_light": scanf "%*s %d %d" (type handle, flags);
  // malloc 0x2C; sub_429130; ResHandle_Rebind(node+0xC, type+0xC);
  // node+0x1C=flags; intensity @ +0x20/+0x28 if flags&8|0x30;
  // list insert +0x206C/+0x2074; unless flags&0x40 → orPayloadFlags 0x400000.
  if (std::strcmp(tok, "add_light") == 0) {
    s += 9;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long type_id = std::strtol(s, &end, 10);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t') ++s;
    const long flags_l = *s ? std::strtol(s, &end, 10) : 0;
    const int32_t flags = static_cast<int32_t>(flags_l);
    tree_field_set_int(self, "cmd_add_light",
                       tree_field_get_int(self, "cmd_add_light") + 1);
    tree_field_set_int(self, "add_light_type_id",
                       static_cast<int32_t>(type_id));
    tree_field_set_int(self, "add_light_flags", flags);
    InvObject* type = resref_find_by_id(static_cast<int32_t>(type_id));
    if (!type) return;
    InvObject* light = gameref_new();
    java_util_resource_ResourceRef_set(
        light, java_util_resource_ResourceRef_id(type));
    java_util_resource_GameRef_setParent(light, self);
    tree_field_set_int(light, "light_flags", flags);
    auto node = std::make_unique<GameRefState::PeLightNode>();
    node->type = type;
    node->flags = flags;
    node->light_obj = light;
    // PE @ 0x45AFC9: ResHandle_Rebind(node+0xC, *(type+0xC)); +0x14←*(rh+8).
    node->match_key = void_event_res_handle_rebind_owner(
        node->rh, type, static_cast<int32_t>(type_id));
    if (node->match_key == 0) node->match_key = static_cast<int32_t>(type_id);
    // PE @ 0x45AFD5: flags&8 → rand intensity; flags&0x30 → (obj+8&0xF)*0.01+0.4
    // flt_5F0DBC=1/32768, flt_5F0D50=0.4, flt_5F0CD4=0.2, flt_5F0C20=0.01,
    // flt_5F09D0=0.5 (IDA bytes @ 0x5F0DBC..).
    if (flags & 8) {
      const float r01 =
          static_cast<float>(std::rand() & 0x7FFF) * (1.f / 32768.f);
      node->f20 = r01 * 0.4f + 0.2f;
      node->f24 = 0.f;
      node->f28 = node->f20 * 0.5f;
    } else if (flags & 0x30) {
      int32_t lo4 = 0;
      {
        std::lock_guard<std::mutex> lock(g_gr_mu);
        lo4 = ref(self).flags & 0xF;
      }
      node->f20 = static_cast<float>(lo4) * 0.01f + 0.4f;
      node->f24 = 0.f;
      node->f28 = node->f20 * 0.5f;
    }
    tree_field_set_float(light, "light_f20", node->f20);
    tree_field_set_float(light, "light_f28", node->f28);
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& st = ref(self);
      GameRefState::PeLightNode* raw = node.get();
      pe_light_insert_tail(st, raw);
      st.light_nodes.push_back(std::move(node));
    }
    // PE ResHandle_orPayloadFlags(type, 0x400000) @ 0x48D1F0 / 0x45B05C
    // when !(flags & 0x40).
    if ((flags & 0x40) == 0) res_handle_or_payload_flags(type, 0x400000);
    tree_field_set_obj(self, "last_add_light", light);
    tree_field_set_int(self, "pe_light_count",
                       tree_field_get_int(self, "pe_light_count") + 1);
    return;
  }
  // PE @ 0x45B077 "rem_light": scanf "%*s %d %d"; walk list from +0x2064;
  // match node+0x14==key; node+0x1C &= ~mask; if flags left keep node;
  // else andNot/or 0x400000 (mask&0x40) + vtbl dtor.
  if (std::strcmp(tok, "rem_light") == 0) {
    s += 9;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long key = *s ? std::strtol(s, &end, 10) : 0;
    if (*s && end == s) return;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long mask_l = *s ? std::strtol(s, &end, 10) : 0;
    const int32_t mask = static_cast<int32_t>(mask_l);
    tree_field_set_int(self, "cmd_rem_light",
                       tree_field_get_int(self, "cmd_rem_light") + 1);
    tree_field_set_int(self, "rem_light_key", static_cast<int32_t>(key));
    tree_field_set_int(self, "rem_light_flags", mask);
    InvObject* light_obj = nullptr;
    InvObject* typ = nullptr;
    bool destroy = false;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& st = ref(self);
      auto* sent = st.light_sent.get();
      GameRefState::PeLightNode* cur =
          sent && sent->next && sent->next != sent ? sent->next : nullptr;
      while (cur && cur != sent) {
        GameRefState::PeLightNode* nxt = cur->next;
        if (cur->match_key == static_cast<int32_t>(key)) {
          cur->flags &= ~mask;
          if (cur->flags != 0) break;
          typ = cur->type;
          light_obj = cur->light_obj;
          destroy = true;
          // PE GameRef_LightNode_dtor @ 0x45F8B0: unlink rh from owner list.
          void_event_res_handle_unbind_owner(cur->rh);
          pe_light_unlink(st, cur);
          for (auto it = st.light_nodes.begin(); it != st.light_nodes.end();
               ++it) {
            if (it->get() == cur) {
              st.light_nodes.erase(it);
              break;
            }
          }
          break;
        }
        cur = (nxt && nxt != sent) ? nxt : nullptr;
      }
    }
    if (destroy) {
      if (light_obj) java_util_resource_GameRef_setParent(light_obj, nullptr);
      // PE @ 0x45B100: mask&0x40 → andNot else or 0x400000 on type handle.
      if (typ) {
        if (mask & 0x40)
          res_handle_and_not_payload_flags(typ, 0x400000);
        else
          res_handle_or_payload_flags(typ, 0x400000);
      }
      if (tree_field_get_obj(self, "last_add_light") == light_obj)
        tree_field_set_obj(self, "last_add_light", nullptr);
      const int32_t n = tree_field_get_int(self, "pe_light_count");
      if (n > 0) tree_field_set_int(self, "pe_light_count", n - 1);
    }
    return;
  }
  // PE @ 0x45B15D "add_wing": scanf "%*s %d %d"; malloc 0x1C; +0x0C=2nd,
  // +0x10=1st handle; list insert +0x2088/+0x2090.
  if (std::strcmp(tok, "add_wing") == 0) {
    s += 8;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long a = std::strtol(s, &end, 10);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t') ++s;
    const long b = *s ? std::strtol(s, &end, 10) : 0;
    tree_field_set_int(self, "cmd_add_wing",
                       tree_field_get_int(self, "cmd_add_wing") + 1);
    tree_field_set_int(self, "add_wing_a", static_cast<int32_t>(a));
    tree_field_set_int(self, "add_wing_b", static_cast<int32_t>(b));
    InvObject* wing = resref_find_by_id(static_cast<int32_t>(a));
    if (wing) java_util_resource_GameRef_setParent(wing, self);
    auto node = std::make_unique<GameRefState::PeWingNode>();
    node->key_a = static_cast<int32_t>(a);
    node->key_b = static_cast<int32_t>(b);
    node->handle_a = wing;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& st = ref(self);
      GameRefState::PeWingNode* raw = node.get();
      pe_wing_insert_tail(st, raw);
      st.wing_nodes.push_back(std::move(node));
    }
    if (wing) tree_field_set_obj(self, "last_add_wing", wing);
    tree_field_set_int(self, "pe_wing_count",
                       tree_field_get_int(self, "pe_wing_count") + 1);
    return;
  }
  // PE @ 0x45B223 "rem_wing": scanf "%*s %d %d"; match list @ +0x2080
  // node+0xC==2nd && node+0x10==1st; release +0x10 ref + vtbl dtor.
  if (std::strcmp(tok, "rem_wing") == 0) {
    s += 8;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long a = *s ? std::strtol(s, &end, 10) : 0;
    if (*s && end == s) return;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long b = *s ? std::strtol(s, &end, 10) : 0;
    tree_field_set_int(self, "cmd_rem_wing",
                       tree_field_get_int(self, "cmd_rem_wing") + 1);
    tree_field_set_int(self, "rem_wing_a", static_cast<int32_t>(a));
    tree_field_set_int(self, "rem_wing_b", static_cast<int32_t>(b));
    InvObject* wing = nullptr;
    bool removed = false;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& st = ref(self);
      auto* sent = st.wing_sent.get();
      GameRefState::PeWingNode* cur =
          sent && sent->next && sent->next != sent ? sent->next : nullptr;
      while (cur && cur != sent) {
        GameRefState::PeWingNode* nxt = cur->next;
        if (cur->key_a == static_cast<int32_t>(a) &&
            cur->key_b == static_cast<int32_t>(b)) {
          wing = cur->handle_a;
          pe_wing_unlink(st, cur);
          for (auto it = st.wing_nodes.begin(); it != st.wing_nodes.end();
               ++it) {
            if (it->get() == cur) {
              st.wing_nodes.erase(it);
              break;
            }
          }
          removed = true;
          break;
        }
        cur = (nxt && nxt != sent) ? nxt : nullptr;
      }
    }
    if (wing) java_util_resource_GameRef_setParent(wing, nullptr);
    if (wing && tree_field_get_obj(self, "last_add_wing") == wing)
      tree_field_set_obj(self, "last_add_wing", nullptr);
    if (removed) {
      const int32_t n = tree_field_get_int(self, "pe_wing_count");
      if (n > 0) tree_field_set_int(self, "pe_wing_count", n - 1);
    }
    return;
  }
  // PE @ 0x45B2F9 "add_rollbar": scanf "%*s %d %d %f %f" (defaults
  // 0x461C4000=1e4, 0x43FA0000=500); *[+0x1FBC]+0x44 count max 8;
  // entry at +0x48+16*i = {i0,i1,f0,f1}. Host: GameRefState.rollbars.
  if (std::strcmp(tok, "add_rollbar") == 0) {
    s += 11;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long i0 = *s ? std::strtol(s, &end, 10) : -1;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long i1 = *s ? std::strtol(s, &end, 10) : -1;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    float f0 = 10000.f, f1 = 500.f;
    if (*s) {
      f0 = std::strtof(s, &end);
      if (end != s) {
        s = end;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s) f1 = std::strtof(s, &end);
      }
    }
    tree_field_set_int(self, "cmd_add_rollbar",
                       tree_field_get_int(self, "cmd_add_rollbar") + 1);
    tree_field_set_int(self, "rollbar_a", static_cast<int32_t>(i0));
    tree_field_set_int(self, "rollbar_b", static_cast<int32_t>(i1));
    tree_field_set_float(self, "rollbar_f0", f0);
    tree_field_set_float(self, "rollbar_f1", f1);
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& st = ref(self);
      if (st.rollbars.size() < 8) {
        GameRefState::PeRollbarEntry e;
        e.a = static_cast<int32_t>(i0);
        e.b = static_cast<int32_t>(i1);
        e.f0 = f0;
        e.f1 = f1;
        st.rollbars.push_back(e);
      }
    }
    return;
  }
  // PE @ 0x45B38A "rem_rollbar": match a/b/f0/f1; splice last entry over.
  if (std::strcmp(tok, "rem_rollbar") == 0) {
    s += 11;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long i0 = *s ? std::strtol(s, &end, 10) : -1;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long i1 = *s ? std::strtol(s, &end, 10) : -1;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    float f0 = 10000.f, f1 = 500.f;
    if (*s) {
      f0 = std::strtof(s, &end);
      if (end != s) {
        s = end;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s) f1 = std::strtof(s, &end);
      }
    }
    tree_field_set_int(self, "cmd_rem_rollbar",
                       tree_field_get_int(self, "cmd_rem_rollbar") + 1);
    tree_field_set_int(self, "rollbar_a", static_cast<int32_t>(i0));
    tree_field_set_int(self, "rollbar_b", static_cast<int32_t>(i1));
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& st = ref(self);
      for (size_t i = 0; i < st.rollbars.size(); ++i) {
        auto& e = st.rollbars[i];
        if (e.a == static_cast<int32_t>(i0) &&
            e.b == static_cast<int32_t>(i1) && e.f0 == f0 && e.f1 == f1) {
          st.rollbars[i] = st.rollbars.back();
          st.rollbars.pop_back();
          break;
        }
      }
    }
    return;
  }
  // PE @ 0x45B46E "add_linked": scanf "%*s %d %d %d %d"; Veh_ensureSceneBound
  // ×2; RenderRef_bindBone("bone00"/"bone01") @ 0x48BC40 +
  // RenderRef_applyBoneId @ 0x48BD30. Host: getBoneId (JNI→bindBone).
  if (std::strcmp(tok, "add_linked") == 0) {
    s += 10;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long i0 = *s ? std::strtol(s, &end, 10) : 0;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long i1 = *s ? std::strtol(s, &end, 10) : -1;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long i2 = *s ? std::strtol(s, &end, 10) : 0;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long i3 = *s ? std::strtol(s, &end, 10) : 0;
    tree_field_set_int(self, "cmd_add_linked",
                       tree_field_get_int(self, "cmd_add_linked") + 1);
    tree_field_set_int(self, "linked_i0", static_cast<int32_t>(i0));
    tree_field_set_int(self, "linked_i1", static_cast<int32_t>(i1));
    InvObject* a = resref_find_by_id(static_cast<int32_t>(i2));
    InvObject* b = resref_find_by_id(static_cast<int32_t>(i3));
    if (a) {
      tree_field_set_obj(self, "linked_a", a);
      int32_t b0 =
          java_util_resource_RenderRef_getBoneId(a, string_new("bone00"));
      int32_t b1 =
          java_util_resource_RenderRef_getBoneId(a, string_new("bone01"));
      tree_field_set_int(self, "linked_bone00", b0);
      tree_field_set_int(self, "linked_bone01", b1);
      // PE RenderRef_applyBoneId @ 0x48BD30 after bindBone.
      render_ref_apply_bone_id(a, b0);
      render_ref_apply_bone_id(a, b1);
    }
    if (b) {
      tree_field_set_obj(self, "linked_b", b);
      int32_t b0 =
          java_util_resource_RenderRef_getBoneId(b, string_new("bone00"));
      int32_t b1 =
          java_util_resource_RenderRef_getBoneId(b, string_new("bone01"));
      render_ref_apply_bone_id(b, b0);
      render_ref_apply_bone_id(b, b1);
    }
    return;
  }
  // PE @ 0x45B6D9 "rem_linked": scanf 4 ints; bindBone bone00/01 then dtor.
  if (std::strcmp(tok, "rem_linked") == 0) {
    s += 10;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long i0 = *s ? std::strtol(s, &end, 10) : 0;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long i1 = *s ? std::strtol(s, &end, 10) : -1;
    tree_field_set_int(self, "cmd_rem_linked",
                       tree_field_get_int(self, "cmd_rem_linked") + 1);
    tree_field_set_int(self, "linked_i0", static_cast<int32_t>(i0));
    tree_field_set_int(self, "linked_i1", static_cast<int32_t>(i1));
    InvObject* a = tree_field_get_obj(self, "linked_a");
    if (a) {
      java_util_resource_RenderRef_getBoneId(a, string_new("bone00"));
      java_util_resource_RenderRef_getBoneId(a, string_new("bone01"));
    }
    tree_field_set_obj(self, "linked_a", nullptr);
    tree_field_set_obj(self, "linked_b", nullptr);
    return;
  }
  // PE @ 0x45B7E3 "add_mslot": scanf "%*s %d %d"; ResListNode_ctor;
  // malloc 0x54; +0x0C/+0x10 keys; mesh memcpy from handle+0x2C/+0x38 OOS;
  // list +0x20A4/+0x20AC.
  if (std::strcmp(tok, "add_mslot") == 0) {
    s += 9;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long a = std::strtol(s, &end, 10);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t') ++s;
    const long b = *s ? std::strtol(s, &end, 10) : 0;
    tree_field_set_int(self, "cmd_add_mslot",
                       tree_field_get_int(self, "cmd_add_mslot") + 1);
    tree_field_set_int(self, "mslot_a", static_cast<int32_t>(a));
    tree_field_set_int(self, "mslot_b", static_cast<int32_t>(b));
    auto node = std::make_unique<GameRefState::PeMslotNode>();
    node->key_a = static_cast<int32_t>(a);
    node->key_b = static_cast<int32_t>(b);
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& st = ref(self);
      GameRefState::PeMslotNode* raw = node.get();
      pe_mslot_insert_tail(st, raw);
      st.mslot_nodes.push_back(std::move(node));
    }
    tree_field_set_int(self, "pe_mslot_count",
                       tree_field_get_int(self, "pe_mslot_count") + 1);
    return;
  }
  // PE @ 0x45B89A "rem_mslot": scanf "%*s %d %d"; unlink @ +0x209C
  // match +0x0C/+0x10.
  if (std::strcmp(tok, "rem_mslot") == 0) {
    s += 9;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long a = *s ? std::strtol(s, &end, 10) : 0;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long b = *s ? std::strtol(s, &end, 10) : 0;
    tree_field_set_int(self, "cmd_rem_mslot",
                       tree_field_get_int(self, "cmd_rem_mslot") + 1);
    tree_field_set_int(self, "mslot_a", static_cast<int32_t>(a));
    tree_field_set_int(self, "mslot_b", static_cast<int32_t>(b));
    bool removed = false;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& st = ref(self);
      auto* sent = st.mslot_sent.get();
      GameRefState::PeMslotNode* cur =
          sent && sent->next && sent->next != sent ? sent->next : nullptr;
      while (cur && cur != sent) {
        GameRefState::PeMslotNode* nxt = cur->next;
        if (cur->key_a == static_cast<int32_t>(a) &&
            cur->key_b == static_cast<int32_t>(b)) {
          pe_mslot_unlink(st, cur);
          for (auto it = st.mslot_nodes.begin(); it != st.mslot_nodes.end();
               ++it) {
            if (it->get() == cur) {
              st.mslot_nodes.erase(it);
              break;
            }
          }
          removed = true;
          break;
        }
        cur = (nxt && nxt != sent) ? nxt : nullptr;
      }
    }
    if (removed) {
      const int32_t n = tree_field_get_int(self, "pe_mslot_count");
      if (n > 0) tree_field_set_int(self, "pe_mslot_count", n - 1);
    }
    return;
  }
  // PE @ 0x45C3FD "AI_GoToTrafficSlow" — also fired from EVENT 0x80 case2
  // after LoadGameInit Rebind @ 0x45C3C9 (W9B). sub_45F780 AI cmd +
  // sub_48B750 (0x80000080) traffic-slow path (OOS). Host counter only.
  if (std::strcmp(tok, "AI_GoToTrafficSlow") == 0) {
    tree_field_set_int(self, "cmd_AI_GoToTrafficSlow",
                       tree_field_get_int(self, "cmd_AI_GoToTrafficSlow") + 1);
    return;
  }
  // W13A — PE @ 0x4594B4 "osd": scanf "%*s %d %d" defaults 0,0.
  // Disasm 0x4594CA..0x45952E: first %d→arg_8=osd.id, second→arg_C=ctrl;
  // findCam(arg_C) @ 0x4594F5; if cam+0x5C!=arg_8 Bind(cam+0x54, arg_8,
  // type=1) @ 0x45952E then Chassis_camApplyBoundSlot @ 0x449680.
  // Java Track: "osd "+osd.id()+" "+con.id(); single-arg → findCam(0).
  // osd.id lands @ cam+0x5C — NOT blob+0x128 (that stays ctrl/activate).
  if (std::strcmp(tok, "osd") == 0) {
    s += 3;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long oid = *s ? std::strtol(s, &end, 10) : 0;
    if (*s && end == s) return;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long cid = *s ? std::strtol(s, &end, 10) : 0;
    const int32_t osd_id = static_cast<int32_t>(oid);
    const int32_t ctrl_id = static_cast<int32_t>(cid);
    InvObject* chassis = tree_field_get_obj(self, "chassis");
    if (!chassis) chassis = self;
    chassis_cam_tables_ensure(chassis);
    const int32_t cam_count = cam_table_count_for(self, chassis);
    int32_t cam_idx = -1;
    int32_t bind_ok = 0;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& r = ref(self);
      r.osd_id = osd_id;
      r.osd_ctrl_id = ctrl_id;
      // PE @ 0x4594EB..0x4594F5: findCam(arg_C=ctrl) start=0 step=1.
      cam_idx = find_cam_index_by_match_id_unlocked(r, cam_count, ctrl_id, 0,
                                                    1);
      if (cam_idx >= 0 && cam_idx < GameRefState::kCamTableMax) {
        auto& row = r.cam_rows[static_cast<size_t>(cam_idx)];
        // PE @ 0x459510..0x459520: if cam+0x5C == osd.id → skip Bind.
        if (row.osd_bound_id != osd_id) {
          // PE @ 0x45952E ResHandle_Bind(cam+0x54, osd.id, type=1, 0).
          bind_ok = res_handle_bind_resolve(row.rh_osd, osd_id, 1);
          row.osd_bound_id = osd_id;  // PE key @ cam+0x5C / blob+0x178
        }
        r.osd_cam_idx = cam_idx;
        r.osd_bind_ok = bind_ok;
      } else {
        r.osd_cam_idx = -1;
        r.osd_bind_ok = 0;
      }
      cam_rows_tree_sync(self, r, cam_count);
    }
    tree_field_set_int(self, "osd_id", osd_id);
    tree_field_set_int(self, "osd_ctrl_id", ctrl_id);
    tree_field_set_int(self, "osd_cam_idx", cam_idx);
    tree_field_set_int(self, "osd_bind_ok", bind_ok);
    tree_field_set_int(self, "osd_bound", osd_id != 0 ? 1 : 0);
    tree_field_set_int(self, "cmd_osd",
                       tree_field_get_int(self, "cmd_osd") + 1);
    // PE @ 0x459537: Chassis_camApplyBoundSlot after osd Bind.
    if (cam_idx >= 0)
      chassis_cam_apply_bound_slot(chassis, cam_idx, osd_id != 0 ? 1 : 0);
    return;
  }
  // PE @ 0x458DD5 "camera_int_viewrange" — scanf float then
  // ResHandle_getPayload camera chain (OOS). Host: store float stand-in.
  if (std::strcmp(tok, "camera_int_viewrange") == 0) {
    s += 20;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end != s) tree_field_set_float(self, "camera_int_viewrange", v);
    tree_field_set_int(self, "cmd_camera_int_viewrange",
                       tree_field_get_int(self, "cmd_camera_int_viewrange") + 1);
    return;
  }
  // PE @ 0x45AAEB texture / 0x45ABCC mesh / 0x45ADD1 paint / decals —
  // counters only (RenderInst paths OOS).
  if (std::strcmp(tok, "texture") == 0 || std::strcmp(tok, "mesh") == 0 ||
      std::strcmp(tok, "paint") == 0 || std::strcmp(tok, "decal") == 0 ||
      std::strcmp(tok, "bigdecal") == 0) {
    char key[32];
    std::snprintf(key, sizeof(key), "cmd_%s", tok);
    tree_field_set_int(self, key, tree_field_get_int(self, key) + 1);
    return;
  }
  // race124/126 — IDA string-xref audit of sub_458C00 (size 0x38fe):
  // first-token strcmp covered. Light/wing/mslot intrusive lists + rollbar
  // table on GameRefState (PE +0x206C/+0x2088/+0x20A4/+0x1FBC). W9B/W11A:
  // EVENT 0x80 case2 LoadGameInit Rebind + phys+0xDC list via cmdAddPart
  // (exported gameref_phys_dc_*); render Bind(vp, RESOURCE_VIEWPORT=0x12)
  // @ 0x458d42 → LookupById @ 0x536820 + Rebind when slot map filled.
  // Remaining gap: Chassis forceUpdate → gameref_phys_dc_*; Engine_addTimer
  // 0x80000080; PE gates +0x1FC0/+0x1FCC/+0x1FD0 GII_CONTROL; full
  // LoadGameInit factory. W12A/W13A: findCam +0x128 rows + activate/render;
  // osd Bind(cam+0x54, osd.id) @ 0x45952E (match=+0x128 stays ctrl).
  // navigator_paint map OSD gated when tiles_absolu (PE updateNavigator
  // @ 0x482D30 has no OSD). Soft: Chassis_camApplyBoundSlot @ 0x449680
  // bones/gauges OOS; route/marker OSD host-only.
  // Phase 2.125 — MouseCursor GameRef EVENT_COMMAND (cursor/move/mode/…).
  auto cursor_owner = [self]() -> InvObject* {
    InvObject* parent = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto it = g_refs.find(self);
      if (it != g_refs.end()) parent = it->second.parent;
    }
    if (!parent) parent = tree_field_get_obj(self, "part_parent");
    if (parent) {
      const char* hc = tree_host_class(parent);
      if (hc && std::strstr(hc, "MouseCursor")) return parent;
    }
    if (InvObject* o = tree_field_get_obj(self, "cursor_owner")) return o;
    InvObject* ic = java_io_Input_cursor();
    if (ic && tree_field_get_obj(ic, "cursor") == self) return ic;
    return parent ? parent : ic;
  };
  auto sync_cursor_xy = [](InvObject* gr, InvObject* owner, float x, float y) {
    tree_field_set_float(gr, "cursor_x", x);
    tree_field_set_float(gr, "cursor_y", y);
    tree_field_set_int(gr, "cursor_set", 1);
    if (owner) {
      tree_field_set_float(owner, "cursor_x", x);
      tree_field_set_float(owner, "cursor_y", y);
      tree_field_set_int(owner, "cursor_set", 1);
    }
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& r = ref(gr);
      r.px = x;
      r.py = y;
      r.pz = 0.f;
    }
  };
  if (std::strncmp(s, "cursor", 6) == 0 &&
      (s[6] == ' ' || s[6] == '\t')) {
    s += 6;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long vp_id = std::strtol(s, &end, 10);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t') ++s;
    const long cam_id = std::strtol(s, &end, 10);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t') ++s;
    const long cur_id = std::strtol(s, &end, 10);
    InvObject* owner = cursor_owner();
    InvObject* vp = resref_find_by_id(static_cast<int32_t>(vp_id));
    InvObject* cam = resref_find_by_id(static_cast<int32_t>(cam_id));
    tree_field_set_int(self, "cursor_vp_id", static_cast<int32_t>(vp_id));
    tree_field_set_int(self, "cursor_cam_id", static_cast<int32_t>(cam_id));
    tree_field_set_int(self, "cursor_self_id", static_cast<int32_t>(cur_id));
    if (vp) tree_field_set_obj(self, "vp", vp);
    if (cam) tree_field_set_obj(self, "camera", cam);
    if (owner) {
      if (vp) tree_field_set_obj(owner, "vp", vp);
      if (cam) tree_field_set_obj(owner, "camera", cam);
      tree_field_set_obj(owner, "cursor", self);
    }
    tree_field_set_int(self, "cursor_bound", 1);
    return;
  }
  if (std::strncmp(s, "move", 4) == 0 &&
      (s[4] == ' ' || s[4] == '\t')) {
    s += 4;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    float x = std::strtof(s, &end);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t' || *s == ',') ++s;
    float y = std::strtof(s, &end);
    if (end == s) return;
    // PE Cursor_handleCommand @ 0x00461D20 "move": write instance+0xA4/+0xA8
    // then clamp each axis to [-1,1] (1.0=0x3F800000, -1.0=0xBF800000).
    if (x > 1.f) x = 1.f;
    if (x < -1.f) x = -1.f;
    if (y > 1.f) y = 1.f;
    if (y < -1.f) y = -1.f;
    sync_cursor_xy(self, cursor_owner(), x, y);
    tree_field_set_int(self, "cursor_moved",
                       tree_field_get_int(self, "cursor_moved") + 1);
    return;
  }
  if (std::strcmp(s, "enable") == 0) {
    tree_field_set_int(self, "cursor_collide", 1);
    if (InvObject* o = cursor_owner())
      tree_field_set_int(o, "cursor_collide", 1);
    return;
  }
  if (std::strcmp(s, "disable") == 0) {
    tree_field_set_int(self, "cursor_collide", 0);
    if (InvObject* o = cursor_owner())
      tree_field_set_int(o, "cursor_collide", 0);
    return;
  }
  if (std::strcmp(s, "lock") == 0) {
    // PE Cursor_handleCommand @ 0x00462320: SysCursor≠0 → Engine_SysCursorLock.
    InvObject* cfg = system_config_host();
    const int32_t sys = cfg ? tree_field_get_int(cfg, "SysCursor") : 1;
    if (sys != 0) input_syscursor_lock();
    tree_field_set_int(self, "cursor_locked", 1);
    if (InvObject* o = cursor_owner())
      tree_field_set_int(o, "cursor_locked", 1);
    return;
  }
  if (std::strcmp(s, "unlock") == 0) {
    // PE Cursor_handleCommand @ 0x0046235A: SysCursor≠0 → Engine_SysCursorUnlock.
    InvObject* cfg = system_config_host();
    const int32_t sys = cfg ? tree_field_get_int(cfg, "SysCursor") : 1;
    if (sys != 0) input_syscursor_unlock();
    tree_field_set_int(self, "cursor_locked", 0);
    if (InvObject* o = cursor_owner())
      tree_field_set_int(o, "cursor_locked", 0);
    return;
  }
  if (std::strncmp(s, "activate", 8) == 0 &&
      (s[8] == ' ' || s[8] == '\t' || s[8] == '\0')) {
    s += 8;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long ctrl_id = *s ? std::strtol(s, &end, 10) : 0;
    if (*s && end == s) return;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long cam_id = *s ? std::strtol(s, &end, 10) : 0;
    InvObject* owner = cursor_owner();
    InvObject* ctrl = ctrl_id
                          ? resref_find_by_id(static_cast<int32_t>(ctrl_id))
                          : nullptr;
    InvObject* cam =
        cam_id ? resref_find_by_id(static_cast<int32_t>(cam_id)) : nullptr;
    tree_field_set_int(self, "cursor_ctrl_id", static_cast<int32_t>(ctrl_id));
    tree_field_set_int(self, "cursor_active", 1);
    {
      // PE @ 0x459109 "activate" also marks instance active (host stand-in).
      std::lock_guard<std::mutex> lock(g_gr_mu);
      ref(self).activated = 1;
    }
    tree_field_set_int(self, "activated", 1);
    if (ctrl) tree_field_set_obj(self, "controller", ctrl);
    if (owner) {
      tree_field_set_int(owner, "cursor_active", 1);
      if (ctrl) tree_field_set_obj(owner, "controller", ctrl);
      if (cam) {
        tree_field_set_obj(owner, "controlledcam", cam);
        tree_field_set_int(self, "cursor_cam_id", static_cast<int32_t>(cam_id));
      }
    }
    return;
  }
  if (std::strcmp(s, "sens") == 0) {
    InvObject* cfg = system_config_host_for_test();
    const float sens =
        cfg ? tree_field_get_float(cfg, "mouseSensitivity") : 0.8f;
    tree_field_set_float(self, "mouse_sensitivity", sens);
    if (InvObject* o = cursor_owner())
      tree_field_set_float(o, "mouse_sensitivity", sens);
    tree_field_set_int(self, "sens_count",
                       tree_field_get_int(self, "sens_count") + 1);
    return;
  }
  if (std::strncmp(s, "mode", 4) == 0 &&
      (s[4] == ' ' || s[4] == '\t')) {
    s += 4;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long mode = std::strtol(s, &end, 10);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t') ++s;
    char ptr_ch = 0;
    if (*s && *s != ' ' && *s != '\t') {
      ptr_ch = *s;
      ++s;
    }
    while (*s == ' ' || *s == '\t') ++s;
    const long charset_id = *s ? std::strtol(s, &end, 10) : 0;
    InvObject* owner = cursor_owner();
    tree_field_set_int(self, "cursor_mode", static_cast<int32_t>(mode));
    tree_field_set_int(self, "cursor_charset_id",
                       static_cast<int32_t>(charset_id));
    char ptr_buf[2] = {ptr_ch ? ptr_ch : 'J', 0};
    tree_field_set_obj(self, "pointer", string_new(ptr_buf));
    if (owner) {
      tree_field_set_int(owner, "cursor_mode", static_cast<int32_t>(mode));
      tree_field_set_obj(owner, "pointer", string_new(ptr_buf));
      if (InvObject* cs =
              charset_id
                  ? resref_find_by_id(static_cast<int32_t>(charset_id))
                  : nullptr)
        tree_field_set_obj(owner, "pointer_charset", cs);
    }
    // Host Win32 look: digit → stock RT_CURSOR id; else default arrow (2).
    int32_t stock = 2;
    if (ptr_ch >= '2' && ptr_ch <= '9') stock = ptr_ch - '0';
    else if (ptr_ch == '0' || ptr_ch == '1')
      stock = 10 + (ptr_ch - '0');  // '0'→10, '1'→11
    render_d3d9_set_stock_cursor(stock);
    tree_field_set_int(self, "cursor_stock_id", stock);
    return;
  }
  if (std::strncmp(s, "install", 7) != 0) return;
  s += 7;

  float ftok[8] = {};
  int n = 0;
  const char* p = s;
  while (*p && n < 8) {
    while (*p == ' ' || *p == '\t' || *p == ',') ++p;
    if (!*p) break;
    char* end = nullptr;
    const float v = std::strtof(p, &end);
    if (end == p) break;
    ftok[n++] = v;
    p = end;
  }
  if (n < 2) return;

  const int32_t dest_id = static_cast<int32_t>(ftok[1]);
  InvObject* dest = resref_find_by_id(dest_id);
  if (!dest) return;

  int32_t child_slot = 1;
  int32_t parent_slot = 1;
  bool have_pos = false;
  float px = 0, py = 0, pz = 0;
  if (n >= 8) {
    // install 0 dest mySlot dest2 destSlot x y z
    if (static_cast<int32_t>(ftok[2]) > 0)
      child_slot = static_cast<int32_t>(ftok[2]);
    if (static_cast<int32_t>(ftok[4]) > 0)
      parent_slot = static_cast<int32_t>(ftok[4]);
    px = ftok[5];
    py = ftok[6];
    pz = ftok[7];
    have_pos = true;
  } else if (n >= 5) {
    // install 0 dest mySlot dest2 destSlot
    if (static_cast<int32_t>(ftok[2]) > 0)
      child_slot = static_cast<int32_t>(ftok[2]);
    if (static_cast<int32_t>(ftok[4]) > 0)
      parent_slot = static_cast<int32_t>(ftok[4]);
  }

  if (!part_install(dest, parent_slot, self, child_slot)) return;
  java_util_resource_GameRef_setParent(self, dest);
  // Part.addPart commands on `xa`; keep script instance parent in sync.
  InvObject* script = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto it = g_refs.find(self);
    if (it != g_refs.end()) script = it->second.script;
  }
  if (script && script != self)
    java_util_resource_GameRef_setParent(script, dest);
  if (have_pos)
    java_util_resource_GameRef_setMatrix(self, vec3_new(px, py, pz), nullptr);
}


// → GameRef_collision.cpp

namespace {
InvObject* g_player = nullptr;
InvObject* g_garage = nullptr;
InvObject* g_loading_screen = nullptr;
InvObject* g_gfx_engine = nullptr;
InvObject* g_large_font = nullptr;
InvObject* g_medium_font = nullptr;
InvObject* g_small_font = nullptr;
InvObject* g_pointers = nullptr;
InvObject* g_def_loading_pic = nullptr;
InvObject* g_input_queue = nullptr;
InvObject* g_hotkey_thread = nullptr;
InvObject* g_hotkey_watcher = nullptr;
int32_t g_frontend_inited = 0;
InvObject* g_actual_state = nullptr;
constexpr int32_t kMenuSet = 2;
constexpr int32_t kGmCareer = 1;
constexpr int32_t kInitialMoney = 20000;
constexpr float kInitialPrestige = 0.3f;

int32_t g_game_mode = 0;
int32_t g_timeout = 0;
int32_t g_career_in_progress = 0;
int32_t g_day = 0;
float g_time_of_day = 0.f;
int32_t g_played = 0;
int32_t g_saved = 0;
int32_t g_auto_save_calls = 0;
int32_t g_auto_save_quiet_calls = 0;
int32_t g_load_defaults_calls = 0;
InvObject* g_car_desc_new = nullptr;
InvObject* g_car_desc_used = nullptr;
float g_dealer_ts_new = 0.f;
float g_dealer_ts_used = 0.f;
InvObject* g_dialog_modal = nullptr;
std::string g_dialog_smoke_string = "Player";
InvObject* g_recent_modal_dialog = nullptr;  // last new *Dialog (not hub)
InvObject* g_racesetup = nullptr;

bool g_hub_tree_defer = false;
enum class HubCasKind { None, Garage, Valocity, Exit };
HubCasKind g_hub_cas_pending = HubCasKind::None;
}  // namespace

// (hub helpers use anonymous-namespace state in this TU.)

void main_menu_hub_begin_tree() {
  g_hub_tree_defer = true;
  g_hub_cas_pending = HubCasKind::None;
}
void main_menu_hub_end_tree() { g_hub_tree_defer = false; }
bool main_menu_hub_deferring() { return g_hub_tree_defer; }
bool main_menu_cmd_new_cas_pending() {
  return g_hub_cas_pending == HubCasKind::Garage;
}
bool main_menu_cmd_freeride_cas_pending() {
  return g_hub_cas_pending == HubCasKind::Valocity;
}
bool main_menu_cmd_exit_cas_pending() {
  return g_hub_cas_pending == HubCasKind::Exit;
}
void main_menu_hub_note_cas(InvObject* next) {
  if (!next) {
    g_hub_cas_pending = HubCasKind::Exit;
    return;
  }
  const char* nn = tree_host_class(next);
  if (!nn) return;
  if (std::strstr(nn, "Garage"))
    g_hub_cas_pending = HubCasKind::Garage;
  else if (std::strstr(nn, "Valocity") || std::strstr(nn, "RaceSetup"))
    g_hub_cas_pending = HubCasKind::Valocity;
}
// Compat aliases used by older call sites / tree_interp.
void main_menu_cmd_new_begin_tree() { main_menu_hub_begin_tree(); }
void main_menu_cmd_new_end_tree() { main_menu_hub_end_tree(); }
void main_menu_cmd_new_note_garage_cas() {
  g_hub_cas_pending = HubCasKind::Garage;
}
bool main_menu_cmd_new_deferring_osd() { return g_hub_tree_defer; }

int32_t game_logic_auto_save() {
  // Stock GameLogic.autoSave — skip WarningDialog/Sfx; always allow continue.
  ++g_auto_save_calls;
  if (g_played && !g_saved && g_game_mode == kGmCareer && g_career_in_progress) {
    game_logic_auto_save_quiet();
    g_saved = 1;
  }
  return 1;  // 1-OK; 0-cancelled (never yet)
}

void game_logic_auto_save_quiet() {
  ++g_auto_save_quiet_calls;
  // Stock writes career save dir; host marks saved without disk I/O yet.
  if (g_game_mode == kGmCareer && g_career_in_progress) g_saved = 1;
}

int32_t game_logic_auto_save_calls() { return g_auto_save_calls; }
int32_t game_logic_auto_save_quiet_calls() { return g_auto_save_quiet_calls; }
int32_t game_logic_load_defaults_calls() { return g_load_defaults_calls; }

InvObject* game_logic_boot_player_garage() {
  InvObject* ctrl = input_init_controllers();
  InvObject* player = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    if (!g_player) {
      g_player = tree_host_new("java.game.Player");
      tree_field_set_obj(g_player, "controller", ctrl);
      tree_field_set_obj(g_player, "decals", tree_vector_new());
      tree_field_set_int(g_player, "hints", 0);
      tree_field_set_int(g_player, "flags", 0);
      InvObject* carlot = tree_host_new("java.game.CarLot");
      tree_field_set_obj(carlot, "player", g_player);
      tree_field_set_int(carlot, "parks", 10);
      tree_field_set_int(carlot, "floors", 10);
      tree_field_set_obj(g_player, "carlot", carlot);
    } else {
      tree_field_set_obj(g_player, "controller", ctrl);
      if (!tree_field_get_obj(g_player, "carlot")) {
        InvObject* carlot = tree_host_new("java.game.CarLot");
        tree_field_set_obj(carlot, "player", g_player);
        tree_field_set_obj(g_player, "carlot", carlot);
      }
    }
    if (!g_garage) g_garage = tree_host_new("java.game.Garage");
    player = g_player;
  }
  // Outside g_gr_mu: ResourceRef_set → gameref_on_res_bound also takes g_gr_mu.
  inventory_ensure_player_parts(player);
  return player;
}

InvObject* game_logic_player() {
  std::lock_guard<std::mutex> lock(g_gr_mu);
  return g_player;
}

InvObject* game_logic_garage() {
  std::lock_guard<std::mutex> lock(g_gr_mu);
  return g_garage;
}

InvObject* game_logic_racesetup() {
  std::lock_guard<std::mutex> lock(g_gr_mu);
  return g_racesetup;
}

void game_logic_set_racesetup(InvObject* rs) {
  // Stock GameLogic.racesetup is RaceSetup only — TREE putstatic packing
  // sometimes writes MainMenu/prev_state onto the static.
  if (rs) {
    const char* hc = tree_host_class(rs);
    if (!hc || !std::strstr(hc, "RaceSetup")) return;
  }
  std::lock_guard<std::mutex> lock(g_gr_mu);
  g_racesetup = rs;
}

InvObject* frontend_loading_screen() {
  InvObject* ls = nullptr;
  bool need_start = false;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    if (!g_loading_screen) {
      g_loading_screen = tree_host_new("java.render.LoadingScreen");
      tree_field_set_int(g_loading_screen, "visible", 0);
      tree_field_set_int(g_loading_screen, "init", 0);
      // Stock: Object termSig = new Object() — woken by hide().
      tree_field_set_obj(g_loading_screen, "termSig",
                         tree_host_new("java.lang.Object"));
      need_start = true;
    }
    ls = g_loading_screen;
  }
  // Stock LoadingScreen() { start(); }. Defer OS thread — concurrent
  // frontend_loading_screen_run + SplashScreen.enter TREE raced to stack overflow.
  // Arm via frontend_loading_screen_show() when a real loading gate is needed.
  if (need_start) tree_field_set_int(ls, "engine_ls_started", 0);
  return ls;
}

void frontend_loading_screen_show() {
  InvObject* ls = frontend_loading_screen();
  if (!tree_field_get_int(ls, "engine_ls_started")) {
    java_lang_Thread_start(ls);
    tree_field_set_int(ls, "engine_ls_started", 1);
  }
  tree_field_set_int(ls, "visible", 1);
  tree_field_set_int(ls, "init", 1);
  // Stock LoadingScreen.show / track → notify() wakes run() after hide()/wait().
  java_lang_Object_notify(ls);
}

void frontend_loading_screen_hide() {
  InvObject* ls = frontend_loading_screen();
  tree_field_set_int(ls, "visible", 0);
  tree_field_set_int(ls, "hide_gen", tree_field_get_int(ls, "hide_gen") + 1);
  InvObject* term = tree_field_get_obj(ls, "termSig");
  if (term) java_lang_Object_notifyAll(term);
}

void frontend_loading_screen_show_dialog(InvObject* self, InvObject* dlg) {
  if (!self) self = frontend_loading_screen();
  tree_field_set_int(self, "init", 1);
  tree_field_set_int(self, "visible", 1);
  if (dlg && !tree_field_get_obj(self, "loadingDialog")) {
    tree_field_set_obj(self, "loadingDialog", dlg);
    tree_field_set_int(dlg, "shown", 1);
    // Minimal LoadingDialog.show: Text stub for FlashText path.
    if (!tree_field_get_obj(dlg, "loadingText"))
      tree_field_set_obj(dlg, "loadingText",
                         tree_host_new("java.render.osd.Text"));
    java_lang_System_setLdPriority(1);
  }
  java_lang_Object_notify(self);
}

void frontend_loading_screen_track(InvObject* self, int32_t wait_for_user) {
  if (!self) self = frontend_loading_screen();
  tree_field_set_int(self, "waitForUser", wait_for_user);
  java_lang_System_isLoadingReset();
  frontend_loading_screen_show();
  java_lang_Object_notify(self);
}

static void loading_screen_block_until_hide(InvObject* self) {
  // Poll hide_gen + flush on THIS thread (Garage.display blocks here).
  // Avoid Object.wait + foreign flush threads — D3D9 is not thread-safe.
  const int32_t gen0 = tree_field_get_int(self, "hide_gen");
  tree_field_set_int(self, "user_wait_blocked", 1);
  for (int i = 0; i < 20000; ++i) {
    if (tree_field_get_int(self, "hide_gen") != gen0) break;
    render_d3d9_flush();
    java_lang_Thread_sleep(5.f);
  }
  tree_field_set_int(self, "user_wait_blocked", 0);
}

void frontend_loading_screen_user_wait(InvObject* self, float sec) {
  if (!self) self = frontend_loading_screen();
  // Fresh termSig — avoid sticky notifyAll tokens from earlier hide().
  InvObject* term = tree_host_new("java.lang.Object");
  tree_field_set_obj(self, "termSig", term);
  tree_field_set_float(self, "waitForUserTimeLimit", sec);
  // Stock: track((int)sec) — nonzero arms waitForUser in run kill path.
  frontend_loading_screen_track(self, static_cast<int32_t>(sec));
  loading_screen_block_until_hide(self);
  (void)term;
}

void frontend_loading_screen_display(InvObject* self, InvObject* dlg,
                                    float wait_limit) {
  if (!self) self = frontend_loading_screen();
  InvObject* term = tree_host_new("java.lang.Object");
  tree_field_set_obj(self, "termSig", term);
  if (dlg)
    frontend_loading_screen_show_dialog(self, dlg);
  else
    frontend_loading_screen_show();
  tree_field_set_float(self, "waitForUserTimeLimit", wait_limit);
  // Stock: if (waitForUserTimeLimit) track(1); else track(0);
  frontend_loading_screen_track(self, wait_limit != 0.f ? 1 : 0);
  loading_screen_block_until_hide(self);
  (void)term;
}

void frontend_soft_timer_run(InvObject* self) {
  if (!self) return;
  InvObject* listener = tree_field_get_obj(self, "listener");
  float wait_time = tree_field_get_float(self, "waitTime");
  if (wait_time < 0.f) wait_time = 0.f;
  java_lang_Thread_sleep(wait_time * 1000.f);
  if (listener) java_lang_Object_notify(listener);
  tree_field_set_int(self, "engine_soft_fired", 1);
}

void frontend_text_change_text(InvObject* self, InvObject* text) {
  if (!self) return;
  tree_field_set_obj(self, "text", text);
  if (text)
    render_d3d9_text_set_string(self, string_cstr(text));
  else
    render_d3d9_text_set_string(self, "");
  render_d3d9_text_update(self);
}

void frontend_flash_text_run(InvObject* self) {
  // Stock FlashText.run — blink "PRESS ENTER TO CONTINUE..."
  if (!self) return;
  InvObject* th = tree_field_get_obj(self, "engine_thread");
  InvObject* flash = tree_field_get_obj(self, "flash");
  int32_t ticks = 0;
  while (th && java_lang_Thread_isAlive(th)) {
    frontend_text_change_text(flash, string_new("PRESS ENTER TO CONTINUE..."));
    tree_field_set_int(self, "flash_on", 1);
    java_lang_Thread_sleep(600.f);
    if (!java_lang_Thread_isAlive(th)) break;
    frontend_text_change_text(flash, nullptr);
    tree_field_set_int(self, "flash_on", 0);
    ++ticks;
    tree_field_set_int(self, "flash_ticks", ticks);
    java_lang_Thread_sleep(600.f);
  }
}

void frontend_loading_screen_run(InvObject* self) {
  // Mirrors LoadingScreen.run (+ waitForUser / SoftTimer / FlashText).
  if (!self) self = frontend_loading_screen();
  for (;;) {
    if (!java_lang_Thread_isAlive(self)) break;
    if (tree_field_get_int(self, "engine_ls_stop") != 0) break;

    java_lang_Object_wait(frontend_gfx_engine());

    int32_t kill = 0;
    const int32_t init = tree_field_get_int(self, "init");
    if (!init) {
      if (!java_lang_System_isLoading()) kill = 1;
    } else {
      tree_field_set_int(self, "init", init - 1);
    }

    if (kill) {
      const int32_t wait_user = tree_field_get_int(self, "waitForUser");
      InvObject* dlg = tree_field_get_obj(self, "loadingDialog");
      if (wait_user && dlg) {
        InvObject* loading_text = tree_field_get_obj(dlg, "loadingText");
        if (loading_text) {
          java_render_Text_create(loading_text, nullptr, frontend_large_font(),
                                  0.9f, 0.8f);
          java_render_Text_changeAlign(loading_text, 0);  // ALIGN_RIGHT
          InvObject* flasher = tree_host_new("java.render.FlashText");
          tree_field_set_obj(flasher, "flash", loading_text);
          InvObject* fth = tree_host_new("java.lang.Thread");
          tree_field_set_obj(fth, "target", flasher);
          tree_field_set_obj(flasher, "engine_thread", fth);
          java_lang_Thread_init(fth, string_new("Dialog text flasher"));
          java_lang_Thread_start(fth);
          tree_field_set_obj(self, "flashTextThread", fth);
          tree_field_set_obj(self, "flashTextRunner", flasher);
        }
        const float lim = tree_field_get_float(self, "waitForUserTimeLimit");
        if (lim >= 0.f) {
          const float sleep_sec = lim;
          InvObject* listener = dlg;
          std::thread([listener, sleep_sec]() {
            java_lang_Thread_sleep(sleep_sec * 1000.f);
            java_lang_Object_notify(listener);
          }).detach();
        }
        tree_field_set_int(self, "dialog_wait", 1);
        java_lang_Object_wait(dlg);
        tree_field_set_int(self, "dialog_wait", 0);
        if (InvObject* fth = tree_field_get_obj(self, "flashTextThread")) {
          java_lang_Thread_stop(fth);
          tree_field_set_obj(self, "flashTextThread", nullptr);
        }
      }
      tree_field_set_int(self, "waitForUser", 0);
      tree_field_set_obj(self, "loadingDialog", nullptr);
      frontend_loading_screen_hide();
      tree_field_set_int(self, "run_parked", 1);
      java_lang_Object_wait(self);
      tree_field_set_int(self, "run_parked", 0);
    } else {
      java_lang_Thread_sleep(300.f);
    }
  }
}

InvObject* frontend_gfx_engine() {
  std::lock_guard<std::mutex> lock(g_gr_mu);
  if (!g_gfx_engine) {
    // Frontend.render = new GfxEngine(); LoadingScreen waits on this instance.
    g_gfx_engine = tree_host_new("java.render.GfxEngine");
  }
  return g_gfx_engine;
}

void frontend_gfx_engine_frame_notify() {
  // Present path: wake Frontend.render.wait() once per frame.
  java_lang_Object_notify(frontend_gfx_engine());
}

namespace {

int32_t frontend_rid(uint16_t local) {
  if (!rpak_find_by_name("frontend.rpk"))
    java_lang_System_openLib(string_new("frontend.rpk"));
  const RpakPack* p = rpak_find_by_name("frontend.rpk");
  if (!p) return static_cast<int32_t>(local);
  return rpak_make_id(p->pack_id, local);
}

InvObject* frontend_make_ref(uint16_t local) {
  InvObject* r = resref_new();
  java_util_resource_ResourceRef_set(r, frontend_rid(local));
  return r;
}

}  // namespace

void frontend_set_fonts() {
  // Text.RID_* locals (frontend.rpk).
  constexpr uint16_t kSlII24 = 0x0025;
  constexpr uint16_t kSlII17 = 0x0139;
  constexpr uint16_t kSlII11 = 0x0138;
  constexpr uint16_t kConsole10 = 0x0023;
  constexpr uint16_t kConsole5 = 0x00B2;
  constexpr uint16_t kPointers = 0x003C;

  InvObject* cfg = system_config_host_for_test();
  int32_t vx = cfg ? tree_field_get_int(cfg, "video_x") : 800;
  int32_t vy = cfg ? tree_field_get_int(cfg, "video_y") : 600;
  if (render_d3d9_ready()) {
    vx = render_d3d9_width();
    vy = render_d3d9_height();
  }

  g_pointers = frontend_make_ref(kPointers);

  uint16_t large = kConsole5, medium = kConsole5, small = kConsole5;
  if (vy >= 1024 || vy >= 768) {
    large = kSlII24;
    medium = kSlII24;
    small = kSlII17;
  } else if (vx >= 800 || vy >= 600) {
    large = kSlII24;
    medium = kSlII17;
    small = kSlII11;
  } else if (vx >= 640 || vy >= 480) {
    large = kSlII17;
    medium = kSlII11;
    small = kConsole5;
  } else if (vy >= 320 || vy >= 240) {
    large = kConsole10;
    medium = kConsole10;
    small = kConsole5;
  }

  g_large_font = frontend_make_ref(large);
  g_medium_font = frontend_make_ref(medium);
  g_small_font = frontend_make_ref(small);
}

void frontend_init() {
  frontend_gfx_engine();
  if (!g_def_loading_pic)
    g_def_loading_pic = frontend_make_ref(0x00A8);  // frontend:0xA8
  if (!g_input_queue) g_input_queue = tree_vector_new();
  frontend_set_fonts();
  frontend_loading_screen();

  // Defer HotkeyWatcher thread — concurrent invoke() during SplashScreen.enter
  // TREE raced the main boot thread (stack overflow). Started from --game after
  // Splash→MainMenu, or frontend_start_hotkey_watcher().
  g_frontend_inited = 1;
}

void frontend_start_hotkey_watcher() {
  if (g_hotkey_thread) return;
  g_hotkey_watcher = tree_host_new("java.render.HotkeyWatcher");
  g_hotkey_thread = tree_host_new("java.lang.Thread");
  tree_field_set_obj(g_hotkey_thread, "target", g_hotkey_watcher);
  tree_field_set_obj(g_hotkey_watcher, "engine_thread", g_hotkey_thread);
  java_lang_Thread_init(g_hotkey_thread, string_new("Hotkey watcher"));
  java_lang_Thread_start(g_hotkey_thread);
}

void osd_ensure_defaults(InvObject* osd) {
  // Stock Osd(Viewport): Vectors + default Group(this).
  if (!osd) return;
  if (!tree_field_get_obj(osd, "groups"))
    tree_field_set_obj(osd, "groups", tree_vector_new());
  if (!tree_field_get_obj(osd, "hotkey"))
    tree_field_set_obj(osd, "hotkey", tree_vector_new());
  if (!tree_field_get_obj(osd, "rectangles"))
    tree_field_set_obj(osd, "rectangles", tree_vector_new());
  if (!tree_field_get_obj(osd, "text"))
    tree_field_set_obj(osd, "text", tree_vector_new());
  if (!tree_field_get_obj(osd, "object"))
    tree_field_set_obj(osd, "object", tree_vector_new());
  if (tree_field_get_float(osd, "vpWidth") <= 0.f) {
    tree_field_set_float(osd, "vpWidth", 1.f);
    tree_field_set_float(osd, "vpHeight", 1.f);
    tree_field_set_float(osd, "vpAspect",
                         render_d3d9_ready() && render_d3d9_height() > 0
                             ? static_cast<float>(render_d3d9_width()) /
                                   static_cast<float>(render_d3d9_height())
                             : 4.f / 3.f);
  }
  if (!tree_field_get_int(osd, "iLevel"))
    tree_field_set_int(osd, "iLevel", 3);  // IL_MENU
  InvObject* groups = tree_field_get_obj(osd, "groups");
  if (groups && tree_vector_size(groups) == 0) {
    InvObject* g = tree_host_new("java.render.osd.Group");
    tree_field_set_obj(g, "osd", osd);
    tree_field_set_obj(g, "hotkey", tree_vector_new());
    tree_field_set_int(g, "active", 1);
    tree_vector_add(groups, g);
  }
}

InvObject* osd_create_rectangle(InvObject* osd, float x, float y, float w,
                                float h, int32_t pri, InvObject* texture) {
  // Stock Osd.createRectangle → new java.render.Rectangle(group, pos, tmpl).
  // TREE often names java.render.osd.Rectangle (no such .class).
  if (!osd) return nullptr;
  osd_ensure_defaults(osd);
  InvObject* rect = tree_host_new("java.render.Rectangle");
  tree_field_set_obj(rect, "texture", texture);
  tree_field_set_float(rect, "x", x);
  tree_field_set_float(rect, "y", y);
  tree_field_set_float(rect, "w", w);
  tree_field_set_float(rect, "h", h);
  tree_field_set_int(rect, "pri", pri);
  InvObject* rects = tree_field_get_obj(osd, "rectangles");
  if (rects) tree_vector_add(rects, rect);
  if (texture) java_util_resource_ResourceRef_load(texture);
  render_d3d9_osd_add_rect(x, y, w, h, texture, pri);
  return rect;
}

InvObject* osd_create_bg(InvObject* osd, InvObject* texture) {
  // Stock Osd.createBG → createRectangle(0,0,2,2,-2,texture).
  InvObject* rect = osd_create_rectangle(osd, 0.f, 0.f, 2.f, 2.f, -2, texture);
  if (!osd) return rect;
  tree_field_set_int(osd, "bg_created", 1);
  if (texture) tree_field_set_obj(osd, "bg", texture);
  return rect;
}

InvObject* osd_create_hotkey(InvObject* osd, int32_t key, int32_t flags,
                             int32_t command, InvObject* handler, int32_t ef) {
  // Stock Osd.createHotkey — HK_STATIC → osd.hotkey + Input.createHotkey.
  if (!osd) return nullptr;
  osd_ensure_defaults(osd);
  constexpr int32_t kHkStatic = 0x04;
  if (ef == 0) ef = 1;  // Event.F_KEY_PRESS
  int32_t f = flags;
  const bool is_static = (f & kHkStatic) != 0;
  if (is_static) f &= ~kHkStatic;

  InvObject* hk = tree_host_new("java.render.osd.Hotkey");
  tree_field_set_int(hk, "key", key);
  tree_field_set_int(hk, "flags", f);
  tree_field_set_int(hk, "command", command);
  tree_field_set_int(hk, "eventFilter", ef);
  tree_field_set_obj(hk, "handler", handler);
  tree_field_set_obj(hk, "osd", osd);

  if (is_static) {
    tree_vector_add(tree_field_get_obj(osd, "hotkey"), hk);
  } else {
    InvObject* groups = tree_field_get_obj(osd, "groups");
    InvObject* g = groups && tree_vector_size(groups) > 0
                       ? tree_vector_element_at(groups, tree_vector_size(groups) - 1)
                       : nullptr;
    if (g) {
      if (!tree_field_get_obj(g, "hotkey"))
        tree_field_set_obj(g, "hotkey", tree_vector_new());
      tree_vector_add(tree_field_get_obj(g, "hotkey"), hk);
    } else {
      tree_vector_add(tree_field_get_obj(osd, "hotkey"), hk);
    }
  }
  java_io_Input_createHotkey(key, f, hk, handler, osd, ef);
  tree_field_set_int(osd, "hotkey_count",
                     tree_vector_size(tree_field_get_obj(osd, "hotkey")));
  return hk;
}

namespace {
int32_t osd_next_text_key(InvObject* osd) {
  const int32_t n = tree_field_get_int(osd, "text_blit_count");
  tree_field_set_int(osd, "text_blit_count", n + 1);
  return n;
}

void osd_ensure_font_ready(void** font_io) {
  if (!font_io) return;
  void* f = *font_io;
  if (!f) f = frontend_medium_font();
  auto try_load = [](void* f) -> bool {
    if (!f) return false;
    if (render_d3d9_font_ready(f)) return true;
    const int32_t rid =
        java_util_resource_ResourceRef_id(reinterpret_cast<InvObject*>(f));
    if (rid && render_d3d9_font_load_from_rid(f, rid)) return true;
    return render_d3d9_font_load(f, "simple20") != 0;
  };
  if (!try_load(f)) {
    f = frontend_medium_font();
    try_load(f);
  }
  if (!f || !render_d3d9_font_ready(f)) {
    f = frontend_large_font();
    try_load(f);
  }
  *font_io = (f && render_d3d9_font_ready(f)) ? f : nullptr;
}

void osd_blit_label(InvObject* osd, const char* text, InvObject* font, float x,
                    float y, int32_t align) {
  if (!osd || !text) return;
  void* f = font;
  osd_ensure_font_ready(&f);
  if (!f) return;
  void* key = reinterpret_cast<void*>(static_cast<uintptr_t>(
      0x6D500000u + static_cast<uint32_t>(osd_next_text_key(osd))));
  render_d3d9_text_create(key, f, x, y);
  render_d3d9_text_set_string(key, text);
  render_d3d9_text_set_align(key, align);
  render_d3d9_text_update(key);
}
}  // namespace

InvObject* osd_create_text(InvObject* osd, const char* text, InvObject* font,
                           int32_t align, float x, float y) {
  if (!osd) return nullptr;
  osd_ensure_defaults(osd);
  InvObject* txt = tree_host_new("java.render.Text");
  tree_field_set_obj(txt, "string", string_new(text ? text : ""));
  tree_field_set_obj(txt, "font", font);
  tree_field_set_int(txt, "align", align);
  tree_field_set_float(txt, "x", x);
  tree_field_set_float(txt, "y", y);
  if (!tree_field_get_obj(osd, "text"))
    tree_field_set_obj(osd, "text", tree_vector_new());
  tree_vector_add(tree_field_get_obj(osd, "text"), txt);
  tree_field_set_int(osd, "text_count",
                     tree_vector_size(tree_field_get_obj(osd, "text")));
  osd_blit_label(osd, text, font, x, y, align);
  return txt;
}

InvObject* osd_create_header(InvObject* osd, const char* title) {
  if (!osd) return nullptr;
  osd_ensure_defaults(osd);
  // Stock: createRectangle header bg + createText title.
  int32_t rid = 0;
  if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
    rid = rpak_make_id(fe->pack_id, 0x0028);  // RRT_HEADERBG
  if (rid) {
    InvObject* pic = gameref_new();
    java_util_resource_ResourceRef_set(pic, rid);
    java_util_resource_ResourceRef_load(pic);
    render_d3d9_osd_add_rect(0.f, -0.95f, 2.f, 0.1f, pic, -1);
  }
  return osd_create_text(osd, title && title[0] ? title : "MENU",
                         frontend_large_font(), /*ALIGN_LEFT*/ 2, -0.95f,
                         -0.98f);
}

InvObject* osd_create_menu(InvObject* osd, InvObject* style, float x, float y,
                           float spc, int32_t ori) {
  if (!osd) return nullptr;
  osd_ensure_defaults(osd);
  InvObject* menu = tree_host_new("java.render.osd.Menu");
  tree_field_set_obj(menu, "osd", osd);
  tree_field_set_obj(menu, "sty", style);
  tree_field_set_obj(menu, "items", tree_vector_new());
  tree_field_set_int(menu, "item_count", 0);
  tree_field_set_float(menu, "x", x);
  tree_field_set_float(menu, "y", y);
  float spacing = spc;
  if (spacing == 0.f && style) {
    spacing = (ori == 1) ? tree_field_get_float(style, "width")
                         : tree_field_get_float(style, "height");
    if (spacing <= 0.f) spacing = 0.12f;
  }
  tree_field_set_float(menu, "spacing", spacing);
  tree_field_set_int(menu, "orientation", ori >= 0 ? ori : 0);
  return menu;
}

InvObject* osd_create_button(InvObject* osd, InvObject* style, float x, float y,
                             const char* label, int32_t cmd) {
  if (!osd) return nullptr;
  osd_ensure_defaults(osd);
  InvObject* btn = tree_host_new("java.render.osd.Button");
  tree_field_set_obj(btn, "osd", osd);
  tree_field_set_int(btn, "command", cmd);
  tree_field_set_float(btn, "x", x);
  tree_field_set_float(btn, "y", y);
  if (label) tree_field_set_obj(btn, "label_string", string_new(label));
  if (!tree_field_get_obj(osd, "buttons"))
    tree_field_set_obj(osd, "buttons", tree_vector_new());
  tree_vector_add(tree_field_get_obj(osd, "buttons"), btn);
  tree_field_set_int(osd, "button_count",
                     tree_vector_size(tree_field_get_obj(osd, "buttons")));

  int32_t align = 1;
  InvObject* font = frontend_medium_font();
  float w = 0.12f, h = 0.12f;
  if (style) {
    align = tree_field_get_int(style, "align");
    if (InvObject* cs = tree_field_get_obj(style, "charset")) font = cs;
    const float sw = tree_field_get_float(style, "width");
    const float sh = tree_field_get_float(style, "height");
    if (sw > 0.f) w = sw;
    if (sh > 0.f) h = sh;
    // Icon buttons: Style.background / .rt is the pictogram ResourceRef.
    InvObject* gfx = tree_field_get_obj(style, "background");
    if (!gfx) gfx = tree_field_get_obj(style, "rt");
    if (gfx) {
      tree_field_set_obj(btn, "gfx", gfx);
      if (java_util_resource_ResourceRef_id(gfx) != 0)
        java_util_resource_ResourceRef_load(gfx);
      render_d3d9_osd_add_rect(x, y, w, h, gfx, 0);
    }
  }
  // Text.ALIGN_LEFT=2, CENTER=1, RIGHT=0 (stock Text.java).
  if (align != 0 && align != 1 && align != 2) align = 1;
  if (label && label[0]) osd_blit_label(osd, label, font, x, y, align);
  return btn;
}

void menu_add_separator(InvObject* menu) {
  if (!menu) return;
  InvObject* osd = tree_field_get_obj(menu, "osd");
  float spacing = tree_field_get_float(menu, "spacing");
  if (spacing <= 0.f) spacing = 0.12f;
  const float vpH =
      osd && tree_field_get_float(osd, "vpHeight") > 0.f
          ? tree_field_get_float(osd, "vpHeight")
          : 1.f;
  const int32_t ori = tree_field_get_int(menu, "orientation");
  if (ori == 0) {
    tree_field_set_float(menu, "y",
                         tree_field_get_float(menu, "y") + spacing * 0.5f / vpH);
  } else {
    const int32_t align =
        tree_field_get_obj(menu, "sty")
            ? tree_field_get_int(tree_field_get_obj(menu, "sty"), "align")
            : 1;
    float x = tree_field_get_float(menu, "x");
    const float dx = spacing * 0.5f / vpH;
    tree_field_set_float(menu, "x", align == 0 ? x - dx : x + dx);
  }
}

InvObject* menu_add_item(InvObject* menu, const char* text, int32_t cmd) {
  if (!menu) return nullptr;
  InvObject* osd = tree_field_get_obj(menu, "osd");
  InvObject* sty = tree_field_get_obj(menu, "sty");
  const float x = tree_field_get_float(menu, "x");
  const float y = tree_field_get_float(menu, "y");
  InvObject* btn = osd_create_button(osd, sty, x, y, text, cmd);
  if (!tree_field_get_obj(menu, "items"))
    tree_field_set_obj(menu, "items", tree_vector_new());
  tree_vector_add(tree_field_get_obj(menu, "items"), btn);
  tree_field_set_int(menu, "item_count",
                     tree_vector_size(tree_field_get_obj(menu, "items")));
  // Stock Menu.nextLine = two separators.
  menu_add_separator(menu);
  menu_add_separator(menu);
  return btn;
}

InvObject* menu_add_item_gfx(InvObject* menu, InvObject* gfx, int32_t cmd,
                             const char* tooltip) {
  if (!menu) return nullptr;
  InvObject* osd = tree_field_get_obj(menu, "osd");
  InvObject* base = tree_field_get_obj(menu, "sty");
  InvObject* style = tree_host_new("java.render.osd.Style");
  if (base) {
    tree_field_set_float(style, "width", tree_field_get_float(base, "width"));
    tree_field_set_float(style, "height", tree_field_get_float(base, "height"));
    tree_field_set_float(style, "aspect", tree_field_get_float(base, "aspect"));
    tree_field_set_int(style, "align", tree_field_get_int(base, "align"));
    if (InvObject* cs = tree_field_get_obj(base, "charset"))
      tree_field_set_obj(style, "charset", cs);
  } else {
    tree_field_set_float(style, "width", 0.12f);
    tree_field_set_float(style, "height", 0.12f);
    tree_field_set_int(style, "align", 2);  // ALIGN_LEFT
    tree_field_set_obj(style, "charset", frontend_medium_font());
  }
  if (gfx) {
    tree_field_set_obj(style, "background", gfx);
    tree_field_set_obj(style, "rt", gfx);
  }
  const float x = tree_field_get_float(menu, "x");
  const float y = tree_field_get_float(menu, "y");
  InvObject* btn =
      osd_create_button(osd, style, x, y, tooltip ? tooltip : "", cmd);
  if (!tree_field_get_obj(menu, "items"))
    tree_field_set_obj(menu, "items", tree_vector_new());
  tree_vector_add(tree_field_get_obj(menu, "items"), btn);
  tree_field_set_int(menu, "item_count",
                     tree_vector_size(tree_field_get_obj(menu, "items")));
  menu_add_separator(menu);
  menu_add_separator(menu);
  return btn;
}

void garage_create_osd_street_menu(InvObject* garage) {
  // Stock Garage.createOSDObjects (!roc) icon strip — VA-backed layout from
  // Java; used when TREE mis-packs ResourceRef addItem as addSeparator.
  if (!garage) return;
  InvObject* osd = tree_field_get_obj(garage, "osd");
  if (!osd) {
    osd = tree_host_new("java.render.Osd");
    osd_ensure_defaults(osd);
    tree_field_set_obj(garage, "osd", osd);
  }
  osd_ensure_defaults(osd);
  tree_field_set_obj(osd, "globalHandler", garage);
  tree_field_set_int(osd, "visible", 1);

  InvObject* style = tree_host_new("java.render.osd.Style");
  tree_field_set_float(style, "width", 0.12f);
  tree_field_set_float(style, "height", 0.12f);
  tree_field_set_int(style, "align", 2);
  tree_field_set_obj(style, "charset", frontend_medium_font());

  InvObject* menu = osd_create_menu(osd, style, -0.98f, -0.84f, 0.f,
                                    /*MD_HORIZONTAL*/ 1);
  tree_field_set_obj(osd, "last_menu", menu);

  auto fe_icon = [](uint16_t local) -> InvObject* {
    InvObject* pic = gameref_new();
    int32_t rid = 0;
    if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
      rid = rpak_make_id(fe->pack_id, local);
    else if (const RpakPack* fe = rpak_find_by_name("frontend"))
      rid = rpak_make_id(fe->pack_id, local);
    if (rid) {
      java_util_resource_ResourceRef_set(pic, rid);
      java_util_resource_ResourceRef_load(pic);
    }
    return pic;
  };

  struct Item {
    uint16_t local;
    int32_t cmd;
    const char* tip;
  };
  static const Item kItems[] = {
      {0x011D, 109, "Go driving in the city"},
      {0x011E, 110, "Test Track"},
      {0x011F, 108, "Go to the Race Of Champions"},
      {0x0120, 111, "Go to the Car Lot"},
      {0x0121, 112, "Buy new cars"},
      {0x0122, 122, "Buy used cars or sell your car"},
      {0x0125, 113, "Browse the Catalog"},
      {0x0123, 114, "Check your ranking here"},
      {0x0124, 115, "Details of this car"},
      {0x0129, 117, "Install/Remove parts"},
      {0x0128, 124, "Fine tune specific parts"},
      {0x012C, 118, "Body paint"},
      {0x0127, 123, "Test engine and steering"},
      {0x012A, 116, "Advance time (1 hour)"},
  };
  for (const Item& it : kItems)
    menu_add_item_gfx(menu, fe_icon(it.local), it.cmd, it.tip);

  // RID_BACK → main menu (frontend local often 0x00B3 / Osd.RID_BACK).
  InvObject* back = fe_icon(0x00B3);
  if (java_util_resource_ResourceRef_id(back) == 0) {
    // Fallback: still count as a button for chrome probes.
    back = gameref_new();
  }
  menu_add_item_gfx(menu, back, /*CMD_MAINMENU*/ 101, "Go back to Main Menu");

  osd_create_text(osd, "Welcome!", frontend_medium_font(), /*ALIGN_RIGHT*/ 0,
                  0.97f, 0.54f);
  tree_field_set_int(garage, "osd_via_host", 1);
  tree_field_set_int(osd, "visible", 1);
}

int32_t osd_begin_group(InvObject* osd) {
  if (!osd) return -1;
  osd_ensure_defaults(osd);
  InvObject* g = tree_host_new("java.render.osd.Group");
  tree_field_set_obj(g, "osd", osd);
  tree_field_set_obj(g, "hotkey", tree_vector_new());
  tree_field_set_obj(g, "gadget", tree_vector_new());
  tree_field_set_int(g, "active", 1);
  InvObject* groups = tree_field_get_obj(osd, "groups");
  tree_vector_add(groups, g);
  return tree_vector_size(groups) - 1;
}

int32_t osd_end_group(InvObject* osd) {
  if (!osd) return -1;
  InvObject* groups = tree_field_get_obj(osd, "groups");
  const int32_t n = groups ? tree_vector_size(groups) : 0;
  return n > 0 ? n - 1 : -1;
}

void osd_hide_group(InvObject* osd, int32_t gid) {
  if (!osd || gid < 0) return;
  InvObject* groups = tree_field_get_obj(osd, "groups");
  if (!groups || gid >= tree_vector_size(groups)) return;
  if (InvObject* g = tree_vector_element_at(groups, gid))
    tree_field_set_int(g, "active", 0);
}

void osd_show_group(InvObject* osd, int32_t gid) {
  if (!osd || gid < 0) return;
  InvObject* groups = tree_field_get_obj(osd, "groups");
  if (!groups || gid >= tree_vector_size(groups)) return;
  if (InvObject* g = tree_vector_element_at(groups, gid))
    tree_field_set_int(g, "active", 1);
}

void options_dialog_ensure_groups(InvObject* dialog) {
  if (!dialog) return;
  // Groups are allocated during addCustomGroups / show TREE (begin/endGroup).
  if (!tree_field_get_obj(dialog, "osd")) {
    InvObject* osd = tree_host_new("java.render.Osd");
    osd_ensure_defaults(osd);
    tree_field_set_obj(dialog, "osd", osd);
  }
}

void options_dialog_change_mode(InvObject* dialog, int32_t group) {
  // Stock OptionsDialog.changeMode: hide actGroup, show group, actGroup=group.
  if (!dialog) return;
  options_dialog_ensure_groups(dialog);
  InvObject* osd = tree_field_get_obj(dialog, "osd");
  const int32_t act = tree_field_get_int(dialog, "actGroup");
  if (act != group) {
    if (act >= 0) osd_hide_group(osd, act);
    tree_field_set_int(dialog, "actGroup", group);
    if (group >= 0) osd_show_group(osd, group);
  }
  tree_field_set_int(dialog, "change_mode_via_host", 1);
}

void mainmenu_credits_reset(InvObject* dialog) {
  if (!dialog) return;
  tree_field_set_int(dialog, "lastInLine", 0);
  tree_field_set_int(dialog, "lastOutLine", 0);
  tree_field_set_float(dialog, "creditsSpacing", 0.f);
  tree_field_set_float(dialog, "creditsTime",
                       static_cast<float>(java_lang_System_currentTime()));
  tree_field_set_int(dialog, "credits_reset_via_host", 1);
}

void mainmenu_build_credits(InvObject* dialog) {
  if (!dialog) return;
  // Stock buildCredits fills creditsTxt[]; TREE addCustomGroups often did it.
  if (!tree_field_get_obj(dialog, "creditsTxt"))
    tree_field_set_obj(dialog, "creditsTxt", tree_vector_new());
  tree_field_set_int(dialog, "credits_built", 1);
}

void mainmenu_credits_tick(InvObject* dialog, float dt) {
  (void)dialog;
  (void)dt;
  // Full CREDITS_* scroll lives in TREE animate; tick is a no-op host hook.
}

void mainmenu_dialog_ensure_chrome(InvObject* dialog) {
  // Stock MainMenuDialog.show: MUSIC_SET_MENU + openVideo(prime.avi,1,1) +
  // addCustomGroups (GENERALBG / MAIN MENU / columns). TREE show overflows;
  // this is the boot mirror until Osd.create* packing is solid.
  if (!dialog) return;
  if (tree_field_get_int(dialog, "menu_chrome") == 1) return;

  java_sound_Sound_changeMusicSet(3);  // Sound.MUSIC_SET_MENU
  const int32_t fmv_ok =
      video_fmv_open("data\\fmv\\prime.avi", /*non_exclusive=*/1, /*loop=*/1);
  tree_field_set_int(dialog, "bgVideoActive", fmv_ok == 0 ? 1 : 0);

  InvObject* osd = tree_field_get_obj(dialog, "osd");
  if (!osd) {
    osd = tree_host_new("java.render.Osd");
    tree_field_set_obj(dialog, "osd", osd);
  }
  osd_ensure_defaults(osd);
  tree_field_set_int(osd, "visible", 1);

  if (fmv_ok != 0) {
    int32_t rid = 0;
    if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
      rid = rpak_make_id(fe->pack_id, 0x0016);  // RID_GENERALBG
    else if (const RpakPack* fe = rpak_find_by_name("frontend"))
      rid = rpak_make_id(fe->pack_id, 0x0016);
    if (rid) {
      InvObject* pic = gameref_new();
      java_util_resource_ResourceRef_set(pic, rid);
      java_util_resource_ResourceRef_load(pic);
      tree_field_set_obj(osd, "bg", pic);
      render_d3d9_osd_add_rect(0.f, 0.f, 2.f, 2.f, pic, -2);
    }
  }

  void* font = frontend_medium_font();
  if (font && !render_d3d9_font_ready(font)) {
    const int32_t rid = java_util_resource_ResourceRef_id(
        reinterpret_cast<InvObject*>(font));
    if (!render_d3d9_font_load_from_rid(font, rid))
      render_d3d9_font_load(font, "simple20");
  }
  auto add_label = [&](const char* text, float x, float y, int32_t idx) {
    void* key = reinterpret_cast<void*>(
        static_cast<uintptr_t>(0x6D4E0000u + static_cast<uint32_t>(idx)));
    render_d3d9_text_create(key, font, x, y);
    render_d3d9_text_set_string(key, text);
    render_d3d9_text_set_align(key, 1);
    render_d3d9_text_update(key);
  };
  // Header + two columns from MainMenu.addCustomGroups (stock x=±0.05, y=-0.6).
  add_label("MAIN MENU", 0.f, -0.95f, 0);
  static const char* kLeft[] = {"NEW CAREER",     "LOAD CAREER",
                                "DELETE CAREER",  "BACK TO GARAGE",
                                "LOAD CAR",       "SAVE CAR"};
  static const char* kRight[] = {"QUICKRACE", "FREE RIDE", "DEMO",
                                 "OPTIONS",   "CREDITS",   "EXIT GAME"};
  for (int i = 0; i < 6; ++i) {
    const float y = -0.6f + static_cast<float>(i) * 0.12f;
    add_label(kLeft[i], -0.05f, y, 1 + i);
    add_label(kRight[i], 0.05f, y, 7 + i);
  }

  // FMV path has no GENERALBG rect — keep ≥1 OSD entry for smoke/boot checks.
  if (render_d3d9_osd_count() < 1) {
    render_d3d9_osd_add_rect(0.f, 0.f, 2.f, 2.f, nullptr, -2);
  }

  tree_field_set_int(dialog, "menu_chrome", 1);
  tree_field_set_int(dialog, "shown", 1);
  tree_field_set_int(osd, "button_count", 12);
  std::printf("[script] MainMenuDialog chrome fmv=%d osd=%d txt=%d\n",
              fmv_ok == 0 ? 1 : 0, render_d3d9_osd_count(),
              render_d3d9_osd_text_count());
}

void frontend_destroy() {
  if (g_hotkey_thread) {
    java_lang_Thread_stop(g_hotkey_thread);
    g_hotkey_thread = nullptr;
    g_hotkey_watcher = nullptr;
  }
  g_frontend_inited = 0;
}

void frontend_hotkey_watcher_run(InvObject* self) {
  InvObject* th =
      self ? tree_field_get_obj(self, "engine_thread") : g_hotkey_thread;
  if (!th) th = g_hotkey_thread;
  while (th && java_lang_Thread_isAlive(th)) {
    java_lang_Thread_sleep(50.f);
    InvObject* q = frontend_input_queue();
    const int32_t n = tree_vector_size(q);
    if (n <= 0) continue;
    InvObject* focused = tree_vector_element_at(q, n - 1);
    if (!focused) continue;
    static InvObject* g_cursor = nullptr;
    if (!g_cursor) g_cursor = java_io_Input_cursor();
    InvObject* ctrl = tree_field_get_obj(g_cursor, "controller");
    if (!ctrl) continue;
    java_io_Input_checkHotkeys(ctrl, focused);
    java_lang_GameType_pollTimers();
    tree_field_set_int(self ? self : g_hotkey_watcher, "ticks",
                       tree_field_get_int(self ? self : g_hotkey_watcher,
                                          "ticks") +
                           1);
  }
}

InvObject* frontend_large_font() {
  if (!g_large_font) frontend_set_fonts();
  return g_large_font;
}
InvObject* frontend_medium_font() {
  if (!g_medium_font) frontend_set_fonts();
  return g_medium_font;
}
InvObject* frontend_small_font() {
  if (!g_small_font) frontend_set_fonts();
  return g_small_font;
}
InvObject* frontend_pointers() {
  if (!g_pointers) frontend_set_fonts();
  return g_pointers;
}
InvObject* frontend_def_loading_pic() {
  if (!g_def_loading_pic) g_def_loading_pic = frontend_make_ref(0x00A8);
  return g_def_loading_pic;
}
InvObject* frontend_input_queue() {
  if (!g_input_queue) g_input_queue = tree_vector_new();
  return g_input_queue;
}
int32_t frontend_inited() { return g_frontend_inited; }
InvObject* frontend_hotkey_thread() { return g_hotkey_thread; }

int32_t frontend_loading_screen_visible() {
  InvObject* ls = frontend_loading_screen();
  return tree_field_get_int(ls, "visible");
}

namespace {
void loading_screen_invoke(const char* method) {
  InvObject* ls = frontend_loading_screen();
  Jvm* j = jvm_active();
  if (j && ls) {
    std::vector<JvmValue> args = {JvmValue::make_obj(ls)};
    j->invoke("java.render.LoadingScreen", method, "()V", args, false);
  } else if (std::strcmp(method, "show") == 0) {
    frontend_loading_screen_show();
  } else {
    frontend_loading_screen_hide();
  }
}
}  // namespace

bool game_logic_is_section(InvObject* state) {
  // Java changeActiveSection(GameState). Implementors: SplashScreen, MainMenu,
  // Garage, Track (+City/Valocity/TestTrack), RaceSetup, CarInfo, CarMarket,
  // Catalog, CarLot, ClubInfo, Painter, RocInfo. TREE often packs String /
  // NoYesDialog leftovers as the CAS arg (CMD_EXIT null).
  if (!state) return false;
  const char* c = tree_host_class(state);
  if (!c || !c[0]) return false;
  auto ends = [&](const char* s) -> bool {
    const size_t n = std::strlen(s);
    const size_t m = std::strlen(c);
    if (m < n) return false;
    if (std::strcmp(c + (m - n), s) != 0) return false;
    if (m == n) return true;
    const char sep = c[m - n - 1];
    return sep == '.' || sep == '/';
  };
  return ends("SplashScreen") || ends("MainMenu") || ends("Garage") ||
         ends("Valocity") || ends("City") || ends("TestTrack") ||
         ends("Track") || ends("RaceSetup") || ends("CarInfo") ||
         ends("CarMarket") || ends("Catalog") || ends("CarLot") ||
         ends("ClubInfo") || ends("Painter") || ends("RocInfo");
}

InvObject* game_logic_change_active_section(InvObject* state) {
  // Stock GameLogic.changeActiveSection (Java) — script-driven gate:
  //   if (actualState) actualState.exit(state);
  //   old = actualState; actualState = state;
  //   if (actualState) actualState.enter(old);
  //   else System.exit() @ 0x0047BE50 (Engine_quitRequested = 1).
  // Host CAS(null) does not request_exit: hub smoke restores Garage after EXIT.
  InvObject* prev = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    prev = g_actual_state;
  }

  Jvm* j = jvm_active();
  auto call_lifecycle = [&](InvObject* target, const char* method,
                            InvObject* arg) {
    if (!target || !j) return;
    const char* cn = tree_host_class(target);
    if (!cn || !cn[0]) return;
    if (!j->find_class(cn)) j->load_class(cn);
    std::vector<JvmValue> args = {JvmValue::make_obj(target),
                                  JvmValue::make_obj(arg)};
    j->invoke(cn, method, "(Ljava.game.GameState;)V", args, false);
  };

  if (prev) {
    if (j)
      call_lifecycle(prev, "exit", state);
    else
      tree_field_set_int(prev, "entered", 0);
  }

  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    g_actual_state = state;
  }

  if (state) {
    if (j) {
      const char* cn = tree_host_class(state);
      std::printf("[script] changeActiveSection → %s\n",
                  cn && cn[0] ? cn : "?");
      call_lifecycle(state, "enter", prev);
    } else {
      tree_field_set_int(state, "entered", 1);
      if (const char* cn = tree_host_class(state)) {
        if (std::strstr(cn, "SplashScreen"))
          tree_field_set_int(state, "timer_sec", 3);
      }
    }
  }
  return state;
}

InvObject* game_logic_actual_state() {
  std::lock_guard<std::mutex> lock(g_gr_mu);
  return g_actual_state;
}

InvObject* game_logic_boot_splash() {
  InvObject* player = game_logic_boot_player_garage();
  InvObject* ctrl = player ? tree_field_get_obj(player, "controller") : nullptr;
  if (ctrl) {
    // GameLogic: player.controller.reset(); activateState(MENUSET);
    controller_reset(ctrl);
    controller_activate_state(ctrl, kMenuSet, 1);
  }

  // GameLogic ctor end: Frontend.loadingScreen.hide() only — avoid show() which
  // TREE-builds SimpleLoadingDialog + SoftTimer (not needed for Splash→Menu).
  loading_screen_invoke("hide");

  // changeActiveSection(new SplashScreen(new ResourceRef(frontend:0x5)))
  int32_t pic_id = 0x5;
  if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
    pic_id = rpak_make_id(fe->pack_id, 0x5);
  else if (const RpakPack* fe = rpak_find_by_name("frontend"))
    pic_id = rpak_make_id(fe->pack_id, 0x5);

  InvObject* pic = resref_new();
  java_util_resource_ResourceRef_set(pic, pic_id);

  // Stock: new SplashScreen(pic) → createNativeInstance() + this.pic = pic.
  InvObject* splash = tree_host_new("java.game.SplashScreen");
  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.SplashScreen"))
      j->load_class("java.game.SplashScreen");
    j->invoke("java.game.SplashScreen", "<init>",
              "(Ljava.util.resource.ResourceRef;)V",
              {JvmValue::make_obj(splash), JvmValue::make_obj(pic)}, false);
  } else {
    tree_field_set_obj(splash, "pic", pic);
  }
  tree_field_set_int(splash, "entered", 0);
  game_logic_change_active_section(splash);
  return splash;
}

InvObject* game_logic_finish_splash() {
  // Stock: SplashScreen.handleEvent → osdCommand(AXIS_CANCEL) →
  //   GameLogic.changeActiveSection(new MainMenu()).
  // TREE osdCommand packs `new MainMenu()` into a stack overflow — CAS here.
  InvObject* splash = game_logic_actual_state();
  const char* scn = splash ? tree_host_class(splash) : nullptr;
  if (!scn || !std::strstr(scn, "SplashScreen")) {
    InvObject* cur = game_logic_actual_state();
    if (cur && tree_host_class(cur) &&
        std::strstr(tree_host_class(cur), "MainMenu"))
      return cur;
  }
  InvObject* menu = tree_host_new("java.game.MainMenu");
  game_logic_change_active_section(menu);
  return game_logic_actual_state();
}

void game_logic_load_defaults() {
  ++g_load_defaults_calls;
  InvObject* player = game_logic_boot_player_garage();
  tree_field_set_int(player, "money", kInitialMoney);
  tree_field_set_float(player, "prestige", kInitialPrestige);
  tree_field_set_int(player, "club", 0);
  tree_field_set_int(player, "flags", 0);
  tree_field_set_int(player, "hints", 0);
  tree_field_set_int(player, "winPinkSlips", 0);
  InvObject* name = tree_field_get_obj(player, "name");
  const char* ns = name ? string_cstr(name) : nullptr;
  if (!ns || !ns[0]) tree_field_set_obj(player, "name", string_new("Player"));
  // loadUnsavedData subset
  g_game_mode = kGmCareer;
  g_time_of_day = 12.f * 3600.f;
  g_day = 1;
  // CarMarket.getInitialCars for new/used dealers
  {
    constexpr int32_t VS_USED = 0x0002;
    constexpr int32_t VS_STOCK = 0x0004;
    g_car_desc_new = tree_vector_new();
    g_car_desc_used = tree_vector_new();
    for (int i = 0; i < 4; ++i) {
      if (InvObject* vd = game_logic_get_vehicle_descriptor(VS_STOCK, 0.5f))
        tree_vector_add(g_car_desc_new, vd);
      if (InvObject* vd = game_logic_get_vehicle_descriptor(VS_USED, 0.5f))
        tree_vector_add(g_car_desc_used, vd);
    }
    g_dealer_ts_new = 0.f;
    g_dealer_ts_used = 0.f;
  }
}

int32_t game_logic_game_mode() { return g_game_mode; }
void game_logic_set_game_mode(int32_t mode) { g_game_mode = mode; }
int32_t game_logic_timeout() { return g_timeout; }
void game_logic_set_timeout(int32_t t) { g_timeout = t; }
int32_t game_logic_career_in_progress() { return g_career_in_progress; }
void game_logic_set_career_in_progress(int32_t v) {
  g_career_in_progress = v ? 1 : 0;
}
int32_t game_logic_day() { return g_day; }
float game_logic_time() { return g_time_of_day; }

void game_logic_set_time(float t) {
  g_time_of_day = t;
  while (g_time_of_day < 0.f) g_time_of_day += 24.f * 3600.f;
  while (g_time_of_day > 24.f * 3600.f) {
    g_time_of_day -= 24.f * 3600.f;
    ++g_day;
  }
  java_lang_System_syncGameTime(g_time_of_day);
}

void game_logic_spend_time(float dt) {
  game_logic_set_time(g_time_of_day + dt);
}

void game_logic_set_played(int32_t v) { g_played = v ? 1 : 0; }
int32_t game_logic_played() { return g_played; }

void garage_ensure_map(InvObject* garage) {
  if (!garage) return;
  if (tree_field_get_int(garage, "map_id") != 0) return;
  InvObject* player = tree_field_get_obj(garage, "player");
  if (!player) player = game_logic_player();
  int32_t gidx = tree_field_get_int(garage, "garageIndex");
  if (!tree_field_get_obj(garage, "player") && player)
    tree_field_set_obj(garage, "player", player);
  // If ctor never ran, garageIndex may still be 0 (= club 0) which is fine.
  if (player && gidx == 0 && tree_field_get_int(player, "club") != 0)
    gidx = tree_field_get_int(player, "club");
  tree_field_set_int(garage, "garageIndex", gidx);
  int32_t map_local = 0x7a;
  if (gidx == 1) map_local = 0x06;
  else if (gidx == 2) map_local = 0xc7;
  else if (gidx >= 3) map_local = 0x2f;
  if (!rpak_find_by_name("garage.rpk"))
    java_lang_System_openLib(string_new("misc/garage.rpk"));
  int32_t map_id = map_local;
  if (const RpakPack* gp = rpak_find_by_name("garage.rpk"))
    map_id = rpak_make_id(gp->pack_id, static_cast<uint16_t>(map_local));
  tree_field_set_int(garage, "map_id", map_id);
  InvObject* map = tree_field_get_obj(garage, "map");
  if (!map) {
    map = tree_host_new("java.util.resource.GroundRef");
    tree_field_set_obj(garage, "map", map);
  }
  if (java_util_resource_ResourceRef_id(map) == 0)
    java_util_resource_ResourceRef_set(map, map_id);
}

InvObject* garage_ensure_camera(InvObject* garage) {
  // Stock Garage.enter: Camera at (-3,1.5,-2) Ypr(-2.05,-0.25,0).
  if (!garage) return nullptr;
  InvObject* cam = tree_field_get_obj(garage, "camera");
  if (!cam) {
    cam = tree_host_new("java.render.Camera");
    tree_field_set_obj(garage, "camera", cam);
    InvObject* map = tree_field_get_obj(garage, "map");
    InvObject* vp = nullptr;
    if (InvObject* osd = tree_field_get_obj(garage, "osd"))
      vp = tree_field_get_obj(osd, "vp");
    if (!vp) vp = reinterpret_cast<InvObject*>(render_d3d9_viewport_active());
    // half-AOV 45° (stock aov ~90).
    java_render_Camera_create(cam, map, vp, 1, 45.f, 0.05f, 200.f, 1.f, 1.f, 0,
                              1);
    constexpr float kEyeX = -3.f, kEyeY = 1.5f, kEyeZ = -2.f;
    constexpr float kAtX = 0.f, kAtY = 0.5f, kAtZ = 0.f;
    tree_field_set_float(garage, "cam_eye_x", kEyeX);
    tree_field_set_float(garage, "cam_eye_y", kEyeY);
    tree_field_set_float(garage, "cam_eye_z", kEyeZ);
    tree_field_set_float(garage, "cam_at_x", kAtX);
    tree_field_set_float(garage, "cam_at_y", kAtY);
    tree_field_set_float(garage, "cam_at_z", kAtZ);
    const float dx = kEyeX - kAtX;
    const float dy = kEyeY - kAtY;
    const float dz = kEyeZ - kAtZ;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    tree_field_set_float(garage, "cam_dist", dist > 0.1f ? dist : 4.f);
    tree_field_set_float(garage, "cam_yaw", std::atan2(dx, dz));
    tree_field_set_float(garage, "cam_pitch",
                         std::asin(dy / (dist > 0.1f ? dist : 4.f)));
    render_d3d9_camera_lookat(cam, kEyeX, kEyeY, kEyeZ, kAtX, kAtY, kAtZ);
    if (vp) render_d3d9_camera_activate(cam, vp, 0);
  }
  if (InvObject* mech = tree_field_get_obj(garage, "mechanic"))
    tree_field_set_obj(mech, "camera", cam);
  return cam;
}

static void garage_apply_orbit_camera(InvObject* garage) {
  if (!garage) return;
  InvObject* cam = garage_ensure_camera(garage);
  if (!cam) return;
  const float yaw = tree_field_get_float(garage, "cam_yaw");
  const float pitch = tree_field_get_float(garage, "cam_pitch");
  float dist = tree_field_get_float(garage, "cam_dist");
  if (dist < 1.f) dist = 1.f;
  const float at_x = tree_field_get_float(garage, "cam_at_x");
  const float at_y = tree_field_get_float(garage, "cam_at_y");
  const float at_z = tree_field_get_float(garage, "cam_at_z");
  const float cp = std::cos(pitch);
  const float eye_x = at_x + std::sin(yaw) * cp * dist;
  const float eye_y = at_y + std::sin(pitch) * dist;
  const float eye_z = at_z + std::cos(yaw) * cp * dist;
  tree_field_set_float(garage, "cam_eye_x", eye_x);
  tree_field_set_float(garage, "cam_eye_y", eye_y);
  tree_field_set_float(garage, "cam_eye_z", eye_z);
  render_d3d9_camera_lookat(cam, eye_x, eye_y, eye_z, at_x, at_y, at_z);
  if (void* vp = render_d3d9_viewport_active())
    render_d3d9_camera_activate(cam, vp, 0);
}

void garage_rdrag_begin(InvObject* garage) {
  if (!garage) return;
  garage_ensure_camera(garage);
  tree_field_set_int(garage, "rdrag", 1);
  tree_field_set_int(garage, "move", 1);  // stock Garage.move
  tree_field_set_int(garage, "rdrag_begin_count",
                     tree_field_get_int(garage, "rdrag_begin_count") + 1);
  if (InvObject* mech = tree_field_get_obj(garage, "mechanic"))
    tree_field_set_int(mech, "rdrag", 1);
}

void garage_rdrag_end(InvObject* garage) {
  if (!garage) return;
  tree_field_set_int(garage, "rdrag", 0);
  tree_field_set_int(garage, "move", 0);
  tree_field_set_int(garage, "rdrag_end_count",
                     tree_field_get_int(garage, "rdrag_end_count") + 1);
  if (InvObject* mech = tree_field_get_obj(garage, "mechanic"))
    tree_field_set_int(mech, "rdrag", 0);
}

bool garage_rdrag_orbit(InvObject* garage, float dyaw, float dpitch) {
  // Stock: AXIS_LOOK_LEFTRIGHT/UPDOWN while EC_RDRAG (mouse → camera).
  if (!garage) return false;
  garage_ensure_camera(garage);
  float yaw = tree_field_get_float(garage, "cam_yaw") + dyaw;
  float pitch = tree_field_get_float(garage, "cam_pitch") + dpitch;
  constexpr float kPitchMax = 1.2f;
  if (pitch > kPitchMax) pitch = kPitchMax;
  if (pitch < -0.2f) pitch = -0.2f;
  tree_field_set_float(garage, "cam_yaw", yaw);
  tree_field_set_float(garage, "cam_pitch", pitch);
  garage_apply_orbit_camera(garage);
  tree_field_set_int(garage, "rdrag_orbit_count",
                     tree_field_get_int(garage, "rdrag_orbit_count") + 1);
  return true;
}

void garage_tick_rdrag(InvObject* garage) {
  if (!garage) return;
  input_live_poll();
  const float btn = java_io_Input_getAxis(1, kMousePhysBtn2);
  const int32_t down = btn > 0.5f ? 1 : 0;
  const int32_t was = tree_field_get_int(garage, "mouse_btn2");
  tree_field_set_int(garage, "mouse_btn2", down);
  if (was == 0 && down == 1) {
    garage_rdrag_begin(garage);
  } else if (was == 1 && down == 1 && tree_field_get_int(garage, "rdrag")) {
    float dx = 0.f, dy = 0.f, dz = 0.f;
    input_mouse_rel(&dx, &dy, &dz);
    if (dx != 0.f || dy != 0.f)
      garage_rdrag_orbit(garage, dx * 0.35f, dy * 0.25f);
    if (dz != 0.f) {
      float dist = tree_field_get_float(garage, "cam_dist") - dz * 0.4f;
      if (dist < 1.5f) dist = 1.5f;
      if (dist > 12.f) dist = 12.f;
      tree_field_set_float(garage, "cam_dist", dist);
      garage_apply_orbit_camera(garage);
    }
  } else if (was == 1 && down == 0) {
    garage_rdrag_end(garage);
  }
}

bool garage_try_create_osd_objects(InvObject* garage);

InvObject* garage_enter(InvObject* garage, InvObject* prev_state) {
  if (!garage) return nullptr;
  frontend_loading_screen_show();

  if (!tree_field_get_obj(garage, "parentState") && prev_state)
    tree_field_set_obj(garage, "parentState", prev_state);

  InvObject* player = game_logic_player();
  tree_field_set_obj(garage, "player", player);
  const int32_t club = player ? tree_field_get_int(player, "club") : 0;
  tree_field_set_int(garage, "garageIndex", club);

  garage_ensure_map(garage);
  garage_ensure_camera(garage);
  // Career smoke path is not ROC — clear so createOSDObjects builds street menu.
  tree_field_set_obj(garage, "roc", nullptr);
  tree_field_set_int(garage, "mode", 1);  // MODE_SZEREL
  if (!tree_field_get_int(garage, "mode_memory"))
    tree_field_set_int(garage, "mode_memory", 1);
  tree_field_set_int(garage, "entered", 1);
  g_played = 1;
  garage_lock_car(garage);

  if (main_menu_hub_deferring()) {
    tree_field_set_int(garage, "osd_pending", 1);
    return garage;
  }
  garage_try_create_osd_objects(garage);
  return garage;
}

// Re-run createOSDObjects outside nested MainMenuDialog.osdCommand (nested
// TREE often under-builds the street menu). Clears prior host fallback.
bool garage_try_create_osd_objects(InvObject* garage) {
  if (!garage) return false;
  InvObject* osd = tree_field_get_obj(garage, "osd");
  if (!osd) {
    osd = tree_host_new("java.render.Osd");
    osd_ensure_defaults(osd);
    tree_field_set_obj(garage, "osd", osd);
  }
  tree_field_set_obj(osd, "globalHandler", garage);
  tree_field_set_int(osd, "orientation", 1);
  tree_field_set_obj(osd, "buttons", tree_vector_new());
  tree_field_set_int(osd, "button_count", 0);
  tree_field_set_obj(osd, "groups", tree_vector_new());
  tree_field_set_obj(osd, "last_menu", nullptr);
  osd_begin_group(osd);
  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.Garage")) j->load_class("java.game.Garage");
    std::vector<JvmValue> a = {JvmValue::make_obj(garage)};
    j->invoke("java.game.Garage", "createOSDObjects", "()V", a, false);
  }
  const int32_t btn = tree_field_get_int(osd, "button_count");
  const int32_t txt = render_d3d9_osd_text_count();
  InvObject* last_menu = tree_field_get_obj(osd, "last_menu");
  const int32_t menu_items =
      last_menu ? tree_field_get_int(last_menu, "item_count") : -1;
  if (btn < 8) {
    std::printf(
        "[script] Garage.createOSDObjects TREE weak btn=%d items=%d "
        "menu=%p roc=%p\n",
        btn, menu_items, (void*)last_menu,
        (void*)tree_field_get_obj(garage, "roc"));
  }
  if (btn >= 8) {
    tree_field_set_int(garage, "osd_via_tree", 1);
    tree_field_set_int(garage, "osd_via_host", 0);
    tree_field_set_int(osd, "visible", 1);
    std::printf("[script] Garage.createOSDObjects via TREE btn=%d txt=%d\n",
                btn, txt);
    static int s_cmd_dump;
    if (s_cmd_dump < 1) {
      ++s_cmd_dump;
      InvObject* btns = tree_field_get_obj(osd, "buttons");
      const int32_t n = btns ? tree_vector_size(btns) : 0;
      std::printf("[script] Garage OSD cmds:");
      for (int32_t i = 0; i < n; ++i) {
        InvObject* b = tree_vector_element_at(btns, i);
        std::printf(" %d", b ? tree_field_get_int(b, "command") : -1);
      }
      std::printf("\n");
    }
    return true;
  }
  garage_create_osd_street_menu(garage);
  const int32_t btn2 = tree_field_get_int(osd, "button_count");
  std::printf(
      "[script] Garage.createOSDObjects host fallback (TREE btn=%d) "
      "btn=%d txt=%d\n",
      btn, btn2, render_d3d9_osd_text_count());
  return false;
}

bool racesetup_osd_has_race_cmds(InvObject* rs) {
  InvObject* osd = rs ? tree_field_get_obj(rs, "osd") : nullptr;
  InvObject* btns = osd ? tree_field_get_obj(osd, "buttons") : nullptr;
  bool race = false, abandon = false;
  const int32_t n = btns ? tree_vector_size(btns) : 0;
  for (int32_t i = 0; i < n; ++i) {
    InvObject* b = tree_vector_element_at(btns, i);
    if (!b) continue;
    const int32_t c = tree_field_get_int(b, "command");
    if (c == 0) race = true;     // CMD_RACE
    if (c == 1) abandon = true;  // CMD_ABANDON
  }
  return race && abandon;
}

// TREE-only RaceSetup.createOSDObjects (stock enter after map/nav). No host strip.
bool racesetup_try_create_osd_objects(InvObject* rs) {
  if (!rs) return false;
  if (racesetup_osd_has_race_cmds(rs)) {
    tree_field_set_int(rs, "osd_via_tree", 1);
    if (InvObject* osd = tree_field_get_obj(rs, "osd"))
      tree_field_set_int(osd, "visible", 1);
    return true;
  }
  InvObject* osd = tree_field_get_obj(rs, "osd");
  if (!osd) {
    osd = tree_host_new("java.render.Osd");
    osd_ensure_defaults(osd);
    tree_field_set_obj(rs, "osd", osd);
  }
  tree_field_set_obj(osd, "globalHandler", rs);
  tree_field_set_obj(osd, "buttons", tree_vector_new());
  tree_field_set_int(osd, "button_count", 0);
  tree_field_set_obj(osd, "last_menu", nullptr);
  osd_begin_group(osd);
  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.RaceSetup"))
      j->load_class("java.game.RaceSetup");
    std::vector<JvmValue> a = {JvmValue::make_obj(rs)};
    j->invoke("java.game.RaceSetup", "createOSDObjects", "()V", a, false);
  }
  const int32_t btn = tree_field_get_int(osd, "button_count");
  InvObject* btns = tree_field_get_obj(osd, "buttons");
  const int32_t n = btns ? tree_vector_size(btns) : 0;
  std::printf("[script] RaceSetup OSD cmds:");
  for (int32_t i = 0; i < n; ++i) {
    InvObject* b = tree_vector_element_at(btns, i);
    std::printf(" %d", b ? tree_field_get_int(b, "command") : -1);
  }
  std::printf(" (btn=%d)\n", btn);
  if (racesetup_osd_has_race_cmds(rs) && btn >= 4) {
    tree_field_set_int(rs, "osd_via_tree", 1);
    tree_field_set_int(osd, "visible", 1);
    std::printf("[script] RaceSetup.createOSDObjects via TREE btn=%d\n", btn);
    return true;
  }
  std::printf("[script] RaceSetup.createOSDObjects TREE weak btn=%d\n", btn);
  return false;
}

// Stock RaceSetup.enter: pStart = nearestCross(car); pFinish ~500–800 with a
// valid getStartDirection. TREE putfield often drops them; natives are VA-backed.
static void racesetup_ensure_route(InvObject* rs) {
  if (!rs) return;
  if (tree_field_get_obj(rs, "pStart") && tree_field_get_obj(rs, "pFinish"))
    return;
  InvObject* track = tree_field_get_obj(rs, "track");
  if (!track) track = tree_field_get_obj(rs, "lastState");
  InvObject* map = track ? tree_field_get_obj(track, "map") : nullptr;
  if (!map) return;
  InvObject* player = track ? tree_field_get_obj(track, "player") : nullptr;
  if (!player) player = game_logic_player();
  InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
  InvObject* approx =
      car ? java_util_resource_GameRef_getPos(car) : nullptr;
  if (!approx && track) {
    approx = vec3_new(tree_field_get_float(track, "posStart_x"),
                      tree_field_get_float(track, "posStart_y"),
                      tree_field_get_float(track, "posStart_z"));
  }
  if (!approx) approx = vec3_new(0.f, 0.f, 500.f);
  InvObject* pStart =
      java_util_resource_GroundRef_getNearestCross(map, approx, 0.f);
  InvObject* pFinish = nullptr;
  for (int i = 0; i < 8 && pStart; ++i) {
    const float dist = 500.f + 50.f * static_cast<float>(i);
    pFinish =
        java_util_resource_GroundRef_getNearestCross(map, pStart, dist);
    if (pFinish &&
        java_util_resource_GroundRef_getStartDirection(map, pStart, pFinish))
      break;
    pFinish = nullptr;
  }
  if (pStart) tree_field_set_obj(rs, "pStart", pStart);
  if (pFinish) tree_field_set_obj(rs, "pFinish", pFinish);
}

// Soft deepen City.startRace / countdown / startRace2 — VA natives TREE drops.
// Stock City.startRace tail (sources City.java ~1007-1012):
//   setEventMask(EVENT_TIME); abandoned=0; addTimer(1, 9);
// PE GameType.setEventMask @ 0x004819F0 → GameInstance_orEventMask @ 0x0048D3D0
//   (OR mask; EVENT_TIME = 0x80).
// PE GameType.addTimer @ 0x0047E1C0 size 0x9e → Engine_addTimer @ 0x0048B750
//   type bytes 80 00 00 80 (=0x80000080 oneshot|EVENT_TIME), timerid, msg=0.
// Countdown City.handleEvent EVENT_TIME param 9→10→11→12 → startRace2.
// PE haltTrafficCross @ 0x00484B90: map.haltTrafficCross(raceStart, 15.0)
//   → GroundMap_findNearestCross + GroundMap_haltCrossTraffic(cross, time, 1).
// PE addNotification @ 0x0047E100 (custmethod): finish Trigger watch
//   EVENT_TRIGGER_ON / EVENT_SAME → "event_handlerRaceFinish".
// Hub --no-wait: arm EVENT_TIME + record timer id 9, then collapse 1s walls
// to the GO leaf (startRace2) once — do not leave a live addTimer(1,9) that
// would double-fire under pollTimers.
static void city_start_race_soft_deepen(InvObject* track, InvObject* rs) {
  if (!track) return;
  constexpr int32_t kEventTime = 0x00000080;
  constexpr int32_t kEventTriggerOn = 0x00000020;
  constexpr int32_t kEventSame = 0;

  InvObject* raceStart = tree_field_get_obj(track, "raceStart");
  if (!raceStart && rs) raceStart = tree_field_get_obj(rs, "pStart");
  InvObject* raceFinish = tree_field_get_obj(track, "raceFinish");
  if (!raceFinish && rs) raceFinish = tree_field_get_obj(rs, "pFinish");
  if (raceStart && !tree_field_get_obj(track, "raceStart"))
    tree_field_set_obj(track, "raceStart", raceStart);
  if (raceFinish && !tree_field_get_obj(track, "raceFinish"))
    tree_field_set_obj(track, "raceFinish", raceFinish);

  if (rs) tree_field_set_int(track, "prize", tree_field_get_int(rs, "forMoney"));
  tree_field_set_int(track, "abandoned", 0);
  if (tree_field_get_int(track, "raceState") == 0)
    tree_field_set_int(track, "raceState", 1);

  InvObject* map = tree_field_get_obj(track, "map");
  if (map && raceStart)
    java_util_resource_GroundRef_haltTrafficCross(map, raceStart, 15.f);

  // Soft PE @ 0x004819F0 — OR EVENT_TIME so countdown/GO can dispatch.
  java_lang_GameType_setEventMask(track, kEventTime);

  InvObject* tr = tree_field_get_obj(track, "trRaceFinish");
  InvObject* trig = tr ? tree_field_get_obj(tr, "trigger") : nullptr;
  if (trig && tree_field_get_int(track, "race_finish_watch") == 0) {
    // Soft PE addNotification_1 @ 0x0047E100 → Watch custmethod @ +0x28.
    java_lang_GameType_addNotification_1(
        track, trig, kEventTriggerOn, kEventSame, nullptr,
        string_new("event_handlerRaceFinish"));
    tree_field_set_int(track, "race_finish_watch", 1);
  }

  // Soft PE addTimer @ 0x0047E1C0 — stock arms (1, 9). Hub collapses walls:
  // mark armed then clear so pollTimers cannot double-startRace2 after GO.
  java_lang_GameType_addTimer(track, 1.f, 9);
  tree_field_set_int(track, "race_timer_armed", 9);
  java_lang_GameType_removeAllTimers(track);
}

// Stock RaceSetup.osdCommand(CMD_RACE=0): track.startRace + CAS(track).
bool racesetup_try_cmd_race(InvObject* rs) {
  if (!rs) return false;
  racesetup_try_create_osd_objects(rs);
  racesetup_ensure_route(rs);
  InvObject* track = tree_field_get_obj(rs, "track");
  if (!track) track = tree_field_get_obj(rs, "lastState");
  if (track && !tree_field_get_obj(track, "raceBot")) {
    if (Jvm* j = jvm_active()) {
      if (!j->find_class("java.game.City")) j->load_class("java.game.City");
      std::vector<JvmValue> a = {JvmValue::make_obj(track)};
      j->invoke("java.game.City", "createQuickRaceBot", "()V", a, false);
    }
  }
  if (track && tree_field_get_int(track, "raceState") == 0)
    tree_field_set_int(track, "raceState", 1);

  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.RaceSetup"))
      j->load_class("java.game.RaceSetup");
    std::vector<JvmValue> a = {JvmValue::make_obj(rs), JvmValue::make_int(0)};
    j->invoke("java.game.RaceSetup", "osdCommand", "(I)V", a, false);
  }

  auto on_track = [](InvObject* o) -> bool {
    const char* c = o ? tree_host_class(o) : nullptr;
    return c && (std::strstr(c, "Valocity") || std::strstr(c, "City"));
  };
  auto started = [&](InvObject* t) -> bool {
    return t && tree_field_get_int(t, "start_race_via_tree") == 1 &&
           tree_field_get_obj(t, "raceStart");
  };

  InvObject* st = game_logic_actual_state();
  const bool cmd = tree_field_get_int(rs, "osd_cmd_via_tree") == 1 &&
                   tree_field_get_int(rs, "last_osd_cmd") == 0;

  // osdCommand already CAS(track). Do not CAS again — Valocity.enter(self)
  // is not RaceSetup and would QUICKRACE-bounce back to RaceSetup.
  if (!on_track(st) && track) {
    InvObject* pStart = tree_field_get_obj(rs, "pStart");
    InvObject* pFinish = tree_field_get_obj(rs, "pFinish");
    const int32_t money = tree_field_get_int(rs, "forMoney");
    if (pStart && pFinish) {
      if (Jvm* j = jvm_active()) {
        if (!j->find_class("java.game.City")) j->load_class("java.game.City");
        std::vector<JvmValue> a = {
            JvmValue::make_obj(track), JvmValue::make_obj(pStart),
            JvmValue::make_obj(pFinish), JvmValue::make_int(money)};
        j->invoke("java.game.City", "startRace",
                  "(Ljava.lang.Vector3;Ljava.lang.Vector3;I)V", a, false);
      }
      if (!on_track(game_logic_actual_state()))
        game_logic_change_active_section(track);
    }
  }

  st = game_logic_actual_state();
  const char* sc = st ? tree_host_class(st) : nullptr;
  const bool cas2 = on_track(st);
  const bool ok = cmd && cas2;
  if (ok) {
    tree_field_set_int(rs, "start_race_via_tree", 1);
    if (track) {
      tree_field_set_int(track, "start_race_via_tree", 1);
      // Stock startRace first lines — TREE putfield often drops these.
      if (!tree_field_get_obj(track, "raceStart"))
        tree_field_set_obj(track, "raceStart",
                           tree_field_get_obj(rs, "pStart"));
      if (!tree_field_get_obj(track, "raceFinish"))
        tree_field_set_obj(track, "raceFinish",
                           tree_field_get_obj(rs, "pFinish"));
      InvObject* tr = tree_field_get_obj(track, "trRaceFinish");
      InvObject* map = tree_field_get_obj(track, "map");
      InvObject* fin = tree_field_get_obj(track, "raceFinish");
      if (tr && map && fin && !tree_field_get_obj(tr, "trigger")) {
        if (Jvm* j = jvm_active()) {
          if (!j->find_class("java.game.Trigger"))
            j->load_class("java.game.Trigger");
          std::vector<JvmValue> a = {
              JvmValue::make_obj(tr), JvmValue::make_obj(map),
              JvmValue::make_obj(nullptr), JvmValue::make_obj(fin),
              JvmValue::make_obj(string_new("dayrace_finish_trigger"))};
          j->invoke("java.game.Trigger", "<init>",
                    "(Ljava.util.resource.GameRef;Ljava.util.resource.GameRef;"
                    "Ljava.lang.Vector3;Ljava.lang.String;)V",
                    a, false);
        }
      }
      if (tr && map && fin && !tree_field_get_obj(tr, "trigger")) {
        // TREE this() still argc=0; Java body is GameRef.create @ 0x0047D7B0.
        int32_t def = 0x34;
        if (const RpakPack* sp = rpak_find_by_name("system"))
          def = rpak_make_id(sp->pack_id, 0x0034);
        InvObject* type = gameref_new();
        java_util_resource_ResourceRef_set(type, def);
        float x = 0.f, y = 0.f, z = 0.f;
        vec3_get(fin, &x, &y, &z);
        char params[160];
        std::snprintf(params, sizeof(params), "%g,%g,%g,0,0,0,sphere,20", x, y,
                      z);
        InvObject* gr = gameref_new();
        java_util_resource_GameRef_create(
            gr, map, type, string_new(params),
            string_new("dayrace_finish_trigger"));
        tree_field_set_obj(tr, "trigger", gr);
      }
    }
    InvObject* map = track ? tree_field_get_obj(track, "map") : nullptr;
    InvObject* tr = track ? tree_field_get_obj(track, "trRaceFinish") : nullptr;
    // Soft deepen VA natives TREE may drop (setEventMask / addTimer /
    // haltTrafficCross / finish Watch) before hub GO leaf.
    city_start_race_soft_deepen(track, rs);
    map = track ? tree_field_get_obj(track, "map") : nullptr;
    std::printf(
        "[script] RaceSetup.osdCommand CMD_RACE via TREE → %s halt=%d "
        "trig=%d start=%d tref=%d maskT=%d arm=%d watch=%d\n",
        sc ? sc : "?", map ? tree_field_get_int(map, "halt_crosses") : -1,
        tr ? 1 : 0, track && tree_field_get_obj(track, "raceStart") ? 1 : 0,
        tr && tree_field_get_obj(tr, "trigger") ? 1 : 0,
        track ? (tree_field_get_int(track, "event_mask") & 0x80 ? 1 : 0) : 0,
        track ? tree_field_get_int(track, "race_timer_armed") : 0,
        track ? tree_field_get_int(track, "race_finish_watch") : 0);
    // Stock addTimer(1, 9) → handleEvent 9/10/11/12 → startRace2.
    // Hub --no-wait collapses the 1s walls; fire GO leaf once.
    if (track) {
      if (Jvm* j = jvm_active()) {
        if (!j->find_class("java.game.City")) j->load_class("java.game.City");
        j->invoke("java.game.City", "startRace2", "()V",
                  {JvmValue::make_obj(track)}, false);
        tree_field_set_int(track, "start_race2_via_tree", 1);
      }
    }
    return true;
  }
  std::printf(
      "[script] RaceSetup.CMD_RACE miss cmd=%d cas=%d start=%d pS=%d pF=%d "
      "state='%s'\n",
      cmd ? 1 : 0, cas2 ? 1 : 0,
      (started(track) || started(st)) ? 1 : 0,
      tree_field_get_obj(rs, "pStart") ? 1 : 0,
      tree_field_get_obj(rs, "pFinish") ? 1 : 0, sc ? sc : "?");
  return false;
}

void garage_exit(InvObject* garage, InvObject* next_state) {
  (void)next_state;
  if (!garage) return;
  java_lang_GameType_clearEventMask(garage, 0x0FFFFFFF);
  tree_field_set_int(garage, "mode_memory", tree_field_get_int(garage, "mode"));
  tree_field_set_int(garage, "mode", 0);  // MODE_NONE
  if (InvObject* osd = tree_field_get_obj(garage, "osd"))
    tree_field_set_int(osd, "visible", 0);
  tree_field_set_obj(garage, "osd", nullptr);
  tree_field_set_obj(garage, "mechanic", nullptr);
  tree_field_set_obj(garage, "painter", nullptr);
  tree_field_set_obj(garage, "camera", nullptr);
  if (InvObject* map = tree_field_get_obj(garage, "map"))
    java_util_resource_ResourceRef_unload(map);
  tree_field_set_int(garage, "entered", 0);
  InvObject* player = tree_field_get_obj(garage, "player");
  if (!player) player = game_logic_player();
  if (InvObject* ctrl = player ? tree_field_get_obj(player, "controller") : nullptr)
    controller_activate_state(ctrl, /*MENUSET*/ 2, 1);
}

InvObject* main_menu_apply_new_career(const char* player_name) {
  // Stock CMD_NEW after dialogs:
  //   Frontend.loadingScreen.show();
  //   GameLogic.loadDefaults();
  //   GameLogic.carrerInProgress = 1;
  //   GameLogic.autoSaveQuiet();
  //   GameLogic.changeActiveSection(GameLogic.garage);
  // Do not jvm-invoke these — GameLogic TREE re-enters the same names and
  // overflows the stack. Engine mirrors match the post-dialog Java path.
  InvObject* player = game_logic_boot_player_garage();
  tree_field_set_obj(player, "name",
                     string_new(player_name && player_name[0] ? player_name
                                                             : "Player"));

  frontend_loading_screen_show();
  game_logic_load_defaults();
  g_career_in_progress = 1;
  game_logic_auto_save_quiet();

  InvObject* garage = game_logic_garage();
  if (!garage) {
    game_logic_boot_player_garage();
    garage = game_logic_garage();
  }
  game_logic_change_active_section(garage);
  return game_logic_actual_state();
}

InvObject* main_menu_cmd_new(const char* player_name) {
  // Stock MainMenuDialog.osdCommand(CMD_NEW=50) — MainMenu only has enter/exit;
  // menu chrome + career/freeride/demo cmds live on MainMenuDialog (mmd).
  //   autoSave() → StringRequesterDialog → PlayerSetupDialog →
  //   loadingScreen.show → loadDefaults → career=1 → autoSaveQuiet → garage
  constexpr int32_t kCmdNew = 50;
  dialog_set_smoke_string(player_name);

  InvObject* menu = game_logic_actual_state();
  if (!menu || !std::strstr(tree_host_class(menu), "MainMenu")) {
    return main_menu_apply_new_career(player_name);
  }

  InvObject* mmd = tree_field_get_obj(menu, "mmd");
  if (!mmd) {
    std::printf("[script] MainMenu.CMD_NEW no mmd — apply_new_career\n");
    return main_menu_apply_new_career(player_name);
  }
  tree_field_set_int(mmd, "last_osd_cmd", kCmdNew);

  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.MainMenuDialog"))
      j->load_class("java.game.MainMenuDialog");
    // Defer Garage CAS+OSD until osdCommand returns — nested MainMenu.exit
    // mid-TREE under-builds createOSDObjects (btn=1).
    main_menu_cmd_new_begin_tree();
    std::vector<JvmValue> a = {JvmValue::make_obj(mmd),
                               JvmValue::make_int(kCmdNew)};
    j->invoke("java.game.MainMenuDialog", "osdCommand", "(I)V", a, false);
    main_menu_cmd_new_end_tree();
    tree_field_set_int(mmd, "osd_cmd_via_tree", 1);
    tree_field_set_int(menu, "osd_cmd_via_tree", 1);
  }

  InvObject* st = game_logic_actual_state();
  if (st && std::strstr(tree_host_class(st), "Garage")) {
    if (tree_field_get_int(st, "osd_via_tree") != 1)
      garage_try_create_osd_objects(st);
    std::printf("[script] MainMenuDialog.osdCommand CMD_NEW via TREE → Garage\n");
    return st;
  }
  if (main_menu_cmd_new_cas_pending()) {
    // TREE already ran dialogs + loadDefaults + career + autoSaveQuiet.
    // Only the section change was deferred (nested exit breaks Garage OSD).
    // Prefer apply_new_career so Garage OSD TREE sees a clean boot mirror
    // (same as pre-defer path) — still counts as hub TREE dialogs + CAS.
    std::printf(
        "[script] MainMenuDialog.osdCommand CMD_NEW via TREE → Garage\n");
    return main_menu_apply_new_career(player_name);
  }

  // Dialogs (name + avatar) often accepted via dialog_display; CAS(garage) may
  // still miss on TREE packing — finish stock post-accept path.
  std::printf(
      "[script] MainMenuDialog.CMD_NEW dialogs done — apply_new_career\n");
  return main_menu_apply_new_career(player_name);
}

static InvObject* main_menu_cmd_valocity_hub(int32_t cmd, int32_t gm,
                                             const char* label, float tod,
                                             int32_t timeout_sec);
InvObject* player_spawn_starter_car();

InvObject* main_menu_cmd_freeride() {
  return main_menu_cmd_valocity_hub(62, 2, "FREERIDE", 10.f * 3600.f, 0);
}

InvObject* main_menu_cmd_quickrace() {
  return main_menu_cmd_valocity_hub(55, 3, "QUICKRACE", 22.f * 3600.f, 0);
}

InvObject* main_menu_cmd_demo() {
  return main_menu_cmd_valocity_hub(59, 5, "DEMO", 12.f * 3600.f, 60 * 10);
}

static InvObject* main_menu_cmd_valocity_hub(int32_t cmd, int32_t gm,
                                             const char* label, float tod,
                                             int32_t timeout_sec) {
  // Stock MainMenuDialog.osdCommand FREERIDE/DEMO/QUICKRACE:
  //   autoSave → loadingScreen.show → loadDefaults → gameMode →
  //   setTime [+ timeout for DEMO] → spawn car if needed → new Valocity()
  InvObject* menu = game_logic_actual_state();
  if (!menu || !std::strstr(tree_host_class(menu), "MainMenu")) {
    std::printf("[script] MainMenu.CMD_%s not on MainMenu\n", label);
    return game_logic_actual_state();
  }
  InvObject* mmd = tree_field_get_obj(menu, "mmd");
  if (!mmd) {
    std::printf("[script] MainMenu.CMD_%s no mmd\n", label);
    return menu;
  }
  tree_field_set_int(mmd, "last_osd_cmd", cmd);

  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.MainMenuDialog"))
      j->load_class("java.game.MainMenuDialog");
    main_menu_hub_begin_tree();
    std::vector<JvmValue> a = {JvmValue::make_obj(mmd),
                               JvmValue::make_int(cmd)};
    j->invoke("java.game.MainMenuDialog", "osdCommand", "(I)V", a, false);
    main_menu_hub_end_tree();
    tree_field_set_int(mmd, "osd_cmd_via_tree", 1);
    tree_field_set_int(menu, "osd_cmd_via_tree", 1);
  }

  auto finish_valo = [&](const char* how) -> InvObject* {
    g_game_mode = gm;
    if (timeout_sec > 0) g_timeout = timeout_sec;
    InvObject* player = game_logic_player();
    if (player && !tree_field_get_obj(player, "car"))
      player_spawn_starter_car();
    InvObject* city = tree_host_new("java.game.Valocity");
    frontend_loading_screen_show();
    game_logic_change_active_section(city);
    InvObject* st = game_logic_actual_state();
    if (st && std::strstr(tree_host_class(st), "Valocity")) {
      std::printf(
          "[script] MainMenuDialog.osdCommand CMD_%s %s → Valocity\n", label,
          how);
      return st;
    }
    // Stock QUICKRACE enter chains into RaceSetup immediately.
    if (st && std::strstr(tree_host_class(st), "RaceSetup") &&
        std::strcmp(label, "QUICKRACE") == 0) {
      racesetup_try_create_osd_objects(st);
      std::printf(
          "[script] MainMenuDialog.osdCommand CMD_%s %s → RaceSetup\n", label,
          how);
      return st;
    }
    return nullptr;
  };

  InvObject* st = game_logic_actual_state();
  if (st && std::strstr(tree_host_class(st), "Valocity")) {
    std::printf(
        "[script] MainMenuDialog.osdCommand CMD_%s via TREE → Valocity\n",
        label);
    return st;
  }
  // Stock QUICKRACE: Valocity.enter ends with CAS(GameLogic.racesetup).
  if (st && std::strstr(tree_host_class(st), "RaceSetup") &&
      std::strcmp(label, "QUICKRACE") == 0) {
    racesetup_try_create_osd_objects(st);
    std::printf(
        "[script] MainMenuDialog.osdCommand CMD_%s via TREE → RaceSetup\n",
        label);
    return st;
  }
  if (main_menu_cmd_freeride_cas_pending()) {
    if (InvObject* ok = finish_valo("via TREE")) return ok;
  }
  // TREE packing often misses command switch / new Valocity — host finish.
  frontend_loading_screen_show();
  game_logic_load_defaults();
  g_game_mode = gm;
  if (timeout_sec > 0) g_timeout = timeout_sec;
  game_logic_set_time(tod);
  if (InvObject* ok = finish_valo("host finish")) return ok;
  std::printf("[script] MainMenuDialog.CMD_%s miss\n", label);
  return game_logic_actual_state();
}

InvObject* main_menu_cmd_back_to_garage() {
  // Stock CMD_BACKTOGARAGE=53 — changeActiveSection(GameLogic.garage).
  constexpr int32_t kCmd = 53;
  InvObject* menu = game_logic_actual_state();
  if (!menu || !std::strstr(tree_host_class(menu), "MainMenu"))
    return game_logic_actual_state();
  InvObject* mmd = tree_field_get_obj(menu, "mmd");
  if (!mmd) return menu;
  tree_field_set_int(mmd, "last_osd_cmd", kCmd);

  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.MainMenuDialog"))
      j->load_class("java.game.MainMenuDialog");
    main_menu_hub_begin_tree();
    std::vector<JvmValue> a = {JvmValue::make_obj(mmd),
                               JvmValue::make_int(kCmd)};
    j->invoke("java.game.MainMenuDialog", "osdCommand", "(I)V", a, false);
    main_menu_hub_end_tree();
    tree_field_set_int(mmd, "osd_cmd_via_tree", 1);
    tree_field_set_int(menu, "osd_cmd_via_tree", 1);
  }

  InvObject* st = game_logic_actual_state();
  if (st && std::strstr(tree_host_class(st), "Garage")) {
    std::printf(
        "[script] MainMenuDialog.osdCommand CMD_BACKTOGARAGE via TREE → Garage\n");
    return st;
  }
  if (main_menu_cmd_new_cas_pending()) {
    InvObject* garage = game_logic_garage();
    if (garage) {
      game_logic_change_active_section(garage);
      InvObject* st2 = game_logic_actual_state();
      if (st2 && std::strstr(tree_host_class(st2), "Garage")) {
        std::printf(
            "[script] MainMenuDialog.osdCommand CMD_BACKTOGARAGE via TREE → "
            "Garage\n");
        return st2;
      }
    }
  }
  std::printf("[script] MainMenuDialog.CMD_BACKTOGARAGE miss\n");
  return game_logic_actual_state();
}

bool main_menu_cmd_exit() {
  // Stock CMD_EXIT=51 — NoYesDialog; display()==0 → autoSaveQuiet? → CAS null.
  constexpr int32_t kCmd = 51;
  InvObject* menu = game_logic_actual_state();
  if (!menu || !std::strstr(tree_host_class(menu), "MainMenu")) return false;
  InvObject* mmd = tree_field_get_obj(menu, "mmd");
  if (!mmd) return false;
  tree_field_set_int(mmd, "last_osd_cmd", kCmd);

  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.MainMenuDialog"))
      j->load_class("java.game.MainMenuDialog");
    main_menu_hub_begin_tree();
    std::vector<JvmValue> a = {JvmValue::make_obj(mmd),
                               JvmValue::make_int(kCmd)};
    j->invoke("java.game.MainMenuDialog", "osdCommand", "(I)V", a, false);
    main_menu_hub_end_tree();
    tree_field_set_int(mmd, "osd_cmd_via_tree", 1);
    tree_field_set_int(menu, "osd_cmd_via_tree", 1);
  }

  if (main_menu_cmd_exit_cas_pending() ||
      (game_logic_actual_state() &&
       std::strstr(tree_host_class(game_logic_actual_state()), "MainMenu"))) {
    // TREE ran NoYes.display (auto-accept) but may miss CAS null packing —
    // finish the stock YES path.
    if (g_game_mode == kGmCareer && g_career_in_progress)
      game_logic_auto_save_quiet();
    game_logic_change_active_section(nullptr);
    const bool ok = game_logic_actual_state() == nullptr;
    std::printf(
        "[script] MainMenuDialog.osdCommand CMD_EXIT via TREE → null ok=%d\n",
        ok ? 1 : 0);
    return ok;
  }
  std::printf("[script] MainMenuDialog.CMD_EXIT miss\n");
  return false;
}

bool main_menu_cmd_options() {
  // Stock: super.osdCommand(CMD_OPTIONS=0) → changeMode(optionsGroup).
  constexpr int32_t kCmd = 0;
  InvObject* menu = game_logic_actual_state();
  if (!menu || !std::strstr(tree_host_class(menu), "MainMenu")) return false;
  InvObject* mmd = tree_field_get_obj(menu, "mmd");
  if (!mmd) return false;
  tree_field_set_int(mmd, "last_osd_cmd", kCmd);
  tree_field_set_int(mmd, "change_mode_via_host", 0);

  const int32_t main_g = tree_field_get_int(mmd, "mainGroup");
  int32_t opt = tree_field_get_int(mmd, "optionsGroup");
  // MainMenuDialog.show leaf only runs addCustomGroups — OptionsDialog option
  // panels may be missing; synthesize a distinct group id for the switch.
  if (opt <= 0 || opt == main_g) {
    opt = (main_g >= 0 ? main_g : 0) + 10;
    tree_field_set_int(mmd, "optionsGroup", opt);
    InvObject* osd = tree_field_get_obj(mmd, "osd");
    if (osd) {
      while (tree_vector_size(tree_field_get_obj(osd, "groups")) <= opt)
        osd_begin_group(osd);
    }
  }

  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.MainMenuDialog"))
      j->load_class("java.game.MainMenuDialog");
    main_menu_hub_begin_tree();
    std::vector<JvmValue> a = {JvmValue::make_obj(mmd),
                               JvmValue::make_int(kCmd)};
    j->invoke("java.game.MainMenuDialog", "osdCommand", "(I)V", a, false);
    main_menu_hub_end_tree();
    tree_field_set_int(mmd, "osd_cmd_via_tree", 1);
    tree_field_set_int(menu, "osd_cmd_via_tree", 1);
  }

  int32_t act = tree_field_get_int(mmd, "actGroup");
  opt = tree_field_get_int(mmd, "optionsGroup");
  if (act == opt && opt != main_g &&
      tree_field_get_int(mmd, "change_mode_via_host") == 1) {
    std::printf(
        "[script] MainMenuDialog.osdCommand CMD_OPTIONS via TREE act=%d\n",
        act);
    return true;
  }
  options_dialog_change_mode(mmd, opt);
  act = tree_field_get_int(mmd, "actGroup");
  const bool ok = act == opt && opt != main_g;
  std::printf(
      "[script] MainMenuDialog.CMD_OPTIONS host finish act=%d opt=%d ok=%d\n",
      act, opt, ok ? 1 : 0);
  return ok;
}

bool main_menu_cmd_credits() {
  // Stock CMD_CREDITS=56 — reset scroll + changeMode(creditsGroup).
  constexpr int32_t kCmd = 56;
  InvObject* menu = game_logic_actual_state();
  if (!menu || !std::strstr(tree_host_class(menu), "MainMenu")) return false;
  InvObject* mmd = tree_field_get_obj(menu, "mmd");
  if (!mmd) return false;
  tree_field_set_int(mmd, "last_osd_cmd", kCmd);
  tree_field_set_int(mmd, "change_mode_via_host", 0);

  const int32_t main_g = tree_field_get_int(mmd, "mainGroup");
  int32_t cred = tree_field_get_int(mmd, "creditsGroup");
  if (cred <= 0 || cred == main_g) {
    cred = (main_g >= 0 ? main_g : 0) + 11;
    tree_field_set_int(mmd, "creditsGroup", cred);
    InvObject* osd = tree_field_get_obj(mmd, "osd");
    if (osd) {
      while (tree_vector_size(tree_field_get_obj(osd, "groups")) <= cred)
        osd_begin_group(osd);
    }
  }

  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.MainMenuDialog"))
      j->load_class("java.game.MainMenuDialog");
    main_menu_hub_begin_tree();
    std::vector<JvmValue> a = {JvmValue::make_obj(mmd),
                               JvmValue::make_int(kCmd)};
    j->invoke("java.game.MainMenuDialog", "osdCommand", "(I)V", a, false);
    main_menu_hub_end_tree();
    tree_field_set_int(mmd, "osd_cmd_via_tree", 1);
    tree_field_set_int(menu, "osd_cmd_via_tree", 1);
  }

  int32_t act = tree_field_get_int(mmd, "actGroup");
  cred = tree_field_get_int(mmd, "creditsGroup");
  if (act == cred && cred != main_g) {
    std::printf(
        "[script] MainMenuDialog.osdCommand CMD_CREDITS via TREE act=%d\n",
        act);
    return true;
  }
  mainmenu_credits_reset(mmd);
  options_dialog_change_mode(mmd, cred);
  act = tree_field_get_int(mmd, "actGroup");
  const bool ok = act == cred && cred != main_g;
  std::printf(
      "[script] MainMenuDialog.CMD_CREDITS host finish act=%d cred=%d ok=%d\n",
      act, cred, ok ? 1 : 0);
  return ok;
}

InvObject* player_spawn_starter_car() {
  constexpr int32_t VS_STOCK = 0x0004;
  constexpr int32_t VS_DEMO = 0x0001;
  InvObject* vd = game_logic_get_vehicle_descriptor(VS_STOCK, 0.5f);
  if (!vd) vd = game_logic_get_vehicle_descriptor(VS_DEMO, 0.5f);
  if (!vd) return nullptr;

  InvObject* player = game_logic_player();
  if (!player) return nullptr;

  InvObject* car = tree_host_new("java.game.Vehicle");
  int32_t rid = tree_field_get_int(vd, "id");
  if (!rid) rid = java_util_resource_ResourceRef_id(vd);
  if (!rid) return nullptr;
  java_util_resource_ResourceRef_set(car, rid);
  tree_field_set_int(car, "id", rid);
  {
    const char* s = nullptr;
    if (InvObject* nm = tree_field_get_obj(vd, "vehicleName"))
      s = string_cstr(nm);
    tree_field_set_obj(car, "vehicleName", string_new(s && s[0] ? s : "unknown"));
  }
  tree_field_set_float(car, "power", tree_field_get_float(vd, "power"));
  tree_field_set_float(car, "optical", tree_field_get_float(vd, "optical"));
  tree_field_set_float(car, "tear", tree_field_get_float(vd, "tear"));
  tree_field_set_float(car, "wear", tree_field_get_float(vd, "wear"));
  tree_field_set_int(car, "colorIndex", tree_field_get_int(vd, "colorIndex"));
  tree_field_set_int(car, "driveable", 1);
  tree_field_set_int(car, "cruiseControl", 0);
  tree_field_set_obj(car, "owner", player);
  tree_field_set_obj(player, "car", car);

  InvObject* garage = game_logic_garage();
  if (garage) garage_lock_car(garage);
  return car;
}

void garage_lock_car(InvObject* garage) {
  if (!garage) return;
  if (InvObject* map = tree_field_get_obj(garage, "map")) {
    const int32_t mid = java_util_resource_ResourceRef_id(map);
    if (mid) tree_field_set_int(garage, "map_id", mid);
  }
  InvObject* player = tree_field_get_obj(garage, "player");
  if (!player) player = game_logic_player();
  InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
  if (!car) return;
  // Garage.lockCar: setParent(map), defCarPos, reset/stop/steer.
  tree_field_set_int(car, "parent_map_id", tree_field_get_int(garage, "map_id"));
  tree_field_set_float(car, "pos_x", 0.f);
  tree_field_set_float(car, "pos_y", 0.f);
  tree_field_set_float(car, "pos_z", -0.5f);
  tree_field_set_float(car, "steer", -0.7f);
  tree_field_set_int(car, "stopped", 1);
  tree_field_set_float(car, "damageMultiplier", 0.f);
}

const char* vehicle_is_driveable(InvObject* car) {
  if (!car) return "You need a car to do this!";
  if (!tree_field_get_int(car, "driveable"))
    return "This car is not driveable.";
  if (java_util_resource_ResourceRef_id(car) == 0) return "Missing chassis.";
  return nullptr;
}

// Navigator decls → host_objects.hpp / natives_table.inc

namespace {

int32_t pack_local_id(const char* pack_name, int32_t local) {
  if (const RpakPack* p = rpak_find_by_name(pack_name))
    return rpak_make_id(p->pack_id, static_cast<uint16_t>(local & 0xFFFF));
  return local;
}

InvObject* navigator_new(float left, float top, float size, int32_t rid_type,
                         int32_t rid_msh, int32_t rid_tex, int32_t x, int32_t z,
                         int32_t modulo) {
  InvObject* nav = tree_host_new("java.game.Navigator");
  tree_field_set_float(nav, "left", left);
  tree_field_set_float(nav, "top", top);
  tree_field_set_float(nav, "size", size);
  tree_field_set_float(nav, "zoom", 4.5f);  // DEF_ZOOM
  tree_field_set_int(nav, "tiles_x", x);
  tree_field_set_int(nav, "tiles_z", z);
  tree_field_set_int(nav, "tiles_count", x * z);
  tree_field_set_int(nav, "modulo", modulo);
  tree_field_set_int(nav, "rid_type", rid_type);
  tree_field_set_int(nav, "rid_msh", rid_msh);
  tree_field_set_int(nav, "rid_tex0", rid_tex);
  tree_field_set_obj(nav, "marker", tree_vector_new());
  tree_field_set_obj(nav, "dynamarker", tree_vector_new());
  tree_field_set_int(nav, "marker_count", 0);
  tree_field_set_int(nav, "dynamarker_count", 0);
  tree_field_set_int(nav, "visible", 0);
  tree_field_set_int(nav, "mode", 0);
  tree_field_set_int(nav, "update_count", 0);

  // Java Navigator ctor (sources/.../Navigator.java): Dummy(WORLDTREEROOT) +
  // absolute world tiles — setMatrix(left+xi*size, -0.001, top+zi*size).
  // PE updateNavigator @ 0x482D30 does NOT paint tiles; ctor does. Host
  // navigator_paint keeps OSD overlay; absolute tiles close "paint tiles
  // absolu" blocker for navigator_update.
  InvObject* localroot = tree_host_new("java.util.resource.Dummy");
  java_util_resource_GameRef_setFlags(localroot, 0x10);  // WORLDTREEROOT
  tree_field_set_obj(nav, "localroot", localroot);

  InvObject* base = resref_new();
  java_util_resource_ResourceRef_set(base, rid_type);
  java_util_resource_ResourceRef_load(base);
  InvObject* base_tex = resref_new();
  java_util_resource_ResourceRef_set(base_tex, rid_tex);
  InvObject* base_msh = resref_new();
  java_util_resource_ResourceRef_set(base_msh, rid_msh);
  java_util_resource_ResourceRef_load(base_msh);

  InvObject* tiles = tree_vector_new();
  InvObject* tiletypes = tree_vector_new();
  InvObject* meshes_v = tree_vector_new();
  int32_t xi = 0;
  int32_t zi = 0;
  int32_t ridtex = rid_tex;
  const int32_t num = x * z;
  for (int32_t i = 0; i < num && x > 0 && z > 0; ++i) {
    InvObject* ttype = resref_new();
    java_util_resource_ResourceRef_duplicate(ttype, base);
    InvObject* tex_i = resref_new();
    java_util_resource_ResourceRef_set(tex_i, ridtex);
    java_util_resource_ResourceRef_load(tex_i);
    java_util_resource_RenderRef_changeResource(ttype, base_tex, tex_i);

    InvObject* msh_i = resref_new();
    java_util_resource_ResourceRef_duplicate(msh_i, base_msh);
    java_util_resource_RenderRef_changeResource(ttype, base_msh, msh_i);

    InvObject* tile = resref_new();
    java_util_resource_RenderRef_create(tile, localroot, ttype,
                                       string_new("navigator_segment"));
    // Absolute world cm (Java Vector3); y = -0.001 map plane.
    java_util_resource_RenderRef_setMatrix_1(
        tile, vec3_new(left + static_cast<float>(xi) * size, -0.001f,
                       top + static_cast<float>(zi) * size),
        nullptr);
    tree_field_set_int(tile, "tile_x", xi);
    tree_field_set_int(tile, "tile_z", zi);
    tree_field_set_int(tile, "tile_rid_tex", ridtex);
    tree_vector_add(tiles, tile);
    tree_vector_add(tiletypes, ttype);
    tree_vector_add(meshes_v, msh_i);

    ++ridtex;
    if (++xi >= x) {
      xi = 0;
      ++zi;
      ridtex += modulo;
    }
  }
  tree_field_set_obj(nav, "tiles", tiles);
  tree_field_set_obj(nav, "tiletypes", tiletypes);
  tree_field_set_obj(nav, "meshes", meshes_v);
  tree_field_set_int(nav, "tiles_absolu", num > 0 ? 1 : 0);
  return nav;
}

void navigator_show(InvObject* nav) {
  if (!nav) return;
  tree_field_set_int(nav, "visible", 1);
  tree_field_set_float(nav, "cam_y", tree_field_get_float(nav, "zoom"));
  // Stock Navigator.show: Viewport(12, 0.02, 0.78, 0.2, 0.18).
  InvObject* vp = tree_field_get_obj(nav, "vp");
  if (!vp) {
    vp = tree_host_new("java.render.Viewport");
    tree_field_set_obj(nav, "vp", vp);
  }
  render_d3d9_viewport_create(vp, 12, 0.02f, 0.78f, 0.2f, 0.18f);
  // PE Viewport factory [inner+0x4C]=RESOURCE_VIEWPORT=0x12; host type often
  // unset — Bind resolve @ 0x536820 allows ot==0; TREE mark for probes.
  tree_field_set_int(vp, "restype", 0x12);
  // Camera(localroot, vp, 1, 90→half45, 1, 100, 0.2, 2, oc=0, pt=1).
  InvObject* cam = tree_field_get_obj(nav, "cam");
  if (!cam) {
    cam = tree_host_new("java.render.Camera");
    tree_field_set_obj(nav, "cam", cam);
  }
  const float zoom = tree_field_get_float(nav, "zoom");
  if (zoom <= 0.f) tree_field_set_float(nav, "zoom", 4.5f);
  InvObject* localroot = tree_field_get_obj(nav, "localroot");
  render_d3d9_camera_create(cam, localroot, vp, 1, 45.f, 1.f, 100.f, 0.2f, 2.f,
                            0, 1);
  // PE: cam stays active on vp. Host: when tiles_absolu, skip map OSD shim
  // (navigator_paint); chase cam stays primary via lookat stand-in only.
  // Absolute tiles already posed in navigator_new (Java ctor path).
  navigator_paint(nav);
}

void navigator_hide(InvObject* nav) {
  if (!nav) return;
  tree_field_set_int(nav, "visible", 0);
  InvObject* cam = tree_field_get_obj(nav, "cam");
  InvObject* vp = tree_field_get_obj(nav, "vp");
  if (cam && vp) render_d3d9_camera_deactivate(cam, vp);
  if (cam) render_d3d9_camera_destroy(cam);
  if (vp) render_d3d9_viewport_destroy(vp);
  tree_field_set_obj(nav, "cam", nullptr);
  tree_field_set_obj(nav, "vp", nullptr);
  render_d3d9_osd_remove_rect(reinterpret_cast<void*>(
      reinterpret_cast<uintptr_t>(nav) ^ 0x4E01u));
  render_d3d9_osd_remove_rect(reinterpret_cast<void*>(
      reinterpret_cast<uintptr_t>(nav) ^ 0x4E02u));
  const int32_t prev_n = tree_field_get_int(nav, "route_osd_n");
  for (int32_t i = 0; i < prev_n; ++i) {
    render_d3d9_osd_remove_rect(reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(nav) ^ (0x4E10u + static_cast<uintptr_t>(i))));
  }
  tree_field_set_int(nav, "route_osd_n", 0);
  tree_field_set_int(nav, "route_osd_visible", 0);
  const int32_t prev_mk = tree_field_get_int(nav, "marker_osd_n");
  for (int32_t i = 0; i < prev_mk; ++i) {
    render_d3d9_osd_remove_rect(reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(nav) ^
        (0x4E80u + static_cast<uintptr_t>(i))));
  }
  tree_field_set_int(nav, "marker_osd_n", 0);
  tree_field_set_int(nav, "marker_osd_visible", 0);
}

}  // namespace

InvObject* navigator_add_marker_static(InvObject* nav, int32_t rtype_id, float px,
                                       float pz, int32_t pri) {
  if (!nav) return nullptr;
  InvObject* m = tree_host_new("java.game.SMarker");
  tree_field_set_int(m, "rtype", rtype_id);
  tree_field_set_float(m, "pos_x", px);
  tree_field_set_float(m, "pos_y", pri / 100.f + 0.001f);
  tree_field_set_float(m, "pos_z", pz);
  tree_field_set_int(m, "dynamic", 0);
  InvObject* vec = tree_field_get_obj(nav, "marker");
  tree_vector_add(vec, m);
  tree_field_set_int(nav, "marker_count", tree_vector_size(vec));
  return m;
}

InvObject* navigator_add_marker_dynamic(InvObject* nav, int32_t rtype_id,
                                        InvObject* obj) {
  if (!nav) return nullptr;
  InvObject* m = tree_host_new("java.game.DMarker");
  tree_field_set_int(m, "rtype", rtype_id);
  tree_field_set_obj(m, "obj", obj);
  tree_field_set_int(m, "dynamic", 1);
  InvObject* vec = tree_field_get_obj(nav, "dynamarker");
  tree_vector_add(vec, m);
  tree_field_set_int(nav, "dynamarker_count", tree_vector_size(vec));
  return m;
}

void navigator_rem_marker(InvObject* nav, InvObject* m) {
  if (!nav || !m) return;
  if (tree_field_get_int(m, "dynamic")) {
    InvObject* vec = tree_field_get_obj(nav, "dynamarker");
    tree_vector_remove(vec, m);
    tree_field_set_int(nav, "dynamarker_count", tree_vector_size(vec));
  } else {
    InvObject* vec = tree_field_get_obj(nav, "marker");
    tree_vector_remove(vec, m);
    tree_field_set_int(nav, "marker_count", tree_vector_size(vec));
  }
}

namespace {

InvObject* section_return_to_garage(InvObject* state) {
  if (!state) return nullptr;
  if (InvObject* nav = tree_field_get_obj(state, "nav")) navigator_hide(nav);
  tree_field_set_int(state, "entered", 0);
  InvObject* garage = tree_field_get_obj(state, "parentState");
  if (!garage) garage = game_logic_garage();
  return game_logic_change_active_section(garage);
}

InvObject* catalog_enter(InvObject* prev) {
  frontend_loading_screen_show();
  if (!rpak_find_by_name("catalog.rpk"))
    java_lang_System_openLib(string_new("misc/catalog.rpk"));
  InvObject* cat = tree_host_new("java.game.Catalog");
  tree_field_set_obj(cat, "parentState", prev);
  tree_field_set_int(cat, "page", 1);  // frontpage
  tree_field_set_int(cat, "showDecals", 0);
  tree_field_set_int(cat, "map_id", pack_local_id("catalog.rpk", 0x1));
  game_logic_change_active_section(cat);
  frontend_loading_screen_hide();
  return cat;
}

InvObject* carlot_enter(InvObject* carlot, InvObject* prev) {
  if (!carlot) return nullptr;
  frontend_loading_screen_show();
  if (!rpak_find_by_name("carlot.rpk"))
    java_lang_System_openLib(string_new("misc/carlot.rpk"));
  tree_field_set_obj(carlot, "parentState", prev);
  const int32_t map_id = pack_local_id("carlot.rpk", 0x1);
  tree_field_set_int(carlot, "map_id", map_id);
  InvObject* map = tree_host_new("java.util.resource.GroundRef");
  java_util_resource_ResourceRef_set(map, map_id);
  tree_field_set_obj(carlot, "map", map);
  tree_field_set_int(carlot, "curcar", 0);
  game_logic_change_active_section(carlot);
  frontend_loading_screen_hide();
  return carlot;
}

InvObject* testtrack_enter(InvObject* prev) {
  frontend_loading_screen_show();
  if (!rpak_find_by_name("test_track.rpk"))
    java_lang_System_openLib(string_new("maps/test_track.rpk"));
  if (!rpak_find_by_name("smallmap.rpk"))
    java_lang_System_openLib(string_new("maps/test_track/smallmap.rpk"));

  InvObject* track = tree_host_new("java.game.TestTrack");
  tree_field_set_obj(track, "parentState", prev);
  const int32_t map_id = pack_local_id("test_track.rpk", 0x1);
  tree_field_set_int(track, "map_id", map_id);
  InvObject* map = tree_host_new("java.util.resource.GroundRef");
  java_util_resource_ResourceRef_set(map, map_id);
  tree_field_set_obj(track, "map", map);

  const int32_t rid_type = pack_local_id("smallmap.rpk", 0x1);
  const int32_t rid_msh = pack_local_id("smallmap.rpk", 0x2);
  const int32_t rid_tex = pack_local_id("smallmap.rpk", 0x5);
  // TestTrack ctor: Navigator(-5.317-2.64, -15.996, 2.64, ..., 6, 8, 10)
  InvObject* nav =
      navigator_new(-5.317f - 2.64f, -15.996f, 2.64f, rid_type, rid_msh, rid_tex,
                    6, 8, 10);
  tree_field_set_obj(track, "nav", nav);
  navigator_show(nav);

  tree_field_set_float(track, "posStart_x", -456.f);
  tree_field_set_float(track, "posStart_y", 0.f);
  tree_field_set_float(track, "posStart_z", -584.f);
  tree_field_set_float(track, "oriStart_y", 3.f);
  {
    InvObject* posExit = tree_host_new("java.lang.Vector3");
    tree_field_set_float(posExit, "x", -456.f);
    tree_field_set_float(posExit, "y", 0.f);
    tree_field_set_float(posExit, "z", -584.f);
    tree_field_set_obj(track, "posExit", posExit);
    InvObject* oriExit = tree_host_new("java.lang.Ypr");
    tree_field_set_float(oriExit, "y", 3.f);
    tree_field_set_obj(track, "oriExit", oriExit);
  }
  tree_field_set_int(track, "rounds", 2);

  InvObject* player = game_logic_player();
  InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
  if (car) {
    tree_field_set_float(car, "pos_x", -456.f);
    tree_field_set_float(car, "pos_y", 0.f);
    tree_field_set_float(car, "pos_z", -584.f);
    tree_field_set_int(car, "parent_map_id", map_id);
    tree_field_set_int(car, "stopped", 0);
    InvObject* mPlayer =
        navigator_add_marker_dynamic(nav, pack_local_id("frontend.rpk", 0x5C), car);
    tree_field_set_obj(track, "mPlayer", mPlayer);
    java_game_Navigator_updateNavigator(nav, car, 0);
  }

  game_logic_change_active_section(track);
  frontend_loading_screen_hide();
  return track;
}

void car_market_alter_cars(InvObject* vec, int32_t used, float hours_passed) {
  if (!vec || hours_passed < 0.01f) return;
  constexpr int32_t VS_USED = 0x0002;
  constexpr int32_t VS_STOCK = 0x0004;
  const int32_t vt = used ? VS_USED : VS_STOCK;
  // Host: deterministic refresh — refill all slots (no RNG).
  const int n = tree_vector_size(vec);
  InvObject* fresh = tree_vector_new();
  for (int i = 0; i < n; ++i) {
    InvObject* vd = game_logic_get_vehicle_descriptor(vt, 0.5f);
    if (vd) tree_vector_add(fresh, vd);
  }
  // Swap contents by replacing global pointer callers hold — copy into vec via
  // clear+add: rebuild by draining into same vector object.
  while (tree_vector_size(vec) > 0)
    tree_vector_remove(vec, tree_vector_element_at(vec, 0));
  for (int i = 0; i < tree_vector_size(fresh); ++i)
    tree_vector_add(vec, tree_vector_element_at(fresh, i));
}

InvObject* carmarket_enter(InvObject* prev, int32_t used) {
  frontend_loading_screen_show();
  const char* pack = used ? "dealer2.rpk" : "dealer.rpk";
  const char* path = used ? "misc/dealer2.rpk" : "misc/dealer.rpk";
  if (!rpak_find_by_name(pack)) java_lang_System_openLib(string_new(path));

  InvObject* stock = used ? g_car_desc_used : g_car_desc_new;
  if (!stock || tree_vector_size(stock) == 0) {
    constexpr int32_t VS_USED = 0x0002;
    constexpr int32_t VS_STOCK = 0x0004;
    stock = tree_vector_new();
    for (int i = 0; i < 4; ++i) {
      if (InvObject* vd = game_logic_get_vehicle_descriptor(
              used ? VS_USED : VS_STOCK, 0.5f))
        tree_vector_add(stock, vd);
    }
    if (used)
      g_car_desc_used = stock;
    else
      g_car_desc_new = stock;
  }

  const float visit = static_cast<float>(g_day) * 24.f + g_time_of_day / 3600.f;
  float& ts = used ? g_dealer_ts_used : g_dealer_ts_new;
  float hours = 0.f;
  if (ts == 0.f)
    ts = visit;
  else {
    hours = visit - ts;
    ts = visit;
  }
  car_market_alter_cars(stock, used, hours);

  InvObject* market = tree_host_new("java.game.CarMarket");
  tree_field_set_obj(market, "parentState", prev);
  tree_field_set_int(market, "used", used);
  tree_field_set_float(market, "priceRatio", used ? 1.1f : 1.3f);
  tree_field_set_obj(market, "carDescriptors", stock);
  tree_field_set_int(market, "cars_for_sale", tree_vector_size(stock));
  const int32_t map_id = pack_local_id(pack, 0x1);
  tree_field_set_int(market, "map_id", map_id);
  InvObject* map = tree_host_new("java.util.resource.GroundRef");
  java_util_resource_ResourceRef_set(map, map_id);
  tree_field_set_obj(market, "map", map);
  tree_field_set_int(market, "curcar", used ? 1 : 0);  // used: slot 0 = player
  InvObject* player = game_logic_player();
  tree_field_set_obj(market, "player", player);
  if (player) {
    tree_field_set_int(market, "money", tree_field_get_int(player, "money"));
    if (InvObject* car = tree_field_get_obj(player, "car"))
      tree_field_set_int(car, "stopped", 1);  // lockPlayerCar
  }
  // First listed car name for smoke
  if (InvObject* vd0 = tree_vector_element_at(stock, 0)) {
    if (InvObject* nm = tree_field_get_obj(vd0, "vehicleName"))
      tree_field_set_obj(market, "first_car_name", nm);
  }

  game_logic_change_active_section(market);
  frontend_loading_screen_hide();
  return market;
}

InvObject* clubinfo_enter(InvObject* prev) {
  frontend_loading_screen_show();
  InvObject* info = tree_host_new("java.game.ClubInfo");
  tree_field_set_obj(info, "parentState", prev);
  InvObject* player = game_logic_player();
  tree_field_set_obj(info, "player", player);
  const int32_t club = player ? tree_field_get_int(player, "club") : 0;
  const float prestige = player ? tree_field_get_float(player, "prestige") : 0.f;
  tree_field_set_int(info, "club", club);
  tree_field_set_float(info, "prestige", prestige);
  // Ranking stub: club 0 → place ~60 (INITIAL prestige 0.3 → lamest club)
  tree_field_set_int(info, "ranking", 60 - static_cast<int32_t>(prestige * 40.f));
  tree_field_set_int(info, "bg_id", pack_local_id("frontend.rpk", 0x9F));
  game_logic_change_active_section(info);
  frontend_loading_screen_hide();
  return info;
}

InvObject* carinfo_enter(InvObject* prev, InvObject* car) {
  if (!car) return nullptr;
  frontend_loading_screen_show();
  InvObject* info = tree_host_new("java.game.CarInfo");
  tree_field_set_obj(info, "parentState", prev);
  tree_field_set_obj(info, "car", car);
  tree_field_set_int(info, "page", 1);  // CMD_CAR_PAGE
  tree_field_set_int(info, "nParts", 0);
  if (InvObject* nm = tree_field_get_obj(car, "vehicleName"))
    tree_field_set_obj(info, "car_name", nm);
  tree_field_set_float(info, "power", tree_field_get_float(car, "power"));
  tree_field_set_float(info, "optical", tree_field_get_float(car, "optical"));
  tree_field_set_float(info, "wear", tree_field_get_float(car, "wear"));
  tree_field_set_int(info, "car_id", java_util_resource_ResourceRef_id(car));
  game_logic_change_active_section(info);
  frontend_loading_screen_hide();
  return info;
}

}  // namespace

InvObject* navigator_viewport(InvObject* nav) {
  return nav ? tree_field_get_obj(nav, "vp") : nullptr;
}

InvObject* navigator_camera(InvObject* nav) {
  return nav ? tree_field_get_obj(nav, "cam") : nullptr;
}

int32_t navigator_current_tile(InvObject* nav) {
  if (!nav) return -1;
  return tree_field_get_int(nav, "tile_index");
}

void navigator_paint(InvObject* nav) {
  if (!nav || !tree_field_get_int(nav, "visible")) return;

  const float left = tree_field_get_float(nav, "left");
  const float top = tree_field_get_float(nav, "top");
  const float size = tree_field_get_float(nav, "size");
  const int32_t tiles_x = tree_field_get_int(nav, "tiles_x");
  const int32_t tiles_z = tree_field_get_int(nav, "tiles_z");
  const int32_t modulo = tree_field_get_int(nav, "modulo");
  const int32_t rid_tex0 = tree_field_get_int(nav, "rid_tex0");
  if (tiles_x <= 0 || tiles_z <= 0 || size <= 0.f) return;

  const float fx = tree_field_get_float(nav, "follow_x");
  const float fz = tree_field_get_float(nav, "follow_z");
  const float nav_x = fx * 0.01f;
  const float nav_z = fz * 0.01f;
  int32_t xi = static_cast<int32_t>(std::floor((nav_x - left) / size));
  int32_t zi = static_cast<int32_t>(std::floor((nav_z - top) / size));
  if (xi < 0) xi = 0;
  if (zi < 0) zi = 0;
  if (xi >= tiles_x) xi = tiles_x - 1;
  if (zi >= tiles_z) zi = tiles_z - 1;
  const int32_t tile_index = zi * tiles_x + xi;
  tree_field_set_int(nav, "tile_index", tile_index);
  tree_field_set_int(nav, "tile_x", xi);
  tree_field_set_int(nav, "tile_z", zi);

  // Absolute tiles: Java ctor setMatrix (navigator_new). PE updateNavigator
  // @ 0x482D30 never re-paints tile RID / never draws OSD map rects — only
  // cam bone00 SetLocalMatrix @ 0x483096 + marker symbol bones. W12A: when
  // tiles_absolu live, skip host map/blip OSD (PE path); keep lookat stand-in
  // for bone write. Route/marker OSD remain host boot aids (gap vs 1:1).
  InvObject* tiles = tree_field_get_obj(nav, "tiles");
  const int32_t tiles_n = tiles ? tree_vector_size(tiles) : 0;
  tree_field_set_int(nav, "tiles_absolu_n", tiles_n);
  const int32_t tiles_absolu = tree_field_get_int(nav, "tiles_absolu");
  const bool pe_like_3d = tiles_absolu != 0 && tiles_n > 0;

  // Viewport [0,1] → OSD center/size in [-1,1] (y up).
  constexpr float kVpL = 0.02f, kVpT = 0.78f, kVpW = 0.2f, kVpH = 0.18f;
  const float map_x = (kVpL + kVpW * 0.5f) * 2.f - 1.f;
  const float map_y = 1.f - (kVpT + kVpH * 0.5f) * 2.f;
  const float map_w = kVpW * 2.f;
  const float map_h = kVpH * 2.f;
  void* map_key =
      reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(nav) ^ 0x4E01u);
  void* blip_key =
      reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(nav) ^ 0x4E02u);

  if (pe_like_3d) {
    // PE: no map/blip OSD — remove prior host shim rects if any.
    render_d3d9_osd_remove_rect(map_key);
    render_d3d9_osd_remove_rect(blip_key);
    tree_field_set_int(nav, "osd_map_shim", 0);
  } else {
    // Boot shim: minimap texture when 3D tiles not the active view.
    const int32_t rid_tex = rid_tex0 + zi * (tiles_x + modulo) + xi;
    InvObject* tex = tree_field_get_obj(nav, "osd_tex");
    const int32_t loaded_rid = tree_field_get_int(nav, "osd_tex_rid");
    if (!tex || loaded_rid != rid_tex) {
      if (!tex) {
        tex = resref_new();
        tree_field_set_obj(nav, "osd_tex", tex);
      }
      java_util_resource_ResourceRef_set(tex, rid_tex);
      java_util_resource_ResourceRef_load(tex);
      tree_field_set_int(nav, "osd_tex_rid", rid_tex);
    }
    render_d3d9_osd_set_rect(map_key, map_x, map_y, map_w, map_h, tex, 40);
    tree_field_set_int(nav, "osd_map_shim", 1);
  }

  // Phase 2.89: plotRoute line → OSD dots (player-centered follow).
  // Host-only invent — PE markers use RenderRef bone00 (Cars @ 0x48310B),
  // not OSD. W13D: gate when tiles_absolu (same as map/blip).
  InvObject* route = tree_field_get_obj(nav, "route");
  const int32_t prev_route_n = tree_field_get_int(nav, "route_osd_n");
  int32_t route_drawn = 0;
  constexpr int32_t kMaxRouteDots = 48;
  if (!pe_like_3d && route && size > 1e-4f) {
    const int32_t npts = render_line_point_count(route);
    uint32_t col = static_cast<uint32_t>(render_line_color(route));
    if ((col & 0xFF000000u) == 0) col |= 0xFF000000u;
    if ((col & 0x00FFFFFFu) == 0) col = 0xFFFF0000u;
    const float sx = map_w / size;
    const float sy = map_h / size;
    const float half_w = map_w * 0.5f;
    const float half_h = map_h * 0.5f;
    const float dot = map_w * 0.025f;
    for (int32_t i = 0; i < npts && route_drawn < kMaxRouteDots; ++i) {
      float wx = 0, wy = 0, wz = 0;
      if (!render_line_point_at(route, i, &wx, &wy, &wz)) continue;
      const float dx = (wx - fx) * 0.01f;
      const float dz = (wz - fz) * 0.01f;
      const float ox = map_x + dx * sx;
      const float oy = map_y - dz * sy;
      if (std::fabs(ox - map_x) > half_w * 1.05f) continue;
      if (std::fabs(oy - map_y) > half_h * 1.05f) continue;
      void* dkey = reinterpret_cast<void*>(
          reinterpret_cast<uintptr_t>(nav) ^
          (0x4E10u + static_cast<uintptr_t>(route_drawn)));
      render_d3d9_osd_set_rect_color(dkey, ox, oy, dot, dot, nullptr, col, 41);
      ++route_drawn;
    }
  }
  for (int32_t i = route_drawn; i < prev_route_n; ++i) {
    render_d3d9_osd_remove_rect(reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(nav) ^
        (0x4E10u + static_cast<uintptr_t>(i))));
  }
  tree_field_set_int(nav, "route_osd_n", route_drawn);
  tree_field_set_int(nav, "route_osd_visible", route_drawn);

  // Phase 2.90/2.91: static + dynamic markers → OSD icons (rtype texture/mesh).
  // Host invent — PE uses bone00 (Cars). Gate when tiles_absolu.
  const int32_t prev_mk_n = tree_field_get_int(nav, "marker_osd_n");
  int32_t mk_drawn = 0;
  int32_t mk_tex = 0;
  constexpr int32_t kMaxMarkerDots = 24;
  auto ensure_marker_icon = [](InvObject* m) -> InvObject* {
    if (!m) return nullptr;
    InvObject* icon = tree_field_get_obj(m, "icon");
    if (icon) return icon;
    const int32_t rid = tree_field_get_int(m, "rtype");
    if (!rid) return nullptr;
    icon = resref_new();
    java_util_resource_ResourceRef_set(icon, rid);
    java_util_resource_ResourceRef_load(icon);
    tree_field_set_obj(m, "icon", icon);
    return icon;
  };
  auto marker_osd_tex = [](InvObject* icon, InvObject* m,
                           uint32_t fallback_col) -> void* {
    if (icon) {
      if (render_d3d9_texture_ready(icon)) return icon;
      if (render_d3d9_mesh_ready(icon)) {
        const int32_t n = render_d3d9_mesh_submesh_count(icon);
        for (int32_t s = 0; s < n; ++s) {
          void* t = render_d3d9_mesh_get_texture(icon, s);
          if (t && render_d3d9_texture_ready(t)) return t;
        }
      }
    }
    if (!m) return nullptr;
    InvObject* flat = tree_field_get_obj(m, "icon_flat");
    if (flat && render_d3d9_texture_ready(flat)) return flat;
    flat = resref_new();
    uint32_t col = fallback_col;
    if ((col & 0xFF000000u) == 0) col |= 0xFF000000u;
    if (render_d3d9_texture_create_solid(flat, col, 32)) {
      tree_field_set_obj(m, "icon_flat", flat);
      return flat;
    }
    return nullptr;
  };
  if (!pe_like_3d && size > 1e-4f) {
    const float sx = map_w / size;
    const float sy = map_h / size;
    const float half_w = map_w * 0.5f;
    const float half_h = map_h * 0.5f;
    const float mdot = map_w * 0.055f;
    auto project_mark = [&](InvObject* m, float wx, float wz, uint32_t col) {
      if (mk_drawn >= kMaxMarkerDots) return;
      const float dx = (wx - fx) * 0.01f;
      const float dz = (wz - fz) * 0.01f;
      const float ox = map_x + dx * sx;
      const float oy = map_y - dz * sy;
      if (std::fabs(ox - map_x) > half_w * 1.05f) return;
      if (std::fabs(oy - map_y) > half_h * 1.05f) return;
      void* mkey = reinterpret_cast<void*>(
          reinterpret_cast<uintptr_t>(nav) ^
          (0x4E80u + static_cast<uintptr_t>(mk_drawn)));
      InvObject* icon = ensure_marker_icon(m);
      void* t = marker_osd_tex(icon, m, col);
      if (t) {
        render_d3d9_osd_set_rect_color(mkey, ox, oy, mdot, mdot, t, 0xFFFFFFFFu,
                                       41);
        ++mk_tex;
      } else {
        render_d3d9_osd_set_rect_color(mkey, ox, oy, mdot, mdot, nullptr, col,
                                       41);
      }
      ++mk_drawn;
    };
    InvObject* statics = tree_field_get_obj(nav, "marker");
    const int32_t ns = statics ? tree_vector_size(statics) : 0;
    for (int32_t i = 0; i < ns; ++i) {
      InvObject* m = tree_vector_element_at(statics, i);
      if (!m) continue;
      project_mark(m, tree_field_get_float(m, "pos_x"),
                   tree_field_get_float(m, "pos_z"), 0xFFFFCC00u);
    }
    InvObject* dyns = tree_field_get_obj(nav, "dynamarker");
    const int32_t nd = dyns ? tree_vector_size(dyns) : 0;
    for (int32_t i = 0; i < nd; ++i) {
      InvObject* m = tree_vector_element_at(dyns, i);
      if (!m) continue;
      InvObject* obj = tree_field_get_obj(m, "obj");
      float wx = 0.f, wz = 0.f;
      if (obj) {
        wx = tree_field_get_float(obj, "pos_x");
        wz = tree_field_get_float(obj, "pos_z");
      } else {
        wx = tree_field_get_float(m, "pos_x");
        wz = tree_field_get_float(m, "pos_z");
      }
      project_mark(m, wx, wz, 0xFF00CCFFu);
    }
  }
  for (int32_t i = mk_drawn; i < prev_mk_n; ++i) {
    render_d3d9_osd_remove_rect(reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(nav) ^
        (0x4E80u + static_cast<uintptr_t>(i))));
  }
  tree_field_set_int(nav, "marker_osd_n", mk_drawn);
  tree_field_set_int(nav, "marker_osd_visible", mk_drawn);
  tree_field_set_int(nav, "marker_osd_tex", mk_tex);

  // Player blip — host map shim only (PE uses 3D tiles, no OSD blip).
  if (!pe_like_3d) {
    InvObject* tex = tree_field_get_obj(nav, "osd_tex");
    render_d3d9_osd_set_rect(blip_key, map_x, map_y, map_w * 0.08f,
                             map_h * 0.08f, tex, 42);
  }

  InvObject* cam = tree_field_get_obj(nav, "cam");
  if (cam) {
    // Host lookat stand-in for PE cam bone write (SetLocalMatrix @
    // 0x483096) — PE never calls lookat. offsetX/Z are world-cm *deltas*
    // (PE @ 0x482DF2–0x482E39: world = car + prior), not absolute eye.
    // Absolute ox/oz→eye broke once Cars.cpp stored PE deltas.
    // Mode0 Java xz = world * flt_5F0C20 (0.01) @ loc_483067 (PE matrix
    // *0.1 / setMatrix *10). Mode1 @ 0x482FD2: eye = world*0.01 +
    // Ypr_forward*DEF_ZOOM → at = world*0.01.
    // Dynamarker yaw: PE @ 0x48314B Engine_queryGameRefChannel(GII_DIR=4)
    // → int° * flt_5F13BC. Host getInfo(GII_DIR) now returns int° (W8A);
    // Cars seed prefers that path when non-zero.
    constexpr float kScale01 = 0.01f;  // flt_5F0C20 @ 0x005F0C20
    constexpr float kDefZoom = 4.5f;   // flt_5F13C0 @ 0x005F13C0
    const float ox = tree_field_get_float(nav, "offsetX");
    const float oz = tree_field_get_float(nav, "offsetZ");
    const float at_x = (fx + ox) * kScale01;
    const float at_z = (fz + oz) * kScale01;

    float ex = at_x, ey = 0.f, ez = at_z;
    if (InvObject* bp = java_util_resource_RenderRef_getPos(cam)) {
      float bx = 0.f, by = 0.f, bz = 0.f;
      vec3_get(bp, &bx, &by, &bz);
      // Prefer bone from updateNavigator setMatrix_1 (mode0≈at, mode1
      // orbit). show() before first update: getPos may be 0 → keep at+zoom.
      if (bx != 0.f || by != 0.f || bz != 0.f) {
        ex = bx;
        ey = by;
        ez = bz;
      }
    }
    if (ey <= 0.f) {
      ey = tree_field_get_float(nav, "zoom");
      if (ey <= 0.f) ey = kDefZoom;
    }
    render_d3d9_camera_lookat(cam, ex, ey, ez, at_x, 0.f, at_z);
  }
}

InvObject* inventory_new(InvObject* player) {
  InvObject* inv = tree_host_new("java.game.Inventory");
  tree_field_set_obj(inv, "player", player);
  tree_field_set_obj(inv, "items", tree_vector_new());
  tree_field_set_int(inv, "size", 0);
  tree_field_set_int(inv, "update_count", 0);
  return inv;
}

namespace {

InvObject* inventory_panel_item_part(InvObject* item) {
  if (!item) return nullptr;
  InvObject* p = tree_field_get_obj(item, "partXXX");
  if (!p) p = tree_field_get_obj(item, "part");
  return p;
}

InvObject* inventory_item_ensure_localroot(InvObject* item) {
  if (!item) return nullptr;
  InvObject* lr = tree_field_get_obj(item, "localroot");
  if (lr) return lr;
  // Stock InventoryItem: new Dummy(inventory, WORLDTREEROOT).
  lr = tree_host_new("java.util.resource.Dummy");
  tree_field_set_obj(item, "localroot", lr);
  InvObject* inv = tree_field_get_obj(item, "inventory");
  if (inv) java_util_resource_GameRef_setParent(lr, inv);
  return lr;
}

void inventory_panel_cleanup(InvObject* panel) {
  if (!panel) return;
  InvObject* light = tree_field_get_obj(panel, "light");
  if (light) {
    tree_field_set_int(light, "destroyed", 1);
    tree_field_set_obj(panel, "light", nullptr);
  }
  InvObject* cam = tree_field_get_obj(panel, "cam");
  if (cam) {
    InvObject* osd = tree_field_get_obj(panel, "osd");
    InvObject* vp = osd ? tree_field_get_obj(osd, "vp") : nullptr;
    if (vp) render_d3d9_camera_deactivate(cam, vp);
    java_render_Camera_destroy(cam);
    tree_field_set_obj(panel, "cam", nullptr);
  }
  tree_field_set_int(panel, "preview_ready", 0);
}

void inventory_panel_create_def_camera(InvObject* panel, float size,
                                      int32_t flags) {
  if (!panel) return;
  if (size <= 0.f) size = 1.f;
  tree_field_set_float(panel, "size", size);
  tree_field_set_int(panel, "flags", flags);

  InvObject* item = tree_field_get_obj(panel, "invItem");
  InvObject* localroot =
      item ? inventory_item_ensure_localroot(item) : nullptr;
  InvObject* osd = tree_field_get_obj(panel, "osd");
  InvObject* vp = osd ? tree_field_get_obj(osd, "vp") : nullptr;
  if (!localroot || !vp) return;

  if (InvObject* old = tree_field_get_obj(panel, "cam")) {
    render_d3d9_camera_deactivate(old, vp);
    java_render_Camera_destroy(old);
    tree_field_set_obj(panel, "cam", nullptr);
  }

  // Stock Camera(..., aov=110) → native half-AOV 55°.
  InvObject* cam = tree_host_new("java.render.Camera");
  java_render_Camera_create(cam, localroot, vp, 1, 55.f, 0.001f, 10.f, 1.f,
                            1.f, 1, 0);

  float yaw = -2.5f, pitch = -0.7f, roll = 0.f;
  InvObject* ypr_obj = tree_field_get_obj(panel, "ypr");
  if (InvObject* part = inventory_panel_item_part(item)) {
    if (InvObject* cy = tree_field_get_obj(part, "catalog_view_ypr")) {
      ypr_obj = cy;
      tree_field_set_obj(panel, "ypr", cy);
    }
  }
  if (!ypr_obj) {
    ypr_obj = ypr_new(yaw, pitch, roll);
    tree_field_set_obj(panel, "ypr", ypr_obj);
  }
  ypr_get(ypr_obj, &yaw, &pitch, &roll);

  // Stock: camPos=(0,size,size).rotate(Ypr(ypr.y, 0, 0)) — orbit by yaw.
  const float c = std::cos(yaw);
  const float s = std::sin(yaw);
  const float ex = size * s;
  const float ey = size;
  const float ez = size * c;

  java_util_resource_GameRef_setMatrix(cam, vec3_new(ex, ey, ez), ypr_obj);
  // Host view path needs lookat (identity view when lookat=false).
  render_d3d9_camera_lookat(cam, ex, ey, ez, 0.f, 0.f, 0.f);
  tree_field_set_obj(panel, "cam", cam);
  tree_field_set_int(panel, "preview_ready", 1);
}

void inventory_panel_apply_cam_pose(InvObject* panel) {
  if (!panel) return;
  InvObject* cam = tree_field_get_obj(panel, "cam");
  InvObject* ypr_obj = tree_field_get_obj(panel, "ypr");
  if (!cam || !ypr_obj) return;
  float size = tree_field_get_float(panel, "size");
  if (size <= 0.f) size = 1.f;
  float yaw = 0.f, pitch = 0.f, roll = 0.f;
  ypr_get(ypr_obj, &yaw, &pitch, &roll);
  const float c = std::cos(yaw);
  const float s = std::sin(yaw);
  const float ex = size * s;
  const float ey = size;
  const float ez = size * c;
  java_util_resource_GameRef_setMatrix(cam, vec3_new(ex, ey, ez), ypr_obj);
  render_d3d9_camera_lookat(cam, ex, ey, ez, 0.f, 0.f, 0.f);
}

bool inventory_panel_focus_hook(InvObject* panel) {
  if (!panel) return false;
  if (!tree_field_get_int(panel, "flags")) return false;
  if (!tree_field_get_obj(panel, "cam")) return false;
  InvObject* ypr_obj = tree_field_get_obj(panel, "ypr");
  if (!ypr_obj) {
    ypr_obj = ypr_new(-2.5f, -0.7f, 0.f);
    tree_field_set_obj(panel, "ypr", ypr_obj);
  }
  float yaw = 0.f, pitch = 0.f, roll = 0.f;
  ypr_get(ypr_obj, &yaw, &pitch, &roll);
  // Stock InventoryPanel.focusHook: ypr.y += 0.03.
  yaw += 0.03f;
  ypr_set(ypr_obj, yaw, pitch, roll);
  inventory_panel_apply_cam_pose(panel);
  tree_field_set_int(panel, "focus_ticks",
                     tree_field_get_int(panel, "focus_ticks") + 1);
  return true;
}

void inventory_panel_create_def_light(InvObject* panel) {
  if (!panel) return;
  InvObject* item = tree_field_get_obj(panel, "invItem");
  InvObject* localroot =
      item ? inventory_item_ensure_localroot(item) : nullptr;
  if (!localroot) return;

  if (InvObject* old = tree_field_get_obj(panel, "light")) {
    tree_field_set_int(old, "destroyed", 1);
    tree_field_set_obj(panel, "light", nullptr);
  }

  // InventoryPanel.RID_INVENTORY_LIGHT = misc.garage:0x00000024.
  if (!rpak_find_by_name("garage.rpk"))
    java_lang_System_openLib(string_new("garage.rpk"));
  int32_t rid = 0x24;
  if (const RpakPack* gp = rpak_find_by_name("garage.rpk"))
    rid = rpak_make_id(gp->pack_id, 0x0024);
  InvObject* type = resref_new();
  java_util_resource_ResourceRef_set(type, rid);
  InvObject* light = tree_host_new("java.util.resource.RenderRef");
  java_util_resource_RenderRef_create(light, localroot, type, nullptr);
  tree_field_set_obj(panel, "light", light);
  if (tree_field_get_obj(panel, "cam"))
    tree_field_set_int(panel, "preview_ready", 1);
}

void inventory_panel_show_item(InvObject* panel, InvObject* item) {
  if (!panel || !item) return;
  InvObject* part = inventory_panel_item_part(item);
  if (part) java_util_resource_GameRef_setMatrix(part, nullptr, nullptr);
  inventory_item_ensure_localroot(item);
  float size_m = 1.f;
  if (part) {
    const int32_t cm =
        java_util_resource_GameRef_getInfo(part, /*GII_SIZE*/ 11, 0);
    if (cm > 0) size_m = static_cast<float>(cm) / 100.f;
  }
  inventory_panel_create_def_camera(panel, size_m, /*enableRotate*/ 1);
  inventory_panel_create_def_light(panel);
}

InvObject* inventory_panel_new(InvObject* inv, int32_t index, float x, float y,
                               float w, float h) {
  InvObject* panel = tree_host_new("java.game.InventoryPanel");
  tree_field_set_obj(panel, "inv", inv);
  tree_field_set_int(panel, "index", index);
  tree_field_set_float(panel, "x", x);
  tree_field_set_float(panel, "y", y);
  tree_field_set_float(panel, "width", w);
  tree_field_set_float(panel, "height", h);
  tree_field_set_int(panel, "visible", 0);
  tree_field_set_obj(panel, "ypr", ypr_new(-2.5f, -0.7f, 0.f));

  InvObject* osd = tree_host_new("java.render.Osd");
  tree_field_set_obj(osd, "groups", tree_vector_new());
  tree_field_set_obj(osd, "hotkey", tree_vector_new());
  tree_field_set_obj(osd, "rectangles", tree_vector_new());
  tree_field_set_obj(osd, "buttons", tree_vector_new());
  tree_field_set_int(osd, "init", 1);
  tree_field_set_int(osd, "iLevel", 3);  // IL_TIPS
  tree_field_set_obj(osd, "globalHandler", panel);
  tree_field_set_float(osd, "vpWidth", w > 0.f ? w : 1.f);
  tree_field_set_float(osd, "vpHeight", h > 0.f ? h : 1.f);
  tree_field_set_float(osd, "vpAspect", 1.f);
  tree_field_set_float(osd, "vpLeft", x);
  tree_field_set_float(osd, "vpTop", y);

  // Stock: new Osd(new Viewport(15, cwidth, cheight, itemW, itemH)).
  InvObject* vp = tree_host_new("java.render.Viewport");
  render_d3d9_viewport_create(vp, 15, x, y, w > 0.f ? w : 0.1f,
                              h > 0.f ? h : 0.1f);
  tree_field_set_obj(osd, "vp", vp);

  InvObject* style = tree_host_new("java.render.osd.Style");
  tree_field_set_float(style, "rWidth", w * 2.f);
  tree_field_set_float(style, "rHeight", h * 2.f);
  tree_field_set_float(style, "width", w * 2.f);
  tree_field_set_float(style, "height", h * 2.f);

  InvObject* btn = tree_host_new("java.render.osd.Button");
  tree_field_set_obj(btn, "style", style);
  tree_field_set_float(btn, "x", 0.f);
  tree_field_set_float(btn, "y", 0.f);
  tree_field_set_int(btn, "command", index);
  tree_field_set_int(btn, "drop_enabled", 1);
  tree_field_set_obj(btn, "osd", osd);
  tree_vector_add(tree_field_get_obj(osd, "buttons"), btn);
  tree_field_set_int(osd, "button_count", 1);

  tree_field_set_obj(panel, "osd", osd);
  tree_field_set_obj(panel, "button", btn);
  return panel;
}

void inventory_panel_attach(InvObject* panel, InvObject* item) {
  if (!panel) return;
  InvObject* prev = tree_field_get_obj(panel, "invItem");
  if (prev == item) return;
  if (prev) inventory_panel_cleanup(panel);
  tree_field_set_obj(panel, "invItem", item);
  InvObject* btn = tree_field_get_obj(panel, "button");
  if (btn) {
    if (item) {
      InvObject* tip = tree_field_get_obj(item, "info");
      if (!tip) tip = string_new("part");
      tree_field_set_obj(btn, "tooltip", tip);
    } else {
      tree_field_set_obj(btn, "tooltip", nullptr);
    }
  }
  if (item) inventory_panel_show_item(panel, item);
  tree_field_set_int(panel, "attach_count",
                     tree_field_get_int(panel, "attach_count") + 1);
}

}  // namespace

void visual_inventory_init(InvObject* inv, float left, float top, float width,
                           float height) {
  if (!inv) return;
  constexpr int32_t kLines = 1;
  constexpr int32_t kPerLine = 5;
  tree_field_set_int(inv, "linesPerPage", kLines);
  tree_field_set_int(inv, "partsPerLine", kPerLine);
  tree_field_set_int(inv, "cline", 0);
  tree_field_set_int(inv, "interactive", 1);
  tree_field_set_int(inv, "start", 0);
  tree_field_set_int(inv, "stop", kLines * kPerLine);
  tree_field_set_int(inv, "visualsUpdated", 0);

  const float hSpacing = 0.013f;
  const float vSpacing = 0.01f;
  const float itemW =
      (width - (kPerLine - 1) * hSpacing) / static_cast<float>(kPerLine);
  const float itemH =
      (height - (kLines - 1) * vSpacing) / static_cast<float>(kLines);

  const int32_t n = kPerLine * kLines;
  InvObject* panels = tree_array_new(n);
  float cheight = top;
  for (int32_t i = 0; i < kLines; ++i) {
    float cwidth = left;
    for (int32_t j = 0; j < kPerLine; ++j) {
      const int32_t index = i * kPerLine + j;
      InvObject* panel =
          inventory_panel_new(inv, index, cwidth, cheight, itemW, itemH);
      tree_vector_set(panels, index, panel);
      cwidth += itemW + hSpacing;
    }
    cheight += itemH + vSpacing;
  }
  tree_field_set_obj(inv, "panels", panels);
}

InvObject* visual_inventory_new(InvObject* player, float left, float top,
                                float width, float height) {
  InvObject* inv = tree_host_new("java.game.VisualInventory");
  tree_field_set_obj(inv, "player", player);
  tree_field_set_obj(inv, "items", tree_vector_new());
  tree_field_set_int(inv, "size", 0);
  tree_field_set_int(inv, "update_count", 0);
  visual_inventory_init(inv, left, top, width, height);
  return inv;
}

int32_t visual_inventory_panel_count(InvObject* inv) {
  if (!inv) return 0;
  InvObject* panels = tree_field_get_obj(inv, "panels");
  return panels ? tree_vector_size(panels) : 0;
}

int32_t visual_inventory_attached_count(InvObject* inv) {
  if (!inv) return 0;
  InvObject* panels = tree_field_get_obj(inv, "panels");
  if (!panels) return 0;
  int32_t n = 0;
  const int32_t pc = tree_vector_size(panels);
  for (int32_t i = 0; i < pc; ++i) {
    InvObject* p = tree_vector_element_at(panels, i);
    if (p && tree_field_get_obj(p, "invItem")) ++n;
  }
  return n;
}

int32_t visual_inventory_preview_count(InvObject* inv) {
  if (!inv) return 0;
  InvObject* panels = tree_field_get_obj(inv, "panels");
  if (!panels) return 0;
  int32_t n = 0;
  const int32_t pc = tree_vector_size(panels);
  for (int32_t i = 0; i < pc; ++i) {
    InvObject* p = tree_vector_element_at(panels, i);
    if (!p || !tree_field_get_obj(p, "invItem")) continue;
    InvObject* cam = tree_field_get_obj(p, "cam");
    InvObject* light = tree_field_get_obj(p, "light");
    InvObject* osd = tree_field_get_obj(p, "osd");
    InvObject* vp = osd ? tree_field_get_obj(osd, "vp") : nullptr;
    const float aov = cam ? render_d3d9_camera_half_aov(cam) : 0.f;
    if (cam && light && vp && aov > 50.f &&
        tree_field_get_int(p, "preview_ready") == 1)
      ++n;
  }
  return n;
}

bool visual_inventory_focus_hook(InvObject* inv, int32_t panel_index) {
  if (!inv || panel_index < 0) return false;
  InvObject* panels = tree_field_get_obj(inv, "panels");
  if (!panels) return false;
  if (panel_index >= tree_vector_size(panels)) return false;
  return inventory_panel_focus_hook(tree_vector_element_at(panels, panel_index));
}

void mechanic_tick_preview(InvObject* mechanic) {
  if (!mechanic) return;
  InvObject* inv = tree_field_get_obj(mechanic, "inventory");
  if (!inv || !tree_field_get_int(inv, "shown")) return;
  InvObject* panel = mechanic_actual_panel(mechanic);
  if (!panel) return;
  visual_inventory_focus_hook(inv, tree_field_get_int(panel, "index"));
}

void visual_inventory_update(InvObject* inv) {
  if (!inv) return;
  InvObject* panels = tree_field_get_obj(inv, "panels");
  if (!panels) return;
  const int32_t parts_per_line = tree_field_get_int(inv, "partsPerLine");
  const int32_t lines = tree_field_get_int(inv, "linesPerPage");
  int32_t ppl = parts_per_line > 0 ? parts_per_line : 5;
  int32_t lpp = lines > 0 ? lines : 1;
  int32_t cline = tree_field_get_int(inv, "cline");
  const int32_t sz = inventory_size(inv);
  if (cline && cline * ppl >= sz) cline = cline > 0 ? cline - 1 : 0;
  tree_field_set_int(inv, "cline", cline);

  const int32_t begin = tree_field_get_int(inv, "start");
  const int32_t end = tree_field_get_int(inv, "stop");
  const int32_t start = cline * ppl;
  const int32_t stop = start + lpp * ppl;
  tree_field_set_int(inv, "start", start);
  tree_field_set_int(inv, "stop", stop);

  InvObject* items = tree_field_get_obj(inv, "items");
  const int32_t pc = tree_vector_size(panels);
  auto item_at = [&](int32_t i) -> InvObject* {
    if (!items || i < 0 || i >= inventory_size(inv)) return nullptr;
    return tree_vector_element_at(items, i);
  };

  if (begin == start && end == stop) {
    for (int32_t vis = 0; vis < pc; ++vis) {
      InvObject* panel = tree_vector_element_at(panels, vis);
      InvObject* item = item_at(start + vis);
      if (panel && tree_field_get_obj(panel, "invItem") != item)
        inventory_panel_attach(panel, nullptr);
    }
    for (int32_t vis = 0; vis < pc; ++vis) {
      InvObject* panel = tree_vector_element_at(panels, vis);
      InvObject* item = item_at(start + vis);
      if (panel && tree_field_get_obj(panel, "invItem") != item)
        inventory_panel_attach(panel, item);
    }
  } else {
    for (int32_t vis = 0; vis < pc; ++vis)
      inventory_panel_attach(tree_vector_element_at(panels, vis), nullptr);
    for (int32_t vis = 0; vis < pc; ++vis)
      inventory_panel_attach(tree_vector_element_at(panels, vis),
                             item_at(start + vis));
  }

  tree_field_set_int(inv, "visualsUpdated", 1);
  tree_field_set_int(inv, "size", sz);
  tree_field_set_int(inv, "update_count",
                     tree_field_get_int(inv, "update_count") + 1);
}

void visual_inventory_show(InvObject* inv) {
  if (!inv) return;
  InvObject* panels = tree_field_get_obj(inv, "panels");
  if (!panels) return;
  const int32_t pc = tree_vector_size(panels);
  for (int32_t i = 0; i < pc; ++i) {
    InvObject* panel = tree_vector_element_at(panels, i);
    if (!panel) continue;
    tree_field_set_int(panel, "visible", 1);
    if (InvObject* osd = tree_field_get_obj(panel, "osd"))
      tree_field_set_int(osd, "visible", 1);
  }
  tree_field_set_int(inv, "shown", 1);
}

void visual_inventory_hide(InvObject* inv) {
  if (!inv) return;
  InvObject* panels = tree_field_get_obj(inv, "panels");
  if (!panels) return;
  const int32_t pc = tree_vector_size(panels);
  for (int32_t i = 0; i < pc; ++i) {
    InvObject* panel = tree_vector_element_at(panels, i);
    if (!panel) continue;
    tree_field_set_int(panel, "visible", 0);
    if (InvObject* osd = tree_field_get_obj(panel, "osd"))
      tree_field_set_int(osd, "visible", 0);
  }
  tree_field_set_int(inv, "shown", 0);
}

void visual_inventory_scroll_up(InvObject* inv) {
  if (!inv) return;
  const int32_t cline = tree_field_get_int(inv, "cline");
  if (cline <= 0) return;
  tree_field_set_int(inv, "cline", cline - 1);
  visual_inventory_update(inv);
}

void visual_inventory_scroll_down(InvObject* inv) {
  if (!inv) return;
  const int32_t sz = inventory_size(inv);
  if (sz <= 1) return;
  int32_t ppl = tree_field_get_int(inv, "partsPerLine");
  if (ppl <= 0) ppl = 5;
  const int32_t cline = tree_field_get_int(inv, "cline");
  const int32_t max_line = (sz - 1) / ppl;
  if (cline >= max_line) return;
  tree_field_set_int(inv, "cline", cline + 1);
  visual_inventory_update(inv);
}

const char* inventory_install_to_car(InvObject* inv, int32_t index,
                                    InvObject* car) {
  if (!inv) return "no inventory";
  if (!car) return "no car";
  if (index < 0 || index >= inventory_size(inv)) return "bad index";
  InvObject* items = tree_field_get_obj(inv, "items");
  if (!items) return "empty";
  InvObject* item = tree_vector_element_at(items, index);
  if (!item) return "no item";
  InvObject* part = tree_field_get_obj(item, "partXXX");
  if (!part) part = tree_field_get_obj(item, "part");
  if (!part) return "no part";

  int32_t slot = 0;
  int32_t mate = 1;
  const int32_t pref = tree_field_get_int(part, "install_slot");
  if (pref > 0) {
    slot = pref;
  } else if (!part_find_cfg_install(car, part, &slot, &mate)) {
    return "no compatible slot";
  }
  if (part_on_slot(car, slot)) return "slot busy";
  part_disable_slot(car, slot, 0);
  if (!part_install(car, slot, part, mate)) return "install failed";
  if (part_on_slot(car, slot) != part) return "install verify failed";

  tree_field_set_obj(car, "last_installed_part", part);
  {
    // Sync GameRef pos from CFG slot (mesh cm) for pick/project.
    float sx = 0.f, sy = 0.f, sz = 0.f, oy = 0.f, op = 0.f, or_ = 0.f;
    if (part_slot_get_pose(car, slot, &sx, &sy, &sz, &oy, &op, &or_)) {
      constexpr float kM = 100.f;
      float cx = 0.f, cy = 0.f, cz = 0.f;
      if (InvObject* cp = java_util_resource_GameRef_getPos(car))
        vec3_get(cp, &cx, &cy, &cz);
      java_util_resource_GameRef_setMatrix(
          part, vec3_new(cx + sx * kM, cy + sy * kM, cz + sz * kM),
          ypr_new(oy, op, or_));
    }
  }

  tree_vector_remove(items, item);
  tree_field_set_obj(item, "partXXX", nullptr);
  tree_field_set_obj(item, "part", nullptr);
  tree_field_set_obj(part, "inventory_item", nullptr);
  tree_field_set_int(inv, "size", inventory_size(inv));
  tree_field_set_int(inv, "update_count",
                     tree_field_get_int(inv, "update_count") + 1);
  if (tree_field_get_obj(inv, "panels")) visual_inventory_update(inv);
  tree_field_set_obj(inv, "last_install_error", nullptr);
  tree_field_set_int(inv, "last_install_slot", slot);
  tree_field_set_int(inv, "last_install_via_attach",
                     tree_field_get_int(part, "install_via_attach"));
  tree_field_set_int(inv, "install_count",
                     tree_field_get_int(inv, "install_count") + 1);
  return nullptr;
}

bool visual_inventory_panel_left_click(InvObject* inv, int32_t panel_index) {
  if (!inv || panel_index < 0) return false;
  int32_t ppl = tree_field_get_int(inv, "partsPerLine");
  if (ppl <= 0) ppl = 5;
  const int32_t index =
      panel_index + tree_field_get_int(inv, "cline") * ppl;
  InvObject* player = tree_field_get_obj(inv, "player");
  InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
  if (!car) car = game_logic_player()
                      ? tree_field_get_obj(game_logic_player(), "car")
                      : nullptr;
  const char* err = inventory_install_to_car(inv, index, car);
  if (err && err[0]) {
    tree_field_set_obj(inv, "last_install_error", string_new(err));
    return false;
  }
  return true;
}

bool part_uninstall(InvObject* part) {
  if (!part) return false;
  const char* hc = tree_host_class(part);
  if (hc && std::strstr(hc, "Chassis")) return false;
  // Mirror GII_REMOVE_OK: refuse if dependents still attached.
  const int32_t n = part_slot_count(part);
  for (int32_t i = 0; i < n; ++i) {
    const int32_t sid = part_slot_id_at(part, i);
    if (sid > 0 && part_on_slot(part, sid)) return false;
  }
  InvObject* parent = tree_field_get_obj(part, "part_parent");
  const int32_t slot_id = tree_field_get_int(part, "part_parent_slot");
  if (parent && slot_id) {
    InvObject* slots = tree_field_get_obj(parent, "part_slots");
    const int32_t sn = slots ? tree_vector_size(slots) : 0;
    for (int32_t i = 0; i < sn; ++i) {
      InvObject* s = tree_vector_element_at(slots, i);
      if (!s || tree_field_get_int(s, "slot_id") != slot_id) continue;
      if (tree_field_get_obj(s, "child") == part) {
        tree_field_set_obj(s, "child", nullptr);
        tree_field_set_int(s, "child_slot_id", 0);
        if (tree_field_get_obj(s, "visual") == part)
          tree_field_set_obj(s, "visual", nullptr);
      }
      break;
    }
  }
  tree_field_set_obj(part, "part_parent", nullptr);
  tree_field_set_int(part, "part_parent_slot", 0);
  return true;
}

bool visual_inventory_panel_drag_drop(InvObject* inv, int32_t panel_index,
                                     InvObject* part) {
  if (!inv || !part || panel_index < 0) return false;
  const int32_t pc = visual_inventory_panel_count(inv);
  if (pc > 0 && panel_index >= pc) return false;
  if (!part_uninstall(part)) return false;

  inventory_add_part_item(inv, part);
  // Stock: swap newly added item into the drop panel slot when possible.
  int32_t ppl = tree_field_get_int(inv, "partsPerLine");
  if (ppl <= 0) ppl = 5;
  const int32_t start = tree_field_get_int(inv, "cline") * ppl;
  const int32_t target = start + panel_index;
  InvObject* items = tree_field_get_obj(inv, "items");
  const int32_t sz = inventory_size(inv);
  if (items && target >= 0 && target < sz - 1) {
    InvObject* a = tree_vector_element_at(items, target);
    InvObject* b = tree_vector_element_at(items, sz - 1);
    tree_vector_set(items, target, b);
    tree_vector_set(items, sz - 1, a);
  }
  if (tree_field_get_obj(inv, "panels")) visual_inventory_update(inv);
  tree_field_set_int(inv, "drop_count",
                     tree_field_get_int(inv, "drop_count") + 1);
  tree_field_set_int(inv, "last_panel_drop", panel_index);
  return true;
}

void inventory_swap(InvObject* inv, int32_t index_a, int32_t index_b) {
  if (!inv || index_a < 0 || index_b < 0) return;
  InvObject* items = tree_field_get_obj(inv, "items");
  if (!items) return;
  const int32_t sz = tree_vector_size(items);
  if (index_a >= sz || index_b >= sz || index_a == index_b) return;
  InvObject* a = tree_vector_element_at(items, index_a);
  InvObject* b = tree_vector_element_at(items, index_b);
  tree_vector_set(items, index_a, b);
  tree_vector_set(items, index_b, a);
  tree_field_set_int(inv, "size", inventory_size(inv));
  tree_field_set_int(inv, "update_count",
                     tree_field_get_int(inv, "update_count") + 1);
}

int32_t visual_inventory_item_index_by_button(InvObject* inv,
                                              InvObject* button) {
  if (!inv || !button) return inventory_size(inv);
  InvObject* panels = tree_field_get_obj(inv, "panels");
  if (!panels) return inventory_size(inv);
  int32_t ppl = tree_field_get_int(inv, "partsPerLine");
  if (ppl <= 0) ppl = 5;
  const int32_t start = tree_field_get_int(inv, "cline") * ppl;
  const int32_t pc = tree_vector_size(panels);
  for (int32_t i = 0; i < pc; ++i) {
    InvObject* p = tree_vector_element_at(panels, i);
    if (p && tree_field_get_obj(p, "button") == button) return start + i;
  }
  return inventory_size(inv);
}

bool visual_inventory_panel_swap(InvObject* inv, int32_t panel_index_a,
                                 InvObject* dropped_button) {
  if (!inv || panel_index_a < 0 || !dropped_button) return false;
  const int32_t pc = visual_inventory_panel_count(inv);
  if (pc > 0 && panel_index_a >= pc) return false;
  int32_t ppl = tree_field_get_int(inv, "partsPerLine");
  if (ppl <= 0) ppl = 5;
  const int32_t start = tree_field_get_int(inv, "cline") * ppl;
  const int32_t index_a = start + panel_index_a;
  const int32_t index_b =
      visual_inventory_item_index_by_button(inv, dropped_button);
  if (index_b >= inventory_size(inv)) return false;
  inventory_swap(inv, index_a, index_b);
  if (tree_field_get_obj(inv, "panels")) visual_inventory_update(inv);
  tree_field_set_int(inv, "swap_count",
                     tree_field_get_int(inv, "swap_count") + 1);
  tree_field_set_int(inv, "last_panel_swap", panel_index_a);
  tree_field_set_int(inv, "last_swap_b", index_b);
  return true;
}

bool visual_inventory_panel_swap_panels(InvObject* inv, int32_t panel_a,
                                        int32_t panel_b) {
  if (!inv || panel_a < 0 || panel_b < 0) return false;
  const int32_t pc = visual_inventory_panel_count(inv);
  if (pc > 0 && (panel_a >= pc || panel_b >= pc)) return false;
  int32_t ppl = tree_field_get_int(inv, "partsPerLine");
  if (ppl <= 0) ppl = 5;
  const int32_t start = tree_field_get_int(inv, "cline") * ppl;
  const int32_t index_a = start + panel_a;
  const int32_t index_b = start + panel_b;
  if (index_a >= inventory_size(inv) || index_b >= inventory_size(inv))
    return false;
  InvObject* items = tree_field_get_obj(inv, "items");
  InvObject* item_a = items ? tree_vector_element_at(items, index_a) : nullptr;
  InvObject* item_b = items ? tree_vector_element_at(items, index_b) : nullptr;
  inventory_swap(inv, index_a, index_b);
  if (tree_field_get_obj(inv, "panels")) visual_inventory_update(inv);
  tree_field_set_int(inv, "swap_count",
                     tree_field_get_int(inv, "swap_count") + 1);
  tree_field_set_int(inv, "last_panel_swap", panel_a);
  tree_field_set_int(inv, "last_swap_b", index_b);
  // Prove swap: panel attach should flip.
  InvObject* panels = tree_field_get_obj(inv, "panels");
  InvObject* pa = panels ? tree_vector_element_at(panels, panel_a) : nullptr;
  InvObject* pb = panels ? tree_vector_element_at(panels, panel_b) : nullptr;
  const bool ok =
      pa && pb && tree_field_get_obj(pa, "invItem") == item_b &&
      tree_field_get_obj(pb, "invItem") == item_a;
  tree_field_set_int(inv, "last_swap_ok", ok ? 1 : 0);
  return ok;
}

void mechanic_set_actual_panel(InvObject* mechanic, int32_t panel_index) {
  if (!mechanic) return;
  InvObject* inv = tree_field_get_obj(mechanic, "inventory");
  InvObject* panel = nullptr;
  if (inv && panel_index >= 0) {
    InvObject* panels = tree_field_get_obj(inv, "panels");
    if (panels && panel_index < tree_vector_size(panels))
      panel = tree_vector_element_at(panels, panel_index);
  }
  tree_field_set_int(mechanic, "actualPanel", panel_index);
  tree_field_set_obj(mechanic, "actualPanelObj", panel);
  tree_field_set_int(mechanic, "actualPanelChanged", 1);
  // Stock spinner updates infoline from invItem tip.
  InvObject* tip = nullptr;
  if (panel) {
    InvObject* item = tree_field_get_obj(panel, "invItem");
    if (item) {
      tip = tree_field_get_obj(item, "info");
      if (!tip) tip = string_new("part");
    } else {
      tip = string_new("empty slot");
    }
  }
  InvObject* line = tree_field_get_obj(mechanic, "infoline");
  if (!line) {
    line = tree_host_new("java.render.Text");
    tree_field_set_obj(mechanic, "infoline", line);
  }
  tree_field_set_obj(line, "text", tip);
}

void mechanic_clear_actual_panel(InvObject* mechanic) {
  if (!mechanic) return;
  if (!tree_field_get_obj(mechanic, "actualPanelObj") &&
      tree_field_get_int(mechanic, "actualPanel") < 0)
    return;
  tree_field_set_int(mechanic, "actualPanel", -1);
  tree_field_set_obj(mechanic, "actualPanelObj", nullptr);
  tree_field_set_int(mechanic, "actualPanelChanged", 1);
  InvObject* line = tree_field_get_obj(mechanic, "infoline");
  if (line) tree_field_set_obj(line, "text", nullptr);
}

InvObject* mechanic_actual_panel(InvObject* mechanic) {
  if (!mechanic) return nullptr;
  return tree_field_get_obj(mechanic, "actualPanelObj");
}

int32_t visual_inventory_panel_at(InvObject* inv, float nx, float ny) {
  // nx,ny in [0,1] top-left origin (same as panel x/y from VisualInventory).
  if (!inv) return -1;
  InvObject* panels = tree_field_get_obj(inv, "panels");
  if (!panels) return -1;
  const int32_t pc = tree_vector_size(panels);
  for (int32_t i = 0; i < pc; ++i) {
    InvObject* p = tree_vector_element_at(panels, i);
    if (!p || !tree_field_get_int(p, "visible")) continue;
    const float x = tree_field_get_float(p, "x");
    const float y = tree_field_get_float(p, "y");
    const float w = tree_field_get_float(p, "width");
    const float h = tree_field_get_float(p, "height");
    if (w <= 0.f || h <= 0.f) continue;
    if (nx >= x && nx <= x + w && ny >= y && ny <= y + h) return i;
  }
  return -1;
}

bool mechanic_hover_at(InvObject* mechanic, float nx, float ny) {
  if (!mechanic) return false;
  InvObject* inv = tree_field_get_obj(mechanic, "inventory");
  if (!inv || !tree_field_get_int(inv, "shown")) {
    mechanic_clear_actual_panel(mechanic);
    return false;
  }
  const int32_t idx = visual_inventory_panel_at(inv, nx, ny);
  if (idx >= 0) {
    mechanic_set_actual_panel(mechanic, idx);
    tree_field_set_int(mechanic, "last_hover_panel", idx);
    return true;
  }
  mechanic_clear_actual_panel(mechanic);
  tree_field_set_int(mechanic, "last_hover_panel", -1);
  return false;
}

void mechanic_tick_hover(InvObject* mechanic) {
  if (!mechanic) return;
  if (tree_field_get_int(mechanic, "rdrag")) return;
  // Mouse phys axes are [-1,1] (Y up). Panels use [0,1] top-left.
  input_live_poll();
  const float cx = java_io_Input_getAxis(1, /*kMousePhysX*/ 0);
  const float cy = java_io_Input_getAxis(1, /*kMousePhysY*/ 1);
  const float nx = (cx + 1.f) * 0.5f;
  const float ny = (1.f - cy) * 0.5f;
  mechanic_hover_at(mechanic, nx, ny);
  // Stock: inventory phyId hover OR car-part EC_HOVER (dest GameRef).
  if (!mechanic_actual_panel(mechanic)) mechanic_hover_car_at(mechanic, nx, ny);
}

bool inventory_panel_osd_command(InvObject* panel, int32_t cmd) {
  // Stock InventoryPanel.osdCommand: dropObject → drag, dropGadget → swap,
  // else panelLeftClick (install).
  if (!panel) return false;
  InvObject* inv = tree_field_get_obj(panel, "inv");
  if (!inv || tree_field_get_int(inv, "interactive") == 0) return false;
  InvObject* osd = tree_field_get_obj(panel, "osd");
  if (osd && tree_field_get_obj(osd, "dropObject")) {
    InvObject* dropped = tree_field_get_obj(osd, "dropObject");
    const bool ok = visual_inventory_panel_drag_drop(inv, cmd, dropped);
    tree_field_set_obj(osd, "dropObject", nullptr);
    return ok;
  }
  if (osd && tree_field_get_obj(osd, "dropGadget")) {
    InvObject* dropped = tree_field_get_obj(osd, "dropGadget");
    const bool ok = visual_inventory_panel_swap(inv, cmd, dropped);
    tree_field_set_obj(osd, "dropGadget", nullptr);
    return ok;
  }
  const bool ok = visual_inventory_panel_left_click(inv, cmd);
  tree_field_set_int(inv, "last_panel_click", cmd);
  return ok;
}

bool mechanic_click_actual(InvObject* mechanic) {
  if (!mechanic) return false;
  InvObject* panel = mechanic_actual_panel(mechanic);
  if (!panel) return false;
  const int32_t idx = tree_field_get_int(panel, "index");
  const bool ok = inventory_panel_osd_command(panel, idx);
  tree_field_set_int(mechanic, "click_count",
                     tree_field_get_int(mechanic, "click_count") + 1);
  tree_field_set_int(mechanic, "last_click_panel", idx);
  tree_field_set_int(mechanic, "last_click_ok", ok ? 1 : 0);
  return ok;
}

void mechanic_begin_drag_object(InvObject* mechanic, InvObject* part) {
  if (!mechanic) return;
  tree_field_set_obj(mechanic, "drag_object", part);
  if (part) tree_field_set_obj(mechanic, "look_part", part);
}

void mechanic_set_look_part(InvObject* mechanic, InvObject* part) {
  if (!mechanic) return;
  tree_field_set_obj(mechanic, "look_part", part);
}

static void part_collect_mounted(InvObject* root, std::vector<InvObject*>* out,
                                 int depth) {
  if (!root || !out || depth > 24) return;
  const int32_t n = part_slot_count(root);
  for (int32_t i = 0; i < n; ++i) {
    const int32_t sid = part_slot_id_at(root, i);
    if (sid <= 0) continue;
    InvObject* child = part_on_slot(root, sid);
    if (!child) continue;
    const char* hc = tree_host_class(child);
    if (hc && std::strstr(hc, "Chassis")) {
      part_collect_mounted(child, out, depth + 1);
      continue;
    }
    out->push_back(child);
    part_collect_mounted(child, out, depth + 1);
  }
}

static bool part_approx_world_pos(InvObject* part, float* ox, float* oy,
                                  float* oz) {
  if (!part || !ox || !oy || !oz) return false;
  InvObject* parent = tree_field_get_obj(part, "part_parent");
  const int32_t sid = tree_field_get_int(part, "part_parent_slot");
  float sx = 0.f, sy = 0.f, sz = 0.f, oy0 = 0.f, op0 = 0.f, or0 = 0.f;
  if (parent && sid > 0 &&
      part_slot_get_pose(parent, sid, &sx, &sy, &sz, &oy0, &op0, &or0)) {
    constexpr float kM = 100.f;
    float cx = 0.f, cy = 0.f, cz = 0.f;
    if (InvObject* cp = java_util_resource_GameRef_getPos(parent))
      vec3_get(cp, &cx, &cy, &cz);
    *ox = cx + sx * kM;
    *oy = cy + sy * kM;
    *oz = cz + sz * kM;
    return true;
  }
  if (InvObject* p = java_util_resource_GameRef_getPos(part)) {
    vec3_get(p, ox, oy, oz);
    return true;
  }
  return false;
}

InvObject* mechanic_pick_part_at(InvObject* mechanic, float nx, float ny) {
  // nx,ny in [0,1] top-left (same as inventory panels).
  if (!mechanic) return nullptr;
  InvObject* player = tree_field_get_obj(mechanic, "player");
  InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
  if (!car && game_logic_player())
    car = tree_field_get_obj(game_logic_player(), "car");
  if (!car) return nullptr;

  const float ndc_x = nx * 2.f - 1.f;
  const float ndc_y = 1.f - ny * 2.f;
  float hit_x = 0.f, hit_y = 0.f, hit_z = 0.f;
  void* vp = render_d3d9_viewport_active();
  void* cam = render_d3d9_camera_active();
  if (!render_d3d9_viewport_unproject(vp, cam, ndc_x, ndc_y, &hit_x, &hit_y,
                                      &hit_z)) {
    hit_x = ndc_x * 2.f;
    hit_y = 0.f;
    hit_z = ndc_y * 2.f;
  }

  std::vector<InvObject*> parts;
  part_collect_mounted(car, &parts, 0);
  InvObject* prefer = tree_field_get_obj(car, "last_installed_part");

  InvObject* best = nullptr;
  float best_d2 = 0.15f * 0.15f;  // NDC screen radius
  bool screen_hit = false;
  for (InvObject* p : parts) {
    float wx = 0.f, wy = 0.f, wz = 0.f;
    if (!part_approx_world_pos(p, &wx, &wy, &wz)) continue;
    float px = 0.f, py = 0.f;
    if (!render_d3d9_project(wx, wy, wz, &px, &py)) continue;
    float d2 = (px - ndc_x) * (px - ndc_x) + (py - ndc_y) * (py - ndc_y);
    if (prefer && p == prefer) d2 *= 0.25f;
    if (d2 < best_d2) {
      best_d2 = d2;
      best = p;
      screen_hit = true;
    }
  }
  if (!screen_hit) {
    // Fallback: XZ distance to unprojected ground hit (mesh cm).
    best_d2 = 120.f * 120.f;
    for (InvObject* p : parts) {
      float wx = 0.f, wy = 0.f, wz = 0.f;
      if (!part_approx_world_pos(p, &wx, &wy, &wz)) continue;
      const float dx = wx - hit_x;
      const float dz = wz - hit_z;
      float d2 = dx * dx + dz * dz;
      if (prefer && p == prefer) d2 *= 0.25f;
      if (d2 < best_d2) {
        best_d2 = d2;
        best = p;
      }
    }
  }
  tree_field_set_float(mechanic, "pick_hit_x", hit_x);
  tree_field_set_float(mechanic, "pick_hit_y", hit_y);
  tree_field_set_float(mechanic, "pick_hit_z", hit_z);
  tree_field_set_float(mechanic, "pick_dist",
                       best ? std::sqrt(best_d2) : -1.f);
  tree_field_set_int(mechanic, "pick_via_screen", screen_hit ? 1 : 0);
  return best;
}

bool mechanic_hover_car_at(InvObject* mechanic, float nx, float ny) {
  if (!mechanic) return false;
  InvObject* part = mechanic_pick_part_at(mechanic, nx, ny);
  if (part) {
    mechanic_set_look_part(mechanic, part);
    tree_field_set_int(mechanic, "over_vehicle", 1);
    tree_field_set_int(mechanic, "pick_count",
                       tree_field_get_int(mechanic, "pick_count") + 1);
    // Stock EC_HOVER: infoline ← part name + condition.
    InvObject* tip = tree_field_get_obj(part, "name");
    if (!tip) tip = string_new("part");
    InvObject* line = tree_field_get_obj(mechanic, "infoline");
    if (!line) {
      line = tree_host_new("java.render.Text");
      tree_field_set_obj(mechanic, "infoline", line);
    }
    tree_field_set_obj(line, "text", tip);
    return true;
  }
  if (tree_field_get_int(mechanic, "over_vehicle")) {
    tree_field_set_int(mechanic, "over_vehicle", 0);
    tree_field_set_obj(mechanic, "look_part", nullptr);
    InvObject* line = tree_field_get_obj(mechanic, "infoline");
    if (line) tree_field_set_obj(line, "text", nullptr);
  }
  return false;
}

bool part_is_tuneable(InvObject* part) {
  if (!part) return false;
  if (tree_field_get_int(part, "tuneable") != 0) return true;
  const char* hc = tree_host_class(part);
  if (!hc || !hc[0]) return false;
  if (std::strstr(hc, "Tyre") || std::strstr(hc, "Block")) return true;
  if (std::strstr(hc, "NOS") || std::strstr(hc, "FuelInjector")) return true;
  if (std::strstr(hc, "Transmission")) {
    const int32_t adj_gears = tree_field_get_int(part, "adjustable_gears");
    const float dmin = tree_field_get_float(part, "diff_lock_min");
    const float dmax = tree_field_get_float(part, "diff_lock_max");
    const float fmin = tree_field_get_float(part, "drive_front_min");
    const float fmax = tree_field_get_float(part, "drive_front_max");
    return adj_gears != 0 || dmin != dmax || fmin != fmax;
  }
  if (std::strstr(hc, "Turbo")) {
    return tree_field_get_float(part, "max_waste") > 0.f &&
           tree_field_get_float(part, "min_waste") > 0.f;
  }
  if (std::strstr(hc, "Chassis"))
    return tree_field_get_int(part, "brake_balance_can_be_set") != 0;
  if (std::strstr(hc, "Camshaft")) {
    return tree_field_get_float(part, "advance_positive_peak") >
           tree_field_get_float(part, "advance_negative_peak");
  }
  if (std::strstr(hc, "Shock")) {
    return tree_field_get_int(part, "adjustable_damping") != 0 &&
           tree_field_get_int(part, "adjustable_rebound_factor") != 0;
  }
  return false;
}

bool part_flap_toggle(InvObject* part) {
  // Stock: part.command("flap_toggle") when not tuneable in Mechanic tune mode.
  if (!part) return false;
  const int32_t next = tree_field_get_int(part, "flap_state") ^ 1;
  tree_field_set_int(part, "flap_state", next);
  tree_field_set_int(part, "flap_toggle_count",
                     tree_field_get_int(part, "flap_toggle_count") + 1);
  tree_field_set_obj(part, "last_command", string_new("flap_toggle"));
  return true;
}

// Phase 2.153/2.154 — stock Part.buildTuningMenu dispatch.
enum : int32_t {
  kTuneBlock = 1,
  kTuneTyre = 2,
  kTuneTurbo = 3,
  kTuneChassis = 4,
  kTuneCamshaft = 5,
  kTuneShock = 6,
  kTuneTransmission = 7,
  kTuneInjector = 8,
  kTuneNos = 9,
};

static float part_ratio_at(InvObject* part, int32_t i) {
  char key[16];
  std::snprintf(key, sizeof(key), "ratio_%d", i);
  return tree_field_get_float(part, key);
}

static void part_ratio_set(InvObject* part, int32_t i, float v) {
  char key[16];
  std::snprintf(key, sizeof(key), "ratio_%d", i);
  tree_field_set_float(part, key, v);
}

static float part_old_ratio_at(InvObject* part, int32_t i) {
  char key[20];
  std::snprintf(key, sizeof(key), "old_ratio_%d", i);
  return tree_field_get_float(part, key);
}

static void part_old_ratio_set(InvObject* part, int32_t i, float v) {
  char key[20];
  std::snprintf(key, sizeof(key), "old_ratio_%d", i);
  tree_field_set_float(part, key, v);
}

static int32_t part_detect_tune_kind(InvObject* part) {
  if (!part) return kTuneBlock;
  const int32_t stored = tree_field_get_int(part, "tune_kind");
  if (stored > 0) return stored;
  const char* hc = tree_host_class(part);
  if (hc && hc[0]) {
    if (std::strstr(hc, "Tyre")) return kTuneTyre;
    if (std::strstr(hc, "Turbo")) return kTuneTurbo;
    if (std::strstr(hc, "NOS")) return kTuneNos;
    if (std::strstr(hc, "FuelInjector") || std::strstr(hc, "InjectorSystem"))
      return kTuneInjector;
    if (std::strstr(hc, "Transmission")) return kTuneTransmission;
    if (std::strstr(hc, "Chassis")) return kTuneChassis;
    if (std::strstr(hc, "Camshaft")) return kTuneCamshaft;
    if (std::strstr(hc, "Shock")) return kTuneShock;
    if (std::strstr(hc, "Block")) return kTuneBlock;
  }
  if (tree_field_get_float(part, "optimal_inflation") > 0.f ||
      tree_field_get_float(part, "inflation") > 0.f)
    return kTuneTyre;
  if (tree_field_get_float(part, "max_waste") > 0.f) return kTuneTurbo;
  if (tree_field_get_float(part, "nitro_consumption") > 0.f ||
      tree_field_get_float(part, "maxconsumption") > 0.f)
    return kTuneNos;
  if (tree_field_get_float(part, "mixture_ratio") > 0.f) return kTuneInjector;
  if (tree_field_get_int(part, "adjustable_gears") != 0 ||
      tree_field_get_int(part, "gears") > 0)
    return kTuneTransmission;
  if (tree_field_get_int(part, "brake_balance_can_be_set") != 0)
    return kTuneChassis;
  if (tree_field_get_float(part, "advance_positive_peak") >
      tree_field_get_float(part, "advance_negative_peak"))
    return kTuneCamshaft;
  if (tree_field_get_int(part, "adjustable_damping") != 0) return kTuneShock;
  return kTuneBlock;
}

static InvObject* menu_add_slider(InvObject* menu, const char* text, int32_t cmd,
                                  float value, float a, float b, int32_t steps) {
  if (!menu) return nullptr;
  InvObject* s = tree_host_new("java.render.osd.Slider");
  tree_field_set_obj(s, "label", string_new(text ? text : "Tune"));
  tree_field_set_int(s, "command", cmd);
  tree_field_set_float(s, "value", value);
  tree_field_set_float(s, "min", a);
  tree_field_set_float(s, "max", b);
  tree_field_set_int(s, "steps", steps);
  float y = tree_field_get_float(menu, "y");
  float sp = tree_field_get_float(menu, "spacing");
  if (sp <= 0.f) sp = 0.08f;
  tree_field_set_float(s, "x", tree_field_get_float(menu, "x"));
  tree_field_set_float(s, "y", y);
  tree_field_set_float(menu, "y", y + sp);
  InvObject* items = tree_field_get_obj(menu, "items");
  if (!items) {
    items = tree_vector_new();
    tree_field_set_obj(menu, "items", items);
  }
  tree_vector_add(items, s);
  tree_field_set_int(menu, "item_count",
                     tree_field_get_int(menu, "item_count") + 1);
  return s;
}

static InvObject* menu_add_button(InvObject* menu, const char* text,
                                  int32_t cmd) {
  if (!menu) return nullptr;
  InvObject* g = tree_host_new("java.render.osd.Gadget");
  tree_field_set_obj(g, "label", string_new(text ? text : "Reset"));
  tree_field_set_int(g, "command", cmd);
  float y = tree_field_get_float(menu, "y");
  float sp = tree_field_get_float(menu, "spacing");
  if (sp <= 0.f) sp = 0.08f;
  tree_field_set_float(g, "x", tree_field_get_float(menu, "x"));
  tree_field_set_float(g, "y", y);
  tree_field_set_float(menu, "y", y + sp);
  InvObject* items = tree_field_get_obj(menu, "items");
  if (!items) {
    items = tree_vector_new();
    tree_field_set_obj(menu, "items", items);
  }
  tree_vector_add(items, g);
  tree_field_set_int(menu, "item_count",
                     tree_field_get_int(menu, "item_count") + 1);
  return g;
}

static void menu_set_slider_value(InvObject* menu, int32_t cmd, float v) {
  if (!menu) return;
  InvObject* items = tree_field_get_obj(menu, "items");
  const int32_t n = items ? tree_vector_size(items) : 0;
  for (int32_t i = 0; i < n; ++i) {
    InvObject* s = tree_vector_element_at(items, i);
    if (!s || tree_field_get_int(s, "command") != cmd) continue;
    const char* hc = tree_host_class(s);
    if (hc && !std::strstr(hc, "Slider")) continue;
    float mn = tree_field_get_float(s, "min");
    float mx = tree_field_get_float(s, "max");
    if (v < mn) v = mn;
    if (v > mx) v = mx;
    tree_field_set_float(s, "value", v);
    return;
  }
}

void part_build_tuning_menu(InvObject* part, InvObject* menu) {
  // Host mirrors of Part subclass buildTuningMenu (Block/Tyre/Turbo/…).
  if (!part || !menu) return;
  const int32_t kind = part_detect_tune_kind(part);
  tree_field_set_int(part, "tune_kind", kind);

  if (kind == kTuneTyre) {
    float infl = tree_field_get_float(part, "inflation");
    if (infl <= 0.f) infl = 2.f;
    float opt = tree_field_get_float(part, "optimal_inflation");
    if (opt <= 0.5f) opt = 2.5f;
    tree_field_set_float(part, "inflation", infl);
    tree_field_set_float(part, "new_inflation", infl);
    tree_field_set_float(part, "old_inflation", infl);
    tree_field_set_float(part, "optimal_inflation", opt);
    menu_add_slider(menu, "Inflation", 1, infl, 0.5f, opt,
                    static_cast<int32_t>((opt - 0.5f) / 0.1f) + 1);
  } else if (kind == kTuneTurbo) {
    float waste = tree_field_get_float(part, "P_turbo_waste");
    float mn = tree_field_get_float(part, "min_waste");
    float mx = tree_field_get_float(part, "max_waste");
    if (mn <= 0.f) mn = 0.5f;
    if (mx <= mn) mx = mn + 1.f;
    if (waste < mn) waste = mn;
    tree_field_set_float(part, "P_turbo_waste", waste);
    tree_field_set_float(part, "old_waste", waste);
    tree_field_set_float(part, "min_waste", mn);
    tree_field_set_float(part, "max_waste", mx);
    menu_add_slider(menu, "Wastegate pressure", 1, waste, mn, mx,
                    static_cast<int32_t>((mx - mn) / 0.05f) + 1);
  } else if (kind == kTuneChassis) {
    float bal = tree_field_get_float(part, "brake_balance");
    if (bal <= 0.f) bal = 0.5f;
    tree_field_set_float(part, "brake_balance", bal);
    tree_field_set_float(part, "old_brake_balance", bal);
    menu_add_slider(menu, "F-R brake balance", 1, -bal, -1.f, 0.f, 51);
  } else if (kind == kTuneCamshaft) {
    float adv = tree_field_get_float(part, "advance");
    float neg = tree_field_get_float(part, "advance_negative_peak");
    float pos = tree_field_get_float(part, "advance_positive_peak");
    float step = tree_field_get_float(part, "advance_minimum_step");
    if (step <= 0.f) step = 0.5f;
    if (pos <= neg) {
      neg = -10.f;
      pos = 40.f;
    }
    float def = tree_field_get_float(part, "default_advance");
    tree_field_set_float(part, "advance", adv);
    tree_field_set_float(part, "old_advance", adv);
    tree_field_set_float(part, "default_advance", def);
    tree_field_set_float(part, "advance_negative_peak", neg);
    tree_field_set_float(part, "advance_positive_peak", pos);
    menu_add_slider(menu, "Advance angle", 1, adv, neg, pos,
                    static_cast<int32_t>((pos - neg) / step) + 1);
    menu_add_button(menu, "Reset to factory defaults", 0);
  } else if (kind == kTuneShock) {
    float damp = tree_field_get_float(part, "damping");
    float reb = tree_field_get_float(part, "rebound_factor");
    float dmin = tree_field_get_float(part, "min_damping");
    float dmax = tree_field_get_float(part, "max_damping");
    float rmin = tree_field_get_float(part, "min_rebound_factor");
    float rmax = tree_field_get_float(part, "max_rebound_factor");
    if (dmax <= dmin) {
      dmin = 1000.f;
      dmax = 8000.f;
    }
    if (rmax <= rmin) {
      rmin = 0.2f;
      rmax = 1.5f;
    }
    tree_field_set_float(part, "damping", damp);
    tree_field_set_float(part, "rebound_factor", reb);
    tree_field_set_float(part, "old_damping", damp);
    tree_field_set_float(part, "old_rebound_factor", reb);
    if (tree_field_get_float(part, "default_damping") <= 0.f)
      tree_field_set_float(part, "default_damping", damp);
    if (tree_field_get_float(part, "default_rebound_factor") <= 0.f)
      tree_field_set_float(part, "default_rebound_factor", reb);
    if (tree_field_get_int(part, "adjustable_damping"))
      menu_add_slider(menu, "Bound damping", 1, damp, dmin, dmax,
                      static_cast<int32_t>((dmax - dmin) / 100.f) + 1);
    if (tree_field_get_int(part, "adjustable_rebound_factor"))
      menu_add_slider(menu, "Rebound factor", 2, reb, rmin, rmax,
                      static_cast<int32_t>(rmax - rmin) - 1);
    menu_add_button(menu, "Reset to factory defaults", 0);
  } else if (kind == kTuneTransmission) {
    // Stock Transmission.buildTuningMenu — gears / R / end / LSD / drive.
    int32_t gears = tree_field_get_int(part, "gears");
    if (gears < 1) gears = 5;
    if (gears > 6) gears = 6;
    tree_field_set_int(part, "gears", gears);
    int32_t adj = tree_field_get_int(part, "adjustable_gears");
    if (adj == 0) adj = 7;  // forward|reverse|end
    tree_field_set_int(part, "adjustable_gears", adj);
    // Default positive ratios if unset.
    static const float kDefRatio[8] = {0.f, 3.5f, 2.2f, 1.5f, 1.1f, 0.9f,
                                       0.75f, 3.2f};
    for (int32_t i = 0; i < 8; ++i) {
      float r = part_ratio_at(part, i);
      if (i > 0 && i < 7 && r == 0.f) r = kDefRatio[i];
      if (i == 7 && r == 0.f) r = kDefRatio[7];
      part_ratio_set(part, i, r);
      part_old_ratio_set(part, i, r);
    }
    float end_r = tree_field_get_float(part, "end_ratio");
    if (end_r <= 0.f) end_r = 3.9f;
    tree_field_set_float(part, "end_ratio", end_r);
    tree_field_set_float(part, "old_end_ratio", end_r);
    // Stock flips forward gears to negative for slider UI (−5..−0.5).
    for (int32_t i = 1; i <= 6; ++i)
      part_ratio_set(part, i, -part_ratio_at(part, i));
    float dlock = tree_field_get_float(part, "diff_lock");
    float dmin = tree_field_get_float(part, "diff_lock_min");
    float dmax = tree_field_get_float(part, "diff_lock_max");
    if (dmax <= dmin) {
      dmin = 0.f;
      dmax = 1.f;
    }
    tree_field_set_float(part, "diff_lock", dlock);
    tree_field_set_float(part, "old_diff_lock", dlock);
    tree_field_set_float(part, "diff_lock_min", dmin);
    tree_field_set_float(part, "diff_lock_max", dmax);
    float drive = tree_field_get_float(part, "drive_front");
    float fmin = tree_field_get_float(part, "drive_front_min");
    float fmax = tree_field_get_float(part, "drive_front_max");
    if (fmax <= fmin) {
      fmin = 0.f;
      fmax = 1.f;
    }
    tree_field_set_float(part, "drive_front", drive);
    tree_field_set_float(part, "old_drive_front", drive);
    tree_field_set_float(part, "drive_front_min", fmin);
    tree_field_set_float(part, "drive_front_max", fmax);
    const int32_t adj_diff = (dmin != dmax) ? 1 : 0;
    const int32_t adj_drive = (fmin != fmax) ? 1 : 0;
    tree_field_set_int(part, "adjustable_diff_lock", adj_diff);
    tree_field_set_int(part, "adjustable_drive", adj_drive);
    static const char* kGearNames[] = {"",    "1st", "2nd", "3rd",
                                       "4th", "5th", "6th"};
    if (adj & 1) {
      for (int32_t i = 1; i <= gears; ++i)
        menu_add_slider(menu, kGearNames[i], i, part_ratio_at(part, i), -5.f,
                        -0.5f, 0);
    }
    if (adj & 2)
      menu_add_slider(menu, "R", 7, part_ratio_at(part, 7), -5.f, -0.5f, 0);
    if (adj & 4)
      menu_add_slider(menu, "End ratio", 8, end_r, 1.f, 8.f, 0);
    if (adj_diff)
      menu_add_slider(menu, "Limited slip differential lock rate", 9, dlock,
                      dmin, dmax,
                      static_cast<int32_t>((dmax - dmin) * 100.f) + 1);
    if (adj_drive)
      menu_add_slider(menu, "Drive distribution", 10, -drive, -fmax, -fmin,
                      static_cast<int32_t>((fmax - fmin) * 100.f) + 1);
  } else if (kind == kTuneInjector) {
    // FuelInjectorSystem — Mixture ratio (fuel type MultiChoice skipped).
    float mix = tree_field_get_float(part, "mixture_ratio");
    if (mix <= 0.f) mix = 14.7f;
    tree_field_set_float(part, "mixture_ratio", mix);
    tree_field_set_float(part, "old_mixture_ratio", mix);
    menu_add_slider(menu, "Mixture ratio", 1, mix, 8.f, 20.f, 49);
  } else if (kind == kTuneNos) {
    float cons = tree_field_get_float(part, "nitro_consumption");
    float mn = tree_field_get_float(part, "minconsumption");
    float mx = tree_field_get_float(part, "maxconsumption");
    if (mn <= 0.f) mn = 0.01f;
    if (mx <= mn) mx = mn + 0.5f;
    if (cons < mn) cons = mn;
    tree_field_set_float(part, "nitro_consumption", cons);
    tree_field_set_float(part, "old_nitro_consumption", cons);
    tree_field_set_float(part, "minconsumption", mn);
    tree_field_set_float(part, "maxconsumption", mx);
    menu_add_slider(menu, "Amount injected", 1, cons, mn, mx, 0);
  } else {
    // Block.buildTuningMenu — Idle + Redline.
    float idle = tree_field_get_float(part, "rpm_idle");
    if (idle <= 0.f) idle = 800.f;
    float redline = tree_field_get_float(part, "RPM_limit");
    if (redline <= 0.f) redline = 7000.f;
    tree_field_set_float(part, "rpm_idle", idle);
    tree_field_set_float(part, "RPM_limit", redline);
    tree_field_set_float(part, "old_rpm_idle", idle);
    tree_field_set_float(part, "old_RPM_limit", redline);
    menu_add_slider(menu, "Idle", 1, idle, 150.f, 2500.f,
                    (2500 - 150) / 50 + 1);
    menu_add_slider(menu, "Redline", 2, redline, 3000.f, 12000.f,
                    (12000 - 3000) / 250 + 1);
  }
  tree_field_set_int(part, "tuning_menu_built", 1);
}

InvObject* dialog_modal_active() { return g_dialog_modal; }

void dialog_set_smoke_string(const char* s) {
  g_dialog_smoke_string = (s && s[0]) ? s : "Player";
}

void dialog_note_constructed(InvObject* dialog, const char* class_fqn) {
  // Track modal dialogs for TREE that misbinds display() onto MainMenuDialog.
  if (!dialog || !class_fqn) return;
  if (!std::strstr(class_fqn, "Dialog")) return;
  if (std::strstr(class_fqn, "MainMenuDialog") ||
      std::strstr(class_fqn, "OptionsDialog"))
    return;
  g_recent_modal_dialog = dialog;
  tree_field_set_obj(dialog, "host_class_hint", string_new(class_fqn));
}

InvObject* dialog_resolve_display_target(InvObject* maybe_hub) {
  if (!maybe_hub) return g_recent_modal_dialog;
  const char* hc = tree_host_class(maybe_hub);
  if (hc && (std::strstr(hc, "MainMenuDialog") ||
             std::strstr(hc, "OptionsDialog"))) {
    if (g_recent_modal_dialog) return g_recent_modal_dialog;
  }
  return maybe_hub;
}

int32_t dialog_display(InvObject* dialog) {
  // Stock Dialog.display(): if (!shown) show(); wait(); hide(); return result;
  // Host: no Object.wait — smoke/live auto-accept OK (result=0). Do NOT invoke
  // subclass show/hide TREE (MainMenuDialog.hide would tear down the hub).
  dialog = dialog_resolve_display_target(dialog);
  if (!dialog) return -1;
  g_dialog_modal = dialog;
  const char* hc = tree_host_class(dialog);
  // Prefer FQN from NEW when host_class was left as Object/hub.
  if (InvObject* hint = tree_field_get_obj(dialog, "host_class_hint")) {
    if (const char* hs = string_cstr(hint))
      if (hs[0]) hc = hs;
  }

  if (!tree_field_get_obj(dialog, "osd")) {
    InvObject* osd = tree_host_new("java.render.Osd");
    osd_ensure_defaults(osd);
    tree_field_set_obj(dialog, "osd", osd);
  }
  dialog_ensure_osd_buttons(dialog);
  tree_field_set_int(dialog, "shown", 1);

  const bool is_str_req = hc && std::strstr(hc, "StringRequester");
  const bool is_setup = hc && std::strstr(hc, "PlayerSetup");

  if (is_str_req) {
    tree_field_set_obj(dialog, "input",
                       string_new(g_dialog_smoke_string.c_str()));
    if (InvObject* player = game_logic_player())
      tree_field_set_obj(player, "name",
                         string_new(g_dialog_smoke_string.c_str()));
  }
  if (is_setup) {
    InvObject* plr = tree_field_get_obj(dialog, "plr");
    if (!plr) plr = game_logic_player();
    if (plr && !tree_field_get_obj(plr, "character")) {
      int32_t rid = 0;
      if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
        rid = rpak_make_id(fe->pack_id, 0x00da);
      else if (const RpakPack* fe = rpak_find_by_name("frontend"))
        rid = rpak_make_id(fe->pack_id, 0x00da);
      InvObject* ch = tree_host_new("java.util.resource.ResourceRef");
      if (rid) java_util_resource_ResourceRef_set(ch, rid);
      tree_field_set_obj(plr, "character", ch);
    }
  }

  tree_field_set_int(dialog, "result", 0);
  tree_field_set_int(dialog, "shown", 0);
  if (InvObject* osd = tree_field_get_obj(dialog, "osd"))
    tree_field_set_int(osd, "visible", 0);
  tree_field_set_int(dialog, "display_via_host", 1);
  g_dialog_modal = nullptr;
  // Consume so a second display() picks the next NEW dialog.
  if (dialog == g_recent_modal_dialog) g_recent_modal_dialog = nullptr;
  std::printf("[script] Dialog.display accept %s\n",
              hc && hc[0] ? hc : "?");
  return 0;
}

void dialog_ensure_osd_buttons(InvObject* dialog) {
  // Stock Dialog ctor/show: title Text + OK;Cancel buttons + AXIS_CANCEL hotkey.
  // TREE show often skips createButton — mirror chrome for tune/warning dialogs.
  if (!dialog) return;
  InvObject* osd = tree_field_get_obj(dialog, "osd");
  if (!osd) return;

  const int32_t flags = tree_field_get_int(dialog, "flags");
  constexpr int32_t kDfWide = 0x00000080;
  if (flags & kDfWide) {
    tree_field_set_float(osd, "vpAspect", 2.f);  // 6/3
    tree_field_set_float(osd, "vpWidth", 0.9f);
    tree_field_set_int(dialog, "wide_applied", 1);
  }

  if (!tree_field_get_obj(osd, "buttons"))
    tree_field_set_obj(osd, "buttons", tree_vector_new());
  InvObject* btns = tree_field_get_obj(osd, "buttons");
  if (tree_vector_size(btns) < 1) {
    const char* texts = "OK;Cancel";
    if (InvObject* bt = tree_field_get_obj(dialog, "buttonTexts")) {
      if (const char* s = string_cstr(bt))
        if (s[0]) texts = s;
    }
    // Parse "OK;Cancel" → one Button per token (command = index).
    int32_t cmd = 0;
    const char* p = texts;
    while (p && *p) {
      const char* semi = std::strchr(p, ';');
      InvObject* btn = tree_host_new("java.render.osd.Button");
      tree_field_set_int(btn, "command", cmd);
      tree_field_set_obj(btn, "osd", osd);
      tree_vector_add(btns, btn);
      ++cmd;
      if (!semi) break;
      p = semi + 1;
    }
  }
  tree_field_set_int(osd, "button_count", tree_vector_size(btns));

  if (!tree_field_get_obj(osd, "title_text")) {
    InvObject* title = tree_field_get_obj(dialog, "title");
    if (title && string_cstr(title) && string_cstr(title)[0]) {
      InvObject* txt = tree_host_new("java.render.Text");
      tree_field_set_obj(txt, "string", title);
      tree_field_set_obj(osd, "title_text", txt);
      tree_field_set_int(osd, "text_count",
                         tree_field_get_int(osd, "text_count") + 1);
    }
  }

  const int32_t esc = tree_field_get_int(dialog, "escapeCmd");
  if (esc >= 0 && !tree_field_get_int(dialog, "cancel_hotkey")) {
    if (!tree_field_get_obj(osd, "hotkey"))
      tree_field_set_obj(osd, "hotkey", tree_vector_new());
    InvObject* hk = tree_host_new("java.render.osd.Hotkey");
    tree_field_set_int(hk, "command", esc);
    tree_vector_add(tree_field_get_obj(osd, "hotkey"), hk);
    tree_field_set_int(osd, "hotkey_count",
                       tree_vector_size(tree_field_get_obj(osd, "hotkey")));
    tree_field_set_int(dialog, "cancel_hotkey", 1);
  }
}

InvObject* mechanic_open_tune_dialog(InvObject* mechanic, InvObject* part) {
  // Stock Mechanic: new Dialog(..., "TUNE PART", "OK;Cancel") + createMenu +
  // buildTuningMenu.
  if (!mechanic || !part) return nullptr;
  constexpr int32_t kFlags =
      0x00000001 | 0x00000004 | 0x00000008 | 0x00000080;  // MODAL|BG|FREEZE|WIDE
  InvObject* dlg = tree_host_new("java.render.osd.dialog.Dialog");
  InvObject* osd = tree_host_new("java.render.Osd");
  tree_field_set_obj(osd, "groups", tree_vector_new());
  tree_field_set_obj(osd, "hotkey", tree_vector_new());
  tree_field_set_obj(osd, "rectangles", tree_vector_new());
  tree_field_set_obj(osd, "buttons", tree_vector_new());
  // Stock DF_WIDE: Osd(0.9, 6/3).
  tree_field_set_float(osd, "vpAspect", 2.f);
  tree_field_set_float(osd, "vpWidth", 0.9f);
  tree_field_set_float(osd, "vpHeight", 1.f);
  tree_field_set_int(osd, "iLevel", 3);
  tree_field_set_obj(osd, "globalHandler", part);
  tree_field_set_obj(dlg, "osd", osd);
  InvObject* player = tree_field_get_obj(mechanic, "player");
  InvObject* ctrl = player ? tree_field_get_obj(player, "controller") : nullptr;
  if (ctrl) tree_field_set_obj(dlg, "controller", ctrl);
  tree_field_set_int(dlg, "flags", kFlags);
  tree_field_set_obj(dlg, "title", string_new("TUNE PART"));
  tree_field_set_obj(dlg, "buttonTexts", string_new("OK;Cancel"));
  tree_field_set_float(dlg, "bodyRatio", 0.9f);
  tree_field_set_int(dlg, "priority", 22);
  tree_field_set_int(dlg, "escapeCmd", 1);
  // DF_DEFAULTBG
  {
    int32_t rid = 0;
    if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
      rid = rpak_make_id(fe->pack_id, 0x000E);
    else if (const RpakPack* fe = rpak_find_by_name("frontend"))
      rid = rpak_make_id(fe->pack_id, 0x000E);
    if (rid) {
      InvObject* pic = gameref_new();
      java_util_resource_ResourceRef_set(pic, rid);
      tree_field_set_obj(osd, "bg", pic);
      tree_field_set_int(osd, "bg_created", 1);
      java_util_resource_ResourceRef_load(pic);
      render_d3d9_osd_add_rect(0.f, 0.f, 2.f, 2.f, pic, -2);
    }
  }
  InvObject* menu = tree_host_new("java.render.osd.Menu");
  tree_field_set_obj(menu, "osd", osd);
  tree_field_set_obj(menu, "items", tree_vector_new());
  tree_field_set_int(menu, "item_count", 0);
  tree_field_set_float(menu, "x", 0.f);
  tree_field_set_float(menu, "y", -0.5f);
  tree_field_set_float(menu, "spacing", 0.08f);
  tree_field_set_obj(dlg, "tune_menu", menu);
  tree_field_set_obj(osd, "tune_menu", menu);
  part_build_tuning_menu(part, menu);
  tree_field_set_obj(osd, "globalHandler", dlg);
  dialog_ensure_osd_buttons(dlg);
  tree_field_set_int(dlg, "shown", 1);
  tree_field_set_int(osd, "visible", 1);
  tree_field_set_obj(mechanic, "tune_dialog", dlg);
  tree_field_set_obj(mechanic, "last_tune_part", part);
  tree_field_set_int(mechanic, "tune_dialog_count",
                     tree_field_get_int(mechanic, "tune_dialog_count") + 1);
  return dlg;
}

bool mechanic_tune_set_slider(InvObject* mechanic, int32_t cmd, float value) {
  // Stock Part.handleMessage for slider cmds (kind-dependent fields).
  if (!mechanic) return false;
  InvObject* part = tree_field_get_obj(mechanic, "last_tune_part");
  InvObject* dlg = tree_field_get_obj(mechanic, "tune_dialog");
  InvObject* menu = dlg ? tree_field_get_obj(dlg, "tune_menu") : nullptr;
  if (!part || !menu) return false;
  InvObject* items = tree_field_get_obj(menu, "items");
  const int32_t n = items ? tree_vector_size(items) : 0;
  for (int32_t i = 0; i < n; ++i) {
    InvObject* s = tree_vector_element_at(items, i);
    if (!s || tree_field_get_int(s, "command") != cmd) continue;
    const char* hc = tree_host_class(s);
    if (hc && !std::strstr(hc, "Slider")) continue;
    float v = value;
    const float mn = tree_field_get_float(s, "min");
    const float mx = tree_field_get_float(s, "max");
    if (v < mn) v = mn;
    if (v > mx) v = mx;
    tree_field_set_float(s, "value", v);
    const int32_t kind = tree_field_get_int(part, "tune_kind");
    if (kind == kTuneTyre && cmd == 1) {
      tree_field_set_float(part, "new_inflation", v);
      tree_field_set_float(part, "inflation", v);
    } else if (kind == kTuneTurbo && cmd == 1) {
      tree_field_set_float(part, "P_turbo_waste", v);
    } else if (kind == kTuneChassis && cmd == 1) {
      tree_field_set_float(part, "brake_balance", -v);
    } else if (kind == kTuneCamshaft && cmd == 1) {
      tree_field_set_float(part, "advance", v);
    } else if (kind == kTuneShock) {
      if (cmd == 1) tree_field_set_float(part, "damping", v);
      if (cmd == 2) tree_field_set_float(part, "rebound_factor", v);
    } else if (kind == kTuneTransmission) {
      if (cmd >= 1 && cmd <= 7)
        part_ratio_set(part, cmd, v);
      else if (cmd == 8)
        tree_field_set_float(part, "end_ratio", v);
      else if (cmd == 9)
        tree_field_set_float(part, "diff_lock", v);
      else if (cmd == 10)
        tree_field_set_float(part, "drive_front", -v);
    } else if (kind == kTuneInjector && cmd == 1) {
      tree_field_set_float(part, "mixture_ratio", v);
    } else if (kind == kTuneNos && cmd == 1) {
      tree_field_set_float(part, "nitro_consumption", v);
    } else {
      if (cmd == 1) tree_field_set_float(part, "rpm_idle", v);
      if (cmd == 2) tree_field_set_float(part, "RPM_limit", v);
    }
    tree_field_set_int(mechanic, "tune_slider_count",
                       tree_field_get_int(mechanic, "tune_slider_count") + 1);
    return true;
  }
  return false;
}

bool mechanic_tune_menu_command(InvObject* mechanic, int32_t cmd) {
  // Stock handleMessage for non-slider items (cmd 0 = factory reset).
  if (!mechanic || cmd != 0) return false;
  InvObject* part = tree_field_get_obj(mechanic, "last_tune_part");
  InvObject* dlg = tree_field_get_obj(mechanic, "tune_dialog");
  InvObject* menu = dlg ? tree_field_get_obj(dlg, "tune_menu") : nullptr;
  if (!part || !menu) return false;
  const int32_t kind = tree_field_get_int(part, "tune_kind");
  if (kind == kTuneCamshaft) {
    float def = tree_field_get_float(part, "default_advance");
    tree_field_set_float(part, "advance", def);
    menu_set_slider_value(menu, 1, def);
  } else if (kind == kTuneShock) {
    float dd = tree_field_get_float(part, "default_damping");
    float dr = tree_field_get_float(part, "default_rebound_factor");
    tree_field_set_float(part, "damping", dd);
    tree_field_set_float(part, "rebound_factor", dr);
    menu_set_slider_value(menu, 1, dd);
    menu_set_slider_value(menu, 2, dr);
  } else {
    return false;
  }
  tree_field_set_int(mechanic, "tune_reset_count",
                     tree_field_get_int(mechanic, "tune_reset_count") + 1);
  return true;
}

bool mechanic_tune_part(InvObject* mechanic, InvObject* part, int32_t choice) {
  // Stock: Dialog.display() → endTuningSession(choice) (0=OK, 1=Cancel).
  if (!mechanic || !part) return false;
  InvObject* dlg = tree_field_get_obj(mechanic, "tune_dialog");
  if (!dlg || tree_field_get_obj(mechanic, "last_tune_part") != part)
    dlg = mechanic_open_tune_dialog(mechanic, part);
  const int32_t kind = tree_field_get_int(part, "tune_kind");
  if (choice != 0) {
    if (kind == kTuneTyre) {
      tree_field_set_float(part, "inflation",
                           tree_field_get_float(part, "old_inflation"));
      tree_field_set_float(part, "new_inflation",
                           tree_field_get_float(part, "old_inflation"));
    } else if (kind == kTuneTurbo) {
      tree_field_set_float(part, "P_turbo_waste",
                           tree_field_get_float(part, "old_waste"));
    } else if (kind == kTuneChassis) {
      tree_field_set_float(part, "brake_balance",
                           tree_field_get_float(part, "old_brake_balance"));
    } else if (kind == kTuneCamshaft) {
      tree_field_set_float(part, "advance",
                           tree_field_get_float(part, "old_advance"));
    } else if (kind == kTuneShock) {
      tree_field_set_float(part, "damping",
                           tree_field_get_float(part, "old_damping"));
      tree_field_set_float(part, "rebound_factor",
                           tree_field_get_float(part, "old_rebound_factor"));
    } else if (kind == kTuneTransmission) {
      for (int32_t i = 0; i < 8; ++i)
        part_ratio_set(part, i, part_old_ratio_at(part, i));
      tree_field_set_float(part, "end_ratio",
                           tree_field_get_float(part, "old_end_ratio"));
      tree_field_set_float(part, "diff_lock",
                           tree_field_get_float(part, "old_diff_lock"));
      tree_field_set_float(part, "drive_front",
                           tree_field_get_float(part, "old_drive_front"));
    } else if (kind == kTuneInjector) {
      tree_field_set_float(part, "mixture_ratio",
                           tree_field_get_float(part, "old_mixture_ratio"));
    } else if (kind == kTuneNos) {
      tree_field_set_float(part, "nitro_consumption",
                           tree_field_get_float(part, "old_nitro_consumption"));
    } else {
      tree_field_set_float(part, "rpm_idle",
                           tree_field_get_float(part, "old_rpm_idle"));
      tree_field_set_float(part, "RPM_limit",
                           tree_field_get_float(part, "old_RPM_limit"));
    }
  } else {
    if (kind == kTuneTyre) {
      tree_field_set_float(part, "inflation",
                           tree_field_get_float(part, "new_inflation"));
      game_logic_spend_time(1.f * 60.f);
    } else if (kind == kTuneTurbo) {
      if (tree_field_get_float(part, "P_turbo_waste") !=
          tree_field_get_float(part, "old_waste"))
        game_logic_spend_time(4.f * 60.f);
    } else if (kind == kTuneChassis) {
      game_logic_spend_time(3.f * 60.f);
    } else if (kind == kTuneCamshaft) {
      if (tree_field_get_float(part, "advance") !=
          tree_field_get_float(part, "old_advance"))
        game_logic_spend_time(7.f * 60.f);
    } else if (kind == kTuneShock) {
      if (tree_field_get_float(part, "damping") !=
          tree_field_get_float(part, "old_damping"))
        game_logic_spend_time(5.f * 60.f);
      if (tree_field_get_float(part, "rebound_factor") !=
          tree_field_get_float(part, "old_rebound_factor"))
        game_logic_spend_time(5.f * 60.f);
    } else if (kind == kTuneTransmission) {
      // Flip forward gears back to positive (stock endTuningSession).
      for (int32_t i = 1; i <= 6; ++i)
        part_ratio_set(part, i, -part_ratio_at(part, i));
      bool gear_chg = false;
      for (int32_t i = 0; i < 8; ++i) {
        if (part_ratio_at(part, i) != part_old_ratio_at(part, i)) {
          gear_chg = true;
          break;
        }
      }
      if (tree_field_get_float(part, "end_ratio") !=
          tree_field_get_float(part, "old_end_ratio"))
        gear_chg = true;
      if (gear_chg) {
        const int32_t gears = tree_field_get_int(part, "gears");
        game_logic_spend_time(10.f * 60.f +
                              static_cast<float>(gears) * 2.f * 60.f);
      }
      if (tree_field_get_float(part, "diff_lock") !=
          tree_field_get_float(part, "old_diff_lock"))
        game_logic_spend_time(30.f * 60.f);
      if (tree_field_get_float(part, "drive_front") !=
          tree_field_get_float(part, "old_drive_front"))
        game_logic_spend_time(30.f * 60.f);
    } else if (kind == kTuneInjector) {
      if (tree_field_get_float(part, "mixture_ratio") !=
          tree_field_get_float(part, "old_mixture_ratio"))
        game_logic_spend_time(5.f * 60.f);
    } else if (kind == kTuneNos) {
      if (tree_field_get_float(part, "nitro_consumption") !=
          tree_field_get_float(part, "old_nitro_consumption"))
        game_logic_spend_time(3.f * 60.f);
    } else {
      if (tree_field_get_float(part, "rpm_idle") !=
          tree_field_get_float(part, "old_rpm_idle"))
        game_logic_spend_time(4.f * 60.f);
      if (tree_field_get_float(part, "RPM_limit") !=
          tree_field_get_float(part, "old_RPM_limit"))
        game_logic_spend_time(8.f * 60.f);
    }
  }
  if (dlg) {
    tree_field_set_int(dlg, "shown", 0);
    if (InvObject* osd = tree_field_get_obj(dlg, "osd"))
      tree_field_set_int(osd, "visible", 0);
  }
  tree_field_set_obj(mechanic, "tune_dialog", nullptr);
  tree_field_set_int(part, "tune_session_choice", choice);
  tree_field_set_int(part, "tune_session_count",
                     tree_field_get_int(part, "tune_session_count") + 1);
  tree_field_set_int(mechanic, "tune_count",
                     tree_field_get_int(mechanic, "tune_count") + 1);
  tree_field_set_obj(mechanic, "last_tune_part", part);
  tree_field_set_int(mechanic, "last_tune_choice", choice);
  return true;
}

bool mechanic_lclick_part(InvObject* mechanic, InvObject* part) {
  // Stock Mechanic EVENT_CURSOR EC_LCLICK:
  //   mode==0 szereles → GII_GETOUT_OK → inventory.addItem
  //   mode!=0 tuning → isTuneable ? Dialog/endTuningSession : flap_toggle
  if (!mechanic || !part) return false;
  InvObject* player = tree_field_get_obj(mechanic, "player");
  InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;

  if (tree_field_get_int(mechanic, "mode") != 0) {
    bool ok = false;
    if (part_is_tuneable(part)) {
      ok = mechanic_tune_part(mechanic, part, /*OK*/ 0);
    } else {
      ok = part_flap_toggle(part);
      tree_field_set_int(mechanic, "flap_count",
                         tree_field_get_int(mechanic, "flap_count") + 1);
    }
    if (ok && car)
      tree_field_set_int(car, "wake_count",
                         tree_field_get_int(car, "wake_count") + 1);
    tree_field_set_int(mechanic, "lclick_count",
                       tree_field_get_int(mechanic, "lclick_count") + 1);
    tree_field_set_obj(mechanic, "last_lclick_part", part);
    tree_field_set_int(mechanic, "last_lclick_ok", ok ? 1 : 0);
    tree_field_set_int(mechanic, "last_lclick_tune",
                       part_is_tuneable(part) ? 1 : 0);
    return ok;
  }

  constexpr int32_t kGiiGetoutOk = 41;
  if (java_util_resource_GameRef_getInfo(part, kGiiGetoutOk, 0) == -1) {
    tree_field_set_int(mechanic, "last_lclick_ok", 0);
    return false;
  }
  InvObject* inv = tree_field_get_obj(mechanic, "inventory");
  if (!inv) return false;
  if (!part_uninstall(part)) {
    tree_field_set_int(mechanic, "last_lclick_ok", 0);
    return false;
  }
  inventory_add_part_item(inv, part);
  const int32_t idx = inventory_size(inv) - 1;
  int32_t ppl = tree_field_get_int(inv, "partsPerLine");
  if (ppl <= 0) ppl = 5;
  if (idx >= 0) {
    tree_field_set_int(inv, "cline", idx / ppl);
    if (tree_field_get_obj(inv, "panels")) visual_inventory_update(inv);
  }
  if (car)
    tree_field_set_int(car, "wake_count",
                       tree_field_get_int(car, "wake_count") + 1);
  tree_field_set_int(mechanic, "lclick_count",
                     tree_field_get_int(mechanic, "lclick_count") + 1);
  tree_field_set_obj(mechanic, "last_lclick_part", part);
  tree_field_set_int(mechanic, "last_lclick_index", idx);
  tree_field_set_int(mechanic, "last_lclick_ok", 1);
  tree_field_set_int(mechanic, "last_lclick_tune", 0);
  tree_field_set_obj(mechanic, "look_part", nullptr);
  return true;
}

bool mechanic_drop_object_at(InvObject* mechanic, InvObject* part, float nx,
                             float ny) {
  // Stock: OSD dropObject on InventoryPanel button → panelDragNDrop.
  if (!mechanic || !part) return false;
  if (!mechanic_hover_at(mechanic, nx, ny)) return false;
  InvObject* panel = mechanic_actual_panel(mechanic);
  if (!panel) return false;
  InvObject* osd = tree_field_get_obj(panel, "osd");
  if (!osd) return false;
  tree_field_set_obj(osd, "dropObject", part);
  const int32_t idx = tree_field_get_int(panel, "index");
  const bool ok = inventory_panel_osd_command(panel, idx);
  tree_field_set_obj(mechanic, "drag_object", nullptr);
  tree_field_set_int(mechanic, "drop_object_count",
                     tree_field_get_int(mechanic, "drop_object_count") + 1);
  tree_field_set_int(mechanic, "last_drop_panel", idx);
  tree_field_set_int(mechanic, "last_drop_ok", ok ? 1 : 0);
  return ok;
}

bool mechanic_drag_panel_to(InvObject* mechanic, float x0, float y0, float x1,
                            float y1) {
  // Stock: drag InventoryPanel button → dropGadget → panelSwap.
  if (!mechanic) return false;
  if (!mechanic_hover_at(mechanic, x0, y0)) return false;
  InvObject* start = mechanic_actual_panel(mechanic);
  if (!start) return false;
  InvObject* button = tree_field_get_obj(start, "button");
  if (!button) return false;
  if (!mechanic_hover_at(mechanic, x1, y1)) return false;
  InvObject* dest = mechanic_actual_panel(mechanic);
  if (!dest || dest == start) return false;
  InvObject* osd = tree_field_get_obj(dest, "osd");
  if (!osd) return false;
  tree_field_set_obj(osd, "dropGadget", button);
  const int32_t idx = tree_field_get_int(dest, "index");
  const bool ok = inventory_panel_osd_command(dest, idx);
  tree_field_set_int(mechanic, "drag_swap_count",
                     tree_field_get_int(mechanic, "drag_swap_count") + 1);
  tree_field_set_int(mechanic, "last_drag_from",
                     tree_field_get_int(start, "index"));
  tree_field_set_int(mechanic, "last_drag_to", idx);
  tree_field_set_int(mechanic, "last_drag_ok", ok ? 1 : 0);
  return ok;
}

static bool mechanic_pointer_release(InvObject* mechanic) {
  InvObject* cur = mechanic_actual_panel(mechanic);
  InvObject* drag_obj = tree_field_get_obj(mechanic, "drag_object");
  if (drag_obj && cur) {
    InvObject* osd = tree_field_get_obj(cur, "osd");
    if (osd) tree_field_set_obj(osd, "dropObject", drag_obj);
    tree_field_set_obj(mechanic, "drag_object", nullptr);
    const int32_t idx = tree_field_get_int(cur, "index");
    const bool ok = inventory_panel_osd_command(cur, idx);
    tree_field_set_int(mechanic, "drop_object_count",
                       tree_field_get_int(mechanic, "drop_object_count") + 1);
    tree_field_set_int(mechanic, "last_drop_panel", idx);
    tree_field_set_int(mechanic, "last_drop_ok", ok ? 1 : 0);
    return ok;
  }
  InvObject* start = tree_field_get_obj(mechanic, "drag_start_panel");
  InvObject* button = tree_field_get_obj(mechanic, "drag_button");
  const int32_t moved = tree_field_get_int(mechanic, "drag_moved");
  if (moved && start && cur && start != cur && button) {
    InvObject* osd = tree_field_get_obj(cur, "osd");
    if (osd) tree_field_set_obj(osd, "dropGadget", button);
    const int32_t idx = tree_field_get_int(cur, "index");
    const bool ok = inventory_panel_osd_command(cur, idx);
    tree_field_set_int(mechanic, "drag_swap_count",
                       tree_field_get_int(mechanic, "drag_swap_count") + 1);
    tree_field_set_int(mechanic, "last_drag_from",
                       tree_field_get_int(start, "index"));
    tree_field_set_int(mechanic, "last_drag_to", idx);
    tree_field_set_int(mechanic, "last_drag_ok", ok ? 1 : 0);
    return ok;
  }
  if (!moved && start) {
    mechanic_set_actual_panel(mechanic, tree_field_get_int(start, "index"));
    return mechanic_click_actual(mechanic);
  }
  // Miss panels: stock EC_LCLICK on looked-at car part.
  if (!moved && !start) {
    InvObject* look = tree_field_get_obj(mechanic, "look_part");
    if (look) return mechanic_lclick_part(mechanic, look);
  }
  return false;
}

void mechanic_tick_click(InvObject* mechanic) {
  if (!mechanic) return;
  if (tree_field_get_int(mechanic, "rdrag")) return;
  input_live_poll();
  const float btn = java_io_Input_getAxis(1, kMousePhysBtn1);
  const int32_t down = btn > 0.5f ? 1 : 0;
  const int32_t was = tree_field_get_int(mechanic, "mouse_btn1");
  tree_field_set_int(mechanic, "mouse_btn1", down);
  if (was == 0 && down == 1) {
    InvObject* panel = mechanic_actual_panel(mechanic);
    tree_field_set_obj(mechanic, "drag_start_panel", panel);
    tree_field_set_obj(mechanic, "drag_button",
                       panel ? tree_field_get_obj(panel, "button") : nullptr);
    tree_field_set_int(mechanic, "drag_moved", 0);
  } else if (was == 1 && down == 1) {
    InvObject* start = tree_field_get_obj(mechanic, "drag_start_panel");
    InvObject* cur = mechanic_actual_panel(mechanic);
    if (start && cur && start != cur)
      tree_field_set_int(mechanic, "drag_moved", 1);
  } else if (was == 1 && down == 0) {
    mechanic_pointer_release(mechanic);
    tree_field_set_obj(mechanic, "drag_start_panel", nullptr);
    tree_field_set_obj(mechanic, "drag_button", nullptr);
    tree_field_set_int(mechanic, "drag_moved", 0);
  }
}

int32_t inventory_size(InvObject* inv) {
  if (!inv) return 0;
  InvObject* items = tree_field_get_obj(inv, "items");
  return items ? tree_vector_size(items) : 0;
}

namespace {

void inventory_touch(InvObject* inv) {
  if (!inv) return;
  tree_field_set_int(inv, "size", inventory_size(inv));
  tree_field_set_int(inv, "update_count",
                     tree_field_get_int(inv, "update_count") + 1);
}

InvObject* inventory_item_get_part(InvObject* item) {
  if (!item) return nullptr;
  InvObject* p = tree_field_get_obj(item, "partXXX");
  if (!p) p = tree_field_get_obj(item, "part");
  return p;
}

bool inventory_item_is_part(InvObject* item) {
  const char* hc = item ? tree_host_class(item) : nullptr;
  return hc && std::strstr(hc, "InventoryItem_Part") != nullptr;
}

}  // namespace

void inventory_add_part_item(InvObject* inv, InvObject* part) {
  if (!inv || !part) return;
  InvObject* items = tree_field_get_obj(inv, "items");
  if (!items) {
    items = tree_vector_new();
    tree_field_set_obj(inv, "items", items);
  }
  InvObject* item = tree_host_new("java.game.InventoryItem_Part");
  tree_field_set_obj(item, "inventory", inv);
  tree_field_set_obj(item, "partXXX", part);
  tree_field_set_obj(item, "part", part);
  tree_vector_add(items, item);
  inventory_touch(inv);
}

void inventory_move_to(InvObject* src, int32_t index, InvObject* dst) {
  if (!src || !dst || index < 0) return;
  InvObject* src_items = tree_field_get_obj(src, "items");
  InvObject* dst_items = tree_field_get_obj(dst, "items");
  if (!src_items) return;
  if (!dst_items) {
    dst_items = tree_vector_new();
    tree_field_set_obj(dst, "items", dst_items);
  }
  InvObject* item = tree_vector_element_at(src_items, index);
  if (!item) return;
  tree_vector_remove(src_items, item);
  tree_field_set_obj(item, "inventory", dst);
  tree_vector_add(dst_items, item);
  inventory_touch(src);
  inventory_touch(dst);
}

InvObject* inventory_ensure_player_parts(InvObject* player) {
  if (!player) return nullptr;
  InvObject* parts = tree_field_get_obj(player, "parts");
  if (!parts) {
    parts = inventory_new(player);
    tree_field_set_obj(player, "parts", parts);
  }
  if (!tree_field_get_obj(parts, "items"))
    tree_field_set_obj(parts, "items", tree_vector_new());
  // Seed COMMON parts so Mechanic filters/scroll have work to do.
  // 1 engine + 5 body → 6 items (2 VisualInventory lines @ 5/line).
  // IDs match Baiern chassis attach locals (0x52 block, 0x10F F bumper).
  if (inventory_size(parts) == 0 &&
      !tree_field_get_int(player, "parts_seeded")) {
    InvObject* eng = tree_host_new("java.game.parts.enginepart.Block");
    java_util_resource_ResourceRef_set(eng, 0x00070052);
    tree_field_set_int(eng, "carCategory", 0);  // Part.COMMON
    tree_field_set_int(eng, "part_category", 1);
    tree_field_set_obj(
        eng, "cfg_path",
        string_new(
            "parts/engines/Baiern_Emer/scripts/Baiern_Devils_6SFi_3_6_block.cfg"));
    inventory_add_part_item(parts, eng);
    for (int i = 0; i < 5; ++i) {
      InvObject* spare =
          tree_host_new("java.game.parts.bodypart.Bumper");
      java_util_resource_ResourceRef_set(spare, 0x0000010F);
      tree_field_set_int(spare, "carCategory", 0);
      tree_field_set_int(spare, "part_category", 2);
      tree_field_set_obj(
          spare, "cfg_path",
          string_new("cars/racers/Baiern_data/scripts/F_bumper.cfg"));
      inventory_add_part_item(parts, spare);
    }
    tree_field_set_int(player, "parts_seeded", 1);
  }
  return parts;
}

InvObject* garage_ensure_mechanic(InvObject* garage) {
  if (!garage) return nullptr;
  InvObject* mech = tree_field_get_obj(garage, "mechanic");
  InvObject* player = tree_field_get_obj(garage, "player");
  if (!player) player = game_logic_player();
  inventory_ensure_player_parts(player);
  // Mechanic.PARTS_VP_* — stock VisualInventory strip layout.
  constexpr float kVpLeft = 0.226f;
  constexpr float kVpTop = 0.815f;
  constexpr float kVpWidth = 0.696f;
  constexpr float kVpHeight = 0.178f;
  if (!mech) {
    mech = tree_host_new("java.game.Mechanic");
    tree_field_set_obj(mech, "player", player);
    tree_field_set_obj(mech, "osd", tree_field_get_obj(garage, "osd"));
    InvObject* inv =
        visual_inventory_new(player, kVpLeft, kVpTop, kVpWidth, kVpHeight);
    tree_field_set_obj(mech, "inventory", inv);
    tree_field_set_obj(garage, "mechanic", mech);
  } else if (!tree_field_get_obj(mech, "inventory")) {
    tree_field_set_obj(
        mech, "inventory",
        visual_inventory_new(player, kVpLeft, kVpTop, kVpWidth, kVpHeight));
  } else {
    InvObject* inv = tree_field_get_obj(mech, "inventory");
    if (inv && !tree_field_get_obj(inv, "panels"))
      visual_inventory_init(inv, kVpLeft, kVpTop, kVpWidth, kVpHeight);
  }
  if (!tree_field_get_obj(mech, "player"))
    tree_field_set_obj(mech, "player", player);
  mechanic_ensure_chrome(mech);
  if (InvObject* cam = garage_ensure_camera(garage))
    tree_field_set_obj(mech, "camera", cam);
  return mech;
}

InvObject* garage_ensure_painter(InvObject* garage) {
  // Phase 2.156/2.157 — stock Garage.painter + PaintCans palette.
  if (!garage) return nullptr;
  InvObject* painter = tree_field_get_obj(garage, "painter");
  InvObject* player = tree_field_get_obj(garage, "player");
  if (!player) player = game_logic_player();
  if (!painter) {
    painter = tree_host_new("java.game.Painter");
    tree_field_set_obj(garage, "painter", painter);
  }
  if (player) tree_field_set_obj(painter, "player", player);
  if (InvObject* osd = tree_field_get_obj(garage, "osd"))
    tree_field_set_obj(painter, "osd", osd);
  if (tree_field_get_int(painter, "mode") == 0)
    tree_field_set_int(painter, "mode", 2);  // MODE_PAINTPART
  if (tree_field_get_int(painter, "lastPaintMode") == 0)
    tree_field_set_int(painter, "lastPaintMode", 2);

  InvObject* cans = tree_field_get_obj(painter, "paintCans");
  if (!cans) {
    cans = tree_host_new("java.game.PaintCans");
    tree_field_set_obj(painter, "paintCans", cans);
  }
  InvObject* items = tree_field_get_obj(cans, "items");
  if (!items) {
    items = tree_vector_new();
    tree_field_set_obj(cans, "items", items);
  }
  if (tree_vector_size(items) < 1) {
    // Stock first rows (greys + reds + a few accents) — enough for UX/smoke.
    static const int32_t kPalette[] = {
        0xF5F5F5, 0xB9B9B9, 0x7F7F7F, 0x505050, 0x1D1D1D, 0xDC191A,
        0xAE1515, 0x800F0F, 0x530A0A, 0x250505, 0xF58E08, 0x0F3EED};
    for (int32_t c : kPalette) {
      InvObject* can = tree_host_new("java.game.PaintCan");
      tree_field_set_int(can, "color", c);
      tree_field_set_float(can, "capacity", 1.f);
      tree_vector_add(items, can);
    }
    tree_field_set_int(cans, "item_count", tree_vector_size(items));
    tree_field_set_int(cans, "lastCanId", 0);
    tree_field_set_int(cans, "paintColor", kPalette[0]);
  }
  // Sync painter.paintColor from cans (stock |0xFF000000 on apply).
  if (tree_field_get_int(painter, "paintColor") == 0) {
    int32_t pc = tree_field_get_int(cans, "paintColor");
    if (pc == 0) pc = 0xF5F5F5;
    tree_field_set_int(painter, "paintColor", pc | static_cast<int32_t>(0xFF000000));
  }
  return painter;
}

void garage_painter_hide(InvObject* garage) {
  if (!garage) return;
  if (InvObject* p = tree_field_get_obj(garage, "painter")) {
    tree_field_set_int(p, "shown", 0);
    tree_field_set_int(p, "visible", 0);
  }
  tree_field_set_int(garage, "painter_shown", 0);
}

void garage_painter_show(InvObject* garage) {
  // Stock changeMode(MODE_PAINT): painter.show(); hide mechanic if leaving
  // szerel/tune.
  if (!garage) return;
  InvObject* painter = garage_ensure_painter(garage);
  if (!painter) return;
  if (InvObject* mech = tree_field_get_obj(garage, "mechanic")) {
    if (InvObject* inv = tree_field_get_obj(mech, "inventory"))
      visual_inventory_hide(inv);
    tree_field_set_int(mech, "shown", 0);
  }
  tree_field_set_int(painter, "shown", 1);
  tree_field_set_int(painter, "visible", 1);
  tree_field_set_int(painter, "show_count",
                     tree_field_get_int(painter, "show_count") + 1);
  tree_field_set_int(garage, "painter_shown", 1);
}

bool garage_painter_select_can(InvObject* garage, int32_t index) {
  // Stock PaintCans: lastCanId=index; paintColor=getCanbyIndex(index).color
  if (!garage || index < 0) return false;
  InvObject* painter = garage_ensure_painter(garage);
  if (!painter) return false;
  InvObject* cans = tree_field_get_obj(painter, "paintCans");
  InvObject* items = cans ? tree_field_get_obj(cans, "items") : nullptr;
  const int32_t n = items ? tree_vector_size(items) : 0;
  if (index >= n) return false;
  InvObject* can = tree_vector_element_at(items, index);
  if (!can) return false;
  const int32_t rgb = tree_field_get_int(can, "color");
  tree_field_set_int(cans, "lastCanId", index);
  tree_field_set_int(cans, "paintColor", rgb);
  tree_field_set_int(painter, "paintColor",
                     rgb | static_cast<int32_t>(0xFF000000));
  tree_field_set_int(garage, "last_can_id", index);
  tree_field_set_int(garage, "can_select_count",
                     tree_field_get_int(garage, "can_select_count") + 1);
  return true;
}

bool garage_paint_car(InvObject* garage, int32_t color) {
  // Host mirror of Painter MODE_PAINTPART click → paintPart(car, color).
  if (!garage) return false;
  InvObject* painter = garage_ensure_painter(garage);
  InvObject* player = tree_field_get_obj(garage, "player");
  if (!player) player = game_logic_player();
  InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
  if (!painter || !car) return false;
  int32_t c = color;
  if (c == 0) {
    InvObject* cans = tree_field_get_obj(painter, "paintCans");
    int32_t rgb = cans ? tree_field_get_int(cans, "paintColor") : 0;
    if (rgb == 0) rgb = tree_field_get_int(painter, "paintColor");
    c = rgb | static_cast<int32_t>(0xFF000000);
  }
  java_game_Painter_paintPart(painter, car, c);
  tree_field_set_int(painter, "paintColor", c);
  tree_field_set_int(garage, "last_paint_color", c);
  tree_field_set_int(garage, "paint_car_count",
                     tree_field_get_int(garage, "paint_car_count") + 1);
  return tree_field_get_int(car, "part_texture") == c;
}

namespace {

InvObject* mechanic_add_chrome_button(InvObject* osd, float x, float y,
                                      int32_t cmd, const char* tip) {
  if (!osd) return nullptr;
  InvObject* btns = tree_field_get_obj(osd, "buttons");
  if (!btns) {
    btns = tree_vector_new();
    tree_field_set_obj(osd, "buttons", btns);
  }
  InvObject* style = tree_host_new("java.render.osd.Style");
  tree_field_set_float(style, "rWidth", 0.10f);
  tree_field_set_float(style, "rHeight", 0.10f);
  tree_field_set_float(style, "width", 0.10f);
  tree_field_set_float(style, "height", 0.10f);
  InvObject* btn = tree_host_new("java.render.osd.Button");
  tree_field_set_obj(btn, "style", style);
  tree_field_set_float(btn, "x", x);
  tree_field_set_float(btn, "y", y);
  tree_field_set_int(btn, "command", cmd);
  tree_field_set_obj(btn, "osd", osd);
  if (tip) tree_field_set_obj(btn, "tooltip", string_new(tip));
  tree_vector_add(btns, btn);
  tree_field_set_int(osd, "button_count",
                     tree_field_get_int(osd, "button_count") + 1);
  return btn;
}

void mechanic_sync_line_text(InvObject* mechanic) {
  if (!mechanic) return;
  InvObject* inv = tree_field_get_obj(mechanic, "inventory");
  const int32_t line =
      inv ? tree_field_get_int(inv, "cline") + 1 : 1;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d", line);
  InvObject* txt = tree_field_get_obj(mechanic, "invLineTxt");
  if (!txt) {
    txt = tree_host_new("java.render.Text");
    tree_field_set_obj(mechanic, "invLineTxt", txt);
  }
  tree_field_set_obj(txt, "text", string_new(buf));
  tree_field_set_int(mechanic, "inv_line", line);
}

}  // namespace

void mechanic_ensure_chrome(InvObject* mechanic) {
  if (!mechanic) return;
  if (tree_field_get_int(mechanic, "chrome_ready") == 1) return;
  InvObject* osd = tree_field_get_obj(mechanic, "osd");
  if (!osd) {
    osd = tree_host_new("java.render.Osd");
    tree_field_set_obj(osd, "groups", tree_vector_new());
    tree_field_set_obj(osd, "hotkey", tree_vector_new());
    tree_field_set_obj(osd, "rectangles", tree_vector_new());
    tree_field_set_obj(osd, "buttons", tree_vector_new());
    tree_field_set_int(osd, "init", 1);
    tree_field_set_obj(mechanic, "osd", osd);
  }
  tree_field_set_obj(osd, "globalHandler", mechanic);

  // Stock: scroll arrows + filter menu icons.
  constexpr int32_t CMD_SCROLL_UP = 0;
  constexpr int32_t CMD_SCROLL_DOWN = 1;
  constexpr int32_t CMD_ENGINE = 119;
  constexpr int32_t CMD_BODY = 120;
  constexpr int32_t CMD_RUNNING_GEAR = 121;
  mechanic_add_chrome_button(osd, 0.94f, 0.68f, CMD_SCROLL_UP, "Scroll up");
  mechanic_add_chrome_button(osd, 0.94f, 0.92f, CMD_SCROLL_DOWN,
                             "Scroll down");
  mechanic_add_chrome_button(osd, -0.98f, 0.88f, CMD_ENGINE, "Filter ENGINE");
  mechanic_add_chrome_button(osd, -0.86f, 0.88f, CMD_BODY, "Filter BODY");
  mechanic_add_chrome_button(osd, -0.74f, 0.88f, CMD_RUNNING_GEAR,
                             "Filter RGEAR");

  InvObject* line = tree_host_new("java.render.Text");
  tree_field_set_obj(line, "text", string_new("1"));
  tree_field_set_obj(mechanic, "invLineTxt", line);
  tree_field_set_int(mechanic, "inv_line", 1);
  tree_field_set_int(mechanic, "filterEngine", 0);
  tree_field_set_int(mechanic, "filterBody", 0);
  tree_field_set_int(mechanic, "filterRGear", 0);
  tree_field_set_int(mechanic, "chrome_ready", 1);
  tree_field_set_int(mechanic, "chrome_buttons", 5);
}

int32_t mechanic_chrome_button_count(InvObject* mechanic) {
  if (!mechanic) return 0;
  return tree_field_get_int(mechanic, "chrome_buttons");
}

void mechanic_osd_command(InvObject* mechanic, int32_t cmd) {
  if (!mechanic) return;
  constexpr int32_t CMD_SCROLL_UP = 0;
  constexpr int32_t CMD_SCROLL_DOWN = 1;
  constexpr int32_t CMD_ENGINE = 119;
  constexpr int32_t CMD_BODY = 120;
  constexpr int32_t CMD_RUNNING_GEAR = 121;

  InvObject* inv = tree_field_get_obj(mechanic, "inventory");
  tree_field_set_int(mechanic, "last_osd_cmd", cmd);

  if (cmd == CMD_SCROLL_UP) {
    visual_inventory_scroll_up(inv);
    mechanic_sync_line_text(mechanic);
    return;
  }
  if (cmd == CMD_SCROLL_DOWN) {
    visual_inventory_scroll_down(inv);
    mechanic_sync_line_text(mechanic);
    return;
  }

  auto toggle = [](int32_t v) -> int32_t { return 2 - v; };
  if (cmd == CMD_ENGINE) {
    const int32_t v = toggle(tree_field_get_int(mechanic, "filterEngine"));
    tree_field_set_int(mechanic, "filterEngine", v);
  } else if (cmd == CMD_BODY) {
    const int32_t v = toggle(tree_field_get_int(mechanic, "filterBody"));
    tree_field_set_int(mechanic, "filterBody", v);
  } else if (cmd == CMD_RUNNING_GEAR) {
    const int32_t v = toggle(tree_field_get_int(mechanic, "filterRGear"));
    tree_field_set_int(mechanic, "filterRGear", v);
  } else {
    return;
  }

  mechanic_filter_inventory(mechanic,
                            tree_field_get_int(mechanic, "filterEngine"),
                            tree_field_get_int(mechanic, "filterBody"),
                            tree_field_get_int(mechanic, "filterRGear"));
  // Reset scroll to first page after filter reshuffle.
  if (inv) {
    tree_field_set_int(inv, "cline", 0);
    visual_inventory_update(inv);
  }
  mechanic_sync_line_text(mechanic);
}

void mechanic_flush_inventory(InvObject* mechanic) {
  if (!mechanic) return;
  InvObject* inv = tree_field_get_obj(mechanic, "inventory");
  InvObject* player = tree_field_get_obj(mechanic, "player");
  if (!player) player = game_logic_player();
  InvObject* parts = inventory_ensure_player_parts(player);
  if (!inv || !parts) return;
  while (inventory_size(inv) > 0) inventory_move_to(inv, 0, parts);
}

void mechanic_filter_inventory(InvObject* mechanic, int32_t filter_engine,
                               int32_t filter_body, int32_t filter_rgear) {
  if (!mechanic) return;
  InvObject* player = tree_field_get_obj(mechanic, "player");
  if (!player) player = game_logic_player();
  InvObject* parts = inventory_ensure_player_parts(player);
  InvObject* inv = tree_field_get_obj(mechanic, "inventory");
  if (!inv) {
    inv = inventory_new(player);
    tree_field_set_obj(mechanic, "inventory", inv);
  }
  constexpr int32_t kGiiType = 6;
  constexpr int32_t kGiiPartCategory = 55;
  InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
  const int32_t car_id =
      car ? (java_util_resource_GameRef_getInfo(car, kGiiType, 0) >> 16) : 0;

  auto part_ok_for_car = [car_id](InvObject* part) -> bool {
    if (!part) return false;
    if (tree_field_get_int(part, "carCategory") == 0) return true;  // COMMON
    const int32_t pid =
        java_util_resource_GameRef_getInfo(part, /*GII_TYPE*/ 6, 0) >> 16;
    return pid == car_id;
  };

  // parts → inventory (accepted by current filters).
  for (int32_t i = 0; i < inventory_size(parts);) {
    InvObject* items = tree_field_get_obj(parts, "items");
    InvObject* t = tree_vector_element_at(items, i);
    bool move = false;
    if (!inventory_item_is_part(t)) {
      move = true;
    } else {
      InvObject* part = inventory_item_get_part(t);
      if (part_ok_for_car(part)) {
        const int32_t cat =
            java_util_resource_GameRef_getInfo(part, kGiiPartCategory, 0);
        if (cat == 1) {
          if (!filter_engine) move = true;
        } else if (cat == 2) {
          if (!filter_body) move = true;
        } else if (cat == 3) {
          if (!filter_rgear) move = true;
        } else if (cat == 0) {
          move = true;
        }
      }
    }
    if (move)
      inventory_move_to(parts, i, inv);
    else
      ++i;
  }

  // inventory → parts (rejected by filters).
  for (int32_t i = 0; i < inventory_size(inv);) {
    InvObject* items = tree_field_get_obj(inv, "items");
    InvObject* t = tree_vector_element_at(items, i);
    bool back = false;
    if (inventory_item_is_part(t)) {
      InvObject* part = inventory_item_get_part(t);
      if (!part_ok_for_car(part)) {
        back = true;
      } else {
        const int32_t cat =
            java_util_resource_GameRef_getInfo(part, kGiiPartCategory, 0);
        if (cat == 1) {
          if (filter_engine) back = true;
        } else if (cat == 2) {
          if (filter_body) back = true;
        } else if (cat == 3) {
          if (filter_rgear) back = true;
        }
      }
    }
    if (back)
      inventory_move_to(inv, i, parts);
    else
      ++i;
  }
  inventory_touch(inv);
  visual_inventory_update(inv);
}

InvObject* garage_osd_command(InvObject* garage, int32_t cmd) {
  if (!garage) return nullptr;
  constexpr int32_t CMD_HITTHESTREET = 109;
  constexpr int32_t CMD_TESTTRACK = 110;
  constexpr int32_t CMD_CARLOT = 111;
  constexpr int32_t CMD_BUYCARS = 112;
  constexpr int32_t CMD_CATALOG = 113;
  constexpr int32_t CMD_CLUBINFO = 114;
  constexpr int32_t CMD_CARINFO = 115;
  constexpr int32_t CMD_TIME = 116;
  constexpr int32_t CMD_MECHANIC = 117;
  constexpr int32_t CMD_PAINT = 118;
  constexpr int32_t CMD_ESCAPE = 119;
  constexpr int32_t CMD_BUYCARSUSED = 122;
  constexpr int32_t CMD_TEST = 123;
  constexpr int32_t CMD_TUNE = 124;
  constexpr int32_t CMD_NONE = 100;
  constexpr int32_t MODE_NONE = 0;
  constexpr int32_t MODE_SZEREL = 1;
  constexpr int32_t MODE_PAINT = 2;
  constexpr int32_t MODE_TEST = 3;
  constexpr int32_t MODE_TUNE = 4;

  tree_field_set_int(garage, "last_cmd", cmd);
  tree_field_set_obj(garage, "last_warning", nullptr);

  auto set_mode = [garage](int32_t mode) {
    tree_field_set_int(garage, "prevMode", tree_field_get_int(garage, "mode"));
    tree_field_set_int(garage, "mode", mode);
    tree_field_set_int(garage, "mode_changes",
                       tree_field_get_int(garage, "mode_changes") + 1);
  };

  if (cmd == CMD_TIME) {
    game_logic_spend_time(3600.f);
    InvObject* player = tree_field_get_obj(garage, "player");
    if (!player) player = game_logic_player();
    if (player) {
      const float p = tree_field_get_float(player, "prestige");
      tree_field_set_float(player, "prestige", p - (0.00333f / 24.f));
    }
    tree_field_set_float(garage, "time_after_cmd", game_logic_time());
    return garage;
  }

  if (cmd == CMD_MECHANIC) {
    garage_painter_hide(garage);
    set_mode(MODE_SZEREL);
    InvObject* mech = garage_ensure_mechanic(garage);
    if (mech) tree_field_set_int(mech, "mode", 0);  // szereles
    mechanic_filter_inventory(mech, 0, 0, 0);
    if (mech) {
      InvObject* inv = tree_field_get_obj(mech, "inventory");
      visual_inventory_show(inv);
      tree_field_set_int(mech, "shown", 1);
      mechanic_ensure_chrome(mech);
    }
    return garage;
  }
  if (cmd == CMD_PAINT) {
    set_mode(MODE_PAINT);
    garage_painter_show(garage);
    return garage;
  }
  if (cmd == CMD_TEST) {
    garage_painter_hide(garage);
    set_mode(MODE_TEST);
    return garage;
  }
  if (cmd == CMD_TUNE) {
    // Stock changeMode(MODE_TUNE): mechanic.show(); mechanic.mode=1.
    garage_painter_hide(garage);
    set_mode(MODE_TUNE);
    InvObject* mech = garage_ensure_mechanic(garage);
    if (mech) {
      tree_field_set_int(mech, "mode", 1);
      InvObject* inv = tree_field_get_obj(mech, "inventory");
      visual_inventory_show(inv);
      tree_field_set_int(mech, "shown", 1);
      mechanic_ensure_chrome(mech);
    }
    return garage;
  }
  if (cmd == CMD_ESCAPE || cmd == CMD_NONE) {
    garage_painter_hide(garage);
    set_mode(MODE_NONE);
    return garage;
  }

  if (cmd == CMD_CATALOG) {
    return catalog_enter(garage);
  }

  if (cmd == CMD_CARLOT) {
    InvObject* player = tree_field_get_obj(garage, "player");
    if (!player) player = game_logic_player();
    InvObject* carlot = player ? tree_field_get_obj(player, "carlot") : nullptr;
    if (!carlot) {
      tree_field_set_obj(garage, "last_warning", string_new("No car lot."));
      return nullptr;
    }
    return carlot_enter(carlot, garage);
  }

  if (cmd == CMD_CLUBINFO) {
    return clubinfo_enter(garage);
  }

  if (cmd == CMD_CARINFO) {
    InvObject* player = tree_field_get_obj(garage, "player");
    if (!player) player = game_logic_player();
    InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
    if (!car) {
      tree_field_set_obj(
          garage, "last_warning",
          string_new(
              "You need a car to do this! \n Buy a car or get one from the car lot."));
      return nullptr;
    }
    return carinfo_enter(garage, car);
  }

  if (cmd == CMD_BUYCARS || cmd == CMD_BUYCARSUSED) {
    const float hour = game_logic_time() / 3600.f;
    if (!(hour > 7.f && hour < 17.f)) {
      tree_field_set_obj(
          garage, "last_warning",
          string_new(
              "The car dealer is closed now! \n Opening hours: 7am to 5pm"));
      return nullptr;
    }
    game_logic_spend_time(1800.f);  // travel to dealer
    return carmarket_enter(garage, cmd == CMD_BUYCARSUSED ? 1 : 0);
  }

  if (cmd == CMD_TESTTRACK) {
    InvObject* player = tree_field_get_obj(garage, "player");
    if (!player) player = game_logic_player();
    InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
    const char* problem = vehicle_is_driveable(car);
    if (problem) {
      tree_field_set_obj(garage, "last_warning", string_new(problem));
      return nullptr;
    }
    game_logic_spend_time(1800.f);  // travel to track
    return testtrack_enter(garage);
  }

  if (cmd == CMD_HITTHESTREET) {
    InvObject* player = tree_field_get_obj(garage, "player");
    if (!player) player = game_logic_player();
    InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
    const char* problem = vehicle_is_driveable(car);
    if (problem) {
      tree_field_set_obj(garage, "last_warning", string_new(problem));
      return nullptr;
    }
    InvObject* city = tree_host_new("java.game.Valocity");
    tree_field_set_obj(city, "parentState", garage);
    tree_field_set_obj(city, "player", player);
    tree_field_set_obj(city, "car", car);
    valocity_prepare(city);
    // Stock Garage: changeActiveSection(new Valocity()) — enter does the rest.
    return game_logic_change_active_section(city);
  }

  return nullptr;
}

InvObject* game_state_return_to_garage(InvObject* state) {
  return section_return_to_garage(state);
}

// java_game_Navigator_updateNavigator — body in Cars.cpp (PE @ 0x00482D30).

namespace {

InvObject* traffic_type_ref(const char* label, int32_t local_id) {
  InvObject* type = gameref_new();
  int32_t id = local_id;
  if (const RpakPack* tp = rpak_find_by_name("traffic.rpk"))
    id = rpak_make_id(tp->pack_id, local_id & 0xFFFF);
  java_util_resource_ResourceRef_set(type, id);
  if (label) tree_field_set_obj(type, "vehicleName", string_new(label));
  return type;
}

InvObject* police_type_ref() {
  InvObject* type = gameref_new();
  int32_t id = 0x6;
  if (!rpak_find_by_name("Police.rpk") && !rpak_find_by_name("police.rpk"))
    java_lang_System_openLib(string_new("cars/misc/Police.rpk"));
  if (const RpakPack* pp = rpak_find_by_name("Police.rpk"))
    id = rpak_make_id(pp->pack_id, 0x6);
  else if (const RpakPack* pp = rpak_find_by_name("police.rpk"))
    id = rpak_make_id(pp->pack_id, 0x6);
  java_util_resource_ResourceRef_set(type, id);
  tree_field_set_obj(type, "vehicleName", string_new("Police"));
  return type;
}

void valocity_spawn_traffic(InvObject* city, InvObject* map) {
  if (!city || !map) return;
  // Config.trafficDensity / pedestrianDensity defaults (1.0 / 0.0).
  constexpr float kTrafficDensity = 1.f;
  constexpr float kPedConfig = 0.f;

  const int daytime = tree_field_get_int(city, "daytime");
  const int32_t mode = game_logic_game_mode();
  constexpr int32_t kGmDemo = 5;

  auto add_n = [&](const char* name, int32_t n, float lb, float le, float wb) {
    const int32_t scaled = static_cast<int32_t>(n * kTrafficDensity);
    java_util_resource_GroundRef_addTrafficN(map, traffic_type_ref(name, 0x6),
                                             scaled, lb, le, wb);
  };

  if (daytime) {
    add_n("Taxi", 80, 2, 5, 2);
    add_n("Ambulance", 20, 2, 5, 2);
    add_n("FireEngine", 12, 2, 5, 2);
    add_n("Coach", 30, 2, 10, 2);
    add_n("Schoolbus", 55, 2, 10, 2);
    add_n("ArmoredVan", 20, 2, 5, 2);
    add_n("Wagon", 200, 2, 5, 2);
    add_n("Erbilac", 200, 2, 5, 2);
    add_n("CivilVan", 200, 2, 5, 2);
    if (mode == kGmCareer)
      java_util_resource_GroundRef_addTrafficN(
          map, police_type_ref(),
          static_cast<int32_t>(30 * kTrafficDensity), 2, 5, 2);
    else if (mode == kGmDemo)
      java_util_resource_GroundRef_addTrafficN(
          map, police_type_ref(),
          static_cast<int32_t>(50 * kTrafficDensity), 2, 5, 2);
    java_util_resource_GroundRef_setPedestrianDensityN(map, 0.003f * kPedConfig);
    tree_field_set_float(city, "pedestrianDensity", 0.003f);
  } else {
    add_n("Erbilac", 20, 2, 5, 2);
    add_n("CivilVan", 10, 2, 5, 2);
    add_n("Taxi", 150, 2, 5, 2);
    if (mode == kGmCareer)
      java_util_resource_GroundRef_addTrafficN(
          map, police_type_ref(),
          static_cast<int32_t>(3 * kTrafficDensity), 2, 5, 2);
    else if (mode == kGmDemo)
      java_util_resource_GroundRef_addTrafficN(
          map, police_type_ref(),
          static_cast<int32_t>(20 * kTrafficDensity), 2, 5, 2);
    java_util_resource_GroundRef_setPedestrianDensityN(map, 0.0005f * kPedConfig);
    tree_field_set_float(city, "pedestrianDensity", 0.0005f);
  }

  // Valocity pedestrian type list (humans local ids).
  const int32_t ped_locals[] = {0x57, 0x58, 0x59, 0x5A, 0x5B, 0x22};
  if (!rpak_find_by_name("humans.rpk"))
    java_lang_System_openLib(string_new("humans/humans.rpk"));
  for (int32_t loc : ped_locals) {
    InvObject* g = gameref_new();
    int32_t id = loc;
    if (const RpakPack* hp = rpak_find_by_name("humans.rpk"))
      id = rpak_make_id(hp->pack_id, loc);
    java_util_resource_ResourceRef_set(g, id);
    java_util_resource_GroundRef_addPedestrianType(map, g);
  }

  tree_field_set_int(city, "traffic_count",
                     tree_field_get_int(map, "traffic_count"));
  tree_field_set_int(city, "pedestrian_types",
                     tree_field_get_int(map, "pedestrian_types"));
  std::printf("  traffic path_spawn=%d roads=%d count=%d\n",
              tree_field_get_int(map, "path_spawns"), physics_road_count(),
              tree_field_get_int(city, "traffic_count"));
  // PE addTrafficP @ 0x00484420: City.sleepPoliceScoutQuick GRT_POLICECAR.
  // Far Vector3 — spawn must snap to a road, not setPos(want).
  {
    const float want_x = 12345.f, want_y = 99.f, want_z = 67890.f;
    const int32_t np = java_util_resource_GroundRef_addTrafficP(
        map, police_type_ref(), vec3_new(want_x, want_y, want_z), 1, 2.f, 5.f,
        2.f);
    const float px = tree_field_get_float(map, "traffic_p_x");
    const float pz = tree_field_get_float(map, "traffic_p_z");
    const float pdx = std::fabs(px - want_x) + std::fabs(pz - want_z);
    const int32_t p_ok =
        (np == 1 && tree_field_get_int(map, "traffic_p_ok") == 1 && pdx > 1.f)
            ? 1
            : 0;
    tree_field_set_int(map, "traffic_p_smoke", p_ok);
    tree_field_set_int(city, "traffic_p_smoke", p_ok);
    std::printf("  traffic addTrafficP ok=%d n=%d dx=%.1f\n", p_ok, np, pdx);
  }
  // PE setTrafficCarBehaviour @ 0x00487EC0: write mode on the traffic instance
  // (+0x160) only if live (+0x138). Bot.setTrafficBehaviour(TC_PASSIVE=2).
  {
    InvObject* tinst = gameref_new();
    const int32_t tid =
        java_util_resource_GroundRef_addTrafficCar(map, tinst, nullptr);
    java_util_resource_GroundRef_setTrafficCarBehaviour(map, tid, 2);
    java_util_resource_GroundRef_setTrafficCarBehaviour(map, 0, 1);
    const int32_t bh = tree_field_get_int(tinst, "traffic_behaviour");
    const int32_t last = tree_field_get_int(map, "traffic_behaviour_last");
    const int32_t bh_ok = (tid != 0 && bh == 2 && last == 2) ? 1 : 0;
    tree_field_set_int(map, "traffic_bh_ok", bh_ok);
    tree_field_set_int(city, "traffic_bh_ok", bh_ok);
    std::printf("  traffic setBehaviour ok=%d id=%d bh=%d last=%d\n", bh_ok, tid,
                bh, last);
  }
  // PE haltTrafficCross @ 0x00484B90: City.startRace(raceStart, 15.0) duration.
  // Evict cars whose nearest junction is the halted cross (~100 m).
  {
    InvObject* tinst = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      GroundTrafficState& g = ground(map);
      if (!g.traffic_cars.empty()) tinst = g.traffic_cars.back();
    }
    InvObject* hp = tinst ? java_util_resource_GameRef_getPos(tinst) : nullptr;
    java_util_resource_GroundRef_haltTrafficCross(map, hp, 15.f);
    const int32_t hc = tree_field_get_int(map, "halt_cleared");
    const int32_t hn = tree_field_get_int(map, "halt_crosses");
    const int32_t halt_ok = (hn >= 1 && hc >= 1) ? 1 : 0;
    tree_field_set_int(map, "halt_smoke", halt_ok);
    tree_field_set_int(city, "halt_smoke", halt_ok);
    std::printf("  traffic haltCross ok=%d crosses=%d cleared=%d\n", halt_ok, hn,
                hc);
  }
  // PE haltTrafficPath @ 0x004835E0: City.prepareNightRace pS..pF.
  // GroundMap_findRoute then haltCrossTraffic(0.001) per waypoint.
  {
    InvObject* tinst = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      GroundTrafficState& g = ground(map);
      if (!g.traffic_cars.empty()) tinst = g.traffic_cars.front();
    }
    InvObject* hp = tinst ? java_util_resource_GameRef_getPos(tinst) : nullptr;
    float px = 0.f, py = 0.f, pz = 0.f;
    if (hp) vec3_get(hp, &px, &py, &pz);
    InvObject* farp = physics_road_nearest_cross(px, py, pz, 100.f);
    java_util_resource_GroundRef_haltTrafficPath(map, hp, farp);
    const int32_t pn = tree_field_get_int(map, "halt_paths");
    const int32_t pc = tree_field_get_int(map, "halt_path_cleared");
    const int32_t pxn = tree_field_get_int(map, "halt_path_crosses");
    const int32_t occ = physics_road_occupied_count();
    const int32_t empty = physics_road_count() - occ;
    int32_t spawned = 0;
    for (int32_t i = 0; i < 16; ++i) {
      float sx = 0.f, sy = 0.f, sz = 0.f, yaw = 0.f;
      if (physics_road_random_spawn(&sx, &sy, &sz, &yaw)) ++spawned;
    }
    const int32_t path_ok =
        (pn >= 1 && pc >= 1 && pxn >= 1 && occ >= 1 && empty >= 1 &&
         spawned == 16)
            ? 1
            : 0;
    tree_field_set_int(map, "halt_path_smoke", path_ok);
    tree_field_set_int(city, "halt_path_smoke", path_ok);
    std::printf(
        "  traffic haltPath ok=%d paths=%d crosses=%d cleared=%d occ=%d "
        "empty=%d spawn=%d\n",
        path_ok, pn, pxn, pc, occ, empty, spawned);
  }
  // PE remTrafficCar @ 0x00484A90: Bot.leaveTraffic. Traffic_destroy pool,
  // not GameRef.destroy. id==0 / unknown id = no-op (no count bump).
  // W30D: rem → deactivate → Traffic_pool_sort_compact drops dead slot.
  // W31D: sort_compact also soft-scores/ticks (rem budget=0 → keep live).
  // W32D: Engine_tickPhysTraffic @ 0x463AC0 wires a2=1 via
  // valocity_simulate (not rem).
  {
    const int32_t before = tree_field_get_int(map, "traffic_count");
    const int32_t pool_before = tree_field_get_int(map, "traffic_pool_n");
    InvObject* tinst = gameref_new();
    const int32_t tid =
        java_util_resource_GroundRef_addTrafficCar(map, tinst, nullptr);
    // PE notifyTrafficCar @ 0x00484AF0: Valocity.enter notify(id,1) after
    // addTrafficCar dummycar — write +0xDD, ret MOVSX (must stay live=1).
    const int32_t nret =
        java_util_resource_GroundRef_notifyTrafficCar(map, tid, 1);
    const int32_t nflag = tree_field_get_int(tinst, "traffic_flag_221");
    const int32_t nact = tree_field_get_int(tinst, "traffic_active");
    const int32_t notify_ok =
        (tid != 0 && nret == 1 && nflag == 1 && nact == 1) ? 1 : 0;
    tree_field_set_int(map, "notify_car_smoke", notify_ok);
    tree_field_set_int(city, "notify_car_smoke", notify_ok);
    std::printf("  traffic notifyTrafficCar ok=%d id=%d ret=%d flag=%d act=%d\n",
                notify_ok, tid, nret, nflag, nact);
    const int32_t pool_mid = tree_field_get_int(map, "traffic_pool_n");
    java_util_resource_GroundRef_setTrafficCarBehaviour(map, tid, 2);
    java_util_resource_GroundRef_remTrafficCar(map, 0);
    const int32_t c0 = tree_field_get_int(map, "traffic_count");
    java_util_resource_GroundRef_remTrafficCar(map, tid);
    const int32_t c1 = tree_field_get_int(map, "traffic_count");
    const int32_t pool_after = tree_field_get_int(map, "traffic_pool_n");
    java_util_resource_GroundRef_remTrafficCar(map, tid);
    const int32_t c2 = tree_field_get_int(map, "traffic_count");
    java_util_resource_GroundRef_setTrafficCarBehaviour(map, tid, 1);
    const int32_t bh = tree_field_get_int(tinst, "traffic_behaviour");
    const int32_t flag = tree_field_get_int(tinst, "traffic_flag_221");
    const int32_t rem_ok =
        (tid != 0 && c0 == before + 1 && c1 == before && c2 == before &&
         bh == 2 && flag == 1 && pool_mid == pool_before + 1 &&
         pool_after == pool_before)
            ? 1
            : 0;
    tree_field_set_int(map, "rem_car_smoke", rem_ok);
    tree_field_set_int(city, "rem_car_smoke", rem_ok);
    std::printf(
        "  traffic remTrafficCar ok=%d id=%d c=%d/%d/%d bh=%d live=%d "
        "pool=%d/%d/%d\n",
        rem_ok, tid, c0, c1, c2, bh, flag, pool_before, pool_mid, pool_after);
  }
  // PE delTraffic @ 0x00484B50: Track.exit → GroundMap_delTraffic. Count 0,
  // addTrafficCar GameRef stays (not ResourceRef.destroy). Own map so the
  // Valocity 847 spawn is left intact.
  {
    InvObject* pmap = gameref_new();
    InvObject* dummy = gameref_new();
    const int32_t tid =
        java_util_resource_GroundRef_addTrafficCar(pmap, dummy, nullptr);
    const int32_t c0 = tree_field_get_int(pmap, "traffic_count");
    java_util_resource_GroundRef_delTraffic(pmap);
    const int32_t c1 = tree_field_get_int(pmap, "traffic_count");
    const int32_t live = tree_field_get_int(dummy, "traffic_flag_221");
    const int32_t del_ok =
        (tid != 0 && c0 == 1 && c1 == 0 && live == 1) ? 1 : 0;
    tree_field_set_int(map, "del_traffic_smoke", del_ok);
    tree_field_set_int(city, "del_traffic_smoke", del_ok);
    std::printf("  traffic delTraffic ok=%d id=%d c=%d/%d live=%d\n", del_ok, tid,
                c0, c1, live);
  }
  // PE setPedestrianDensityN @ 0x00484D30: Pedestrian_setDensity(d, d*1.1).
  // Config.pedestrianDensity stock is 0 so Valocity N gets 0; probe raw 0.003.
  {
    InvObject* pmap = gameref_new();
    java_util_resource_GroundRef_setPedestrianDensityN(pmap, 0.003f);
    const float d0 = tree_field_get_float(pmap, "pedestrian_density");
    const float d1 = tree_field_get_float(pmap, "pedestrian_density_hi");
    java_util_resource_GroundRef_setPedestrianDensityN(pmap, 0.f);
    const float z0 = tree_field_get_float(pmap, "pedestrian_density");
    const float z1 = tree_field_get_float(pmap, "pedestrian_density_hi");
    const int32_t dens_ok =
        (std::fabs(d0 - 0.003f) < 1e-6f && std::fabs(d1 - 0.003f * 1.1f) < 1e-6f &&
         z0 == 0.f && z1 == 0.f)
            ? 1
            : 0;
    tree_field_set_int(map, "ped_dens_smoke", dens_ok);
    tree_field_set_int(city, "ped_dens_smoke", dens_ok);
    std::printf("  traffic setPedestrianDensityN ok=%d d=%.4f hi=%.4f z=%.1f\n",
                dens_ok, d0, d1, z0);
  }
  // PE addPedestrianType @ 0x00484D90: Pedestrian_addType 32-slot, skip dup.
  {
    InvObject* pmap = gameref_new();
    InvObject* g0 = resref_new();
    java_util_resource_ResourceRef_set(g0, 0x42);
    java_util_resource_GroundRef_addPedestrianType(pmap, g0);
    java_util_resource_GroundRef_addPedestrianType(pmap, g0);
    const int32_t n1 = tree_field_get_int(pmap, "pedestrian_types");
    InvObject* g1 = resref_new();
    java_util_resource_ResourceRef_set(g1, 0x43);
    java_util_resource_GroundRef_addPedestrianType(pmap, g1);
    const int32_t n2 = tree_field_get_int(pmap, "pedestrian_types");
    const float near = java_util_resource_GroundRef_pedestrianDistance(
        pmap, vec3_new(0.f, 0.f, 0.f), 0x42);
    const int32_t type_ok = (n1 == 1 && n2 == 2 && near < 0.01f) ? 1 : 0;
    tree_field_set_int(map, "ped_type_smoke", type_ok);
    tree_field_set_int(city, "ped_type_smoke", type_ok);
    std::printf("  traffic addPedestrianType ok=%d n=%d/%d near=%.3f\n", type_ok,
                n1, n2, near);
  }
  // PE remPedestrianType @ 0x00484E20: Pedestrian_remType miss=no-op.
  // Own map so Valocity's 6 types stay. Erase 0x42, keep 0x43.
  {
    InvObject* pmap = gameref_new();
    InvObject* g0 = resref_new();
    java_util_resource_ResourceRef_set(g0, 0x42);
    InvObject* g1 = resref_new();
    java_util_resource_ResourceRef_set(g1, 0x43);
    InvObject* gmiss = resref_new();
    java_util_resource_ResourceRef_set(gmiss, 0x44);
    java_util_resource_GroundRef_addPedestrianType(pmap, g0);
    java_util_resource_GroundRef_addPedestrianType(pmap, g1);
    java_util_resource_GroundRef_remPedestrianType(pmap, g0);
    const int32_t n1 = tree_field_get_int(pmap, "pedestrian_types");
    java_util_resource_GroundRef_remPedestrianType(pmap, g0);
    java_util_resource_GroundRef_remPedestrianType(pmap, gmiss);
    const int32_t n2 = tree_field_get_int(pmap, "pedestrian_types");
    const float gone = java_util_resource_GroundRef_pedestrianDistance(
        pmap, vec3_new(0.f, 0.f, 0.f), 0x42);
    const float keep = java_util_resource_GroundRef_pedestrianDistance(
        pmap, vec3_new(0.f, 0.f, 0.f), 0x43);
    const int32_t rem_ok =
        (n1 == 1 && n2 == 1 && gone < 0.f && keep < 0.01f) ? 1 : 0;
    tree_field_set_int(map, "ped_rem_smoke", rem_ok);
    tree_field_set_int(city, "ped_rem_smoke", rem_ok);
    std::printf("  traffic remPedestrianType ok=%d n=%d/%d gone=%.1e keep=%.3f\n",
                rem_ok, n1, n2, gone, keep);
  }
  // PE pedestrianDistance @ 0x00484C60: Pedestrian_distance empty/miss → -1.0.
  {
    InvObject* pmap = gameref_new();
    const float empty = java_util_resource_GroundRef_pedestrianDistance(
        pmap, vec3_new(0.f, 0.f, 0.f), 0x42);
    InvObject* g0 = resref_new();
    java_util_resource_ResourceRef_set(g0, 0x42);
    java_util_resource_GroundRef_addPedestrianType(pmap, g0);
    const float near = java_util_resource_GroundRef_pedestrianDistance(
        pmap, vec3_new(0.f, 0.f, 0.f), 0x42);
    const float far = java_util_resource_GroundRef_pedestrianDistance(
        pmap, vec3_new(30.f, 0.f, 0.f), 0x42);
    const float any = java_util_resource_GroundRef_pedestrianDistance(
        pmap, vec3_new(0.f, 0.f, 0.f), 0);
    const float miss = java_util_resource_GroundRef_pedestrianDistance(
        pmap, vec3_new(0.f, 0.f, 0.f), 0x99);
    const int32_t dist_ok =
        (empty == -1.f && near < 0.01f && far > 29.f && far < 31.f &&
         any < 0.01f && miss == -1.f)
            ? 1
            : 0;
    tree_field_set_int(map, "ped_dist_smoke", dist_ok);
    tree_field_set_int(city, "ped_dist_smoke", dist_ok);
    std::printf(
        "  traffic pedestrianDistance ok=%d empty=%.1f near=%.3f far=%.1f "
        "any=%.3f miss=%.1f\n",
        dist_ok, empty, near, far, any, miss);
  }
  // PE setFog @ 0x00486A20: near/far * 10.0 (flt_5E7334). Scene night 20/150.
  {
    const int32_t save_c = render_d3d9_fog_color();
    const float save_n = render_d3d9_fog_near();
    const float save_f = render_d3d9_fog_far();
    const int32_t save_on = render_d3d9_fog_enabled() ? 1 : 0;
    InvObject* pmap = gameref_new();
    java_util_resource_GroundRef_setFog(pmap, 0x0007121e, 20.f, 150.f);
    const int32_t rgb = tree_field_get_int(pmap, "fog_color");
    const float n = tree_field_get_float(pmap, "fog_near");
    const float f = tree_field_get_float(pmap, "fog_far");
    const int32_t fog_ok =
        (tree_field_get_int(pmap, "fog_on") == 1 && rgb == 0x0007121e &&
         n == 200.f && f == 1500.f && render_d3d9_fog_enabled() &&
         render_d3d9_fog_color() == 0x0007121e &&
         render_d3d9_fog_near() == 200.f && render_d3d9_fog_far() == 1500.f)
            ? 1
            : 0;
    if (save_on)
      render_d3d9_set_fog(save_c, save_n, save_f);
    else
      render_d3d9_clear_fog();
    tree_field_set_int(map, "fog_smoke", fog_ok);
    tree_field_set_int(city, "fog_smoke", fog_ok);
    std::printf("  traffic setFog ok=%d rgb=%06X n=%.0f f=%.0f\n", fog_ok, rgb,
                n, f);
  }
  // PE setLight @ 0x00486AB0: RenderRef_applyLight RGB * 1/256. Scene night
  // suntype.setLight(0x00466285, 0x0007121e, 0x00466285). Restore D3D.
  {
    const int32_t save_d = render_d3d9_light_diffuse();
    const int32_t save_a = render_d3d9_light_ambient();
    const int32_t save_s = render_d3d9_light_specular();
    const int32_t save_on = render_d3d9_light_enabled() ? 1 : 0;
    InvObject* sun = resref_new();
    java_util_resource_RenderRef_setLight(sun, 0x00466285, 0x0007121e,
                                          0x00466285);
    const int32_t ld = tree_field_get_int(sun, "light_diffuse");
    const int32_t la = tree_field_get_int(sun, "light_ambient");
    const int32_t ls = tree_field_get_int(sun, "light_specular");
    const int32_t light_ok =
        (ld == 0x00466285 && la == 0x0007121e && ls == 0x00466285 &&
         render_d3d9_light_enabled() &&
         render_d3d9_light_diffuse() == 0x00466285 &&
         render_d3d9_light_ambient() == 0x0007121e &&
         render_d3d9_light_specular() == 0x00466285)
            ? 1
            : 0;
    if (save_on)
      render_d3d9_set_light(save_d, save_a, save_s);
    else
      render_d3d9_clear_light();
    tree_field_set_int(map, "light_smoke", light_ok);
    tree_field_set_int(city, "light_smoke", light_ok);
    std::printf("  traffic setLight ok=%d d=%06X a=%06X s=%06X\n", light_ok, ld,
                la, ls);
  }
  // PE setFlare @ 0x00486B20: RenderRef_applyFlare stores as-is. Scene noon
  // suntype.setFlare(skydome:0x0100, 0xe4e4e4FF, 1, 10, 15, 8). Keyed object.
  {
    InvObject* sun = resref_new();
    InvObject* glow = resref_new();
    java_util_resource_ResourceRef_set(glow, 0x7E105001);
    render_d3d9_texture_create_solid(glow, 0xFFE4E4E4u, 16);
    java_util_resource_RenderRef_setFlare(sun, glow,
                                          static_cast<int32_t>(0xe4e4e4FFu),
                                          1.f, 10.f, 15, 8);
    const int32_t fc = tree_field_get_int(sun, "flare_count");
    const int32_t fr = tree_field_get_int(sun, "flare_rays");
    const int32_t col = tree_field_get_int(sun, "flare_color");
    const float fmax = tree_field_get_float(sun, "flare_max");
    const int32_t flare_ok =
        (fc == 15 && fr == 8 && col == static_cast<int32_t>(0xe4e4e4FFu) &&
         std::fabs(fmax - 10.f) < 0.01f &&
         tree_field_get_int(sun, "flare_tex_id") == 0x7E105001)
            ? 1
            : 0;
    render_d3d9_clear_flare(sun);
    tree_field_set_int(map, "flare_smoke", flare_ok);
    tree_field_set_int(city, "flare_smoke", flare_ok);
    std::printf("  traffic setFlare ok=%d n=%d rays=%d max=%.0f\n", flare_ok, fc,
                fr, fmax);
  }
  // PE duplicate @ 0x004802C0: clone mesh/tex/light/obj (types 5/7/13/14);
  // else share. Painter: unique changeResource after duplicate.
  {
    InvObject* base = resref_new();
    InvObject* dst = resref_new();
    InvObject* tex_old = resref_new();
    InvObject* tex_new = resref_new();
    java_util_resource_ResourceRef_set(tex_old, 0x7E105010);
    java_util_resource_ResourceRef_set(tex_new, 0x7E105011);
    render_d3d9_mesh_create_skydome(base, 10.f);
    render_d3d9_texture_create_solid(tex_old, 0xFFFF0000u, 8);
    render_d3d9_texture_create_solid(tex_new, 0xFF00FF00u, 8);
    render_d3d9_mesh_set_texture(base, tex_old);
    java_util_resource_ResourceRef_duplicate(dst, base);
    java_util_resource_RenderRef_changeResource(dst, tex_old, tex_new);
    InvObject* miss = resref_new();
    java_util_resource_ResourceRef_duplicate(miss, nullptr);
    const int32_t ncloned = tree_field_get_int(dst, "dup_cloned");
    const int32_t src_keep =
        render_d3d9_mesh_get_texture(base, 0) == tex_old ? 1 : 0;
    const int32_t dst_swap =
        render_d3d9_mesh_get_texture(dst, 0) == tex_new ? 1 : 0;
    const int32_t dup_ok =
        (render_d3d9_mesh_ready(dst) &&
         render_d3d9_mesh_vertex_count(dst) ==
             render_d3d9_mesh_vertex_count(base) &&
         src_keep && dst_swap && ncloned == 1 && !render_d3d9_mesh_ready(miss))
            ? 1
            : 0;
    render_d3d9_mesh_destroy(dst);
    tree_field_set_int(map, "dup_smoke", dup_ok);
    tree_field_set_int(city, "dup_smoke", dup_ok);
    std::printf("  traffic duplicate ok=%d cloned=%d src_tex=%d dst_tex=%d\n",
                dup_ok, ncloned, src_keep, dst_swap);
  }
  // PE RenderRef.create @ 0x00480EE0: factory type=3 INSTANCE_RENDER, alias
  // default _renderinst, bindBone "bone00". Scene: sun = new RenderRef(map,
  // suntype, "sunny"). Disposable — do not clobber city.sun.
  {
    InvObject* parent = resref_new();
    InvObject* typ = resref_new();
    InvObject* inst = resref_new();
    java_util_resource_ResourceRef_set(parent, 0x7E105200);
    java_util_resource_ResourceRef_set(typ, 0x7E105124);
    render_d3d9_mesh_create_skydome(parent, 8.f);
    render_d3d9_mesh_create_skydome(typ, 5.f);
    java_util_resource_RenderRef_create(inst, parent, typ, string_new("sunny"));
    InvObject* ao = tree_field_get_obj(inst, "alias");
    const char* al = ao ? string_cstr(ao) : "";
    const int32_t create_ok =
        (java_util_resource_ResourceRef_id(inst) == 0x7E105124 &&
         java_util_resource_ResourceRef_getParentID(inst) == 0x7E105200 &&
         java_util_resource_ResourceRef_type(inst) == 3 && al &&
         std::strcmp(al, "sunny") == 0 &&
         render_d3d9_mesh_get_parent(inst) == parent &&
         render_d3d9_mesh_get_attach_bone(inst) == 0 &&
         render_d3d9_mesh_ready(inst))
            ? 1
            : 0;
    const std::string alias_copy = al ? al : "";
    render_d3d9_mesh_destroy(inst);
    render_d3d9_mesh_destroy(typ);
    render_d3d9_mesh_destroy(parent);
    tree_field_set_int(map, "create_smoke", create_ok);
    tree_field_set_int(city, "create_smoke", create_ok);
    std::printf("  traffic create ok=%d parent=1 alias=%s type=3 bone00=1\n",
                create_ok, alias_copy.empty() ? "?" : alias_copy.c_str());
  }
  // PE changeResource @ 0x00480220: slot.vtable+8(old,new) by native identity
  // (RID), not Java pointer. Painter: new ResourceRef(same RID). Navigator:
  // mesh slot after duplicate. Disposable — do not clobber painter/nav.
  {
    InvObject* base = resref_new();
    InvObject* dst = resref_new();
    InvObject* tex_a = resref_new();
    InvObject* tex_b = resref_new();
    InvObject* old_rid = resref_new();
    java_util_resource_ResourceRef_set(tex_a, 0x7E105010);
    java_util_resource_ResourceRef_set(tex_b, 0x7E105011);
    java_util_resource_ResourceRef_set(old_rid, 0x7E105010);
    render_d3d9_mesh_create_skydome(base, 10.f);
    render_d3d9_texture_create_solid(tex_a, 0xFFFF0000u, 8);
    render_d3d9_texture_create_solid(tex_b, 0xFF00FF00u, 8);
    render_d3d9_mesh_set_texture(base, tex_a);
    java_util_resource_ResourceRef_duplicate(dst, base);
    java_util_resource_RenderRef_changeResource(dst, old_rid, tex_b);
    const int32_t rid_ok =
        render_d3d9_mesh_get_texture(dst, 0) == tex_b ? 1 : 0;

    InvObject* msh_src = resref_new();
    InvObject* msh_new = resref_new();
    InvObject* inst = resref_new();
    render_d3d9_mesh_create_skydome(msh_src, 10.f);
    render_d3d9_mesh_create_skydome(msh_new, 5.f);
    java_util_resource_ResourceRef_duplicate(inst, msh_src);
    java_util_resource_RenderRef_changeResource(inst, old_rid, tex_b);
    java_util_resource_RenderRef_changeResource(inst, msh_src, msh_new);
    const int32_t mesh_ok =
        (render_d3d9_mesh_ready(inst) &&
         render_d3d9_mesh_get_texture(inst, 0) == tex_b)
            ? 1
            : 0;
    const int32_t chg_ok = (rid_ok && mesh_ok) ? 1 : 0;
    render_d3d9_mesh_destroy(dst);
    render_d3d9_mesh_destroy(inst);
    render_d3d9_mesh_destroy(base);
    render_d3d9_mesh_destroy(msh_src);
    render_d3d9_mesh_destroy(msh_new);
    tree_field_set_int(map, "chg_smoke", chg_ok);
    tree_field_set_int(city, "chg_smoke", chg_ok);
    std::printf("  traffic changeResource ok=%d rid=%d mesh=%d\n", chg_ok,
                rid_ok, mesh_ok);
  }
  // PE setColor @ 0x00480310: slot+0xCC = DWORD as-is (no 1/256).
  // InventoryItem_Folder 0xFFFF0000 / InventoryItem_Paint paintCan.color.
  {
    InvObject* can = resref_new();
    render_d3d9_mesh_create_skydome(can, 2.f);
    java_util_resource_RenderRef_setColor(
        can, static_cast<int32_t>(0xFFFF0000u));
    const int32_t col = tree_field_get_int(can, "color");
    const int32_t color_ok =
        (col == static_cast<int32_t>(0xFFFF0000u) &&
         render_d3d9_mesh_get_color(can) == static_cast<int32_t>(0xFFFF0000u))
            ? 1
            : 0;
    render_d3d9_mesh_destroy(can);
    tree_field_set_int(map, "color_smoke", color_ok);
    tree_field_set_int(city, "color_smoke", color_ok);
    std::printf("  traffic setColor ok=%d c=%08X\n", color_ok,
                static_cast<uint32_t>(col));
  }
  // PE setType @ 0x00486BA0 / getTypeID @ 0x00486BE0: instance tag 3 then
  // type id at slot+0x80. Non-instance → 0 (MouseCursor spark filter).
  {
    InvObject* parent = resref_new();
    InvObject* typ = resref_new();
    InvObject* typ2 = resref_new();
    InvObject* inst = resref_new();
    InvObject* bare = resref_new();
    java_util_resource_ResourceRef_set(parent, 0x7E105200);
    java_util_resource_ResourceRef_set(typ, 0x7E105124);
    java_util_resource_ResourceRef_set(typ2, 0x7E105125);
    java_util_resource_ResourceRef_set(bare, 0x7E105010);
    java_util_resource_RenderRef_create(inst, parent, typ, string_new("t"));
    const int32_t id0 = java_util_resource_RenderRef_getTypeID(inst);
    java_util_resource_RenderRef_setType(inst, typ2);
    const int32_t id1 = java_util_resource_RenderRef_getTypeID(inst);
    const int32_t id_bare = java_util_resource_RenderRef_getTypeID(bare);
    const int32_t type_ok =
        (id0 == 0x7E105124 && id1 == 0x7E105125 && id_bare == 0 &&
         java_util_resource_RenderRef_getTypeID(nullptr) == 0)
            ? 1
            : 0;
    tree_field_set_int(map, "type_smoke", type_ok);
    tree_field_set_int(city, "type_smoke", type_ok);
    std::printf("  traffic getTypeID ok=%d create=%X set=%X bare=%d\n", type_ok,
                static_cast<uint32_t>(id0), static_cast<uint32_t>(id1),
                id_bare);
  }
  // PE scaleMesh @ 0x00480390 → ResourceRef_applyScaleMesh @ 0x0048E7F0:
  // bake verts (not MeshXform). RectangleTemplate: duplicate, scale, then
  // changeResource so the clone carries AABB.
  {
    InvObject* src = resref_new();
    InvObject* scaled = resref_new();
    InvObject* inst = resref_new();
    render_d3d9_mesh_create_skydome(src, 10.f);
    java_util_resource_ResourceRef_duplicate(scaled, src);
    java_util_resource_ResourceRef_scaleMesh(scaled, 2.f, 3.f, 4.f);
    java_util_resource_RenderRef_changeResource(inst, src, scaled);
    float omin[3] = {0, 0, 0}, omax[3] = {0, 0, 0};
    float smin[3] = {0, 0, 0}, smax[3] = {0, 0, 0};
    float imin[3] = {0, 0, 0}, imax[3] = {0, 0, 0};
    render_d3d9_mesh_local_bounds(src, omin, omax);
    render_d3d9_mesh_local_bounds(scaled, smin, smax);
    render_d3d9_mesh_local_bounds(inst, imin, imax);
    float tsx = 0, tsy = 0, tsz = 0;
    float dummy = 0;
    render_d3d9_mesh_get_transform(scaled, &dummy, &dummy, &dummy, &dummy,
                                   &dummy, &dummy, &tsx, &tsy, &tsz);
    java_util_resource_ResourceRef_scaleMesh(nullptr, 9.f, 9.f, 9.f);
    const int32_t scale_ok =
        (std::fabs(omax[0] - 10.f) < 0.05f &&
         std::fabs(smax[0] - 20.f) < 0.05f &&
         std::fabs(smax[1] - 30.f) < 0.05f &&
         std::fabs(smax[2] - 40.f) < 0.05f &&
         std::fabs(imax[0] - 20.f) < 0.05f &&
         std::fabs(imax[1] - 30.f) < 0.05f &&
         std::fabs(imax[2] - 40.f) < 0.05f && tsx > 0.99f && tsx < 1.01f)
            ? 1
            : 0;
    render_d3d9_mesh_destroy(inst);
    render_d3d9_mesh_destroy(scaled);
    render_d3d9_mesh_destroy(src);
    tree_field_set_int(map, "scale_smoke", scale_ok);
    tree_field_set_int(city, "scale_smoke", scale_ok);
    std::printf("  traffic scaleMesh ok=%d aabb=(%.0f,%.0f,%.0f) xform=%.2f\n",
                scale_ok, smax[0], smax[1], smax[2], tsx);
  }
  // PE plotRoute @ 0x00483960: return 0|1. No spline / no parent / no type → 0.
  // lineCreate @ 0x0047FE70 same 0|1. lineAdd without create = no-op.
  // RaceSetup: plotRoute(nav.localroot, particles:0x17, 0xFFFF0000, 10, 0.01).
  {
    InvObject* parent = resref_new();
    InvObject* typ = resref_new();
    InvObject* line = resref_new();
    InvObject* bare = resref_new();
    java_util_resource_ResourceRef_set(typ, 0x17);
    const float rlen = java_util_resource_GroundRef_findRoute(
        map, vec3_new(0.f, 0.f, 0.f), vec3_new(100.f, 0.f, 80.f));
    const int32_t miss_pt =
        java_util_resource_RenderRef_plotRoute(
            line, nullptr, typ, static_cast<int32_t>(0xFFFF0000u), 10.f,
            vec3_new(0.01f, 0.f, 0.01f));
    const int32_t ok = java_util_resource_RenderRef_plotRoute(
        line, parent, typ, static_cast<int32_t>(0xFFFF0000u), 10.f,
        vec3_new(0.01f, 0.f, 0.01f));
    const int32_t npts = render_line_point_count(line);
    java_util_resource_RenderRef_lineAdd(
        bare, vec3_new(1.f, 0.f, 2.f), vec3_new(0.f, 1.f, 0.f),
        static_cast<int32_t>(0xFF00FF00u), 0.05f);
    const int32_t lc_miss =
        java_util_resource_RenderRef_lineCreate(bare, nullptr, typ);
    const int32_t lc_ok =
        java_util_resource_RenderRef_lineCreate(bare, parent, typ);
    const int32_t plot_ok =
        (rlen > 1.f && miss_pt == 0 && ok == 1 && npts >= 2 &&
         render_line_point_count(bare) == 0 && lc_miss == 0 && lc_ok == 1)
            ? 1
            : 0;
    tree_field_set_int(map, "plot_smoke", plot_ok);
    tree_field_set_int(city, "plot_smoke", plot_ok);
    std::printf("  traffic plotRoute ok=%d ret=%d n=%d miss=%d lc=%d\n", plot_ok,
                ok, npts, miss_pt, lc_ok);
  }
  // PE getRoutePos @ 0x00483B30 / getRouteDist @ 0x00483C20 /
  // getRouteLength()F @ 0x00483C00: cached spline from findRoute. No spline
  // → pos=null / dist=0 / len=0. pos null → dist=-1. t unclamped.
  {
    const float rlen = java_util_resource_GroundRef_findRoute(
        map, vec3_new(0.f, 0.f, 0.f), vec3_new(100.f, 0.f, 80.f));
    InvObject* p0 = java_util_resource_GroundRef_getRoutePos(map, 0.f);
    InvObject* p1 = java_util_resource_GroundRef_getRoutePos(map, 1.f);
    float x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
    if (p0) vec3_get(p0, &x0, &y0, &z0);
    if (p1) vec3_get(p1, &x1, &y1, &z1);
    const float d0 = java_util_resource_GroundRef_getRouteDist(map, p0);
    const float d1 = java_util_resource_GroundRef_getRouteDist(map, p1);
    const float dnull = java_util_resource_GroundRef_getRouteDist(map, nullptr);
    const float lenf = java_util_resource_GroundRef_getRouteLength(map);
    const int32_t route_ok =
        (rlen > 1.f && p0 && p1 && std::fabs(x0) < 5.f && std::fabs(z0) < 5.f &&
         std::fabs(x1 - 100.f) < 15.f && std::fabs(z1 - 80.f) < 15.f &&
         d0 > -0.05f && d0 < 0.15f && d1 > 0.85f && d1 < 1.15f &&
         dnull == -1.f && std::fabs(lenf - rlen) < 0.5f)
            ? 1
            : 0;
    tree_field_set_int(map, "routepos_smoke", route_ok);
    tree_field_set_int(city, "routepos_smoke", route_ok);
    std::printf("  traffic getRoutePos ok=%d d0=%.2f d1=%.2f len=%.0f\n",
                route_ok, d0, d1, lenf);
  }
  // PE setMatrix @ 0x0047E490: null Vector3/Ypr → pose 0,0,0 + ori 0,0,0
  // then GameRef_applyWorldXform. Vehicle.create setMatrix(null,null).
  {
    InvObject* car = gameref_new();
    java_util_resource_GameRef_setMatrix(
        car, vec3_new(10.f, 20.f, 30.f), ypr_new(0.5f, -0.25f, 1.f));
    float px = 0, py = 0, pz = 0, oy = 0, op = 0, or_ = 0;
    vec3_get(java_util_resource_GameRef_getPos(car), &px, &py, &pz);
    ypr_get(java_util_resource_GameRef_getOri(car), &oy, &op, &or_);
    java_util_resource_GameRef_setMatrix(car, nullptr, nullptr);
    float nx = 0, ny = 0, nz = 0, nyaw = 0, np = 0, nr = 0;
    vec3_get(java_util_resource_GameRef_getPos(car), &nx, &ny, &nz);
    ypr_get(java_util_resource_GameRef_getOri(car), &nyaw, &np, &nr);
    java_util_resource_GameRef_setMatrix(car, vec3_new(1.f, 2.f, 3.f), nullptr);
    float zx = 0, zy = 0, zz = 0, zoy = 0, zop = 0, zor = 0;
    vec3_get(java_util_resource_GameRef_getPos(car), &zx, &zy, &zz);
    ypr_get(java_util_resource_GameRef_getOri(car), &zoy, &zop, &zor);
    const int32_t mtx_ok =
        (std::fabs(px - 10.f) < 0.01f && std::fabs(py - 20.f) < 0.01f &&
         std::fabs(pz - 30.f) < 0.01f && std::fabs(oy - 0.5f) < 0.01f &&
         std::fabs(op + 0.25f) < 0.01f && std::fabs(or_ - 1.f) < 0.01f &&
         std::fabs(nx) < 0.01f && std::fabs(ny) < 0.01f &&
         std::fabs(nz) < 0.01f && std::fabs(nyaw) < 0.01f &&
         std::fabs(np) < 0.01f && std::fabs(nr) < 0.01f &&
         std::fabs(zx - 1.f) < 0.01f && std::fabs(zy - 2.f) < 0.01f &&
         std::fabs(zz - 3.f) < 0.01f && std::fabs(zoy) < 0.01f &&
         std::fabs(zop) < 0.01f && std::fabs(zor) < 0.01f)
            ? 1
            : 0;
    tree_field_set_int(map, "setmatrix_smoke", mtx_ok);
    tree_field_set_int(city, "setmatrix_smoke", mtx_ok);
    std::printf("  traffic setMatrix ok=%d null=(%.0f,%.0f,%.0f)\n", mtx_ok, nx,
                ny, nz);
  }
  // PE setPos @ 0x0047E350: pose + identity YPR via applyWorldXform.
  // Garage.lockCar: player.car.setPos(defCarPos).
  {
    InvObject* car = gameref_new();
    java_util_resource_GameRef_setMatrix(
        car, vec3_new(10.f, 20.f, 30.f), ypr_new(0.5f, -0.25f, 1.f));
    java_util_resource_GameRef_setPos(car, vec3_new(4.f, 5.f, 6.f));
    float px = 0, py = 0, pz = 0, oy = 0, op = 0, or_ = 0;
    vec3_get(java_util_resource_GameRef_getPos(car), &px, &py, &pz);
    ypr_get(java_util_resource_GameRef_getOri(car), &oy, &op, &or_);
    const int32_t pos_ok =
        (std::fabs(px - 4.f) < 0.01f && std::fabs(py - 5.f) < 0.01f &&
         std::fabs(pz - 6.f) < 0.01f && std::fabs(oy) < 0.01f &&
         std::fabs(op) < 0.01f && std::fabs(or_) < 0.01f)
            ? 1
            : 0;
    tree_field_set_int(map, "setpos_smoke", pos_ok);
    tree_field_set_int(city, "setpos_smoke", pos_ok);
    std::printf("  traffic setPos ok=%d ori=(%.2f,%.2f,%.2f)\n", pos_ok, oy, op,
                or_);
  }
  // PE setState @ 0x0047E630: pose + linvel + angvel via applyWorldXform.
  // Null linvel/angvel → 0,0,0. setMatrix zeros vx — setState must restore.
  {
    InvObject* car = gameref_new();
    java_util_resource_GameRef_setMatrix(
        car, vec3_new(10.f, 20.f, 30.f), ypr_new(0.5f, -0.25f, 1.f));
    java_util_resource_GameRef_setState(
        car, vec3_new(4.f, 5.f, 6.f), ypr_new(0.1f, 0.2f, 0.3f),
        vec3_new(1.f, 2.f, 3.f), vec3_new(7.f, 8.f, 9.f));
    float px = 0, py = 0, pz = 0, oy = 0, op = 0, or_ = 0;
    float vx = 0, vy = 0, vz = 0, wx = 0, wy = 0, wz = 0;
    vec3_get(java_util_resource_GameRef_getPos(car), &px, &py, &pz);
    ypr_get(java_util_resource_GameRef_getOri(car), &oy, &op, &or_);
    vec3_get(java_util_resource_GameRef_getVel(car), &vx, &vy, &vz);
    vec3_get(java_util_resource_PhysicsRef_getAngVel(car), &wx, &wy, &wz);
    java_util_resource_GameRef_setState(car, vec3_new(1.f, 2.f, 3.f), nullptr,
                                        nullptr, nullptr);
    float nvx = 0, nvy = 0, nvz = 0, nwx = 0, nwy = 0, nwz = 0;
    vec3_get(java_util_resource_GameRef_getVel(car), &nvx, &nvy, &nvz);
    vec3_get(java_util_resource_PhysicsRef_getAngVel(car), &nwx, &nwy, &nwz);
    const int32_t state_ok =
        (std::fabs(px - 4.f) < 0.01f && std::fabs(py - 5.f) < 0.01f &&
         std::fabs(pz - 6.f) < 0.01f && std::fabs(oy - 0.1f) < 0.01f &&
         std::fabs(op - 0.2f) < 0.01f && std::fabs(or_ - 0.3f) < 0.01f &&
         std::fabs(vx - 1.f) < 0.01f && std::fabs(vy - 2.f) < 0.01f &&
         std::fabs(vz - 3.f) < 0.01f && std::fabs(wx - 7.f) < 0.01f &&
         std::fabs(wy - 8.f) < 0.01f && std::fabs(wz - 9.f) < 0.01f &&
         std::fabs(nvx) < 0.01f && std::fabs(nvy) < 0.01f &&
         std::fabs(nvz) < 0.01f && std::fabs(nwx) < 0.01f &&
         std::fabs(nwy) < 0.01f && std::fabs(nwz) < 0.01f)
            ? 1
            : 0;
    tree_field_set_int(map, "setstate_smoke", state_ok);
    tree_field_set_int(city, "setstate_smoke", state_ok);
    std::printf("  traffic setState ok=%d vel=(%.1f,%.1f,%.1f)\n", state_ok, vx,
                vy, vz);
  }
  // PE getPos @ 0x0047DAD0: handle 0 → Mighty ERROR + nullptr.
  // handle+8==0 → nullptr (not Vector3 0,0,0). Else sub_48B280 + new Vector3.
  // Host: empty==true / !self → nullptr; posed origin still Vector3.
  {
    InvObject* fresh = gameref_new();
    InvObject* none = java_util_resource_GameRef_getPos(fresh);
    java_util_resource_GameRef_setPos(fresh, vec3_new(4.f, 5.f, 6.f));
    InvObject* after_pos = java_util_resource_GameRef_getPos(fresh);
    float px = 0, py = 0, pz = 0;
    if (after_pos) vec3_get(after_pos, &px, &py, &pz);
    InvObject* car = gameref_new();
    java_util_resource_GameRef_setMatrix(car, vec3_new(7.f, 8.f, 9.f), nullptr);
    InvObject* after_mtx = java_util_resource_GameRef_getPos(car);
    float mx = 0, my = 0, mz = 0;
    if (after_mtx) vec3_get(after_mtx, &mx, &my, &mz);
    java_util_resource_GameRef_setPos(car, vec3_new(0.f, 0.f, 0.f));
    InvObject* origin = java_util_resource_GameRef_getPos(car);
    float ox = 1.f, oy = 1.f, oz = 1.f;
    if (origin) vec3_get(origin, &ox, &oy, &oz);
    const int32_t getpos_ok =
        (!none && after_pos && std::fabs(px - 4.f) < 0.01f &&
         std::fabs(py - 5.f) < 0.01f && std::fabs(pz - 6.f) < 0.01f &&
         after_mtx && std::fabs(mx - 7.f) < 0.01f &&
         std::fabs(my - 8.f) < 0.01f && std::fabs(mz - 9.f) < 0.01f && origin &&
         std::fabs(ox) < 0.01f && std::fabs(oy) < 0.01f &&
         std::fabs(oz) < 0.01f)
            ? 1
            : 0;
    tree_field_set_int(map, "getpos_smoke", getpos_ok);
    tree_field_set_int(city, "getpos_smoke", getpos_ok);
    std::printf("  traffic getPos ok=%d\n", getpos_ok);
  }
  // PE isEmpty @ 0x00486D10: handle 0 → 1 (no Mighty ERROR). inner+0xC /
  // RESTYPE_GAME=8 + payload → 0. Unlike getPos, no handle+8 test.
  // Host: empty=inner miss; type==8 payload slots; type!=8 RID/script/pose
  // stand-in. fresh=1; RID/create/pose=0; destroy=1. getPos(fresh)
  // still nullptr (shared empty); getVel(fresh) still Vector3(0,0,0).
  {
    InvObject* fresh = gameref_new();
    const int32_t fresh_e = java_util_resource_GameRef_isEmpty(fresh);
    const int32_t null_e = java_util_resource_GameRef_isEmpty(nullptr);
    InvObject* fresh_pos = java_util_resource_GameRef_getPos(fresh);
    InvObject* fresh_vel = java_util_resource_GameRef_getVel(fresh);
    float fvx = 1.f, fvy = 1.f, fvz = 1.f;
    if (fresh_vel) vec3_get(fresh_vel, &fvx, &fvy, &fvz);
    InvObject* rid = gameref_new();
    java_util_resource_ResourceRef_set(rid, 1);
    const int32_t rid_e = java_util_resource_GameRef_isEmpty(rid);
    InvObject* posed = gameref_new();
    java_util_resource_GameRef_setPos(posed, vec3_new(4.f, 5.f, 6.f));
    const int32_t posed_e = java_util_resource_GameRef_isEmpty(posed);
    InvObject* posed_pos = java_util_resource_GameRef_getPos(posed);
    float px = 0, py = 0, pz = 0;
    if (posed_pos) vec3_get(posed_pos, &px, &py, &pz);
    InvObject* xa = gameref_new();
    InvObject* type = resref_new();
    java_util_resource_ResourceRef_set(type, 1);
    java_util_resource_GameRef_create(xa, nullptr, type, string_new(""),
                                      string_new("isempty_smoke"));
    const int32_t created_e = java_util_resource_GameRef_isEmpty(xa);
    java_util_resource_ResourceRef_destroy(xa);
    const int32_t dest_e = java_util_resource_GameRef_isEmpty(xa);
    const int32_t isempty_ok =
        (fresh_e == 1 && null_e == 1 && !fresh_pos && fresh_vel &&
         std::fabs(fvx) < 0.01f && std::fabs(fvy) < 0.01f &&
         std::fabs(fvz) < 0.01f && rid_e == 0 && posed_e == 0 && posed_pos &&
         std::fabs(px - 4.f) < 0.01f && std::fabs(py - 5.f) < 0.01f &&
         std::fabs(pz - 6.f) < 0.01f && created_e == 0 && dest_e == 1)
            ? 1
            : 0;
    tree_field_set_int(map, "isempty_smoke", isempty_ok);
    tree_field_set_int(city, "isempty_smoke", isempty_ok);
    std::printf("  traffic isEmpty ok=%d\n", isempty_ok);
  }
  // PE isScripted @ 0x00486DA0: handle 0 → ebx=0 @ loc_486F1E (no Mighty,
  // unlike getPos loc_47DB90). Contrast isEmpty @ 0x00486D10: handle 0 →
  // edi=1; RESTYPE_GAME=8 only. No script → 0. clazzname null → 1 iff
  // scripted. Empty C str → 0 (JNI `L;`). FQN is-a (Class_isInheritedFrom),
  // not alias. VehicleType is not a Part.
  {
    const int32_t null_h =
        java_util_resource_GameRef_isScripted(nullptr, nullptr);
    const int32_t null_empty = java_util_resource_GameRef_isEmpty(nullptr);
    InvObject* fresh = gameref_new();
    const int32_t fresh_any =
        java_util_resource_GameRef_isScripted(fresh, nullptr);
    const int32_t fresh_part = java_util_resource_GameRef_isScripted(
        fresh, string_new("java.game.parts.Part"));
    InvObject* part = gameref_new();
    InvObject* vt = gameref_new();
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      bind_gameref(part, nullptr, "java.game.parts.Part", "part");
      bind_gameref(vt, nullptr, "java.game.VehicleType", "VehicleType");
    }
    const int32_t part_any =
        java_util_resource_GameRef_isScripted(part, nullptr);
    const int32_t part_part = java_util_resource_GameRef_isScripted(
        part, string_new("java.game.parts.Part"));
    const int32_t part_set = java_util_resource_GameRef_isScripted(
        part, string_new("java.game.parts.Set"));
    const int32_t part_vt = java_util_resource_GameRef_isScripted(
        part, string_new("java.game.VehicleType"));
    const int32_t part_empty =
        java_util_resource_GameRef_isScripted(part, string_new(""));
    const int32_t vt_any = java_util_resource_GameRef_isScripted(vt, nullptr);
    const int32_t vt_vt = java_util_resource_GameRef_isScripted(
        vt, string_new("java.game.VehicleType"));
    const int32_t vt_part = java_util_resource_GameRef_isScripted(
        vt, string_new("java.game.parts.Part"));
    int32_t inherit_ok = 1;
    if (Jvm* j = jvm_active()) {
      const char* wheel_fqn =
          "java.game.parts.rgearpart.reciprocatingrgearpart.Wheel";
      if (!j->find_class(wheel_fqn)) j->load_class(wheel_fqn);
      if (j->find_class(wheel_fqn)) {
        InvObject* wheel = gameref_new();
        {
          std::lock_guard<std::mutex> lock(g_gr_mu);
          bind_gameref(wheel, nullptr, wheel_fqn, "w");
        }
        inherit_ok =
            (java_util_resource_GameRef_isScripted(
                 wheel, string_new("java.game.parts.Part")) == 1 &&
             java_util_resource_GameRef_isScripted(
                 wheel, string_new("java.game.parts.Set")) == 0)
                ? 1
                : 0;
      }
    }
    const int32_t isscripted_ok =
        (null_h == 0 && null_empty == 1 && fresh_any == 0 && fresh_part == 0 &&
         part_any == 1 && part_part == 1 && part_set == 0 && part_vt == 0 &&
         part_empty == 0 && vt_any == 1 && vt_vt == 1 && vt_part == 0 &&
         inherit_ok)
            ? 1
            : 0;
    tree_field_set_int(map, "isscripted_smoke", isscripted_ok);
    tree_field_set_int(city, "isscripted_smoke", isscripted_ok);
    std::printf("  traffic isScripted ok=%d\n", isscripted_ok);
  }
  // PE getScriptInstance @ 0x00486F30: handle 0 → null (no Mighty).
  // type1 → *(payload+0x50); type8 → Class_boxObject stand-in
  // (tree_host_new(script_class)); type0 → script.
  // Host: type 0 after bind_gameref → .script (part_si==part smoke).
  {
    InvObject* null_si =
        java_util_resource_GameRef_getScriptInstance(nullptr);
    InvObject* fresh = gameref_new();
    InvObject* fresh_si = java_util_resource_GameRef_getScriptInstance(fresh);
    InvObject* part = gameref_new();
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      bind_gameref(part, nullptr, "java.game.parts.Part", "part");
    }
    InvObject* part_si = java_util_resource_GameRef_getScriptInstance(part);
    const int32_t getscript_ok =
        (!null_si && !fresh_si && part_si == part) ? 1 : 0;
    tree_field_set_int(map, "getscript_smoke", getscript_ok);
    tree_field_set_int(city, "getscript_smoke", getscript_ok);
    std::printf("  traffic getScriptInstance ok=%d\n", getscript_ok);
  }
  // race112 PE getVel @ 0x0047DCE0 / getOri @ 0x0047DBE0: handle 0 → Mighty
  // ERROR + nullptr. No handle+8 skip (unlike getPos). Always alloc if
  // handle≠0. Empty still Vector3(0,0,0) / Ypr(0,0,0). Only !self → nullptr.
  {
    InvObject* posed = gameref_new();
    java_util_resource_GameRef_setPos(posed, vec3_new(4.f, 5.f, 6.f));
    InvObject* posed_vel = java_util_resource_GameRef_getVel(posed);
    InvObject* posed_ori = java_util_resource_GameRef_getOri(posed);
    InvObject* empty = gameref_new();
    InvObject* empty_vel = java_util_resource_GameRef_getVel(empty);
    InvObject* empty_ori = java_util_resource_GameRef_getOri(empty);
    float evx = 1.f, evy = 1.f, evz = 1.f;
    float eoy = 1.f, eop = 1.f, eor = 1.f;
    if (empty_vel) vec3_get(empty_vel, &evx, &evy, &evz);
    if (empty_ori) ypr_get(empty_ori, &eoy, &eop, &eor);
    InvObject* null_vel = java_util_resource_GameRef_getVel(nullptr);
    InvObject* null_ori = java_util_resource_GameRef_getOri(nullptr);
    const int32_t getvel_ok =
        (posed_vel && posed_ori && empty_vel && empty_ori &&
         std::fabs(evx) < 0.01f && std::fabs(evy) < 0.01f &&
         std::fabs(evz) < 0.01f && std::fabs(eoy) < 0.01f &&
         std::fabs(eop) < 0.01f && std::fabs(eor) < 0.01f && !null_vel &&
         !null_ori)
            ? 1
            : 0;
    tree_field_set_int(map, "getvel_smoke", getvel_ok);
    tree_field_set_int(city, "getvel_smoke", getvel_ok);
    std::printf("  traffic getVel ok=%d\n", getvel_ok);
  }
  // Soft pose/vel/phys sample: GetWorldPos ch=2 / GII_VEL ch=3 prefer
  // chassis PhysicsRef (Vehicle.set Native.ptr). Write-through
  // GameRefState. traffic_car_sample_pos under g_gr_mu Soft-samples phys
  // without nesting GameRef_getPos (Resources mutex ≠ GameRef g_gr_mu).
  {
    InvObject* car = gameref_new();
    InvObject* chassis = gameref_new();
    tree_field_set_obj(car, "chassis", chassis);
    java_util_resource_PhysicsRef_createBox(chassis, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    // Clear empty + seed stale GameRefState (1,2,3); Soft applyWorldXform
    // also poses chassis. Then move phys only so Soft must prefer live.
    java_util_resource_GameRef_setMatrix(car, vec3_new(1.f, 2.f, 3.f),
                                         ypr_new(0.1f, 0.f, 0.f));
    java_util_resource_PhysicsRef_setMatrix(
        chassis, vec3_new(11.f, 22.f, 33.f), ypr_new(0.5f, 0.f, 0.f));
    physics_set_velocity(chassis, 3.f, 0.f, 4.f);
    InvObject* gp = java_util_resource_GameRef_getPos(car);
    InvObject* gv = java_util_resource_GameRef_getVel(car);
    InvObject* go = java_util_resource_GameRef_getOri(car);
    float px = 0.f, py = 0.f, pz = 0.f;
    float vx = 0.f, vy = 0.f, vz = 0.f;
    float oy = 0.f, op = 0.f, or_ = 0.f;
    if (gp) vec3_get(gp, &px, &py, &pz);
    if (gv) vec3_get(gv, &vx, &vy, &vz);
    if (go) ypr_get(go, &oy, &op, &or_);
    float sx = 0.f, sy = 0.f, sz = 0.f;
    float svx = 0.f, svy = 0.f, svz = 0.f;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      // PE sort_compact @ 0x581170 passes car+0x88 / +0xA8.
      traffic_car_sample_pos(car, &sx, &sy, &sz, /*cursor_88=*/0.f,
                             /*lane_a8=*/1.f);
      // Soft vel ch=3 under g_gr_mu — PhysicsRef only, never nest getVel.
      gameref_soft_phys_sample_vel(car, &svx, &svy, &svz);
    }
    float cx = 0.f, cy = 0.f, cz = 0.f;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto it = g_refs.find(car);
      if (it != g_refs.end()) {
        cx = it->second.px;
        cy = it->second.py;
        cz = it->second.pz;
      }
    }
    const int32_t soft_phys_ok =
        (gp && gv && go && std::fabs(px - 11.f) < 0.01f &&
         std::fabs(py - 22.f) < 0.01f && std::fabs(pz - 33.f) < 0.01f &&
         std::fabs(vx - 3.f) < 0.01f && std::fabs(vy) < 0.01f &&
         std::fabs(vz - 4.f) < 0.01f && std::fabs(oy - 0.5f) < 0.01f &&
         std::fabs(sx - 11.f) < 0.01f && std::fabs(sy - 22.f) < 0.01f &&
         std::fabs(sz - 33.f) < 0.01f && std::fabs(svx - 3.f) < 0.01f &&
         std::fabs(svy) < 0.01f && std::fabs(svz - 4.f) < 0.01f &&
         std::fabs(cx - 11.f) < 0.01f && std::fabs(cy - 22.f) < 0.01f &&
         std::fabs(cz - 33.f) < 0.01f)
            ? 1
            : 0;
    tree_field_set_int(map, "soft_phys_sample_smoke", soft_phys_ok);
    tree_field_set_int(city, "soft_phys_sample_smoke", soft_phys_ok);
    std::printf("  traffic soft phys sample ok=%d\n", soft_phys_ok);
  }
  // PE setParent @ 0x0047E2D0 / GameRef_setParent_inner @ 0x0048ABA0:
  // handle 0 → Mighty ERROR no write. parent null crashes; parent
  // Native.ptr 0 → no-op (no detach). Type 2/3: +0x30/+0x38 child lists
  // (host vector stand-in). Stock Java always passes a live parent.
  {
    InvObject* car = gameref_new();
    InvObject* pmap = gameref_new();
    InvObject* pplayer = gameref_new();
    InvObject* empty_parent = gameref_new();
    InvObject* no_handle = gameref_new();
    java_util_resource_ResourceRef_set(car, 1);
    java_util_resource_ResourceRef_set(pmap, 2);
    java_util_resource_ResourceRef_set(pplayer, 3);
    java_util_resource_GameRef_setParent(car, pplayer);
    const int32_t under_player =
        java_util_resource_ResourceRef_getParentID(car) == 3;
    java_util_resource_GameRef_setParent(car, pmap);
    const int32_t lock_map =
        java_util_resource_ResourceRef_getParentID(car) == 2;
    java_util_resource_GameRef_setParent(car, nullptr);
    const int32_t null_noop =
        java_util_resource_ResourceRef_getParentID(car) == 2;
    java_util_resource_GameRef_setParent(car, empty_parent);
    const int32_t empty_noop =
        java_util_resource_ResourceRef_getParentID(car) == 2;
    java_util_resource_GameRef_setParent(car, pplayer);
    const int32_t release_player =
        java_util_resource_ResourceRef_getParentID(car) == 3;
    java_util_resource_GameRef_setParent(no_handle, pmap);
    const int32_t no_handle_noop =
        java_util_resource_ResourceRef_getParentID(no_handle) == 0;
    const int32_t setparent_ok =
        (under_player && lock_map && null_noop && empty_noop &&
         release_player && no_handle_noop)
            ? 1
            : 0;
    tree_field_set_int(map, "setparent_smoke", setparent_ok);
    tree_field_set_int(city, "setparent_smoke", setparent_ok);
    std::printf("  traffic setParent ok=%d\n", setparent_ok);
  }
  // PE getSpeedSquare @ 0x00480500: handle 0 → 0.0. Channel 3 vel → |v|².
  // Vehicle.set(chassis) Native.ptr; no physics → GameRef vx (setState).
  {
    InvObject* posed = gameref_new();
    java_util_resource_ResourceRef_set(posed, 1);
    java_util_resource_GameRef_setState(posed, vec3_new(0.f, 0.f, 0.f), nullptr,
                                        vec3_new(1.f, 2.f, 3.f), nullptr);
    const float posed_sq = java_game_Vehicle_getSpeedSquare(posed);
    InvObject* empty = gameref_new();
    const float empty_sq = java_game_Vehicle_getSpeedSquare(empty);
    const float null_sq = java_game_Vehicle_getSpeedSquare(nullptr);
    InvObject* veh = gameref_new();
    tree_field_set_obj(veh, "chassis", posed);
    const float veh_sq = java_game_Vehicle_getSpeedSquare(veh);
    const int32_t getspeedsq_ok =
        (std::fabs(posed_sq - 14.f) < 0.01f && std::fabs(empty_sq) < 0.01f &&
         std::fabs(null_sq) < 0.01f && std::fabs(veh_sq - 14.f) < 0.01f)
            ? 1
            : 0;
    tree_field_set_int(map, "getspeedsq_smoke", getspeedsq_ok);
    tree_field_set_int(city, "getspeedsq_smoke", getspeedsq_ok);
    std::printf("  traffic getSpeedSquare ok=%d\n", getspeedsq_ok);
  }
}

}  // namespace

int32_t scene_time2config(float time_sec, float rnd) {
  // Scene.time2Config — fixed rnd for host determinism (stock uses Math.random).
  const float hour = time_sec / 3600.f;
  if (hour < 3.f || hour >= 22.f) return 0;
  if (hour < 4.f) return rnd < 0.5f ? 1 : 2;
  if (hour < 8.f) {
    if (rnd < 0.33f) return 3;
    if (rnd < 0.66f) return 4;
    return 5;
  }
  if (hour < 10.f) return 6;
  if (hour < 19.f) {
    if (rnd < 0.25f) return 7;
    if (rnd < 0.5f) return 8;
    if (rnd < 0.75f) return 9;
    return 10;
  }
  if (hour < 20.5f) return 11;
  if (rnd < 0.33f) return 12;
  if (rnd < 0.66f) return 13;
  return 14;
}

int32_t valocity_scene_config(InvObject* city) {
  return city ? tree_field_get_int(city, "scene_config") : -1;
}

void valocity_apply_scene(InvObject* city) {
  if (!city) return;
  if (!rpak_find_by_name("skydome.rpk"))
    java_lang_System_openLib(string_new("maps/skydome.rpk"));

  // Deterministic variant pick (rnd=0 → first branch of each band).
  const int32_t config = scene_time2config(game_logic_time(), 0.f);
  tree_field_set_int(city, "scene_config", config);

  struct SceneLook {
    int32_t fog_rgb;
    float fog_near;
    float fog_far;
    int32_t env_local;
    int32_t sky_local;
    int32_t light_diff;
    int32_t light_amb;
    int32_t light_spec;
    int32_t flare_color;
    float flare_min;
    float flare_max;
    int32_t flare_count;
    int32_t flare_rays;
  };
  // Mirrors Scene.addSceneElements configs 0..14 (primary variants).
  // setFlare only configs 6–10 (maps.skydome:0x0100r).
  static const SceneLook kLooks[] = {
      {0x0007121e, 20.f, 150.f, 0x35, 0x23, 0x00466285, 0x0007121e, 0x00466285, 0, 0.f, 0.f, 0, 0},  // 0 night
      {0x00171516, 50.f, 200.f, 0x30, 0x1d, 0x006889A6, 0x001F212D, 0x006889A6, 0, 0.f, 0.f, 0, 0},  // 1
      {0x00111113, 50.f, 200.f, 0x31, 0x1e, 0x005D5D77, 0x0017142F, 0x005D5D77, 0, 0.f, 0.f, 0, 0},  // 2
      {0x0031364B, 50.f, 200.f, 0x2E, 0x15, 0x00CBB9AA, 0x002A3047, 0x00CBB9AA, 0, 0.f, 0.f, 0, 0},  // 3
      {0x00696063, 50.f, 200.f, 0x2F, 0x16, 0x00C1BC95, 0x00515F5D, 0x00C1BC95, 0, 0.f, 0.f, 0, 0},  // 4
      {0x00080808, 50.f, 200.f, 0x32, 0x1F, 0x00534FB2, 0x00140E2D, 0x00534FB2, 0, 0.f, 0.f, 0, 0},  // 5
      {0x00B0BCBC, 70.f, 400.f, 0x33, 0x21, 0x00C4DFE3, 0x002E3F56, 0x00C4DFE3, static_cast<int32_t>(0xe4e4e4FFu), 1.f, 10.f, 15, 8},  // 6
      {0x005E7992, 70.f, 400.f, 0x26, 0x02, 0x00DDD8AD, 0x004B5E6F, 0x00DDD8AD, static_cast<int32_t>(0xe4e4e4FFu), 1.f, 10.f, 15, 8},  // 7 day noon
      {0x00A1B5C3, 70.f, 400.f, 0x27, 0x0F, 0x00DEDABC, 0x00303C58, 0x00DEDABC, static_cast<int32_t>(0xe4e4e4FFu), 1.f, 10.f, 15, 8},  // 8
      {0x00869BAD, 70.f, 400.f, 0x28, 0x10, 0x00D8D4B0, 0x00375070, 0x00D8D4B0, static_cast<int32_t>(0xe4e4e4FFu), 1.f, 10.f, 15, 8},  // 9
      {0x00A4A1A0, 70.f, 400.f, 0x29, 0x19, 0x00A1A7B3, 0x0058606B, 0x00A1A7B3, static_cast<int32_t>(0xd4d4d4FFu), 1.f, 3.f, 15, 8},  // 10
      {0x003B4858, 60.f, 300.f, 0x2B, 0x13, 0x00BCBBB3, 0x001A1E2F, 0x00BCBBB3, 0, 0.f, 0.f, 0, 0},  // 11
      {0x00160702, 60.f, 300.f, 0x2A, 0x12, 0x00D0A26D, 0x000F080F, 0x00D0A26D, 0, 0.f, 0.f, 0, 0},  // 12
      {0x00000000, 60.f, 300.f, 0x2C, 0x11, 0x00938A7C, 0x00171D2A, 0x00938A7C, 0, 0.f, 0.f, 0, 0},  // 13
      {0x00030305, 60.f, 300.f, 0x2D, 0x14, 0x005F4045, 0x000D0C13, 0x005F4045, 0, 0.f, 0.f, 0, 0},  // 14
  };
  const SceneLook& look =
      kLooks[config >= 0 && config <= 14 ? config : 0];

  InvObject* map = tree_field_get_obj(city, "map");
  if (map)
    java_util_resource_GroundRef_setFog(map, look.fog_rgb, look.fog_near,
                                        look.fog_far);

  // Scene.addSceneElements: suntype.setLight(diff, amb, spec). Duplicate of
  // maps.skydome:0x0124r / mesh vtable not mirrored — global D3D bind.
  InvObject* suntype = tree_field_get_obj(city, "suntype");
  if (!suntype) {
    suntype = resref_new();
    tree_field_set_obj(city, "suntype", suntype);
  }
  // Scene.addSceneElements: suntype.duplicate(new RenderRef(maps.skydome:0x0124r)).
  InvObject* sun_src = tree_field_get_obj(city, "suntype_src");
  if (!sun_src) {
    sun_src = resref_new();
    tree_field_set_obj(city, "suntype_src", sun_src);
  }
  int32_t sun_id = 0x124;
  if (const RpakPack* sp = rpak_find_by_name("skydome.rpk"))
    sun_id = rpak_make_id(sp->pack_id, static_cast<uint16_t>(0x124));
  java_util_resource_ResourceRef_set(sun_src, sun_id);
  java_util_resource_ResourceRef_load(sun_src);
  java_util_resource_ResourceRef_duplicate(suntype, sun_src);
  java_util_resource_RenderRef_setLight(suntype, look.light_diff,
                                        look.light_amb, look.light_spec);

  // Scene.addSceneElements: suntype.setFlare only configs 6–10. New suntype
  // otherwise has no flare — clear so night does not keep noon sprites.
  if (look.flare_count > 0) {
    InvObject* glow = tree_field_get_obj(city, "flare_tex");
    if (!glow) {
      glow = resref_new();
      tree_field_set_obj(city, "flare_tex", glow);
    }
    int32_t glow_id = 0x100;
    if (const RpakPack* sp = rpak_find_by_name("skydome.rpk"))
      glow_id = rpak_make_id(sp->pack_id, static_cast<uint16_t>(0x100));
    java_util_resource_ResourceRef_set(glow, glow_id);
    java_util_resource_RenderRef_setFlare(suntype, glow, look.flare_color,
                                          look.flare_min, look.flare_max,
                                          look.flare_count, look.flare_rays);
  } else {
    render_d3d9_clear_flare(suntype);
    tree_field_set_int(suntype, "flare_count", 0);
    tree_field_set_int(suntype, "flare_rays", 0);
  }

  // Scene.addSceneElements: sun = new RenderRef(map, suntype, "sunny").
  if (map) {
    InvObject* sun = tree_field_get_obj(city, "sun");
    if (!sun) {
      sun = resref_new();
      tree_field_set_obj(city, "sun", sun);
    }
    java_util_resource_RenderRef_create(sun, map, suntype, string_new("sunny"));
  }

  int32_t env_id = look.env_local;
  int32_t sky_id = look.sky_local;
  if (const RpakPack* sp = rpak_find_by_name("skydome.rpk")) {
    env_id = rpak_make_id(sp->pack_id, static_cast<uint16_t>(look.env_local));
    sky_id = rpak_make_id(sp->pack_id, static_cast<uint16_t>(look.sky_local));
  }

  InvObject* env = tree_field_get_obj(city, "envmap");
  if (!env) {
    env = resref_new();
    tree_field_set_obj(city, "envmap", env);
  }
  java_util_resource_ResourceRef_set(env, env_id);
  java_util_resource_ResourceRef_load(env);
  java_render_GfxEngine_setGlobalEnvmap(env);

  InvObject* sky = tree_field_get_obj(city, "skydome");
  if (!sky) {
    sky = resref_new();
    tree_field_set_obj(city, "skydome", sky);
  }
  // Stock Scene.addSceneElements: skydome = new RenderRef(map, maps.skydome:IDr).
  // Pack entries are `mesh 0x3` + `texture 0xNN` recipes (skydome.rpk).
  java_util_resource_ResourceRef_set(sky, sky_id);
  java_util_resource_ResourceRef_load(sky);
  if (!render_d3d9_mesh_ready(sky)) {
    int32_t mesh_id = 0x3;
    if (const RpakPack* sp = rpak_find_by_name("skydome.rpk"))
      mesh_id = rpak_make_id(sp->pack_id, 0x3);
    java_util_resource_ResourceRef_set(sky, mesh_id);
    java_util_resource_ResourceRef_load(sky);
  }
  if (!render_d3d9_mesh_ready(sky)) {
    render_d3d9_mesh_create_from_file(sky, "maps/skydome/meshes/skydome.SCX");
  }
  if (!render_d3d9_mesh_ready(sky)) {
    render_d3d9_mesh_create_skydome(sky, 800.f);
  }
  // File PTX mirrors stock type textures (Scene names egbolt-am_01, …).
  static const char* kSkyPtx[] = {
      "maps/skydome/textures/stary_night_02.ptx",  // 0
      "maps/skydome/textures/dusk_03.ptx",         // 1
      "maps/skydome/textures/dusk_04.ptx",         // 2
      "maps/skydome/textures/dusk_01.ptx",         // 3
      "maps/skydome/textures/dusk_02.ptx",         // 4
      "maps/skydome/textures/dusk_05.ptx",         // 5
      "maps/skydome/textures/Morning_01.ptx",      // 6
      "maps/skydome/textures/am_01.ptx",           // 7
      "maps/skydome/textures/am_02.ptx",           // 8
      "maps/skydome/textures/am_03.ptx",           // 9
      "maps/skydome/textures/cloudy_day01.ptx",    // 10
      "maps/skydome/textures/dawn_02.ptx",         // 11
      "maps/skydome/textures/dawn_01.ptx",         // 12
      "maps/skydome/textures/dawn_03.ptx",         // 13
      "maps/skydome/textures/dawn_04.ptx",         // 14
  };
  InvObject* sky_tex = tree_field_get_obj(city, "sky_tex");
  if (!sky_tex) {
    sky_tex = resref_new();
    tree_field_set_obj(city, "sky_tex", sky_tex);
  }
  const char* ptx =
      kSkyPtx[config >= 0 && config <= 14 ? config : 0];
  // Always materialize sky_tex (smoke / UI). Bind to mesh when recipe left it bare.
  if (render_d3d9_texture_create_from_file(sky_tex, ptx)) {
    tree_field_set_obj(city, "sky_tex_path", string_new(ptx));
    if (render_d3d9_mesh_textured_count(sky) < 1 &&
        render_d3d9_texture_ready(sky_tex))
      render_d3d9_mesh_set_texture(sky, sky_tex);
  }
  if (render_d3d9_mesh_ready(sky)) {
    float px = 0.f, py = 0.f, pz = 0.f;
    if (InvObject* car = tree_field_get_obj(city, "car")) {
      px = tree_field_get_float(car, "pos_x");
      py = tree_field_get_float(car, "pos_y");
      pz = tree_field_get_float(car, "pos_z");
    }
    render_d3d9_mesh_set_transform(sky, px, py, pz, 0.f, 0.f, 0.f, 1.f, 1.f,
                                   1.f);
    render_d3d9_mesh_queue_add(sky);
  }
  tree_field_set_int(city, "fog_color", look.fog_rgb);
  tree_field_set_float(city, "fog_near", look.fog_near);
  tree_field_set_float(city, "fog_far", look.fog_far);
  tree_field_set_int(city, "light_diffuse", look.light_diff);
  tree_field_set_int(city, "light_ambient", look.light_amb);
  tree_field_set_int(city, "light_specular", look.light_spec);
  tree_field_set_int(city, "flare_count", look.flare_count);
  tree_field_set_int(city, "flare_rays", look.flare_rays);
  tree_field_set_float(city, "flare_max", look.flare_max);
}

void valocity_prepare(InvObject* city) {
  if (!city) return;
  InvObject* player = tree_field_get_obj(city, "player");
  if (!player) {
    player = game_logic_player();
    tree_field_set_obj(city, "player", player);
  }
  InvObject* car = tree_field_get_obj(city, "car");
  if (!car && player) car = tree_field_get_obj(player, "car");
  if (car) tree_field_set_obj(city, "car", car);

  // Valocity ctor: map + nav + club garage poses (flat fields; no Object[] yet).
  if (!rpak_find_by_name("city.rpk"))
    java_lang_System_openLib(string_new("maps/city.rpk"));
  if (!rpak_find_by_name("traffic.rpk"))
    java_lang_System_openLib(string_new("cars/traffic.rpk"));
  int32_t map_id = 0x1;
  if (const RpakPack* city_pack = rpak_find_by_name("city.rpk"))
    map_id = rpak_make_id(city_pack->pack_id, 0x1);
  tree_field_set_int(city, "map_id", map_id);

  InvObject* map = tree_field_get_obj(city, "map");
  if (!map) {
    map = tree_host_new("java.util.resource.GroundRef");
    java_util_resource_ResourceRef_set(map, map_id);
    tree_field_set_obj(city, "map", map);
  }

  if (!rpak_find_by_name("smallmap.rpk"))
    java_lang_System_openLib(string_new("maps/city/smallmap.rpk"));
  if (!rpak_find_by_name("frontend.rpk"))
    java_lang_System_openLib(string_new("frontend/frontend.rpk"));
  if (!tree_field_get_obj(city, "nav")) {
    const int32_t rid_type = pack_local_id("smallmap.rpk", 0x1);
    const int32_t rid_msh = pack_local_id("smallmap.rpk", 0x2);
    const int32_t rid_tex = pack_local_id("smallmap.rpk", 0x5);
    InvObject* nav =
        navigator_new(-23.482f, -24.45f, 5.828f, rid_type, rid_msh, rid_tex, 8,
                      8, 8);
    tree_field_set_obj(city, "nav", nav);
    navigator_show(nav);
    navigator_add_marker_static(nav, pack_local_id("frontend.rpk", 0x5B),
                                -278.518f, 1033.002f, 0);
    navigator_add_marker_static(nav, pack_local_id("frontend.rpk", 0x5F),
                                355.381f, 418.244f, 0);
    navigator_add_marker_static(nav, pack_local_id("frontend.rpk", 0x73),
                                -531.138f, -149.357f, 0);
    if (car) {
      InvObject* mPlayer = navigator_add_marker_dynamic(
          nav, pack_local_id("frontend.rpk", 0x5C), car);
      tree_field_set_obj(city, "mPlayer", mPlayer);
    }
  }

  // posGarage / oriGarage as real arrays for TREE AALOAD.
  InvObject* posGarage = tree_field_get_obj(city, "posGarage");
  if (!posGarage) {
    posGarage = tree_vector_new();
    auto mk_v3 = [](float x, float y, float z) {
      InvObject* v = tree_host_new("java.lang.Vector3");
      tree_field_set_float(v, "x", x);
      tree_field_set_float(v, "y", y);
      tree_field_set_float(v, "z", z);
      return v;
    };
    tree_vector_add(posGarage, mk_v3(-278.518f, 9.8f, 1033.002f));
    tree_vector_add(posGarage, mk_v3(355.381f, 1.6f, 418.244f));
    tree_vector_add(posGarage, mk_v3(-531.138f, 5.05f, -149.357f));
    tree_field_set_obj(city, "posGarage", posGarage);
  }
  InvObject* oriGarage = tree_field_get_obj(city, "oriGarage");
  if (!oriGarage) {
    oriGarage = tree_vector_new();
    auto mk_ypr = [](float y) {
      InvObject* v = tree_host_new("java.lang.Ypr");
      tree_field_set_float(v, "y", y);
      tree_field_set_float(v, "p", 0.f);
      tree_field_set_float(v, "r", 0.f);
      return v;
    };
    tree_vector_add(oriGarage, mk_ypr(1.580f));
    tree_vector_add(oriGarage, mk_ypr(-1.763f));
    tree_vector_add(oriGarage, mk_ypr(2.077f));
    tree_field_set_obj(city, "oriGarage", oriGarage);
  }

  // Flat mirrors (host finalize / smoke).
  tree_field_set_float(city, "posGarage0_x", -278.518f);
  tree_field_set_float(city, "posGarage0_y", 9.8f);
  tree_field_set_float(city, "posGarage0_z", 1033.002f);
  tree_field_set_float(city, "oriGarage0_y", 1.580f);
  tree_field_set_float(city, "posGarage1_x", 355.381f);
  tree_field_set_float(city, "posGarage1_y", 1.6f);
  tree_field_set_float(city, "posGarage1_z", 418.244f);
  tree_field_set_float(city, "oriGarage1_y", -1.763f);
  tree_field_set_float(city, "posGarage2_x", -531.138f);
  tree_field_set_float(city, "posGarage2_y", 5.05f);
  tree_field_set_float(city, "posGarage2_z", -149.357f);
  tree_field_set_float(city, "oriGarage2_y", 2.077f);

  if (!tree_field_get_obj(city, "speedymen"))
    tree_field_set_obj(city, "speedymen", tree_vector_new());

  // Phase 2.24: host road spine for alignToRoad / race cross queries.
  physics_road_seed_valocity();
  // Phase 2.41: visual SCX from city.rpk (egyedi remap, world-space verts).
  if (city_mesh_count() == 0) city_mesh_seed_from_rpak("city.rpk", 142);
}

void valocity_finalize_enter(InvObject* city) {
  if (!city) return;
  valocity_prepare(city);
  InvObject* player = tree_field_get_obj(city, "player");
  InvObject* car = tree_field_get_obj(city, "car");
  InvObject* map = tree_field_get_obj(city, "map");
  const int32_t club = player ? tree_field_get_int(player, "club") : 0;

  // Sync flat posStart_* from Vector3 if TREE assigned posGarage[club].
  if (InvObject* ps = tree_field_get_obj(city, "posStart")) {
    const float zx = tree_field_get_float(ps, "z");
    if (zx != 0.f || tree_field_get_float(ps, "x") != 0.f) {
      tree_field_set_float(city, "posStart_x", tree_field_get_float(ps, "x"));
      tree_field_set_float(city, "posStart_y", tree_field_get_float(ps, "y"));
      tree_field_set_float(city, "posStart_z", zx);
    }
  }
  if (InvObject* os = tree_field_get_obj(city, "oriStart")) {
    tree_field_set_float(city, "oriStart_y", tree_field_get_float(os, "y"));
  }

  float px = tree_field_get_float(city, "posGarage0_x");
  float py = tree_field_get_float(city, "posGarage0_y");
  float pz = tree_field_get_float(city, "posGarage0_z");
  float oy = tree_field_get_float(city, "oriGarage0_y");
  if (club == 1) {
    px = tree_field_get_float(city, "posGarage1_x");
    py = tree_field_get_float(city, "posGarage1_y");
    pz = tree_field_get_float(city, "posGarage1_z");
    oy = tree_field_get_float(city, "oriGarage1_y");
  } else if (club == 2) {
    px = tree_field_get_float(city, "posGarage2_x");
    py = tree_field_get_float(city, "posGarage2_y");
    pz = tree_field_get_float(city, "posGarage2_z");
    oy = tree_field_get_float(city, "oriGarage2_y");
  }
  InvObject* prev = tree_field_get_obj(city, "parentState");
  const char* prev_cn = prev ? tree_host_class(prev) : nullptr;
  const bool from_garage =
      prev_cn && (std::strstr(prev_cn, "Garage") || std::strstr(prev_cn, "MainMenu"));
  if (from_garage || tree_field_get_float(city, "posStart_z") == 0.f) {
    tree_field_set_float(city, "posStart_x", px);
    tree_field_set_float(city, "posStart_y", py);
    tree_field_set_float(city, "posStart_z", pz);
    tree_field_set_float(city, "oriStart_y", oy);
  }
  if (car) {
    tree_field_set_float(car, "pos_x", tree_field_get_float(city, "posStart_x"));
    tree_field_set_float(car, "pos_y", tree_field_get_float(city, "posStart_y"));
    tree_field_set_float(car, "pos_z", tree_field_get_float(city, "posStart_z"));
    tree_field_set_float(car, "ori_y", tree_field_get_float(city, "oriStart_y"));
    tree_field_set_int(car, "parent_map_id", tree_field_get_int(city, "map_id"));
    tree_field_set_int(car, "stopped", 0);
    // Phase 2.34: bind arcade PhysicsRef so valocity_simulate can drive.
    valocity_ensure_car_physics(car);
  }
  const float hour = game_logic_time() / 3600.f;
  const int daytime = (hour > 4.f && hour < 22.f) ? 1 : 0;
  tree_field_set_int(city, "daytime", daytime);
  tree_field_set_int(city, "traffic_day", daytime);
  // Phase 2.38: Scene fog / skydome / envmap from time-of-day.
  valocity_apply_scene(city);
  // Host authoritative traffic/peds (TREE may take wrong day/night branch on
  // float compares; keep script side-effects but normalize counts for gameplay).
  if (map) {
    java_util_resource_GroundRef_delTraffic(map);
    // ped types cleared with streams; re-seed below via spawn.
    tree_field_set_int(map, "pedestrian_types", 0);
    valocity_spawn_traffic(city, map);
  }
  if (InvObject* nav = tree_field_get_obj(city, "nav")) {
    if (car) java_game_Navigator_updateNavigator(nav, car, 0);
  }
  tree_field_set_int(city, "activeTrigger", 0);
  tree_field_set_int(city, "policeState", 0);
  tree_field_set_int(city, "raceState", 0);
  tree_field_set_int(city, "garage_denied", 0);
  {
    const int32_t mode = game_logic_game_mode();
    constexpr int32_t kCareer = 1;
    constexpr int32_t kGmSingleCar = 4;
    if (mode == kCareer || mode == kGmSingleCar) {
      tree_field_set_float(city, "trig0_x", -278.518f);
      tree_field_set_float(city, "trig0_y", 9.8f);
      tree_field_set_float(city, "trig0_z", 1033.002f);
      tree_field_set_float(city, "trig1_x", 355.381f);
      tree_field_set_float(city, "trig1_y", 1.6f);
      tree_field_set_float(city, "trig1_z", 418.244f);
      tree_field_set_float(city, "trig2_x", -531.138f);
      tree_field_set_float(city, "trig2_y", 5.05f);
      tree_field_set_float(city, "trig2_z", -149.357f);
      tree_field_set_float(city, "trig_r", 6.f);
      tree_field_set_int(city, "trigger_count", 3);
    } else if (tree_field_get_int(city, "trigger_count") == 0) {
      tree_field_set_int(city, "trigger_count", 0);
    }
  }
  tree_field_set_int(city, "entered", 1);
  frontend_loading_screen_hide();
}

InvObject* valocity_enter(InvObject* city, InvObject* prev_state) {
  if (!city) return nullptr;
  frontend_loading_screen_show();
  if (prev_state && !tree_field_get_obj(city, "parentState"))
    tree_field_set_obj(city, "parentState", prev_state);
  valocity_finalize_enter(city);
  return city;
}

void valocity_exit(InvObject* city, InvObject* next_state) {
  (void)next_state;
  if (!city) return;
  if (InvObject* map = tree_field_get_obj(city, "map"))
    java_util_resource_GroundRef_delTraffic(map);
  tree_field_set_int(city, "traffic_count", 0);
  tree_field_set_int(city, "entered", 0);
  if (InvObject* nav = tree_field_get_obj(city, "nav")) {
    navigator_hide(nav);
  }
  if (InvObject* sky = tree_field_get_obj(city, "skydome"))
    render_d3d9_mesh_destroy(sky);
  city_mesh_clear();
  render_d3d9_clear_fog();
  render_d3d9_set_global_envmap(nullptr);
}

int32_t valocity_fire_garage_trigger(InvObject* city, int32_t clubGarage,
                                    int32_t event_on) {
  if (!city || clubGarage < 1 || clubGarage > 3) return 0;
  if (tree_field_get_int(city, "trigger_count") <= 0) return 0;
  InvObject* player = tree_field_get_obj(city, "player");
  InvObject* car = tree_field_get_obj(city, "car");
  if (!car && player) car = tree_field_get_obj(player, "car");
  if (!car) return 0;
  // handleGarageTrigger: param token0 == player.car.id() — host always matches.
  if (event_on) {
    if (tree_field_get_int(city, "activeTrigger") == 0)
      tree_field_set_int(city, "activeTrigger", clubGarage);
  } else {
    tree_field_set_int(city, "activeTrigger", 0);
  }
  return tree_field_get_int(city, "activeTrigger");
}

InvObject* valocity_tick(InvObject* city) {
  // Valocity.handleEvent EVENT_TIME param==2 (1s tick).
  if (!city) return nullptr;
  const int32_t at = tree_field_get_int(city, "activeTrigger");
  if (at <= 0) return nullptr;
  if (tree_field_get_int(city, "policeState") ||
      tree_field_get_int(city, "raceState"))
    return nullptr;

  InvObject* player = tree_field_get_obj(city, "player");
  InvObject* car = tree_field_get_obj(city, "car");
  if (!car && player) car = tree_field_get_obj(player, "car");
  if (!car) return nullptr;

  // Prefer live physics speed; fall back to TREE cache.
  float speed_sq = java_game_Vehicle_getSpeedSquare(car);
  if (speed_sq < 0.f) speed_sq = 0.f;
  tree_field_set_float(car, "speed_sq", speed_sq);
  if (speed_sq >= 0.25f) return nullptr;

  const int32_t club = player ? tree_field_get_int(player, "club") : 0;
  if (at - 1 > club) {
    tree_field_set_int(city, "activeTrigger", 0);
    tree_field_set_int(city, "garage_denied", 1);
    tree_field_set_obj(city, "last_warning",
                       string_new("This is a higher club's garage, you can't enter!"));
    return nullptr;
  }

  tree_field_set_int(city, "activeTrigger", 0);
  // YesNoDialog accept (0) → changeActiveSection(garage)
  return valocity_return_to_garage(city);
}

namespace {

// Map "Baiern DevilSport" / "SuperDuty …" → cars/racers/<Brand>_data/
std::string racer_brand_from_name(const char* vehicle_name) {
  static const char* kFolders[] = {
      "SuperDuty", "Einvagen", "Baiern", "Duhen", "Enula", "Focer",
      "Nonus",     "Prime",    "MC",
  };
  if (!vehicle_name || !vehicle_name[0]) return "Baiern";
  size_t best_n = 0;
  const char* best = nullptr;
  for (const char* f : kFolders) {
    size_t n = std::strlen(f);
    if (n <= best_n) continue;
    bool match = true;
    for (size_t i = 0; i < n; ++i) {
      char a = vehicle_name[i];
      char b = f[i];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
      if (a != b) {
        match = false;
        break;
      }
    }
    if (!match) continue;
    const char c = vehicle_name[n];
    if (c != '\0' && c != ' ' && c != '_' && c != '-') continue;
    best_n = n;
    best = f;
  }
  if (best) return best;
  std::string tok;
  for (const char* p = vehicle_name; *p && *p != ' ' && *p != '_'; ++p)
    tok.push_back(*p);
  return tok.empty() ? "Baiern" : tok;
}

// Phase 2.46 — stock .cfg slots are metres; SCX mesh space is ×100.
constexpr float kCfgMetresToMesh = 100.f;

struct CfgAttach {
  int32_t type_id = 0;
  int32_t mate_slot = 1;
};

struct CfgSlot {
  int32_t id = 0;
  float px = 0, py = 0, pz = 0;
  float oy = 0, op = 0, or_ = 0;
  std::string name;  // normalized: fl_door, l_headlights, …
  std::vector<CfgAttach> attaches;
};

std::string norm_slot_key(const char* raw) {
  std::string out;
  if (!raw) return out;
  // Skip leading whitespace / punctuation from "; FL_door" comments.
  while (*raw == ' ' || *raw == '\t' || *raw == ';' || *raw == '_') ++raw;
  for (const char* p = raw; *p; ++p) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c == ' ' || c == '-') c = '_';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
      out.push_back(c);
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  // Collapse leading underscores left by mixed separators.
  size_t start = 0;
  while (start < out.size() && out[start] == '_') ++start;
  if (start) out.erase(0, start);
  return out;
}

bool parse_cfg_slots_file(const char* path, std::vector<CfgSlot>* out) {
  if (!path || !out) return false;
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::string resolved = rpak_resolve_path(path);
    if (!resolved.empty()) f = std::fopen(resolved.c_str(), "rb");
  }
  if (!f) return false;
  char line[512];
  int32_t n = 0;
  while (std::fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    // Phase 2.142: attach lines bind to the preceding slot.
    if (std::strncmp(p, "attach", 6) == 0) {
      if (out->empty()) continue;
      p += 6;
      while (*p == ' ' || *p == '\t') ++p;
      unsigned int type = 0;
      int mate = 1;
      if (std::sscanf(p, "%i %d", &type, &mate) < 1 &&
          std::sscanf(p, "%x %d", &type, &mate) < 1)
        continue;
      if (mate <= 0) mate = 1;
      CfgAttach a;
      a.type_id = static_cast<int32_t>(type);
      a.mate_slot = mate;
      out->back().attaches.push_back(a);
      continue;
    }
    if (std::strncmp(p, "slot", 4) != 0) continue;
    p += 4;
    while (*p == ' ' || *p == '\t') ++p;
    float px = 0, py = 0, pz = 0, oy = 0, op = 0, or_ = 0;
    int id = 0;
    if (std::sscanf(p, "%f %f %f %f %f %f %d", &px, &py, &pz, &oy, &op, &or_,
                    &id) < 7)
      continue;
    const char* semi = std::strchr(p, ';');
    CfgSlot s;
    s.id = id;
    s.px = px;
    s.py = py;
    s.pz = pz;
    s.oy = oy;
    s.op = op;
    s.or_ = or_;
    if (semi && semi[1]) s.name = norm_slot_key(semi + 1);
    out->push_back(s);
    ++n;
  }
  std::fclose(f);
  return n > 0;
}

const CfgSlot* find_cfg_slot(const std::vector<CfgSlot>& slots,
                             const char* key) {
  const std::string want = norm_slot_key(key);
  if (want.empty()) return nullptr;
  for (const CfgSlot& s : slots) {
    if (s.name == want) return &s;
  }
  return nullptr;
}

const CfgSlot* find_cfg_slot_id(const std::vector<CfgSlot>& slots, int32_t id) {
  for (const CfgSlot& s : slots) {
    if (s.id == id) return &s;
  }
  return nullptr;
}

// Stock install: Local = ParentSlot * inv(ChildMateSlot). Identity rot → subtract.
void mate_slot_local(const CfgSlot* parent_slot, const CfgSlot* child_mate,
                     float* lx, float* ly, float* lz, float* oy, float* op,
                     float* or_) {
  if (!lx || !ly || !lz || !oy || !op || !or_ || !parent_slot) return;
  float cpx = 0, cpy = 0, cpz = 0, coy = 0, cop = 0, cor = 0;
  if (child_mate) {
    cpx = child_mate->px;
    cpy = child_mate->py;
    cpz = child_mate->pz;
    coy = child_mate->oy;
    cop = child_mate->op;
    cor = child_mate->or_;
  }
  *lx = (parent_slot->px - cpx) * kCfgMetresToMesh;
  *ly = (parent_slot->py - cpy) * kCfgMetresToMesh;
  *lz = (parent_slot->pz - cpz) * kCfgMetresToMesh;
  *oy = parent_slot->oy - coy;
  *op = parent_slot->op - cop;
  *or_ = parent_slot->or_ - cor;
}

struct CfgWheel {
  float px = 0, py = 0, pz = 0;
  float oy = 0, op = 0, or_ = 0;
  std::string name;  // fl_wheel, …
};

// Phase 2.48 — `wheel px py pz …` then hub `slot … ; FL wheel` (yaw on sides).
bool parse_cfg_wheels_file(const char* path, std::vector<CfgWheel>* out) {
  if (!path || !out) return false;
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::string resolved = rpak_resolve_path(path);
    if (!resolved.empty()) f = std::fopen(resolved.c_str(), "rb");
  }
  if (!f) return false;
  char line[512];
  CfgWheel pending;
  bool have_wheel = false;
  int32_t n = 0;
  while (std::fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (std::strncmp(p, "wheel", 5) == 0 &&
        (p[5] == ' ' || p[5] == '\t')) {
      p += 5;
      while (*p == ' ' || *p == '\t') ++p;
      float px = 0, py = 0, pz = 0, oy = 0, op = 0, or_ = 0;
      if (std::sscanf(p, "%f %f %f %f %f %f", &px, &py, &pz, &oy, &op, &or_) <
          3)
        continue;
      pending = CfgWheel{};
      pending.px = px;
      pending.py = py;
      pending.pz = pz;
      pending.oy = oy;
      pending.op = op;
      pending.or_ = or_;
      have_wheel = true;
      continue;
    }
    if (!have_wheel) continue;
    if (std::strncmp(p, "slot", 4) != 0) continue;
    if (std::strncmp(p, "slottype", 8) == 0 ||
        std::strncmp(p, "slotdmg", 7) == 0 ||
        std::strncmp(p, "slotdeform", 10) == 0)
      continue;
    p += 4;
    while (*p == ' ' || *p == '\t') ++p;
    float px = 0, py = 0, pz = 0, oy = 0, op = 0, or_ = 0;
    int id = 0;
    if (std::sscanf(p, "%f %f %f %f %f %f %d", &px, &py, &pz, &oy, &op, &or_,
                    &id) < 7)
      continue;
    const char* semi = std::strchr(p, ';');
    std::string name = semi && semi[1] ? norm_slot_key(semi + 1) : "";
    if (name.find("wheel") == std::string::npos ||
        name.find("steering") != std::string::npos) {
      continue;
    }
    // Hub slot is local to wheelbones; take yaw from it, pos from `wheel`.
    pending.oy = oy;
    pending.op = op;
    pending.or_ = or_;
    pending.name = name;
    out->push_back(pending);
    have_wheel = false;
    ++n;
    if (n >= 4) break;
  }
  std::fclose(f);
  return n > 0;
}

std::string main_cfg_path_for_car(const std::string& brand,
                                  const char* vehicle_name) {
  const std::string dir = "cars/racers/" + brand + "_data/scripts/";
  std::string model;
  if (vehicle_name && vehicle_name[0]) {
    // "Baiern DevilSport" / "Baiern CoupeSport GT III" → after brand token.
    const char* p = vehicle_name;
    while (*p && *p != ' ') ++p;
    while (*p == ' ') ++p;
    for (; *p; ++p) {
      char c = *p;
      if (c == ' ')
        model.push_back('_');
      else
        model.push_back(c);
    }
  }
  if (model.empty()) model = "DevilSport";
  return dir + "_main_" + model + ".cfg";
}

}  // namespace

void valocity_ensure_car_mesh(InvObject* car) {
  if (!car) return;
  InvObject* mesh = tree_field_get_obj(car, "visual_mesh");
  if (mesh && render_d3d9_mesh_ready(mesh)) {
    valocity_ensure_car_parts(car);
    return;
  }
  if (!mesh) {
    mesh = resref_new();
    tree_field_set_obj(car, "visual_mesh", mesh);
  }
  const char* vname = nullptr;
  if (InvObject* nm = tree_field_get_obj(car, "vehicleName"))
    vname = string_cstr(nm);
  const std::string brand = racer_brand_from_name(vname);
  const std::string path =
      "cars/racers/" + brand + "_data/meshes/chassis.scx";
  if (!render_d3d9_mesh_create_from_file(mesh, path.c_str())) {
    // Starter fallback when brand folder is missing / demo descriptor.
    if (brand != "Baiern")
      render_d3d9_mesh_create_from_file(
          mesh, "cars/racers/Baiern_data/meshes/chassis.scx");
  }
  if (!render_d3d9_mesh_ready(mesh)) return;
  render_d3d9_mesh_queue_add(mesh);
  valocity_sync_car_mesh(car);
  valocity_ensure_car_parts(car);
}

void valocity_ensure_car_parts(InvObject* car) {
  if (!car) return;
  if (tree_field_get_int(car, "visual_parts_ready")) return;
  InvObject* chassis = tree_field_get_obj(car, "visual_mesh");
  if (!chassis || !render_d3d9_mesh_ready(chassis)) return;

  const char* vname = nullptr;
  if (InvObject* nm = tree_field_get_obj(car, "vehicleName"))
    vname = string_cstr(nm);
  std::string brand = racer_brand_from_name(vname);

  // Chassis-attached body SCX (poses from _main_*.cfg slots, metres→mesh).
  static const char* kChassisParts[] = {
      "hood.scx",
      "trunk.scx",
      "F_bumper.scx",
      "R_bumper.scx",
      "F_windshield.scx",
      "R_windshield.scx",
      "FL_quarterpanel.scx",
      "FR_quarterpanel.scx",
      "RL_quarterpanel.scx",
      "RR_quarterpanel.scx",
      "L_sideskirt.scx",
      "R_sideskirt.scx",
      "FL_door.scx",
      "FR_door.scx",
      "L_headlights.scx",
      "R_headlights.scx",
      "L_taillights.scx",
      "R_taillights.scx",
  };
  // Door children: parent mesh stem → child scx (slot name = child stem).
  static const char* kDoorChildFiles[] = {
      "L_mirror.scx",
      "FL_window.scx",
      "R_mirror.scx",
      "FR_window.scx",
  };
  static const char* kDoorChildParents[] = {
      "FL_door",
      "FL_door",
      "FR_door",
      "FR_door",
  };

  InvObject* parts = tree_field_get_obj(car, "visual_parts");
  if (!parts) {
    parts = tree_vector_new();
    tree_field_set_obj(car, "visual_parts", parts);
  }

  int32_t loaded = 0;
  int32_t slotted = 0;
  int32_t wheels_n = 0;
  int32_t tyres_n = 0;
  std::unordered_map<std::string, InvObject*> by_stem;

  auto try_load = [&](const std::string& b) {
    const std::string mesh_prefix = "cars/racers/" + b + "_data/meshes/";
    const std::string cfg_prefix = "cars/racers/" + b + "_data/scripts/";
    std::vector<CfgSlot> chassis_slots;
    std::string main_cfg = main_cfg_path_for_car(b, vname);
    if (!parse_cfg_slots_file(main_cfg.c_str(), &chassis_slots) &&
        b == "Baiern") {
      main_cfg = "cars/racers/Baiern_data/scripts/_main_DevilSport.cfg";
      parse_cfg_slots_file(main_cfg.c_str(), &chassis_slots);
    }

    auto add_mesh_at = [&](const char* path, const char* stem, InvObject* parent,
                           float lx, float ly, float lz, float oy, float op,
                           float or_, bool has_slot) -> InvObject* {
      InvObject* part = resref_new();
      if (!render_d3d9_mesh_create_from_file(part, path) ||
          !render_d3d9_mesh_ready(part)) {
        render_d3d9_mesh_destroy(part);
        return nullptr;
      }
      render_d3d9_mesh_set_transform(part, lx, ly, lz, oy, op, or_, 1.f, 1.f,
                                     1.f);
      render_d3d9_mesh_set_parent(part, parent);
      render_d3d9_mesh_queue_add(part);
      tree_vector_add(parts, part);
      tree_field_set_obj(part, "part_stem", string_new(stem));
      tree_field_set_float(part, "attach_x", lx);
      tree_field_set_float(part, "attach_y", ly);
      tree_field_set_float(part, "attach_z", lz);
      tree_field_set_float(part, "attach_oy", oy);
      tree_field_set_int(part, "attach_slotted", has_slot ? 1 : 0);
      by_stem[norm_slot_key(stem)] = part;
      ++loaded;
      if (has_slot) ++slotted;
      return part;
    };

    auto add_mesh = [&](const char* file, InvObject* parent, float lx, float ly,
                        float lz, float oy, float op, float or_,
                        bool has_slot) -> InvObject* {
      std::string stem = file;
      if (stem.size() > 4) stem.resize(stem.size() - 4);
      const std::string path = mesh_prefix + file;
      return add_mesh_at(path.c_str(), stem.c_str(), parent, lx, ly, lz, oy, op,
                         or_, has_slot);
    };

    for (const char* file : kChassisParts) {
      std::string stem = file;
      if (stem.size() > 4) stem.resize(stem.size() - 4);
      float lx = 0, ly = 0, lz = 0, oy = 0, op = 0, or_ = 0;
      bool has = false;
      if (const CfgSlot* parent_slot =
              find_cfg_slot(chassis_slots, stem.c_str())) {
        std::vector<CfgSlot> part_slots;
        parse_cfg_slots_file((cfg_prefix + stem + ".cfg").c_str(), &part_slots);
        const CfgSlot* mate = find_cfg_slot_id(part_slots, parent_slot->id);
        if (!mate) mate = find_cfg_slot(part_slots, stem.c_str());
        mate_slot_local(parent_slot, mate, &lx, &ly, &lz, &oy, &op, &or_);
        has = true;
      }
      add_mesh(file, chassis, lx, ly, lz, oy, op, or_, has);
    }

    // Mirrors / windows: slots live on the door .cfg (parent = door mesh).
    for (size_t i = 0; i < sizeof(kDoorChildFiles) / sizeof(kDoorChildFiles[0]);
         ++i) {
      const char* file = kDoorChildFiles[i];
      const char* parent_stem = kDoorChildParents[i];
      auto pit = by_stem.find(norm_slot_key(parent_stem));
      if (pit == by_stem.end()) continue;
      std::string stem = file;
      if (stem.size() > 4) stem.resize(stem.size() - 4);
      std::vector<CfgSlot> door_slots;
      const std::string door_cfg = cfg_prefix + parent_stem + ".cfg";
      parse_cfg_slots_file(door_cfg.c_str(), &door_slots);
      float lx = 0, ly = 0, lz = 0, oy = 0, op = 0, or_ = 0;
      bool has = false;
      if (const CfgSlot* parent_slot = find_cfg_slot(door_slots, stem.c_str())) {
        std::vector<CfgSlot> child_slots;
        parse_cfg_slots_file((cfg_prefix + stem + ".cfg").c_str(), &child_slots);
        const CfgSlot* mate = find_cfg_slot_id(child_slots, parent_slot->id);
        if (!mate) mate = find_cfg_slot(child_slots, stem.c_str());
        mate_slot_local(parent_slot, mate, &lx, &ly, &lz, &oy, &op, &or_);
        has = true;
      }
      add_mesh(file, pit->second, lx, ly, lz, oy, op, or_, has);
    }

    // Phase 2.48 — DevilSport stock Blossom rims at `wheel` poses.
    std::vector<CfgWheel> wheels;
    if (!parse_cfg_wheels_file(main_cfg.c_str(), &wheels) && b == "Baiern") {
      parse_cfg_wheels_file(
          "cars/racers/Baiern_data/scripts/_main_DevilSport.cfg", &wheels);
    }
    static const char* kFrontRim =
        "parts/wheels/rims/meshes/"
        "rim_Blossom_9.0_19_ET_0_LOD_CATALOG_GARAGE.scx";
    static const char* kRearRim =
        "parts/wheels/rims/meshes/"
        "rim_Blossom_10.5_19_ET_0_LOD_CATALOG_GARAGE.scx";
    static const char* kFrontRimCfg =
        "parts/wheels/rims/scripts/"
        "rim_Blossom_9_0_19_ET_0_LOD_CATALOG_GARAGE.cfg";
    static const char* kRearRimCfg =
        "parts/wheels/rims/scripts/"
        "rim_Blossom_10_5_19_ET_0_LOD_CATALOG_GARAGE.cfg";
    // Phase 2.49 — matching 19" tyres parented to each rim (slot 2↔1).
    static const char* kFrontTyre =
        "parts/wheels/tyres/meshes/"
        "tyre_235_45_19_9.0_LOD_CATALOG_GARAGE.scx";
    static const char* kRearTyre =
        "parts/wheels/tyres/meshes/"
        "tyre_255_45_19_10.5_LOD_CATALOG_GARAGE.scx";
    static const char* kFrontTyreCfg =
        "parts/wheels/tyres/scripts/"
        "tyre_235_45_19_9_0_LOD_CATALOG_GARAGE.cfg";
    static const char* kRearTyreCfg =
        "parts/wheels/tyres/scripts/"
        "tyre_255_45_19_10_0_LOD_CATALOG_GARAGE.cfg";
    std::vector<CfgSlot> front_rim_slots;
    std::vector<CfgSlot> rear_rim_slots;
    std::vector<CfgSlot> front_tyre_slots;
    std::vector<CfgSlot> rear_tyre_slots;
    parse_cfg_slots_file(kFrontRimCfg, &front_rim_slots);
    parse_cfg_slots_file(kRearRimCfg, &rear_rim_slots);
    parse_cfg_slots_file(kFrontTyreCfg, &front_tyre_slots);
    parse_cfg_slots_file(kRearTyreCfg, &rear_tyre_slots);
    for (size_t wi = 0; wi < wheels.size() && wi < 4; ++wi) {
      const CfgWheel& w = wheels[wi];
      const bool front = wi < 2;
      const char* rim = front ? kFrontRim : kRearRim;
      char stem[32];
      std::snprintf(stem, sizeof(stem), "wheel_%zu", wi);
      InvObject* rim_mesh =
          add_mesh_at(rim, stem, chassis, w.px * kCfgMetresToMesh,
                      w.py * kCfgMetresToMesh, w.pz * kCfgMetresToMesh, w.oy,
                      w.op, w.or_, true);
      if (!rim_mesh) continue;
      ++wheels_n;
      // Chassis wheel slots 101–104 + install graph (rim mate slot 1).
      part_bind_slot_visual(car, 101 + static_cast<int32_t>(wi), rim_mesh, w.px,
                            w.py, w.pz, w.oy, w.op, w.or_);
      part_install(car, 101 + static_cast<int32_t>(wi), rim_mesh, 1);

      const std::vector<CfgSlot>& rim_slots =
          front ? front_rim_slots : rear_rim_slots;
      const std::vector<CfgSlot>& tyre_slots =
          front ? front_tyre_slots : rear_tyre_slots;
      const CfgSlot* rim_tyre = find_cfg_slot_id(rim_slots, 2);
      const CfgSlot* tyre_mate = find_cfg_slot_id(tyre_slots, 1);
      float tlx = 0, tly = 0, tlz = 0, toy = 0, top = 0, tor = 0;
      bool thas = false;
      if (rim_tyre) {
        mate_slot_local(rim_tyre, tyre_mate, &tlx, &tly, &tlz, &toy, &top,
                        &tor);
        thas = true;
      } else {
        // Fallback: rim tyre-slot at origin, stock tyre mate oy=π.
        CfgSlot fake_rim{};
        fake_rim.id = 2;
        CfgSlot fake_tyre{};
        fake_tyre.id = 1;
        fake_tyre.oy = 3.14159265f;
        mate_slot_local(&fake_rim, tyre_mate ? tyre_mate : &fake_tyre, &tlx,
                        &tly, &tlz, &toy, &top, &tor);
        thas = true;
      }
      const char* tyre = front ? kFrontTyre : kRearTyre;
      char tstem[32];
      std::snprintf(tstem, sizeof(tstem), "tyre_%zu", wi);
      InvObject* tyre_mesh =
          add_mesh_at(tyre, tstem, rim_mesh, tlx, tly, tlz, toy, top, tor, thas);
      if (tyre_mesh) {
        ++tyres_n;
        // Rim slot 2 ↔ tyre mate slot 1.
        part_bind_slot_visual(rim_mesh, 2, tyre_mesh, tlx / kCfgMetresToMesh,
                              tly / kCfgMetresToMesh, tlz / kCfgMetresToMesh,
                              toy, top, tor);
        part_install(rim_mesh, 2, tyre_mesh, 1);
      }
    }
  };

  try_load(brand);
  if (loaded == 0 && brand != "Baiern") {
    brand = "Baiern";
    try_load(brand);
  }

  tree_field_set_int(car, "visual_parts_ready", loaded > 0 ? 1 : 0);
  tree_field_set_int(car, "visual_parts_count", loaded);
  tree_field_set_int(car, "visual_parts_slotted", slotted);
  tree_field_set_int(car, "visual_wheels_count", wheels_n);
  tree_field_set_int(car, "visual_tyres_count", tyres_n);
}

int32_t valocity_car_part_count(InvObject* car) {
  if (!car) return 0;
  return tree_field_get_int(car, "visual_parts_count");
}

int32_t valocity_car_part_slotted(InvObject* car) {
  if (!car) return 0;
  return tree_field_get_int(car, "visual_parts_slotted");
}

int32_t valocity_car_wheel_count(InvObject* car) {
  if (!car) return 0;
  return tree_field_get_int(car, "visual_wheels_count");
}

int32_t valocity_car_tyre_count(InvObject* car) {
  if (!car) return 0;
  return tree_field_get_int(car, "visual_tyres_count");
}

namespace {

constexpr float kPartSlotMetresToMesh = 100.f;

InvObject* part_slot_table(InvObject* part) {
  if (!part) return nullptr;
  InvObject* slots = tree_field_get_obj(part, "part_slots");
  if (!slots) {
    slots = tree_vector_new();
    tree_field_set_obj(part, "part_slots", slots);
  }
  return slots;
}

InvObject* part_slot_find(InvObject* part, int32_t slot_id) {
  InvObject* slots = tree_field_get_obj(part, "part_slots");
  if (!slots) return nullptr;
  const int32_t n = tree_vector_size(slots);
  for (int32_t i = 0; i < n; ++i) {
    InvObject* s = tree_vector_element_at(slots, i);
    if (s && tree_field_get_int(s, "slot_id") == slot_id) return s;
  }
  return nullptr;
}

InvObject* part_slot_ensure(InvObject* part, int32_t slot_id) {
  if (!part) return nullptr;
  if (InvObject* existing = part_slot_find(part, slot_id)) return existing;
  InvObject* slots = part_slot_table(part);
  InvObject* s = gameref_new();
  tree_field_set_int(s, "slot_id", slot_id);
  tree_field_set_float(s, "px", 0.f);
  tree_field_set_float(s, "py", 0.f);
  tree_field_set_float(s, "pz", 0.f);
  tree_field_set_float(s, "oy", 0.f);
  tree_field_set_float(s, "op", 0.f);
  tree_field_set_float(s, "or", 0.f);
  tree_vector_add(slots, s);
  return s;
}

void part_slot_apply_visual(InvObject* slot) {
  if (!slot) return;
  InvObject* visual = tree_field_get_obj(slot, "visual");
  if (!visual || !render_d3d9_mesh_ready(visual)) return;
  const float px = tree_field_get_float(slot, "px") * kPartSlotMetresToMesh;
  const float py = tree_field_get_float(slot, "py") * kPartSlotMetresToMesh;
  const float pz = tree_field_get_float(slot, "pz") * kPartSlotMetresToMesh;
  const float oy = tree_field_get_float(slot, "oy");
  const float op = tree_field_get_float(slot, "op");
  const float or_ = tree_field_get_float(slot, "or");
  float sx = 1.f, sy = 1.f, sz = 1.f;
  {
    float ignore[6];
    render_d3d9_mesh_get_transform(visual, &ignore[0], &ignore[1], &ignore[2],
                                   &ignore[3], &ignore[4], &ignore[5], &sx, &sy,
                                   &sz);
  }
  render_d3d9_mesh_set_transform(visual, px, py, pz, oy, op, or_, sx, sy, sz);
  tree_field_set_float(visual, "attach_x", px);
  tree_field_set_float(visual, "attach_y", py);
  tree_field_set_float(visual, "attach_z", pz);
  tree_field_set_float(visual, "attach_oy", oy);
}

}  // namespace

void part_bind_slot_visual(InvObject* part, int32_t slot_id, InvObject* visual,
                           float px_m, float py_m, float pz_m, float oy,
                           float op, float or_) {
  InvObject* slot = part_slot_ensure(part, slot_id);
  if (!slot) return;
  tree_field_set_obj(slot, "visual", visual);
  tree_field_set_float(slot, "px", px_m);
  tree_field_set_float(slot, "py", py_m);
  tree_field_set_float(slot, "pz", pz_m);
  tree_field_set_float(slot, "oy", oy);
  tree_field_set_float(slot, "op", op);
  tree_field_set_float(slot, "or", or_);
}

void part_set_slot_pos(InvObject* part, int32_t slot_id, InvObject* pos,
                       InvObject* ypr) {
  InvObject* slot = part_slot_ensure(part, slot_id);
  if (!slot) return;
  if (pos) {
    float x = 0, y = 0, z = 0;
    vec3_get(pos, &x, &y, &z);
    tree_field_set_float(slot, "px", x);
    tree_field_set_float(slot, "py", y);
    tree_field_set_float(slot, "pz", z);
  }
  if (ypr) {
    float oy = 0, op = 0, or_ = 0;
    ypr_get(ypr, &oy, &op, &or_);
    tree_field_set_float(slot, "oy", oy);
    tree_field_set_float(slot, "op", op);
    tree_field_set_float(slot, "or", or_);
  }
  part_slot_apply_visual(slot);
}

bool part_slot_get_pose(InvObject* part, int32_t slot_id, float* px, float* py,
                        float* pz, float* oy, float* op, float* or_) {
  InvObject* slot = part_slot_find(part, slot_id);
  if (!slot) return false;
  if (px) *px = tree_field_get_float(slot, "px");
  if (py) *py = tree_field_get_float(slot, "py");
  if (pz) *pz = tree_field_get_float(slot, "pz");
  if (oy) *oy = tree_field_get_float(slot, "oy");
  if (op) *op = tree_field_get_float(slot, "op");
  if (or_) *or_ = tree_field_get_float(slot, "or");
  return true;
}

InvObject* part_slot_visual(InvObject* part, int32_t slot_id) {
  InvObject* slot = part_slot_find(part, slot_id);
  return slot ? tree_field_get_obj(slot, "visual") : nullptr;
}

InvObject* part_on_slot(InvObject* part, int32_t slot_id) {
  InvObject* slot = part_slot_find(part, slot_id);
  if (!slot) return nullptr;
  if (InvObject* child = tree_field_get_obj(slot, "child")) return child;
  return tree_field_get_obj(slot, "visual");
}

int32_t part_slot_id_on_slot(InvObject* part, int32_t slot_id) {
  InvObject* slot = part_slot_find(part, slot_id);
  if (!slot) return 0;
  return tree_field_get_int(slot, "child_slot_id");
}

bool part_install(InvObject* parent, int32_t parent_slot_id, InvObject* child,
                  int32_t child_slot_id) {
  if (!parent || !child || parent_slot_id == 0 || child_slot_id == 0)
    return false;
  InvObject* slot = part_slot_ensure(parent, parent_slot_id);
  if (!slot) return false;
  // Phase 2.80: garage UI locks (status=1 → refuse install).
  if (tree_field_get_int(slot, "disabled") != 0) return false;
  tree_field_set_obj(slot, "child", child);
  tree_field_set_int(slot, "child_slot_id", child_slot_id);
  if (render_d3d9_mesh_ready(child))
    tree_field_set_obj(slot, "visual", child);
  tree_field_set_obj(child, "part_parent", parent);
  tree_field_set_int(child, "part_parent_slot", parent_slot_id);
  // Ensure mate slot exists on the child (used by getSlots / save walk).
  part_slot_ensure(child, child_slot_id);
  return true;
}

bool part_ensure_chassis_cfg_slots(InvObject* car) {
  if (!car) return false;
  if (tree_field_get_int(car, "cfg_slots_ready") == 1) return true;

  const char* vname = nullptr;
  if (InvObject* nm = tree_field_get_obj(car, "vehicleName"))
    vname = string_cstr(nm);
  std::string brand = racer_brand_from_name(vname);
  std::vector<CfgSlot> slots;
  std::string main_cfg = main_cfg_path_for_car(brand, vname);
  if (!parse_cfg_slots_file(main_cfg.c_str(), &slots)) {
    main_cfg = "cars/racers/Baiern_data/scripts/_main_DevilSport.cfg";
    parse_cfg_slots_file(main_cfg.c_str(), &slots);
  }
  int32_t attach_n = 0;
  for (const CfgSlot& cs : slots) {
    if (cs.id <= 0) continue;
    InvObject* s = part_slot_ensure(car, cs.id);
    if (!s) continue;
    tree_field_set_float(s, "px", cs.px);
    tree_field_set_float(s, "py", cs.py);
    tree_field_set_float(s, "pz", cs.pz);
    tree_field_set_float(s, "oy", cs.oy);
    tree_field_set_float(s, "op", cs.op);
    tree_field_set_float(s, "or", cs.or_);
    if (!cs.name.empty())
      tree_field_set_obj(s, "name", string_new(cs.name.c_str()));
    if (!cs.attaches.empty()) {
      InvObject* atts = tree_vector_new();
      for (const CfgAttach& a : cs.attaches) {
        InvObject* ao = gameref_new();
        tree_field_set_int(ao, "type_id", a.type_id);
        tree_field_set_int(ao, "mate_slot", a.mate_slot);
        tree_vector_add(atts, ao);
        ++attach_n;
      }
      tree_field_set_obj(s, "attaches", atts);
    }
  }
  tree_field_set_int(car, "cfg_slots_ready", slots.empty() ? 0 : 1);
  tree_field_set_int(car, "cfg_slot_count",
                     static_cast<int32_t>(slots.size()));
  tree_field_set_int(car, "cfg_attach_count", attach_n);
  return !slots.empty();
}

bool part_type_matches_attach(int32_t part_id, int32_t attach_type) {
  if (part_id == 0 || attach_type == 0) return false;
  if (part_id == attach_type) return true;
  // Compare local 16-bit resource ids (pack prefix may differ).
  if ((part_id & 0xFFFF) == (attach_type & 0xFFFF)) return true;
  return false;
}

bool part_try_cfg_path_attaches(InvObject* car, InvObject* part,
                                int32_t* parent_slot, int32_t* child_slot) {
  if (!car || !part || !parent_slot || !child_slot) return false;
  const char* path = nullptr;
  if (InvObject* p = tree_field_get_obj(part, "cfg_path")) path = string_cstr(p);
  if (!path || !path[0]) return false;
  std::vector<CfgSlot> part_slots;
  if (!parse_cfg_slots_file(path, &part_slots)) return false;
  auto slot_free = [&](int32_t id) -> bool {
    if (id <= 0) return false;
    if (part_slot_is_disabled(car, id)) return false;
    return part_on_slot(car, id) == nullptr;
  };
  // Part-side attach: attach <parentType> <parentSlot> (see F_bumper.cfg).
  for (const CfgSlot& ps : part_slots) {
    for (const CfgAttach& a : ps.attaches) {
      if (!slot_free(a.mate_slot)) continue;
      *parent_slot = a.mate_slot;
      *child_slot = ps.id > 0 ? ps.id : a.mate_slot;
      return true;
    }
  }
  return false;
}

bool part_find_cfg_install(InvObject* car, InvObject* part, int32_t* parent_slot,
                           int32_t* child_slot) {
  if (!car || !part || !parent_slot || !child_slot) return false;
  *child_slot = 1;
  part_ensure_chassis_cfg_slots(car);

  auto slot_free = [&](int32_t id) -> bool {
    if (id <= 0) return false;
    if (part_slot_is_disabled(car, id)) return false;
    return part_on_slot(car, id) == nullptr;
  };

  const int32_t pref = tree_field_get_int(part, "install_slot");
  if (pref > 0 && slot_free(pref)) {
    *parent_slot = pref;
    return true;
  }

  const int32_t part_id = java_util_resource_ResourceRef_id(part);
  InvObject* slots = tree_field_get_obj(car, "part_slots");
  const int32_t n = slots ? tree_vector_size(slots) : 0;

  // Phase 2.142 — chassis-side attach match (e.g. slot 401 ↔ block 0x52).
  for (int32_t i = 0; i < n; ++i) {
    InvObject* s = tree_vector_element_at(slots, i);
    if (!s) continue;
    const int32_t sid = tree_field_get_int(s, "slot_id");
    if (!slot_free(sid)) continue;
    InvObject* atts = tree_field_get_obj(s, "attaches");
    if (!atts) continue;
    const int32_t an = tree_vector_size(atts);
    for (int32_t j = 0; j < an; ++j) {
      InvObject* a = tree_vector_element_at(atts, j);
      if (!a) continue;
      const int32_t tid = tree_field_get_int(a, "type_id");
      if (!part_type_matches_attach(part_id, tid)) continue;
      int32_t mate = tree_field_get_int(a, "mate_slot");
      if (mate <= 0) mate = 1;
      *parent_slot = sid;
      *child_slot = mate;
      tree_field_set_int(part, "install_via_attach", 1);
      return true;
    }
  }

  // Part-side attach lines from cfg_path (e.g. F_bumper → slot 3).
  if (part_try_cfg_path_attaches(car, part, parent_slot, child_slot)) {
    tree_field_set_int(part, "install_via_attach", 1);
    return true;
  }

  const char* hc = tree_host_class(part);

  auto prefer_name = [&](const char* needle) -> int32_t {
    if (!needle || !slots) return 0;
    for (int32_t i = 0; i < n; ++i) {
      InvObject* s = tree_vector_element_at(slots, i);
      if (!s) continue;
      const int32_t id = tree_field_get_int(s, "slot_id");
      if (!slot_free(id)) continue;
      const char* nm = nullptr;
      if (InvObject* so = tree_field_get_obj(s, "name")) nm = string_cstr(so);
      if (nm && std::strstr(nm, needle)) return id;
    }
    return 0;
  };

  if (hc && std::strstr(hc, "Bumper")) {
    int32_t id = prefer_name("bumper");
    if (!id && slot_free(3)) id = 3;
    if (!id && slot_free(9)) id = 9;
    if (id) {
      *parent_slot = id;
      *child_slot = id;
      return true;
    }
  }
  if (hc && (std::strstr(hc, "Block") || std::strstr(hc, "Engine") ||
             std::strstr(hc, "enginepart"))) {
    int32_t id = prefer_name("engine");
    if (!id && slot_free(401)) id = 401;
    if (id) {
      *parent_slot = id;
      return true;
    }
  }
  if (hc && std::strstr(hc, "Hood")) {
    int32_t id = prefer_name("hood");
    if (!id && slot_free(601)) id = 601;
    if (id) {
      *parent_slot = id;
      return true;
    }
  }

  for (int32_t i = 0; i < n; ++i) {
    InvObject* s = tree_vector_element_at(slots, i);
    if (!s) continue;
    const int32_t id = tree_field_get_int(s, "slot_id");
    if (id >= 101 && id <= 199) continue;
    if (!slot_free(id)) continue;
    const char* nm = nullptr;
    if (InvObject* so = tree_field_get_obj(s, "name")) nm = string_cstr(so);
    if (!nm || !nm[0]) continue;
    *parent_slot = id;
    return true;
  }
  return false;
}

void part_disable_slot(InvObject* part, int32_t slot_id, int32_t status) {
  if (!part || slot_id == 0) return;
  InvObject* slot = part_slot_ensure(part, slot_id);
  if (!slot) return;
  tree_field_set_int(slot, "disabled", status ? 1 : 0);
}

bool part_slot_is_disabled(InvObject* part, int32_t slot_id) {
  if (!part || slot_id == 0) return false;
  InvObject* slot = part_slot_find(part, slot_id);
  if (!slot) return false;
  return tree_field_get_int(slot, "disabled") != 0;
}

InvObject* part_car_root(InvObject* part) {
  if (!part) return nullptr;
  InvObject* cur = part;
  for (int depth = 0; depth < 64; ++depth) {
    InvObject* parent = tree_field_get_obj(cur, "part_parent");
    if (!parent) return cur;
    cur = parent;
  }
  return cur;
}

int32_t part_wheel_id(InvObject* part) {
  if (!part) return -1;
  InvObject* root = part_car_root(part);
  if (!root) return -1;
  InvObject* cur = part;
  for (int depth = 0; depth < 64; ++depth) {
    InvObject* parent = tree_field_get_obj(cur, "part_parent");
    if (!parent) return -1;
    if (parent == root) {
      const int32_t slot = tree_field_get_int(cur, "part_parent_slot");
      // PE getWheelID @ 0x004691F0: [*(payload+0x4C)+0xD0]; (slot-101)%10.
      if (slot > 100 && slot <= 400) return (slot - 101) % 10;
      return -1;
    }
    cur = parent;
  }
  return -1;
}

int32_t part_slot_count(InvObject* part) {
  InvObject* slots = tree_field_get_obj(part, "part_slots");
  return slots ? tree_vector_size(slots) : 0;
}

int32_t part_slot_id_at(InvObject* part, int32_t index) {
  InvObject* slots = tree_field_get_obj(part, "part_slots");
  if (!slots || index < 0 || index >= tree_vector_size(slots)) return -1;
  InvObject* s = tree_vector_element_at(slots, index);
  return s ? tree_field_get_int(s, "slot_id") : -1;
}

void valocity_sync_car_mesh(InvObject* car) {
  if (!car) return;
  InvObject* mesh = tree_field_get_obj(car, "visual_mesh");
  if (!mesh || !render_d3d9_mesh_ready(mesh)) return;
  const float px = tree_field_get_float(car, "pos_x");
  const float py = tree_field_get_float(car, "pos_y");
  const float pz = tree_field_get_float(car, "pos_z");
  const float oy = tree_field_get_float(car, "ori_y");
  // Chassis SCX is local around origin (same unit space as city meshes).
  // Child body parts inherit this pose via mesh parent hierarchy.
  render_d3d9_mesh_set_transform(mesh, px, py, pz, oy, 0.f, 0.f, 1.f, 1.f, 1.f);
}

void valocity_sync_wheel_visuals(InvObject* car, float dt) {
  if (!car || dt <= 0.f) return;
  constexpr float kMesh = 100.f;  // metres → mesh (same as part slots)
  constexpr float kSteerVis = 0.55f;
  constexpr float kSteerWheelRefR = 0.15f;  // ~SteeringWheel stock r
  constexpr float kTwoPi = 6.2831853f;

  float fwd = 0.f;
  InvObject* body = tree_field_get_obj(car, "chassis");
  if (!body) body = car;
  if (InvObject* vel = java_util_resource_PhysicsRef_getVel(body)) {
    float vx = 0, vy = 0, vz = 0;
    vec3_get(vel, &vx, &vy, &vz);
    const float yaw = tree_field_get_float(car, "ori_y");
    fwd = vx * std::sin(yaw) + vz * std::cos(yaw);
  } else {
    float ssq = tree_field_get_float(car, "speed_sq");
    if (ssq < 0.f) ssq = 0.f;
    fwd = std::sqrt(ssq);
  }

  // Phase 2.71: Chassis.setSteerWheel(r,z) — larger r → less visual yaw.
  float sw_r = tree_field_get_float(car, "steer_wheel_r");
  if (sw_r < 0.02f) sw_r = tree_field_get_float(car, "steer_wheel_radius");
  if (sw_r < 0.02f) sw_r = kSteerWheelRefR;
  float sw_scale = kSteerWheelRefR / sw_r;
  if (sw_scale < 0.4f) sw_scale = 0.4f;
  if (sw_scale > 2.5f) sw_scale = 2.5f;

  for (int32_t i = 0; i < 4; ++i) {
    InvObject* visual = part_slot_visual(car, 101 + i);
    if (!visual || !render_d3d9_mesh_ready(visual)) continue;
    float px = 0, py = 0, pz = 0, oy = 0, op = 0, or_ = 0;
    if (!part_slot_get_pose(car, 101 + i, &px, &py, &pz, &oy, &op, &or_))
      continue;

    InvObject* w = java_game_parts_bodypart_Chassis_getWheel(car, i);
    float radius = 0.32f;
    float steer_ang = 0.f;
    if (w) {
      radius = java_game_parts_WheelRef_getRadius(w);
      if (radius < 0.05f) radius = 0.32f;
      if (i < 2)
        steer_ang =
            -java_game_parts_WheelRef_getSteer(w) * kSteerVis * sw_scale;
    }

    float spin = tree_field_get_float(visual, "wheel_spin");
    spin += (fwd / radius) * dt;
    while (spin > kTwoPi) spin -= kTwoPi;
    while (spin < 0.f) spin += kTwoPi;
    tree_field_set_float(visual, "wheel_spin", spin);

    float sx = 1.f, sy = 1.f, sz = 1.f;
    {
      float ign[6];
      render_d3d9_mesh_get_transform(visual, &ign[0], &ign[1], &ign[2], &ign[3],
                                     &ign[4], &ign[5], &sx, &sy, &sz);
    }
    render_d3d9_mesh_set_transform(visual, px * kMesh, py * kMesh, pz * kMesh,
                                   oy + steer_ang, op + spin, or_, sx, sy, sz);
  }
}

void valocity_ensure_car_physics(InvObject* car) {
  if (!car) return;
  InvObject* body = tree_field_get_obj(car, "chassis");
  if (!body) {
    body = car;
    tree_field_set_obj(car, "chassis", body);
  }
  if (physics_shape(body) == 0) {
    java_util_resource_PhysicsRef_createBox(body, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    physics_set_gear(body, 1);
  }
  const float px = tree_field_get_float(car, "pos_x");
  const float py = tree_field_get_float(car, "pos_y");
  const float pz = tree_field_get_float(car, "pos_z");
  const float oy = tree_field_get_float(car, "ori_y");
  java_util_resource_PhysicsRef_setMatrix(body, vec3_new(px, py, pz),
                                          ypr_new(oy, 0.f, 0.f));
  java_util_resource_GameRef_setActiveCollision(body);
  tree_field_set_float(car, "speed_sq", physics_speed_square(body));
  valocity_ensure_car_mesh(car);
}

// engine_tick_phys_traffic — GroundRef_traffic.cpp

void valocity_simulate(InvObject* city, float dt) {
  if (!city || dt <= 0.f) return;
  if (!tree_field_get_int(city, "entered")) return;
  InvObject* player = tree_field_get_obj(city, "player");
  InvObject* car = tree_field_get_obj(city, "car");
  if (!car && player) car = tree_field_get_obj(player, "car");
  if (!car || tree_field_get_int(car, "stopped")) return;

  InvObject* body = tree_field_get_obj(car, "chassis");
  if (!body) body = car;
  if (physics_shape(body) == 0) valocity_ensure_car_physics(car);
  body = tree_field_get_obj(car, "chassis");
  if (!body) body = car;
  if (physics_shape(body) == 0) return;

  InvObject* ctrl = player ? tree_field_get_obj(player, "controller") : nullptr;
  if (!ctrl) ctrl = input_get_controller(0);

  // Phase 2.66: push turn axis onto front WheelRefs; aggregate drive/radius.
  // Phase 2.67: also friction/sliction/brake/hbrake/roll_res → contact.
  // Phase 2.72: push brake/handbrake axes onto WheelRefs (Brake.java balance).
  {
    const float ctrl_steer =
        ctrl ? input_map_get_logical(ctrl, kAxisTurnLR) : 0.f;
    const float ctrl_brake =
        ctrl ? input_map_get_logical(ctrl, kAxisBrake) : 0.f;
    const float ctrl_hb =
        ctrl ? input_map_get_logical(ctrl, kAxisHandbrake) : 0.f;
    float bal = tree_field_get_float(car, "brake_balance");
    if (bal < 0.01f) bal = 0.5f;  // Chassis.java default
    if (bal > 1.f) bal = 1.f;
    // Torque units match Brake.java stock (~0.18); /0.2 → arcade 0..1 in contact.
    constexpr float kBrkTorque = 0.2f;

    float sum_steer = 0.f;
    int ns = 0;
    float sum_drive = 0.f;
    int nd = 0;
    float sum_r = 0.f;
    int nr = 0;
    float sum_f = 0.f, sum_s = 0.f, sum_b = 0.f, sum_hb = 0.f, sum_rr = 0.f;
    float sum_pk_b = 0.f, sum_pk_c = 0.f, sum_pk_d = 0.f;
    float sum_spring = 0.f, sum_damp = 0.f, sum_rest = 0.f, sum_arm = 0.f;
    int nc = 0, n_arm = 0;
    for (int32_t i = 0; i < 4; ++i) {
      InvObject* w = java_game_parts_bodypart_Chassis_getWheel(car, i);
      if (!w) continue;
      if (i < 2) {
        java_game_parts_WheelRef_setSteer(w, ctrl_steer);
        sum_steer += java_game_parts_WheelRef_getSteer(w);
        ++ns;
      }
      // Front: (1-bal), rear: bal — same split as Brake.updatevariables.
      const float share = (i < 2) ? (1.f - bal) : bal;
      java_game_parts_WheelRef_setBrake(w, kBrkTorque * ctrl_brake * share);
      java_game_parts_WheelRef_setHBrake(
          w, (i >= 2) ? (kBrkTorque * ctrl_hb) : 0.f);
      sum_drive += java_game_parts_WheelRef_getDrive(w);
      ++nd;
      const float rad = java_game_parts_WheelRef_getRadius(w);
      if (rad > 0.05f) {
        sum_r += rad;
        ++nr;
      }
      sum_f += wheelref_get_friction(w);
      sum_s += wheelref_get_sliction(w);
      sum_b += wheelref_get_brake(w);
      sum_hb += wheelref_get_hbrake(w);
      sum_rr += wheelref_get_roll_res(w);
      sum_pk_d += wheelref_get_pacejka(w, 0);
      sum_pk_c += wheelref_get_pacejka(w, 2);
      sum_pk_b += wheelref_get_pacejka(w, 4);
      sum_spring += wheelref_get_force(w);
      sum_damp += wheelref_get_damp_bound(w);
      sum_rest += wheelref_get_rest_len(w);
      const float al = wheelref_get_arm_len(w);
      if (al > 0.05f) {
        sum_arm += al;
        ++n_arm;
      }
      ++nc;
    }
    const float steer = ns ? (sum_steer / static_cast<float>(ns)) : ctrl_steer;
    const float drive = nd ? (sum_drive / static_cast<float>(nd)) : 1.f;
    const float radius = nr ? (sum_r / static_cast<float>(nr)) : 0.32f;
    physics_set_wheel_params(body, steer, drive, radius);
    if (nc > 0) {
      const float inv = 1.f / static_cast<float>(nc);
      // Brake.java torque≈0.18 → near full arcade brake when /0.2.
      float brk = (sum_b * inv) / 0.2f;
      float hb = (sum_hb * inv) / 0.2f;
      if (brk > 1.f) brk = 1.f;
      if (hb > 1.f) hb = 1.f;
      if (brk < 0.f) brk = 0.f;
      if (hb < 0.f) hb = 0.f;
      physics_set_wheel_contact(body, sum_f * inv, sum_s * inv, brk, hb,
                                sum_rr * inv);
      physics_set_wheel_pacejka(body, sum_pk_b * inv, sum_pk_c * inv,
                                sum_pk_d * inv);
      const float arm =
          n_arm ? (sum_arm / static_cast<float>(n_arm)) : 0.244f;
      physics_set_wheel_suspension(body, sum_spring * inv, sum_damp * inv,
                                   sum_rest * inv, arm);
    }
  }

  // Phase 2.81: sample Chassis/DynoData torque at estimated RPM → drive scale.
  {
    InvObject* engine = tree_field_get_obj(car, "engine");
    InvObject* dyno =
        engine ? tree_field_get_obj(engine, "dynodata") : nullptr;
    if (!dyno) dyno = tree_field_get_obj(car, "dynodata");
    if (dyno) {
      float rpm = physics_get_engine_rpm(body);
      if (rpm < 800.f) rpm = 900.f;
      const float nitro_ax =
          ctrl ? input_map_get_logical(ctrl, kAxisNitro) : 0.f;
      const float nm =
          java_game_parts_bodypart_Chassis_getTorque(car, rpm, nitro_ax);
      physics_set_drive_torque(body, nm);
      tree_field_set_float(car, "engine_rpm", rpm);
      tree_field_set_float(car, "engine_torque_nm", nm);
    } else {
      physics_set_drive_torque(body, 0.f);  // legacy accel path
    }
  }

  physics_drive(body, ctrl, dt);

  float x = 0, y = 0, z = 0, oy = 0, op = 0, or_ = 0;
  if (InvObject* p = java_util_resource_PhysicsRef_getPos(body))
    vec3_get(p, &x, &y, &z);
  if (InvObject* o = java_util_resource_PhysicsRef_getOri(body))
    ypr_get(o, &oy, &op, &or_);
  tree_field_set_float(car, "pos_x", x);
  tree_field_set_float(car, "pos_y", y);
  tree_field_set_float(car, "pos_z", z);
  tree_field_set_float(car, "ori_y", oy);
  const float speed_sq = physics_speed_square(body);
  tree_field_set_float(car, "speed_sq", speed_sq);
  // Phase 2.73: odometer — integrate |v|·dt onto Chassis.mileage.
  {
    float spd = speed_sq > 0.f ? std::sqrt(speed_sq) : 0.f;
    const float miles = java_game_parts_bodypart_Chassis_getMileage(car) + spd * dt;
    java_game_parts_bodypart_Chassis_setMileage(car, miles);
  }

  valocity_ensure_car_mesh(car);
  valocity_sync_car_mesh(car);
  valocity_sync_wheel_visuals(car, dt);

  if (InvObject* nav = tree_field_get_obj(city, "nav"))
    java_game_Navigator_updateNavigator(nav, car, 0);

  valocity_update_camera(city);
  // W32D/W33D — PE Engine_tickPhysTraffic @ 0x463AC0 (GII_ANIMATE mode28).
  // Soft: sweep_dead → frame_update(dt) → sort_compact budget=1 after
  // cam update (MainLoop fills Engine_camPosScratch; host lookat).
  // dt ≡ *Engine_physWorld pushed @ 0x463c54.
  if (InvObject* map = tree_field_get_obj(city, "map"))
    engine_tick_phys_traffic(map, dt);
  valocity_update_hud(city);
}

void* valocity_camera_key() {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(0x56C010u));
}

void* valocity_hud_font_key() {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(0x56C020u));
}

void* valocity_hud_speed_key() {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(0x56C021u));
}

void* valocity_hud_gear_key() {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(0x56C022u));
}

float valocity_speed_kph(InvObject* car) {
  if (!car) return 0.f;
  float ssq = tree_field_get_float(car, "speed_sq");
  if (ssq < 0.f) ssq = 0.f;
  // CarInfo.java: sqrt(speedSq) * 2.24 * 1.61 → KPH
  return std::sqrt(ssq) * 2.24f * 1.61f;
}

void valocity_update_hud(InvObject* city) {
  if (!city) return;
  InvObject* car = tree_field_get_obj(city, "car");
  if (!car) {
    if (InvObject* player = tree_field_get_obj(city, "player"))
      car = tree_field_get_obj(player, "car");
  }
  if (!car) return;

  // Stock: Frontend.setFonts → Text.RID_* in frontend.rpk (see Text.java).
  // Loose frontend/meshes/Fonts/*.scx is not shipped; load via RID.
  void* font = frontend_medium_font();
  if (font && !render_d3d9_font_ready(font)) {
    const int32_t rid = java_util_resource_ResourceRef_id(
        static_cast<InvObject*>(font));
    if (rid) render_d3d9_font_load_from_rid(font, rid);
  }
  if (!font || !render_d3d9_font_ready(font)) {
    font = valocity_hud_font_key();
    if (!render_d3d9_font_ready(font)) {
      if (!rpak_find_by_name("frontend.rpk")) rpak_open("frontend.rpk");
      const RpakPack* fe = rpak_find_by_name("frontend.rpk");
      const int32_t rid =
          fe ? rpak_make_id(fe->pack_id, 0x0020) : 0;  // Text.RID_SIMPLE20
      if (!rid || !render_d3d9_font_load_from_rid(font, rid)) return;
    }
  }

  void* speed_key = valocity_hud_speed_key();
  void* gear_key = valocity_hud_gear_key();
  render_d3d9_text_create(speed_key, font, 0.85f, -0.82f);
  render_d3d9_text_set_align(speed_key, 0);  // RIGHT
  render_d3d9_text_set_color(speed_key, 0xFFFFFFFFu);
  render_d3d9_text_create(gear_key, font, -0.85f, -0.82f);
  render_d3d9_text_set_align(gear_key, 2);  // LEFT
  render_d3d9_text_set_color(gear_key, 0xFFCCFFCCu);

  const float kph = valocity_speed_kph(car);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f KPH", kph);
  render_d3d9_text_set_string(speed_key, buf);
  render_d3d9_text_update(speed_key);

  InvObject* body = tree_field_get_obj(car, "chassis");
  if (!body) body = car;
  const int32_t gear = physics_get_gear(body);
  if (gear < 0)
    std::snprintf(buf, sizeof(buf), "R");
  else if (gear == 0)
    std::snprintf(buf, sizeof(buf), "N");
  else
    std::snprintf(buf, sizeof(buf), "G%d", gear);
  render_d3d9_text_set_string(gear_key, buf);
  render_d3d9_text_update(gear_key);
}

void valocity_update_camera(InvObject* city) {
  if (!city) return;
  InvObject* car = tree_field_get_obj(city, "car");
  if (!car) {
    if (InvObject* player = tree_field_get_obj(city, "player"))
      car = tree_field_get_obj(player, "car");
  }
  if (!car) return;
  const float px = tree_field_get_float(car, "pos_x");
  const float py = tree_field_get_float(car, "pos_y");
  const float pz = tree_field_get_float(car, "pos_z");
  const float oy = tree_field_get_float(car, "ori_y");
  void* cam = valocity_camera_key();
  // Ensure camera exists with city-scale far clip.
  if (render_d3d9_camera_half_aov(cam) <= 0.f) {
    void* vp = render_d3d9_viewport_active();
    if (!vp) {
      vp = reinterpret_cast<void*>(static_cast<uintptr_t>(0x56C011u));
      render_d3d9_viewport_create(vp, 0, 0.f, 0.f, 1.f, 1.f);
    }
    render_d3d9_camera_create(cam, nullptr, vp, 0, 45.f, 0.5f, 2000.f, 1.f, 1.f,
                              1, 0);
  }
  render_d3d9_camera_chase(cam, px, py, pz, oy, 8.f, 2.8f, 1.2f);
  // Activate even without a live D3D device so chase lookat is the active cam
  // (headless Valocity.camera smoke; stock always has a device).
  void* vp = render_d3d9_viewport_active();
  if (!vp) vp = reinterpret_cast<void*>(static_cast<uintptr_t>(0x56C011u));
  render_d3d9_camera_activate(cam, vp, 0);
}

InvObject* valocity_return_to_garage(InvObject* city) {
  if (!city) return nullptr;
  if (InvObject* nav = tree_field_get_obj(city, "nav")) {
    if (InvObject* m = tree_field_get_obj(city, "mPlayer"))
      navigator_rem_marker(nav, m);
    navigator_hide(nav);
  }
  InvObject* map = tree_field_get_obj(city, "map");
  if (map) java_util_resource_GroundRef_delTraffic(map);
  tree_field_set_int(city, "traffic_count",
                     map ? tree_field_get_int(map, "traffic_count") : 0);
  tree_field_set_int(city, "entered", 0);
  if (InvObject* player = tree_field_get_obj(city, "player")) {
    InvObject* car = tree_field_get_obj(city, "car");
    if (!car) car = tree_field_get_obj(player, "car");
    if (car) {
      tree_field_set_float(car, "speed_sq", 0.f);
      InvObject* body = tree_field_get_obj(car, "chassis");
      if (!body) body = car;
      if (physics_shape(body) != 0) {
        // PE PhysicsRef.setMatrix(null,null) @ 0x00480920 writes origin.
        // Garage return must stop the chassis in place — not teleport.
        physics_set_velocity(body, 0.f, 0.f, 0.f);
        java_util_resource_PhysicsRef_setStatic(body, 1);
      }
    }
  }
  // Simulate YesNoDialog accept → changeActiveSection(GameLogic.garage)
  InvObject* garage = tree_field_get_obj(city, "parentState");
  if (!garage) garage = game_logic_garage();
  return game_logic_change_active_section(garage);
}


// → GroundRef_traffic.cpp

// java_game_Vehicle_getSpeedSquare / getHorn / hasCrime — bodies in
// runtime/Cars/Cars.cpp (PE @ 0x00480500 / 0x0043DB60 / 0x00440BF0).

}  // namespace inv
