#pragma once

#include "natives.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace inv {

enum class JvmTag : uint8_t { Int, Float, Obj, Void };

struct JvmValue {
  JvmTag tag = JvmTag::Void;
  union {
    int32_t i;
    float f;
    InvObject* o;
  } v{};

  static JvmValue make_int(int32_t x) {
    JvmValue r;
    r.tag = JvmTag::Int;
    r.v.i = x;
    return r;
  }
  static JvmValue make_float(float x) {
    JvmValue r;
    r.tag = JvmTag::Float;
    r.v.f = x;
    return r;
  }
  static JvmValue make_obj(InvObject* x) {
    JvmValue r;
    r.tag = JvmTag::Obj;
    r.v.o = x;
    return r;
  }
  static JvmValue make_void() { return {}; }
};

// On-disk SLRR TREE node (3 or 7 bytes) — see TREE_readNode @ 0041a5f0.
struct TreeNode {
  uint8_t op = 0;
  uint16_t slot = 0;
  uint32_t imm = 0;
  bool has_imm = false;
};

struct TreeBody {
  std::vector<TreeNode> nodes;
};

struct JvmMethod {
  std::string name;
  std::string signature;  // JNI-like
  bool is_native = false;
  int tree_index = -1;
  uint32_t flags = 0;
};

// Instance field with a dedicated TREE initializer (FILD: w0==0, tree!=-1).
struct JvmFieldInit {
  std::string name;
  int tree_index = -1;
};

// One FILD record (PE reader @ 0x00419250, 16 bytes after the vtbl slot):
// +4 w0, +8 name CP index, +0xC type CP index, +0x10 init tree (-1 = none).
struct JvmFieldDecl {
  std::string name;
  std::string type;
  uint32_t w0 = 0;
  int tree_index = -1;
};

struct JvmClass {
  std::string name;
  std::string super_name;
  std::string file;
  std::vector<JvmMethod> methods;
  std::vector<TreeBody> trees;
  std::vector<JvmFieldInit> field_inits;
  // FILD ships two ordered vectors (PE JVM_addClass_fromChunks @ 0x00411BA1):
  // statics land at class+0x54, instance fields at class+0x4C. The order is
  // the slot order Class_findFieldSlot @ 0x00405690 scans, so it must be kept.
  std::vector<JvmFieldDecl> static_fields;
  std::vector<JvmFieldDecl> instance_fields;
  // Const pool (parallel arrays). Strings for Utf8; mref_name[i] set when
  // entry i is an mref resolving to a field/method name.
  std::vector<std::string> const_strings;
  std::vector<std::string> const_mref_name;
  // Parallel int constants (RID / INT pool entries); valid[i]==1 when set.
  std::vector<int32_t> const_ints;
  std::vector<uint8_t> const_int_valid;
  // When entry i is a RID (CONS kind 3), pack path e.g. "cars\\racers\\Baiern.rpk".
  std::vector<std::string> const_rid_pack;
};

class Jvm {
 public:
  // Game install root containing system/Scripts/...
  void set_game_root(const char* root);

  bool load_index(const char* path);
  bool load_class_file(const char* path);
  // Resolve FQN via classpath map and load the stock .class.
  bool load_class(const char* fqn);

  // MainMenu CMD_COMPILEFILES: scan path (or all classpath roots if ".") for
  // *.class and load TUFA. Returns number of .class files loaded OK.
  int32_t compile_all(const char* rel_path);

  const JvmClass* find_class(const char* fqn) const;
  const JvmMethod* find_method(const JvmClass& cls, const char* name,
                               const char* signature) const;

  JvmValue invoke(const char* class_fqn, const char* name, const char* signature,
                  const std::vector<JvmValue>& args, bool is_static);

  size_t class_count() const { return classes_.size(); }
  const char* game_root() const { return game_root_.c_str(); }

 private:
  void upsert_class(JvmClass cls);
  std::string game_root_;
  std::vector<JvmClass> classes_;
};

// Active JVM for natives that need to invoke TREE (GameRef.create → *_VT.<init>).
void jvm_set_active(Jvm* j);
Jvm* jvm_active();

bool resolve_classpath_file(const char* game_root, const char* fqn,
                            std::string* out_path);

