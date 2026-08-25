#include "jvm.hpp"
#include "callinfo.hpp"
#include "tufa.hpp"
#include "jvm_bridge.hpp"
#include "tree_interp.hpp"
#include "host_objects.hpp"
#include "runtime.hpp"
#include "natives.hpp"
#include "rpak.hpp"
#include "video_fmv.hpp"
#include "render_d3d9.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace inv {

// Trigger.java: 4-arg delegates to 5-arg (r=20); type==null → system:0x34;
// trigger = new GameRef(parent, type.id(), pos+",0,0,0,sphere,"+r, alias).
static int32_t trigger_default_type_id() {
  const RpakPack* sp = rpak_find_by_name("system");
  if (!sp) {
    java_lang_System_openLib(string_new("system.rpk"));
    sp = rpak_find_by_name("system");
  }
  if (sp) return rpak_make_id(sp->pack_id, 0x0034);
  return 0x34;
}

static bool trigger_apply_ctor(InvObject* self, const std::vector<JvmValue>& args) {
  // Stock signatures (Trigger.java), this at [0]:
  //   (parent, type, pos, alias) | + float r | + float x,y,z
  // vec3_new / gameref_new often have empty host-class, so sniff by position.
  if (!self || args.size() < 5) return false;
  auto obj_at = [&](size_t i) -> InvObject* {
    return (i < args.size() && args[i].tag == JvmTag::Obj) ? args[i].v.o
                                                           : nullptr;
  };
  auto flt_at = [&](size_t i) -> float {
    if (i >= args.size()) return 0.f;
    if (args[i].tag == JvmTag::Float) return args[i].v.f;
    if (args[i].tag == JvmTag::Int) return static_cast<float>(args[i].v.i);
    return 0.f;
  };
  InvObject* parent = obj_at(1);
  InvObject* type = obj_at(2);
  InvObject* pos = obj_at(3);
  InvObject* alias = nullptr;
  float r = 20.f;
  float box[3] = {};
  bool is_box = false;
  if (args.size() >= 8 &&
      (args[4].tag == JvmTag::Float || args[4].tag == JvmTag::Int)) {
    box[0] = flt_at(4);
    box[1] = flt_at(5);
    box[2] = flt_at(6);
    alias = obj_at(7);
    is_box = true;
  } else if (args.size() >= 6 &&
             (args[4].tag == JvmTag::Float || args[4].tag == JvmTag::Int)) {
    r = flt_at(4);
    alias = obj_at(5);
  } else {
    alias = obj_at(4);
  }
  if (!parent || !pos) {
    std::vector<InvObject*> objs;
    for (size_t i = 1; i < args.size(); ++i) {
      if (args[i].tag == JvmTag::Obj) objs.push_back(args[i].v.o);
    }
    if (!parent && !objs.empty()) parent = objs[0];
    if (!pos && objs.size() >= 3) pos = objs[2];
    if (!alias && objs.size() >= 4) alias = objs.back();
  }
  if (!parent || !pos) return false;
  if (!type) {
    type = gameref_new();
    java_util_resource_ResourceRef_set(type, trigger_default_type_id());
  }
  float x = 0.f, y = 0.f, z = 0.f;
  vec3_get(pos, &x, &y, &z);
  char params[160];
  if (is_box) {
    std::snprintf(params, sizeof(params), "%g,%g,%g,0,0,0,box,%g,%g,%g", x, y, z,
                  box[0], box[1], box[2]);
  } else {
    std::snprintf(params, sizeof(params), "%g,%g,%g,0,0,0,sphere,%g", x, y, z, r);
  }
  InvObject* gr = gameref_new();
  java_util_resource_GameRef_create(gr, parent, type, string_new(params), alias);
  tree_field_set_obj(self, "trigger", gr);
  return true;
}

// City.ParkingCar(parent, type, pos, ori, colorSeed) — Java TREE, not a native.
// GameRef.create @ 0x0047D7B0 + setMatrix @ 0x0047E490.
static bool parking_car_apply_ctor(InvObject* self,
                                   const std::vector<JvmValue>& args) {
  if (!self) return false;
  InvObject* parent = nullptr;
  InvObject* type = nullptr;
  InvObject* pos = nullptr;
  InvObject* ori = nullptr;
  int32_t color_seed = 0;
  for (size_t i = 1; i < args.size(); ++i) {
    const JvmValue& a = args[i];
    if (a.tag == JvmTag::Int) {
      color_seed = a.v.i;
      continue;
    }
    if (a.tag == JvmTag::Float) {
      color_seed = static_cast<int32_t>(a.v.f);
      continue;
    }
    if (a.tag != JvmTag::Obj || !a.v.o) continue;
    const char* hc = tree_host_class(a.v.o);
    if (hc && std::strstr(hc, "Vector3") && !pos)
      pos = a.v.o;
    else if (hc && std::strstr(hc, "Ypr") && !ori)
      ori = a.v.o;
    else if (!parent)
      parent = a.v.o;
    else if (!type)
      type = a.v.o;
    else if (!pos)
      pos = a.v.o;
    else if (!ori)
      ori = a.v.o;
  }
  if (!parent || !pos) return false;
  float px = 0.f, py = 0.f, pz = 0.f, oy = 0.f, op = 0.f, or_ = 0.f;
  if (pos) vec3_get(pos, &px, &py, &pz);
  if (ori) ypr_get(ori, &oy, &op, &or_);
  InvObject* orig_pos = vec3_new(px, py, pz);
  InvObject* orig_ori = ypr_new(oy, op, or_);
  tree_field_set_obj(self, "origPos", orig_pos);
  tree_field_set_obj(self, "origOri", orig_ori);
  char params[160];
  std::snprintf(params, sizeof(params), "%g,%g,%g,%g,0,0,%d", px, py, pz, oy,
                color_seed);
  InvObject* gr = gameref_new();
  java_util_resource_GameRef_create(gr, parent, type, string_new(params),
                                    string_new("_.*0*._"));
  java_util_resource_GameRef_setMatrix(gr, orig_pos, orig_ori);
  tree_field_set_obj(self, "gr", gr);
  return true;
}

static bool queue_event_is_string(InvObject* o) {
  if (!o) return false;
  const char* c = tree_host_class(o);
  return string_cstr(o) && (!c || !c[0] || std::strstr(c, "String"));
}

// GameRef.queueEvent @ 0x0047DA30. TREE names MouseCursor/Player/Vehicle/Bot
// when packing fails; Java calls cursor/controller/car/brain (GameRef).
static InvObject* queue_event_retarget(InvObject* self) {
  if (!self) return self;
  const char* hc = tree_host_class(self);
  if (!hc || !hc[0]) return self;
  if (std::strstr(hc, "MouseCursor")) {
    if (InvObject* c = tree_field_get_obj(self, "cursor")) return c;
  }
  if (std::strstr(hc, "Player")) {
    if (InvObject* c = tree_field_get_obj(self, "controller")) return c;
  }
  if (std::strstr(hc, "City") || std::strstr(hc, "Valocity") ||
      std::strstr(hc, "RaceSetup") || std::strstr(hc, "Garage") ||
      std::strstr(hc, "Track") || std::strstr(hc, "MainMenu")) {
    if (InvObject* p = tree_field_get_obj(self, "player")) {
      if (InvObject* car = tree_field_get_obj(p, "car")) return car;
    }
    if (InvObject* ic = java_io_Input_cursor()) {
      if (InvObject* c = tree_field_get_obj(ic, "cursor")) return c;
    }
  }
  return self;
}

static void queue_event_apply(const std::vector<JvmValue>& args) {
  InvObject* self = nullptr;
  InvObject* ro = nullptr;
  InvObject* param = nullptr;
  int32_t type = 0x10;  // GameRef.EVENT_COMMAND
  for (const JvmValue& a : args) {
    if (a.tag == JvmTag::Int) {
      type = a.v.i;
      continue;
    }
    if (a.tag == JvmTag::Float) {
      type = static_cast<int32_t>(a.v.f);
      continue;
    }
    if (a.tag != JvmTag::Obj) continue;
    if (!a.v.o) continue;
    if (queue_event_is_string(a.v.o)) {
      param = a.v.o;
      continue;
    }
    if (!self)
      self = a.v.o;
    else if (!ro)
      ro = a.v.o;
  }
  self = queue_event_retarget(self);
  java_util_resource_GameRef_queueEvent(self, ro, type, param);
}

static bool is_renderref_not_camera(InvObject* o, const char* class_fqn) {
  if (class_fqn && std::strstr(class_fqn, "Camera")) return false;
  const char* hc = o ? tree_host_class(o) : nullptr;
  if (hc && std::strstr(hc, "Camera")) return false;
  if (hc && std::strstr(hc, "RenderRef")) return true;
  if (class_fqn && std::strstr(class_fqn, "RenderRef")) return true;
  return false;
}

// RenderRef.create(ResourceRef,RenderRef,String)V @ 0x00480EE0
// Java ctor: super(); create(parent, type|new RenderRef(rid), alias).
static void renderref_apply_create(const std::vector<JvmValue>& args) {
  if (args.empty() || args[0].tag != JvmTag::Obj || !args[0].v.o) return;
  InvObject* self = args[0].v.o;
  InvObject* parent = nullptr;
  InvObject* type = nullptr;
  InvObject* alias = nullptr;
  int32_t rid = 0;
  bool have_rid = false;
  for (size_t i = 1; i < args.size(); ++i) {
    const JvmValue& a = args[i];
    if (a.tag == JvmTag::Int) {
      rid = a.v.i;
      have_rid = true;
      continue;
    }
    if (a.tag != JvmTag::Obj) continue;
    if (!a.v.o) continue;
    if (queue_event_is_string(a.v.o)) {
      alias = a.v.o;
      continue;
    }
    const char* hc = tree_host_class(a.v.o);
    if (hc && std::strstr(hc, "RenderRef") && self && a.v.o != self && !type) {
      type = a.v.o;
      continue;
    }
    if (!parent)
      parent = a.v.o;
    else if (!type)
      type = a.v.o;
  }
  if (!type && have_rid) {
    type = tree_host_new("java.util.resource.RenderRef");
    java_util_resource_ResourceRef_set(type, rid);
  }
  java_util_resource_RenderRef_create(self, parent, type, alias);
}

namespace {

std::vector<std::string> split_ws(const std::string& s) {
  std::istringstream iss(s);
  std::vector<std::string> out;
  std::string w;
  while (iss >> w) out.push_back(w);
  return out;
}

const NativeEntry* resolve_native(const char* fqn, const char* name,
                                  const char* /*jni_sig*/) {
  const NativeEntry* first = nullptr;
  for (size_t i = 0; i < kNativeTableCount; ++i) {
    if (std::strcmp(kNativeTable[i].class_fqn, fqn) != 0) continue;
    if (std::strcmp(kNativeTable[i].method_name, name) != 0) continue;
    if (!first) first = &kNativeTable[i];
  }
  return first;
}

bool file_exists(const char* path) {
  if (FILE* f = std::fopen(path, "rb")) {
    std::fclose(f);
    return true;
  }
  return false;
}

int count_jni_args(const char* sig) {
  if (!sig || sig[0] != '(') return -1;
  int n = 0;
  const char* p = sig + 1;
  while (*p && *p != ')') {
    if (*p == 'L') {
      while (*p && *p != ';') ++p;
      if (*p == ';') ++p;
      ++n;
    } else if (*p == '[') {
      while (*p == '[') ++p;
      if (*p == 'L') {
        while (*p && *p != ';') ++p;
        if (*p == ';') ++p;
      } else if (*p) {
        ++p;
      }
      ++n;
    } else {
      ++p;
      ++n;
    }
  }
  return n;
}

class JvmTreeHost final : public TreeHost {
 public:
  explicit JvmTreeHost(Jvm* j) : jvm_(j) {}

  InvObject* new_instance(const char* class_fqn) override {
    if (!class_fqn) return string_new("");
    // Invictus `new String[n]` → class `[Ljava.lang.String;` (no .class file).
    if (class_fqn[0] == '[') return tree_array_new_desc(0, class_fqn);
    if (std::strcmp(class_fqn, "java.io.FindFile") == 0) return findfile_new();
    if (std::strcmp(class_fqn, "java.lang.String") == 0) return string_new("");
    if (std::strcmp(class_fqn, "java.util.Vector") == 0) return tree_vector_new();
    if (std::strcmp(class_fqn, "java.render.osd.Rectangle") == 0)
      class_fqn = "java.render.Rectangle";
    if (std::strcmp(class_fqn, "java.lang.Integer") == 0) {
      InvObject* o = tree_host_new(class_fqn);
      tree_field_set_int(o, "value", 0);
      return o;
    }
    if (std::strcmp(class_fqn, "java.lang.Vector3") == 0) {
      InvObject* o = tree_host_new(class_fqn);
      vec3_set(o, 0.f, 0.f, 0.f);
      tree_field_set_float(o, "x", 0.f);
      tree_field_set_float(o, "y", 0.f);
      tree_field_set_float(o, "z", 0.f);
      return o;
    }
    if (std::strcmp(class_fqn, "java.lang.Ypr") == 0) {
      InvObject* o = tree_host_new(class_fqn);
      ypr_set(o, 0.f, 0.f, 0.f);
      tree_field_set_float(o, "y", 0.f);
      tree_field_set_float(o, "p", 0.f);
      tree_field_set_float(o, "r", 0.f);
      return o;
    }
    if (std::strstr(class_fqn, "Dialog")) {
      InvObject* o = tree_host_new(class_fqn);
      dialog_note_constructed(o, class_fqn);
      return o;
    }
    if (std::strcmp(class_fqn, "java.game.VehicleModel") == 0) {
      InvObject* o = tree_host_new(class_fqn);
      tree_field_set_float(o, "prevalence", 1.f);
      tree_field_set_int(o, "vehicleSetMask", 0x3f);  // all VS_* default
      tree_field_set_obj(o, "preferredColorIndexes", tree_vector_new());
      tree_field_set_float(o, "minPower", 1.f);
      tree_field_set_float(o, "maxPower", 1.5f);
      tree_field_set_float(o, "minOptical", 1.f);
      tree_field_set_float(o, "maxOptical", 1.5f);
      tree_field_set_float(o, "minTear", 1.f);
      tree_field_set_float(o, "maxTear", 1.f);
      tree_field_set_float(o, "minWear", 1.f);
      tree_field_set_float(o, "maxWear", 1.f);
      return o;
    }
    // VehicleType / *_VT / GameType: field-init Vectors like the Java sources.
    if (std::strstr(class_fqn, "VehicleType") ||
        std::strstr(class_fqn, "_VT") ||
        std::strcmp(class_fqn, "java.game.GameType") == 0) {
      InvObject* o = tree_host_new(class_fqn);
      tree_field_set_obj(o, "vtdarr", tree_vector_new());
      tree_field_set_obj(o, "preferredColorIndexes", tree_vector_new());
      tree_field_set_float(o, "prevalence", 1.f);
      tree_field_set_int(o, "vehicleSetMask", 0);
      return o;
    }
    return tree_host_new(class_fqn);
  }

  JvmValue call(const char* class_fqn, const char* method, const char* jni_sig,
                const std::vector<JvmValue>& args, bool is_static) override {
    return jvm_->invoke(class_fqn, method, jni_sig, args, is_static);
  }

