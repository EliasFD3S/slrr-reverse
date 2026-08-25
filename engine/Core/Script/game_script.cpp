#include "game_script.hpp"
#include "host_objects.hpp"

namespace inv {

// Host script shims retired — Java runs via TREE + VA-backed natives only.

bool game_script_is_lifecycle_method(const char* method) {
  (void)method;
  return false;
}

bool game_script_prefer_host_before_tree(const char* class_fqn,
                                         const char* method) {
  (void)class_fqn;
  (void)method;
  return false;
}

bool game_script_try_host_method(const char* class_fqn, const char* method,
                                 const char* signature,
                                 const std::vector<JvmValue>& args,
                                 bool is_static, JvmValue* out) {
  (void)class_fqn;
  (void)method;
  (void)signature;
  (void)args;
  (void)is_static;
  (void)out;
  return false;
}

// dialog_ensure_osd_buttons — implemented in GameRef.cpp (Dialog chrome).
// options_dialog_* / mainmenu_credits_* — implemented in GameRef.cpp.

void osd_tick_pointer(InvObject* osd) { (void)osd; }

void osd_create_button_calls_reset() {}

int32_t osd_create_button_calls() { return 0; }

}  // namespace inv
