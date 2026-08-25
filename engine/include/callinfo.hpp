#pragma once

#include "jvm.hpp"
#include "natives.hpp"

#include <string>
#include <vector>

namespace inv {

// Lightweight rewrite-side call frame (stock uses CallInfo* + JVM_UnboxArg).
struct CallFrame {
  const char* class_fqn = nullptr;
  const char* method_name = nullptr;
  const char* jni_signature = nullptr;
  bool is_static = true;
  std::vector<JvmValue> args;  // instance methods: args[0] = this
  JvmValue result;
};

// Parse JNI-like descriptor into return tag + arg tags (objects collapsed to Obj).
bool jni_parse(const char* signature, JvmTag* ret, std::vector<JvmTag>* args,
               std::string* err);

// Stock JVM_UnboxArg @ 0x0045D910 reads the *registered* native descriptor,
// not TREE boxed tags. Convert kNativeTable java_signature → JNI.
std::string java_sig_to_jni(const char* java_sig);

// Dispatch already-unboxed host native (kNativeTable fn) from a CallFrame.
bool call_native(const NativeEntry* entry, CallFrame* frame, std::string* err);

}  // namespace inv
