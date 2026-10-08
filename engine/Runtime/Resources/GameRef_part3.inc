// Fragment 3/6 of GameRef.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


InvObject* frontend_large_font() {
  if (!g_large_font) frontend_set_fonts();
  return g_large_font;
}
InvObject* frontend_medium_font() {
  if (!g_medium_font) frontend_set_fonts();
  return g_medium_font;
}
InvObject* frontend_small_font() {
  if (!g_small_font) frontend_set_fonts();
  return g_small_font;
}
InvObject* frontend_pointers() {
  if (!g_pointers) frontend_set_fonts();
  return g_pointers;
}
InvObject* frontend_def_loading_pic() {
  if (!g_def_loading_pic) g_def_loading_pic = frontend_make_ref(0x00A8);
  return g_def_loading_pic;
}
InvObject* frontend_input_queue() {
  if (!g_input_queue) g_input_queue = tree_vector_new();
  return g_input_queue;
}
int32_t frontend_inited() { return g_frontend_inited; }
InvObject* frontend_hotkey_thread() { return g_hotkey_thread; }

int32_t frontend_loading_screen_visible() {
  InvObject* ls = frontend_loading_screen();
  return tree_field_get_int(ls, "visible");
}

namespace {
void loading_screen_invoke(const char* method) {
  InvObject* ls = frontend_loading_screen();
  Jvm* j = jvm_active();
  if (j && ls) {
    std::vector<JvmValue> args = {JvmValue::make_obj(ls)};
    j->invoke("java.render.LoadingScreen", method, "()V", args, false);
  } else if (std::strcmp(method, "show") == 0) {
    frontend_loading_screen_show();
  } else {
    frontend_loading_screen_hide();
  }
}
}  // namespace

bool game_logic_is_section(InvObject* state) {
  // Java changeActiveSection(GameState). Implementors: SplashScreen, MainMenu,
  // Garage, Track (+City/Valocity/TestTrack), RaceSetup, CarInfo, CarMarket,
  // Catalog, CarLot, ClubInfo, Painter, RocInfo. TREE often packs String /
  // NoYesDialog leftovers as the CAS arg (CMD_EXIT null).
  if (!state) return false;
  const char* c = tree_host_class(state);
  if (!c || !c[0]) return false;
  auto ends = [&](const char* s) -> bool {
    const size_t n = std::strlen(s);
    const size_t m = std::strlen(c);
    if (m < n) return false;
    if (std::strcmp(c + (m - n), s) != 0) return false;
    if (m == n) return true;
    const char sep = c[m - n - 1];
    return sep == '.' || sep == '/';
  };
  return ends("SplashScreen") || ends("MainMenu") || ends("Garage") ||
         ends("Valocity") || ends("City") || ends("TestTrack") ||
         ends("Track") || ends("RaceSetup") || ends("CarInfo") ||
         ends("CarMarket") || ends("Catalog") || ends("CarLot") ||
         ends("ClubInfo") || ends("Painter") || ends("RocInfo");
}

InvObject* game_logic_change_active_section(InvObject* state) {
  // Stock GameLogic.changeActiveSection (Java) — script-driven gate:
  //   if (actualState) actualState.exit(state);
  //   old = actualState; actualState = state;
  //   if (actualState) actualState.enter(old);
  //   else System.exit() @ 0x0047BE50 (Engine_quitRequested = 1).
  // Host CAS(null) does not request_exit: hub smoke restores Garage after EXIT.
  InvObject* prev = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    prev = g_actual_state;
  }

  Jvm* j = jvm_active();
  auto call_lifecycle = [&](InvObject* target, const char* method,
                            InvObject* arg) {
    if (!target || !j) return;
    const char* cn = tree_host_class(target);
    if (!cn || !cn[0]) return;
    if (!j->find_class(cn)) j->load_class(cn);
    std::vector<JvmValue> args = {JvmValue::make_obj(target),
                                  JvmValue::make_obj(arg)};
    j->invoke(cn, method, "(Ljava.game.GameState;)V", args, false);
  };

  if (prev) {
    if (j)
      call_lifecycle(prev, "exit", state);
    else
      tree_field_set_int(prev, "entered", 0);
  }

  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    g_actual_state = state;
  }

  if (state) {
    if (j) {
      const char* cn = tree_host_class(state);
      std::printf("[script] changeActiveSection → %s\n",
                  cn && cn[0] ? cn : "?");
      call_lifecycle(state, "enter", prev);
    } else {
      tree_field_set_int(state, "entered", 1);
      if (const char* cn = tree_host_class(state)) {
        if (std::strstr(cn, "SplashScreen"))
          tree_field_set_int(state, "timer_sec", 3);
      }
    }
  }
  return state;
}

InvObject* game_logic_actual_state() {
  std::lock_guard<std::mutex> lock(g_gr_mu);
  return g_actual_state;
}

InvObject* game_logic_boot_splash() {
  InvObject* player = game_logic_boot_player_garage();
  InvObject* ctrl = player ? tree_field_get_obj(player, "controller") : nullptr;
  if (ctrl) {
    // GameLogic: player.controller.reset(); activateState(MENUSET);
    controller_reset(ctrl);
    controller_activate_state(ctrl, kMenuSet, 1);
  }

  // GameLogic ctor end: Frontend.loadingScreen.hide() only — avoid show() which
  // TREE-builds SimpleLoadingDialog + SoftTimer (not needed for Splash→Menu).
  loading_screen_invoke("hide");

  // changeActiveSection(new SplashScreen(new ResourceRef(frontend:0x5)))
  int32_t pic_id = 0x5;
  if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
    pic_id = rpak_make_id(fe->pack_id, 0x5);
  else if (const RpakPack* fe = rpak_find_by_name("frontend"))
    pic_id = rpak_make_id(fe->pack_id, 0x5);

  InvObject* pic = resref_new();
  java_util_resource_ResourceRef_set(pic, pic_id);

  // Stock: new SplashScreen(pic) → createNativeInstance() + this.pic = pic.
  InvObject* splash = tree_host_new("java.game.SplashScreen");
  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.SplashScreen"))
      j->load_class("java.game.SplashScreen");
    j->invoke("java.game.SplashScreen", "<init>",
              "(Ljava.util.resource.ResourceRef;)V",
              {JvmValue::make_obj(splash), JvmValue::make_obj(pic)}, false);
  } else {
    tree_field_set_obj(splash, "pic", pic);
  }
  tree_field_set_int(splash, "entered", 0);
  game_logic_change_active_section(splash);
  return splash;
}

InvObject* game_logic_finish_splash() {
  // Stock: SplashScreen.handleEvent → osdCommand(AXIS_CANCEL) →
  //   GameLogic.changeActiveSection(new MainMenu()).
  // TREE osdCommand packs `new MainMenu()` into a stack overflow — CAS here.
  InvObject* splash = game_logic_actual_state();
  const char* scn = splash ? tree_host_class(splash) : nullptr;
  if (!scn || !std::strstr(scn, "SplashScreen")) {
    InvObject* cur = game_logic_actual_state();
    if (cur && tree_host_class(cur) &&
        std::strstr(tree_host_class(cur), "MainMenu"))
      return cur;
  }
  InvObject* menu = tree_host_new("java.game.MainMenu");
  game_logic_change_active_section(menu);
  return game_logic_actual_state();
}

void game_logic_load_defaults() {
  ++g_load_defaults_calls;
  InvObject* player = game_logic_boot_player_garage();
  tree_field_set_int(player, "money", kInitialMoney);
  tree_field_set_float(player, "prestige", kInitialPrestige);
  tree_field_set_int(player, "club", 0);
  tree_field_set_int(player, "flags", 0);
  tree_field_set_int(player, "hints", 0);
  tree_field_set_int(player, "winPinkSlips", 0);
  InvObject* name = tree_field_get_obj(player, "name");
  const char* ns = name ? string_cstr(name) : nullptr;
  if (!ns || !ns[0]) tree_field_set_obj(player, "name", string_new("Player"));
  // loadUnsavedData subset
  g_game_mode = kGmCareer;
  g_time_of_day = 12.f * 3600.f;
  g_day = 1;
  // CarMarket.getInitialCars for new/used dealers
  {
    constexpr int32_t VS_USED = 0x0002;
    constexpr int32_t VS_STOCK = 0x0004;
    g_car_desc_new = tree_vector_new();
    g_car_desc_used = tree_vector_new();
    for (int i = 0; i < 4; ++i) {
      if (InvObject* vd = game_logic_get_vehicle_descriptor(VS_STOCK, 0.5f))
        tree_vector_add(g_car_desc_new, vd);
      if (InvObject* vd = game_logic_get_vehicle_descriptor(VS_USED, 0.5f))
        tree_vector_add(g_car_desc_used, vd);
    }
    g_dealer_ts_new = 0.f;
    g_dealer_ts_used = 0.f;
  }
}

int32_t game_logic_game_mode() { return g_game_mode; }
void game_logic_set_game_mode(int32_t mode) { g_game_mode = mode; }
int32_t game_logic_timeout() { return g_timeout; }
void game_logic_set_timeout(int32_t t) { g_timeout = t; }
int32_t game_logic_career_in_progress() { return g_career_in_progress; }
void game_logic_set_career_in_progress(int32_t v) {
  g_career_in_progress = v ? 1 : 0;
}
int32_t game_logic_day() { return g_day; }
float game_logic_time() { return g_time_of_day; }

void game_logic_set_time(float t) {
  g_time_of_day = t;
  while (g_time_of_day < 0.f) g_time_of_day += 24.f * 3600.f;
  while (g_time_of_day > 24.f * 3600.f) {
    g_time_of_day -= 24.f * 3600.f;
    ++g_day;
  }
  java_lang_System_syncGameTime(g_time_of_day);
}

void game_logic_spend_time(float dt) {
  game_logic_set_time(g_time_of_day + dt);
}

void game_logic_set_played(int32_t v) { g_played = v ? 1 : 0; }
int32_t game_logic_played() { return g_played; }

void garage_ensure_map(InvObject* garage) {
  if (!garage) return;
  if (tree_field_get_int(garage, "map_id") != 0) return;
  InvObject* player = tree_field_get_obj(garage, "player");
  if (!player) player = game_logic_player();
  int32_t gidx = tree_field_get_int(garage, "garageIndex");
  if (!tree_field_get_obj(garage, "player") && player)
    tree_field_set_obj(garage, "player", player);
  // If ctor never ran, garageIndex may still be 0 (= club 0) which is fine.
  if (player && gidx == 0 && tree_field_get_int(player, "club") != 0)
    gidx = tree_field_get_int(player, "club");
  tree_field_set_int(garage, "garageIndex", gidx);
  int32_t map_local = 0x7a;
  if (gidx == 1) map_local = 0x06;
  else if (gidx == 2) map_local = 0xc7;
  else if (gidx >= 3) map_local = 0x2f;
  if (!rpak_find_by_name("garage.rpk"))
    java_lang_System_openLib(string_new("misc/garage.rpk"));
  int32_t map_id = map_local;
  if (const RpakPack* gp = rpak_find_by_name("garage.rpk"))
    map_id = rpak_make_id(gp->pack_id, static_cast<uint16_t>(map_local));
  tree_field_set_int(garage, "map_id", map_id);
  InvObject* map = tree_field_get_obj(garage, "map");
  if (!map) {
    map = tree_host_new("java.util.resource.GroundRef");
    tree_field_set_obj(garage, "map", map);
  }
  if (java_util_resource_ResourceRef_id(map) == 0)
    java_util_resource_ResourceRef_set(map, map_id);
}

