#include "jvm.hpp"
#include "jvm_internal.hpp"
#include "callinfo.hpp"
#include "tufa.hpp"
#include "jvm_bridge.hpp"
#include "tree_interp.hpp"
#include "host_objects.hpp"
#include "runtime.hpp"
#include "natives.hpp"
#include "rpak.hpp"
#include "video_fmv.hpp"
#include "render_d3d9.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace inv {
namespace {
Jvm* g_active_jvm = nullptr;
}

void jvm_set_active(Jvm* j) { g_active_jvm = j; }
Jvm* jvm_active() { return g_active_jvm; }

namespace {

// PE g_CallFramePool @ 0x62DDF0 / CallFramePool_alloc @ 0x408D90:
// freelist at pool+0x10, else carve 64B from 0x4000 chunks (256 slots).
// Host: freelist of heap VmCallFrame (no PE chunk table).
struct CallFramePoolHost {
  std::vector<VmCallFrame*> freelist;
  std::vector<std::unique_ptr<VmCallFrame>> owned;
};
CallFramePoolHost g_callframe_pool;

VmCallFrame* callframe_pool_alloc() {
  if (!g_callframe_pool.freelist.empty()) {
    VmCallFrame* f = g_callframe_pool.freelist.back();
    g_callframe_pool.freelist.pop_back();
    return f;
  }
  auto up = std::make_unique<VmCallFrame>();
  VmCallFrame* p = up.get();
  g_callframe_pool.owned.push_back(std::move(up));
  return p;
}

void callframe_pool_release(VmCallFrame* f) {
  if (!f) return;
  f->locals.clear();
  f->operand.clear();
  f->dllist_prev = nullptr;
  f->dllist_next = nullptr;
  f->pe = {};
  g_callframe_pool.freelist.push_back(f);
}

// PE VMThread_CallFrame_ctor @ 0x407B10 (thiscall):
// vtbl off_5E7414→off_5E7410; local vec capacity=a6; op vec empty;
// +0x18=&locals, +0x28=&operand; +0x2C=a4 +0x30=a5 +0x34=a3 +0x3C=a2.
VmCallFrame* callframe_ctor(VmCallFrame* frame, VmThread* thr, uint32_t clazz,
                            uint32_t code, uint32_t instance, int local_slots) {
  if (!frame) return nullptr;
  frame->pe = {};
  frame->locals.clear();
  frame->operand.clear();
  frame->dllist_prev = nullptr;
  frame->dllist_next = nullptr;
  if (local_slots > 0) {
    frame->locals.resize(static_cast<std::size_t>(local_slots));
    frame->pe.local_cap = static_cast<uint32_t>(local_slots);
  }
  frame->pe.local_hdr = 1;  // PE this+0x0C header live
  frame->pe.op_hdr = 1;     // PE this+0x1C header live (pushCallFrame)
  frame->pe.code = code;
  frame->pe.clazz = clazz;
  frame->pe.instance = instance;
  frame->pe.vmthread =
      static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(thr));
  return frame;
}

void frame_list_link(VmFrameList* list, VmCallFrame* frame) {
  if (!list || !frame) return;
  // PE: insert before cursor in circular dllist (frame_list+8).
  frame->dllist_next = list->cursor;
  frame->dllist_prev = list->cursor ? list->cursor->dllist_prev : nullptr;
  if (list->cursor) list->cursor->dllist_prev = frame;
  list->cursor = frame;
  list->frames.push_back(frame);
}

// PE Class_isInheritedFrom_desc("java.lang.Native") @ invokeMethod Native.ptr.
bool class_inherits_native(Jvm* j, const char* fqn) {
  if (!j || !fqn || !fqn[0]) return false;
  const JvmClass* c = j->find_class(fqn);
  if (!c) {
    j->load_class(fqn);
    c = j->find_class(fqn);
  }
  for (int guard = 0; c && guard < 64; ++guard) {
    if (c->name == "java.lang.Native") return true;
    if (c->super_name.empty() || c->super_name == "java.lang.Object") break;
    if (!j->find_class(c->super_name.c_str())) j->load_class(c->super_name.c_str());
    c = j->find_class(c->super_name.c_str());
  }
  return false;
}

// PE Object_getField(obj,"ptr")+8 → Native.ptr (aPtr @ 0x60C338).
void unbox_native_ptr_args(Jvm* j, std::vector<JvmValue>* args) {
  if (!j || !args) return;
  for (JvmValue& a : *args) {
    if (a.tag != JvmTag::Obj || !a.v.o) continue;
    const char* cn = tree_host_class(a.v.o);
    if (!class_inherits_native(j, cn)) continue;
    // PE: if obj+8 (payload) != 0 → box Int(ptr); else leave/zero.
    const int32_t ptr = tree_field_get_int(a.v.o, "ptr");
    a = JvmValue::make_int(ptr);
  }
}

}  // namespace

