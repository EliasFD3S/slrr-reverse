#include "tree_interp.hpp"
#include "runtime.hpp"
#include "rpak.hpp"
#include "natives.hpp"
#include "host_objects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace inv {
namespace {

struct FieldMap {
  std::unordered_map<std::string, JvmValue> by_name;
};
std::unordered_map<InvObject*, FieldMap> g_fields;
std::unordered_map<InvObject*, std::vector<InvObject*>> g_vectors;
std::unordered_map<InvObject*, std::string> g_host_class;

JvmValue* field_slot(InvObject* obj, const std::string& name, bool create) {
  if (!obj || name.empty()) return nullptr;
  auto& m = g_fields[obj].by_name;
  auto it = m.find(name);
  if (it == m.end()) {
    if (!create) return nullptr;
    it = m.emplace(name, JvmValue::make_int(0)).first;
  }
  return &it->second;
}

std::string strip_class_desc(const std::string& d) {
  if (d.size() >= 2 && d[0] == 'L' && d.back() == ';')
    return d.substr(1, d.size() - 2);
  return d;
}

bool truthy(const JvmValue& v) {
  if (v.tag == JvmTag::Obj) {
    if (!v.v.o) return false;
    // Opaque host objects (ResourceRef, FindFile, …) use InvString{nullptr}.
    // Real Java Strings always have a non-null utf8 (possibly "").
    const auto* s = reinterpret_cast<const InvString*>(v.v.o);
    if (!s->utf8) return true;
    return s->utf8[0] != '\0';
  }
  if (v.tag == JvmTag::Float) return v.v.f != 0.f;
  if (v.tag == JvmTag::Int) return v.v.i != 0;
  return false;
}

InvObject* concat_str(InvObject* a, InvObject* b) {
  const char* sa = a ? string_cstr(a) : "";
  const char* sb = b ? string_cstr(b) : "";
  std::string out = std::string(sa ? sa : "") + (sb ? sb : "");
  return string_new(out.c_str());
}

// OptionsDialog: `w + " X " + h` — operands may be Int/Float, not String.
InvObject* value_as_string(const JvmValue& v) {
  if (v.tag == JvmTag::Obj) return v.v.o ? v.v.o : string_new("");
  char buf[64];
  if (v.tag == JvmTag::Float)
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v.v.f));
  else
    std::snprintf(buf, sizeof(buf), "%d", v.v.i);
  return string_new(buf);
}

float static_qm(const std::string& fname) {
  // VehicleType static finals used by *_VT ctors (from sources).
  struct Q {
    const char* n;
    float v;
  };
  static const Q k[] = {
      {"qm_stock_Baiern_CoupeSport_2_5", 14.8294f},
      {"qm_full_Baiern_CoupeSport_2_5", 12.6816f},
      {"qm_stock_Baiern_CoupeSport_GT_III", 9.9572f},
      {"qm_full_Baiern_CoupeSport_GT_III", 8.6888f},
      {"qm_stock_Baiern_DevilSport", 12.5978f},
      {"qm_full_Baiern_DevilSport", 10.921f},
  };
  for (const auto& e : k) {
    if (fname == e.n) return e.v;
  }
  return 0.f;
}

int32_t static_vs(const std::string& fname) {
  if (fname == "VS_DEMO") return 0x0001;
  if (fname == "VS_USED") return 0x0002;
  if (fname == "VS_STOCK") return 0x0004;
  if (fname == "VS_DRACE") return 0x0008;
  if (fname == "VS_NRACE") return 0x0010;
  if (fname == "VS_RRACE") return 0x0020;
  return -1;
}

int32_t static_rid_carcolor(const std::string& fname) {
  // GameLogic.RID_CARCOLOR_* — small indices into CARCOLORS[].
  static const struct {
    const char* n;
    int32_t v;
  } k[] = {
      {"RID_CARCOLOR_Baiern_Devils_eye_red", 0},
      {"RID_CARCOLOR_Baiern_Spring_yellow", 1},
      {"RID_CARCOLOR_Einvagen_Zucker", 2},
      {"RID_CARCOLOR_Einvagen_Tornado_rot", 3},
      {"RID_CARCOLOR_Einvagen_Nacht", 4},
      {"RID_CARCOLOR_Einvagen_Smaragd", 5},
      {"RID_CARCOLOR_Einvagen_Black_mage", 6},
      {"RID_CARCOLOR_Einvagen_Hamvas_Grun", 7},
      {"RID_CARCOLOR_Einvagen_Indigo", 8},
      {"RID_CARCOLOR_Einvagen_Jazz", 9},
      {"RID_CARCOLOR_Einvagen_Antracit", 10},
      {"RID_CARCOLOR_Einvagen_Mercator_Blau", 11},
      {"RID_CARCOLOR_Einvagen_Murano", 12},
      {"RID_CARCOLOR_Einvagen_Champagner", 13},
      {"RID_CARCOLOR_Einvagen_Ozean", 14},
      {"RID_CARCOLOR_Einvagen_Reflex", 15},
      {"RID_CARCOLOR_Einvagen_Saratoga", 16},
      {"RID_CARCOLOR_Used_Rusty_Cherry", 17},
      {"RID_CARCOLOR_Used_Rusty_Smaragd", 18},
      {"RID_CARCOLOR_Used_Rusty_Nacht", 19},
      {"RID_CARCOLOR_Used_Rusty_Zucker", 20},
  };
  for (const auto& e : k) {
    if (fname == e.n) return e.v;
  }
  return -1;
}

int32_t resolve_rid_const(const JvmClass& cls, uint32_t imm) {
  if (imm >= cls.const_int_valid.size() || !cls.const_int_valid[imm]) return 0;
  const int32_t local = cls.const_ints[imm];
  auto try_path = [&](std::string path) -> int32_t {
    if (path.empty() || path.find(".rpk") == std::string::npos) return 0;
    for (char& c : path)
      if (c == '/') c = '\\';
    std::string base = path;
    const auto slash = base.find_last_of('\\');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    const RpakPack* pack = rpak_find_by_name(base.c_str());
    if (!pack) {
      java_lang_System_openLib(string_new(path.c_str()));
      pack = rpak_find_by_name(base.c_str());
    }
    if (!pack) return 0;
    return rpak_make_id(pack->pack_id, static_cast<uint16_t>(local & 0xFFFF));
  };
  if (imm < cls.const_rid_pack.size() && !cls.const_rid_pack[imm].empty()) {
    if (int32_t id = try_path(cls.const_rid_pack[imm])) return id;
    // Garage icon RIDs: pack_idx often hits a non-.rpk Utf8; real pack
    // (frontend.rpk) sits a few CONS entries before the RID.
    for (int j = static_cast<int>(imm) - 1;
         j >= 0 && j + 8 >= static_cast<int>(imm); --j) {
      if (static_cast<size_t>(j) < cls.const_strings.size()) {
        if (int32_t id = try_path(cls.const_strings[static_cast<size_t>(j)]))
          return id;
      }
    }
  }
  return local;
}

}  // namespace

void tree_field_set_int(InvObject* obj, const char* name, int32_t v) {
  if (JvmValue* s = field_slot(obj, name, true)) *s = JvmValue::make_int(v);
}

int32_t tree_field_get_int(InvObject* obj, const char* name) {
  if (JvmValue* s = field_slot(obj, name, false)) {
    if (s->tag == JvmTag::Float) return static_cast<int32_t>(s->v.f);
    return s->v.i;
  }
  return 0;
}

void tree_field_set_obj(InvObject* obj, const char* name, InvObject* v) {
  if (JvmValue* s = field_slot(obj, name, true)) *s = JvmValue::make_obj(v);
}

InvObject* tree_field_get_obj(InvObject* obj, const char* name) {
  if (JvmValue* s = field_slot(obj, name, false)) {
    if (s->tag == JvmTag::Obj) return s->v.o;
  }
  return nullptr;
}

void tree_field_set_float(InvObject* obj, const char* name, float v) {
  if (JvmValue* s = field_slot(obj, name, true)) *s = JvmValue::make_float(v);
}

float tree_field_get_float(InvObject* obj, const char* name) {
  if (JvmValue* s = field_slot(obj, name, false)) {
    if (s->tag == JvmTag::Int) return static_cast<float>(s->v.i);
    return s->v.f;
  }
  return 0.f;
}

InvObject* tree_vector_new() {
  InvObject* o = reinterpret_cast<InvObject*>(new InvString{nullptr});
  g_vectors[o] = {};
  g_host_class[o] = "java.util.Vector";
  return o;
}

bool tree_vector_is(InvObject* vec) {
  return vec && g_vectors.find(vec) != g_vectors.end();
}

InvObject* tree_array_new(int32_t length) {
  return tree_array_new_desc(length, "[Ljava.lang.Object;");
}

InvObject* tree_array_new_desc(int32_t length, const char* desc) {
  InvObject* o = reinterpret_cast<InvObject*>(new InvString{nullptr});
  g_host_class[o] = (desc && desc[0]) ? desc : "[Ljava.lang.Object;";
  if (length > 0)
    g_vectors[o].assign(static_cast<size_t>(length), nullptr);
  else
    g_vectors[o] = {};
  return o;
}

int32_t tree_vector_size(InvObject* vec) {
  auto it = g_vectors.find(vec);
  if (it == g_vectors.end()) return 0;
  return static_cast<int32_t>(it->second.size());
}

void tree_vector_add(InvObject* vec, InvObject* elem) {
  if (!vec) return;
  g_vectors[vec].push_back(elem);
}

void tree_vector_remove(InvObject* vec, InvObject* elem) {
  if (!vec || !elem) return;
  auto it = g_vectors.find(vec);
  if (it == g_vectors.end()) return;
  auto& v = it->second;
  v.erase(std::remove(v.begin(), v.end(), elem), v.end());
}

InvObject* tree_vector_element_at(InvObject* vec, int32_t idx) {
  auto it = g_vectors.find(vec);
  if (it == g_vectors.end()) return nullptr;
  if (idx < 0 || static_cast<size_t>(idx) >= it->second.size()) return nullptr;
  return it->second[static_cast<size_t>(idx)];
}

void tree_vector_set(InvObject* vec, int32_t idx, InvObject* elem) {
  if (!vec || idx < 0) return;
  auto& v = g_vectors[vec];
  if (static_cast<size_t>(idx) >= v.size())
    v.resize(static_cast<size_t>(idx) + 1, nullptr);
  v[static_cast<size_t>(idx)] = elem;
}

void tree_vector_resize(InvObject* vec, int32_t n) {
  if (!vec || n < 0) return;
  g_vectors[vec].resize(static_cast<size_t>(n), nullptr);
}

InvObject* tree_host_new(const char* class_fqn) {
  InvObject* o = reinterpret_cast<InvObject*>(new InvString{nullptr});
  if (class_fqn) g_host_class[o] = class_fqn;
  return o;
}

const char* tree_host_class(InvObject* obj) {
  auto it = g_host_class.find(obj);
  if (it == g_host_class.end()) return "";
  return it->second.c_str();
}

// GameRef.queueEvent(ResourceRef,I,String)V @ 0x0047DA30
// Java: queueEvent(null, EVENT_COMMAND=0x10, param). String often on top.
static void pack_queue_event(std::vector<JvmValue>& stack,
                             const std::vector<JvmValue>& locals,
                             std::vector<JvmValue>& args, JvmValue* recv_out) {
  auto is_str = [](const JvmValue& v) -> bool {
    if (v.tag != JvmTag::Obj || !v.v.o) return false;
    const char* c = tree_host_class(v.v.o);
    return string_cstr(v.v.o) && (!c || !c[0] || std::strstr(c, "String"));
  };
  auto pop = [&]() -> JvmValue {
    JvmValue v = stack.back();
    stack.pop_back();
    return v;
  };
  JvmValue param = JvmValue::make_obj(nullptr);
  JvmValue ro = JvmValue::make_obj(nullptr);
  JvmValue recv = JvmValue::make_obj(nullptr);
  int32_t type = 0x10;
  const bool jvm_order = !stack.empty() && is_str(stack.back());
  if (jvm_order) {
    param = pop();
    if (!stack.empty() && (stack.back().tag == JvmTag::Int ||
                           stack.back().tag == JvmTag::Float)) {
      type = stack.back().tag == JvmTag::Int
                 ? pop().v.i
                 : static_cast<int32_t>(pop().v.f);
    }
    if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
      if (!stack.back().v.o)
        ro = pop();
      else if (is_str(stack.back()))
        pop();
      else
        recv = pop();
    }
    if ((recv.tag != JvmTag::Obj || !recv.v.o) && !stack.empty() &&
        stack.back().tag == JvmTag::Obj && stack.back().v.o &&
        !is_str(stack.back()))
      recv = pop();
  } else {
    if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
        stack.back().v.o && !is_str(stack.back()))
      recv = pop();
    if (!stack.empty() && is_str(stack.back())) param = pop();
    if (!stack.empty() && (stack.back().tag == JvmTag::Int ||
                           stack.back().tag == JvmTag::Float)) {
      type = stack.back().tag == JvmTag::Int
                 ? pop().v.i
                 : static_cast<int32_t>(pop().v.f);
    }
    if (!stack.empty() && stack.back().tag == JvmTag::Obj) ro = pop();
  }
  if ((recv.tag != JvmTag::Obj || !recv.v.o) && !locals.empty())
    recv = locals[0];
  args.push_back(recv);
  args.push_back(ro);
  args.push_back(JvmValue::make_int(type));
  args.push_back(param);
  if (recv_out) *recv_out = recv;
}

static bool is_tree_renderref(InvObject* o) {
  if (!o) return false;
  const char* c = tree_host_class(o);
  if (!c || !c[0]) return false;
  if (std::strstr(c, "Camera")) return false;
  return std::strstr(c, "RenderRef") != nullptr;
}

static bool is_tree_vector3(InvObject* o) {
  if (!o) return false;
  const char* c = tree_host_class(o);
  if (c && std::strstr(c, "Vector3")) return true;
  if (c && c[0]) return false;
  if (string_cstr(o)) return false;
  return vec3_is(o);
}

static bool is_tree_ypr(InvObject* o) {
  if (!o) return false;
  const char* c = tree_host_class(o);
  if (c && std::strstr(c, "Ypr")) return true;
  if (c && c[0]) return false;
  return ypr_is(o);
}

static bool is_v3_binop_junk(InvObject* o) {
  if (!o) return false;
  const char* c = tree_host_class(o);
  if (!c || !c[0]) return false;
  return std::strstr(c, "Valocity") || std::strstr(c, "City") ||
         std::strstr(c, "Track") || std::strstr(c, "RaceSetup") ||
         std::strstr(c, "Garage") || std::strstr(c, "ResourceRef") ||
         std::strstr(c, "GameRef") || std::strstr(c, "GroundRef") ||
         std::strstr(c, "RenderRef");
}

// Vector3.add/mul/sub — Java TREE (not native). Recv often a leftover
// ResourceRef from the enclosing method (smoke: ResourceRef.add argc=1).
static void pack_vector3_binop(std::vector<JvmValue>& stack,
                               const std::vector<JvmValue>& locals,
                               std::vector<JvmValue>& args,
                               JvmValue* recv_out) {
  auto pop = [&]() -> JvmValue {
    JvmValue v = stack.back();
    stack.pop_back();
    return v;
  };
  auto under_is_v3_or_num = [&]() -> bool {
    if (stack.size() < 2) return false;
    const JvmValue& u = stack[stack.size() - 2];
    if (u.tag == JvmTag::Float || u.tag == JvmTag::Int) return true;
    return u.tag == JvmTag::Obj && is_tree_vector3(u.v.o);
  };
  while (!stack.empty() && stack.back().tag == JvmTag::Obj &&
         is_v3_binop_junk(stack.back().v.o) && under_is_v3_or_num())
    pop();
  JvmValue recv = JvmValue::make_obj(nullptr);
  JvmValue arg = JvmValue::make_obj(nullptr);
  if (!stack.empty() && (stack.back().tag == JvmTag::Float ||
                         stack.back().tag == JvmTag::Int)) {
    arg = pop();
    while (!stack.empty() && stack.back().tag == JvmTag::Obj &&
           is_v3_binop_junk(stack.back().v.o) && under_is_v3_or_num())
      pop();
  }
  if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
      is_tree_vector3(stack.back().v.o))
    recv = pop();
  if (arg.tag != JvmTag::Float && arg.tag != JvmTag::Int && !stack.empty()) {
    if (stack.back().tag == JvmTag::Float || stack.back().tag == JvmTag::Int)
      arg = pop();
    else if (stack.back().tag == JvmTag::Obj &&
             (is_tree_vector3(stack.back().v.o) || !stack.back().v.o))
      arg = pop();
  }
  if (recv.tag == JvmTag::Obj && recv.v.o && !is_tree_vector3(recv.v.o) &&
      arg.tag == JvmTag::Obj && is_tree_vector3(arg.v.o))
    std::swap(recv, arg);
  args.push_back(recv);
  args.push_back(arg);
  (void)locals;
  if (recv_out) *recv_out = recv;
}

// RenderRef.create(ResourceRef,RenderRef,String)V @ 0x00480EE0
// Java: create(parent, type|rid, alias). String often on top.
static void pack_renderref_create(std::vector<JvmValue>& stack,
                                  const std::vector<JvmValue>& locals,
                                  std::vector<JvmValue>& args,
                                  JvmValue* recv_out) {
  auto is_str = [](const JvmValue& v) -> bool {
    if (v.tag != JvmTag::Obj || !v.v.o) return false;
    const char* c = tree_host_class(v.v.o);
    return string_cstr(v.v.o) && (!c || !c[0] || std::strstr(c, "String"));
  };
  auto pop = [&]() -> JvmValue {
    JvmValue v = stack.back();
    stack.pop_back();
    return v;
  };
  JvmValue alias = JvmValue::make_obj(nullptr);
  JvmValue type = JvmValue::make_obj(nullptr);
  JvmValue parent = JvmValue::make_obj(nullptr);
  JvmValue recv = JvmValue::make_obj(nullptr);
  const bool jvm_order = !stack.empty() && is_str(stack.back());
  if (jvm_order) {
    alias = pop();
    if (!stack.empty()) type = pop();
    if (!stack.empty() && stack.back().tag == JvmTag::Obj) parent = pop();
    if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
        stack.back().v.o && !is_str(stack.back()))
      recv = pop();
  } else {
    if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
        stack.back().v.o && !is_str(stack.back()))
      recv = pop();
    if (!stack.empty() && (is_str(stack.back()) ||
                           (stack.back().tag == JvmTag::Obj && !stack.back().v.o)))
      alias = pop();
    if (!stack.empty()) type = pop();
    if (!stack.empty() && stack.back().tag == JvmTag::Obj) parent = pop();
  }
  if ((recv.tag != JvmTag::Obj || !recv.v.o) && !locals.empty())
    recv = locals[0];
  args.push_back(recv);
  args.push_back(parent);
  args.push_back(type);
  args.push_back(alias);
  if (recv_out) *recv_out = recv;
}

