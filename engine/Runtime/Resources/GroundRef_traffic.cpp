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

// traffic_spawn_host — PE Traffic_pool_alloc @ 0x0057C380 on
// g_CarContainer_trafficCars @ 0x798A08. Layout: +0 pages**, +4 page_n,
// +8 next_i, +0xC cap, +0x10 freelist. Freelist pop else grow
// cap+=0x400, Engine_realloc(4*page_n), Engine_malloc(0x59000=1024*356),
// return pages[i/1024]+356*(i%1024). Callers: trySpawnOnRandomPath
// @ 0x57b479, trySpawnFacingPath @ 0x5823ba, GroundMap_delTraffic
// default eng+0x3118 @ 0x58172d. Soft: SoftTrafficCarBlob stand-in
// (PE 356B body / ResHandle / path dllist OOS); bind InvObject host.
constexpr int32_t kTrafficPoolPageSlots = 1024;  // PE add 0x400 @ 0x57c399

struct SoftTrafficCarBlob {
  SoftTrafficCarBlob* freelist_next = nullptr;  // PE +0 when free
  InvObject* host = nullptr;                    // Soft InvObject bind
  int32_t serial = 0;  // PE live +0 ← ++g_TrafficCarSerial @ 0x798A0C
  int32_t in_use = 0;
};

struct SoftTrafficCarsPool {
  std::vector<SoftTrafficCarBlob*> pages;  // PE +0
  int32_t page_n = 0;                      // PE +4
  int32_t next_i = 0;                      // PE +8
  int32_t cap = 0;                         // PE +0xC
  SoftTrafficCarBlob* freelist = nullptr;  // PE +0x10
};

static SoftTrafficCarsPool g_soft_traffic_cars;
static SoftTrafficCarBlob* g_soft_default_car = nullptr;  // eng+0x3118
static std::unordered_map<InvObject*, SoftTrafficCarBlob*> g_soft_car_blob;
static int32_t g_soft_traffic_serial = 0;  // g_TrafficCarSerial @ 0x798A0C

// PE @ 0x0057C380 — thiscall pool; Soft file-local g_soft_traffic_cars.
static SoftTrafficCarBlob* traffic_pool_alloc() {
  SoftTrafficCarsPool& p = g_soft_traffic_cars;
  if (p.freelist) {
    SoftTrafficCarBlob* r = p.freelist;
    p.freelist = r->freelist_next;  // PE @ 0x57c38c
    r->freelist_next = nullptr;
    return r;
  }
  if (p.next_i == p.cap) {
    // PE @ 0x57c399..0x57c3cc: cap+=1024; ++page_n; realloc; malloc page.
    p.cap += kTrafficPoolPageSlots;
    ++p.page_n;
    p.pages.push_back(new SoftTrafficCarBlob[kTrafficPoolPageSlots]);
  }
  const int32_t i = p.next_i++;  // PE @ 0x57c3d0..0x57c3d6
  return &p.pages[static_cast<size_t>(i / kTrafficPoolPageSlots)]
              [static_cast<size_t>(i % kTrafficPoolPageSlots)];
}

// PE freelist push inline @ trySpawn reject 0x57b899 / Traffic_destroy
// @ 0x57902f: *blob = freelist; freelist = blob.
static void traffic_pool_free(SoftTrafficCarBlob* blob) {
  if (!blob) return;
  blob->host = nullptr;
  blob->serial = 0;
  blob->in_use = 0;
  blob->freelist_next = g_soft_traffic_cars.freelist;
  g_soft_traffic_cars.freelist = blob;
}

// PE Traffic_car_ctor @ 0x00575C30 — Soft serial + in_use only
// (path/RH/float defaults OOS; spawn TREE seeds elsewhere).
static SoftTrafficCarBlob* traffic_car_ctor(SoftTrafficCarBlob* blob) {
  if (!blob) return nullptr;
  blob->freelist_next = nullptr;
  blob->host = nullptr;
  blob->serial = ++g_soft_traffic_serial;  // PE ++g_TrafficCarSerial
  blob->in_use = 1;
  return blob;
}

static void traffic_pool_bind_host(InvObject* car, SoftTrafficCarBlob* blob) {
  if (!car || !blob) return;
  blob->host = car;
  g_soft_car_blob[car] = blob;
  tree_field_set_int(car, "traffic_blob_serial", blob->serial);
}

static void traffic_pool_unbind_host(InvObject* car) {
  if (!car) return;
  auto it = g_soft_car_blob.find(car);
  if (it == g_soft_car_blob.end()) return;
  SoftTrafficCarBlob* blob = it->second;
  g_soft_car_blob.erase(it);
  traffic_pool_free(blob);
  tree_field_set_int(car, "traffic_blob_serial", 0);
}

void ground_sync_fields(InvObject* self) {
  if (!self) return;
  GroundTrafficState& g = ground(self);
  tree_field_set_int(self, "traffic_count", g.traffic_count);
  tree_field_set_int(self, "traffic_streams", g.traffic_streams);
  tree_field_set_float(self, "pedestrian_density", g.ped_density);
  tree_field_set_float(self, "pedestrian_density_hi", g.ped_density_hi);
  tree_field_set_int(self, "pedestrian_types", g.ped_types);
  tree_field_set_int(self, "path_spawns", g.path_spawns);
  // W28D — Traffic_activate_apply pool / car_rem counters.
  tree_field_set_int(self, "traffic_pool_active", g.pool_active);
  tree_field_set_int(self, "traffic_activate_n", g.activate_n);
  tree_field_set_int(self, "traffic_deactivate_n", g.deactivate_n);
  tree_field_set_int(self, "traffic_car_rem_n", g.car_rem_n);
  // W29/30D — eng+0x3110 stand-in (push count; compact rewrites).
  tree_field_set_int(self, "traffic_pool_n",
                     static_cast<int32_t>(g.pool_slots.size()));
  // Soft Traffic_pool_alloc @ 0x57C380 (g_CarContainer_trafficCars).
  int32_t blob_free = 0;
  for (SoftTrafficCarBlob* n = g_soft_traffic_cars.freelist; n;
       n = n->freelist_next)
    ++blob_free;
  tree_field_set_int(self, "traffic_blob_live_n",
                     static_cast<int32_t>(g_soft_car_blob.size()));
  tree_field_set_int(self, "traffic_blob_free_n", blob_free);
  tree_field_set_int(self, "traffic_blob_page_n", g_soft_traffic_cars.page_n);
  tree_field_set_int(self, "traffic_blob_next_i", g_soft_traffic_cars.next_i);
  tree_field_set_int(self, "traffic_default_car",
                     g_soft_default_car ? 1 : 0);
  // W34 — eng+0x98 live / eng+0xAC freelist counts (soft owned).
  int32_t pn_live = 0, pn_free = 0;
  for (auto* n = g.pathnodes_live; n; n = n->list_next) ++pn_live;
  for (auto* n = g.pathnodes_free; n; n = n->list_next) ++pn_free;
  tree_field_set_int(self, "traffic_pathnode_n", pn_live);
  tree_field_set_int(self, "traffic_pathnode_free_n", pn_free);
  // W35-04 — eng+4 timer-heap count / pop fire counter.
  tree_field_set_int(self, "traffic_timer_heap_n",
                     static_cast<int32_t>(g.timer_heap.size()));
  tree_field_set_int(self, "traffic_timer_heap_pops", g.timer_heap_pops);
  tree_field_set_float(self, "traffic_sim_clock", g.sim_clock);
  tree_field_set_float(self, "traffic_tick_phys_ema", g.tick_phys_ema);
  tree_field_set_int(self, "traffic_tick_phys_n", g.tick_phys_n);
  // W35-05 — traffic_list88_n set by engine_tick_phys_traffic (TREE).
  tree_field_set_float(self, "water_level", g.water_level);
  tree_field_set_float(self, "water_density", g.water_density);
  tree_field_set_float(self, "water_viscosity", g.water_viscosity);
  tree_field_set_int(self, "water_plane", g.water_plane ? 1 : 0);
  tree_field_set_int(self, "water_limits",
                     static_cast<int32_t>(g.water_limits.size()));
  tree_field_set_int(self, "halt_crosses",
                     static_cast<int32_t>(g.halt_crosses.size()));
  tree_field_set_int(self, "halt_paths",
                     static_cast<int32_t>(g.halt_paths.size()));
}

// W29D — PE Traffic_pool_push @ 0x5810A0 (thiscall eng, arg car):
// if count==cap → cap+=0x400, realloc(16*cap) into eng+0x310C; ++count;
// Traffic_pool_slot_write(slot, car) @ 0x575AC0. Soft: push car ptr
// (PE slot[0]=*car dword OOS on InvObject). No pop on deactivate —
// compact via Traffic_pool_sort_compact (W30D) / delTraffic clear.
static void traffic_pool_push(InvObject* car, GroundTrafficState& g) {
  if (!car) return;
  g.pool_slots.push_back(car);
}