// PE green-thread dllist: VMThread_init links into *(JVM+0x18)+0x18.
// Host: circular-ish doubly-linked list headed by g_vm_sched_head.
static VmThread* g_vm_sched_head = nullptr;
static VmThread* g_vm_sched_cursor = nullptr;  // PE JVM+0x78 run cursor
static int g_jvm_pump_gen = 0;                // PE Jvm+0x40 bump
// PE Jvm+0x3C GC slice budget (Jvm_GcSlice clamp [4,64]). Host mirror
// only — never recalculated (no +0x44/+0x30 dllist lens).
static int g_jvm_gc_slice_budget = 4;
// PE Jvm+0x24 — EMA of mark-sweep wall time (µs-ish). PumpFrame @
// 0x418E62..0x418E90: ftol(delta_ms*1000*0.8 + old*0.2). System.info(3).
static int32_t g_jvm_gc_ema = 0;

// PE Jvm_GcSlice @ 0x418C20 size 0xE8 — host SKIP (W34-17: no soft path).
// PE: if (Jvm+0x40 & 0xFF)==0: count finalizeQ(+0x44) + grey(+0x30)/budget
//   → write +0x3C = max(4, min(64, (nFin + nGrey/budget)>>6));
//   then pop ≤+0x3C heads via Object_FinalizeFree @ 0x408560;
//   if budget remain & Q empty: splice +0x30 dllist into +0x2C.
// Skip: host has no PE object dllists at +0x44/+0x30/+0x2C;
// Object_FinalizeFree @ 0x408560 → Thread_callMethod("finalize") →
// VMThread_run opcode switch @ 0x4210D4 (do not invent interpreter).
static void jvm_gc_slice() {
  (void)g_jvm_gc_slice_budget;  // PE +0x3C; live only with dllists
}

// W35-17: mark-sweep body of Jvm_PumpFrame @ 0x418D37..0x418E5F / JVM_GC
// @ 0x417F80 — SKIP (no soft shell without bytecode).
// Gate PE: *(Jvm+0x34) > *(Jvm+0x38). Then:
//   grey@+0x20 set → Jvm_MarkSweepDrainGrey @ 0x418970 → walk live
//     dllist(+0x10) unmarked → Object_EnqueueFinalize @ 0x408500 →
//     *(+0x34) -= *(+0x38);
//   else alloc grey PtrVec → Jvm_MarkSweepSeedSceneRoots @ 0x418110 →
//     Jvm_MarkSweepSeedThreadLocals @ 0x418290 → Object_MarkGrey @
//     0x4198B0 over class slots (ret -1 busy).
// Host: no PE object dllists / grey stack / Class shells; GcSlice
// FinalizeFree still → opcode@4210D4. Soft path = EMA only (below).
static void jvm_mark_sweep_shell() {}

static void vm_sched_link(VmThread* thr) {
  if (!thr) return;
  thr->sched_prev = nullptr;
  thr->sched_next = g_vm_sched_head;
  if (g_vm_sched_head) g_vm_sched_head->sched_prev = thr;
  g_vm_sched_head = thr;
}

static void vm_sched_unlink(VmThread* thr) {
  if (!thr) return;
  if (g_vm_sched_cursor == thr) g_vm_sched_cursor = thr->sched_next;
  if (thr->sched_prev)
    thr->sched_prev->sched_next = thr->sched_next;
  else if (g_vm_sched_head == thr)
    g_vm_sched_head = thr->sched_next;
  if (thr->sched_next) thr->sched_next->sched_prev = thr->sched_prev;
  thr->sched_prev = thr->sched_next = nullptr;
}

static float vm_now_ms() {
#ifdef _WIN32
  return static_cast<float>(GetTickCount());
#else
  return 0.f;
#endif
}

// PE VMThread_init @ 0x41F340 — fills 56B blob; FrameList malloc(28);
// CallFrame from g_CallFramePool via CallFrame_ctor; links frame dllist;
// sets curr_frame; links green-thread list. CallNamedMethod_va @ 0x4256CC.
VmThread* vmthread_init(void* jvm, int32_t priority, int32_t sync_flags,
                        const char* name) {
  auto* thr = new VmThread();
  // PE &VMThread_vtbl @ 0x5F098C — host keeps 0 (no PE vtbl image).
  thr->pe.vtbl = 0;
  thr->pe.link_prev = 0;
  thr->pe.link_next = 0;
  // Truncate host pointer into PE dword slot (stock VM is 32-bit).
  thr->pe.jvm = static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(jvm));
  thr->pe.jvm_aux = 0;  // PE *(JVM+4) when JVM live
  thr->pe.java_backref = 0;
  thr->pe.unk_24 = 0;
  thr->pe.priority = priority;  // +0x28
  thr->pe.flags = sync_flags;   // +0x2C (CallNamedMethod sync 0|1)
  thr->pe.sleep_deadline = 0.f; // +0x30
  thr->pe.notify_34 = 0;        // +0x34
  if (name && name[0])
    thr->name_storage = name;
  else
    thr->name_storage.clear();
  // PE +0x1C = strdup name; host keeps string in name_storage (no PE heap).
  thr->pe.name = 0;

  // PE malloc(28) FrameList + VMThread_FrameList_vtbl @ 0x5F0988.
  thr->frame_list = new VmFrameList();
  thr->pe.frame_list = 1;  // host: non-zero = live (no 32-bit ptr store)

  // PE CallFramePool_alloc(g_CallFramePool) + CallFrame_ctor(thr,0,0,0,0).
  VmCallFrame* fr = callframe_pool_alloc();
  callframe_ctor(fr, thr, /*clazz=*/0, /*code=*/0, /*instance=*/0,
                 /*local_slots=*/0);
  frame_list_link(thr->frame_list, fr);
  thr->curr_frame = fr;
  thr->pe.curr_frame = 1;

  // PE @ 0x41F4AA..0x41F4C4: insert into JVM green-thread dllist.
  vm_sched_link(thr);
  return thr;
}

