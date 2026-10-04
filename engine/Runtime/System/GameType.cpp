#include "natives.hpp"
#include "runtime.hpp"
#include "tree_interp.hpp"
#include "jvm.hpp"
#include "System.h"
#include "Resources.h"

#include <cstdint>
#include <cstdio>
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
  // Soft PE Engine_addTimer node @ 0x48B750 malloc(0x30): +0x1C fire_at,
  // +0x20 period, +0x24 type, +0x28 timerid, +0x2C msg (Always nullptr from
  // GameType.addTimer → TickTimers a5 custmethod). Host keeps no engine
  // dllist — per-GameType vector.
  struct Timer {
    float fire_at = 0;   // PE +0x1C
    float period = 0;    // PE +0x20 (0 from addTimer)
    int32_t type = 0;    // PE +0x24 (0x80000080 oneshot|EVENT_TIME)
    int32_t id = 0;      // PE +0x28 timerid
    // PE +0x2C msg → queueEvent_dispatch a5; addTimer always 0 (empty).
    std::string msg;
  };
  std::vector<Timer> timers;
  // Soft PE Watch nodes on GI+0x5C (GameType_addNotificationWatch @ 0x48B500):
  // +0x1C etype, +0x20 ealias, +0x24 custmsg, +0x28 custmethod.
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
// PE LoadGameInit stores script at GI+0x50 (32-bit). Host side-map (64-bit
// InvObject* cannot overwrite HostLoadGameInitGi.rebind_key int32 @ +0x50).
std::unordered_map<void*, InvObject*> g_lgi_scripts;
// PE stores GameTypeCtor_create result at GI mid+0x4C (@ 0x53A8C9). Host
// side-map (node+0x4C is RESTYPE=1 on HostLoadGameInitGi — not mid).
std::unordered_map<void*, void*> g_lgi_natives;

// PE GameTypeCtor_create @ 0x429A00: Engine_malloc(28) zeroed instance.
// Layout mirrors ResHandle link slice: +0/+4 list, +8 RID, +0xC owner,
// +0x10/+0x14/+0x18 zero. Linked into owner+0x48 (GI rh_list_head).
struct HostGameTypeNativeInst {
  void* link_prev = nullptr;  // +0x0
  void* link_next = nullptr;  // +0x4
  int32_t rid = 0;            // +0x8 ← *(owner+0x50)
  void* owner = nullptr;      // +0xC
  int32_t pad_10 = 0;         // +0x10
  void* pad_14 = nullptr;     // +0x14
  void* pad_18 = nullptr;     // +0x18
};
static_assert(sizeof(HostGameTypeNativeInst) == 28, "PE malloc(28)");

GameTypeState& st(InvObject* self) { return g_gt[self]; }

void sync(InvObject* self) {
  if (!self) return;
  GameTypeState& s = st(self);
  tree_field_set_int(self, "event_mask", s.event_mask);
  tree_field_set_int(self, "timer_count", static_cast<int32_t>(s.timers.size()));
  tree_field_set_int(self, "native_created", s.native_created);
  tree_field_set_int(self, "notif_count", static_cast<int32_t>(s.notifs.size()));
  if (!s.timers.empty()) {
    tree_field_set_float(self, "timer0_deadline", s.timers[0].fire_at);
    tree_field_set_int(self, "timer0_id", s.timers[0].id);
    tree_field_set_int(self, "timer0_type", s.timers[0].type);
  }
  if (!s.notifs.empty()) {
    tree_field_set_obj(self, "notif0_ref", s.notifs[0].ref);
    tree_field_set_int(self, "notif0_etype", s.notifs[0].etype);
    tree_field_set_int(self, "notif0_ealias", s.notifs[0].ealias);
    tree_field_set_obj(self, "notif0_method",
                       string_new(s.notifs[0].custmethod.c_str()));
  }
}

// GameRef.java EVENT_* (int_convert): TIME=0x80, CURSOR=0x10000, SAME=0,
// ANY=0x0FFFFFFF. Watch ealias bit 0x10000000 → custmsg param (queueEvent
// watcher walk @ 0x426740); etype bit 0x40000000 skips when related!=0;
// etype bit 0x80000000 oneshot destroy after recursive dispatch (@ 0x42679C).
constexpr int32_t kEventTime = 0x00000080;
constexpr int32_t kEventCursor = 0x00010000;
constexpr int32_t kEventAny = 0x0FFFFFFF;
constexpr int32_t kEventSame = 0;  // GameRef.EVENT_SAME — keep original type
constexpr int32_t kWatchEaliasCustmsg = 0x10000000;
constexpr int32_t kWatchEtypeSkipRelated = 0x40000000;
constexpr int32_t kWatchEtypeOneshot =
    static_cast<int32_t>(0x80000000u);
