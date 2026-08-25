#include "natives.hpp"
#include "runtime.hpp"
#include "tree_interp.hpp"
#include "jvm.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace inv {
namespace {

std::mutex g_mu;

struct GameTypeState {
  int32_t event_mask = 0;
  int32_t native_created = 0;
  int32_t type_id = 0;
  InvObject* parent = nullptr;
  struct Timer {
    float deadline = 0;
    int32_t id = 0;
  };
  std::vector<Timer> timers;
  // Phase 2.85 — event watchers (Track/City/Osd).
  struct Notification {
    InvObject* ref = nullptr;
    int32_t etype = 0;
    int32_t ealias = 0;
    std::string custmsg;
    std::string custmethod;
  };
  std::vector<Notification> notifs;
};

std::unordered_map<InvObject*, GameTypeState> g_gt;

GameTypeState& st(InvObject* self) { return g_gt[self]; }

void sync(InvObject* self) {
  if (!self) return;
  GameTypeState& s = st(self);
  tree_field_set_int(self, "event_mask", s.event_mask);
  tree_field_set_int(self, "timer_count", static_cast<int32_t>(s.timers.size()));
  tree_field_set_int(self, "native_created", s.native_created);
  tree_field_set_int(self, "notif_count", static_cast<int32_t>(s.notifs.size()));
  if (!s.timers.empty()) {
    tree_field_set_float(self, "timer0_deadline", s.timers[0].deadline);
    tree_field_set_int(self, "timer0_id", s.timers[0].id);
  }
  if (!s.notifs.empty()) {
    tree_field_set_obj(self, "notif0_ref", s.notifs[0].ref);
    tree_field_set_int(self, "notif0_etype", s.notifs[0].etype);
    tree_field_set_int(self, "notif0_ealias", s.notifs[0].ealias);
    tree_field_set_obj(self, "notif0_method",
                       string_new(s.notifs[0].custmethod.c_str()));
  }
}

constexpr int32_t kEventCursor = 0x00010000;

// GII_* callback slots (GameType.java): stock keeps three engine linked lists.
constexpr int32_t kCallbackControl = 1;   // GII_CONTROL = 8
constexpr int32_t kCallbackDrive = 2;     // GII_DRIVE = 9
constexpr int32_t kCallbackAnimate = 4;   // GII_ANIMATE = 28

int32_t callback_mode_bit(int32_t mode) {
  if (mode == 8) return kCallbackControl;
  if (mode == 9) return kCallbackDrive;
  if (mode == 28) return kCallbackAnimate;
  return 0;
}

void invoke_cursor_handler(InvObject* handler, InvObject* obj_ref,
                           InvObject* param, const char* method) {
  if (!handler || !param) return;
  tree_field_set_int(handler, "last_event", kEventCursor);
  tree_field_set_int(handler, "cursor_event_count",
                     tree_field_get_int(handler, "cursor_event_count") + 1);
  tree_field_set_obj(handler, "last_cursor_param", param);
  Jvm* j = jvm_active();
  const char* cn = tree_host_class(handler);
  if (!j || !cn || !cn[0]) return;
  const char* name = (method && method[0]) ? method : "handleEvent";
  const char* sig = nullptr;
  const JvmClass* cls = j->find_class(cn);
  while (cls) {
    for (const JvmMethod& m : cls->methods) {
      if (m.name != name) continue;
      if (m.signature.find("Hotkey") != std::string::npos) continue;
      if (m.signature.find("String") == std::string::npos) continue;
      sig = m.signature.c_str();
      break;
    }
    if (sig) break;
    if (cls->super_name.empty()) break;
    cls = j->find_class(cls->super_name.c_str());
  }
  if (!sig) return;
  std::vector<JvmValue> args = {JvmValue::make_obj(handler),
                                JvmValue::make_obj(obj_ref),
                                JvmValue::make_int(kEventCursor),
                                JvmValue::make_obj(param)};
  j->invoke(cn, name, sig, args, false);
}

}  // namespace