// W31D/W32D/W35 — PE Traffic_car_sample_pos @ 0x575890 (thiscall car,
// out*, f88, fA8): if car+4 path: lane sign at +8 <0 →
// Traffic_path_sample_pos_rev @ 0x57EA00 else
// Traffic_path_sample_pos_fwd @ 0x57CD20 with cursor floats
// (+0x88/+0xA8 from sort_compact @ 0x581170). Soft: path cursor OOS —
// stand-in GetWorldPos ch=2 via PhysicsRef / GameRefState under g_gr_mu.
// CRITICAL: never GameRef_getPos (g_gr_mu non-recursive). PE no-path →
// zeros; Soft keeps inner+0x84 cache so sort scores stay live without
// path objects. f88/fA8 Soft-stashed on TREE for VA parity.
void traffic_car_sample_pos(InvObject* car, float* ox, float* oy,
                                   float* oz, float cursor_88,
                                   float lane_a8) {
  *ox = *oy = *oz = 0.f;
  if (!car) return;
  // PE passes *(car+0x88)/+0xA8 into path sample; Soft path OOS —
  // args kept for VA call shape (sort_compact @ 0x581170).
  (void)cursor_88;
  (void)lane_a8;
  // Caller holds g_gr_mu. Soft under_mu: PhysicsRef then GameRefState.
  (void)gameref_soft_sample_pos_under_mu(car, ox, oy, oz);
}

// W31D — PE Traffic_pool_heap_gt @ 0x57C340: slot+4 car &&
// (!peer+4 || this.score > peer.score). Soft: ptr + float score.
struct TrafficPoolSlot {
  InvObject* car = nullptr;
  float score = 0.f;
};

static bool traffic_pool_heap_gt(const TrafficPoolSlot& a,
                                 const TrafficPoolSlot& b) {
  return a.car != nullptr && (b.car == nullptr || a.score > b.score);
}

// W31D/W32D — PE Traffic_pool_car_tick @ 0x5791E0 (thiscall car, int*
// budget): ret1 drop: !+0xDC / !+0x14C / +0x138!=0 / path-parent miss;
// else path follow + optional LoadGameInit("traffic_car") when
// *budget!=0 (--budget); ret0 keep when budget exhausted. Soft gates
// only — path follow / ResHandle / Engine_LoadGameInit OOS.
// remTrafficCar: budget=0 → keep live (ret0). Engine_tickPhysTraffic:
// a2=1 @ 0x463c72 (wired W32D).
static bool traffic_pool_car_tick(InvObject* car, int32_t& budget) {
  if (!car) return true;
  // PE +0xDC (220) visible — host traffic_flag_220 (set on activate).
  if (tree_field_get_int(car, "traffic_flag_220") == 0) return true;
  // PE +0x14C (332) map — soft: still activated (traffic_active).
  if (tree_field_get_int(car, "traffic_active") == 0) return true;
  // PE +0x138 (312) GameInit RH — NOT host traffic_live (armed at
  // activate). Soft: traffic_gameinit after budget LoadGameInit stand-in.
  if (tree_field_get_int(car, "traffic_gameinit") != 0) return true;
  // Path sample / follow / setParent / LoadGameInit body OOS.
  if (budget == 0) return false;  // PE ret0 keep
  if (budget > 0) --budget;       // PE a2=-1 (ldHigh) stays nonzero
  tree_field_set_int(car, "traffic_gameinit", 1);
  return true;  // PE ret1 after LoadGameInit → zero slot
}

// W30D/W31D/W32D — PE Traffic_pool_sort_compact @ 0x581100 (thiscall eng,
// a2 budget, a3 float* cam|null). System_ldHigh @ 0x640920 ≠0 → a2=-1
// (@ 0x581120). Score live slots (-dist^2 via Traffic_car_sample_pos
// @ 0x575890), heap-sort (Traffic_pool_heap_gt @ 0x57C340), walk:
// Traffic_pool_slot_live @ 0x575B40 → break on first dead;
// Traffic_pool_car_tick @ 0x5791E0 (ret1 → zero slot; ret0 keep);
// rewrite eng+0x3110. Sole PE caller Engine_tickPhysTraffic @ 0x463c72
// (a2=1). Soft: ldHigh override + dead compact + score/sort + car_tick
// gates; path cursor sample / LoadGameInit OOS. remTrafficCar: budget=0,
// cam=null. Tick: budget=1, cam=Engine_camPosScratch|null.
static void traffic_pool_sort_compact(GroundTrafficState& g,
                                      int32_t budget = 0,
                                      const float* cam = nullptr) {
  // PE @ 0x581120: System_ldHigh → unlimited car_tick budget.
  if (system_ld_priority_for_test() != 0) budget = -1;
  std::vector<TrafficPoolSlot> slots;
  slots.reserve(g.pool_slots.size());
  for (InvObject* car : g.pool_slots) {
    if (!car) continue;
    // PE slot_live: slot[1]!=0 && slot[0]==*car. Soft: still armed.
    if (tree_field_get_int(car, "traffic_active") == 0 &&
        tree_field_get_int(car, "traffic_live") == 0)
      continue;
    TrafficPoolSlot s;
    s.car = car;
    if (cam) {
      float x = 0.f, y = 0.f, z = 0.f;
      // PE @ 0x581170: sample_pos(car, out, *(car+0x88), *(car+0xA8)).
      const float f88 = tree_field_get_float(car, "traffic_cursor");
      const float fA8 = tree_field_get_float(car, "traffic_lane");
      traffic_car_sample_pos(car, &x, &y, &z, f88, fA8);
      const float dx = x - cam[0];
      const float dy = y - cam[1];
      const float dz = z - cam[2];
      s.score = -(dx * dx + dy * dy + dz * dz);
    } else {
      s.score = 0.f;  // PE cam null → score 0
    }
    slots.push_back(s);
  }
  // Soft heapsort: stable_sort by heap_gt (higher -dist^2 / closer first).
  std::stable_sort(slots.begin(), slots.end(), traffic_pool_heap_gt);
  // PE walk zeros on car_tick ret1; rewrite count. Soft: drop vs keep.
  std::vector<InvObject*> kept;
  kept.reserve(slots.size());
  for (TrafficPoolSlot& s : slots) {
    if (traffic_pool_car_tick(s.car, budget)) continue;
    kept.push_back(s.car);
  }
  g.pool_slots.swap(kept);
}

// W34 — PE Traffic_pathnode_clear @ 0x575710 (thiscall node).
// Drain node[+0x14] child entries → freelist eng+0xC0 (dllist unlink
// via entry[+0xC] parent+4); zero +0x14. If node[+0x10] parent: unlink
// RH hook (+4/+8) from parent+0x48; else zero +0xC only. Soft: no
// ResHandle_Link/Unlink — parent_head / sibling ptrs. Caller holds g_gr_mu
// — never GameRef_getPos.
static void traffic_pathnode_clear(GroundTrafficState::TrafficPathNode* node,
                                   GroundTrafficState& g) {
  if (!node) return;
  // PE @ 0x575710: walk children at +0x14 (chain via entry+0x8).
  GroundTrafficState::TrafficPathEntry* e = node->children;
  while (e) {
    GroundTrafficState::TrafficPathEntry* chain = e->chain_next;
    if (e->parent_head) {
      if (e->dll_prev)
        e->dll_prev->dll_next = e->dll_next;
      else
        *e->parent_head = e->dll_next;
      if (e->dll_next) e->dll_next->dll_prev = e->dll_prev;
      e->parent_head = nullptr;
    }
    e->dll_prev = nullptr;
    e->dll_next = nullptr;
    e->chain_next = g.path_entries_free;  // PE eng+0xC0 push
    g.path_entries_free = e;
    e = chain;
  }
  node->children = nullptr;
  // PE @ 0x57576e: unlink from parent+0x48 sibling dllist.
  if (node->parent_head) {
    if (node->sib_prev)
      node->sib_prev->sib_next = node->sib_next;
    else
      *node->parent_head = node->sib_next;
    if (node->sib_next) node->sib_next->sib_prev = node->sib_prev;
    node->parent_head = nullptr;
    node->live = 0;
    node->sib_prev = nullptr;
    node->sib_next = nullptr;
  } else {
    node->live = 0;  // PE @ 0x5757a4
  }
}

// W33D/W34 — PE Traffic_pool_sweep_dead @ 0x584BF0 (thiscall eng).
// Walks eng+0x98; node[+0xC]==0 → Traffic_pathnode_clear @ 0x575710
// then push freelist eng+0xAC. Soft pathnode list + clear; also keep
// W33 pool_slots cull (eng+0x310C stand-in — not PE sweep body).
// Under g_gr_mu: TREE / pathnode ptrs only — never GameRef_getPos.
static void traffic_pool_sweep_dead(GroundTrafficState& g) {
  // PE @ 0x584bf4: v3=&eng+0x98, v2=*v3; live keep advances v3=node.
  GroundTrafficState::TrafficPathNode** link = &g.pathnodes_live;
  GroundTrafficState::TrafficPathNode* cur = g.pathnodes_live;
  while (cur) {
    if (cur->live != 0) {
      link = &cur->list_next;
      cur = cur->list_next;
    } else {
      traffic_pathnode_clear(cur, g);
      GroundTrafficState::TrafficPathNode* next = cur->list_next;
      *link = next;
      cur->list_next = g.pathnodes_free;  // eng+0xAC
      g.pathnodes_free = cur;
      cur = next;
    }
  }
  // W33 soft car pool cull (PE sweep is pathnodes-only).
  std::vector<InvObject*> kept;
  kept.reserve(g.pool_slots.size());
  for (InvObject* car : g.pool_slots) {
    if (!car) continue;
    if (tree_field_get_int(car, "traffic_active") == 0 &&
        tree_field_get_int(car, "traffic_live") == 0)
      continue;
    kept.push_back(car);
  }
  g.pool_slots.swap(kept);
}

