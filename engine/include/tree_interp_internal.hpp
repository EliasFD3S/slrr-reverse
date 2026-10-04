#pragma once
// Shared TREE state + helpers for tree_fields.cpp / tree_eval.cpp.

#include "tree_interp.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace inv {

struct TreeFieldMap {
  std::unordered_map<std::string, JvmValue> by_name;
};

extern std::unordered_map<InvObject*, TreeFieldMap> g_tree_fields;
extern std::unordered_map<InvObject*, std::vector<InvObject*>> g_tree_vectors;
extern std::unordered_map<InvObject*, std::string> g_tree_host_class;

JvmValue* tree_field_slot(InvObject* obj, const std::string& name, bool create);
std::string tree_strip_class_desc(const std::string& d);
bool tree_truthy(const JvmValue& v);
InvObject* tree_concat_str(InvObject* a, InvObject* b);
InvObject* tree_value_as_string(const JvmValue& v);
float tree_static_qm(const std::string& fname);
int32_t tree_static_vs(const std::string& fname);
int32_t tree_static_rid_carcolor(const std::string& fname);
int32_t tree_resolve_rid_const(const JvmClass& cls, uint32_t imm);

void tree_pack_queue_event(std::vector<JvmValue>& stack,
                           const std::vector<JvmValue>& locals,
                           std::vector<JvmValue>& args, JvmValue* recv_out);
void tree_pack_vector3_binop(std::vector<JvmValue>& stack,
                             const std::vector<JvmValue>& locals,
                             std::vector<JvmValue>& args, JvmValue* recv_out);
void tree_pack_renderref_create(std::vector<JvmValue>& stack,
                                const std::vector<JvmValue>& locals,
                                std::vector<JvmValue>& args, JvmValue* recv_out);
bool tree_is_renderref(InvObject* o);
bool tree_is_vector3(InvObject* o);
bool tree_is_ypr(InvObject* o);
bool tree_is_v3_binop_junk(InvObject* o);

}  // namespace inv