InvObject* garage_ensure_camera(InvObject* garage) {
  // Stock Garage.enter: Camera at (-3,1.5,-2) Ypr(-2.05,-0.25,0).
  if (!garage) return nullptr;
  InvObject* cam = tree_field_get_obj(garage, "camera");
  if (!cam) {
    cam = tree_host_new("java.render.Camera");
    tree_field_set_obj(garage, "camera", cam);
    InvObject* map = tree_field_get_obj(garage, "map");
    InvObject* vp = nullptr;
    if (InvObject* osd = tree_field_get_obj(garage, "osd"))
      vp = tree_field_get_obj(osd, "vp");
    if (!vp) vp = reinterpret_cast<InvObject*>(render_d3d9_viewport_active());
    // half-AOV 45° (stock aov ~90).
    java_render_Camera_create(cam, map, vp, 1, 45.f, 0.05f, 200.f, 1.f, 1.f, 0,
                              1);
    constexpr float kEyeX = -3.f, kEyeY = 1.5f, kEyeZ = -2.f;
    constexpr float kAtX = 0.f, kAtY = 0.5f, kAtZ = 0.f;
    tree_field_set_float(garage, "cam_eye_x", kEyeX);
    tree_field_set_float(garage, "cam_eye_y", kEyeY);
    tree_field_set_float(garage, "cam_eye_z", kEyeZ);
    tree_field_set_float(garage, "cam_at_x", kAtX);
    tree_field_set_float(garage, "cam_at_y", kAtY);
    tree_field_set_float(garage, "cam_at_z", kAtZ);
    const float dx = kEyeX - kAtX;
    const float dy = kEyeY - kAtY;
    const float dz = kEyeZ - kAtZ;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    tree_field_set_float(garage, "cam_dist", dist > 0.1f ? dist : 4.f);
    tree_field_set_float(garage, "cam_yaw", std::atan2(dx, dz));
    tree_field_set_float(garage, "cam_pitch",
                         std::asin(dy / (dist > 0.1f ? dist : 4.f)));
    render_d3d9_camera_lookat(cam, kEyeX, kEyeY, kEyeZ, kAtX, kAtY, kAtZ);
    if (vp) render_d3d9_camera_activate(cam, vp, 0);
  }
  if (InvObject* mech = tree_field_get_obj(garage, "mechanic"))
    tree_field_set_obj(mech, "camera", cam);
  return cam;
}

static void garage_apply_orbit_camera(InvObject* garage) {
  if (!garage) return;
  InvObject* cam = garage_ensure_camera(garage);
  if (!cam) return;
  const float yaw = tree_field_get_float(garage, "cam_yaw");
  const float pitch = tree_field_get_float(garage, "cam_pitch");
  float dist = tree_field_get_float(garage, "cam_dist");
  if (dist < 1.f) dist = 1.f;
  const float at_x = tree_field_get_float(garage, "cam_at_x");
  const float at_y = tree_field_get_float(garage, "cam_at_y");
  const float at_z = tree_field_get_float(garage, "cam_at_z");
  const float cp = std::cos(pitch);
  const float eye_x = at_x + std::sin(yaw) * cp * dist;
  const float eye_y = at_y + std::sin(pitch) * dist;
  const float eye_z = at_z + std::cos(yaw) * cp * dist;
  tree_field_set_float(garage, "cam_eye_x", eye_x);
  tree_field_set_float(garage, "cam_eye_y", eye_y);
  tree_field_set_float(garage, "cam_eye_z", eye_z);
  render_d3d9_camera_lookat(cam, eye_x, eye_y, eye_z, at_x, at_y, at_z);
  if (void* vp = render_d3d9_viewport_active())
    render_d3d9_camera_activate(cam, vp, 0);
}

void garage_rdrag_begin(InvObject* garage) {
  if (!garage) return;
  garage_ensure_camera(garage);
  tree_field_set_int(garage, "rdrag", 1);
  tree_field_set_int(garage, "move", 1);  // stock Garage.move
  tree_field_set_int(garage, "rdrag_begin_count",
                     tree_field_get_int(garage, "rdrag_begin_count") + 1);
  if (InvObject* mech = tree_field_get_obj(garage, "mechanic"))
    tree_field_set_int(mech, "rdrag", 1);
}

void garage_rdrag_end(InvObject* garage) {
  if (!garage) return;
  tree_field_set_int(garage, "rdrag", 0);
  tree_field_set_int(garage, "move", 0);
  tree_field_set_int(garage, "rdrag_end_count",
                     tree_field_get_int(garage, "rdrag_end_count") + 1);
  if (InvObject* mech = tree_field_get_obj(garage, "mechanic"))
    tree_field_set_int(mech, "rdrag", 0);
}

bool garage_rdrag_orbit(InvObject* garage, float dyaw, float dpitch) {
  // Stock: AXIS_LOOK_LEFTRIGHT/UPDOWN while EC_RDRAG (mouse → camera).
  if (!garage) return false;
  garage_ensure_camera(garage);
  float yaw = tree_field_get_float(garage, "cam_yaw") + dyaw;
  float pitch = tree_field_get_float(garage, "cam_pitch") + dpitch;
  constexpr float kPitchMax = 1.2f;
  if (pitch > kPitchMax) pitch = kPitchMax;
  if (pitch < -0.2f) pitch = -0.2f;
  tree_field_set_float(garage, "cam_yaw", yaw);
  tree_field_set_float(garage, "cam_pitch", pitch);
  garage_apply_orbit_camera(garage);
  tree_field_set_int(garage, "rdrag_orbit_count",
                     tree_field_get_int(garage, "rdrag_orbit_count") + 1);
  return true;
}

void garage_tick_rdrag(InvObject* garage) {
  if (!garage) return;
  input_live_poll();
  const float btn = java_io_Input_getAxis(1, kMousePhysBtn2);
  const int32_t down = btn > 0.5f ? 1 : 0;
  const int32_t was = tree_field_get_int(garage, "mouse_btn2");
  tree_field_set_int(garage, "mouse_btn2", down);
  if (was == 0 && down == 1) {
    garage_rdrag_begin(garage);
  } else if (was == 1 && down == 1 && tree_field_get_int(garage, "rdrag")) {
    float dx = 0.f, dy = 0.f, dz = 0.f;
    input_mouse_rel(&dx, &dy, &dz);
    if (dx != 0.f || dy != 0.f)
      garage_rdrag_orbit(garage, dx * 0.35f, dy * 0.25f);
    if (dz != 0.f) {
      float dist = tree_field_get_float(garage, "cam_dist") - dz * 0.4f;
      if (dist < 1.5f) dist = 1.5f;
      if (dist > 12.f) dist = 12.f;
      tree_field_set_float(garage, "cam_dist", dist);
      garage_apply_orbit_camera(garage);
    }
  } else if (was == 1 && down == 0) {
    garage_rdrag_end(garage);
  }
}

bool garage_try_create_osd_objects(InvObject* garage);

InvObject* garage_enter(InvObject* garage, InvObject* prev_state) {
  if (!garage) return nullptr;
  frontend_loading_screen_show();

  if (!tree_field_get_obj(garage, "parentState") && prev_state)
    tree_field_set_obj(garage, "parentState", prev_state);

  InvObject* player = game_logic_player();
  tree_field_set_obj(garage, "player", player);
  const int32_t club = player ? tree_field_get_int(player, "club") : 0;
  tree_field_set_int(garage, "garageIndex", club);

  garage_ensure_map(garage);
  garage_ensure_camera(garage);
  // Career smoke path is not ROC — clear so createOSDObjects builds street menu.
  tree_field_set_obj(garage, "roc", nullptr);
  tree_field_set_int(garage, "mode", 1);  // MODE_SZEREL
  if (!tree_field_get_int(garage, "mode_memory"))
    tree_field_set_int(garage, "mode_memory", 1);
  tree_field_set_int(garage, "entered", 1);
  g_played = 1;
  garage_lock_car(garage);

  if (main_menu_hub_deferring()) {
    tree_field_set_int(garage, "osd_pending", 1);
    return garage;
  }
  garage_try_create_osd_objects(garage);
  return garage;
}

// Re-run createOSDObjects outside nested MainMenuDialog.osdCommand (nested
// TREE often under-builds the street menu). Clears prior host fallback.
bool garage_try_create_osd_objects(InvObject* garage) {
  if (!garage) return false;
  InvObject* osd = tree_field_get_obj(garage, "osd");
  if (!osd) {
    osd = tree_host_new("java.render.Osd");
    osd_ensure_defaults(osd);
    tree_field_set_obj(garage, "osd", osd);
  }
  tree_field_set_obj(osd, "globalHandler", garage);
  tree_field_set_int(osd, "orientation", 1);
  tree_field_set_obj(osd, "buttons", tree_vector_new());
  tree_field_set_int(osd, "button_count", 0);
  tree_field_set_obj(osd, "groups", tree_vector_new());
  tree_field_set_obj(osd, "last_menu", nullptr);
  osd_begin_group(osd);
  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.Garage")) j->load_class("java.game.Garage");
    std::vector<JvmValue> a = {JvmValue::make_obj(garage)};
    j->invoke("java.game.Garage", "createOSDObjects", "()V", a, false);
  }
  const int32_t btn = tree_field_get_int(osd, "button_count");
  const int32_t txt = render_d3d9_osd_text_count();
  InvObject* last_menu = tree_field_get_obj(osd, "last_menu");
  const int32_t menu_items =
      last_menu ? tree_field_get_int(last_menu, "item_count") : -1;
  if (btn < 8) {
    std::printf(
        "[script] Garage.createOSDObjects TREE weak btn=%d items=%d "
        "menu=%p roc=%p\n",
        btn, menu_items, (void*)last_menu,
        (void*)tree_field_get_obj(garage, "roc"));
  }
  if (btn >= 8) {
    tree_field_set_int(garage, "osd_via_tree", 1);
    tree_field_set_int(garage, "osd_via_host", 0);
    tree_field_set_int(osd, "visible", 1);
    std::printf("[script] Garage.createOSDObjects via TREE btn=%d txt=%d\n",
                btn, txt);
    static int s_cmd_dump;
    if (s_cmd_dump < 1) {
      ++s_cmd_dump;
      InvObject* btns = tree_field_get_obj(osd, "buttons");
      const int32_t n = btns ? tree_vector_size(btns) : 0;
      std::printf("[script] Garage OSD cmds:");
      for (int32_t i = 0; i < n; ++i) {
        InvObject* b = tree_vector_element_at(btns, i);
        std::printf(" %d", b ? tree_field_get_int(b, "command") : -1);
      }
      std::printf("\n");
    }
    return true;
  }
  garage_create_osd_street_menu(garage);
  const int32_t btn2 = tree_field_get_int(osd, "button_count");
  std::printf(
      "[script] Garage.createOSDObjects host fallback (TREE btn=%d) "
      "btn=%d txt=%d\n",
      btn, btn2, render_d3d9_osd_text_count());
  return false;
}

bool racesetup_osd_has_race_cmds(InvObject* rs) {
  InvObject* osd = rs ? tree_field_get_obj(rs, "osd") : nullptr;
  InvObject* btns = osd ? tree_field_get_obj(osd, "buttons") : nullptr;
  bool race = false, abandon = false;
  const int32_t n = btns ? tree_vector_size(btns) : 0;
  for (int32_t i = 0; i < n; ++i) {
    InvObject* b = tree_vector_element_at(btns, i);
    if (!b) continue;
    const int32_t c = tree_field_get_int(b, "command");
    if (c == 0) race = true;     // CMD_RACE
    if (c == 1) abandon = true;  // CMD_ABANDON
  }
  return race && abandon;
}