// PE addTimer type: sign bit oneshot | EVENT_TIME (bytes 80 00 00 80).
constexpr int32_t kTimerTypeOneshotTime =
    static_cast<int32_t>(0x80000000u | static_cast<uint32_t>(kEventTime));

// Soft PE Engine_queueEvent_dispatch @ 0x4265C0 watcher remap (@ 0x426733):
// ealias&0x0FFFFFFF!=0 → recursive event = ealias&0x0FFFFFFF; else keep.
int32_t watch_fwd_event(int32_t event, int32_t ealias) {
  if ((ealias & kEventAny) != kEventSame) return ealias & kEventAny;
  return event & kEventAny;
}

// Soft PE @ 0x426653: test [GI+0x70], event — ZF → skip script only.
bool event_mask_allows(int32_t mask, int32_t event) {
  return (mask & event) != 0;
}

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

// W35-13 — Engine_dispatchScriptEvent @ 0x00425C60 → Engine_CallNamedMethod
// @ 0x425A90 (va @ 0x4255E0 → packArgs @ 0x425240 → Object_callMethod).
// java.lang.GameType.handleEvent is NOT UnboxArg / RegisterAll (W34-13):
// RegisterAll @ 0x6158B8..0x615B1C is remNotification..createNativeInstance
// only; Java defaults (GameType.java:202-203) are empty non-ACC_NATIVE.
// PE strings: handleEvent @ 0x60C9D0/0x60CA08/0x60CA14, handleEventAsync
// @ 0x60CA20/0x60CA34; asyncMode field @ 0x60C9FC on *(child+0x50).
// packArgs tags (int_convert): 2=int, 4=object, 79='O' sentinel.
// LABEL_21 non-HOTKEY: CNM(child, name, sync=0, 4, GameRef, 2, event,
//   4|2, String|int, 79). Host soft: script InvObject as self (no PE child
//   +0x44 RH gate / +0x50); pack_vec + vmthread like control @ System.cpp.
constexpr int kPackArgInt = 2;
constexpr int kPackArgObj = 4;
constexpr int kPackArgSentinelO = 79;  // 'O'

const char* find_handle_event_sig(Jvm* j, const char* cn, const char* name,
                                  bool string_param) {
  if (!j || !cn || !cn[0] || !name || !name[0]) return nullptr;
  const JvmClass* cls = j->find_class(cn);
  while (cls) {
    for (const JvmMethod& m : cls->methods) {
      if (m.name != name) continue;
      if (m.signature.find("Hotkey") != std::string::npos) continue;
      const bool has_str = m.signature.find("String") != std::string::npos;
      if (has_str == string_param) return m.signature.c_str();
    }
    if (cls->super_name.empty()) break;
    cls = j->find_class(cls->super_name.c_str());
  }
  return nullptr;
}