// ---------------------------------------------------------------------------
// PE VMThread blob — CallNamedMethod_va @ 0x4255E0 / Thread.init @ 0x47C510
// malloc(56) → VMThread_init @ 0x41F340. DWORD layout (int_convert):
//   +0x00 vtbl (VMThread_vtbl @ 0x5F098C)
//   +0x04 / +0x08 JVM green-thread dllist links
//   +0x0C frame_list* (malloc 28 / VMThread_FrameList_vtbl @ 0x5F0988)
//   +0x10 JVM*  +0x14 *(JVM+4)  +0x18 java back-ref  +0x1C name*
//   +0x20 curr_frame* (VMThread_CallFrame_ctor @ 0x407B10, pool 64B)
//   +0x24 0  +0x28 priority  +0x2C flags (sync; STOP |= 0x80 @ requestStop)
//   +0x30..0x37 residual of malloc(56)
// pushCallFrame @ 0x41F9F0 pushes packArgs boxes onto curr_frame+0x28.
// CallFrame pool g_CallFramePool @ 0x62DDF0 / CallFramePool_alloc @ 0x408D90
//   (64B slots, 256 per 0x4000 chunk). invokeMethod @ 0x41FBC0:
//   ACC_NATIVE ret1; Java queue ret0 → VMThread_run / Jvm_PumpFrame drain.
// Host: PE-sized uint32 words + host CallFrame/FrameList + green sched list.
// ---------------------------------------------------------------------------
constexpr std::size_t kVmThreadBlobSize = 56;   // 0x38
constexpr std::size_t kCallFrameBlobSize = 64;  // 0x40
constexpr std::size_t kVmFrameListBlobSize = 28;  // 0x1C

struct VmThreadPe {
  uint32_t vtbl = 0;          // +0x00
  uint32_t link_prev = 0;     // +0x04
  uint32_t link_next = 0;     // +0x08
  uint32_t frame_list = 0;    // +0x0C
  uint32_t jvm = 0;           // +0x10
  uint32_t jvm_aux = 0;       // +0x14
  uint32_t java_backref = 0;  // +0x18
  uint32_t name = 0;          // +0x1C (PE char*; host uses name_storage)
  uint32_t curr_frame = 0;    // +0x20
  uint32_t unk_24 = 0;        // +0x24
  int32_t priority = 0;       // +0x28
  int32_t flags = 0;          // +0x2C
  float sleep_deadline = 0.f; // +0x30 Thread_setSleepDeadline @ 0x41F630
  uint32_t notify_34 = 0;     // +0x34 Instance notify list (markStopped)
};
static_assert(sizeof(VmThreadPe) == kVmThreadBlobSize, "PE VMThread 56B");

// PE VMThread_CallFrame_ctor @ 0x407B10 — 16 dwords / 64B.
struct VmCallFramePe {
  uint32_t vtbl = 0;         // +0x00 off_5E7410
  uint32_t link_prev = 0;    // +0x04
  uint32_t link_next = 0;    // +0x08
  uint32_t local_cap = 0;    // +0x0C
  uint32_t local_data = 0;   // +0x10
  uint32_t local_count = 0;  // +0x14
  uint32_t local_hdr = 0;    // +0x18 (= this+0x0C PE)
  uint32_t op_cap = 0;       // +0x1C
  uint32_t op_data = 0;      // +0x20
  uint32_t op_count = 0;     // +0x24
  uint32_t op_hdr = 0;       // +0x28 (= this+0x1C PE; pushCallFrame target)
  uint32_t code = 0;         // +0x2C a4
  uint32_t instance = 0;     // +0x30 a5
  uint32_t clazz = 0;        // +0x34 a3
  uint32_t method_name = 0;  // +0x38
  uint32_t vmthread = 0;     // +0x3C a2
};
static_assert(sizeof(VmCallFramePe) == kCallFrameBlobSize, "PE CallFrame 64B");

// PE ValueField_ctor @ 0x00423EC0: a Value {vtbl, typeDesc, payload} with a
// name at +0xC, re-pointed at ValueField_vtbl @ 0x005E7344 whose slot 0 is
// ValueField_getType @ 0x00405030 (returns 2) rather than Value_getType.
// Everything VMThread_run calls "a variable on the stack" is this handle:
// op 0x1001 @ 0x00421714 pushes locals[i], op 0x101B @ 0x004234B4 pushes a
// field, op 0x1011 @ 0x00422E98 pushes a resolved name path, and
// Value_assignOp @ 0x0041ACB0 refuses anything else ("stack access: value on
// top (instead of variable)") because it needs somewhere to write.
//
// The host operand stack holds JvmValue by value and is shared with natives
// and tree_eval, so widening it is not an option. Instead each operand slot
// carries a parallel descriptor naming where the value came from; pure
// r-values leave it None, matching a plain Value on the PE stack.
enum class VmRefKind : uint8_t { None, Local, Field, Static };

struct VmValueRef {
  VmRefKind kind = VmRefKind::None;
  uint32_t local_index = 0;        // Local: index into VmCallFrame::locals
  InvObject* obj = nullptr;        // Field: receiver
  const JvmClass* owner = nullptr; // Static: class owning the Instance @ +0x1D0
  std::string name;                // Field / Static: field name
};