// W35-04 — PE Traffic_timer_heap_push @ 0x57FE80 (thiscall eng, float
// delay, payload). due = delay + eng+0x84; grow cap+=0x400; min-heap
// sift-up. Soft: vector + sim_clock stand-in for *Engine_physWorld.
static void traffic_timer_heap_push(GroundTrafficState& g, float delay,
                                   InvObject* car) {
  const float due = delay + g.sim_clock;
  GroundTrafficState::TimerHeapEntry ent{due, car};
  g.timer_heap.push_back(ent);
  size_t i = g.timer_heap.size() - 1;
  while (i > 0) {
    const size_t p = (i - 1) / 2;
    if (g.timer_heap[p].due <= due) break;
    g.timer_heap[i] = g.timer_heap[p];
    i = p;
  }
  g.timer_heap[i] = ent;
}

// W35-04 — siftdown after root remove (inline @ 0x580ef5..0x580f87).
static void traffic_timer_heap_siftdown(
    GroundTrafficState& g, size_t hole,
    GroundTrafficState::TimerHeapEntry last) {
  const size_t n = g.timer_heap.size();
  size_t child = hole * 2 + 1;
  while (child < n) {
    if (child + 1 < n &&
        g.timer_heap[child + 1].due < g.timer_heap[child].due)
      ++child;
    if (last.due <= g.timer_heap[child].due) break;
    g.timer_heap[hole] = g.timer_heap[child];
    hole = child;
    child = hole * 2 + 1;
  }
  g.timer_heap[hole] = last;
}

// W35-04/W35-05 — PE Traffic_timer_heap_pop @ 0x57AB00 (thiscall
// payload). Sole caller eng_frame_update @ 0x580ef0 after due root.
// PE: always --(+0xAC); path body when *(BYTE*)(+0xA8)==0 (lane float
// overlay OOS); LABEL_29: +0xAC==0 && +8≠0 → push(1.0) + ++(+0xAC).
// Soft: TREE traffic_timer_armed; always decrement; LABEL_29 re-arm.
// Path walk sub_576350 / sub_575A20 / sub_5789E0 / sub_579D60 OOS.
static void traffic_timer_heap_pop_fire(GroundTrafficState& g,
                                       InvObject* car) {
  ++g.timer_heap_pops;
  if (!car) return;
  // PE @ 0x57ab0c: *(a1+0xAC) = *(a1+0xAC) - 1 (may go negative).
  int32_t armed = tree_field_get_int(car, "traffic_timer_armed") - 1;
  tree_field_set_int(car, "traffic_timer_armed", armed);
  // PE +0xA8 BYTE gate / path-delay push middle OOS — Soft LABEL_29 only.
  // PE LABEL_29 @ 0x57ac81: +0xAC==0 && +8≠0 → push 1.0; ++(+0xAC).
  if (armed == 0 &&
      (tree_field_get_int(car, "traffic_active") != 0 ||
       tree_field_get_int(car, "traffic_live") != 0)) {
    tree_field_set_int(car, "traffic_timer_armed", 1);
    traffic_timer_heap_push(g, 1.0f, car);
  }
}

// W35-05 — PE Traffic_car_frame_update @ 0x577FA0 (thiscall car).
// Callers: Traffic_list88_tick @ 0x5787ae/0x578800,
// Engine_tickTrafficCarSlot @ 0x44c15e. Gates @ 0x577fdd:
//   !+0xC (path) || !+0xDC (visible) || +0xE8 >= eng+0x84 → ret.
// Else dt = eng+0x84 − +0xE8; stamp +0xE8 = eng+0x84; path project +
// GameRef_applyWorldXform OOS. Soft: TREE stamp/cursor; advance
// traffic_cursor by traffic_speed * dt (path/xform OOS). No getPos.
static bool traffic_car_frame_update(InvObject* car, float abs_clock) {
  if (!car) return false;
  // Soft +0xC path: require active|live (PE path ptr).
  if (tree_field_get_int(car, "traffic_active") == 0 &&
      tree_field_get_int(car, "traffic_live") == 0)
    return false;
  // PE +0xDC visible.
  if (tree_field_get_int(car, "traffic_flag_220") == 0) return false;
  // PE +0xE8 last stamp vs eng+0x84 (g_CarContainer+132).
  const float stamp = tree_field_get_float(car, "traffic_frame_stamp");
  if (stamp >= abs_clock) return false;
  const float dt = abs_clock - stamp;
  tree_field_set_float(car, "traffic_frame_stamp", abs_clock);
  // Soft path follow stand-in: cursor +0x88 += speed(+0x2C) * dt.
  // PE pathseg_project / Mat3x4 / applyWorldXform remain OOS.
  const float speed = tree_field_get_float(car, "traffic_speed");
  const float cursor = tree_field_get_float(car, "traffic_cursor");
  tree_field_set_float(car, "traffic_cursor", cursor + speed * dt);
  // PE +0x140 once-flag / ch64/70/65 dims / sub_577AC0 OOS.
  return true;
}

// W33D/W35-04/W35-05 — PE Traffic_eng_frame_update @ 0x580E70
// (thiscall eng, float a2=*Engine_physWorld). Call @ 0x463c46..55:
// ecx=eng@a1+0x88. Sets dword_798A54=eng, eng+0x84=a2; optional
// eng+0x3118 cascade; timer-heap drain → Traffic_timer_heap_pop;
// list walks Traffic_list90/94/88/8C_tick; drain +0xC8 →
// Engine_queueEvent. Soft: sim_clock + heap drain/pop + list88 soft
// car_frame_update over pool_slots; list90/94/8C / queueEvent OOS.
// Returns soft list88 fire count (PE ret keeps last dllist walk).
static int32_t traffic_eng_frame_update(GroundTrafficState& g, float dt) {
  g.sim_clock += dt;   // soft *Engine_physWorld absolute
  g.frame_dt = dt;     // last host delta (PE stores absolute a2 @ +0x84)
  // PE @ 0x580e87: eng+0x3118 cascade (+0xE8/+0xE4/+0xE0) OOS.
  // PE @ 0x580ecc: while eng+4!=0 && heap[0].due < a2 → pop fire +
  // siftdown. Soft: due < sim_clock.
  while (!g.timer_heap.empty() && g.timer_heap[0].due < g.sim_clock) {
    InvObject* car = g.timer_heap[0].car;
    GroundTrafficState::TimerHeapEntry last = g.timer_heap.back();
    g.timer_heap.pop_back();
    if (!g.timer_heap.empty()) traffic_timer_heap_siftdown(g, 0, last);
    traffic_timer_heap_pop_fire(g, car);
  }
  // PE @ 0x580f98..0x58105a: list90/94/88/8C dllist ticks.
  // Soft list88 @ 0x580ff0 (eng+0x88): walk pool_slots →
  // Traffic_car_frame_update @ 0x577FA0. list90/94/8C + +0xC8 OOS.
  int32_t list88_n = 0;
  for (InvObject* car : g.pool_slots) {
    if (traffic_car_frame_update(car, g.sim_clock)) ++list88_n;
  }
  return list88_n;
}