// Soft PE Engine_dispatchScriptEvent LABEL_21 → CallNamedMethod handleEvent
// / handleEventAsync (or a6 custmethod). use_string_param ≡ PE v25
// (case 0x80 EVENT_TIME clears v25 → int param; default/cursor keep String).
// OOS: HOTKEY 0x100000 early (IO.cpp); case 1/4/0x20/0x40 sprintf; GameRef
// malloc(28)+ResHandle link @ ptr; CallNamedMethod_va child RH gate.
void engine_dispatch_script_event_call_named(InvObject* handler,
                                             InvObject* obj_ref,
                                             int32_t event, InvObject* str_param,
                                             int32_t int_param,
                                             bool use_string_param,
                                             const char* method_override) {
  if (!handler) return;
  Jvm* j = jvm_active();
  const char* cn = tree_host_class(handler);
  if (!j || !cn || !cn[0]) return;

  // PE @ 0x425F29: asyncMode on *(child+0x50); a6 overrides both branches.
  const char* name = nullptr;
  if (method_override && method_override[0]) {
    name = method_override;
  } else if (tree_field_get_int(handler, "asyncMode") != 0) {
    name = "handleEventAsync";
  } else {
    name = "handleEvent";
  }

  const char* sig =
      find_handle_event_sig(j, cn, name, use_string_param);
  if (!sig) {
    // Stock GameType.java overloads when class not loaded / TREE only.
    sig = use_string_param
              ? "(Ljava.util.resource.GameRef;ILjava.lang.String;)V"
              : "(Ljava.util.resource.GameRef;II)V";
  }

  static_assert(kPackArgInt == 2 && kPackArgObj == 4 &&
                    kPackArgSentinelO == 79,
                "PE packArgs dispatchScriptEvent tags");
  static_assert(kVmThreadBlobSize == 56, "PE malloc(56) VMThread");

  // PE CallNamedMethod_va @ 0x425665: "THRD-RUNVMI "+getResNameCstr+"."+name.
  // Host: no child+0x38 ResHandle → empty res name (same as control gap).
  char thr_name[64];
  std::snprintf(thr_name, sizeof(thr_name), "THRD-RUNVMI .%s", name);
  constexpr int32_t kSync = 0;  // PE CallNamedMethod a3=0 at all LABEL_21 sites
  VmThread* thr =
      vmthread_init(j, /*priority=*/10, /*sync_flags=*/kSync, thr_name);
  if (!thr) return;

  // PE pack: tag4 GameRef, tag2 event&0xFFFFFFF, tag4 String | tag2 int, 79.
  thr->pack_vec.push_back(JvmValue::make_obj(obj_ref));
  thr->pack_vec.push_back(JvmValue::make_int(event));
  if (use_string_param)
    thr->pack_vec.push_back(JvmValue::make_obj(str_param));
  else
    thr->pack_vec.push_back(JvmValue::make_int(int_param));
  vmthread_push_call_frame(thr);

  const int inv =
      vmthread_invoke_method(thr, handler, cn, name, sig);
  // PE @ 0x42570F: invoke ret==0 → VMThread_run(0.0).
  if (inv == 0) vmthread_run(thr, /*budget_ms=*/0.f);
  // PE @ 0x42571B: sync!=0 || ret!=0 → requestStop + dtor; else leave
  // green-thread for Jvm_PumpFrame (sync==0 && Java queued).
  if (kSync != 0 || inv != 0) {
    vmthread_request_stop(thr);
    vmthread_destroy(thr);
  }
}

// Soft PE Engine_queueEvent_dispatch watcher walk @ 0x426728..0x426797:
// match (event & etype); ealias&0x0FFFFFFF!=0 → remap event; ealias&0x10000000
// → param=custmsg; etype&0x40000000 && related!=0 → skip; custmethod → a6;
// recursive mask gate handler GI+0x70 vs remapped (@ 0x426653); then
// etype&0x80000000 oneshot dtor (@ 0x42679C) even if mask/script missed.
// EVENT_SAME (0) keeps original type. dispatchScriptEvent case 0x80 → int
// param (v25=0); default/cursor → String.
// Returns true when Soft should erase the Watch (oneshot bit).
bool fire_watch_notification(InvObject* handler, InvObject* obj_ref,
                             int32_t event, InvObject* str_param,
                             int32_t int_param, bool related_nonzero,
                             int32_t etype, int32_t ealias,
                             const std::string& custmsg,
                             const std::string& custmethod,
                             int32_t handler_mask) {
  if (!handler) return false;
  if ((etype & event) == 0) return false;
  // PE @ 0x426772: etype&0x40000000 && related≠0 → skip recursive, still
  // fall through to oneshot test @ 0x42679C.
  if ((etype & kWatchEtypeSkipRelated) != 0 && related_nonzero)
    return (etype & kWatchEtypeOneshot) != 0;

  const int32_t fwd_event = watch_fwd_event(event, ealias);
  // Soft PE recursive @ 0x426653: mask miss → no script; oneshot still.
  if (!event_mask_allows(handler_mask, fwd_event))
    return (etype & kWatchEtypeOneshot) != 0;

  const bool use_custmsg = (ealias & kWatchEaliasCustmsg) != 0;
  const bool use_string =
      (fwd_event & kEventAny) != kEventTime;  // PE case 0x80 clears v25
  InvObject* str_out = str_param;
  int32_t int_out = int_param;
  if (use_custmsg) {
    if (use_string) {
      str_out = custmsg.empty() ? nullptr : string_new(custmsg.c_str());
    } else {
      // PE still int-path for EVENT_TIME; custmsg ptr as a4 OOS → 0 stand-in.
      int_out = 0;
    }
  }

  tree_field_set_int(handler, "last_event", fwd_event);
  if (!use_string)
    tree_field_set_int(handler, "last_timer_id", int_out);
  else if (str_out)
    tree_field_set_obj(handler, "last_cursor_param", str_out);

  const char* method =
      custmethod.empty() ? nullptr : custmethod.c_str();
  engine_dispatch_script_event_call_named(handler, obj_ref, fwd_event, str_out,
                                          int_out, use_string, method);
  // Soft PE @ 0x42679C: etype&0x80000000 → oneshot destroy Watch node.
  return (etype & kWatchEtypeOneshot) != 0;
}

