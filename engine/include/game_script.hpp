#pragma once

#include "jvm.hpp"

#include <vector>

namespace inv {

// Former host-script layer (prefer_host / try_host) is retired.
// All APIs below are no-ops; execution is TREE + natives only.

bool game_script_try_host_method(const char* class_fqn, const char* method,
                                 const char* signature,
                                 const std::vector<JvmValue>& args,
                                 bool is_static, JvmValue* out);

bool game_script_is_lifecycle_method(const char* method);

bool game_script_prefer_host_before_tree(const char* class_fqn,
                                         const char* method);

}  // namespace inv