  JvmValue call_by_name(const char* class_fqn, const char* method,
                        const std::vector<JvmValue>& args,
                        bool prefer_static) override {
    // Retarget hops skip Jvm::invoke — cap them or City↔Osd/createCar loops
    // blow the native stack (0xC00000FD) before invoke's depth abort.
    thread_local int tls_cbn = 0;
    thread_local char tls_cbn_trail[16][96];
    struct CbnGuard {
      int* d;
      CbnGuard(int* p, const char* c, const char* m) : d(p) {
        if (*d >= 0 && *d < 16)
          std::snprintf(tls_cbn_trail[*d], sizeof(tls_cbn_trail[*d]),
                        "%.60s.%.30s", c ? c : "?", m ? m : "?");
        ++(*d);
      }
      ~CbnGuard() { --(*d); }
    } cbn_guard(&tls_cbn, class_fqn, method);
    if (tls_cbn > 32) {
      std::fprintf(stderr, "[jvm] call_by_name depth=%d abort %s.%s\n", tls_cbn,
                   class_fqn ? class_fqn : "?", method ? method : "?");
      for (int i = 0; i < 16 && i < tls_cbn; ++i)
        std::fprintf(stderr, "  cbn#%d %s\n", i, tls_cbn_trail[i]);
        return JvmValue::make_void();
      }

    // Native.<init>() — empty Java ctor (Native.java). TREE super() from
    // ResourceRef/String/File/Thread. No native. VM unbox of Native.ptr is
    // Thread::callMethod @ 0x0041FBC0 (string java.lang.Native).
    if (method && std::strcmp(method, "<init>") == 0 && class_fqn &&
        std::strcmp(class_fqn, "java.lang.Native") == 0)
        return JvmValue::make_void();

    // Scene.addSceneElements(float) TREE → time2Config → addSceneElements(int)
    // with the same argc=1, so inherited walk re-enters forever. Host leaf
    // (valocity_apply_scene) mirrors Scene.java configs 0..14.
    if (method && std::strcmp(method, "addSceneElements") == 0) {
      InvObject* self = nullptr;
      if (!args.empty() && args[0].tag == JvmTag::Obj) self = args[0].v.o;
      if (!self) self = game_logic_actual_state();
      if (self) {
        valocity_apply_scene(self);
        if (args.size() >= 2 && args[1].tag == JvmTag::Int)
          tree_field_set_int(self, "lastConfig", args[1].v.i);
      }
      return JvmValue::make_void();
    }
    if (method && std::strcmp(method, "time2Config") == 0) {
      float t = game_logic_time();
      InvObject* self = nullptr;
      if (!prefer_static && !args.empty() && args[0].tag == JvmTag::Obj)
        self = args[0].v.o;
      const size_t i0 = (!prefer_static && !args.empty()) ? 1 : 0;
      for (size_t i = i0; i < args.size(); ++i) {
        if (args[i].tag == JvmTag::Float) {
          t = args[i].v.f;
          break;
        }
        if (args[i].tag == JvmTag::Int) {
          t = static_cast<float>(args[i].v.i);
          break;
        }
      }
      float rnd = self ? tree_field_get_float(self, "lastSelectionSeed") : 0.f;
      return JvmValue::make_int(scene_time2config(t, rnd));
    }
    if (method && std::strcmp(method, "alignToRoad") == 0) {
      InvObject* recv = nullptr;
      if (!args.empty() && args[0].tag == JvmTag::Obj) recv = args[0].v.o;
      InvObject* map = recv;
      if (map) {
        const char* hc = tree_host_class(map);
        if (!hc || !std::strstr(hc, "GroundRef")) {
          InvObject* m = tree_field_get_obj(map, "map");
          if (!m) {
            if (InvObject* track = tree_field_get_obj(map, "track"))
              m = tree_field_get_obj(track, "map");
          }
          if (m) map = m;
        }
      }
      InvObject* rp = nullptr;
      for (size_t i = 1; i < args.size(); ++i) {
        if (args[i].tag != JvmTag::Obj || !args[i].v.o) continue;
        const char* ac = tree_host_class(args[i].v.o);
        if (ac && std::strstr(ac, "Vector3")) {
          rp = args[i].v.o;
          break;
        }
        if (!rp) rp = args[i].v.o;
      }
      if (!rp && recv) {
        rp = tree_field_get_obj(recv, "pS");
        if (!rp) rp = tree_field_get_obj(recv, "posStart");
      }
      if (!rp) {
        if (InvObject* player = game_logic_player()) {
          if (InvObject* car = tree_field_get_obj(player, "car"))
            rp = tree_field_get_obj(car, "pos");
        }
      }
      if (!rp) rp = vec3_new(0.f, 0.f, 0.f);
      return JvmValue::make_obj(
          java_util_resource_GroundRef_alignToRoad(map, rp));
    }

    // No host-script path: TREE methods + native table only.

    // Dialog flag constants often packed as 0x12/call_by_name (not 0x1b).
    if (method) {
      if (std::strcmp(method, "DF_MODAL") == 0)
        return JvmValue::make_int(0x00000001);
      if (std::strcmp(method, "DF_FULLSCREEN") == 0)
        return JvmValue::make_int(0x00000002);
      if (std::strcmp(method, "DF_DEFAULTBG") == 0)
        return JvmValue::make_int(0x00000004);
      if (std::strcmp(method, "DF_FREEZE") == 0)
        return JvmValue::make_int(0x00000008);
      if (std::strcmp(method, "DF_LEAVEPOINTER") == 0)
        return JvmValue::make_int(0x00000010);
      if (std::strcmp(method, "DF_HIGHPRI") == 0)
        return JvmValue::make_int(0x00000020);
      if (std::strcmp(method, "DF_DARKEN") == 0)
        return JvmValue::make_int(0x00000100);
      if (std::strcmp(method, "DF_SILENT") == 0)
        return JvmValue::make_int(0x00000200);
      if (std::strcmp(method, "SIF_NOEMPTY") == 0)
        return JvmValue::make_int(0x00001000);
    }

    // Dialog.display — stock wait()/notify(); host auto-accepts (career dialogs).
    // TREE names City.display argc=0 (WarningDialog leftover).
    if (method && std::strcmp(method, "display") == 0) {
      InvObject* dlg = nullptr;
      bool loading = false;
      for (const auto& a : args) {
        if (a.tag != JvmTag::Obj || !a.v.o) continue;
        const char* hc = tree_host_class(a.v.o);
        if ((hc && std::strstr(hc, "Dialog")) ||
            (class_fqn && std::strstr(class_fqn, "Dialog")))
          dlg = a.v.o;
        if ((hc && std::strstr(hc, "LoadingScreen")) ||
            (class_fqn && std::strstr(class_fqn, "LoadingScreen")))
          loading = true;
      }
      if (dlg) return JvmValue::make_int(dialog_display(dlg));
      const bool cityish =
          class_fqn && (std::strstr(class_fqn, "City") ||
                        std::strstr(class_fqn, "Valocity") ||
                        std::strstr(class_fqn, "Track") ||
                        (std::strstr(class_fqn, "Object") &&
                         !std::strstr(class_fqn, "Game")));
      if (cityish && !loading) return JvmValue::make_int(0);
    }

    // OptionsDialog / MainMenuDialog.changeMode(group) — hide/show OSD groups.
    if (method && std::strcmp(method, "changeMode") == 0 && !args.empty() &&
        args[0].tag == JvmTag::Obj && args[0].v.o) {
      const char* hc = tree_host_class(args[0].v.o);
      const bool is_dlg =
          (hc && (std::strstr(hc, "OptionsDialog") ||
                  std::strstr(hc, "MainMenuDialog") ||
                  std::strstr(hc, "Dialog"))) ||
          (class_fqn && (std::strstr(class_fqn, "OptionsDialog") ||
                         std::strstr(class_fqn, "MainMenuDialog")));
      if (is_dlg) {
        int32_t group = -1;
        if (args.size() >= 2 && args[1].tag == JvmTag::Int)
          group = args[1].v.i;
        options_dialog_change_mode(args[0].v.o, group);
        // MainMenuDialog.changeMode also toggles career buttons when on main.
        if (hc && std::strstr(hc, "MainMenuDialog")) {
          const int32_t main_g = tree_field_get_int(args[0].v.o, "mainGroup");
          if (group == main_g) {
            InvObject* back = tree_field_get_obj(args[0].v.o, "backGarageButton");
            InvObject* save = tree_field_get_obj(args[0].v.o, "saveCarButton");
            const bool played = game_logic_played() != 0;
            if (back) tree_field_set_int(back, "enabled", played ? 1 : 0);
            if (save) {
              const int32_t gm = game_logic_game_mode();
              const bool car =
                  game_logic_player() &&
                  tree_field_get_obj(game_logic_player(), "car") != nullptr;
              tree_field_set_int(save, "enabled",
                                 ((gm == 1 || gm == 4) && car) ? 1 : 0);
            }
          }
        }
        return JvmValue::make_void();
      }
    }

    // GameLogic statics — TREE bodies re-enter / overflow; engine mirrors.
    if (method && (prefer_static ||
                   (class_fqn && std::strstr(class_fqn, "GameLogic")))) {
      if (std::strcmp(method, "autoSave") == 0)
        return JvmValue::make_int(game_logic_auto_save());
      if (std::strcmp(method, "autoSaveQuiet") == 0) {
        game_logic_auto_save_quiet();
        return JvmValue::make_void();
      }
      if (std::strcmp(method, "loadDefaults") == 0) {
        game_logic_load_defaults();
        return JvmValue::make_void();
      }
      if (std::strcmp(method, "setTime") == 0) {
        float t = 12.f * 3600.f;
        if (prefer_static && !args.empty()) {
          if (args[0].tag == JvmTag::Float) t = args[0].v.f;
          else if (args[0].tag == JvmTag::Int)
            t = static_cast<float>(args[0].v.i);
        } else if (args.size() >= 2) {
          if (args[1].tag == JvmTag::Float) t = args[1].v.f;
          else if (args[1].tag == JvmTag::Int)
            t = static_cast<float>(args[1].v.i);
        }
        game_logic_set_time(t);
        return JvmValue::make_void();
      }
      if (std::strcmp(method, "changeActiveSection") == 0) {
        InvObject* next = nullptr;
        if (prefer_static && !args.empty() && args[0].tag == JvmTag::Obj)
          next = args[0].v.o;
        else if (!prefer_static && args.size() >= 2 && args[1].tag == JvmTag::Obj)
          next = args[1].v.o;
        else if (!args.empty() && args.back().tag == JvmTag::Obj)
          next = args.back().v.o;
        // Java signature is GameState. TREE CMD_EXIT `changeActiveSection(null)`
        // often leaves String/Dialog on the stack — that is not a section.
        if (next && !game_logic_is_section(next)) next = nullptr;
        // TREE packing often passes null here and would wipe MainMenu mid CMD_NEW.
        // Stock EXIT uses explicit null — defer/note while hub TREE is live.
        if (!next) {
          if (main_menu_hub_deferring()) {
            main_menu_hub_note_cas(nullptr);
            return JvmValue::make_void();
          }
          InvObject* cur = game_logic_actual_state();
          const char* cn = cur ? tree_host_class(cur) : nullptr;
          if (cn && std::strstr(cn, "MainMenu") &&
              !main_menu_cmd_exit_cas_pending())
            return JvmValue::make_void();
        }
        // Defer Garage/Valocity CAS until MainMenuDialog.osdCommand returns —
        // nested MainMenu.exit mid-TREE breaks createOSDObjects / city enter.
        if (main_menu_hub_deferring() && next) {
          const char* nn = tree_host_class(next);
          if (nn && (std::strstr(nn, "Garage") || std::strstr(nn, "Valocity") ||
                     std::strstr(nn, "RaceSetup"))) {
            main_menu_hub_note_cas(next);
            return JvmValue::make_void();
          }
        }
        // Valocity QUICKRACE handoff: TREE packs changeActiveSection(racesetup)
        // with MainMenu/prev on the stack. Redirect to a real RaceSetup.
        if (next && game_logic_game_mode() == 3) {
          InvObject* cur = game_logic_actual_state();
          const char* cn = cur ? tree_host_class(cur) : nullptr;
          const char* nn = tree_host_class(next);
          if (cn && std::strstr(cn, "Valocity") && nn &&
              std::strstr(nn, "MainMenu")) {
            InvObject* rs = game_logic_racesetup();
            if (!rs) {
              rs = tree_host_new("java.game.RaceSetup");
              game_logic_set_racesetup(rs);
            }
            tree_field_set_obj(rs, "lastState", cur);
            tree_field_set_obj(rs, "track", cur);
            next = rs;
          }
        }
        game_logic_change_active_section(next);
        return JvmValue::make_void();
      }
    }

    // Frontend.loadingScreen.show/hide — avoid nested LoadingScreen.run TREE.
    if (method && !args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
      const char* hc = tree_host_class(args[0].v.o);
      const bool is_ls =
          (hc && std::strstr(hc, "LoadingScreen")) ||
          (class_fqn && std::strstr(class_fqn, "LoadingScreen"));
      if (is_ls) {
        if (std::strcmp(method, "show") == 0) {
          frontend_loading_screen_show();
          return JvmValue::make_void();
        }
        if (std::strcmp(method, "hide") == 0) {
          frontend_loading_screen_hide();
          return JvmValue::make_void();
        }
      }
    }

    // LoadingScreen.userWait(float) — Java track + termSig.wait(). TREE often
    // names Object.userWait (termSig). Never Object.wait condvar (D3D9).
    if (method && std::strcmp(method, "userWait") == 0) {
      InvObject* ls = nullptr;
      float sec = -1.f;
      for (const auto& a : args) {
        if (a.tag == JvmTag::Obj && a.v.o) {
          const char* hc = tree_host_class(a.v.o);
          if (hc && std::strstr(hc, "LoadingScreen")) ls = a.v.o;
        } else if (a.tag == JvmTag::Float)
          sec = a.v.f;
        else if (a.tag == JvmTag::Int)
          sec = static_cast<float>(a.v.i);
      }
      if (!ls) {
        // Leftover termSig.wait after hide — do not re-show / block smoke.
        if (frontend_loading_screen_visible() == 0)
          return JvmValue::make_void();
        ls = frontend_loading_screen();
      }
      frontend_loading_screen_user_wait(ls, sec);
      return JvmValue::make_void();
    }

    // RenderRef alias leftover — neon / tesztmen (City night crowd).
    // TREE string "/" (club+"/"+ranking) packed as City./ argc=0.
    if (method && (std::strcmp(method, "neon") == 0 ||
                   std::strcmp(method, "tesztmen") == 0 ||
                   std::strcmp(method, "/") == 0) &&
        args.size() <= 1)
      return JvmValue::make_void();

    // Player.checkHint(int) — TREE packs argc=0 onto Garage.
    if (method && std::strcmp(method, "checkHint") == 0) {
      InvObject* player = nullptr;
      int32_t mask = 1;  // Player.H_GARAGE
      for (const auto& a : args) {
        if (a.tag == JvmTag::Obj && a.v.o) {
          const char* hc = tree_host_class(a.v.o);
          if (hc && std::strstr(hc, "Player") && !std::strstr(hc, "Setup"))
            player = a.v.o;
          else if (hc && std::strstr(hc, "Garage")) {
            player = tree_field_get_obj(a.v.o, "player");
          }
        } else if (a.tag == JvmTag::Int)
          mask = a.v.i;
      }
      if (!player) player = game_logic_player();
      if (!player) return JvmValue::make_int(1);
      const int32_t hints = tree_field_get_int(player, "hints");
      const int32_t shown = hints & mask;
      tree_field_set_int(player, "hints", hints | mask);
      return JvmValue::make_int(shown ? 0 : 1);
    }

    // player.controller.reset() stolen onto Garage (static 0x12 / empty hc).
    if (method && std::strcmp(method, "reset") == 0) {
      InvObject* self = nullptr;
      if (!args.empty() && args[0].tag == JvmTag::Obj) self = args[0].v.o;
      const char* hc = self ? tree_host_class(self) : nullptr;
      const bool named_garage =
          (class_fqn && std::strstr(class_fqn, "Garage")) ||
          (hc && std::strstr(hc, "Garage"));
      const bool is_ctrl =
          hc && (std::strstr(hc, "Controller") ||
                 std::strstr(hc, "ControlSet"));
      const bool is_player =
          hc && std::strstr(hc, "Player") && !std::strstr(hc, "Setup");
      const bool named_track =
          (class_fqn && (std::strstr(class_fqn, "Track") ||
                         std::strstr(class_fqn, "Valocity") ||
                         std::strstr(class_fqn, "City"))) ||
          (hc && (std::strstr(hc, "Track") || std::strstr(hc, "Valocity") ||
                  std::strstr(hc, "City")));
      bool has_v3 = false;
      for (const auto& a : args) {
        if (a.tag == JvmTag::Obj && a.v.o) {
          const char* ac = tree_host_class(a.v.o);
          if (ac && std::strstr(ac, "Vector3")) has_v3 = true;
        }
      }
      // ParkingCar.reset(Vector3) is a different Java method — leave it.
      if ((named_garage || named_track || is_ctrl || is_player) && !has_v3) {
        InvObject* ctrl = self;
        if (!is_ctrl) {
          InvObject* player = is_player ? self : nullptr;
          if (!player && self) player = tree_field_get_obj(self, "player");
          if (!player) player = game_logic_player();
          ctrl = player ? tree_field_get_obj(player, "controller") : nullptr;
        }
        if (ctrl) controller_reset(ctrl);
        return JvmValue::make_void();
      }
    }

    // Garage.osd.show() — TREE names Garage.show; osd field / hc often empty.
    if (method &&
        (std::strcmp(method, "show") == 0 || std::strcmp(method, "hide") == 0)) {
      InvObject* self = nullptr;
      if (!args.empty() && args[0].tag == JvmTag::Obj) self = args[0].v.o;
      const char* hc = self ? tree_host_class(self) : nullptr;
      const bool named_garage =
          (class_fqn && std::strstr(class_fqn, "Garage")) ||
          (hc && std::strstr(hc, "Garage"));
      const bool named_player =
          (class_fqn && std::strstr(class_fqn, "Player") &&
           !std::strstr(class_fqn, "Setup")) ||
          (hc && std::strstr(hc, "Player") && !std::strstr(hc, "Setup"));
      const bool named_city =
          (class_fqn && (std::strstr(class_fqn, "City") ||
                         std::strstr(class_fqn, "Valocity") ||
                         std::strstr(class_fqn, "Track"))) ||
          (hc && (std::strstr(hc, "City") || std::strstr(hc, "Valocity") ||
                  std::strstr(hc, "Track")));
      if (named_garage || named_player || named_city) {
        if (!self) self = game_logic_actual_state();
        InvObject* osd = self ? tree_field_get_obj(self, "osd") : nullptr;
        const char* oc = osd ? tree_host_class(osd) : nullptr;
        const bool osd_ok =
            osd && oc &&
            (oc[0] == '\0' ||
             (std::strstr(oc, "Osd") && !std::strstr(oc, "dialog")));
        if (!osd_ok) {
          osd = tree_host_new("java.render.Osd");
          if (self) tree_field_set_obj(self, "osd", osd);
          osd_ensure_defaults(osd);
        }
        const bool do_show = std::strcmp(method, "show") == 0;
        tree_field_set_int(osd, "visible", do_show ? 1 : 0);
        tree_field_set_int(osd, "shown", do_show ? 1 : 0);
        if (do_show) tree_field_set_int(osd, "init", 0);
        if (self) {
          tree_field_set_int(self, "shown", do_show ? 1 : 0);
          tree_field_set_int(self, "visible", do_show ? 1 : 0);
        }
        return JvmValue::make_void();
      }
    }

    // RaceSetup.deleteOSDObjects — TREE names Object.deleteOSDObjects.
    // CarInfo.deleteOSDObjects is empty.
    if (method && std::strcmp(method, "deleteOSDObjects") == 0) {
      InvObject* rs = nullptr;
      InvObject* click = nullptr;
      InvObject* track = nullptr;
      for (const auto& a : args) {
        if (a.tag != JvmTag::Obj || !a.v.o) continue;
        const char* ac = tree_host_class(a.v.o);
        if (ac && std::strstr(ac, "RaceSetup"))
          rs = a.v.o;
        else if (ac && (std::strstr(ac, "City") || std::strstr(ac, "Valocity") ||
                        std::strstr(ac, "Track")))
          track = a.v.o;
      }
      if (!rs && !args.empty() && args[0].tag == JvmTag::Obj) {
        InvObject* self = args[0].v.o;
        const char* hc = tree_host_class(self);
        if (hc && std::strstr(hc, "CarInfo")) return JvmValue::make_void();
        click = tree_field_get_obj(self, "click");
        if (!track) track = tree_field_get_obj(self, "track");
      }
      if (rs) {
        click = tree_field_get_obj(rs, "click");
        if (!track) track = tree_field_get_obj(rs, "track");
      }
      if (!track) {
        InvObject* st = game_logic_actual_state();
        const char* sc = st ? tree_host_class(st) : nullptr;
        if (sc && (std::strstr(sc, "City") || std::strstr(sc, "Valocity") ||
                   std::strstr(sc, "Track")))
          track = st;
      }
      if (click) java_util_resource_ResourceRef_destroy(click);
      InvObject* nav = track ? tree_field_get_obj(track, "nav") : nullptr;
      if (nav) {
        tree_field_set_float(nav, "vp_l", 0.02f);
        tree_field_set_float(nav, "vp_t", 0.78f);
        tree_field_set_float(nav, "vp_w", 0.2f);
        tree_field_set_float(nav, "vp_h", 0.18f);
        tree_field_set_float(nav, "offsetX", 0.f);
        tree_field_set_float(nav, "offsetZ", 0.f);
        tree_field_set_float(nav, "zoom", 4.5f);  // Navigator.DEF_ZOOM
        InvObject* cfg = system_config_host();
        const int32_t mode =
            cfg ? tree_field_get_int(cfg, "gpsMode") : 0;
        tree_field_set_int(nav, "mode", mode);
        InvObject* player = track ? tree_field_get_obj(track, "player") : nullptr;
        if (!player) player = game_logic_player();
        InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
        java_game_Navigator_updateNavigator(nav, car, mode);
      }
          return JvmValue::make_void();
        }

    // Viewport.activate(I)V @ 0x00481680. MouseCursor.enable → vp.activate(1).
    if (method && std::strcmp(method, "activate") == 0) {
      InvObject* self = nullptr;
      if (!args.empty() && args[0].tag == JvmTag::Obj) self = args[0].v.o;
      const char* hc = self ? tree_host_class(self) : nullptr;
      const bool is_cam =
          (class_fqn && std::strstr(class_fqn, "Camera")) ||
          (hc && std::strstr(hc, "Camera"));
      if (!is_cam) {
        InvObject* vp = self;
        const bool is_cursor =
            (class_fqn && std::strstr(class_fqn, "MouseCursor")) ||
            (hc && std::strstr(hc, "MouseCursor"));
        if (is_cursor) {
          if (!self) self = java_io_Input_cursor();
          vp = self ? tree_field_get_obj(self, "vp") : nullptr;
        } else if (hc && !std::strstr(hc, "Viewport") && self) {
          InvObject* nested = tree_field_get_obj(self, "vp");
          if (nested) vp = nested;
        }
        int32_t flags = 1;  // Viewport.RENDERFLAG_CLEARDEPTH
        for (size_t i = 1; i < args.size(); ++i) {
          if (args[i].tag == JvmTag::Int) flags = args[i].v.i;
        }
        if (vp) java_render_Viewport_activate(vp, flags);
          return JvmValue::make_void();
        }
    }

    // MouseCursor.cleanup — particle walk; TREE names Object.cleanup.
    if (method && std::strcmp(method, "cleanup") == 0)
          return JvmValue::make_void();

    // MouseCursor.disableCameraControl — Java; TREE names Object.
    if (method && std::strcmp(method, "disableCameraControl") == 0) {
      InvObject* cur = nullptr;
      if (!args.empty() && args[0].tag == JvmTag::Obj) cur = args[0].v.o;
      const char* hc = cur ? tree_host_class(cur) : nullptr;
      if (!cur || (hc && !std::strstr(hc, "MouseCursor")))
        cur = java_io_Input_cursor();
      if (cur) {
        InvObject* cam = tree_field_get_obj(cur, "controlledcam");
        if (cam)
          java_util_resource_GameRef_queueEvent(
              cam, nullptr, 0x10, string_new("deactivate"));
        tree_field_set_obj(cur, "controlledcam", nullptr);
      }
          return JvmValue::make_void();
        }

    // GameType.remNotification(GameRef,int) @ 0x0047E000.
    if (method && std::strcmp(method, "remNotification") == 0) {
      InvObject* handler = nullptr;
      InvObject* ref = nullptr;
      int32_t etype = 0x10000;  // GameRef.EVENT_CURSOR
      for (const auto& a : args) {
        if (a.tag == JvmTag::Obj && a.v.o) {
          const char* ac = tree_host_class(a.v.o);
          if (!handler) handler = a.v.o;
          else if (!ref && ac &&
                   (std::strstr(ac, "GameRef") || std::strstr(ac, "Cursor") ||
                    ac[0] == '\0'))
            ref = a.v.o;
        } else if (a.tag == JvmTag::Int)
          etype = a.v.i;
      }
      const char* hc = handler ? tree_host_class(handler) : nullptr;
      if ((class_fqn && std::strstr(class_fqn, "MouseCursor")) ||
          (hc && std::strstr(hc, "MouseCursor")) ||
          (class_fqn && std::strstr(class_fqn, "Object"))) {
        if (!ref && handler) ref = tree_field_get_obj(handler, "cursor");
        InvObject* st = game_logic_actual_state();
        if (st) handler = st;
      }
      if (!ref) {
        InvObject* cur = java_io_Input_cursor();
        ref = cur ? tree_field_get_obj(cur, "cursor") : cur;
      }
      if (handler)
        java_lang_GameType_remNotification(handler, ref, etype);
          return JvmValue::make_void();
        }

    // Controller.activateState(group[, state]) — TREE packs argc=0 onto Track.
    if (method && std::strcmp(method, "activateState") == 0) {
      InvObject* self = nullptr;
      if (!args.empty() && args[0].tag == JvmTag::Obj) self = args[0].v.o;
      const char* hc = self ? tree_host_class(self) : nullptr;
      int32_t group = 2;  // ControlSet.MENUSET
      int32_t stv = 1;
      std::vector<int32_t> ints;
      for (size_t i = 1; i < args.size(); ++i) {
        if (args[i].tag == JvmTag::Int) ints.push_back(args[i].v.i);
      }
      if (ints.size() >= 1) group = ints[0];
      if (ints.size() >= 2) stv = ints[1];
      InvObject* ctrl = self;
      const bool is_ctrl =
          hc && (std::strstr(hc, "Controller") ||
                 std::strstr(hc, "ControlSet"));
      if (!is_ctrl) {
        InvObject* player = nullptr;
        if (hc && std::strstr(hc, "Player") && !std::strstr(hc, "Setup"))
          player = self;
        if (!player && self) player = tree_field_get_obj(self, "player");
        if (!player) player = game_logic_player();
        ctrl = player ? tree_field_get_obj(player, "controller") : nullptr;
      }
      if (ctrl) controller_activate_state(ctrl, group, stv);
          return JvmValue::make_void();
        }

    // Mechanic.flushInventory — TREE names Garage.flushInventory.
    if (method && std::strcmp(method, "flushInventory") == 0) {
      InvObject* self = nullptr;
      if (!args.empty() && args[0].tag == JvmTag::Obj) self = args[0].v.o;
      if (!self) self = game_logic_actual_state();
      const char* hc = self ? tree_host_class(self) : nullptr;
      InvObject* mech = self;
      if (hc && !std::strstr(hc, "Mechanic"))
        mech = self ? tree_field_get_obj(self, "mechanic") : nullptr;
      mechanic_flush_inventory(mech);
          return JvmValue::make_void();
        }

    // VisualInventory.currentLine — TREE names Garage/PaintCans.
    if (method && std::strcmp(method, "currentLine") == 0) {
      InvObject* self = nullptr;
      if (!args.empty() && args[0].tag == JvmTag::Obj) self = args[0].v.o;
      if (!self) self = game_logic_actual_state();
      InvObject* cans = self;
      const char* hc = self ? tree_host_class(self) : nullptr;
      if (hc && std::strstr(hc, "Garage")) {
        InvObject* painter = tree_field_get_obj(self, "painter");
        cans = painter ? tree_field_get_obj(painter, "paintCans") : nullptr;
      }
      return JvmValue::make_int(cans ? tree_field_get_int(cans, "cline") : 0);
    }

    // Track.enableOsd(int) — TREE packs argc=0 onto Valocity.
    if (method && std::strcmp(method, "enableOsd") == 0) {
      InvObject* self = nullptr;
      if (!args.empty() && args[0].tag == JvmTag::Obj) self = args[0].v.o;
      if (!self) self = game_logic_actual_state();
      int32_t enable = 1;
      for (size_t i = 1; i < args.size(); ++i) {
        if (args[i].tag == JvmTag::Int) enable = args[i].v.i;
      }
      int32_t c = self ? tree_field_get_int(self, "osdCounter") : 0;
      InvObject* player = self ? tree_field_get_obj(self, "player") : nullptr;
      if (!player) player = game_logic_player();
      InvObject* posd = player ? tree_field_get_obj(player, "osd") : nullptr;
      InvObject* nav = self ? tree_field_get_obj(self, "nav") : nullptr;
      if (enable) {
        ++c;
        if (c == 1) {
          if (posd) {
            tree_field_set_int(posd, "visible", 1);
            tree_field_set_int(posd, "shown", 1);
          }
          if (nav) tree_field_set_int(nav, "visible", 1);
        }
      } else {
        --c;
        if (c <= 0) {
          c = 0;
          if (posd) {
            tree_field_set_int(posd, "visible", 0);
            tree_field_set_int(posd, "shown", 0);
          }
          if (nav) tree_field_set_int(nav, "visible", 0);
        }
      }
      if (self) tree_field_set_int(self, "osdCounter", c);
          return JvmValue::make_void();
        }

    // GameRef.command(String) → queueEvent(null, EVENT_COMMAND, param).
    // TREE names the command itself (SfxRef."filter 2").
    if (method && std::strchr(method, ' ')) {
      InvObject* self = nullptr;
      if (!args.empty() && args[0].tag == JvmTag::Obj) self = args[0].v.o;
      self = queue_event_retarget(self);
      if (self)
        java_util_resource_GameRef_queueEvent(self, nullptr, 0x10,
                                              string_new(method));
      return JvmValue::make_void();
    }

    // GfxEngine.flush()V @ 0x0047C2D0 — Track.exit; TREE names Valocity.flush.
    if (method && std::strcmp(method, "flush") == 0) {
      InvObject* self = nullptr;
      if (!args.empty() && args[0].tag == JvmTag::Obj) self = args[0].v.o;
      const char* hc = self ? tree_host_class(self) : nullptr;
      const bool gfx =
          prefer_static ||
          (class_fqn && (std::strstr(class_fqn, "GfxEngine") ||
                         std::strstr(class_fqn, "Valocity") ||
                         std::strstr(class_fqn, "Track") ||
                         std::strstr(class_fqn, "City") ||
                         (std::strstr(class_fqn, "Object") &&
                          !std::strstr(class_fqn, "Game")))) ||
          (hc && (std::strstr(hc, "GfxEngine") || std::strstr(hc, "Valocity") ||
                  std::strstr(hc, "Track") || std::strstr(hc, "City") ||
                  (std::strstr(hc, "Object") && !std::strstr(hc, "Game"))));
      if (gfx) {
        java_render_GfxEngine_flush();
        return JvmValue::make_void();
      }
    }

    // System.log(String)V @ 0x0047BE80 — Dialog ctor; TREE names Dialog.log.
    if (method && std::strcmp(method, "log") == 0) {
      InvObject* s = nullptr;
      for (const auto& a : args) {
        if (a.tag == JvmTag::Obj && queue_event_is_string(a.v.o)) {
          s = a.v.o;
          break;
        }
      }
      if (s) java_lang_System_log(s);
      return JvmValue::make_void();
    }

    // GameState.osdCommand(int) — TREE names Object.osdCommand argc=0.
    if (method && std::strcmp(method, "osdCommand") == 0) {
      InvObject* self = nullptr;
      if (!args.empty() && args[0].tag == JvmTag::Obj) self = args[0].v.o;
      const char* hc = self ? tree_host_class(self) : nullptr;
      int32_t cmd = 0;
      bool have_cmd = false;
      for (size_t i = 1; i < args.size(); ++i) {
        if (args[i].tag == JvmTag::Int) {
          cmd = args[i].v.i;
          have_cmd = true;
        }
      }
      const bool stolen =
          (class_fqn && std::strstr(class_fqn, "Object") &&
           !std::strstr(class_fqn, "Game")) ||
          (hc && std::strstr(hc, "Object") && !std::strstr(hc, "Game")) ||
          !have_cmd;
      if (stolen) {
        if (have_cmd) {
          InvObject* st = game_logic_actual_state();
          const char* sc = st ? tree_host_class(st) : nullptr;
          if (sc && std::strstr(sc, "Garage"))
            garage_osd_command(st, cmd);
          else if (sc && std::strstr(sc, "Mechanic"))
            mechanic_osd_command(st, cmd);
          else if (st)
            tree_field_set_int(st, "last_osd_cmd", cmd);
        }
        return JvmValue::make_void();
      }
    }

    // CarMarket.changePointer — TREE names Object.changePointer.
    if (method && std::strcmp(method, "changePointer") == 0) {
      InvObject* self = nullptr;
      if (!args.empty() && args[0].tag == JvmTag::Obj) self = args[0].v.o;
      if (!self) self = game_logic_actual_state();
      const int32_t move = self ? tree_field_get_int(self, "move") : 0;
      InvObject* cur = java_io_Input_cursor();
      if (cur) {
        const char* ch = move ? "M" : "J";
        tree_field_set_obj(cur, "pointer", string_new(ch));
        InvObject* inner = tree_field_get_obj(cur, "cursor");
        if (inner)
          java_util_resource_GameRef_queueEvent(
              inner, nullptr, 0x10, string_new(move ? "mode 0 M" : "mode 0 J"));
      }
      return JvmValue::make_void();
    }

    // Garage.giveWarning(title, text) — TREE names Catalog.giveWarning.
    if (method && std::strcmp(method, "giveWarning") == 0) {
      const char* title = "WARNING";
      const char* text = "";
      std::vector<const char*> strs;
      for (const auto& a : args) {
        if (a.tag == JvmTag::Obj && queue_event_is_string(a.v.o)) {
          if (const char* s = string_cstr(a.v.o)) {
            if (s[0]) strs.push_back(s);
          }
        }
      }
      if (strs.size() >= 2) {
        title = strs[0];
        text = strs[1];
      } else if (strs.size() == 1)
        text = strs[0];
      InvObject* dlg = tree_host_new("java.render.osd.dialog.WarningDialog");
      tree_field_set_int(dlg, "flags", 0x00000005);  // DF_MODAL|DF_DEFAULTBG
      tree_field_set_obj(dlg, "title", string_new(title));
      tree_field_set_obj(dlg, "text", string_new(text));
      InvObject* player = game_logic_player();
      if (player)
        tree_field_set_obj(dlg, "controller",
                           tree_field_get_obj(player, "controller"));
      dialog_display(dlg);
      return JvmValue::make_void();
    }

    // MessagePort.putMessage — Track.osdCommand; TREE names GameType.putMessage.
    if (method && std::strcmp(method, "putMessage") == 0) {
      InvObject* port = nullptr;
      InvObject* msg = nullptr;
      for (const auto& a : args) {
        if (a.tag != JvmTag::Obj || !a.v.o) continue;
        const char* ac = tree_host_class(a.v.o);
        if (ac && std::strstr(ac, "MessagePort"))
          port = a.v.o;
        else if (!msg)
          msg = a.v.o;
      }
      if (!port && !args.empty() && args[0].tag == JvmTag::Obj) {
        InvObject* self = args[0].v.o;
        port = tree_field_get_obj(self, "mp");
        if (!port) port = self;
      }
      if (port) {
        InvObject* fifo = tree_field_get_obj(port, "fifo");
        if (!fifo || !tree_vector_is(fifo)) {
          fifo = tree_vector_new();
          tree_field_set_obj(port, "fifo", fifo);
        }
        if (msg) tree_vector_add(fifo, msg);
      }
      return JvmValue::make_void();
    }

    // Thread.setPriority — Track.enter t.setPriority; TREE names Track.setPriority.
    if (method && std::strcmp(method, "setPriority") == 0) {
      InvObject* th = nullptr;
      if (!args.empty() && args[0].tag == JvmTag::Obj) th = args[0].v.o;
      const char* hc = th ? tree_host_class(th) : nullptr;
      if (th && hc && !std::strstr(hc, "Thread"))
        th = tree_field_get_obj(th, "t");
      int32_t pri = 10;  // Thread.MAX_PRIORITY
      for (size_t i = 1; i < args.size(); ++i) {
        if (args[i].tag == JvmTag::Int) pri = args[i].v.i;
      }
      if (th) java_lang_Thread_setPriority(th, pri);
      return JvmValue::make_void();
    }

    // GameRef.setPos(Vector3)V @ 0x0047E350 — Garage.lockCar; TREE argc=0.
    // Do not steal Text.setPos(FF) or WheelRef.setPos.
    if (method && std::strcmp(method, "setPos") == 0) {
      InvObject* self = nullptr;
      InvObject* vec = nullptr;
      bool two_floats = false;
      int nfloat = 0;
      for (const auto& a : args) {
        if (a.tag == JvmTag::Obj && a.v.o) {
          const char* ac = tree_host_class(a.v.o);
          if (ac && std::strstr(ac, "Vector3"))
            vec = a.v.o;
          else if (vec3_is(a.v.o) && (!ac || !ac[0]))
            vec = a.v.o;
          else if (ac && std::strstr(ac, "Text"))
            two_floats = true;
          else if (ac && std::strstr(ac, "WheelRef"))
            two_floats = true;
          else if (!self)
            self = a.v.o;
        } else if (a.tag == JvmTag::Float || a.tag == JvmTag::Int)
          ++nfloat;
      }
      if (two_floats || nfloat >= 2) {
        // Text.setPos — native @ 0x004870D0.
      } else {
        const char* hc = self ? tree_host_class(self) : nullptr;
        const char* named = class_fqn;
        const bool skip =
            (named && (std::strstr(named, "Text") ||
                       std::strstr(named, "WheelRef"))) ||
            (hc && (std::strstr(hc, "Text") || std::strstr(hc, "WheelRef")));
        if (!skip) {
          if (!self) {
            InvObject* p = game_logic_player();
            self = p ? tree_field_get_obj(p, "car") : nullptr;
          }
          if (!vec) {
            InvObject* st = game_logic_actual_state();
            const char* sc = st ? tree_host_class(st) : nullptr;
            if (sc && std::strstr(sc, "Garage"))
              vec = vec3_new(0.f, 0.f, -0.5f);  // Garage.defCarPos
          }
          if (self && vec) java_util_resource_GameRef_setPos(self, vec);
          return JvmValue::make_void();
        }
      }
    }

    // Track.changeCamTarget(GameRef) — Java only; TREE argc=0 onto Track/City.
    if (method && (std::strcmp(method, "changeCamTarget") == 0 ||
                   std::strcmp(method, "changeCamTarget2") == 0)) {
      const bool tgt2 = std::strcmp(method, "changeCamTarget2") == 0;
      InvObject* self = nullptr;
      InvObject* obj = nullptr;
      auto is_track = [](const char* c) -> bool {
        return c && (std::strstr(c, "Track") || std::strstr(c, "City") ||
                     std::strstr(c, "Valocity"));
      };
      auto is_cam_obj = [](InvObject* o) -> bool {
        if (!o) return false;
        const char* c = tree_host_class(o);
        if (!c || !c[0]) return false;
        if (std::strstr(c, "Vector3") || std::strstr(c, "Track") ||
            std::strstr(c, "City") || std::strstr(c, "Valocity") ||
            std::strstr(c, "Bot") || std::strstr(c, "Player"))
          return false;
        return std::strstr(c, "Vehicle") || std::strstr(c, "GameRef") ||
               std::strstr(c, "RenderRef");
      };
      for (const auto& a : args) {
        if (a.tag != JvmTag::Obj || !a.v.o) continue;
        const char* ac = tree_host_class(a.v.o);
        if (is_track(ac) && !self)
          self = a.v.o;
        else if (is_cam_obj(a.v.o) && !obj)
          obj = a.v.o;
      }
      if (!self && !args.empty() && args[0].tag == JvmTag::Obj)
        self = args[0].v.o;
      const char* hc = self ? tree_host_class(self) : nullptr;
      const bool packed_tree = is_track(hc) && obj != nullptr;
      if (!packed_tree) {
        if (!is_track(hc)) {
          if (self) {
            InvObject* tr = tree_field_get_obj(self, "track");
            if (tr) self = tr;
          }
          if (!is_track(self ? tree_host_class(self) : nullptr))
            self = game_logic_actual_state();
        }
        if (!obj) {
          if (tgt2) {
            InvObject* bot = self ? tree_field_get_obj(self, "raceBot") : nullptr;
            obj = bot ? tree_field_get_obj(bot, "car") : nullptr;
          } else {
            InvObject* p = self ? tree_field_get_obj(self, "player") : nullptr;
            if (!p) p = game_logic_player();
            obj = p ? tree_field_get_obj(p, "car") : nullptr;
          }
        }
        if (self)
          tree_field_set_obj(self, tgt2 ? "cameraTarget2" : "cameraTarget",
                             obj);
        return JvmValue::make_void();
      }
    }

    // Bot.createCar — TREE names Valocity.createCar (prefer_static 0x12).
    if (method && std::strcmp(method, "createCar") == 0) {
      InvObject* self = nullptr;
      if (!args.empty() && args[0].tag == JvmTag::Obj) self = args[0].v.o;
      const char* hc = self ? tree_host_class(self) : nullptr;
      const bool named_bot =
          (class_fqn && std::strstr(class_fqn, "Bot")) ||
          (hc && std::strstr(hc, "Bot"));
      const bool named_vhc =
          (class_fqn && std::strstr(class_fqn, "Vehicle")) ||
          (hc && std::strstr(hc, "Vehicle"));
      const bool stolen =
          (class_fqn && (std::strstr(class_fqn, "Valocity") ||
                         std::strstr(class_fqn, "City") ||
                         std::strstr(class_fqn, "Track") ||
                         (std::strstr(class_fqn, "Object") &&
                          !std::strstr(class_fqn, "Game")))) ||
          (hc && (std::strstr(hc, "Valocity") || std::strstr(hc, "City") ||
                  std::strstr(hc, "Track")));
      auto ok_bot = [&](InvObject* o) -> bool {
        if (!o) return false;
        const char* c = tree_host_class(o);
        if (!c || !c[0]) return true;
        return std::strstr(c, "Bot") != nullptr;
      };
      if (!named_bot && (named_vhc || stolen)) {
        InvObject* bot = nullptr;
        InvObject* map = nullptr;
        InvObject* car = named_vhc ? self : nullptr;
        for (const auto& a : args) {
          if (a.tag != JvmTag::Obj || !a.v.o) continue;
          InvObject* o = a.v.o;
          const char* ac = tree_host_class(o);
          if (ac && std::strstr(ac, "Bot"))
            bot = o;
          else if (ac && std::strstr(ac, "GroundRef"))
            map = o;
          else if (ac && std::strstr(ac, "Vehicle") && o != self)
            car = o;
          else if (ac && !std::strstr(ac, "Valocity") &&
                   !std::strstr(ac, "City") && !std::strstr(ac, "Track") &&
                   !map)
            map = o;
        }
        if (!ok_bot(bot)) {
          InvObject* st = self;
          if (!st || !tree_field_get_obj(st, "raceBot"))
            st = game_logic_actual_state();
          bot = st ? tree_field_get_obj(st, "raceBot") : nullptr;
          if (!ok_bot(bot) && st)
            bot = tree_field_get_obj(st, "demoBot");
        }
        if (ok_bot(bot)) {
          if (!tree_field_get_obj(bot, "botVd"))
            tree_field_set_obj(bot, "botVd",
                               game_logic_get_vehicle_descriptor(1, -1.f));
          std::vector<JvmValue> a = {JvmValue::make_obj(bot)};
          if (map) a.push_back(JvmValue::make_obj(map));
          if (car) a.push_back(JvmValue::make_obj(car));
          return call_by_name("java.game.Bot", "createCar", a, false);
        }
        return JvmValue::make_void();
      }
    }

    // Vehicle.setTransmission / setDefault* → queueEvent EVENT_COMMAND @ 0x0047DA30.
    if (method && (std::strcmp(method, "setTransmission") == 0 ||
                   std::strcmp(method, "setDefaultTransmission") == 0 ||
                   std::strcmp(method, "setDefaultSteeringHelp") == 0 ||
                   std::strcmp(method, "setDefaultASR") == 0 ||
                   std::strcmp(method, "setDefaultABS") == 0)) {
      InvObject* car = nullptr;
      InvObject* bot = nullptr;
      int32_t tr = 0;
      bool have_tr = false;
      for (const auto& a : args) {
        if (a.tag == JvmTag::Obj && a.v.o) {
          const char* ac = tree_host_class(a.v.o);
          if (ac && std::strstr(ac, "Vehicle"))
            car = a.v.o;
          else if (ac && std::strstr(ac, "Bot"))
            bot = a.v.o;
        } else if (a.tag == JvmTag::Int) {
          tr = a.v.i;
          have_tr = true;
        }
      }
      if (!car && bot) car = tree_field_get_obj(bot, "car");
      if (!car) {
        InvObject* p = game_logic_player();
        car = p ? tree_field_get_obj(p, "car") : nullptr;
      }
      InvObject* cfg = system_config_host();
      char cmd[64];
      cmd[0] = 0;
      if (std::strcmp(method, "setTransmission") == 0 ||
          std::strcmp(method, "setDefaultTransmission") == 0) {
        if (!have_tr) {
          const bool botish =
              (class_fqn && std::strstr(class_fqn, "Bot")) || bot;
          if (botish && std::strcmp(method, "setTransmission") == 0)
            tr = 5;  // Vehicle.TRANSMISSION_SEMIAUTO
          else
            tr = cfg ? tree_field_get_int(cfg, "player_transmission") : 1;
        }
        std::snprintf(cmd, sizeof(cmd), "transmission %d", tr);
      } else if (std::strcmp(method, "setDefaultSteeringHelp") == 0) {
        const float v =
            cfg ? tree_field_get_float(cfg, "player_steeringhelp") : 0.666f;
        std::snprintf(cmd, sizeof(cmd), "steerhelp %g", v);
      } else if (std::strcmp(method, "setDefaultASR") == 0) {
        const float v = cfg ? tree_field_get_float(cfg, "player_asr") : 0.f;
        std::snprintf(cmd, sizeof(cmd), "asr %g", v);
      } else {
        const float v = cfg ? tree_field_get_float(cfg, "player_abs") : 0.f;
        std::snprintf(cmd, sizeof(cmd), "abs %g", v);
      }
      if (car && cmd[0])
        java_util_resource_GameRef_queueEvent(car, nullptr, 0x10,
                                              string_new(cmd));
      return JvmValue::make_void();
    }

    // Bot.leaveCar(int) — TREE names Object.leaveCar argc=0.
    if (method && std::strcmp(method, "leaveCar") == 0) {
      InvObject* bot = nullptr;
      int32_t leave_in_traffic = 0;
      for (const auto& a : args) {
        if (a.tag == JvmTag::Obj && a.v.o) {
          const char* ac = tree_host_class(a.v.o);
          if (ac && std::strstr(ac, "Bot")) bot = a.v.o;
        } else if (a.tag == JvmTag::Int)
          leave_in_traffic = a.v.i;
      }
      if (!bot) {
        InvObject* st = game_logic_actual_state();
        bot = st ? tree_field_get_obj(st, "raceBot") : nullptr;
      }
      if (bot) {
        InvObject* car = tree_field_get_obj(bot, "car");
        InvObject* brain = tree_field_get_obj(bot, "brain");
        InvObject* render = tree_field_get_obj(bot, "render");
        if (render) java_util_resource_ResourceRef_destroy(render);
        if (brain && car) {
          char cmd[48];
          std::snprintf(cmd, sizeof(cmd), "leave %d",
                        java_util_resource_ResourceRef_id(car));
          java_util_resource_GameRef_queueEvent(brain, nullptr, 0x10,
                                                string_new(cmd));
          java_util_resource_ResourceRef_destroy(brain);
        }
        tree_field_set_obj(bot, "render", nullptr);
        tree_field_set_obj(bot, "brain", nullptr);
        tree_field_set_obj(bot, "controller", nullptr);
        if (leave_in_traffic) tree_field_set_obj(bot, "car", nullptr);
      }
      return JvmValue::make_void();
    }

    // Bot.constructName(int) — TREE names Object.constructName argc=0.
    if (method && std::strcmp(method, "constructName") == 0) {
      static const char* kFirst[] = {
          "John ",  "David ", "Bill ",  "Stewart ", "Joe ",  "Sam ",
          "Alan ",  "Marc ",  "Jason ", "Sean ",    "Tony ", "Leo "};
      static const char* kLast[] = {
          "Galahad",     "Butterfly", "Robertson", "Cocker",   "Johnson",
          "Livingstone", "Dunnigan",  "Little",    "Luciano",  "Evans",
          "Murphy",      "Speaker",   "Sterkovic", "Scott",    "McDonell",
          "Bonnett",     "Bakers",    "Perkins",   "Olson",    "Polansky",
          "O'Connor",    "Kozak"};
      int32_t seed = 0;
      bool have_seed = false;
      InvObject* bot = nullptr;
      for (const auto& a : args) {
        if (a.tag == JvmTag::Int) {
          seed = a.v.i;
          have_seed = true;
        } else if (a.tag == JvmTag::Obj && a.v.o) {
          const char* ac = tree_host_class(a.v.o);
          if (ac && std::strstr(ac, "Bot")) bot = a.v.o;
        }
      }
      if (!have_seed && bot) seed = tree_field_get_int(bot, "seed");
      const char* first = kFirst[(seed * 19) % 12];
      const char* last = kLast[(seed * 23) % 22];
      const char* post = ((seed * 17) % 20 == 0) ? " jr." : "";
      char buf[96];
      std::snprintf(buf, sizeof(buf), "%s%s%s", first, last, post);
      return JvmValue::make_obj(string_new(buf));
    }

    // ResourceRef.set(I)/(ResourceRef) @ 0x0047CF80 / 0x0047CFC0.
    // TREE names Object.set argc=0.
    if (method && std::strcmp(method, "set") == 0) {
      const bool named_ok =
          !class_fqn || std::strstr(class_fqn, "Object") ||
          std::strstr(class_fqn, "ResourceRef") ||
          std::strstr(class_fqn, "Vehicle") ||
          std::strstr(class_fqn, "GameRef") ||
          std::strstr(class_fqn, "GameType") ||
          std::strstr(class_fqn, "RenderRef");
      if (named_ok && !(class_fqn && std::strstr(class_fqn, "Config"))) {
        InvObject* self = nullptr;
        InvObject* ref = nullptr;
        int32_t id = 0;
        bool have_id = false;
        for (const auto& a : args) {
          if (a.tag == JvmTag::Obj && a.v.o) {
            const char* ac = tree_host_class(a.v.o);
            if (ac && std::strstr(ac, "Vector3")) continue;
            if (!self)
              self = a.v.o;
            else if (!ref)
              ref = a.v.o;
          } else if (a.tag == JvmTag::Int) {
            id = a.v.i;
            have_id = true;
          }
        }
        if (self && have_id)
          java_util_resource_ResourceRef_set(self, id);
        else if (self && ref)
          java_util_resource_ResourceRef_set_1(self, ref);
        return JvmValue::make_void();
      }
    }

    // Thin FindFile.first(String) → native first(path, FILES_DIRS=0).
    std::vector<JvmValue> args_storage;
    const std::vector<JvmValue>* pargs = &args;
    if (!prefer_static && method && std::strcmp(method, "first") == 0 &&
        args.size() == 2) {
      args_storage = args;
      args_storage.push_back(JvmValue::make_int(0));
      pargs = &args_storage;
    }

    // Array classes ([Ljava.lang.String; …): runtime vectors, no .class file.
    if (class_fqn && class_fqn[0] == '[') {
      if (method && std::strcmp(method, "<init>") == 0) {
        InvObject* self =
            (!prefer_static && !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj)
                ? (*pargs)[0].v.o
                : nullptr;
        int32_t n = 0;
        if (pargs->size() >= (prefer_static ? 1u : 2u)) {
          const JvmValue& a = (*pargs)[prefer_static ? 0 : 1];
          if (a.tag == JvmTag::Int) n = a.v.i;
        }
        if (self && n > 0) tree_vector_resize(self, n);
        return JvmValue::make_void();
      }
      return JvmValue::make_void();
    }

    // java.util.Vector has no classpath .class — runtime g_vectors bag.
    // TREE often names the owner as the element type (VehicleModel.addElement)
    // when recv/arg order is flipped; pick the real Vector handle.
    if (!prefer_static && method && !pargs->empty()) {
      const bool is_add = std::strcmp(method, "addElement") == 0;
      const bool is_at = std::strcmp(method, "elementAt") == 0;
      const bool is_sz = std::strcmp(method, "size") == 0;
      const bool is_rm = std::strcmp(method, "removeElement") == 0;
      const bool is_rmall = std::strcmp(method, "removeAllElements") == 0;
      const bool is_last = std::strcmp(method, "lastElement") == 0;
      const bool is_empty = std::strcmp(method, "isEmpty") == 0;
      if (is_add || is_at || is_sz || is_rm || is_rmall || is_last ||
          is_empty) {
        InvObject* a0 =
            ((*pargs)[0].tag == JvmTag::Obj) ? (*pargs)[0].v.o : nullptr;
        InvObject* a1 = (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Obj)
                            ? (*pargs)[1].v.o
                            : nullptr;
        InvObject* vec = nullptr;
        InvObject* elem = nullptr;
        int32_t idx = 0;
        if (a0 && tree_vector_is(a0)) {
          vec = a0;
          if (pargs->size() >= 2) {
            if ((*pargs)[1].tag == JvmTag::Obj) elem = (*pargs)[1].v.o;
            else if ((*pargs)[1].tag == JvmTag::Int) idx = (*pargs)[1].v.i;
          }
        } else if (a1 && tree_vector_is(a1)) {
          vec = a1;
          elem = a0;
        }
        if (vec) {
          if (is_add) {
            tree_vector_add(vec, elem);
            return JvmValue::make_void();
          }
          if (is_sz) return JvmValue::make_int(tree_vector_size(vec));
          if (is_empty)
            return JvmValue::make_int(tree_vector_size(vec) == 0 ? 1 : 0);
          if (is_last) {
            const int32_t n = tree_vector_size(vec);
            return JvmValue::make_obj(
                n > 0 ? tree_vector_element_at(vec, n - 1) : nullptr);
          }
          if (is_at) {
            if (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Int)
              idx = (*pargs)[1].v.i;
            return JvmValue::make_obj(tree_vector_element_at(vec, idx));
          }
          if (is_rm) {
            tree_vector_remove(vec, elem);
            return JvmValue::make_void();
          }
          if (is_rmall) {
            tree_vector_resize(vec, 0);
            return JvmValue::make_void();
          }
        }
      }
    }

    // Osd UI leaves — TREE packing SO / wrong owner (Dialog named instead of Osd).
    if (!prefer_static && method && !pargs->empty() &&
        (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      const char* hc = tree_host_class(self);
      const bool self_is_osd =
          (hc && std::strstr(hc, "Osd") && !std::strstr(hc, "dialog")) ||
          (class_fqn && std::strstr(class_fqn, ".Osd") &&
           !std::strstr(class_fqn, "dialog"));
      const bool self_is_menu =
          (hc && std::strstr(hc, "Menu") && !std::strstr(hc, "MainMenu")) ||
          (class_fqn && std::strstr(class_fqn, ".Menu"));
      const bool osd_method =
          std::strcmp(method, "createBG") == 0 ||
          std::strcmp(method, "createHotkey") == 0 ||
          std::strcmp(method, "createHeader") == 0 ||
          std::strcmp(method, "createMenu") == 0 ||
          std::strcmp(method, "createText") == 0 ||
          std::strcmp(method, "createButton") == 0 ||
          std::strcmp(method, "createRectangle") == 0 ||
          std::strcmp(method, "beginGroup") == 0 ||
          std::strcmp(method, "endGroup") == 0 ||
          std::strcmp(method, "hideGroup") == 0 ||
          std::strcmp(method, "showGroup") == 0 ||
          std::strcmp(method, "show") == 0 ||
          std::strcmp(method, "hide") == 0 ||
          std::strcmp(method, "removeAllElements") == 0 ||
          std::strcmp(method, "<init>") == 0;
      const bool menu_method =
          std::strcmp(method, "addItem") == 0 ||
          std::strcmp(method, "addSeparator") == 0 ||
          std::strcmp(method, "setSliderStyle") == 0;

      auto is_osd_obj = [](InvObject* o) -> bool {
        const char* c = o ? tree_host_class(o) : nullptr;
        if (!c) return false;
        if (!c[0]) return true;  // new Osd() often has empty host-class
        return std::strstr(c, "Osd") && !std::strstr(c, "dialog");
      };
      auto resolve_osd = [&]() -> InvObject* {
        if (self_is_osd) return self;
        if (InvObject* nested = tree_field_get_obj(self, "osd")) {
          if (is_osd_obj(nested)) return nested;
        }
        if (class_fqn && std::strstr(class_fqn, "Osd") &&
            !std::strstr(class_fqn, "dialog"))
          return self;
        return nullptr;
      };

      if (osd_method) {
        InvObject* osd = resolve_osd();
        if (!osd &&
            ((hc && (std::strstr(hc, "Valocity") || std::strstr(hc, "City") ||
                     std::strstr(hc, "Track") || std::strstr(hc, "RaceSetup") ||
                     std::strstr(hc, "Garage") || std::strstr(hc, "Dialog"))) ||
             (class_fqn && (std::strstr(class_fqn, "Valocity") ||
                            std::strstr(class_fqn, "Garage") ||
                            std::strstr(class_fqn, "RaceSetup"))))) {
          osd = tree_host_new("java.render.Osd");
          tree_field_set_obj(self, "osd", osd);
          osd_ensure_defaults(osd);
        }
        if (osd && std::strcmp(method, "createBG") == 0) {
          InvObject* tex = nullptr;
          if (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Obj)
            tex = (*pargs)[1].v.o;
          if (!tex && !self_is_osd && self != osd) tex = self;
          return JvmValue::make_obj(osd_create_bg(osd, tex));
        }
        if (osd && std::strcmp(method, "createRectangle") == 0) {
          float x = 0.f, y = 0.f, w = 2.f, h = 2.f;
          int32_t pri = -1;
          InvObject* tex = nullptr;
          std::vector<float> floats;
          std::vector<int32_t> ints;
          for (size_t i = 1; i < pargs->size(); ++i) {
            const JvmValue& a = (*pargs)[i];
            if (a.tag == JvmTag::Obj && a.v.o) {
              const char* ac = tree_host_class(a.v.o);
              if (ac && (std::strstr(ac, "ResourceRef") ||
                         std::strstr(ac, "GameRef")))
                tex = a.v.o;
            } else if (a.tag == JvmTag::Float)
              floats.push_back(a.v.f);
            else if (a.tag == JvmTag::Int)
              ints.push_back(a.v.i);
          }
          if (floats.size() >= 1) x = floats[0];
          if (floats.size() >= 2) y = floats[1];
          if (floats.size() >= 3) w = floats[2];
          if (floats.size() >= 4) h = floats[3];
          if (!ints.empty()) pri = ints.back();
          return JvmValue::make_obj(
              osd_create_rectangle(osd, x, y, w, h, pri, tex));
        }
        if (osd && (std::strcmp(method, "show") == 0 ||
                    std::strcmp(method, "hide") == 0)) {
          const bool do_show = std::strcmp(method, "show") == 0;
          tree_field_set_int(osd, "visible", do_show ? 1 : 0);
          tree_field_set_int(osd, "shown", do_show ? 1 : 0);
          if (do_show) tree_field_set_int(osd, "init", 0);
          if (self != osd) {
            tree_field_set_int(self, "shown", do_show ? 1 : 0);
            tree_field_set_int(self, "visible", do_show ? 1 : 0);
          }
          return JvmValue::make_void();
        }
        if (osd && std::strcmp(method, "removeAllElements") == 0) {
          auto clr = [](InvObject* v) {
            if (v && tree_vector_is(v)) tree_vector_resize(v, 0);
          };
          clr(tree_field_get_obj(osd, "rectangles"));
          clr(tree_field_get_obj(osd, "text"));
          clr(tree_field_get_obj(osd, "hotkey"));
          clr(tree_field_get_obj(osd, "object"));
          return JvmValue::make_void();
        }
        if (osd && std::strcmp(method, "createHotkey") == 0) {
          int32_t key = 0, flags = 0, cmd = 0, ef = 1;
          InvObject* handler = nullptr;
          std::vector<int32_t> ints;
          for (size_t i = 1; i < pargs->size(); ++i) {
            if ((*pargs)[i].tag == JvmTag::Int) ints.push_back((*pargs)[i].v.i);
            else if ((*pargs)[i].tag == JvmTag::Obj && !handler &&
                     (*pargs)[i].v.o != self && (*pargs)[i].v.o != osd)
              handler = (*pargs)[i].v.o;
          }
          if (ints.size() >= 1) key = ints[0];
          if (ints.size() >= 2) flags = ints[1];
          if (ints.size() >= 3) cmd = ints[2];
          if (ints.size() >= 4) ef = ints[3];
          return JvmValue::make_obj(
              osd_create_hotkey(osd, key, flags, cmd, handler, ef));
        }
        if (osd && std::strcmp(method, "createHeader") == 0) {
          const char* title = "";
          for (size_t i = 1; i < pargs->size(); ++i) {
            if ((*pargs)[i].tag == JvmTag::Obj && (*pargs)[i].v.o) {
              if (const char* s = string_cstr((*pargs)[i].v.o)) {
                title = s;
                break;
              }
            }
          }
          return JvmValue::make_obj(osd_create_header(osd, title));
        }
        if (osd && std::strcmp(method, "createText") == 0) {
          const char* text = "";
          InvObject* font = frontend_medium_font();
          int32_t align = 1;
          float x = 0.f, y = 0.f;
          std::vector<float> floats;
          std::vector<int32_t> ints;
          for (size_t i = 1; i < pargs->size(); ++i) {
            const JvmValue& a = (*pargs)[i];
            if (a.tag == JvmTag::Obj && a.v.o) {
              if (const char* s = string_cstr(a.v.o)) {
                if (!text[0]) text = s;
              } else {
                font = a.v.o;
              }
            } else if (a.tag == JvmTag::Int)
              ints.push_back(a.v.i);
            else if (a.tag == JvmTag::Float)
              floats.push_back(a.v.f);
          }
          if (!ints.empty()) align = ints[0];
          if (floats.size() >= 1) x = floats[0];
          if (floats.size() >= 2) y = floats[1];
          return JvmValue::make_obj(
              osd_create_text(osd, text, font, align, x, y));
        }
        if (osd && std::strcmp(method, "createMenu") == 0) {
          InvObject* style = nullptr;
          float x = 0.f, y = 0.f, spc = 0.f;
          int32_t ori = 0;
          std::vector<float> floats;
          std::vector<int32_t> ints;
          for (size_t i = 1; i < pargs->size(); ++i) {
            const JvmValue& a = (*pargs)[i];
            if (a.tag == JvmTag::Obj && a.v.o && a.v.o != osd && a.v.o != self) {
              const char* ohc = tree_host_class(a.v.o);
              if (ohc && std::strstr(ohc, "Style") && !style) style = a.v.o;
            } else if (a.tag == JvmTag::Float)
              floats.push_back(a.v.f);
            else if (a.tag == JvmTag::Int)
              ints.push_back(a.v.i);
          }
          if (floats.size() >= 1) x = floats[0];
          if (floats.size() >= 2) y = floats[1];
          if (floats.size() >= 3) spc = floats[2];
          if (!ints.empty()) ori = ints.back();
          // Mispacked icon addItem (cmd≥100, no layout floats) → addItem.
          if (floats.empty() && ori >= 100) {
            InvObject* menu = tree_field_get_obj(osd, "last_menu");
            if (!menu && self &&
                std::strstr(tree_host_class(self), "Menu"))
              menu = self;
            InvObject* gfx = nullptr;
            const char* tip = "";
            for (size_t i = 1; i < pargs->size(); ++i) {
              const JvmValue& a = (*pargs)[i];
              if (a.tag != JvmTag::Obj || !a.v.o) continue;
              const char* hc = tree_host_class(a.v.o);
              if (hc && (std::strstr(hc, "ResourceRef") ||
                         std::strstr(hc, "GameRef"))) {
                if (!gfx) gfx = a.v.o;
              } else if (const char* s = string_cstr(a.v.o)) {
                if (!tip[0]) tip = s;
              }
            }
            static int s_poison;
            if (s_poison < 8) {
              std::printf(
                  "[script] createMenu→addItem poison cmd=%d gfx=%d menu=%d "
                  "argc=%d\n",
                  ori, gfx ? 1 : 0, menu ? 1 : 0,
                  prefer_static ? static_cast<int>(pargs->size())
                                : static_cast<int>(pargs->size()) - 1);
              ++s_poison;
            }
            if (menu && gfx)
              return JvmValue::make_obj(
                  menu_add_item_gfx(menu, gfx, ori, tip));
            return JvmValue::make_obj(menu);
          }
          InvObject* menu = osd_create_menu(osd, style, x, y, spc, ori);
          tree_field_set_obj(osd, "last_menu", menu);
          return JvmValue::make_obj(menu);
        }
        if (osd && std::strcmp(method, "createButton") == 0) {
          InvObject* style = nullptr;
          const char* label = "";
          float x = 0.f, y = 0.f;
          int32_t cmd = 0;
          std::vector<float> floats;
          std::vector<int32_t> ints;
          for (size_t i = 1; i < pargs->size(); ++i) {
            const JvmValue& a = (*pargs)[i];
            if (a.tag == JvmTag::Obj && a.v.o) {
              const char* ohc = tree_host_class(a.v.o);
              if (ohc && std::strstr(ohc, "Style") && !style)
                style = a.v.o;
              else if (const char* s = string_cstr(a.v.o))
                label = s;
            } else if (a.tag == JvmTag::Float)
              floats.push_back(a.v.f);
            else if (a.tag == JvmTag::Int)
              ints.push_back(a.v.i);
          }
          if (floats.size() >= 1) x = floats[0];
          if (floats.size() >= 2) y = floats[1];
          if (!ints.empty()) cmd = ints.back();
          return JvmValue::make_obj(
              osd_create_button(osd, style, x, y, label, cmd));
        }
        if (osd && std::strcmp(method, "beginGroup") == 0)
          return JvmValue::make_int(osd_begin_group(osd));
        if (osd && std::strcmp(method, "endGroup") == 0)
          return JvmValue::make_int(osd_end_group(osd));
        if (osd && std::strcmp(method, "hideGroup") == 0) {
          int32_t gid = -1;
          if (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Int)
            gid = (*pargs)[1].v.i;
          osd_hide_group(osd, gid);
          return JvmValue::make_void();
        }
        if (osd && std::strcmp(method, "showGroup") == 0) {
          int32_t gid = -1;
          if (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Int)
            gid = (*pargs)[1].v.i;
          osd_show_group(osd, gid);
          return JvmValue::make_void();
        }
        if (self_is_osd && std::strcmp(method, "<init>") == 0) {
          osd_ensure_defaults(self);
          return JvmValue::make_void();
        }
      }

      if (menu_method ||
          (!self_is_menu && (std::strcmp(method, "addItem") == 0 ||
                             std::strcmp(method, "addSeparator") == 0))) {
        InvObject* menu = self_is_menu ? self : nullptr;
        if (!menu) {
          if (InvObject* osd = resolve_osd())
            menu = tree_field_get_obj(osd, "last_menu");
        }
        if (menu && std::strcmp(method, "addSeparator") == 0) {
          menu_add_separator(menu);
          return JvmValue::make_void();
        }
        if (menu && std::strcmp(method, "setSliderStyle") == 0) {
          InvObject* ss = nullptr;
          InvObject* sk = nullptr;
          for (size_t i = 1; i < pargs->size(); ++i) {
            if ((*pargs)[i].tag != JvmTag::Obj || !(*pargs)[i].v.o) continue;
            if (!ss)
              ss = (*pargs)[i].v.o;
            else if (!sk)
              sk = (*pargs)[i].v.o;
          }
          if (ss) tree_field_set_obj(menu, "stySld", ss);
          if (sk) tree_field_set_obj(menu, "styKnob", sk);
          return JvmValue::make_void();
        }
        if (menu && std::strcmp(method, "addItem") == 0) {
          const char* text = "";
          const char* tip = "";
          int32_t cmd = 0;
          InvObject* gfx = nullptr;
          for (size_t i = 1; i < pargs->size(); ++i) {
            if ((*pargs)[i].tag == JvmTag::Obj && (*pargs)[i].v.o) {
              const char* hc = tree_host_class((*pargs)[i].v.o);
              if (hc && (std::strstr(hc, "ResourceRef") ||
                         std::strstr(hc, "GameRef"))) {
                if (!gfx) gfx = (*pargs)[i].v.o;
                continue;
              }
              if (hc && hc[0] && !std::strstr(hc, "String")) continue;
              if (const char* s = string_cstr((*pargs)[i].v.o)) {
                if (!text[0])
                  text = s;
                else if (!tip[0])
                  tip = s;
              }
            } else if ((*pargs)[i].tag == JvmTag::Int) {
              if ((*pargs)[i].v.i > 0x10000) {
                if (gfx)
                  java_util_resource_ResourceRef_set(gfx, (*pargs)[i].v.i);
              } else
                cmd = (*pargs)[i].v.i;
            }
          }
          if (gfx) {
            static int s_ai;
            if (s_ai < 30) {
              std::printf("[script] Menu.addItem gfx cmd=%d tip='%s'\n", cmd,
                          tip[0] ? tip : text);
              ++s_ai;
            }
            return JvmValue::make_obj(
                menu_add_item_gfx(menu, gfx, cmd, tip[0] ? tip : text));
          }
          static int s_ai2;
          if (s_ai2 < 20) {
            std::printf("[script] Menu.addItem text cmd=%d '%s'\n", cmd, text);
            ++s_ai2;
          }
          return JvmValue::make_obj(menu_add_item(menu, text, cmd));
        }
      }
    }

    // GameType.createNativeInstance() / (parent) overloads → 4-arg native.
    // TREE often names owner java.lang.Object after NEW packing.
    if (!prefer_static && method &&
        std::strcmp(method, "createNativeInstance") == 0 && !pargs->empty() &&
        (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      InvObject* parent = nullptr;
      int32_t type_id = 0;
      InvObject* params = nullptr;
      InvObject* alias = nullptr;
      if (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Obj)
        parent = (*pargs)[1].v.o;
      if (pargs->size() >= 3 && (*pargs)[2].tag == JvmTag::Int)
        type_id = (*pargs)[2].v.i;
      if (pargs->size() >= 4 && (*pargs)[3].tag == JvmTag::Obj)
        params = (*pargs)[3].v.o;
      if (pargs->size() >= 5 && (*pargs)[4].tag == JvmTag::Obj)
        alias = (*pargs)[4].v.o;
      java_lang_GameType_createNativeInstance(self, parent, type_id, params,
                                               alias);
      return JvmValue::make_void();
    }

    // Thread.sleep is static; SoftTimer/FlashText TREE often mis-names owner.
    if (method && std::strcmp(method, "sleep") == 0) {
      float ms = 0.f;
      if (!pargs->empty() && (*pargs)[0].tag == JvmTag::Float) ms = (*pargs)[0].v.f;
      else if (!pargs->empty() && (*pargs)[0].tag == JvmTag::Int)
        ms = static_cast<float>((*pargs)[0].v.i);
      else if (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Float)
        ms = (*pargs)[1].v.f;
      else if (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Int)
        ms = static_cast<float>((*pargs)[1].v.i);
      java_lang_Thread_sleep(ms);
      return JvmValue::make_void();
    }
    if (method && std::strcmp(method, "setLdPriority") == 0) {
      int32_t p = 0;
      if (!pargs->empty() && (*pargs)[0].tag == JvmTag::Int) p = (*pargs)[0].v.i;
      else if (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Int)
        p = (*pargs)[1].v.i;
      java_lang_System_setLdPriority(p);
      return JvmValue::make_void();
    }

    // Integer boxing used by VehicleType.addColorIndex (no host layer).
    if (!prefer_static && method && class_fqn &&
        std::strcmp(class_fqn, "java.lang.Integer") == 0 && !pargs->empty() &&
        (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      if (std::strcmp(method, "intValue") == 0)
        return JvmValue::make_int(tree_field_get_int(self, "value"));
      if (std::strcmp(method, "setValue") == 0 && pargs->size() >= 2 &&
          (*pargs)[1].tag == JvmTag::Int) {
        tree_field_set_int(self, "value", (*pargs)[1].v.i);
        return JvmValue::make_void();
      }
      if (std::strcmp(method, "<init>") == 0 && pargs->size() >= 2 &&
          (*pargs)[1].tag == JvmTag::Int) {
        tree_field_set_int(self, "value", (*pargs)[1].v.i);
        return JvmValue::make_void();
      }
    }

    // Vector3.<init>: Java (FFF) / copy; native (Ypr) @ 0x00482020.
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      const char* hc = tree_host_class(self);
      const bool is_v3 =
          (class_fqn && std::strstr(class_fqn, "Vector3")) ||
          (hc && std::strstr(hc, "Vector3")) || vec3_is(self);
      const bool named_other =
          hc && hc[0] && !std::strstr(hc, "Vector3");
      if (is_v3 && !named_other) {
        InvObject* ypr = nullptr;
        InvObject* copy = nullptr;
        float xyz[3] = {0.f, 0.f, 0.f};
        int nf = 0;
        for (size_t i = 1; i < pargs->size(); ++i) {
          const JvmValue& a = (*pargs)[i];
          if (a.tag == JvmTag::Obj && a.v.o) {
            const char* ac = tree_host_class(a.v.o);
            if (ac && std::strstr(ac, "Ypr"))
              ypr = a.v.o;
            else if (a.v.o != self &&
                     ((ac && std::strstr(ac, "Vector3")) || vec3_is(a.v.o)))
              copy = a.v.o;
          } else if (nf < 3 &&
                     (a.tag == JvmTag::Float || a.tag == JvmTag::Int)) {
            xyz[nf++] = a.tag == JvmTag::Float ? a.v.f
                                               : static_cast<float>(a.v.i);
          }
        }
        if (ypr && nf < 3) {
          java_lang_Vector3_init_Ypr(self, ypr);
          float x = 0, y = 0, z = 0;
          vec3_get(self, &x, &y, &z);
          tree_field_set_float(self, "x", x);
          tree_field_set_float(self, "y", y);
          tree_field_set_float(self, "z", z);
          return JvmValue::make_void();
        }
        if (copy && nf < 3) {
          float x = 0, y = 0, z = 0;
          vec3_get(copy, &x, &y, &z);
          if (x == 0.f && y == 0.f && z == 0.f) {
            x = tree_field_get_float(copy, "x");
            y = tree_field_get_float(copy, "y");
            z = tree_field_get_float(copy, "z");
          }
          vec3_set(self, x, y, z);
          tree_field_set_float(self, "x", x);
          tree_field_set_float(self, "y", y);
          tree_field_set_float(self, "z", z);
          return JvmValue::make_void();
        }
        vec3_set(self, xyz[0], xyz[1], xyz[2]);
        tree_field_set_float(self, "x", xyz[0]);
        tree_field_set_float(self, "y", xyz[1]);
        tree_field_set_float(self, "z", xyz[2]);
        return JvmValue::make_void();
      }
    }

    // java.render.Rectangle(group, pos, tmpl) — TREE names osd.Rectangle (no .class).
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      const char* hc = tree_host_class(self);
      const bool is_rect =
          (class_fqn && std::strstr(class_fqn, "Rectangle")) ||
          (hc && std::strstr(hc, "Rectangle"));
      if (is_rect) {
        InvObject* group = nullptr;
        InvObject* pos = nullptr;
        InvObject* tmpl = nullptr;
        InvObject* tex = nullptr;
        float x = 0.f, y = 0.f, w = 2.f, h = 2.f;
        int32_t pri = -1;
        int nf = 0;
        for (size_t i = 1; i < pargs->size(); ++i) {
          const JvmValue& a = (*pargs)[i];
          if (a.tag == JvmTag::Obj && a.v.o) {
            const char* ac = tree_host_class(a.v.o);
            if (ac && std::strstr(ac, "Vector3"))
              pos = a.v.o;
            else if (ac && std::strstr(ac, "Template"))
              tmpl = a.v.o;
            else if (ac && (std::strstr(ac, "ResourceRef") ||
                            std::strstr(ac, "Group") ||
                            std::strstr(ac, "Osd"))) {
              if (!group) group = a.v.o;
              if (ac && std::strstr(ac, "ResourceRef")) tex = a.v.o;
            }
          } else if (a.tag == JvmTag::Float || a.tag == JvmTag::Int) {
            const float f = a.tag == JvmTag::Float ? a.v.f
                                                   : static_cast<float>(a.v.i);
            if (nf == 0) x = f;
            else if (nf == 1) y = f;
            else if (nf == 2) w = f;
            else if (nf == 3) h = f;
            else if (a.tag == JvmTag::Int) pri = a.v.i;
            ++nf;
          }
        }
        if (pos) {
          x = tree_field_get_float(pos, "x");
          y = tree_field_get_float(pos, "y");
        }
        if (tmpl) {
          const float tw = tree_field_get_float(tmpl, "width");
          const float th = tree_field_get_float(tmpl, "height");
          if (tw != 0.f) w = tw;
          if (th != 0.f) h = th;
          if (!tex) tex = tree_field_get_obj(tmpl, "texture");
        }
        tree_field_set_obj(self, "root", group);
        tree_field_set_obj(self, "pos", pos);
        tree_field_set_obj(self, "rt", tmpl);
        tree_field_set_obj(self, "texture", tex);
        tree_field_set_float(self, "x", x);
        tree_field_set_float(self, "y", y);
        tree_field_set_float(self, "w", w);
        tree_field_set_float(self, "h", h);
        tree_field_set_int(self, "pri", pri);
        InvObject* osd = group;
        if (osd) {
          const char* gc = tree_host_class(osd);
          if (gc && std::strstr(gc, "Group"))
            osd = tree_field_get_obj(group, "osd");
          else if (gc && !std::strstr(gc, "Osd"))
            osd = nullptr;
        }
        if (osd) {
          osd_ensure_defaults(osd);
          InvObject* rects = tree_field_get_obj(osd, "rectangles");
          if (rects) tree_vector_add(rects, self);
        }
        if (tex) java_util_resource_ResourceRef_load(tex);
        render_d3d9_osd_add_rect(x, y, w, h, tex, pri);
        return JvmValue::make_void();
      }
    }

    // java.render.osd.Menu(Osd, Style, x, y, spc[, ori]) — TREE argc=0.
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      const char* hc = tree_host_class(self);
      const bool is_menu =
          ((class_fqn && std::strstr(class_fqn, "Menu") &&
            !std::strstr(class_fqn, "MainMenu")) ||
           (hc && std::strstr(hc, "Menu") && !std::strstr(hc, "MainMenu")));
      if (is_menu) {
        InvObject* osd = nullptr;
        InvObject* style = nullptr;
        float x = 0.f, y = 0.f, spc = 0.f;
        int32_t ori = 0;
        std::vector<float> floats;
        std::vector<int32_t> ints;
        for (size_t i = 1; i < pargs->size(); ++i) {
          const JvmValue& a = (*pargs)[i];
          if (a.tag == JvmTag::Obj && a.v.o) {
            const char* ac = tree_host_class(a.v.o);
            if (ac && std::strstr(ac, "Osd") && !std::strstr(ac, "dialog") &&
                !std::strstr(ac, "Rectangle") && !osd)
              osd = a.v.o;
            else if (ac && std::strstr(ac, "Style") && !style)
              style = a.v.o;
          } else if (a.tag == JvmTag::Float)
            floats.push_back(a.v.f);
          else if (a.tag == JvmTag::Int)
            ints.push_back(a.v.i);
        }
        if (floats.size() >= 1) x = floats[0];
        if (floats.size() >= 2) y = floats[1];
        if (floats.size() >= 3) spc = floats[2];
        if (!ints.empty()) ori = ints.back();
        if (!osd) osd = tree_field_get_obj(self, "osd");
        InvObject* made = osd_create_menu(osd, style, x, y, spc, ori);
        if (made && made != self) {
          tree_field_set_obj(self, "osd", tree_field_get_obj(made, "osd"));
          tree_field_set_obj(self, "sty", tree_field_get_obj(made, "sty"));
          tree_field_set_obj(self, "items", tree_field_get_obj(made, "items"));
          tree_field_set_int(self, "item_count",
                             tree_field_get_int(made, "item_count"));
          tree_field_set_float(self, "x", tree_field_get_float(made, "x"));
          tree_field_set_float(self, "y", tree_field_get_float(made, "y"));
          tree_field_set_float(self, "spacing",
                               tree_field_get_float(made, "spacing"));
          tree_field_set_int(self, "orientation",
                             tree_field_get_int(made, "orientation"));
        }
        if (osd) tree_field_set_obj(osd, "last_menu", self);
        return JvmValue::make_void();
      }
    }

    // ResourceRef(int id) — RaceSetup/Garage `new ResourceRef(Osd.RID_*)`.
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      const char* hc = tree_host_class(self);
      const bool is_rr =
          (hc && std::strstr(hc, "ResourceRef")) ||
          (class_fqn && std::strstr(class_fqn, "ResourceRef"));
      if (is_rr && pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Int) {
        java_util_resource_ResourceRef_set(self, (*pargs)[1].v.i);
        return JvmValue::make_void();
      }
    }