void java_lang_GameType_remNotification(InvObject* self, InvObject* ref,
                                        int32_t etype) {
  // PE @ 0x0047E000 size 0x4b (75): Unbox dest0=this, dest1=GameRef,
  // dest2=etype. dest1==0 → ret. Native.ptr (dword_62E008) on this ==0 →
  // ret (NO Mighty / no Watch — unlike add @ 0x0047E050). Else thiscall
  // GameType_remNotificationWatch @ 0x0048B670 size 0xd3 (ECX=GameRef,
  // handle, etype): walk instance+0x5C; first match only
  // (listener=*(handle+8), etype exact @ +0x1C) unlink → free-list
  // dword_6363F0.
  // Gaps (host stand-in; no invent Watch/blob APIs):
  // - no Native.ptr gate: still erase (script GameType City/Track/Osd
  //   often has no blob; stock would silent-ret).
  // - match key = InvObject* ref + etype (not *(handle+8) listener).
  // - list on GameType self (not GameRef instance+0x5C / free-list).
  if (!self) return;
  if (!ref) return;  // stock dest1==0
  std::lock_guard<std::mutex> lock(g_mu);
  auto& n = st(self).notifs;
  for (auto it = n.begin(); it != n.end(); ++it) {
    if (it->ref == ref && it->etype == etype) {
      n.erase(it);  // stock Watch: first match only
      break;
    }
  }
  sync(self);
}

void java_lang_GameType_addNotification(InvObject* self, InvObject* ref,
                                        int32_t etype, int32_t ealias,
                                        InvObject* custmsg) {
  // PE @ 0x0047E050 size 0xaa (170): Unbox dest0=this, dest1=GameRef,
  // dest2=etype, dest3=ealias, dest4=custmsg. dest1==0 → ret. Native.ptr
  // (dword_62E008) on this ==0 → Mighty ERROR ("!" @ 0x00612F44 +
  // "Mighty ERROR" @ 0x00612F48 via CRT_strcat_n_thunk +
  // Engine_ErrorLogPrintf) — stock does NOT Watch. Contrast rem @
  // 0x0047E000 (race110): ptr==0 → silent ret (no Mighty / no Watch).
  // Host still insert (script GameType City/Track/Osd often has no blob).
  // Else thiscall GameType_addNotificationWatch @ 0x0048B500 size 0x16b
  // (ECX=GameRef, stack handle/etype/ealias/custmsg/0). Watch:
  // GameRef+8/+0C gate; resolve instance via sub_419860; walk
  // instance+0x5C; first match node+0x14==*(handle+8) && node+0x1C==etype
  // → unlink to free-list dword_6363F0/F8 (same first-only as rem Watch
  // @ 0x0048B670); Engine_malloc(0x2C); node+0x1C=etype, +0x20=ealias,
  // +0x24=custmsg String*, +0x28=0 (no custmethod); insert head at +0x5C.
  // Gaps (host stand-in; no invent Watch/blob APIs):
  // - no Native.ptr gate / Mighty: still insert when script has no blob.
  // - match key = InvObject* ref + etype (not *(handle+8) listener).
  // - list on GameType self (not GameRef instance+0x5C / free-list).
  if (!self) return;
  if (!ref) return;  // stock dest1==0
  std::lock_guard<std::mutex> lock(g_mu);
  auto& nlist = st(self).notifs;
  for (auto it = nlist.begin(); it != nlist.end(); ++it) {
    if (it->ref == ref && it->etype == etype) {
      nlist.erase(it);  // stock Watch: first match only (like rem)
      break;
    }
  }
  GameTypeState::Notification n;
  n.ref = ref;
  n.etype = etype;
  n.ealias = ealias;
  if (custmsg) {
    if (const char* s = string_cstr(custmsg)) n.custmsg = s;
  }
  // stock node+0x28 = 0 (no custmethod); leave n.custmethod empty
  nlist.insert(nlist.begin(), std::move(n));
  sync(self);
}