// W32D/W33D/W34/W35 Soft — PE Engine_tickPhysTraffic @ 0x463AC0
// (stdcall a1,a2 unused). Soft traffic slice (a1+136 eng gate @ 0x463c2b):
//   Traffic_pool_sweep_dead @ 0x584BF0 (pathnodes eng+0x98 + pool cull);
//   Traffic_eng_frame_update @ 0x580E70 (sim_clock + timer-heap + list88);
//   Traffic_pool_sort_compact(eng,1,cam|0) @ 0x463c72
//   (cam=Engine_camPosScratch when hasCameraMatrix>0 else null);
//   Soft EMA eng+0x90 @ 0x463c89 (host tick_phys_ema via frame dt);
//   dword_798A2C/38/3C/08 pathGrid/grids/trafficCars stats OOS.
// a1+140 ped half (queryGameRefChannel ch2/ch3 → moverBuf) OOS.
// Host: GroundRef map ≡ eng; call from valocity_simulate. Holds g_gr_mu —
// sample_pos Soft via PhysicsRef/GameRefState only (never getPos).
void engine_tick_phys_traffic(InvObject* map, float dt) {
  if (!map) return;
  float cam_xyz[3] = {0.f, 0.f, 0.f};
  const float* cam = nullptr;
  // PE @ 0x463c60: Engine_hasCameraMatrix<=0 → cam=null; else
  // &Engine_camPosScratch @ 0x62F140 (MainLoop copies CameraMatrix_t*).
  void* ckey = render_d3d9_camera_active();
  if (!ckey) ckey = valocity_camera_key();
  float at_x = 0.f, at_y = 0.f, at_z = 0.f;
  if (ckey &&
      render_d3d9_camera_get_lookat(ckey, &cam_xyz[0], &cam_xyz[1],
                                    &cam_xyz[2], &at_x, &at_y, &at_z)) {
    cam = cam_xyz;
  }
  std::lock_guard<std::mutex> lock(g_gr_mu);
  GroundTrafficState& g = ground(map);
  // PE gates on eng ptr @ a1+136; host: any armed pool / cars.
  if (g.pool_slots.empty() && g.traffic_cars.empty()) return;
  traffic_pool_sweep_dead(g);             // PE @ 0x463c41
  const int32_t list88_n =
      traffic_eng_frame_update(g, dt);    // PE @ 0x463c55
  traffic_pool_sort_compact(g, /*budget=*/1, cam);  // PE a2=1 @ 0x463c72
  // Soft PE @ 0x463c89: EMA eng+0x90 = old + 0.1*(sample-old).
  // Host: sample = frame dt (PerfTimer3 OOS).
  g.tick_phys_ema = g.tick_phys_ema + 0.1f * (dt - g.tick_phys_ema);
  ++g.tick_phys_n;
  // W35-05 — soft list88 fire count for ground_sync / scripts.
  tree_field_set_int(map, "traffic_list88_n", list88_n);
  ground_sync_fields(map);
}

// W28D — PE Traffic_activate @ 0x5791C0 → Traffic_activate_apply @ 0x579040.
// this+0xC4 path → a2=[path+0x10]+0x68. Gates: +0x138 live (RH+8), +0x14C
// map, +0xDC visible (Traffic_setVisibleDC @ 0x576040), +0xDD ch69.
// Activate pool @ 0x5790c6: ++g_TrafficPoolActive, ++stats.lo,
// Traffic_pool_push(eng@798A54) when map&&DC&&!+0x138. Deactivate @
// 0x5790fe: if +0xDD queueEvent "car_rem %d %d"; unlink +0x130; --pool;
// ++stats.hi. Spawn-time +0x14C usually 0 → setParent default eng+0x311C
// (OOS); host soft: TREE active + pool counters + pool_slots. ch69 query
// OOS → flag_221 soft 1. Path cars +8 / path+0x74=116 free-space stays
// W27D XZ stand-in (no Resources).
static void traffic_activate_apply(InvObject* car, GroundTrafficState& g) {
  if (!car) return;
  // PE Rebind(+0x130) before activate → +0x138 live stand-in.
  tree_field_set_int(car, "traffic_live", 1);
  // PE pool push needs +0xDC (Traffic_setVisibleDC). Soft DC for W31D
  // car_tick gate (!+0xDC → drop).
  tree_field_set_int(car, "traffic_flag_220", 1);
  if (tree_field_get_int(car, "traffic_active") != 0) return;
  tree_field_set_int(car, "traffic_active", 1);
  ++g.pool_active;
  ++g.activate_n;
  // Soft PE Traffic_pool_alloc @ 0x57C380 + Traffic_car_ctor @ 0x575C30
  // (trySpawn* @ 0x57b479 before activate). Soft bind after gates —
  // 356B PE body OOS; serial via g_TrafficCarSerial @ 0x798A0C.
  if (g_soft_car_blob.find(car) == g_soft_car_blob.end()) {
    SoftTrafficCarBlob* blob = traffic_car_ctor(traffic_pool_alloc());
    traffic_pool_bind_host(car, blob);
  }
  // PE @ 0x5790e7 Traffic_pool_push(dword_798A54, car).
  traffic_pool_push(car, g);
  // W35-04 soft arm: PE spawn paths call Traffic_timer_heap_push;
  // LABEL_29 also re-pushes 1.0. Soft first arm delay=1.0.
  tree_field_set_int(car, "traffic_timer_armed", 1);
  traffic_timer_heap_push(g, 1.0f, car);
  // W35-05 — PE +0xE8 frame stamp: seed to eng+0x84 so first
  // Traffic_car_frame_update waits one abs-clock step (no catch-up).
  tree_field_set_float(car, "traffic_frame_stamp", g.sim_clock);
}

static void traffic_deactivate_apply(InvObject* car, GroundTrafficState& g) {
  if (!car) return;
  const int32_t live = tree_field_get_int(car, "traffic_live");
  const int32_t active = tree_field_get_int(car, "traffic_active");
  if (live == 0 && active == 0) return;
  // PE @ 0x5790fe: cmp [esi+0DDh]; queueEvent "car_rem %d %d".
  if (tree_field_get_int(car, "traffic_flag_221") != 0) ++g.car_rem_n;
  tree_field_set_int(car, "traffic_active", 0);
  tree_field_set_int(car, "traffic_live", 0);
  tree_field_set_int(car, "traffic_flag_220", 0);  // +0xDC
  tree_field_set_int(car, "traffic_gameinit", 0);  // W31D +0x138 soft
  tree_field_set_int(car, "traffic_timer_armed", 0);  // W35-04 +0xAC soft
  tree_field_set_float(car, "traffic_frame_stamp", 0.f);  // W35-05 +0xE8
  if (g.pool_active > 0) --g.pool_active;
  ++g.deactivate_n;
  // Soft PE Traffic_destroy freelist push @ 0x57902f (pool+0x10).
  traffic_pool_unbind_host(car);
  // PE: no pool array pop here — dead slots linger until
  // Traffic_pool_sweep_dead (W33D) / Traffic_pool_sort_compact.
}

// W27D — PE Traffic_trySpawnOnRandomPath @ 0x0057B420 free-space gate
// @ 0x57B5B7 (fcomp 0 → jbe @ 0x57B866 pool-free ret 0). PE walks path
// cars via +8 summing gaps with spawn args a2/a4/a5 (hardcoded 1/4/1 from
// addTrafficN @ 0x48429f); empty path uses path+0x74 (116) − a4 vs cursor
// a2 — reject when free ≤ 0 (pathLen ≤ a4+a2). Path car list / +0x74 still
// OOS (no Resources) — keep XZ proximity vs
// GroundTrafficState::traffic_cars (+ pending) with min sep a4+a2=5.f.
// Not pathfinding AI; halt +196 stays RoadSeg.occupied.
static bool traffic_spawn_free_space_ok(InvObject* self, float px, float pz,
                                       const std::vector<InvObject*>& pending) {
  constexpr float kA2 = 1.f;  // PE trySpawn a2 @ 0x57B4A5
  constexpr float kA4 = 4.f;  // PE trySpawn a4 @ 0x57B4D1
  constexpr float kMinSep = kA4 + kA2;
  constexpr float kMinSep2 = kMinSep * kMinSep;
  std::vector<InvObject*> cars;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    cars = ground(self).traffic_cars;
  }
  cars.insert(cars.end(), pending.begin(), pending.end());
  for (InvObject* car : cars) {
    if (!car) continue;
    // Soft: PhysicsRef first (Resources mutex), else GameRefState under
    // brief g_gr_mu — never GameRef_getPos (same Soft rule as sample_pos).
    float x = 0.f, y = 0.f, z = 0.f;
    if (!gameref_soft_phys_sample_pos(car, &x, &y, &z)) {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto it = g_refs.find(car);
      if (it == g_refs.end() || it->second.empty) continue;
      x = it->second.px;
      y = it->second.py;
      z = it->second.pz;
    }
    const float dx = x - px;
    const float dz = z - pz;
    if (dx * dx + dz * dz < kMinSep2) return false;
  }
  return true;
}

