#pragma once
// Internal shared decls for jvm_*.cpp split (not a public API).

#include "jvm.hpp"
#include "natives.hpp"
#include "callinfo.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace inv {

uint32_t pe_native_hash(const char* class_fqn, const char* method_name);
bool pe_jni_sig_equal(const char* registered_java_sig, const char* want_jni);
void soft_unbox_bind_registered_sig(CallFrame* frame, const NativeEntry* ne);
const NativeEntry* resolve_native(const char* fqn, const char* name,
                                  const char* jni_sig);
bool jvm_file_exists(const char* path);
std::vector<std::string> jvm_split_ws(const std::string& s);

// Soft PE opcode hosts in jvm_vmthread.cpp (VMThread_run @ 0x420FF0 /
// switch @ 0x4210D4). Wired on the TREE return path (op42/43/16); cold
// helpers kept for PC-stream slices (op4/5/24/40/29/0x1001/0x1007).
void vmthread_op16_done(VmThread* thr);             // PE @ 0x423932
void vmthread_op4_cond_skip(VmThread* thr);         // PE @ 0x42116C
void vmthread_op5_cond_skip(VmThread* thr);         // PE @ 0x4211E0
void vmthread_op24_pop_dtor(VmThread* thr);         // PE @ 0x4216BB
void vmthread_op40_pop_push(VmThread* thr);         // PE @ 0x42164E
void vmthread_op41_pool_alloc(VmThread* thr);       // PE @ 0x421583 soft
void vmthread_op42_ret_void(VmThread* thr);         // PE @ 0x421697
void vmthread_op43_ret_val(VmThread* thr);          // PE @ 0x421662
void vmthread_op1001_local_load(VmThread* thr, uint32_t idx);  // PE @ 0x421714
void vmthread_op1002_local_declare(VmThread* thr, uint32_t idx);  // PE @ 0x421752
void vmthread_op1007_literal(VmThread* thr, const JvmValue& lit);  // PE @ 0x4218A5
void vmthread_op29_field_get(VmThread* thr, InvObject* obj,
                             const char* name);  // PE @ 0x4214BC soft TREE
void vmthread_op32_array_access(VmThread* thr);   // PE @ 0x421398 soft
void vmthread_op45_assign_store(VmThread* thr);   // PE @ 0x4210EC soft
// The PE ValueField handle model (ValueField_ctor @ 0x423EC0, getType()==2
// "is a variable") lives entirely inside jvm_vmthread.cpp: push_ref/pop_ref
// and Value_assignOp '#' operate on VmCallFrame::operand_ref.
// Hi cascade (op>0x4A) — Soft cold helpers; NOT mainloop_1to1.
void vmthread_op1016_bool_cond_skip(VmThread* thr, int32_t imm);  // PE @ 0x423267
void vmthread_op1017_case_eq_skip(VmThread* thr, int32_t imm);    // PE @ 0x423608
void vmthread_op101F_push_nulls(VmThread* thr, int32_t count);    // PE @ 0x423585
void vmthread_op101B_field_ref(VmThread* thr, const char* name, bool is_static,
                               const JvmClass* owner);  // PE @ 0x4234B4
// PE Class_findFieldSlot @ 0x405690 — static hit returns slot | 0x40000000,
// instance hit returns slot + ancestor instance-field counts, miss -1.
int32_t vmthread_class_find_field_slot(const JvmClass* cls,
                                       const std::string& name,
                                       const JvmClass** owner_out);
// PE ConstantPool_resolveFieldRef @ 0x406D00 — name + the static bit 0x4234D9
// branches on.
bool vmthread_resolve_field_ref(const JvmClass* cls, uint32_t cp_index,
                                std::string* name_out, bool* is_static_out,
                                const JvmClass** owner_out = nullptr);
void vmthread_op101E_array_init(VmThread* thr, int32_t count);  // PE @ 0x4232BB soft
void vmthread_op1003_pop_release(VmThread* thr);                // PE @ 0x421823
void vmthread_op1014_goto(VmThread* thr, int32_t imm);          // PE @ 0x423648
void vmthread_op1027_box_imm(VmThread* thr, int32_t imm);       // PE @ 0x423677

// PE insn stream walk (VMThread_run @ 0x420FF0, loop @ 0x42108B). The stream
// is clazz.trees[method.tree_index] — 8B nodes, PC = thr+0x24, stride 8.
constexpr int kVmStreamDone = 0;         // op16 @ 0x423932 / code exhausted
constexpr int kVmStreamReturned = 1;     // op42 @ 0x421697 / op43 @ 0x421662
constexpr int kVmStreamYield = 2;        // budget or flags&0x78 @ 0x423918
constexpr int kVmStreamUnsupported = 3;  // opcode without a host case yet

uint16_t vmthread_pe_opcode(const TreeNode& n);  // TreeOpFlags @ 0x5F0914
int vmthread_pe_case(uint16_t op);               // case map @ 0x4239B0
bool vmthread_pe_op_walkable(uint16_t op);
bool vmthread_node_walkable(const TreeBody& code, size_t idx);
bool vmthread_stream_walkable(const TreeBody& code);
void vmthread_stream_census(const TreeBody& code, uint32_t* walkable,
                            uint32_t* needs_case);
// Per-opcode tally of the nodes the walker still cannot execute.
void vmthread_stream_blocking_ops(const TreeBody& code,
                                  std::map<uint16_t, uint32_t>* hist);
// Returns false on unsupported tag. When skip_carrier is set, tag 0x20
// (instanceof) reports that PE already advanced past the following carrier.
bool vmthread_op1007_literal(VmThread* thr, const JvmClass* cls, int32_t tag,
                             const TreeNode* carrier,
                             bool* skip_carrier = nullptr);  // PE @ 0x4218A5
int vmthread_exec_stream(VmThread* thr, const JvmClass* cls,
                         const TreeBody& code, float budget_ms);
// PE CallFrame seed @ 0x420211 — used by VMThread_run and Jvm::invoke.
VmCallFrame* vmthread_build_tree_frame(VmThread* thr, const JvmClass* cls,
                                       const JvmMethod* m, InvObject* self,
                                       const std::vector<JvmValue>& args);
// Soft sync path for Jvm::invoke: when the TREE is fully walkable, build a
// CallFrame and run vmthread_exec_stream (PE invokeMethod→run). Returns false
// to keep tree_eval. Opt-out SLRR_PE_STREAM=0.
bool vmthread_try_stream_eval(const JvmClass* cls, const JvmMethod* m,
                              InvObject* self,
                              const std::vector<JvmValue>& args,
                              JvmValue* out);

struct VmStreamStats {
  uint32_t trees = 0;
  uint32_t trees_walkable = 0;
  uint32_t insns = 0;
  uint32_t insns_walkable = 0;
};
const VmStreamStats& vmthread_stream_stats();
// op33/op34/op35/op36 public in jvm.hpp (Thread_callMethod /
// Object_callMethod / Object_callMethod_init / Object_callInitIf path).

}  // namespace inv