void java_lang_GameType_addNotification_1(InvObject* self, InvObject* ref,
                                          int32_t etype, int32_t ealias,
                                          InvObject* custmsg,
                                          InvObject* custmethod) {
  // PE @ 0x0047E100 size 0xb3 (179): Unbox dest0=this, dest1=GameRef,
  // dest2=etype, dest3=ealias, dest4=custmsg, dest5=custmethod. dest1==0
  // → ret. Native.ptr (dword_62E008) on this ==0 → Mighty ERROR ("!" @
  // 0x00612F58 + "Mighty ERROR" @ 0x00612F5C via CRT_strcat_n_thunk +
  // Engine_ErrorLogPrintf) — stock does NOT Watch. Contrast 3-string add
  // @ 0x0047E050 (race111): same Mighty / Watch path, but last push 0
  // (Watch a6=nullptr → node+0x28=0). Here push dest5; Watch a6 non-null
  // → Engine_malloc(strlen+1) + CRT_strncpy_thunk → node+0x28.
  // Host still insert (script GameType City/Track/Osd often has no blob).
  // Else thiscall GameType_addNotificationWatch @ 0x0048B500 size 0x16b
  // (ECX=GameRef, stack handle/etype/ealias/custmsg/custmethod). Watch:
  // GameRef+8/+0C gate; resolve instance via sub_419860; walk
  // instance+0x5C; first match node+0x14==*(handle+8) && node+0x1C==etype
  // → unlink to free-list dword_6363F0/F8 (same first-only as rem Watch
  // @ 0x0048B670 / add @ 0x0047E050); Engine_malloc(0x2C); node+0x1C=etype,
  // +0x20=ealias, +0x24=custmsg String*, +0x28=custmethod C-string; insert
  // head at +0x5C.
  // Gaps (host stand-in; no invent Watch/blob APIs):
  // - no Native.ptr gate / Mighty: still insert when script has no blob.
  // - match key = InvObject* ref + etype (not *(handle+8) listener).
  // - list on GameType self (not GameRef instance+0x5C / free-list).
  if (!self) return;
  if (!ref) return;  // stock dest1==0
  std::lock_guard<std::mutex> lock(g_mu);
  auto& nlist = st(self).notifs;
  for (auto it = nlist.begin(); it != nlist.end(); ++it) {
    if (it->ref == ref && it->etype == etype) {
      nlist.erase(it);  // stock Watch: first match only (like race111 add)
      break;
    }
  }
  GameTypeState::Notification n;
  n.ref = ref;
  n.etype = etype;
  n.ealias = ealias;
  if (custmsg) {
    if (const char* s = string_cstr(custmsg)) n.custmsg = s;
  }
  if (custmethod) {
    if (const char* s = string_cstr(custmethod)) n.custmethod = s;
  }
  // stock Watch a6 → node+0x28 (copied C-string); race111 leaves +0x28=0
  nlist.insert(nlist.begin(), std::move(n));
  sync(self);
}

void java_lang_GameType_addTimer(InvObject* self, float deadline,
                                 int32_t timerid) {
  // PE @ 0x0047E1C0 size 0x9e: Unbox dest0=this, dest1=deadline F,
  // dest2=timerid I. Native.ptr (dword_62E008) on this ==0 → Mighty ERROR
  // ("!" @ 0x00612F6C + "Mighty ERROR" @ 0x00612F70 via CRT_strcat_n_thunk
  // + Engine_ErrorLogPrintf) (host: still insert — script GameType often
  // has no blob). Else thiscall Engine_addTimer @ 0x0048B750 size 0xd7
  // (ECX=handle, fire_at=deadline+*(float*)Engine_simTime, type=0x80000080
  // EVENT_TIME|oneshot, timerid, msg=0). Node 0x30 on dword_6363D4; tick
  // @ 0x00427160 due → queueEvent type&0x0FFFFFFF (=0x80); type<0 destroy.
  // Not 1:1 (engine list); stand-in uses time_current() to match
  // pollTimers (PE clock is Engine_simTime).
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  st(self).timers.push_back({time_current() + deadline, timerid});
  sync(self);
}