// PE Engine_CallNamedMethod_packArgs tag3 @ 0x425380: box float (dword_62DEEC
// class) from va qword; append to arg vec. Host: JvmValue float only.
void vmthread_pack_arg_float(VmThread* thr, float f) {
  if (!thr) return;
  thr->pack_vec.push_back(JvmValue::make_float(f));
}

// PE VMThread_pushCallFrame @ 0x41F9F0 (thiscall): for i in [0, a2.count)
// push a2.data[i] onto *(this+8)+0x28 operand vector; alloc boxed Int
// (dword_62E00C) with value=count; push box; return count.
int vmthread_push_call_frame(VmThread* thr) {
  if (!thr) return 0;
  if (!thr->curr_frame) {
    VmCallFrame* fr = callframe_pool_alloc();
    callframe_ctor(fr, thr, 0, 0, 0, 0);
    if (thr->frame_list) frame_list_link(thr->frame_list, fr);
    thr->curr_frame = fr;
    thr->pe.curr_frame = 1;
  }
  VmCallFrame* fr = thr->curr_frame;
  fr->operand.clear();
  thr->operand.clear();
  fr->operand.reserve(thr->pack_vec.size() + 1);
  thr->operand.reserve(thr->pack_vec.size() + 1);
  for (const JvmValue& v : thr->pack_vec) {
    fr->operand.push_back(v);
    thr->operand.push_back(v);
  }
  const int count = static_cast<int>(thr->pack_vec.size());
  const JvmValue box = JvmValue::make_int(count);  // PE boxed count
  fr->operand.push_back(box);
  thr->operand.push_back(box);
  fr->pe.op_count = static_cast<uint32_t>(fr->operand.size());
  fr->pe.op_cap = fr->pe.op_count;
  return count;
}

// PE VMThread_invokeMethod @ 0x41FBC0 — host slice (no bytecode interp).
// ACC_NATIVE (method flags & 0x40) @ 0x41FC2D:
//   pe_native_hash(class,method) → soft NativeHash_find/next (128×0xC);
//   strcmp NativeRec +0xC/+0x10; NativeSigDesc_sameIntern @ 0x41DED0;
//   build CallInfo → nativeImpl(CallInfo*) → JVM_UnboxArg @ 0x45D910.
// Host soft: resolve_native (bucket dig) + soft_unbox_bind + jvm->invoke;
//   Native.ptr unbox via Object_getField("ptr")+8 → Int before call.
// Java path: PE CallFramePool_alloc+ctor+dllist queue; ret 0. Host: stash
// pending TREE invoke for VMThread_run / Jvm_RunThreadsBudgeted; ret 0.
// Do NOT invent opcode switch @ 0x4210D4 here.
int vmthread_invoke_method(VmThread* thr, InvObject* self, const char* class_fqn,
                           const char* method, const char* signature) {
  if (!thr || !class_fqn || !method || !signature) return -1;
  Jvm* j = jvm_active();
  if (!j) return -1;

  // PE Java path @ 0x420247: CallFramePool_alloc + CallFrame_ctor(this, clazz,
  // code, instance, local_slots) + dllist link; curr_frame = new frame.
  VmCallFrame* fr = callframe_pool_alloc();
  const uint32_t inst_u =
      static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(self));
  callframe_ctor(fr, thr, /*clazz=*/0, /*code=*/0, /*instance=*/inst_u,
                 /*local_slots=*/0);
  if (thr->frame_list) frame_list_link(thr->frame_list, fr);
  thr->curr_frame = fr;
  thr->pe.curr_frame = 1;

  std::vector<JvmValue> args;
  args.reserve(thr->pack_vec.size() + 1);
  if (self) args.push_back(JvmValue::make_obj(self));
  for (const JvmValue& v : thr->pack_vec) args.push_back(v);

  // Copy packed args onto new frame locals (PE pops operand → locals).
  fr->locals = args;
  fr->pe.local_count = static_cast<uint32_t>(fr->locals.size());
  fr->pe.local_cap = fr->pe.local_count;

  const NativeEntry* ne = resolve_native(class_fqn, method, signature);
  bool is_native = ne && ne->fn;
  if (!is_native) {
    if (!j->find_class(class_fqn)) j->load_class(class_fqn);
    if (const JvmClass* cls = j->find_class(class_fqn)) {
      if (const JvmMethod* m = j->find_method(*cls, method, signature))
        is_native = m->is_native;
    }
  }

  if (is_native) {
    // PE ACC_NATIVE @ 0x41FC2D..0x4201D8 — RegisterNative lookup + UnboxArg
    // descriptor; host: Native.ptr arg unbox then invoke (registered sig).
    unbox_native_ptr_args(j, &args);
    JvmValue result = j->invoke(class_fqn, method, signature, args, false);
    fr->operand.push_back(result);
    thr->operand.push_back(result);
    fr->pe.op_count = static_cast<uint32_t>(fr->operand.size());
    thr->pending.live = false;
    return 1;
  }

  // PE bytecode queue returns 0; CallNamedMethod_va @ 0x42570F → VMThread_run.
  thr->pending.live = true;
  thr->pending.class_fqn = class_fqn;
  thr->pending.method = method;
  thr->pending.signature = signature;
  thr->pending.args = std::move(args);
  return 0;
}