void invoke_cursor_handler(InvObject* handler, InvObject* obj_ref,
                           InvObject* param, int32_t etype, int32_t ealias,
                           const std::string& custmsg,
                           const std::string& custmethod, int32_t handler_mask) {
  if (!handler || !param) return;
  // Soft PE related=&var_10 nonzero → etype&0x40000000 skips script (@ 0x426772).
  if ((etype & kWatchEtypeSkipRelated) != 0) return;
  const int32_t fwd = watch_fwd_event(kEventCursor, ealias);
  // Soft PE recursive mask @ 0x426653 — no script / no count on miss.
  // Caller still oneshot-erases (@ 0x42679C) when etype&0x80000000.
  if (!event_mask_allows(handler_mask, fwd)) return;
  tree_field_set_int(handler, "cursor_event_count",
                     tree_field_get_int(handler, "cursor_event_count") + 1);
  // PE default case: a4 cstr → String; EVENT_CURSOR 0x10000.
  (void)fire_watch_notification(handler, obj_ref, kEventCursor, param,
                                /*int_param=*/0, /*related_nonzero=*/true, etype,
                                ealias, custmsg, custmethod, handler_mask);
}

void erase_oneshot_watch(InvObject* handler, InvObject* ref, int32_t etype) {
  if (!handler) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& n = st(handler).notifs;
  for (auto it = n.begin(); it != n.end(); ++it) {
    if (it->ref == ref && it->etype == etype) {
      n.erase(it);  // PE first-match unlink @ 0x42679C
      break;
    }
  }
  sync(handler);
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
  // PE @ 0x0047E1C0 size 0x9e (158): Unbox dest0=this, dest1=deadline F,
  // dest2=timerid I. Native.ptr (dword_62E008) on this ==0 → Mighty ERROR
  // ("!" @ 0x00612F6C + "Mighty ERROR" @ 0x00612F70 via Engine_strcat_cap +
  // Engine_ErrorLogMsgBox) (host: still insert — script GameType often has
  // no blob). Else thiscall Engine_addTimer @ 0x0048B750 size 0xd7
  // (ECX=handle, fire_at=deadline+*(float*)Engine_physWorld @ 0x617650 →
  // float @ 0x6409C4 — same absolute clock System.simTime reads; type=
  // 0x80000080 EVENT_TIME|oneshot, timerid, msg=0). Node malloc(0x30):
  // +0x1C fire_at, +0x20 period=0, +0x24 type, +0x28 id, +0x2C msg; link
  // dword_6363D4/DC; ++MEMORY[0x636410]. Tick @ Engine_TickTimers 0x00427160
  // due → queueEvent type&0x0FFFFFFF (=0x80); type<0 oneshot destroy.
  // Not 1:1 (engine list); stand-in uses time_current()+deadline (PE clock
  // *Engine_physWorld OOS on host).
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  GameTypeState::Timer t;
  t.fire_at = time_current() + deadline;
  t.period = 0.f;
  t.type = kTimerTypeOneshotTime;
  t.id = timerid;
  st(self).timers.push_back(t);
  sync(self);
}

void java_lang_GameType_removeAllTimers(InvObject* self) {
  // PE @ 0x0047E260 size 0x6e (110): Unbox dest0=this. Native.ptr
  // (dword_62E008) on this ==0 → Mighty ERROR ("!" @ 0x00612F80 +
  // "Mighty ERROR" @ 0x00612F84) — stock does NOT clear. Host still clear
  // (script GameType City/Track/Osd often has no blob). Else jmp tail @
  // 0x0048B8C0: handle+8==0 → ret0. Else walk dword_6363CC via node+4;
  // match node+0x14==*(handle+8); unlink prev/next at +0xC/+0x10 (no prev
  // → *(owner+0x48)=next where owner=node+0x18); zero +0x18/+0x14/+0xC/
  // +0x10 (or only +0x14 if +0x18==0). Not 1:1 (engine list); stand-in
  // clears host timers.
  // race124/125 deepen: size 0x6e; Mighty on null; dword_6363CC walk;
  // host clear stand-in ok (no blob).
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  st(self).timers.clear();
  sync(self);
}