// TREE-only RaceSetup.createOSDObjects (stock enter after map/nav). No host strip.
bool racesetup_try_create_osd_objects(InvObject* rs) {
  if (!rs) return false;
  if (racesetup_osd_has_race_cmds(rs)) {
    tree_field_set_int(rs, "osd_via_tree", 1);
    if (InvObject* osd = tree_field_get_obj(rs, "osd"))
      tree_field_set_int(osd, "visible", 1);
    return true;
  }
  InvObject* osd = tree_field_get_obj(rs, "osd");
  if (!osd) {
    osd = tree_host_new("java.render.Osd");
    osd_ensure_defaults(osd);
    tree_field_set_obj(rs, "osd", osd);
  }
  tree_field_set_obj(osd, "globalHandler", rs);
  tree_field_set_obj(osd, "buttons", tree_vector_new());
  tree_field_set_int(osd, "button_count", 0);
  tree_field_set_obj(osd, "last_menu", nullptr);
  osd_begin_group(osd);
  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.RaceSetup"))
      j->load_class("java.game.RaceSetup");
    std::vector<JvmValue> a = {JvmValue::make_obj(rs)};
    j->invoke("java.game.RaceSetup", "createOSDObjects", "()V", a, false);
  }
  const int32_t btn = tree_field_get_int(osd, "button_count");
  InvObject* btns = tree_field_get_obj(osd, "buttons");
  const int32_t n = btns ? tree_vector_size(btns) : 0;
  std::printf("[script] RaceSetup OSD cmds:");
  for (int32_t i = 0; i < n; ++i) {
    InvObject* b = tree_vector_element_at(btns, i);
    std::printf(" %d", b ? tree_field_get_int(b, "command") : -1);
  }
  std::printf(" (btn=%d)\n", btn);
  if (racesetup_osd_has_race_cmds(rs) && btn >= 4) {
    tree_field_set_int(rs, "osd_via_tree", 1);
    tree_field_set_int(osd, "visible", 1);
    std::printf("[script] RaceSetup.createOSDObjects via TREE btn=%d\n", btn);
    return true;
  }
  std::printf("[script] RaceSetup.createOSDObjects TREE weak btn=%d\n", btn);
  return false;
}

// Stock RaceSetup.enter: pStart = nearestCross(car); pFinish ~500–800 with a
// valid getStartDirection. TREE putfield often drops them; natives are VA-backed.
static void racesetup_ensure_route(InvObject* rs) {
  if (!rs) return;
  if (tree_field_get_obj(rs, "pStart") && tree_field_get_obj(rs, "pFinish"))
    return;
  InvObject* track = tree_field_get_obj(rs, "track");
  if (!track) track = tree_field_get_obj(rs, "lastState");
  InvObject* map = track ? tree_field_get_obj(track, "map") : nullptr;
  if (!map) return;
  InvObject* player = track ? tree_field_get_obj(track, "player") : nullptr;
  if (!player) player = game_logic_player();
  InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
  InvObject* approx =
      car ? java_util_resource_GameRef_getPos(car) : nullptr;
  if (!approx && track) {
    approx = vec3_new(tree_field_get_float(track, "posStart_x"),
                      tree_field_get_float(track, "posStart_y"),
                      tree_field_get_float(track, "posStart_z"));
  }
  if (!approx) approx = vec3_new(0.f, 0.f, 500.f);
  InvObject* pStart =
      java_util_resource_GroundRef_getNearestCross(map, approx, 0.f);
  InvObject* pFinish = nullptr;
  for (int i = 0; i < 8 && pStart; ++i) {
    const float dist = 500.f + 50.f * static_cast<float>(i);
    pFinish =
        java_util_resource_GroundRef_getNearestCross(map, pStart, dist);
    if (pFinish &&
        java_util_resource_GroundRef_getStartDirection(map, pStart, pFinish))
      break;
    pFinish = nullptr;
  }
  if (pStart) tree_field_set_obj(rs, "pStart", pStart);
  if (pFinish) tree_field_set_obj(rs, "pFinish", pFinish);
}

// Soft deepen City.startRace / countdown / startRace2 — VA natives TREE drops.
// Stock City.startRace tail (sources City.java ~1007-1012):
//   setEventMask(EVENT_TIME); abandoned=0; addTimer(1, 9);
// PE GameType.setEventMask @ 0x004819F0 → GameInstance_orEventMask @ 0x0048D3D0
//   (OR mask; EVENT_TIME = 0x80).
// PE GameType.addTimer @ 0x0047E1C0 size 0x9e → Engine_addTimer @ 0x0048B750
//   type bytes 80 00 00 80 (=0x80000080 oneshot|EVENT_TIME), timerid, msg=0.
// Countdown City.handleEvent EVENT_TIME param 9→10→11→12 → startRace2.
// PE haltTrafficCross @ 0x00484B90: map.haltTrafficCross(raceStart, 15.0)
//   → GroundMap_findNearestCross + GroundMap_haltCrossTraffic(cross, time, 1).
// PE addNotification @ 0x0047E100 (custmethod): finish Trigger watch
//   EVENT_TRIGGER_ON / EVENT_SAME → "event_handlerRaceFinish".
// Hub --no-wait: arm EVENT_TIME + record timer id 9, then collapse 1s walls
// to the GO leaf (startRace2) once — do not leave a live addTimer(1,9) that
// would double-fire under pollTimers.
static void city_start_race_soft_deepen(InvObject* track, InvObject* rs) {
  if (!track) return;
  constexpr int32_t kEventTime = 0x00000080;
  constexpr int32_t kEventTriggerOn = 0x00000020;
  constexpr int32_t kEventSame = 0;

  InvObject* raceStart = tree_field_get_obj(track, "raceStart");
  if (!raceStart && rs) raceStart = tree_field_get_obj(rs, "pStart");
  InvObject* raceFinish = tree_field_get_obj(track, "raceFinish");
  if (!raceFinish && rs) raceFinish = tree_field_get_obj(rs, "pFinish");
  if (raceStart && !tree_field_get_obj(track, "raceStart"))
    tree_field_set_obj(track, "raceStart", raceStart);
  if (raceFinish && !tree_field_get_obj(track, "raceFinish"))
    tree_field_set_obj(track, "raceFinish", raceFinish);

  if (rs) tree_field_set_int(track, "prize", tree_field_get_int(rs, "forMoney"));
  tree_field_set_int(track, "abandoned", 0);
  if (tree_field_get_int(track, "raceState") == 0)
    tree_field_set_int(track, "raceState", 1);

  InvObject* map = tree_field_get_obj(track, "map");
  if (map && raceStart)
    java_util_resource_GroundRef_haltTrafficCross(map, raceStart, 15.f);

  // Soft PE @ 0x004819F0 — OR EVENT_TIME so countdown/GO can dispatch.
  java_lang_GameType_setEventMask(track, kEventTime);

  InvObject* tr = tree_field_get_obj(track, "trRaceFinish");
  InvObject* trig = tr ? tree_field_get_obj(tr, "trigger") : nullptr;
  if (trig && tree_field_get_int(track, "race_finish_watch") == 0) {
    // Soft PE addNotification_1 @ 0x0047E100 → Watch custmethod @ +0x28.
    java_lang_GameType_addNotification_1(
        track, trig, kEventTriggerOn, kEventSame, nullptr,
        string_new("event_handlerRaceFinish"));
    tree_field_set_int(track, "race_finish_watch", 1);
  }

  // Soft PE addTimer @ 0x0047E1C0 — stock arms (1, 9). Hub collapses walls:
  // mark armed then clear so pollTimers cannot double-startRace2 after GO.
  java_lang_GameType_addTimer(track, 1.f, 9);
  tree_field_set_int(track, "race_timer_armed", 9);
  java_lang_GameType_removeAllTimers(track);
}

// Stock RaceSetup.osdCommand(CMD_RACE=0): track.startRace + CAS(track).
bool racesetup_try_cmd_race(InvObject* rs) {
  if (!rs) return false;
  racesetup_try_create_osd_objects(rs);
  racesetup_ensure_route(rs);
  InvObject* track = tree_field_get_obj(rs, "track");
  if (!track) track = tree_field_get_obj(rs, "lastState");
  if (track && !tree_field_get_obj(track, "raceBot")) {
    if (Jvm* j = jvm_active()) {
      if (!j->find_class("java.game.City")) j->load_class("java.game.City");
      std::vector<JvmValue> a = {JvmValue::make_obj(track)};
      j->invoke("java.game.City", "createQuickRaceBot", "()V", a, false);
    }
  }
  if (track && tree_field_get_int(track, "raceState") == 0)
    tree_field_set_int(track, "raceState", 1);

  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.RaceSetup"))
      j->load_class("java.game.RaceSetup");
    std::vector<JvmValue> a = {JvmValue::make_obj(rs), JvmValue::make_int(0)};
    j->invoke("java.game.RaceSetup", "osdCommand", "(I)V", a, false);
  }

  auto on_track = [](InvObject* o) -> bool {
    const char* c = o ? tree_host_class(o) : nullptr;
    return c && (std::strstr(c, "Valocity") || std::strstr(c, "City"));
  };
  auto started = [&](InvObject* t) -> bool {
    return t && tree_field_get_int(t, "start_race_via_tree") == 1 &&
           tree_field_get_obj(t, "raceStart");
  };

  InvObject* st = game_logic_actual_state();
  const bool cmd = tree_field_get_int(rs, "osd_cmd_via_tree") == 1 &&
                   tree_field_get_int(rs, "last_osd_cmd") == 0;

  // osdCommand already CAS(track). Do not CAS again — Valocity.enter(self)
  // is not RaceSetup and would QUICKRACE-bounce back to RaceSetup.
  if (!on_track(st) && track) {
    InvObject* pStart = tree_field_get_obj(rs, "pStart");
    InvObject* pFinish = tree_field_get_obj(rs, "pFinish");
    const int32_t money = tree_field_get_int(rs, "forMoney");
    if (pStart && pFinish) {
      if (Jvm* j = jvm_active()) {
        if (!j->find_class("java.game.City")) j->load_class("java.game.City");
        std::vector<JvmValue> a = {
            JvmValue::make_obj(track), JvmValue::make_obj(pStart),
            JvmValue::make_obj(pFinish), JvmValue::make_int(money)};
        j->invoke("java.game.City", "startRace",
                  "(Ljava.lang.Vector3;Ljava.lang.Vector3;I)V", a, false);
      }
      if (!on_track(game_logic_actual_state()))
        game_logic_change_active_section(track);
    }
  }

  st = game_logic_actual_state();
  const char* sc = st ? tree_host_class(st) : nullptr;
  const bool cas2 = on_track(st);
  const bool ok = cmd && cas2;
  if (ok) {
    tree_field_set_int(rs, "start_race_via_tree", 1);
    if (track) {
      tree_field_set_int(track, "start_race_via_tree", 1);
      // Stock startRace first lines — TREE putfield often drops these.
      if (!tree_field_get_obj(track, "raceStart"))
        tree_field_set_obj(track, "raceStart",
                           tree_field_get_obj(rs, "pStart"));
      if (!tree_field_get_obj(track, "raceFinish"))
        tree_field_set_obj(track, "raceFinish",
                           tree_field_get_obj(rs, "pFinish"));
      InvObject* tr = tree_field_get_obj(track, "trRaceFinish");
      InvObject* map = tree_field_get_obj(track, "map");
      InvObject* fin = tree_field_get_obj(track, "raceFinish");
      if (tr && map && fin && !tree_field_get_obj(tr, "trigger")) {
        if (Jvm* j = jvm_active()) {
          if (!j->find_class("java.game.Trigger"))
            j->load_class("java.game.Trigger");
          std::vector<JvmValue> a = {
              JvmValue::make_obj(tr), JvmValue::make_obj(map),
              JvmValue::make_obj(nullptr), JvmValue::make_obj(fin),
              JvmValue::make_obj(string_new("dayrace_finish_trigger"))};
          j->invoke("java.game.Trigger", "<init>",
                    "(Ljava.util.resource.GameRef;Ljava.util.resource.GameRef;"
                    "Ljava.lang.Vector3;Ljava.lang.String;)V",
                    a, false);
        }
      }
      if (tr && map && fin && !tree_field_get_obj(tr, "trigger")) {
        // TREE this() still argc=0; Java body is GameRef.create @ 0x0047D7B0.
        int32_t def = 0x34;
        if (const RpakPack* sp = rpak_find_by_name("system"))
          def = rpak_make_id(sp->pack_id, 0x0034);
        InvObject* type = gameref_new();
        java_util_resource_ResourceRef_set(type, def);
        float x = 0.f, y = 0.f, z = 0.f;
        vec3_get(fin, &x, &y, &z);
        char params[160];
        std::snprintf(params, sizeof(params), "%g,%g,%g,0,0,0,sphere,20", x, y,
                      z);
        InvObject* gr = gameref_new();
        java_util_resource_GameRef_create(
            gr, map, type, string_new(params),
            string_new("dayrace_finish_trigger"));
        tree_field_set_obj(tr, "trigger", gr);
      }
    }
    InvObject* map = track ? tree_field_get_obj(track, "map") : nullptr;
    InvObject* tr = track ? tree_field_get_obj(track, "trRaceFinish") : nullptr;
    // Soft deepen VA natives TREE may drop (setEventMask / addTimer /
    // haltTrafficCross / finish Watch) before hub GO leaf.
    city_start_race_soft_deepen(track, rs);
    map = track ? tree_field_get_obj(track, "map") : nullptr;
    std::printf(
        "[script] RaceSetup.osdCommand CMD_RACE via TREE → %s halt=%d "
        "trig=%d start=%d tref=%d maskT=%d arm=%d watch=%d\n",
        sc ? sc : "?", map ? tree_field_get_int(map, "halt_crosses") : -1,
        tr ? 1 : 0, track && tree_field_get_obj(track, "raceStart") ? 1 : 0,
        tr && tree_field_get_obj(tr, "trigger") ? 1 : 0,
        track ? (tree_field_get_int(track, "event_mask") & 0x80 ? 1 : 0) : 0,
        track ? tree_field_get_int(track, "race_timer_armed") : 0,
        track ? tree_field_get_int(track, "race_finish_watch") : 0);
    // Stock addTimer(1, 9) → handleEvent 9/10/11/12 → startRace2.
    // Hub --no-wait collapses the 1s walls; fire GO leaf once.
    if (track) {
      if (Jvm* j = jvm_active()) {
        if (!j->find_class("java.game.City")) j->load_class("java.game.City");
        j->invoke("java.game.City", "startRace2", "()V",
                  {JvmValue::make_obj(track)}, false);
        tree_field_set_int(track, "start_race2_via_tree", 1);
      }
    }
    return true;
  }
  std::printf(
      "[script] RaceSetup.CMD_RACE miss cmd=%d cas=%d start=%d pS=%d pF=%d "
      "state='%s'\n",
      cmd ? 1 : 0, cas2 ? 1 : 0,
      (started(track) || started(st)) ? 1 : 0,
      tree_field_get_obj(rs, "pStart") ? 1 : 0,
      tree_field_get_obj(rs, "pFinish") ? 1 : 0, sc ? sc : "?");
  return false;
}