namespace {

// Soft ≡ NativeSigDesc_ctorFromOperands @ 0x41DCB0 + getJni @ 0x41DEF0:
// PE pops argc box, concatenates Value type strings, Intern "("+types+")",
// then getJni adds ret (default "V"). Soft: pack_vec tags → "(… )V".
std::string soft_jni_from_pack_vec(const std::vector<JvmValue>& pack) {
  std::string s = "(";
  for (const JvmValue& v : pack) {
    switch (v.tag) {
      case JvmTag::Int:
        s += 'I';
        break;
      case JvmTag::Float:
        s += 'F';
        break;
      case JvmTag::Obj:
        s += "Ljava/lang/Object;";
        break;
      default:
        s += 'I';
        break;
    }
  }
  s += ")V";  // PE getJni default ret 'V' when ret slot empty
  return s;
}

// Soft Class_lookupMethod_nameSig @ 0x404910: name+sig hash, else first name.
const JvmMethod* soft_lookup_method_name(Jvm* j, const char* class_fqn,
                                         const char* method,
                                         const char* soft_jni) {
  if (!j || !class_fqn || !method) return nullptr;
  if (!j->find_class(class_fqn)) j->load_class(class_fqn);
  const JvmClass* cls = j->find_class(class_fqn);
  for (int guard = 0; cls && guard < 64; ++guard) {
    if (const JvmMethod* m = j->find_method(*cls, method, soft_jni)) return m;
    if (cls->super_name.empty() || cls->super_name == "java.lang.Object") break;
    if (!j->find_class(cls->super_name.c_str()))
      j->load_class(cls->super_name.c_str());
    cls = j->find_class(cls->super_name.c_str());
  }
  return nullptr;
}

}  // namespace

// PE Thread_callMethod @ 0x4207C0 — name-only resolve + invokeMethod.
// Object_callMethod @ 0x408A30 / Object_callMethod_init @ 0x408A70 /
// Object_callInitIf @ 0x408A90 thin wrappers.
int vmthread_thread_call_method(VmThread* thr, InvObject* self,
                                const char* class_fqn, const char* method) {
  if (!thr || !class_fqn || !method || !method[0]) return -1;
  // PE null clazz → "callMethod: null clazz %s"; ret -1.
  Jvm* j = jvm_active();
  if (!j) return -1;

  // Ensure pack_vec mirrors pushCallFrame operand args (drop trailing count
  // box if present — PE ctorFromOperands pops argc Value first).
  if (thr->pack_vec.empty() && thr->curr_frame &&
      !thr->curr_frame->operand.empty()) {
    const auto& op = thr->curr_frame->operand;
    if (op.back().tag == JvmTag::Int) {
      const int argc = op.back().v.i;
      if (argc >= 0 && static_cast<std::size_t>(argc) < op.size()) {
        thr->pack_vec.assign(op.end() - 1 - argc, op.end() - 1);
      }
    }
  }

  const std::string soft_jni = soft_jni_from_pack_vec(thr->pack_vec);
  const JvmMethod* m =
      soft_lookup_method_name(j, class_fqn, method, soft_jni.c_str());
  // PE miss → ScriptError "not found"; Soft: still attempt soft_jni invoke
  // (resolve_native / TREE may bind). Exact PE: return name on miss.
  const char* sig = m ? m->signature.c_str() : soft_jni.c_str();
  return vmthread_invoke_method(thr, self, class_fqn, method, sig);
}

// PE op33 @ 0x421254: peek recv from operand; Object_callMethod(recv, thr,
// "<init>"). Soft: caller supplies name (PE case hardcodes "<init>").
int vmthread_op33_call_method(VmThread* thr, InvObject* self,
                              const char* class_fqn, const char* method) {
  // PE null instance on instance method → "instance missing" + continue.
  if (!self) return -1;
  return vmthread_thread_call_method(thr, self, class_fqn, method);
}

// PE op34 @ 0x4212C7 → Object_callMethod_init @ 0x408A70:
//   Thread_callMethod(thr, clazz, this, "<init>"); frame+0x30 / +0x34.
int vmthread_op34_call_method_init(VmThread* thr, InvObject* self,
                                   const char* class_fqn) {
  return vmthread_thread_call_method(thr, self, class_fqn, "<init>");
}

// PE op35 @ 0x4212A4 → Object_callInitIf @ 0x408A90:
//   a3!=0 → Thread_callMethod(thr, clazz, this, "<init>"); else ret 1
//   (continue / default advance). Soft: null/empty class_fqn ≡ a3==0.
int vmthread_op35_call_init_if(VmThread* thr, InvObject* self,
                               const char* class_fqn) {
  if (!class_fqn || !class_fqn[0]) return 1;
  return vmthread_thread_call_method(thr, self, class_fqn, "<init>");
}

