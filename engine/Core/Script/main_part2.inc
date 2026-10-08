// Fragment 2/4 of main.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


    InvObject* air_ctrl = gameref_new();
    InvObject* air = resref_new();
    InvObject* gnd = resref_new();
    java_util_resource_PhysicsRef_createBox(air, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(gnd, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(air, vec3_new(0.f, 5.f, 20.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(gnd, vec3_new(10.f, 0.5f, 20.f),
                                            nullptr);
    physics_set_gear(air, 2);
    physics_set_gear(gnd, 2);
    physics_set_velocity(air, 0.f, 0.f, 18.f);
    physics_set_velocity(gnd, 0.f, 0.f, 18.f);
    java_io_Controller_user_SetAxisForce(air_ctrl, kAxisThrottle, 0.f, 0.f);
    java_io_Controller_user_SetAxisForce(air_ctrl, kAxisTurnLR, 0.f, 1.f);
    physics_drive(air, air_ctrl, 0.05f);
    const int32_t air_flag = physics_is_airborne(air);
    float ay0 = 0, ap0 = 0, ar0 = 0, gy0 = 0, gp0 = 0, gr0 = 0;
    ypr_get(java_util_resource_PhysicsRef_getOri(air), &ay0, &ap0, &ar0);
    ypr_get(java_util_resource_PhysicsRef_getOri(gnd), &gy0, &gp0, &gr0);
    for (int i = 0; i < 25; ++i) {
      // Keep the air body aloft so the airborne grip cut stays active.
      float axp = 0, ayp = 0, azp = 0, aoy = 0, aop = 0, aor = 0;
      vec3_get(java_util_resource_PhysicsRef_getPos(air), &axp, &ayp, &azp);
      ypr_get(java_util_resource_PhysicsRef_getOri(air), &aoy, &aop, &aor);
      java_util_resource_PhysicsRef_setMatrix(air, vec3_new(axp, 5.f, azp),
                                              ypr_new(aoy, aop, aor));
      physics_set_velocity(air, 0.f, 0.f, 18.f);
      physics_drive(air, air_ctrl, 0.05f);
      physics_drive(gnd, air_ctrl, 0.05f);
    }
    float ay1 = 0, ap1 = 0, ar1 = 0, gy1 = 0, gp1 = 0, gr1 = 0;
    ypr_get(java_util_resource_PhysicsRef_getOri(air), &ay1, &ap1, &ar1);
    ypr_get(java_util_resource_PhysicsRef_getOri(gnd), &gy1, &gp1, &gr1);
    const float air_turn = std::fabs(ay1 - ay0);
    const float gnd_turn = std::fabs(gy1 - gy0);
    java_util_resource_PhysicsRef_setStatic(air, 1);
    java_util_resource_PhysicsRef_setStatic(gnd, 1);
    java_io_Controller_user_SetAxisForce(air_ctrl, kAxisTurnLR, 0.f, 0.f);
    physics_road_clear();
    const bool air_ok =
        air_flag == 1 && gnd_turn > 0.35f && air_turn * 2.5f < gnd_turn;
    const bool roadfeel_ok = yaw_ok && air_ok;
    std::printf("phys roadfeel ok=%d yaw_err=%.2f air=%d turn=%.2f<%.2f\n",
                roadfeel_ok ? 1 : 0, dyaw, air_flag, air_turn, gnd_turn);
    if (!roadfeel_ok) {
      std::printf("FAIL physics road yaw / airborne\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.95: road pitch from segment ΔY.
    {
      physics_road_clear();
      physics_road_add_segment(0.f, 0.f, 0.f, 100.f, 25.f, 0.f);  // slope 0.25
      float prx = 0, pry = 0, prz = 0, tdx = 0, tdy = 0, tdz = 0;
      const bool proj_ok =
          physics_road_project(40.f, 0.f, &prx, &pry, &prz, &tdx, &tdy, &tdz);
      const float want_pitch = std::atan(0.25f);
      InvObject* rp_ctrl = gameref_new();
      InvObject* rp_body = resref_new();
      java_util_resource_PhysicsRef_createBox(rp_body, nullptr, 1.f, 0.5f, 2.f,
                                              nullptr);
      // Face +X (yaw ≈ π/2), start on the climb.
      java_util_resource_PhysicsRef_setMatrix(
          rp_body, vec3_new(10.f, 3.f, 0.f), ypr_new(1.5707963f, 0.f, 0.f));
      physics_set_gear(rp_body, 2);
      physics_set_velocity(rp_body, 16.f, 0.f, 0.f);
      java_io_Controller_user_SetAxisForce(rp_ctrl, kAxisThrottle, 0.f, 0.2f);
      java_io_Controller_user_SetAxisForce(rp_ctrl, kAxisTurnLR, 0.f, 0.f);
      for (int i = 0; i < 40; ++i) physics_drive(rp_body, rp_ctrl, 0.05f);
      float rp_y = 0, rp_p = 0, rp_r = 0;
      ypr_get(java_util_resource_PhysicsRef_getOri(rp_body), &rp_y, &rp_p,
              &rp_r);
      java_util_resource_PhysicsRef_setStatic(rp_body, 1);
      java_io_Controller_user_SetAxisForce(rp_ctrl, kAxisThrottle, 0.f, 0.f);
      physics_road_clear();
      const bool roadpitch_ok =
          proj_ok && std::fabs(tdy - 0.25f) < 0.02f &&
          std::fabs(pry - 10.f) < 0.5f && rp_p > want_pitch * 0.45f &&
          rp_p < want_pitch + 0.08f;
      std::printf(
          "phys roadpitch ok=%d tan_dy=%.3f py=%.1f pitch=%.3f want=%.3f\n",
          roadpitch_ok ? 1 : 0, tdy, pry, rp_p, want_pitch);
      if (!roadpitch_ok) {
        std::printf("FAIL physics road pitch\n");
        render_d3d9_close();
        return 6;
      }
    }

    // Phase 2.66: WheelRef drive/steer/radius → physics_drive.
    InvObject* wctrl = gameref_new();
    InvObject* wnd = resref_new();
    InvObject* wd1 = resref_new();
    InvObject* wr_big = resref_new();
    InvObject* wr_sml = resref_new();
    InvObject* wst = resref_new();
    java_util_resource_PhysicsRef_createBox(wnd, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(wd1, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(wr_big, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(wr_sml, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(wst, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(wnd, vec3_new(100.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(wd1, vec3_new(110.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(wr_big, vec3_new(120.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(wr_sml, vec3_new(130.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(wst, vec3_new(140.f, 0.5f, 0.f),
                                            nullptr);
    physics_set_gear(wnd, 1);
    physics_set_gear(wd1, 1);
    physics_set_gear(wr_big, 1);
    physics_set_gear(wr_sml, 1);
    physics_set_gear(wst, 1);
    physics_set_wheel_params(wnd, 0.f, 0.f, 0.32f);
    physics_set_wheel_params(wd1, 0.f, 1.f, 0.32f);
    physics_set_wheel_params(wr_big, 0.f, 1.f, 0.48f);
    physics_set_wheel_params(wr_sml, 0.f, 1.f, 0.20f);
    physics_set_wheel_params(wst, 1.f, 1.f, 0.32f);
    java_io_Controller_user_SetAxisForce(wctrl, kAxisThrottle, 0.f, 1.f);
    for (int i = 0; i < 25; ++i) {
      physics_drive(wnd, wctrl, 0.05f);
      physics_drive(wd1, wctrl, 0.05f);
      physics_drive(wr_big, wctrl, 0.05f);
      physics_drive(wr_sml, wctrl, 0.05f);
      physics_drive(wst, wctrl, 0.05f);
    }
    java_io_Controller_user_SetAxisForce(wctrl, kAxisThrottle, 0.f, 0.f);
    const float spd_nd = std::sqrt(physics_speed_square(wnd));
    const float spd_d1 = std::sqrt(physics_speed_square(wd1));
    const float spd_big = std::sqrt(physics_speed_square(wr_big));
    const float spd_sml = std::sqrt(physics_speed_square(wr_sml));
    float st_oy = 0.f, st_op = 0.f, st_or = 0.f;
    if (InvObject* o = java_util_resource_PhysicsRef_getOri(wst))
      ypr_get(o, &st_oy, &st_op, &st_or);
    java_util_resource_PhysicsRef_setStatic(wnd, 1);
    java_util_resource_PhysicsRef_setStatic(wd1, 1);
    java_util_resource_PhysicsRef_setStatic(wr_big, 1);
    java_util_resource_PhysicsRef_setStatic(wr_sml, 1);
    java_util_resource_PhysicsRef_setStatic(wst, 1);
    const bool wheel_phys_ok =
        spd_nd < 0.5f && spd_d1 > 5.f && spd_big > spd_sml + 1.5f &&
        std::fabs(st_oy) > 0.15f;
    std::printf("phys wheel ok=%d drive0=%.2f drive1=%.1f rad=%.1f>%.1f "
                "steer_oy=%.2f\n",
                wheel_phys_ok ? 1 : 0, spd_nd, spd_d1, spd_big, spd_sml, st_oy);
    if (!wheel_phys_ok) {
      std::printf("FAIL physics wheel params\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.67: friction/sliction → grip; wheel brake; roll_res → drag.
    InvObject* grip_hi = resref_new();
    InvObject* grip_lo = resref_new();
    InvObject* brk_on = resref_new();
    InvObject* brk_off = resref_new();
    InvObject* roll_hi = resref_new();
    InvObject* roll_lo = resref_new();
    java_util_resource_PhysicsRef_createBox(grip_hi, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(grip_lo, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(brk_on, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(brk_off, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(roll_hi, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(roll_lo, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(grip_hi, vec3_new(150.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(grip_lo, vec3_new(160.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(brk_on, vec3_new(170.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(brk_off, vec3_new(180.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(roll_hi, vec3_new(190.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(roll_lo, vec3_new(200.f, 0.5f, 0.f),
                                            nullptr);
    physics_set_wheel_params(grip_hi, 0.f, 1.f, 0.32f);
    physics_set_wheel_params(grip_lo, 0.f, 1.f, 0.32f);
    physics_set_wheel_contact(grip_hi, 2.f, 1.f, 0.f, 0.f, 0.f);
    physics_set_wheel_contact(grip_lo, 0.1f, 0.1f, 0.f, 0.f, 0.f);
    physics_set_velocity(grip_hi, 12.f, 0.f, 20.f);
    physics_set_velocity(grip_lo, 12.f, 0.f, 20.f);
    for (int i = 0; i < 30; ++i) {
      physics_drive(grip_hi, nullptr, 0.05f);
      physics_drive(grip_lo, nullptr, 0.05f);
    }
    float ghx = 0, ghy = 0, ghz = 0, glx = 0, gly = 0, glz = 0;
    vec3_get(java_util_resource_PhysicsRef_getVel(grip_hi), &ghx, &ghy, &ghz);
    vec3_get(java_util_resource_PhysicsRef_getVel(grip_lo), &glx, &gly, &glz);
    physics_set_wheel_params(brk_on, 0.f, 1.f, 0.32f);
    physics_set_wheel_params(brk_off, 0.f, 1.f, 0.32f);
    physics_set_wheel_contact(brk_on, 1.f, 1.f, 1.f, 0.f, 0.f);
    physics_set_wheel_contact(brk_off, 1.f, 1.f, 0.f, 0.f, 0.f);
    physics_set_velocity(brk_on, 0.f, 0.f, 25.f);
    physics_set_velocity(brk_off, 0.f, 0.f, 25.f);
    for (int i = 0; i < 20; ++i) {
      physics_drive(brk_on, nullptr, 0.05f);
      physics_drive(brk_off, nullptr, 0.05f);
    }
    const float brk_on_spd = std::sqrt(physics_speed_square(brk_on));
    const float brk_off_spd = std::sqrt(physics_speed_square(brk_off));
    physics_set_wheel_params(roll_hi, 0.f, 1.f, 0.32f);
    physics_set_wheel_params(roll_lo, 0.f, 1.f, 0.32f);
    physics_set_wheel_contact(roll_hi, 1.f, 1.f, 0.f, 0.f, 0.01f);
    physics_set_wheel_contact(roll_lo, 1.f, 1.f, 0.f, 0.f, 0.f);
    physics_set_velocity(roll_hi, 0.f, 0.f, 30.f);
    physics_set_velocity(roll_lo, 0.f, 0.f, 30.f);
    for (int i = 0; i < 40; ++i) {
      physics_drive(roll_hi, nullptr, 0.05f);
      physics_drive(roll_lo, nullptr, 0.05f);
    }
    const float roll_hi_spd = std::sqrt(physics_speed_square(roll_hi));
    const float roll_lo_spd = std::sqrt(physics_speed_square(roll_lo));
    java_util_resource_PhysicsRef_setStatic(grip_hi, 1);
    java_util_resource_PhysicsRef_setStatic(grip_lo, 1);
    java_util_resource_PhysicsRef_setStatic(brk_on, 1);
    java_util_resource_PhysicsRef_setStatic(brk_off, 1);
    java_util_resource_PhysicsRef_setStatic(roll_hi, 1);
    java_util_resource_PhysicsRef_setStatic(roll_lo, 1);
    const bool contact_ok =
        std::fabs(ghx) + 0.5f < std::fabs(glx) &&
        brk_on_spd + 5.f < brk_off_spd && roll_hi_spd + 2.f < roll_lo_spd;
    std::printf("phys contact ok=%d lat=%.1f<%.1f brk=%.1f<%.1f roll=%.1f<%.1f\n",
                contact_ok ? 1 : 0, std::fabs(ghx), std::fabs(glx), brk_on_spd,
                brk_off_spd, roll_hi_spd, roll_lo_spd);
    if (!contact_ok) {
      std::printf("FAIL physics wheel contact\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.68: Pacejka B/C/D → lateral grip (Magic Formula arcade).
    InvObject* pk_hi = resref_new();
    InvObject* pk_lo = resref_new();
    java_util_resource_PhysicsRef_createBox(pk_hi, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(pk_lo, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(pk_hi, vec3_new(210.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(pk_lo, vec3_new(220.f, 0.5f, 0.f),
                                            nullptr);
    physics_set_wheel_params(pk_hi, 0.f, 1.f, 0.32f);
    physics_set_wheel_params(pk_lo, 0.f, 1.f, 0.32f);
    physics_set_wheel_contact(pk_hi, 1.f, 1.f, 0.f, 0.f, 0.f);
    physics_set_wheel_contact(pk_lo, 1.f, 1.f, 0.f, 0.f, 0.f);
    physics_set_wheel_pacejka(pk_hi, 20.f, 1.6f, 2.8f);
    physics_set_wheel_pacejka(pk_lo, 4.f, 1.0f, 0.35f);
    physics_set_velocity(pk_hi, 14.f, 0.f, 18.f);
    physics_set_velocity(pk_lo, 14.f, 0.f, 18.f);
    for (int i = 0; i < 25; ++i) {
      physics_drive(pk_hi, nullptr, 0.05f);
      physics_drive(pk_lo, nullptr, 0.05f);
    }
    float pk_hx = 0, pk_hy = 0, pk_hz = 0, pk_lx = 0, pk_ly = 0, pk_lz = 0;
    vec3_get(java_util_resource_PhysicsRef_getVel(pk_hi), &pk_hx, &pk_hy,
             &pk_hz);
    vec3_get(java_util_resource_PhysicsRef_getVel(pk_lo), &pk_lx, &pk_ly,
             &pk_lz);
    // Host WheelRef setPacejka ↔ get.
    InvObject* pk_wr = gameref_new();
    java_game_parts_WheelRef_setPacejka(pk_wr, 0, 1.4f);
    java_game_parts_WheelRef_setPacejka(pk_wr, 2, 1.49f);
    java_game_parts_WheelRef_setPacejka(pk_wr, 4, 15.2f);
    const float pk_g0 = wheelref_get_pacejka(pk_wr, 0);
    const float pk_g2 = wheelref_get_pacejka(pk_wr, 2);
    const float pk_g4 = wheelref_get_pacejka(pk_wr, 4);
    java_util_resource_PhysicsRef_setStatic(pk_hi, 1);
    java_util_resource_PhysicsRef_setStatic(pk_lo, 1);
    const bool pacejka_ok =
        std::fabs(pk_hx) + 0.5f < std::fabs(pk_lx) && pk_g0 > 1.39f &&
        pk_g0 < 1.41f && pk_g2 > 1.48f && pk_g2 < 1.50f && pk_g4 > 15.1f &&
        pk_g4 < 15.3f;
    std::printf("phys pacejka ok=%d lat=%.1f<%.1f BCD=%.2f,%.2f,%.2f\n",
                pacejka_ok ? 1 : 0, std::fabs(pk_hx), std::fabs(pk_lx), pk_g4,
                pk_g2, pk_g0);
    if (!pacejka_ok) {
      std::printf("FAIL physics pacejka\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.69: suspension spring/damp/rest + arm/hub host getters.
    InvObject* sus_hi = resref_new();
    InvObject* sus_lo = resref_new();
    InvObject* rest_hi = resref_new();
    InvObject* rest_lo = resref_new();
    InvObject* arm_short = resref_new();
    InvObject* arm_long = resref_new();
    java_util_resource_PhysicsRef_createBox(sus_hi, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(sus_lo, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(rest_hi, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(rest_lo, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(arm_short, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(arm_long, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(sus_hi, vec3_new(230.f, 2.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(sus_lo, vec3_new(240.f, 2.5f, 0.f),
                                            nullptr);
    physics_set_wheel_params(sus_hi, 0.f, 1.f, 0.32f);
    physics_set_wheel_params(sus_lo, 0.f, 1.f, 0.32f);
    physics_set_wheel_suspension(sus_hi, 20000.f, 9000.f, 0.39f, 0.244f);
    physics_set_wheel_suspension(sus_lo, 20000.f, 200.f, 0.39f, 0.244f);
    float peak_hi = 0.f, peak_lo = 0.f;
    for (int i = 0; i < 40; ++i) {
      physics_drive(sus_hi, nullptr, 0.05f);
      physics_drive(sus_lo, nullptr, 0.05f);
      float shx = 0, shy = 0, shz = 0, slx = 0, sly = 0, slz = 0;
      vec3_get(java_util_resource_PhysicsRef_getVel(sus_hi), &shx, &shy, &shz);
      vec3_get(java_util_resource_PhysicsRef_getVel(sus_lo), &slx, &sly, &slz);
      if (std::fabs(shy) > peak_hi) peak_hi = std::fabs(shy);
      if (std::fabs(sly) > peak_lo) peak_lo = std::fabs(sly);
    }
    java_util_resource_PhysicsRef_setMatrix(rest_hi, vec3_new(250.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(rest_lo, vec3_new(260.f, 0.5f, 0.f),
                                            nullptr);
    physics_set_wheel_params(rest_hi, 0.f, 1.f, 0.32f);
    physics_set_wheel_params(rest_lo, 0.f, 1.f, 0.32f);
    physics_set_wheel_suspension(rest_hi, 20000.f, 4000.f, 0.55f, 0.244f);
    physics_set_wheel_suspension(rest_lo, 20000.f, 4000.f, 0.39f, 0.244f);
    for (int i = 0; i < 20; ++i) {
      physics_drive(rest_hi, nullptr, 0.05f);
      physics_drive(rest_lo, nullptr, 0.05f);
    }
    float rhx = 0, rhy = 0, rhz = 0, rlx = 0, rly = 0, rlz = 0;
    vec3_get(java_util_resource_PhysicsRef_getPos(rest_hi), &rhx, &rhy, &rhz);
    vec3_get(java_util_resource_PhysicsRef_getPos(rest_lo), &rlx, &rly, &rlz);
    InvObject* arm_ctrl = gameref_new();
    java_io_Controller_user_SetAxisForce(arm_ctrl, kAxisThrottle, 0.f, 1.f);
    java_util_resource_PhysicsRef_setMatrix(arm_short,
                                            vec3_new(270.f, 0.5f, 0.f), nullptr);
    java_util_resource_PhysicsRef_setMatrix(arm_long, vec3_new(280.f, 0.5f, 0.f),
                                            nullptr);
    physics_set_wheel_params(arm_short, 1.f, 1.f, 0.32f);
    physics_set_wheel_params(arm_long, 1.f, 1.f, 0.32f);
    physics_set_wheel_suspension(arm_short, 0.f, 0.f, 0.39f, 0.15f);
    physics_set_wheel_suspension(arm_long, 0.f, 0.f, 0.39f, 0.50f);
    for (int i = 0; i < 30; ++i) {
      physics_drive(arm_short, arm_ctrl, 0.05f);
      physics_drive(arm_long, arm_ctrl, 0.05f);
    }
    java_io_Controller_user_SetAxisForce(arm_ctrl, kAxisThrottle, 0.f, 0.f);
    float as_oy = 0, as_op = 0, as_or = 0, al_oy = 0, al_op = 0, al_or = 0;
    if (InvObject* o = java_util_resource_PhysicsRef_getOri(arm_short))
      ypr_get(o, &as_oy, &as_op, &as_or);
    if (InvObject* o = java_util_resource_PhysicsRef_getOri(arm_long))
      ypr_get(o, &al_oy, &al_op, &al_or);
    InvObject* ah_wr = gameref_new();
    java_game_parts_WheelRef_setArm(ah_wr, 0.244f, 0.344f, 0.054f, 0.f, 0.f,
                                    0.f, 1.f);
    java_game_parts_WheelRef_setHub(ah_wr, 0.263f, 0.f, -0.125f, 0.f, 0.f,
                                    0.143f, 0.f, 0.360f, 0.323f, 0.f);
    java_game_parts_WheelRef_setForce(ah_wr, 20000.f);
    java_game_parts_WheelRef_setDamping_1(ah_wr, 3000.f, 4500.f);
    java_game_parts_WheelRef_setRestLen(ah_wr, 0.39f);
    float arm_out[7] = {};
    float hub_out[10] = {};
    const bool arm_got = wheelref_get_arm(ah_wr, arm_out);
    const bool hub_got = wheelref_get_hub(ah_wr, hub_out);
    java_util_resource_PhysicsRef_setStatic(sus_hi, 1);
    java_util_resource_PhysicsRef_setStatic(sus_lo, 1);
    java_util_resource_PhysicsRef_setStatic(rest_hi, 1);
    java_util_resource_PhysicsRef_setStatic(rest_lo, 1);
    java_util_resource_PhysicsRef_setStatic(arm_short, 1);
    java_util_resource_PhysicsRef_setStatic(arm_long, 1);
    const bool susp_ok =
        peak_hi + 0.5f < peak_lo && rhy > rly + 0.04f &&
        std::fabs(as_oy) > std::fabs(al_oy) + 0.2f && arm_got && hub_got &&
        arm_out[0] > 0.24f && arm_out[0] < 0.25f && hub_out[0] > 0.26f &&
        hub_out[0] < 0.27f && wheelref_get_force(ah_wr) > 19999.f &&
        wheelref_get_damp_bound(ah_wr) > 2999.f &&
        wheelref_get_rest_len(ah_wr) > 0.38f;
    std::printf(
        "phys susp ok=%d damp_vy=%.2f<%.2f rest_y=%.2f>%.2f arm_oy=%.2f>%.2f "
        "arm/hub=%d/%d\n",
        susp_ok ? 1 : 0, peak_hi, peak_lo, rhy, rly, std::fabs(as_oy),
        std::fabs(al_oy), arm_got ? 1 : 0, hub_got ? 1 : 0);
    if (!susp_ok) {
      std::printf("FAIL physics suspension\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.81: drive_torque_nm scales arcade accel (DynoData path).
    InvObject* tq_ctrl = gameref_new();
    InvObject* tq_hi = resref_new();
    InvObject* tq_lo = resref_new();
    java_util_resource_PhysicsRef_createBox(tq_hi, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(tq_lo, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(tq_hi, vec3_new(300.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(tq_lo, vec3_new(310.f, 0.5f, 0.f),
                                            nullptr);
    physics_set_gear(tq_hi, 1);
    physics_set_gear(tq_lo, 1);
    physics_set_drive_torque(tq_hi, 400.f);
    physics_set_drive_torque(tq_lo, 100.f);
    java_io_Controller_user_SetAxisForce(tq_ctrl, kAxisThrottle, 0.f, 1.f);
    for (int i = 0; i < 25; ++i) {
      physics_drive(tq_hi, tq_ctrl, 0.05f);
      physics_drive(tq_lo, tq_ctrl, 0.05f);
    }
    java_io_Controller_user_SetAxisForce(tq_ctrl, kAxisThrottle, 0.f, 0.f);
    const float spd_tq_hi = std::sqrt(physics_speed_square(tq_hi));
    const float spd_tq_lo = std::sqrt(physics_speed_square(tq_lo));
    const float rpm_hi = physics_get_engine_rpm(tq_hi);
    java_util_resource_PhysicsRef_setStatic(tq_hi, 1);
    java_util_resource_PhysicsRef_setStatic(tq_lo, 1);
    const bool tq_ok =
        spd_tq_hi > spd_tq_lo * 1.25f && spd_tq_lo > 1.f && rpm_hi > 1000.f;
    std::printf("phys torque ok=%d hi=%.1f>lo=%.1f rpm=%.0f\n", tq_ok ? 1 : 0,
                spd_tq_hi, spd_tq_lo, rpm_hi);
    if (!tq_ok) {
      std::printf("FAIL physics torque scale\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.29: SCX road mesh → centerline segments (PCA XZ).
    physics_road_clear();
    const char* road_scx =
        "objects/meshes/roadtest20_egyedi/roadtest20_egyedi.scx";
    std::string road_path = rpak_resolve_path(road_scx);
    if (road_path.empty()) road_path = road_scx;
    const int32_t added = physics_road_add_from_scx(road_path.c_str());
    float scx_x = 0, scx_y = 0, scx_z = 0, scx_dx = 0, scx_dy = 0, scx_dz = 0;
    const bool proj = physics_road_project(0.f, 0.f, &scx_x, &scx_y, &scx_z,
                                           &scx_dx, &scx_dy, &scx_dz);
    InvObject* scx_cross = java_util_resource_GroundRef_getNearestCross(
        map, vec3_new(scx_x, scx_y, scx_z), 0.f);
    float scx_cx = 0, scx_cy = 0, scx_cz = 0;
    if (scx_cross) vec3_get(scx_cross, &scx_cx, &scx_cy, &scx_cz);
    const bool scx_ok =
        added >= 1 && physics_road_count() >= 1 && proj &&
        (std::fabs(scx_dx) + std::fabs(scx_dz)) > 0.5f;
    std::printf("phys roadscx ok=%d added=%d segs=%d dir=(%.2f,%.2f) "
                "cross=(%.1f,%.1f,%.1f) path='%s'\n",
                scx_ok ? 1 : 0, added, physics_road_count(), scx_dx, scx_dz,
                scx_cx, scx_cy, scx_cz, road_path.c_str());
    physics_road_clear();
    if (!scx_ok) {
      std::printf("FAIL road SCX centerline\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.30: city.rpk sourcefile→road*.scx (+ egyedi remap).
    physics_road_clear();
    int32_t rpak_meshes = 0;
    const int32_t rpak_segs =
        physics_road_seed_from_rpak("city.rpk", &rpak_meshes);
    float px = 0, py = 0, pz = 0, pdx = 0, pdy = 0, pdz = 0;
    const bool rpak_proj =
        physics_road_project(400.f, 2300.f, &px, &py, &pz, &pdx, &pdy, &pdz);
    const bool rpak_ok = rpak_meshes >= 1 && rpak_segs >= 1 && rpak_proj &&
                         (std::fabs(pdx) + std::fabs(pdz)) > 0.5f;
    std::printf("phys roadrpak ok=%d meshes=%d segs=%d dir=(%.2f,%.2f) "
                "at=(%.1f,%.1f,%.1f)\n",
                rpak_ok ? 1 : 0, rpak_meshes, rpak_segs, pdx, pdz, px, py, pz);
    physics_road_clear();
    if (!rpak_ok) {
      std::printf("FAIL city RPAK road seed\n");
      render_d3d9_close();
      return 6;
    }
  }
  Jvm jvm;
  const char* game_root = find_game_root();
  if (!game_root) {
    std::printf("FAIL game root not found\n");
    return 3;
  }
  jvm.set_game_root(game_root);
  rpak_set_game_root(game_root);
  jvm_set_active(&jvm);

  if (want_boot) {
    if (!jvm.load_class("java.lang.System")) {
      std::printf("FAIL classpath load System\n");
      return 3;
    }
    return game_boot_run(jvm, game_root, player_name, boot_wait);
  }
  if (want_game) {
    // Stock: WinMain@0x551430 → Engine_boot@0x58C700 → Engine_MainLoop@0x428960.
    // TextureLog_FlushWrite is post-loop teardown (texture.log), not script boot.
    if (!jvm.load_class("java.lang.System")) {
      std::printf("FAIL classpath load System\n");
      return 3;
    }
    // Interactive requires a window; open early if not already (--window smoke).
    if (!want_window && !render_d3d9_ready()) {
      if (!render_d3d9_open(1024, 768, "SLRR - MainMenu")) {
        std::printf("FAIL --game render open\n");
        return 6;
      }
    }
    const int32_t max_frames = boot_wait ? 0 : 60;
    const int rc = game_interactive_run(jvm, game_root, player_name,
                                        game_auto_new, max_frames);
    if (render_d3d9_ready()) render_d3d9_close();
    // Host static maps (WheelRef/world_state/GameRef) outlive JVM heap;
    // CRT teardown can AV on corrupted cross-TU order. Stock process
    // exits via ExitProcess — match that for --game smoke (race167).
    if (!boot_wait) {
      std::printf("EXIT=%d\n", rc);
      std::fflush(stdout);
      ::ExitProcess(rc < 0 ? 1 : static_cast<UINT>(rc));
    }
    return rc;
  }

  if (!jvm.load_class("java.lang.System")) {
    std::printf("FAIL classpath load System\n");
    return 3;
  }
  const JvmClass* sys = jvm.find_class("java.lang.System");
  std::printf("classpath System methods=%zu trees=%zu root=%s\n",
              sys ? sys->methods.size() : 0, sys ? sys->trees.size() : 0,
              game_root);
  if (!sys || sys->methods.size() < 20 || sys->trees.size() < 20) {
    std::printf("FAIL System parse counts\n");
    return 3;
  }

  JvmValue bn = jvm.invoke("java.lang.System", "buildNumber", "()I", {}, true);
  JvmValue ct = jvm.invoke("java.lang.System", "currentTime", "()F", {}, true);
  jvm.invoke("java.lang.System", "log", "(Ljava.lang.String;)V",
             {JvmValue::make_obj(string_new("tufa-smoke"))}, true);
  std::printf("jvm buildNumber=%d currentTime=%.3f\n", bn.v.i, ct.v.f);
  if (bn.tag != JvmTag::Int || bn.v.i != 0x25e) {
    std::printf("FAIL jvm buildNumber\n");
    return 4;
  }

  // Classpath + natives: Math.sqrt, String.length
  if (!jvm.load_class("java.lang.Math") || !jvm.load_class("java.lang.String") ||
      !jvm.load_class("java.lang.Object")) {
    std::printf("FAIL load Math/String/Object\n");
    return 5;
  }
  JvmValue sq = jvm.invoke("java.lang.Math", "sqrt", "(F)F",
                           {JvmValue::make_float(9.f)}, true);
  InvObject* hello = string_new("redline");
  JvmValue sl = jvm.invoke("java.lang.String", "length", "()I",
                           {JvmValue::make_obj(hello)}, false);
  const JvmClass* obj = jvm.find_class("java.lang.Object");
  size_t equals_nodes = 0;
  if (obj) {
    for (auto& m : obj->methods) {
      if (m.name == "equals" && m.tree_index >= 0 &&
          static_cast<size_t>(m.tree_index) < obj->trees.size()) {
        equals_nodes = obj->trees[static_cast<size_t>(m.tree_index)].nodes.size();
      }
    }
  }
  JvmValue eq1 = jvm.invoke("java.lang.Object", "equals", "(Ljava.lang.Object;)I",
                            {JvmValue::make_obj(hello), JvmValue::make_obj(hello)},
                            false);
  JvmValue eq0 = jvm.invoke("java.lang.Object", "equals", "(Ljava.lang.Object;)I",
                            {JvmValue::make_obj(hello),
                             JvmValue::make_obj(string_new("other"))},
                            false);
  std::printf("classpath sqrt=%.1f strlen=%d equals_nodes=%zu eq=%d/%d\n", sq.v.f,
              sl.v.i, equals_nodes, eq1.v.i, eq0.v.i);
  if (sq.tag != JvmTag::Float || sq.v.f < 2.9f || sq.v.f > 3.1f || sl.v.i != 7 ||
      equals_nodes != 9 || eq1.v.i != 1 || eq0.v.i != 0) {
    std::printf("FAIL classpath/tree smoke\n");
    return 5;
  }

  // Integer.setValue / intValue via TREE field ops
  if (!jvm.load_class("java.lang.Integer")) {
    std::printf("FAIL load Integer\n");
    return 5;
  }
  InvObject* box = string_new("int-box");  // stand-in instance handle
  jvm.invoke("java.lang.Integer", "setValue", "(I)V",
             {JvmValue::make_obj(box), JvmValue::make_int(42)}, false);
  JvmValue box_iv = jvm.invoke("java.lang.Integer", "intValue", "()I",
                               {JvmValue::make_obj(box)}, false);
  std::printf("tree Integer.set/get=%d\n", box_iv.v.i);
  if (box_iv.v.i != 42) {
    std::printf("FAIL Integer TREE fields\n");
    return 5;
  }

  // File.delete(path, mask) — TREE with INVOKESTATIC + FindFile loop
  if (!jvm.load_class("java.io.File") || !jvm.load_class("java.io.FindFile")) {
    std::printf("FAIL load File/FindFile\n");
    return 5;
  }
  {
    const char* dir = "tree_invoke_smoke_dir";
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    std::string f1 = std::string(dir) + "/a.tmp";
    std::string f2 = std::string(dir) + "/b.tmp";
    FILE* tf = std::fopen(f1.c_str(), "wb");
    if (tf) {
      std::fputs("x", tf);
      std::fclose(tf);
    }
    tf = std::fopen(f2.c_str(), "wb");
    if (tf) {
      std::fputs("y", tf);
      std::fclose(tf);
    }
    jvm.invoke("java.io.File", "delete", "(Ljava.lang.String;Ljava.lang.String;)V",
               {JvmValue::make_obj(string_new((std::string(dir) + "/").c_str())),
                JvmValue::make_obj(string_new("*.tmp"))},
               true);
    FILE* c1 = std::fopen(f1.c_str(), "rb");
    FILE* c2 = std::fopen(f2.c_str(), "rb");
    const int still1 = c1 ? 1 : 0;
    const int still2 = c2 ? 1 : 0;
    if (c1) std::fclose(c1);
    if (c2) std::fclose(c2);
    std::printf("tree File.delete mask still=%d/%d\n", still1, still2);
    if (still1 || still2) {
      std::printf("FAIL TREE File.delete(path,mask)\n");
      return 5;
    }
#ifdef _WIN32
    _rmdir(dir);
#else
    rmdir(dir);
#endif
  }

  // File.copy(path, mask, path2) — first(path+mask, FILES_ONLY) + dual concat
  {
    const char* src = "tree_copy_src";
    const char* dst = "tree_copy_dst";
#ifdef _WIN32
    _mkdir(src);
    _mkdir(dst);
#else
    mkdir(src, 0755);
    mkdir(dst, 0755);
#endif
    std::string f1 = std::string(src) + "/a.txt";
    std::string f2 = std::string(src) + "/b.txt";
    FILE* tf = std::fopen(f1.c_str(), "wb");
    if (tf) {
      std::fputs("aaa", tf);
      std::fclose(tf);
    }
    tf = std::fopen(f2.c_str(), "wb");
    if (tf) {
      std::fputs("bbb", tf);
      std::fclose(tf);
    }
    jvm.invoke(
        "java.io.File", "copy",
        "(Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;)V",
        {JvmValue::make_obj(string_new((std::string(src) + "/").c_str())),
         JvmValue::make_obj(string_new("*.txt")),
         JvmValue::make_obj(string_new((std::string(dst) + "/").c_str()))},
        true);
    FILE* c1 = std::fopen((std::string(dst) + "/a.txt").c_str(), "rb");
    FILE* c2 = std::fopen((std::string(dst) + "/b.txt").c_str(), "rb");
    const int ok1 = c1 ? 1 : 0;
    const int ok2 = c2 ? 1 : 0;
    if (c1) std::fclose(c1);
    if (c2) std::fclose(c2);
    std::printf("tree File.copy mask dst=%d/%d\n", ok1, ok2);
    if (!ok1 || !ok2) {
      std::printf("FAIL TREE File.copy(path,mask,path2)\n");
      return 5;
    }
    java_io_File_delete(string_new(f1.c_str()));
    java_io_File_delete(string_new(f2.c_str()));
    java_io_File_delete(string_new((std::string(dst) + "/a.txt").c_str()));
    java_io_File_delete(string_new((std::string(dst) + "/b.txt").c_str()));
#ifdef _WIN32
    _rmdir(src);
    _rmdir(dst);
#else
    rmdir(src);
    rmdir(dst);
#endif
  }

  // File.move(path, mask, path2) — same TREE shape as copy
  {
    const char* src = "tree_move_src";
    const char* dst = "tree_move_dst";
#ifdef _WIN32
    _mkdir(src);
    _mkdir(dst);
#else
    mkdir(src, 0755);
    mkdir(dst, 0755);
#endif
    std::string f1 = std::string(src) + "/a.dat";
    std::string f2 = std::string(src) + "/b.dat";
    FILE* tf = std::fopen(f1.c_str(), "wb");
    if (tf) {
      std::fputs("1", tf);
      std::fclose(tf);
    }
    tf = std::fopen(f2.c_str(), "wb");
    if (tf) {
      std::fputs("2", tf);
      std::fclose(tf);
    }
    jvm.invoke(
        "java.io.File", "move",
        "(Ljava.lang.String;Ljava.lang.String;Ljava.lang.String;)V",
        {JvmValue::make_obj(string_new((std::string(src) + "/").c_str())),
         JvmValue::make_obj(string_new("*.dat")),
         JvmValue::make_obj(string_new((std::string(dst) + "/").c_str()))},
        true);
    FILE* left1 = std::fopen(f1.c_str(), "rb");
    FILE* left2 = std::fopen(f2.c_str(), "rb");
    FILE* d1 = std::fopen((std::string(dst) + "/a.dat").c_str(), "rb");
    FILE* d2 = std::fopen((std::string(dst) + "/b.dat").c_str(), "rb");
    const int moved =
        (!left1 && !left2 && d1 && d2) ? 1 : 0;
    if (left1) std::fclose(left1);
    if (left2) std::fclose(left2);
    if (d1) std::fclose(d1);
    if (d2) std::fclose(d2);
    std::printf("tree File.move mask moved=%d\n", moved);
    if (!moved) {
      std::printf("FAIL TREE File.move(path,mask,path2)\n");
      return 5;
    }
    java_io_File_delete(string_new((std::string(dst) + "/a.dat").c_str()));
    java_io_File_delete(string_new((std::string(dst) + "/b.dat").c_str()));
#ifdef _WIN32
    _rmdir(src);
    _rmdir(dst);
#else
    rmdir(src);
    rmdir(dst);
#endif
  }

  // System.rpkScan(dir) — TREE: FindFile + openLib + IINC
  if (!jvm.load_class("java.lang.System")) {
    std::printf("FAIL load System\n");
    return 5;
  }
  {
    const char* dir = "tree_rpk_scan";
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    std::string r1 = std::string(dir) + "/a.rpk";
    std::string r2 = std::string(dir) + "/b.rpk";
    auto write_min_rpak = [](const char* path) {
      // Minimal valid RPAK header (registry-style, packs=0, entries=0).
      unsigned char hdr[24] = {
          'R', 'P', 'A', 'K', 0x00, 0x02, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0,
          0, 0, 0, 0, 0, 0, 0, 0};
      FILE* tf = std::fopen(path, "wb");
      if (tf) {
        std::fwrite(hdr, 1, sizeof(hdr), tf);
        std::fclose(tf);
      }
    };
    write_min_rpak(r1.c_str());
    write_min_rpak(r2.c_str());
    JvmValue found = jvm.invoke("java.lang.System", "rpkScan",
                                "(Ljava.lang.String;)I",
                                {JvmValue::make_obj(
                                    string_new((std::string(dir) + "/").c_str()))},
                                true);
    std::printf("tree System.rpkScan found=%d\n", found.v.i);
    if (found.tag != JvmTag::Int || found.v.i != 2) {
      std::printf("FAIL TREE System.rpkScan\n");
      return 5;
    }
    java_io_File_delete(string_new(r1.c_str()));
    java_io_File_delete(string_new(r2.c_str()));
#ifdef _WIN32
    _rmdir(dir);
#else
    rmdir(dir);
#endif
  }

  // Real RPAK openLib — system.rpk (registry) + frontend.rpk (file index)
  {
    const int32_t sys_id = java_lang_System_openLib(string_new("system.rpk"));
    const RpakPack* sys = rpak_get(sys_id);
    std::printf("rpak system.rpk id=%d registry=%d ver=%u\n", sys_id,
                sys && sys->is_registry ? 1 : 0, sys ? sys->version : 0);
    if (sys_id == 0 || !sys || !sys->is_registry) {
      std::printf("FAIL openLib system.rpk\n");
      return 5;
    }
    const int32_t fe_id = java_lang_System_openLib(string_new("frontend.rpk"));
    const RpakPack* fe = rpak_get(fe_id);
    size_t files = 0;
    if (fe) {
      for (const auto& e : fe->entries)
        if (!e.is_dir) ++files;
    }
    std::printf("rpak frontend.rpk id=%d entries=%zu files=%zu deps=%zu\n",
                fe_id, fe ? fe->entries.size() : 0, files,
                fe ? fe->deps.size() : 0);
    if (fe_id == 0 || !fe || !fe->parsed_entries || files == 0) {
      std::printf("FAIL openLib frontend.rpk\n");
      return 5;
    }
    if (java_lang_System_openLib(string_new("frontend.rpk")) != fe_id) {
      std::printf("FAIL openLib idempotent\n");
      return 5;
    }

    // ResourceRef: frontend:0x01 = RID_MENULIGHT (type_id, not table index)
    const int32_t rid = rpak_make_id(fe_id, 0x0001);
    const RpakEntry* ent = rpak_find_entry(rid);
    InvObject* rr_fe = resref_new();
    java_util_resource_ResourceRef_set(rr_fe, rid);
    java_util_resource_ResourceRef_load(rr_fe);
    const int32_t got_id = java_util_resource_ResourceRef_id(rr_fe);
    const int32_t got_ty = java_util_resource_ResourceRef_type(rr_fe);
    std::printf("resref frontend:0x1 id=0x%X type=%d entry='%s' sz=%u\n", got_id,
                got_ty, ent ? ent->path.c_str() : "?", ent ? ent->size : 0);
    if (got_id != rid || !ent || ent->name != "menulight" || ent->size == 0) {
      std::printf("FAIL ResourceRef↔rpak bind\n");
      return 5;
    }
    std::vector<uint8_t> blob;
    if (!rpak_read_entry(rid, &blob) || blob.empty()) {
      std::printf("FAIL rpak_read_entry\n");
      return 5;
    }

    // Parent/child: fonts (0x8) → console/simple/… siblings
    const int32_t fonts_id = rpak_make_id(fe_id, 0x0008);
    InvObject* fonts = resref_new();
    java_util_resource_ResourceRef_set(fonts, fonts_id);
    int child_count = 0;
    InvObject* loop = java_util_resource_ResourceRef_getFirstChild(fonts);
    InvObject* first = loop;
    while (loop) {
      ++child_count;
      InvObject* parent = java_util_resource_ResourceRef_getParent(loop);
      if (!parent ||
          java_util_resource_ResourceRef_id(parent) != fonts_id) {
        std::printf("FAIL child parent link at %d\n", child_count);
        return 5;
      }
      loop = java_util_resource_ResourceRef_getNextChild(loop);
    }
    std::printf("resref tree fonts children=%d first=0x%X\n", child_count,
                first ? java_util_resource_ResourceRef_id(first) : 0);
    if (child_count < 5) {
      std::printf("FAIL expected frontend fonts children\n");
      return 5;
    }

    // GameRef RID bind shares ResourceRef id + clears empty
    InvObject* gr_fe = gameref_new();
    java_util_resource_ResourceRef_set(gr_fe, rid);
    std::printf("gameref rid empty=%d id=0x%X\n",
                java_util_resource_GameRef_isEmpty(gr_fe),
                java_util_resource_ResourceRef_id(gr_fe));
    if (java_util_resource_GameRef_isEmpty(gr_fe) ||
        java_util_resource_ResourceRef_id(gr_fe) != rid) {
      std::printf("FAIL GameRef RID bind\n");
      return 5;
    }

    // RenderRef.create(parent, type) copies type resource id
    InvObject* rr_type = resref_new();
    java_util_resource_ResourceRef_set(rr_type, rid);
    InvObject* rr_inst = resref_new();
    java_util_resource_RenderRef_create(rr_inst, fonts, rr_type, nullptr);
    std::printf("renderref create id=0x%X type=%d parent=0x%X\n",
                java_util_resource_ResourceRef_id(rr_inst),
                java_util_resource_ResourceRef_type(rr_inst),
                java_util_resource_ResourceRef_getParentID(rr_inst));
    if (java_util_resource_ResourceRef_id(rr_inst) != rid ||
        java_util_resource_ResourceRef_getParentID(rr_inst) != fonts_id ||
        java_util_resource_ResourceRef_type(rr_inst) != 3) {
      std::printf("FAIL RenderRef.create bind\n");
      return 5;
    }
  }

  // parts.rpk type-tree + Catalog-style parts:0xF227r ("Main")
  {
    const int32_t parts_id = java_lang_System_openLib(string_new("parts.rpk"));
    const RpakPack* parts = rpak_get(parts_id);
    size_t type_nodes = 0;
    if (parts) {
      for (const auto& e : parts->entries)
        if (!e.is_dir && static_cast<uint32_t>(e.type_id) >= 0xF000u)
          ++type_nodes;
    }
    std::printf("rpak parts.rpk id=%d entries=%zu typeF=%zu registry=%d\n",
                parts_id, parts ? parts->entries.size() : 0, type_nodes,
                parts && parts->is_registry ? 1 : 0);
    if (parts_id == 0 || !parts || !parts->parsed_entries ||
        parts->entries.size() < 500 || type_nodes < 20) {
      std::printf("FAIL parts.rpk type-tree parse\n");
      return 5;
    }
    const int32_t rid = rpak_make_id(parts_id, 0xF227);
    const RpakEntry* ent = rpak_find_entry(rid);
    InvObject* rr = resref_new();
    java_util_resource_ResourceRef_set(rr, rid);
    std::printf("parts:0xF227 type_id=0x%X kind=0x%X name='%s' id=0x%X\n",
                ent ? ent->type_id : 0, ent ? ent->kind : 0,
                ent ? ent->name.c_str() : "?",
                java_util_resource_ResourceRef_id(rr));
    if (!ent || ent->type_id != 0xF227 || ent->name != "Main" ||
        java_util_resource_ResourceRef_id(rr) != rid) {
      std::printf("FAIL parts:0xF227r lookup\n");
      return 5;
    }
    // Catalog collectObjects walk: children of Main (blocks, camshafts, …)
    InvObject* root = resref_new();
    java_util_resource_ResourceRef_set(root, rid);
    int kids = 0;
    for (InvObject* c = java_util_resource_ResourceRef_getFirstChild(root); c;
         c = java_util_resource_ResourceRef_getNextChild(c)) {
      ++kids;
      if (java_util_resource_ResourceRef_getParentID(c) != rid) {
        std::printf("FAIL parts child parent\n");
        return 5;
      }
    }
    std::printf("parts:0xF227 children=%d\n", kids);
    if (kids < 5) {
      std::printf("FAIL parts:0xF227 expected category children\n");
      return 5;
    }
  }

  // GameLogic boot-style rpkScan of stock directories (cwd-relative → game_root).
  {
    const size_t before = rpak_count();
    const char* dirs[] = {"parts/engines/", "parts/", "cars/racers/",
                          "cars/fake_racers/", "maps/"};
    int total_found = 0;
    for (const char* d : dirs) {
      JvmValue n = jvm.invoke("java.lang.System", "rpkScan",
                              "(Ljava.lang.String;)I",
                              {JvmValue::make_obj(string_new(d))}, true);
      const int got = (n.tag == JvmTag::Int) ? n.v.i : -1;
      total_found += (got > 0) ? got : 0;
      std::printf("boot rpkScan '%s' -> %d\n", d, got);
      if (got < 0) {
        std::printf("FAIL boot rpkScan %s\n", d);
        return 5;
      }
    }
    const size_t after = rpak_count();
    std::printf("boot rpkScan total_found=%d packs %zu->%zu\n", total_found,
                before, after);
    if (total_found < 10 || after <= before) {
      std::printf("FAIL boot rpkScan expected many packs\n");
      return 5;
    }
    // Spot-check a known map pack basename if present on disk.
    if (FILE* f = std::fopen(
            (std::string(game_root) + "/maps/city.rpk").c_str(), "rb")) {
      std::fclose(f);
      if (!rpak_find_by_name("city.rpk") && !rpak_find_by_name("city")) {
        std::printf("FAIL city.rpk not registered after scan\n");
        return 5;
      }
    }

    // parts/wheels.rpk (via parts/ scan) — type_id tree
    const RpakPack* wheels = rpak_find_by_name("wheels.rpk");
    if (!wheels) wheels = rpak_find_by_name("wheels");
    if (!wheels || !wheels->parsed_entries || wheels->entries.size() < 100) {
      std::printf("FAIL wheels.rpk after parts/ scan\n");
      return 5;
    }
    const RpakEntry* rim = nullptr;
    for (const auto& e : wheels->entries) {
      if (!e.is_dir && e.name == "rim") {
        rim = &e;
        break;
      }
    }
    std::printf("rpak wheels.rpk id=%d entries=%zu rim type_id=0x%X\n",
                wheels->pack_id, wheels->entries.size(),
                rim ? rim->type_id : -1);
    if (!rim || !rpak_find_entry(rpak_make_id(
                    wheels->pack_id, static_cast<uint16_t>(rim->type_id)))) {
      std::printf("FAIL wheels rim type_id lookup\n");
      return 5;
    }
  }

  // GameLogic boot slice: cars/humans RIDs + TREE preCacheGametypes
  {
    const int32_t cars_id = java_lang_System_openLib(string_new("cars.rpk"));
    const int32_t humans_id = java_lang_System_openLib(string_new("humans.rpk"));
    const RpakEntry* veh = rpak_find_entry(rpak_make_id(cars_id, 0x1000));
    const RpakEntry* traffic = rpak_find_entry(rpak_make_id(cars_id, 0x4));
    const RpakEntry* humans = rpak_find_entry(rpak_make_id(humans_id, 0x1));
    std::printf("boot cars:0x1000='%s' cars:0x4='%s' humans:0x1='%s'\n",
                veh ? veh->name.c_str() : "?", traffic ? traffic->name.c_str() : "?",
                humans ? humans->name.c_str() : "?");
    if (!veh || veh->name != "vehicle" || !traffic || !humans) {
      std::printf("FAIL GameLogic RID roots\n");
      return 5;
    }

    // Native walk mirroring preCacheGametypes(cars:0x4)
    InvObject* root = resref_new();
    java_util_resource_ResourceRef_set(root, rpak_make_id(cars_id, 0x4));
    int cached = 0;
    for (InvObject* c = java_util_resource_ResourceRef_getFirstChild(root); c;
         c = java_util_resource_ResourceRef_getNextChild(c)) {
      java_util_resource_ResourceRef_cache(c);
      ++cached;
    }
    std::printf("boot preCache native cars:0x4 children=%d\n", cached);
    if (cached < 1) {
      std::printf("FAIL cars:0x4 children\n");
      return 5;
    }

    if (!jvm.load_class("java.game.GameLogic")) {
      std::printf("FAIL load GameLogic.class\n");
      return 5;
    }
    const JvmClass* gl = jvm.find_class("java.game.GameLogic");
    std::printf("classpath GameLogic methods=%zu trees=%zu\n",
                gl ? gl->methods.size() : 0, gl ? gl->trees.size() : 0);
    const char* precache_sig = nullptr;
    if (gl) {
      for (const auto& m : gl->methods) {
        if (m.name == "preCacheGametypes") {
          std::printf("  method preCacheGametypes sig='%s' tree=%d\n",
                      m.signature.c_str(), m.tree_index);
          precache_sig = m.signature.c_str();
        }
      }
    }
    if (!precache_sig) {
      std::printf("FAIL GameLogic.preCacheGametypes missing\n");
      return 5;
    }

    InvObject* humans_root = resref_new();
    java_util_resource_ResourceRef_set(humans_root,
                                       rpak_make_id(humans_id, 0x1));
    jvm.invoke("java.game.GameLogic", "preCacheGametypes", precache_sig,
               {JvmValue::make_obj(humans_root)}, true);
    InvObject* hchild =
        java_util_resource_ResourceRef_getFirstChild(humans_root);
    std::printf("boot TREE preCacheGametypes humans child=0x%X\n",
                hchild ? java_util_resource_ResourceRef_id(hchild) : 0);
    if (!hchild) {
      std::printf("FAIL humans:0x1 child after preCache\n");
      return 5;
    }

    // VEHICLETYPE_ROOT cars:0x1000 → cross-pack *_VT children (after rpkScan)
    InvObject* vt_root = resref_new();
    java_util_resource_ResourceRef_set(vt_root, rpak_make_id(cars_id, 0x1000));
    int vt_kids = 0;
    int vt_named = 0;
    for (InvObject* c = java_util_resource_ResourceRef_getFirstChild(vt_root); c;
         c = java_util_resource_ResourceRef_getNextChild(c)) {
      ++vt_kids;
      const int32_t cid = java_util_resource_ResourceRef_id(c);
      const RpakEntry* ce = rpak_find_entry(cid);
      if (ce && ce->name.size() >= 3 &&
          ce->name.find("_VT") != std::string::npos)
        ++vt_named;
      const int32_t pid = java_util_resource_ResourceRef_getParentID(c);
      if (pid != rpak_make_id(cars_id, 0x1000)) {
        std::printf("FAIL VT parent id=0x%X want cars:0x1000\n", pid);
        return 5;
      }
    }
    std::printf("boot cars:0x1000 VehicleType children=%d named_VT=%d\n",
                vt_kids, vt_named);
    if (vt_kids < 9 || vt_named < 9) {
      std::printf("FAIL expected >=9 *_VT under cars:0x1000\n");
      return 5;
    }

    // GameLogic.initVehicleTypes: create+init all *_VT under cars:0x1000
    JvmValue ivt =
        jvm.invoke("java.game.GameLogic", "initVehicleTypes", "()V", {}, true);
    InvObject* vts = game_logic_vehicle_types();
    const int nvt = tree_vector_size(vts);
    int scripted = 0;
    int with_mask = 0;
    int with_models = 0;
    for (int i = 0; i < nvt; ++i) {
      InvObject* vt = tree_vector_element_at(vts, i);
      if (!vt) continue;
      if (java_util_resource_GameRef_isScripted(
              vt, string_new("java.game.VehicleType")))
        ++scripted;
      if (tree_field_get_int(vt, "vehicleSetMask") != 0) ++with_mask;
      if (tree_vector_size(tree_field_get_obj(vt, "vtdarr")) > 0) ++with_models;
      if (i == 0 || tree_host_class(vt)) {
        std::printf("  VT[%d] class='%s' id=0x%X mask=0x%X models=%d\n", i,
                    tree_host_class(vt), java_util_resource_ResourceRef_id(vt),
                    tree_field_get_int(vt, "vehicleSetMask"),
                    tree_vector_size(tree_field_get_obj(vt, "vtdarr")));
      }
    }
    std::printf(
        "boot initVehicleTypes n=%d scripted=%d masked=%d modeled=%d\n", nvt,
        scripted, with_mask, with_models);
    if (nvt < 9 || scripted < 9 || with_mask < 9 || with_models < 9) {
      std::printf("FAIL initVehicleTypes\n");
      return 5;
    }

    // Probe Baiern models: every entry needs non-zero id + name
    int baiern_bad = 0;
    for (int i = 0; i < nvt; ++i) {
      InvObject* vt = tree_vector_element_at(vts, i);
      if (!vt || !tree_host_class(vt) ||
          !std::strstr(tree_host_class(vt), "Baiern"))
        continue;
      InvObject* arr = tree_field_get_obj(vt, "vtdarr");
      for (int j = 0; j < tree_vector_size(arr); ++j) {
        InvObject* m = tree_vector_element_at(arr, j);
        const char* nm =
            string_cstr(tree_field_get_obj(m, "vehicleName"));
        const int32_t mid = tree_field_get_int(m, "id");
        const int32_t mask = tree_field_get_int(m, "vehicleSetMask");
        if (!nm || !nm[0] || mid == 0 || mask == 0) {
          ++baiern_bad;
          std::printf("  Baiern BAD model[%d] id=0x%X name='%s' mask=0x%X prev=%.1f\n",
                      j, mid, nm ? nm : "?", mask,
                      tree_field_get_float(m, "prevalence"));
        }
      }
      std::printf("boot Baiern models ok=%d bad=%d\n",
                  tree_vector_size(arr) - baiern_bad, baiern_bad);
      break;
    }
    if (baiern_bad) {
      std::printf("FAIL Baiern VehicleModel id/mask\n");
      return 5;
    }

    // getVehicleDescriptor(VS_DEMO, 0.5) / (VS_USED, -1)
    constexpr int32_t VS_DEMO = 0x0001;
    constexpr int32_t VS_USED = 0x0002;
    JvmValue vd0 = jvm.invoke(
        "java.game.GameLogic", "getVehicleDescriptor", "(IF)L;",
        {JvmValue::make_int(VS_DEMO), JvmValue::make_float(0.5f)}, true);
    JvmValue vd1 = jvm.invoke("java.game.GameLogic", "getVehicleDescriptor",
                              "(I)L;", {JvmValue::make_int(VS_USED)}, true);
    InvObject* d0 = vd0.tag == JvmTag::Obj ? vd0.v.o : nullptr;
    InvObject* d1 = vd1.tag == JvmTag::Obj ? vd1.v.o : nullptr;
    const char* n0 =
        d0 ? string_cstr(tree_field_get_obj(d0, "vehicleName")) : "?";
    const char* n1 =
        d1 ? string_cstr(tree_field_get_obj(d1, "vehicleName")) : "?";
    const float p0 = d0 ? tree_field_get_float(d0, "power") : -1.f;
    const float o0 = d0 ? tree_field_get_float(d0, "optical") : -1.f;
    std::printf(
        "boot getVehicleDescriptor DEMO name='%s' id=%d power=%.2f optical=%.2f "
        "USED name='%s' id=%d\n",
        n0 ? n0 : "?", d0 ? tree_field_get_int(d0, "id") : -1, p0, o0,
        n1 ? n1 : "?", d1 ? tree_field_get_int(d1, "id") : -1);
    if (!d0 || !d1 || p0 < 0.05f || p0 > 5.f || o0 < 0.05f || o0 > 5.f) {
      std::printf("FAIL getVehicleDescriptor\n");
      return 5;
    }
    if (!n0 || !n0[0] || std::strcmp(n0, "unknown") == 0 ||
        tree_field_get_int(d0, "id") == 0) {
      std::printf("FAIL getVehicleDescriptor id/name\n");
      return 5;
    }

    // Boot slice: Input controller + host Player/Garage (GameLogic ctor mid)
    InvObject* player = game_logic_boot_player_garage();
    InvObject* garage = game_logic_garage();
    InvObject* ctrl = player ? tree_field_get_obj(player, "controller") : nullptr;
    const int32_t active = input_is_player_active(0);
    const int32_t cid = ctrl ? java_util_resource_ResourceRef_id(ctrl) : 0;
    std::printf(
        "boot player/garage player=%s garage=%s active=%d controller_id=0x%X\n",
        player && tree_host_class(player) ? tree_host_class(player) : "?",
        garage && tree_host_class(garage) ? tree_host_class(garage) : "?",
        active, cid);
    if (!player || !garage || !active || cid == 0) {
      std::printf("FAIL player/garage boot\n");
      return 5;
    }

    // MENUSET + loadingScreen.hide + SplashScreen(frontend:0x5)
    constexpr int32_t MENUSET = 2;
    InvObject* splash = game_logic_boot_splash();
    const int32_t def_on = controller_css_get(ctrl, 0);
    const int32_t menu_on = controller_css_get(ctrl, MENUSET);
    const int32_t loading_vis = frontend_loading_screen_visible();
    InvObject* state = game_logic_actual_state();
    InvObject* pic = splash ? tree_field_get_obj(splash, "pic") : nullptr;
    const int32_t pic_id = pic ? java_util_resource_ResourceRef_id(pic) : 0;
    const int32_t entered =
        splash ? tree_field_get_int(splash, "entered") : 0;
    const int32_t via_tree =
        splash ? tree_field_get_int(splash, "enter_via_tree") : 0;
    const int32_t event_mask =
        splash ? tree_field_get_int(splash, "event_mask") : 0;
    const int32_t timer_count =
        splash ? tree_field_get_int(splash, "timer_count") : 0;
    InvObject* osd = splash ? tree_field_get_obj(splash, "osd") : nullptr;
    const int32_t osd_vis = osd ? tree_field_get_int(osd, "visible") : 0;
    const int32_t bg_ok = osd ? tree_field_get_int(osd, "bg_created") : 0;
    const int32_t hk_n = osd ? tree_field_get_int(osd, "hotkey_count") : 0;
    InvObject* ls0 = frontend_loading_screen();
    const int32_t ls_hide =
        ls0 ? tree_field_get_int(ls0, "hide_count") : 0;
    const int32_t ls_hide_host =
        ls0 ? tree_field_get_int(ls0, "hide_via_host") : 0;
    std::printf(
        "boot splash state='%s' entered=%d via_tree=%d event_mask=%d "
        "timers=%d osd=%d vis=%d bg=%d hk=%d pic=0x%X DEFAULTSET=%d "
        "MENUSET=%d loading_vis=%d ls_hide=%d\n",
        state && tree_host_class(state) ? tree_host_class(state) : "?", entered,
        via_tree, event_mask, timer_count, osd ? 1 : 0, osd_vis, bg_ok, hk_n,
        pic_id, def_on, menu_on, loading_vis, ls_hide);
    if (!splash || state != splash || entered != 1 || via_tree != 1 ||
        event_mask != 0x80 || timer_count < 1 || !osd || bg_ok != 1 ||
        pic_id == 0 || def_on != 1 || menu_on != 1 || loading_vis != 0 ||
        ls_hide < 1 || ls_hide_host != 1) {
      std::printf("FAIL splash/MENUSET boot\n");
      return 5;
    }

    // OptionsDialog.show TREE (after splash — show mutates Frontend music).
    if (!jvm.load_class("java.game.OptionsDialog")) {
      std::printf("FAIL load OptionsDialog\n");
      return 5;
    }
    {
      InvObject* dlg = tree_host_new("java.game.OptionsDialog");
      InvObject* osd_opt = tree_host_new("java.render.Osd");
      tree_field_set_obj(dlg, "osd", osd_opt);
      tree_field_set_int(osd_opt, "text_count", 0);
      options_dialog_ensure_groups(dlg);
      const int32_t via = tree_field_get_int(dlg, "options_via_tree");
      const int32_t opt = tree_field_get_int(dlg, "optionsGroup");
      const int32_t g2 = tree_field_get_int(dlg, "game2Group");
      std::printf("options TREE via=%d optGroup=%d game2=%d\n", via, opt, g2);
      if (!via || opt < 0 || g2 < 0) {
        std::printf("FAIL OptionsDialog.show TREE incomplete\n");
        return 5;
      }
    }

    // ControlSet.load fixture (no save/controls in install)
    constexpr int32_t CTRLFILEID = 0x4c525443;
    constexpr int32_t CTRLFILEVERSION = 16;
    constexpr int NCONTROLS = 58 + 8;
    const char* ctrl_path = "native_engine_controls.tmp";
    {
      InvObject* wf = file_new(ctrl_path);
      if (!java_io_File_open(wf, 1)) {
        std::printf("FAIL control set write open\n");
        return 5;
      }
      java_io_File_write(wf, CTRLFILEID);
      java_io_File_write(wf, CTRLFILEVERSION);
      java_io_File_write(wf, 2);  // devices
      java_io_File_write_3(wf, string_new("Keyboard"));
      java_io_File_write_3(wf, string_new("Mouse"));
      java_io_File_write(wf, NCONTROLS);
      for (int i = 0; i < NCONTROLS; ++i) {
        // Sprinkle MENUSET (2) bindings like a real menu control set.
        java_io_File_write(wf, (i % 5 == 2) ? MENUSET : 0);
        java_io_File_write(wf, i);      // vaxis
        java_io_File_write(wf, 0);      // device idx
        java_io_File_write(wf, i);      // axis
        java_io_File_write_1(wf, 0.f);
        java_io_File_write_1(wf, 1.f);
        java_io_File_write_1(wf, 0.f);
        java_io_File_write_1(wf, 1.f);
        java_io_File_write_1(wf, 0.f);
      }
      java_io_File_write(wf, 0);  // vasp count
      java_io_File_close(wf);
    }
    if (!control_set_file_check(ctrl_path)) {
      std::printf("FAIL control_set_file_check\n");
      return 5;
    }
    InvObject* cs = control_set_new();
    if (!control_set_load(cs, ctrl_path)) {
      std::printf("FAIL control_set_load\n");
      return 5;
    }
    const int32_t menu_binds = control_set_count_group(cs, MENUSET);
    std::printf("boot ControlSet devices=%d items=%d MENUSET_binds=%d\n",
                control_set_ndevices(cs), control_set_nitems(cs), menu_binds);
    if (control_set_ndevices(cs) != 2 || control_set_nitems(cs) != NCONTROLS ||
        menu_binds < 10) {
      std::printf("FAIL ControlSet load\n");
      return 5;
    }
    std::remove(ctrl_path);

    // Splash timer / cancel → MainMenu (SplashScreen.exit + MainMenu.enter TREE)
    InvObject* menu = game_logic_finish_splash();
    InvObject* cur = game_logic_actual_state();
    InvObject* mmd = menu ? tree_field_get_obj(menu, "mmd") : nullptr;
    const int32_t mmd_shown = mmd ? tree_field_get_int(mmd, "shown") : 0;
    const int32_t menu_via_tree =
        menu ? tree_field_get_int(menu, "enter_via_tree") : 0;
    const int32_t splash_left =
        splash ? tree_field_get_int(splash, "entered") : -1;
    const int32_t splash_exit_tree =
        splash ? tree_field_get_int(splash, "exit_via_tree") : 0;
    const int32_t splash_mask =
        splash ? tree_field_get_int(splash, "event_mask") : -1;
    InvObject* splash_osd =
        splash ? tree_field_get_obj(splash, "osd") : nullptr;
    const int32_t splash_osd_vis =
        splash_osd ? tree_field_get_int(splash_osd, "visible") : -1;
    const int32_t splash_osd_tree =
        splash ? tree_field_get_int(splash, "osd_cmd_via_tree") : 0;
    const int32_t splash_osd_cmd =
        splash ? tree_field_get_int(splash, "last_osd_cmd") : -1;
    const int32_t splash_he_tree =
        splash ? tree_field_get_int(splash, "handle_event_via_tree") : 0;
    std::printf(
        "boot MainMenu state='%s' via_tree=%d mmd_shown=%d splash_entered=%d "
        "splash_exit_tree=%d splash_mask=%d splash_osd_vis=%d "
        "he_tree=%d osd_cmd_tree=%d osd_cmd=%d\n",
        cur && tree_host_class(cur) ? tree_host_class(cur) : "?", menu_via_tree,
        mmd_shown, splash_left, splash_exit_tree, splash_mask, splash_osd_vis,
        splash_he_tree, splash_osd_tree, splash_osd_cmd);
    if (!menu || cur != menu || menu_via_tree != 1 || mmd_shown != 1 ||
        splash_left != 0 || splash_exit_tree != 1 || splash_mask != 0 ||
        splash_osd_vis != 0 || splash_he_tree != 1 || splash_osd_tree != 1 ||
        splash_osd_cmd != 35) {
      std::printf("FAIL MainMenu transition\n");
      return 5;
    }

    // WarningDialog → TextDialog.show → Dialog.show (osd.dialog TREE path)
    {
      InvObject* ctrl = player ? tree_field_get_obj(player, "controller") : nullptr;
      InvObject* wd =
          tree_host_new("java.render.osd.dialog.WarningDialog");
      constexpr int32_t kDfModal = 0x1;
      constexpr int32_t kDfDefaultBg = 0x4;
      std::vector<JvmValue> init_args = {
          JvmValue::make_obj(wd),
          JvmValue::make_obj(ctrl),
          JvmValue::make_int(kDfModal | kDfDefaultBg),
          JvmValue::make_obj(string_new("WARN")),
          JvmValue::make_obj(string_new("hello dialog tree")),
      };
      jvm.invoke("java.render.osd.dialog.WarningDialog", "<init>",
                 "(Ljava.io.Controller;ILjava.lang.String;Ljava.lang.String;)V",
                 init_args, false);
      // Phase 2.124: Dialog.show enables Input.cursor unless DF_LEAVEPOINTER.
      InvObject* cursor = java_io_Input_cursor();
      java_io_MouseCursor_enable(cursor, 0);
      osd_create_button_calls_reset();
      std::vector<JvmValue> show_args = {JvmValue::make_obj(wd)};
      jvm.invoke("java.render.osd.dialog.WarningDialog", "show", "()V",
                 show_args, false);
      const int32_t shown = tree_field_get_int(wd, "shown");
      const int32_t via = tree_field_get_int(wd, "show_via_tree");
      InvObject* osd = tree_field_get_obj(wd, "osd");
      const int32_t osd_vis = osd ? tree_field_get_int(osd, "visible") : 0;
      const int32_t ptr_on = tree_field_get_int(cursor, "visible");
      const int32_t prev_ptr = tree_field_get_int(wd, "prevPointerState");
      const int32_t bg_created = osd ? tree_field_get_int(osd, "bg_created") : 0;
      const int32_t btn_n = osd ? tree_field_get_int(osd, "button_count") : 0;
      const int32_t btn_tree = osd_create_button_calls();
      InvObject* btns = osd ? tree_field_get_obj(osd, "buttons") : nullptr;
      int32_t ok_cmd = -1;
      if (btns) {
        for (int32_t bi = 0; bi < tree_vector_size(btns); ++bi) {
          InvObject* b = tree_vector_element_at(btns, bi);
          if (b && tree_field_get_int(b, "command") == 0) ok_cmd = 0;
        }
      }
      InvObject* shield = tree_field_get_obj(wd, "backShieldOsd");
      const int32_t shield_n =
          shield ? tree_field_get_int(shield, "button_count") : 0;
      const int32_t darken = tree_field_get_int(wd, "darken_applied");
      const int32_t title_n = osd ? tree_field_get_int(osd, "text_count") : 0;
      InvObject* title_txt = osd ? tree_field_get_obj(osd, "title_text") : nullptr;
      const int32_t hk_n = osd ? tree_field_get_int(osd, "hotkey_count") : 0;
      const int32_t shield_dark =
          shield ? tree_field_get_int(shield, "darken_blit") : 0;
      // Phase 2.152: DF_DARKEN + title Text + AXIS_CANCEL hotkey.
      const bool dialog_osd_ok =
          bg_created == 1 && btn_n >= 1 && ok_cmd == 0 && btn_tree >= 1 &&
          osd && tree_field_get_obj(osd, "bg") != nullptr &&
          (shield_n >= 1 || btn_n >= 2) && darken == 1 && title_txt &&
          title_n >= 1 && hk_n >= 1 && shield_dark == 1;
      std::printf("boot WarningDialog show via_tree=%d shown=%d osd_vis=%d "
                  "ptr=%d prev=%d\n",
                  via, shown, osd_vis, ptr_on, prev_ptr);
      std::printf("boot dialog_osd ok=%d bg=%d buttons=%d shield=%d ok_cmd=%d "
                  "createButton_tree=%d darken=%d title=%d hotkey=%d\n",
                  dialog_osd_ok ? 1 : 0, bg_created, btn_n, shield_n, ok_cmd,
                  btn_tree, darken, title_txt ? 1 : 0, hk_n);
      if (shown != 1 || via != 1 || osd_vis != 1 || ptr_on != 1 ||
          prev_ptr != 0) {
        std::printf("FAIL WarningDialog.show TREE\n");
        return 5;
      }
      if (!dialog_osd_ok) {
        std::printf("FAIL dialog_osd BG/buttons/darken/title\n");
        return 5;
      }
      jvm.invoke("java.render.osd.dialog.WarningDialog", "hide", "()V",
                 show_args, false);
      const int32_t hid = tree_field_get_int(wd, "hide_via_tree");
      const int32_t shown2 = tree_field_get_int(wd, "shown");
      const int32_t ptr_off = tree_field_get_int(cursor, "visible");
      std::printf("boot WarningDialog hide via_tree=%d shown=%d ptr=%d\n", hid,
                  shown2, ptr_off);
      if (hid != 1 || shown2 != 0 || ptr_off != 0) {
        std::printf("FAIL WarningDialog.hide TREE\n");
        return 5;
      }

      // DF_LEAVEPOINTER: show/hide must not touch Input.cursor.visible.
      constexpr int32_t kDfLeavePointer = 0x10;
      InvObject* wd2 =
          tree_host_new("java.render.osd.dialog.WarningDialog");
      std::vector<JvmValue> init2 = {
          JvmValue::make_obj(wd2),
          JvmValue::make_obj(ctrl),
          JvmValue::make_int(kDfModal | kDfDefaultBg | kDfLeavePointer),
          JvmValue::make_obj(string_new("LEAVE")),
          JvmValue::make_obj(string_new("leave pointer")),
      };
      jvm.invoke("java.render.osd.dialog.WarningDialog", "<init>",
                 "(Ljava.io.Controller;ILjava.lang.String;Ljava.lang.String;)V",
                 init2, false);
      java_io_MouseCursor_enable(cursor, 0);
      std::vector<JvmValue> show2 = {JvmValue::make_obj(wd2)};
      jvm.invoke("java.render.osd.dialog.WarningDialog", "show", "()V", show2,
                 false);
      const int32_t leave_show_vis = tree_field_get_int(cursor, "visible");
      jvm.invoke("java.render.osd.dialog.WarningDialog", "hide", "()V", show2,
                 false);
      const int32_t leave_hide_vis = tree_field_get_int(cursor, "visible");
      const bool leave_ok = tree_field_get_int(wd2, "show_via_tree") == 1 &&
                            leave_show_vis == 0 && leave_hide_vis == 0;
      std::printf("boot DF_LEAVEPOINTER ok=%d show_ptr=%d hide_ptr=%d\n",
                  leave_ok ? 1 : 0, leave_show_vis, leave_hide_vis);
      if (!leave_ok) {
        std::printf("FAIL DF_LEAVEPOINTER\n");
        return 5;
      }
    }

    // MainMenuDialog CMD_NEW via JVM → Garage (MainMenu.exit via TREE)
    InvObject* mmd_before = menu ? tree_field_get_obj(menu, "mmd") : nullptr;
    InvObject* garage2 = main_menu_cmd_new("Elias");
    InvObject* after = game_logic_actual_state();
    const char* pname =
        string_cstr(tree_field_get_obj(player, "name"));
    const int32_t money = tree_field_get_int(player, "money");
    const int32_t gidx =
        garage2 ? tree_field_get_int(garage2, "garageIndex") : -1;
    const int32_t map_id =
        garage2 ? tree_field_get_int(garage2, "map_id") : 0;
    const int32_t g_entered =
        garage2 ? tree_field_get_int(garage2, "entered") : 0;
    const int32_t garage_enter_tree =
        garage2 ? tree_field_get_int(garage2, "enter_via_tree") : 0;
    const int32_t menu_left =
        menu ? tree_field_get_int(menu, "entered") : -1;
    const int32_t menu_exit_tree =
        menu ? tree_field_get_int(menu, "exit_via_tree") : 0;
    InvObject* mmd_after = menu ? tree_field_get_obj(menu, "mmd") : menu;
    const int32_t mmd_cmd =
        mmd_before ? tree_field_get_int(mmd_before, "last_osd_cmd") : -1;
    const int32_t mmd_host =
        mmd_before ? tree_field_get_int(mmd_before, "osd_cmd_via_host") : 0;
    InvObject* ls = frontend_loading_screen();
    const int32_t ls_show =
        ls ? tree_field_get_int(ls, "show_count") : 0;
    const int32_t ls_host =
        ls ? tree_field_get_int(ls, "show_via_host") : 0;
    std::printf(
        "boot CMD_NEW name='%s' money=%d mode=%d career=%d day=%d "
        "garageIndex=%d map=0x%X entered=%d enter_via_tree=%d "
        "menu_entered=%d exit_via_tree=%d mmd_null=%d mmd_cmd=%d mmd_host=%d "
        "autoSave_calls=%d loadDefaults=%d autoSaveQuiet=%d "
        "ls_show=%d loading=%d played=%d\n",
        pname ? pname : "?", money, game_logic_game_mode(),
        game_logic_career_in_progress(), game_logic_day(), gidx, map_id,
        g_entered, garage_enter_tree, menu_left, menu_exit_tree,
        mmd_after ? 0 : 1, mmd_cmd, mmd_host, game_logic_auto_save_calls(),
        game_logic_load_defaults_calls(), game_logic_auto_save_quiet_calls(),
        ls_show, frontend_loading_screen_visible(), game_logic_played());
    if (!garage2 || after != garage2 || !pname || std::strcmp(pname, "Elias") ||
        money != 20000 || game_logic_game_mode() != 1 ||
        game_logic_career_in_progress() != 1 || game_logic_day() != 1 ||
        gidx != 0 || map_id == 0 || g_entered != 1 || garage_enter_tree != 1 ||
        menu_left != 0 || menu_exit_tree != 1 || mmd_after != nullptr ||
        mmd_cmd != 50 || mmd_host != 1 || game_logic_auto_save_calls() < 1 ||
        game_logic_load_defaults_calls() < 1 ||
        game_logic_auto_save_quiet_calls() < 1 || ls_show < 1 ||
        ls_host != 1 || frontend_loading_screen_visible() != 0 ||
        game_logic_played() != 1) {
      std::printf("FAIL CMD_NEW/Garage\n");
      return 5;
    }