int32_t java_util_resource_GroundRef_addTrafficCar(InvObject* self, InvObject* instance,
                                                   InvObject* pos) {
  // PE @ 0x00484730 size 0x351. pos → Traffic_trySpawnNearCross @ 0x581E00
  // (nearest junction + empty path); null pos → pathGrid scan +0xA8==0 then
  // Traffic_trySpawnOnRandomPath(1,2,4,1). Success @ 0x484974: Rebind +0x130,
  // ch56 bind, +0xDD=ch69, setParent, speed +0x2C=(u*0.4+0.7)*27.777779,
  // scale +0x30, lane +0xA8=2u-1, +0x154=0, Traffic_activate @ 0x5791C0.
  // Return traffic* (host: sequential id → cars_by_id for rem/notify/behaviour).
  // W26D: lane + Traffic_activate sideband. W27D: free-space reject @ 0x57B5B7.
  // W28D: Traffic_activate_apply pool/car_rem soft PE. W29D: pool_slots.
  // W30D: sort_compact dead on rem. W31D: car_tick gates + sample/heap soft.
  if (!self || !instance) return 0;
  int32_t on_path = 0;
  if (pos) {
    float x = 0.f, y = 0.f, z = 0.f;
    vec3_get(pos, &x, &y, &z);
    float cx = x, cy = y, cz = z;
    if (InvObject* cross = physics_road_nearest_cross(x, y, z, 0.f))
      vec3_get(cross, &cx, &cy, &cz);
    float ox = cx, oy = cy, oz = cz, dx = 0.f, dy = 0.f, dz = 1.f;
    if (physics_road_project(cx, cz, &ox, &oy, &oz, &dx, &dy, &dz)) {
      // PE trySpawnNearCross → trySpawnOnRandomPath free-space; fail → 0.
      if (!traffic_spawn_free_space_ok(self, ox, oz, {})) return 0;
      java_util_resource_GameRef_setMatrix(
          instance, vec3_new(ox, oy, oz), ypr_new(std::atan2(dx, dz), 0.f, 0.f));
      on_path = 1;
    } else {
      java_util_resource_GameRef_setPos(instance, pos);
    }
  } else {
    float px = 0.f, py = 0.f, pz = 0.f, yaw = 0.f;
    if (physics_road_random_spawn(&px, &py, &pz, &yaw)) {
      if (!traffic_spawn_free_space_ok(self, px, pz, {})) return 0;
      java_util_resource_GameRef_setMatrix(instance, vec3_new(px, py, pz),
                                           ypr_new(yaw, 0.f, 0.f));
      on_path = 1;
    }
  }
  const int r1 = std::rand() & 0x7FFF;
  const int r2 = std::rand() & 0x7FFF;
  const int r3 = std::rand() & 0x7FFF;
  const float kRand15 = 1.f / 32768.f;
  const float u1 = static_cast<float>(r1) * kRand15;
  const float u2 = static_cast<float>(r2) * kRand15;
  const float u3 = static_cast<float>(r3) * kRand15;
  tree_field_set_float(instance, "traffic_speed", (u1 * 0.4f + 0.7f) * 27.777779f);
  tree_field_set_float(instance, "traffic_scale", u2 * 0.4f + 0.7f);
  tree_field_set_float(instance, "traffic_lane", u3 + u3 - 1.f);  // +0xA8
  tree_field_set_float(instance, "traffic_cursor", 0.f);  // Soft PE +0x88
  tree_field_set_int(instance, "traffic_color", 0);
  tree_field_set_int(instance, "traffic_flag_221", 1);  // ch69 getInfo OOS → 1
  // PE Traffic_activate @ 0x5791C0 → Traffic_activate_apply @ 0x579040
  // (pool/parent apply). W28D: soft pool counters. W29D: pool_slots push.
  // W30D: compact on rem (not here).
  tree_field_set_int(instance, "spawned_on_path", on_path);
  std::lock_guard<std::mutex> lock(g_gr_mu);
  GroundTrafficState& g = ground(self);
  traffic_activate_apply(instance, g);
  g.traffic_cars.push_back(instance);
  const int32_t id = g.next_car_id++;
  g.car_ids.push_back(id);
  g.cars_by_id[id] = instance;
  g.traffic_count += 1;
  g.path_spawns += on_path;
  ground_sync_fields(self);
  return id;
}

void java_util_resource_GroundRef_remTrafficCar(InvObject* self, int32_t id) {
  // PE @ 0x00484A90: Unbox I as traffic ptr; id==0 no-op. If +0x138 live,
  // type 0x38=56 on +0x130 then Traffic_destroy @ 0x00578F20 (unlink path,
  // return to pool). Java Bot.dummycar / City.startRace keep the GameRef —
  // do not ResourceRef.destroy.
  // W28D: Traffic_activate_apply deactivate sideband (car_rem if +0xDD).
  // W30D/W31D: soft Traffic_pool_sort_compact after deactivate (PE tick
  // @ 0x463c72 a2=1). Host rem: budget=0 so car_tick keeps live slots.
  // W32D: tick path (valocity_simulate) uses budget=1 separately.
  if (!self || id == 0) return;
  std::lock_guard<std::mutex> lock(g_gr_mu);
  GroundTrafficState& g = ground(self);
  auto mit = g.cars_by_id.find(id);
  if (mit == g.cars_by_id.end() || !mit->second) return;
  InvObject* inst = mit->second;
  traffic_deactivate_apply(inst, g);
  traffic_pool_sort_compact(g, /*budget=*/0, /*cam=*/nullptr);
  g.cars_by_id.erase(mit);
  g.car_behaviour.erase(id);
  for (auto it = g.car_ids.begin(); it != g.car_ids.end(); ++it) {
    if (*it == id) {
      g.car_ids.erase(it);
      break;
    }
  }
  for (auto it = g.traffic_cars.begin(); it != g.traffic_cars.end(); ++it) {
    if (*it == inst) {
      g.traffic_cars.erase(it);
      break;
    }
  }
  if (g.traffic_count > 0) --g.traffic_count;
  ground_sync_fields(self);
}

int32_t java_util_resource_GroundRef_notifyTrafficCar(InvObject* self, int32_t id,
                                                      int32_t state) {
  // PE @ 0x00484AF0 size 0x52 (int_convert 82). UnboxArg: state→var_8,
  // id→arg_0 (traffic*), this→var_4 (unread). id==0 → 0. If state>=0
  // (test ecx / jl @ 0x484b25) write (byte)state to traffic*+0xDD
  // (int_convert 221). Return MOVSX byte+0xDD. No GroundMap / +0x138 gate.
  // Valocity.enter: notifyTrafficCar(traffic_id, 1) after addTrafficCar
  // dummycar — must KEEP flag=1 (old host forced 0; Hex-Rays lied).
  // W26D PE deepen — residual after W25D addTrafficN.
  if (!self || id == 0) return 0;
  std::lock_guard<std::mutex> lock(g_gr_mu);
  GroundTrafficState& g = ground(self);
  auto it = g.cars_by_id.find(id);
  if (it == g.cars_by_id.end() || !it->second) return 0;
  if (state >= 0)
    tree_field_set_int(it->second, "traffic_flag_221", state & 0xFF);
  const int32_t flag = tree_field_get_int(it->second, "traffic_flag_221") & 0xFF;
  return static_cast<int32_t>(static_cast<int8_t>(static_cast<uint8_t>(flag)));
}

int32_t java_util_resource_GroundRef_addTrafficN(InvObject* self, InvObject* type,
                                                 int32_t n, float lenBegin,
                                                 float lenEnd, float wheelBase) {
  // PE @ 0x00484050 size 0x3CC (int_convert 972). Unbox this+type+n+
  // lenBegin+lenEnd+wheelBase (stack defs n=1, lb=1.0, le=4.0, wb=2.0 —
  // match GroundRef.java addTraffic defaults). Native.ptr → ch57=GroundMap;
  // n<=0 or map==0 → 0. Config.trafficDensity via sub_4261F0; templateN =
  // min(n, Engine_ftol(dens*20)) LoadGameInit("traffic_car","0,-10000…") +
  // sub_584C60. Spawn loop i=0..n-1 @ 0x484247: CRT_rand path pick then
  // Traffic_trySpawnOnRandomPath(1.0,2.0,4.0,1.0) HARDCODED (Java FFF not
  // geometry). Success @ 0x4842A3: Rebind type; color/speed/scale; pack
  // +0x14=lb +0x18=wb(+ch64) +0x1C=ch70(def 0.5) +0x20=le +0x24=1.0;
  // +0xA8=2*u-1 lane; +0xDD=ch69; Traffic_activate @ 0x5791C0; ++spawned.
  // Return spawned (var_44) — host keeps Java n for Valocity smoke 847/183.
  if (!self || n <= 0) return 0;

  float dens = 1.f;
  if (InvObject* cfg = system_config_host())
    dens = tree_field_get_float(cfg, "trafficDensity");
  // PE @ 0x484197: templateN = min(n, Engine_ftol(dens * 20.0)).
  int32_t template_n = static_cast<int32_t>(dens * 20.f);
  if (template_n > n) template_n = n;
  if (template_n < 0) template_n = 0;

  // ch64/69/70 type queries OOS — keep unboxed FFF; ch70 stack def 0.5.
  constexpr float kCh70Default = 0.5f;
  constexpr float kParam24 = 1.f;
  constexpr float kRand15 = 1.f / 32768.f;

  std::vector<InvObject*> created;
  std::vector<InvObject*> pending_pos;  // W27D free-space vs in-flight insts
  int32_t on_path = 0;
  // PE spawn loop runs n times (disasm cmp i,n / jl @ 0x48439d) — not once.
  // Each success → pool car; host materializes a traffic_car GameRef.
  // W27D: free-space reject @ 0x57B5B7 (halt +196 still RoadSeg.occupied).
  for (int32_t i = 0; i < n; ++i) {
    if (!type) continue;
    float px = 0.f, py = 0.f, pz = 0.f, yaw = 0.f;
    if (!physics_road_random_spawn(&px, &py, &pz, &yaw)) continue;
    if (!traffic_spawn_free_space_ok(self, px, pz, pending_pos)) continue;
    InvObject* wrapper = gameref_new();
    InvObject* inst = java_util_resource_GameRef_create(
        wrapper, self, type, string_new("0,-10000,0,0,0,0"),
        string_new("traffic_car"));
    if (!inst) continue;
    java_util_resource_GameRef_setMatrix(inst, vec3_new(px, py, pz),
                                         ypr_new(yaw, 0.f, 0.f));
    const int r0 = std::rand() & 0x7FFF;
    const int r1 = std::rand() & 0x7FFF;
    const int r2 = std::rand() & 0x7FFF;
    const int r3 = std::rand() & 0x7FFF;
    const float u1 = static_cast<float>(r1) * kRand15;
    const float u2 = static_cast<float>(r2) * kRand15;
    const float u3 = static_cast<float>(r3) * kRand15;
    tree_field_set_int(inst, "traffic_color", (0xFFFF * r0) / 0x8000);
    tree_field_set_float(inst, "traffic_speed", (u1 * 0.4f + 0.7f) * 19.444445f);
    tree_field_set_float(inst, "traffic_scale", u2 * 0.4f + 0.7f);
    // PE @ 0x484336 — Java FFF onto traffic blob (geometry used 1/2/4/1).
    tree_field_set_float(inst, "traffic_len_begin", lenBegin);
    tree_field_set_float(inst, "traffic_wheelbase", wheelBase);
    tree_field_set_float(inst, "traffic_param_1c", kCh70Default);
    tree_field_set_float(inst, "traffic_len_end", lenEnd);
    tree_field_set_float(inst, "traffic_param_24", kParam24);
    tree_field_set_float(inst, "traffic_lane", u3 + u3 - 1.f);  // +0xA8
    tree_field_set_float(inst, "traffic_cursor", 0.f);  // Soft PE +0x88
    tree_field_set_int(inst, "traffic_flag_221", 1);            // ch69 OOS
    tree_field_set_int(inst, "spawned_on_path", 1);
    created.push_back(inst);
    pending_pos.push_back(inst);
    if (wrapper && wrapper != inst) created.push_back(wrapper);
    ++on_path;
  }
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    GroundTrafficState& g = ground(self);
    for (InvObject* c : created) {
      // PE @ 0x48438b Traffic_activate per success (skip wrappers).
      if (c && tree_field_get_int(c, "spawned_on_path") == 1)
        traffic_activate_apply(c, g);
      g.traffic_cars.push_back(c);
    }
    g.traffic_count += n;
    g.traffic_streams += 1;
    g.path_spawns += on_path;
    ground_sync_fields(self);
  }
  tree_field_set_int(self, "traffic_template_n", template_n);
  tree_field_set_int(self, "traffic_spawned", on_path);
  return n;
}