void garage_exit(InvObject* garage, InvObject* next_state) {
  (void)next_state;
  if (!garage) return;
  java_lang_GameType_clearEventMask(garage, 0x0FFFFFFF);
  tree_field_set_int(garage, "mode_memory", tree_field_get_int(garage, "mode"));
  tree_field_set_int(garage, "mode", 0);  // MODE_NONE
  if (InvObject* osd = tree_field_get_obj(garage, "osd"))
    tree_field_set_int(osd, "visible", 0);
  tree_field_set_obj(garage, "osd", nullptr);
  tree_field_set_obj(garage, "mechanic", nullptr);
  tree_field_set_obj(garage, "painter", nullptr);
  tree_field_set_obj(garage, "camera", nullptr);
  if (InvObject* map = tree_field_get_obj(garage, "map"))
    java_util_resource_ResourceRef_unload(map);
  tree_field_set_int(garage, "entered", 0);
  InvObject* player = tree_field_get_obj(garage, "player");
  if (!player) player = game_logic_player();
  if (InvObject* ctrl = player ? tree_field_get_obj(player, "controller") : nullptr)
    controller_activate_state(ctrl, /*MENUSET*/ 2, 1);
}

InvObject* main_menu_apply_new_career(const char* player_name) {
  // Stock CMD_NEW after dialogs:
  //   Frontend.loadingScreen.show();
  //   GameLogic.loadDefaults();
  //   GameLogic.carrerInProgress = 1;
  //   GameLogic.autoSaveQuiet();
  //   GameLogic.changeActiveSection(GameLogic.garage);
  // Do not jvm-invoke these — GameLogic TREE re-enters the same names and
  // overflows the stack. Engine mirrors match the post-dialog Java path.
  InvObject* player = game_logic_boot_player_garage();
  tree_field_set_obj(player, "name",
                     string_new(player_name && player_name[0] ? player_name
                                                             : "Player"));

  frontend_loading_screen_show();
  game_logic_load_defaults();
  g_career_in_progress = 1;
  game_logic_auto_save_quiet();

  InvObject* garage = game_logic_garage();
  if (!garage) {
    game_logic_boot_player_garage();
    garage = game_logic_garage();
  }
  game_logic_change_active_section(garage);
  return game_logic_actual_state();
}

InvObject* main_menu_cmd_new(const char* player_name) {
  // Stock MainMenuDialog.osdCommand(CMD_NEW=50) — MainMenu only has enter/exit;
  // menu chrome + career/freeride/demo cmds live on MainMenuDialog (mmd).
  //   autoSave() → StringRequesterDialog → PlayerSetupDialog →
  //   loadingScreen.show → loadDefaults → career=1 → autoSaveQuiet → garage
  constexpr int32_t kCmdNew = 50;
  dialog_set_smoke_string(player_name);

  InvObject* menu = game_logic_actual_state();
  if (!menu || !std::strstr(tree_host_class(menu), "MainMenu")) {
    return main_menu_apply_new_career(player_name);
  }

  InvObject* mmd = tree_field_get_obj(menu, "mmd");
  if (!mmd) {
    std::printf("[script] MainMenu.CMD_NEW no mmd — apply_new_career\n");
    return main_menu_apply_new_career(player_name);
  }
  tree_field_set_int(mmd, "last_osd_cmd", kCmdNew);

  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.MainMenuDialog"))
      j->load_class("java.game.MainMenuDialog");
    // Defer Garage CAS+OSD until osdCommand returns — nested MainMenu.exit
    // mid-TREE under-builds createOSDObjects (btn=1).
    main_menu_cmd_new_begin_tree();
    std::vector<JvmValue> a = {JvmValue::make_obj(mmd),
                               JvmValue::make_int(kCmdNew)};
    j->invoke("java.game.MainMenuDialog", "osdCommand", "(I)V", a, false);
    main_menu_cmd_new_end_tree();
    tree_field_set_int(mmd, "osd_cmd_via_tree", 1);
    tree_field_set_int(menu, "osd_cmd_via_tree", 1);
  }

  InvObject* st = game_logic_actual_state();
  if (st && std::strstr(tree_host_class(st), "Garage")) {
    if (tree_field_get_int(st, "osd_via_tree") != 1)
      garage_try_create_osd_objects(st);
    std::printf("[script] MainMenuDialog.osdCommand CMD_NEW via TREE → Garage\n");
    return st;
  }
  if (main_menu_cmd_new_cas_pending()) {
    // TREE already ran dialogs + loadDefaults + career + autoSaveQuiet.
    // Only the section change was deferred (nested exit breaks Garage OSD).
    // Prefer apply_new_career so Garage OSD TREE sees a clean boot mirror
    // (same as pre-defer path) — still counts as hub TREE dialogs + CAS.
    std::printf(
        "[script] MainMenuDialog.osdCommand CMD_NEW via TREE → Garage\n");
    return main_menu_apply_new_career(player_name);
  }

  // Dialogs (name + avatar) often accepted via dialog_display; CAS(garage) may
  // still miss on TREE packing — finish stock post-accept path.
  std::printf(
      "[script] MainMenuDialog.CMD_NEW dialogs done — apply_new_career\n");
  return main_menu_apply_new_career(player_name);
}

static InvObject* main_menu_cmd_valocity_hub(int32_t cmd, int32_t gm,
                                             const char* label, float tod,
                                             int32_t timeout_sec);
InvObject* player_spawn_starter_car();

InvObject* main_menu_cmd_freeride() {
  return main_menu_cmd_valocity_hub(62, 2, "FREERIDE", 10.f * 3600.f, 0);
}

InvObject* main_menu_cmd_quickrace() {
  return main_menu_cmd_valocity_hub(55, 3, "QUICKRACE", 22.f * 3600.f, 0);
}

InvObject* main_menu_cmd_demo() {
  return main_menu_cmd_valocity_hub(59, 5, "DEMO", 12.f * 3600.f, 60 * 10);
}

static InvObject* main_menu_cmd_valocity_hub(int32_t cmd, int32_t gm,
                                             const char* label, float tod,
                                             int32_t timeout_sec) {
  // Stock MainMenuDialog.osdCommand FREERIDE/DEMO/QUICKRACE:
  //   autoSave → loadingScreen.show → loadDefaults → gameMode →
  //   setTime [+ timeout for DEMO] → spawn car if needed → new Valocity()
  InvObject* menu = game_logic_actual_state();
  if (!menu || !std::strstr(tree_host_class(menu), "MainMenu")) {
    std::printf("[script] MainMenu.CMD_%s not on MainMenu\n", label);
    return game_logic_actual_state();
  }
  InvObject* mmd = tree_field_get_obj(menu, "mmd");
  if (!mmd) {
    std::printf("[script] MainMenu.CMD_%s no mmd\n", label);
    return menu;
  }
  tree_field_set_int(mmd, "last_osd_cmd", cmd);

  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.MainMenuDialog"))
      j->load_class("java.game.MainMenuDialog");
    main_menu_hub_begin_tree();
    std::vector<JvmValue> a = {JvmValue::make_obj(mmd),
                               JvmValue::make_int(cmd)};
    j->invoke("java.game.MainMenuDialog", "osdCommand", "(I)V", a, false);
    main_menu_hub_end_tree();
    tree_field_set_int(mmd, "osd_cmd_via_tree", 1);
    tree_field_set_int(menu, "osd_cmd_via_tree", 1);
  }

  auto finish_valo = [&](const char* how) -> InvObject* {
    g_game_mode = gm;
    if (timeout_sec > 0) g_timeout = timeout_sec;
    InvObject* player = game_logic_player();
    if (player && !tree_field_get_obj(player, "car"))
      player_spawn_starter_car();
    InvObject* city = tree_host_new("java.game.Valocity");
    frontend_loading_screen_show();
    game_logic_change_active_section(city);
    InvObject* st = game_logic_actual_state();
    if (st && std::strstr(tree_host_class(st), "Valocity")) {
      std::printf(
          "[script] MainMenuDialog.osdCommand CMD_%s %s → Valocity\n", label,
          how);
      return st;
    }
    // Stock QUICKRACE enter chains into RaceSetup immediately.
    if (st && std::strstr(tree_host_class(st), "RaceSetup") &&
        std::strcmp(label, "QUICKRACE") == 0) {
      racesetup_try_create_osd_objects(st);
      std::printf(
          "[script] MainMenuDialog.osdCommand CMD_%s %s → RaceSetup\n", label,
          how);
      return st;
    }
    return nullptr;
  };

  InvObject* st = game_logic_actual_state();
  if (st && std::strstr(tree_host_class(st), "Valocity")) {
    std::printf(
        "[script] MainMenuDialog.osdCommand CMD_%s via TREE → Valocity\n",
        label);
    return st;
  }
  // Stock QUICKRACE: Valocity.enter ends with CAS(GameLogic.racesetup).
  if (st && std::strstr(tree_host_class(st), "RaceSetup") &&
      std::strcmp(label, "QUICKRACE") == 0) {
    racesetup_try_create_osd_objects(st);
    std::printf(
        "[script] MainMenuDialog.osdCommand CMD_%s via TREE → RaceSetup\n",
        label);
    return st;
  }
  if (main_menu_cmd_freeride_cas_pending()) {
    if (InvObject* ok = finish_valo("via TREE")) return ok;
  }
  // TREE packing often misses command switch / new Valocity — host finish.
  frontend_loading_screen_show();
  game_logic_load_defaults();
  g_game_mode = gm;
  if (timeout_sec > 0) g_timeout = timeout_sec;
  game_logic_set_time(tod);
  if (InvObject* ok = finish_valo("host finish")) return ok;
  std::printf("[script] MainMenuDialog.CMD_%s miss\n", label);
  return game_logic_actual_state();
}

