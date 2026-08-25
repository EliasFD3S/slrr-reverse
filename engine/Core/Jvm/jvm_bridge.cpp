#include "jvm_bridge.hpp"
#include "natives.hpp"

#include <cstdio>
#include <cstring>

namespace inv {

// From JVM_bootstrap_GameRef (0040fe70): FUN_0041cfe0(fs, package)
const ClasspathMapEntry kClasspathMap[] = {
    {"system/Scripts/lang", "java.lang"},
    {"system/Scripts/io", "java.io"},
    {"system/Scripts/util", "java.util"},
    {"system/Scripts/render", "java.render"},
    {"system/Scripts/sound", "java.sound"},
    {"sl/Scripts/game", "java.game"},
    {"parts/scripts", "java.game.parts"},
    {"parts/accessories/scripts", "java.game.parts.accessories"},
};

const size_t kClasspathMapCount =
    sizeof(kClasspathMap) / sizeof(kClasspathMap[0]);

struct DynamicNative {
  const char* class_fqn;
  const char* method_name;
  const char* signature;
  NativeFn fn;
};

static constexpr size_t kDynCap = 512;
static DynamicNative g_dyn[kDynCap];
static size_t g_dyn_count = 0;

bool register_native(const char* class_fqn,
                     const char* method_name,
                     const char* signature,
                     NativeFn fn) {
  if (g_dyn_count >= kDynCap) {
    std::fprintf(stderr, "[jvm] register_native table full\n");
    return false;
  }
  g_dyn[g_dyn_count++] = {class_fqn, method_name, signature, fn};
  return true;
}

const NativeEntry* find_native_sig(const char* class_fqn,
                                   const char* method_name,
                                   const char* signature) {
  // Prefer static generated table (Java inventory).
  for (size_t i = 0; i < kNativeTableCount; ++i) {
    if (std::strcmp(kNativeTable[i].class_fqn, class_fqn) != 0) continue;
    if (std::strcmp(kNativeTable[i].method_name, method_name) != 0) continue;
    if (signature == nullptr ||
        std::strcmp(kNativeTable[i].java_signature, signature) == 0) {
      return &kNativeTable[i];
    }
  }
  return nullptr;
}

void register_all_stubs() {
  // Re-register generated stubs through the same API the exe uses.
  g_dyn_count = 0;
  for (size_t i = 0; i < kNativeTableCount; ++i) {
    register_native(kNativeTable[i].class_fqn,
                    kNativeTable[i].method_name,
                    kNativeTable[i].java_signature,
                    kNativeTable[i].fn);
  }
  std::printf("[jvm] registered %zu stub natives via register_native()\n",
              g_dyn_count);
}

}  // namespace inv