int32_t java_util_resource_GroundRef_addTrafficP(InvObject* self, InvObject* type,
                                                 InvObject* pos, int32_t n,
                                                 float lenBegin, float lenEnd,
                                                 float wheelBase) {
  (void)n;
  (void)lenBegin;
  (void)lenEnd;
  (void)wheelBase;
  // PE @ 0x00484420: Unbox Vector3 x/y/z; loop once (v15<1 — Java n unused)
  // Traffic_trySpawnNearCross @ 0x00581E00 with hardcoded 0, 1.0, 2.0, 4.0,
  // 1.0 (not lenBegin/lenEnd/wheelBase). Color (0xFFFF*rand15)/0x8000.
  // Speed (rand15/32768*0.4+0.7)*19.444445. Return spawned 0|1.
  // W27D: free-space reject via trySpawnOnRandomPath @ 0x57B5B7.
  if (!self || !type || !pos) return 0;
  float x = 0.f, y = 0.f, z = 0.f;
  vec3_get(pos, &x, &y, &z);
  float cx = x, cy = y, cz = z;
  if (InvObject* cross = physics_road_nearest_cross(x, y, z, 0.f))
    vec3_get(cross, &cx, &cy, &cz);
  float ox = cx, oy = cy, oz = cz, dx = 0.f, dy = 0.f, dz = 1.f;
  int32_t on_path = 0;
  if (!physics_road_project(cx, cz, &ox, &oy, &oz, &dx, &dy, &dz)) {
    tree_field_set_int(self, "traffic_p_ok", 0);
    return 0;
  }
  if (!traffic_spawn_free_space_ok(self, ox, oz, {})) {
    tree_field_set_int(self, "traffic_p_ok", 0);
    return 0;
  }
  InvObject* wrapper = gameref_new();
  InvObject* inst = java_util_resource_GameRef_create(
      wrapper, self, type, string_new("0,-10000,0,0,0,0"),
      string_new("traffic_car"));
  if (!inst) return 0;
  {
    java_util_resource_GameRef_setMatrix(
        inst, vec3_new(ox, oy, oz), ypr_new(std::atan2(dx, dz), 0.f, 0.f));
    on_path = 1;
    const int r0 = std::rand() & 0x7FFF;
    const int r1 = std::rand() & 0x7FFF;
    const int r2 = std::rand() & 0x7FFF;
    const int r3 = std::rand() & 0x7FFF;
    const float kRand15 = 1.f / 32768.f;
    const float u1 = static_cast<float>(r1) * kRand15;
    const float u2 = static_cast<float>(r2) * kRand15;
    const float u3 = static_cast<float>(r3) * kRand15;
    tree_field_set_int(inst, "traffic_color", (0xFFFF * r0) / 0x8000);
    tree_field_set_float(inst, "traffic_speed", (u1 * 0.4f + 0.7f) * 19.444445f);
    tree_field_set_float(inst, "traffic_scale", u2 * 0.4f + 0.7f);
    tree_field_set_float(inst, "traffic_len_begin", 1.f);
    tree_field_set_float(inst, "traffic_len_end", 2.f);
    tree_field_set_float(inst, "traffic_wheelbase", 4.f);
    tree_field_set_float(inst, "traffic_lane", u3 + u3 - 1.f);  // +0xA8
    tree_field_set_float(inst, "traffic_cursor", 0.f);  // Soft PE +0x88
    tree_field_set_int(inst, "traffic_flag_221", 1);           // ch69 OOS
    tree_field_set_int(inst, "spawned_on_path", 1);
  }
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    GroundTrafficState& g = ground(self);
    if (on_path) {
      // PE @ 0x4846a7 Traffic_activate after +0xDD / lane.
      traffic_activate_apply(inst, g);
      g.traffic_cars.push_back(inst);
      if (wrapper && wrapper != inst) g.traffic_cars.push_back(wrapper);
      g.traffic_count += 1;
      g.traffic_streams += 1;
      g.path_spawns += 1;
    }
    ground_sync_fields(self);
  }
  tree_field_set_int(self, "traffic_p_ok", on_path);
  tree_field_set_float(self, "traffic_p_x", ox);
  tree_field_set_float(self, "traffic_p_y", oy);
  tree_field_set_float(self, "traffic_p_z", oz);
  return on_path;
}

void java_util_resource_GroundRef_delTraffic(InvObject* self) {
  // PE @ 0x00484B50 size 0x3e: Unbox this. Native.ptr →
  // Engine_queryGameRefChannel(handle, 0x39=57, 0); fail → ret. Else
  // GroundMap_delTraffic @ 0x00581480: unbind type 56, Traffic_detach,
  // zero occupancy, count=0. Does not ResourceRef.destroy. addTrafficCar
  // GameRefs (cars_by_id) stay alive — Bot.dummycar. Host addTrafficN/P
  // wrappers are native stand-ins and are destroyed to avoid leaking the
  // 847-car spawn. race123: channel-57 gate via g_grounds always proceeds;
  // PE GroundMap_delTraffic @ 0x00581480 still OOS (occupancy zero stand-in).
  // race125: deepen_partial - channel-57 / occupancy zero stand-in.
  if (!self) return;
  std::vector<InvObject*> cars;
  std::vector<InvObject*> bound;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    GroundTrafficState& g = ground(self);
    for (const auto& kv : g.cars_by_id) {
      if (kv.second) {
        // PE delTraffic keeps GameRef; clear activate sideband only.
        tree_field_set_int(kv.second, "traffic_active", 0);
        tree_field_set_int(kv.second, "traffic_live", 0);
        tree_field_set_int(kv.second, "traffic_flag_220", 0);
        tree_field_set_int(kv.second, "traffic_gameinit", 0);
        tree_field_set_int(kv.second, "traffic_timer_armed", 0);
        bound.push_back(kv.second);
      }
    }
    cars.swap(g.traffic_cars);
    g.traffic_count = 0;
    g.traffic_streams = 0;
    g.path_spawns = 0;
    g.pool_active = 0;  // W28D: g_TrafficPoolActive stand-in
    g.pool_slots.clear();  // W29D: eng+0x310C / +0x3110 (CarContainer_ctor zero)
    // Soft PE Traffic_destroy freelist @ 0x57902f for bound blobs; then
    // GroundMap_delTraffic @ 0x58172d: Traffic_pool_alloc+car_ctor → +0x3118.
    for (auto& kv : g_soft_car_blob) {
      if (kv.second) traffic_pool_free(kv.second);
    }
    g_soft_car_blob.clear();
    if (g_soft_default_car) {
      traffic_pool_free(g_soft_default_car);
      g_soft_default_car = nullptr;
    }
    g_soft_default_car = traffic_car_ctor(traffic_pool_alloc());
    // W35-04 — eng+0/+4 timer heap zero (CarContainer teardown stand-in).
    g.timer_heap.clear();
    g.timer_heap_pops = 0;
    // W34 — eng+0x98 live → clear → freelist eng+0xAC (delTraffic teardown).
    while (g.pathnodes_live) {
      GroundTrafficState::TrafficPathNode* n = g.pathnodes_live;
      g.pathnodes_live = n->list_next;
      traffic_pathnode_clear(n, g);
      n->list_next = g.pathnodes_free;
      g.pathnodes_free = n;
    }
    g.car_ids.clear();
    g.cars_by_id.clear();
    g.car_behaviour.clear();
    ground_sync_fields(self);
  }
  for (InvObject* c : cars) {
    if (!c) continue;
    bool keep = false;
    for (InvObject* b : bound) {
      if (b == c) {
        keep = true;
        break;
      }
    }
    if (!keep) java_util_resource_ResourceRef_destroy(c);
  }
}