void java_lang_GameType_removeAllTimers(InvObject* self) {
  // PE @ 0x0047E260 size 0x6e: Unbox dest0=this. Native.ptr
  // (dword_62E008) on this ==0 → Mighty ERROR ("!" @ 0x00612F80 +
  // "Mighty ERROR" @ 0x00612F84 via CRT_strcat_n_thunk +
  // Engine_ErrorLogPrintf) — stock does NOT clear. Host still clear
  // (script GameType City/Track/Osd often has no blob). Else jmp
  // thiscall @ 0x0048B8C0 size 0x71 (ECX=handle): handle+8==0 → ret0.
  // Else walk dword_6363CC via node+4; match node+0x14==*(handle+8);
  // unlink prev/next at +0xC/+0x10 (no prev → *(owner+0x48)=next where
  // owner=node+0x18); zero +0x18/+0x14/+0xC/+0x10 (or only +0x14 if
  // +0x18==0). Not 1:1 (engine list); stand-in clears host timers.
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  st(self).timers.clear();
  sync(self);
}

void java_lang_GameType_pollTimers() {
  // PE Engine_tickTimers @ 0x00427160: due → Engine_queueEvent_dispatch
  // type&0x0FFFFFFF (EVENT_TIME=0x80); type 0x80000080 < 0 → oneshot destroy.
  constexpr int32_t kEventTime = 0x00000080;
  struct Due {
    InvObject* self = nullptr;
    int32_t id = 0;
    int32_t mask = 0;
  };
  std::vector<Due> due;
  const float now = time_current();
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& kv : g_gt) {
      InvObject* self = kv.first;
      GameTypeState& s = kv.second;
      for (auto it = s.timers.begin(); it != s.timers.end();) {
        if (it->deadline <= now) {
          due.push_back({self, it->id, s.event_mask});
          it = s.timers.erase(it);
        } else {
          ++it;
        }
      }
      if (self) sync(self);
    }
  }
  Jvm* j = jvm_active();
  for (const Due& d : due) {
    if (!d.self) continue;
    tree_field_set_int(d.self, "last_event", kEventTime);
    tree_field_set_int(d.self, "last_timer_id", d.id);
    if ((d.mask & kEventTime) == 0) continue;
    const char* cn = tree_host_class(d.self);
    if (!j || !cn || !cn[0]) continue;
    const char* sig = nullptr;
    const JvmClass* cls = j->find_class(cn);
    while (cls) {
      for (const JvmMethod& m : cls->methods) {
        if (m.name == "handleEvent" &&
            m.signature.find("Hotkey") == std::string::npos) {
          sig = m.signature.c_str();
          break;
        }
      }
      if (sig) break;
      if (cls->super_name.empty()) break;
      cls = j->find_class(cls->super_name.c_str());
    }
    if (!sig) continue;
    std::vector<JvmValue> args = {JvmValue::make_obj(d.self),
                                  JvmValue::make_obj(nullptr),
                                  JvmValue::make_int(kEventTime),
                                  JvmValue::make_int(d.id)};
    j->invoke(cn, "handleEvent", sig, args, false);
  }
}

void java_lang_GameType_dispatchCursor(InvObject* obj_ref, InvObject* param) {
  // PE Engine_queueEvent(dest, 0, EVENT_CURSOR=0x10000, sprintf buf, 0)
  // → Engine_dispatchScriptEvent default: handleEvent(GameRef,I,String).
  // Host stores addNotification on the handler; fire when notif.ref == dest.
  struct Fire {
    InvObject* handler = nullptr;
    std::string method;
  };
  std::vector<Fire> jobs;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& kv : g_gt) {
      InvObject* handler = kv.first;
      if (!handler) continue;
      for (const auto& n : kv.second.notifs) {
        if (n.etype != kEventCursor) continue;
        if (n.ref != obj_ref) continue;
        jobs.push_back({handler, n.custmethod});
      }
    }
  }
  for (const Fire& f : jobs) {
    invoke_cursor_handler(f.handler, obj_ref, param,
                          f.method.empty() ? nullptr : f.method.c_str());
  }
}

