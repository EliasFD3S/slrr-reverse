#pragma once

#include "natives.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace inv {

enum class JvmTag : uint8_t { Int, Float, Obj, Void };

struct JvmValue {
  JvmTag tag = JvmTag::Void;
  union {
    int32_t i;
    float f;
    InvObject* o;
  } v{};

  static JvmValue make_int(int32_t x) {
    JvmValue r;
    r.tag = JvmTag::Int;
    r.v.i = x;
    return r;
  }
  static JvmValue make_float(float x) {
    JvmValue r;
    r.tag = JvmTag::Float;
    r.v.f = x;
    return r;
  }
  static JvmValue make_obj(InvObject* x) {
    JvmValue r;
    r.tag = JvmTag::Obj;
    r.v.o = x;
    return r;
  }
  static JvmValue make_void() { return {}; }
};

// On-disk SLRR TREE node (3 or 7 bytes) — see TREE_readNode @ 0041a5f0.
struct TreeNode {
  uint8_t op = 0;
  uint16_t slot = 0;
  uint32_t imm = 0;
  bool has_imm = false;
};

struct TreeBody {
  std::vector<TreeNode> nodes;
};

struct JvmMethod {
  std::string name;
  std::string signature;  // JNI-like
  bool is_native = false;
  int tree_index = -1;
  uint32_t flags = 0;
};

// Instance field with a dedicated TREE initializer (FILD: w0==0, tree!=-1).
struct JvmFieldInit {
  std::string name;
  int tree_index = -1;
};

struct JvmClass {
  std::string name;
  std::string super_name;
  std::string file;
  std::vector<JvmMethod> methods;
  std::vector<TreeBody> trees;
  std::vector<JvmFieldInit> field_inits;
  // Const pool (parallel arrays). Strings for Utf8; mref_name[i] set when
  // entry i is an mref resolving to a field/method name.
  std::vector<std::string> const_strings;
  std::vector<std::string> const_mref_name;
  // Parallel int constants (RID / INT pool entries); valid[i]==1 when set.
  std::vector<int32_t> const_ints;
  std::vector<uint8_t> const_int_valid;
  // When entry i is a RID (CONS kind 3), pack path e.g. "cars\\racers\\Baiern.rpk".
  std::vector<std::string> const_rid_pack;
};

class Jvm {
 public:
  // Game install root containing system/Scripts/...
  void set_game_root(const char* root);

  bool load_index(const char* path);
  bool load_class_file(const char* path);
  // Resolve FQN via classpath map and load the stock .class.
  bool load_class(const char* fqn);

  // MainMenu CMD_COMPILEFILES: scan path (or all classpath roots if ".") for
  // *.class and load TUFA. Returns number of .class files loaded OK.
  int32_t compile_all(const char* rel_path);

  const JvmClass* find_class(const char* fqn) const;
  const JvmMethod* find_method(const JvmClass& cls, const char* name,
                               const char* signature) const;

  JvmValue invoke(const char* class_fqn, const char* name, const char* signature,
                  const std::vector<JvmValue>& args, bool is_static);

  size_t class_count() const { return classes_.size(); }
  const char* game_root() const { return game_root_.c_str(); }

 private:
  void upsert_class(JvmClass cls);
  std::string game_root_;
  std::vector<JvmClass> classes_;
};

// Active JVM for natives that need to invoke TREE (GameRef.create → *_VT.<init>).
void jvm_set_active(Jvm* j);
Jvm* jvm_active();

bool resolve_classpath_file(const char* game_root, const char* fqn,
                            std::string* out_path);

}  // namespace inv
