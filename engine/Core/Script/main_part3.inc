// Fragment 3/4 of main.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


    // Spawn starter car + Garage OSD commands
    constexpr int32_t CMD_MECHANIC = 117;
    constexpr int32_t CMD_HITTHESTREET = 109;
    InvObject* car = player_spawn_starter_car();
    const char* cname =
        car ? string_cstr(tree_field_get_obj(car, "vehicleName")) : nullptr;
    const int32_t cid_car =
        car ? java_util_resource_ResourceRef_id(car) : 0;
    const int32_t stopped = car ? tree_field_get_int(car, "stopped") : 0;
    std::printf("boot car spawn name='%s' id=0x%X stopped=%d driveable_ok=%d\n",
                cname ? cname : "?", cid_car, stopped,
                car && !vehicle_is_driveable(car) ? 1 : 0);
    if (!car || cid_car == 0 || !cname || !cname[0] || stopped != 1 ||
        vehicle_is_driveable(car)) {
      std::printf("FAIL car spawn\n");
      return 5;
    }

    // HITTHESTREET without car already covered by spawn; test MECHANIC then street.
    garage_osd_command(garage2, CMD_MECHANIC);
    if (tree_field_get_int(garage2, "mode") != 1 ||
        tree_field_get_int(garage2, "last_cmd") != CMD_MECHANIC) {
      std::printf("FAIL CMD_MECHANIC\n");
      return 5;
    }

    constexpr int32_t CMD_TESTTRACK = 110;
    constexpr int32_t CMD_CARLOT = 111;
    constexpr int32_t CMD_CATALOG = 113;
    InvObject* cat = garage_osd_command(garage2, CMD_CATALOG);
    if (!cat || !std::strstr(tree_host_class(cat), "Catalog") ||
        tree_field_get_int(cat, "enter_via_tree") != 1 ||
        !game_state_return_to_garage(cat) ||
        tree_field_get_int(cat, "exit_via_tree") != 1 ||
        game_logic_actual_state() != garage2) {
      std::printf("FAIL Catalog roundtrip\n");
      return 5;
    }
    InvObject* lot = garage_osd_command(garage2, CMD_CARLOT);
    if (!lot || !std::strstr(tree_host_class(lot), "CarLot") ||
        tree_field_get_int(lot, "map_id") == 0 ||
        !game_state_return_to_garage(lot) ||
        tree_field_get_int(lot, "exit_via_tree") != 1) {
      std::printf("FAIL CarLot roundtrip\n");
      return 5;
    }
    InvObject* track = garage_osd_command(garage2, CMD_TESTTRACK);
    InvObject* tnav = track ? tree_field_get_obj(track, "nav") : nullptr;
    std::printf("boot TestTrack tiles=%d dyn=%d upd=%d\n",
                tnav ? tree_field_get_int(tnav, "tiles_count") : 0,
                tnav ? tree_field_get_int(tnav, "dynamarker_count") : 0,
                tnav ? tree_field_get_int(tnav, "update_count") : 0);
    if (!track || !tnav || tree_field_get_int(tnav, "tiles_count") != 48 ||
        tree_field_get_int(tnav, "update_count") < 1 ||
        !game_state_return_to_garage(track) ||
        tree_field_get_int(track, "exit_via_tree") != 1 ||
        tree_field_get_int(car, "stopped") != 1) {
      std::printf("FAIL TestTrack roundtrip\n");
      return 5;
    }

    constexpr int32_t CMD_BUYCARS = 112;
    constexpr int32_t CMD_CLUBINFO = 114;
    constexpr int32_t CMD_CARINFO = 115;
    constexpr int32_t CMD_BUYCARSUSED = 122;
    InvObject* market = garage_osd_command(garage2, CMD_BUYCARS);
    if (!market || !std::strstr(tree_host_class(market), "CarMarket") ||
        tree_field_get_int(market, "cars_for_sale") != 4 ||
        tree_field_get_int(market, "used") != 0 ||
        !game_state_return_to_garage(market) ||
        tree_field_get_int(market, "exit_via_tree") != 1) {
      std::printf("FAIL CarMarket new\n");
      return 5;
    }
    InvObject* usedm = garage_osd_command(garage2, CMD_BUYCARSUSED);
    if (!usedm || tree_field_get_int(usedm, "used") != 1 ||
        tree_field_get_int(usedm, "cars_for_sale") != 4 ||
        !game_state_return_to_garage(usedm) ||
        tree_field_get_int(usedm, "exit_via_tree") != 1) {
      std::printf("FAIL CarMarket used\n");
      return 5;
    }
    InvObject* club = garage_osd_command(garage2, CMD_CLUBINFO);
    if (!club || !std::strstr(tree_host_class(club), "ClubInfo") ||
        tree_field_get_int(club, "enter_via_tree") != 1 ||
        tree_field_get_int(club, "osd_created") != 1 ||
        !game_state_return_to_garage(club) ||
        tree_field_get_int(club, "exit_via_tree") != 1) {
      std::printf("FAIL ClubInfo\n");
      return 5;
    }
    InvObject* cinfo = garage_osd_command(garage2, CMD_CARINFO);
    if (!cinfo || !std::strstr(tree_host_class(cinfo), "CarInfo") ||
        tree_field_get_int(cinfo, "car_id") == 0 ||
        tree_field_get_int(cinfo, "enter_via_tree") != 1 ||
        !game_state_return_to_garage(cinfo) ||
        tree_field_get_int(cinfo, "exit_via_tree") != 1) {
      std::printf("FAIL CarInfo\n");
      return 5;
    }
    std::printf("boot market/club/carinfo ok\n");

    InvObject* city = garage_osd_command(garage2, CMD_HITTHESTREET);
    InvObject* cur2 = game_logic_actual_state();
    const int32_t city_map =
        city ? tree_field_get_int(city, "map_id") : 0;
    const float start_z =
        city ? tree_field_get_float(city, "posStart_z") : 0.f;
    const int32_t daytime =
        city ? tree_field_get_int(city, "daytime") : -1;
    const float car_z = car ? tree_field_get_float(car, "pos_z") : 0.f;
    const int32_t traffic_n =
        city ? tree_field_get_int(city, "traffic_count") : 0;
    const int32_t peds =
        city ? tree_field_get_int(city, "pedestrian_types") : 0;
    const int32_t trigs =
        city ? tree_field_get_int(city, "trigger_count") : 0;
    InvObject* cnav = city ? tree_field_get_obj(city, "nav") : nullptr;
    const int32_t ntiles = cnav ? tree_field_get_int(cnav, "tiles_count") : 0;
    const int32_t nmark = cnav ? tree_field_get_int(cnav, "marker_count") : 0;
    const int32_t ndyn = cnav ? tree_field_get_int(cnav, "dynamarker_count") : 0;
    std::printf(
        "boot Valocity map=0x%X start_z=%.1f daytime=%d car_z=%.1f "
        "traffic=%d peds=%d trigs=%d nav=%dx%d mark=%d dyn=%d "
        "roads=%d garage_entered=%d\n",
        city_map, start_z, daytime, car_z, traffic_n, peds, trigs, ntiles,
        cnav ? tree_field_get_int(cnav, "tiles_z") : 0, nmark, ndyn,
        physics_road_count(), tree_field_get_int(garage2, "entered"));
    // Day + career: 847; nav 64 tiles, 3 static + player dyn
    if (!city || cur2 != city || !std::strstr(tree_host_class(city), "Valocity") ||
        tree_field_get_int(garage2, "entered") != 0 ||
        tree_field_get_int(garage2, "exit_via_tree") != 1 || city_map == 0 ||
        start_z < 1000.f || daytime != 1 || car_z < 1000.f || traffic_n != 847 ||
        peds != 6 || trigs != 3 || ntiles != 64 || nmark != 3 || ndyn < 1 ||
        physics_road_count() < 6 ||
        tree_field_get_int(city, "enter_via_tree") != 1 ||
        frontend_loading_screen_visible() != 0) {
      std::printf("FAIL Valocity.enter\n");
      return 5;
    }

    // Phase 2.38: day Scene look (noon → config 7).
    {
      const int32_t scfg = valocity_scene_config(city);
      InvObject* env = tree_field_get_obj(city, "envmap");
      InvObject* sky = tree_field_get_obj(city, "skydome");
      InvObject* sky_tex = tree_field_get_obj(city, "sky_tex");
      const int32_t sky_verts =
          sky ? render_d3d9_mesh_vertex_count(sky) : 0;
      // Phase 2.51: stock skydome.SCX is INVO v3 → 376 verts (proc was 561).
      InvObject* suntype = tree_field_get_obj(city, "suntype");
      const int32_t flare_n =
          suntype ? tree_field_get_int(suntype, "flare_count") : -1;
      InvObject* sun = tree_field_get_obj(city, "sun");
      InvObject* cmap = tree_field_get_obj(city, "map");
      InvObject* salias = sun ? tree_field_get_obj(sun, "alias") : nullptr;
      const char* sal = salias ? string_cstr(salias) : "";
      const bool sun_ok =
          sun && cmap && java_util_resource_ResourceRef_type(sun) == 3 &&
          java_util_resource_ResourceRef_getParentID(sun) ==
              java_util_resource_ResourceRef_id(cmap) &&
          sal && std::strcmp(sal, "sunny") == 0;
      const bool scene_ok =
          scfg == 7 && render_d3d9_fog_enabled() &&
          render_d3d9_fog_color() == 0x005E7992 &&
          render_d3d9_fog_near() > 69.f && render_d3d9_fog_far() > 399.f &&
          render_d3d9_light_enabled() &&
          render_d3d9_light_diffuse() == 0x00DDD8AD &&
          render_d3d9_light_ambient() == 0x004B5E6F &&
          flare_n == 15 &&
          suntype && tree_field_get_int(suntype, "dup_src_id") != 0 &&
          sun_ok &&
          env && render_d3d9_global_envmap() == env &&
          sky && render_d3d9_mesh_ready(sky) && sky_verts == 376 && sky_tex &&
          render_d3d9_texture_ready(sky_tex) &&
          render_d3d9_texture_width(sky_tex) >= 512 &&
          render_d3d9_mesh_textured_count(sky) >= 1;
      std::printf("boot valocity_scene ok=%d cfg=%d fog=%06X near=%.0f "
                  "far=%.0f light=%06X flare=%d env=%d sky=%d verts=%d ptx=%dx%d tex=%d\n",
                  scene_ok ? 1 : 0, scfg, render_d3d9_fog_color(),
                  render_d3d9_fog_near(), render_d3d9_fog_far(),
                  render_d3d9_light_diffuse(), flare_n,
                  env && render_d3d9_global_envmap() == env ? 1 : 0,
                  sky && render_d3d9_mesh_ready(sky) ? 1 : 0,
                  sky_verts,
                  sky_tex ? render_d3d9_texture_width(sky_tex) : 0,
                  sky_tex ? render_d3d9_texture_height(sky_tex) : 0,
                  sky ? render_d3d9_mesh_textured_count(sky) : 0);
      if (!scene_ok) {
        std::printf("FAIL Valocity.scene\n");
        return 5;
      }
    }

    // Phase 2.41/2.45/2.52/2.57/2.58: city visuals — full instance budget.
    {
      const int32_t cm = city_mesh_count();
      const int32_t cv = city_mesh_vertex_total();
      const int32_t ci = city_instance_count();
      const int32_t cd = city_instance_drawn();
      const int32_t q = render_d3d9_mesh_queue_count();
      // Stock install: ~53 area/road SCX resolve; budget 142 covers all of them.
      const bool city_mesh_ok =
          cm >= 50 && cv > 1000 && q >= cm && ci >= 100 && cd >= 50 &&
          cd <= ci && cd == cm;
      std::printf("boot valocity_citymesh ok=%d meshes=%d instances=%d "
                  "drawn=%d verts=%d queue=%d\n",
                  city_mesh_ok ? 1 : 0, cm, ci, cd, cv, q);
      if (!city_mesh_ok) {
        std::printf("FAIL Valocity.citymesh\n");
        return 5;
      }
    }

    // Phase 2.34: Valocity frame simulate drives the player car.
    const float x0 = tree_field_get_float(car, "pos_x");
    const float z0 = tree_field_get_float(car, "pos_z");
    InvObject* pctrl =
        player ? tree_field_get_obj(player, "controller") : nullptr;
    if (!pctrl) pctrl = input_get_controller(0);
    java_io_Controller_user_SetAxisForce(pctrl, kAxisThrottle, 0.f, 1.f);
    for (int i = 0; i < 40; ++i) valocity_simulate(city, 0.05f);
    java_io_Controller_user_SetAxisForce(pctrl, kAxisThrottle, 0.f, 0.f);
    const float x1 = tree_field_get_float(car, "pos_x");
    const float z1 = tree_field_get_float(car, "pos_z");
    const float sim_spd = tree_field_get_float(car, "speed_sq");
    const float live_spd = java_game_Vehicle_getSpeedSquare(car);
    const float dist = std::sqrt((x1 - x0) * (x1 - x0) + (z1 - z0) * (z1 - z0));
    const bool sim_ok =
        physics_shape(car) == 1 && dist > 2.f && sim_spd > 1.f && live_spd > 1.f;
    std::printf("boot valocity_sim ok=%d shape=%d dist=%.1f spd=%.1f/%.1f\n",
                sim_ok ? 1 : 0, physics_shape(car), dist, sim_spd, live_spd);

    // Phase 2.72: AXIS_BRAKE / HANDBRAKE → WheelRef (+ balance) → decelerate.
    {
      InvObject* body = tree_field_get_obj(car, "chassis");
      if (!body) body = car;
      tree_field_set_float(car, "brake_balance", 0.5f);
      java_io_Controller_user_SetAxisForce(pctrl, kAxisThrottle, 0.f, 0.f);
      java_io_Controller_user_SetAxisForce(pctrl, kAxisBrake, 0.f, 0.f);
      java_io_Controller_user_SetAxisForce(pctrl, kAxisHandbrake, 0.f, 0.f);
      physics_set_velocity(body, 0.f, 0.f, 18.f);
      for (int i = 0; i < 4; ++i) valocity_simulate(city, 0.05f);
      const float coast_spd = physics_speed_square(body);
      physics_set_velocity(body, 0.f, 0.f, 18.f);
      java_io_Controller_user_SetAxisForce(pctrl, kAxisBrake, 0.f, 1.f);
      for (int i = 0; i < 4; ++i) valocity_simulate(city, 0.05f);
      const float brk_spd = physics_speed_square(body);
      InvObject* wfl = java_game_parts_bodypart_Chassis_getWheel(car, 0);
      InvObject* wfr = java_game_parts_bodypart_Chassis_getWheel(car, 1);
      InvObject* wrr = java_game_parts_bodypart_Chassis_getWheel(car, 3);
      const float fl_brk = wheelref_get_brake(wfl);
      const float fr_brk = wheelref_get_brake(wfr);
      const float fl_hb = wheelref_get_hbrake(wfl);
      java_io_Controller_user_SetAxisForce(pctrl, kAxisBrake, 0.f, 0.f);
      java_io_Controller_user_SetAxisForce(pctrl, kAxisHandbrake, 0.f, 1.f);
      valocity_simulate(city, 0.05f);
      const float rr_hb = wheelref_get_hbrake(wrr);
      const float fl_hb2 = wheelref_get_hbrake(wfl);
      java_io_Controller_user_SetAxisForce(pctrl, kAxisHandbrake, 0.f, 0.f);
      const float restore = sim_spd > 1.f ? sim_spd : 64.f;
      physics_set_velocity(body, 0.f, 0.f, std::sqrt(restore));
      tree_field_set_float(car, "speed_sq", restore);
      // bal=0.5 → front share 0.1 torque; HB rear-only 0.2.
      const bool brake_ok =
          fl_brk > 0.08f && fl_brk < 0.12f && fr_brk > 0.08f && fl_hb < 0.01f &&
          rr_hb > 0.15f && fl_hb2 < 0.01f && brk_spd + 5.f < coast_spd;
      std::printf("boot valocity_brake ok=%d fl_brk=%.2f fr=%.2f "
                  "coast=%.0f>brk=%.0f rr_hb=%.2f fl_hb=%.2f\n",
                  brake_ok ? 1 : 0, fl_brk, fr_brk, coast_spd, brk_spd, rr_hb,
                  fl_hb2);
      if (!brake_ok) {
        std::printf("FAIL Valocity.brake\n");
        return 5;
      }
    }

    // Phase 2.42: chassis SCX visual mesh follows car pose.
    {
      InvObject* vmesh = tree_field_get_obj(car, "visual_mesh");
      valocity_sync_car_mesh(car);
      float wx = 0.f, wy = 0.f, wz = 0.f;
      if (vmesh) render_d3d9_mesh_world_origin(vmesh, &wx, &wy, &wz);
      const float cx = tree_field_get_float(car, "pos_x");
      const float cy = tree_field_get_float(car, "pos_y");
      const float cz = tree_field_get_float(car, "pos_z");
      const float mdx = wx - cx;
      const float mdy = wy - cy;
      const float mdz = wz - cz;
      const float mesh_err =
          std::sqrt(mdx * mdx + mdy * mdy + mdz * mdz);
      const int32_t mverts =
          vmesh ? render_d3d9_mesh_vertex_count(vmesh) : 0;
      const bool carmesh_ok =
          sim_ok && vmesh && render_d3d9_mesh_ready(vmesh) && mverts > 500 &&
          mesh_err < 1.f;
      std::printf("boot valocity_carmesh ok=%d verts=%d err=%.2f "
                  "origin=(%.1f,%.1f,%.1f)\n",
                  carmesh_ok ? 1 : 0, mverts, mesh_err, wx, wy, wz);
      if (!carmesh_ok) {
        std::printf("FAIL Valocity.carmesh\n");
        return 5;
      }
    }

    // Phase 2.46–2.49: cfg slots + mating + Blossom wheels + tyres.
    {
      InvObject* vmesh = tree_field_get_obj(car, "visual_mesh");
      InvObject* parts = tree_field_get_obj(car, "visual_parts");
      const int32_t nparts = valocity_car_part_count(car);
      const int32_t nslot = valocity_car_part_slotted(car);
      const int32_t nwheels = valocity_car_wheel_count(car);
      const int32_t ntyres = valocity_car_tyre_count(car);
      InvObject* door = nullptr;
      InvObject* mirror = nullptr;
      InvObject* glass = nullptr;
      InvObject* w0 = nullptr;
      InvObject* t0 = nullptr;
      for (int32_t i = 0; i < nparts; ++i) {
        InvObject* p = tree_vector_element_at(parts, i);
        if (!p) continue;
        const char* stem = nullptr;
        if (InvObject* s = tree_field_get_obj(p, "part_stem"))
          stem = string_cstr(s);
        if (!stem) continue;
        if (!door && std::strcmp(stem, "FL_door") == 0) door = p;
        if (!mirror && std::strcmp(stem, "L_mirror") == 0) mirror = p;
        if (!glass && std::strcmp(stem, "F_windshield") == 0) glass = p;
        if (!w0 && std::strcmp(stem, "wheel_0") == 0) w0 = p;
        if (!t0 && std::strcmp(stem, "tyre_0") == 0) t0 = p;
      }
      const float dax = door ? tree_field_get_float(door, "attach_x") : 0.f;
      const float day = door ? tree_field_get_float(door, "attach_y") : 0.f;
      const float daz = door ? tree_field_get_float(door, "attach_z") : 0.f;
      // DevilSport FL_door: chassis(-0.865,-0.105,-0.818) - mate(-0.120,0,-0.703)
      const float door_err = std::sqrt(
          (dax + 74.5f) * (dax + 74.5f) + (day + 10.5f) * (day + 10.5f) +
          (daz + 11.5f) * (daz + 11.5f));
      const float max = mirror ? tree_field_get_float(mirror, "attach_x") : 0.f;
      const float may = mirror ? tree_field_get_float(mirror, "attach_y") : 0.f;
      const float maz = mirror ? tree_field_get_float(mirror, "attach_z") : 0.f;
      // Door L_mirror (0.069,0.293,-0.498) - mate (0.104,-0.052,-0.027)
      const float mir_err = std::sqrt(
          (max + 3.5f) * (max + 3.5f) + (may - 34.5f) * (may - 34.5f) +
          (maz + 47.1f) * (maz + 47.1f));
      const float wax = w0 ? tree_field_get_float(w0, "attach_x") : 0.f;
      const float way = w0 ? tree_field_get_float(w0, "attach_y") : 0.f;
      const float waz = w0 ? tree_field_get_float(w0, "attach_z") : 0.f;
      // FL wheel line (-0.709,-0.590,-1.480)×100
      const float wheel_err = std::sqrt(
          (wax + 70.9f) * (wax + 70.9f) + (way + 59.0f) * (way + 59.0f) +
          (waz + 148.0f) * (waz + 148.0f));
      const float tax = t0 ? tree_field_get_float(t0, "attach_x") : 0.f;
      const float tay = t0 ? tree_field_get_float(t0, "attach_y") : 0.f;
      const float taz = t0 ? tree_field_get_float(t0, "attach_z") : 0.f;
      const float toy = t0 ? tree_field_get_float(t0, "attach_oy") : 0.f;
      // Rim slot2 − tyre mate (0,0,0 / π) → local (0,0,0) oy=-π
      const float tyre_pos_err =
          std::sqrt(tax * tax + tay * tay + taz * taz);
      const float tyre_oy_err = std::fabs(toy + 3.14159265f);
      const int32_t glass_verts =
          glass ? render_d3d9_mesh_vertex_count(glass) : 0;
      const bool parts_ok =
          nparts >= 28 && nslot >= 18 && nwheels >= 4 && ntyres >= 4 && door &&
          tree_field_get_int(door, "attach_slotted") == 1 &&
          render_d3d9_mesh_get_parent(door) == vmesh && door_err < 2.f &&
          glass && glass_verts >= 20 && mirror &&
          render_d3d9_mesh_get_parent(mirror) == door &&
          tree_field_get_int(mirror, "attach_slotted") == 1 && mir_err < 2.f &&
          w0 && render_d3d9_mesh_get_parent(w0) == vmesh && wheel_err < 2.f &&
          t0 && render_d3d9_mesh_get_parent(t0) == w0 &&
          tree_field_get_int(t0, "attach_slotted") == 1 && tyre_pos_err < 2.f &&
          tyre_oy_err < 0.05f;
      std::printf("boot valocity_carparts ok=%d parts=%d slotted=%d wheels=%d "
                  "tyres=%d door_err=%.2f mir_err=%.2f "
                  "wheel0=(%.1f,%.1f,%.1f) wheel_err=%.2f "
                  "tyre0_oy=%.3f tyre_err=%.2f glass_verts=%d\n",
                  parts_ok ? 1 : 0, nparts, nslot, nwheels, ntyres, door_err,
                  mir_err, wax, way, waz, wheel_err, toy, tyre_pos_err,
                  glass_verts);
      if (!parts_ok) {
        std::printf("FAIL Valocity.carparts\n");
        return 5;
      }
    }

    // Phase 2.50: Part.setSlotPos moves chassis wheel slot 101 → FL rim.
    {
      InvObject* parts = tree_field_get_obj(car, "visual_parts");
      const int32_t nparts = valocity_car_part_count(car);
      InvObject* w0 = nullptr;
      for (int32_t i = 0; i < nparts; ++i) {
        InvObject* p = tree_vector_element_at(parts, i);
        if (!p) continue;
        const char* stem = nullptr;
        if (InvObject* s = tree_field_get_obj(p, "part_stem"))
          stem = string_cstr(s);
        if (stem && std::strcmp(stem, "wheel_0") == 0) {
          w0 = p;
          break;
        }
      }
      const int32_t nslot_ids = java_game_parts_Part_getSlots(car);
      InvObject* on101 = java_game_parts_Part_partOnSlot(car, 101);
      java_game_parts_Part_setSlotPos(
          car, 101, vec3_new(-0.809f, -0.590f, -1.480f), nullptr);
      const float ax = w0 ? tree_field_get_float(w0, "attach_x") : 0.f;
      const float ay = w0 ? tree_field_get_float(w0, "attach_y") : 0.f;
      const float az = w0 ? tree_field_get_float(w0, "attach_z") : 0.f;
      const float slot_err = std::sqrt(
          (ax + 80.9f) * (ax + 80.9f) + (ay + 59.0f) * (ay + 59.0f) +
          (az + 148.0f) * (az + 148.0f));
      // Restore stock FL wheel pose for later checks / visuals.
      java_game_parts_Part_setSlotPos(
          car, 101, vec3_new(-0.709f, -0.590f, -1.480f), nullptr);
      const bool slot_ok = w0 && on101 == w0 && nslot_ids >= 4 &&
                           slot_err < 2.f &&
                           java_game_parts_Part_getSlotID(car, 0) == 101;
      std::printf("boot valocity_slotpos ok=%d slots=%d on101=%d "
                  "moved=(-80.9 target) err=%.2f\n",
                  slot_ok ? 1 : 0, nslot_ids, on101 == w0 ? 1 : 0, slot_err);
      if (!slot_ok) {
        std::printf("FAIL Valocity.slotpos\n");
        return 5;
      }
    }

    // Phase 2.53: install graph — car.101↔rim.1, rim.2↔tyre.1.
    {
      InvObject* parts = tree_field_get_obj(car, "visual_parts");
      const int32_t nparts = valocity_car_part_count(car);
      InvObject* w0 = nullptr;
      InvObject* t0 = nullptr;
      for (int32_t i = 0; i < nparts; ++i) {
        InvObject* p = tree_vector_element_at(parts, i);
        if (!p) continue;
        const char* stem = nullptr;
        if (InvObject* s = tree_field_get_obj(p, "part_stem"))
          stem = string_cstr(s);
        if (!stem) continue;
        if (!w0 && std::strcmp(stem, "wheel_0") == 0) w0 = p;
        if (!t0 && std::strcmp(stem, "tyre_0") == 0) t0 = p;
      }
      const InvObject* on101 = java_game_parts_Part_partOnSlot(car, 101);
      const int32_t mate101 = java_game_parts_Part_slotIDOnSlot(car, 101);
      const InvObject* on_rim2 =
          w0 ? java_game_parts_Part_partOnSlot(w0, 2) : nullptr;
      const int32_t mate_rim2 =
          w0 ? java_game_parts_Part_slotIDOnSlot(w0, 2) : 0;

      // Phase 2.62: getWheelID before install_OK (which may rebind slots).
      InvObject* w3 = nullptr;
      InvObject* t3 = nullptr;
      for (int32_t i = 0; i < nparts; ++i) {
        InvObject* p = tree_vector_element_at(parts, i);
        if (!p) continue;
        const char* stem = nullptr;
        if (InvObject* s = tree_field_get_obj(p, "part_stem"))
          stem = string_cstr(s);
        if (!stem) continue;
        if (!w3 && std::strcmp(stem, "wheel_3") == 0) w3 = p;
        if (!t3 && std::strcmp(stem, "tyre_3") == 0) t3 = p;
      }
      const int32_t wid0 = java_game_parts_Part_getWheelID(w0);
      const int32_t tid0 = java_game_parts_Part_getWheelID(t0);
      const int32_t wid3 = w3 ? java_game_parts_Part_getWheelID(w3) : -2;
      const int32_t tid3 = t3 ? java_game_parts_Part_getWheelID(t3) : -2;
      InvObject* gw0 = java_game_parts_bodypart_Chassis_getWheel(car, 0);
      InvObject* gw3 = java_game_parts_bodypart_Chassis_getWheel(car, 3);
      const bool wheel_ok =
          wid0 == 0 && tid0 == 0 && wid3 == 3 && tid3 == 3 && gw0 == w0 &&
          gw3 == w3;
      const int32_t nwheels =
          java_game_parts_bodypart_Chassis_getWheels(car);
      InvObject* whl0 = java_game_parts_bodypart_Chassis_getWheel(car, 0);
      java_game_parts_WheelRef_setRadius(whl0, 0.31f);
      java_game_parts_WheelRef_setSteer(whl0, 0.15f);
      java_game_parts_WheelRef_setDrive(whl0, 1.f);
      java_game_parts_WheelRef_setArm(whl0, 0.244f, 0.344f, 0.054f, 0.f, 0.f,
                                     0.f, 1.f);
      java_game_parts_WheelRef_setHub(whl0, 0.263f, 0.f, -0.125f, 0.f, 0.f,
                                     0.143f, 0.f, 0.360f, 0.323f, 0.f);
      float wpx = 0, wpy = 0, wpz = 0;
      vec3_get(java_game_parts_WheelRef_getPos(whl0), &wpx, &wpy, &wpz);
      const float wr = java_game_parts_WheelRef_getRadius(whl0);
      const float ws = java_game_parts_WheelRef_getSteer(whl0);
      const float wd = java_game_parts_WheelRef_getDrive(whl0);
      const float pos_err = std::sqrt(
          (wpx + 0.709f) * (wpx + 0.709f) + (wpy + 0.590f) * (wpy + 0.590f) +
          (wpz + 1.480f) * (wpz + 1.480f));
      const bool wheels_ok =
          wheel_ok && nwheels == 4 && whl0 == w0 && wr > 0.309f && wr < 0.311f &&
          ws > 0.14f && ws < 0.16f && wd > 0.99f && pos_err < 0.02f;
      // Phase 2.65: setSlotPos ↔ WheelRef.getPos / setPos → slot.
      java_game_parts_Part_setSlotPos(car, 101, vec3_new(-0.10f, 0.02f, 0.05f),
                                      nullptr);
      float spx = 0, spy = 0, spz = 0;
      vec3_get(java_game_parts_WheelRef_getPos(whl0), &spx, &spy, &spz);
      const float slot_err = std::sqrt(
          (spx + 0.10f) * (spx + 0.10f) + (spy - 0.02f) * (spy - 0.02f) +
          (spz - 0.05f) * (spz - 0.05f));
      java_game_parts_WheelRef_setPos(
          whl0, vec3_new(-0.709f, -0.590f, -1.480f));
      float rpx = 0, rpy = 0, rpz = 0;
      part_slot_get_pose(car, 101, &rpx, &rpy, &rpz, nullptr, nullptr, nullptr);
      float gpx = 0, gpy = 0, gpz = 0;
      vec3_get(java_game_parts_WheelRef_getPos(whl0), &gpx, &gpy, &gpz);
      const float back_err = std::sqrt(
          (rpx + 0.709f) * (rpx + 0.709f) + (rpy + 0.590f) * (rpy + 0.590f) +
          (rpz + 1.480f) * (rpz + 1.480f) + (gpx + 0.709f) * (gpx + 0.709f) +
          (gpy + 0.590f) * (gpy + 0.590f) + (gpz + 1.480f) * (gpz + 1.480f));
      const bool sync_ok =
          wheels_ok && slot_err < 0.01f && back_err < 0.02f;
      std::printf("boot valocity_wheelid ok=%d w0=%d t0=%d w3=%d t3=%d "
                  "getWheel=%d/%d getWheels=%d radius=%.2f steer=%.2f "
                  "pos_err=%.3f slot_err=%.3f sync_err=%.3f\n",
                  sync_ok ? 1 : 0, wid0, tid0, wid3, tid3, gw0 == w0 ? 1 : 0,
                  gw3 == w3 ? 1 : 0, nwheels, wr, ws, pos_err, slot_err,
                  back_err);
      if (!sync_ok) {
        std::printf("FAIL Valocity.wheelid\n");
        return 5;
      }

      // Phase 2.70: front steer yaw + roll spin on rim mesh transforms.
      InvObject* rim_vis = part_slot_visual(car, 101);
      if (rim_vis) tree_field_set_float(rim_vis, "wheel_spin", 0.f);
      java_game_parts_WheelRef_setSteer(whl0, 0.f);
      if (InvObject* w1 = java_game_parts_bodypart_Chassis_getWheel(car, 1))
        java_game_parts_WheelRef_setSteer(w1, 0.f);
      tree_field_set_float(car, "speed_sq", 0.f);
      InvObject* chassis = tree_field_get_obj(car, "chassis");
      if (!chassis) chassis = car;
      physics_set_velocity(chassis, 0.f, 0.f, 0.f);
      valocity_sync_wheel_visuals(car, 0.05f);
      float bx = 0, by = 0, bz = 0, boy = 0, bop = 0, bor = 0;
      float bsx = 1, bsy = 1, bsz = 1;
      if (rim_vis && render_d3d9_mesh_ready(rim_vis))
        render_d3d9_mesh_get_transform(rim_vis, &bx, &by, &bz, &boy, &bop, &bor,
                                       &bsx, &bsy, &bsz);
      java_game_parts_WheelRef_setSteer(whl0, 1.f);
      if (InvObject* w1 = java_game_parts_bodypart_Chassis_getWheel(car, 1))
        java_game_parts_WheelRef_setSteer(w1, 1.f);
      valocity_sync_wheel_visuals(car, 0.05f);
      float sx0 = 0, sy0 = 0, sz0 = 0, soy = 0, sop = 0, sor = 0;
      float ssx = 1, ssy = 1, ssz = 1;
      if (rim_vis && render_d3d9_mesh_ready(rim_vis))
        render_d3d9_mesh_get_transform(rim_vis, &sx0, &sy0, &sz0, &soy, &sop,
                                       &sor, &ssx, &ssy, &ssz);
      const float steer_doy = soy - boy;
      const float spin0 =
          rim_vis ? tree_field_get_float(rim_vis, "wheel_spin") : 0.f;
      physics_set_velocity(chassis, 0.f, 0.f, 10.f);
      for (int i = 0; i < 20; ++i) valocity_sync_wheel_visuals(car, 0.05f);
      float spin1 =
          rim_vis ? tree_field_get_float(rim_vis, "wheel_spin") : 0.f;
      float px1 = 0, py1 = 0, pz1 = 0, oy1 = 0, op1 = 0, or1 = 0;
      float s1 = 1, s2 = 1, s3 = 1;
      if (rim_vis && render_d3d9_mesh_ready(rim_vis))
        render_d3d9_mesh_get_transform(rim_vis, &px1, &py1, &pz1, &oy1, &op1,
                                       &or1, &s1, &s2, &s3);
      float slot_oy = 0;
      part_slot_get_pose(car, 101, nullptr, nullptr, nullptr, &slot_oy, nullptr,
                         nullptr);
      java_game_parts_WheelRef_setSteer(whl0, 0.15f);
      // Restore speed for HUD smoke (this probe cleared chassis vel).
      const float restore_spd = sim_spd > 1.f ? sim_spd : 64.f;
      physics_set_velocity(chassis, 0.f, 0.f, std::sqrt(restore_spd));
      tree_field_set_float(car, "speed_sq", restore_spd);
      const bool wvis_ok =
          rim_vis && steer_doy < -0.4f && steer_doy > -0.7f &&
          spin1 > spin0 + 0.5f && op1 > bop + 0.5f &&
          std::fabs(slot_oy - boy) < 0.05f;
      std::printf("boot valocity_wheelvis ok=%d steer_doy=%.2f spin=%.2f->%.2f "
                  "op=%.2f->%.2f slot_oy=%.2f\n",
                  wvis_ok ? 1 : 0, steer_doy, spin0, spin1, bop, op1, slot_oy);
      if (!wvis_ok) {
        std::printf("FAIL Valocity.wheelvis\n");
        return 5;
      }

      // Phase 2.71: Chassis.setSteerWheel(r,z) scales front visual yaw.
      if (rim_vis) tree_field_set_float(rim_vis, "wheel_spin", 0.f);
      physics_set_velocity(chassis, 0.f, 0.f, 0.f);
      java_game_parts_WheelRef_setSteer(whl0, 0.f);
      valocity_sync_wheel_visuals(car, 0.05f);
      float sw_bx = 0, sw_by = 0, sw_bz = 0, sw_boy = 0, sw_bop = 0, sw_bor = 0;
      float sw_s0 = 1, sw_s1 = 1, sw_s2 = 1;
      if (rim_vis && render_d3d9_mesh_ready(rim_vis))
        render_d3d9_mesh_get_transform(rim_vis, &sw_bx, &sw_by, &sw_bz, &sw_boy,
                                       &sw_bop, &sw_bor, &sw_s0, &sw_s1, &sw_s2);
      java_game_parts_bodypart_Chassis_setSteerWheel(car, 0.05f, 0.12f);
      java_game_parts_bodypart_Chassis_setSteerWheelRadius(car, 0.05f);
      java_game_parts_WheelRef_setSteer(whl0, 1.f);
      valocity_sync_wheel_visuals(car, 0.05f);
      float sw_oy_hi = 0;
      {
        float a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 1, h = 1, i = 1;
        if (rim_vis && render_d3d9_mesh_ready(rim_vis))
          render_d3d9_mesh_get_transform(rim_vis, &a, &b, &c, &sw_oy_hi, &d, &e,
                                         &g, &h, &i);
      }
      const float doy_hi = std::fabs(sw_oy_hi - sw_boy);
      java_game_parts_bodypart_Chassis_setSteerWheel(car, 0.30f, 0.12f);
      valocity_sync_wheel_visuals(car, 0.05f);
      float sw_oy_lo = 0;
      {
        float a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 1, h = 1, i = 1;
        if (rim_vis && render_d3d9_mesh_ready(rim_vis))
          render_d3d9_mesh_get_transform(rim_vis, &a, &b, &c, &sw_oy_lo, &d, &e,
                                         &g, &h, &i);
      }
      const float doy_lo = std::fabs(sw_oy_lo - sw_boy);
      const float stored_r = tree_field_get_float(car, "steer_wheel_r");
      const float stored_z = tree_field_get_float(car, "steer_wheel_z");
      java_game_parts_WheelRef_setSteer(whl0, 0.15f);
      java_game_parts_bodypart_Chassis_setSteerWheel(car, 0.15f, 0.f);
      const bool steerwheel_ok =
          doy_hi > doy_lo + 0.15f && stored_r > 0.29f && stored_r < 0.31f &&
          stored_z > 0.11f && stored_z < 0.13f;
      std::printf("boot valocity_steerwheel ok=%d doy=%.2f>%.2f r=%.2f z=%.2f\n",
                  steerwheel_ok ? 1 : 0, doy_hi, doy_lo, stored_r, stored_z);
      if (!steerwheel_ok) {
        std::printf("FAIL Valocity.steerwheel\n");
        return 5;
      }

      // Phase 2.73: getWheelPos / mileage / getMass.
      InvObject* wp0 =
          java_game_parts_bodypart_Chassis_getWheelPos(car, 0);
      float gwx = 0, gwy = 0, gwz = 0;
      if (wp0) vec3_get(wp0, &gwx, &gwy, &gwz);
      float grx = 0, gry = 0, grz = 0;
      vec3_get(java_game_parts_WheelRef_getPos(whl0), &grx, &gry, &grz);
      const float wp_err = std::sqrt(
          (gwx - grx) * (gwx - grx) + (gwy - gry) * (gwy - gry) +
          (gwz - grz) * (gwz - grz));
      java_game_parts_bodypart_Chassis_setMileage(car, 1234567.f);
      const float miles_rt =
          java_game_parts_bodypart_Chassis_getMileage(car);
      const float mass0 = java_game_parts_bodypart_Chassis_getMass(car);
      tree_field_set_float(car, "mass", 1540.f);
      const float mass1 = java_game_parts_bodypart_Chassis_getMass(car);
      const float miles_before =
          java_game_parts_bodypart_Chassis_getMileage(car);
      java_game_parts_bodypart_Chassis_setMileage(car, 100.f);
      InvObject* body_m = tree_field_get_obj(car, "chassis");
      if (!body_m) body_m = car;
      physics_set_velocity(body_m, 0.f, 0.f, 10.f);
      for (int i = 0; i < 10; ++i) valocity_simulate(city, 0.05f);
      const float miles_after =
          java_game_parts_bodypart_Chassis_getMileage(car);
      physics_set_velocity(body_m, 0.f, 0.f, std::sqrt(sim_spd > 1.f ? sim_spd : 64.f));
      tree_field_set_float(car, "speed_sq", sim_spd > 1.f ? sim_spd : 64.f);
      java_game_parts_bodypart_Chassis_setMileage(car, miles_before);
      const bool chassis_ok =
          wp_err < 0.02f && miles_rt > 1234566.f && miles_rt < 1234568.f &&
          mass0 > 1100.f && mass1 > 1539.f && mass1 < 1541.f &&
          miles_after > 100.5f;
      std::printf("boot valocity_chassis ok=%d wp_err=%.3f miles=%.0f "
                  "mass=%.0f->%.0f odo=%.1f->%.1f\n",
                  chassis_ok ? 1 : 0, wp_err, miles_rt, mass0, mass1, 100.f,
                  miles_after);
      if (!chassis_ok) {
        std::printf("FAIL Valocity.chassis\n");
        return 5;
      }

      // Phase 2.74: getCM / getMin / getMax from wheel AABB.
      InvObject* vcm = java_game_parts_bodypart_Chassis_getCM(car);
      InvObject* vmin = java_game_parts_bodypart_Chassis_getMin(car);
      InvObject* vmax = java_game_parts_bodypart_Chassis_getMax(car);
      float cmx = 0, cmy = 0, cmz = 0, mnx = 0, mny = 0, mnz = 0;
      float mxx = 0, mxy = 0, mxz = 0;
      if (vcm) vec3_get(vcm, &cmx, &cmy, &cmz);
      if (vmin) vec3_get(vmin, &mnx, &mny, &mnz);
      if (vmax) vec3_get(vmax, &mxx, &mxy, &mxz);
      const float len_m = mxz - mnz;
      const float wid_m = mxx - mnx;
      tree_field_set_float(car, "cm_set", 1.f);
      tree_field_set_float(car, "cm_x", 0.1f);
      tree_field_set_float(car, "cm_y", -0.2f);
      tree_field_set_float(car, "cm_z", 0.05f);
      InvObject* vcm2 = java_game_parts_bodypart_Chassis_getCM(car);
      float c2x = 0, c2y = 0, c2z = 0;
      if (vcm2) vec3_get(vcm2, &c2x, &c2y, &c2z);
      tree_field_set_float(car, "cm_set", 0.f);
      const bool bounds_ok =
          vmin && vmax && vcm && len_m > 2.0f && len_m < 5.0f && wid_m > 1.0f &&
          wid_m < 3.0f && mnx < mxx && mnz < mxz &&
          cmx > mnx && cmx < mxx && cmz > mnz && cmz < mxz &&
          std::fabs(c2x - 0.1f) < 0.01f && std::fabs(c2y + 0.2f) < 0.01f;
      std::printf("boot valocity_bounds ok=%d len=%.2f wid=%.2f "
                  "cm=(%.2f,%.2f,%.2f) ov=(%.2f,%.2f)\n",
                  bounds_ok ? 1 : 0, len_m, wid_m, cmx, cmy, cmz, c2x, c2y);
      if (!bounds_ok) {
        std::printf("FAIL Valocity.bounds\n");
        return 5;
      }

      // Phase 2.75: forceUpdate + wheel damage save/load blobs (indices 0..3).
      tree_field_set_int(car, "suspend_update", 1);
      tree_field_set_int(car, "force_update_count", 0);
      java_game_parts_bodypart_Chassis_setWheelDamage(car, 0, string_new("flat"));
      java_game_parts_bodypart_Chassis_setWheelDamage(car, 3, string_new("bent"));
      InvObject* dmg0 = java_game_parts_bodypart_Chassis_getWheelDamage(car, 0);
      InvObject* dmg3 = java_game_parts_bodypart_Chassis_getWheelDamage(car, 3);
      InvObject* dmg1 = java_game_parts_bodypart_Chassis_getWheelDamage(car, 1);
      const char* wd0 = dmg0 ? string_cstr(dmg0) : "";
      const char* wd3 = dmg3 ? string_cstr(dmg3) : "";
      const char* wd1 = dmg1 ? string_cstr(dmg1) : "";
      // Chassis.save: while(wheels--) write(getWheelDamage(wheels)) with wheels=4.
      int wheels = 4;
      InvObject* save_order[4] = {};
      while (wheels--)
        save_order[wheels] = java_game_parts_bodypart_Chassis_getWheelDamage(
            car, wheels);
      java_game_parts_bodypart_Chassis_forceUpdate(car);
      const int32_t fu_n = tree_field_get_int(car, "force_update_count");
      const int32_t susp = tree_field_get_int(car, "suspend_update");
      const bool force_ok =
          wd0 && std::strcmp(wd0, "flat") == 0 && wd3 &&
          std::strcmp(wd3, "bent") == 0 && (!wd1 || wd1[0] == '\0') &&
          save_order[0] && string_cstr(save_order[0]) &&
          std::strcmp(string_cstr(save_order[0]), "flat") == 0 &&
          save_order[3] && string_cstr(save_order[3]) &&
          std::strcmp(string_cstr(save_order[3]), "bent") == 0 && fu_n == 1 &&
          susp == 0;
      std::printf("boot valocity_forceupd ok=%d dmg0='%s' dmg3='%s' "
                  "fu=%d susp=%d\n",
                  force_ok ? 1 : 0, wd0 ? wd0 : "", wd3 ? wd3 : "", fu_n, susp);
      if (!force_ok) {
        std::printf("FAIL Valocity.forceupd\n");
        return 5;
      }

      // Phase 2.76: cooling + SfxTable (engine/exhaust) + horn/nitro slots.
      java_game_parts_bodypart_Chassis_setCooling(car, 10.f, 50.f, 0.01f);
      const float cmin = tree_field_get_float(car, "cooling_min");
      const float cmax = tree_field_get_float(car, "cooling_max");
      const float cspd = tree_field_get_float(car, "cooling_spd");
      InvObject* tab0 = java_game_parts_bodypart_Chassis_getSfxTable(car, 0);
      InvObject* tab2 = java_game_parts_bodypart_Chassis_getSfxTable(car, 2);
      InvObject* tab0b = java_game_parts_bodypart_Chassis_getSfxTable(car, 0);
      InvObject* sfx_a = resref_new();
      InvObject* sfx_b = resref_new();
      java_util_resource_ResourceRef_set(sfx_a, 0xA2);
      java_util_resource_ResourceRef_set(sfx_b, 0xAB);
      if (tab0) {
        java_game_parts_SfxTable_clear(tab0);
        java_game_parts_SfxTable_addItem(tab0, sfx_a, 3500.f, 500.f, 6500.f,
                                        0.42f, 0.42f);
        java_game_parts_SfxTable_addItem(tab0, sfx_b, 5000.f, 3000.f, 18000.f,
                                        0.42f, 0.42f);
      }
      const int32_t n0 = java_game_parts_SfxTable_getItems(tab0);
      if (tab0) java_game_parts_SfxTable_clear(tab0);
      const int32_t n0c = java_game_parts_SfxTable_getItems(tab0);
      if (tab2) {
        java_game_parts_SfxTable_clear(tab2);
        java_game_parts_SfxTable_addItem(tab2, sfx_b, 1500.f, 750.f, 2500.f, 1.f,
                                        1.f);
      }
      java_game_parts_bodypart_Chassis_setSfxExhaustMinVol(car, 0.6f);
      const float exh = tree_field_get_float(car, "sfx_exhaust_min_vol");
      InvObject* horn = resref_new();
      java_util_resource_ResourceRef_set(horn, 0x15);
      java_game_parts_bodypart_Chassis_setHornSFX(car, horn, 1.f, 1);
      java_game_parts_bodypart_Chassis_setHornSFX(car, horn, 1.f, 3);
      InvObject* nitro = resref_new();
      java_util_resource_ResourceRef_set(nitro, 0x99);
      java_game_parts_bodypart_Chassis_setNitroSFX(car, nitro, 1.25f);
      InvObject* horn1 = tree_field_get_obj(car, "horn_sfx_1");
      InvObject* horn3 = tree_field_get_obj(car, "horn_sfx_3");
      InvObject* nsfx = tree_field_get_obj(car, "nitro_sfx");
      // PE setNitroSFX: pitch unboxed but never stored — no nitro_sfx_pitch.
      const bool sfx_ok =
          std::fabs(cmin - 10.f) < 0.01f && std::fabs(cmax - 50.f) < 0.01f &&
          std::fabs(cspd - 0.01f) < 1e-5f && tab0 && tab2 && tab0 == tab0b &&
          tab0 != tab2 && n0 == 2 && n0c == 0 &&
          java_game_parts_SfxTable_getItems(tab2) == 1 &&
          std::fabs(exh - 0.6f) < 0.01f && horn1 == horn && horn3 == horn &&
          nsfx == nitro &&
          java_game_parts_bodypart_Chassis_getSfxTable(car, 3) == nullptr;
      std::printf("boot valocity_sfx ok=%d cool=(%.0f,%.0f,%.2f) items=%d->%d "
                  "exh=%.1f horn=%d nitro=%d\n",
                  sfx_ok ? 1 : 0, cmin, cmax, cspd, n0, n0c, exh,
                  horn1 == horn ? 1 : 0, nsfx == nitro ? 1 : 0);
      if (!sfx_ok) {
        std::printf("FAIL Valocity.sfx\n");
        return 5;
      }

      // Phase 2.77: DynoData calcDyno / getTorque / getHP + Chassis setBuck.
      InvObject* dyno = tree_host_new("java.game.parts.DynoData");
      java_game_parts_DynoData_newNative(dyno);
      tree_field_set_float(dyno, "cylinders", 6.f);
      tree_field_set_float(dyno, "bore", 0.089f);
      tree_field_set_float(dyno, "stroke", 0.096f);
      tree_field_set_float(dyno, "maxRPM", 7000.f);
      tree_field_set_float(dyno, "RPM_limit", 7000.f);
      tree_field_set_float(dyno, "torque", 1.f);
      tree_field_set_float(dyno, "torque2", 1.4f);
      // PE @ 0x0046AA20 always fldz (0.0) @ 0x0046B0D5; Java reads fields.
      const float ret0 = java_game_parts_DynoData_calcDyno(dyno, 28.f);
      const float max_t = tree_field_get_float(dyno, "maxTorque");
      const float max_hp = tree_field_get_float(dyno, "maxHP");
      const float rpm_t = tree_field_get_float(dyno, "RPM_maxTorque");
      const float rpm_hp = tree_field_get_float(dyno, "RPM_maxHP");
      const float t_peak = java_game_parts_DynoData_getTorque(dyno, rpm_t, 0.f);
      const float t_idle = java_game_parts_DynoData_getTorque(dyno, 900.f, 0.f);
      const float t_nos = java_game_parts_DynoData_getTorque(dyno, rpm_t, 1.f);
      const float watts = java_game_parts_DynoData_getHP(dyno, rpm_hp, 0.f);
      const float hp_chk = watts * 0.001f * 1.341f;
      tree_field_set_obj(car, "dynodata", dyno);
      const float cht =
          java_game_parts_bodypart_Chassis_getTorque(car, rpm_t, 0.f);
      java_game_parts_bodypart_Chassis_setBuck(car, 42, 1, 12.f, 0.5f, 0.2f,
                                               0.8f);
      java_game_parts_bodypart_Chassis_setBuck(car, 42, 1, 15.f, 0.6f, 0.3f,
                                               1.0f);
      const int32_t bucks = tree_field_get_int(car, "buck_count");
      const bool dyno_ok =
          std::fabs(ret0) < 0.001f && max_t > 100.f && max_hp > 50.f &&
          rpm_t > 1000.f &&
          std::fabs(t_peak - max_t) < 1.f && t_idle < max_t * 0.10f &&
          std::fabs(t_nos - t_peak) < 1.f && std::fabs(hp_chk - max_hp) < 5.f &&
          std::fabs(cht - t_peak) < 0.5f && bucks == 1;
      std::printf("boot valocity_dyno ok=%d peak=%.0fNm hp=%.0f@%.0f "
                  "idle=%.0f nos=%.0f bucks=%d\n",
                  dyno_ok ? 1 : 0, max_t, max_hp, rpm_t, t_idle, t_nos, bucks);
      if (!dyno_ok) {
        std::printf("FAIL Valocity.dyno\n");
        return 5;
      }
      // Torque path: Chassis.getTorque → physics_set_drive_torque on body.
      InvObject* body_tq = tree_field_get_obj(car, "chassis");
      if (!body_tq) body_tq = car;
      const float nm_rt =
          java_game_parts_bodypart_Chassis_getTorque(car, rpm_t, 0.f);
      physics_set_drive_torque(body_tq, nm_rt);
      const float rpm_body = physics_get_engine_rpm(body_tq);
      tree_field_set_float(car, "engine_torque_nm", nm_rt);
      const bool tq_wire =
          nm_rt > 100.f && std::fabs(nm_rt - t_peak) < 1.f && rpm_body >= 0.f;
      std::printf("boot valocity_tqwire ok=%d nm=%.0f rpm_est=%.0f\n",
                  tq_wire ? 1 : 0, nm_rt, rpm_body);
      if (!tq_wire) {
        std::printf("FAIL Valocity.tqwire\n");
        return 5;
      }

      // Phase 2.78: Part wear/tear/maxWear + slot damage blobs.
      const float wear0 = java_game_parts_Part_getWear(w0);
      const float tear0 = java_game_parts_Part_getTear(w0);
      const float sw = java_game_parts_Part_setWear(w0, 0.72f);
      const float st = java_game_parts_Part_setTear(w0, 0.85f);
      java_game_parts_Part_setMaxWear(w0, 300000.f * 1000.f);
      const float mw = tree_field_get_float(w0, "max_wear");
      const float gw = java_game_parts_Part_getWear(w0);
      const float gt = java_game_parts_Part_getTear(w0);
      const float clamp_hi = java_game_parts_Part_setWear(w0, 1.5f);
      const float clamp_lo = java_game_parts_Part_setTear(w0, -0.2f);
      java_game_parts_Part_setSlotDamage(w0, 0, string_new("bent"));
      java_game_parts_Part_setSlotDamage(w0, 2, string_new("crack"));
      InvObject* sd0 = java_game_parts_Part_getSlotDamage(w0, 0);
      InvObject* sd1 = java_game_parts_Part_getSlotDamage(w0, 1);
      InvObject* sd2 = java_game_parts_Part_getSlotDamage(w0, 2);
      const char* sds0 = sd0 ? string_cstr(sd0) : "";
      const char* sds1 = sd1 ? string_cstr(sd1) : "";
      const char* sds2 = sd2 ? string_cstr(sd2) : "";
      java_game_parts_Part_setWear(w0, 0.72f);
      java_game_parts_Part_setTear(w0, 0.85f);
      // PE setWear/setTear: no clamp, always return value (0x469420 / 0x4696A0).
      const bool wear_ok =
          std::fabs(wear0 - 1.f) < 0.01f && std::fabs(tear0 - 1.f) < 0.01f &&
          std::fabs(sw - 0.72f) < 0.01f && std::fabs(st - 0.85f) < 0.01f &&
          std::fabs(gw - 0.72f) < 0.01f && std::fabs(gt - 0.85f) < 0.01f &&
          std::fabs(mw - 3.0e8f) < 1.f && std::fabs(clamp_hi - 1.5f) < 0.01f &&
          std::fabs(clamp_lo - (-0.2f)) < 0.01f && sds0 &&
          std::strcmp(sds0, "bent") == 0 && (!sds1 || sds1[0] == '\0') &&
          sds2 && std::strcmp(sds2, "crack") == 0;
      std::printf("boot valocity_wear ok=%d wear=%.2f tear=%.2f max=%.0e "
                  "slot0='%s'\n",
                  wear_ok ? 1 : 0, gw, gt, mw, sds0 ? sds0 : "");
      if (!wear_ok) {
        std::printf("FAIL Valocity.wear\n");
        return 5;
      }

      // Phase 2.79: Part texture / mesh / renderType resource IDs.
      const int32_t tex0 = java_game_parts_Part_getTexture(w0);
      const int32_t msh0 = java_game_parts_Part_getMesh(w0);
      const int32_t rt0 = java_game_parts_Part_getRenderType(w0);
      const int32_t prev_tex =
          java_game_parts_Part_setTexture(w0, 0x10042);
      const int32_t prev_msh = java_game_parts_Part_setMesh(w0, 0x20099);
      const int32_t prev_rt = java_game_parts_Part_setRenderType(w0, 21);
      const int32_t tex1 = java_game_parts_Part_getTexture(w0);
      const int32_t msh1 = java_game_parts_Part_getMesh(w0);
      const int32_t rt1 = java_game_parts_Part_getRenderType(w0);
      // Repair swap pattern: move mesh to self, clear donor.
      InvObject* donor = w0;
      const int32_t moved = java_game_parts_Part_setMesh(car, msh1);
      const int32_t cleared = java_game_parts_Part_setMesh(donor, 0);
      const int32_t car_msh = java_game_parts_Part_getMesh(car);
      const int32_t donor_msh = java_game_parts_Part_getMesh(donor);
      // Restore rim mesh for later checks.
      java_game_parts_Part_setMesh(w0, msh1);
      java_game_parts_Part_setMesh(car, moved);
      const bool mesh_ok =
          tex0 == 0 && msh0 == 0 && rt0 == 0 && prev_tex == 0 &&
          prev_msh == 0 && prev_rt == 0 && tex1 == 0x10042 &&
          msh1 == 0x20099 && rt1 == 21 && car_msh == 0x20099 && cleared == 0x20099 &&
          donor_msh == 0 &&
          java_game_parts_Part_setTexture(w0, tex1) == 0x10042;
      std::printf("boot valocity_meshids ok=%d tex=0x%X mesh=0x%X rt=%d "
                  "swap=%d\n",
                  mesh_ok ? 1 : 0, tex1, msh1, rt1, car_msh == 0x20099 ? 1 : 0);
      if (!mesh_ok) {
        std::printf("FAIL Valocity.meshids\n");
        return 5;
      }

      // Phase 2.80: flap toggle + disableSlot blocks install.
      const int32_t flap0 = java_game_parts_Part_flap(car, 0);
      const int32_t flap1 = java_game_parts_Part_flap(car, 1);
      const int32_t flap2 = java_game_parts_Part_flap(car, 0);
      const int32_t flap3 = java_game_parts_Part_flap(car, 1);
      java_game_parts_Part_disableSlot(car, 199, 1);
      InvObject* probe = gameref_new();
      const bool blocked = !part_install(car, 199, probe, 1);
      const bool disabled = part_slot_is_disabled(car, 199);
      java_game_parts_Part_disableSlot(car, 199, 0);
      const bool unlocked = !part_slot_is_disabled(car, 199);
      const bool installed = part_install(car, 199, probe, 1);
      const bool on199 = part_on_slot(car, 199) == probe;
      // Clear probe slot so later checks stay clean.
      // (leave child; slot 199 unused by Valocity stock.)
      const bool flap_ok =
          flap0 == 0 && flap1 == 1 && flap2 == 1 && flap3 == 0 && blocked &&
          disabled && unlocked && installed && on199;
      std::printf("boot valocity_flap ok=%d flap=%d->%d->%d block=%d "
                  "unlock_install=%d\n",
                  flap_ok ? 1 : 0, flap0, flap1, flap3, blocked ? 1 : 0,
                  installed ? 1 : 0);
      if (!flap_ok) {
        std::printf("FAIL Valocity.flap\n");
        return 5;
      }

      InvObject* ok_arr = java_game_parts_Part_install_OK(
          nullptr, car, 102, w0, 1, nullptr);
      // Restore FL rim on slot 101 after the install_OK probe.
      part_install(car, 101, w0, 1);
      const bool install_ok =
          w0 && t0 && on101 == w0 && mate101 == 1 && on_rim2 == t0 &&
          mate_rim2 == 1 && ok_arr && tree_vector_size(ok_arr) == 2;
      std::printf("boot valocity_install ok=%d on101=%d mate101=%d "
                  "rim2_tyre=%d mate2=%d install_OK=%d\n",
                  install_ok ? 1 : 0, on101 == w0 ? 1 : 0, mate101,
                  on_rim2 == t0 ? 1 : 0, mate_rim2,
                  ok_arr && tree_vector_size(ok_arr) == 2 ? 1 : 0);
      if (!install_ok) {
        std::printf("FAIL Valocity.install\n");
        return 5;
      }
    }

    // Phase 2.36: OSD speed/gear HUD while still moving.
    valocity_update_hud(city);
    const float hud_kph = valocity_speed_kph(car);
    const char* speed_txt =
        render_d3d9_text_get_string(valocity_hud_speed_key());
    const char* gear_txt = render_d3d9_text_get_string(valocity_hud_gear_key());
    const bool hud_ok =
        sim_ok && hud_kph > 5.f && speed_txt && std::strstr(speed_txt, "KPH") &&
        gear_txt && gear_txt[0] != '\0' && render_d3d9_osd_text_count() >= 2;
    std::printf("boot valocity_hud ok=%d kph=%.0f speed='%s' gear='%s' "
                "texts=%d\n",
                hud_ok ? 1 : 0, hud_kph, speed_txt ? speed_txt : "",
                gear_txt ? gear_txt : "", render_d3d9_osd_text_count());
    // Park for garage-trigger tick (needs speed_sq < 0.25).
    {
      InvObject* body = tree_field_get_obj(car, "chassis");
      if (!body) body = car;
      physics_set_velocity(body, 0.f, 0.f, 0.f);
      tree_field_set_float(car, "speed_sq", 0.f);
      java_util_resource_PhysicsRef_setMatrix(
          body,
          vec3_new(tree_field_get_float(car, "pos_x"),
                   tree_field_get_float(car, "pos_y"),
                   tree_field_get_float(car, "pos_z")),
          ypr_new(tree_field_get_float(car, "ori_y"), 0.f, 0.f));
      physics_set_velocity(body, 0.f, 0.f, 0.f);
      tree_field_set_float(car, "speed_sq", 0.f);
    }
    if (!sim_ok) {
      std::printf("FAIL Valocity.simulate\n");
      return 5;
    }
    if (!hud_ok) {
      std::printf("FAIL Valocity.hud\n");
      return 5;
    }

    // Phase 2.37: Navigator minimap vp/cam + OSD tile under car.
    InvObject* nav37 = tree_field_get_obj(city, "nav");
    InvObject* nvp = navigator_viewport(nav37);
    InvObject* ncam = navigator_camera(nav37);
    const int32_t ntile = navigator_current_tile(nav37);
    InvObject* ntex = nav37 ? tree_field_get_obj(nav37, "osd_tex") : nullptr;
    const bool nav_ok =
        nav37 && nvp && ncam && ntile >= 0 &&
        render_d3d9_camera_half_aov(ncam) > 40.f &&
        render_d3d9_viewport_get_width(nvp) > 0.15f &&
        ntex && render_d3d9_texture_ready(ntex) &&
        render_d3d9_osd_count() >= 2 &&
        tree_field_get_int(nav37, "update_count") > 0;
    std::printf("boot valocity_nav ok=%d tile=%d vp=%.2fx%.2f cam_aov=%.0f "
                "tex=%d osd=%d updates=%d\n",
                nav_ok ? 1 : 0, ntile,
                nvp ? render_d3d9_viewport_get_width(nvp) : 0.f,
                nvp ? render_d3d9_viewport_get_height(nvp) : 0.f,
                ncam ? render_d3d9_camera_half_aov(ncam) : 0.f,
                ntex && render_d3d9_texture_ready(ntex) ? 1 : 0,
                render_d3d9_osd_count(),
                nav37 ? tree_field_get_int(nav37, "update_count") : 0);
    if (!nav_ok) {
      std::printf("FAIL Valocity.nav\n");
      return 5;
    }

    const float cpx = tree_field_get_float(car, "pos_x");
    const float cpy = tree_field_get_float(car, "pos_y");
    const float cpz = tree_field_get_float(car, "pos_z");

    // Phase 2.89: navigator paints plotRoute line as OSD dots.
    physics_road_clear();
    physics_road_add_segment(cpx - 50.f, cpy, cpz, cpx + 150.f, cpy, cpz);
    physics_road_add_segment(cpx + 150.f, cpy, cpz, cpx + 150.f, cpy,
                             cpz + 80.f);
    InvObject* map89 = tree_field_get_obj(city, "map");
    if (!map89) map89 = resref_new();
    const float rlen89 = java_util_resource_GroundRef_findRoute(
        map89, vec3_new(cpx, cpy, cpz),
        vec3_new(cpx + 150.f, cpy, cpz + 80.f));
    InvObject* route89 = resref_new();
    InvObject* route89_root = resref_new();
    InvObject* route89_type = resref_new();
    java_util_resource_ResourceRef_set(route89_type, 0x17);
    const int32_t rpts89 = java_util_resource_RenderRef_plotRoute(
        route89, route89_root, route89_type, 0xFFFF0000, 20.f,
        vec3_new(0.01f, 0.f, 0.01f));
    tree_field_set_obj(nav37, "route", route89);
    java_game_Navigator_updateNavigator(nav37, car, 0);
    const int32_t route_vis = tree_field_get_int(nav37, "route_osd_visible");
    const int32_t route_n = tree_field_get_int(nav37, "route_osd_n");
    const int32_t osd_after = render_d3d9_osd_count();
    const int32_t route_pts = render_line_point_count(route89);
    const bool nav_route_ok =
        rlen89 > 200.f && rpts89 == 1 && route_pts >= 5 && route_vis >= 3 &&
        route_n == route_vis && osd_after >= 2 + route_n;
    std::printf("boot valocity_navroute ok=%d len=%.0f ret=%d pts=%d osd_dots=%d "
                "osd_total=%d\n",
                nav_route_ok ? 1 : 0, rlen89, rpts89, route_pts, route_vis,
                osd_after);
    physics_road_clear();
    if (!nav_route_ok) {
      std::printf("FAIL Valocity.navroute\n");
      return 5;
    }

    // Phase 2.90: static/dynamic markers painted on minimap OSD.
    java_game_Navigator_updateNavigator(nav37, car, 0);
    const int32_t mk_vis = tree_field_get_int(nav37, "marker_osd_visible");
    const int32_t mk_n = tree_field_get_int(nav37, "marker_osd_n");
    const int32_t mk_static = tree_field_get_int(nav37, "marker_count");
    const int32_t mk_dyn = tree_field_get_int(nav37, "dynamarker_count");
    const int32_t mk_tex = tree_field_get_int(nav37, "marker_osd_tex");
    // At club-0 spawn: garage0 static + player dyn near center (≥2).
    // Phase 2.91: at least one visible marker uses a loaded rtype texture.
    InvObject* mk0 = nullptr;
    if (InvObject* statics = tree_field_get_obj(nav37, "marker"))
      mk0 = tree_vector_element_at(statics, 0);
    InvObject* icon0 = mk0 ? tree_field_get_obj(mk0, "icon") : nullptr;
    InvObject* flat0 = mk0 ? tree_field_get_obj(mk0, "icon_flat") : nullptr;
    const bool icon_loaded =
        icon0 && (render_d3d9_texture_ready(icon0) ||
                  render_d3d9_mesh_ready(icon0) ||
                  (flat0 && render_d3d9_texture_ready(flat0)));
    const bool nav_mark_ok =
        mk_static >= 3 && mk_dyn >= 1 && mk_vis >= 2 && mk_n == mk_vis;
    const bool nav_icon_ok = nav_mark_ok && mk_tex >= 1 && icon_loaded;
    std::printf("boot valocity_navmark ok=%d static=%d dyn=%d osd=%d\n",
                nav_mark_ok ? 1 : 0, mk_static, mk_dyn, mk_vis);
    std::printf("boot valocity_navicon ok=%d tex_dots=%d icon_mesh=%d "
                "icon_tex=%d flat=%d\n",
                nav_icon_ok ? 1 : 0, mk_tex,
                icon0 && render_d3d9_mesh_ready(icon0) ? 1 : 0,
                icon0 && render_d3d9_texture_ready(icon0) ? 1 : 0,
                flat0 && render_d3d9_texture_ready(flat0) ? 1 : 0);
    if (!nav_mark_ok) {
      std::printf("FAIL Valocity.navmark\n");
      return 5;
    }
    if (!nav_icon_ok) {
      std::printf("FAIL Valocity.navicon\n");
      return 5;
    }

    // Phase 2.94: Vehicle horn via queueEvent("sethorn N") / getHorn.
    // Also exercises host Vehicle.command (City / Bot.pressHorn path).
    {
      constexpr int32_t kEventCommand = 0x10;
      const int32_t h_off0 = java_game_Vehicle_getHorn(car);
      java_util_resource_GameRef_queueEvent(car, nullptr, kEventCommand,
                                            string_new("sethorn 1"));
      const int32_t h_on = java_game_Vehicle_getHorn(car);
      java_util_resource_GameRef_queueEvent(car, nullptr, kEventCommand,
                                            string_new("sethorn 0"));
      const int32_t h_off1 = java_game_Vehicle_getHorn(car);
      java_util_resource_GameRef_queueEvent(car, nullptr, kEventCommand,
                                            string_new("  sethorn 1"));
      const int32_t h_on2 = java_game_Vehicle_getHorn(car);
      java_util_resource_GameRef_queueEvent(car, nullptr, kEventCommand,
                                            string_new("sethorn 0"));
      JvmValue cmd_out;
      const bool cmd_host = game_script_try_host_method(
          "java.game.Vehicle", "command", "(Ljava.lang.String;)V",
          {JvmValue::make_obj(car),
           JvmValue::make_obj(string_new("sethorn 1"))},
          false, &cmd_out);
      const int32_t h_cmd = java_game_Vehicle_getHorn(car);
      java_util_resource_GameRef_queueEvent(car, nullptr, kEventCommand,
                                            string_new("sethorn 0"));
      const bool horn_ok =
          h_off0 == 0 && h_on == 1 && h_off1 == 0 && h_on2 == 1 && cmd_host &&
          h_cmd == 1 && java_game_Vehicle_getHorn(car) == 0;
      std::printf("boot valocity_horn ok=%d off0=%d on=%d off1=%d on2=%d "
                  "cmd=%d\n",
                  horn_ok ? 1 : 0, h_off0, h_on, h_off1, h_on2, h_cmd);
      if (!horn_ok) {
        std::printf("FAIL Valocity.horn\n");
        return 5;
      }
    }

    // Phase 2.35: chase camera sits behind the car facing forward.
    valocity_update_camera(city);
    void* vcam = valocity_camera_key();
    float ex = 0, ey = 0, ez = 0, ax = 0, ay = 0, az = 0;
    const bool got =
        render_d3d9_camera_get_lookat(vcam, &ex, &ey, &ez, &ax, &ay, &az);
    const float coy = tree_field_get_float(car, "ori_y");
    const float cfx = std::sin(coy);
    const float cfz = std::cos(coy);
    const float behind = (ex - cpx) * cfx + (ez - cpz) * cfz;
    const bool cam_ok35 =
        got && render_d3d9_camera_active() == vcam && behind < -2.f &&
        ey > cpy + 1.f && (ax - cpx) * cfx + (az - cpz) * cfz > 0.5f;
    std::printf("boot valocity_cam ok=%d behind=%.1f eye_y=%.1f lookat=%d\n",
                cam_ok35 ? 1 : 0, behind, ey, got ? 1 : 0);
    if (!cam_ok35) {
      std::printf("FAIL Valocity.camera\n");
      return 5;
    }

    // Higher club deny then club-0 trigger path home.
    valocity_fire_garage_trigger(city, 2, 1);
    if (valocity_tick(city) || tree_field_get_int(city, "garage_denied") != 1) {
      std::printf("FAIL garage deny\n");
      return 5;
    }
    valocity_fire_garage_trigger(city, 1, 1);
    InvObject* back = valocity_tick(city);
    InvObject* cur3 = game_logic_actual_state();
    std::printf(
        "boot trigger→garage=%d traffic=%d car_stopped=%d loading=%d\n",
        back && cur3 == back ? 1 : 0,
        city ? tree_field_get_int(city, "traffic_count") : -1,
        car ? tree_field_get_int(car, "stopped") : -1,
        frontend_loading_screen_visible());
    if (!back || cur3 != back || !std::strstr(tree_host_class(back), "Garage") ||
        tree_field_get_int(city, "traffic_count") != 0 ||
        tree_field_get_int(car, "stopped") != 1 ||
        tree_field_get_int(back, "entered") != 1 ||
        tree_field_get_int(back, "enter_via_tree") != 1 ||
        tree_field_get_int(city, "exit_via_tree") != 1 ||
        frontend_loading_screen_visible() != 0) {
      std::printf("FAIL Valocity→Garage\n");
      return 5;
    }

    constexpr int32_t CMD_TIME = 116;
    for (int i = 0; i < 11; ++i) garage_osd_command(garage2, CMD_TIME);
    InvObject* night = garage_osd_command(garage2, CMD_HITTHESTREET);
    const int32_t ntraffic =
        night ? tree_field_get_int(night, "traffic_count") : 0;
    std::printf("boot night time=%.0fs daytime=%d traffic=%d\n",
                game_logic_time(),
                night ? tree_field_get_int(night, "daytime") : -1, ntraffic);
    if (!night || tree_field_get_int(night, "daytime") != 0 || ntraffic != 183) {
      std::printf("FAIL night Valocity\n");
      return 5;
    }
    {
      const int32_t ncfg = valocity_scene_config(night);
      InvObject* nsun = night ? tree_field_get_obj(night, "suntype") : nullptr;
      const int32_t nflare =
          nsun ? tree_field_get_int(nsun, "flare_count") : -1;
      const bool nscene_ok =
          ncfg == 0 && render_d3d9_fog_enabled() &&
          render_d3d9_fog_color() == 0x0007121e &&
          render_d3d9_fog_near() > 19.f && render_d3d9_fog_far() > 149.f &&
          render_d3d9_light_enabled() &&
          render_d3d9_light_diffuse() == 0x00466285 &&
          render_d3d9_light_ambient() == 0x0007121e && nflare == 0;
      std::printf("boot valocity_scene_night ok=%d cfg=%d fog=%06X light=%06X "
                  "flare=%d\n",
                  nscene_ok ? 1 : 0, ncfg, render_d3d9_fog_color(),
                  render_d3d9_light_diffuse(), nflare);
      if (!nscene_ok) {
        std::printf("FAIL Valocity.scene night\n");
        return 5;
      }
    }
    valocity_fire_garage_trigger(night, 1, 1);
    InvObject* backn = valocity_tick(night);
    if (!backn || game_logic_actual_state() != backn ||
        tree_field_get_int(night, "traffic_count") != 0) {
      std::printf("FAIL night→Garage\n");
      return 5;
    }
    InvObject* closed = garage_osd_command(garage2, CMD_BUYCARS);
    InvObject* warn = tree_field_get_obj(garage2, "last_warning");
    const char* ws = warn ? string_cstr(warn) : nullptr;
    if (closed || !ws || !std::strstr(ws, "closed")) {
      std::printf("FAIL dealer closed at night\n");
      return 5;
    }
    std::printf("boot dealer closed at night ok\n");
    (void)ivt;
  }

  // Optional bulk index still useful for non-loaded packages.
  if (const char* idx = class_index_path()) {
    Jvm bulk;
    if (bulk.load_index(idx)) {
      std::printf("index classes=%zu from %s\n", bulk.class_count(), idx);
    }
  }

  // File roundtrip
  const char* path = "native_engine_smoke.tmp";
  InvObject* f = file_new(path);
  if (!java_io_File_open(f, 1)) {
    std::printf("FAIL open write\n");
    return 1;
  }
  java_io_File_write(f, 123);
  java_io_File_write_1(f, 4.5f);
  java_io_File_write_3(f, string_new("redline"));
  // Phase 2.116: write(ResourceRef) → packed id (saveGame / Part.save).
  InvObject* rr_save = gameref_new();
  java_util_resource_ResourceRef_set(rr_save, 0x00C0FFEE);
  java_io_File_write_2(f, rr_save);
  java_io_File_write_2(f, nullptr);  // null → 0
  java_io_File_close(f);

  f = file_new(path);
  java_io_File_open(f, 0);
  const int32_t iv = java_io_File_readInt(f);
  const float fv = java_io_File_readFloat(f);
  InvObject* sv = java_io_File_readString(f);
  const int32_t rid = java_io_File_readResID(f);
  const int32_t rid0 = java_io_File_readResID(f);
  java_io_File_close(f);
  const bool file_res_ok = rid == 0x00C0FFEE && rid0 == 0;
  std::printf("file rw int=%d float=%.1f str='%s' resid=0x%X nullid=%d ok=%d\n",
              iv, fv, string_cstr(sv), rid, rid0, file_res_ok ? 1 : 0);
  if (!file_res_ok || iv != 123) {
    std::printf("FAIL File.write(ResourceRef) / readResID\n");
    if (want_window) render_d3d9_close();
    return 2;
  }

  std::printf("exists=%d\n", java_io_File_exists(string_new(path)));
  java_io_File_delete(string_new(path));
  std::printf("exists_after_delete=%d\n", java_io_File_exists(string_new(path)));

  // FindFile
  InvObject* ff = findfile_new();
  InvObject* name = java_io_FindFile_first(ff, string_new("native\\engine\\src\\*.cpp"), 1);
  std::printf("find first='%s'\n", string_cstr(name));
  java_io_FindFile_close(ff);

  // Thread — Phase 2.117: start() spawns OS thread → target.run().
  InvObject* th = thread_new("t");
  java_lang_Thread_init(th, string_new("worker"));
  tree_field_set_obj(th, "target", th);
  java_lang_Thread_start(th);
  const int32_t alive_mid = java_lang_Thread_isAlive(th);
  java_lang_Thread_sleep(50.f);
  // Empty Thread.run() finishes quickly; join via stop.
  java_lang_Thread_stop(th);
  const int32_t entered = tree_field_get_int(th, "engine_run_entered");
  const int32_t done = tree_field_get_int(th, "engine_run_done");
  const int32_t alive_after = java_lang_Thread_isAlive(th);
  const bool thread_ok = entered == 1 && done == 1 && alive_after == 0;
  std::printf("thread start_alive=%d entered=%d done=%d stopped=%d ok=%d\n",
              alive_mid, entered, done, alive_after, thread_ok ? 1 : 0);
  if (!thread_ok) {
    std::printf("FAIL Thread.start→run\n");
    if (want_window) render_d3d9_close();
    return 2;
  }

  // Phase 2.118: Frontend.render.wait() woken by render_d3d9_flush notify.
  {
    InvObject* gfx = frontend_gfx_engine();
    // Drain sticky tokens from earlier flush Present calls.
    for (int i = 0; i < 200000; ++i) {
      const int32_t nc = tree_field_get_int(gfx, "notify_count");
      const int32_t wc = tree_field_get_int(gfx, "wake_count");
      if (wc >= nc) break;
      java_lang_Object_wait(gfx);
    }
    std::atomic<int> stage{0};
    std::thread waiter([&] {
      stage.store(1);
      java_lang_Object_wait(gfx);
      stage.store(2);
    });
    for (int i = 0; i < 200 && stage.load() != 1; ++i) Sleep(1);
    const int before = stage.load();
    Sleep(40);
    const int still = stage.load();
    render_d3d9_flush();
    waiter.join();
    const int after = stage.load();
    const int32_t wake = tree_field_get_int(gfx, "wake_count");
    const bool render_wait_ok =
        before == 1 && still == 1 && after == 2 && wake >= 1;
    std::printf("boot render_wait ok=%d block=%d->%d wake=%d\n",
                render_wait_ok ? 1 : 0, still, after, wake);
    if (!render_wait_ok) {
      std::printf("FAIL Frontend.render.wait / flush notify\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.119: LoadingScreen.run parks on self after kill; show wakes it.
  {
    InvObject* ls = frontend_loading_screen();
    // Ensure run thread entered and drained to parked wait (init=0, !loading).
    for (int i = 0; i < 400; ++i) {
      if (tree_field_get_int(ls, "engine_run_entered") == 1 &&
          tree_field_get_int(ls, "run_parked") == 1)
        break;
      render_d3d9_flush();
      Sleep(5);
    }
    const int32_t entered = tree_field_get_int(ls, "engine_run_entered");
    const int32_t parked0 = tree_field_get_int(ls, "run_parked");
    const int32_t vis0 = frontend_loading_screen_visible();

    frontend_loading_screen_show();
    const int32_t vis1 = frontend_loading_screen_visible();
    // init=1 → first frame decrements; second frame kills → park.
    // (re-park can finish before we sample run_parked after show — vis is enough)
    render_d3d9_flush();
    Sleep(350);
    render_d3d9_flush();
    Sleep(50);
    for (int i = 0; i < 200 && tree_field_get_int(ls, "run_parked") == 0; ++i) {
      render_d3d9_flush();
      Sleep(10);
    }
    const int32_t vis2 = frontend_loading_screen_visible();
    const int32_t parked2 = tree_field_get_int(ls, "run_parked");

    const bool ls_run_ok = entered == 1 && parked0 == 1 && vis0 == 0 &&
                           vis1 == 1 && vis2 == 0 && parked2 == 1;
    std::printf("boot loading_run ok=%d entered=%d park=%d->%d vis=%d->%d->%d\n",
                ls_run_ok ? 1 : 0, entered, parked0, parked2, vis0, vis1, vis2);
    if (!ls_run_ok) {
      std::printf("FAIL LoadingScreen.run park/show/hide\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }

  // Phase 2.120: Frontend.init — fonts (800x600→SLII) + HotkeyWatcher thread.
  {
    jvm.invoke("java.render.Frontend", "init", "()V", {}, true);
    const int32_t inited = frontend_inited();
    InvObject* lg = frontend_large_font();
    InvObject* md = frontend_medium_font();
    InvObject* sm = frontend_small_font();
    InvObject* ptr = frontend_pointers();
    InvObject* defp = frontend_def_loading_pic();
    InvObject* q = frontend_input_queue();
    InvObject* hk = frontend_hotkey_thread();
    const int32_t lg_id = lg ? java_util_resource_ResourceRef_id(lg) : 0;
    const int32_t md_id = md ? java_util_resource_ResourceRef_id(md) : 0;
    const int32_t sm_id = sm ? java_util_resource_ResourceRef_id(sm) : 0;
    const int32_t ptr_id = ptr ? java_util_resource_ResourceRef_id(ptr) : 0;
    const int32_t def_id = defp ? java_util_resource_ResourceRef_id(defp) : 0;
    // Pack-relative locals: SLII24=0x25, SLII17=0x139, SLII11=0x138, ptr=0x3c, pic=0xA8
    const bool fonts_ok = (lg_id & 0xFFFF) == 0x0025 &&
                          (md_id & 0xFFFF) == 0x0139 &&
                          (sm_id & 0xFFFF) == 0x0138 &&
                          (ptr_id & 0xFFFF) == 0x003C &&
                          (def_id & 0xFFFF) == 0x00A8;
    Sleep(120);
    const int32_t hk_alive = hk ? java_lang_Thread_isAlive(hk) : 0;
    InvObject* osd = tree_host_new("java.render.Osd");
    tree_vector_add(q, osd);
    InvObject* cursor = tree_host_new("java.io.MouseCursor");
    InvObject* ctrl = input_init_controllers();
    tree_field_set_obj(cursor, "controller", ctrl);
    // Watcher ticks only when queue+cursor.controller set — mirror Input.cursor.
    // Host watcher uses its own static cursor; seed that path via check once.
    java_io_Input_checkHotkeys(ctrl, osd);
    Sleep(150);
    InvObject* watcher = tree_field_get_obj(hk, "target");
    const int32_t ticks =
        watcher ? tree_field_get_int(watcher, "ticks") : 0;
    jvm.invoke("java.render.Frontend", "destroy", "()V", {}, true);
    Sleep(80);
    const int32_t hk_after = hk ? java_lang_Thread_isAlive(hk) : 0;
    const bool fe_ok = inited == 1 && fonts_ok && q && hk_alive == 1 &&
                       hk_after == 0;
    std::printf("boot frontend_init ok=%d fonts=%d hk=%d->%d ticks=%d "
                "lg=0x%X md=0x%X sm=0x%X\n",
                fe_ok ? 1 : 0, fonts_ok ? 1 : 0, hk_alive, hk_after, ticks,
                lg_id, md_id, sm_id);
    if (!fe_ok) {
      std::printf("FAIL Frontend.init fonts/HotkeyWatcher\n");
      if (want_window) render_d3d9_close();
      return 2;
    }
  }