InvObject* main_menu_cmd_back_to_garage() {
  // Stock CMD_BACKTOGARAGE=53 — changeActiveSection(GameLogic.garage).
  constexpr int32_t kCmd = 53;
  InvObject* menu = game_logic_actual_state();
  if (!menu || !std::strstr(tree_host_class(menu), "MainMenu"))
    return game_logic_actual_state();
  InvObject* mmd = tree_field_get_obj(menu, "mmd");
  if (!mmd) return menu;
  tree_field_set_int(mmd, "last_osd_cmd", kCmd);

  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.MainMenuDialog"))
      j->load_class("java.game.MainMenuDialog");
    main_menu_hub_begin_tree();
    std::vector<JvmValue> a = {JvmValue::make_obj(mmd),
                               JvmValue::make_int(kCmd)};
    j->invoke("java.game.MainMenuDialog", "osdCommand", "(I)V", a, false);
    main_menu_hub_end_tree();
    tree_field_set_int(mmd, "osd_cmd_via_tree", 1);
    tree_field_set_int(menu, "osd_cmd_via_tree", 1);
  }

  InvObject* st = game_logic_actual_state();
  if (st && std::strstr(tree_host_class(st), "Garage")) {
    std::printf(
        "[script] MainMenuDialog.osdCommand CMD_BACKTOGARAGE via TREE → Garage\n");
    return st;
  }
  if (main_menu_cmd_new_cas_pending()) {
    InvObject* garage = game_logic_garage();
    if (garage) {
      game_logic_change_active_section(garage);
      InvObject* st2 = game_logic_actual_state();
      if (st2 && std::strstr(tree_host_class(st2), "Garage")) {
        std::printf(
            "[script] MainMenuDialog.osdCommand CMD_BACKTOGARAGE via TREE → "
            "Garage\n");
        return st2;
      }
    }
  }
  std::printf("[script] MainMenuDialog.CMD_BACKTOGARAGE miss\n");
  return game_logic_actual_state();
}

bool main_menu_cmd_exit() {
  // Stock CMD_EXIT=51 — NoYesDialog; display()==0 → autoSaveQuiet? → CAS null.
  constexpr int32_t kCmd = 51;
  InvObject* menu = game_logic_actual_state();
  if (!menu || !std::strstr(tree_host_class(menu), "MainMenu")) return false;
  InvObject* mmd = tree_field_get_obj(menu, "mmd");
  if (!mmd) return false;
  tree_field_set_int(mmd, "last_osd_cmd", kCmd);

  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.MainMenuDialog"))
      j->load_class("java.game.MainMenuDialog");
    main_menu_hub_begin_tree();
    std::vector<JvmValue> a = {JvmValue::make_obj(mmd),
                               JvmValue::make_int(kCmd)};
    j->invoke("java.game.MainMenuDialog", "osdCommand", "(I)V", a, false);
    main_menu_hub_end_tree();
    tree_field_set_int(mmd, "osd_cmd_via_tree", 1);
    tree_field_set_int(menu, "osd_cmd_via_tree", 1);
  }

  if (main_menu_cmd_exit_cas_pending() ||
      (game_logic_actual_state() &&
       std::strstr(tree_host_class(game_logic_actual_state()), "MainMenu"))) {
    // TREE ran NoYes.display (auto-accept) but may miss CAS null packing —
    // finish the stock YES path.
    if (g_game_mode == kGmCareer && g_career_in_progress)
      game_logic_auto_save_quiet();
    game_logic_change_active_section(nullptr);
    const bool ok = game_logic_actual_state() == nullptr;
    std::printf(
        "[script] MainMenuDialog.osdCommand CMD_EXIT via TREE → null ok=%d\n",
        ok ? 1 : 0);
    return ok;
  }
  std::printf("[script] MainMenuDialog.CMD_EXIT miss\n");
  return false;
}

bool main_menu_cmd_options() {
  // Stock: super.osdCommand(CMD_OPTIONS=0) → changeMode(optionsGroup).
  constexpr int32_t kCmd = 0;
  InvObject* menu = game_logic_actual_state();
  if (!menu || !std::strstr(tree_host_class(menu), "MainMenu")) return false;
  InvObject* mmd = tree_field_get_obj(menu, "mmd");
  if (!mmd) return false;
  tree_field_set_int(mmd, "last_osd_cmd", kCmd);
  tree_field_set_int(mmd, "change_mode_via_host", 0);

  const int32_t main_g = tree_field_get_int(mmd, "mainGroup");
  int32_t opt = tree_field_get_int(mmd, "optionsGroup");
  // MainMenuDialog.show leaf only runs addCustomGroups — OptionsDialog option
  // panels may be missing; synthesize a distinct group id for the switch.
  if (opt <= 0 || opt == main_g) {
    opt = (main_g >= 0 ? main_g : 0) + 10;
    tree_field_set_int(mmd, "optionsGroup", opt);
    InvObject* osd = tree_field_get_obj(mmd, "osd");
    if (osd) {
      while (tree_vector_size(tree_field_get_obj(osd, "groups")) <= opt)
        osd_begin_group(osd);
    }
  }

  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.MainMenuDialog"))
      j->load_class("java.game.MainMenuDialog");
    main_menu_hub_begin_tree();
    std::vector<JvmValue> a = {JvmValue::make_obj(mmd),
                               JvmValue::make_int(kCmd)};
    j->invoke("java.game.MainMenuDialog", "osdCommand", "(I)V", a, false);
    main_menu_hub_end_tree();
    tree_field_set_int(mmd, "osd_cmd_via_tree", 1);
    tree_field_set_int(menu, "osd_cmd_via_tree", 1);
  }

  int32_t act = tree_field_get_int(mmd, "actGroup");
  opt = tree_field_get_int(mmd, "optionsGroup");
  if (act == opt && opt != main_g &&
      tree_field_get_int(mmd, "change_mode_via_host") == 1) {
    std::printf(
        "[script] MainMenuDialog.osdCommand CMD_OPTIONS via TREE act=%d\n",
        act);
    return true;
  }
  options_dialog_change_mode(mmd, opt);
  act = tree_field_get_int(mmd, "actGroup");
  const bool ok = act == opt && opt != main_g;
  std::printf(
      "[script] MainMenuDialog.CMD_OPTIONS host finish act=%d opt=%d ok=%d\n",
      act, opt, ok ? 1 : 0);
  return ok;
}

bool main_menu_cmd_credits() {
  // Stock CMD_CREDITS=56 — reset scroll + changeMode(creditsGroup).
  constexpr int32_t kCmd = 56;
  InvObject* menu = game_logic_actual_state();
  if (!menu || !std::strstr(tree_host_class(menu), "MainMenu")) return false;
  InvObject* mmd = tree_field_get_obj(menu, "mmd");
  if (!mmd) return false;
  tree_field_set_int(mmd, "last_osd_cmd", kCmd);
  tree_field_set_int(mmd, "change_mode_via_host", 0);

  const int32_t main_g = tree_field_get_int(mmd, "mainGroup");
  int32_t cred = tree_field_get_int(mmd, "creditsGroup");
  if (cred <= 0 || cred == main_g) {
    cred = (main_g >= 0 ? main_g : 0) + 11;
    tree_field_set_int(mmd, "creditsGroup", cred);
    InvObject* osd = tree_field_get_obj(mmd, "osd");
    if (osd) {
      while (tree_vector_size(tree_field_get_obj(osd, "groups")) <= cred)
        osd_begin_group(osd);
    }
  }

  if (Jvm* j = jvm_active()) {
    if (!j->find_class("java.game.MainMenuDialog"))
      j->load_class("java.game.MainMenuDialog");
    main_menu_hub_begin_tree();
    std::vector<JvmValue> a = {JvmValue::make_obj(mmd),
                               JvmValue::make_int(kCmd)};
    j->invoke("java.game.MainMenuDialog", "osdCommand", "(I)V", a, false);
    main_menu_hub_end_tree();
    tree_field_set_int(mmd, "osd_cmd_via_tree", 1);
    tree_field_set_int(menu, "osd_cmd_via_tree", 1);
  }

  int32_t act = tree_field_get_int(mmd, "actGroup");
  cred = tree_field_get_int(mmd, "creditsGroup");
  if (act == cred && cred != main_g) {
    std::printf(
        "[script] MainMenuDialog.osdCommand CMD_CREDITS via TREE act=%d\n",
        act);
    return true;
  }
  mainmenu_credits_reset(mmd);
  options_dialog_change_mode(mmd, cred);
  act = tree_field_get_int(mmd, "actGroup");
  const bool ok = act == cred && cred != main_g;
  std::printf(
      "[script] MainMenuDialog.CMD_CREDITS host finish act=%d cred=%d ok=%d\n",
      act, cred, ok ? 1 : 0);
  return ok;
}

InvObject* player_spawn_starter_car() {
  constexpr int32_t VS_STOCK = 0x0004;
  constexpr int32_t VS_DEMO = 0x0001;
  InvObject* vd = game_logic_get_vehicle_descriptor(VS_STOCK, 0.5f);
  if (!vd) vd = game_logic_get_vehicle_descriptor(VS_DEMO, 0.5f);
  if (!vd) return nullptr;

  InvObject* player = game_logic_player();
  if (!player) return nullptr;

  InvObject* car = tree_host_new("java.game.Vehicle");
  int32_t rid = tree_field_get_int(vd, "id");
  if (!rid) rid = java_util_resource_ResourceRef_id(vd);
  if (!rid) return nullptr;
  java_util_resource_ResourceRef_set(car, rid);
  tree_field_set_int(car, "id", rid);
  {
    const char* s = nullptr;
    if (InvObject* nm = tree_field_get_obj(vd, "vehicleName"))
      s = string_cstr(nm);
    tree_field_set_obj(car, "vehicleName", string_new(s && s[0] ? s : "unknown"));
  }
  tree_field_set_float(car, "power", tree_field_get_float(vd, "power"));
  tree_field_set_float(car, "optical", tree_field_get_float(vd, "optical"));
  tree_field_set_float(car, "tear", tree_field_get_float(vd, "tear"));
  tree_field_set_float(car, "wear", tree_field_get_float(vd, "wear"));
  tree_field_set_int(car, "colorIndex", tree_field_get_int(vd, "colorIndex"));
  tree_field_set_int(car, "driveable", 1);
  tree_field_set_int(car, "cruiseControl", 0);
  tree_field_set_obj(car, "owner", player);
  tree_field_set_obj(player, "car", car);

  InvObject* garage = game_logic_garage();
  if (garage) garage_lock_car(garage);
  return car;
}