// PE op36 @ 0x4212E4: PC+=8; Thread_evalName → Thread_callMethod (script
// marker method+0xC==-2) or VMThread_invokeMethod; ret0→yield else advance.
int vmthread_op36_invoke(VmThread* thr, InvObject* self, const char* class_fqn,
                         const char* method, const char* signature) {
  if (!thr) return -1;
  // Soft PC stride-8 (PE thr+0x24 write before evalName). No CP evalName.
  thr->pe.unk_24 += 8;
  return vmthread_invoke_method(thr, self, class_fqn, method, signature);
}

// ---------------------------------------------------------------------------
// PE VMThread_run @ 0x420FF0 size 0x2979 — opcode switch @ 0x4210D4.
// Soft: no PE insn stream (thr+0x24 / pe.unk_24). TREE invoke stands in for
// the bytecode body; hosted cases below cover return/popFrame/stack/literal/
// local-load/getfield/invoke slices behind vmthread_run / RunThreadsBudgeted.
//
// Low switch 0..0x48 via VMThread_opcodeCaseMap @ 0x4239B0 (insn stride 8):
//   16 @ 0x423932 DONE; 42 @ 0x421697 ret-void; 43 @ 0x421662 ret-val;
//   24 @ 0x4216BB pop; 40 @ 0x42164E pop+push; 29 @ 0x4214BC field-get;
//   33 @ 0x421254 Object_callMethod DONE soft; 34 @ 0x4212C7 init DONE soft;
//   35 @ 0x4212A4 callInitIf DONE soft; 36 @ 0x4212E4 invoke DONE soft;
//   4 @ 0x42116C / 5 @ 0x4211E0 cond-skip DONE soft; 32 array residual;
//   41 pool; 0/19/66/72 fatal.
// Hi @ 0x4216D5: 0x1001 local; 0x1007 literal; 0x1008.. ops; JT_FIELD_REF.
// Helpers: ValueStack_pop@0x423D40, ValueStack_push@0x423BA0,
//   VMThread_popOperand@0x41F7D0, VMThread_popFrameRestorePc@0x41F7B0,
//   CallFrame_cleanup@0x407BA0, VMThread_canContinue@0x423B90,
//   Value_getInt@0x41BD90 → JVM_booleanConversion@0x417840,
//   VMThread_opDefault_advance@0x4238ED, Thread_evalName@0x4208E0,
//   Thread_callMethod@0x4207C0.
// ---------------------------------------------------------------------------

