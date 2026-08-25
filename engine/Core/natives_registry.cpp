#include "natives.hpp"

#include <cstdio>
#include <cstring>

namespace inv {

void inv_log_native(const char* class_fqn, const char* method) {
  std::fprintf(stderr, "[native-stub] %s.%s\n", class_fqn, method);
}

const NativeEntry* find_native(const char* class_fqn, const char* method_name) {
  for (size_t i = 0; i < kNativeTableCount; ++i) {
    if (std::strcmp(kNativeTable[i].class_fqn, class_fqn) == 0 &&
        std::strcmp(kNativeTable[i].method_name, method_name) == 0) {
      return &kNativeTable[i];
    }
  }
  return nullptr;
}

}  // namespace inv
