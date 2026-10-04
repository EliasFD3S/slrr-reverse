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

// Soft PE deepen — JVM_RegisterNative @ 0x00416B00 / UnboxArg @ 0x0045D910 /
// NativeHash_find @ 0x00423DA0 / NativeHash_next @ 0x00423DE0.
// ACC_NATIVE @ 0x0041FC2D..0x0041FD3C (no bytecode @ 0x4210D4):
//   hash = 73 * sum(methodName bytes) + sum(classFqn bytes);  // IDA 73=0x49
//   bucket = hash & 0x7F on table at *(JVM+0xC) (12B=0xC slots × 128);
//   find → chain next; strcmp NativeRec +0xC/+0x10;
//   NativeSigDesc_sameIntern @ 0x0041DED0 (Method.sig ptr == SigDesc+4).
// Soft: 128-bucket index over kNativeTable (append = register order).
//
// PE NativeRec 0x18 (vtbl NativeRec_vftable @ 0x5F08FC):
//   +0x00 vtbl | +0x04 JVM* | +0x08 nativeImpl
//   +0x0C classFqn | +0x10 methodName | +0x14 NativeSigDesc*
// PE NativeSigDesc 0x0C (NativeSigDesc_ctor @ 0x0041DC30):
//   +0 JVM* | +4 interned JNI via JVM_InternCString @ 0x0041E800
//   NativeSigDesc_getJni @ 0x0041DEF0 returns +4 (full "(..)X" from register).
// PE NativeHash cursor dword @ table+0x628 (this+394 dwords).
uint32_t pe_native_hash(const char* class_fqn, const char* method_name) {
  // PE RegisterNative @ 0x416B84..0x416BB0: sum(method) first, then class.
  uint32_t sum_m = 0;
  if (method_name) {
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(method_name);
         *p; ++p)
      sum_m += *p;
  }
  uint32_t h = 73u * sum_m;
  if (class_fqn) {
    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(class_fqn);
         *p; ++p)
      h += *p;
  }
  return h;
}

bool pe_jni_sig_equal(const char* registered_java_sig, const char* want_jni) {
  // Soft NativeSigDesc_sameIntern @ 0x41DED0 (PE: interned ptr eq).
  if (!want_jni || !want_jni[0]) return true;
  if (!registered_java_sig || !registered_java_sig[0]) return false;
  if (std::strcmp(registered_java_sig, want_jni) == 0) return true;
  const std::string reg = java_sig_to_jni(registered_java_sig);
  if (!reg.empty() && reg == want_jni) return true;
  // want may also be inventory Java form ("float foo(int)").
  const std::string want = java_sig_to_jni(want_jni);
  if (want.empty()) return false;
  if (!reg.empty()) return reg == want;
  return std::strcmp(registered_java_sig, want.c_str()) == 0;
}