void java_lang_GameType_pollTimers() {
  // Soft PE Engine_TickTimers @ 0x00427160: walk EngState+0x94 dllist; due
  // when +0x1C <= *(float*)Engine_physWorld @ 0x617650; queueEvent_dispatch
  // (RH+0xC, 0, type&0x0FFFFFFF, timerid, msg); type<0 → oneshot vtbl dtor
  // else fire_at += period. Soft PE Engine_queueEvent_dispatch @ 0x4265C0:
  //   1) mask gate test [GI+0x70], event (@ 0x426653) → script +0x50
  //      dispatchScriptEvent (skip script on ZF; watchers still run)
  //   2) always walk watchers GI+0x5C (@ 0x4266EA); recursive dispatch
  //      mask-gates handler; etype&0x80000000 oneshot (@ 0x42679C)
  // case 0x80 @ 0x425DFB: v25=0, v27=a4 → LABEL_21 handleEvent(GameRef,I,I)
  // (W35-13). Host: per-GameType vector + time_current(); oneshot erase
  // (type<0); mask gate Soft (Java setEventMask); fan-out EVENT_TIME notifs.
  struct Due {
    InvObject* self = nullptr;
    int32_t id = 0;
    int32_t mask = 0;
    int32_t event = 0;  // PE type&0x0FFFFFFF
    std::string msg;    // PE +0x2C → a5 custmethod (addTimer always empty)
  };
  std::vector<Due> due;
  struct WatchFire {
    InvObject* handler = nullptr;
    InvObject* watched = nullptr;
    int32_t id = 0;
    int32_t event = 0;
    int32_t etype = 0;
    int32_t ealias = 0;
    int32_t hmask = 0;
    std::string custmsg;
    std::string custmethod;
  };
  std::vector<WatchFire> watches;
  const float now = time_current();
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& kv : g_gt) {
      InvObject* self = kv.first;
      GameTypeState& s = kv.second;
      for (auto it = s.timers.begin(); it != s.timers.end();) {
        if (it->fire_at <= now) {
          // Soft PE TickTimers @ 0x4271e4: event = type&0x0FFFFFFF (=0x80).
          due.push_back({self, it->id, s.event_mask, it->type & kEventAny,
                         it->msg});
          // PE type<0 → destroy; type>=0 → fire_at += period (addTimer
          // always 0x80000080 oneshot — erase). period path Soft-ready.
          if (it->type < 0) {
            it = s.timers.erase(it);
          } else {
            it->fire_at += it->period;
            ++it;
          }
        } else {
          ++it;
        }
      }
      if (self) sync(self);
    }
    // Soft PE watcher walk: notifs on handler with ref==watched. Collect
    // even when handler mask misses — recursive still returns then oneshot
    // dtor @ 0x42679C; script gated inside fire_watch_notification.
    for (const Due& d : due) {
      if (!d.self || d.event == 0) continue;
      for (auto& kv : g_gt) {
        InvObject* handler = kv.first;
        if (!handler) continue;
        const int32_t hmask = kv.second.event_mask;
        for (const auto& n : kv.second.notifs) {
          if (n.ref != d.self) continue;
          if ((n.etype & d.event) == 0) continue;
          watches.push_back({handler, d.self, d.id, d.event, n.etype, n.ealias,
                             hmask, n.custmsg, n.custmethod});
        }
      }
    }
  }
  for (const Due& d : due) {
    if (!d.self) continue;
    tree_field_set_int(d.self, "last_event", d.event);
    tree_field_set_int(d.self, "last_timer_id", d.id);
    // PE @ 0x426653: test [GI+0x70], event — skip script if mask bit clear.
    // Soft gate kept (Java setEventMask); watchers still fire below.
    if (!event_mask_allows(d.mask, d.event)) continue;
    // PE case 0x80: int param path (v25=0); obj_ref GameRef box OOS → null.
    // a5 msg custmethod (addTimer 0 → nullptr).
    const char* msg =
        d.msg.empty() ? nullptr : d.msg.c_str();
    engine_dispatch_script_event_call_named(
        d.self, /*obj_ref=*/nullptr, d.event, /*str_param=*/nullptr, d.id,
        /*use_string_param=*/false, /*method_override=*/msg);
  }
  // PE watchers run after script path (and even when mask missed).
  for (const WatchFire& w : watches) {
    const bool oneshot = fire_watch_notification(
        w.handler, w.watched, w.event, /*str_param=*/nullptr, w.id,
        /*related_nonzero=*/false, w.etype, w.ealias, w.custmsg, w.custmethod,
        w.hmask);
    if (oneshot) erase_oneshot_watch(w.handler, w.watched, w.etype);
  }
}

