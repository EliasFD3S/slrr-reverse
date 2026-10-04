#pragma once
// Internal shared decls for jvm_*.cpp split (not a public API).

#include "jvm.hpp"
#include "natives.hpp"
#include "callinfo.hpp"

#include <cstdint>
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
void vmthread_op42_ret_void(VmThread* thr);         // PE @ 0x421697
void vmthread_op43_ret_val(VmThread* thr);          // PE @ 0x421662
void vmthread_op1001_local_load(VmThread* thr, uint32_t idx);  // PE @ 0x421714
void vmthread_op1007_literal(VmThread* thr, const JvmValue& lit);  // PE @ 0x4218A5
void vmthread_op29_field_get(VmThread* thr, InvObject* obj,
                             const char* name);  // PE @ 0x4214BC soft TREE
// op33/op34/op35/op36 public in jvm.hpp (Thread_callMethod /
// Object_callMethod / Object_callMethod_init / Object_callInitIf path).

}  // namespace inv
