#pragma once

#include <cstddef>
#include <cstdint>

namespace inv {

// Path → Java package roots recovered from JVM_bootstrap_GameRef.
struct ClasspathMapEntry {
  const char* filesystem_prefix;  // e.g. "system/Scripts/lang"
  const char* java_package;       // e.g. "java.lang"
};

extern const ClasspathMapEntry kClasspathMap[];
extern const size_t kClasspathMapCount;

// Mirrors FUN_00416b00 / JVM_RegisterNative:
//   RegisterNative(classFqn, methodName, jniLikeSignature, fn)
using NativeFn = void*;  // actual ABI TBD per signature

bool register_native(const char* class_fqn,
                     const char* method_name,
                     const char* signature,
                     NativeFn fn);

// Lookup by class+name+signature (signature may be nullptr to match first name).
const struct NativeEntry* find_native_sig(const char* class_fqn,
                                          const char* method_name,
                                          const char* signature);

void register_all_stubs();

}  // namespace inv