void java_lang_GameType_dispatchCursorTo(InvObject* dest, InvObject* obj_ref,
                                         InvObject* param) {
  // Group.activate: setEventMask(EVENT_CURSOR); physics auto-send to parent
  // (no addNotification). Cursor_tick queues short sprintf to +0xEC.
  invoke_cursor_handler(dest, obj_ref, param, "handleEvent");
}

void java_lang_GameType_setEventMask(InvObject* self, int32_t eventmask) {
  // PE @ 0x004819F0 → GameInstance_orEventMask @ 0x0048D3D0:
  // inner=*(handle+0xC); vtbl+0xC(1.0f); *(payload+0x70) |= mask.
  // OR, not replace; mask 0 is a no-op. Wrapper push 0 unused.
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  st(self).event_mask |= eventmask;
  sync(self);
}

void java_lang_GameType_clearEventMask(InvObject* self, int32_t eventmask) {
  // PE @ 0x00481A30 → GameInstance_clearEventMask @ 0x0048D420:
  // *(payload+0x70) &= ~mask (mask 0 no-op; EVENT_ANY 0x0FFFFFFF keeps bit28+).
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  st(self).event_mask &= ~eventmask;
  sync(self);
}

void java_lang_GameType_registerCallback(InvObject* self, int32_t mode) {
  // PE @ 0x00481C40 size 0x40: Unbox dest0=this, dest1=mode I.
  // Native.ptr (dword_62E008 @ 0x0062E008) via JVM_vm_get_int_field @
  // 0x0042AB50 — stock does NOT Mighty/early-out on handle==0 (unlike
  // addNotification @ 0x0047E050 / addTimer @ 0x0047E1C0). thiscall
  // Engine_registerGameInstanceCallback @ 0x00427370 size 0x157
  // (ECX=g_EngineState @ 0x636338, handle, mode): mode==8 GII_CONTROL —
  // Engine_malloc(0x1C), ctor sub_429130, vtbl 0x5F09B8→sub_428FC0,
  // vtbl 0x5F09B4, ResHandle_Rebind @ 0x429060 (handle+0xC, *(handle+0xC)),
  // tail insert eng+0x10/+0x18, ++eng+0xC4; mode==9 GII_DRIVE — same node
  // path, eng+0x2C/+0x34, ++eng+0xC8; mode==28 GII_ANIMATE — zero
  // node+0xC..+0x1C, vtbl 0x5F09B4, ResHandle_Rebind, tail insert
  // eng+0x48/+0x50, ++eng+0xCC; else no-op @ 0x4274C3. Not 1:1 (engine
  // callback lists). Host: OR callback_mode bit for modes 8/9/28 (three
  // independent slots; pairs unregisterCallback(mode)).
  if (!self) return;
  const int32_t handle = tree_field_get_int(self, "ptr");  // Native.ptr
  (void)handle;  // stock always calls Engine_registerGameInstanceCallback
  const int32_t bit = callback_mode_bit(mode);
  if (!bit) return;
  const int32_t mask = tree_field_get_int(self, "callback_mode");
  tree_field_set_int(self, "callback_mode", mask | bit);
}