    // Style(float,float[,aspect],charset,align,bg) — TREE mis-routes to
    // Animation.<init> via cross-class native argc match.
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      const char* hc = tree_host_class(self);
      const bool is_style =
          (hc && std::strstr(hc, "Style")) ||
          (class_fqn && std::strstr(class_fqn, "Style"));
      if (is_style) {
        float w = 0.45f, h = 0.12f, aspect = 4.f / 3.f;
        int32_t align = 1;
        InvObject* charset = nullptr;
        InvObject* bg = nullptr;
        std::vector<float> floats;
        std::vector<int32_t> ints;
        for (size_t i = 1; i < pargs->size(); ++i) {
          const JvmValue& a = (*pargs)[i];
          if (a.tag == JvmTag::Float)
            floats.push_back(a.v.f);
          else if (a.tag == JvmTag::Int)
            ints.push_back(a.v.i);
          else if (a.tag == JvmTag::Obj && a.v.o) {
            if (!charset)
              charset = a.v.o;
            else
              bg = a.v.o;
          }
        }
        if (floats.size() >= 1) w = floats[0];
        if (floats.size() >= 2) h = floats[1];
        if (floats.size() >= 3) aspect = floats[2];
        if (!ints.empty()) align = ints.back();
        tree_field_set_float(self, "width", w);
        tree_field_set_float(self, "height", h);
        tree_field_set_float(self, "aspect", aspect);
        tree_field_set_int(self, "align", align);
        if (charset) tree_field_set_obj(self, "charset", charset);
        if (bg) tree_field_set_obj(self, "background", bg);
        tree_field_set_float(self, "rWidth", w * 1.47f * aspect);
        tree_field_set_float(self, "rHeight", h * 1.47f);
        return JvmValue::make_void();
      }
    }