void java_lang_GameType_dispatchCursor(InvObject* obj_ref, InvObject* param) {
  // Soft PE Engine_queueEvent(dest, 0, EVENT_CURSOR=0x10000, sprintf buf, 0)
  // → queueEvent_dispatch @ 0x4265C0: mask gate GI+0x70; script; watcher walk
  // GI+0x5C (etype&event, ealias remap, custmsg bit 0x10000000, custmethod a6,
  // oneshot 0x80000000) → Engine_dispatchScriptEvent default →
  // handleEvent(GameRef,I,String) (W35-13). Not UnboxArg.
  // Host: Cursor dest is GameRef (script +0x50 OOS); Soft fans Watch notifs
  // on handler with ref==dest. Soft gate on remapped event (Java
  // setEventMask) — keep Cursor LDRAG smoke (setEventMask+addNotification).
  struct Fire {
    InvObject* handler = nullptr;
    int32_t etype = 0;
    int32_t ealias = 0;
    int32_t hmask = 0;
    std::string custmsg;
    std::string custmethod;
  };
  std::vector<Fire> jobs;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& kv : g_gt) {
      InvObject* handler = kv.first;
      if (!handler) continue;
      for (const auto& n : kv.second.notifs) {
        if ((n.etype & kEventCursor) == 0) continue;
        if (n.ref != obj_ref) continue;
        // Collect even on mask miss (oneshot @ 0x42679C); script gated later.
        jobs.push_back({handler, n.etype, n.ealias, kv.second.event_mask,
                        n.custmsg, n.custmethod});
      }
    }
  }
  for (const Fire& f : jobs) {
    invoke_cursor_handler(f.handler, obj_ref, param, f.etype, f.ealias,
                          f.custmsg, f.custmethod, f.hmask);
    // Soft PE @ 0x42679C: etype&0x80000000 oneshot Watch destroy (even if
    // mask/script missed or 0x40000000 skip).
    if ((f.etype & kWatchEtypeOneshot) != 0)
      erase_oneshot_watch(f.handler, obj_ref, f.etype);
  }
}

void java_lang_GameType_dispatchCursorTo(InvObject* dest, InvObject* obj_ref,
                                         InvObject* param) {
  // Group.activate: setEventMask(EVENT_CURSOR); physics auto-send to parent
  // (no addNotification). Cursor_tick queues short sprintf to +0xEC.
  // Soft PE @ 0x426653: script only if dest GI+0x70 & EVENT_CURSOR (Java
  // setEventMask). Soft direct script path (no Watch); EVENT_SAME / no
  // custmethod → asyncMode picks handleEvent(Async) like PE a6=null.
  if (!dest || !param) return;
  int32_t mask = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    mask = st(dest).event_mask;
  }
  if (!event_mask_allows(mask, kEventCursor)) return;
  invoke_cursor_handler(dest, obj_ref, param, kEventCursor, kEventSame,
                        /*custmsg=*/{}, /*custmethod=*/{}, mask);
}

void java_lang_GameType_setEventMask(InvObject* self, int32_t eventmask) {
  // PE @ 0x004819F0 → GameInstance_orEventMask @ 0x0048D3D0:
  // inner=*(handle+0xC); vtbl+0xC(1.0f); *(payload+0x70) |= mask.
  // OR, not replace; mask 0 is a no-op. Soft: host event_mask (queueEvent
  // gate @ 0x426653 test [GI+0x70], event — EVENT_TIME 0x80 / CURSOR etc.).
  // Soft deepen: keep this OR gate — scripts (Garage/Group.activate) set bits;
  // pollTimers/dispatchCursor/To must not bypass it.
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  st(self).event_mask |= eventmask;
  sync(self);
}

void java_lang_GameType_clearEventMask(InvObject* self, int32_t eventmask) {
  // PE @ 0x00481A30 → GameInstance_clearEventMask @ 0x0048D420:
  // *(payload+0x70) &= ~mask (mask 0 no-op; EVENT_ANY 0x0FFFFFFF keeps
  // bits above 28). Soft: host event_mask mirror of GI+0x70.
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
  // Engine_malloc(0x1C), Engine_SimCallbackNode_ctor @ 0x429130,
  // Engine_ResHandleSlot_clear @ 0x428FC0, ResHandle_Rebind @ 0x429060
  // (*(handle+12)), tail insert eng+0x10/+0x18, ++eng+0xC4; mode==9
  // GII_DRIVE — same node path, eng+0x2C/+0x34, ++eng+0xC8; mode==28
  // GII_ANIMATE — zero node+0xC..+0x1C, ResHandle_Rebind, tail insert
  // eng+0x48/+0x50, ++eng+0xCC; else no-op @ 0x4274C3.
  // Host: mode8 → engine_gii_control_register (System CONTROL dllist);
  // modes 9/28 still bitmask-only (DRIVE/ANIMATE lists OOS).
  if (!self) return;
  const int32_t handle = tree_field_get_int(self, "ptr");  // Native.ptr
  if (mode == 8) {
    engine_gii_control_register(handle, self);  // PE @ 0x427370 mode 8
                                               // + Tick CallNamedMethod script
  }
  const int32_t bit = callback_mode_bit(mode);
  if (!bit) return;
  const int32_t mask = tree_field_get_int(self, "callback_mode");
  tree_field_set_int(self, "callback_mode", mask | bit);
}

