// Fragment 4/4 of main.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


  // Phase 2.121: display(dlg, limit) → SoftTimer → dialog.wait → hide → termSig.
  {
    InvObject* ls = frontend_loading_screen();
    for (int i = 0; i < 200 && tree_field_get_int(ls, "run_parked") == 0; ++i) {
      render_d3d9_flush();
      Sleep(10);
    }
    InvObject* dlg = tree_host_new("java.render.SimpleLoadingDialog");
    tree_field_set_int(dlg, "flags", 0x01 | 0x02 | 0x08 | 0x10);
    const int32_t gen0 = tree_field_get_int(ls, "hide_gen");
    // Same-thread block (pumps flush internally).
    frontend_loading_screen_display(ls, dlg, 0.05f);
    const int32_t gen1 = tree_field_get_int(ls, "hide_gen");
    for (int i = 0; i < 100 && tree_field_get_int(ls, "run_parked") == 0; ++i) {
      render_d3d9_flush();
      Sleep(5);
    }
    const int32_t vis = frontend_loading_screen_visible();
    const int32_t parked = tree_field_get_int(ls, "run_parked");
    const bool ld_ok = gen1 > gen0 && vis == 0 && parked == 1;
    std::printf("boot loading_dialog ok=%d hide_gen=%d->%d vis=%d park=%d\n",
                ld_ok ? 1 : 0, gen0, gen1, vis, parked);
    if (!ld_ok) {
      std::printf("FAIL LoadingScreen.display SoftTimer/termSig\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.122: FlashText blink during waitForUser display.
  {
    InvObject* ls = frontend_loading_screen();
    for (int i = 0; i < 100 && tree_field_get_int(ls, "run_parked") == 0; ++i) {
      render_d3d9_flush();
      Sleep(5);
    }
    InvObject* dlg = tree_host_new("java.render.SimpleLoadingDialog");
    tree_field_set_int(dlg, "flags", 0x01 | 0x02 | 0x08 | 0x10);
    // Long enough for at least one on→off flash cycle (600+600ms).
    frontend_loading_screen_display(ls, dlg, 1.3f);
    InvObject* runner = tree_field_get_obj(ls, "flashTextRunner");
    const int32_t ticks = runner ? tree_field_get_int(runner, "flash_ticks") : 0;
    InvObject* txt = tree_field_get_obj(dlg, "loadingText");
    const char* last = txt ? render_d3d9_text_get_string(txt) : "";
    const bool flash_ok = ticks >= 1;
    std::printf("boot flash_text ok=%d ticks=%d last='%s'\n", flash_ok ? 1 : 0,
                ticks, last ? last : "");
    if (!flash_ok) {
      std::printf("FAIL FlashText blink during LoadingDialog\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Input
  input_set_axis(0, 1, 0.75f);
  input_set_last_key(42);
  InvObject* dev0 = java_io_Input_getDeviceName(0);
  InvObject* dev1 = java_io_Input_getDeviceName(1);
  InvObject* dev2 = java_io_Input_getDeviceName(2);
  InvObject* an_esc = java_io_Input_axisName(0, 1);
  InvObject* an_mx = java_io_Input_axisName(1, 0);
  const char* d0 = string_cstr(dev0);
  const char* d1 = string_cstr(dev1);
  const char* ae = string_cstr(an_esc);
  const char* mx = string_cstr(an_mx);
  const bool devices_ok =
      d0 && std::strcmp(d0, "SysKeyboard") == 0 && d1 &&
      std::strcmp(d1, "SysMouse") == 0 && !dev2 && ae &&
      std::strcmp(ae, "Escape") == 0 && mx && std::strcmp(mx, "Mouse X") == 0;
  // PE getAxis: kb digital 0/1, oob/unknown device → 0.
  const float g_esc = java_io_Input_getAxis(0, 1);
  const float g_oob = java_io_Input_getAxis(0, 256);
  const float g_ndev = java_io_Input_getAxis(2, 0);
  const float g_neg = java_io_Input_getAxis(0, -1);
  const bool getaxis_ok =
      g_esc > 0.5f && g_oob == 0.f && g_ndev == 0.f && g_neg == 0.f;
  std::printf("axis=%.2f lastKey=%d dev='%s'/'%s' end=%d esc='%s' mx='%s'\n",
              g_esc, java_io_Input_lastKey(),
              d0 ? d0 : "?", d1 ? d1 : "?", dev2 ? 1 : 0, ae ? ae : "?",
              mx ? mx : "?");
  if (!devices_ok || !getaxis_ok) {
    std::printf("FAIL Input device table\n");
    if (want_window) render_d3d9_close();
    return 2;
  }

  // Object
  InvObject* o = string_new("x");
  std::printf("hash=%d toString='%s'\n", java_lang_Object_hashCode(o),
              string_cstr(java_lang_Object_toString(o)));

  const int32_t slen = java_lang_String_length(sv);
  std::printf("check iv=%d fv=%.9f slen=%d cstr_len=%zu\n", iv, fv, slen,
              std::strlen(string_cstr(sv)));
  // GameRef
  InvObject* gr = gameref_new();
  std::printf("gameref empty=%d\n", java_util_resource_GameRef_isEmpty(gr));
  java_util_resource_GameRef_setPos(gr, vec3_new(10.f, 20.f, 30.f));
  java_util_resource_GameRef_setFlags(gr, 0x11);
  InvObject* gp = java_util_resource_GameRef_getPos(gr);
  float gx, gy, gz;
  vec3_get(gp, &gx, &gy, &gz);
  std::printf("gameref pos=%.0f,%.0f,%.0f flags=%d empty=%d\n", gx, gy, gz,
              java_util_resource_GameRef_getFlags(gr),
              java_util_resource_GameRef_isEmpty(gr));

  // Phase 2.55: create params "px,py,pz,oy,op,or" → instance pose.
  InvObject* parent_gr = gameref_new();
  InvObject* type_stub = resref_new();
  java_util_resource_ResourceRef_set(type_stub, 0);
  InvObject* xa = gameref_new();
  InvObject* inst = java_util_resource_GameRef_create(
      xa, parent_gr, type_stub, string_new("1.5, 2.0, -3.25, 0.5, -0.25, 1.0"),
      string_new("gameinstance_smoke"));
  float ipx = 0, ipy = 0, ipz = 0, ioy = 0, iop = 0, ior = 0;
  if (inst) {
    vec3_get(java_util_resource_GameRef_getPos(inst), &ipx, &ipy, &ipz);
    ypr_get(java_util_resource_GameRef_getOri(inst), &ioy, &iop, &ior);
  }
  const bool inst_ok =
      inst && std::fabs(ipx - 1.5f) < 1e-3f && std::fabs(ipy - 2.0f) < 1e-3f &&
      std::fabs(ipz + 3.25f) < 1e-3f && std::fabs(ioy - 0.5f) < 1e-3f &&
      std::fabs(iop + 0.25f) < 1e-3f && std::fabs(ior - 1.0f) < 1e-3f;
  std::printf("gameref create_pose ok=%d pos=(%.2f,%.2f,%.2f) "
              "ori=(%.2f,%.2f,%.2f)\n",
              inst_ok ? 1 : 0, ipx, ipy, ipz, ioy, iop, ior);
  if (!inst_ok) {
    std::printf("FAIL GameRef.create_pose\n");
    if (want_window) render_d3d9_close();
    return 2;
  }

  // Phase 2.96: GameRef.getInfo — GII_ID / TYPE / CATEGORY / CAR_DRIVETYPE.
  {
    constexpr int32_t kGiiId = 5;
    constexpr int32_t kGiiType = 6;
    constexpr int32_t kGiiCategory = 7;
    constexpr int32_t kGiiCarDrivetype = 52;
    constexpr int32_t kTypeId = 0x00B0157;
    InvObject* car = tree_host_new("java.game.Vehicle");
    InvObject* type = resref_new();
    java_util_resource_ResourceRef_set(type, kTypeId);
    java_util_resource_ResourceRef_set(car, kTypeId);
    java_util_resource_RenderRef_setType(car, type);
    InvObject* chassis = tree_host_new("java.game.parts.bodypart.Chassis");
    tree_field_set_obj(car, "chassis", chassis);
    tree_field_set_int(chassis, "drive_type", 2);  // DT_RWD
    const int32_t gi_id = java_util_resource_GameRef_getInfo(car, kGiiId, 0);
    const int32_t gi_type =
        java_util_resource_GameRef_getInfo(car, kGiiType, 0);
    const int32_t gi_cat =
        java_util_resource_GameRef_getInfo(car, kGiiCategory, 0);
    const int32_t gi_rwd =
        java_util_resource_GameRef_getInfo(car, kGiiCarDrivetype, 0);
    tree_field_set_int(chassis, "drive_type", 1);  // DT_FWD
    const int32_t gi_fwd =
        java_util_resource_GameRef_getInfo(car, kGiiCarDrivetype, 0);
    tree_field_set_int(chassis, "drive_type", 3);  // FWD|RWD
    const int32_t gi_awd =
        java_util_resource_GameRef_getInfo(car, kGiiCarDrivetype, 0);
    tree_field_set_int(chassis, "drive_type", 0);
    const int32_t gi_none =
        java_util_resource_GameRef_getInfo(car, kGiiCarDrivetype, 0);
    const bool getinfo_ok = gi_id == kTypeId && gi_type == kTypeId &&
                            gi_cat == 5 && gi_rwd == 3 && gi_fwd == 2 &&
                            gi_awd == 1 && gi_none == 0;
    std::printf("gameref getInfo ok=%d id=0x%X type=0x%X cat=%d "
                "drv=%d/%d/%d/%d\n",
                getinfo_ok ? 1 : 0, gi_id, gi_type, gi_cat, gi_none, gi_fwd,
                gi_rwd, gi_awd);
    if (!getinfo_ok) {
      std::printf("FAIL GameRef.getInfo\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.97: getInfo(String) — GII_COMPATIBLE / GII_INSTALL_OK (Catalog).
  {
    constexpr int32_t kGiiInstallOk = 71;
    constexpr int32_t kGiiCompatible = 72;
    // Synthetic ids (avoid colliding with open RPK entries in find_by_id).
    constexpr int32_t kCarId = 0x7E010001;
    constexpr int32_t kPartId = 0x7E010099;
    constexpr int32_t kOtherPack = 0x7EAA0099;
    InvObject* car = tree_host_new("java.game.Vehicle");
    InvObject* chassis = tree_host_new("java.game.parts.bodypart.Chassis");
    InvObject* part = gameref_new();
    InvObject* alien = gameref_new();
    InvObject* type_car = resref_new();
    InvObject* type_part = resref_new();
    java_util_resource_ResourceRef_set(type_car, kCarId);
    java_util_resource_ResourceRef_set(type_part, kPartId);
    java_util_resource_ResourceRef_set(car, kCarId);
    java_util_resource_RenderRef_setType(car, type_car);
    java_util_resource_ResourceRef_set(part, kPartId);
    java_util_resource_RenderRef_setType(part, type_part);
    // Drop type-stub ids so resref_find_by_id resolves to the instances.
    java_util_resource_ResourceRef_set(type_car, 0);
    java_util_resource_ResourceRef_set(type_part, 0);
    java_util_resource_ResourceRef_set(alien, kOtherPack);
    tree_field_set_obj(car, "chassis", chassis);
    // Free wheel slot on chassis.
    part_disable_slot(chassis, 101, 0);
    char car_id_s[32];
    std::snprintf(car_id_s, sizeof(car_id_s), "%d", kCarId);
    InvObject* car_s = string_new(car_id_s);
    const int32_t comp =
        java_util_resource_GameRef_getInfo_1(part, kGiiCompatible, car_s);
    const int32_t ok1 =
        java_util_resource_GameRef_getInfo_1(part, kGiiInstallOk, car_s);
    // Fill the only free slot → INSTALL_OK fails.
    InvObject* filler = gameref_new();
    java_util_resource_ResourceRef_set(filler, 0x7E0100AA);
    part_install(chassis, 101, filler, 1);
    part_disable_slot(chassis, 1, 1);
    const int32_t ok0 =
        java_util_resource_GameRef_getInfo_1(part, kGiiInstallOk, car_s);
    // Different pack, no vehicle → not compatible.
    char alien_s[32];
    std::snprintf(alien_s, sizeof(alien_s), "%d", kOtherPack);
    const int32_t comp0 = java_util_resource_GameRef_getInfo_1(
        part, kGiiCompatible, string_new(alien_s));
    // Same-pack bare part accepts INSTALL_OK (inventory mate).
    InvObject* mate = gameref_new();
    java_util_resource_ResourceRef_set(mate, 0x7E0100BB);
    char mate_s[32];
    std::snprintf(mate_s, sizeof(mate_s), "%d", 0x7E0100BB);
    const int32_t ok_mate = java_util_resource_GameRef_getInfo_1(
        part, kGiiInstallOk, string_new(mate_s));
    const bool gi1_ok =
        comp == 1 && ok1 == 1 && ok0 == 0 && comp0 == 0 && ok_mate == 1;
    std::printf("gameref getInfo_str ok=%d comp=%d install=%d->%d "
                "alien=%d mate=%d\n",
                gi1_ok ? 1 : 0, comp, ok1, ok0, comp0, ok_mate);
    if (!gi1_ok) {
      std::printf("FAIL GameRef.getInfo_str\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.98: GII_REMOVE_OK / PART_CATEGORY / OWNER (garage / Mechanic).
  {
    constexpr int32_t kGiiOwner = 24;
    constexpr int32_t kGiiRemoveOk = 41;
    constexpr int32_t kGiiPartCategory = 55;
    constexpr int32_t kParentId = 0x7E020001;
    constexpr int32_t kPartId = 0x7E020099;
    InvObject* parent = gameref_new();
    InvObject* part = tree_host_new(
        "java.game.parts.enginepart.Block");  // engine → cat 1
    InvObject* body = tree_host_new(
        "java.game.parts.bodypart.Bumper");  // body → cat 2
    InvObject* chassis =
        tree_host_new("java.game.parts.bodypart.Chassis");
    java_util_resource_ResourceRef_set(parent, kParentId);
    java_util_resource_ResourceRef_set(part, kPartId);
    java_util_resource_GameRef_setParent(part, parent);
    const int32_t owner =
        java_util_resource_GameRef_getInfo(part, kGiiOwner, 0);
    const int32_t rem0 =
        java_util_resource_GameRef_getInfo(part, kGiiRemoveOk, 0);
    // Attach a child → remove blocked.
    InvObject* child = gameref_new();
    java_util_resource_ResourceRef_set(child, 0x7E0200AA);
    part_install(part, 2, child, 1);
    const int32_t rem1 =
        java_util_resource_GameRef_getInfo(part, kGiiRemoveOk, 0);
    const int32_t rem_ch =
        java_util_resource_GameRef_getInfo(chassis, kGiiRemoveOk, 0);
    const int32_t cat_eng =
        java_util_resource_GameRef_getInfo(part, kGiiPartCategory, 0);
    const int32_t cat_body =
        java_util_resource_GameRef_getInfo(body, kGiiPartCategory, 0);
    tree_field_set_int(body, "part_category", 3);
    const int32_t cat_ov =
        java_util_resource_GameRef_getInfo(body, kGiiPartCategory, 0);
    const bool gi2_ok = owner == kParentId && rem0 == 0 && rem1 == -1 &&
                        rem_ch == -1 && cat_eng == 1 && cat_body == 2 &&
                        cat_ov == 3;
    std::printf("gameref getInfo_mech ok=%d owner=0x%X rem=%d->%d chassis=%d "
                "cat=%d/%d/%d\n",
                gi2_ok ? 1 : 0, owner, rem0, rem1, rem_ch, cat_eng, cat_body,
                cat_ov);
    if (!gi2_ok) {
      std::printf("FAIL GameRef.getInfo_mech\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.99: GII_SIZE / BONE / RENDER / CAMERA (inventory + Track).
  {
    constexpr int32_t kGiiBone = 1;
    constexpr int32_t kGiiSize = 11;
    constexpr int32_t kGiiCamera = 34;
    constexpr int32_t kGiiRender = 48;
    constexpr int32_t kId = 0x7E030001;
    InvObject* car = tree_host_new("java.game.Vehicle");
    InvObject* part = gameref_new();
    InvObject* chassis =
        tree_host_new("java.game.parts.bodypart.Chassis");
    java_util_resource_ResourceRef_set(car, kId);
    java_util_resource_ResourceRef_set(part, 0x7E030099);
    tree_field_set_obj(car, "chassis", chassis);
    tree_field_set_int(part, "gii_size", 160);
    tree_field_set_int(car, "gii_bone", 0x7E0300B0);
    tree_field_set_int(car, "camera_count", 3);
    tree_field_set_int(car, "gii_camera", 0x7E0300C0);
    const int32_t sz =
        java_util_resource_GameRef_getInfo(part, kGiiSize, 0);
    const int32_t sz_def =
        java_util_resource_GameRef_getInfo(gameref_new(), kGiiSize, 0);
    const int32_t bone =
        java_util_resource_GameRef_getInfo(car, kGiiBone, 0);
    const int32_t bone_self =
        java_util_resource_GameRef_getInfo(part, kGiiBone, 0);
    const int32_t cams =
        java_util_resource_GameRef_getInfo(car, kGiiRender, 0);
    const int32_t cams0 =
        java_util_resource_GameRef_getInfo(part, kGiiRender, 0);
    const int32_t cam_id =
        java_util_resource_GameRef_getInfo(car, kGiiCamera, 0);
    const bool gi3_ok = sz == 160 && sz_def == 100 && bone == 0x7E0300B0 &&
                        bone_self == 0x7E030099 && cams == 3 && cams0 == 0 &&
                        cam_id == 0x7E0300C0;
    std::printf("gameref getInfo_cam ok=%d size=%d/%d bone=0x%X/0x%X "
                "cams=%d/%d cam=0x%X\n",
                gi3_ok ? 1 : 0, sz, sz_def, bone, bone_self, cams, cams0,
                cam_id);
    if (!gi3_ok) {
      std::printf("FAIL GameRef.getInfo_cam\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.100: GII_AXIS (Input.getInput) + GII_CAR_TRAFFICPTR (City).
  {
    constexpr int32_t kGiiAxis = 25;
    constexpr int32_t kGiiCarTrafficPtr = 56;
    InvObject* ctrl = gameref_new();
    java_io_Controller_user_SetAxisForce(ctrl, kAxisSelect, 0.f, 0.f);
    const int32_t a0 =
        java_util_resource_GameRef_getInfo(ctrl, kGiiAxis, kAxisSelect);
    java_io_Controller_user_SetAxisForce(ctrl, kAxisSelect, 0.f, 1.f);
    const int32_t a1 =
        java_util_resource_GameRef_getInfo(ctrl, kGiiAxis, kAxisSelect);
    java_io_Controller_user_SetAxisForce(ctrl, kAxisThrottle, 0.f, 0.42f);
    const int32_t ath =
        java_util_resource_GameRef_getInfo(ctrl, kGiiAxis, kAxisThrottle);
    java_io_Controller_user_SetAxisForce(ctrl, kAxisSelect, 0.f, 0.f);
    java_io_Controller_user_SetAxisForce(ctrl, kAxisThrottle, 0.f, 0.f);
    InvObject* tcar = gameref_new();
    tree_field_set_int(tcar, "traffic_ptr", 0x55AA1234);
    const int32_t tptr =
        java_util_resource_GameRef_getInfo(tcar, kGiiCarTrafficPtr, 0);
    const bool gi4_ok =
        a0 == 0 && a1 >= 900 && ath >= 400 && ath <= 450 && tptr == 0x55AA1234;
    std::printf("gameref getInfo_axis ok=%d sel=%d/%d thr=%d traffic=0x%X\n",
                gi4_ok ? 1 : 0, a0, a1, ath, tptr);
    if (!gi4_ok) {
      std::printf("FAIL GameRef.getInfo_axis\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.101: hasCrime speed limit + wakeup/start/stop/reset commands.
  {
    constexpr int32_t kEventCommand = 0x10;
    InvObject* car = tree_host_new("java.game.Vehicle");
    java_util_resource_ResourceRef_set(car, 0x7E040001);
    InvObject* body = resref_new();
    java_util_resource_PhysicsRef_createBox(body, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    physics_set_velocity(body, 5.f, 0.f, 5.f);
    physics_set_asleep(body, 1);
    const int32_t asleep1 = physics_is_asleep(body);
    java_util_resource_GameRef_queueEvent(body, nullptr, kEventCommand,
                                          string_new("wakeup"));
    const int32_t asleep0 = physics_is_asleep(body);
    const float crime_def = java_game_Vehicle_hasCrime(car);
    tree_field_set_float(car, "crime_speed", 20.f);
    const float crime_set = java_game_Vehicle_hasCrime(car);
    java_util_resource_GameRef_queueEvent(body, nullptr, kEventCommand,
                                          string_new("stop"));
    InvObject* held = gameref_new();
    java_util_resource_GameRef_queueEvent(held, nullptr, kEventCommand,
                                          string_new("stop"));
    const int32_t held1 = tree_field_get_int(held, "drive_held");
    java_util_resource_GameRef_queueEvent(held, nullptr, kEventCommand,
                                          string_new("start"));
    const int32_t held0 = tree_field_get_int(held, "drive_held");
    java_util_resource_GameRef_queueEvent(body, nullptr, kEventCommand,
                                          string_new("reset"));
    const int32_t resets = tree_field_get_int(body, "reset_count");
    // Confirm stop cleared velocity on body.
    physics_set_velocity(body, 3.f, 0.f, 4.f);
    java_util_resource_GameRef_queueEvent(body, nullptr, kEventCommand,
                                          string_new("stop"));
    const float spd2 = physics_speed_square(body);
    const bool cmd_ok =
        asleep1 == 1 && asleep0 == 0 && crime_def == -1.f &&
        crime_set == 20.f && held1 == 1 && held0 == 0 && resets >= 1 &&
        spd2 < 0.01f;
    std::printf("boot vehicle_cmd ok=%d wake=%d->%d crime=%.2f/%.0f "
                "hold=%d->%d reset=%d spd2=%.3f\n",
                cmd_ok ? 1 : 0, asleep1, asleep0, crime_def, crime_set, held1,
                held0, resets, spd2);
    if (!cmd_ok) {
      std::printf("FAIL Vehicle.cmd/hasCrime\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.102: transmission / steerhelp / asr / abs / cruise / setsteer.
  {
    constexpr int32_t kEventCommand = 0x10;
    InvObject* car = gameref_new();
    java_util_resource_GameRef_queueEvent(
        car, nullptr, kEventCommand, string_new("transmission 1"));
    java_util_resource_GameRef_queueEvent(
        car, nullptr, kEventCommand, string_new("steerhelp 0.75"));
    java_util_resource_GameRef_queueEvent(car, nullptr, kEventCommand,
                                          string_new("asr 0.5"));
    java_util_resource_GameRef_queueEvent(car, nullptr, kEventCommand,
                                          string_new("abs 0.8"));
    java_util_resource_GameRef_queueEvent(car, nullptr, kEventCommand,
                                          string_new("difflock 1"));
    java_util_resource_GameRef_queueEvent(car, nullptr, kEventCommand,
                                          string_new("cruise 1"));
    java_util_resource_GameRef_queueEvent(
        car, nullptr, kEventCommand, string_new("damage_multiplier 1.5"));
    java_util_resource_GameRef_queueEvent(car, nullptr, kEventCommand,
                                          string_new("setsteer -0.35"));
    const int32_t tr = tree_field_get_int(car, "transmission");
    const float sh = tree_field_get_float(car, "steerhelp");
    const float asr = tree_field_get_float(car, "asr");
    const float absv = tree_field_get_float(car, "abs");
    const float diff = tree_field_get_float(car, "difflock");
    const int32_t cc = tree_field_get_int(car, "cruise");
    const float dmg = tree_field_get_float(car, "damage_multiplier");
    const float st = tree_field_get_float(car, "setsteer");
    const bool assist_ok =
        tr == 1 && std::fabs(sh - 0.75f) < 1e-3f &&
        std::fabs(asr - 0.5f) < 1e-3f && std::fabs(absv - 0.8f) < 1e-3f &&
        std::fabs(diff - 1.f) < 1e-3f && cc == 1 &&
        std::fabs(dmg - 1.5f) < 1e-3f && std::fabs(st + 0.35f) < 1e-3f;
    std::printf("boot vehicle_assist ok=%d tr=%d sh=%.2f asr=%.2f abs=%.2f "
                "diff=%.1f cruise=%d dmg=%.1f steer=%.2f\n",
                assist_ok ? 1 : 0, tr, sh, asr, absv, diff, cc, dmg, st);
    if (!assist_ok) {
      std::printf("FAIL Vehicle.assist cmds\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.103: filter cmd + ABS grip assist under braking.
  {
    constexpr int32_t kEventCommand = 0x10;
    InvObject* car = gameref_new();
    java_util_resource_GameRef_queueEvent(car, nullptr, kEventCommand,
                                          string_new("filter 1 1"));
    java_util_resource_GameRef_queueEvent(car, nullptr, kEventCommand,
                                          string_new("filter 2 0"));
    java_util_resource_GameRef_queueEvent(car, nullptr, kEventCommand,
                                          string_new("filter 3 2"));
    const int32_t f1 = tree_field_get_int(car, "filter_1");
    const int32_t f2 = tree_field_get_int(car, "filter_2");
    const int32_t f3 = tree_field_get_int(car, "filter_3");
    InvObject* noabs = resref_new();
    InvObject* withabs = resref_new();
    java_util_resource_PhysicsRef_createBox(noabs, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(withabs, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(noabs, vec3_new(0.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(withabs, vec3_new(10.f, 0.5f, 0.f),
                                            nullptr);
    physics_set_wheel_params(noabs, 0.f, 1.f, 0.32f);
    physics_set_wheel_params(withabs, 0.f, 1.f, 0.32f);
    // Low base grip + wheel brake; ABS boost should cut leftover |vx|.
    physics_set_wheel_contact(noabs, 0.25f, 0.25f, 1.f, 0.f, 0.f);
    physics_set_wheel_contact(withabs, 0.25f, 0.25f, 1.f, 0.f, 0.f);
    physics_set_velocity(noabs, 12.f, 0.f, 20.f);
    physics_set_velocity(withabs, 12.f, 0.f, 20.f);
    tree_field_set_float(withabs, "abs", 1.f);
    for (int i = 0; i < 24; ++i) {
      physics_drive(noabs, nullptr, 0.05f);
      physics_drive(withabs, nullptr, 0.05f);
    }
    float nvx = 0, nvy = 0, nvz = 0, avx = 0, avy = 0, avz = 0;
    vec3_get(java_util_resource_PhysicsRef_getVel(noabs), &nvx, &nvy, &nvz);
    vec3_get(java_util_resource_PhysicsRef_getVel(withabs), &avx, &avy, &avz);
    java_util_resource_PhysicsRef_setStatic(noabs, 1);
    java_util_resource_PhysicsRef_setStatic(withabs, 1);
    const bool filt_ok = f1 == 1 && f2 == 0 && f3 == 2;
    const bool abs_ok = std::fabs(avx) + 0.08f < std::fabs(nvx);
    const bool f103_ok = filt_ok && abs_ok;
    std::printf("boot vehicle_filter ok=%d f=%d/%d/%d abs_vx=%.2f<%.2f\n",
                f103_ok ? 1 : 0, f1, f2, f3, std::fabs(avx), std::fabs(nvx));
    if (!f103_ok) {
      std::printf("FAIL Vehicle.filter/ABS\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.104: Input.createHotkey / checkHotkeys / flush / delete.
  {
    constexpr int32_t kAxisCancel = 35;
    constexpr int32_t kVirtual = 1;
    constexpr int32_t kCmdExit = 42;
    InvObject* ctrl = gameref_new();
    InvObject* osd = tree_host_new("java.render.Osd");
    tree_field_set_obj(osd, "hotkey", tree_vector_new());
    InvObject* handler = gameref_new();
    InvObject* hk = tree_host_new("java.io.Hotkey");
    tree_field_set_int(hk, "command", kCmdExit);
    tree_field_set_obj(hk, "handler", handler);
    // PE Engine_queueEvent_dispatch: eventMask(+0x70) & EVENT_HOTKEY.
    java_lang_GameType_setEventMask(handler, 0x00100000);
    java_io_Input_createHotkey(kAxisCancel, kVirtual, hk, handler, osd, 1);
    java_io_Controller_user_SetAxisForce(ctrl, kAxisCancel, 0.f, 0.f);
    java_io_Input_checkHotkeys(ctrl, osd);  // arm edge (flush sentinel)
    java_io_Controller_user_SetAxisForce(ctrl, kAxisCancel, 0.f, 1.f);
    java_io_Input_checkHotkeys(ctrl, osd);  // rising → fire
    const int32_t cmd0 = tree_field_get_int(handler, "last_osd_cmd");
    const int32_t n0 = tree_field_get_int(handler, "osd_cmd_count");
    // Held: no re-fire.
    java_io_Input_checkHotkeys(ctrl, osd);
    const int32_t n1 = tree_field_get_int(handler, "osd_cmd_count");
    // Flush while held → no press until release+press.
    java_io_Input_flushHotkeys();
    java_io_Input_checkHotkeys(ctrl, osd);
    java_io_Input_checkHotkeys(ctrl, osd);
    const int32_t n2 = tree_field_get_int(handler, "osd_cmd_count");
    java_io_Controller_user_SetAxisForce(ctrl, kAxisCancel, 0.f, 0.f);
    java_io_Input_checkHotkeys(ctrl, osd);  // release
    java_io_Controller_user_SetAxisForce(ctrl, kAxisCancel, 0.f, 1.f);
    java_io_Input_checkHotkeys(ctrl, osd);  // press again
    const int32_t n3 = tree_field_get_int(handler, "osd_cmd_count");
    // Wrong OSD focus → no fire.
    InvObject* other = tree_host_new("java.render.Osd");
    java_io_Controller_user_SetAxisForce(ctrl, kAxisCancel, 0.f, 0.f);
    java_io_Input_checkHotkeys(ctrl, osd);
    java_io_Controller_user_SetAxisForce(ctrl, kAxisCancel, 0.f, 1.f);
    java_io_Input_checkHotkeys(ctrl, other);
    const int32_t n4 = tree_field_get_int(handler, "osd_cmd_count");
    java_io_Input_deleteHotkey(hk);
    java_io_Controller_user_SetAxisForce(ctrl, kAxisCancel, 0.f, 0.f);
    java_io_Input_checkHotkeys(ctrl, osd);
    java_io_Controller_user_SetAxisForce(ctrl, kAxisCancel, 0.f, 1.f);
    java_io_Input_checkHotkeys(ctrl, osd);
    const int32_t n5 = tree_field_get_int(handler, "osd_cmd_count");
    java_io_Controller_user_SetAxisForce(ctrl, kAxisCancel, 0.f, 0.f);
    const bool hk_ok = cmd0 == kCmdExit && n0 == 1 && n1 == 1 && n2 == 1 &&
                       n3 == 2 && n4 == 2 && n5 == 2 &&
                       tree_field_get_int(hk, "active") == 0;
    std::printf("boot input_hotkey ok=%d cmd=%d n=%d/%d/%d/%d/%d/%d\n",
                hk_ok ? 1 : 0, cmd0, n0, n1, n2, n3, n4, n5);
    if (!hk_ok) {
      std::printf("FAIL Input.hotkeys\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.105: RenderRef.setLight/setFlare/changeResource + ResourceRef.cache.
  {
    InvObject* sun = resref_new();
    java_util_resource_RenderRef_setLight(sun, 0x00466285, 0x0007121e,
                                          0x00466285);
    const int32_t ld = tree_field_get_int(sun, "light_diffuse");
    const int32_t la = tree_field_get_int(sun, "light_ambient");
    const bool light_ok = ld == 0x00466285 && la == 0x0007121e &&
                          render_d3d9_light_enabled() &&
                          render_d3d9_light_diffuse() == 0x00466285 &&
                          render_d3d9_light_ambient() == 0x0007121e;

    InvObject* glow = resref_new();
    java_util_resource_ResourceRef_set(glow, 0x7E105001);
    render_d3d9_texture_create_solid(glow, 0xFFE4E4E4u, 16);
    java_util_resource_RenderRef_setFlare(sun, glow, 0xe4e4e4FF, 1.f, 10.f, 15,
                                          8);
    const bool flare_ok =
        tree_field_get_int(sun, "flare_count") == 15 &&
        tree_field_get_int(sun, "flare_rays") == 8 &&
        std::fabs(tree_field_get_float(sun, "flare_max") - 10.f) < 0.01f &&
        tree_field_get_int(sun, "flare_tex_id") == 0x7E105001;

    InvObject* mesh = resref_new();
    InvObject* tex_old = resref_new();
    InvObject* tex_new = resref_new();
    java_util_resource_ResourceRef_set(tex_old, 0x7E105010);
    java_util_resource_ResourceRef_set(tex_new, 0x7E105011);
    render_d3d9_mesh_create_skydome(mesh, 10.f);
    render_d3d9_texture_create_solid(tex_old, 0xFFFF0000u, 8);
    render_d3d9_texture_create_solid(tex_new, 0xFF00FF00u, 8);
    render_d3d9_mesh_set_texture(mesh, tex_old);
    java_util_resource_RenderRef_changeResource(mesh, tex_old, tex_new);
    const bool swap_ok =
        render_d3d9_mesh_get_texture(mesh, 0) == tex_new &&
        tree_field_get_int(mesh, "swapped_tex_id") == 0x7E105011;

    InvObject* cached = resref_new();
    java_util_resource_ResourceRef_set(cached, 0x7E105020);
    render_d3d9_texture_create_solid(cached, 0xFF808080u, 4);
    java_util_resource_ResourceRef_cache(cached);
    const bool cache_ok = tree_field_get_int(cached, "cached") == 1 &&
                          render_d3d9_texture_ready(cached);

    render_d3d9_mesh_destroy(mesh);
    render_d3d9_texture_destroy(tex_old);
    render_d3d9_texture_destroy(tex_new);
    render_d3d9_texture_destroy(glow);
    render_d3d9_texture_destroy(cached);
    render_d3d9_clear_light();

    const bool r105_ok = light_ok && flare_ok && swap_ok && cache_ok;
    std::printf("boot render_light ok=%d light=%d flare=%d swap=%d cache=%d\n",
                r105_ok ? 1 : 0, light_ok ? 1 : 0, flare_ok ? 1 : 0,
                swap_ok ? 1 : 0, cache_ok ? 1 : 0);
    if (!r105_ok) {
      std::printf("FAIL RenderRef.light/flare/swap/cache\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.106: Object.wait / notify / notifyAll + GC enable/disable.
  {
    InvObject* sig = tree_host_new("java.lang.Object");
    // Sticky notify before wait (GameLogic setTimerText race).
    java_lang_Object_notify(sig);
    java_lang_Object_wait(sig);
    const int32_t wake0 = tree_field_get_int(sig, "wake_count");

    std::atomic<int> stage{0};
    std::thread waiter([&] {
      stage.store(1);
      java_lang_Object_wait(sig);
      stage.store(2);
    });
    for (int i = 0; i < 200 && stage.load() != 1; ++i) Sleep(1);
    const int before = stage.load();
    Sleep(30);
    const int still = stage.load();
    java_lang_Object_notify(sig);
    waiter.join();
    const int after = stage.load();
    const int32_t wake1 = tree_field_get_int(sig, "wake_count");

    InvObject* sig2 = tree_host_new("java.lang.Object");
    std::atomic<int> w0{0}, w1{0};
    std::thread t0([&] {
      java_lang_Object_wait(sig2);
      w0.store(1);
    });
    std::thread t1([&] {
      java_lang_Object_wait(sig2);
      w1.store(1);
    });
    Sleep(40);
    java_lang_Object_notifyAll(sig2);
    t0.join();
    t1.join();

    java_lang_Object_disableGC(sig);
    const int32_t gcd1 = tree_field_get_int(sig, "gc_disabled");
    java_lang_Object_enableGC(sig);
    const int32_t gcd0 = tree_field_get_int(sig, "gc_disabled");

    const bool obj_ok = wake0 == 1 && before == 1 && still == 1 && after == 2 &&
                        wake1 == 2 && w0.load() == 1 && w1.load() == 1 &&
                        gcd1 == 1 && gcd0 == 0;
    std::printf("boot object_monitor ok=%d wake=%d/%d block=%d->%d "
                "all=%d/%d gc=%d->%d\n",
                obj_ok ? 1 : 0, wake0, wake1, still, after, w0.load(),
                w1.load(), gcd1, gcd0);
    if (!obj_ok) {
      std::printf("FAIL Object.wait/notify/GC\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.107: Input.setAxisSmooth / activeAxis + Controller smooth.
  {
    constexpr int32_t kDikUp = 0xc8;
    InvObject* raw = gameref_new();
    InvObject* sm = gameref_new();
    java_io_Input_mapAxis(raw, kAxisThrottle, 0, kDikUp, 0.f, 1.f, 0.f, 1.f);
    java_io_Input_mapAxis(sm, kAxisThrottle, 0, kDikUp, 0.f, 1.f, 0.f, 1.f);
    // Slow ramp: 1.0 unit/sec toward target.
    java_io_Input_setAxisSmooth(sm, kAxisThrottle, 0.1f, 2.f, 4.f, 1.f);
    input_set_axis(0, kDikUp, 1.f);
    const float v_raw0 = java_io_Controller_user_GetAxisVal(raw, kAxisThrottle);
    const float v_sm0 = java_io_Controller_user_GetAxisVal(sm, kAxisThrottle);
    Sleep(120);
    input_set_axis(0, kDikUp, 1.f);
    const float v_sm1 = java_io_Controller_user_GetAxisVal(sm, kAxisThrottle);
    Sleep(120);
    input_set_axis(0, kDikUp, 1.f);
    const float v_sm2 = java_io_Controller_user_GetAxisVal(sm, kAxisThrottle);
    // Controller path with power curve.
    InvObject* ctrl = gameref_new();
    java_io_Controller_user_Add(ctrl, kAxisBrake, 0, kDikUp, 0.f, 1.f, 0.f, 1.f);
    java_io_Controller_user_SetAxisSmooth(ctrl, kAxisBrake, 0.1f, 2.f, 4.f, 8.f,
                                          1.f);
    input_set_axis(0, kDikUp, 0.f);
    java_io_Controller_user_GetAxisVal(ctrl, kAxisBrake);  // arm dt
    input_set_axis(0, kDikUp, 1.f);
    Sleep(50);
    input_set_axis(0, kDikUp, 1.f);
    const float v_brk = java_io_Controller_user_GetAxisVal(ctrl, kAxisBrake);

    input_set_axis(0, kDikUp, 0.f);
    // Isolate from DirectInput leftovers for Options-style remap probe.
    // PE 0x00557AA0: exactly one axis with v>=0.25 (not max-abs / 0.5).
    const bool live_was = input_live_enabled();
    input_live_enable(false);
    for (int a = 0; a < 256; ++a) input_set_axis(0, a, 0.f);
    const int32_t none = java_io_Input_activeAxis(0);
    input_set_axis(0, 0x1e, 1.f);  // RCDIK_A
    const int32_t act = java_io_Input_activeAxis(0);
    input_set_axis(0, 0x1e, 0.30f);  // above 0.25, below old host 0.5
    const int32_t act_lo = java_io_Input_activeAxis(0);
    input_set_axis(0, 0x1f, 1.f);  // second key → PE returns -1
    const int32_t act_two = java_io_Input_activeAxis(0);
    input_set_axis(0, 0x1f, 0.f);
    input_set_axis(0, 0x1e, 0.20f);  // deadzone
    const int32_t act_dz = java_io_Input_activeAxis(0);
    input_set_axis(0, 0x1e, 0.f);
    const int32_t act_oor = java_io_Input_activeAxis(2);
    input_live_enable(live_was);

    const bool smooth_ok = v_raw0 > 0.95f && v_sm0 < 0.35f &&
                           v_sm1 > v_sm0 + 0.05f && v_sm2 > v_sm1 + 0.05f &&
                           v_sm2 < 0.95f && v_brk > 0.05f && v_brk < 0.95f;
    const bool active_ok = none < 0 && act == 0x1e && act_lo == 0x1e &&
                           act_two < 0 && act_dz < 0 && act_oor < 0;
    const bool ax_ok = smooth_ok && active_ok;
    std::printf("boot axis_smooth ok=%d raw=%.2f sm=%.2f->%.2f->%.2f "
                "brk=%.2f active=%d/%d\n",
                ax_ok ? 1 : 0, v_raw0, v_sm0, v_sm1, v_sm2, v_brk, none, act);
    input_set_axis(0, kDikUp, 0.f);
    if (!ax_ok) {
      std::printf("FAIL Input.setAxisSmooth/activeAxis\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.108: SetAxisSpeed + sun flare sprites on flush.
  {
    constexpr int32_t kDikUp = 0xc8;
    InvObject* slow = gameref_new();
    InvObject* fast = gameref_new();
    java_io_Input_mapAxis(slow, kAxisThrottle, 0, kDikUp, 0.f, 1.f, 0.f, 1.f);
    java_io_Input_mapAxis(fast, kAxisThrottle, 0, kDikUp, 0.f, 1.f, 0.f, 1.f);
    java_io_Input_setAxisSmooth(slow, kAxisThrottle, 0.1f, 2.f, 4.f, 1.f);
    java_io_Input_setAxisSmooth(fast, kAxisThrottle, 0.1f, 2.f, 4.f, 1.f);
    java_io_Controller_user_SetAxisSpeed(fast, kAxisThrottle, 3.f);
    input_set_axis(0, kDikUp, 1.f);
    java_io_Controller_user_GetAxisVal(slow, kAxisThrottle);
    java_io_Controller_user_GetAxisVal(fast, kAxisThrottle);
    Sleep(100);
    input_set_axis(0, kDikUp, 1.f);
    const float v_slow = java_io_Controller_user_GetAxisVal(slow, kAxisThrottle);
    const float v_fast = java_io_Controller_user_GetAxisVal(fast, kAxisThrottle);
    input_set_axis(0, kDikUp, 0.f);

    InvObject* sun = resref_new();
    InvObject* glow = resref_new();
    java_util_resource_ResourceRef_set(glow, 0x7E108001);
    render_d3d9_texture_create_solid(glow, 0xFFE4E4E4u, 16);
    render_d3d9_clear_flare(nullptr);
    java_util_resource_RenderRef_setFlare(sun, glow, 0xe4e4e4FF, 1.f, 10.f, 8,
                                          4);
    const int32_t src0 = render_d3d9_flare_sources();
    render_d3d9_flush();
    const int32_t sprites = render_d3d9_flare_sprites_last();
    const int32_t osd_n = render_d3d9_osd_count();
    render_d3d9_clear_flare(sun);
    render_d3d9_texture_destroy(glow);

    const bool speed_ok = v_fast > v_slow + 0.08f;
    const bool flare_ok = src0 >= 1 && sprites >= 8 && osd_n >= sprites;
    const bool f108_ok = speed_ok && flare_ok;
    std::printf("boot axis_speed_flare ok=%d slow=%.2f fast=%.2f "
                "flare_src=%d sprites=%d osd=%d\n",
                f108_ok ? 1 : 0, v_slow, v_fast, src0, sprites, osd_n);
    if (!f108_ok) {
      std::printf("FAIL SetAxisSpeed/flare\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.109: world→screen flare projection (active camera).
  {
    void* vp = reinterpret_cast<void*>(static_cast<uintptr_t>(0xF10901u));
    void* cam = reinterpret_cast<void*>(static_cast<uintptr_t>(0xF10902u));
    render_d3d9_viewport_create(vp, 0, 0.f, 0.f, 1.f, 1.f);
    render_d3d9_camera_create(cam, nullptr, vp, 0, 45.f, 0.1f, 500.f, 0.f, 0.f,
                              0, 0);
    render_d3d9_camera_lookat(cam, 0.f, 2.f, -12.f, 0.f, 0.f, 0.f);
    render_d3d9_camera_activate(cam, vp, 0);
    render_d3d9_viewport_activate(vp, 0);

    float sx_r = 0, sy_r = 0, sx_l = 0, sy_l = 0;
    const bool pr =
        render_d3d9_project(6.f, 3.f, 0.f, &sx_r, &sy_r);
    const bool pl =
        render_d3d9_project(-6.f, 3.f, 0.f, &sx_l, &sy_l);
    float sx_back = 0, sy_back = 0;
    const bool pback =
        render_d3d9_project(0.f, 2.f, -20.f, &sx_back, &sy_back);

    InvObject* sun = resref_new();
    InvObject* glow = resref_new();
    java_util_resource_ResourceRef_set(glow, 0x7E109001);
    render_d3d9_texture_create_solid(glow, 0xFFFFFFAAu, 8);
    render_d3d9_clear_flare(nullptr);
    java_util_resource_RenderRef_setMatrix_1(sun, vec3_new(6.f, 3.f, 0.f),
                                             nullptr);
    java_util_resource_RenderRef_setFlare(sun, glow, 0xe4e4e4FF, 1.f, 8.f, 6,
                                          0);
    render_d3d9_flush();
    float fsx = 0, fsy = 0;
    const bool fscr = render_d3d9_flare_screen_pos(sun, &fsx, &fsy);
    const int32_t sprites_on = render_d3d9_flare_sprites_last();

    java_util_resource_RenderRef_setMatrix_1(sun, vec3_new(0.f, 2.f, -20.f),
                                             nullptr);
    render_d3d9_flush();
    const int32_t sprites_off = render_d3d9_flare_sprites_last();
    float fsx2 = 0, fsy2 = 0;
    const bool fscr2 = render_d3d9_flare_screen_pos(sun, &fsx2, &fsy2);

    render_d3d9_clear_flare(sun);
    render_d3d9_texture_destroy(glow);
    render_d3d9_camera_deactivate(cam, vp);
    render_d3d9_viewport_deactivate(vp);
    render_d3d9_camera_destroy(cam);
    render_d3d9_viewport_destroy(vp);

    const bool proj_ok = pr && pl && !pback && sx_l > sx_r + 0.15f;
    const bool flare_ok =
        fscr && sprites_on >= 6 && std::fabs(fsx - sx_r) < 0.08f &&
        sprites_off == 0 && !fscr2;
    const bool f109_ok = proj_ok && flare_ok;
    std::printf("boot flare_project ok=%d +X=%.2f -X=%.2f back=%d "
                "scr=%.2f sprites=%d->%d\n",
                f109_ok ? 1 : 0, sx_r, sx_l, pback ? 1 : 0, fsx, sprites_on,
                sprites_off);
    if (!f109_ok) {
      std::printf("FAIL flare world→screen project\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.110: MouseCursor pick + System.getConfigOptions.
  {
    void* vp = reinterpret_cast<void*>(static_cast<uintptr_t>(0xF11001u));
    void* cam = reinterpret_cast<void*>(static_cast<uintptr_t>(0xF11002u));
    render_d3d9_viewport_create(vp, 0, 0.f, 0.f, 1.f, 1.f);
    render_d3d9_camera_create(cam, nullptr, vp, 0, 45.f, 0.1f, 500.f, 0.f, 0.f,
                              0, 0);
    render_d3d9_camera_lookat(cam, 0.f, 5.f, -20.f, 0.f, 0.f, 0.f);
    render_d3d9_camera_activate(cam, vp, 0);
    render_d3d9_viewport_activate(vp, 0);

    InvObject* cursor = tree_host_new("java.io.MouseCursor");
    InvObject* inner = gameref_new();
    tree_field_set_obj(cursor, "cursor", inner);
    tree_field_set_obj(cursor, "vp", reinterpret_cast<InvObject*>(vp));
    tree_field_set_obj(cursor, "camera", reinterpret_cast<InvObject*>(cam));
    java_util_resource_GameRef_setPos(inner, vec3_new(0.25f, -0.1f, 0.f));
    float px = 0, py = 0, pz = 0;
    vec3_get(java_io_MouseCursor_getPos(cursor), &px, &py, &pz);
    float qx = 0, qy = 0, qz = 0;
    vec3_get(java_io_MouseCursor_getPickedPos(cursor), &qx, &qy, &qz);
    const bool pick_ok = tree_field_get_int(cursor, "pick_ok") == 1 &&
                         std::fabs(px - 0.25f) < 1e-4f &&
                         std::fabs(py + 0.1f) < 1e-4f &&
                         std::fabs(qy) < 0.5f;  // ground plane ~ at_y

    InvObject* cfg = system_config_host_for_test();
    java_lang_System_getConfigOptions();
    const int32_t n0 = tree_field_get_int(cfg, "apply_count");
    const bool flares_on = render_d3d9_flares_enabled();
    tree_field_set_int(cfg, "flares", 0);
    java_lang_System_getConfigOptions();
    const int32_t n1 = tree_field_get_int(cfg, "apply_count");
    const bool flares_off = !render_d3d9_flares_enabled();
    tree_field_set_int(cfg, "flares", 1);
    java_lang_System_getConfigOptions();
    const bool flares_back = render_d3d9_flares_enabled();
    const int32_t vx = tree_field_get_int(cfg, "applied_video_x");

    InvObject* sun = resref_new();
    InvObject* glow = resref_new();
    render_d3d9_texture_create_solid(glow, 0xFFFFFFFF, 4);
    render_d3d9_clear_flare(nullptr);
    java_util_resource_RenderRef_setFlare(sun, glow, 0xFFFFFFFF, 1.f, 5.f, 5,
                                          0);
    render_d3d9_set_flare_world(sun, 0.f, 5.f, 0.f);
    tree_field_set_int(cfg, "flares", 0);
    java_lang_System_getConfigOptions();
    render_d3d9_flush();
    const int32_t spr_off = render_d3d9_flare_sprites_last();
    tree_field_set_int(cfg, "flares", 1);
    java_lang_System_getConfigOptions();
    render_d3d9_flush();
    const int32_t spr_on = render_d3d9_flare_sprites_last();
    render_d3d9_clear_flare(nullptr);
    render_d3d9_texture_destroy(glow);

    render_d3d9_camera_deactivate(cam, vp);
    render_d3d9_viewport_deactivate(vp);
    render_d3d9_camera_destroy(cam);
    render_d3d9_viewport_destroy(vp);

    const bool cfg_ok = n0 >= 1 && n1 == n0 + 1 && flares_on && flares_off &&
                        flares_back && vx > 0 && spr_off == 0 && spr_on >= 5;
    const bool f110_ok = pick_ok && cfg_ok;
    std::printf("boot cursor_config ok=%d pick=(%.2f,%.2f,%.2f) "
                "cfg=%d/%d flares=%d->%d spr=%d->%d\n",
                f110_ok ? 1 : 0, qx, qy, qz, n0, n1, flares_off ? 0 : 1,
                flares_back ? 1 : 0, spr_off, spr_on);
    if (!f110_ok) {
      std::printf("FAIL MouseCursor/getConfigOptions\n");
      if (want_window) render_d3d9_close();
      return 2;
    }

    // Phase 2.125 — GameRef cursor EVENT_COMMAND (move/mode/enable/…).
    {
      constexpr int32_t kEventCommand = 0x10;
      InvObject* mc = tree_host_new("java.io.MouseCursor");
      InvObject* gr = gameref_new();
      java_util_resource_GameRef_setParent(gr, mc);
      tree_field_set_obj(mc, "cursor", gr);
      tree_field_set_obj(gr, "cursor_owner", mc);
      java_util_resource_GameRef_queueEvent(
          gr, nullptr, kEventCommand, string_new("move 0.25, -0.10"));
      java_util_resource_GameRef_queueEvent(gr, nullptr, kEventCommand,
                                            string_new("enable"));
      java_util_resource_GameRef_queueEvent(gr, nullptr, kEventCommand,
                                            string_new("activate 42"));
      java_util_resource_GameRef_queueEvent(gr, nullptr, kEventCommand,
                                            string_new("sens"));
      java_util_resource_GameRef_queueEvent(gr, nullptr, kEventCommand,
                                            string_new("mode 0 5 99"));
      java_util_resource_GameRef_queueEvent(gr, nullptr, kEventCommand,
                                            string_new("disable"));
      java_util_resource_GameRef_queueEvent(gr, nullptr, kEventCommand,
                                            string_new("lock"));
      const bool locked_ok = tree_field_get_int(gr, "cursor_locked") == 1 &&
                             input_syscursor_locked();
      java_util_resource_GameRef_queueEvent(gr, nullptr, kEventCommand,
                                            string_new("unlock"));
      const bool unlocked_ok = tree_field_get_int(gr, "cursor_locked") == 0 &&
                               !input_syscursor_locked();
      float mx = 0, my = 0, mz = 0;
      vec3_get(java_io_MouseCursor_getPos(mc), &mx, &my, &mz);
      const bool move_ok = tree_field_get_int(mc, "cursor_set") == 1 &&
                           std::fabs(mx - 0.25f) < 1e-4f &&
                           std::fabs(my + 0.1f) < 1e-4f &&
                           tree_field_get_int(gr, "cursor_moved") >= 1;
      const bool mode_ok = tree_field_get_int(gr, "cursor_mode") == 0 &&
                           tree_field_get_int(gr, "cursor_stock_id") == 5 &&
                           render_d3d9_stock_cursor() == 5;
      InvObject* ptr = tree_field_get_obj(mc, "pointer");
      const char* ptr_s = ptr ? string_cstr(ptr) : "";
      const bool sens_ok =
          tree_field_get_int(gr, "sens_count") >= 1 &&
          tree_field_get_float(mc, "mouse_sensitivity") > 0.f;
      const bool act_ok = tree_field_get_int(gr, "cursor_ctrl_id") == 42 &&
                          tree_field_get_int(gr, "cursor_collide") == 0;
      const bool c125_ok = move_ok && mode_ok && sens_ok && act_ok && ptr_s &&
                           ptr_s[0] == '5' && locked_ok && unlocked_ok;
      std::printf("boot cursor_cmds ok=%d move=%.2f,%.2f mode=%d stock=%d "
                  "sens=%.2f collide=%d ptr='%s' lock=%d/%d\n",
                  c125_ok ? 1 : 0, mx, my,
                  tree_field_get_int(gr, "cursor_mode"),
                  tree_field_get_int(gr, "cursor_stock_id"),
                  tree_field_get_float(mc, "mouse_sensitivity"),
                  tree_field_get_int(gr, "cursor_collide"),
                  ptr_s ? ptr_s : "?", locked_ok ? 1 : 0, unlocked_ok ? 1 : 0);
      if (!c125_ok) {
        std::printf("FAIL cursor GameRef commands\n");
        if (want_window) render_d3d9_close();
        return 2;
      }
      render_d3d9_set_stock_cursor(2);

      // PE Cursor_tick SysCursor: WndProc NDC → Input.cursor getPos, clamp.
      InvObject* ic = java_io_Input_cursor();
      if (!tree_field_get_obj(ic, "cursor"))
        tree_field_set_obj(ic, "cursor", gameref_new());
      input_syscursor_set_ndc(0.50f, -0.25f);
      java_io_MouseCursor_tickSysCursor();
      float sx = 0, sy = 0, sz = 0;
      vec3_get(java_io_MouseCursor_getPos(ic), &sx, &sy, &sz);
      input_syscursor_set_ndc(2.0f, -3.0f);
      java_io_MouseCursor_tickSysCursor();
      float cx2 = 0, cy2 = 0, cz2 = 0;
      vec3_get(java_io_MouseCursor_getPos(ic), &cx2, &cy2, &cz2);
      const bool sys_ok = std::fabs(sx - 0.50f) < 1e-4f &&
                          std::fabs(sy + 0.25f) < 1e-4f &&
                          std::fabs(cx2 - 1.0f) < 1e-4f &&
                          std::fabs(cy2 + 1.0f) < 1e-4f;
      std::printf("boot cursor_syscursor ok=%d ndc=%.2f,%.2f clamp=%.2f,%.2f\n",
                  sys_ok ? 1 : 0, sx, sy, cx2, cy2);
      if (!sys_ok) {
        std::printf("FAIL cursor SysCursor NDC tick\n");
        if (want_window) render_d3d9_close();
        return 2;
      }

      InvObject* gt = tree_host_new("java.lang.GameType");
      // Soft PE @ 0x426653: handler GI+0x70 must include EVENT_CURSOR.
      java_lang_GameType_setEventMask(gt, 0x00010000);
      java_lang_GameType_addNotification(gt, tree_field_get_obj(ic, "cursor"),
                                         0x00010000, 0, nullptr);
      input_syscursor_set_buttons(1);
      java_io_MouseCursor_tickSysCursor();
      input_syscursor_set_buttons(0);
      java_io_MouseCursor_tickSysCursor();
      const bool cur_ok = tree_field_get_int(gt, "last_event") == 0x00010000 &&
                          tree_field_get_int(gt, "cursor_event_count") >= 3;
      std::printf("boot cursor_event ok=%d n=%d last=%d\n", cur_ok ? 1 : 0,
                  tree_field_get_int(gt, "cursor_event_count"),
                  tree_field_get_int(gt, "last_event"));
      if (!cur_ok) {
        std::printf("FAIL cursor EVENT_CURSOR drain\n");
        if (want_window) render_d3d9_close();
        return 2;
      }

      // PE Cursor_tick loc_460FD3 / loc_4612A0: L hold → EC_LDRAGBEGIN=9,
      // LUP skips LCLICK → LDRAGEND=10 (LDROP=11 only if +0xF4 pick).
      const int32_t n_click = tree_field_get_int(gt, "cursor_event_count");
      input_syscursor_set_ndc(0.0f, 0.0f);
      input_syscursor_set_buttons(1);
      java_io_MouseCursor_tickSysCursor();
      input_syscursor_set_ndc(0.25f, 0.15f);
      java_io_MouseCursor_tickSysCursor();
      input_syscursor_set_buttons(0);
      java_io_MouseCursor_tickSysCursor();
      const int32_t n_drag = tree_field_get_int(gt, "cursor_event_count");
      InvObject* lp = tree_field_get_obj(gt, "last_cursor_param");
      const char* lps = lp ? string_cstr(lp) : "";
      int32_t last_ec = 0;
      if (lps && lps[0]) std::sscanf(lps, "%d", &last_ec);
      const bool ldrag_ok =
          (n_drag - n_click) >= 4 && (last_ec == 10 || last_ec == 11);
      std::printf("boot cursor_ldrag ok=%d dn=%d last_ec=%d param='%s'\n",
                  ldrag_ok ? 1 : 0, n_drag - n_click, last_ec,
                  lps ? lps : "");
      if (!ldrag_ok) {
        std::printf("FAIL cursor EC_LDRAG BEGIN/END\n");
        if (want_window) render_d3d9_close();
        return 2;
      }
    }
  }

  // Phase 2.111: System.arraycopy (Object[] / Vector elementData hosts).
  {
    InvObject* a = string_new("a");
    InvObject* b = string_new("b");
    InvObject* c = string_new("c");
    InvObject* d = string_new("d");
    InvObject* src = tree_array_new(4);
    tree_vector_set(src, 0, a);
    tree_vector_set(src, 1, b);
    tree_vector_set(src, 2, c);
    tree_vector_set(src, 3, d);
    InvObject* dst = tree_array_new(4);
    java_lang_System_arraycopy(src, 1, dst, 0, 3);
    const bool copy_ok = tree_vector_element_at(dst, 0) == b &&
                         tree_vector_element_at(dst, 1) == c &&
                         tree_vector_element_at(dst, 2) == d &&
                         tree_vector_element_at(dst, 3) == nullptr;
    // Overlap shift right (insert hole style): [a,b,c,d] → [a,a,b,c]
    java_lang_System_arraycopy(src, 0, src, 1, 3);
    const bool overlap_ok = tree_vector_element_at(src, 0) == a &&
                            tree_vector_element_at(src, 1) == a &&
                            tree_vector_element_at(src, 2) == b &&
                            tree_vector_element_at(src, 3) == c;
    // Vector.ensureCapacity-style grow: old → new larger array.
    InvObject* grown = tree_array_new(8);
    InvObject* old = tree_array_new(3);
    tree_vector_set(old, 0, a);
    tree_vector_set(old, 1, b);
    tree_vector_set(old, 2, c);
    java_lang_System_arraycopy(old, 0, grown, 0, 3);
    const bool grow_ok = tree_vector_size(grown) == 8 &&
                         tree_vector_element_at(grown, 0) == a &&
                         tree_vector_element_at(grown, 2) == c &&
                         tree_vector_element_at(grown, 7) == nullptr;
    const bool f111_ok = copy_ok && overlap_ok && grow_ok;
    std::printf("boot arraycopy ok=%d copy=%d overlap=%d grow=%d\n",
                f111_ok ? 1 : 0, copy_ok ? 1 : 0, overlap_ok ? 1 : 0,
                grow_ok ? 1 : 0);
    if (!f111_ok) {
      std::printf("FAIL System.arraycopy\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.112: System.compileAll — scan *.class under path / classpath roots.
  {
    const size_t before = jvm.class_count();
    const bool loading_before = java_lang_System_isLoading() != 0;
    JvmValue n = jvm.invoke("java.lang.System", "compileAll",
                            "(Ljava.lang.String;)I",
                            {JvmValue::make_obj(string_new("system/Scripts/sound"))},
                            true);
    const int32_t loaded = (n.tag == JvmTag::Int) ? n.v.i : -1;
    const bool found = jvm.find_class("java.sound.Sound") != nullptr;
    const bool idle = java_lang_System_isLoading() == 0;
    const size_t after = jvm.class_count();
    // Second call still reports files scanned/loaded OK.
    const int32_t again = java_lang_System_compileAll(
        string_new("system/Scripts/sound"));
    const bool f112_ok = !loading_before && loaded >= 1 && again >= 1 && found &&
                         idle && after >= before;
    std::printf("boot compile_all ok=%d n=%d again=%d sound=%d classes=%zu->%zu\n",
                f112_ok ? 1 : 0, loaded, again, found ? 1 : 0, before, after);
    if (!f112_ok) {
      std::printf("FAIL System.compileAll\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.113: isLoading depth around openLib + LoadingScreen ld priority.
  {
    java_lang_System_isLoadingReset();
    const int32_t opens0 = system_loading_opens_for_test();
    const bool idle0 = java_lang_System_isLoading() == 0;
    const int32_t peak0 = system_loading_peak_for_test();
    const int32_t pack = java_lang_System_openLib(string_new("system.rpk"));
    const int32_t peak1 = system_loading_peak_for_test();
    const bool idle1 = java_lang_System_isLoading() == 0;
    const int32_t opens1 = system_loading_opens_for_test();
    java_lang_System_setLdPriority(1);
    const int32_t hi = system_ld_priority_for_test();
    InvObject* ls = frontend_loading_screen();
    jvm.invoke("java.render.LoadingScreen", "show", "()V",
               {JvmValue::make_obj(ls)}, false);
    const int32_t show_pri = system_ld_priority_for_test();
    const int32_t vis = frontend_loading_screen_visible();
    jvm.invoke("java.render.LoadingScreen", "hide", "()V",
               {JvmValue::make_obj(ls)}, false);
    const int32_t hide_pri = system_ld_priority_for_test();
    const bool f113_ok = idle0 && peak0 == 0 && pack != 0 && peak1 >= 1 &&
                         idle1 && opens1 == opens0 + 1 && hi == 1 &&
                         show_pri == 1 && vis == 1 && hide_pri == 0 &&
                         frontend_loading_screen_visible() == 0;
    std::printf("boot loading_openlib ok=%d peak=%d opens=%d->%d pack=%d "
                "ld=%d->%d->%d vis=%d\n",
                f113_ok ? 1 : 0, peak1, opens0, opens1, pack, hi, show_pri,
                hide_pri, vis);
    if (!f113_ok) {
      std::printf("FAIL isLoading/openLib/LoadingScreen priority\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.114: TREE AASTORE (0x20+0x08/35) — Valocity posGarage[i]=new Vector3.
  {
    if (!jvm.load_class("java.game.Valocity") ||
        !jvm.load_class("java.lang.Vector3") ||
        !jvm.load_class("java.lang.Ypr")) {
      std::printf("FAIL load Valocity/Vector3/Ypr\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
    InvObject* city = tree_host_new("java.game.Valocity");
    InvObject* pos = tree_array_new(3);
    InvObject* ori = tree_array_new(3);
    tree_field_set_obj(city, "posGarage", pos);
    tree_field_set_obj(city, "oriGarage", ori);
    jvm.invoke("java.game.Valocity", "<init>", "()V",
               {JvmValue::make_obj(city)}, false);
    // City FILD may replace host-seeded arrays during super().
    pos = tree_field_get_obj(city, "posGarage");
    ori = tree_field_get_obj(city, "oriGarage");
    InvObject* p0 = tree_vector_element_at(pos, 0);
    InvObject* p1 = tree_vector_element_at(pos, 1);
    InvObject* p2 = tree_vector_element_at(pos, 2);
    InvObject* o0 = tree_vector_element_at(ori, 0);
    float x0 = 0, y0 = 0, z0 = 0, x2 = 0, y2 = 0, z2 = 0, oy = 0, op = 0,
          or_ = 0;
    if (p0) vec3_get(p0, &x0, &y0, &z0);
    if (p2) vec3_get(p2, &x2, &y2, &z2);
    if (o0) ypr_get(o0, &oy, &op, &or_);
    // Fallback to TREE fields if native bag missed.
    if (p0 && x0 == 0.f && z0 == 0.f) {
      x0 = tree_field_get_float(p0, "x");
      y0 = tree_field_get_float(p0, "y");
      z0 = tree_field_get_float(p0, "z");
    }
    if (p2 && x2 == 0.f && z2 == 0.f) {
      x2 = tree_field_get_float(p2, "x");
      y2 = tree_field_get_float(p2, "y");
      z2 = tree_field_get_float(p2, "z");
    }
    if (o0 && oy == 0.f) oy = tree_field_get_float(o0, "y");
    const bool f114_ok =
        p0 && p1 && p2 && o0 && std::fabs(x0 + 278.518f) < 0.05f &&
        std::fabs(z0 - 1033.002f) < 0.05f && std::fabs(x2 + 531.138f) < 0.05f &&
        std::fabs(oy - 1.580f) < 0.05f;
    std::printf("boot aastore ok=%d p0=(%.2f,%.2f,%.2f) p2=(%.2f,%.2f,%.2f) "
                "ori0_y=%.3f\n",
                f114_ok ? 1 : 0, x0, y0, z0, x2, y2, z2, oy);
    if (!f114_ok) {
      std::printf("FAIL TREE AASTORE Valocity posGarage\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.115: TREE NEWARRAY (0x07/31 + 0x06 '[') + 0x1c PUTFIELD.
  {
    if (!jvm.load_class("java.util.Vector")) {
      std::printf("FAIL load Vector\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
    InvObject* vec = tree_vector_new();
    jvm.invoke("java.util.Vector", "<init>", "(II)V",
               {JvmValue::make_obj(vec), JvmValue::make_int(5),
                JvmValue::make_int(0)},
               false);
    InvObject* data = tree_field_get_obj(vec, "elementData");
    const int32_t n = tree_vector_size(data);
    const int32_t cap = tree_field_get_int(vec, "capacityIncrement");
    // Also (I)V → this(n,0) via 0x22.
    InvObject* vec2 = tree_vector_new();
    jvm.invoke("java.util.Vector", "<init>", "(I)V",
               {JvmValue::make_obj(vec2), JvmValue::make_int(7)}, false);
    const int32_t n2 =
        tree_vector_size(tree_field_get_obj(vec2, "elementData"));
    // Direct NEWARRAY host path (City: new Vector3[CLUBS]).
    InvObject* pg = nullptr;
    {
      // Mimic size=3 + type on stack via Vector-style: use tree_array_new_desc.
      pg = tree_array_new_desc(3, "[Ljava.lang.Vector3;");
    }
    // City.<init> field inits: posGarage = new Vector3[CLUBS].
    InvObject* bare = tree_host_new("java.game.City");
    if (!jvm.load_class("java.game.City")) {
      std::printf("FAIL load City\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
    jvm.invoke("java.game.City", "<init>", "()V",
               {JvmValue::make_obj(bare)}, false);
    InvObject* cpos = tree_field_get_obj(bare, "posGarage");
    InvObject* cori = tree_field_get_obj(bare, "oriGarage");
    const int32_t cpn = tree_vector_size(cpos);
    const int32_t con = tree_vector_size(cori);

    // Valocity without host-seeded arrays: super City.<init> via 0x23.
    InvObject* city = tree_host_new("java.game.Valocity");
    jvm.invoke("java.game.Valocity", "<init>", "()V",
               {JvmValue::make_obj(city)}, false);
    InvObject* pos = tree_field_get_obj(city, "posGarage");
    InvObject* ori = tree_field_get_obj(city, "oriGarage");
    const int32_t pn = tree_vector_size(pos);
    const int32_t on = tree_vector_size(ori);
    InvObject* p0 = pos ? tree_vector_element_at(pos, 0) : nullptr;
    float x0 = 0, y0 = 0, z0 = 0;
    if (p0) {
      vec3_get(p0, &x0, &y0, &z0);
      if (x0 == 0.f && z0 == 0.f) {
        x0 = tree_field_get_float(p0, "x");
        y0 = tree_field_get_float(p0, "y");
        z0 = tree_field_get_float(p0, "z");
      }
    }
    const bool vec_ok = data && n == 5 && cap == 0 && n2 == 7;
    const bool city_init_ok = cpos && cori && cpn == 3 && con == 3;
    const bool valo_ok = pos && ori && pn >= 3 && on >= 3 && p0 &&
                         std::fabs(x0 + 278.518f) < 0.05f;
    const bool desc_ok = pg && tree_vector_size(pg) == 3 &&
                         std::strcmp(tree_host_class(pg),
                                     "[Ljava.lang.Vector3;") == 0;
    const bool f115_ok = vec_ok && city_init_ok && valo_ok && desc_ok;
    std::printf("boot newarray ok=%d vec=%d/%d city=%d/%d valo=%d/%d "
                "p0x=%.2f desc=%d\n",
                f115_ok ? 1 : 0, n, n2, cpn, con, pn, on, x0, desc_ok ? 1 : 0);
    if (!f115_ok) {
      std::printf("FAIL TREE NEWARRAY / Vector elementData / Valocity arrays\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.56: command("install …") → part_install + setParent.
  {
    constexpr int32_t kCarId = 0x51001;
    constexpr int32_t kPartId = 0x51002;
    constexpr int32_t kEventCommand = 0x10;
    InvObject* car_g = gameref_new();
    InvObject* part_g = gameref_new();
    java_util_resource_ResourceRef_set(car_g, kCarId);
    java_util_resource_ResourceRef_set(part_g, kPartId);
    char cmd[96];
    std::snprintf(cmd, sizeof(cmd), "install 0 %d 1 %d 101", kCarId, kCarId);
    java_util_resource_GameRef_queueEvent(part_g, nullptr, kEventCommand,
                                          string_new(cmd));
    const InvObject* on101 = java_game_parts_Part_partOnSlot(car_g, 101);
    const int32_t mate = java_game_parts_Part_slotIDOnSlot(car_g, 101);
    const int32_t pid = java_util_resource_ResourceRef_getParentID(part_g);
    // Short form + world pose.
    InvObject* part2 = gameref_new();
    java_util_resource_ResourceRef_set(part2, 0x51003);
    java_util_resource_GameRef_queueEvent(
        part2, nullptr, kEventCommand,
        string_new("install 0 331777 0 0 0 4.5 -1.0 2.25"));
    float p2x = 0, p2y = 0, p2z = 0;
    vec3_get(java_util_resource_GameRef_getPos(part2), &p2x, &p2y, &p2z);
    const InvObject* on1 = java_game_parts_Part_partOnSlot(car_g, 1);
    const bool cmd_ok =
        on101 == part_g && mate == 1 && pid == kCarId && on1 == part2 &&
        std::fabs(p2x - 4.5f) < 1e-3f && std::fabs(p2y + 1.0f) < 1e-3f &&
        std::fabs(p2z - 2.25f) < 1e-3f;
    std::printf("gameref cmd_install ok=%d on101=%d mate=%d parent=%d "
                "pos2=(%.2f,%.2f,%.2f)\n",
                cmd_ok ? 1 : 0, on101 == part_g ? 1 : 0, mate, pid, p2x, p2y,
                p2z);
    if (!cmd_ok) {
      std::printf("FAIL GameRef.cmd_install\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.59: Part.addPart — create under root, command install onto car.
  {
    constexpr int32_t kRootId = 0x10;
    constexpr int32_t kCarId = 0x52001;
    constexpr int32_t kTypeId = 0x52099;
    constexpr int32_t kEventCommand = 0x10;
    InvObject* root = gameref_new();
    java_util_resource_ResourceRef_set(root, kRootId);
    InvObject* car = gameref_new();
    java_util_resource_ResourceRef_set(car, kCarId);
    java_util_resource_GameRef_setParent(car, root);
    const int32_t root_id = java_util_resource_ResourceRef_getParentID(car);
    InvObject* xa = gameref_new();
    InvObject* type = resref_new();
    java_util_resource_ResourceRef_set(type, kTypeId);
    InvObject* part = java_util_resource_GameRef_create(
        xa, root, type, string_new(""), string_new("loaded carpart"));
    const int32_t before = java_util_resource_ResourceRef_getParentID(xa);
    char cmd[96];
    // Stock Part.addPart: "install 0 " + carID + " 0 " + this.id() + " 0"
    std::snprintf(cmd, sizeof(cmd), "install 0 %d 0 %d 0", kCarId, kCarId);
    java_util_resource_GameRef_queueEvent(xa, nullptr, kEventCommand,
                                          string_new(cmd));
    const int32_t after = java_util_resource_ResourceRef_getParentID(xa);
    const int32_t after_script =
        part ? java_util_resource_ResourceRef_getParentID(part) : 0;
    const InvObject* on1 = java_game_parts_Part_partOnSlot(car, 1);
    const bool add_ok = part && root_id == kRootId && before == kRootId &&
                        after == kCarId && after != root_id &&
                        after_script == kCarId && on1 == xa;
    std::printf("gameref addPart_install ok=%d root=%d before=%d after=%d "
                "script=%d on1=%d\n",
                add_ok ? 1 : 0, root_id, before, after, after_script,
                on1 == xa ? 1 : 0);
    if (!add_ok) {
      std::printf("FAIL GameRef.addPart_install\n");
      if (want_window) render_d3d9_close();
      return 2;
    }

    // Phase 2.60: getCar / getCarRef walk part_parent to chassis.
    InvObject* mid = gameref_new();
    java_util_resource_ResourceRef_set(mid, 0x520A1);
    InvObject* leaf = gameref_new();
    java_util_resource_ResourceRef_set(leaf, 0x520A2);
    part_install(car, 2, mid, 1);
    part_install(mid, 2, leaf, 1);
    const int32_t car_id_leaf = java_game_parts_Part_getCar(leaf);
    const int32_t car_id_xa = java_game_parts_Part_getCar(xa);
    const int32_t car_id_self = java_game_parts_Part_getCar(car);
    InvObject* cref = java_game_parts_Part_getCarRef(leaf);
    const bool getcar_ok = car_id_leaf == kCarId && car_id_xa == kCarId &&
                           car_id_self == kCarId && cref == car;
    std::printf("gameref getCar ok=%d leaf=%d xa=%d self=%d cref=%d\n",
                getcar_ok ? 1 : 0, car_id_leaf, car_id_xa, car_id_self,
                cref == car ? 1 : 0);
    if (!getcar_ok) {
      std::printf("FAIL GameRef.getCar\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.61: failed install → destroy (Part.addPart else branch).
  {
    constexpr int32_t kRootId = 0x11;
    constexpr int32_t kTypeId = 0x53001;
    constexpr int32_t kBogusCar = 0x00DEAD;
    constexpr int32_t kEventCommand = 0x10;
    InvObject* root = gameref_new();
    java_util_resource_ResourceRef_set(root, kRootId);
    InvObject* xa = gameref_new();
    InvObject* type = resref_new();
    java_util_resource_ResourceRef_set(type, kTypeId);
    InvObject* part = java_util_resource_GameRef_create(
        xa, root, type, string_new(""), string_new("temp repair part"));
    const int32_t before = java_util_resource_ResourceRef_getParentID(xa);
    char cmd[64];
    std::snprintf(cmd, sizeof(cmd), "install 0 %d 0 %d 0", kBogusCar, kBogusCar);
    java_util_resource_GameRef_queueEvent(xa, nullptr, kEventCommand,
                                          string_new(cmd));
    const int32_t after_fail =
        java_util_resource_ResourceRef_getParentID(xa);
    const int32_t succeeded = (after_fail != before) ? 1 : 0;
    const int32_t empty_before =
        java_util_resource_GameRef_isEmpty(xa);
    if (!succeeded) java_util_resource_ResourceRef_destroy(xa);
    const int32_t empty_after = java_util_resource_GameRef_isEmpty(xa);
    const int32_t pid_after =
        java_util_resource_ResourceRef_getParentID(xa);
    const int32_t id_after = java_util_resource_ResourceRef_id(xa);
    InvObject* found = resref_find_by_id(kTypeId);
    // Script instance may linger; xa itself must be empty / unbound.
    const bool dest_ok =
        before == kRootId && succeeded == 0 && empty_before == 0 &&
        empty_after == 1 && pid_after == 0 && id_after == 0 &&
        found != xa && part != nullptr;
    std::printf("gameref destroy_fail ok=%d before=%d succeeded=%d "
                "empty=%d->%d id=%d found_xa=%d\n",
                dest_ok ? 1 : 0, before, succeeded, empty_before, empty_after,
                id_after, found == xa ? 1 : 0);
    if (!dest_ok) {
      std::printf("FAIL GameRef.destroy_fail\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // ResourceRef
  InvObject* rr = resref_new();
  java_util_resource_ResourceRef_newNative(rr);
  java_util_resource_ResourceRef_set(rr, 99);
  java_util_resource_ResourceRef_makeTexture(rr, nullptr, string_new("x.dds"));
  std::printf("res id=%d type=%d\n", java_util_resource_ResourceRef_id(rr),
              java_util_resource_ResourceRef_type(rr));

  const bool ok = iv == 123 && fv > 4.49f && fv < 4.51f && slen == 7 &&
                  java_lang_Vector3_length(vec3_new(3.f, 4.f, 0.f)) > 4.9f &&
                  gx == 10.f && java_util_resource_GameRef_getFlags(gr) == 0x11 &&
                  java_util_resource_ResourceRef_type(rr) == 7 &&
                  render_d3d9_num_display_modes() >= 1 &&
                  (!want_window || render_d3d9_ready());
  std::printf("ok=%d\n", ok ? 1 : 0);
  if (want_window) {
    render_d3d9_pump(50);
    render_d3d9_close();
  }
  return ok ? 0 : 2;
}