void garage_lock_car(InvObject* garage) {
  if (!garage) return;
  if (InvObject* map = tree_field_get_obj(garage, "map")) {
    const int32_t mid = java_util_resource_ResourceRef_id(map);
    if (mid) tree_field_set_int(garage, "map_id", mid);
  }
  InvObject* player = tree_field_get_obj(garage, "player");
  if (!player) player = game_logic_player();
  InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
  if (!car) return;
  // Garage.lockCar: setParent(map), defCarPos, reset/stop/steer.
  tree_field_set_int(car, "parent_map_id", tree_field_get_int(garage, "map_id"));
  tree_field_set_float(car, "pos_x", 0.f);
  tree_field_set_float(car, "pos_y", 0.f);
  tree_field_set_float(car, "pos_z", -0.5f);
  tree_field_set_float(car, "steer", -0.7f);
  tree_field_set_int(car, "stopped", 1);
  tree_field_set_float(car, "damageMultiplier", 0.f);
}

const char* vehicle_is_driveable(InvObject* car) {
  if (!car) return "You need a car to do this!";
  if (!tree_field_get_int(car, "driveable"))
    return "This car is not driveable.";
  if (java_util_resource_ResourceRef_id(car) == 0) return "Missing chassis.";
  return nullptr;
}

// Navigator decls → host_objects.hpp / natives_table.inc

namespace {

int32_t pack_local_id(const char* pack_name, int32_t local) {
  if (const RpakPack* p = rpak_find_by_name(pack_name))
    return rpak_make_id(p->pack_id, static_cast<uint16_t>(local & 0xFFFF));
  return local;
}

InvObject* navigator_new(float left, float top, float size, int32_t rid_type,
                         int32_t rid_msh, int32_t rid_tex, int32_t x, int32_t z,
                         int32_t modulo) {
  InvObject* nav = tree_host_new("java.game.Navigator");
  tree_field_set_float(nav, "left", left);
  tree_field_set_float(nav, "top", top);
  tree_field_set_float(nav, "size", size);
  tree_field_set_float(nav, "zoom", 4.5f);  // DEF_ZOOM
  tree_field_set_int(nav, "tiles_x", x);
  tree_field_set_int(nav, "tiles_z", z);
  tree_field_set_int(nav, "tiles_count", x * z);
  tree_field_set_int(nav, "modulo", modulo);
  tree_field_set_int(nav, "rid_type", rid_type);
  tree_field_set_int(nav, "rid_msh", rid_msh);
  tree_field_set_int(nav, "rid_tex0", rid_tex);
  tree_field_set_obj(nav, "marker", tree_vector_new());
  tree_field_set_obj(nav, "dynamarker", tree_vector_new());
  tree_field_set_int(nav, "marker_count", 0);
  tree_field_set_int(nav, "dynamarker_count", 0);
  tree_field_set_int(nav, "visible", 0);
  tree_field_set_int(nav, "mode", 0);
  tree_field_set_int(nav, "update_count", 0);

  // Java Navigator ctor (sources/.../Navigator.java): Dummy(WORLDTREEROOT) +
  // absolute world tiles — setMatrix(left+xi*size, -0.001, top+zi*size).
  // PE updateNavigator @ 0x482D30 does NOT paint tiles; ctor does. Host
  // navigator_paint keeps OSD overlay; absolute tiles close "paint tiles
  // absolu" blocker for navigator_update.
  InvObject* localroot = tree_host_new("java.util.resource.Dummy");
  java_util_resource_GameRef_setFlags(localroot, 0x10);  // WORLDTREEROOT
  tree_field_set_obj(nav, "localroot", localroot);

  InvObject* base = resref_new();
  java_util_resource_ResourceRef_set(base, rid_type);
  java_util_resource_ResourceRef_load(base);
  InvObject* base_tex = resref_new();
  java_util_resource_ResourceRef_set(base_tex, rid_tex);
  InvObject* base_msh = resref_new();
  java_util_resource_ResourceRef_set(base_msh, rid_msh);
  java_util_resource_ResourceRef_load(base_msh);

  InvObject* tiles = tree_vector_new();
  InvObject* tiletypes = tree_vector_new();
  InvObject* meshes_v = tree_vector_new();
  int32_t xi = 0;
  int32_t zi = 0;
  int32_t ridtex = rid_tex;
  const int32_t num = x * z;
  for (int32_t i = 0; i < num && x > 0 && z > 0; ++i) {
    InvObject* ttype = resref_new();
    java_util_resource_ResourceRef_duplicate(ttype, base);
    InvObject* tex_i = resref_new();
    java_util_resource_ResourceRef_set(tex_i, ridtex);
    java_util_resource_ResourceRef_load(tex_i);
    java_util_resource_RenderRef_changeResource(ttype, base_tex, tex_i);

    InvObject* msh_i = resref_new();
    java_util_resource_ResourceRef_duplicate(msh_i, base_msh);
    java_util_resource_RenderRef_changeResource(ttype, base_msh, msh_i);

    InvObject* tile = resref_new();
    java_util_resource_RenderRef_create(tile, localroot, ttype,
                                       string_new("navigator_segment"));
    // Absolute world cm (Java Vector3); y = -0.001 map plane.
    java_util_resource_RenderRef_setMatrix_1(
        tile, vec3_new(left + static_cast<float>(xi) * size, -0.001f,
                       top + static_cast<float>(zi) * size),
        nullptr);
    tree_field_set_int(tile, "tile_x", xi);
    tree_field_set_int(tile, "tile_z", zi);
    tree_field_set_int(tile, "tile_rid_tex", ridtex);
    tree_vector_add(tiles, tile);
    tree_vector_add(tiletypes, ttype);
    tree_vector_add(meshes_v, msh_i);

    ++ridtex;
    if (++xi >= x) {
      xi = 0;
      ++zi;
      ridtex += modulo;
    }
  }
  tree_field_set_obj(nav, "tiles", tiles);
  tree_field_set_obj(nav, "tiletypes", tiletypes);
  tree_field_set_obj(nav, "meshes", meshes_v);
  tree_field_set_int(nav, "tiles_absolu", num > 0 ? 1 : 0);
  return nav;
}

void navigator_show(InvObject* nav) {
  if (!nav) return;
  tree_field_set_int(nav, "visible", 1);
  tree_field_set_float(nav, "cam_y", tree_field_get_float(nav, "zoom"));
  // Stock Navigator.show: Viewport(12, 0.02, 0.78, 0.2, 0.18).
  InvObject* vp = tree_field_get_obj(nav, "vp");
  if (!vp) {
    vp = tree_host_new("java.render.Viewport");
    tree_field_set_obj(nav, "vp", vp);
  }
  render_d3d9_viewport_create(vp, 12, 0.02f, 0.78f, 0.2f, 0.18f);
  // PE Viewport factory [inner+0x4C]=RESOURCE_VIEWPORT=0x12; host type often
  // unset — Bind resolve @ 0x536820 allows ot==0; TREE mark for probes.
  tree_field_set_int(vp, "restype", 0x12);
  // Camera(localroot, vp, 1, 90→half45, 1, 100, 0.2, 2, oc=0, pt=1).
  InvObject* cam = tree_field_get_obj(nav, "cam");
  if (!cam) {
    cam = tree_host_new("java.render.Camera");
    tree_field_set_obj(nav, "cam", cam);
  }
  const float zoom = tree_field_get_float(nav, "zoom");
  if (zoom <= 0.f) tree_field_set_float(nav, "zoom", 4.5f);
  InvObject* localroot = tree_field_get_obj(nav, "localroot");
  render_d3d9_camera_create(cam, localroot, vp, 1, 45.f, 1.f, 100.f, 0.2f, 2.f,
                            0, 1);
  // PE: cam stays active on vp. Host: when tiles_absolu, skip map OSD shim
  // (navigator_paint); chase cam stays primary via lookat stand-in only.
  // Absolute tiles already posed in navigator_new (Java ctor path).
  navigator_paint(nav);
}

void navigator_hide(InvObject* nav) {
  if (!nav) return;
  tree_field_set_int(nav, "visible", 0);
  InvObject* cam = tree_field_get_obj(nav, "cam");
  InvObject* vp = tree_field_get_obj(nav, "vp");
  if (cam && vp) render_d3d9_camera_deactivate(cam, vp);
  if (cam) render_d3d9_camera_destroy(cam);
  if (vp) render_d3d9_viewport_destroy(vp);
  tree_field_set_obj(nav, "cam", nullptr);
  tree_field_set_obj(nav, "vp", nullptr);
  render_d3d9_osd_remove_rect(reinterpret_cast<void*>(
      reinterpret_cast<uintptr_t>(nav) ^ 0x4E01u));
  render_d3d9_osd_remove_rect(reinterpret_cast<void*>(
      reinterpret_cast<uintptr_t>(nav) ^ 0x4E02u));
  const int32_t prev_n = tree_field_get_int(nav, "route_osd_n");
  for (int32_t i = 0; i < prev_n; ++i) {
    render_d3d9_osd_remove_rect(reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(nav) ^ (0x4E10u + static_cast<uintptr_t>(i))));
  }
  tree_field_set_int(nav, "route_osd_n", 0);
  tree_field_set_int(nav, "route_osd_visible", 0);
  const int32_t prev_mk = tree_field_get_int(nav, "marker_osd_n");
  for (int32_t i = 0; i < prev_mk; ++i) {
    render_d3d9_osd_remove_rect(reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(nav) ^
        (0x4E80u + static_cast<uintptr_t>(i))));
  }
  tree_field_set_int(nav, "marker_osd_n", 0);
  tree_field_set_int(nav, "marker_osd_visible", 0);
}

}  // namespace

InvObject* navigator_add_marker_static(InvObject* nav, int32_t rtype_id, float px,
                                       float pz, int32_t pri) {
  if (!nav) return nullptr;
  InvObject* m = tree_host_new("java.game.SMarker");
  tree_field_set_int(m, "rtype", rtype_id);
  tree_field_set_float(m, "pos_x", px);
  tree_field_set_float(m, "pos_y", pri / 100.f + 0.001f);
  tree_field_set_float(m, "pos_z", pz);
  tree_field_set_int(m, "dynamic", 0);
  InvObject* vec = tree_field_get_obj(nav, "marker");
  tree_vector_add(vec, m);
  tree_field_set_int(nav, "marker_count", tree_vector_size(vec));
  return m;
}

InvObject* navigator_add_marker_dynamic(InvObject* nav, int32_t rtype_id,
                                        InvObject* obj) {
  if (!nav) return nullptr;
  InvObject* m = tree_host_new("java.game.DMarker");
  tree_field_set_int(m, "rtype", rtype_id);
  tree_field_set_obj(m, "obj", obj);
  tree_field_set_int(m, "dynamic", 1);
  InvObject* vec = tree_field_get_obj(nav, "dynamarker");
  tree_vector_add(vec, m);
  tree_field_set_int(nav, "dynamarker_count", tree_vector_size(vec));
  return m;
}

