#include "tree_interp.hpp"
#include "tree_interp_internal.hpp"
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

std::unordered_map<InvObject*, TreeFieldMap> g_tree_fields;
std::unordered_map<InvObject*, std::vector<InvObject*>> g_tree_vectors;
std::unordered_map<InvObject*, std::string> g_tree_host_class;
std::unordered_map<std::string, TreeStaticBag> g_tree_statics;

JvmValue* tree_field_slot(InvObject* obj, const std::string& name, bool create) {
  // PE field lookup chain (VMThread_run op29 @ 0x4214BC / typed JNI helpers):
  //   Object_getField @ 0x408800 → Class_getFieldByName @ 0x404820
  //     → FieldTable_lookupByName @ 0x403840 (strcmp entry+0xC; miss→0)
  //   fallback instance table: FieldTable_lookupByName(obj+0x10, name)
  // Miss returns 0 — never allocate. Soft get must use create=false so
  // g_tree_fields[obj] operator[] does not invent an empty TreeFieldMap either.
  // Soft put (create=true) invents a bag slot — PE put requires a pre-existing
  // Value* (JVM_vm_set_int_field @ 0x42A9E0 / set_float @ 0x42A040).
  if (!obj || name.empty()) return nullptr;
  if (!create) {
    auto fit = g_tree_fields.find(obj);
    if (fit == g_tree_fields.end()) return nullptr;
    auto it = fit->second.by_name.find(name);
    if (it == fit->second.by_name.end()) return nullptr;
    return &it->second;
  }
  auto& m = g_tree_fields[obj].by_name;
  auto it = m.find(name);
  if (it == m.end())
    it = m.emplace(name, JvmValue::make_int(0)).first;
  return &it->second;
}

namespace {

// PE Class_ensureInitialized @ 0x004040CE builds every static ValueField with
// payload 0 (@ 0x4040E7 v23[2] = 0) and the type descriptor interned from
// Utf8(decl+0xC) (@ 0x4040E2). Thread_evalName_fieldPath @ 0x00420E06 reads
// that descriptor's first byte and treats 'L' / '[' as the reference forms, so
// the tag follows the same first byte here.
JvmValue tree_static_zero(const JvmFieldDecl& fd) {
  const char t = fd.type.empty() ? 'I' : fd.type[0];
  if (t == 'F' || t == 'D') return JvmValue::make_float(0.f);
  if (t == 'L' || t == '[') return JvmValue::make_obj(nullptr);
  return JvmValue::make_int(0);
}

// Soft stand-in for the "<static field init>" CallFrame @ 0x004042D3: the PE
// runs decl+0x10 (JvmFieldDecl::tree_index) on a THRD-CLASSVAR-INI VMThread
// and Value_assignOp('#') the popped operand into the ValueField. Running a
// tree from here is OOS, so only the static finals already recovered from the
// stock sources are seeded; everything else keeps the payload-0 default.
JvmValue tree_static_seed(const JvmFieldDecl& fd) {
  const int32_t vs = tree_static_vs(fd.name);
  if (vs >= 0) return JvmValue::make_int(vs);
  const int32_t cc = tree_static_rid_carcolor(fd.name);
  if (cc >= 0) return JvmValue::make_int(cc);
  if (fd.name.rfind("qm_", 0) == 0)
    return JvmValue::make_float(tree_static_qm(fd.name));
  return tree_static_zero(fd);
}

}  // namespace

// PE Class_ensureInitialized @ 0x00403E80. Guards on class+0x1D0: non-null →
// return immediately, so the body runs once per Class. It mallocs a 20-byte
// Instance (Instance_ctor @ 0x004036B0, PtrVec at instance+0xC) into
// class+0x1D0, then walks the STATIC decl vector at classdesc+0x54 in order
// and appends one ValueField per entry via Instance_addField @ 0x004038C0.
// Soft: one bag per class FQN, seeded with the payload-0 / known-final value.
// Supers keep their own bag — the PE recurses ensureInitialized on
// class+0x1C8 but never copies inherited statics into the child Instance;
// Class_findFieldSlot @ 0x00405690 walks the chain and names the owner.
void tree_static_ensure_initialized(const JvmClass* owner) {
  if (!owner || owner->name.empty()) return;
  if (g_tree_statics.find(owner->name) != g_tree_statics.end())
    return;  // PE class+0x1D0 != 0 @ 0x403E91
  TreeStaticBag& bag = g_tree_statics[owner->name];
  for (const JvmFieldDecl& fd : owner->static_fields) {
    // PE Instance_addField returns 0 on a name already in the PtrVec and does
    // NOT append (Class_ensureInitialized only logs "member redefinition"
    // @ 0x404152), so the duplicate consumes no slot.
    if (!fd.name.empty() && bag.by_name.count(fd.name) != 0) continue;
    if (!fd.name.empty()) bag.by_name.emplace(fd.name, bag.slots.size());
    bag.slots.push_back(tree_static_seed(fd));
  }
}