struct VmCallFrame {
  VmCallFramePe pe;
  std::vector<JvmValue> locals;
  std::vector<JvmValue> operand;
  // Parallel to `operand`, same size; see VmValueRef above.
  std::vector<VmValueRef> operand_ref;
  VmCallFrame* dllist_prev = nullptr;
  VmCallFrame* dllist_next = nullptr;
};

// PE malloc(28) FrameList @ VMThread_init — circular dllist of CallFrames.
struct VmFrameList {
  VmCallFrame* cursor = nullptr;  // PE frame_list+8 insert point
  std::vector<VmCallFrame*> frames;
};

// PE VMThread+0x2C flags (Jvm_RunThreadsBudgeted @ 0x416940 / VMThread_run).
constexpr int32_t kVmThreadFlagRunning = 0x4;    // set while VMThread_run
constexpr int32_t kVmThreadFlagSleep = 0x8;      // skip until +0x30 deadline
constexpr int32_t kVmThreadFlagDone = 0x40;      // case16 @ 0x423932; → STOP
constexpr int32_t kVmThreadFlagStop = 0x80;      // requestStop; pump dtor
constexpr int32_t kVmThreadFlagSkipMask = 0x82;  // STOP|0x02 — outer skip
constexpr int32_t kVmThreadFlagBusyMask = 0x44;  // RUNNING|DONE — no run
constexpr int32_t kVmThreadFlagYieldMask = 0x78; // PE @ 0x423918 mid-loop yield
                                                 // (Sleep|0x10|0x20|Done); OOS host

// Pending TREE/jvm invoke queued by invokeMethod Java path (PE ret 0).
struct VmPendingInvoke {
  bool live = false;
  std::string class_fqn;
  std::string method;
  std::string signature;
  std::vector<JvmValue> args;
};

struct VmThread {
  VmThreadPe pe;
  std::string name_storage;
  // Host mirrors of packArgs vec (malloc 12) + pushCallFrame operand stack.
  std::vector<JvmValue> pack_vec;
  std::vector<JvmValue> operand;
  VmCallFrame* curr_frame = nullptr;
  VmFrameList* frame_list = nullptr;
  // PE green-thread dllist (JVM+0x18 list @ VMThread_init); host registry.
  VmThread* sched_prev = nullptr;
  VmThread* sched_next = nullptr;
  VmPendingInvoke pending;
};

// PE VMThread_init @ 0x41F340 — CallNamedMethod uses prio=10, sync=0|1.
// Links into host green-thread list (PE: *(JVM+0x18)+0x18 dllist).
// LoadGameInit THRD-CREATE @ 0x53A8FD: same init (prio=10, sync=0, name).
VmThread* vmthread_init(void* jvm, int32_t priority, int32_t sync_flags,
                        const char* name);
// PE packArgs tag3 @ 0x425380: box float → append to pack_vec (host).
void vmthread_pack_arg_float(VmThread* thr, float f);
// THRD-CREATE @ 0x53A960 packs InvObject(+8=AllocLocalRid) as <init>(I) —
// host: thr->pack_vec.push_back(JvmValue::make_int(rid)) then pushCallFrame.
// PE VMThread_pushCallFrame @ 0x41F9F0: copy pack_vec → curr_frame+0x28 + count.
int vmthread_push_call_frame(VmThread* thr);
// PE VMThread_invokeMethod @ 0x41FBC0:
//   ACC_NATIVE → Native.ptr unbox + jvm invoke; ret 1.
//   else CallFramePool+ctor queue + pending TREE; ret 0 (no bytecode VM).
// Returns 0 (Java queued), 1 (native done), -1 (error) — PE sentinel.
int vmthread_invoke_method(VmThread* thr, InvObject* self, const char* class_fqn,
                           const char* method, const char* signature);
// PE Thread_callMethod @ 0x4207C0: NativeSigDesc_ctorFromOperands @ 0x41DCB0
// + getJni @ 0x41DEF0 + Class_lookupMethod_nameSig @ 0x404910 → invokeMethod.
// Soft: JNI from pack_vec tags + name lookup (find_method name fallback).
int vmthread_thread_call_method(VmThread* thr, InvObject* self,
                                const char* class_fqn, const char* method);
// PE op33 @ 0x421254 → Object_callMethod @ 0x408A30 (PE pushes "<init>").
// Soft: Thread_callMethod with caller-supplied name (Object_callMethod general).
int vmthread_op33_call_method(VmThread* thr, InvObject* self,
                              const char* class_fqn, const char* method);