    // Trigger(parent, type, pos[, r], alias) — TREE this() often arrives argc=0.
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      const char* hc = tree_host_class(self);
      const bool is_trig =
          (hc && std::strstr(hc, "Trigger")) ||
          (class_fqn && std::strstr(class_fqn, "Trigger"));
      if (is_trig && trigger_apply_ctor(self, *pargs))
        return JvmValue::make_void();
      const bool is_pcar =
          (hc && std::strstr(hc, "ParkingCar")) ||
          (class_fqn && std::strstr(class_fqn, "ParkingCar"));
      if (is_pcar && parking_car_apply_ctor(self, *pargs))
        return JvmValue::make_void();
    }

    // GameRef(parent, type|rid, params, alias) → create native @ 0x0047D7B0.
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        class_fqn && std::strstr(class_fqn, "GameRef") && !pargs->empty() &&
        (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      if (pargs->size() == 2 && (*pargs)[1].tag == JvmTag::Int) {
        java_util_resource_ResourceRef_set(self, (*pargs)[1].v.i);
        return JvmValue::make_void();
      }
      if (pargs->size() >= 5) {
        InvObject* parent =
            (*pargs)[1].tag == JvmTag::Obj ? (*pargs)[1].v.o : nullptr;
        InvObject* type = nullptr;
        if ((*pargs)[2].tag == JvmTag::Int) {
          type = gameref_new();
          java_util_resource_ResourceRef_set(type, (*pargs)[2].v.i);
        } else if ((*pargs)[2].tag == JvmTag::Obj) {
          type = (*pargs)[2].v.o;
        }
        InvObject* params =
            (*pargs)[3].tag == JvmTag::Obj ? (*pargs)[3].v.o : nullptr;
        InvObject* alias =
            (*pargs)[4].tag == JvmTag::Obj ? (*pargs)[4].v.o : nullptr;
        java_util_resource_GameRef_create(self, parent, type, params, alias);
        return JvmValue::make_void();
      }
    }

    // RenderRef(parent, type|rid, alias) → create @ 0x00480EE0 (not GameRef 4-arg).
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o &&
        is_renderref_not_camera((*pargs)[0].v.o, class_fqn)) {
      InvObject* self = (*pargs)[0].v.o;
      if (pargs->size() == 2 && (*pargs)[1].tag == JvmTag::Int) {
        java_util_resource_ResourceRef_set(self, (*pargs)[1].v.i);
        return JvmValue::make_void();
      }
      if (pargs->size() >= 3) {
        renderref_apply_create(*pargs);
        return JvmValue::make_void();
      }
    }

    // VehicleModel(int id, int mask) — TREE <init> never writes id (stays 0).
    if (!prefer_static && method && class_fqn &&
        std::strcmp(class_fqn, "java.game.VehicleModel") == 0 &&
        std::strcmp(method, "<init>") == 0 && !pargs->empty() &&
        (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      int32_t id = 0;
      int32_t mask = 0x3f;
      if (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Int)
        id = (*pargs)[1].v.i;
      if (pargs->size() >= 3 && (*pargs)[2].tag == JvmTag::Int)
        mask = (*pargs)[2].v.i;
      tree_field_set_int(self, "id", id);
      tree_field_set_int(self, "vehicleSetMask", mask);
      return JvmValue::make_void();
    }

    if (!jvm_->find_class(class_fqn)) jvm_->load_class(class_fqn);
    const JvmClass* cls = jvm_->find_class(class_fqn);
    const int argc =
        prefer_static ? static_cast<int>(pargs->size())
                      : (pargs->empty() ? 0 : static_cast<int>(pargs->size()) - 1);

    // Math.random() — TREE binds onto Valocity/City (createQuickRaceBot).
    if (method && std::strcmp(method, "random") == 0 && argc == 0)
      return JvmValue::make_float(java_lang_Math_random());

    // System.exit(String) stolen as Bot.exit when botVd is null.
    if (method && std::strcmp(method, "exit") == 0) {
      InvObject* msg = nullptr;
      for (size_t i = prefer_static ? 0 : 1; i < pargs->size(); ++i) {
        if ((*pargs)[i].tag == JvmTag::Obj && (*pargs)[i].v.o &&
            string_cstr((*pargs)[i].v.o)) {
          msg = (*pargs)[i].v.o;
          break;
        }
      }
      const char* hc0 =
          (!prefer_static && !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj)
              ? tree_host_class((*pargs)[0].v.o)
              : nullptr;
      const bool gamestate =
          hc0 && (std::strstr(hc0, "Splash") || std::strstr(hc0, "MainMenu") ||
                  std::strstr(hc0, "Garage") || std::strstr(hc0, "Valocity") ||
                  std::strstr(hc0, "City") || std::strstr(hc0, "Track") ||
                  std::strstr(hc0, "RaceSetup") || std::strstr(hc0, "Dialog"));
      if (msg && !gamestate) {
        std::fprintf(stderr, "[script] System.exit stolen: %s\n",
                     string_cstr(msg));
        return JvmValue::make_void();
      }
      if ((hc0 && std::strstr(hc0, "Bot")) ||
          (class_fqn && std::strstr(class_fqn, "Bot"))) {
        std::fprintf(stderr, "[script] System.exit stolen as Bot.exit\n");
        return JvmValue::make_void();
      }
    }

    // GameType.addNotification @ 0x0047E050 / 0x0047E100 → Watch @ 0x0048B500.
    // TREE permutes (ref,etype,ealias,msg[,method]) into unsupported JNI shapes.
    if (!prefer_static && method && std::strcmp(method, "addNotification") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      InvObject* ref = nullptr;
      InvObject* msg = nullptr;
      InvObject* meth = nullptr;
      int32_t etype = 0, ealias = 0;
      int nint = 0;
      for (size_t i = 1; i < pargs->size(); ++i) {
        const JvmValue& a = (*pargs)[i];
        if (a.tag == JvmTag::Int || a.tag == JvmTag::Float) {
          const int32_t v = a.tag == JvmTag::Int
                                ? a.v.i
                                : static_cast<int32_t>(a.v.f);
          if (nint == 0)
            etype = v;
          else if (nint == 1)
            ealias = v;
          ++nint;
          continue;
        }
        if (a.tag != JvmTag::Obj) continue;
        if (!a.v.o) continue;
        if (string_cstr(a.v.o) &&
            (!tree_host_class(a.v.o)[0] ||
             std::strstr(tree_host_class(a.v.o), "String"))) {
          if (!meth)
            meth = a.v.o;
          else
            msg = a.v.o;
          continue;
        }
        if (!ref) ref = a.v.o;
      }
      if (meth && string_cstr(meth) &&
          std::strncmp(string_cstr(meth), "event_", 6) == 0) {
        /* method name in meth, msg stays null */
      } else if (meth && !msg) {
        msg = meth;
        meth = nullptr;
      }
      if (meth)
        java_lang_GameType_addNotification_1(self, ref, etype, ealias, msg,
                                             meth);
      else
        java_lang_GameType_addNotification(self, ref, etype, ealias, msg);
      return JvmValue::make_void();
    }

    // Vector3.add/mul/sub — Java TREE; recv often ResourceRef leftover.
    if (method &&
        (std::strcmp(method, "add") == 0 || std::strcmp(method, "mul") == 0 ||
         std::strcmp(method, "sub") == 0)) {
      auto is_v3 = [](InvObject* o) -> bool {
        if (!o) return false;
        const char* c = tree_host_class(o);
        if (c && std::strstr(c, "Vector3")) return true;
        if (c && c[0]) return false;
        if (string_cstr(o)) return false;
        return vec3_is(o);
      };
      InvObject* recv = nullptr;
      for (size_t i = 0; i < pargs->size(); ++i) {
        if ((*pargs)[i].tag == JvmTag::Obj && is_v3((*pargs)[i].v.o)) {
          recv = (*pargs)[i].v.o;
          break;
        }
      }
      if (is_v3(recv)) {
        float x = 0, y = 0, z = 0;
        vec3_get(recv, &x, &y, &z);
        if (x == 0.f && y == 0.f && z == 0.f) {
          x = tree_field_get_float(recv, "x");
          y = tree_field_get_float(recv, "y");
          z = tree_field_get_float(recv, "z");
        }
        float scale = 1.f;
        bool have_scale = false;
        InvObject* oth = nullptr;
        for (size_t i = 0; i < pargs->size(); ++i) {
          const JvmValue& a = (*pargs)[i];
          if (a.tag == JvmTag::Float || a.tag == JvmTag::Int) {
            scale = a.tag == JvmTag::Float ? a.v.f : static_cast<float>(a.v.i);
            have_scale = true;
          } else if (a.tag == JvmTag::Obj && a.v.o && a.v.o != recv && !oth)
            oth = a.v.o;
        }
        if (std::strcmp(method, "mul") == 0 && have_scale) {
          x *= scale;
          y *= scale;
          z *= scale;
        } else if (oth) {
            float ox = 0, oy = 0, oz = 0;
            vec3_get(oth, &ox, &oy, &oz);
            if (ox == 0.f && oy == 0.f && oz == 0.f) {
              ox = tree_field_get_float(oth, "x");
              oy = tree_field_get_float(oth, "y");
              oz = tree_field_get_float(oth, "z");
            }
            if (std::strcmp(method, "sub") == 0) {
              x -= ox;
              y -= oy;
              z -= oz;
            } else if (std::strcmp(method, "mul") == 0) {
              x *= ox;
              y *= oy;
              z *= oz;
            } else {
              x += ox;
              y += oy;
              z += oz;
            }
        }
        vec3_set(recv, x, y, z);
        tree_field_set_float(recv, "x", x);
        tree_field_set_float(recv, "y", y);
        tree_field_set_float(recv, "z", z);
        return JvmValue::make_obj(recv);
      }
      // Stolen Vector3.add/mul/sub onto City/Valocity/ResourceRef.
      return JvmValue::make_void();
    }

    // VehicleDescriptor has no 2-arg Java ctor — TREE leftover (id, colorIndex).
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        class_fqn && std::strstr(class_fqn, "VehicleDescriptor") &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      if (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Int)
        tree_field_set_int(self, "id", (*pargs)[1].v.i);
      if (pargs->size() >= 3 && (*pargs)[2].tag == JvmTag::Int)
        tree_field_set_int(self, "colorIndex", (*pargs)[2].v.i);
      if (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Obj &&
          (*pargs)[1].v.o) {
        InvObject* src = (*pargs)[1].v.o;
        tree_field_set_int(self, "id", tree_field_get_int(src, "id"));
        tree_field_set_int(self, "colorIndex",
                           tree_field_get_int(src, "colorIndex"));
        tree_field_set_float(self, "power", tree_field_get_float(src, "power"));
        tree_field_set_float(self, "optical",
                             tree_field_get_float(src, "optical"));
        tree_field_set_float(self, "wear", tree_field_get_float(src, "wear"));
        tree_field_set_float(self, "tear", tree_field_get_float(src, "tear"));
      }
      return JvmValue::make_void();
    }

    // PhysicsRef.createBox(parent,x,y,z,alias) — TREE shape noise.
    if (!prefer_static && method && std::strcmp(method, "createBox") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      InvObject* parent = nullptr;
      InvObject* alias = nullptr;
      float xyz[3] = {1.f, 0.5f, 2.f};
      int nf = 0;
      for (size_t i = 1; i < pargs->size(); ++i) {
        const JvmValue& a = (*pargs)[i];
        if (a.tag == JvmTag::Float || a.tag == JvmTag::Int) {
          if (nf < 3)
            xyz[nf++] = a.tag == JvmTag::Float ? a.v.f
                                               : static_cast<float>(a.v.i);
        } else if (a.tag == JvmTag::Obj && a.v.o) {
          if (string_cstr(a.v.o))
            alias = a.v.o;
          else if (!parent)
            parent = a.v.o;
        }
      }
      java_util_resource_PhysicsRef_createBox(self, parent, xyz[0], xyz[1],
                                              xyz[2], alias);
      return JvmValue::make_void();
    }

    // GameRef.queueEvent(ResourceRef,I,String)V @ 0x0047DA30.
    // TREE 0x12 defaults static argc=0 (MouseCursor/Player/Vehicle/Bot miss).
    if (method && std::strcmp(method, "queueEvent") == 0) {
      if (argc == 0) return JvmValue::make_void();
      queue_event_apply(*pargs);
      return JvmValue::make_void();
    }

    // RenderRef.create(parent, type, alias)V @ 0x00480EE0.
    // TREE 0x12 defaults static argc=0; Java wrapper re-invokes native create.
    // Camera.create is a different native — skip Camera recv / FQN.
    if (method && std::strcmp(method, "create") == 0 && !pargs->empty() &&
        (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      if (is_renderref_not_camera(self, class_fqn)) {
        if (argc == 0) return JvmValue::make_void();
        renderref_apply_create(*pargs);
        return JvmValue::make_void();
      }
    }

    if (!prefer_static && method && std::strcmp(method, "createCar") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      const char* hc = tree_host_class(self);
      const bool is_bot = hc && std::strstr(hc, "Bot");
      auto ok_bot = [&](InvObject* o) -> bool {
        if (!o || o == self) return false;
        const char* c = tree_host_class(o);
        if (!c || !c[0]) return true;
        return std::strstr(c, "Bot") != nullptr;
      };
      // Bot.createCar(map, Vehicle c) stolen onto Vehicle — host leaf, no
      // retarget (1-arg TREE ↔ 2-arg Vehicle ping-pongs).
      if (hc && std::strstr(hc, "Vehicle")) {
        InvObject* bot = nullptr;
        InvObject* map = nullptr;
        InvObject* car = self;
        for (size_t i = 1; i < pargs->size(); ++i) {
          if ((*pargs)[i].tag != JvmTag::Obj || !(*pargs)[i].v.o) continue;
          InvObject* o = (*pargs)[i].v.o;
          const char* ac = tree_host_class(o);
          if (ac && std::strstr(ac, "Bot"))
            bot = o;
          else if (ac && std::strstr(ac, "GroundRef"))
            map = o;
          else if (ac && std::strstr(ac, "Vehicle") && o != self)
            car = o;
          else if (!map)
            map = o;
        }
        if (!ok_bot(bot)) {
          InvObject* st = game_logic_actual_state();
          bot = st ? tree_field_get_obj(st, "raceBot") : nullptr;
        }
        if (ok_bot(bot)) {
          if (map) tree_field_set_obj(bot, "world", map);
          tree_field_set_obj(bot, "car", car);
          std::vector<JvmValue> ec = {JvmValue::make_obj(bot),
                                      JvmValue::make_obj(car)};
          call_by_name("java.game.Bot", "enterCar", ec, false);
        }
        return JvmValue::make_void();
      }
      const bool stolen =
          hc && (std::strstr(hc, "Valocity") || std::strstr(hc, "City") ||
                 std::strstr(hc, "Track") || std::strstr(hc, "RaceSetup"));
      if (is_bot) {
        if (!tree_field_get_obj(self, "botVd"))
          tree_field_set_obj(self, "botVd",
                             game_logic_get_vehicle_descriptor(1, -1.f));
        // createCar(map) TREE often binds the filename overload → System.exit.
        // Java: new Vehicle(this, botVd.*) then createCar(map, vhc).
        if (argc <= 1) {
          InvObject* vd = tree_field_get_obj(self, "botVd");
          InvObject* map = nullptr;
          if (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Obj)
            map = (*pargs)[1].v.o;
          if (!map) map = tree_field_get_obj(self, "world");
          InvObject* car = tree_field_get_obj(self, "car");
          if (!car) {
            car = tree_host_new("java.game.Vehicle");
            int32_t rid = vd ? tree_field_get_int(vd, "id") : 0;
            if (!rid && vd) rid = java_util_resource_ResourceRef_id(vd);
            if (rid) {
              java_util_resource_ResourceRef_set(car, rid);
              tree_field_set_int(car, "id", rid);
            }
            if (vd) {
              tree_field_set_float(car, "power",
                                   tree_field_get_float(vd, "power"));
              tree_field_set_float(car, "optical",
                                   tree_field_get_float(vd, "optical"));
              tree_field_set_float(car, "tear",
                                   tree_field_get_float(vd, "tear"));
              tree_field_set_float(car, "wear",
                                   tree_field_get_float(vd, "wear"));
              tree_field_set_int(car, "colorIndex",
                                 tree_field_get_int(vd, "colorIndex"));
            }
            tree_field_set_int(car, "driveable", 1);
            tree_field_set_obj(car, "owner", self);
          }
          if (map) tree_field_set_obj(self, "world", map);
          tree_field_set_obj(self, "car", car);
          std::vector<JvmValue> ec = {JvmValue::make_obj(self),
                                      JvmValue::make_obj(car)};
          call_by_name("java.game.Bot", "enterCar", ec, false);
          return JvmValue::make_void();
        }
      } else if (stolen) {
        InvObject* bot = tree_field_get_obj(self, "raceBot");
        if (!ok_bot(bot)) {
          InvObject* st = game_logic_actual_state();
          bot = st ? tree_field_get_obj(st, "raceBot") : nullptr;
        }
        if (ok_bot(bot)) {
          if (!tree_field_get_obj(bot, "botVd"))
            tree_field_set_obj(bot, "botVd",
                               game_logic_get_vehicle_descriptor(1, -1.f));
          std::vector<JvmValue> a = {JvmValue::make_obj(bot)};
          for (size_t i = 1; i < pargs->size(); ++i) a.push_back((*pargs)[i]);
          return call_by_name("java.game.Bot", "createCar", a, false);
        }
      }
    }

    // GameType.java wrappers → registerCallback natives (PE @ 0x00481C40).
    if (!prefer_static && method && !pargs->empty() &&
        (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      if (std::strcmp(method, "enableAnimateHook") == 0 && argc == 0) {
        java_lang_GameType_registerCallback(self, 28);  // GII_ANIMATE
        return JvmValue::make_void();
      }
      if (std::strcmp(method, "disableAnimateHook") == 0 && argc == 0) {
        java_lang_GameType_unregisterCallback(self, 28);
        return JvmValue::make_void();
      }
      if (std::strcmp(method, "enableControlHook") == 0 && argc == 0) {
        java_lang_GameType_registerCallback(self, 8);  // GII_CONTROL
        return JvmValue::make_void();
      }
      if (std::strcmp(method, "disableControlHook") == 0 && argc == 0) {
        java_lang_GameType_unregisterCallback(self, 8);
        return JvmValue::make_void();
      }
      // Track/City/Nav/map methods mis-bound onto RaceSetup during CAS.
      const char* hc = tree_host_class(self);
      if (hc && (std::strstr(hc, "RaceSetup") || std::strstr(hc, "Valocity") ||
                 std::strstr(hc, "City") || std::strstr(hc, "Track"))) {
        InvObject* track = tree_field_get_obj(self, "track");
        if (!track) track = tree_field_get_obj(self, "lastState");
        // Host handoff / TREE putfield may lag — prev_state is Valocity.
        if (!track && pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Obj)
          track = (*pargs)[1].v.o;
        if (!track) {
          InvObject* st = game_logic_actual_state();
          const char* sc = st ? tree_host_class(st) : nullptr;
          if (sc && (std::strstr(sc, "Valocity") || std::strstr(sc, "City")))
            track = st;
        }
        InvObject* map = track ? tree_field_get_obj(track, "map") : nullptr;
        if (!map) map = tree_field_get_obj(self, "map");
        InvObject* nav = track ? tree_field_get_obj(track, "nav") : nullptr;
        if (!nav && self) nav = tree_field_get_obj(self, "nav");
        InvObject* osd = tree_field_get_obj(self, "osd");
        if (!osd && track) osd = tree_field_get_obj(track, "osd");
        InvObject* raceBot = tree_field_get_obj(self, "raceBot");
        if (!raceBot && track) raceBot = tree_field_get_obj(track, "raceBot");
        auto host_has = [](InvObject* o, const char* n) -> bool {
          const char* c = o ? tree_host_class(o) : nullptr;
          return c && std::strstr(c, n);
        };
        auto host_has_or_empty = [](InvObject* o, const char* n) -> bool {
          const char* c = o ? tree_host_class(o) : nullptr;
          if (!c) return false;
          if (!c[0]) return true;
          return std::strstr(c, n) != nullptr;
        };
        auto retarget = [&](InvObject* recv, const char* owner) -> JvmValue {
          if (!recv || recv == self) return JvmValue::make_void();
          std::vector<JvmValue> a = {JvmValue::make_obj(recv)};
          for (size_t i = 1; i < pargs->size(); ++i)
            a.push_back((*pargs)[i]);
          return call_by_name(owner, method, a, false);
        };
        if (hc && std::strstr(hc, "RaceSetup") && track && track != self &&
            (std::strcmp(method, "createQuickRaceBot") == 0 ||
             std::strcmp(method, "destroyRaceBot") == 0 ||
             std::strcmp(method, "startRace2") == 0 ||
             std::strcmp(method, "startRace") == 0 ||
             std::strcmp(method, "abandonRace") == 0 ||
             std::strcmp(method, "changeCamTarget2") == 0)) {
          const char* tc = tree_host_class(track);
          return retarget(track, tc && tc[0] ? tc : "java.game.City");
        }
        if (osd && osd != self && host_has_or_empty(osd, "Osd") &&
            !host_has(osd, "dialog") &&
            (std::strcmp(method, "createHotkey") == 0 ||
             std::strcmp(method, "createMenu") == 0 ||
             std::strcmp(method, "createBG") == 0 ||
             std::strcmp(method, "createText") == 0 ||
             std::strcmp(method, "createButton") == 0 ||
             std::strcmp(method, "endGroup") == 0 ||
             std::strcmp(method, "beginGroup") == 0 ||
             std::strcmp(method, "hideGroup") == 0 ||
             std::strcmp(method, "showGroup") == 0 ||
             std::strcmp(method, "show") == 0 ||
             std::strcmp(method, "hide") == 0 ||
             std::strcmp(method, "removeAllElements") == 0)) {
          return retarget(osd, "java.render.Osd");
        }
        if (std::strcmp(method, "createCar") == 0 && raceBot &&
            raceBot != self && host_has_or_empty(raceBot, "Bot") &&
            !(hc && std::strstr(hc, "Bot"))) {
          if (!tree_field_get_obj(raceBot, "botVd"))
            tree_field_set_obj(raceBot, "botVd",
                               game_logic_get_vehicle_descriptor(1, -1.f));
          return retarget(raceBot, "java.game.Bot");
        }
        if (map && map != self && host_has_or_empty(map, "GroundRef") &&
            std::strcmp(method, "alignToRoad") == 0) {
          return retarget(map, "java.util.resource.GroundRef");
        }
        if ((std::strcmp(method, "cleanupNightRace") == 0 ||
             std::strcmp(method, "changeCamNone") == 0 ||
             std::strcmp(method, "enableOsd") == 0 ||
             std::strcmp(method, "leaveAsyncMode_Script") == 0 ||
             std::strcmp(method, "leaveAsyncMode") == 0) &&
            argc <= 1 && track && track != self) {
          const char* tc = tree_host_class(track);
          return retarget(track, tc && tc[0] ? tc : "java.game.City");
        }
        if (map && map != self && host_has_or_empty(map, "GroundRef") &&
            (std::strcmp(method, "getNearestCross") == 0 ||
             std::strcmp(method, "getStartDirection") == 0 ||
             std::strcmp(method, "getRouteLength") == 0 ||
             std::strcmp(method, "findRoute") == 0 ||
             std::strcmp(method, "addTraffic") == 0 ||
             std::strcmp(method, "setPedestrianDensity") == 0)) {
          return retarget(map, "java.util.resource.GroundRef");
        }
        if (std::strcmp(method, "addMarker") == 0 ||
            std::strcmp(method, "remMarker") == 0 ||
            std::strcmp(method, "updateNavigator") == 0 ||
            std::strcmp(method, "changeMode") == 0 ||
            std::strcmp(method, "changeSize") == 0 ||
            std::strcmp(method, "changeZoom") == 0) {
          if (nav && nav != self &&
              (host_has(nav, "Navigator") || host_has(nav, "Nav"))) {
            const char* nc = tree_host_class(nav);
            return retarget(nav, nc && nc[0] ? nc : "java.game.Navigator");
          }
          // Soft no-op — avoid miss spam mid-enter before nav is wired.
          if (std::strcmp(method, "addMarker") == 0)
            return JvmValue::make_obj(nullptr);
          return JvmValue::make_void();
        }
        if (std::strcmp(method, "plotRoute") == 0) {
          // Stock RenderRef.plotRoute(parent, type, color, step, scale).
          // TREE often packs (IFFIF) onto RaceSetup — recover nav.route + defaults.
          InvObject* line = nav ? tree_field_get_obj(nav, "route") : nullptr;
          if (!line) line = tree_field_get_obj(self, "route");
          InvObject* parent = nav ? tree_field_get_obj(nav, "localroot") : nullptr;
          InvObject* type = nullptr;
          int32_t color = static_cast<int32_t>(0xFFFF0000);
          float step = 10.f;
          InvObject* scale = vec3_new(0.01f, 0.f, 0.01f);
          for (size_t i = 1; i < pargs->size(); ++i) {
            const JvmValue& a = (*pargs)[i];
            if (a.tag == JvmTag::Obj && a.v.o) {
              const char* ac = tree_host_class(a.v.o);
              if (ac && std::strstr(ac, "Vector3"))
                scale = a.v.o;
              else if (ac && (std::strstr(ac, "ResourceRef") ||
                              std::strstr(ac, "GameRef") ||
                              std::strstr(ac, "Dummy"))) {
                if (!parent)
                  parent = a.v.o;
                else if (!type)
                  type = a.v.o;
              }
            } else if (a.tag == JvmTag::Int) {
              if (a.v.i > 256 || (static_cast<uint32_t>(a.v.i) & 0xFF000000u))
                color = a.v.i;
            } else if (a.tag == JvmTag::Float) {
              if (a.v.f > 1.f) step = a.v.f;
            }
          }
          if (!line) line = resref_new();
          if (!type) type = resref_new();
          if (nav) tree_field_set_obj(nav, "route", line);
          return JvmValue::make_int(java_util_resource_RenderRef_plotRoute(
              line, parent, type, color, step, scale));
        }
        if (std::strcmp(method, "timeWarp") == 0) {
          std::vector<JvmValue> a;
          for (size_t i = 1; i < pargs->size(); ++i) a.push_back((*pargs)[i]);
          if (a.empty()) a.push_back(JvmValue::make_float(0.f));
          return call_by_name("java.lang.System", method, a, true);
        }
      }
    }

    // GroundRef.java wrappers (not PE natives). Prefer leaf over broken TREE
    // body that re-invokes addTrafficN with argc=0. Also fix Valocity-as-recv.
    if (!prefer_static && method && !pargs->empty() &&
        (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o &&
        (std::strcmp(method, "addTraffic") == 0 ||
         std::strcmp(method, "addTrafficN") == 0 ||
         std::strcmp(method, "setPedestrianDensity") == 0 ||
         std::strcmp(method, "getNearestCross") == 0 ||
         std::strcmp(method, "haltTrafficCross") == 0 ||
         std::strcmp(method, "alignToRoad") == 0)) {
      auto resolve_map = [&](InvObject* self) -> InvObject* {
        if (!self) return nullptr;
        const char* hc = tree_host_class(self);
        if (hc && std::strstr(hc, "GroundRef")) return self;
        if (InvObject* map = tree_field_get_obj(self, "map")) return map;
        if (InvObject* track = tree_field_get_obj(self, "track")) {
          if (InvObject* map = tree_field_get_obj(track, "map")) return map;
        }
        if (InvObject* last = tree_field_get_obj(self, "lastState")) {
          if (InvObject* map = tree_field_get_obj(last, "map")) return map;
        }
        for (size_t i = 1; i < pargs->size(); ++i) {
          if ((*pargs)[i].tag != JvmTag::Obj || !(*pargs)[i].v.o) continue;
          const char* ac = tree_host_class((*pargs)[i].v.o);
          if (ac && std::strstr(ac, "GroundRef")) return (*pargs)[i].v.o;
        }
        return nullptr;
      };
      // Stock Java: getNearestCross(approx) → getNearestCross(approx, 0.0).
      // TREE 1-arg body re-invokes with argc=1 forever — call native here.
      if (std::strcmp(method, "getNearestCross") == 0 && argc == 1) {
        InvObject* map = resolve_map((*pargs)[0].v.o);
        InvObject* approx =
            (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Obj)
                ? (*pargs)[1].v.o
                : nullptr;
        if (map)
          return JvmValue::make_obj(
              java_util_resource_GroundRef_getNearestCross(map, approx, 0.f));
      }
      // GroundRef.alignToRoad(Vector3) @ 0x00483E60. TREE often argc=0 onto City.
      if (std::strcmp(method, "alignToRoad") == 0) {
        InvObject* map = resolve_map((*pargs)[0].v.o);
        InvObject* rp = nullptr;
        for (size_t i = 1; i < pargs->size(); ++i) {
          if ((*pargs)[i].tag != JvmTag::Obj || !(*pargs)[i].v.o) continue;
          const char* ac = tree_host_class((*pargs)[i].v.o);
          if (ac && std::strstr(ac, "Vector3")) {
            rp = (*pargs)[i].v.o;
            break;
          }
          if (!rp) rp = (*pargs)[i].v.o;
        }
        if (!rp) {
          InvObject* city = (*pargs)[0].v.o;
          rp = tree_field_get_obj(city, "pS");
          if (!rp) rp = tree_field_get_obj(city, "posStart");
          if (!rp) {
            InvObject* player = tree_field_get_obj(city, "player");
            if (!player) player = game_logic_player();
            if (player) {
              if (InvObject* car = tree_field_get_obj(player, "car"))
                rp = tree_field_get_obj(car, "pos");
            }
          }
        }
        if (!map) map = (*pargs)[0].v.o;
        if (!rp) rp = vec3_new(0.f, 0.f, 0.f);
        return JvmValue::make_obj(
            java_util_resource_GroundRef_alignToRoad(map, rp));
      }
      // Stock: map.haltTrafficCross(raceStart, 15.0). TREE often swaps to (F,Obj).
      if (std::strcmp(method, "haltTrafficCross") == 0 && argc >= 1) {
        InvObject* map = resolve_map((*pargs)[0].v.o);
        InvObject* pos = nullptr;
        float time = 15.f;
        for (size_t i = 1; i < pargs->size(); ++i) {
          const JvmValue& a = (*pargs)[i];
          if (a.tag == JvmTag::Obj && a.v.o) {
            const char* ac = tree_host_class(a.v.o);
            if (ac && std::strstr(ac, "Vector3"))
              pos = a.v.o;
            else if (!pos)
              pos = a.v.o;
          } else if (a.tag == JvmTag::Float) {
            time = a.v.f;
          } else if (a.tag == JvmTag::Int) {
            time = static_cast<float>(a.v.i);
          }
        }
        if (map) {
          java_util_resource_GroundRef_haltTrafficCross(map, pos, time);
          return JvmValue::make_void();
        }
      }
      if (std::strcmp(method, "addTraffic") == 0 ||
          std::strcmp(method, "addTrafficN") == 0 ||
          std::strcmp(method, "setPedestrianDensity") == 0) {
      auto arg_int = [&](size_t i) -> int32_t {
        if (i >= pargs->size()) return 0;
        if ((*pargs)[i].tag == JvmTag::Int) return (*pargs)[i].v.i;
        if ((*pargs)[i].tag == JvmTag::Float)
          return static_cast<int32_t>((*pargs)[i].v.f);
        return 0;
      };
      auto arg_float = [&](size_t i) -> float {
        if (i >= pargs->size()) return 0.f;
        if ((*pargs)[i].tag == JvmTag::Float) return (*pargs)[i].v.f;
        if ((*pargs)[i].tag == JvmTag::Int)
          return static_cast<float>((*pargs)[i].v.i);
        return 0.f;
      };
      auto config_f = [](const char* key, float def) -> float {
        InvObject* cfg = system_config_host();
        if (!cfg) return def;
        return tree_field_get_float(cfg, key);
      };
      if ((std::strcmp(method, "addTraffic") == 0 ||
           std::strcmp(method, "addTrafficN") == 0) &&
          (argc == 5 || argc == 2 || argc >= 1)) {
        InvObject* map = resolve_map((*pargs)[0].v.o);
        InvObject* type =
            (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Obj)
                ? (*pargs)[1].v.o
                : nullptr;
        if (map && type) {
          const float dens = config_f("trafficDensity", 1.f);
          int32_t n = arg_int(2);
          // Java addTraffic multiplies n*Config.trafficDensity; addTrafficN
          // @ 0x00484050 does not (it reads trafficDensity itself).
          if (std::strcmp(method, "addTraffic") == 0)
            n = static_cast<int32_t>(static_cast<float>(n) * dens);
          const float lb = argc >= 5 ? arg_float(3) : 1.f;
          const float le = argc >= 5 ? arg_float(4) : 4.f;
          const float wb = argc >= 5 ? arg_float(5) : 2.f;
          return JvmValue::make_int(java_util_resource_GroundRef_addTrafficN(
              map, type, n, lb, le, wb));
        }
      }
      if (std::strcmp(method, "setPedestrianDensity") == 0 && argc == 1) {
        InvObject* map = resolve_map((*pargs)[0].v.o);
        if (map) {
          const float dens = config_f("pedestrianDensity", 0.f);
          java_util_resource_GroundRef_setPedestrianDensityN(
              map, arg_float(1) * dens);
          return JvmValue::make_void();
        }
      }
      }  // addTraffic / setPedestrianDensity
    }

    // Stock GameRef/RenderRef.setMatrix(Vector3, Ypr). finishObject uses null
    // Ypr (TREE packs as Int 0) → JNI shape (Ljava.lang.Object;I)V misses.
    if (!prefer_static && method && std::strcmp(method, "setMatrix") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o &&
        argc >= 1 && argc <= 2) {
      InvObject* self = (*pargs)[0].v.o;
      const char* hc = tree_host_class(self);
      InvObject* pos = nullptr;
      InvObject* ori = nullptr;
      for (size_t i = 1; i < pargs->size(); ++i) {
        const JvmValue& a = (*pargs)[i];
        if (a.tag != JvmTag::Obj || !a.v.o) continue;
        const char* ac = tree_host_class(a.v.o);
        if (ac && std::strstr(ac, "Ypr"))
          ori = a.v.o;
        else if (ac && std::strstr(ac, "Vector3") && !pos)
          pos = a.v.o;
        else if (!pos)
          pos = a.v.o;
      }
      if (hc && std::strstr(hc, "RenderRef"))
        java_util_resource_RenderRef_setMatrix_1(self, pos, ori);
      else
        java_util_resource_GameRef_setMatrix(self, pos, ori);
      return JvmValue::make_void();
    }

    // Animation.setSpeed(F)V @ 0x0047EC00. TREE 0x25 packs argc=0 onto
    // ResourceRef (RenderRef/Animation host-class often collapsed).
    if (!prefer_static && method && std::strcmp(method, "setSpeed") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      float spd = 1.f;
      for (size_t i = 1; i < pargs->size(); ++i) {
        if ((*pargs)[i].tag == JvmTag::Float) {
          spd = (*pargs)[i].v.f;
          break;
        }
        if ((*pargs)[i].tag == JvmTag::Int) {
          spd = static_cast<float>((*pargs)[i].v.i);
          break;
        }
      }
      java_util_resource_Animation_setSpeed((*pargs)[0].v.o, spd);
      return JvmValue::make_void();
    }

    // Prefer native table overload matching argc on the named class.
    auto argc_of = [](const NativeEntry& e) -> int {
      const char* js = e.java_signature;
      if (!js) return 0;
      const char* paren = std::strchr(js, '(');
      if (!paren || paren[1] == ')') return 0;
      int n = 1;
      for (const char* p = paren; *p && *p != ')'; ++p)
        if (*p == ',') ++n;
      return n;
    };
    const NativeEntry* best = nullptr;
    for (size_t i = 0; i < kNativeTableCount; ++i) {
      if (std::strcmp(kNativeTable[i].class_fqn, class_fqn) != 0) continue;
      if (std::strcmp(kNativeTable[i].method_name, method) != 0) continue;
      if (kNativeTable[i].is_static != prefer_static) continue;
      if (argc_of(kNativeTable[i]) == argc) {
        best = &kNativeTable[i];
        break;
      }
    }
    // Cross-class fallback: GameLogic TREE invokes ResourceRef.getFirstChild/cache
    // while owner is still java.game.GameLogic. Never steal Menu.addItem → SfxTable.
    // Never steal <init> across classes (Style → Animation noise).
    if (!best || argc_of(*best) != argc) {
      best = nullptr;
      const bool skip_cross_additem =
          method && std::strcmp(method, "addItem") == 0;
      const bool skip_cross_init =
          method && std::strcmp(method, "<init>") == 0;
      for (size_t i = 0; i < kNativeTableCount; ++i) {
        if (std::strcmp(kNativeTable[i].method_name, method) != 0) continue;
        if (kNativeTable[i].is_static != prefer_static) continue;
        if (skip_cross_init) continue;
        if (skip_cross_additem &&
            std::strstr(kNativeTable[i].class_fqn, "SfxTable"))
          continue;
        // Exact argc only — wrong-arity GameRef.queueEvent blew the stack when
        // TREE named MouseCursor.queueEvent with argc=0.
        if (argc_of(kNativeTable[i]) == argc) {
          best = &kNativeTable[i];
          break;
        }
      }
    }
    // RenderRef.plotRoute(parent, type, color, step, scale) — TREE often
    // packs primitives only (IFFIF). Recover from nav.route / defaults.
    if (method && std::strcmp(method, "plotRoute") == 0 && !pargs->empty() &&
        (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* line = (*pargs)[0].v.o;
      const char* lc = tree_host_class(line);
      InvObject* nav = nullptr;
      if (lc && std::strstr(lc, "RaceSetup")) {
        InvObject* track = tree_field_get_obj(line, "track");
        if (!track) track = tree_field_get_obj(line, "lastState");
        nav = track ? tree_field_get_obj(track, "nav") : nullptr;
        InvObject* route = nav ? tree_field_get_obj(nav, "route") : nullptr;
        if (route) line = route;
      } else if (lc && std::strstr(lc, "Navigator")) {
        nav = line;
        InvObject* route = tree_field_get_obj(nav, "route");
        if (route) line = route;
      }
      InvObject* parent = nav ? tree_field_get_obj(nav, "localroot") : nullptr;
      InvObject* type = nullptr;
      int32_t color = static_cast<int32_t>(0xFFFF0000);
      float step = 10.f;
      InvObject* scale = vec3_new(0.01f, 0.f, 0.01f);
      for (size_t i = 1; i < pargs->size(); ++i) {
        const JvmValue& a = (*pargs)[i];
        if (a.tag == JvmTag::Obj && a.v.o) {
          const char* ac = tree_host_class(a.v.o);
          if (ac && std::strstr(ac, "Vector3"))
            scale = a.v.o;
          else if (ac && (std::strstr(ac, "ResourceRef") ||
                          std::strstr(ac, "GameRef") ||
                          std::strstr(ac, "Dummy"))) {
            if (!parent)
              parent = a.v.o;
            else if (!type)
              type = a.v.o;
          }
        } else if (a.tag == JvmTag::Int) {
          if (a.v.i > 256 || (static_cast<uint32_t>(a.v.i) & 0xFF000000u))
            color = a.v.i;
        } else if (a.tag == JvmTag::Float) {
          if (a.v.f > 1.f) step = a.v.f;
        }
      }
      if (!type) type = resref_new();
      if (nav) tree_field_set_obj(nav, "route", line);
      return JvmValue::make_int(java_util_resource_RenderRef_plotRoute(
          line, parent, type, color, step, scale));
    }
    if (best && best->fn && argc_of(*best) == argc) {
      // UnboxArg @ 0x0045D910: registered descriptor, not TREE arg tags.
      std::string sig = java_sig_to_jni(best->java_signature);
      if (sig.empty()) {
      size_t start = prefer_static ? 0 : 1;
      char ret = 'V';
      if (best->java_signature) {
        if (std::strncmp(best->java_signature, "int ", 4) == 0) ret = 'I';
        else if (std::strncmp(best->java_signature, "float ", 6) == 0)
          ret = 'F';
        else if (std::strncmp(best->java_signature, "void ", 5) == 0)
          ret = 'V';
        else
            ret = 'L';
      }
        sig = "(";
      for (size_t i = start; i < pargs->size(); ++i) {
        if ((*pargs)[i].tag == JvmTag::Float) sig += 'F';
        else if ((*pargs)[i].tag == JvmTag::Obj) sig += "Ljava.lang.Object;";
        else sig += 'I';
      }
      sig += ')';
      if (ret == 'L') sig += "Ljava.lang.Object;";
      else sig += ret;
      }

      CallFrame frame;
      frame.class_fqn = best->class_fqn;
      frame.method_name = method;
      frame.jni_signature = sig.c_str();
      frame.is_static = prefer_static;
      frame.args = *pargs;
      std::string e;
      if (!call_native(best, &frame, &e)) {
        std::fprintf(stderr, "[jvm] call_by_name native failed %s.%s: %s\n",
                     best->class_fqn, method, e.c_str());
        return JvmValue::make_void();
      }
      return frame.result;
    }

    // Fall back to class method table + JNI sig match by argc.
    if (cls) {
      const JvmMethod* tree_init = nullptr;
      const JvmMethod* any_match = nullptr;
      for (auto& m : cls->methods) {
        if (m.name != method) continue;
        int n = count_jni_args(m.signature.c_str());
        if (n != argc) continue;
        if (!any_match) any_match = &m;
        const bool has_nodes =
            m.tree_index >= 0 &&
            static_cast<size_t>(m.tree_index) < cls->trees.size() &&
            !cls->trees[static_cast<size_t>(m.tree_index)].nodes.empty();
        if (has_nodes) {
          tree_init = &m;
          break;
        }
      }
      // City ships stub/native <init> MTHD rows plus FILD posGarage — prefer a
      // real TREE body, else fall through to the super-walk below.
      const JvmMethod* pick = tree_init ? tree_init : any_match;
      if (pick && (tree_init || method == nullptr ||
                   std::strcmp(method, "<init>") != 0 ||
                   cls->field_inits.empty())) {
        return jvm_->invoke(class_fqn, method, pick->signature.c_str(), args,
                            prefer_static);
      }
    }

    // City has FILD but no TREE <init> — walk supers via invoke (Scene/Track).
    if (method && std::strcmp(method, "<init>") == 0 && cls &&
        !cls->field_inits.empty()) {
      return jvm_->invoke(class_fqn, "<init>", "()V", args, prefer_static);
    }

    // Inherited TREE on *_VT: call_by_name only scanned the leaf MTHD table.
    // Narrow fallback — a blanket invoke() steals common names (show/hide).
    if (cls && method && !cls->super_name.empty() &&
        std::strcmp(method, "addColorIndex") == 0) {
      return jvm_->invoke(class_fqn, method, "(I)V", *pargs, prefer_static);
    }

    // Inherited TREE: Valocity.startRace / createQuickRaceBot live on City.
    if (cls && method) {
      std::string super = cls->super_name;
      for (int depth = 0; !super.empty() && depth < 16; ++depth) {
        if (!jvm_->find_class(super.c_str())) jvm_->load_class(super.c_str());
        const JvmClass* sc = jvm_->find_class(super.c_str());
        if (!sc) break;
        for (auto& sm : sc->methods) {
          if (sm.name != method) continue;
          int n = count_jni_args(sm.signature.c_str());
          if (n != argc) continue;
          const bool has_nodes =
              sm.tree_index >= 0 &&
              static_cast<size_t>(sm.tree_index) < sc->trees.size() &&
              !sc->trees[static_cast<size_t>(sm.tree_index)].nodes.empty();
          if (has_nodes) {
            return jvm_->invoke(super.c_str(), method, sm.signature.c_str(),
                                args, prefer_static);
          }
        }
        super = sc->super_name;
      }
    }

    std::fprintf(stderr, "[jvm] call_by_name miss %s.%s argc=%d\n", class_fqn,
                 method, argc);
    return JvmValue::make_void();
  }

 private:
  Jvm* jvm_;
};

}  // namespace

bool resolve_classpath_file(const char* game_root, const char* fqn,
                            std::string* out_path) {
  if (!game_root || !fqn || !out_path) return false;
  const ClasspathMapEntry* best = nullptr;
  size_t best_len = 0;
  for (size_t i = 0; i < kClasspathMapCount; ++i) {
    const char* pkg = kClasspathMap[i].java_package;
    const size_t plen = std::strlen(pkg);
    if (std::strncmp(fqn, pkg, plen) != 0) continue;
    if (fqn[plen] != '.' && fqn[plen] != '\0') continue;
    if (plen >= best_len) {
      best_len = plen;
      best = &kClasspathMap[i];
    }
  }

  auto file_ok = [](const std::string& p) -> bool {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
  };

  if (best) {
    const char* rest = fqn + best_len;
    if (*rest == '.') ++rest;
    *out_path = std::string(game_root);
    if (!out_path->empty() && out_path->back() != '/' &&
        out_path->back() != '\\') {
      out_path->push_back('/');
    }
    *out_path += best->filesystem_prefix;
    out_path->push_back('/');
    for (const char* p = rest; *p; ++p) {
      out_path->push_back(*p == '.' ? '/' : *p);
    }
    *out_path += ".class";
    if (file_ok(*out_path)) return true;
  }

  // java.game.cars.Baiern_VT → cars/racers/*/scripts/Baiern_VT.class
  if (std::strncmp(fqn, "java.game.cars.", 15) == 0) {
    const char* simple = fqn + 15;
    for (const char* p = simple; *p; ++p)
      if (*p == '.') simple = p + 1;
#ifdef _WIN32
    std::string pattern = std::string(game_root) + "\\cars\\racers\\*_data\\scripts\\";
    pattern += simple;
    pattern += ".class";
    // Expand *_data via FindFirstFile on racers\*
    std::string search = std::string(game_root) + "\\cars\\racers\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        std::string cand = std::string(game_root) + "/cars/racers/" +
                           fd.cFileName + "/scripts/" + simple + ".class";
        if (file_ok(cand)) {
          *out_path = cand;
          FindClose(h);
          return true;
        }
      } while (FindNextFileA(h, &fd));
      FindClose(h);
    }
#else
    (void)simple;
#endif
  }
  return best != nullptr && !out_path->empty();
}