void java_util_resource_GroundRef_setPedestrianDensityN(InvObject* self, float d) {
  // PE @ 0x00484D30: Unbox F; type 0x3D=61 gate; Pedestrian_setDensity @
  // 0x00589B80 stores d and d*1.1 (flt_5F0C6C) to g_pedestrianDensity /
  // g_pedestrianDensityHi. Java already multiplied Config.pedestrianDensity.
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_gr_mu);
  GroundTrafficState& g = ground(self);
  g.ped_density = d;
  g.ped_density_hi = d * 1.1f;
  ground_sync_fields(self);
}

void java_util_resource_GroundRef_addPedestrianType(InvObject* self, InvObject* g) {
  // PE @ 0x00484D90: type 0x3D=61 gate; prep map native GameRef at this+0xC;
  // Pedestrian_addType @ 0x00589940: 32-slot table, skip duplicate obj+8.
  // Host: skip duplicate type_id; keep origin sample for pedestrianDistance
  // (Phase 2.84). Mesh vtable / sub_5447D0 not mirrored.
  if (!self) return;
  const int32_t type_id = g ? java_util_resource_ResourceRef_id(g) : 0;
  std::lock_guard<std::mutex> lock(g_gr_mu);
  GroundTrafficState& st = ground(self);
  if (st.ped_types >= 32) return;
  for (const auto& s : st.ped_samples) {
    if (s.type_id == type_id) return;
  }
  st.ped_types += 1;
  GroundTrafficState::PedSample sample;
  sample.type_id = type_id;
  sample.x = sample.y = sample.z = 0.f;
  st.ped_samples.push_back(sample);
  ground_sync_fields(self);
}

void java_util_resource_GroundRef_remPedestrianType(InvObject* self, InvObject* g) {
  // PE @ 0x00484E20 size 0x49 (int_convert 73) — same body as
  // removePedestrianType (PE name string). Unbox dest0=this dest1=g.
  // Gate: Native.ptr on THIS + Engine_queryGameRefChannel(handle, 0x3D=61,
  // 0); fail → early out. Pedestrian_remType @ 0x00589A20 (ecx=channel,
  // push g): scan 32-slot table keyed [g+8]; miss=no-op; hit compact.
  // Host: no channel-61 query (always proceed); erase matching type_id
  // (same key as add). List unlink +0x48 not mirrored.
  if (!self) return;
  const int32_t type_id = g ? java_util_resource_ResourceRef_id(g) : 0;
  std::lock_guard<std::mutex> lock(g_gr_mu);
  GroundTrafficState& st = ground(self);
  for (auto it = st.ped_samples.begin(); it != st.ped_samples.end(); ++it) {
    if (it->type_id != type_id) continue;
    st.ped_samples.erase(it);
    if (st.ped_types > 0) --st.ped_types;
    ground_sync_fields(self);
    return;
  }
}

void java_util_resource_GroundRef_setWater(InvObject* self, float level,
                                          float density, float viscosity) {
  // PE @ 0x004866C0 size 0xe6:
  // GroundRef.setWater(FFF)V — table sig (FFF)V @ 0x0061659C (NOT Vector3).
  // UnboxArg(ci, &this, &level→point.y, &density, &viscosity). Defaults
  // before unbox: point=(0,-12,0) normal=(0,1,0) dens=300 visc=550
  // (imm 0xC1400000 / 0x3F800000 / 0x43960000 / 0x44098000).
  // Handle=*[vm_get_int_field(this, Native_ptr)+0xC]; null → early out.
  // dens<=0 || visc<=0 → [Engine_simTime+0x80]=0 (no Engine_setWater);
  // else Engine_setWater @ 0x0049B440 (ecx=Engine_simTime, dens, visc,
  // &normal, &point, 0) writes dens/visc @ +0x1E4/+0x1E8, normal @ +0x1C4
  // (normalize), point @ +0x1B8, frees water-limit array +0x1D0/+0x1D4=0;
  // then flag=1.
  // Contrast VVFF @ 0x004867B0: same gate + Engine_setWater + flag; but
  // UnboxArg(this, point, normal, dens, visc) then vm_get_float_field x/y/z.
  // Contrast addWaterLimit @ 0x00486920: Engine_addWaterLimit only — no
  // dens/visc, no normalize, no [simTime+0x80].
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_gr_mu);
  GroundTrafficState& g = ground(self);
  if (density <= 0.f || viscosity <= 0.f) {
    tree_field_set_int(self, "water_enabled", 0);
    return;
  }
  g.water_px = 0.f;
  g.water_py = level;
  g.water_pz = 0.f;
  g.water_nx = 0.f;
  g.water_ny = 1.f;
  g.water_nz = 0.f;
  g.water_density = density;
  g.water_viscosity = viscosity;
  g.water_plane = true;
  g.water_level = level;
  g.water_limits.clear();
  tree_field_set_int(self, "water_enabled", 1);
  ground_sync_fields(self);
}

void java_util_resource_GroundRef_setWater_1(InvObject* self, InvObject* point,
                                            InvObject* normal, float density,
                                            float viscosity) {
  // PE @ 0x004867B0 size 0x16B:
  // GroundRef.setWater(Ljava.lang.Vector3;Ljava.lang.Vector3;FF)V.
  // Contrast FFF @ 0x004866C0: UnboxArg(this, level→point.y, dens, visc)
  // with defaults point=(0,-12,0) normal=(0,1,0) — no vm_get_float_field.
  // This overload: UnboxArg(this, point, normal, dens, visc); same defaults
  // dens=300 visc=550; handle = *[vm_get_int_field(this, Native_ptr)+0xC];
  // dens<=0 || visc<=0 → [Engine_simTime+0x80]=0 (no Engine_setWater);
  // else read Vector3 x/y/z via vm_get_float_field, then Engine_setWater
  // @ 0x0049B440 (ecx=Engine_simTime, dens, visc, &normal, &point, 0) which
  // writes dens/visc @ +0x1E4/+0x1E8, normal @ +0x1C4 (normalize), point @
  // +0x1B8, frees water-limit array +0x1D0/+0x1D4=0; then flag=1.
  // Distinct from addWaterLimit @ 0x00486920.
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_gr_mu);
  GroundTrafficState& g = ground(self);
  if (density <= 0.f || viscosity <= 0.f) {
    tree_field_set_int(self, "water_enabled", 0);
    return;
  }
  float px = 0.f, py = -12.f, pz = 0.f;
  float nx = 0.f, ny = 1.f, nz = 0.f;
  if (point) vec3_get(point, &px, &py, &pz);
  if (normal) vec3_get(normal, &nx, &ny, &nz);
  g.water_px = px;
  g.water_py = py;
  g.water_pz = pz;
  g.water_nx = nx;
  g.water_ny = ny;
  g.water_nz = nz;
  g.water_density = density;
  g.water_viscosity = viscosity;
  g.water_plane = true;
  g.water_level = py;
  g.water_limits.clear();
  tree_field_set_int(self, "water_enabled", 1);
  ground_sync_fields(self);
}

void java_util_resource_GroundRef_addWaterLimit(InvObject* self, InvObject* point,
                                               InvObject* normal) {
  // PE @ 0x00486920 size 0xF9:
  // GroundRef.addWaterLimit(Ljava.lang.Vector3;Ljava.lang.Vector3;)V.
  // Contrast setWater(FFF) @ 0x004866C0: UnboxArg(this, level→point.y, dens,
  // visc) with same defaults point=(0,-12,0) normal=(0,1,0); dens<=0||visc<=0
  // → [Engine_simTime+0x80]=0 else Engine_setWater @ 0x0049B440 (writes dens/
  // visc @ +0x1E4/+0x1E8, normalizes normal @ +0x1C4, point @ +0x1B8, frees
  // water-limit array +0x1D0/+0x1D4=0) then flag=1.
  // This native: UnboxArg(this, point, normal); same defaults; handle =
  // *[vm_get_int_field(this, Native_ptr)+0xC]; if handle: read Vector3 x/y/z
  // via vm_get_float_field then Engine_addWaterLimit @ 0x0049B530
  // (ecx=Engine_simTime) appends 24-byte {point,normal} to +0x1D0, ++count
  // +0x1D4 — NO dens/visc, NO normalize, NO [simTime+0x80]. Ret index discarded.
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_gr_mu);
  GroundTrafficState::WaterLimit lim;
  if (point) vec3_get(point, &lim.px, &lim.py, &lim.pz);
  if (normal) vec3_get(normal, &lim.nx, &lim.ny, &lim.nz);
  ground(self).water_limits.push_back(lim);
  ground_sync_fields(self);
}