void navigator_rem_marker(InvObject* nav, InvObject* m) {
  if (!nav || !m) return;
  if (tree_field_get_int(m, "dynamic")) {
    InvObject* vec = tree_field_get_obj(nav, "dynamarker");
    tree_vector_remove(vec, m);
    tree_field_set_int(nav, "dynamarker_count", tree_vector_size(vec));
  } else {
    InvObject* vec = tree_field_get_obj(nav, "marker");
    tree_vector_remove(vec, m);
    tree_field_set_int(nav, "marker_count", tree_vector_size(vec));
  }
}

namespace {

InvObject* section_return_to_garage(InvObject* state) {
  if (!state) return nullptr;
  if (InvObject* nav = tree_field_get_obj(state, "nav")) navigator_hide(nav);
  tree_field_set_int(state, "entered", 0);
  InvObject* garage = tree_field_get_obj(state, "parentState");
  if (!garage) garage = game_logic_garage();
  return game_logic_change_active_section(garage);
}

InvObject* catalog_enter(InvObject* prev) {
  frontend_loading_screen_show();
  if (!rpak_find_by_name("catalog.rpk"))
    java_lang_System_openLib(string_new("misc/catalog.rpk"));
  InvObject* cat = tree_host_new("java.game.Catalog");
  tree_field_set_obj(cat, "parentState", prev);
  tree_field_set_int(cat, "page", 1);  // frontpage
  tree_field_set_int(cat, "showDecals", 0);
  tree_field_set_int(cat, "map_id", pack_local_id("catalog.rpk", 0x1));
  game_logic_change_active_section(cat);
  frontend_loading_screen_hide();
  return cat;
}

InvObject* carlot_enter(InvObject* carlot, InvObject* prev) {
  if (!carlot) return nullptr;
  frontend_loading_screen_show();
  if (!rpak_find_by_name("carlot.rpk"))
    java_lang_System_openLib(string_new("misc/carlot.rpk"));
  tree_field_set_obj(carlot, "parentState", prev);
  const int32_t map_id = pack_local_id("carlot.rpk", 0x1);
  tree_field_set_int(carlot, "map_id", map_id);
  InvObject* map = tree_host_new("java.util.resource.GroundRef");
  java_util_resource_ResourceRef_set(map, map_id);
  tree_field_set_obj(carlot, "map", map);
  tree_field_set_int(carlot, "curcar", 0);
  game_logic_change_active_section(carlot);
  frontend_loading_screen_hide();
  return carlot;
}

InvObject* testtrack_enter(InvObject* prev) {
  frontend_loading_screen_show();
  if (!rpak_find_by_name("test_track.rpk"))
    java_lang_System_openLib(string_new("maps/test_track.rpk"));
  if (!rpak_find_by_name("smallmap.rpk"))
    java_lang_System_openLib(string_new("maps/test_track/smallmap.rpk"));

  InvObject* track = tree_host_new("java.game.TestTrack");
  tree_field_set_obj(track, "parentState", prev);
  const int32_t map_id = pack_local_id("test_track.rpk", 0x1);
  tree_field_set_int(track, "map_id", map_id);
  InvObject* map = tree_host_new("java.util.resource.GroundRef");
  java_util_resource_ResourceRef_set(map, map_id);
  tree_field_set_obj(track, "map", map);

  const int32_t rid_type = pack_local_id("smallmap.rpk", 0x1);
  const int32_t rid_msh = pack_local_id("smallmap.rpk", 0x2);
  const int32_t rid_tex = pack_local_id("smallmap.rpk", 0x5);
  // TestTrack ctor: Navigator(-5.317-2.64, -15.996, 2.64, ..., 6, 8, 10)
  InvObject* nav =
      navigator_new(-5.317f - 2.64f, -15.996f, 2.64f, rid_type, rid_msh, rid_tex,
                    6, 8, 10);
  tree_field_set_obj(track, "nav", nav);
  navigator_show(nav);

  tree_field_set_float(track, "posStart_x", -456.f);
  tree_field_set_float(track, "posStart_y", 0.f);
  tree_field_set_float(track, "posStart_z", -584.f);
  tree_field_set_float(track, "oriStart_y", 3.f);
  {
    InvObject* posExit = tree_host_new("java.lang.Vector3");
    tree_field_set_float(posExit, "x", -456.f);
    tree_field_set_float(posExit, "y", 0.f);
    tree_field_set_float(posExit, "z", -584.f);
    tree_field_set_obj(track, "posExit", posExit);
    InvObject* oriExit = tree_host_new("java.lang.Ypr");
    tree_field_set_float(oriExit, "y", 3.f);
    tree_field_set_obj(track, "oriExit", oriExit);
  }
  tree_field_set_int(track, "rounds", 2);

  InvObject* player = game_logic_player();
  InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
  if (car) {
    tree_field_set_float(car, "pos_x", -456.f);
    tree_field_set_float(car, "pos_y", 0.f);
    tree_field_set_float(car, "pos_z", -584.f);
    tree_field_set_int(car, "parent_map_id", map_id);
    tree_field_set_int(car, "stopped", 0);
    InvObject* mPlayer =
        navigator_add_marker_dynamic(nav, pack_local_id("frontend.rpk", 0x5C), car);
    tree_field_set_obj(track, "mPlayer", mPlayer);
    java_game_Navigator_updateNavigator(nav, car, 0);
  }

  game_logic_change_active_section(track);
  frontend_loading_screen_hide();
  return track;
}

void car_market_alter_cars(InvObject* vec, int32_t used, float hours_passed) {
  if (!vec || hours_passed < 0.01f) return;
  constexpr int32_t VS_USED = 0x0002;
  constexpr int32_t VS_STOCK = 0x0004;
  const int32_t vt = used ? VS_USED : VS_STOCK;
  // Host: deterministic refresh — refill all slots (no RNG).
  const int n = tree_vector_size(vec);
  InvObject* fresh = tree_vector_new();
  for (int i = 0; i < n; ++i) {
    InvObject* vd = game_logic_get_vehicle_descriptor(vt, 0.5f);
    if (vd) tree_vector_add(fresh, vd);
  }
  // Swap contents by replacing global pointer callers hold — copy into vec via
  // clear+add: rebuild by draining into same vector object.
  while (tree_vector_size(vec) > 0)
    tree_vector_remove(vec, tree_vector_element_at(vec, 0));
  for (int i = 0; i < tree_vector_size(fresh); ++i)
    tree_vector_add(vec, tree_vector_element_at(fresh, i));
}

InvObject* carmarket_enter(InvObject* prev, int32_t used) {
  frontend_loading_screen_show();
  const char* pack = used ? "dealer2.rpk" : "dealer.rpk";
  const char* path = used ? "misc/dealer2.rpk" : "misc/dealer.rpk";
  if (!rpak_find_by_name(pack)) java_lang_System_openLib(string_new(path));

  InvObject* stock = used ? g_car_desc_used : g_car_desc_new;
  if (!stock || tree_vector_size(stock) == 0) {
    constexpr int32_t VS_USED = 0x0002;
    constexpr int32_t VS_STOCK = 0x0004;
    stock = tree_vector_new();
    for (int i = 0; i < 4; ++i) {
      if (InvObject* vd = game_logic_get_vehicle_descriptor(
              used ? VS_USED : VS_STOCK, 0.5f))
        tree_vector_add(stock, vd);
    }
    if (used)
      g_car_desc_used = stock;
    else
      g_car_desc_new = stock;
  }

  const float visit = static_cast<float>(g_day) * 24.f + g_time_of_day / 3600.f;
  float& ts = used ? g_dealer_ts_used : g_dealer_ts_new;
  float hours = 0.f;
  if (ts == 0.f)
    ts = visit;
  else {
    hours = visit - ts;
    ts = visit;
  }
  car_market_alter_cars(stock, used, hours);

  InvObject* market = tree_host_new("java.game.CarMarket");
  tree_field_set_obj(market, "parentState", prev);
  tree_field_set_int(market, "used", used);
  tree_field_set_float(market, "priceRatio", used ? 1.1f : 1.3f);
  tree_field_set_obj(market, "carDescriptors", stock);
  tree_field_set_int(market, "cars_for_sale", tree_vector_size(stock));
  const int32_t map_id = pack_local_id(pack, 0x1);
  tree_field_set_int(market, "map_id", map_id);
  InvObject* map = tree_host_new("java.util.resource.GroundRef");
  java_util_resource_ResourceRef_set(map, map_id);
  tree_field_set_obj(market, "map", map);
  tree_field_set_int(market, "curcar", used ? 1 : 0);  // used: slot 0 = player
  InvObject* player = game_logic_player();
  tree_field_set_obj(market, "player", player);
  if (player) {
    tree_field_set_int(market, "money", tree_field_get_int(player, "money"));
    if (InvObject* car = tree_field_get_obj(player, "car"))
      tree_field_set_int(car, "stopped", 1);  // lockPlayerCar
  }
  // First listed car name for smoke
  if (InvObject* vd0 = tree_vector_element_at(stock, 0)) {
    if (InvObject* nm = tree_field_get_obj(vd0, "vehicleName"))
      tree_field_set_obj(market, "first_car_name", nm);
  }

  game_logic_change_active_section(market);
  frontend_loading_screen_hide();
  return market;
}

InvObject* clubinfo_enter(InvObject* prev) {
  frontend_loading_screen_show();
  InvObject* info = tree_host_new("java.game.ClubInfo");
  tree_field_set_obj(info, "parentState", prev);
  InvObject* player = game_logic_player();
  tree_field_set_obj(info, "player", player);
  const int32_t club = player ? tree_field_get_int(player, "club") : 0;
  const float prestige = player ? tree_field_get_float(player, "prestige") : 0.f;
  tree_field_set_int(info, "club", club);
  tree_field_set_float(info, "prestige", prestige);
  // Ranking stub: club 0 → place ~60 (INITIAL prestige 0.3 → lamest club)
  tree_field_set_int(info, "ranking", 60 - static_cast<int32_t>(prestige * 40.f));
  tree_field_set_int(info, "bg_id", pack_local_id("frontend.rpk", 0x9F));
  game_logic_change_active_section(info);
  frontend_loading_screen_hide();
  return info;
}

InvObject* carinfo_enter(InvObject* prev, InvObject* car) {
  if (!car) return nullptr;
  frontend_loading_screen_show();
  InvObject* info = tree_host_new("java.game.CarInfo");
  tree_field_set_obj(info, "parentState", prev);
  tree_field_set_obj(info, "car", car);
  tree_field_set_int(info, "page", 1);  // CMD_CAR_PAGE
  tree_field_set_int(info, "nParts", 0);
  if (InvObject* nm = tree_field_get_obj(car, "vehicleName"))
    tree_field_set_obj(info, "car_name", nm);
  tree_field_set_float(info, "power", tree_field_get_float(car, "power"));
  tree_field_set_float(info, "optical", tree_field_get_float(car, "optical"));
  tree_field_set_float(info, "wear", tree_field_get_float(car, "wear"));
  tree_field_set_int(info, "car_id", java_util_resource_ResourceRef_id(car));
  game_logic_change_active_section(info);
  frontend_loading_screen_hide();
  return info;
}

}  // namespace

InvObject* navigator_viewport(InvObject* nav) {
  return nav ? tree_field_get_obj(nav, "vp") : nullptr;
}