void Jvm::set_game_root(const char* root) {
  game_root_ = root ? root : "";
  classes_.reserve(256);  // avoid realloc invalidating tree_eval snapshots
}

void Jvm::upsert_class(JvmClass cls) {
  for (auto& c : classes_) {
    if (c.name == cls.name) {
      c = std::move(cls);
      return;
    }
  }
  classes_.push_back(std::move(cls));
}

bool Jvm::load_index(const char* path) {
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "[jvm] cannot open %s\n", path);
    return false;
  }
  classes_.clear();
  JvmClass* cur = nullptr;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    auto tok = split_ws(line);
    if (tok.empty()) continue;
    if (tok[0] == "CLASS" && tok.size() >= 2) {
      classes_.push_back({});
      cur = &classes_.back();
      cur->name = tok[1];
    } else if (!cur) {
      continue;
    } else if (tok[0] == "SUPER" && tok.size() >= 2) {
      cur->super_name = tok[1];
    } else if (tok[0] == "FILE" && tok.size() >= 2) {
      cur->file = tok[1];
    } else if (tok[0] == "METHOD" && tok.size() >= 3) {
      JvmMethod m;
      m.name = tok[1];
      m.signature = tok[2];
      m.is_native = true;
      for (size_t i = 3; i < tok.size(); ++i) {
        if (tok[i] == "native=0") m.is_native = false;
        if (tok[i] == "native=1") m.is_native = true;
      }
      cur->methods.push_back(std::move(m));
    }
  }
  return !classes_.empty();
}

