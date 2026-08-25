#pragma once

#include <cstddef>
#include <cstdint>

namespace inv {

// Opaque Invictus heap object / Java instance handle (layout TBD via Ghidra).
struct InvObject;

struct NativeEntry {
  const char* class_fqn;
  const char* method_name;
  const char* java_signature;
  bool is_static;
  void* fn;
};

void inv_log_native(const char* class_fqn, const char* method);

extern const NativeEntry kNativeTable[];
extern const size_t kNativeTableCount;

const NativeEntry* find_native(const char* class_fqn, const char* method_name);

#include "natives_table.inc"

}  // namespace inv