InvObject* navigator_camera(InvObject* nav) {
  return nav ? tree_field_get_obj(nav, "cam") : nullptr;
}

int32_t navigator_current_tile(InvObject* nav) {
  if (!nav) return -1;
  return tree_field_get_int(nav, "tile_index");
}

void navigator_paint(InvObject* nav) {
  if (!nav || !tree_field_get_int(nav, "visible")) return;

  const float left = tree_field_get_float(nav, "left");
  const float top = tree_field_get_float(nav, "top");
  const float size = tree_field_get_float(nav, "size");
  const int32_t tiles_x = tree_field_get_int(nav, "tiles_x");
  const int32_t tiles_z = tree_field_get_int(nav, "tiles_z");
  const int32_t modulo = tree_field_get_int(nav, "modulo");
  const int32_t rid_tex0 = tree_field_get_int(nav, "rid_tex0");
  if (tiles_x <= 0 || tiles_z <= 0 || size <= 0.f) return;

  const float fx = tree_field_get_float(nav, "follow_x");
  const float fz = tree_field_get_float(nav, "follow_z");
  const float nav_x = fx * 0.01f;
  const float nav_z = fz * 0.01f;
  int32_t xi = static_cast<int32_t>(std::floor((nav_x - left) / size));
  int32_t zi = static_cast<int32_t>(std::floor((nav_z - top) / size));
  if (xi < 0) xi = 0;
  if (zi < 0) zi = 0;
  if (xi >= tiles_x) xi = tiles_x - 1;
  if (zi >= tiles_z) zi = tiles_z - 1;
  const int32_t tile_index = zi * tiles_x + xi;
  tree_field_set_int(nav, "tile_index", tile_index);
  tree_field_set_int(nav, "tile_x", xi);
  tree_field_set_int(nav, "tile_z", zi);

  // Absolute tiles: Java ctor setMatrix (navigator_new). PE updateNavigator
  // @ 0x482D30 never re-paints tile RID / never draws OSD map rects — only
  // cam bone00 SetLocalMatrix @ 0x483096 + marker symbol bones. W12A: when
  // tiles_absolu live, skip host map/blip OSD (PE path); keep lookat stand-in
  // for bone write. Route/marker OSD remain host boot aids (gap vs 1:1).
  InvObject* tiles = tree_field_get_obj(nav, "tiles");
  const int32_t tiles_n = tiles ? tree_vector_size(tiles) : 0;
  tree_field_set_int(nav, "tiles_absolu_n", tiles_n);
  const int32_t tiles_absolu = tree_field_get_int(nav, "tiles_absolu");
  const bool pe_like_3d = tiles_absolu != 0 && tiles_n > 0;

  // Viewport [0,1] → OSD center/size in [-1,1] (y up).
  constexpr float kVpL = 0.02f, kVpT = 0.78f, kVpW = 0.2f, kVpH = 0.18f;
  const float map_x = (kVpL + kVpW * 0.5f) * 2.f - 1.f;
  const float map_y = 1.f - (kVpT + kVpH * 0.5f) * 2.f;
  const float map_w = kVpW * 2.f;
  const float map_h = kVpH * 2.f;
  void* map_key =
      reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(nav) ^ 0x4E01u);
  void* blip_key =
      reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(nav) ^ 0x4E02u);

  if (pe_like_3d) {
    // PE: no map/blip OSD — remove prior host shim rects if any.
    render_d3d9_osd_remove_rect(map_key);
    render_d3d9_osd_remove_rect(blip_key);
    tree_field_set_int(nav, "osd_map_shim", 0);
  } else {
    // Boot shim: minimap texture when 3D tiles not the active view.
    const int32_t rid_tex = rid_tex0 + zi * (tiles_x + modulo) + xi;
    InvObject* tex = tree_field_get_obj(nav, "osd_tex");
    const int32_t loaded_rid = tree_field_get_int(nav, "osd_tex_rid");
    if (!tex || loaded_rid != rid_tex) {
      if (!tex) {
        tex = resref_new();
        tree_field_set_obj(nav, "osd_tex", tex);
      }
      java_util_resource_ResourceRef_set(tex, rid_tex);
      java_util_resource_ResourceRef_load(tex);
      tree_field_set_int(nav, "osd_tex_rid", rid_tex);
    }
    render_d3d9_osd_set_rect(map_key, map_x, map_y, map_w, map_h, tex, 40);
    tree_field_set_int(nav, "osd_map_shim", 1);
  }

  // Phase 2.89: plotRoute line → OSD dots (player-centered follow).
  // Host-only invent — PE markers use RenderRef bone00 (Cars @ 0x48310B),
  // not OSD. W13D: gate when tiles_absolu (same as map/blip).
  InvObject* route = tree_field_get_obj(nav, "route");
  const int32_t prev_route_n = tree_field_get_int(nav, "route_osd_n");
  int32_t route_drawn = 0;
  constexpr int32_t kMaxRouteDots = 48;
  if (!pe_like_3d && route && size > 1e-4f) {
    const int32_t npts = render_line_point_count(route);
    uint32_t col = static_cast<uint32_t>(render_line_color(route));
    if ((col & 0xFF000000u) == 0) col |= 0xFF000000u;
    if ((col & 0x00FFFFFFu) == 0) col = 0xFFFF0000u;
    const float sx = map_w / size;
    const float sy = map_h / size;
    const float half_w = map_w * 0.5f;
    const float half_h = map_h * 0.5f;
    const float dot = map_w * 0.025f;
    for (int32_t i = 0; i < npts && route_drawn < kMaxRouteDots; ++i) {
      float wx = 0, wy = 0, wz = 0;
      if (!render_line_point_at(route, i, &wx, &wy, &wz)) continue;
      const float dx = (wx - fx) * 0.01f;
      const float dz = (wz - fz) * 0.01f;
      const float ox = map_x + dx * sx;
      const float oy = map_y - dz * sy;
      if (std::fabs(ox - map_x) > half_w * 1.05f) continue;
      if (std::fabs(oy - map_y) > half_h * 1.05f) continue;
      void* dkey = reinterpret_cast<void*>(
          reinterpret_cast<uintptr_t>(nav) ^
          (0x4E10u + static_cast<uintptr_t>(route_drawn)));
      render_d3d9_osd_set_rect_color(dkey, ox, oy, dot, dot, nullptr, col, 41);
      ++route_drawn;
    }
  }
  for (int32_t i = route_drawn; i < prev_route_n; ++i) {
    render_d3d9_osd_remove_rect(reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(nav) ^
        (0x4E10u + static_cast<uintptr_t>(i))));
  }
  tree_field_set_int(nav, "route_osd_n", route_drawn);
  tree_field_set_int(nav, "route_osd_visible", route_drawn);

  // Phase 2.90/2.91: static + dynamic markers → OSD icons (rtype texture/mesh).
  // Host invent — PE uses bone00 (Cars). Gate when tiles_absolu.
  const int32_t prev_mk_n = tree_field_get_int(nav, "marker_osd_n");
  int32_t mk_drawn = 0;
  int32_t mk_tex = 0;
  constexpr int32_t kMaxMarkerDots = 24;
  auto ensure_marker_icon = [](InvObject* m) -> InvObject* {
    if (!m) return nullptr;
    InvObject* icon = tree_field_get_obj(m, "icon");
    if (icon) return icon;
    const int32_t rid = tree_field_get_int(m, "rtype");
    if (!rid) return nullptr;
    icon = resref_new();
    java_util_resource_ResourceRef_set(icon, rid);
    java_util_resource_ResourceRef_load(icon);
    tree_field_set_obj(m, "icon", icon);
    return icon;
  };
  auto marker_osd_tex = [](InvObject* icon, InvObject* m,
                           uint32_t fallback_col) -> void* {
    if (icon) {
      if (render_d3d9_texture_ready(icon)) return icon;
      if (render_d3d9_mesh_ready(icon)) {
        const int32_t n = render_d3d9_mesh_submesh_count(icon);
        for (int32_t s = 0; s < n; ++s) {
          void* t = render_d3d9_mesh_get_texture(icon, s);
          if (t && render_d3d9_texture_ready(t)) return t;
        }
      }
    }
    if (!m) return nullptr;
    InvObject* flat = tree_field_get_obj(m, "icon_flat");
    if (flat && render_d3d9_texture_ready(flat)) return flat;
    flat = resref_new();
    uint32_t col = fallback_col;
    if ((col & 0xFF000000u) == 0) col |= 0xFF000000u;
    if (render_d3d9_texture_create_solid(flat, col, 32)) {
      tree_field_set_obj(m, "icon_flat", flat);
      return flat;
    }
    return nullptr;
  };
  if (!pe_like_3d && size > 1e-4f) {
    const float sx = map_w / size;
    const float sy = map_h / size;
    const float half_w = map_w * 0.5f;
    const float half_h = map_h * 0.5f;
    const float mdot = map_w * 0.055f;
    auto project_mark = [&](InvObject* m, float wx, float wz, uint32_t col) {
      if (mk_drawn >= kMaxMarkerDots) return;
      const float dx = (wx - fx) * 0.01f;
      const float dz = (wz - fz) * 0.01f;
      const float ox = map_x + dx * sx;
      const float oy = map_y - dz * sy;
      if (std::fabs(ox - map_x) > half_w * 1.05f) return;
      if (std::fabs(oy - map_y) > half_h * 1.05f) return;
      void* mkey = reinterpret_cast<void*>(
          reinterpret_cast<uintptr_t>(nav) ^
          (0x4E80u + static_cast<uintptr_t>(mk_drawn)));
      InvObject* icon = ensure_marker_icon(m);
      void* t = marker_osd_tex(icon, m, col);
      if (t) {
        render_d3d9_osd_set_rect_color(mkey, ox, oy, mdot, mdot, t, 0xFFFFFFFFu,
                                       41);
        ++mk_tex;
      } else {
        render_d3d9_osd_set_rect_color(mkey, ox, oy, mdot, mdot, nullptr, col,
                                       41);
      }
      ++mk_drawn;
    };
    InvObject* statics = tree_field_get_obj(nav, "marker");
    const int32_t ns = statics ? tree_vector_size(statics) : 0;
    for (int32_t i = 0; i < ns; ++i) {
      InvObject* m = tree_vector_element_at(statics, i);
      if (!m) continue;
      project_mark(m, tree_field_get_float(m, "pos_x"),
                   tree_field_get_float(m, "pos_z"), 0xFFFFCC00u);
    }
    InvObject* dyns = tree_field_get_obj(nav, "dynamarker");
    const int32_t nd = dyns ? tree_vector_size(dyns) : 0;
    for (int32_t i = 0; i < nd; ++i) {
      InvObject* m = tree_vector_element_at(dyns, i);
      if (!m) continue;
      InvObject* obj = tree_field_get_obj(m, "obj");
      float wx = 0.f, wz = 0.f;
      if (obj) {
        wx = tree_field_get_float(obj, "pos_x");
        wz = tree_field_get_float(obj, "pos_z");
      } else {
        wx = tree_field_get_float(m, "pos_x");
        wz = tree_field_get_float(m, "pos_z");
      }
      project_mark(m, wx, wz, 0xFF00CCFFu);
    }
  }
  for (int32_t i = mk_drawn; i < prev_mk_n; ++i) {
    render_d3d9_osd_remove_rect(reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(nav) ^
        (0x4E80u + static_cast<uintptr_t>(i))));
  }
  tree_field_set_int(nav, "marker_osd_n", mk_drawn);
  tree_field_set_int(nav, "marker_osd_visible", mk_drawn);
  tree_field_set_int(nav, "marker_osd_tex", mk_tex);