// PE Class_getFieldAt @ 0x00404800 keyed by name instead of slot — the pair
// Class_findFieldSlot @ 0x00405690 hands its callers (op 0x101B @ 0x0042352E,
// Thread_evalName_fieldPath @ 0x00420F40) is (owner class, slot|0x40000000),
// and the host resolves that slot back through the field name.
// Mirrors tree_field_slot: get never invents a slot, put may. Unlike the
// instance bag, a get on a *declared* static does materialise the bag —
// that is Class_ensureInitialized, not an invention.
JvmValue* tree_static_slot(const JvmClass* owner, const std::string& name,
                           bool create) {
  if (!owner || name.empty()) return nullptr;
  tree_static_ensure_initialized(owner);  // PE @ 0x404803
  auto bit = g_tree_statics.find(owner->name);
  if (bit == g_tree_statics.end()) return nullptr;
  TreeStaticBag& bag = bit->second;
  auto it = bag.by_name.find(name);
  if (it != bag.by_name.end()) return &bag.slots[it->second];
  if (!create) return nullptr;  // PE Instance_getFieldAt out-of-range → 0
  // Undeclared static on a put: the PE has no ValueField to assign into, so
  // this slot is a Soft invention kept symmetric with tree_field_slot.
  bag.by_name.emplace(name, bag.slots.size());
  bag.slots.push_back(JvmValue::make_int(0));
  return &bag.slots.back();
}

// PE Class_getFieldAt @ 0x00404800 → Instance_getFieldAt @ 0x00403820:
// slot < 0 → 0 @ 0x403826, slot past the PtrVec count → 0 @ 0x403838, else
// PtrVec[slot]. Callers strip the static bit with 0xBFFFFFFF (@ 0x00420F2D)
// before calling, so a raw slot|0x40000000 is tolerated here too; the -1 miss
// from Class_findFieldSlot stays negative and yields nullptr.
JvmValue* tree_static_slot_at(const JvmClass* owner, int32_t slot) {
  if (!owner || slot < 0) return nullptr;
  tree_static_ensure_initialized(owner);
  auto bit = g_tree_statics.find(owner->name);
  if (bit == g_tree_statics.end()) return nullptr;
  const uint32_t idx = static_cast<uint32_t>(slot) & 0xBFFFFFFFu;
  TreeStaticBag& bag = bit->second;
  if (idx >= bag.slots.size()) return nullptr;
  return &bag.slots[idx];
}

std::string tree_strip_class_desc(const std::string& d) {
  if (d.size() >= 2 && d[0] == 'L' && d.back() == ';')
    return d.substr(1, d.size() - 2);
  return d;
}

bool tree_truthy(const JvmValue& v) {
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

InvObject* tree_concat_str(InvObject* a, InvObject* b) {
  const char* sa = a ? string_cstr(a) : "";
  const char* sb = b ? string_cstr(b) : "";
  std::string out = std::string(sa ? sa : "") + (sb ? sb : "");
  return string_new(out.c_str());
}

// OptionsDialog: `w + " X " + h` — operands may be Int/Float, not String.
InvObject* tree_value_as_string(const JvmValue& v) {
  if (v.tag == JvmTag::Obj) return v.v.o ? v.v.o : string_new("");
  char buf[64];
  if (v.tag == JvmTag::Float)
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v.v.f));
  else
    std::snprintf(buf, sizeof(buf), "%d", v.v.i);
  return string_new(buf);
}