void java_lang_GameType_unregisterCallback(InvObject* self, int32_t mode) {
  // PE @ 0x00481C80 size 0x3a: Unbox dest0=this, dest1=mode.
  // Native.ptr (dword_62E008) via JVM_vm_get_int_field — stock does NOT
  // Mighty/early-out on 0 (unlike addTimer). thiscall
  // Engine_unregisterGameInstanceCallback @ 0x004274E0 size 0x138
  // (ECX=g_EngineState @ 0x636338, handle, mode): mode==8 GII_CONTROL
  // walk eng[2] match node[5]==*(handle+8) → ResHandle_Unlink(owner+0x44)
  // @ 0x429010 or zero slot+8; mode==9 GII_DRIVE eng[9]; mode==28
  // GII_ANIMATE eng[16] manual unlink; else no-op.
  // Host: mode8 → engine_gii_control_unregister; clear callback_mode bit.
  if (!self) return;
  const int32_t handle = tree_field_get_int(self, "ptr");  // Native.ptr
  if (mode == 8) {
    engine_gii_control_unregister(handle);  // PE @ 0x4274E0 mode 8
  }
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
  // callback lists. Host: true no-op (do NOT clear callback_mode or
  // CONTROL list — that would invent bulk clear stock lacks; use
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
  // PE @ 0x481AD6 typeID==0: ResourceEngine_type_gametype → class+0x1D4 cache.
  // Host: seed mid+0x10 Class* via type_gametype(name) → class_box_object.
  const char* fqn = tree_host_class(self);
  if (!fqn || !fqn[0]) fqn = "java.lang.GameType";
  (void)resource_engine_type_gametype(/*parent_rh=*/nullptr, /*clazz=*/nullptr,
                                      fqn);
  std::lock_guard<std::mutex> lock(g_mu);
  GameTypeState& s = st(self);
  s.native_created = 1;
  s.parent = parent;
  s.type_id = typeID;
  sync(self);
}

void* engine_load_game_init_run_gametype_ctor(void* gi, void* type_payload,
                                              const char* params) {
  // PE Engine_LoadGameInit @ 0x53A8B2..0x53A8C9 (before THRD-CREATE):
  //   ecx = *(type_payload+0xC)  // GameTypeCtor* (HostMidPayload.leaf)
  //   if ecx==0 → skip (same as PE jz loc_53A8CE)
  //   else thiscall GameTypeCtor_create@429A00 (GameTypeCtor_vtbl+0xC @
  //     0x5F09EC): (this=ctor, &tmpRH, params) → eax
  //   mov [GI_mid+0x4C], eax
  // PE @ 0x53A860..0x53A8A0: stack tmpRH linked to GI (+0x48 head / +0x50
  // RID) before the call — create reads *(tmpRH+0xC)=owner.
  // GameTypeCtor_create: malloc(28) zero; link into owner+0x48; +8=RID;
  // ++*(ctor+0x14) inst count. Base ctor ignores params (a3 unused).
  // PE @ 0x53AA14..0x53AA26: GameTypeCtor_bindInstance vtbl+0x40(ctor,
  // mid+0x4C) — base @ 0x429F80 is nullsub.
  // Host: mirror create link into GI+0x48; side-map result (node+0x4C is
  // RESTYPE, not mid). No PE vtbl call (host process ≠ stock image).
  (void)params;
  if (!gi) return nullptr;
  if (!type_payload) return nullptr;
  void* ctor =
      *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(type_payload) + 0xC);
  if (!ctor) return nullptr;

  auto* inst = new HostGameTypeNativeInst{};
  auto* gi_b = reinterpret_cast<uint8_t*>(gi);
  void** rh_head = reinterpret_cast<void**>(gi_b + 0x48);
  const int32_t rid = *reinterpret_cast<int32_t*>(gi_b + 0x50);

  // PE ResHandle head-insert @ owner+0x48 (same as create @ 0x429A6D):
  //   if (*(owner+0x48)) **(owner+0x48) = result;  // old head→prev
  //   result→next = *(owner+0x48); *(owner+0x48) = result;
  //   result→owner = owner; result→rid = *(owner+0x50);
  if (*rh_head) {
    *reinterpret_cast<void**>(*rh_head) = inst;  // old head link_prev
  }
  inst->link_prev = nullptr;
  inst->link_next = *rh_head;
  *rh_head = inst;
  inst->owner = gi;
  inst->rid = rid;

  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_lgi_natives[gi] = inst;
  }
  // PE bindInstance vtbl+0x40: base nullsub — no host work.
  return inst;
}