bool Jvm::load_class_file(const char* path) {
  std::vector<JvmClass> all;
  std::string err;
  if (!tufa_load_file_all(path, &all, &err)) {
    std::fprintf(stderr, "[jvm] tufa load failed %s: %s\n", path, err.c_str());
    return false;
  }
  for (auto& cls : all) upsert_class(std::move(cls));
  return !all.empty();
}

bool Jvm::load_class(const char* fqn) {
  if (fqn && std::strcmp(fqn, "java.render.osd.Rectangle") == 0)
    fqn = "java.render.Rectangle";
  if (find_class(fqn)) return true;
  if (game_root_.empty()) {
    std::fprintf(stderr, "[jvm] load_class: no game_root\n");
    return false;
  }
  std::string path;
  if (!resolve_classpath_file(game_root_.c_str(), fqn, &path)) {
    std::fprintf(stderr, "[jvm] no classpath entry for %s\n", fqn);
    return false;
  }
  if (file_exists(path.c_str())) {
    if (!load_class_file(path.c_str())) return false;
    return find_class(fqn) != nullptr;
  }

  // Sibling class: MainMenuDialog lives inside MainMenu.class (multi-TUFA).
  // Scan the package directory for other .class files that might contain it.
  const size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) {
    std::fprintf(stderr, "[jvm] class file missing %s\n", path.c_str());
    return false;
  }
  const std::string dir = path.substr(0, slash);