float tree_static_qm(const std::string& fname) {
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

int32_t tree_static_vs(const std::string& fname) {
  if (fname == "VS_DEMO") return 0x0001;
  if (fname == "VS_USED") return 0x0002;
  if (fname == "VS_STOCK") return 0x0004;
  if (fname == "VS_DRACE") return 0x0008;
  if (fname == "VS_NRACE") return 0x0010;
  if (fname == "VS_RRACE") return 0x0020;
  return -1;
}

int32_t tree_static_rid_carcolor(const std::string& fname) {
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

int32_t tree_resolve_rid_const(const JvmClass& cls, uint32_t imm) {
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

void tree_field_set_int(InvObject* obj, const char* name, int32_t v) {
  // Soft ≡ PE JVM_vm_set_int_field @ 0x42A9E0 write of Value+8 when type 'I',
  // but Soft invents the bag slot (PE requires Instance_getFieldAt hit).
  if (JvmValue* s = tree_field_slot(obj, name, true)) *s = JvmValue::make_int(v);
}

int32_t tree_field_get_int(InvObject* obj, const char* name) {
  // Soft ≡ PE JVM_vm_get_int_field_by_name @ 0x42A430:
  //   Object_getField → type desc 'I' → payload+8; miss/wrong type → 0
  //   (soft skips Engine_ErrorLogMsgBox). Soft bag: Int hit; Float→i truncate
  //   for TREE numeric sugar; Obj/other → 0 (never read pointer bits as int).
  if (!obj || !name || !name[0]) return 0;
  // Soft array/Vector.length (TREE sugar; PE array length is a real field).
  if (std::strcmp(name, "length") == 0 && tree_vector_is(obj))
    return tree_vector_size(obj);
  if (JvmValue* s = tree_field_slot(obj, name, false)) {
    if (s->tag == JvmTag::Int) return s->v.i;
    if (s->tag == JvmTag::Float) return static_cast<int32_t>(s->v.f);
    return 0;
  }
  return 0;
}

void tree_field_set_obj(InvObject* obj, const char* name, InvObject* v) {
  // Soft ≡ PE JVM_vm_get_instance_field / Object_getField payload write path
  // for L/[ types — Soft invents bag slot on put.
  if (JvmValue* s = tree_field_slot(obj, name, true)) *s = JvmValue::make_obj(v);
}

InvObject* tree_field_get_obj(InvObject* obj, const char* name) {
  // Soft ≡ PE JVM_vm_get_instance_field @ 0x42A690 payload+8 on L/[ hit;
  // miss / non-ref Soft tag → nullptr (PE ErrorLog → 0).
  if (JvmValue* s = tree_field_slot(obj, name, false)) {
    if (s->tag == JvmTag::Obj) return s->v.o;
  }
  return nullptr;
}

void tree_field_set_float(InvObject* obj, const char* name, float v) {
  // Soft ≡ PE JVM_vm_set_float_field @ 0x42A040 (type 'F' → Value+8);
  // Soft invents bag slot on put.
  if (JvmValue* s = tree_field_slot(obj, name, true)) *s = JvmValue::make_float(v);
}

float tree_field_get_float(InvObject* obj, const char* name) {
  // Soft ≡ PE JVM_vm_get_float_field @ 0x42A560:
  //   Object_getField → type 'F' → float(Value+8); miss/wrong type → 0.0
  // Soft bag: Float hit; Int→f for TREE numeric sugar; Obj/other → 0.f.
  if (JvmValue* s = tree_field_slot(obj, name, false)) {
    if (s->tag == JvmTag::Float) return s->v.f;
    if (s->tag == JvmTag::Int) return static_cast<float>(s->v.i);
    return 0.f;
  }
  return 0.f;
}

InvObject* tree_vector_new() {
  InvObject* o = reinterpret_cast<InvObject*>(new InvString{nullptr});
  g_tree_vectors[o] = {};
  g_tree_host_class[o] = "java.util.Vector";
  return o;
}

bool tree_vector_is(InvObject* vec) {
  return vec && g_tree_vectors.find(vec) != g_tree_vectors.end();
}

InvObject* tree_array_new(int32_t length) {
  return tree_array_new_desc(length, "[Ljava.lang.Object;");
}

InvObject* tree_array_new_desc(int32_t length, const char* desc) {
  InvObject* o = reinterpret_cast<InvObject*>(new InvString{nullptr});
  g_tree_host_class[o] = (desc && desc[0]) ? desc : "[Ljava.lang.Object;";
  if (length > 0)
    g_tree_vectors[o].assign(static_cast<size_t>(length), nullptr);
  else
    g_tree_vectors[o] = {};
  return o;
}

int32_t tree_vector_size(InvObject* vec) {
  auto it = g_tree_vectors.find(vec);
  if (it == g_tree_vectors.end()) return 0;
  return static_cast<int32_t>(it->second.size());
}

void tree_vector_add(InvObject* vec, InvObject* elem) {
  if (!vec) return;
  g_tree_vectors[vec].push_back(elem);
}

void tree_vector_remove(InvObject* vec, InvObject* elem) {
  if (!vec || !elem) return;
  auto it = g_tree_vectors.find(vec);
  if (it == g_tree_vectors.end()) return;
  auto& v = it->second;
  v.erase(std::remove(v.begin(), v.end(), elem), v.end());
}

InvObject* tree_vector_element_at(InvObject* vec, int32_t idx) {
  auto it = g_tree_vectors.find(vec);
  if (it == g_tree_vectors.end()) return nullptr;
  if (idx < 0 || static_cast<size_t>(idx) >= it->second.size()) return nullptr;
  return it->second[static_cast<size_t>(idx)];
}

void tree_vector_set(InvObject* vec, int32_t idx, InvObject* elem) {
  if (!vec || idx < 0) return;
  auto& v = g_tree_vectors[vec];
  if (static_cast<size_t>(idx) >= v.size())
    v.resize(static_cast<size_t>(idx) + 1, nullptr);
  v[static_cast<size_t>(idx)] = elem;
}

void tree_vector_resize(InvObject* vec, int32_t n) {
  if (!vec || n < 0) return;
  g_tree_vectors[vec].resize(static_cast<size_t>(n), nullptr);
}

InvObject* tree_host_new(const char* class_fqn) {
  InvObject* o = reinterpret_cast<InvObject*>(new InvString{nullptr});
  if (class_fqn) g_tree_host_class[o] = class_fqn;
  return o;
}

const char* tree_host_class(InvObject* obj) {
  auto it = g_tree_host_class.find(obj);
  if (it == g_tree_host_class.end()) return "";
  return it->second.c_str();
}

// GameRef.queueEvent(ResourceRef,I,String)V @ 0x0047DA30
// Java: queueEvent(null, EVENT_COMMAND=0x10, param). String often on top.
void tree_pack_queue_event(std::vector<JvmValue>& stack,
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

bool tree_is_renderref(InvObject* o) {
  if (!o) return false;
  const char* c = tree_host_class(o);
  if (!c || !c[0]) return false;
  if (std::strstr(c, "Camera")) return false;
  return std::strstr(c, "RenderRef") != nullptr;
}

bool tree_is_vector3(InvObject* o) {
  if (!o) return false;
  const char* c = tree_host_class(o);
  if (c && std::strstr(c, "Vector3")) return true;
  if (c && c[0]) return false;
  if (string_cstr(o)) return false;
  return vec3_is(o);
}

bool tree_is_ypr(InvObject* o) {
  if (!o) return false;
  const char* c = tree_host_class(o);
  if (c && std::strstr(c, "Ypr")) return true;
  if (c && c[0]) return false;
  return ypr_is(o);
}

bool tree_is_v3_binop_junk(InvObject* o) {
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
void tree_pack_vector3_binop(std::vector<JvmValue>& stack,
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
    return u.tag == JvmTag::Obj && tree_is_vector3(u.v.o);
  };
  while (!stack.empty() && stack.back().tag == JvmTag::Obj &&
         tree_is_v3_binop_junk(stack.back().v.o) && under_is_v3_or_num())
    pop();
  JvmValue recv = JvmValue::make_obj(nullptr);
  JvmValue arg = JvmValue::make_obj(nullptr);
  if (!stack.empty() && (stack.back().tag == JvmTag::Float ||
                         stack.back().tag == JvmTag::Int)) {
    arg = pop();
    while (!stack.empty() && stack.back().tag == JvmTag::Obj &&
           tree_is_v3_binop_junk(stack.back().v.o) && under_is_v3_or_num())
      pop();
  }
  if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
      tree_is_vector3(stack.back().v.o))
    recv = pop();
  if (arg.tag != JvmTag::Float && arg.tag != JvmTag::Int && !stack.empty()) {
    if (stack.back().tag == JvmTag::Float || stack.back().tag == JvmTag::Int)
      arg = pop();
    else if (stack.back().tag == JvmTag::Obj &&
             (tree_is_vector3(stack.back().v.o) || !stack.back().v.o))
      arg = pop();
  }
  if (recv.tag == JvmTag::Obj && recv.v.o && !tree_is_vector3(recv.v.o) &&
      arg.tag == JvmTag::Obj && tree_is_vector3(arg.v.o))
    std::swap(recv, arg);
  args.push_back(recv);
  args.push_back(arg);
  (void)locals;
  if (recv_out) *recv_out = recv;
}

// RenderRef.create(ResourceRef,RenderRef,String)V @ 0x00480EE0
// Java: create(parent, type|rid, alias). String often on top.
void tree_pack_renderref_create(std::vector<JvmValue>& stack,
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

}  // namespace inv