JvmValue tree_eval(TreeHost* host, const JvmClass& cls, const JvmMethod& method,
                   std::vector<JvmValue> locals, std::string* err) {
  if (!host) {
    if (err) *err = "null host";
    return JvmValue::make_void();
  }
  if (method.tree_index < 0 ||
      static_cast<size_t>(method.tree_index) >= cls.trees.size()) {
    if (err) *err = "bad tree_index";
    return JvmValue::make_void();
  }
  // Snapshot: nested load_class during CALL can reallocate Jvm::classes_ and
  // invalidate references into `cls` (SplashScreen.enter → createBG → Osd).
  const TreeBody tree = cls.trees[static_cast<size_t>(method.tree_index)];
  const std::string cls_name = cls.name;
  const std::string super_name = cls.super_name;
  // Garage icons use CMD 100..199; RaceSetup.createOSDObjects uses 0..6
  // (CMD_RACE..CMD_PRIZE). Same ResourceRef+tip addItem packing.
  auto is_icon_osd_cmd = [&](int32_t c) -> bool {
    if (c >= 100 && c < 200) return true;
    if (cls_name.find("RaceSetup") != std::string::npos && c >= 0 && c <= 6)
      return true;
    return false;
  };
  const std::vector<std::string> const_strings = cls.const_strings;
  const std::vector<std::string> const_mref_name = cls.const_mref_name;
  const std::vector<int32_t> const_ints = cls.const_ints;
  const std::vector<uint8_t> const_int_valid = cls.const_int_valid;
  const std::vector<std::string> const_rid_pack = cls.const_rid_pack;

  // Grow locals on demand (TREE may declare slots beyond args).
  auto ensure_local = [&](size_t i) {
    if (locals.size() <= i) locals.resize(i + 1, JvmValue::make_int(0));
  };

  std::vector<JvmValue> stack;
  stack.reserve(32);
  std::optional<uint32_t> pending_binop;
  std::string pending_class;
  int pending_local = -1;
  bool pending_concat = false;
  int pending_concat_args = 0;  // how many string operands waiting (2 or 4)
  InvObject* last_new = nullptr;
  InvObject* recent_new = nullptr;  // survives 0x21 clear for AASTORE+<init>
  InvObject* last_menu = nullptr;   // OptionsDialog: createMenu → setSliderStyle/addItem
  InvObject* last_menu_item = nullptr;  // addItem → printValue/setValue chain
  std::string pending_invoke_name;  // Garage: 0x09 method name before 0x12
  int ff_local = -1;
  int name_local = -1;
  int last_loaded_local = -1;
  int vm_local = -1;  // VehicleModel local slot (reused across NEW in VT ctors)
  std::string pending_putfield;  // field name after NEW+0x21+0x1B hint
  // Phase 2.114: arr[i] = new T(...) / arr[i] = null — 0x20 then 0x08/35.
  InvObject* pending_aastore_arr = nullptr;
  int32_t pending_aastore_idx = 0;
  bool pending_aastore = false;
  // Phase 2.115: 0x22 this()/0x23 super() completed by 0x08/34.
  int pending_special_init = 0;  // 1=this, 2=super
  // getstatic Class.Field + NEWARRAY: 0x11/2 0x19/cls 0x1b/fld 0x07/31 0x06 0x10
  bool pending_static_newarray = false;
  size_t steps = 0;

  auto pop = [&]() -> JvmValue {
    if (stack.empty()) return JvmValue::make_int(0);
    JvmValue v = stack.back();
    stack.pop_back();
    return v;
  };
  auto push = [&](JvmValue v) { stack.push_back(v); };

  auto is_osd_invoke_name = [](const std::string& s) -> bool {
    return s == "createMenu" || s == "addItem" || s == "addSeparator" ||
           s == "createText" || s == "createTextBox" || s == "createButton" ||
           s == "createRectangle" || s == "createHeader" || s == "createBG" ||
           s == "createHotkey" || s == "disable" || s == "enable" ||
           s == "setToolTip" || s == "endGroup" || s == "beginGroup" ||
           s == "hideGroup" || s == "showGroup" || s == "createOSDObjects";
  };
  auto mname_is_garbage = [&](const std::string& s) -> bool {
    if (s.empty()) return true;
    if (is_osd_invoke_name(s)) return false;
    // Coords / tooltips / class descs leaked through empty mref slots.
    if (s[0] == ' ' || s[0] == 'L' || s.find(',') != std::string::npos)
      return true;
    if (s.find(' ') != std::string::npos) return true;
    if (s.find('.') != std::string::npos && s.find('(') == std::string::npos)
      return true;  // java.game.Scene etc.
    return false;
  };
  auto resolve_invoke_name = [&](std::string mname) -> std::string {
    auto stack_has_icon_rr = [&]() -> bool {
      for (size_t i = 0; i < 6 && i < stack.size(); ++i) {
        const JvmValue& v = stack[stack.size() - 1 - i];
        if (v.tag != JvmTag::Obj || !v.v.o) continue;
        const char* hc = tree_host_class(v.v.o);
        if (hc && (std::strstr(hc, "ResourceRef") ||
                   std::strstr(hc, "GameRef")))
          return true;
      }
      return false;
    };
    // Named createMenu with icon-addItem args (Garage TREE) → addItem.
    if (!mname_is_garbage(mname)) {
      if (mname == "createMenu" && last_menu) {
        bool rr = stack_has_icon_rr();
        if (!rr && recent_new) {
          const char* rhc = tree_host_class(recent_new);
          if (rhc && (std::strstr(rhc, "ResourceRef") ||
                      std::strstr(rhc, "GameRef")))
            rr = true;
        }
        bool cmd = false;
        for (size_t i = 0; i < 6 && i < stack.size(); ++i) {
          const JvmValue& v = stack[stack.size() - 1 - i];
          if (v.tag == JvmTag::Int && is_icon_osd_cmd(v.v.i)) cmd = true;
        }
        if (rr || cmd) return "addItem";
      }
      return mname;
    }
    // Osd on top + broken imm → createMenu when floats under Osd.
    // Style often lives only in a local (Garage buttonStyle).
    // Do NOT treat trailing ori int as addItem — that skipped createMenu.
    if (!stack.empty() && stack.back().tag == JvmTag::Obj && stack.back().v.o) {
      const char* thc = tree_host_class(stack.back().v.o);
      if (thc && std::strstr(thc, "Osd")) {
        bool looks_menu_args = false;
        bool looks_additem = false;
        bool has_float = false;
        bool looks_text = false;  // createText: String/Font + floats
        for (size_t i = 1; i < 6 && i < stack.size(); ++i) {
          const JvmValue& v = stack[stack.size() - 1 - i];
          if (v.tag == JvmTag::Obj && v.v.o) {
            const char* hc = tree_host_class(v.v.o);
            if (hc && std::strstr(hc, "Style")) looks_menu_args = true;
            if (hc && (std::strstr(hc, "ResourceRef") ||
                       std::strstr(hc, "GameRef")))
              looks_additem = true;
            if (hc && (std::strstr(hc, "Font") || std::strstr(hc, "Text")))
              looks_text = true;
            if ((!hc || !hc[0] || std::strstr(hc, "String")) &&
                string_cstr(v.v.o))
              looks_text = true;
          } else if (v.tag == JvmTag::Float)
            has_float = true;
        }
        // Garage.createOSDObjects keeps buttonStyle in a local — stack is
        // often just floats[+ori] under Osd (sty=0). Floats + !RR ⇒ createMenu.
        // Do not steal createText (String/Font + floats).
        if (!looks_menu_args) {
          for (const JvmValue& loc : locals) {
            if (loc.tag != JvmTag::Obj || !loc.v.o) continue;
            const char* hc = tree_host_class(loc.v.o);
            if (hc && std::strstr(hc, "Style")) {
              looks_menu_args = true;
              break;
            }
          }
        }
        if ((looks_menu_args || has_float) && has_float && !looks_additem &&
            !looks_text) {
          pending_invoke_name.clear();
          return "createMenu";
        }
        if (looks_additem && last_menu) {
          pending_invoke_name.clear();
          return "addItem";
        }
      }
    }
    // Icon addItem only after createMenu. Skip leftover Style (buttonStyle)
    // left on the stack after createMenu / packing sugar.
    auto stack_looks_additem = [&]() -> bool {
      if (!last_menu || stack.empty()) return false;
      size_t top_i = 0;
      while (top_i < 3 && top_i < stack.size()) {
        const JvmValue& v =
            stack[stack.size() - 1 - top_i];
        if (v.tag == JvmTag::Obj && v.v.o) {
          const char* hc = tree_host_class(v.v.o);
          if (hc && std::strstr(hc, "Style")) {
            ++top_i;
            continue;
          }
        }
        break;
      }
      if (top_i >= stack.size()) return false;
      const JvmValue& top = stack[stack.size() - 1 - top_i];
      if (top.tag == JvmTag::Obj && top.v.o) {
        const char* hc = tree_host_class(top.v.o);
        if (hc && std::strstr(hc, "Osd")) return false;
        if (hc && std::strstr(hc, "Menu")) return false;
        if (hc && (std::strstr(hc, "ResourceRef") || std::strstr(hc, "GameRef")))
          return true;
        if (!hc || !hc[0] || std::strstr(hc, "String")) {
          bool rr = false, cmd = false;
          for (size_t i = top_i + 1;
               i < top_i + 5 && i < stack.size(); ++i) {
            const JvmValue& v = stack[stack.size() - 1 - i];
            if (v.tag == JvmTag::Int) cmd = true;
            if (v.tag == JvmTag::Obj && v.v.o) {
              const char* vh = tree_host_class(v.v.o);
              if (vh && (std::strstr(vh, "ResourceRef") ||
                         std::strstr(vh, "GameRef")))
                rr = true;
            }
          }
          return rr && cmd;
        }
      }
      // RR buried under tip/cmd with Style above.
      for (size_t i = top_i; i < top_i + 5 && i < stack.size(); ++i) {
        const JvmValue& v = stack[stack.size() - 1 - i];
        if (v.tag != JvmTag::Obj || !v.v.o) continue;
        const char* vh = tree_host_class(v.v.o);
        if (vh && (std::strstr(vh, "ResourceRef") ||
                   std::strstr(vh, "GameRef")))
          return true;
      }
      return false;
    };
    if (stack_looks_additem()) {
      pending_invoke_name.clear();
      return "addItem";
    }
    // Garage icon addItem: ResourceRef stays in recent_new after NEW+0x21
    // (not pushed). Tip+CMD are on the stack — without this, mname stays the
    // broken 0x12 pool string (" 3.5") and the invoke is dropped (no 109/110…).
    if (last_menu && recent_new) {
      const char* rhc = tree_host_class(recent_new);
      if (rhc && (std::strstr(rhc, "ResourceRef") ||
                  std::strstr(rhc, "GameRef"))) {
        bool cmd = false;
        for (size_t i = 0; i < 6 && i < stack.size(); ++i) {
          const JvmValue& v = stack[stack.size() - 1 - i];
          if (v.tag == JvmTag::Int && is_icon_osd_cmd(v.v.i)) {
            cmd = true;
            break;
          }
        }
        if (cmd) {
          pending_invoke_name.clear();
          return "addItem";
        }
      }
    }
    if (!pending_invoke_name.empty()) {
      // Stale "createMenu" string before icon addItem (Garage TREE pool).
      if (pending_invoke_name == "createMenu" && last_menu) {
        bool rr = false, cmd = false;
        for (size_t i = 0; i < 6 && i < stack.size(); ++i) {
          const JvmValue& v = stack[stack.size() - 1 - i];
          if (v.tag == JvmTag::Int && is_icon_osd_cmd(v.v.i)) cmd = true;
          if (v.tag != JvmTag::Obj || !v.v.o) continue;
          const char* hc = tree_host_class(v.v.o);
          if (hc && (std::strstr(hc, "ResourceRef") ||
                     std::strstr(hc, "GameRef")))
            rr = true;
        }
        if (rr || cmd) {
          pending_invoke_name.clear();
          return "addItem";
        }
      }
      mname = pending_invoke_name;
      pending_invoke_name.clear();
      return mname;
    }
    // Method name may still sit on the stack (0x09 pushed before packing).
    for (int i = 0; i < 4 && !stack.empty(); ++i) {
      const JvmValue& top = stack.back();
      if (top.tag != JvmTag::Obj || !top.v.o) break;
      const char* hc = tree_host_class(top.v.o);
      if (hc && hc[0]) break;  // not a bare String
      const char* s = string_cstr(top.v.o);
      if (s && is_osd_invoke_name(s)) {
        mname = s;
        pop();
        return mname;
      }
      break;
    }
    return mname;
  };

  // INSTANCEOF after 0x07/32 + TYPE(pending_class). Object from stack or last local.
  auto eval_instanceof = [&]() -> int32_t {
    InvObject* obj = nullptr;
    if (!stack.empty() && stack.back().tag == JvmTag::Obj)
      obj = pop().v.o;
    else if (last_loaded_local >= 0 &&
             static_cast<size_t>(last_loaded_local) < locals.size() &&
             locals[static_cast<size_t>(last_loaded_local)].tag == JvmTag::Obj)
      obj = locals[static_cast<size_t>(last_loaded_local)].v.o;
    if (!obj || pending_class.empty()) return 0;
    const char* hc = tree_host_class(obj);
    if (!hc || !hc[0]) return 0;
    if (std::strstr(hc, pending_class.c_str()) != nullptr) return 1;
    const char* slash = std::strrchr(pending_class.c_str(), '.');
    const char* simple = slash ? slash + 1 : pending_class.c_str();
    return (simple && std::strstr(hc, simple)) ? 1 : 0;
  };

  auto cstr = [&](uint32_t imm) -> std::string {
    if (imm < const_strings.size()) return const_strings[imm];
    return {};
  };
  auto field_name = [&](uint32_t imm) -> std::string {
    if (imm < const_mref_name.size() && !const_mref_name[imm].empty())
      return const_mref_name[imm];
    return cstr(imm);
  };

  auto resolve_rid = [&](uint32_t imm) -> int32_t {
    if (imm >= const_int_valid.size() || !const_int_valid[imm]) return 0;
    const int32_t local = const_ints[imm];
    auto try_path = [&](std::string path) -> int32_t {
      if (path.empty() || path.find(".rpk") == std::string::npos) return 0;
      for (char& c : path)
        if (c == '/') c = '\\';
      std::string base = path;
      const auto slash = base.find_last_of('\\');
      if (slash != std::string::npos) base = base.substr(slash + 1);
      const RpakPack* pack = rpak_find_by_name(base.c_str());
      if (!pack) {
        java_lang_System_openLib(string_new(path.c_str()));
        pack = rpak_find_by_name(base.c_str());
      }
      if (!pack) return 0;
      return rpak_make_id(pack->pack_id, static_cast<uint16_t>(local & 0xFFFF));
    };
    if (imm < const_rid_pack.size() && !const_rid_pack[imm].empty()) {
      if (int32_t id = try_path(const_rid_pack[imm])) return id;
      // Garage icons: pack_idx → non-.rpk; frontend.rpk is nearby in CONS.
      for (int j = static_cast<int>(imm) - 1;
           j >= 0 && j + 8 >= static_cast<int>(imm); --j) {
        if (static_cast<size_t>(j) < const_strings.size()) {
          if (int32_t id = try_path(const_strings[static_cast<size_t>(j)]))
            return id;
        }
      }
    }
    return local;
  };


  // hideGroup(optionsGroup = endGroup()): putfield name often sits in a nearby
  // 0x1b hint — write the closed group id onto this.*Group when present.
  auto capture_endgroup_field = [&](int32_t gid, size_t at_ip) {
    if (gid < 0 || locals.empty() || locals[0].tag != JvmTag::Obj ||
        !locals[0].v.o)
      return;
    for (size_t j = at_ip + 1; j < tree.nodes.size() && j < at_ip + 10; ++j) {
      const TreeNode& tn = tree.nodes[j];
      if (tn.op != 0x1b || !tn.has_imm) continue;
      std::string fname = field_name(tn.imm);
      if (fname.size() < 5 || fname.find("Group") == std::string::npos)
        continue;
      tree_field_set_int(locals[0].v.o, fname.c_str(), gid);
      return;
    }
  };

  for (size_t ip = 0; ip < tree.nodes.size(); ++ip) {
    if (++steps > 2000000) {
      if (err) {
        *err = "TREE step limit at ip=" + std::to_string(ip) + "/" +
               std::to_string(tree.nodes.size());
      }
      return JvmValue::make_void();
    }
    const TreeNode& n = tree.nodes[ip];
    switch (n.op) {
      case 0x01: {  // LOCAL_LOAD
        if (!n.has_imm) break;
        ensure_local(n.imm);
        last_loaded_local = static_cast<int>(n.imm);
        push(locals[n.imm]);
        break;
      }
      case 0x02:  // declare / bind local index for upcoming NEW
        if (n.has_imm) {
          pending_local = static_cast<int>(n.imm);
          vm_local = pending_local;
        }
        break;
      case 0x03:
        break;
      case 0x04: {
        // AND short-circuit: cmp; 0x04; 0x14 +N — if falsy, keep 0 and goto.
        // OptionsDialog.show video filter: w>=0 && w<=2048 && …
        // Garage: pending ICMP_EQ (gameMode==SINGLECAR) must not survive into
        // the !majomParade IFEQ — that compared gm==1 and skipped the rest of
        // the street menu for FREERIDE/QUICKRACE/DEMO (ai=2).
        if (ip + 1 < tree.nodes.size() && tree.nodes[ip + 1].op == 0x14 &&
            tree.nodes[ip + 1].has_imm &&
            (tree.nodes[ip + 1].imm & 0x80000000u) == 0 && !stack.empty()) {
          const int32_t rel = static_cast<int32_t>(tree.nodes[ip + 1].imm);
          if (!truthy(stack.back())) {
            if (pending_binop && *pending_binop == 7) pending_binop.reset();
            ip = ip + 1 + static_cast<size_t>(rel) - 1;
          } else {
            if (pending_binop && *pending_binop == 7) pending_binop.reset();
            pop();
            ++ip;  // skip forward GOTO; evaluate next conjunct
          }
          break;
        }
        break;
      }
      case 0x05: {
        // OR short-circuit: cmp; 0x05; 0x14 +N — if truthy, keep 1 and goto.
        // OptionsDialog keyText: if (i < 10 || i > 14). Not INSTANCEOF.
        if ((!pending_binop || *pending_binop != 32) &&
            ip + 1 < tree.nodes.size() && tree.nodes[ip + 1].op == 0x14 &&
            tree.nodes[ip + 1].has_imm &&
            (tree.nodes[ip + 1].imm & 0x80000000u) == 0 && !stack.empty() &&
            stack.back().tag == JvmTag::Int) {
          const int32_t rel = static_cast<int32_t>(tree.nodes[ip + 1].imm);
          if (truthy(stack.back())) {
            ip = ip + 1 + static_cast<size_t>(rel) - 1;
          } else {
            pop();
            ++ip;
          }
          break;
        }
        // INSTANCEOF: after LOCAL_LOAD + 0x07/32 + TYPE(pending_class).
        int32_t ok = eval_instanceof();
        // OR-chain accumulator (instanceof A || B || ...).
        if (!stack.empty() && stack.back().tag == JvmTag::Int) {
          if (pop().v.i) ok = 1;
        }
        push(JvmValue::make_int(ok));
        if (pending_binop) pending_binop.reset();
        break;
      }
      case 0x06: {  // type / class const
        if (!n.has_imm) break;
        std::string t = cstr(n.imm);
        if (t == "I" || t == "Z" || t == "B" || t == "C" || t == "S") {
          if (pending_local >= 0) {
            ensure_local(static_cast<size_t>(pending_local));
            locals[static_cast<size_t>(pending_local)] = JvmValue::make_int(0);
          }
        } else if (t == "F" || t == "D") {
          if (pending_local >= 0) {
            ensure_local(static_cast<size_t>(pending_local));
            locals[static_cast<size_t>(pending_local)] = JvmValue::make_float(0.f);
          }
        } else if (!t.empty() && t[0] == '[') {
          // ANEWARRAY / NEWARRAY: size on stack, then 0x07/31 + type.
          pending_class = t;
          if (pending_binop && *pending_binop == 31) {
            int32_t len = 0;
            if (!stack.empty()) {
              JvmValue top = pop();
              if (top.tag == JvmTag::Int) len = top.v.i;
              else if (top.tag == JvmTag::Float)
                len = static_cast<int32_t>(top.v.f);
            }
            if (len < 0) len = 0;
            if (len > 1 << 20) len = 1 << 20;
            push(JvmValue::make_obj(tree_array_new_desc(len, t.c_str())));
            pending_binop.reset();
          }
        } else if (!t.empty() && (t[0] == 'L' || t.find('.') != std::string::npos)) {
          pending_class = strip_class_desc(t);
          if (pending_class == "java.render.osd.Rectangle")
            pending_class = "java.render.Rectangle";
          if (pending_local >= 0 && pending_class == "java.lang.String" &&
              ff_local >= 0 && name_local < 0) {
            name_local = pending_local;
          }
        }
        break;
      }
      case 0x07: {
        if (!n.has_imm) break;
        auto apply2 = [&](uint32_t op) -> bool {
          if (stack.size() < 2) return false;
          JvmValue b = pop();
          JvmValue a = pop();
          switch (op) {
            case 15: {  // ISUB
              if (a.tag == JvmTag::Float || b.tag == JvmTag::Float) {
                float af =
                    a.tag == JvmTag::Float ? a.v.f : static_cast<float>(a.v.i);
                float bf =
                    b.tag == JvmTag::Float ? b.v.f : static_cast<float>(b.v.i);
                push(JvmValue::make_float(af - bf));
              } else {
                push(JvmValue::make_int(a.v.i - b.v.i));
              }
              return true;
            }
            case 8: {  // ICMP_GE (i >= k) — for-loop i>=0
              push(JvmValue::make_int(a.v.i >= b.v.i ? 1 : 0));
              return true;
            }
            case 9: {  // ICMP_LE — OptionsDialog: for (i=10; i<=14; i++)
              push(JvmValue::make_int(a.v.i <= b.v.i ? 1 : 0));
              return true;
            }
            case 3: {  // IOR (VS_RRACE | VS_NRACE before VehicleModel NEW)
              push(JvmValue::make_int(a.v.i | b.v.i));
              return true;
            }
            case 5: {  // IAND
              push(JvmValue::make_int(a.v.i & b.v.i));
              return true;
            }
            case 10: {  // ICMP_GT
              push(JvmValue::make_int(a.v.i > b.v.i ? 1 : 0));
              return true;
            }
            case 11: {  // ICMP_LT
              push(JvmValue::make_int(a.v.i < b.v.i ? 1 : 0));
              return true;
            }
            default:
              push(a);
              push(b);
              return false;
          }
        };
        if (n.imm == 7) {
          pending_binop = 7;
        } else if (n.imm == 32) {
          pending_binop = 32;  // INSTANCEOF (resolved by OP_05 / IFEQ / NOT)
        } else if (n.imm == 21) {
          // NOT — Valocity.exit: !(next instanceof RaceSetup)
          int32_t v = 0;
          if (pending_binop && *pending_binop == 32 && !pending_class.empty()) {
            v = eval_instanceof();
            pending_binop.reset();
          } else if (!stack.empty()) {
            v = truthy(pop()) ? 1 : 0;
          }
          push(JvmValue::make_int(v ? 0 : 1));
        } else if (n.imm == 1) {
          // OR — `instanceof A || instanceof B` after TYPE without OP_05.
          if (pending_binop && *pending_binop == 32 && !pending_class.empty()) {
            int32_t ok = eval_instanceof();
            pending_binop.reset();
            if (!stack.empty() && stack.back().tag == JvmTag::Int && pop().v.i)
              ok = 1;
            push(JvmValue::make_int(ok));
          } else {
            pending_binop = 1;
          }
        } else if (n.imm == 16) {
          pending_concat = true;
          pending_concat_args += 2;
        } else if (n.imm == 27 || n.imm == 28 || n.imm == 30) {
          // structural markers (thunk / cleanup / new-object hint)
          if (n.imm == 30 && pending_local < 0 && vm_local >= 0) {
            // Re-NEW into the same local (Baiern_VT: many VehicleModel in one slot).
            pending_local = vm_local;
          }
        } else if (n.imm == 22) {
          // FNEG — Valocity: posGarage coords (prefix before const or postfix).
          if (!stack.empty() && (stack.back().tag == JvmTag::Float ||
                                 stack.back().tag == JvmTag::Int)) {
            JvmValue v = pop();
            float f =
                v.tag == JvmTag::Float ? v.v.f : static_cast<float>(v.v.i);
            push(JvmValue::make_float(-f));
          } else {
            pending_binop = 22;
          }
        } else if (n.imm == 31) {
          // NEWARRAY / ANEWARRAY — size already on stack; type follows via 0x06.
          pending_binop = 31;
        } else if (n.imm == 2) {
          // Logical-AND marker between 0x04 short-circuit conjuncts
          // (OptionsDialog video mode filter). Combine is branchy, not stacky.
        } else if (apply2(n.imm)) {
          // binary op applied
        } else {
          pending_binop = n.imm;
        }
        break;
      }
      case 0x08: {
        if (n.has_imm && n.imm == 34) {
          // Completes this()/super() started by 0x22 / 0x23.
          if (pending_special_init && !locals.empty() &&
              locals[0].tag == JvmTag::Obj && locals[0].v.o) {
            InvObject* self = locals[0].v.o;
            // Drop stray NEW result left for sugar.
            if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                stack.back().v.o != self) {
              const char* shc = tree_host_class(stack.back().v.o);
              if (!shc || !shc[0] || std::strcmp(shc, "java.lang.Object") == 0)
                pop();
            }
            const bool mixed_init =
                cls_name.find("Trigger") != std::string::npos ||
                pending_class.find("Trigger") != std::string::npos ||
                cls_name.find("GameRef") != std::string::npos ||
                cls_name.find("RenderRef") != std::string::npos;
            // this() leaves `this` on top; do not stop mixed packing at self.
            if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                stack.back().v.o == self)
              pop();
            std::vector<JvmValue> params;
            while (params.size() < 8 && !stack.empty()) {
              if (stack.back().tag == JvmTag::Obj) {
                if (!mixed_init) break;
                InvObject* o = stack.back().v.o;
                if (!o) {
                  params.push_back(pop());
                  continue;
                }
                const char* hc = tree_host_class(o);
                if (!hc || !hc[0] || std::strstr(hc, "GameRef") ||
                    std::strstr(hc, "ResourceRef") ||
                    std::strstr(hc, "Vector3") || std::strstr(hc, "String") ||
                    std::strstr(hc, "Ypr") || std::strstr(hc, "GroundRef")) {
                  params.push_back(pop());
                  continue;
                }
                break;
              }
              params.push_back(pop());
            }
            std::vector<JvmValue> args;
            args.push_back(JvmValue::make_obj(self));
            for (auto it = params.rbegin(); it != params.rend(); ++it)
              args.push_back(*it);
            const char* target = cls_name.c_str();
            if (pending_special_init == 2 && !super_name.empty())
              target = super_name.c_str();
            host->call_by_name(target, "<init>", args, false);
            pending_special_init = 0;
          }
          break;
        }
        if (n.has_imm && n.imm == 35) {
          // Local store sugar: `x = -0.4` → push val; load x; 0x08/35.
          // Must run before <init>/AASTORE — both also use imm 35.
          // Object assigns (`m = createMenu()`) are NOT handled here: both
          // stack slots are Obj and would collide with INVOKESPECIAL <init>.
          if (!pending_aastore && last_loaded_local >= 0 && stack.size() >= 2 &&
              stack.back().tag != JvmTag::Obj &&
              stack[stack.size() - 2].tag != JvmTag::Obj) {
            pop();  // discard current local value
            JvmValue val = pop();
            ensure_local(static_cast<size_t>(last_loaded_local));
            locals[static_cast<size_t>(last_loaded_local)] = val;
            break;
          }
          // PUTFIELD is consumed via 0x1b lookahead. Leftover 0x08/35 is
          // INVOKESPECIAL <init>: stack = [..., arg0, arg1, recv].
          // Or AASTORE commit after 0x20 (value = recent_new / stack).
          if (pending_aastore) {
            InvObject* val = recent_new;
            // `arr[i] = null` leaves aconst_null on the stack; prefer it over a
            // stale recent_new from an earlier NEW in the same TREE.
            if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                stack.back().v.o != pending_aastore_arr) {
              if (!val || stack.back().v.o == nullptr) {
                val = pop().v.o;
              }
            }
            if (val && val != pending_aastore_arr) {
              // Finish <init> when this is `arr[i] = new T(args)`.
              // ParkingCar(map, type, pos, ori, seed) is mixed Obj+prim —
              // primitive-only pops left argc=3 (leftover floats).
              const char* vhc = tree_host_class(val);
              const bool pcar =
                  (vhc && std::strstr(vhc, "ParkingCar")) ||
                  (!pending_class.empty() &&
                   pending_class.find("ParkingCar") != std::string::npos);
              const bool aastore_v3 =
                  (vhc && std::strstr(vhc, "Vector3")) ||
                  (!pending_class.empty() &&
                   pending_class.find("Vector3") != std::string::npos);
              std::vector<JvmValue> params;
              if (pcar) {
                while (params.size() < 5 && !stack.empty()) {
                  if (stack.back().tag == JvmTag::Obj) {
                    InvObject* o = stack.back().v.o;
                    if (!o) {
                      params.push_back(pop());
                      continue;
                    }
                    const char* hc = tree_host_class(o);
                    if (!hc || !hc[0] || std::strstr(hc, "ResourceRef") ||
                        std::strstr(hc, "GameRef") ||
                        std::strstr(hc, "Vector3") ||
                        std::strstr(hc, "String") || std::strstr(hc, "Ypr") ||
                        std::strstr(hc, "GroundRef")) {
                      params.push_back(pop());
                      continue;
                    }
                    break;
                  }
                  params.push_back(pop());
                }
              } else {
                const size_t prim_cap = aastore_v3 ? 3u : 8u;
                while (params.size() < prim_cap && !stack.empty() &&
                       stack.back().tag != JvmTag::Obj) {
                  params.push_back(pop());
                }
              }
              if (!params.empty() || pcar) {
                std::vector<JvmValue> args;
                args.push_back(JvmValue::make_obj(val));
                for (auto it = params.rbegin(); it != params.rend(); ++it)
                  args.push_back(*it);
                const char* cn = tree_host_class(val);
                if (!cn || !cn[0]) cn = pending_class.c_str();
                if (cn && cn[0])
                  host->call_by_name(cn, "<init>", args, false);
              }
            }
            if (pending_aastore_arr)
              tree_vector_set(pending_aastore_arr, pending_aastore_idx, val);
            pending_aastore = false;
            pending_aastore_arr = nullptr;
            recent_new = nullptr;
            break;
          }
          if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
            JvmValue recv = pop();
            // Cap at 2 for general <init> (VT leftover prevalence+= on stack).
            // Vector3/Ypr (FFF) need 3 — detect via pending_class / host class.
            // Style(FF,charset,align,bg): charset/bg are Obj — must not stop at
            // first object or align stays default CENTER and Menu columns overlap.
            const char* peek_cn = tree_host_class(recv.v.o);
            if (!peek_cn || !peek_cn[0]) {
              peek_cn = pending_class.empty() ? "" : pending_class.c_str();
            }
            const bool style_init =
                (peek_cn && std::strstr(peek_cn, "Style") != nullptr) ||
                (!pending_class.empty() &&
                 pending_class.find("Style") != std::string::npos);
            const bool vehicle_model_init =
                (peek_cn && std::strstr(peek_cn, "VehicleModel") != nullptr) ||
                (!pending_class.empty() &&
                 pending_class.find("VehicleModel") != std::string::npos);
            const bool trigger_init =
                (peek_cn && std::strstr(peek_cn, "Trigger") != nullptr) ||
                (!pending_class.empty() &&
                 pending_class.find("Trigger") != std::string::npos);
            const bool parking_car_init =
                (peek_cn && std::strstr(peek_cn, "ParkingCar") != nullptr) ||
                (!pending_class.empty() &&
                 pending_class.find("ParkingCar") != std::string::npos);
            const bool renderref_init =
                ((peek_cn && std::strstr(peek_cn, "RenderRef") &&
                  !std::strstr(peek_cn, "Camera")) ||
                 (!pending_class.empty() &&
                  pending_class.find("RenderRef") != std::string::npos &&
                  pending_class.find("Camera") == std::string::npos));
            const bool gameref_init =
                (peek_cn && std::strstr(peek_cn, "GameRef")) ||
                (!pending_class.empty() &&
                 pending_class.find("GameRef") != std::string::npos);
            const bool vector3_init =
                (peek_cn && std::strstr(peek_cn, "Vector3")) ||
                (!pending_class.empty() &&
                 pending_class.find("Vector3") != std::string::npos);
            const bool ypr_init =
                !vector3_init &&
                ((peek_cn && std::strstr(peek_cn, "Ypr")) ||
                 (!pending_class.empty() &&
                  pending_class.find("Ypr") != std::string::npos));
            const bool rectangle_init =
                (peek_cn && std::strstr(peek_cn, "Rectangle")) ||
                (!pending_class.empty() &&
                 pending_class.find("Rectangle") != std::string::npos);
            const bool menu_init =
                ((peek_cn && std::strstr(peek_cn, "Menu") &&
                  !std::strstr(peek_cn, "MainMenu")) ||
                 (!pending_class.empty() &&
                  pending_class.find("Menu") != std::string::npos &&
                  pending_class.find("MainMenu") == std::string::npos));
            const size_t max_args =
                style_init ? 6u
                : menu_init ? 6u
                : trigger_init ? 5u
                : parking_car_init ? 5u
                : renderref_init ? 3u
                : gameref_init ? 4u
                : vehicle_model_init ? 2u
                : rectangle_init ? 3u
                : (vector3_init || ypr_init) ? 3u
                      : 2u;
            std::vector<JvmValue> params;
            if (vector3_init || ypr_init) {
              // Java Vector3(FFF) / copy / native Vector3(Ypr) @ 0x00482020.
              if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                  stack.back().v.o &&
                  (is_tree_vector3(stack.back().v.o) ||
                   is_tree_ypr(stack.back().v.o))) {
                params.push_back(pop());
              } else {
                while (params.size() < 3 && !stack.empty() &&
                       (stack.back().tag == JvmTag::Float ||
                        stack.back().tag == JvmTag::Int))
                  params.push_back(pop());
              }
            } else {
            while (params.size() < max_args && !stack.empty()) {
              if (!style_init && !trigger_init && !gameref_init &&
                  !renderref_init && !parking_car_init && !rectangle_init &&
                  !menu_init) {
                if (stack.back().tag == JvmTag::Obj) break;
                params.push_back(pop());
                continue;
              }
              // Style / Trigger / ParkingCar / GameRef / Rectangle / Menu:
              // mixed Obj+prim.
              if (stack.back().tag == JvmTag::Obj) {
                InvObject* o = stack.back().v.o;
                if (!o) {
                  params.push_back(pop());
                  continue;
                }
                const char* hc = tree_host_class(o);
                // vec3_new / gameref_new leave empty host-class.
                if (!hc || !hc[0] || std::strstr(hc, "ResourceRef") ||
                    std::strstr(hc, "GameRef") || std::strstr(hc, "Vector3") ||
                    std::strstr(hc, "String") || std::strstr(hc, "Ypr") ||
                    std::strstr(hc, "GroundRef") ||
                    (rectangle_init && (std::strstr(hc, "Template") ||
                                        std::strstr(hc, "Group") ||
                                        std::strstr(hc, "Osd"))) ||
                    (menu_init && (std::strstr(hc, "Osd") ||
                                   std::strstr(hc, "Style")))) {
                  params.push_back(pop());
                  continue;
                }
                break;
              }
              params.push_back(pop());
            }
            }
            std::vector<JvmValue> args;
            args.push_back(recv);
            for (auto it = params.rbegin(); it != params.rend(); ++it)
              args.push_back(*it);
            const char* cn = tree_host_class(recv.v.o);
            if (!cn || !cn[0]) {
              cn = pending_class.empty() ? cls_name.c_str()
                                         : pending_class.c_str();
            }
            host->call_by_name(cn, "<init>", args, false);
            if (!pending_putfield.empty() && recv.tag == JvmTag::Obj &&
                !locals.empty() && locals[0].tag == JvmTag::Obj) {
              if (JvmValue* slot =
                      field_slot(locals[0].v.o, pending_putfield, true)) {
                *slot = recv;
              }
              pending_putfield.clear();
            }
          }
        } else if (n.has_imm && n.imm == 39) {
          // FADD statement: y += 0.12 (OptionsDialog keyText) OR prevalence +=.
          JvmValue b = pop();
          JvmValue a = pop();
          float af =
              a.tag == JvmTag::Float ? a.v.f : static_cast<float>(a.v.i);
          float bf =
              b.tag == JvmTag::Float ? b.v.f : static_cast<float>(b.v.i);
          JvmValue sum = JvmValue::make_float(af + bf);
          if (last_loaded_local >= 0) {
            ensure_local(static_cast<size_t>(last_loaded_local));
            JvmValue& loc = locals[static_cast<size_t>(last_loaded_local)];
            if (loc.tag == JvmTag::Float || loc.tag == JvmTag::Int) {
              loc = sum;
              break;
            }
          }
          if (!locals.empty() && locals[0].tag == JvmTag::Obj) {
            if (JvmValue* slot =
                    field_slot(locals[0].v.o, "prevalence", true)) {
              *slot = sum;
            }
          }
        } else if (n.has_imm && n.imm == 46) {
          // Statement: vehicleSetMask |= … on this (VehicleType.init).
          JvmValue b = pop();
          JvmValue a = pop();
          JvmValue r = JvmValue::make_int(a.v.i | b.v.i);
          if (!locals.empty() && locals[0].tag == JvmTag::Obj) {
            if (JvmValue* slot =
                    field_slot(locals[0].v.o, "vehicleSetMask", true)) {
              *slot = r;
            }
          }
        } else if (n.has_imm && n.imm == 23) {
          // IINC last_loaded (for-loop i++ — OptionsDialog video/keyText).
          const int li = last_loaded_local >= 0 ? last_loaded_local : 1;
          ensure_local(static_cast<size_t>(li));
          JvmValue& loc = locals[static_cast<size_t>(li)];
          if (!stack.empty() && stack.back().tag == JvmTag::Int) {
            JvmValue v = pop();
            v.v.i += 1;
            loc = v;
          } else if (loc.tag == JvmTag::Int) {
            loc.v.i += 1;
          } else if (loc.tag == JvmTag::Float) {
            loc.v.f += 1.f;
          }
        } else if (n.has_imm && n.imm == 24) {
          // IDEC last_loaded / local 1 (for-loop i--).
          const int li = last_loaded_local >= 0 ? last_loaded_local : 1;
          ensure_local(static_cast<size_t>(li));
          if (locals[static_cast<size_t>(li)].tag == JvmTag::Int)
            locals[static_cast<size_t>(li)].v.i -= 1;
          else if (locals[static_cast<size_t>(li)].tag == JvmTag::Float)
            locals[static_cast<size_t>(li)].v.f -= 1.f;
        } else if (n.has_imm && n.imm == 27) {
          // expression terminator — ignore
        } else if (last_loaded_local >= 0 && !stack.empty() &&
                   stack.back().tag == JvmTag::Int) {
          // Empirically: CAST local; STRING <n> → IINC local (rpksfound++).
          JvmValue v = pop();
          v.v.i += 1;
          ensure_local(static_cast<size_t>(last_loaded_local));
          locals[static_cast<size_t>(last_loaded_local)] = v;
        }
        break;
      }
      case 0x09: {  // STRING literal (prefer Utf8; ints use 0x0B / RID use 0x0E)
        if (!n.has_imm) break;
        // Garage.createOSDObjects: TREE imms for icon tip/RID are biased -6 vs
        // CONS (stock pairs start at frontend.rpk+RID; early imms hit camera
        // strings / createMenu). Shift only that band.
        uint32_t cimm = n.imm;
        if (cls_name.find("Garage") != std::string::npos && cimm >= 950 &&
            cimm < 1020)
          cimm += 6;
        // Garage icon TREE: tip slot sometimes holds a RID const (kind 3).
        if (cimm < const_rid_pack.size() && !const_rid_pack[cimm].empty()) {
          push(JvmValue::make_int(resolve_rid(cimm)));
          break;
        }
        std::string s = cstr(cimm);
        if (!s.empty()) {
          // Garage createOSDObjects: method name is a STRING before NEW+ARRAY+0x12.
          if (is_osd_invoke_name(s)) {
            bool invoke_soon = false;
            for (size_t j = ip + 1; j < tree.nodes.size() && j < ip + 10; ++j) {
              const uint8_t op = tree.nodes[j].op;
              if (op == 0x12 || op == 0x1a || op == 0x10) {
                invoke_soon = true;
                break;
              }
              if (op == 0x27 || op == 0x24 || op == 0x11 || op == 0x01 ||
                  op == 0x07 || op == 0x08)
                continue;
              break;
            }
            if (invoke_soon) {
              pending_invoke_name = s;
              break;
            }
          }
          push(JvmValue::make_obj(string_new(s.c_str())));
        } else if (cimm < const_int_valid.size() &&
                   const_int_valid[cimm]) {
          push(JvmValue::make_int(const_ints[cimm]));
        } else {
          push(JvmValue::make_obj(string_new("")));
        }
        break;
      }
      case 0x0a: {  // FLOAT const (IEEE bits in imm)
        if (!n.has_imm) break;
        float f = 0.f;
        std::memcpy(&f, &n.imm, sizeof(f));
        if (pending_binop && *pending_binop == 22) {
          f = -f;
          pending_binop.reset();
        }
        push(JvmValue::make_float(f));
        break;
      }
      case 0x0b: {  // INT immediate (imm is the value; locals use 0x01)
        if (!n.has_imm) break;
        push(JvmValue::make_int(static_cast<int32_t>(n.imm)));
        break;
      }
      case 0x0c:  // ACONST_NULL (MainMenu.exit: mmd = null)
        push(JvmValue::make_obj(nullptr));
        break;
      case 0x0e: {  // RID (kind 3) — or Utf8 tip (Garage icon TREE)
        if (!n.has_imm) break;
        uint32_t cimm = n.imm;
        if (cls_name.find("Garage") != std::string::npos && cimm >= 950 &&
            cimm < 1020)
          cimm += 6;
        if (cimm < const_rid_pack.size() && !const_rid_pack[cimm].empty()) {
          push(JvmValue::make_int(resolve_rid(cimm)));
        } else {
          std::string s = cstr(cimm);
          // Real tooltips are prose; coord fragments (" 0,0,0") and pack
          // names must not pollute the stack (they broke createMenu).
          const bool tip_like =
              !s.empty() && s[0] != ' ' && s.find(',') == std::string::npos &&
              s.find(".rpk") == std::string::npos && s.size() > 3;
          if (tip_like)
            push(JvmValue::make_obj(string_new(s.c_str())));
          else
            push(JvmValue::make_int(
                (cimm < const_int_valid.size() && const_int_valid[cimm])
                    ? resolve_rid(cimm)
                    : 0));
        }
        break;
      }
      case 0x10:
      case 0x11:
      case 0x12: {  // INVOKE / INVOKESPECIAL / INVOKESTATIC
        auto do_concat_n = [&](int pairs) {
          // pairs of (left,right) on stack; evaluate top pair first then deeper.
          std::vector<JvmValue> out;
          out.reserve(static_cast<size_t>(pairs));
          for (int i = 0; i < pairs; ++i) {
            JvmValue right = pop();
            JvmValue left = pop();
            out.push_back(JvmValue::make_obj(
                concat_str(value_as_string(left), value_as_string(right))));
          }
          // out is top-first; push deeper first so top matches Java arg order.
          for (auto it = out.rbegin(); it != out.rend(); ++it) push(*it);
          pending_concat = false;
          pending_concat_args = 0;
          last_new = nullptr;
        };

        if (n.op == 0x11) {
          if (pending_concat) {
            const int pairs = std::max(1, pending_concat_args / 2);
            do_concat_n(pairs);
            break;
          }
          // City FILD: getstatic GameLogic.CLUBS + anewarray Vector3/Ypr.
          if (ip + 5 < tree.nodes.size() && n.has_imm &&
              tree.nodes[ip + 1].op == 0x19 && tree.nodes[ip + 2].op == 0x1b &&
              tree.nodes[ip + 3].op == 0x07 && tree.nodes[ip + 3].has_imm &&
              tree.nodes[ip + 3].imm == 31 && tree.nodes[ip + 4].op == 0x06 &&
              tree.nodes[ip + 5].op == 0x10) {
            pending_static_newarray = true;
            break;
          }
          const bool after_array =
              ip > 0 && tree.nodes[ip - 1].op == 0x24;  // ARRAY_INIT
          if (after_array) {
            last_new = nullptr;
            // ResourceRef call sugar: NEW+ARRAY+0x11 … 0x1A method-ref.
            bool method_ref = false;
            for (size_t j = ip + 1; j < tree.nodes.size() && j < ip + 8; ++j) {
              const TreeNode& peek = tree.nodes[j];
              if (peek.op == 0x1a && peek.has_imm) {
                const bool named =
                    peek.imm < const_mref_name.size() &&
                    (const_mref_name[peek.imm] == "addElement" ||
                     const_mref_name[peek.imm] == "getFirstChild" ||
                     const_mref_name[peek.imm] == "getNextChild" ||
                     const_mref_name[peek.imm] == "cache" ||
                     const_mref_name[peek.imm] == "size" ||
                     const_mref_name[peek.imm] == "elementAt" ||
                     const_mref_name[peek.imm] == "addColorIndex");
                if (named || peek.imm >= tree.nodes.size()) {
                  method_ref = true;
                  break;
                }
              }
              if (peek.op == 0x10 || peek.op == 0x12) break;
              if (peek.op == 0x19 || peek.op == 0x2b) break;
            }
            if (method_ref) {
              // Drop NEW packing object only; keep invoke arg under it (vmd).
              if (ip >= 2 && tree.nodes[ip - 2].op == 0x27 && !stack.empty() &&
                  stack.back().tag == JvmTag::Obj) {
                pop();
              }
              break;
            }
            // Also packing before 0x1A/0x12 Splash/Osd/GameType/GameLogic calls.
            bool call_mref = false;
            for (size_t j = ip + 1; j < tree.nodes.size() && j < ip + 8; ++j) {
              const TreeNode& peek = tree.nodes[j];
              if ((peek.op == 0x1a || peek.op == 0x12) && peek.has_imm &&
                  peek.imm < const_mref_name.size()) {
                const std::string& mn = const_mref_name[peek.imm];
                if (mn == "createBG" || mn == "createHotkey" ||
                    mn == "createButton" || mn == "createText" ||
                    mn == "createTextBox" || mn == "createHeader" ||
                    mn == "createMenu" || mn == "addItem" ||
                    mn == "addSeparator" || mn == "show" || mn == "hide" ||
                    mn == "setEventMask" || mn == "clearEventMask" ||
                    mn == "addTimer" || mn == "changeActiveSection" ||
                    mn == "osdCommand") {
                  call_mref = true;
                  break;
                }
              }
              // Bare string pool may hold the name when mref slot is empty.
              if ((peek.op == 0x1a || peek.op == 0x12) && peek.has_imm) {
                std::string mn = field_name(peek.imm);
                if (mn.empty()) mn = cstr(peek.imm);
                if (mn == "createBG" || mn == "createHotkey" ||
                    mn == "createButton" || mn == "createText" ||
                    mn == "createTextBox" || mn == "createHeader" ||
                    mn == "createMenu" || mn == "addItem" ||
                    mn == "addSeparator" || mn == "show" || mn == "hide" ||
                    mn == "setEventMask" || mn == "clearEventMask" ||
                    mn == "addTimer" || mn == "changeActiveSection" ||
                    mn == "osdCommand") {
                  call_mref = true;
                  break;
                }
              }
              if (peek.op == 0x10 || peek.op == 0x2b) break;
              // 0x19 often sits between packing and invoke (getstatic sugar).
            }
            if (call_mref) {
              if (ip >= 2 && tree.nodes[ip - 2].op == 0x27 && !stack.empty() &&
                  stack.back().tag == JvmTag::Obj) {
                pop();
              }
              break;
            }
            if (!stack.empty() && stack.back().tag == JvmTag::Obj) pop();

            // Packing sugar before INVOKESTATIC (e.g. first(path, FLAGS)) — not next.
            bool invoke_soon = false;
            for (size_t j = ip + 1; j < tree.nodes.size() && j < ip + 8; ++j) {
              const uint8_t op = tree.nodes[j].op;
              if (op == 0x10 || op == 0x12) {
                invoke_soon = true;
                break;
              }
              if (op == 0x19) continue;  // peek/getstatic sugar before invoke
              if (op == 0x1a || op == 0x2b) break;
            }
            if (invoke_soon) break;

            InvObject* ff = nullptr;
            const int idx = ff_local >= 0 ? ff_local : 2;
            if (static_cast<size_t>(idx) < locals.size() &&
                locals[static_cast<size_t>(idx)].tag == JvmTag::Obj) {
              ff = locals[static_cast<size_t>(idx)].v.o;
            }
            bool has_rel_goto = false;
            for (size_t j = ip + 1; j < tree.nodes.size() && j < ip + 15; ++j) {
              const TreeNode& peek = tree.nodes[j];
              if (peek.op == 0x14 && peek.has_imm && (peek.imm & 0x80000000u)) {
                has_rel_goto = true;
                break;
              }
              if (peek.op == 0x2b) break;
            }
            if (ff) {
              if (has_rel_goto) {
                JvmValue r = host->call_by_name("java.io.FindFile", "next",
                                                {JvmValue::make_obj(ff)}, false);
                push(r);
                const int ni = name_local >= 0 ? name_local : 3;
                ensure_local(static_cast<size_t>(ni));
                locals[static_cast<size_t>(ni)] = r;
              } else {
                host->call_by_name("java.io.FindFile", "close",
                                   {JvmValue::make_obj(ff)}, false);
              }
            }
            break;
          }
          if (last_new) {
            push(JvmValue::make_obj(last_new));
            last_new = nullptr;
            break;
          }
          break;
        }

        if (pending_concat) {
          do_concat_n(std::max(1, pending_concat_args / 2));
          break;
        }

        // End of getstatic+NEWARRAY FILD expression — leave array on stack.
        if (pending_static_newarray && n.op == 0x10) {
          pending_static_newarray = false;
          break;
        }

        std::string mname;
        if (n.has_imm) {
          mname = field_name(n.imm);
          if (mname.empty()) mname = cstr(n.imm);
        }
        mname = resolve_invoke_name(mname);
        // createMenu: getfield osd then 0x12 with broken imm (Garage TREE).
        // Only when stack below Osd looks like Style+floats — not addItem.
        if (mname_is_garbage(mname) && ip > 0) {
          for (int k = 1; k <= 3 && ip >= static_cast<size_t>(k); ++k) {
            const TreeNode& pn = tree.nodes[ip - static_cast<size_t>(k)];
            if (pn.op != 0x1b || !pn.has_imm) continue;
            std::string fn = field_name(pn.imm);
            if (fn.empty()) fn = cstr(pn.imm);
            if (fn == "osd") {
              // Style may be local-only; floats under Osd identify createMenu.
              // RR is addItem; String/Font is createText — ori int alone must
              // not veto.
              bool ok = false, bad = false, has_float = false, text = false;
              for (size_t i = 0; i < 5 && i < stack.size(); ++i) {
                const JvmValue& v = stack[stack.size() - 1 - i];
                if (v.tag == JvmTag::Obj && v.v.o) {
                  const char* hc = tree_host_class(v.v.o);
                  if (hc && std::strstr(hc, "Style")) ok = true;
                  if (hc && (std::strstr(hc, "ResourceRef") ||
                             std::strstr(hc, "GameRef")))
                    bad = true;
                  if (hc && (std::strstr(hc, "Font") || std::strstr(hc, "Text")))
                    text = true;
                  if ((!hc || !hc[0] || std::strstr(hc, "String")) &&
                      string_cstr(v.v.o))
                    text = true;
                } else if (v.tag == JvmTag::Float)
                  has_float = true;
              }
              if (!ok) {
                for (const JvmValue& loc : locals) {
                  if (loc.tag != JvmTag::Obj || !loc.v.o) continue;
                  const char* hc = tree_host_class(loc.v.o);
                  if (hc && std::strstr(hc, "Style")) {
                    ok = true;
                    break;
                  }
                }
              }
              if ((ok || has_float) && has_float && !bad && !text)
                mname = "createMenu";
              break;
            }
          }
        }
        if (mname.find('.') != std::string::npos && mname.find('(') == std::string::npos) {
          break;
        }
        if (mname_is_garbage(mname)) break;

        auto argc_for_method = [&](const std::string& mname) -> int {
          // 0x12 <init>: recv on top after NEW+0x11. argc=0 was dropping
          // Trigger(map,null,pos,alias) / GameRef(parent,rid,params,alias).
          if (mname.find(' ') != std::string::npos) return 0;
          if (mname == "<init>") {
            if (stack.empty() || stack.back().tag != JvmTag::Obj ||
                !stack.back().v.o)
              return -1;
            const char* hc = tree_host_class(stack.back().v.o);
            if (hc && std::strstr(hc, "Trigger")) {
              for (size_t i = 1; i < 6 && i < stack.size(); ++i) {
                if (stack[stack.size() - 1 - i].tag == JvmTag::Float)
                  return 5;
              }
              return 4;
            }
            if (hc && std::strstr(hc, "ParkingCar")) return 5;
            if (hc && std::strstr(hc, "Camera")) return -1;
            if (hc && std::strstr(hc, "RenderRef")) {
              if (stack.size() >= 4) return 3;
              return 1;
            }
            if (hc && std::strstr(hc, "GameRef")) {
              if (stack.size() >= 5) return 4;
              return 1;
            }
            if (hc && std::strstr(hc, "Rectangle")) return 3;
            if (hc && std::strstr(hc, "Menu") && !std::strstr(hc, "MainMenu")) {
              if (stack.size() >= 7 &&
                  stack[stack.size() - 2].tag == JvmTag::Int)
                return 6;
              return 5;
            }
            if (is_tree_vector3(stack.back().v.o) ||
                ((!hc || !hc[0]) &&
                 pending_class.find("Vector3") != std::string::npos)) {
              if (stack.size() >= 2 &&
                  stack[stack.size() - 2].tag == JvmTag::Obj &&
                  stack[stack.size() - 2].v.o &&
                  (is_tree_vector3(stack[stack.size() - 2].v.o) ||
                   is_tree_ypr(stack[stack.size() - 2].v.o)))
                return 1;
              int nf = 0;
              for (size_t i = 1; i < 4 && i < stack.size(); ++i) {
                const JvmValue& v = stack[stack.size() - 1 - i];
                if (v.tag == JvmTag::Float || v.tag == JvmTag::Int)
                  ++nf;
                else
                  break;
              }
              return nf > 0 ? nf : 0;
            }
            return -1;
          }
          if (mname == "create") {
            auto is_rr = [](InvObject* o) -> bool {
              if (!o) return false;
              const char* c = tree_host_class(o);
              if (!c || !c[0]) return false;
              if (std::strstr(c, "Camera")) return false;
              return std::strstr(c, "RenderRef") != nullptr;
            };
            if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                stack.back().v.o) {
              const char* tc = tree_host_class(stack.back().v.o);
              if (tc && std::strstr(tc, "Camera")) return -1;
              if (is_rr(stack.back().v.o)) return 3;
              const char* sc = string_cstr(stack.back().v.o);
              if (sc && (!tc || !tc[0] || std::strstr(tc, "String")) &&
                  stack.size() >= 4) {
                const JvmValue& maybe = stack[stack.size() - 4];
                if (maybe.tag == JvmTag::Obj && is_rr(maybe.v.o)) return 3;
              }
            }
            if (!pending_class.empty() &&
                pending_class.find("RenderRef") != std::string::npos &&
                pending_class.find("Camera") == std::string::npos)
              return 3;
            if (!locals.empty() && locals[0].tag == JvmTag::Obj &&
                is_rr(locals[0].v.o))
              return 3;
            return -1;
          }
          if (mname == "createBG") return 1;
          if (mname == "createHotkey") {
            // (key,flags,cmd,handler[+ef]); recv on top.
            if (stack.size() >= 6 && stack[stack.size() - 2].tag == JvmTag::Int &&
                stack[stack.size() - 3].tag == JvmTag::Obj)
              return 5;
            return 4;
          }
          if (mname == "show" || mname == "hide") {
            if (stack.size() >= 2 && stack[stack.size() - 2].tag == JvmTag::Int)
              return 1;
            return 0;
          }
          if (mname == "setEventMask" || mname == "clearEventMask") return 1;
          if (mname == "addTimer") return 2;
          if (mname == "addTraffic" || mname == "addTrafficN") return 5;
          if (mname == "queueEvent") return 3;
          if (mname == "enter" || mname == "exit") return 1;
          if (mname == "setPedestrianDensity" || mname == "setPedestrianDensityN")
            return 1;
          if (mname == "addPedestrianType" || mname == "remPedestrianType" ||
              mname == "removePedestrianType" || mname == "remTrafficCar" ||
              mname == "enableOsd")
            return 1;
          if (mname == "findRoute") return 2;
          if (mname == "getRoutePos" || mname == "getRouteDist") return 1;
          // Stock: getRouteLength(V3,V3)F caches route; ()F reads cache.
          if (mname == "getRouteLength") {
            if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
                stack[stack.size() - 2].tag == JvmTag::Obj)
              return 2;
            return 0;
          }
          if (mname == "getStartDirection") return 2;
          if (mname == "haltTrafficCross" || mname == "haltTrafficPath")
            return 2;
          if (mname == "startRace") return 3;
          if (mname == "mul" || mname == "add" || mname == "sub" ||
              mname == "setParent")
            return 1;
          if (mname == "diff") return 2;
          if (mname == "getVel" || mname == "stop" ||
              mname == "createQuickRaceBot")
            return 0;
          if (mname == "createCar") {
            if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
                stack[stack.size() - 2].tag == JvmTag::Obj)
              return 2;
            return 1;
          }
          if (mname == "getNearestCross") {
            if (stack.size() >= 2 &&
                (stack.back().tag == JvmTag::Float ||
                 stack.back().tag == JvmTag::Int) &&
                stack[stack.size() - 2].tag == JvmTag::Obj)
              return 2;
            return 1;
          }
          if (mname == "alignToRoad" || mname == "time2Config") return 1;
          if (mname == "plotRoute") return 5;
          if (mname == "addMarker") {
            if (stack.size() >= 3 && stack.back().tag == JvmTag::Int &&
                stack[stack.size() - 2].tag == JvmTag::Obj &&
                stack[stack.size() - 3].tag == JvmTag::Obj)
              return 3;
            if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
                stack[stack.size() - 2].tag == JvmTag::Obj)
              return 2;
            return 1;
          }
          if (mname == "remMarker" || mname == "updateNavigator") return 1;
          if (mname == "timeWarp") return 1;
          if (mname == "changeZoom") return 1;
          if (mname == "changeSize") return 4;
          if (mname == "setWater") return 4;
          if (mname == "addWaterLimit" || mname == "addTrafficCar" ||
              mname == "notifyTrafficCar" || mname == "setTrafficCarBehaviour")
            return 2;
          if (mname == "addTrigger") return 6;
          if (mname == "addNotification") {
            // (ref,etype,ealias,msg) or + custmethod (City.startRace).
            if (stack.size() >= 5 && stack.back().tag == JvmTag::Obj) {
              const JvmValue& a2 = stack[stack.size() - 3];
              const JvmValue& a3 = stack[stack.size() - 4];
              if (a2.tag == JvmTag::Int && a3.tag == JvmTag::Int) return 5;
            }
            return 4;
          }
          if (mname == "prepareNightRace" || mname == "removeAllTimers" ||
              mname == "flush" || mname == "changeCamChase")
            return 0;
          if (mname == "createOSDObjects" || mname == "lockCar" ||
              mname == "releaseCar" || mname == "remSceneElements" ||
              mname == "endGroup" || mname == "beginGroup" ||
              mname == "display" || mname == "flushInventory" ||
              mname == "wakeUp" || mname == "precache" ||
              mname == "countTokens" || mname == "addCustomGroups" ||
              mname == "enableAnimateHook" || mname == "disableAnimateHook" ||
              mname == "addSeparator")
            return 0;
          if (mname == "changeMode" || mname == "addSceneElements" ||
              mname == "cameraSetup" || mname == "enable" ||
              mname == "addHandler" || mname == "remHandler" ||
              mname == "enableCameraControl" || mname == "filterInventory" ||
              mname == "scrollToLine" || mname == "command" ||
              mname == "setDamageMultiplier" || mname == "setCruiseControl" ||
              mname == "duplicate" || mname == "play" || mname == "nextToken" ||
              mname == "printValue" || mname == "setValue" ||
              mname == "setTicks" || mname == "setOffset" ||
              mname == "setToolTip" || mname == "token" ||
              mname == "hideGroup" || mname == "showGroup")
            return 1;
          if (mname == "setSliderStyle" || mname == "setRange") return 2;
          if (mname == "giveWarning" || mname == "getAxis" ||
              mname == "setMatrix")
            return 2;
          if (mname == "setSpeed" || mname == "setFade" || mname == "seek")
            return 1;
          if (mname == "createTextBox") return 6;
          if (mname == "createButton") {
            // (style,x,y,label,cmd) or (style,x,y,pri,label,cmd)
            if (stack.size() >= 7 && stack.back().tag == JvmTag::Int &&
                stack[stack.size() - 2].tag == JvmTag::Obj &&
                stack[stack.size() - 3].tag == JvmTag::Int)
              return 6;
            return 5;
          }
          if (mname == "createHeader") return 1;
          if (mname == "createRectangle") return 6;
          if (mname == "neon") return 0;
          if (mname == "userWait") {
            if (stack.size() >= 2 &&
                (stack[stack.size() - 2].tag == JvmTag::Float ||
                 stack[stack.size() - 2].tag == JvmTag::Int))
              return 1;
            return 0;
          }
          if (mname == "checkHint") return 1;
          if (mname == "activateState") {
            if (stack.size() >= 3 &&
                stack[stack.size() - 2].tag == JvmTag::Int &&
                stack[stack.size() - 3].tag == JvmTag::Int)
              return 2;
            return 1;
          }
          if (mname == "activate") {
            if (stack.size() >= 2 &&
                (stack[stack.size() - 2].tag == JvmTag::Int ||
                 stack[stack.size() - 2].tag == JvmTag::Obj))
              return 1;
            return 0;
          }
          if (mname == "remNotification") return 2;
          if (mname == "cleanup" || mname == "disableCameraControl") return 0;
          if (mname == "flushInventory" || mname == "currentLine") return 0;
          if (mname == "osdCommand") return 1;
          if (mname == "giveWarning") return 2;
          if (mname == "changePointer") return 0;
          if (mname == "log") {
            if (stack.size() >= 2 && stack[stack.size() - 2].tag == JvmTag::Obj)
              return 1;
            return 0;
          }
          if (mname == "putMessage") return 1;
          if (mname == "setPriority") return 1;
          if (mname == "setPos") {
            if (stack.size() >= 3 &&
                (stack[stack.size() - 2].tag == JvmTag::Float ||
                 stack[stack.size() - 2].tag == JvmTag::Int) &&
                (stack[stack.size() - 3].tag == JvmTag::Float ||
                 stack[stack.size() - 3].tag == JvmTag::Int))
              return 2;
            if (stack.size() >= 2 && stack[stack.size() - 2].tag == JvmTag::Obj)
              return 1;
            return 0;
          }
          if (mname == "changeCamTarget" || mname == "changeCamTarget2") {
            if (stack.size() >= 2 && stack[stack.size() - 2].tag == JvmTag::Obj)
              return 1;
            return 0;
          }
          if (mname == "setTransmission" || mname == "leaveCar" ||
              mname == "constructName") {
            if (stack.size() >= 2 &&
                (stack[stack.size() - 2].tag == JvmTag::Int ||
                 stack[stack.size() - 2].tag == JvmTag::Float))
              return 1;
            return 0;
          }
          if (mname == "setDefaultTransmission" ||
              mname == "setDefaultSteeringHelp" || mname == "setDefaultASR" ||
              mname == "setDefaultABS")
            return 0;
          if (mname == "set") {
            if (stack.size() >= 2 &&
                (stack[stack.size() - 2].tag == JvmTag::Int ||
                 stack[stack.size() - 2].tag == JvmTag::Obj))
              return 1;
            return 0;
          }
          if (mname == "removeAllElements") return 0;
          if (mname == "createMenu") {
            // (style,x,y,spc[,ori]) or +stySld/+knob. Do NOT use raw
            // stack.size() — leftover ResourceRef/look packs inflate it and
            // Garage.createOSDObjects then mis-packs (TREE btn=1).
            // ResourceRef is addItem, never a createMenu arg — stop before it.
            int base = 0;
            if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
              const char* hc = tree_host_class(stack.back().v.o);
              if (hc && std::strstr(hc, "Osd")) base = 1;
            }
            auto at = [&](int i) -> const JvmValue* {
              if (i < 0 || static_cast<size_t>(i) >= stack.size())
                return nullptr;
              return &stack[stack.size() - 1 - static_cast<size_t>(i)];
            };
            int nobj = 0, nfloat = 0, nint = 0;
            for (int i = base; i < base + 8; ++i) {
              const JvmValue* v = at(i);
              if (!v) break;
              if (v->tag == JvmTag::Float) ++nfloat;
              else if (v->tag == JvmTag::Int) ++nint;
              else if (v->tag == JvmTag::Obj && v->v.o) {
                const char* hc = tree_host_class(v->v.o);
                if (hc && std::strstr(hc, "Style"))
                  ++nobj;
                else if (hc && std::strstr(hc, "ResourceRef"))
                  break;
                else if (hc && std::strstr(hc, "Osd"))
                  break;
                else if (hc && (std::strstr(hc, "Menu") ||
                                std::strstr(hc, "Button") ||
                                std::strstr(hc, "Garage")))
                  break;
                else
                  ++nobj;
              } else
                break;
            }
            const int nargs = nobj + nfloat + nint;
            if (nargs >= 6) return 6;
            if (nargs >= 5) return 5;
            if (nargs >= 4) return 4;
            return 5;
          }
          if (mname == "addItem") {
            // Button / slider / multi overloads — sniff stack (Menu recv on top
            // or JVM [menu, args…] with last arg on top).
            int base = 0;
            if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
              const char* hc = tree_host_class(stack.back().v.o);
              if (hc && std::strstr(hc, "Menu")) base = 1;
            }
            auto at = [&](int i) -> const JvmValue* {
              if (i < 0 || static_cast<size_t>(i) >= stack.size())
                return nullptr;
              return &stack[stack.size() - 1 - static_cast<size_t>(i)];
            };
            for (int i = base; i < base + 8; ++i) {
              const JvmValue* v = at(i);
              if (!v || v->tag != JvmTag::Obj || !v->v.o) continue;
              const char* hc = tree_host_class(v->v.o);
              if (hc && (std::strstr(hc, "Vector") || hc[0] == '['))
                return 5;  // multi: text,cmd,value,choices,tooltip
            }
            int floats = 0;
            for (int i = base; i < base + 8; ++i) {
              const JvmValue* v = at(i);
              if (v && v->tag == JvmTag::Float) ++floats;
            }
            // Garage icon: RR lives in recent_new (NEW+0x21). Leftover createMenu
            // floats must not force slider argc=4 — that skips icon packing and
            // yields text addItem for CMD_HITTHESTREET/TESTTRACK.
            bool icon_cmd = false;
            for (int i = base; i < base + 6; ++i) {
              const JvmValue* v = at(i);
              if (v && v->tag == JvmTag::Int && is_icon_osd_cmd(v->v.i)) {
                icon_cmd = true;
                break;
              }
            }
            if (icon_cmd && recent_new) {
              const char* rhc = tree_host_class(recent_new);
              if (rhc && (std::strstr(rhc, "ResourceRef") ||
                          std::strstr(rhc, "GameRef")))
                return 3;
            }
            if (floats >= 3) return 7;  // ranged slider
            if (floats >= 1 && !icon_cmd) return 4;  // simple slider
            // (ResourceRef,int,String) icon buttons — Garage.createOSDObjects.
            for (int i = base; i < base + 4; ++i) {
              const JvmValue* v = at(i);
              if (!v || v->tag != JvmTag::Obj || !v->v.o) continue;
              const char* hc = tree_host_class(v->v.o);
              if (hc && (std::strstr(hc, "ResourceRef") ||
                         std::strstr(hc, "GameRef")))
                return 3;
            }
            // (String,int,String tooltip)
            {
              int strs = 0, ints = 0;
              for (int i = base; i < base + 4; ++i) {
                const JvmValue* v = at(i);
                if (!v) break;
                if (v->tag == JvmTag::Int) ++ints;
                if (v->tag == JvmTag::Obj && v->v.o) {
                  const char* hc = tree_host_class(v->v.o);
                  if (!hc || !hc[0] || std::strstr(hc, "String")) ++strs;
                }
              }
              if (strs >= 2 && ints >= 1) return 3;
            }
            // (String,int) — never return 3 just because a String Obj is at base
            // (that ate the label and invented a phantom 3rd arg).
            return 2;
          }
          if (mname == "disable" || mname == "lockPlayerCar" ||
              mname == "moveCamera" || mname == "loadVisibleCars" ||
              mname == "deleteOSDObjects" || mname == "loop" ||
              mname == "enterAsyncMode")
            return 0;
          if (mname == "startdir" || mname == "changeColor") return 1;
          if (mname == "point") {
            if (stack.size() >= 6) return 5;
            return 4;
          }
          if (mname == "duplicate") return 1;
          if (mname == "createText") return 5;
          if (mname == "createBox") return 5;
          // PE @ 0x004806F0 createSphere(ResourceRef,F,String)V — argc 3, not box 5.
          if (mname == "createSphere") return 3;
          if (mname == "timeWarp") return 1;
          if (mname == "getTime" || mname == "changeMusicSet" ||
              mname == "setGlobalEnvmap" || mname == "setTimerText") {
            // resolved as static below
            return -1;
          }
          if (mname == "addElement" || mname == "elementAt" ||
              mname == "addColorIndex")
            return 1;
          if (mname == "size" || mname == "intValue" ||
              mname == "getFirstChild" || mname == "getNextChild" ||
              mname == "cache" || mname == "currentLine" ||
              mname == "toString" ||
              mname == "getPos" || mname == "getOri" || mname == "destroy" ||
              mname == "unload" || mname == "id" || mname == "reset" ||
              mname == "disableCameraControl")
            return 0;
          return -1;  // unknown
        };

        int argc = 0;
        bool is_static_call = (n.op == 0x12);
        std::string owner = cls_name;
        if (mname == "first") {
          is_static_call = false;
          owner = "java.io.FindFile";
          // recv on top; FLAGS int just under recv → first(path, flags).
          argc = 1;
          if (stack.size() >= 2 && stack[stack.size() - 2].tag == JvmTag::Int)
            argc = 2;
        } else if (mname == "next" || mname == "close") {
          argc = 0;
          is_static_call = false;
          owner = "java.io.FindFile";
        } else if (mname == "delete" || mname == "exists") {
          argc = 1;
          is_static_call = true;
          owner = "java.io.File";
        } else if (mname == "copy" || mname == "move") {
          argc = 2;
          is_static_call = true;
          owner = "java.io.File";
        } else if (mname == "openLib") {
          argc = 1;
          is_static_call = true;
          owner = "java.lang.System";
        } else if (mname == "addColorIndex") {
          argc = 1;
          is_static_call = false;
        } else if (mname == "createHotkey") {
          // SplashScreen encodes osd.createHotkey as 0x12 + packed args;
          // still an instance call on Osd (recv on top after getfield).
          argc = 4;
          if (stack.size() >= 6 && stack[stack.size() - 2].tag == JvmTag::Int &&
              stack[stack.size() - 3].tag == JvmTag::Obj)
            argc = 5;
          is_static_call = false;
          owner = "java.render.Osd";
        } else if (mname == "createMenu" || mname == "createText" ||
                   mname == "createTextBox" || mname == "createButton" ||
                   mname == "createRectangle" || mname == "createHeader" ||
                   mname == "createBG") {
          const int a = argc_for_method(mname);
          argc = a >= 0 ? a : 4;
          is_static_call = false;
          owner = "java.render.Osd";
        } else if (mname == "addItem" || mname == "addSeparator" ||
                   mname == "setSliderStyle") {
          const int a = argc_for_method(mname);
          argc = a >= 0 ? a : (mname == "addSeparator" ? 0 : 2);
          is_static_call = false;
          owner = "java.render.osd.Menu";
        } else if (mname == "disable") {
          argc = 0;
          is_static_call = false;
        } else if (mname == "changeActiveSection") {
          argc = 1;
          is_static_call = true;
          owner = "java.game.GameLogic";
        } else if (mname == "autoSave") {
          argc = 0;
          is_static_call = true;
          owner = "java.game.GameLogic";
        } else if (mname == "autoSaveQuiet") {
          argc = 0;
          is_static_call = true;
          owner = "java.game.GameLogic";
        } else if (mname == "loadDefaults") {
          argc = 0;
          is_static_call = true;
          owner = "java.game.GameLogic";
        } else if (mname == "setTime") {
          argc = 1;
          is_static_call = true;
          owner = "java.game.GameLogic";
        } else if (mname == "getTime") {
          argc = 0;
          is_static_call = true;
          owner = "java.game.GameLogic";
        } else if (mname == "setTimerText") {
          argc = 2;
          is_static_call = true;
          owner = "java.game.GameLogic";
        } else if (mname == "setGlobalEnvmap") {
          argc = 1;
          is_static_call = true;
          owner = "java.render.GfxEngine";
        } else if (mname == "changeMusicSet") {
          argc = 1;
          is_static_call = true;
          owner = "java.sound.Sound";
        } else if (mname == "getVolume" || mname == "setVolume") {
          argc = (mname == "setVolume") ? 2 : 1;
          is_static_call = true;
          owner = "java.sound.Sound";
        } else if (mname == "numDisplayModes" || mname == "currDisplayMode") {
          argc = 0;
          is_static_call = true;
          owner = "java.render.GfxEngine";
        } else if (mname == "displayModeName" || mname == "changeVideoMode") {
          argc = (mname == "changeVideoMode") ? 3 : 1;
          is_static_call = true;
          owner = "java.render.GfxEngine";
        } else if (mname == "getConfigOptions") {
          argc = 0;
          is_static_call = true;
          owner = "java.lang.System";
        } else if (mname == "getAxis") {
          argc = 2;
          is_static_call = true;
          owner = "java.io.Input";
        } else if (mname == "timeWarp") {
          argc = 1;
          is_static_call = true;
          owner = "java.lang.System";
        } else if (mname == "show" || mname == "hide") {
          // May be Osd (argc 0) or LoadingScreen — prefer recv class.
          if (stack.size() >= 1 && stack.back().tag == JvmTag::Obj) {
            const char* hc = tree_host_class(stack.back().v.o);
            if (hc && std::strstr(hc, "LoadingScreen")) {
              argc = 0;
              is_static_call = false;
              owner = "java.render.LoadingScreen";
            } else {
              const int a = argc_for_method(mname);
              argc = a >= 0 ? a : 0;
              is_static_call = false;
            }
          } else {
            const int a = argc_for_method(mname);
            argc = a >= 0 ? a : 0;
            is_static_call = false;
          }
        } else if (mname == "userWait") {
          argc = 0;
          if (stack.size() >= 2 &&
              (stack[stack.size() - 2].tag == JvmTag::Float ||
               stack[stack.size() - 2].tag == JvmTag::Int))
            argc = 1;
          is_static_call = false;
          owner = "java.render.LoadingScreen";
        } else if (mname == "osdCommand") {
          // SplashScreen.handleEvent encodes osdCommand as 0x12 instance call.
          argc = 1;
          is_static_call = false;
        } else if (mname == "random" || mname == "randomize") {
          argc = 0;
          is_static_call = true;
          owner = "java.lang.Math";
        } else if (mname == "<init>") {
          const int a = argc_for_method(mname);
          argc = a >= 0 ? a : 0;
          if (argc <= 0 &&
              pending_class.find("Trigger") != std::string::npos) {
            argc = 4;
            for (size_t i = 0; i < stack.size() && i < 6; ++i) {
              if (stack[stack.size() - 1 - i].tag == JvmTag::Float) {
                argc = 5;
                break;
              }
            }
          } else if (argc <= 0 &&
                     pending_class.find("ParkingCar") != std::string::npos) {
            argc = 5;
          } else if (argc <= 0 &&
                     pending_class.find("RenderRef") != std::string::npos &&
                     pending_class.find("Camera") == std::string::npos) {
            argc = (stack.size() >= 4) ? 3 : 1;
          } else if (argc <= 0 &&
                     pending_class.find("GameRef") != std::string::npos) {
            argc = (stack.size() >= 5) ? 4 : 1;
          } else if (argc <= 0 &&
                     pending_class.find("Vector3") != std::string::npos) {
            const int a = argc_for_method(mname);
            argc = a >= 0 ? a : 0;
          } else if (argc <= 0 &&
                     pending_class.find("Rectangle") != std::string::npos) {
            argc = 3;
          } else if (argc <= 0 &&
                     pending_class.find("Menu") != std::string::npos &&
                     pending_class.find("MainMenu") == std::string::npos) {
            argc = 5;
            if (stack.size() >= 7 &&
                stack[stack.size() - 2].tag == JvmTag::Int)
              argc = 6;
          }
          is_static_call = false;
        } else {
          const int a = argc_for_method(mname);
          if (a >= 0) {
            argc = a;
            is_static_call = false;
          } else if (!mname.empty()) {
            argc = 0;
          } else {
            break;
          }
        }

        std::vector<JvmValue> args;
        if (!is_static_call) {
          auto is_self_method = [&](const std::string& m) {
            // changeMode is Garage/Dialog self OR Navigator (track.nav) —
            // never force RaceSetup/City this as recv.
            return m == "createOSDObjects" || m == "lockCar" ||
                   m == "releaseCar" ||
                   m == "addSceneElements" || m == "remSceneElements" ||
                   m == "cameraSetup" || m == "giveWarning" ||
                   m == "setEventMask" || m == "clearEventMask" ||
                   m == "addTimer" || m == "addCustomGroups" ||
                   m == "enableAnimateHook" || m == "disableAnimateHook" ||
                   m == "createQuickRaceBot" || m == "destroyRaceBot" ||
                   m == "startRace2";
          };
          auto is_osd_builder = [&](const std::string& m) {
            return m == "createButton" || m == "createText" ||
                   m == "createTextBox" || m == "createRectangle" ||
                   m == "createHeader" || m == "createMenu" ||
                   m == "createBG" || m == "createHotkey" ||
                   m == "addItem" || m == "addSeparator" ||
                   m == "setSliderStyle";
          };
          JvmValue recv = JvmValue::make_obj(nullptr);
          auto is_osd_junk = [&](const JvmValue& v) -> bool {
            if (v.tag != JvmTag::Obj || !v.v.o) return false;
            const char* hc = tree_host_class(v.v.o);
            if (!hc || !hc[0]) return false;
            return std::strstr(hc, "Rectangle") || std::strstr(hc, "Button") ||
                   std::strstr(hc, "Text") || std::strstr(hc, "Hotkey") ||
                   std::strstr(hc, "Group");
          };
          if (mname == "addItem" && (argc == 2 || argc == 3)) {
            while (!stack.empty() && is_osd_junk(stack.back())) pop();
            // Garage: buttonStyle often left on stack after createMenu packing.
            while (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                   stack.back().v.o) {
              const char* shc = tree_host_class(stack.back().v.o);
              if (shc && std::strstr(shc, "Style"))
                pop();
              else
                break;
            }
            JvmValue label = JvmValue::make_obj(nullptr);
            JvmValue tip = JvmValue::make_obj(nullptr);
            JvmValue gfx = JvmValue::make_obj(nullptr);
            JvmValue cmdv = JvmValue::make_int(0);
            bool have_cmd = false;
            int32_t gfx_rid = 0;
            for (int guard = 0; guard < 12 && !stack.empty(); ++guard) {
              JvmValue v = pop();
              if (v.tag == JvmTag::Int) {
                if (v.v.i > 0x10000)
                  gfx_rid = v.v.i;
                else if (is_icon_osd_cmd(v.v.i) || !have_cmd) {
                  // Last icon CMD wins — leftover 0 (createMenu ori) is popped
                  // first; RaceSetup CMD_ABANDON=1 must replace it.
                  cmdv = v;
                  have_cmd = true;
                }
              } else if (v.tag == JvmTag::Obj && v.v.o) {
                const char* hc = tree_host_class(v.v.o);
                if (hc && (std::strstr(hc, "ResourceRef") ||
                           std::strstr(hc, "GameRef"))) {
                  if (!gfx.v.o) gfx = v;
                } else if (!hc || !hc[0] || std::strstr(hc, "String")) {
                  const char* sc = string_cstr(v.v.o);
                  // 0x09 method-name ldc (addItem/createMenu) is not a tip.
                  if (sc && is_osd_invoke_name(sc)) {
                    /* skip */
                  } else if (!label.v.o)
                    label = v;
                  else if (!tip.v.o)
                    tip = v;
                } else if (hc && std::strstr(hc, "Menu"))
                  recv = v;
              }
              if (have_cmd && (gfx.v.o || label.v.o) && cmdv.v.i != 0)
                break;
            }
            if (!gfx.v.o && recent_new) {
              const char* rhc = tree_host_class(recent_new);
              if (rhc && (std::strstr(rhc, "ResourceRef") ||
                          std::strstr(rhc, "GameRef"))) {
                gfx = JvmValue::make_obj(recent_new);
                recent_new = nullptr;
              }
            }
            if (gfx.v.o && gfx_rid)
              java_util_resource_ResourceRef_set(gfx.v.o, gfx_rid);
            if (recv.tag != JvmTag::Obj || !recv.v.o) {
              if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                const char* thc = tree_host_class(stack.back().v.o);
                if (thc && std::strstr(thc, "Menu")) recv = pop();
              }
              if ((recv.tag != JvmTag::Obj || !recv.v.o) && last_menu)
                recv = JvmValue::make_obj(last_menu);
            }
            while (!stack.empty() && is_osd_junk(stack.back())) pop();
            // Recover RR under tip/cmd — do not discard it as sugar.
            while (!stack.empty() && stack.back().tag == JvmTag::Obj) {
              const char* hc = tree_host_class(stack.back().v.o);
              if (hc && (std::strstr(hc, "ResourceRef") ||
                         std::strstr(hc, "GameRef"))) {
                if (!gfx.v.o) gfx = pop();
                else
                  pop();
              } else if (hc && (std::strstr(hc, "Style") ||
                                std::strstr(hc, "Button")))
                pop();
              else
                break;
            }
            args.push_back(recv);
            if (gfx.v.o) {
              args.push_back(gfx);
              args.push_back(cmdv);
              args.push_back(tip.v.o ? tip : label);
            } else {
              args.push_back(label);
              args.push_back(cmdv);
              if (tip.v.o) args.push_back(tip);
            }
          } else if (is_osd_builder(mname)) {
            // Dialog.show: LOCAL_LOAD osd then 0x12 → recv on top.
            // Splash createBG often pushes osd first (JVM order).
            bool recv_on_top = false;
            if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
              const char* thc = tree_host_class(stack.back().v.o);
              if (thc && std::strstr(thc, "Osd")) recv_on_top = true;
            }
            if (mname == "createButton") {
              if (recv_on_top) {
                if (stack.size() >= 8 &&
                    stack[stack.size() - 2].tag == JvmTag::Int &&
                    stack[stack.size() - 3].tag == JvmTag::Obj &&
                    stack[stack.size() - 4].tag == JvmTag::Int)
                  argc = 6;
                else
                  argc = 5;
              } else if (stack.size() >= 7 && stack.back().tag == JvmTag::Int &&
                         stack[stack.size() - 2].tag == JvmTag::Obj &&
                         stack[stack.size() - 3].tag == JvmTag::Int) {
                argc = 6;
              } else {
                argc = 5;
              }
            }
            if (recv_on_top) {
              recv = pop();
              for (int i = 0; i < argc; ++i) {
                if (!stack.empty())
                  args.insert(args.begin(), pop());
                else
                  args.insert(args.begin(), JvmValue::make_int(0));
              }
              args.insert(args.begin(), recv);
            } else {
              // JVM: [osdRecv, args…] last arg on top.
              for (int i = 0; i < argc; ++i) {
                if (!stack.empty())
                  args.insert(args.begin(), pop());
                else
                  args.insert(args.begin(), JvmValue::make_int(0));
              }
              if (!stack.empty() && stack.back().tag == JvmTag::Obj)
                recv = pop();
              else if (!locals.empty() && locals[0].tag == JvmTag::Obj) {
                if (InvObject* osd =
                        tree_field_get_obj(locals[0].v.o, "osd"))
                  recv = JvmValue::make_obj(osd);
                else
                  recv = locals[0];
              }
              args.insert(args.begin(), recv);
            }
            // Garage: buttonStyle lives in a local; inject if packing missed it.
            if (mname == "createMenu") {
              bool has_sty = false;
              for (size_t i = 1; i < args.size(); ++i) {
                if (args[i].tag != JvmTag::Obj || !args[i].v.o) continue;
                const char* hc = tree_host_class(args[i].v.o);
                if (hc && std::strstr(hc, "Style")) {
                  has_sty = true;
                  break;
                }
              }
              if (!has_sty) {
                for (const JvmValue& loc : locals) {
                  if (loc.tag != JvmTag::Obj || !loc.v.o) continue;
                  const char* hc = tree_host_class(loc.v.o);
                  if (hc && std::strstr(hc, "Style")) {
                    args.insert(args.begin() + 1, loc);
                    break;
                  }
                }
              }
            }
          } else if (mname == "addTraffic" || mname == "addTrafficCar" ||
                     mname == "addTrafficN" || mname == "addTrafficP" ||
                     mname == "notifyTrafficCar" || mname == "setTrafficCarBehaviour" || mname == "remTrafficCar" ||
                     mname == "setPedestrianDensity" ||
                     mname == "setPedestrianDensityN" ||
                     mname == "addPedestrianType" ||
                     mname == "remPedestrianType" || mname == "setWater" ||
                     mname == "addWaterLimit" || mname == "getNearestCross" ||
                     mname == "getStartDirection" || mname == "getRouteLength" ||
                     mname == "findRoute" || mname == "haltTrafficCross" ||
                     mname == "haltTrafficPath" || mname == "alignToRoad" ||
                     mname == "delTraffic") {
            // JVM: [map, args…] last arg on top. Do not fall back to Valocity
            // `this` when floats/ints sit on the stack. RaceSetup uses track.map.
            auto resolve_map = [&](InvObject* o) -> InvObject* {
              if (!o) return nullptr;
              if (InvObject* map = tree_field_get_obj(o, "map")) return map;
              if (InvObject* track = tree_field_get_obj(o, "track")) {
                if (InvObject* map = tree_field_get_obj(track, "map"))
                  return map;
              }
              if (InvObject* last = tree_field_get_obj(o, "lastState")) {
                if (InvObject* map = tree_field_get_obj(last, "map"))
                  return map;
              }
              return nullptr;
            };
            for (int i = 0; i < argc; ++i) {
              if (!stack.empty())
                args.insert(args.begin(), pop());
              else
                args.insert(args.begin(), JvmValue::make_int(0));
            }
            if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
              const char* thc = tree_host_class(stack.back().v.o);
              if (thc && std::strstr(thc, "GroundRef"))
                recv = pop();
            }
            if ((recv.tag != JvmTag::Obj || !recv.v.o) && !locals.empty() &&
                locals[0].tag == JvmTag::Obj && locals[0].v.o) {
              if (InvObject* map = resolve_map(locals[0].v.o))
                recv = JvmValue::make_obj(map);
              else
                recv = locals[0];
            }
            args.insert(args.begin(), recv);
          } else if (mname == "addMarker" || mname == "remMarker" ||
                     mname == "updateNavigator" || mname == "changeMode" ||
                     mname == "changeSize" || mname == "changeZoom") {
            auto resolve_nav = [&](InvObject* o) -> InvObject* {
              if (!o) return nullptr;
              const char* ohc = tree_host_class(o);
              if (ohc && std::strstr(ohc, "Navigator")) return o;
              if (InvObject* nav = tree_field_get_obj(o, "nav")) return nav;
              if (InvObject* track = tree_field_get_obj(o, "track")) {
                if (InvObject* nav = tree_field_get_obj(track, "nav"))
                  return nav;
              }
              if (InvObject* last = tree_field_get_obj(o, "lastState")) {
                if (InvObject* nav = tree_field_get_obj(last, "nav"))
                  return nav;
              }
              return nullptr;
            };
            for (int i = 0; i < argc; ++i) {
              if (!stack.empty())
                args.insert(args.begin(), pop());
              else
                args.insert(args.begin(), JvmValue::make_int(0));
            }
            if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
              const char* thc = tree_host_class(stack.back().v.o);
              if (thc && (std::strstr(thc, "Navigator") ||
                          std::strstr(thc, "Nav")))
                recv = pop();
            }
            // Garage/Dialog.changeMode(group) stays on self.
            if ((recv.tag != JvmTag::Obj || !recv.v.o) && !locals.empty() &&
                locals[0].tag == JvmTag::Obj && locals[0].v.o) {
              const char* lhc = tree_host_class(locals[0].v.o);
              const bool garage_or_dlg =
                  lhc && (std::strstr(lhc, "Garage") ||
                          std::strstr(lhc, "Dialog") ||
                          std::strstr(lhc, "MainMenu"));
              if (mname == "changeMode" && garage_or_dlg)
                recv = locals[0];
              else if (InvObject* nav = resolve_nav(locals[0].v.o))
                recv = JvmValue::make_obj(nav);
              else
                recv = locals[0];
            }
            args.insert(args.begin(), recv);
          } else if (is_self_method(mname) && !locals.empty() &&
              locals[0].tag == JvmTag::Obj) {
            // Garage/Scene methods bind to `this`; ignore stray Osd/Vector3 on stack.
            if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                stack.back().v.o != locals[0].v.o)
              pop();
            recv = locals[0];
            for (int i = 0; i < argc; ++i) {
              if (!stack.empty())
                args.insert(args.begin(), pop());
              else
                args.insert(args.begin(), JvmValue::make_int(0));
            }
            args.insert(args.begin(), recv);
          } else if (mname == "queueEvent") {
            pack_queue_event(stack, locals, args, &recv);
          } else if (mname == "create") {
            pack_renderref_create(stack, locals, args, &recv);
          } else if (mname == "add" || mname == "mul" || mname == "sub") {
            pack_vector3_binop(stack, locals, args, &recv);
          } else {
            if (!stack.empty() && stack.back().tag == JvmTag::Obj)
              recv = pop();
            else if (!locals.empty())
              recv = locals[0];
            for (int i = 0; i < argc; ++i) {
              if (!stack.empty())
                args.insert(args.begin(), pop());
              else
                args.insert(args.begin(), JvmValue::make_int(0));
            }
            args.insert(args.begin(), recv);
          }
          const char* hc = tree_host_class(recv.v.o);
          if (hc && hc[0]) owner = hc;
          if ((mname == "add" || mname == "mul" || mname == "sub") &&
              recv.tag == JvmTag::Obj && is_tree_vector3(recv.v.o))
            owner = "java.lang.Vector3";
          if (mname == "<init>" &&
              pending_class.find("Trigger") != std::string::npos) {
            auto is_trig = [](InvObject* o) -> bool {
              const char* c = tree_host_class(o);
              return c && std::strstr(c, "Trigger");
            };
            InvObject* t = is_trig(recv.v.o) ? recv.v.o : nullptr;
            if (!t && recent_new && is_trig(recent_new)) t = recent_new;
            if (!t && last_new && is_trig(last_new)) t = last_new;
            if (t && (!args.empty() && args[0].v.o != t)) {
              if (recv.tag == JvmTag::Obj && recv.v.o && recv.v.o != t)
                args.push_back(recv);
              args[0] = JvmValue::make_obj(t);
              recv = args[0];
            }
            owner = "java.game.Trigger";
          }
          if (mname == "addElement" || mname == "elementAt" ||
              mname == "size") {
            // Prefer the real Vector handle when TREE left the element on top.
            if (!args.empty() && args[0].tag == JvmTag::Obj &&
                !tree_vector_is(args[0].v.o) && args.size() >= 2 &&
                args[1].tag == JvmTag::Obj && tree_vector_is(args[1].v.o)) {
              std::swap(args[0], args[1]);
            }
            owner = "java.util.Vector";
          }
          if (mname == "createHotkey" || mname == "createBG" ||
              mname == "createButton" || mname == "createText" ||
              mname == "createTextBox" || mname == "createRectangle" ||
              mname == "createHeader" || mname == "createMenu" ||
              mname == "show" || mname == "hide") {
            if (hc && std::strstr(hc, "Osd")) {
              owner = "java.render.Osd";
            } else if (!locals.empty() && locals[0].tag == JvmTag::Obj) {
              if (InvObject* osd = tree_field_get_obj(locals[0].v.o, "osd")) {
                args[0] = JvmValue::make_obj(osd);
                owner = "java.render.Osd";
              }
            } else {
              owner = "java.render.Osd";
            }
          }
          if (mname == "addTraffic" || mname == "addTrafficCar" ||
              mname == "addTrafficN" || mname == "addTrafficP" ||
              mname == "notifyTrafficCar" || mname == "setTrafficCarBehaviour" || mname == "remTrafficCar" ||
              mname == "setPedestrianDensity" ||
              mname == "setPedestrianDensityN" ||
              mname == "addPedestrianType" || mname == "remPedestrianType" ||
              mname == "setWater" || mname == "addWaterLimit" ||
              mname == "getNearestCross" || mname == "getStartDirection" ||
              mname == "getRouteLength" || mname == "findRoute" ||
              mname == "haltTrafficCross" || mname == "haltTrafficPath" ||
              mname == "alignToRoad" || mname == "delTraffic") {
            owner = "java.util.resource.GroundRef";
            if (!args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
              const char* rhc = tree_host_class(args[0].v.o);
              if (!rhc || !std::strstr(rhc, "GroundRef")) {
                if (!locals.empty() && locals[0].tag == JvmTag::Obj &&
                    locals[0].v.o) {
                  InvObject* o = locals[0].v.o;
                  InvObject* map = tree_field_get_obj(o, "map");
                  if (!map) {
                    if (InvObject* track = tree_field_get_obj(o, "track"))
                      map = tree_field_get_obj(track, "map");
                  }
                  if (!map) {
                    if (InvObject* last = tree_field_get_obj(o, "lastState"))
                      map = tree_field_get_obj(last, "map");
                  }
                  if (map) args[0] = JvmValue::make_obj(map);
                }
              }
            }
          }
          if (mname == "startRace") {
            if (!args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
              const char* rhc = tree_host_class(args[0].v.o);
              if (rhc && std::strstr(rhc, "RaceSetup")) {
                InvObject* track = tree_field_get_obj(args[0].v.o, "track");
                if (!track)
                  track = tree_field_get_obj(args[0].v.o, "lastState");
                if (track) {
                  args[0] = JvmValue::make_obj(track);
                  const char* tc = tree_host_class(track);
                  if (tc && tc[0]) owner = tc;
                }
              }
            }
          }
          if (mname == "addMarker" || mname == "remMarker" ||
              mname == "updateNavigator" || mname == "changeMode" ||
              mname == "changeSize" || mname == "changeZoom") {
            // Garage/Dialog keep self; RaceSetup/City → track.nav.
            if (!args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
              const char* rhc = tree_host_class(args[0].v.o);
              if (rhc && (std::strstr(rhc, "Garage") ||
                          std::strstr(rhc, "Dialog") ||
                          std::strstr(rhc, "MainMenu")) &&
                  mname == "changeMode") {
                owner = rhc;
              } else if (!rhc || !std::strstr(rhc, "Navigator")) {
                if (!locals.empty() && locals[0].tag == JvmTag::Obj &&
                    locals[0].v.o) {
                  InvObject* o = locals[0].v.o;
                  InvObject* nav = tree_field_get_obj(o, "nav");
                  if (!nav) {
                    if (InvObject* track = tree_field_get_obj(o, "track"))
                      nav = tree_field_get_obj(track, "nav");
                  }
                  if (!nav) {
                    if (InvObject* last = tree_field_get_obj(o, "lastState"))
                      nav = tree_field_get_obj(last, "nav");
                  }
                  if (nav) {
                    args[0] = JvmValue::make_obj(nav);
                    owner = "java.game.Navigator";
                  }
                }
              } else {
                owner = "java.game.Navigator";
              }
            } else {
              owner = "java.game.Navigator";
            }
          }
        } else {
          for (int i = 0; i < argc; ++i) args.insert(args.begin(), pop());
        }

        // OptionsDialog Menu chain: createBG leaves Rectangle on stack; if
        // createMenu result was discarded, setSliderStyle/addItem steal it.
        if ((mname == "setSliderStyle" || mname == "addItem" ||
             mname == "addSeparator") &&
            last_menu && !args.empty()) {
          const char* rhc = tree_host_class(args[0].v.o);
          if (!rhc || !std::strstr(rhc, "Menu")) {
            args[0] = JvmValue::make_obj(last_menu);
            owner = "java.render.osd.Menu";
          }
        }
        if ((mname == "printValue" || mname == "setValue" ||
             mname == "setRange" || mname == "setTicks" ||
             mname == "setOffset" || mname == "setToolTip") &&
            last_menu_item && !args.empty()) {
          const char* rhc = tree_host_class(args[0].v.o);
          if (!rhc || (!std::strstr(rhc, "Slider") &&
                       !std::strstr(rhc, "MultiChoice") &&
                       !std::strstr(rhc, "Button") &&
                       !std::strstr(rhc, "Gadget"))) {
            args[0] = JvmValue::make_obj(last_menu_item);
            owner = rhc && rhc[0] ? rhc : "java.render.osd.Slider";
            const char* ihc = tree_host_class(last_menu_item);
            if (ihc && ihc[0]) owner = ihc;
          }
        }
        if (mname == "addCustomGroups" && !locals.empty() &&
            locals[0].tag == JvmTag::Obj && locals[0].v.o) {
          args.clear();
          args.push_back(locals[0]);
          const char* hc = tree_host_class(locals[0].v.o);
          owner = (hc && hc[0]) ? hc : "java.game.MainMenuDialog";
        }

        JvmValue r =
            host->call_by_name(owner.c_str(), mname.c_str(), args, is_static_call);
        if (mname == "createMenu" && r.tag == JvmTag::Obj && r.v.o)
          last_menu = r.v.o;
        if (mname == "addItem" && r.tag == JvmTag::Obj && r.v.o)
          last_menu_item = r.v.o;
        if (mname == "endGroup" && r.tag == JvmTag::Int)
          capture_endgroup_field(r.v.i, ip);
        // MainMenuDialog.osdCommand if/else chain often lacks a clean GOTO end
        // after changeActiveSection(garage) — stop before super.osdCommand.
        if (mname == "changeActiveSection" && !locals.empty() &&
            locals[0].tag == JvmTag::Obj && locals[0].v.o) {
          const char* shc = tree_host_class(locals[0].v.o);
          if (shc && std::strstr(shc, "MainMenuDialog")) {
            InvObject* cur = game_logic_actual_state();
            const char* cn = cur ? tree_host_class(cur) : nullptr;
            if (!cn || !std::strstr(cn, "MainMenu") ||
                main_menu_cmd_new_cas_pending() ||
                main_menu_cmd_freeride_cas_pending() ||
                main_menu_cmd_exit_cas_pending())
              return JvmValue::make_void();
          }
        }
        if (pending_local >= 0 && r.tag != JvmTag::Void) {
          ensure_local(static_cast<size_t>(pending_local));
          locals[static_cast<size_t>(pending_local)] = r;
          if (name_local < 0) name_local = pending_local;
          pending_local = -1;
        } else if (r.tag != JvmTag::Void) {
          // Expression results (loops / chaining). Statement calls discard
          // (stock TREE often omits POP after createBG/createHotkey).
          if (mname == "getFirstChild" || mname == "getNextChild" ||
              mname == "first" || mname == "next" || mname == "elementAt" ||
              mname == "size" || mname == "intValue" || mname == "exists" ||
              mname == "createButton" || mname == "createText" ||
              mname == "createTextBox" || mname == "createHotkey" ||
              mname == "createMenu" || mname == "beginGroup" ||
              mname == "endGroup" || mname == "nextToken" ||
              mname == "countTokens" || mname == "token" ||
              mname == "display" || mname == "autoSave" ||
              mname == "displayModeName" || mname == "numDisplayModes" ||
              mname == "currDisplayMode" || mname == "getVolume") {
            push(r);
          }
        }
        break;
      }
      case 0x14: {  // FIELD_REF_STATIC or relative GOTO when imm is negative
        if (n.has_imm && (n.imm & 0x80000000u)) {
          const int32_t rel = static_cast<int32_t>(n.imm);
          const int64_t next = static_cast<int64_t>(ip) + rel;
          if (next < 0 || next >= static_cast<int64_t>(tree.nodes.size())) {
            if (err) *err = "GOTO OOB";
            return JvmValue::make_void();
          }
          ip = static_cast<size_t>(next) - 1;  // compensated by loop ++
        }
        break;
      }
      case 0x15: {
        // IFEQ / relative forward jump when condition falsy.
        // With pending ICMP_EQ (0x07/7): compare top two stack values first
        // (SplashScreen.osdCommand: cmd == Input.AXIS_CANCEL).
        // With pending INSTANCEOF (0x07/32)+TYPE: Valocity.enter RaceSetup gate.
        JvmValue cond;
        bool from_stack = false;
        if (pending_binop && *pending_binop == 7 && stack.size() >= 2) {
          JvmValue b = pop();
          JvmValue a = pop();
          int eq = 0;
          if (a.tag == JvmTag::Obj || b.tag == JvmTag::Obj)
            eq = (a.v.o == b.v.o) ? 1 : 0;
          else if (a.tag == JvmTag::Float || b.tag == JvmTag::Float)
            eq = (a.v.f == b.v.f) ? 1 : 0;
          else
            eq = (a.v.i == b.v.i) ? 1 : 0;
          cond = JvmValue::make_int(eq);
          pending_binop.reset();
          from_stack = true;
        } else if (pending_binop && *pending_binop == 32 &&
                   !pending_class.empty()) {
          cond = JvmValue::make_int(eval_instanceof());
          pending_binop.reset();
          from_stack = true;
        } else if (!stack.empty()) {
          cond = pop();
          from_stack = true;
        } else {
          cond = !locals.empty() ? locals[0] : JvmValue::make_obj(nullptr);
        }
        // Jump when condition matches IFEQ polarity. Special case: after
        // Dialog.display(), stock Java is `if (display() == 0) { body }` but
        // TREE emits IFEQ that must fall through on 0 (OK) and skip on cancel.
        bool invert_display_eq0 = false;
        if (!pending_binop) {
          for (int k = 1; k <= 8 && ip >= static_cast<size_t>(k); ++k) {
            const TreeNode& pn = tree.nodes[ip - static_cast<size_t>(k)];
            if (pn.op == 0x07 || pn.op == 0x0b || pn.op == 0x06 ||
                pn.op == 0x01 || pn.op == 0x02 || pn.op == 0x03)
              continue;
            if ((pn.op == 0x1a || pn.op == 0x12 || pn.op == 0x25) &&
                pn.has_imm) {
              std::string mn = field_name(pn.imm);
              if (mn.empty()) mn = cstr(pn.imm);
              if (mn == "display") invert_display_eq0 = true;
            }
            // First meaningful prior op decides polarity; stop scanning.
            break;
          }
        }
        const bool take_jump =
            invert_display_eq0 ? truthy(cond) : !truthy(cond);
        if (take_jump && n.has_imm) {
          const int32_t rel = static_cast<int32_t>(n.imm);
          if (rel > 0) {
            const int64_t next = static_cast<int64_t>(ip) + rel;
            if (next >= 0 && next < static_cast<int64_t>(tree.nodes.size())) {
              ip = static_cast<size_t>(next) - 1;
            }
          } else if (n.imm < tree.nodes.size()) {
            ip = n.imm - 1;
          }
        } else if (from_stack && !take_jump) {
          // fall through; cond already consumed
        }
        break;
      }
      case 0x19: {  // JMP_EQ — peek stack (keep value for later use, e.g. first path)
        // Class hint in getstatic+NEWARRAY FILD pattern (skip invoke).
        if (pending_static_newarray) break;
        // getstatic sugar: … 0x11/0x19/0x1b Field (Frontend.mediumFont,
        // Osd.MD_HORIZONTAL). Must not treat spc=0 / other zeros as IFEQ —
        // Garage.createOSDObjects was jumping over createMenu.
        if (ip + 1 < tree.nodes.size() && tree.nodes[ip + 1].op == 0x1b)
          break;
        // Oversized imm = method-ref (Garage.exit SfxRef.play / Vehicle.command…).
        if (n.has_imm && n.imm >= tree.nodes.size()) {
          std::string mname = field_name(n.imm);
          if (mname.empty()) mname = cstr(n.imm);
          if (!mname.empty()) {
            int argc = 0;
            if (mname == "command" || mname == "play" || mname == "enable" ||
                mname == "activateState" || mname == "setSpeed" ||
                mname == "setFade" || mname == "seek")
              argc = 1;
            else if (mname == "giveWarning" || mname == "setMatrix")
              argc = 2;
            else if (mname == "queueEvent")
              argc = 3;
            else if (mname == "add" || mname == "mul" || mname == "sub")
              argc = 1;
            else if (mname == "create") {
              if ((!stack.empty() && stack.back().tag == JvmTag::Obj &&
                   is_tree_renderref(stack.back().v.o)) ||
                  (!locals.empty() && locals[0].tag == JvmTag::Obj &&
                   is_tree_renderref(locals[0].v.o)))
                argc = 3;
            }
            JvmValue recv = JvmValue::make_obj(nullptr);
            std::vector<JvmValue> args;
            if (mname == "enable") {
              JvmValue arg = JvmValue::make_int(0);
              if (stack.size() >= 2 && stack.back().tag == JvmTag::Int &&
                  stack[stack.size() - 2].tag == JvmTag::Obj) {
                arg = pop();
                recv = pop();
              } else if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                recv = pop();
                if (!stack.empty()) arg = pop();
              } else {
                recv = JvmValue::make_obj(java_io_Input_cursor());
                if (!stack.empty()) arg = pop();
              }
              args.push_back(recv);
              args.push_back(arg);
            } else if (mname == "queueEvent") {
              pack_queue_event(stack, locals, args, &recv);
            } else if (mname == "create" && argc == 3) {
              pack_renderref_create(stack, locals, args, &recv);
            } else if (mname == "add" || mname == "mul" || mname == "sub") {
              pack_vector3_binop(stack, locals, args, &recv);
            } else {
              if (!stack.empty() && stack.back().tag == JvmTag::Obj)
                recv = pop();
              else if (!locals.empty())
                recv = locals[0];
              args.push_back(recv);
              for (int i = 0; i < argc; ++i) {
                if (!stack.empty())
                  args.insert(args.begin() + 1, pop());
                else
                  args.insert(args.begin() + 1, JvmValue::make_int(0));
              }
            }
            const char* owner = cls_name.c_str();
            const char* hc = tree_host_class(recv.v.o);
            if (hc && hc[0]) owner = hc;
            if ((mname == "add" || mname == "mul" || mname == "sub") &&
                recv.tag == JvmTag::Obj && is_tree_vector3(recv.v.o))
              owner = "java.lang.Vector3";
            JvmValue r = host->call_by_name(owner, mname.c_str(), args, false);
            if (r.tag != JvmTag::Void) push(r);
          }
          break;
        }
        // Empty stack = statement epilogue after void calls (SplashScreen
        // createBG → 0x08/27 → 0x11 → 0x19), not a failed FindFile.first.
        if (stack.empty()) break;
        JvmValue cond = stack.back();
        if (!truthy(cond)) {
          pop();
          if (!n.has_imm || n.imm >= tree.nodes.size()) {
            if (err) *err = "JMP target OOB";
            return JvmValue::make_void();
          }
          if (n.imm > ip) {
            ip = n.imm - 1;
          }
        }
        break;
      }
      case 0x1a: {  // JMP_EQ2 — while end  OR  method-ref invoke (imm = const idx)
        // Method-ref when imm is outside the node table (preCacheGametypes), OR
        // when imm names a VT-ctor method that collides with a valid jump index
        // (Baiern_VT addElement imm=20 in a 2238-node tree). Never treat File.*
        // control-flow names (delete/copy/…) as methods here — those are JMPs.
        auto is_inline_mref = [](const std::string& m) {
          // GameRef.command strings ("filter 2") are the method name.
          if (m.find(' ') != std::string::npos) return true;
          return m == "addElement" || m == "elementAt" || m == "size" ||
                 m == "addColorIndex" || m == "intValue" || m == "createBG" ||
                 m == "createHotkey" || m == "show" || m == "hide" ||
                 m == "setEventMask" || m == "clearEventMask" ||
                 m == "addTimer" || m == "removeAllTimers" ||
                 m == "createOSDObjects" || m == "lockCar" ||
                 m == "releaseCar" || m == "changeMode" ||
                 m == "addSceneElements" || m == "remSceneElements" ||
                 m == "cameraSetup" || m == "giveWarning" || m == "endGroup" ||
                 m == "beginGroup" || m == "hideGroup" || m == "showGroup" ||
                 m == "display" || m == "filterInventory" ||
                 m == "flushInventory" || m == "play" || m == "enable" ||
                 m == "addHandler" || m == "remHandler" ||
                 m == "enableCameraControl" || m == "disableCameraControl" ||
                 m == "command" || m == "destroy" || m == "unload" ||
                 m == "getPos" || m == "getOri" || m == "reset" ||
                 m == "activateState" || m == "checkHint" || m == "neon" ||
                 m == "tesztmen" || m == "/" || m == "deleteOSDObjects" ||
                 m == "userWait" || m == "activate" || m == "cleanup" ||
                 m == "remNotification" || m == "flushInventory" ||
                 m == "enableOsd" || m == "removeAllElements" ||
                 m == "osdCommand" || m == "changePointer" || m == "flush" ||
                 m == "log" || m == "putMessage" || m == "setPriority" ||
                 m == "setPos" || m == "changeCamTarget" ||
                 m == "changeCamTarget2" || m == "setTransmission" ||
                 m == "leaveCar" || m == "constructName" ||
                 m == "setDefaultTransmission" ||
                 m == "setDefaultSteeringHelp" || m == "setDefaultASR" ||
                 m == "setDefaultABS" || m == "set" ||
                 m == "setDamageMultiplier" || m == "setCruiseControl" ||
                 m == "wakeUp" || m == "duplicate" || m == "createBox" ||
                 m == "createSphere" ||
                 m == "setMatrix" || m == "setSpeed" || m == "toString" || m == "id" ||
                 m == "scrollToLine" || m == "currentLine" ||
                 m == "createButton" || m == "createText" ||
                 m == "createTextBox" || m == "createHeader" ||
                 m == "createMenu" || m == "countTokens" ||
                 m == "nextToken" || m == "precache" || m == "timeWarp" ||
                 m == "addCustomGroups" || m == "addItem" ||
                 m == "addSeparator" || m == "setSliderStyle" ||
                 m == "enableAnimateHook" || m == "disableAnimateHook" ||
                 m == "printValue" || m == "setValue" || m == "token" ||
                 // OptionsDialog.show video-mode loop (else 0x1a → false JMP).
                 m == "numDisplayModes" || m == "currDisplayMode" ||
                 m == "displayModeName" || m == "changeVideoMode" ||
                 m == "startRace" || m == "haltTrafficCross" ||
                 m == "haltTrafficPath" || m == "setParent" ||
                 m == "getVel" || m == "createCar" ||
                 m == "createQuickRaceBot" || m == "addNotification" ||
                 m == "queueEvent" ||
                 m == "add" || m == "mul" || m == "sub" ||
                 m == "changeActiveSection";
        };
        const bool named_mref =
            n.has_imm && n.imm < const_mref_name.size() &&
            is_inline_mref(const_mref_name[n.imm]);
        if (named_mref || (n.has_imm && n.imm >= tree.nodes.size())) {
          std::string mname = field_name(n.imm);
          if (mname.empty()) mname = cstr(n.imm);
          if (!mname.empty()) {
            // NEW+ARRAY packing immediately before setEventMask/addTimer.
            if ((mname == "setEventMask" || mname == "clearEventMask" ||
                 mname == "addTimer" || mname == "show" || mname == "hide") &&
                ip > 0 && tree.nodes[ip - 1].op == 0x24 && !stack.empty() &&
                stack.back().tag == JvmTag::Obj) {
              pop();
            }

            // Instance call: TREE often leaves recv on top; JVM leaves args on
            // top. Prefer recv-on-top when top is Obj of a plausible owner.
            std::vector<JvmValue> args;
            int argc = 0;
            if (mname == "addElement" || mname == "elementAt" ||
                mname == "addColorIndex")
              argc = 1;
            else if (mname == "size" || mname == "intValue" ||
                     mname == "getFirstChild" || mname == "getNextChild" ||
                     mname == "cache" || mname == "show" || mname == "hide" ||
                     mname == "createOSDObjects" || mname == "lockCar" ||
                     mname == "releaseCar" || mname == "remSceneElements" ||
                     mname == "endGroup" || mname == "beginGroup" ||
                     mname == "display" || mname == "flushInventory" ||
                     mname == "wakeUp" || mname == "destroy" ||
                     mname == "unload" || mname == "getPos" ||
                     mname == "getOri" || mname == "reset" ||
                     mname == "disableCameraControl" || mname == "neon" ||
                     mname == "cleanup" || mname == "activate" ||
                     mname == "removeAllElements" ||
                     mname == "toString" || mname == "id" ||
                     mname == "currentLine" || mname == "countTokens" ||
                     mname == "precache" || mname == "addCustomGroups" ||
                     mname == "enableAnimateHook" ||
                     mname == "disableAnimateHook" || mname == "addSeparator" ||
                     mname == "flush" || mname == "changePointer" ||
                     mname == "tesztmen" || mname == "deleteOSDObjects")
              argc = 0;
            else if (mname == "setDefaultTransmission" ||
                     mname == "setDefaultSteeringHelp" ||
                     mname == "setDefaultASR" || mname == "setDefaultABS")
              argc = 0;
            else if (mname.find(' ') != std::string::npos)
              argc = 0;
            else if (mname == "createBG" || mname == "setEventMask" ||
                     mname == "clearEventMask" || mname == "changeMode" ||
                     mname == "addSceneElements" || mname == "cameraSetup" ||
                     mname == "enable" || mname == "addHandler" ||
                     mname == "remHandler" || mname == "enableCameraControl" ||
                     mname == "filterInventory" || mname == "scrollToLine" ||
                     mname == "command" || mname == "setDamageMultiplier" ||
                     mname == "setCruiseControl" || mname == "duplicate" ||
                     mname == "play" || mname == "activateState" ||
                     mname == "nextToken" || mname == "hideGroup" ||
                     mname == "showGroup" || mname == "printValue" ||
                     mname == "setValue" || mname == "token" ||
                     mname == "createHeader" || mname == "checkHint" ||
                     mname == "enableOsd" || mname == "activateState" ||
                     mname == "osdCommand" || mname == "setPriority" ||
                     mname == "putMessage" || mname == "log" ||
                     mname == "changeCamTarget" || mname == "changeCamTarget2")
              argc = 1;
            else if (mname == "setTransmission" || mname == "leaveCar" ||
                     mname == "constructName" || mname == "set") {
              if (stack.size() >= 2 &&
                  (stack[stack.size() - 2].tag == JvmTag::Int ||
                   stack[stack.size() - 2].tag == JvmTag::Float ||
                   stack[stack.size() - 2].tag == JvmTag::Obj))
                argc = 1;
              else
                argc = 0;
            }
            else if (mname == "setPos") {
              if (stack.size() >= 3 &&
                  (stack[stack.size() - 2].tag == JvmTag::Float ||
                   stack[stack.size() - 2].tag == JvmTag::Int) &&
                  (stack[stack.size() - 3].tag == JvmTag::Float ||
                   stack[stack.size() - 3].tag == JvmTag::Int))
                argc = 2;
              else if (stack.size() >= 2 &&
                       stack[stack.size() - 2].tag == JvmTag::Obj)
                argc = 1;
              else
                argc = 0;
            }
            else if (mname == "userWait") {
              argc = 0;
              if (stack.size() >= 2 &&
                  (stack[stack.size() - 2].tag == JvmTag::Float ||
                   stack[stack.size() - 2].tag == JvmTag::Int))
                argc = 1;
            }
            else if (mname == "setSliderStyle" || mname == "setRange" ||
                     mname == "remNotification")
              argc = 2;
            else if (mname == "addItem") {
              // Stock Menu.addItem(String,int)=2. Size heuristics stole the
              // prior addItem Button and dropped the label (cmd→0).
              int base = 0;
              if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                const char* hc = tree_host_class(stack.back().v.o);
                if (hc && std::strstr(hc, "Menu")) base = 1;
              }
              auto at = [&](int i) -> const JvmValue* {
                if (i < 0 || static_cast<size_t>(i) >= stack.size())
                  return nullptr;
                return &stack[stack.size() - 1 - static_cast<size_t>(i)];
              };
              int floats = 0;
              bool multi = false;
              bool icon_cmd = false;
              for (int i = base; i < base + 8; ++i) {
                const JvmValue* v = at(i);
                if (!v) break;
                if (v->tag == JvmTag::Float) ++floats;
                if (v->tag == JvmTag::Int && is_icon_osd_cmd(v->v.i))
                  icon_cmd = true;
                if (v->tag == JvmTag::Obj && v->v.o) {
                  const char* hc = tree_host_class(v->v.o);
                  if (hc && (std::strstr(hc, "Vector") || hc[0] == '['))
                    multi = true;
                }
              }
              if (multi)
                argc = 5;
              else if (icon_cmd && recent_new) {
                const char* rhc = tree_host_class(recent_new);
                if (rhc && (std::strstr(rhc, "ResourceRef") ||
                            std::strstr(rhc, "GameRef")))
                  argc = 3;
                else if (floats >= 3)
                  argc = 7;
                else if (floats >= 1)
                  argc = 4;
                else
                  argc = 3;
              } else if (floats >= 3)
                argc = 7;
              else if (floats >= 1)
                argc = 4;
              else {
                bool icon = false;
                int strs = 0, ints = 0;
                for (int i = base; i < base + 4; ++i) {
                  const JvmValue* v = at(i);
                  if (!v) break;
                  if (v->tag == JvmTag::Int) ++ints;
                  if (v->tag == JvmTag::Obj && v->v.o) {
                    const char* hc = tree_host_class(v->v.o);
                    if (hc && (std::strstr(hc, "ResourceRef") ||
                               std::strstr(hc, "GameRef")))
                      icon = true;
                    if (!hc || !hc[0] || std::strstr(hc, "String")) ++strs;
                  }
                }
                if (icon || (strs >= 2 && ints >= 1))
                  argc = 3;
                else
                  argc = 2;
              }
            }
            else if (mname == "createMenu") {
              // Same as argc_for_method: ignore leftover ResourceRefs on stack.
              int base = 0;
              if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                const char* hc = tree_host_class(stack.back().v.o);
                if (hc && std::strstr(hc, "Osd")) base = 1;
              }
              int n = 0;
              for (int i = base; i < base + 8 &&
                   static_cast<size_t>(i) < stack.size();
                   ++i) {
                const JvmValue& v = stack[stack.size() - 1 - static_cast<size_t>(i)];
                if (v.tag == JvmTag::Float || v.tag == JvmTag::Int) {
                  ++n;
                  continue;
                }
                if (v.tag == JvmTag::Obj && v.v.o) {
                  const char* hc = tree_host_class(v.v.o);
                  if (hc && (std::strstr(hc, "Style") ||
                             std::strstr(hc, "ResourceRef"))) {
                    ++n;
                    continue;
                  }
                }
                break;
              }
              argc = n >= 6 ? 6 : (n >= 4 ? n : 5);
            }
            else if (mname == "addTimer" || mname == "giveWarning" ||
                     mname == "setMatrix")
              argc = 2;
            else if (mname == "setSpeed" || mname == "setFade" ||
                     mname == "seek")
              argc = 1;
            else if (mname == "createText")
              argc = 5;
            else if (mname == "numDisplayModes" || mname == "currDisplayMode")
              argc = 0;
            else if (mname == "displayModeName")
              argc = 1;
            else if (mname == "changeVideoMode")
              argc = 3;
            else if (mname == "createButton") {
              argc = 5;
              // Shield: (style,x,y,pri,label,cmd) — pri+label+cmd on top.
              if (stack.size() >= 7 && stack.back().tag == JvmTag::Int &&
                  stack[stack.size() - 2].tag == JvmTag::Obj &&
                  stack[stack.size() - 3].tag == JvmTag::Int)
                argc = 6;
            }
            else if (mname == "createTextBox")
              argc = 6;
            else if (mname == "createBox")
              argc = 5;
            else if (mname == "createSphere")
              argc = 3;
            else if (mname == "createHotkey") {
              argc = 4;
              if (stack.size() >= 6 &&
                  stack[stack.size() - 2].tag == JvmTag::Int &&
                  stack[stack.size() - 3].tag == JvmTag::Obj)
                argc = 5;
            }
            else if (mname == "addTraffic" || mname == "addTrafficN")
              argc = 5;
            else if (mname == "queueEvent")
              argc = 3;
            else if (mname == "create") {
              if ((!stack.empty() && stack.back().tag == JvmTag::Obj &&
                   is_tree_renderref(stack.back().v.o)) ||
                  (!locals.empty() && locals[0].tag == JvmTag::Obj &&
                   is_tree_renderref(locals[0].v.o)))
                argc = 3;
            }
            else if (mname == "enter" || mname == "exit")
              argc = 1;
            else if (mname == "setPedestrianDensity" ||
                     mname == "setPedestrianDensityN" ||
                     mname == "addPedestrianType" ||
                     mname == "remPedestrianType" ||
                     mname == "remTrafficCar" || mname == "delTraffic")
              argc = (mname == "delTraffic") ? 0 : 1;
            else if (mname == "addTrafficCar" || mname == "notifyTrafficCar" || mname == "setTrafficCarBehaviour" ||
                     mname == "addWaterLimit")
              argc = 2;
            else if (mname == "getNearestCross") {
              if (stack.size() >= 2 &&
                  (stack.back().tag == JvmTag::Float ||
                   stack.back().tag == JvmTag::Int) &&
                  stack[stack.size() - 2].tag == JvmTag::Obj)
                argc = 2;
              else
                argc = 1;
            }
            else if (mname == "alignToRoad" || mname == "time2Config")
              argc = 1;
            else if (mname == "getStartDirection" ||
                     mname == "haltTrafficCross" || mname == "haltTrafficPath")
              argc = 2;
            else if (mname == "startRace")
              argc = 3;
            else if (mname == "mul" || mname == "add" || mname == "sub" ||
                     mname == "setParent")
              argc = 1;
            else if (mname == "diff")
              argc = 2;
            else if (mname == "getVel" || mname == "stop" ||
                     mname == "createQuickRaceBot")
              argc = 0;
            else if (mname == "createCar") {
              if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
                  stack[stack.size() - 2].tag == JvmTag::Obj)
                argc = 2;
              else
                argc = 1;
            }
            else if (mname == "getRouteLength") {
              if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
                  stack[stack.size() - 2].tag == JvmTag::Obj)
                argc = 2;
              else
                argc = 0;
            }
            else if (mname == "findRoute")
              argc = 2;
            else if (mname == "addMarker") {
              if (stack.size() >= 3 && stack.back().tag == JvmTag::Int &&
                  stack[stack.size() - 2].tag == JvmTag::Obj &&
                  stack[stack.size() - 3].tag == JvmTag::Obj)
                argc = 3;
              else if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
                       stack[stack.size() - 2].tag == JvmTag::Obj)
                argc = 2;
              else
                argc = 1;
            }
            else if (mname == "remMarker" || mname == "updateNavigator")
              argc = 1;
            else if (mname == "changeZoom")
              argc = 1;
            else if (mname == "changeSize")
              argc = 4;
            else if (mname == "setWater")
              argc = 4;
            else if (mname == "addTrigger")
              argc = 6;
            else if (mname == "addNotification") {
              argc = 4;
              if (stack.size() >= 5 && stack.back().tag == JvmTag::Obj) {
                const JvmValue& a2 = stack[stack.size() - 3];
                const JvmValue& a3 = stack[stack.size() - 4];
                if (a2.tag == JvmTag::Int && a3.tag == JvmTag::Int) argc = 5;
              }
            }

            JvmValue recv = JvmValue::make_obj(nullptr);
            // GfxEngine statics — OptionsDialog.show bounds videoModes loop.
            if (mname == "numDisplayModes" || mname == "currDisplayMode" ||
                mname == "displayModeName" || mname == "changeVideoMode") {
              std::vector<JvmValue> sargs;
              for (int i = 0; i < argc; ++i) {
                if (!stack.empty())
                  sargs.insert(sargs.begin(), pop());
                else
                  sargs.insert(sargs.begin(), JvmValue::make_int(0));
              }
              JvmValue r = host->call_by_name("java.render.GfxEngine",
                                              mname.c_str(), sargs, true);
              if (r.tag != JvmTag::Void) push(r);
              if (pending_local >= 0 && r.tag != JvmTag::Void) {
                ensure_local(static_cast<size_t>(pending_local));
                locals[static_cast<size_t>(pending_local)] = r;
                pending_local = -1;
              }
              break;
            }
            if (mname == "changeActiveSection") {
              // Stock GameLogic.changeActiveSection(state) — static. RaceSetup
              // osdCommand 0x1a otherwise binds Valocity argc=0 and skips CAS.
              JvmValue next = JvmValue::make_obj(nullptr);
              if (!stack.empty()) next = pop();
              if (next.tag != JvmTag::Obj || !game_logic_is_section(next.v.o)) {
                next = JvmValue::make_obj(nullptr);
                if (!locals.empty() && locals[0].tag == JvmTag::Obj) {
                  InvObject* o = locals[0].v.o;
                  const char* lc = tree_host_class(o);
                  // Only RaceSetup/Track `this.track` / lastState — not
                  // MainMenuDialog CMD_EXIT (explicit null → System.exit).
                  if (lc && (std::strstr(lc, "RaceSetup") ||
                             std::strstr(lc, "Valocity") ||
                             std::strstr(lc, "Track") ||
                             std::strstr(lc, "City"))) {
                    InvObject* t = tree_field_get_obj(o, "track");
                    if (!t) t = tree_field_get_obj(o, "lastState");
                    if (game_logic_is_section(t)) next = JvmValue::make_obj(t);
                  }
                }
              }
              host->call_by_name("java.game.GameLogic", "changeActiveSection",
                                 {next}, true);
              break;
            }
            // Input.cursor.enable(int): JVM order [MouseCursor, int] with int on
            // top. Generic recv-on-top would steal Dialog `this` and miss the call.
            if (mname == "enable") {
              JvmValue arg = JvmValue::make_int(0);
              if (stack.size() >= 2 && stack.back().tag == JvmTag::Int &&
                  stack[stack.size() - 2].tag == JvmTag::Obj) {
                arg = pop();
                recv = pop();
              } else if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                recv = pop();
                if (!stack.empty()) arg = pop();
              } else {
                recv = JvmValue::make_obj(java_io_Input_cursor());
                if (!stack.empty()) arg = pop();
              }
              args.push_back(recv);
              args.push_back(arg);
            } else if (mname == "createButton" || mname == "createText" ||
                       mname == "createTextBox" || mname == "createRectangle" ||
                       mname == "createHeader" || mname == "createMenu" ||
                       mname == "addItem" || mname == "addSeparator" ||
                       mname == "setSliderStyle") {
              // JVM order: [recv, args…] with last arg on top. For Menu.* the
              // receiver is often under args; fall back to last_menu.
              // Drop OSD leftovers (Rectangle/Button/Text) that createBG/
              // createHeader/addItem left when mis-kept on the stack.
              auto is_osd_junk = [&](const JvmValue& v) -> bool {
                if (v.tag != JvmTag::Obj || !v.v.o) return false;
                const char* hc = tree_host_class(v.v.o);
                if (!hc || !hc[0]) return false;  // bare String — keep
                return std::strstr(hc, "Rectangle") ||
                       std::strstr(hc, "Button") ||
                       std::strstr(hc, "Text") ||
                       std::strstr(hc, "Hotkey") ||
                       std::strstr(hc, "Group");
              };
              while (!stack.empty() && is_osd_junk(stack.back())) pop();

              if (mname == "addItem" && (argc == 2 || argc == 3)) {
                // Prefer real (String,int) or (ResourceRef,int,String): scan.
                JvmValue label = JvmValue::make_obj(nullptr);
                JvmValue tip = JvmValue::make_obj(nullptr);
                JvmValue gfx = JvmValue::make_obj(nullptr);
                JvmValue cmdv = JvmValue::make_int(0);
                bool have_cmd = false;
                int32_t gfx_rid = 0;
                for (int guard = 0; guard < 12 && !stack.empty(); ++guard) {
                  JvmValue v = pop();
                  if (v.tag == JvmTag::Int) {
                    if (v.v.i > 0x10000)
                      gfx_rid = v.v.i;
                    else if (is_icon_osd_cmd(v.v.i) || !have_cmd) {
                      cmdv = v;
                      have_cmd = true;
                    }
                  } else if (v.tag == JvmTag::Obj && v.v.o) {
                    const char* hc = tree_host_class(v.v.o);
                    if (hc && (std::strstr(hc, "ResourceRef") ||
                               std::strstr(hc, "GameRef"))) {
                      if (!gfx.v.o) gfx = v;
                    } else if (!hc || !hc[0] || std::strstr(hc, "String")) {
                      const char* sc = string_cstr(v.v.o);
                      if (sc && is_osd_invoke_name(sc)) {
                        /* skip method-name ldc */
                      } else if (!label.v.o)
                        label = v;
                      else if (!tip.v.o)
                        tip = v;
                    } else if (hc && std::strstr(hc, "Menu")) {
                      recv = v;
                    }
                  }
                  if (have_cmd && (gfx.v.o || label.v.o) && cmdv.v.i != 0)
                    break;
                }
                if (!gfx.v.o && recent_new) {
                  const char* rhc = tree_host_class(recent_new);
                  if (rhc && (std::strstr(rhc, "ResourceRef") ||
                              std::strstr(rhc, "GameRef"))) {
                    gfx = JvmValue::make_obj(recent_new);
                    recent_new = nullptr;
                  }
                }
                if (gfx.v.o && gfx_rid)
                  java_util_resource_ResourceRef_set(gfx.v.o, gfx_rid);
                if (recv.tag != JvmTag::Obj || !recv.v.o) {
                  if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                    const char* thc = tree_host_class(stack.back().v.o);
                    if (thc && std::strstr(thc, "Menu"))
                      recv = pop();
                  }
                  if ((recv.tag != JvmTag::Obj || !recv.v.o) && last_menu)
                    recv = JvmValue::make_obj(last_menu);
                }
                while (!stack.empty() && is_osd_junk(stack.back())) pop();
                while (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                  const char* hc = tree_host_class(stack.back().v.o);
                  if (hc && (std::strstr(hc, "ResourceRef") ||
                             std::strstr(hc, "GameRef"))) {
                    if (!gfx.v.o) gfx = pop();
                    else
                      pop();
                  } else if (hc && (std::strstr(hc, "Style") ||
                                    std::strstr(hc, "Button")))
                    pop();
                  else
                    break;
                }
                args.clear();
                if (gfx.v.o) {
                  args.push_back(gfx);
                  args.push_back(cmdv);
                  args.push_back(tip.v.o ? tip : label);
                } else {
                  args.push_back(label);
                  args.push_back(cmdv);
                  if (tip.v.o) args.push_back(tip);
                }
                args.insert(args.begin(), recv);
              } else {
                for (int i = 0; i < argc; ++i) {
                  while (!stack.empty() && is_osd_junk(stack.back())) pop();
                  if (!stack.empty())
                    args.insert(args.begin(), pop());
                  else
                    args.insert(args.begin(), JvmValue::make_int(0));
                }
                if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                  const char* thc = tree_host_class(stack.back().v.o);
                  if (mname == "addItem" || mname == "addSeparator" ||
                      mname == "setSliderStyle") {
                    if (thc && std::strstr(thc, "Menu"))
                      recv = pop();
                    else if (last_menu)
                      recv = JvmValue::make_obj(last_menu);
                    else
                      recv = last_menu ? JvmValue::make_obj(last_menu)
                                       : pop();
                  } else {
                    recv = pop();
                  }
                } else if (last_menu &&
                           (mname == "addItem" || mname == "addSeparator" ||
                            mname == "setSliderStyle")) {
                  recv = JvmValue::make_obj(last_menu);
                } else if (!locals.empty() && locals[0].tag == JvmTag::Obj) {
                  if (InvObject* osd =
                          tree_field_get_obj(locals[0].v.o, "osd"))
                    recv = JvmValue::make_obj(osd);
                  else
                    recv = locals[0];
                }
                while (!stack.empty() && is_osd_junk(stack.back())) pop();
                // Also drop stray Menu/Style duplicates.
                while (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                  const char* hc = tree_host_class(stack.back().v.o);
                  if (hc && (std::strstr(hc, "Button") ||
                             std::strstr(hc, "Menu") ||
                             std::strstr(hc, "Style")))
                    pop();
                  else
                    break;
                }
                args.insert(args.begin(), recv);
              }
            } else if (mname == "addTraffic" || mname == "addTrafficCar" ||
                       mname == "addTrafficN" || mname == "addTrafficP" ||
                       mname == "notifyTrafficCar" || mname == "setTrafficCarBehaviour" ||
                       mname == "remTrafficCar" ||
                       mname == "setPedestrianDensity" ||
                       mname == "setPedestrianDensityN" ||
                       mname == "addPedestrianType" ||
                       mname == "remPedestrianType" || mname == "setWater" ||
                       mname == "addWaterLimit" ||
                       mname == "getNearestCross" ||
                       mname == "getStartDirection" ||
                       mname == "getRouteLength" || mname == "findRoute" ||
                       mname == "haltTrafficCross" ||
                       mname == "haltTrafficPath" || mname == "alignToRoad" ||
                       mname == "delTraffic") {
              auto resolve_map = [&](InvObject* o) -> InvObject* {
                if (!o) return nullptr;
                if (InvObject* map = tree_field_get_obj(o, "map")) return map;
                if (InvObject* track = tree_field_get_obj(o, "track")) {
                  if (InvObject* map = tree_field_get_obj(track, "map"))
                    return map;
                }
                if (InvObject* last = tree_field_get_obj(o, "lastState")) {
                  if (InvObject* map = tree_field_get_obj(last, "map"))
                    return map;
                }
                return nullptr;
              };
              for (int i = 0; i < argc; ++i) {
                if (!stack.empty())
                  args.insert(args.begin(), pop());
                else
                  args.insert(args.begin(), JvmValue::make_int(0));
              }
              if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                const char* thc = tree_host_class(stack.back().v.o);
                if (thc && std::strstr(thc, "GroundRef"))
                  recv = pop();
              }
              if ((recv.tag != JvmTag::Obj || !recv.v.o) && !locals.empty() &&
                  locals[0].tag == JvmTag::Obj && locals[0].v.o) {
                if (InvObject* map = resolve_map(locals[0].v.o))
                  recv = JvmValue::make_obj(map);
                else
                  recv = locals[0];
              }
              args.insert(args.begin(), recv);
            } else if (mname == "addMarker" || mname == "remMarker" ||
                       mname == "updateNavigator" || mname == "changeMode" ||
                       mname == "changeSize" || mname == "changeZoom") {
              auto resolve_nav = [&](InvObject* o) -> InvObject* {
                if (!o) return nullptr;
                const char* ohc = tree_host_class(o);
                if (ohc && std::strstr(ohc, "Navigator")) return o;
                if (InvObject* nav = tree_field_get_obj(o, "nav")) return nav;
                if (InvObject* track = tree_field_get_obj(o, "track")) {
                  if (InvObject* nav = tree_field_get_obj(track, "nav"))
                    return nav;
                }
                if (InvObject* last = tree_field_get_obj(o, "lastState")) {
                  if (InvObject* nav = tree_field_get_obj(last, "nav"))
                    return nav;
                }
                return nullptr;
              };
              for (int i = 0; i < argc; ++i) {
                if (!stack.empty())
                  args.insert(args.begin(), pop());
                else
                  args.insert(args.begin(), JvmValue::make_int(0));
              }
              if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                const char* thc = tree_host_class(stack.back().v.o);
                if (thc && (std::strstr(thc, "Navigator") ||
                            std::strstr(thc, "Nav")))
                  recv = pop();
              }
              if ((recv.tag != JvmTag::Obj || !recv.v.o) && !locals.empty() &&
                  locals[0].tag == JvmTag::Obj && locals[0].v.o) {
                const char* lhc = tree_host_class(locals[0].v.o);
                const bool garage_or_dlg =
                    lhc && (std::strstr(lhc, "Garage") ||
                            std::strstr(lhc, "Dialog") ||
                            std::strstr(lhc, "MainMenu"));
                if (mname == "changeMode" && garage_or_dlg)
                  recv = locals[0];
                else if (InvObject* nav = resolve_nav(locals[0].v.o))
                  recv = JvmValue::make_obj(nav);
                else
                  recv = locals[0];
              }
              args.insert(args.begin(), recv);
            } else if (mname == "setEventMask" || mname == "clearEventMask" ||
                mname == "addTimer") {
              for (int i = 0; i < argc; ++i) {
                if (!stack.empty())
                  args.insert(args.begin(), pop());
                else
                  args.insert(args.begin(), JvmValue::make_int(0));
              }
              if (mname == "addTimer" && !args.empty() &&
                  args[0].tag == JvmTag::Int) {
                args[0] =
                    JvmValue::make_float(static_cast<float>(args[0].v.i));
              }
              recv = !locals.empty() ? locals[0] : JvmValue::make_obj(nullptr);
              args.insert(args.begin(), recv);
            } else if (mname == "queueEvent") {
              pack_queue_event(stack, locals, args, &recv);
            } else if (mname == "create" && argc == 3) {
              pack_renderref_create(stack, locals, args, &recv);
            } else if (mname == "add" || mname == "mul" || mname == "sub") {
              pack_vector3_binop(stack, locals, args, &recv);
            } else {
              // TREE convention: receiver on top (VT addElement, Osd.createBG…).
              auto is_self_method = [&](const std::string& m) {
                return m == "createOSDObjects" || m == "lockCar" ||
                       m == "releaseCar" ||
                       m == "addSceneElements" || m == "remSceneElements" ||
                       m == "cameraSetup" || m == "giveWarning" ||
                       m == "createQuickRaceBot" || m == "destroyRaceBot" ||
                       m == "startRace2";
              };
              if (is_self_method(mname) && !locals.empty() &&
                  locals[0].tag == JvmTag::Obj) {
                if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                    stack.back().v.o != locals[0].v.o)
                  pop();
                recv = locals[0];
              } else if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                recv = pop();
              } else if (!locals.empty()) {
                recv = locals[0];
              }
              args.push_back(recv);
              for (int i = 0; i < argc; ++i) {
                if (!stack.empty())
                  args.insert(args.begin() + 1, pop());
                else
                  args.insert(args.begin() + 1, JvmValue::make_int(0));
              }
            }

            const char* owner = cls_name.c_str();
            const char* hc = tree_host_class(recv.v.o);
            if (hc && hc[0]) owner = hc;
            if ((mname == "add" || mname == "mul" || mname == "sub") &&
                recv.tag == JvmTag::Obj && is_tree_vector3(recv.v.o))
              owner = "java.lang.Vector3";
            if (mname == "enable") owner = "java.io.MouseCursor";
            if (mname == "addElement" || mname == "elementAt" ||
                mname == "size") {
              if (!args.empty() && args[0].tag == JvmTag::Obj &&
                  !tree_vector_is(args[0].v.o) && args.size() >= 2 &&
                  args[1].tag == JvmTag::Obj && tree_vector_is(args[1].v.o)) {
                std::swap(args[0], args[1]);
              }
              owner = "java.util.Vector";
            }
            if ((mname == "createBG" || mname == "createHotkey" ||
                 mname == "createButton" || mname == "createText" ||
                 mname == "createTextBox" || mname == "createRectangle" ||
                 mname == "createHeader" || mname == "createMenu" ||
                 mname == "show" || mname == "hide")) {
              if (hc && std::strstr(hc, "Osd")) {
                owner = "java.render.Osd";
              } else if (!locals.empty() && locals[0].tag == JvmTag::Obj) {
                if (InvObject* osd =
                        tree_field_get_obj(locals[0].v.o, "osd")) {
                  args[0] = JvmValue::make_obj(osd);
                  owner = "java.render.Osd";
                }
              }
            }
            if (mname == "addTraffic" || mname == "addTrafficCar" ||
                mname == "addTrafficN" || mname == "addTrafficP" ||
                mname == "notifyTrafficCar" || mname == "setTrafficCarBehaviour" || mname == "remTrafficCar" ||
                mname == "setPedestrianDensity" ||
                mname == "setPedestrianDensityN" ||
                mname == "addPedestrianType" ||
                mname == "remPedestrianType" || mname == "setWater" ||
                mname == "addWaterLimit" || mname == "getNearestCross" ||
                mname == "getStartDirection" || mname == "getRouteLength" ||
                mname == "findRoute" || mname == "haltTrafficCross" ||
                mname == "haltTrafficPath" || mname == "alignToRoad" ||
                mname == "delTraffic") {
              owner = "java.util.resource.GroundRef";
              if (!args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
                const char* rhc = tree_host_class(args[0].v.o);
                if (!rhc || !std::strstr(rhc, "GroundRef")) {
                  if (!locals.empty() && locals[0].tag == JvmTag::Obj &&
                      locals[0].v.o) {
                    InvObject* o = locals[0].v.o;
                    InvObject* map = tree_field_get_obj(o, "map");
                    if (!map) {
                      if (InvObject* track = tree_field_get_obj(o, "track"))
                        map = tree_field_get_obj(track, "map");
                    }
                    if (!map) {
                      if (InvObject* last = tree_field_get_obj(o, "lastState"))
                        map = tree_field_get_obj(last, "map");
                    }
                    if (map) args[0] = JvmValue::make_obj(map);
                  }
                }
              }
            }
            if (mname == "addMarker" || mname == "remMarker" ||
                mname == "updateNavigator" || mname == "changeMode" ||
                mname == "changeSize" || mname == "changeZoom") {
              if (!args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
                const char* rhc = tree_host_class(args[0].v.o);
                if (rhc && (std::strstr(rhc, "Garage") ||
                            std::strstr(rhc, "Dialog") ||
                            std::strstr(rhc, "MainMenu")) &&
                    mname == "changeMode") {
                  owner = rhc;
                } else if (!rhc || !std::strstr(rhc, "Navigator")) {
                  if (!locals.empty() && locals[0].tag == JvmTag::Obj &&
                      locals[0].v.o) {
                    InvObject* o = locals[0].v.o;
                    InvObject* nav = tree_field_get_obj(o, "nav");
                    if (!nav) {
                      if (InvObject* track = tree_field_get_obj(o, "track"))
                        nav = tree_field_get_obj(track, "nav");
                    }
                    if (!nav) {
                      if (InvObject* last = tree_field_get_obj(o, "lastState"))
                        nav = tree_field_get_obj(last, "nav");
                    }
                    if (nav) {
                      args[0] = JvmValue::make_obj(nav);
                      owner = "java.game.Navigator";
                    }
                  }
                } else {
                  owner = "java.game.Navigator";
                }
              } else {
                owner = "java.game.Navigator";
              }
            }

            if ((mname == "setSliderStyle" || mname == "addItem" ||
                 mname == "addSeparator") &&
                last_menu && !args.empty()) {
              const char* rhc = tree_host_class(args[0].v.o);
              if (!rhc || !std::strstr(rhc, "Menu")) {
                args[0] = JvmValue::make_obj(last_menu);
                owner = "java.render.osd.Menu";
              }
            }
            if (mname == "addCustomGroups" && !locals.empty() &&
                locals[0].tag == JvmTag::Obj && locals[0].v.o) {
              args.clear();
              args.push_back(locals[0]);
              const char* hc = tree_host_class(locals[0].v.o);
              owner = (hc && hc[0]) ? hc : "java.game.MainMenuDialog";
            }

            JvmValue r =
                host->call_by_name(owner, mname.c_str(), args, false);
            if (mname == "createMenu" && r.tag == JvmTag::Obj && r.v.o)
              last_menu = r.v.o;
            if (mname == "addItem" && r.tag == JvmTag::Obj && r.v.o)
              last_menu_item = r.v.o;
            if (mname == "endGroup" && r.tag == JvmTag::Int)
              capture_endgroup_field(r.v.i, ip);
            if (mname == "changeActiveSection" && !locals.empty() &&
                locals[0].tag == JvmTag::Obj && locals[0].v.o) {
              const char* shc = tree_host_class(locals[0].v.o);
              if (shc && std::strstr(shc, "MainMenuDialog")) {
                InvObject* cur = game_logic_actual_state();
                const char* cn = cur ? tree_host_class(cur) : nullptr;
                if (!cn || !std::strstr(cn, "MainMenu") ||
                    main_menu_cmd_new_cas_pending() ||
                    main_menu_cmd_freeride_cas_pending() ||
                    main_menu_cmd_exit_cas_pending())
                  return JvmValue::make_void();
              }
            }
            if (r.tag != JvmTag::Void) {
              bool keep =
                  pending_local >= 0 || mname == "getFirstChild" ||
                  mname == "getNextChild" || mname == "elementAt" ||
                  mname == "size" || mname == "intValue" ||
                  mname == "enable" || mname == "createMenu" ||
                  mname == "beginGroup" || mname == "endGroup" ||
                  mname == "token" || mname == "numDisplayModes" ||
                  mname == "currDisplayMode" || mname == "displayModeName" ||
                  mname == "autoSave";
              // display(): push only when used in if(display()==0). Statement
              // calls (PlayerSetupDialog.display()) must not leave 0 for a
              // later IFEQ or CMD_NEW skips loadDefaults/CAS(garage).
              if (mname == "display") {
                // StringRequester: if (display()==0). PlayerSetup: statement.
                keep = false;
                if (!args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
                  InvObject* d = args[0].v.o;
                  const char* dhc = tree_host_class(d);
                  if (InvObject* hint = tree_field_get_obj(d, "host_class_hint"))
                    if (const char* hs = string_cstr(hint))
                      if (hs && hs[0]) dhc = hs;
                  if (dhc && std::strstr(dhc, "StringRequester")) keep = true;
                }
              }
              // createBG/Header/addItem returns only when assigned; keep-push
              // left Rectangle/Button on stack and poisoned the next addItem.
              if (keep) push(r);
              if (mname == "getFirstChild" || mname == "getNextChild") {
                if (!locals.empty()) locals[0] = r;
              }
            }
            if (pending_local >= 0 && r.tag != JvmTag::Void) {
              ensure_local(static_cast<size_t>(pending_local));
              locals[static_cast<size_t>(pending_local)] = r;
              pending_local = -1;
            }
          }
          break;
        }
        JvmValue cond;
        const int ni = name_local >= 0 ? name_local : 3;
        if (ni >= 0 && static_cast<size_t>(ni) < locals.size() &&
            locals[static_cast<size_t>(ni)].tag == JvmTag::Obj) {
          cond = locals[static_cast<size_t>(ni)];
        } else {
          cond = pop();
        }
        // Two codegen shapes:
        //  A) target is exit/cleanup → jump when falsy (File.delete)
        //  B) target is continue (has relative GOTO) → jump when truthy (File.copy)
        bool target_continue = false;
        if (n.has_imm && n.imm < tree.nodes.size()) {
          for (size_t j = n.imm; j < tree.nodes.size() && j < n.imm + 5; ++j) {
            if (tree.nodes[j].op == 0x14 && tree.nodes[j].has_imm &&
                (tree.nodes[j].imm & 0x80000000u)) {
              target_continue = true;
              break;
            }
          }
        }
        const bool do_jump = target_continue ? truthy(cond) : !truthy(cond);
        if (do_jump) {
          if (!n.has_imm || n.imm >= tree.nodes.size()) {
            if (err) *err = "JMP target OOB";
            return JvmValue::make_void();
          }
          // Ignore degenerate targets that point at/before this JMP (rpkScan epilogue).
          if (!target_continue && n.imm <= ip) break;
          ip = n.imm - 1;
        } else if (target_continue) {
          // Falsy: skip continue block, find close() sugar (NEW+ARRAY+INVOKESPECIAL).
          for (size_t j = ip + 1; j < tree.nodes.size(); ++j) {
            if (tree.nodes[j].op != 0x27 || j + 2 >= tree.nodes.size()) continue;
            if (tree.nodes[j + 1].op != 0x24 || tree.nodes[j + 2].op != 0x11)
              continue;
            bool rel = false;
            for (size_t k = j + 3; k < tree.nodes.size() && k < j + 12; ++k) {
              if (tree.nodes[k].op == 0x14 && tree.nodes[k].has_imm &&
                  (tree.nodes[k].imm & 0x80000000u)) {
                rel = true;
                break;
              }
              if (tree.nodes[k].op == 0x2b) break;
            }
            if (!rel) {
              ip = j - 1;
              break;
            }
          }
        }
        break;
      }
      case 0x1b: {
        std::string fname = field_name(n.has_imm ? n.imm : 0);
        // FILD getstatic+NEWARRAY: Class.Field size (GameLogic.CLUBS).
        // Pool imm may be a sig/nat slot — fall back to CLUBS when in pattern.
        if (pending_static_newarray) {
          if (fname.empty() || fname[0] == '(' || fname.find('.') != std::string::npos) {
            fname = "CLUBS";
          }
          const int32_t vs = static_vs(fname);
          if (vs >= 0) {
            push(JvmValue::make_int(vs));
            break;
          }
          if (fname == "CLUBS") {
            push(JvmValue::make_int(3));
            break;
          }
        }
        if (ip + 1 < tree.nodes.size()) {
          const TreeNode& next = tree.nodes[ip + 1];
          if (next.op == 0x28) {
            InvObject* self = nullptr;
            if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
              self = pop().v.o;
            } else if (last_loaded_local >= 0 &&
                       static_cast<size_t>(last_loaded_local) < locals.size() &&
                       locals[static_cast<size_t>(last_loaded_local)].tag ==
                           JvmTag::Obj) {
              self = locals[static_cast<size_t>(last_loaded_local)].v.o;
            } else if (!locals.empty()) {
              self = locals[0].v.o;
            }
            JvmValue* slot = field_slot(self, fname, true);
            push(slot ? *slot : JvmValue::make_int(0));
            ++ip;
            pending_binop.reset();
            break;
          }
          if (next.op == 0x08 && next.has_imm && next.imm == 35) {
            // After NEW+0x21, 0x1B is a type/field hint before <init> — skip it.
            if (ip > 0 && tree.nodes[ip - 1].op == 0x21) {
              pending_putfield = fname;
              break;
            }
            // PUTFIELD: prefer JVM [objectref, value] when under is `this`.
            InvObject* self = nullptr;
            JvmValue val = JvmValue::make_int(0);
            if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
                stack[stack.size() - 2].tag == JvmTag::Obj && !locals.empty() &&
                locals[0].tag == JvmTag::Obj &&
                stack[stack.size() - 2].v.o == locals[0].v.o) {
              val = pop();
              self = pop().v.o;
            } else if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                       (stack.size() < 2 ||
                        stack[stack.size() - 2].tag != JvmTag::Obj)) {
              // Single obj on stack = value; target is this.
              val = pop();
              self = locals.empty() ? nullptr : locals[0].v.o;
            } else if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
              // TREE order [value, object] with object on top.
              self = pop().v.o;
              val = pop();
            } else {
              val = pop();
              if (last_loaded_local >= 0 &&
                  static_cast<size_t>(last_loaded_local) < locals.size() &&
                  locals[static_cast<size_t>(last_loaded_local)].tag ==
                      JvmTag::Obj) {
                self = locals[static_cast<size_t>(last_loaded_local)].v.o;
              } else if (!locals.empty()) {
                self = locals[0].v.o;
              }
            }
            if (JvmValue* slot = field_slot(self, fname, true)) *slot = val;
            if (fname == "played" && val.tag == JvmTag::Int)
              game_logic_set_played(val.v.i);
            if (fname == "gameMode" && val.tag == JvmTag::Int)
              game_logic_set_game_mode(val.v.i);
            if (fname == "timeout" && val.tag == JvmTag::Int)
              game_logic_set_timeout(val.v.i);
            if ((fname == "carrerInProgress" || fname == "careerInProgress") &&
                val.tag == JvmTag::Int)
              game_logic_set_career_in_progress(val.v.i);
            if (fname == "racesetup" && val.tag == JvmTag::Obj)
              game_logic_set_racesetup(val.v.o);
            ++ip;
            if (ip + 1 < tree.nodes.size() && tree.nodes[ip + 1].op == 0x29) ++ip;
            break;
          }
        }
        // Bare 0x1B: getstatic or getfield.
        const int32_t vs = static_vs(fname);
        if (vs >= 0) {
          push(JvmValue::make_int(vs));
          break;
        }
        const int32_t cc = static_rid_carcolor(fname);
        if (cc >= 0) {
          push(JvmValue::make_int(cc));
          break;
        }
        if (fname.rfind("qm_", 0) == 0) {
          push(JvmValue::make_float(static_qm(fname)));
          break;
        }
        if (fname == "RID_GENERALBG" || fname == "RRT_HEADERBG" ||
            fname == "RRT_TEST" || fname == "RRT_NONE" ||
            fname == "RRT_DARKEN" || fname == "RRT_TOOLTIP") {
          // OptionsDialog / Osd ResourceRef statics (frontend locals).
          uint16_t local = 0;
          if (fname == "RID_GENERALBG") local = 0x0016;
          else if (fname == "RRT_HEADERBG") local = 0x0028;
          else if (fname == "RRT_TEST" || fname == "RRT_NONE") local = 0x0018;
          else if (fname == "RRT_DARKEN") local = 0x0019;
          else if (fname == "RRT_TOOLTIP") local = 0x001A;
          int32_t rid = 0;
          if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
            rid = rpak_make_id(fe->pack_id, local);
          else if (const RpakPack* fe = rpak_find_by_name("frontend"))
            rid = rpak_make_id(fe->pack_id, local);
          InvObject* pic = gameref_new();
          if (rid) java_util_resource_ResourceRef_set(pic, rid);
          push(JvmValue::make_obj(pic));
          break;
        }
        if (fname.rfind("RID_", 0) == 0) {
          // Other GameLogic.RID_* ints — unresolved → 0 is acceptable for now.
          push(JvmValue::make_int(0));
          break;
        }
        if (fname == "FILES_ONLY")
          push(JvmValue::make_int(1));
        else if (fname == "DIRS_ONLY")
          push(JvmValue::make_int(2));
        else if (fname == "FILES_DIRS")
          push(JvmValue::make_int(0));
        else if (fname == "EVENT_TIME")
          push(JvmValue::make_int(0x00000080));
        else if (fname == "EVENT_TRIGGER_ON")
          push(JvmValue::make_int(0x00000020));
        else if (fname == "EVENT_TRIGGER_OFF")
          push(JvmValue::make_int(0x00000040));
        else if (fname == "EVENT_COMMAND")
          push(JvmValue::make_int(0x00000010));
        else if (fname == "EVENT_SAME")
          push(JvmValue::make_int(0));
        else if (fname == "EVENT_COLLISION")
          push(JvmValue::make_int(0x00000001));
        else if (fname == "EVENT_ANY")
          push(JvmValue::make_int(0x0FFFFFFF));
        else if (fname == "EVENT_CURSOR")
          push(JvmValue::make_int(0x00010000));
        else if (fname == "EVENT_HOTKEY")
          push(JvmValue::make_int(0x00000002));
        else if (fname == "AXIS_CANCEL")
          push(JvmValue::make_int(35));
        else if (fname == "VIRTUAL")
          push(JvmValue::make_int(1));
        else if (fname == "HK_STATIC")
          push(JvmValue::make_int(0x04));
        else if (fname == "loadingScreen") {
          // Frontend.loadingScreen (static) — no Frontend.class in install.
          push(JvmValue::make_obj(frontend_loading_screen()));
        }
        else if (fname == "render") {
          // Frontend.render.wait() only — never hijack player.render getfield.
          bool next_wait = false;
          if (ip + 1 < tree.nodes.size()) {
            const TreeNode& peek = tree.nodes[ip + 1];
            if (peek.has_imm &&
                (peek.op == 0x10 || peek.op == 0x11 || peek.op == 0x12 ||
                 peek.op == 0x1a) &&
                peek.imm < const_mref_name.size() &&
                const_mref_name[peek.imm] == "wait") {
              next_wait = true;
            }
          }
          if (next_wait) {
            push(JvmValue::make_obj(frontend_gfx_engine()));
            break;
          }
          // Fall through to instance getfield (player.render, …).
          InvObject* self = nullptr;
          if (!stack.empty() && stack.back().tag == JvmTag::Obj)
            self = pop().v.o;
          else if (!locals.empty())
            self = locals[0].v.o;
          if (JvmValue* slot = field_slot(self, fname, true))
            push(*slot);
          else
            push(JvmValue::make_int(0));
        }
        else if (fname == "player") {
          push(JvmValue::make_obj(game_logic_player()));
        }
        else if (fname == "garage") {
          push(JvmValue::make_obj(game_logic_garage()));
        }
        else if (fname == "racesetup") {
          push(JvmValue::make_obj(game_logic_racesetup()));
        }
        else if (fname == "CLUBS") {
          push(JvmValue::make_int(3));
        }
        else if (fname == "cursor") {
          // Input.cursor static MouseCursor
          push(JvmValue::make_obj(java_io_Input_cursor()));
        }
        else if (fname == "MUSIC_SET_GARAGE") {
          push(JvmValue::make_int(0));
        }
        else if (fname == "MUSIC_SET_MENU") {
          push(JvmValue::make_int(1));
        }
        else if (fname == "IL_NONE") {
          push(JvmValue::make_int(0));
        }
        else if (fname == "OF_MODAL") {
          push(JvmValue::make_int(0x01));
        }
        else if (fname == "ALIGN_CENTER") {
          push(JvmValue::make_int(1));
        }
        else if (fname == "ALIGN_LEFT") {
          // Text.java: ALIGN_LEFT=2, ALIGN_RIGHT=0, ALIGN_CENTER=1
          push(JvmValue::make_int(2));
        }
        else if (fname == "ALIGN_RIGHT") {
          push(JvmValue::make_int(0));
        }
        else if (fname == "DF_MODAL") {
          push(JvmValue::make_int(0x00000001));
        }
        else if (fname == "DF_DEFAULTBG") {
          push(JvmValue::make_int(0x00000004));
        }
        else if (fname == "SIF_NOEMPTY") {
          push(JvmValue::make_int(0x00001000));
        }
        else if (fname == "DF_DARKEN") {
          push(JvmValue::make_int(0x00000100));
        }
        else if (fname == "DF_SILENT") {
          push(JvmValue::make_int(0x00000200));
        }
        else if (fname == "DF_MODAL") {
          push(JvmValue::make_int(0x00000001));
        }
        else if (fname == "DF_FULLSCREEN") {
          push(JvmValue::make_int(0x00000002));
        }
        else if (fname == "DF_DEFAULTBG") {
          push(JvmValue::make_int(0x00000004));
        }
        else if (fname == "DF_FREEZE") {
          push(JvmValue::make_int(0x00000008));
        }
        else if (fname == "DF_LEAVEPOINTER") {
          push(JvmValue::make_int(0x00000010));
        }
        else if (fname == "DF_LOWPRI") {
          push(JvmValue::make_int(0x00000020));
        }
        else if (fname == "DF_HIGHPRI") {
          push(JvmValue::make_int(0x00000040));
        }
        else if (fname == "CHANNEL_EFFECTS") {
          push(JvmValue::make_int(0));
        }
        else if (fname == "CHANNEL_MUSIC") {
          push(JvmValue::make_int(1));
        }
        else if (fname == "CHANNEL_ENGINE") {
          push(JvmValue::make_int(2));
        }
        else if (fname == "RID_SLD_BACK") {
          int32_t rid = 0;
          if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
            rid = rpak_make_id(fe->pack_id, 0x0089);
          else if (const RpakPack* fe = rpak_find_by_name("frontend"))
            rid = rpak_make_id(fe->pack_id, 0x0089);
          push(JvmValue::make_int(rid));
        }
        else if (fname == "RID_SLD_KNOB") {
          int32_t rid = 0;
          if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
            rid = rpak_make_id(fe->pack_id, 0x0091);
          else if (const RpakPack* fe = rpak_find_by_name("frontend"))
            rid = rpak_make_id(fe->pack_id, 0x0091);
          push(JvmValue::make_int(rid));
        }
        else if (fname == "HIGH") {
          push(JvmValue::make_int(2));
        }
        else if (fname == "MID") {
          push(JvmValue::make_int(1));
        }
        else if (fname == "LOW") {
          push(JvmValue::make_int(0));
        }
        else if (fname == "OFF") {
          push(JvmValue::make_int(-1));
        }
        else if (fname == "texture_size_high") {
          push(JvmValue::make_int(0));
        }
        else if (fname == "texture_size_mid") {
          push(JvmValue::make_int(2));
        }
        else if (fname == "texture_size_low") {
          push(JvmValue::make_int(3));
        }
        else if (fname == "shadow_detail_high") {
          push(JvmValue::make_float(0.1f));
        }
        else if (fname == "shadow_detail_mid") {
          push(JvmValue::make_float(0.25f));
        }
        else if (fname == "shadow_detail_low") {
          push(JvmValue::make_float(0.5f));
        }
        else if (fname == "shadow_detail_off") {
          push(JvmValue::make_float(10.f));
        }
        else if (fname == "NCONTROLS") {
          push(JvmValue::make_int(25));
        }
        else if (fname == "largeFont") {
          push(JvmValue::make_obj(frontend_large_font()));
        }
        else if (fname == "mediumFont") {
          push(JvmValue::make_obj(frontend_medium_font()));
        }
        else if (fname == "smallFont") {
          push(JvmValue::make_obj(frontend_small_font()));
        }
        else if (fname == "pointers") {
          push(JvmValue::make_obj(frontend_pointers()));
        }
        else if (fname == "defLoadingPic") {
          push(JvmValue::make_obj(frontend_def_loading_pic()));
        }
        else if (fname == "inputQueue") {
          push(JvmValue::make_obj(frontend_input_queue()));
        }
        else if (fname == "RRT_DARKEN" || fname == "RRT_TEST" ||
                 fname == "RRT_NONE" || fname == "RRT_EMPTY" ||
                 fname == "RRT_HEADERBG" || fname == "RRT_TOOLTIP") {
          push(JvmValue::make_obj(
              tree_host_new("java.util.resource.ResourceRef")));
        }
        else if (fname == "RID_OK" || fname == "RID_CANCEL" ||
                 fname == "RID_BACK" || fname == "RID_EXIT" ||
                 fname == "RID_ARROWLF" || fname == "RID_ARROWRG" ||
                 fname == "RID_ARROWUP" || fname == "RID_ARROWDN" ||
                 fname == "RID_SLD_BACK" || fname == "RID_SLD_KNOB" ||
                 fname == "RID_GHOSTBUTTON") {
          // Osd.java: RID_* are frontend packed ints, not ResourceRef.
          int32_t local = 0;
          if (fname == "RID_OK") local = 0x0010;
          else if (fname == "RID_CANCEL") local = 0x001D;
          else if (fname == "RID_BACK") local = 0x0126;
          else if (fname == "RID_EXIT") local = 0x0135;
          else if (fname == "RID_ARROWUP") local = 0x004C;
          else if (fname == "RID_ARROWDN") local = 0x0084;
          else if (fname == "RID_ARROWLF") local = 0x0085;
          else if (fname == "RID_ARROWRG") local = 0x0088;
          else if (fname == "RID_SLD_BACK") local = 0x0089;
          else if (fname == "RID_SLD_KNOB") local = 0x0091;
          else if (fname == "RID_GHOSTBUTTON") local = 0x0038;
          int32_t rid = local;
          if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
            rid = rpak_make_id(fe->pack_id, local & 0xFFFF);
          else if (const RpakPack* fe = rpak_find_by_name("frontend"))
            rid = rpak_make_id(fe->pack_id, local & 0xFFFF);
          push(JvmValue::make_int(rid));
        }
        else if (fname == "MD_HORIZONTAL") {
          push(JvmValue::make_int(1));
        }
        else if (fname == "MD_VERTICAL") {
          push(JvmValue::make_int(0));
        }
        else if (fname == "AXIS_SELECT" || fname == "AXIS_MENU_LEFT" ||
                 fname == "AXIS_MENU_RIGHT" || fname == "AXIS_MENU_UP" ||
                 fname == "AXIS_MENU_DOWN") {
          push(JvmValue::make_int(1));
        }
        else if (fname == "MAX_PRIORITY") {
          push(JvmValue::make_int(10));
        }
        else if (fname == "RID_DIALOGBG") {
          int32_t rid = 0;
          if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
            rid = rpak_make_id(fe->pack_id, 0x000E);
          else if (const RpakPack* fe = rpak_find_by_name("frontend"))
            rid = rpak_make_id(fe->pack_id, 0x000E);
          push(JvmValue::make_int(rid));
        }
        else if (fname.rfind("CMD_", 0) == 0) {
          // Static final command ids (OptionsDialog / MainMenuDialog). Must not
          // fall through to getfield — that pops the String label for addItem.
          static const std::unordered_map<std::string, int32_t> kCmd = {
              // OptionsDialog
              {"CMD_OPTIONS", 0},
              {"CMD_VIDEO_OPTIONS", 1},
              {"CMD_CONTROL_OPTIONS", 2},
              {"CMD_SOUND_OPTIONS", 3},
              {"CMD_REDEFINE_CONTROLS", 4},
              {"CMD_MAIN", 5},
              {"CMD_REDEFINE_DONE", 6},
              {"CMD_RESET_CONTROLS", 7},
              {"CMD_LOAD_CONTROLS", 8},
              {"CMD_SAVE_CONTROLS", 9},
              {"CMD_RESOLUTION", 10},
              {"CMD_VIDEO_OPTIONS_DONE", 11},
              {"CMD_TEXTURE_DETAIL", 12},
              {"CMD_SHADOW_DETAIL", 13},
              {"CMD_VIEW_RANGE", 14},
              {"CMD_EFFECTS_VOL", 15},
              {"CMD_MUSIC_VOL", 16},
              {"CMD_ENGINE_VOL", 17},
              {"CMD_GAME_OPTIONS", 18},
              {"CMD_DIFFICULTY", 19},
              {"CMD_TRAFFICDENSITY", 20},
              {"CMD_PEDESTRIANDENSITY", 21},
              {"CMD_CLUTCH", 22},
              {"CMD_STEERHELP", 23},
              {"CMD_GAME_OPTIONS_DONE", 24},
              {"CMD_MOUSE_SENS", 25},
              {"CMD_REDEFINE_CONTROLS2", 26},
              {"CMD_REDEFINE_CONTROLS3", 27},
              {"CMD_METRIC", 28},
              {"CMD_GAMMA", 29},
              {"CMD_OBJECT_DETAIL", 30},
              {"CMD_LOD_DETAIL", 31},
              {"CMD_ABS_SLIDER", 32},
              {"CMD_ASR_SLIDER", 33},
              {"CMD_GPSMODE", 34},
              {"CMD_AXISCHECK", 35},
              {"CMD_AXISCHECK_DONE", 36},
              {"CMD_UNUSED", 37},
              {"CMD_SOUND_3D", 38},
              {"CMD_SOUND_HW", 39},
              {"CMD_GAME_OPTIONS2", 40},
              {"CMD_HMF_1", 41},
              {"CMD_HMF_2", 42},
              {"CMD_HMF_3", 43},
              {"CMD_PARTICLE", 44},
              {"CMD_HEADLIGHTS", 45},
              {"CMD_FLARES", 46},
              {"CMD_GETKEY", 100},
              {"CMD_DEAD_ZONE", 200},
              {"CMD_POWER", 300},
              {"CMD_FFB", 400},
              // MainMenuDialog
              {"CMD_NEW", 50},
              {"CMD_EXIT", 51},
              {"CMD_LOAD", 52},
              {"CMD_BACKTOGARAGE", 53},
              {"CMD_DELETE", 54},
              {"CMD_QUICKRACE", 55},
              {"CMD_CREDITS", 56},
              {"CMD_CREDITS_DONE", 57},
              {"CMD_COMPILEFILES", 58},
              {"CMD_DEMO", 59},
              {"CMD_LOADCAR", 60},
              {"CMD_SAVECAR", 61},
              {"CMD_FREERIDE", 62},
              {"CMD_NEXT", 100},
              {"CMD_PREV", 101},
              {"CMD_HNEXT", 102},
              {"CMD_HPREV", 103},
              // Garage
              {"CMD_NONE", 100},
              {"CMD_MAINMENU", 101},
              {"CMD_MENU", 107},
              {"CMD_ROC", 108},
              {"CMD_HITTHESTREET", 109},
              {"CMD_TESTTRACK", 110},
              {"CMD_CARLOT", 111},
              {"CMD_BUYCARS", 112},
              {"CMD_CATALOG", 113},
              {"CMD_CLUBINFO", 114},
              {"CMD_CARINFO", 115},
              {"CMD_TIME", 116},
              {"CMD_MECHANIC", 117},
              {"CMD_PAINT", 118},
              {"CMD_ESCAPE", 119},
              {"CMD_ROCRACE", 120},
              {"CMD_ROCTEST", 121},
              {"CMD_BUYCARSUSED", 122},
              {"CMD_TEST", 123},
              {"CMD_TUNE", 124},
              {"CMD_ROCINFO", 125},
              {"CMD_ROCQUIT", 126},
              {"CMD_CHEATMONEY", 127},
              {"CMD_BEGIN_ROC", 128},
              // RaceSetup.createOSDObjects
              {"CMD_RACE", 0},
              {"CMD_ABANDON", 1},
              {"CMD_ZOOM_IN", 2},
              {"CMD_ZOOM_OUT", 3},
              {"CMD_LESS_MONEY", 4},
              {"CMD_MORE_MONEY", 5},
              {"CMD_PRIZE", 6},
          };
          auto it = kCmd.find(fname);
          push(JvmValue::make_int(it != kCmd.end() ? it->second : 0));
        }
        else if (fname.rfind("SFX_", 0) == 0 || fname == "RID_CAMERA") {
          push(JvmValue::make_int(0));
        }
        else if (fname == "EVENT_COMMAND") {
          push(JvmValue::make_int(0x00000004));
        }
        else if (fname == "MENUSET") {
          push(JvmValue::make_int(2));
        }
        else if (fname == "played") {
          push(JvmValue::make_int(game_logic_played()));
        }
        else if (fname == "speedymen" || fname == "CARCOLORS") {
          static InvObject* g_empty_vec = nullptr;
          if (!g_empty_vec) g_empty_vec = tree_vector_new();
          push(JvmValue::make_obj(g_empty_vec));
        }
        else if (fname == "GM_CARREER" || fname == "GM_CAREER") {
          push(JvmValue::make_int(1));
        }
        else if (fname == "GM_DEMO") {
          push(JvmValue::make_int(5));
        }
        else if (fname == "GM_FREERIDE") {
          // Stock GameLogic.java: FREERIDE=2, QUICKRACE=3
          push(JvmValue::make_int(2));
        }
        else if (fname == "GM_QUICKRACE") {
          push(JvmValue::make_int(3));
        }
        else if (fname == "GM_SINGLECAR") {
          push(JvmValue::make_int(4));
        }
        else if (fname == "gameMode") {
          push(JvmValue::make_int(game_logic_game_mode()));
        }
        else if (fname == "timeout") {
          push(JvmValue::make_int(game_logic_timeout()));
        }
        else if (fname == "carrerInProgress" || fname == "careerInProgress") {
          push(JvmValue::make_int(game_logic_career_in_progress()));
        }
        else if (fname == "texture_size" || fname == "shadow_size" ||
                 fname == "shadows" || fname == "shadow_detail" ||
                 fname == "video_windowed" || fname == "video_x" ||
                 fname == "video_y" || fname == "video_depth" ||
                 fname == "video_gamma" || fname == "flares" ||
                 fname == "headlight_rays" || fname == "object_detail" ||
                 fname == "object_detail_amp" || fname == "particle_density" ||
                 fname == "camera_ext_viewrange" ||
                 fname == "camera_int_viewrange" ||
                 fname == "trafficDensity" || fname == "pedestrianDensity" ||
                 fname == "mouseSensitivity" || fname == "FFB_strength" ||
                 fname == "FFB_strength_emulated" || fname == "metricSystem" ||
                 fname == "gpsMode" || fname == "player_transmission" ||
                 fname == "player_steeringhelp" || fname == "player_abs" ||
                 fname == "player_asr" || fname == "deformation" ||
                 fname == "internal_damage" ||
                 fname == "player_damage_multiplier" ||
                 fname == "head_move_steer" || fname == "head_move_vel" ||
                 fname == "head_move_acc" || fname == "Sound_Mix_HW" ||
                 fname == "Sound_3D_HW" || fname == "version") {
          // Config.* getstatic → host Config mirror (OptionsDialog.show).
          InvObject* cfg = system_config_host();
          JvmValue* slot = field_slot(cfg, fname, true);
          if (slot)
            push(*slot);
          else
            push(JvmValue::make_int(0));
        }
        else {
          InvObject* self = nullptr;
          const bool field_invoke =
              ip + 1 < tree.nodes.size() && tree.nodes[ip + 1].op == 0x1a &&
              tree.nodes[ip + 1].has_imm &&
              tree.nodes[ip + 1].imm < const_mref_name.size() &&
              (const_mref_name[tree.nodes[ip + 1].imm] == "addElement" ||
               const_mref_name[tree.nodes[ip + 1].imm] == "elementAt" ||
               const_mref_name[tree.nodes[ip + 1].imm] == "size");
          if (field_invoke) {
            // vtdarr.addElement(x): keep x on stack, field is on this.
            if (!locals.empty()) self = locals[0].v.o;
          } else if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
            self = pop().v.o;
          } else if (!locals.empty()) {
            self = locals[0].v.o;
          }
          JvmValue* slot = field_slot(self, fname, true);
          if (fname == "length" && self && g_vectors.count(self)) {
            push(JvmValue::make_int(tree_vector_size(self)));
          } else if (slot)
            push(*slot);
          else
            push(JvmValue::make_int(0));
        }
        break;
      }
      case 0x1c: {
        // GETFIELD (quick) — or PUTFIELD when followed by 0x08/35
        // (Vector: elementData = new Object[n]).
        std::string fname = field_name(n.has_imm ? n.imm : 0);
        if (ip + 1 < tree.nodes.size() && tree.nodes[ip + 1].op == 0x08 &&
            tree.nodes[ip + 1].has_imm && tree.nodes[ip + 1].imm == 35) {
          InvObject* self = nullptr;
          JvmValue val = JvmValue::make_int(0);
          if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj) {
            // Common TREE order [value, object] with object on top.
            self = pop().v.o;
            val = pop();
          } else if (!stack.empty()) {
            val = pop();
            self = (!locals.empty() && locals[0].tag == JvmTag::Obj)
                       ? locals[0].v.o
                       : nullptr;
          }
          if (JvmValue* slot = field_slot(self, fname, true)) *slot = val;
          ++ip;
          break;
        }
        InvObject* self = nullptr;
        if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
          self = pop().v.o;
        } else if (!locals.empty()) {
          self = locals[0].v.o;
        }
        if (fname == "length" && self && g_vectors.count(self)) {
          push(JvmValue::make_int(tree_vector_size(self)));
        } else {
          JvmValue* slot = field_slot(self, fname, true);
          push(slot ? *slot : JvmValue::make_int(0));
        }
        break;
      }
      case 0x21: {  // store last_new into pending_local
        if (pending_local >= 0 && last_new) {
          ensure_local(static_cast<size_t>(pending_local));
          locals[static_cast<size_t>(pending_local)] =
              JvmValue::make_obj(last_new);
          recent_new = last_new;
          // NEW+store+hint+<init>: leave instance on stack for INVOKESPECIAL.
          if (ip + 2 < tree.nodes.size() && tree.nodes[ip + 1].op == 0x1b &&
              tree.nodes[ip + 2].op == 0x08 && tree.nodes[ip + 2].has_imm &&
              tree.nodes[ip + 2].imm == 35) {
            push(JvmValue::make_obj(last_new));
          }
          // Garage icon ResourceRef: apply packed rid without disturbing tip/CMD.
          else if (!pending_class.empty() &&
                   pending_class.find("ResourceRef") != std::string::npos &&
                   !stack.empty() && stack.back().tag == JvmTag::Int) {
            const int32_t rid = stack.back().v.i;
            if (rid > 0x10000) {
              pop();
              java_util_resource_ResourceRef_set(last_new, rid);
            }
          }
          if (pending_class == "java.io.FindFile" || ff_local < 0)
            ff_local = pending_local;
          pending_local = -1;
          last_new = nullptr;
        }
        break;
      }
      case 0x22:  // this(...) — other <init> overload
        pending_special_init = 1;
        break;
      case 0x23:  // super(...)
        pending_special_init = 2;
        break;
      case 0x24:  // ARRAY_INIT — part of concat/new sugar
        break;
      case 0x25:  // INVOKESPECIAL / instance call by name (SfxRef.precache)
      case 0x26: {  // INVOKESPECIAL super.method (TextDialog → Dialog.show)
        // Phase 2.134: TextDialog.show encodes `super.show()` as 0x26+name;
        // without this, Dialog.show (createButton) never runs.
        std::string mname;
        if (n.has_imm) {
          mname = field_name(n.imm);
          if (mname.empty()) mname = cstr(n.imm);
        }
        if (mname.empty() || mname.find('.') != std::string::npos) break;

        std::string owner = cls_name;
        if (n.op == 0x26 && !super_name.empty()) owner = super_name;

        // Drop NEW packing sugar left for the call (TextDialog: NEW then 0x26).
        // queueEvent(null, EVENT_COMMAND, String): String often has empty
        // host-class — do not drop it (or aconst_null ro) as packing Object.
        if (mname != "queueEvent" && mname != "create" && !stack.empty() &&
            stack.back().tag == JvmTag::Obj && !locals.empty() &&
            locals[0].tag == JvmTag::Obj &&
            stack.back().v.o != locals[0].v.o) {
          const char* phc = tree_host_class(stack.back().v.o);
          // Keep a real Osd/SfxRef recv on top; drop anonymous packing Object.
          if (!phc || !phc[0] || std::strcmp(phc, "java.lang.Object") == 0)
            pop();
        }

        int argc = 0;
        if (mname == "precache" || mname == "play" || mname == "show" ||
            mname == "hide" || mname == "reset" || mname == "nofocus" ||
            mname == "display" || mname == "loopPlay" || mname == "pause")
          argc = 0;
        else if (mname == "enable" || mname == "activateState" ||
                 mname == "enter" || mname == "exit" || mname == "setSpeed" ||
                 mname == "setFade" || mname == "seek")
          argc = 1;
        else if (mname == "setMatrix")
          argc = 2;
        else if (mname == "addTraffic" || mname == "addTrafficN")
          argc = 5;
        else if (mname == "queueEvent")
          argc = 3;
        else if (mname == "create") {
          if ((!stack.empty() && stack.back().tag == JvmTag::Obj &&
               is_tree_renderref(stack.back().v.o)) ||
              (!locals.empty() && locals[0].tag == JvmTag::Obj &&
               is_tree_renderref(locals[0].v.o)) ||
              pending_class.find("RenderRef") != std::string::npos)
            argc = 3;
        }
        else if (mname == "getNearestCross") {
          if (stack.size() >= 2 &&
              (stack.back().tag == JvmTag::Float ||
               stack.back().tag == JvmTag::Int) &&
              stack[stack.size() - 2].tag == JvmTag::Obj)
            argc = 2;
          else
            argc = 1;
        } else if (mname == "alignToRoad" || mname == "time2Config")
          argc = 1;
        else if (mname == "getStartDirection" || mname == "findRoute" ||
                   mname == "haltTrafficCross" || mname == "haltTrafficPath")
          argc = 2;
        else if (mname == "startRace")
          argc = 3;
        else if (mname == "<init>") {
          // Trigger(map,null,pos,alias) / GameRef(parent,rid,params,alias).
          // this() inside Trigger.<init> has empty pending_class — use cls_name.
          const bool trig =
              pending_class.find("Trigger") != std::string::npos ||
              cls_name.find("Trigger") != std::string::npos;
          const bool rref =
              (pending_class.find("RenderRef") != std::string::npos ||
               cls_name.find("RenderRef") != std::string::npos) &&
              pending_class.find("Camera") == std::string::npos &&
              cls_name.find("Camera") == std::string::npos;
          const bool gref =
              pending_class.find("GameRef") != std::string::npos ||
              cls_name.find("GameRef") != std::string::npos;
          const bool pcar =
              pending_class.find("ParkingCar") != std::string::npos ||
              cls_name.find("ParkingCar") != std::string::npos;
          if (trig) {
            argc = 4;
            for (size_t i = 0; i < stack.size() && i < 6; ++i) {
              if (stack[stack.size() - 1 - i].tag == JvmTag::Float) {
                argc = 5;
                break;
              }
            }
          } else if (pcar) {
            argc = 5;
          } else if (rref) {
            argc = 3;
            if (stack.size() < 3) argc = 1;
          } else if (gref) {
            argc = 4;
            if (stack.size() < 4) argc = 1;
          }
        }
        else if (mname == "mul" || mname == "add" || mname == "sub" ||
                 mname == "setParent")
          argc = 1;
        else if (mname == "createCar") {
          if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
              stack[stack.size() - 2].tag == JvmTag::Obj)
            argc = 2;
          else
            argc = 1;
        }
        else if (mname == "getRouteLength") {
          if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
              stack[stack.size() - 2].tag == JvmTag::Obj)
            argc = 2;
          else
            argc = 0;
        } else if (mname == "addTrafficCar" || mname == "notifyTrafficCar" || mname == "setTrafficCarBehaviour")
          argc = 2;
        else if (mname == "addMarker") {
          if (stack.size() >= 3 && stack.back().tag == JvmTag::Int &&
              stack[stack.size() - 2].tag == JvmTag::Obj &&
              stack[stack.size() - 3].tag == JvmTag::Obj)
            argc = 3;
          else if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
                   stack[stack.size() - 2].tag == JvmTag::Obj)
            argc = 2;
          else
            argc = 1;
        } else if (mname == "remMarker" || mname == "updateNavigator")
          argc = 1;
        else if (mname == "plotRoute")
          argc = 5;
        else if (mname == "timeWarp")
          argc = 1;
        else if (mname == "changeActiveSection")
          argc = 1;

        JvmValue recv = JvmValue::make_obj(nullptr);
        if (n.op == 0x26) {
          recv = !locals.empty() ? locals[0] : JvmValue::make_obj(nullptr);
        } else if (mname == "queueEvent") {
          // Mixed (null, I, String) — pack_queue_event pops recv itself.
        } else if (mname == "create") {
          // Mixed (parent, type, alias) — pack_renderref_create pops recv.
        } else if (mname == "add" || mname == "mul" || mname == "sub") {
          // Vector3.add — pack_vector3_binop pops recv (skip ResourceRef junk).
        } else if (mname == "<init>" && n.op == 0x25 &&
                   cls_name.find("Trigger") != std::string::npos &&
                   pending_class.find("GameRef") == std::string::npos &&
                   pending_class.find("RenderRef") == std::string::npos) {
          // this(parent,type,pos[,r],alias) — String alias is on top, not recv.
          recv = !locals.empty() ? locals[0] : JvmValue::make_obj(nullptr);
          if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
              recv.tag == JvmTag::Obj && stack.back().v.o == recv.v.o)
            pop();
        } else if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
          recv = pop();
        } else if (recent_new) {
          recv = JvmValue::make_obj(recent_new);
        } else if (last_new) {
          recv = JvmValue::make_obj(last_new);
        } else if (!locals.empty()) {
          recv = locals[0];
        }

        // RaceSetup.enter: map/nav/line calls often leave RaceSetup as recv.
        if (recv.tag == JvmTag::Obj && recv.v.o) {
          const char* rhc = tree_host_class(recv.v.o);
          if (rhc && std::strstr(rhc, "RaceSetup")) {
            InvObject* track = tree_field_get_obj(recv.v.o, "track");
            if (!track) track = tree_field_get_obj(recv.v.o, "lastState");
            if (mname == "getNearestCross" || mname == "getStartDirection" ||
                mname == "getRouteLength" || mname == "findRoute" ||
                mname == "haltTrafficCross" || mname == "haltTrafficPath" ||
                mname == "alignToRoad") {
              InvObject* map =
                  track ? tree_field_get_obj(track, "map") : nullptr;
              if (map) {
                recv = JvmValue::make_obj(map);
                owner = "java.util.resource.GroundRef";
              }
            } else if (mname == "startRace") {
              if (track) {
                recv = JvmValue::make_obj(track);
                owner = tree_host_class(track) ? tree_host_class(track)
                                               : "java.game.City";
              }
            } else if (mname == "addMarker" || mname == "remMarker" ||
                       mname == "updateNavigator" || mname == "changeMode" ||
                       mname == "changeSize" || mname == "changeZoom") {
              InvObject* nav =
                  track ? tree_field_get_obj(track, "nav") : nullptr;
              if (nav) {
                recv = JvmValue::make_obj(nav);
                owner = "java.game.Navigator";
              }
            } else if (mname == "plotRoute") {
              InvObject* nav =
                  track ? tree_field_get_obj(track, "nav") : nullptr;
              InvObject* line = nav ? tree_field_get_obj(nav, "route") : nullptr;
              if (!line && recent_new) line = recent_new;
              if (!line && last_new) line = last_new;
              if (line) {
                recv = JvmValue::make_obj(line);
                owner = "java.util.resource.RenderRef";
              }
            }
          }
        }

        std::vector<JvmValue> args;
        if (mname == "queueEvent") {
          pack_queue_event(stack, locals, args, &recv);
        } else if (mname == "create" && argc == 3) {
          pack_renderref_create(stack, locals, args, &recv);
        } else if (mname == "add" || mname == "mul" || mname == "sub") {
          pack_vector3_binop(stack, locals, args, &recv);
        } else {
          args.push_back(recv);
          for (int i = 0; i < argc; ++i) {
            if (!stack.empty())
              args.push_back(pop());
            else
              args.push_back(JvmValue::make_int(0));
          }
        }
        const char* hc = tree_host_class(recv.v.o);
        if (n.op != 0x26 && hc && hc[0] &&
            owner.find("RaceSetup") == std::string::npos &&
            owner != "java.util.resource.GroundRef" &&
            owner != "java.game.Navigator" &&
            owner != "java.util.resource.RenderRef")
          owner = hc;
        if ((mname == "add" || mname == "mul" || mname == "sub") &&
            recv.tag == JvmTag::Obj && is_tree_vector3(recv.v.o))
          owner = "java.lang.Vector3";

        if (mname == "changeActiveSection") {
          JvmValue next = JvmValue::make_obj(nullptr);
          for (auto it = args.rbegin(); it != args.rend(); ++it) {
            if (it->tag == JvmTag::Obj && game_logic_is_section(it->v.o)) {
              next = *it;
              break;
            }
          }
          JvmValue r = host->call_by_name("java.game.GameLogic",
                                          "changeActiveSection", {next}, true);
          if (r.tag != JvmTag::Void) push(r);
          break;
        }

        JvmValue r =
            host->call_by_name(owner.c_str(), mname.c_str(), args, false);
        // PlayerSetupDialog.display is 0x25 — discard return (statement).
        if (mname != "display" && r.tag != JvmTag::Void) push(r);
        break;
      }
      case 0x27: {  // NEW
        if (pending_concat) break;  // concat path uses NEW as sugar
        const char* cn =
            pending_class.empty() ? "java.lang.Object" : pending_class.c_str();
        last_new = host->new_instance(cn);
        if (pending_local >= 0) {
          ensure_local(static_cast<size_t>(pending_local));
          locals[static_cast<size_t>(pending_local)] =
              JvmValue::make_obj(last_new);
          if (pending_class == "java.io.FindFile") ff_local = pending_local;
        } else {
          push(JvmValue::make_obj(last_new));
        }
        break;
      }
      case 0x28: {
        if (pending_binop && *pending_binop == 7) {
          JvmValue b = pop();
          JvmValue a = pop();
          int eq = 0;
          if (a.tag == JvmTag::Obj || b.tag == JvmTag::Obj)
            eq = (a.v.o == b.v.o) ? 1 : 0;
          else if (a.tag == JvmTag::Float || b.tag == JvmTag::Float)
            eq = (a.v.f == b.v.f) ? 1 : 0;
          else
            eq = (a.v.i == b.v.i) ? 1 : 0;
          push(JvmValue::make_int(eq));
          pending_binop.reset();
        }
        break;
      }
      case 0x29:
        pop();
        break;
      case 0x20: {
        // AALOAD — array[index]; or AASTORE setup when followed by 0x08/35
        // (Valocity: posGarage[i] = new Vector3(...); Vector: elementData[i]=null).
        if (ip + 1 < tree.nodes.size() && tree.nodes[ip + 1].op == 0x08 &&
            tree.nodes[ip + 1].has_imm && tree.nodes[ip + 1].imm == 35) {
          JvmValue arrv = pop();
          JvmValue idxv = pop();
          pending_aastore_arr =
              (arrv.tag == JvmTag::Obj) ? arrv.v.o : nullptr;
          pending_aastore_idx =
              (idxv.tag == JvmTag::Int)
                  ? idxv.v.i
                  : (idxv.tag == JvmTag::Float ? static_cast<int32_t>(idxv.v.f)
                                               : 0);
          pending_aastore = true;
          break;
        }
        JvmValue idxv = pop();
        JvmValue arrv = pop();
        const int32_t idx = idxv.tag == JvmTag::Int ? idxv.v.i : 0;
        InvObject* elem = nullptr;
        if (arrv.tag == JvmTag::Obj && arrv.v.o)
          elem = tree_vector_element_at(arrv.v.o, idx);
        push(JvmValue::make_obj(elem));
        break;
      }
      case 0x2c:
        // TYPE-declare marker (File.delete / for-loop / checkcast sugar).
        // Value is stored later via 0x2d or NEW+0x21; keep pending_local.
        break;
      case 0x2b:
        if (stack.empty()) return JvmValue::make_void();
        return stack.back();
      case 0x2d:  // DUP2 — also commits stack top into pending_local
        if (pending_local >= 0 && !stack.empty()) {
          ensure_local(static_cast<size_t>(pending_local));
          locals[static_cast<size_t>(pending_local)] = stack.back();
          if (name_local < 0) name_local = pending_local;
          pending_local = -1;
        }
        break;
      case 0x2f: {  // IADD
        JvmValue b = pop();
        JvmValue a = pop();
        const int32_t sum = a.v.i + b.v.i;
        // for (i=0; i<N; i++) often compiles as i = i + 1 without a clean
        // istore — write back when +1 matches the last-loaded local.
        if (last_loaded_local >= 0 &&
            ((b.tag == JvmTag::Int && b.v.i == 1) ||
             (a.tag == JvmTag::Int && a.v.i == 1))) {
          ensure_local(static_cast<size_t>(last_loaded_local));
          JvmValue& loc = locals[static_cast<size_t>(last_loaded_local)];
          const int32_t base = (b.tag == JvmTag::Int && b.v.i == 1) ? a.v.i
                                                                   : b.v.i;
          if (loc.tag == JvmTag::Int && loc.v.i == base) {
            loc.v.i = sum;
          }
        }
        push(JvmValue::make_int(sum));
        break;
      }
      case 0x30: {  // FADD
        JvmValue b = pop();
        JvmValue a = pop();
        float af = a.tag == JvmTag::Float ? a.v.f : static_cast<float>(a.v.i);
        float bf = b.tag == JvmTag::Float ? b.v.f : static_cast<float>(b.v.i);
        const float sum = af + bf;
        // OptionsDialog keyText loop: y += 0.12
        if (last_loaded_local >= 0) {
          ensure_local(static_cast<size_t>(last_loaded_local));
          JvmValue& loc = locals[static_cast<size_t>(last_loaded_local)];
          if (loc.tag == JvmTag::Float &&
              (std::fabs(loc.v.f - af) < 1e-6f ||
               std::fabs(loc.v.f - bf) < 1e-6f)) {
            loc.v.f = sum;
          }
        }
        push(JvmValue::make_float(sum));
        break;
      }
      case 0x36: {  // IOR (guess — vehicleSetMask |= …)
        JvmValue b = pop();
        JvmValue a = pop();
        push(JvmValue::make_int(a.v.i | b.v.i));
        break;
      }
      default:
        break;
    }
  }

  if (!stack.empty()) return stack.back();
  return JvmValue::make_void();
}

}  // namespace inv