#if defined(_WIN32)
  WIN32_FIND_DATAA fd;
  const std::string pattern = dir + "\\*.class";
  HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    std::fprintf(stderr, "[jvm] class file missing %s\n", path.c_str());
    return false;
  }
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::string cand = dir + "\\" + fd.cFileName;
    if (!load_class_file(cand.c_str())) continue;
    if (find_class(fqn)) {
      FindClose(h);
      return true;
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  (void)dir;
#endif
  std::fprintf(stderr, "[jvm] class file missing %s\n", path.c_str());
  return false;
}

namespace {

int compile_scan_dir(Jvm* jvm, const std::string& dir, int depth) {
  if (!jvm || depth > 10) return 0;
  int n = 0;
#ifdef _WIN32
  WIN32_FIND_DATAA fd;
  const HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    if (fd.cFileName[0] == '.') continue;
    const std::string full = dir + "\\" + fd.cFileName;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      n += compile_scan_dir(jvm, full, depth + 1);
      continue;
    }
    const size_t len = std::strlen(fd.cFileName);
    if (len < 7) continue;
    if (_stricmp(fd.cFileName + (len - 6), ".class") != 0) continue;
    if (jvm->load_class_file(full.c_str())) ++n;
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  (void)dir;
#endif
  return n;
}

std::string join_root(const std::string& root, const char* rel) {
  std::string out = root;
  if (!out.empty() && out.back() != '/' && out.back() != '\\') out.push_back('/');
  if (rel) {
    while (*rel == '/' || *rel == '\\') ++rel;
    out += rel;
  }
  while (!out.empty() && (out.back() == '/' || out.back() == '\\')) out.pop_back();
  return out;
}

}  // namespace

int32_t Jvm::compile_all(const char* rel_path) {
  if (game_root_.empty()) return 0;
  const char* rel = (rel_path && rel_path[0]) ? rel_path : ".";
  int32_t total = 0;
  if (std::strcmp(rel, ".") == 0 || std::strcmp(rel, "./") == 0) {
    for (size_t i = 0; i < kClasspathMapCount; ++i) {
      total += compile_scan_dir(
          this, join_root(game_root_, kClasspathMap[i].filesystem_prefix), 0);
    }
  } else {
    total = compile_scan_dir(this, join_root(game_root_, rel), 0);
  }
  return total;
}

const JvmClass* Jvm::find_class(const char* fqn) const {
  for (auto& c : classes_) {
    if (c.name == fqn) return &c;
  }
  return nullptr;
}

const JvmMethod* Jvm::find_method(const JvmClass& cls, const char* name,
                                  const char* signature) const {
  const JvmMethod* by_name = nullptr;
  for (auto& m : cls.methods) {
    if (m.name != name) continue;
    if (signature && m.signature == signature) return &m;
    if (!by_name) by_name = &m;
  }
  // TUFA MTHD rows occasionally mis-pair name/sig (e.g. addCustomGroups
  // tagged "(I)V"). Fall back to first name match when exact sig misses.
  return by_name;
}

