// Fragment 1/2 of jvm.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


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

    // java.lang.Object.<init>()V — empty Java ctor. Not a PE UnboxArg
    // native; TREE super() chains hit it heavily (Valocity traffic/GameRef).
    if (method && std::strcmp(method, "<init>") == 0 && class_fqn &&
        std::strcmp(class_fqn, "java.lang.Object") == 0)
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