void java_lang_GameType_unregisterCallback(InvObject* self, int32_t mode) {
  // PE @ 0x00481C80 size 0x3a: Unbox dest0=this, dest1=mode.
  // Native.ptr (dword_62E008) via JVM_vm_get_int_field — stock does NOT
  // Mighty/early-out on 0 (unlike addTimer). thiscall sub_4274E0
  // @ 0x004274E0 size 0x138 (ECX=g_EngineState @ 0x636338, handle, mode):
  // mode==8 GII_CONTROL walk eng[2] match node[5]==*(handle+8) →
  // ResHandle_Unlink(owner+0x44) or zero +8; mode==9 GII_DRIVE eng[9];
  // mode==28 GII_ANIMATE eng[16] manual unlink +0xC/+0x10 (no prev →
  // *(owner+0x48)=next); else no-op. Not 1:1 (engine lists). Host:
  // clear callback_mode bit for mode (pairs registerCallback bitmask).
  if (!self) return;
  const int32_t bit = callback_mode_bit(mode);
  if (!bit) return;
  const int32_t mask = tree_field_get_int(self, "callback_mode");
  if (mask & bit) tree_field_set_int(self, "callback_mode", mask & ~bit);
}

void java_lang_GameType_unregisterCallbacks(InvObject* self) {
  // PE @ 0x00481CC0 size 0x2f: Unbox dest0=this. Native.ptr
  // (dword_62E008) via JVM_vm_get_int_field @ 0x0042AB50 — stock does
  // NOT Mighty/early-out on 0 (same as unregisterCallback). thiscall
  // Engine_unregisterAllGameInstanceCallbacks @ 0x004274D0 size 0x3
  // (ECX=g_EngineState @ 0x636338, push handle): retn 4 — engine no-op
  // (also called from GameType unload @ 0x53EDD5). Contrast
  // unregisterCallback(I) @ 0x00481C80 →
  // Engine_unregisterGameInstanceCallback @ 0x004274E0 (mode 8/9/28
  // unlink). Java "unregister all" is aspirational; PE never walks
  // callback lists. Host: true no-op (do NOT clear callback_mode —
  // that would invent bulk clear stock lacks; use
  // unregisterCallback(mode) per slot).
  if (!self) return;
  const int32_t handle = tree_field_get_int(self, "ptr");  // Native.ptr
  (void)handle;  // stock always calls Engine_unregisterAllGameInstanceCallbacks
}

void java_lang_GameType_createNativeInstance(InvObject* self, InvObject* parent,
                                             int32_t typeID, InvObject* params,
                                             InvObject* alias) {
  // PE @ 0x00481A70 size 0x1d0 (464): Unbox dest0=this, dest1=parent,
  // dest2=typeID, dest3=params, dest4=alias. Zero local type handle
  // (var_10..var_4). typeID!=0 → thiscall sub_546070(&handle, typeID,
  // kind=8, 0). typeID==0 → class+0x1D4 cache: if 0, sub_404EA0(class)
  // → ResourceEngine_type_gametype @ 0x53A1A0 (dword_62F260, class,
  // name) → store [eax+0x50] at class+0x1D4; then sub_546070(&handle,
  // cached, 8, 0). parent==0 || *(parent+8)==0 → g_WorldTreeRoot @
  // 0x636460. alias==0 → sub_404EA0(class) C-string. Factory
  // Engine_CreateGameInstanceNative @ 0x53A2A0 (parent, &typeHandle,
  // script=this, params, alias) — contrast GameRef.create_native
  // @ 0x0047D900 a3=0. Then Native.ptr (dword_62E008): 0 → Mighty
  // ("!" @ 0x00613384 + "Mighty ERROR" @ 0x00613388 via
  // CRT_strcat_n_thunk + Engine_ErrorLogPrintf) AFTER factory
  // (create_native gates before). Else if handle+0xC != new_inst:
  // unlink old; if inst ResHandle_Link(inst+0x44, handle),
  // handle+8=inst+0x50. Epilogue unlink temp type handle if var_4.
  // Not 1:1 (no PE factory/blob). Host: mark native_created + stash
  // parent/typeID (script GameType often has no blob; still proceed).
  (void)params;
  (void)alias;
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  GameTypeState& s = st(self);
  s.native_created = 1;
  s.parent = parent;
  s.type_id = typeID;
  sync(self);
}

}  // namespace inv