void java_util_resource_GroundRef_setTrafficCarBehaviour(InvObject* self,
                                                        int32_t id,
                                                        int32_t mode) {
  // PE @ 0x00487EC0: UnboxArg writes id over CallInfo; if (id && *(id+0x138))
  // *(id+0x160)=mode. Java TC_ACTIVE=1 TC_PASSIVE=2. GroundRef unused after unbox.
  if (!self || id == 0) return;
  std::lock_guard<std::mutex> lock(g_gr_mu);
  GroundTrafficState& g = ground(self);
  auto it = g.cars_by_id.find(id);
  if (it == g.cars_by_id.end() || !it->second) return;
  g.car_behaviour[id] = mode;
  tree_field_set_int(it->second, "traffic_behaviour", mode);
  tree_field_set_int(self, "traffic_behaviour_last", mode);
}

// PE Traffic_evictFromCross @ 0x0057BFA0: cars whose nearest junction is this
// cross are despawned or Traffic_trySpawnNearCross(..., 100.0). Host: project
// ~100 m away. Soft pose via PhysicsRef / GameRefState — never hold g_gr_mu
// across getPos (non-recursive); Soft sample outside lock.
static int32_t ground_evict_cars_at_cross(float cx, float cy, float cz,
                                          const std::vector<InvObject*>& cars) {
  int32_t cleared = 0;
  for (InvObject* car : cars) {
    if (!car) continue;
    float px = 0.f, py = 0.f, pz = 0.f;
    if (!gameref_soft_phys_sample_pos(car, &px, &py, &pz)) {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto it = g_refs.find(car);
      if (it == g_refs.end() || it->second.empty) continue;
      px = it->second.px;
      py = it->second.py;
      pz = it->second.pz;
    }
    float nx = px, ny = py, nz = pz;
    if (InvObject* nc = physics_road_nearest_cross(px, py, pz, 0.f))
      vec3_get(nc, &nx, &ny, &nz);
    const float ddx = nx - cx;
    const float ddz = nz - cz;
    if (ddx * ddx + ddz * ddz > 1.f) continue;
    float tx = cx, ty = cy, tz = cz;
    if (InvObject* farc = physics_road_nearest_cross(cx, cy, cz, 100.f))
      vec3_get(farc, &tx, &ty, &tz);
    float ox = tx, oy = ty, oz = tz, dx = 0.f, dy = 0.f, dz = 1.f;
    if (physics_road_project(tx, tz, &ox, &oy, &oz, &dx, &dy, &dz)) {
      java_util_resource_GameRef_setMatrix(
          car, vec3_new(ox, oy, oz), ypr_new(std::atan2(dx, dz), 0.f, 0.f));
      ++cleared;
    }
  }
  return cleared;
}

void java_util_resource_GroundRef_haltTrafficCross(InvObject* self, InvObject* pos,
                                                  float time) {
  // PE @ 0x00484B90: GroundMap_findNearestCross(xyz, 0, 0) then
  // GroundMap_haltCrossTraffic @ 0x0057C170 (duration=time, flag=1).
  // cross+64 = now + duration. Traffic_evictFromCross @ 0x0057BFA0:
  // despawn or Traffic_trySpawnNearCross(..., 100.0, ...).
  if (!self) return;
  float x = 0.f, y = 0.f, z = 0.f;
  if (pos) vec3_get(pos, &x, &y, &z);
  float cx = x, cy = y, cz = z;
  if (InvObject* cross = physics_road_nearest_cross(x, y, z, 0.f))
    vec3_get(cross, &cx, &cy, &cz);

  std::vector<InvObject*> cars;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    GroundTrafficState& g = ground(self);
    GroundTrafficState::HaltCross h;
    h.x = cx;
    h.y = cy;
    h.z = cz;
    h.time = time;
    g.halt_crosses.push_back(h);
    cars = g.traffic_cars;
    ground_sync_fields(self);
  }
  tree_field_set_float(self, "halt_until", game_logic_time() + time);
  tree_field_set_float(self, "halt_cx", cx);
  tree_field_set_float(self, "halt_cz", cz);
  tree_field_set_int(self, "halt_cleared",
                     ground_evict_cars_at_cross(cx, cy, cz, cars));
}

void java_util_resource_GroundRef_haltTrafficPath(InvObject* self, InvObject* p1,
                                                 InvObject* p2) {
  // PE @ 0x004835E0: Unbox two V3; GroundMap type 0x39=57;
  // GroundMap_haltTrafficPath @ 0x00583FD0: GroundMap_findRoute then per
  // waypoint haltCrossTraffic(0.001, 1) + markPathOccupied (+196). Duration
  // 0.001 evicts now; spawn skip is path+196 (host RoadSeg.occupied).
  // Do not push halt_crosses — Phase 2.84 counts those from haltTrafficCross.
  if (!self || !p1 || !p2) return;
  float x1 = 0.f, y1 = 0.f, z1 = 0.f, x2 = 0.f, y2 = 0.f, z2 = 0.f;
  vec3_get(p1, &x1, &y1, &z1);
  vec3_get(p2, &x2, &y2, &z2);
  std::vector<InvObject*> cars;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    GroundTrafficState::HaltPath h;
    h.x1 = x1;
    h.y1 = y1;
    h.z1 = z1;
    h.x2 = x2;
    h.y2 = y2;
    h.z2 = z2;
    GroundTrafficState& g = ground(self);
    g.halt_paths.push_back(h);
    cars = g.traffic_cars;
    ground_sync_fields(self);
  }
  physics_road_route_length(x1, y1, z1, x2, y2, z2);
  struct PathCross {
    float x = 0.f, y = 0.f, z = 0.f;
  };
  std::vector<PathCross> crosses;
  auto add_cross = [&](float x, float y, float z) {
    for (const PathCross& c : crosses) {
      const float dx = c.x - x;
      const float dz = c.z - z;
      if (dx * dx + dz * dz <= 1.f) return;
    }
    crosses.push_back(PathCross{x, y, z});
  };
  const int32_t n = physics_road_last_route_count();
  if (n <= 0) {
    float cx = x1, cy = y1, cz = z1;
    if (InvObject* c = physics_road_nearest_cross(x1, y1, z1, 0.f))
      vec3_get(c, &cx, &cy, &cz);
    add_cross(cx, cy, cz);
    cx = x2;
    cy = y2;
    cz = z2;
    if (InvObject* c = physics_road_nearest_cross(x2, y2, z2, 0.f))
      vec3_get(c, &cx, &cy, &cz);
    add_cross(cx, cy, cz);
  } else {
    for (int32_t i = 0; i < n; ++i) {
      float rx = 0.f, ry = 0.f, rz = 0.f;
      if (!physics_road_last_route_point(i, &rx, &ry, &rz)) continue;
      float cx = rx, cy = ry, cz = rz;
      if (InvObject* c = physics_road_nearest_cross(rx, ry, rz, 0.f))
        vec3_get(c, &cx, &cy, &cz);
      add_cross(cx, cy, cz);
    }
  }
  int32_t cleared = 0;
  for (const PathCross& c : crosses)
    cleared += ground_evict_cars_at_cross(c.x, c.y, c.z, cars);
  // PE zeros all path+196 then GroundMap_markPathOccupied per cross adj path.
  // BFS of unmarked neighbors not mirrored.
  physics_road_clear_occupied();
  for (const PathCross& c : crosses)
    physics_road_mark_occupied_at(c.x, c.y, c.z);
  tree_field_set_int(self, "halt_path_cleared", cleared);
  tree_field_set_int(self, "halt_path_crosses",
                     static_cast<int32_t>(crosses.size()));
  tree_field_set_int(self, "halt_path_occupied", physics_road_occupied_count());
}

float java_util_resource_GroundRef_pedestrianDistance(InvObject* self,
                                                     InvObject* pos,
                                                     int32_t typeID) {
  // PE @ 0x00484C60: type 0x3D=61 gate else -1.0 (flt_5F0C70).
  // Pedestrian_distance @ 0x00589BE0: live list this+0x3398; typeID==0 or
  // ped+0xC0==typeID; min dist via sub_5862E0; empty/miss → -1.0.
  // Host: ped_samples from addPedestrianType (origin placeholders).
  // Spawn list / terrain transform not mirrored.
  if (!self || !pos) return -1.f;
  float px = 0, py = 0, pz = 0;
  vec3_get(pos, &px, &py, &pz);
  std::lock_guard<std::mutex> lock(g_gr_mu);
  const GroundTrafficState& g = ground(self);
  float best = -1.f;
  for (const auto& s : g.ped_samples) {
    if (typeID != 0 && s.type_id != typeID) continue;
    const float dx = px - s.x;
    const float dy = py - s.y;
    const float dz = pz - s.z;
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (best < 0.f || d2 < best) best = d2;
  }
  if (best < 0.f) return -1.f;
  return std::sqrt(best);
}

}  // namespace inv