void* engine_load_game_init_native(void* gi) {
  if (!gi) return nullptr;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_lgi_natives.find(gi);
  return it == g_lgi_natives.end() ? nullptr : it->second;
}

InvObject* engine_load_game_init_attach_gametype(void* gi,
                                                 const char* script_fqn,
                                                 int32_t local_rid) {
  // PE Engine_LoadGameInit @ 0x0053A5E0 — GameType/THRD-CREATE slice
  // @ 0x53A8CE..0x53A9E6 (gate: type payload+0x10 Class* != 0):
  //   0x53A8D7 malloc(56) + VMThread_init(g_JVM@63641C, prio=10, sync=0,
  //           "THRD-CREATE") @ 0x53A8FD
  //   0x53A908 Class_boxObject(*(payload+0x10)) @ 0x404E20
  //   0x53A915 *(GI+0x50) = boxed instance (overwrites create-desc RID)
  //   0x53A918..0x53A98B pack InvObject (+8=AllocLocalRid) →
  //           VMThread_pushCallFrame @ 0x41F9F0
  //   0x53A990 malloc(16) ResHandle + ResHandle_Rebind(GI) @ 0x429060 →
  //           JVM_vm_set_int_field(script, Native.ptr@62E008, rh)
  //   0x53A9DA Object_callMethod_init @ 0x408A70 → Thread_callMethod
  //           (thr, clazz, instance, "<init>") → VMThread_invokeMethod
  //   0x53A9E1 if ret==0 → VMThread_run @ 0x420FF0 (bytecode OOS)
  // Prior @ 0x53A8B2: GameTypeCtor_create → mid+0x4C — see
  // engine_load_game_init_run_gametype_ctor (Resources must call first).
  // Host stand-in: tree_host_new(FQN) + vmthread_init THRD-CREATE +
  // <init>(I)V with local_rid; Native.ptr = truncated GI addr; side-map
  // gi→script (no GI+0x50 pointer write — PE slot is 32-bit RID/script*).
  if (!gi || !script_fqn || !script_fqn[0]) return nullptr;

  Jvm* j = jvm_active();
  if (!j) return nullptr;
  if (!j->find_class(script_fqn)) j->load_class(script_fqn);

  // PE Class_boxObject @ 0x404E20: fresh instance of Class* (not Class shell).
  InvObject* script = tree_host_new(script_fqn);
  if (!script) return nullptr;

  // PE Native.ptr after Rebind(GI): host uses GI address as handle stand-in
  // (no 16B ResHandle / dword_62E008 field id).
  const int32_t handle =
      static_cast<int32_t>(reinterpret_cast<std::uintptr_t>(gi));
  tree_field_set_int(script, "ptr", handle);

  static_assert(kVmThreadBlobSize == 56, "PE malloc(56) THRD-CREATE");
  // PE @ 0x53A8FD: VMThread_init(JVM, 10, 0, "THRD-CREATE").
  VmThread* thr =
      vmthread_init(j, /*priority=*/10, /*sync_flags=*/0, "THRD-CREATE");
  if (thr) {
    // PE pack: InvObject with +8 = AllocLocalRid → <init>(I)V (GameType/GameRef).
    thr->pack_vec.push_back(JvmValue::make_int(local_rid));
    vmthread_push_call_frame(thr);
    // PE Object_callMethod_init @ 0x408A70 / op34 @ 0x4212C7 → Thread_callMethod.
    vmthread_op34_call_method_init(thr, script, script_fqn);
    // PE may VMThread_run if invoke returns 0; host TREE already sync-invoked.
    // Host frees blob (PE leaves thread — cooperative scheduler GAP).
    vmthread_request_stop(thr);
    vmthread_destroy(thr);
  }

  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_lgi_scripts[gi] = script;
    GameTypeState& s = st(script);
    s.native_created = 1;
    s.type_id = local_rid;
    sync(script);
  }
  return script;
}

InvObject* engine_load_game_init_script(void* gi) {
  if (!gi) return nullptr;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_lgi_scripts.find(gi);
  return it == g_lgi_scripts.end() ? nullptr : it->second;
}

}  // namespace inv