JvmValue Jvm::invoke(const char* class_fqn, const char* name, const char* signature,
                     const std::vector<JvmValue>& args, bool is_static) {
  thread_local int tls_depth = 0;
  thread_local char tls_trail[16][96];
  struct DepthGuard {
    int* d;
    DepthGuard(int* p, const char* c, const char* m) : d(p) {
      if (*d >= 0 && *d < 16)
        std::snprintf(tls_trail[*d], sizeof(tls_trail[*d]), "%.60s.%.30s",
                      c ? c : "?", m ? m : "?");
      ++(*d);
    }
    ~DepthGuard() { --(*d); }
  } depth_guard(&tls_depth, class_fqn, name);
  if (tls_depth > 48) {
    std::fprintf(stderr, "[jvm] invoke depth=%d abort %s.%s\n", tls_depth,
                 class_fqn ? class_fqn : "?", name ? name : "?");
    for (int i = 0; i < tls_depth && i < 16; ++i)
      std::fprintf(stderr, "  #%d %s\n", i, tls_trail[i]);
    return JvmValue::make_void();
  }

  if (name && std::strcmp(name, "queueEvent") == 0) {
    if (args.size() <= 1) return JvmValue::make_void();
    queue_event_apply(args);
    return JvmValue::make_void();
  }
  if (name && std::strcmp(name, "create") == 0 && !args.empty() &&
      args[0].tag == JvmTag::Obj && args[0].v.o &&
      is_renderref_not_camera(args[0].v.o, class_fqn)) {
    if (args.size() <= 1) return JvmValue::make_void();
    renderref_apply_create(args);
    return JvmValue::make_void();
  }
  if (name && (std::strcmp(name, "addTraffic") == 0 ||
               std::strcmp(name, "addTrafficN") == 0) &&
      !args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
    InvObject* map = args[0].v.o;
    const char* hc = tree_host_class(map);
    if (!hc || !std::strstr(hc, "GroundRef")) {
      if (InvObject* m = tree_field_get_obj(map, "map")) map = m;
    }
    InvObject* type = nullptr;
    int32_t n = 0;
    float lb = 2.f, le = 5.f, wb = 2.f;
    for (size_t i = 1; i < args.size(); ++i) {
      const JvmValue& a = args[i];
      if (a.tag == JvmTag::Obj && a.v.o && !type)
        type = a.v.o;
      else if (a.tag == JvmTag::Int || a.tag == JvmTag::Float) {
        const float f = a.tag == JvmTag::Float ? a.v.f
                                               : static_cast<float>(a.v.i);
        if (n == 0 && a.tag == JvmTag::Int)
          n = a.v.i;
        else if (n == 0)
          n = static_cast<int32_t>(f);
        else if (lb == 2.f)
          lb = f;
        else if (le == 5.f)
          le = f;
        else
          wb = f;
      }
    }
    if (map && type) {
      if (std::strcmp(name, "addTraffic") == 0) {
        InvObject* cfg = system_config_host();
        const float dens =
            cfg ? tree_field_get_float(cfg, "trafficDensity") : 1.f;
        n = static_cast<int32_t>(static_cast<float>(n) * dens);
      }
      return JvmValue::make_int(java_util_resource_GroundRef_addTrafficN(
          map, type, n, lb, le, wb));
    }
  }
  if (name && std::strcmp(name, "addSceneElements") == 0) {
    InvObject* self = nullptr;
    if (!args.empty() && args[0].tag == JvmTag::Obj) self = args[0].v.o;
    if (!self) self = game_logic_actual_state();
    if (self) valocity_apply_scene(self);
    return JvmValue::make_void();
  }
  if (name && std::strcmp(name, "time2Config") == 0) {
    float t = game_logic_time();
    InvObject* self = nullptr;
    if (!is_static && !args.empty() && args[0].tag == JvmTag::Obj)
      self = args[0].v.o;
    const size_t i0 = (!is_static && !args.empty()) ? 1 : 0;
    for (size_t i = i0; i < args.size(); ++i) {
      if (args[i].tag == JvmTag::Float) {
        t = args[i].v.f;
        break;
      }
      if (args[i].tag == JvmTag::Int) {
        t = static_cast<float>(args[i].v.i);
        break;
      }
    }
    float rnd = self ? tree_field_get_float(self, "lastSelectionSeed") : 0.f;
    return JvmValue::make_int(scene_time2config(t, rnd));
  }

  // Navigator.changeMode / changeSize / changeZoom — pure Java; leaf avoids
  // RaceSetup/RenderRef misbinds and cam.setMatrix TREE noise.
  if (!is_static && name && !args.empty() && args[0].tag == JvmTag::Obj &&
      args[0].v.o) {
    InvObject* self = args[0].v.o;
    const char* hc = tree_host_class(self);
    const bool is_nav =
        (class_fqn && std::strstr(class_fqn, "Navigator")) ||
        (hc && std::strstr(hc, "Navigator"));
    if (is_nav && std::strcmp(name, "changeMode") == 0) {
      int32_t mode = 0;
      if (args.size() >= 2) {
        if (args[1].tag == JvmTag::Int) mode = args[1].v.i;
        else if (args[1].tag == JvmTag::Float)
          mode = static_cast<int32_t>(args[1].v.f);
      }
      tree_field_set_int(self, "mode", mode);
      return JvmValue::make_void();
    }
    if (is_nav && std::strcmp(name, "changeZoom") == 0) {
      float z = 4.5f;
      if (args.size() >= 2) {
        if (args[1].tag == JvmTag::Float) z = args[1].v.f;
        else if (args[1].tag == JvmTag::Int)
          z = static_cast<float>(args[1].v.i);
      }
      tree_field_set_float(self, "zoom", z);
      return JvmValue::make_void();
    }
    if (is_nav && std::strcmp(name, "changeSize") == 0 && args.size() >= 5) {
      auto af = [&](size_t i) -> float {
        if (args[i].tag == JvmTag::Float) return args[i].v.f;
        if (args[i].tag == JvmTag::Int)
          return static_cast<float>(args[i].v.i);
        return 0.f;
      };
      tree_field_set_float(self, "vp_l", af(1));
      tree_field_set_float(self, "vp_t", af(2));
      tree_field_set_float(self, "vp_w", af(3));
      tree_field_set_float(self, "vp_h", af(4));
      return JvmValue::make_void();
    }
  }

  // Stock Valocity.enter(RaceSetup): TREE instanceof often misses, then the
  // QUICKRACE else-branch CAS(racesetup) undoes startRace. Mirror the Java gate.
  if (!is_static && name && std::strcmp(name, "enter") == 0 && class_fqn &&
      std::strstr(class_fqn, "Valocity") && args.size() >= 2 &&
      args[0].tag == JvmTag::Obj && args[0].v.o && args[1].tag == JvmTag::Obj &&
      args[1].v.o) {
    const char* pc = tree_host_class(args[1].v.o);
    if (pc && std::strstr(pc, "RaceSetup")) {
      InvObject* self = args[0].v.o;
      if (tree_field_get_int(self, "raceState") == 0) {
        InvObject* parent = tree_field_get_obj(self, "parentState");
        if (parent) game_logic_change_active_section(parent);
        tree_field_set_int(self, "enter_via_tree", 1);
        std::printf("[script] java.game.Valocity.enter via TREE (RaceSetup abandon)\n");
        return JvmValue::make_void();
      }
      frontend_loading_screen_show();
      if (InvObject* osd = tree_field_get_obj(self, "osd"))
        tree_field_set_int(osd, "visible", 1);
      tree_field_set_int(self, "entered", 1);
      tree_field_set_int(self, "enter_via_tree", 1);
      frontend_loading_screen_hide();
      std::printf("[script] java.game.Valocity.enter via TREE (RaceSetup return)\n");
      return JvmValue::make_void();
    }
  }

  // Trigger.<init> — Java body is GameRef.create; TREE this() packing is lossy.
  if (!is_static && name && std::strcmp(name, "<init>") == 0 && class_fqn &&
      std::strstr(class_fqn, "Trigger") && !args.empty() &&
      args[0].tag == JvmTag::Obj && args[0].v.o) {
    if (trigger_apply_ctor(args[0].v.o, args))
      return JvmValue::make_void();
  }

  // Thread ctors TREE: Thread(){this(this);} recurses when packing is wrong.
  // RaceSetup: new Thread(this, "…") → init(name)+target.
  if (!is_static && name && std::strcmp(name, "<init>") == 0 && class_fqn &&
      std::strstr(class_fqn, "Thread") && !args.empty() &&
      args[0].tag == JvmTag::Obj && args[0].v.o) {
    InvObject* self = args[0].v.o;
    InvObject* target = nullptr;
    InvObject* tname = nullptr;
    if (args.size() >= 2 && args[1].tag == JvmTag::Obj) {
      const char* ac = tree_host_class(args[1].v.o);
      if (ac && std::strstr(ac, "String"))
        tname = args[1].v.o;
      else
        target = args[1].v.o;
    }
    if (args.size() >= 3 && args[2].tag == JvmTag::Obj) {
      const char* ac = tree_host_class(args[2].v.o);
      if (ac && std::strstr(ac, "String"))
        tname = args[2].v.o;
      else if (!target)
        target = args[2].v.o;
    }
    if (target) tree_field_set_obj(self, "target", target);
    java_lang_Thread_init(self, tname);
    return JvmValue::make_void();
  }

  // Navigator.updateNavigator(car) → updateNavigator(car, mode) Java wrapper.
  if (!is_static && name && std::strcmp(name, "updateNavigator") == 0 &&
      !args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
    InvObject* self = args[0].v.o;
    const char* hc = tree_host_class(self);
    const bool is_nav =
        (class_fqn && std::strstr(class_fqn, "Navigator")) ||
        (hc && std::strstr(hc, "Navigator"));
    if (is_nav) {
      const int argc = static_cast<int>(args.size()) - 1;
      InvObject* car =
          (argc >= 1 && args[1].tag == JvmTag::Obj) ? args[1].v.o : nullptr;
      int32_t mode = tree_field_get_int(self, "mode");
      if (argc >= 2) {
        if (args[2].tag == JvmTag::Int) mode = args[2].v.i;
        else if (args[2].tag == JvmTag::Float)
          mode = static_cast<int32_t>(args[2].v.f);
      }
      java_game_Navigator_updateNavigator(self, car, mode);
      return JvmValue::make_void();
    }
  }

  // Navigator.addMarker overloads — TREE wrappers recurse when argc packing
  // collapses (RaceSetup static markers are the 3-arg form).
  if (!is_static && name && std::strcmp(name, "addMarker") == 0 &&
      !args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
    InvObject* self = args[0].v.o;
    const char* hc = tree_host_class(self);
    const bool is_nav =
        (class_fqn && std::strstr(class_fqn, "Navigator")) ||
        (hc && std::strstr(hc, "Navigator"));
    if (is_nav) {
      const int argc = static_cast<int>(args.size()) - 1;
      if (argc >= 3 && args[1].tag == JvmTag::Obj &&
          args[2].tag == JvmTag::Obj) {
        int32_t rtype = 0;
        if (args[1].v.o) rtype = tree_field_get_int(args[1].v.o, "id");
        float px = 0.f, pz = 0.f;
        if (args[2].v.o) {
          px = tree_field_get_float(args[2].v.o, "x");
          pz = tree_field_get_float(args[2].v.o, "z");
        }
        int32_t pri = 0;
        if (args[3].tag == JvmTag::Int) pri = args[3].v.i;
        else if (args[3].tag == JvmTag::Float)
          pri = static_cast<int32_t>(args[3].v.f);
        return JvmValue::make_obj(
            navigator_add_marker_static(self, rtype, px, pz, pri));
      }
      if (argc == 2 && args[1].tag == JvmTag::Obj &&
          args[2].tag == JvmTag::Obj) {
        int32_t rtype = 0;
        if (args[1].v.o) rtype = tree_field_get_int(args[1].v.o, "id");
        return JvmValue::make_obj(
            navigator_add_marker_dynamic(self, rtype, args[2].v.o));
      }
      if (argc == 1 && args[1].tag == JvmTag::Obj && args[1].v.o) {
        InvObject* rc = args[1].v.o;
        InvObject* marker = tree_field_get_obj(rc, "marker");
        InvObject* car = tree_field_get_obj(rc, "car");
        int32_t rtype = marker ? tree_field_get_int(marker, "id") : 0;
        return JvmValue::make_obj(
            navigator_add_marker_dynamic(self, rtype, car));
      }
      return JvmValue::make_obj(nullptr);
    }
  }

  // GroundRef.getNearestCross(approx) Java wrapper TREE re-invokes itself when
  // packing drops the 0.0f — hit the PE native (distance=0) directly.
  if (!is_static && name && std::strcmp(name, "getNearestCross") == 0 &&
      !args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
    InvObject* self = args[0].v.o;
    const char* hc = tree_host_class(self);
    const bool named_gr =
        class_fqn && std::strstr(class_fqn, "GroundRef") != nullptr;
    const bool self_gr = hc && std::strstr(hc, "GroundRef") != nullptr;
    InvObject* map = self_gr ? self : nullptr;
    if (!map && self) {
      map = tree_field_get_obj(self, "map");
      if (!map) {
        if (InvObject* track = tree_field_get_obj(self, "track"))
          map = tree_field_get_obj(track, "map");
      }
      if (!map) {
        if (InvObject* last = tree_field_get_obj(self, "lastState"))
          map = tree_field_get_obj(last, "map");
      }
    }
    if ((named_gr || self_gr || map)) {
      if (!map) map = self;
      const int argc = static_cast<int>(args.size()) - 1;
      if (argc <= 1) {
        InvObject* approx =
            (argc >= 1 && args[1].tag == JvmTag::Obj) ? args[1].v.o : nullptr;
        return JvmValue::make_obj(java_util_resource_GroundRef_getNearestCross(
            map, approx, 0.f));
      }
      if (argc >= 2) {
        InvObject* approx =
            (args[1].tag == JvmTag::Obj) ? args[1].v.o : nullptr;
        float dist = 0.f;
        if (args[2].tag == JvmTag::Float) dist = args[2].v.f;
        else if (args[2].tag == JvmTag::Int)
          dist = static_cast<float>(args[2].v.i);
        return JvmValue::make_obj(java_util_resource_GroundRef_getNearestCross(
            map, approx, dist));
      }
    }
  }
  // Host fast-path: GameLogic.initVehicleTypes (arrays + static field).
  if (is_static && name && std::strcmp(name, "initVehicleTypes") == 0 &&
      class_fqn && std::strcmp(class_fqn, "java.game.GameLogic") == 0) {
    const int32_t n = game_logic_init_vehicle_types();
    return JvmValue::make_int(n);
  }
  // LoadingScreen / HotkeyWatcher run loops: C++ mirrors (TREE spins / races).
  if (!is_static && name && std::strcmp(name, "run") == 0 && class_fqn &&
      !args.empty() && args[0].tag == JvmTag::Obj) {
    if (std::strstr(class_fqn, "LoadingScreen")) {
      frontend_loading_screen_run(args[0].v.o);
      return JvmValue::make_void();
    }
    if (std::strstr(class_fqn, "HotkeyWatcher")) {
      frontend_hotkey_watcher_run(args[0].v.o);
      return JvmValue::make_void();
    }
  }
  // Dialog.display — stock Java wait()/notify(); host auto-accepts after show.
  if (!is_static && name && std::strcmp(name, "display") == 0 && class_fqn &&
      std::strstr(class_fqn, "Dialog") && !args.empty() &&
      args[0].tag == JvmTag::Obj && args[0].v.o) {
    return JvmValue::make_int(dialog_display(args[0].v.o));
  }
  // MainMenuDialog / OptionsDialog.show — huge TREE (video modes + menus) hits
  // invoke-depth abort and leaves incomplete chrome. Leaf: music/FMV +
  // addCustomGroups TREE + Dialog shown.
  if (!is_static && name && std::strcmp(name, "show") == 0 && class_fqn &&
      (std::strstr(class_fqn, "MainMenuDialog") ||
       std::strstr(class_fqn, "OptionsDialog")) &&
      !args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
    InvObject* self = args[0].v.o;
    InvObject* osd = tree_field_get_obj(self, "osd");
    if (!osd) {
      osd = tree_host_new("java.render.Osd");
      tree_field_set_obj(self, "osd", osd);
    }
    osd_ensure_defaults(osd);
    if (std::strstr(class_fqn, "MainMenuDialog")) {
      java_sound_Sound_changeMusicSet(3);
      const int32_t fmv_ok =
          video_fmv_open("data\\fmv\\prime.avi", 1, 1);
      tree_field_set_int(self, "bgVideoActive", fmv_ok == 0 ? 1 : 0);
    }
    // Prefer stock addCustomGroups TREE (createBG/header/menus via leaves).
    std::vector<JvmValue> acg = {JvmValue::make_obj(self)};
    // Avoid re-entering this show leaf: call_by_name path for addCustomGroups.
    // Use invoke with a name that has TREE; temporarily no show recursion.
    {
      // Direct tree eval via invoke on addCustomGroups only.
      const char* owner = class_fqn;
      if (std::strstr(class_fqn, "MainMenuDialog"))
        owner = "java.game.MainMenuDialog";
      invoke(owner, "addCustomGroups", "()V", acg, false);
    }
    tree_field_set_int(osd, "visible", 1);
    tree_field_set_int(self, "shown", 1);
    tree_field_set_int(self, "show_via_tree", 1);
    const int32_t btn = tree_field_get_int(osd, "button_count");
    const int32_t txt = render_d3d9_osd_text_count();
    if (btn >= 12 && txt >= 12)
      tree_field_set_int(self, "menu_chrome", 1);
    std::printf("[script] %s.show via TREE addCustomGroups btn=%d txt=%d chrome=%d\n",
                class_fqn, btn, txt, tree_field_get_int(self, "menu_chrome"));
    return JvmValue::make_void();
  }
  // SplashScreen.enter — run TREE (createBG/createHotkey are engine leaves).
  // SplashScreen.exit / MainMenu.exit — TREE often mis-packs argc; host mirrors
  // the Java body. SplashScreen.exit MUST clearEventMask(EVENT_ANY) or the
  // addTimer(3,1) oneshot still has EVENT_TIME and CAS(MainMenu) mid-Valocity.
  if (!is_static && name && std::strcmp(name, "exit") == 0 && class_fqn &&
      (std::strstr(class_fqn, "SplashScreen") ||
       std::strcmp(class_fqn, "java.game.MainMenu") == 0) &&
      !args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
    InvObject* self = args[0].v.o;
    tree_field_set_int(self, "entered", 0);
    tree_field_set_int(self, "exit_via_tree", 1);
    if (std::strstr(class_fqn, "SplashScreen")) {
      // Java: clearEventMask(EVENT_ANY); osd.hide();
      constexpr int32_t kEventAny = 0x0FFFFFFF;
      java_lang_GameType_clearEventMask(self, kEventAny);
      if (InvObject* osd = tree_field_get_obj(self, "osd")) {
        tree_field_set_int(osd, "visible", 0);
        java_lang_GameType_clearEventMask(osd, kEventAny);
      }
    }
    if (std::strcmp(class_fqn, "java.game.MainMenu") == 0) {
      if (InvObject* mmd = tree_field_get_obj(self, "mmd")) {
        tree_field_set_int(mmd, "shown", 0);
        video_fmv_close();
        tree_field_set_obj(self, "mmd", nullptr);
      }
    }
    std::printf("[script] %s.exit via engine mirror\n", class_fqn);
    return JvmValue::make_void();
  }
  // Garage.enter / exit: TREE-first (createOSDObjects + Scene leaves).
  // On TREE success, garage_ensure_map / lockCar run below; exit calls garage_exit.
  // Valocity.enter: TREE-first (City/Track super + GroundRef leaves).
  // On success, valocity_finalize_enter normalizes traffic/triggers below.
  if (name && std::strcmp(name, "getVehicleDescriptor") == 0) {
    if (is_static && class_fqn &&
        std::strcmp(class_fqn, "java.game.GameLogic") == 0) {
      int32_t set = 0;
      float param = -1.f;
      if (!args.empty() && args[0].tag == JvmTag::Int) set = args[0].v.i;
      if (args.size() >= 2) {
        if (args[1].tag == JvmTag::Float) param = args[1].v.f;
        else if (args[1].tag == JvmTag::Int)
          param = static_cast<float>(args[1].v.i);
      }
      return JvmValue::make_obj(game_logic_get_vehicle_descriptor(set, param));
    }
    if (!is_static && !args.empty() && args[0].tag == JvmTag::Obj) {
      int32_t set = 0;
      float param = -1.f;
      if (args.size() >= 2 && args[1].tag == JvmTag::Int) set = args[1].v.i;
      if (args.size() >= 3) {
        if (args[2].tag == JvmTag::Float) param = args[2].v.f;
        else if (args[2].tag == JvmTag::Int)
          param = static_cast<float>(args[2].v.i);
      }
      return JvmValue::make_obj(
          vehicle_type_get_vehicle_descriptor(args[0].v.o, set, param));
    }
  }

  if (!find_class(class_fqn)) {
    if (!load_class(class_fqn)) {
      std::fprintf(stderr, "[jvm] no class %s\n", class_fqn);
      return JvmValue::make_void();
    }
  }
  const JvmClass* cls = find_class(class_fqn);
  if (!cls) return JvmValue::make_void();

  // Walk supers for inherited TREE methods (e.g. VehicleType.init on *_VT).
  const JvmClass* owner = cls;
  const JvmMethod* m = find_method(*cls, name, signature);
  auto method_has_tree = [](const JvmClass& c, const JvmMethod* meth) -> bool {
    if (!meth || meth->tree_index < 0) return false;
    if (static_cast<size_t>(meth->tree_index) >= c.trees.size()) return false;
    return !c.trees[static_cast<size_t>(meth->tree_index)].nodes.empty();
  };
  // City: stub/native <init> MTHD + FILD arrays — skip empty body, use Scene.
  if (m && name && std::strcmp(name, "<init>") == 0 && !method_has_tree(*cls, m) &&
      !cls->field_inits.empty()) {
    m = nullptr;
  }
  if (!m || !method_has_tree(*owner, m)) {
    const JvmMethod* leaf = m;
    const JvmClass* leaf_owner = owner;
    if (m && !method_has_tree(*owner, m)) m = nullptr;
    std::string super = cls->super_name;
    for (int depth = 0; !m && !super.empty() && depth < 16; ++depth) {
      if (!find_class(super.c_str())) load_class(super.c_str());
      owner = find_class(super.c_str());
      if (!owner) break;
      m = find_method(*owner, name, signature);
      if (m && !method_has_tree(*owner, m)) m = nullptr;
      if (m) break;
      super = owner->super_name;
    }
    if (!m) {
      m = leaf;
      owner = leaf_owner;
    }
  }
  if (!m) {
    // Native-only path (no MTHD entry match) — still try table.
    const NativeEntry* ne = resolve_native(class_fqn, name, signature);
    if (ne && ne->fn) {
      CallFrame frame;
      frame.class_fqn = class_fqn;
      frame.method_name = name;
      frame.jni_signature = signature;
      frame.is_static = is_static;
      frame.args = args;
      std::string err;
      if (!call_native(ne, &frame, &err)) {
        std::fprintf(stderr, "[jvm] call_native failed: %s\n", err.c_str());
        return JvmValue::make_void();
      }
      return frame.result;
    }
    std::fprintf(stderr, "[jvm] no method %s.%s%s\n", class_fqn, name, signature);
    return JvmValue::make_void();
  }

  const bool has_tree =
      m->tree_index >= 0 &&
      static_cast<size_t>(m->tree_index) < owner->trees.size() &&
      !owner->trees[static_cast<size_t>(m->tree_index)].nodes.empty();

  if (has_tree) {
    JvmTreeHost host(this);
    auto apply_field_inits = [&](const JvmClass& c, InvObject* self) {
      if (!self || c.field_inits.empty()) return;
      for (const JvmFieldInit& fi : c.field_inits) {
        if (fi.tree_index < 0 ||
            static_cast<size_t>(fi.tree_index) >= c.trees.size())
          continue;
        // Skip heavy FILD trees (SfxRef / bots). City.posGarage NEWARRAY packs
        // to ~40+ nodes; allow up to 96 for array/scalar field inits.
        if (c.trees[static_cast<size_t>(fi.tree_index)].nodes.size() > 96)
          continue;
        JvmMethod fim;
        fim.name = "<fieldinit>";
        fim.signature = "()V";
        fim.tree_index = fi.tree_index;
        std::string ferr;
        JvmValue fv =
            tree_eval(&host, c, fim, {JvmValue::make_obj(self)}, &ferr);
        if (!ferr.empty()) continue;
        if (fv.tag == JvmTag::Obj)
          tree_field_set_obj(self, fi.name.c_str(), fv.v.o);
        else if (fv.tag == JvmTag::Int)
          tree_field_set_int(self, fi.name.c_str(), fv.v.i);
        else if (fv.tag == JvmTag::Float)
          tree_field_set_float(self, fi.name.c_str(), fv.v.f);
      }
    };
    std::vector<JvmValue> locals = args;
    // Invictus static TREE locals are argN..arg0 (reversed). Instance keeps this
    // at [0]; <init> args after this are also reversed (Vector elementData size).
    if (is_static && locals.size() > 1) {
      std::reverse(locals.begin(), locals.end());
    } else if (!is_static && name && std::strcmp(name, "<init>") == 0 &&
               locals.size() > 2) {
      std::reverse(locals.begin() + 1, locals.end());
    }
    // Phase 2.115: FILD before <init> body. Java runs superclass field
    // initializers via super(); City ships stub/native <init> MTHD rows so
    // Valocity.super() often never allocates posGarage — apply the chain.
    if (!is_static && name && std::strcmp(name, "<init>") == 0 &&
        !args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
      std::vector<const JvmClass*> chain;
      for (const JvmClass* c = cls; c;) {
        chain.push_back(c);
        if (c->super_name.empty()) break;
        if (!find_class(c->super_name.c_str()))
          load_class(c->super_name.c_str());
        c = find_class(c->super_name.c_str());
      }
      for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        apply_field_inits(**it, args[0].v.o);
    }
    std::string err;
    JvmValue r = tree_eval(&host, *owner, *m, std::move(locals), &err);
    if (err.empty()) {
      // Synthetic subclass <init> (City has FILD but no MTHD <init>): after
      // inherited Track.<init>, run the requested class field inits.
      if (!is_static && name && std::strcmp(name, "<init>") == 0 &&
          owner != cls && !args.empty() && args[0].tag == JvmTag::Obj &&
          args[0].v.o) {
        apply_field_inits(*cls, args[0].v.o);
      }
      if (!args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
        InvObject* self = args[0].v.o;
        if (std::strcmp(name, "enter") == 0) {
          tree_field_set_int(self, "enter_via_tree", 1);
          tree_field_set_int(self, "entered", 1);
          if (std::strstr(class_fqn, "Garage")) {
            garage_ensure_map(self);
            garage_ensure_camera(self);
            garage_lock_car(self);
            game_logic_set_played(1);
            InvObject* osd = tree_field_get_obj(self, "osd");
            const int32_t btn =
                osd ? tree_field_get_int(osd, "button_count") : 0;
            // Mechanic/painter inflate button_count; require street strip
            // (CMD_HITTHESTREET=109) before trusting TREE OSD.
            bool street = false;
            if (InvObject* btns =
                    osd ? tree_field_get_obj(osd, "buttons") : nullptr) {
              const int32_t n = tree_vector_size(btns);
              for (int32_t i = 0; i < n; ++i) {
                InvObject* b = tree_vector_element_at(btns, i);
                if (b && tree_field_get_int(b, "command") == 109) {
                  street = true;
                  break;
                }
              }
            }
            if (btn >= 8 && street) {
              tree_field_set_int(self, "osd_via_tree", 1);
              tree_field_set_int(self, "osd_via_host", 0);
              if (osd) tree_field_set_int(osd, "visible", 1);
            } else {
              garage_try_create_osd_objects(self);
            }
          }
          if (std::strstr(class_fqn, "RaceSetup")) {
            racesetup_try_create_osd_objects(self);
          }
          if (std::strstr(class_fqn, "Valocity")) {
            InvObject* prev =
                (args.size() >= 2 && args[1].tag == JvmTag::Obj) ? args[1].v.o
                                                                 : nullptr;
            const char* pc = prev ? tree_host_class(prev) : nullptr;
            const bool from_rs = pc && std::strstr(pc, "RaceSetup");
            // Stock Valocity.enter(RaceSetup): unpause only — do not reseat
            // traffic / raceState=0 (would bounce QUICKRACE back to parent).
            if (!from_rs) valocity_finalize_enter(self);
            // Stock Valocity.enter QUICKRACE: always CAS(GameLogic.racesetup)
            // when prev is not RaceSetup. TREE often packs that CAS as MainMenu
            // (polluted racesetup / wrong stack) — recover here.
            if (game_logic_game_mode() == 3) {
              InvObject* st = game_logic_actual_state();
              const char* sc = st ? tree_host_class(st) : nullptr;
              const bool on_rs = sc && std::strstr(sc, "RaceSetup");
              if (!from_rs && !on_rs) {
                InvObject* player = game_logic_player();
                tree_field_set_obj(self, "challenger", player);
                tree_field_set_int(self, "raceState", 1);
                InvObject* rs = game_logic_racesetup();
                if (!rs) {
                  rs = tree_host_new("java.game.RaceSetup");
                  game_logic_set_racesetup(rs);
                }
                tree_field_set_obj(rs, "lastState", self);
                tree_field_set_obj(rs, "track", self);
                game_logic_change_active_section(rs);
                std::printf(
                    "[script] Valocity QUICKRACE → RaceSetup (stock handoff)\n");
              }
            }
          }
          frontend_loading_screen_hide();
          std::printf("[script] %s.enter via TREE\n", class_fqn);
        } else if (std::strcmp(name, "exit") == 0) {
          tree_field_set_int(self, "exit_via_tree", 1);
          tree_field_set_int(self, "entered", 0);
          if (std::strstr(class_fqn, "Valocity")) {
            valocity_exit(self, nullptr);
          }
          if (std::strstr(class_fqn, "Garage")) {
            InvObject* next =
                (args.size() >= 2 && args[1].tag == JvmTag::Obj) ? args[1].v.o
                                                                 : nullptr;
            garage_exit(self, next);
          }
          std::printf("[script] %s.exit via TREE\n", class_fqn);
        } else if (std::strcmp(name, "osdCommand") == 0) {
          tree_field_set_int(self, "osd_cmd_via_tree", 1);
          if (args.size() >= 2 && args[1].tag == JvmTag::Int)
            tree_field_set_int(self, "last_osd_cmd", args[1].v.i);
          std::printf("[script] %s.osdCommand via TREE\n", class_fqn);
        } else if (std::strcmp(name, "startRace") == 0) {
          tree_field_set_int(self, "start_race_via_tree", 1);
          std::printf("[script] %s.startRace via TREE\n", class_fqn);
        } else if (std::strcmp(name, "handleEvent") == 0) {
          tree_field_set_int(self, "handle_event_via_tree", 1);
          std::printf("[script] %s.handleEvent via TREE\n", class_fqn);
        } else if (std::strcmp(name, "show") == 0) {
          tree_field_set_int(self, "show_via_tree", 1);
          tree_field_set_int(self, "shown", 1);
          if (InvObject* osd = tree_field_get_obj(self, "osd"))
            tree_field_set_int(osd, "visible", 1);
          std::printf("[script] %s.show via TREE\n", class_fqn);
        } else if (std::strcmp(name, "hide") == 0) {
          tree_field_set_int(self, "hide_via_tree", 1);
          tree_field_set_int(self, "shown", 0);
          if (InvObject* osd = tree_field_get_obj(self, "osd"))
            tree_field_set_int(osd, "visible", 0);
          tree_field_set_obj(self, "backShieldOsd", nullptr);
          std::printf("[script] %s.hide via TREE\n", class_fqn);
        }
      }
      return r;
    }
    std::fprintf(stderr, "[jvm] TREE eval %s.%s: %s\n", owner->name.c_str(),
                 name, err.c_str());
    return r;
  }

  const NativeEntry* ne = resolve_native(class_fqn, name, signature);
  if (ne && ne->fn) {
    CallFrame frame;
    frame.class_fqn = class_fqn;
    frame.method_name = name;
    frame.jni_signature = signature;
    frame.is_static = is_static;
    frame.args = args;
    std::string err;
    if (!call_native(ne, &frame, &err)) {
      std::fprintf(stderr, "[jvm] call_native failed: %s\n", err.c_str());
      return JvmValue::make_void();
    }
    return frame.result;
  }

  std::fprintf(stderr, "[jvm] no native binding %s.%s\n", class_fqn, name);
  return JvmValue::make_void();
}

namespace {
Jvm* g_active_jvm = nullptr;
}

void jvm_set_active(Jvm* j) { g_active_jvm = j; }
Jvm* jvm_active() { return g_active_jvm; }

}  // namespace inv