// PE op34 @ 0x4212C7 → Object_callMethod_init @ 0x408A70:
//   Thread_callMethod(thr, clazz, this, "<init>") — hardcodes "<init>".
int vmthread_op34_call_method_init(VmThread* thr, InvObject* self,
                                   const char* class_fqn);
// PE op35 @ 0x4212A4 → Object_callInitIf @ 0x408A90:
//   clazz!=0 → Thread_callMethod "<init>"; else ret 1 (continue).
int vmthread_op35_call_init_if(VmThread* thr, InvObject* self,
                               const char* class_fqn);
// PE op36 @ 0x4212E4: PC+=8; Thread_evalName @ 0x4208E0 → invokeMethod /
// Thread_callMethod; ret0 → yield (no default advance), else PC default.
// Soft: no CP stream — resolved FQN/method/sig; soft PC += 8; invoke_method.
int vmthread_op36_invoke(VmThread* thr, InvObject* self, const char* class_fqn,
                         const char* method, const char* signature);
// PE VMThread_run @ 0x420FF0 size 0x2979 stand-in: drain pending → TREE.
// Prologue: empty +0x24 / already RUNNING&4; |=4; set JVM+0x78 cursor.
// Epilogue @ 0x423936: flags&=~4; restore JVM+0x78. Host: TREE one-shot
// ≡ op16 DONE (0x423932 |=0x40) — no bytecode loop.
// budget_ms==0 → no cutoff (CallNamedMethod_va @ 0x425714).
// W14D opcode inventory (IDA; do NOT invent interpreter) — residual
// switch @ 0x4210D4..0x4238ED size 0x2979 / 10617B, 73 low cases:
//   control: 16 DONE; 42 ret-void; 43 ret-val;
//     4 @0x42116C / 5 @0x4211E0 cond-skip DONE soft (vmthread_op4/5);
//     loop VMThread_opDefault_advance @0x4238ED PC+8 + budget + flags&0x78
//   stack: 24 pop+dtor; 40 pop+ValueStack_push; 45 dual-pop store
//   invoke: 33 Object_callMethod DONE soft; 34 Object_callMethod_init DONE soft
//     (vmthread_op34_call_method_init); 35 Object_callInitIf DONE soft
//     (vmthread_op35_call_init_if); 36 DONE soft via vmthread_op36_invoke
//     (Thread_callMethod@0x4207C0)
//   data: 29 field-get; 32 JT_ARRAYACCESS; 41 pool alloc DONE soft
//     (vmthread_op41_pool_alloc @ 0x421583)
//   illegal: 0/19/66/72 + 'J'/'M' → fatal script error
//   hi>0x4A @0x4216D5: 0x1001..0x1027+ literals(0x1007×47)/ops/
//     fields(JT_FIELD_REF)/calls — residual, no host handlers
int vmthread_run(VmThread* thr, float budget_ms);
// PE Thread_setSleepDeadline @ 0x41F630: +0x30 = now+ms; flags |= 8.
void vmthread_set_sleep_deadline(VmThread* thr, float ms);
// PE Thread_requestStop @ 0x41F780: flags |= 0x80.
void vmthread_request_stop(VmThread* thr);
// PE vtbl dtor after CallNamedMethod_va / RunThreadsBudgeted STOP — free blob.
void vmthread_destroy(VmThread* thr);

// PE Jvm_RunThreadsBudgeted @ 0x416940: walk green list under time budget;
// skip &0x82; require !(flags&0x44) && prio>=min; clear sleep&8 past +0x30;
// prio-scale slice; VMThread_run; 0x40&!1→STOP; STOP&!RUNNING → dtor.
// Returns 1 if budget exhausted, 0 if list drained.
int jvm_run_threads_budgeted(float budget_ms, int32_t min_prio = 0);
// PE Jvm_PumpFrame @ 0x418D10: MainLoop when EngineState+0xE4 (==g_JVM
// @ 0x63641C); ++Jvm+0x40 → Jvm_GcSlice @ 0x418C20 (host SKIP: needs
// +0x44/+0x30/+0x2C dllists + Object_FinalizeFree→bytecode) →
// mark-sweep if +0x34>+0x38 (host SKIP — DrainGrey/Seed*/MarkGrey need
// object dllists; Finalize path → opcode@4210D4) → EMA Jvm+0x24
// (soft: ftol(delta_ms*1000*0.8+old*0.2)) → RunThreadsBudgeted(budget, 0).
int32_t jvm_gc_time_ema();  // PE dword Jvm+0x24 / System.info(3)
int jvm_pump_frame(float budget_ms = 10.f);

}  // namespace inv
