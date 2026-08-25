#pragma once

#include "jvm.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace inv {

// Callbacks from TREE → host (load classes, call methods, allocate).
struct TreeHost {
  virtual ~TreeHost() = default;
  virtual InvObject* new_instance(const char* class_fqn) = 0;
  virtual JvmValue call(const char* class_fqn, const char* method,
                        const char* jni_sig, const std::vector<JvmValue>& args,
                        bool is_static) = 0;
  // Resolve overload by name only (picks first matching argc in class).
  virtual JvmValue call_by_name(const char* class_fqn, const char* method,
                                const std::vector<JvmValue>& args,
                                bool prefer_static) = 0;
};

JvmValue tree_eval(TreeHost* host, const JvmClass& cls, const JvmMethod& method,
                   std::vector<JvmValue> locals, std::string* err);

void tree_field_set_int(InvObject* obj, const char* name, int32_t v);
int32_t tree_field_get_int(InvObject* obj, const char* name);
void tree_field_set_obj(InvObject* obj, const char* name, InvObject* v);
InvObject* tree_field_get_obj(InvObject* obj, const char* name);
void tree_field_set_float(InvObject* obj, const char* name, float v);
float tree_field_get_float(InvObject* obj, const char* name);

// Host java.util.Vector / Object[] (TREE sugar + System.arraycopy).
InvObject* tree_vector_new();
InvObject* tree_array_new(int32_t length);
InvObject* tree_array_new_desc(int32_t length, const char* desc);
bool tree_vector_is(InvObject* vec);
int32_t tree_vector_size(InvObject* vec);
void tree_vector_add(InvObject* vec, InvObject* elem);
void tree_vector_remove(InvObject* vec, InvObject* elem);
InvObject* tree_vector_element_at(InvObject* vec, int32_t idx);
void tree_vector_set(InvObject* vec, int32_t idx, InvObject* elem);
void tree_vector_resize(InvObject* vec, int32_t n);

// Opaque instance tagged with class FQN (field bag).
InvObject* tree_host_new(const char* class_fqn);
const char* tree_host_class(InvObject* obj);

}  // namespace inv