namespace {

// PE @ 0x423B90: !(flags & 0x82).
bool vmthread_can_continue(const VmThread* thr) {
  return thr && (thr->pe.flags & kVmThreadFlagSkipMask) == 0;
}

// PE ValueStack_push @ 0x423BA0: push onto curr_frame operand (+0x28 hdr).
void vmthread_value_stack_push(VmThread* thr, const JvmValue& v) {
  if (!thr) return;
  if (thr->curr_frame) {
    thr->curr_frame->operand.push_back(v);
    thr->curr_frame->pe.op_count =
        static_cast<uint32_t>(thr->curr_frame->operand.size());
  }
  thr->operand.push_back(v);
}

// PE VMThread_popOperand @ 0x41F7D0 / ValueStack_pop @ 0x423D40.
bool vmthread_pop_operand(VmThread* thr, JvmValue* out) {
  if (!thr || !out) return false;
  if (thr->curr_frame && !thr->curr_frame->operand.empty()) {
    *out = thr->curr_frame->operand.back();
    thr->curr_frame->operand.pop_back();
    thr->curr_frame->pe.op_count =
        static_cast<uint32_t>(thr->curr_frame->operand.size());
    if (!thr->operand.empty()) thr->operand.pop_back();
    return true;
  }
  if (!thr->operand.empty()) {
    *out = thr->operand.back();
    thr->operand.pop_back();
    return true;
  }
  return false;
}

// PE CallFrame_cleanup @ 0x407BA0 + popFrameRestorePc @ 0x41F7B0:
// unlink/release curr_frame; thr+0x20 → parent; thr+0x24 ← parent+0x2C (code).
void vmthread_pop_frame_restore_pc(VmThread* thr) {
  if (!thr) return;
  VmCallFrame* dying = thr->curr_frame;
  if (!dying) {
    thr->pe.unk_24 = 0;  // PE PC @ +0x24
    return;
  }
  VmCallFrame* parent = nullptr;
  if (thr->frame_list) {
    auto& frames = thr->frame_list->frames;
    for (std::size_t i = 0; i < frames.size(); ++i) {
      if (frames[i] != dying) continue;
      if (i > 0) parent = frames[i - 1];
      frames.erase(frames.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
    if (!parent && dying->dllist_prev) parent = dying->dllist_prev;
    thr->frame_list->cursor =
        parent ? parent : (frames.empty() ? nullptr : frames.back());
  }
  // Soft leaf frames have pe.code==0 (invokeMethod ctor); parent keeps saved
  // resume in pe.code (PE writePcToFrame @ 0x41F7C0 / invokeMethod @ 0x420237).
  const uint32_t resume = parent ? parent->pe.code : dying->pe.code;
  callframe_pool_release(dying);
  thr->curr_frame = parent;
  thr->pe.curr_frame = parent ? 1u : 0u;
  thr->pe.unk_24 = resume;  // PE popFrameRestorePc: thr+0x24 = frame+0x2C
}

bool jni_sig_returns_void(const std::string& sig) {
  const auto rp = sig.rfind(')');
  if (rp == std::string::npos) return sig == "V" || sig.empty();
  return rp + 1 < sig.size() && sig[rp + 1] == 'V';
}

}  // namespace

// PE op16 DONE @ 0x423932: flags |= 0x40 (RunThreadsBudgeted → STOP).
void vmthread_op16_done(VmThread* thr) {
  if (!thr) return;
  thr->pe.flags |= kVmThreadFlagDone;
}

// Soft slice of Value_getInt @ 0x41BD90 → JVM_booleanConversion @ 0x417840:
// F→0/1 by ==0.f; L/[ → nonzero ptr→1; else payload+8 as int. Soft reads
// JvmTag without mutating TOS (PE in-place type rewrite OOS without Value*).
static bool vmthread_value_getint_truthy(const JvmValue& v) {
  switch (v.tag) {
    case JvmTag::Int:
      return v.v.i != 0;
    case JvmTag::Float:
      return v.v.f != 0.f;
    case JvmTag::Obj:
      return v.v.o != nullptr;
    default:
      return false;
  }
}

// PE op4 @ 0x42116C (VMThread_op4_condSkip): ValueStack_pop; optional
// ValueField(getType==2) clone OOS soft; push; Value_getInt; if truthy
// thr+0x24 += 8 @ 0x4211D7; fallthrough default advance (caller / loop).
void vmthread_op4_cond_skip(VmThread* thr) {
  if (!thr) return;
  JvmValue v;
  if (!vmthread_pop_operand(thr, &v)) v = JvmValue{};
  vmthread_value_stack_push(thr, v);
  if (vmthread_value_getint_truthy(v)) thr->pe.unk_24 += 8;  // PE @ 0x4211D7
}

// PE op5 @ 0x4211E0 (VMThread_op5_condSkip): same pop→push as op4; if
// Value_getInt falsy thr+0x24 += 8 @ 0x42124B (polarity of op4).
void vmthread_op5_cond_skip(VmThread* thr) {
  if (!thr) return;
  JvmValue v;
  if (!vmthread_pop_operand(thr, &v)) v = JvmValue{};
  vmthread_value_stack_push(thr, v);
  if (!vmthread_value_getint_truthy(v)) thr->pe.unk_24 += 8;  // PE @ 0x42124B
}

// PE op24 @ 0x4216BB: popOperand + vtbl dtor(1) — soft: discard TOS.
void vmthread_op24_pop_dtor(VmThread* thr) {
  JvmValue discarded;
  (void)vmthread_pop_operand(thr, &discarded);
}

// PE op40 @ 0x42164E: popOperand + ValueStack_push (copy/refresh TOS).
void vmthread_op40_pop_push(VmThread* thr) {
  JvmValue v;
  if (!vmthread_pop_operand(thr, &v)) return;
  vmthread_value_stack_push(thr, v);
}

// PE op42 ret-void @ 0x421697: CallFrame dtor; popFrameRestorePc; empty→DONE.
void vmthread_op42_ret_void(VmThread* thr) {
  if (!thr) return;
  vmthread_pop_frame_restore_pc(thr);
  if (thr->pe.unk_24 == 0) vmthread_op16_done(thr);
}

// PE op43 ret-val @ 0x421662: pop; dtor; popFrame; push; empty→DONE.
void vmthread_op43_ret_val(VmThread* thr) {
  if (!thr) return;
  JvmValue ret;
  if (!vmthread_pop_operand(thr, &ret)) ret = JvmValue{};
  vmthread_pop_frame_restore_pc(thr);
  vmthread_value_stack_push(thr, ret);
  if (thr->pe.unk_24 == 0) vmthread_op16_done(thr);
}

// PE op 0x1001 local-load @ 0x421714: locals[idx] → operand.
void vmthread_op1001_local_load(VmThread* thr, uint32_t idx) {
  if (!thr || !thr->curr_frame) return;
  const auto& locals = thr->curr_frame->locals;
  if (idx >= locals.size()) {
    vmthread_value_stack_push(thr, JvmValue{});
    return;
  }
  vmthread_value_stack_push(thr, locals[idx]);
}

// PE op 0x1007 literal @ 0x4218A5 — soft slice of type-tag switch47 (int/float/obj).
void vmthread_op1007_literal(VmThread* thr, const JvmValue& lit) {
  vmthread_value_stack_push(thr, lit);
}

// PE op29 field-get @ 0x4214BC — soft via TREE field bag (no CP/Object_getField).
// Instance path: tree_field_get_*; miss → push null/0 (PE pushes null on miss
// after script-error; soft skips ScriptError).
void vmthread_op29_field_get(VmThread* thr, InvObject* obj, const char* name) {
  if (!thr || !name || !name[0]) {
    vmthread_value_stack_push(thr, JvmValue::make_obj(nullptr));
    return;
  }
  if (!obj) {
    vmthread_value_stack_push(thr, JvmValue::make_obj(nullptr));
    return;
  }
  // Prefer object field; fall back to int then float (PE Value* carries type).
  if (InvObject* fo = tree_field_get_obj(obj, name)) {
    vmthread_value_stack_push(thr, JvmValue::make_obj(fo));
    return;
  }
  const int32_t iv = tree_field_get_int(obj, name);
  if (iv != 0) {
    vmthread_value_stack_push(thr, JvmValue::make_int(iv));
    return;
  }
  const float fv = tree_field_get_float(obj, name);
  if (fv != 0.f) {
    vmthread_value_stack_push(thr, JvmValue::make_float(fv));
    return;
  }
  // Ambiguous zero: PE still pushes the field Value*; soft push int 0.
  vmthread_value_stack_push(thr, JvmValue::make_int(0));
}

int vmthread_run(VmThread* thr, float budget_ms) {
  (void)budget_ms;  // PE @ 0x4238ED cooperative time + flags&0x78 yield — OOS
                    // without PE PC stream (pe.unk_24 soft sentinel only).
  if (!thr) return -1;
  // PE @ 0x421018: flags&4 → "already running!"
  if ((thr->pe.flags & kVmThreadFlagRunning) != 0) return -1;
  if (!thr->pending.live) {
    // PE empty thread (no code @ +0x24) → error path; host: idle ok.
    return 0;
  }
  Jvm* j = jvm_active();
  if (!j) return -1;
  if (!vmthread_can_continue(thr)) return 0;

  thr->pe.flags |= kVmThreadFlagRunning;  // PE @ 0x42107D |= 4
  g_vm_sched_cursor = thr;                // PE stash JVM+0x78 cursor

  // Soft body: TREE/jvm invoke ≡ PE bytecode loop (residual cases OOS).
  JvmValue result =
      j->invoke(thr->pending.class_fqn.c_str(), thr->pending.method.c_str(),
                thr->pending.signature.c_str(), thr->pending.args, false);
  const bool ret_void = jni_sig_returns_void(thr->pending.signature);
  thr->pending.live = false;
  thr->pending.args.clear();

  // Method produced return value on operand (PE leaves TOS for op43).
  if (!ret_void) {
    vmthread_value_stack_push(thr, result);
    // PE @ 0x421662 op43 ret-val → popFrame → empty PC → op16 DONE.
    vmthread_op43_ret_val(thr);
  } else {
    // PE @ 0x421697 op42 ret-void → popFrame → empty PC → op16 DONE.
    vmthread_op42_ret_void(thr);
  }
  // Ensure DONE if popFrame left a soft non-zero PC without parent (leaf).
  if ((thr->pe.flags & kVmThreadFlagDone) == 0 && thr->pe.unk_24 == 0)
    vmthread_op16_done(thr);

  // PE epilogue @ 0x423936: flags &= ~4; restore JVM+0x78 (cursor unchanged).
  thr->pe.flags &= ~kVmThreadFlagRunning;
  return 0;
}

// PE Thread_setSleepDeadline @ 0x41F630 (Thread.sleep → JNI @ 0x47C650).
void vmthread_set_sleep_deadline(VmThread* thr, float ms) {
  if (!thr) return;
  thr->pe.sleep_deadline = vm_now_ms() + ms;
  thr->pe.flags |= kVmThreadFlagSleep;
}

// PE Thread_requestStop @ 0x41F780: if flags&0x10 → MonitorDequeue; |= 0x80.
void vmthread_request_stop(VmThread* thr) {
  if (!thr) return;
  thr->pe.flags |= kVmThreadFlagStop;
}

void vmthread_destroy(VmThread* thr) {
  if (!thr) return;
  vm_sched_unlink(thr);
  if (thr->frame_list) {
    for (VmCallFrame* f : thr->frame_list->frames) callframe_pool_release(f);
    thr->frame_list->frames.clear();
    thr->frame_list->cursor = nullptr;
    delete thr->frame_list;
    thr->frame_list = nullptr;
  }
  thr->curr_frame = nullptr;
  thr->pending.live = false;
  delete thr;
}

// PE Jvm_RunThreadsBudgeted @ 0x416940 (W34-17 deepen).
// while time < now+budget (budget==0 → until idle wrap):
//   cursor = JVM+0x78; wrap from *(JVM+0x18)+8 dllist head;
//   skip flags&0x82; require !(flags&0x44) && prio>=min;
//   clear sleep&8 when GetTimeMs > *(float*)(+0x30); prio-scale slice;
//   VMThread_run (PE always; host: pending.live → TREE + op42/43/16);
//   inside !&0x82: 0x40&!1 → |=STOP; THEN advance cursor via thr+4;
//   STOP&!RUNNING → vtbl dtor(1). saw_run (PE bl) only on VMThread_run.
int jvm_run_threads_budgeted(float budget_ms, int32_t min_prio) {
  const bool unlimited = (budget_ms == 0.f);
  float deadline = vm_now_ms() + budget_ms;
  float slice = 10.f;  // PE v17 / flt_5E7334 = 10.0f
  if (!unlimited && budget_ms < 10.f) slice = budget_ms;

  // PE bl @ 0x416950: run resets; second wrap without run → return 1.
  bool saw_run = true;
  while (true) {
    // PE @ 0x4169A7..0x4169B5: budget cut (skipped when a2==0).
    if (!unlimited && vm_now_ms() >= deadline) return 1;

    // PE @ 0x4169BB..0x4169DC: cursor null → reload list head; !saw_run→1.
    if (!g_vm_sched_cursor) {
      g_vm_sched_cursor = g_vm_sched_head;  // PE *(JVM+0x18)+8 gated
      if (!saw_run) return 1;
      saw_run = false;
    }
    VmThread* cur = g_vm_sched_cursor;
    if (!cur) return 0;  // PE @ 0x4169E3 empty → ret 0

    // PE body uses JVM+0x78 throughout; advance only @ 0x416AA4.
    if ((cur->pe.flags & kVmThreadFlagSkipMask) == 0) {
      // PE @ 0x4169F4: !(flags&0x44) && priority >= min.
      if ((cur->pe.flags & kVmThreadFlagBusyMask) == 0 &&
          cur->pe.priority >= min_prio) {
        // PE @ 0x416A05..0x416A1B: sleep&8 && now > +0x30 → clear &8.
        if ((cur->pe.flags & kVmThreadFlagSleep) != 0 &&
            vm_now_ms() > cur->pe.sleep_deadline) {
          cur->pe.flags &= ~kVmThreadFlagSleep;
        }
        if ((cur->pe.flags & kVmThreadFlagSleep) == 0) {
          // PE @ 0x416A28..0x416A6C: flt_5F08F8=0.1, F4=99, F0=1,
          // EC=0.9, E8=0.1 — priority-scaled slice.
          const int32_t prio = cur->pe.priority;
          float scale = 1.f;
          if (prio < 0)
            scale = static_cast<float>(prio + 10) * 0.1f * 0.9f + 0.1f;
          else
            scale = static_cast<float>(prio) * 0.1f * 99.f + 1.f;
          const float scaled = scale * slice;
          // PE @ 0x416A70..0x416A81: deadline = deadline - slice + scaled.
          if (!unlimited) deadline = deadline - slice + scaled;
          // PE always VMThread_run; host pending → TREE + op42/43/16 slice.
          if (cur->pending.live) {
            vmthread_run(cur, unlimited ? 0.f : scaled);
            saw_run = true;  // PE bl=1 @ 0x416A8C only here
          }
        }
      }
      // PE @ 0x416A8E..0x416AA1: DONE&!(flags&1 sync) → |= STOP.
      // Bit0 = CallNamedMethod sync; sync==1 keeps thread for caller.
      if ((cur->pe.flags & kVmThreadFlagDone) != 0 &&
          (cur->pe.flags & 1) == 0) {
        cur->pe.flags |= kVmThreadFlagStop;
      }
    }

    // PE @ 0x416AA4..0x416AB7: advance before dtor (thr+4 next; sentinel).
    g_vm_sched_cursor = cur->sched_next;

    // PE @ 0x416ABA..0x416AD9: !RUNNING && STOP → vtbl dtor(1).
    // Do NOT set saw_run — PE bl stays clear across destroy-only wraps.
    if ((cur->pe.flags & kVmThreadFlagRunning) == 0 &&
        (cur->pe.flags & kVmThreadFlagStop) != 0) {
      vmthread_destroy(cur);
    }
  }
}

// PE Jvm_PumpFrame @ 0x418D10 — MainLoop @ 0x428ABC when EngineState+0xE4
// (== g_JVM @ 0x63641C). PE order: ++Jvm+0x40 → Jvm_GcSlice @ 0x418C20 →
// (time start) mark-sweep if +0x34>+0x38 → EMA Jvm+0x24 →
// Jvm_RunThreadsBudgeted(budget, 0).
// W35-17: soft EMA +0x24 (VA clear, no bytecode); GcSlice + mark-sweep
// SKIP (dllists / FinalizeFree→opcode@4210D4). RunThreadsBudgeted already
// deepened W34-17 (cursor after body; saw_run only on run).
int32_t jvm_gc_time_ema() { return g_jvm_gc_ema; }

int jvm_pump_frame(float budget_ms) {
  ++g_jvm_pump_gen;  // PE ++*(Jvm+0x40)
  jvm_gc_slice();    // PE Jvm_GcSlice — SKIP body (see stub)
  // PE @ 0x418D2D: TimeMs AFTER GcSlice — EMA covers mark-sweep only.
  const float t0 = vm_now_ms();
  jvm_mark_sweep_shell();  // PE @ 0x418D37..0x418E5F — SKIP (OOS)
  const float t1 = vm_now_ms();
  // PE @ 0x418E62..0x418E90: Engine_ftol(delta_ms*1000*0.8 + old*0.2).
  g_jvm_gc_ema = static_cast<int32_t>((t1 - t0) * 1000.0 * 0.8 +
                                      static_cast<double>(g_jvm_gc_ema) * 0.2);
  return jvm_run_threads_budgeted(budget_ms, /*min_prio=*/0);
}

}  // namespace inv