namespace {

constexpr size_t kPeNativeBucketCount = 128;  // hash & 0x7F

// Soft stand-in for PE 12B hash slot / overflow node (hash | NativeRec** | next).
struct SoftNativeHashNode {
  uint32_t hash = 0;
  size_t table_index = 0;  // → kNativeTable[i] ≡ NativeRec*
  SoftNativeHashNode* next = nullptr;
};

struct SoftNativeHashTable {
  SoftNativeHashNode* buckets[kPeNativeBucketCount]{};
  SoftNativeHashNode* cursor = nullptr;  // PE NativeHash +0x628
  std::vector<std::unique_ptr<SoftNativeHashNode>> owned;
  bool built = false;
};

SoftNativeHashTable g_soft_native_hash;

// Soft RegisterNative insert (bucket append ≈ first-registered-first-found).
void soft_native_hash_ensure() {
  if (g_soft_native_hash.built) return;
  for (size_t i = 0; i < kNativeTableCount; ++i) {
    const NativeEntry& e = kNativeTable[i];
    const uint32_t h = pe_native_hash(e.class_fqn, e.method_name);
    const size_t b = static_cast<size_t>(h & 0x7Fu);
    auto node = std::make_unique<SoftNativeHashNode>();
    node->hash = h;
    node->table_index = i;
    SoftNativeHashNode** slot = &g_soft_native_hash.buckets[b];
    while (*slot) slot = &(*slot)->next;
    *slot = node.get();
    g_soft_native_hash.owned.push_back(std::move(node));
  }
  g_soft_native_hash.built = true;
}

// Soft NativeHash_find @ 0x00423DA0 — walk bucket until full hash match.
SoftNativeHashNode* soft_native_hash_find(uint32_t hash) {
  soft_native_hash_ensure();
  SoftNativeHashNode* n = g_soft_native_hash.buckets[hash & 0x7Fu];
  while (n) {
    if (n->hash == hash) {
      g_soft_native_hash.cursor = n;
      return n;
    }
    n = n->next;
  }
  g_soft_native_hash.cursor = nullptr;
  return nullptr;
}

// Soft NativeHash_next @ 0x00423DE0 — next node with same full hash.
SoftNativeHashNode* soft_native_hash_next() {
  SoftNativeHashNode* cur = g_soft_native_hash.cursor;
  if (!cur) return nullptr;
  const uint32_t h = cur->hash;
  SoftNativeHashNode* n = cur->next;
  while (n) {
    if (n->hash == h) {
      g_soft_native_hash.cursor = n;
      return n;
    }
    n = n->next;
  }
  g_soft_native_hash.cursor = nullptr;
  return nullptr;
}

// Soft NativeSigDesc_getJni @ 0x0041DEF0 — cache inventory → "(..)X".
const char* soft_sigdesc_get_jni(const NativeEntry* ne) {
  if (!ne) return "()V";
  if (!ne->java_signature || !ne->java_signature[0]) return "()V";
  if (ne->java_signature[0] == '(') return ne->java_signature;
  struct SoftJniCacheEntry {
    const NativeEntry* ne;
    std::string jni;
  };
  static std::vector<SoftJniCacheEntry> cache;
  for (const SoftJniCacheEntry& kv : cache) {
    if (kv.ne == ne) return kv.jni.c_str();
  }
  std::string jni = java_sig_to_jni(ne->java_signature);
  if (jni.empty()) return ne->java_signature;
  cache.push_back(SoftJniCacheEntry{ne, std::move(jni)});
  return cache.back().jni.c_str();
}

// Soft UnboxArg type walk @ 0x45D94E..0x45DB4E (I/F/L/[ → box+8).
// Returns JNI param count, or -1 on unknown type (PE ErrorLogMsgBox path).
int soft_unbox_pe_argc(const char* jni) {
  if (!jni || jni[0] != '(') return -1;
  const char* p = jni + 1;
  int n = 0;
  while (*p && *p != ')') {
    const char t = *p;
    if (t == 'I' || t == 'F') {
      ++p;
      ++n;
      continue;
    }
    if (t == '[') {
      ++p;
      while (*p == '[') ++p;
      // Fall into L/I/F element handling (PE LABEL_16).
    } else if (t != 'L') {
      return -1;
    }
    // L or […L / […I / […F
    if (*p == 'I' || *p == 'F') {
      ++p;
      ++n;
      continue;
    }
    if (*p != 'L') return -1;
    ++p;
    while (*p && *p != ';') ++p;
    if (*p != ';') return -1;
    ++p;
    ++n;
  }
  return (*p == ')') ? n : -1;
}

}  // namespace

// Soft UnboxArg @ 0x0045D910: CallInfo+0xC → NativeRec+0x14 → SigDesc_getJni;
// host CallFrame.jni_signature = registered "(..)X", not TREE arg tags
// (e.g. getAxis TREE (FF)F vs registry (II)F).
void soft_unbox_bind_registered_sig(CallFrame* frame, const NativeEntry* ne) {
  if (!frame || !ne) return;
  frame->class_fqn = ne->class_fqn;
  frame->method_name = ne->method_name;
  frame->is_static = ne->is_static;
  // Soft SigDesc_getJni; call_native also re-derives from ne->java_signature.
  frame->jni_signature = soft_sigdesc_get_jni(ne);
  // Soft UnboxArg arity: walk I/F/L/[ ; pad if short (PE ErrorLog @ 0x45DACE).
  const int pe_argc = soft_unbox_pe_argc(frame->jni_signature);
  if (pe_argc >= 0) {
    const size_t need =
        static_cast<size_t>(pe_argc) + (ne->is_static ? 0u : 1u);
    while (frame->args.size() < need)
      frame->args.push_back(JvmValue::make_int(0));
  }
}

const NativeEntry* resolve_native(const char* fqn, const char* name,
                                  const char* jni_sig) {
  if (!fqn || !name) return nullptr;
  // PE ACC_NATIVE: pe_native_hash → NativeHash_find → next; strcmp; sameIntern.
  const uint32_t h = pe_native_hash(fqn, name);
  SoftNativeHashNode* node = soft_native_hash_find(h);
  const NativeEntry* first = nullptr;
  const NativeEntry* sig_hit = nullptr;
  while (node) {
    const NativeEntry& e = kNativeTable[node->table_index];
    if (std::strcmp(e.class_fqn, fqn) == 0 &&
        std::strcmp(e.method_name, name) == 0) {
      if (!first) first = &e;
      if (pe_jni_sig_equal(e.java_signature, jni_sig)) {
        sig_hit = &e;
        break;
      }
    }
    node = soft_native_hash_next();
  }
  return sig_hit ? sig_hit : first;
}

}  // namespace inv
