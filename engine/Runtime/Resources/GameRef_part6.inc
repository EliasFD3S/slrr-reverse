// Fragment 6/6 of GameRef.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.

    java_util_resource_PhysicsRef_createBox(chassis, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    // Clear empty + seed stale GameRefState (1,2,3); Soft applyWorldXform
    // also poses chassis. Then move phys only so Soft must prefer live.
    java_util_resource_GameRef_setMatrix(car, vec3_new(1.f, 2.f, 3.f),
                                         ypr_new(0.1f, 0.f, 0.f));
    java_util_resource_PhysicsRef_setMatrix(
        chassis, vec3_new(11.f, 22.f, 33.f), ypr_new(0.5f, 0.f, 0.f));
    physics_set_velocity(chassis, 3.f, 0.f, 4.f);
    InvObject* gp = java_util_resource_GameRef_getPos(car);
    InvObject* gv = java_util_resource_GameRef_getVel(car);
    InvObject* go = java_util_resource_GameRef_getOri(car);
    float px = 0.f, py = 0.f, pz = 0.f;
    float vx = 0.f, vy = 0.f, vz = 0.f;
    float oy = 0.f, op = 0.f, or_ = 0.f;
    if (gp) vec3_get(gp, &px, &py, &pz);
    if (gv) vec3_get(gv, &vx, &vy, &vz);
    if (go) ypr_get(go, &oy, &op, &or_);
    float sx = 0.f, sy = 0.f, sz = 0.f;
    float svx = 0.f, svy = 0.f, svz = 0.f;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      // PE sort_compact @ 0x581170 passes car+0x88 / +0xA8.
      traffic_car_sample_pos(car, &sx, &sy, &sz, /*cursor_88=*/0.f,
                             /*lane_a8=*/1.f);
      // Soft vel ch=3 under g_gr_mu — PhysicsRef only, never nest getVel.
      gameref_soft_phys_sample_vel(car, &svx, &svy, &svz);
    }
    float cx = 0.f, cy = 0.f, cz = 0.f;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto it = g_refs.find(car);
      if (it != g_refs.end()) {
        cx = it->second.px;
        cy = it->second.py;
        cz = it->second.pz;
      }
    }
    const int32_t soft_phys_ok =
        (gp && gv && go && std::fabs(px - 11.f) < 0.01f &&
         std::fabs(py - 22.f) < 0.01f && std::fabs(pz - 33.f) < 0.01f &&
         std::fabs(vx - 3.f) < 0.01f && std::fabs(vy) < 0.01f &&
         std::fabs(vz - 4.f) < 0.01f && std::fabs(oy - 0.5f) < 0.01f &&
         std::fabs(sx - 11.f) < 0.01f && std::fabs(sy - 22.f) < 0.01f &&
         std::fabs(sz - 33.f) < 0.01f && std::fabs(svx - 3.f) < 0.01f &&
         std::fabs(svy) < 0.01f && std::fabs(svz - 4.f) < 0.01f &&
         std::fabs(cx - 11.f) < 0.01f && std::fabs(cy - 22.f) < 0.01f &&
         std::fabs(cz - 33.f) < 0.01f)
            ? 1
            : 0;
    tree_field_set_int(map, "soft_phys_sample_smoke", soft_phys_ok);
    tree_field_set_int(city, "soft_phys_sample_smoke", soft_phys_ok);
    std::printf("  traffic soft phys sample ok=%d\n", soft_phys_ok);
  }
  // PE setParent @ 0x0047E2D0 / GameRef_setParent_inner @ 0x0048ABA0:
  // handle 0 → Mighty ERROR no write. parent null crashes; parent
  // Native.ptr 0 → no-op (no detach). Type 2/3: +0x30/+0x38 child lists
  // (host vector stand-in). Stock Java always passes a live parent.
  {
    InvObject* car = gameref_new();
    InvObject* pmap = gameref_new();
    InvObject* pplayer = gameref_new();
    InvObject* empty_parent = gameref_new();
    InvObject* no_handle = gameref_new();
    java_util_resource_ResourceRef_set(car, 1);
    java_util_resource_ResourceRef_set(pmap, 2);
    java_util_resource_ResourceRef_set(pplayer, 3);
    java_util_resource_GameRef_setParent(car, pplayer);
    const int32_t under_player =
        java_util_resource_ResourceRef_getParentID(car) == 3;
    java_util_resource_GameRef_setParent(car, pmap);
    const int32_t lock_map =
        java_util_resource_ResourceRef_getParentID(car) == 2;
    java_util_resource_GameRef_setParent(car, nullptr);
    const int32_t null_noop =
        java_util_resource_ResourceRef_getParentID(car) == 2;
    java_util_resource_GameRef_setParent(car, empty_parent);
    const int32_t empty_noop =
        java_util_resource_ResourceRef_getParentID(car) == 2;
    java_util_resource_GameRef_setParent(car, pplayer);
    const int32_t release_player =
        java_util_resource_ResourceRef_getParentID(car) == 3;
    java_util_resource_GameRef_setParent(no_handle, pmap);
    const int32_t no_handle_noop =
        java_util_resource_ResourceRef_getParentID(no_handle) == 0;
    const int32_t setparent_ok =
        (under_player && lock_map && null_noop && empty_noop &&
         release_player && no_handle_noop)
            ? 1
            : 0;
    tree_field_set_int(map, "setparent_smoke", setparent_ok);
    tree_field_set_int(city, "setparent_smoke", setparent_ok);
    std::printf("  traffic setParent ok=%d\n", setparent_ok);
  }
  // PE getSpeedSquare @ 0x00480500: handle 0 → 0.0. Channel 3 vel → |v|².
  // Vehicle.set(chassis) Native.ptr; no physics → GameRef vx (setState).
  {
    InvObject* posed = gameref_new();
    java_util_resource_ResourceRef_set(posed, 1);
    java_util_resource_GameRef_setState(posed, vec3_new(0.f, 0.f, 0.f), nullptr,
                                        vec3_new(1.f, 2.f, 3.f), nullptr);
    const float posed_sq = java_game_Vehicle_getSpeedSquare(posed);
    InvObject* empty = gameref_new();
    const float empty_sq = java_game_Vehicle_getSpeedSquare(empty);
    const float null_sq = java_game_Vehicle_getSpeedSquare(nullptr);
    InvObject* veh = gameref_new();
    tree_field_set_obj(veh, "chassis", posed);
    const float veh_sq = java_game_Vehicle_getSpeedSquare(veh);
    const int32_t getspeedsq_ok =
        (std::fabs(posed_sq - 14.f) < 0.01f && std::fabs(empty_sq) < 0.01f &&
         std::fabs(null_sq) < 0.01f && std::fabs(veh_sq - 14.f) < 0.01f)
            ? 1
            : 0;
    tree_field_set_int(map, "getspeedsq_smoke", getspeedsq_ok);
    tree_field_set_int(city, "getspeedsq_smoke", getspeedsq_ok);
    std::printf("  traffic getSpeedSquare ok=%d\n", getspeedsq_ok);
  }
}

}  // namespace

int32_t scene_time2config(float time_sec, float rnd) {
  // Scene.time2Config — fixed rnd for host determinism (stock uses Math.random).
  const float hour = time_sec / 3600.f;
  if (hour < 3.f || hour >= 22.f) return 0;
  if (hour < 4.f) return rnd < 0.5f ? 1 : 2;
  if (hour < 8.f) {
    if (rnd < 0.33f) return 3;
    if (rnd < 0.66f) return 4;
    return 5;
  }
  if (hour < 10.f) return 6;
  if (hour < 19.f) {
    if (rnd < 0.25f) return 7;
    if (rnd < 0.5f) return 8;
    if (rnd < 0.75f) return 9;
    return 10;
  }
  if (hour < 20.5f) return 11;
  if (rnd < 0.33f) return 12;
  if (rnd < 0.66f) return 13;
  return 14;
}

int32_t valocity_scene_config(InvObject* city) {
  return city ? tree_field_get_int(city, "scene_config") : -1;
}

void valocity_apply_scene(InvObject* city) {
  if (!city) return;
  if (!rpak_find_by_name("skydome.rpk"))
    java_lang_System_openLib(string_new("maps/skydome.rpk"));

  // Deterministic variant pick (rnd=0 → first branch of each band).
  const int32_t config = scene_time2config(game_logic_time(), 0.f);
  tree_field_set_int(city, "scene_config", config);

  struct SceneLook {
    int32_t fog_rgb;
    float fog_near;
    float fog_far;
    int32_t env_local;
    int32_t sky_local;
    int32_t light_diff;
    int32_t light_amb;
    int32_t light_spec;
    int32_t flare_color;
    float flare_min;
    float flare_max;
    int32_t flare_count;
    int32_t flare_rays;
  };
  // Mirrors Scene.addSceneElements configs 0..14 (primary variants).
  // setFlare only configs 6–10 (maps.skydome:0x0100r).
  static const SceneLook kLooks[] = {
      {0x0007121e, 20.f, 150.f, 0x35, 0x23, 0x00466285, 0x0007121e, 0x00466285, 0, 0.f, 0.f, 0, 0},  // 0 night
      {0x00171516, 50.f, 200.f, 0x30, 0x1d, 0x006889A6, 0x001F212D, 0x006889A6, 0, 0.f, 0.f, 0, 0},  // 1
      {0x00111113, 50.f, 200.f, 0x31, 0x1e, 0x005D5D77, 0x0017142F, 0x005D5D77, 0, 0.f, 0.f, 0, 0},  // 2
      {0x0031364B, 50.f, 200.f, 0x2E, 0x15, 0x00CBB9AA, 0x002A3047, 0x00CBB9AA, 0, 0.f, 0.f, 0, 0},  // 3
      {0x00696063, 50.f, 200.f, 0x2F, 0x16, 0x00C1BC95, 0x00515F5D, 0x00C1BC95, 0, 0.f, 0.f, 0, 0},  // 4
      {0x00080808, 50.f, 200.f, 0x32, 0x1F, 0x00534FB2, 0x00140E2D, 0x00534FB2, 0, 0.f, 0.f, 0, 0},  // 5
      {0x00B0BCBC, 70.f, 400.f, 0x33, 0x21, 0x00C4DFE3, 0x002E3F56, 0x00C4DFE3, static_cast<int32_t>(0xe4e4e4FFu), 1.f, 10.f, 15, 8},  // 6
      {0x005E7992, 70.f, 400.f, 0x26, 0x02, 0x00DDD8AD, 0x004B5E6F, 0x00DDD8AD, static_cast<int32_t>(0xe4e4e4FFu), 1.f, 10.f, 15, 8},  // 7 day noon
      {0x00A1B5C3, 70.f, 400.f, 0x27, 0x0F, 0x00DEDABC, 0x00303C58, 0x00DEDABC, static_cast<int32_t>(0xe4e4e4FFu), 1.f, 10.f, 15, 8},  // 8
      {0x00869BAD, 70.f, 400.f, 0x28, 0x10, 0x00D8D4B0, 0x00375070, 0x00D8D4B0, static_cast<int32_t>(0xe4e4e4FFu), 1.f, 10.f, 15, 8},  // 9
      {0x00A4A1A0, 70.f, 400.f, 0x29, 0x19, 0x00A1A7B3, 0x0058606B, 0x00A1A7B3, static_cast<int32_t>(0xd4d4d4FFu), 1.f, 3.f, 15, 8},  // 10
      {0x003B4858, 60.f, 300.f, 0x2B, 0x13, 0x00BCBBB3, 0x001A1E2F, 0x00BCBBB3, 0, 0.f, 0.f, 0, 0},  // 11
      {0x00160702, 60.f, 300.f, 0x2A, 0x12, 0x00D0A26D, 0x000F080F, 0x00D0A26D, 0, 0.f, 0.f, 0, 0},  // 12
      {0x00000000, 60.f, 300.f, 0x2C, 0x11, 0x00938A7C, 0x00171D2A, 0x00938A7C, 0, 0.f, 0.f, 0, 0},  // 13
      {0x00030305, 60.f, 300.f, 0x2D, 0x14, 0x005F4045, 0x000D0C13, 0x005F4045, 0, 0.f, 0.f, 0, 0},  // 14
  };
  const SceneLook& look =
      kLooks[config >= 0 && config <= 14 ? config : 0];

  InvObject* map = tree_field_get_obj(city, "map");
  if (map)
    java_util_resource_GroundRef_setFog(map, look.fog_rgb, look.fog_near,
                                        look.fog_far);

  // Scene.addSceneElements: suntype.setLight(diff, amb, spec). Duplicate of
  // maps.skydome:0x0124r / mesh vtable not mirrored — global D3D bind.
  InvObject* suntype = tree_field_get_obj(city, "suntype");
  if (!suntype) {
    suntype = resref_new();
    tree_field_set_obj(city, "suntype", suntype);
  }
  // Scene.addSceneElements: suntype.duplicate(new RenderRef(maps.skydome:0x0124r)).
  InvObject* sun_src = tree_field_get_obj(city, "suntype_src");
  if (!sun_src) {
    sun_src = resref_new();
    tree_field_set_obj(city, "suntype_src", sun_src);
  }
  int32_t sun_id = 0x124;
  if (const RpakPack* sp = rpak_find_by_name("skydome.rpk"))
    sun_id = rpak_make_id(sp->pack_id, static_cast<uint16_t>(0x124));
  java_util_resource_ResourceRef_set(sun_src, sun_id);
  java_util_resource_ResourceRef_load(sun_src);
  java_util_resource_ResourceRef_duplicate(suntype, sun_src);
  java_util_resource_RenderRef_setLight(suntype, look.light_diff,
                                        look.light_amb, look.light_spec);

  // Scene.addSceneElements: suntype.setFlare only configs 6–10. New suntype
  // otherwise has no flare — clear so night does not keep noon sprites.
  if (look.flare_count > 0) {
    InvObject* glow = tree_field_get_obj(city, "flare_tex");
    if (!glow) {
      glow = resref_new();
      tree_field_set_obj(city, "flare_tex", glow);
    }
    int32_t glow_id = 0x100;
    if (const RpakPack* sp = rpak_find_by_name("skydome.rpk"))
      glow_id = rpak_make_id(sp->pack_id, static_cast<uint16_t>(0x100));
    java_util_resource_ResourceRef_set(glow, glow_id);
    java_util_resource_RenderRef_setFlare(suntype, glow, look.flare_color,
                                          look.flare_min, look.flare_max,
                                          look.flare_count, look.flare_rays);
  } else {
    render_d3d9_clear_flare(suntype);
    tree_field_set_int(suntype, "flare_count", 0);
    tree_field_set_int(suntype, "flare_rays", 0);
  }

  // Scene.addSceneElements: sun = new RenderRef(map, suntype, "sunny").
  if (map) {
    InvObject* sun = tree_field_get_obj(city, "sun");
    if (!sun) {
      sun = resref_new();
      tree_field_set_obj(city, "sun", sun);
    }
    java_util_resource_RenderRef_create(sun, map, suntype, string_new("sunny"));
  }

  int32_t env_id = look.env_local;
  int32_t sky_id = look.sky_local;
  if (const RpakPack* sp = rpak_find_by_name("skydome.rpk")) {
    env_id = rpak_make_id(sp->pack_id, static_cast<uint16_t>(look.env_local));
    sky_id = rpak_make_id(sp->pack_id, static_cast<uint16_t>(look.sky_local));
  }

  InvObject* env = tree_field_get_obj(city, "envmap");
  if (!env) {
    env = resref_new();
    tree_field_set_obj(city, "envmap", env);
  }
  java_util_resource_ResourceRef_set(env, env_id);
  java_util_resource_ResourceRef_load(env);
  java_render_GfxEngine_setGlobalEnvmap(env);

  InvObject* sky = tree_field_get_obj(city, "skydome");
  if (!sky) {
    sky = resref_new();
    tree_field_set_obj(city, "skydome", sky);
  }
  // Stock Scene.addSceneElements: skydome = new RenderRef(map, maps.skydome:IDr).
  // Pack entries are `mesh 0x3` + `texture 0xNN` recipes (skydome.rpk).
  java_util_resource_ResourceRef_set(sky, sky_id);
  java_util_resource_ResourceRef_load(sky);
  if (!render_d3d9_mesh_ready(sky)) {
    int32_t mesh_id = 0x3;
    if (const RpakPack* sp = rpak_find_by_name("skydome.rpk"))
      mesh_id = rpak_make_id(sp->pack_id, 0x3);
    java_util_resource_ResourceRef_set(sky, mesh_id);
    java_util_resource_ResourceRef_load(sky);
  }
  if (!render_d3d9_mesh_ready(sky)) {
    render_d3d9_mesh_create_from_file(sky, "maps/skydome/meshes/skydome.SCX");
  }
  if (!render_d3d9_mesh_ready(sky)) {
    render_d3d9_mesh_create_skydome(sky, 800.f);
  }
  // File PTX mirrors stock type textures (Scene names egbolt-am_01, …).
  static const char* kSkyPtx[] = {
      "maps/skydome/textures/stary_night_02.ptx",  // 0
      "maps/skydome/textures/dusk_03.ptx",         // 1
      "maps/skydome/textures/dusk_04.ptx",         // 2
      "maps/skydome/textures/dusk_01.ptx",         // 3
      "maps/skydome/textures/dusk_02.ptx",         // 4
      "maps/skydome/textures/dusk_05.ptx",         // 5
      "maps/skydome/textures/Morning_01.ptx",      // 6
      "maps/skydome/textures/am_01.ptx",           // 7
      "maps/skydome/textures/am_02.ptx",           // 8
      "maps/skydome/textures/am_03.ptx",           // 9
      "maps/skydome/textures/cloudy_day01.ptx",    // 10
      "maps/skydome/textures/dawn_02.ptx",         // 11
      "maps/skydome/textures/dawn_01.ptx",         // 12
      "maps/skydome/textures/dawn_03.ptx",         // 13
      "maps/skydome/textures/dawn_04.ptx",         // 14
  };
  InvObject* sky_tex = tree_field_get_obj(city, "sky_tex");
  if (!sky_tex) {
    sky_tex = resref_new();
    tree_field_set_obj(city, "sky_tex", sky_tex);
  }
  const char* ptx =
      kSkyPtx[config >= 0 && config <= 14 ? config : 0];
  // Always materialize sky_tex (smoke / UI). Bind to mesh when recipe left it bare.
  if (render_d3d9_texture_create_from_file(sky_tex, ptx)) {
    tree_field_set_obj(city, "sky_tex_path", string_new(ptx));
    if (render_d3d9_mesh_textured_count(sky) < 1 &&
        render_d3d9_texture_ready(sky_tex))
      render_d3d9_mesh_set_texture(sky, sky_tex);
  }
  if (render_d3d9_mesh_ready(sky)) {
    float px = 0.f, py = 0.f, pz = 0.f;
    if (InvObject* car = tree_field_get_obj(city, "car")) {
      px = tree_field_get_float(car, "pos_x");
      py = tree_field_get_float(car, "pos_y");
      pz = tree_field_get_float(car, "pos_z");
    }
    render_d3d9_mesh_set_transform(sky, px, py, pz, 0.f, 0.f, 0.f, 1.f, 1.f,
                                   1.f);
    render_d3d9_mesh_queue_add(sky);
  }
  tree_field_set_int(city, "fog_color", look.fog_rgb);
  tree_field_set_float(city, "fog_near", look.fog_near);
  tree_field_set_float(city, "fog_far", look.fog_far);
  tree_field_set_int(city, "light_diffuse", look.light_diff);
  tree_field_set_int(city, "light_ambient", look.light_amb);
  tree_field_set_int(city, "light_specular", look.light_spec);
  tree_field_set_int(city, "flare_count", look.flare_count);
  tree_field_set_int(city, "flare_rays", look.flare_rays);
  tree_field_set_float(city, "flare_max", look.flare_max);
}

void valocity_prepare(InvObject* city) {
  if (!city) return;
  InvObject* player = tree_field_get_obj(city, "player");
  if (!player) {
    player = game_logic_player();
    tree_field_set_obj(city, "player", player);
  }
  InvObject* car = tree_field_get_obj(city, "car");
  if (!car && player) car = tree_field_get_obj(player, "car");
  if (car) tree_field_set_obj(city, "car", car);

  // Valocity ctor: map + nav + club garage poses (flat fields; no Object[] yet).
  if (!rpak_find_by_name("city.rpk"))
    java_lang_System_openLib(string_new("maps/city.rpk"));
  if (!rpak_find_by_name("traffic.rpk"))
    java_lang_System_openLib(string_new("cars/traffic.rpk"));
  int32_t map_id = 0x1;
  if (const RpakPack* city_pack = rpak_find_by_name("city.rpk"))
    map_id = rpak_make_id(city_pack->pack_id, 0x1);
  tree_field_set_int(city, "map_id", map_id);

  InvObject* map = tree_field_get_obj(city, "map");
  if (!map) {
    map = tree_host_new("java.util.resource.GroundRef");
    java_util_resource_ResourceRef_set(map, map_id);
    tree_field_set_obj(city, "map", map);
  }

  if (!rpak_find_by_name("smallmap.rpk"))
    java_lang_System_openLib(string_new("maps/city/smallmap.rpk"));
  if (!rpak_find_by_name("frontend.rpk"))
    java_lang_System_openLib(string_new("frontend/frontend.rpk"));
  if (!tree_field_get_obj(city, "nav")) {
    const int32_t rid_type = pack_local_id("smallmap.rpk", 0x1);
    const int32_t rid_msh = pack_local_id("smallmap.rpk", 0x2);
    const int32_t rid_tex = pack_local_id("smallmap.rpk", 0x5);
    InvObject* nav =
        navigator_new(-23.482f, -24.45f, 5.828f, rid_type, rid_msh, rid_tex, 8,
                      8, 8);
    tree_field_set_obj(city, "nav", nav);
    navigator_show(nav);
    navigator_add_marker_static(nav, pack_local_id("frontend.rpk", 0x5B),
                                -278.518f, 1033.002f, 0);
    navigator_add_marker_static(nav, pack_local_id("frontend.rpk", 0x5F),
                                355.381f, 418.244f, 0);
    navigator_add_marker_static(nav, pack_local_id("frontend.rpk", 0x73),
                                -531.138f, -149.357f, 0);
    if (car) {
      InvObject* mPlayer = navigator_add_marker_dynamic(
          nav, pack_local_id("frontend.rpk", 0x5C), car);
      tree_field_set_obj(city, "mPlayer", mPlayer);
    }
  }

  // posGarage / oriGarage as real arrays for TREE AALOAD.
  InvObject* posGarage = tree_field_get_obj(city, "posGarage");
  if (!posGarage) {
    posGarage = tree_vector_new();
    auto mk_v3 = [](float x, float y, float z) {
      InvObject* v = tree_host_new("java.lang.Vector3");
      tree_field_set_float(v, "x", x);
      tree_field_set_float(v, "y", y);
      tree_field_set_float(v, "z", z);
      return v;
    };
    tree_vector_add(posGarage, mk_v3(-278.518f, 9.8f, 1033.002f));
    tree_vector_add(posGarage, mk_v3(355.381f, 1.6f, 418.244f));
    tree_vector_add(posGarage, mk_v3(-531.138f, 5.05f, -149.357f));
    tree_field_set_obj(city, "posGarage", posGarage);
  }
  InvObject* oriGarage = tree_field_get_obj(city, "oriGarage");
  if (!oriGarage) {
    oriGarage = tree_vector_new();
    auto mk_ypr = [](float y) {
      InvObject* v = tree_host_new("java.lang.Ypr");
      tree_field_set_float(v, "y", y);
      tree_field_set_float(v, "p", 0.f);
      tree_field_set_float(v, "r", 0.f);
      return v;
    };
    tree_vector_add(oriGarage, mk_ypr(1.580f));
    tree_vector_add(oriGarage, mk_ypr(-1.763f));
    tree_vector_add(oriGarage, mk_ypr(2.077f));
    tree_field_set_obj(city, "oriGarage", oriGarage);
  }

  // Flat mirrors (host finalize / smoke).
  tree_field_set_float(city, "posGarage0_x", -278.518f);
  tree_field_set_float(city, "posGarage0_y", 9.8f);
  tree_field_set_float(city, "posGarage0_z", 1033.002f);
  tree_field_set_float(city, "oriGarage0_y", 1.580f);
  tree_field_set_float(city, "posGarage1_x", 355.381f);
  tree_field_set_float(city, "posGarage1_y", 1.6f);
  tree_field_set_float(city, "posGarage1_z", 418.244f);
  tree_field_set_float(city, "oriGarage1_y", -1.763f);
  tree_field_set_float(city, "posGarage2_x", -531.138f);
  tree_field_set_float(city, "posGarage2_y", 5.05f);
  tree_field_set_float(city, "posGarage2_z", -149.357f);
  tree_field_set_float(city, "oriGarage2_y", 2.077f);

  if (!tree_field_get_obj(city, "speedymen"))
    tree_field_set_obj(city, "speedymen", tree_vector_new());

  // Phase 2.24: host road spine for alignToRoad / race cross queries.
  physics_road_seed_valocity();
  // Phase 2.41: visual SCX from city.rpk (egyedi remap, world-space verts).
  if (city_mesh_count() == 0) city_mesh_seed_from_rpak("city.rpk", 142);
}

void valocity_finalize_enter(InvObject* city) {
  if (!city) return;
  valocity_prepare(city);
  InvObject* player = tree_field_get_obj(city, "player");
  InvObject* car = tree_field_get_obj(city, "car");
  InvObject* map = tree_field_get_obj(city, "map");
  const int32_t club = player ? tree_field_get_int(player, "club") : 0;

  // Sync flat posStart_* from Vector3 if TREE assigned posGarage[club].
  if (InvObject* ps = tree_field_get_obj(city, "posStart")) {
    const float zx = tree_field_get_float(ps, "z");
    if (zx != 0.f || tree_field_get_float(ps, "x") != 0.f) {
      tree_field_set_float(city, "posStart_x", tree_field_get_float(ps, "x"));
      tree_field_set_float(city, "posStart_y", tree_field_get_float(ps, "y"));
      tree_field_set_float(city, "posStart_z", zx);
    }
  }
  if (InvObject* os = tree_field_get_obj(city, "oriStart")) {
    tree_field_set_float(city, "oriStart_y", tree_field_get_float(os, "y"));
  }

  float px = tree_field_get_float(city, "posGarage0_x");
  float py = tree_field_get_float(city, "posGarage0_y");
  float pz = tree_field_get_float(city, "posGarage0_z");
  float oy = tree_field_get_float(city, "oriGarage0_y");
  if (club == 1) {
    px = tree_field_get_float(city, "posGarage1_x");
    py = tree_field_get_float(city, "posGarage1_y");
    pz = tree_field_get_float(city, "posGarage1_z");
    oy = tree_field_get_float(city, "oriGarage1_y");
  } else if (club == 2) {
    px = tree_field_get_float(city, "posGarage2_x");
    py = tree_field_get_float(city, "posGarage2_y");
    pz = tree_field_get_float(city, "posGarage2_z");
    oy = tree_field_get_float(city, "oriGarage2_y");
  }
  InvObject* prev = tree_field_get_obj(city, "parentState");
  const char* prev_cn = prev ? tree_host_class(prev) : nullptr;
  const bool from_garage =
      prev_cn && (std::strstr(prev_cn, "Garage") || std::strstr(prev_cn, "MainMenu"));
  if (from_garage || tree_field_get_float(city, "posStart_z") == 0.f) {
    tree_field_set_float(city, "posStart_x", px);
    tree_field_set_float(city, "posStart_y", py);
    tree_field_set_float(city, "posStart_z", pz);
    tree_field_set_float(city, "oriStart_y", oy);
  }
  if (car) {
    tree_field_set_float(car, "pos_x", tree_field_get_float(city, "posStart_x"));
    tree_field_set_float(car, "pos_y", tree_field_get_float(city, "posStart_y"));
    tree_field_set_float(car, "pos_z", tree_field_get_float(city, "posStart_z"));
    tree_field_set_float(car, "ori_y", tree_field_get_float(city, "oriStart_y"));
    tree_field_set_int(car, "parent_map_id", tree_field_get_int(city, "map_id"));
    tree_field_set_int(car, "stopped", 0);
    // Phase 2.34: bind arcade PhysicsRef so valocity_simulate can drive.
    valocity_ensure_car_physics(car);
  }
  const float hour = game_logic_time() / 3600.f;
  const int daytime = (hour > 4.f && hour < 22.f) ? 1 : 0;
  tree_field_set_int(city, "daytime", daytime);
  tree_field_set_int(city, "traffic_day", daytime);
  // Phase 2.38: Scene fog / skydome / envmap from time-of-day.
  valocity_apply_scene(city);
  // Host authoritative traffic/peds (TREE may take wrong day/night branch on
  // float compares; keep script side-effects but normalize counts for gameplay).
  if (map) {
    java_util_resource_GroundRef_delTraffic(map);
    // ped types cleared with streams; re-seed below via spawn.
    tree_field_set_int(map, "pedestrian_types", 0);
    valocity_spawn_traffic(city, map);
  }
  if (InvObject* nav = tree_field_get_obj(city, "nav")) {
    if (car) java_game_Navigator_updateNavigator(nav, car, 0);
  }
  tree_field_set_int(city, "activeTrigger", 0);
  tree_field_set_int(city, "policeState", 0);
  tree_field_set_int(city, "raceState", 0);
  tree_field_set_int(city, "garage_denied", 0);
  {
    const int32_t mode = game_logic_game_mode();
    constexpr int32_t kCareer = 1;
    constexpr int32_t kGmSingleCar = 4;
    if (mode == kCareer || mode == kGmSingleCar) {
      tree_field_set_float(city, "trig0_x", -278.518f);
      tree_field_set_float(city, "trig0_y", 9.8f);
      tree_field_set_float(city, "trig0_z", 1033.002f);
      tree_field_set_float(city, "trig1_x", 355.381f);
      tree_field_set_float(city, "trig1_y", 1.6f);
      tree_field_set_float(city, "trig1_z", 418.244f);
      tree_field_set_float(city, "trig2_x", -531.138f);
      tree_field_set_float(city, "trig2_y", 5.05f);
      tree_field_set_float(city, "trig2_z", -149.357f);
      tree_field_set_float(city, "trig_r", 6.f);
      tree_field_set_int(city, "trigger_count", 3);
    } else if (tree_field_get_int(city, "trigger_count") == 0) {
      tree_field_set_int(city, "trigger_count", 0);
    }
  }
  tree_field_set_int(city, "entered", 1);
  frontend_loading_screen_hide();
}

InvObject* valocity_enter(InvObject* city, InvObject* prev_state) {
  if (!city) return nullptr;
  frontend_loading_screen_show();
  if (prev_state && !tree_field_get_obj(city, "parentState"))
    tree_field_set_obj(city, "parentState", prev_state);
  valocity_finalize_enter(city);
  return city;
}

void valocity_exit(InvObject* city, InvObject* next_state) {
  (void)next_state;
  if (!city) return;
  if (InvObject* map = tree_field_get_obj(city, "map"))
    java_util_resource_GroundRef_delTraffic(map);
  tree_field_set_int(city, "traffic_count", 0);
  tree_field_set_int(city, "entered", 0);
  if (InvObject* nav = tree_field_get_obj(city, "nav")) {
    navigator_hide(nav);
  }
  if (InvObject* sky = tree_field_get_obj(city, "skydome"))
    render_d3d9_mesh_destroy(sky);
  city_mesh_clear();
  render_d3d9_clear_fog();
  render_d3d9_set_global_envmap(nullptr);
}

int32_t valocity_fire_garage_trigger(InvObject* city, int32_t clubGarage,
                                    int32_t event_on) {
  if (!city || clubGarage < 1 || clubGarage > 3) return 0;
  if (tree_field_get_int(city, "trigger_count") <= 0) return 0;
  InvObject* player = tree_field_get_obj(city, "player");
  InvObject* car = tree_field_get_obj(city, "car");
  if (!car && player) car = tree_field_get_obj(player, "car");
  if (!car) return 0;
  // handleGarageTrigger: param token0 == player.car.id() — host always matches.
  if (event_on) {
    if (tree_field_get_int(city, "activeTrigger") == 0)
      tree_field_set_int(city, "activeTrigger", clubGarage);
  } else {
    tree_field_set_int(city, "activeTrigger", 0);
  }
  return tree_field_get_int(city, "activeTrigger");
}

InvObject* valocity_tick(InvObject* city) {
  // Valocity.handleEvent EVENT_TIME param==2 (1s tick).
  if (!city) return nullptr;
  const int32_t at = tree_field_get_int(city, "activeTrigger");
  if (at <= 0) return nullptr;
  if (tree_field_get_int(city, "policeState") ||
      tree_field_get_int(city, "raceState"))
    return nullptr;

  InvObject* player = tree_field_get_obj(city, "player");
  InvObject* car = tree_field_get_obj(city, "car");
  if (!car && player) car = tree_field_get_obj(player, "car");
  if (!car) return nullptr;

  // Prefer live physics speed; fall back to TREE cache.
  float speed_sq = java_game_Vehicle_getSpeedSquare(car);
  if (speed_sq < 0.f) speed_sq = 0.f;
  tree_field_set_float(car, "speed_sq", speed_sq);
  if (speed_sq >= 0.25f) return nullptr;

  const int32_t club = player ? tree_field_get_int(player, "club") : 0;
  if (at - 1 > club) {
    tree_field_set_int(city, "activeTrigger", 0);
    tree_field_set_int(city, "garage_denied", 1);
    tree_field_set_obj(city, "last_warning",
                       string_new("This is a higher club's garage, you can't enter!"));
    return nullptr;
  }

  tree_field_set_int(city, "activeTrigger", 0);
  // YesNoDialog accept (0) → changeActiveSection(garage)
  return valocity_return_to_garage(city);
}

namespace {

// Map "Baiern DevilSport" / "SuperDuty …" → cars/racers/<Brand>_data/
std::string racer_brand_from_name(const char* vehicle_name) {
  static const char* kFolders[] = {
      "SuperDuty", "Einvagen", "Baiern", "Duhen", "Enula", "Focer",
      "Nonus",     "Prime",    "MC",
  };
  if (!vehicle_name || !vehicle_name[0]) return "Baiern";
  size_t best_n = 0;
  const char* best = nullptr;
  for (const char* f : kFolders) {
    size_t n = std::strlen(f);
    if (n <= best_n) continue;
    bool match = true;
    for (size_t i = 0; i < n; ++i) {
      char a = vehicle_name[i];
      char b = f[i];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
      if (a != b) {
        match = false;
        break;
      }
    }
    if (!match) continue;
    const char c = vehicle_name[n];
    if (c != '\0' && c != ' ' && c != '_' && c != '-') continue;
    best_n = n;
    best = f;
  }
  if (best) return best;
  std::string tok;
  for (const char* p = vehicle_name; *p && *p != ' ' && *p != '_'; ++p)
    tok.push_back(*p);
  return tok.empty() ? "Baiern" : tok;
}

// Phase 2.46 — stock .cfg slots are metres; SCX mesh space is ×100.
constexpr float kCfgMetresToMesh = 100.f;

struct CfgAttach {
  int32_t type_id = 0;
  int32_t mate_slot = 1;
};

struct CfgSlot {
  int32_t id = 0;
  float px = 0, py = 0, pz = 0;
  float oy = 0, op = 0, or_ = 0;
  std::string name;  // normalized: fl_door, l_headlights, …
  std::vector<CfgAttach> attaches;
};

std::string norm_slot_key(const char* raw) {
  std::string out;
  if (!raw) return out;
  // Skip leading whitespace / punctuation from "; FL_door" comments.
  while (*raw == ' ' || *raw == '\t' || *raw == ';' || *raw == '_') ++raw;
  for (const char* p = raw; *p; ++p) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c == ' ' || c == '-') c = '_';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
      out.push_back(c);
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  // Collapse leading underscores left by mixed separators.
  size_t start = 0;
  while (start < out.size() && out[start] == '_') ++start;
  if (start) out.erase(0, start);
  return out;
}

bool parse_cfg_slots_file(const char* path, std::vector<CfgSlot>* out) {
  if (!path || !out) return false;
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::string resolved = rpak_resolve_path(path);
    if (!resolved.empty()) f = std::fopen(resolved.c_str(), "rb");
  }
  if (!f) return false;
  char line[512];
  int32_t n = 0;
  while (std::fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    // Phase 2.142: attach lines bind to the preceding slot.
    if (std::strncmp(p, "attach", 6) == 0) {
      if (out->empty()) continue;
      p += 6;
      while (*p == ' ' || *p == '\t') ++p;
      unsigned int type = 0;
      int mate = 1;
      if (std::sscanf(p, "%i %d", &type, &mate) < 1 &&
          std::sscanf(p, "%x %d", &type, &mate) < 1)
        continue;
      if (mate <= 0) mate = 1;
      CfgAttach a;
      a.type_id = static_cast<int32_t>(type);
      a.mate_slot = mate;
      out->back().attaches.push_back(a);
      continue;
    }
    if (std::strncmp(p, "slot", 4) != 0) continue;
    p += 4;
    while (*p == ' ' || *p == '\t') ++p;
    float px = 0, py = 0, pz = 0, oy = 0, op = 0, or_ = 0;
    int id = 0;
    if (std::sscanf(p, "%f %f %f %f %f %f %d", &px, &py, &pz, &oy, &op, &or_,
                    &id) < 7)
      continue;
    const char* semi = std::strchr(p, ';');
    CfgSlot s;
    s.id = id;
    s.px = px;
    s.py = py;
    s.pz = pz;
    s.oy = oy;
    s.op = op;
    s.or_ = or_;
    if (semi && semi[1]) s.name = norm_slot_key(semi + 1);
    out->push_back(s);
    ++n;
  }
  std::fclose(f);
  return n > 0;
}

const CfgSlot* find_cfg_slot(const std::vector<CfgSlot>& slots,
                             const char* key) {
  const std::string want = norm_slot_key(key);
  if (want.empty()) return nullptr;
  for (const CfgSlot& s : slots) {
    if (s.name == want) return &s;
  }
  return nullptr;
}

const CfgSlot* find_cfg_slot_id(const std::vector<CfgSlot>& slots, int32_t id) {
  for (const CfgSlot& s : slots) {
    if (s.id == id) return &s;
  }
  return nullptr;
}

// Stock install: Local = ParentSlot * inv(ChildMateSlot). Identity rot → subtract.
void mate_slot_local(const CfgSlot* parent_slot, const CfgSlot* child_mate,
                     float* lx, float* ly, float* lz, float* oy, float* op,
                     float* or_) {
  if (!lx || !ly || !lz || !oy || !op || !or_ || !parent_slot) return;
  float cpx = 0, cpy = 0, cpz = 0, coy = 0, cop = 0, cor = 0;
  if (child_mate) {
    cpx = child_mate->px;
    cpy = child_mate->py;
    cpz = child_mate->pz;
    coy = child_mate->oy;
    cop = child_mate->op;
    cor = child_mate->or_;
  }
  *lx = (parent_slot->px - cpx) * kCfgMetresToMesh;
  *ly = (parent_slot->py - cpy) * kCfgMetresToMesh;
  *lz = (parent_slot->pz - cpz) * kCfgMetresToMesh;
  *oy = parent_slot->oy - coy;
  *op = parent_slot->op - cop;
  *or_ = parent_slot->or_ - cor;
}

struct CfgWheel {
  float px = 0, py = 0, pz = 0;
  float oy = 0, op = 0, or_ = 0;
  std::string name;  // fl_wheel, …
};

// Phase 2.48 — `wheel px py pz …` then hub `slot … ; FL wheel` (yaw on sides).
bool parse_cfg_wheels_file(const char* path, std::vector<CfgWheel>* out) {
  if (!path || !out) return false;
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    std::string resolved = rpak_resolve_path(path);
    if (!resolved.empty()) f = std::fopen(resolved.c_str(), "rb");
  }
  if (!f) return false;
  char line[512];
  CfgWheel pending;
  bool have_wheel = false;
  int32_t n = 0;
  while (std::fgets(line, sizeof(line), f)) {
    char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (std::strncmp(p, "wheel", 5) == 0 &&
        (p[5] == ' ' || p[5] == '\t')) {
      p += 5;
      while (*p == ' ' || *p == '\t') ++p;
      float px = 0, py = 0, pz = 0, oy = 0, op = 0, or_ = 0;
      if (std::sscanf(p, "%f %f %f %f %f %f", &px, &py, &pz, &oy, &op, &or_) <
          3)
        continue;
      pending = CfgWheel{};
      pending.px = px;
      pending.py = py;
      pending.pz = pz;
      pending.oy = oy;
      pending.op = op;
      pending.or_ = or_;
      have_wheel = true;
      continue;
    }
    if (!have_wheel) continue;
    if (std::strncmp(p, "slot", 4) != 0) continue;
    if (std::strncmp(p, "slottype", 8) == 0 ||
        std::strncmp(p, "slotdmg", 7) == 0 ||
        std::strncmp(p, "slotdeform", 10) == 0)
      continue;
    p += 4;
    while (*p == ' ' || *p == '\t') ++p;
    float px = 0, py = 0, pz = 0, oy = 0, op = 0, or_ = 0;
    int id = 0;
    if (std::sscanf(p, "%f %f %f %f %f %f %d", &px, &py, &pz, &oy, &op, &or_,
                    &id) < 7)
      continue;
    const char* semi = std::strchr(p, ';');
    std::string name = semi && semi[1] ? norm_slot_key(semi + 1) : "";
    if (name.find("wheel") == std::string::npos ||
        name.find("steering") != std::string::npos) {
      continue;
    }
    // Hub slot is local to wheelbones; take yaw from it, pos from `wheel`.
    pending.oy = oy;
    pending.op = op;
    pending.or_ = or_;
    pending.name = name;
    out->push_back(pending);
    have_wheel = false;
    ++n;
    if (n >= 4) break;
  }
  std::fclose(f);
  return n > 0;
}

std::string main_cfg_path_for_car(const std::string& brand,
                                  const char* vehicle_name) {
  const std::string dir = "cars/racers/" + brand + "_data/scripts/";
  std::string model;
  if (vehicle_name && vehicle_name[0]) {
    // "Baiern DevilSport" / "Baiern CoupeSport GT III" → after brand token.
    const char* p = vehicle_name;
    while (*p && *p != ' ') ++p;
    while (*p == ' ') ++p;
    for (; *p; ++p) {
      char c = *p;
      if (c == ' ')
        model.push_back('_');
      else
        model.push_back(c);
    }
  }
  if (model.empty()) model = "DevilSport";
  return dir + "_main_" + model + ".cfg";
}

}  // namespace

void valocity_ensure_car_mesh(InvObject* car) {
  if (!car) return;
  InvObject* mesh = tree_field_get_obj(car, "visual_mesh");
  if (mesh && render_d3d9_mesh_ready(mesh)) {
    valocity_ensure_car_parts(car);
    return;
  }
  if (!mesh) {
    mesh = resref_new();
    tree_field_set_obj(car, "visual_mesh", mesh);
  }
  const char* vname = nullptr;
  if (InvObject* nm = tree_field_get_obj(car, "vehicleName"))
    vname = string_cstr(nm);
  const std::string brand = racer_brand_from_name(vname);
  const std::string path =
      "cars/racers/" + brand + "_data/meshes/chassis.scx";
  if (!render_d3d9_mesh_create_from_file(mesh, path.c_str())) {
    // Starter fallback when brand folder is missing / demo descriptor.
    if (brand != "Baiern")
      render_d3d9_mesh_create_from_file(
          mesh, "cars/racers/Baiern_data/meshes/chassis.scx");
  }
  if (!render_d3d9_mesh_ready(mesh)) return;
  render_d3d9_mesh_queue_add(mesh);
  valocity_sync_car_mesh(car);
  valocity_ensure_car_parts(car);
}

void valocity_ensure_car_parts(InvObject* car) {
  if (!car) return;
  if (tree_field_get_int(car, "visual_parts_ready")) return;
  InvObject* chassis = tree_field_get_obj(car, "visual_mesh");
  if (!chassis || !render_d3d9_mesh_ready(chassis)) return;

  const char* vname = nullptr;
  if (InvObject* nm = tree_field_get_obj(car, "vehicleName"))
    vname = string_cstr(nm);
  std::string brand = racer_brand_from_name(vname);

  // Chassis-attached body SCX (poses from _main_*.cfg slots, metres→mesh).
  static const char* kChassisParts[] = {
      "hood.scx",
      "trunk.scx",
      "F_bumper.scx",
      "R_bumper.scx",
      "F_windshield.scx",
      "R_windshield.scx",
      "FL_quarterpanel.scx",
      "FR_quarterpanel.scx",
      "RL_quarterpanel.scx",
      "RR_quarterpanel.scx",
      "L_sideskirt.scx",
      "R_sideskirt.scx",
      "FL_door.scx",
      "FR_door.scx",
      "L_headlights.scx",
      "R_headlights.scx",
      "L_taillights.scx",
      "R_taillights.scx",
  };
  // Door children: parent mesh stem → child scx (slot name = child stem).
  static const char* kDoorChildFiles[] = {
      "L_mirror.scx",
      "FL_window.scx",
      "R_mirror.scx",
      "FR_window.scx",
  };
  static const char* kDoorChildParents[] = {
      "FL_door",
      "FL_door",
      "FR_door",
      "FR_door",
  };

  InvObject* parts = tree_field_get_obj(car, "visual_parts");
  if (!parts) {
    parts = tree_vector_new();
    tree_field_set_obj(car, "visual_parts", parts);
  }

  int32_t loaded = 0;
  int32_t slotted = 0;
  int32_t wheels_n = 0;
  int32_t tyres_n = 0;
  std::unordered_map<std::string, InvObject*> by_stem;

  auto try_load = [&](const std::string& b) {
    const std::string mesh_prefix = "cars/racers/" + b + "_data/meshes/";
    const std::string cfg_prefix = "cars/racers/" + b + "_data/scripts/";
    std::vector<CfgSlot> chassis_slots;
    std::string main_cfg = main_cfg_path_for_car(b, vname);
    if (!parse_cfg_slots_file(main_cfg.c_str(), &chassis_slots) &&
        b == "Baiern") {
      main_cfg = "cars/racers/Baiern_data/scripts/_main_DevilSport.cfg";
      parse_cfg_slots_file(main_cfg.c_str(), &chassis_slots);
    }

    auto add_mesh_at = [&](const char* path, const char* stem, InvObject* parent,
                           float lx, float ly, float lz, float oy, float op,
                           float or_, bool has_slot) -> InvObject* {
      InvObject* part = resref_new();
      if (!render_d3d9_mesh_create_from_file(part, path) ||
          !render_d3d9_mesh_ready(part)) {
        render_d3d9_mesh_destroy(part);
        return nullptr;
      }
      render_d3d9_mesh_set_transform(part, lx, ly, lz, oy, op, or_, 1.f, 1.f,
                                     1.f);
      render_d3d9_mesh_set_parent(part, parent);
      render_d3d9_mesh_queue_add(part);
      tree_vector_add(parts, part);
      tree_field_set_obj(part, "part_stem", string_new(stem));
      tree_field_set_float(part, "attach_x", lx);
      tree_field_set_float(part, "attach_y", ly);
      tree_field_set_float(part, "attach_z", lz);
      tree_field_set_float(part, "attach_oy", oy);
      tree_field_set_int(part, "attach_slotted", has_slot ? 1 : 0);
      by_stem[norm_slot_key(stem)] = part;
      ++loaded;
      if (has_slot) ++slotted;
      return part;
    };

    auto add_mesh = [&](const char* file, InvObject* parent, float lx, float ly,
                        float lz, float oy, float op, float or_,
                        bool has_slot) -> InvObject* {
      std::string stem = file;
      if (stem.size() > 4) stem.resize(stem.size() - 4);
      const std::string path = mesh_prefix + file;
      return add_mesh_at(path.c_str(), stem.c_str(), parent, lx, ly, lz, oy, op,
                         or_, has_slot);
    };

    for (const char* file : kChassisParts) {
      std::string stem = file;
      if (stem.size() > 4) stem.resize(stem.size() - 4);
      float lx = 0, ly = 0, lz = 0, oy = 0, op = 0, or_ = 0;
      bool has = false;
      if (const CfgSlot* parent_slot =
              find_cfg_slot(chassis_slots, stem.c_str())) {
        std::vector<CfgSlot> part_slots;
        parse_cfg_slots_file((cfg_prefix + stem + ".cfg").c_str(), &part_slots);
        const CfgSlot* mate = find_cfg_slot_id(part_slots, parent_slot->id);
        if (!mate) mate = find_cfg_slot(part_slots, stem.c_str());
        mate_slot_local(parent_slot, mate, &lx, &ly, &lz, &oy, &op, &or_);
        has = true;
      }
      add_mesh(file, chassis, lx, ly, lz, oy, op, or_, has);
    }

    // Mirrors / windows: slots live on the door .cfg (parent = door mesh).
    for (size_t i = 0; i < sizeof(kDoorChildFiles) / sizeof(kDoorChildFiles[0]);
         ++i) {
      const char* file = kDoorChildFiles[i];
      const char* parent_stem = kDoorChildParents[i];
      auto pit = by_stem.find(norm_slot_key(parent_stem));
      if (pit == by_stem.end()) continue;
      std::string stem = file;
      if (stem.size() > 4) stem.resize(stem.size() - 4);
      std::vector<CfgSlot> door_slots;
      const std::string door_cfg = cfg_prefix + parent_stem + ".cfg";
      parse_cfg_slots_file(door_cfg.c_str(), &door_slots);
      float lx = 0, ly = 0, lz = 0, oy = 0, op = 0, or_ = 0;
      bool has = false;
      if (const CfgSlot* parent_slot = find_cfg_slot(door_slots, stem.c_str())) {
        std::vector<CfgSlot> child_slots;
        parse_cfg_slots_file((cfg_prefix + stem + ".cfg").c_str(), &child_slots);
        const CfgSlot* mate = find_cfg_slot_id(child_slots, parent_slot->id);
        if (!mate) mate = find_cfg_slot(child_slots, stem.c_str());
        mate_slot_local(parent_slot, mate, &lx, &ly, &lz, &oy, &op, &or_);
        has = true;
      }
      add_mesh(file, pit->second, lx, ly, lz, oy, op, or_, has);
    }

    // Phase 2.48 — DevilSport stock Blossom rims at `wheel` poses.
    std::vector<CfgWheel> wheels;
    if (!parse_cfg_wheels_file(main_cfg.c_str(), &wheels) && b == "Baiern") {
      parse_cfg_wheels_file(
          "cars/racers/Baiern_data/scripts/_main_DevilSport.cfg", &wheels);
    }
    static const char* kFrontRim =
        "parts/wheels/rims/meshes/"
        "rim_Blossom_9.0_19_ET_0_LOD_CATALOG_GARAGE.scx";
    static const char* kRearRim =
        "parts/wheels/rims/meshes/"
        "rim_Blossom_10.5_19_ET_0_LOD_CATALOG_GARAGE.scx";
    static const char* kFrontRimCfg =
        "parts/wheels/rims/scripts/"
        "rim_Blossom_9_0_19_ET_0_LOD_CATALOG_GARAGE.cfg";
    static const char* kRearRimCfg =
        "parts/wheels/rims/scripts/"
        "rim_Blossom_10_5_19_ET_0_LOD_CATALOG_GARAGE.cfg";
    // Phase 2.49 — matching 19" tyres parented to each rim (slot 2↔1).
    static const char* kFrontTyre =
        "parts/wheels/tyres/meshes/"
        "tyre_235_45_19_9.0_LOD_CATALOG_GARAGE.scx";
    static const char* kRearTyre =
        "parts/wheels/tyres/meshes/"
        "tyre_255_45_19_10.5_LOD_CATALOG_GARAGE.scx";
    static const char* kFrontTyreCfg =
        "parts/wheels/tyres/scripts/"
        "tyre_235_45_19_9_0_LOD_CATALOG_GARAGE.cfg";
    static const char* kRearTyreCfg =
        "parts/wheels/tyres/scripts/"
        "tyre_255_45_19_10_0_LOD_CATALOG_GARAGE.cfg";
    std::vector<CfgSlot> front_rim_slots;
    std::vector<CfgSlot> rear_rim_slots;
    std::vector<CfgSlot> front_tyre_slots;
    std::vector<CfgSlot> rear_tyre_slots;
    parse_cfg_slots_file(kFrontRimCfg, &front_rim_slots);
    parse_cfg_slots_file(kRearRimCfg, &rear_rim_slots);
    parse_cfg_slots_file(kFrontTyreCfg, &front_tyre_slots);
    parse_cfg_slots_file(kRearTyreCfg, &rear_tyre_slots);
    for (size_t wi = 0; wi < wheels.size() && wi < 4; ++wi) {
      const CfgWheel& w = wheels[wi];
      const bool front = wi < 2;
      const char* rim = front ? kFrontRim : kRearRim;
      char stem[32];
      std::snprintf(stem, sizeof(stem), "wheel_%zu", wi);
      InvObject* rim_mesh =
          add_mesh_at(rim, stem, chassis, w.px * kCfgMetresToMesh,
                      w.py * kCfgMetresToMesh, w.pz * kCfgMetresToMesh, w.oy,
                      w.op, w.or_, true);
      if (!rim_mesh) continue;
      ++wheels_n;
      // Chassis wheel slots 101–104 + install graph (rim mate slot 1).
      part_bind_slot_visual(car, 101 + static_cast<int32_t>(wi), rim_mesh, w.px,
                            w.py, w.pz, w.oy, w.op, w.or_);
      part_install(car, 101 + static_cast<int32_t>(wi), rim_mesh, 1);

      const std::vector<CfgSlot>& rim_slots =
          front ? front_rim_slots : rear_rim_slots;
      const std::vector<CfgSlot>& tyre_slots =
          front ? front_tyre_slots : rear_tyre_slots;
      const CfgSlot* rim_tyre = find_cfg_slot_id(rim_slots, 2);
      const CfgSlot* tyre_mate = find_cfg_slot_id(tyre_slots, 1);
      float tlx = 0, tly = 0, tlz = 0, toy = 0, top = 0, tor = 0;
      bool thas = false;
      if (rim_tyre) {
        mate_slot_local(rim_tyre, tyre_mate, &tlx, &tly, &tlz, &toy, &top,
                        &tor);
        thas = true;
      } else {
        // Fallback: rim tyre-slot at origin, stock tyre mate oy=π.
        CfgSlot fake_rim{};
        fake_rim.id = 2;
        CfgSlot fake_tyre{};
        fake_tyre.id = 1;
        fake_tyre.oy = 3.14159265f;
        mate_slot_local(&fake_rim, tyre_mate ? tyre_mate : &fake_tyre, &tlx,
                        &tly, &tlz, &toy, &top, &tor);
        thas = true;
      }
      const char* tyre = front ? kFrontTyre : kRearTyre;
      char tstem[32];
      std::snprintf(tstem, sizeof(tstem), "tyre_%zu", wi);
      InvObject* tyre_mesh =
          add_mesh_at(tyre, tstem, rim_mesh, tlx, tly, tlz, toy, top, tor, thas);
      if (tyre_mesh) {
        ++tyres_n;
        // Rim slot 2 ↔ tyre mate slot 1.
        part_bind_slot_visual(rim_mesh, 2, tyre_mesh, tlx / kCfgMetresToMesh,
                              tly / kCfgMetresToMesh, tlz / kCfgMetresToMesh,
                              toy, top, tor);
        part_install(rim_mesh, 2, tyre_mesh, 1);
      }
    }
  };

  try_load(brand);
  if (loaded == 0 && brand != "Baiern") {
    brand = "Baiern";
    try_load(brand);
  }

  tree_field_set_int(car, "visual_parts_ready", loaded > 0 ? 1 : 0);
  tree_field_set_int(car, "visual_parts_count", loaded);
  tree_field_set_int(car, "visual_parts_slotted", slotted);
  tree_field_set_int(car, "visual_wheels_count", wheels_n);
  tree_field_set_int(car, "visual_tyres_count", tyres_n);
}

int32_t valocity_car_part_count(InvObject* car) {
  if (!car) return 0;
  return tree_field_get_int(car, "visual_parts_count");
}

int32_t valocity_car_part_slotted(InvObject* car) {
  if (!car) return 0;
  return tree_field_get_int(car, "visual_parts_slotted");
}

int32_t valocity_car_wheel_count(InvObject* car) {
  if (!car) return 0;
  return tree_field_get_int(car, "visual_wheels_count");
}

int32_t valocity_car_tyre_count(InvObject* car) {
  if (!car) return 0;
  return tree_field_get_int(car, "visual_tyres_count");
}

namespace {

constexpr float kPartSlotMetresToMesh = 100.f;

InvObject* part_slot_table(InvObject* part) {
  if (!part) return nullptr;
  InvObject* slots = tree_field_get_obj(part, "part_slots");
  if (!slots) {
    slots = tree_vector_new();
    tree_field_set_obj(part, "part_slots", slots);
  }
  return slots;
}

InvObject* part_slot_find(InvObject* part, int32_t slot_id) {
  InvObject* slots = tree_field_get_obj(part, "part_slots");
  if (!slots) return nullptr;
  const int32_t n = tree_vector_size(slots);
  for (int32_t i = 0; i < n; ++i) {
    InvObject* s = tree_vector_element_at(slots, i);
    if (s && tree_field_get_int(s, "slot_id") == slot_id) return s;
  }
  return nullptr;
}

InvObject* part_slot_ensure(InvObject* part, int32_t slot_id) {
  if (!part) return nullptr;
  if (InvObject* existing = part_slot_find(part, slot_id)) return existing;
  InvObject* slots = part_slot_table(part);
  InvObject* s = gameref_new();
  tree_field_set_int(s, "slot_id", slot_id);
  tree_field_set_float(s, "px", 0.f);
  tree_field_set_float(s, "py", 0.f);
  tree_field_set_float(s, "pz", 0.f);
  tree_field_set_float(s, "oy", 0.f);
  tree_field_set_float(s, "op", 0.f);
  tree_field_set_float(s, "or", 0.f);
  tree_vector_add(slots, s);
  return s;
}

void part_slot_apply_visual(InvObject* slot) {
  if (!slot) return;
  InvObject* visual = tree_field_get_obj(slot, "visual");
  if (!visual || !render_d3d9_mesh_ready(visual)) return;
  const float px = tree_field_get_float(slot, "px") * kPartSlotMetresToMesh;
  const float py = tree_field_get_float(slot, "py") * kPartSlotMetresToMesh;
  const float pz = tree_field_get_float(slot, "pz") * kPartSlotMetresToMesh;
  const float oy = tree_field_get_float(slot, "oy");
  const float op = tree_field_get_float(slot, "op");
  const float or_ = tree_field_get_float(slot, "or");
  float sx = 1.f, sy = 1.f, sz = 1.f;
  {
    float ignore[6];
    render_d3d9_mesh_get_transform(visual, &ignore[0], &ignore[1], &ignore[2],
                                   &ignore[3], &ignore[4], &ignore[5], &sx, &sy,
                                   &sz);
  }
  render_d3d9_mesh_set_transform(visual, px, py, pz, oy, op, or_, sx, sy, sz);
  tree_field_set_float(visual, "attach_x", px);
  tree_field_set_float(visual, "attach_y", py);
  tree_field_set_float(visual, "attach_z", pz);
  tree_field_set_float(visual, "attach_oy", oy);
}

}  // namespace

void part_bind_slot_visual(InvObject* part, int32_t slot_id, InvObject* visual,
                           float px_m, float py_m, float pz_m, float oy,
                           float op, float or_) {
  InvObject* slot = part_slot_ensure(part, slot_id);
  if (!slot) return;
  tree_field_set_obj(slot, "visual", visual);
  tree_field_set_float(slot, "px", px_m);
  tree_field_set_float(slot, "py", py_m);
  tree_field_set_float(slot, "pz", pz_m);
  tree_field_set_float(slot, "oy", oy);
  tree_field_set_float(slot, "op", op);
  tree_field_set_float(slot, "or", or_);
}

void part_set_slot_pos(InvObject* part, int32_t slot_id, InvObject* pos,
                       InvObject* ypr) {
  InvObject* slot = part_slot_ensure(part, slot_id);
  if (!slot) return;
  if (pos) {
    float x = 0, y = 0, z = 0;
    vec3_get(pos, &x, &y, &z);
    tree_field_set_float(slot, "px", x);
    tree_field_set_float(slot, "py", y);
    tree_field_set_float(slot, "pz", z);
  }
  if (ypr) {
    float oy = 0, op = 0, or_ = 0;
    ypr_get(ypr, &oy, &op, &or_);
    tree_field_set_float(slot, "oy", oy);
    tree_field_set_float(slot, "op", op);
    tree_field_set_float(slot, "or", or_);
  }
  part_slot_apply_visual(slot);
}

bool part_slot_get_pose(InvObject* part, int32_t slot_id, float* px, float* py,
                        float* pz, float* oy, float* op, float* or_) {
  InvObject* slot = part_slot_find(part, slot_id);
  if (!slot) return false;
  if (px) *px = tree_field_get_float(slot, "px");
  if (py) *py = tree_field_get_float(slot, "py");
  if (pz) *pz = tree_field_get_float(slot, "pz");
  if (oy) *oy = tree_field_get_float(slot, "oy");
  if (op) *op = tree_field_get_float(slot, "op");
  if (or_) *or_ = tree_field_get_float(slot, "or");
  return true;
}

InvObject* part_slot_visual(InvObject* part, int32_t slot_id) {
  InvObject* slot = part_slot_find(part, slot_id);
  return slot ? tree_field_get_obj(slot, "visual") : nullptr;
}

InvObject* part_on_slot(InvObject* part, int32_t slot_id) {
  InvObject* slot = part_slot_find(part, slot_id);
  if (!slot) return nullptr;
  if (InvObject* child = tree_field_get_obj(slot, "child")) return child;
  return tree_field_get_obj(slot, "visual");
}

int32_t part_slot_id_on_slot(InvObject* part, int32_t slot_id) {
  InvObject* slot = part_slot_find(part, slot_id);
  if (!slot) return 0;
  return tree_field_get_int(slot, "child_slot_id");
}

bool part_install(InvObject* parent, int32_t parent_slot_id, InvObject* child,
                  int32_t child_slot_id) {
  if (!parent || !child || parent_slot_id == 0 || child_slot_id == 0)
    return false;
  InvObject* slot = part_slot_ensure(parent, parent_slot_id);
  if (!slot) return false;
  // Phase 2.80: garage UI locks (status=1 → refuse install).
  if (tree_field_get_int(slot, "disabled") != 0) return false;
  tree_field_set_obj(slot, "child", child);
  tree_field_set_int(slot, "child_slot_id", child_slot_id);
  if (render_d3d9_mesh_ready(child))
    tree_field_set_obj(slot, "visual", child);
  tree_field_set_obj(child, "part_parent", parent);
  tree_field_set_int(child, "part_parent_slot", parent_slot_id);
  // Ensure mate slot exists on the child (used by getSlots / save walk).
  part_slot_ensure(child, child_slot_id);
  return true;
}

bool part_ensure_chassis_cfg_slots(InvObject* car) {
  if (!car) return false;
  if (tree_field_get_int(car, "cfg_slots_ready") == 1) return true;

  const char* vname = nullptr;
  if (InvObject* nm = tree_field_get_obj(car, "vehicleName"))
    vname = string_cstr(nm);
  std::string brand = racer_brand_from_name(vname);
  std::vector<CfgSlot> slots;
  std::string main_cfg = main_cfg_path_for_car(brand, vname);
  if (!parse_cfg_slots_file(main_cfg.c_str(), &slots)) {
    main_cfg = "cars/racers/Baiern_data/scripts/_main_DevilSport.cfg";
    parse_cfg_slots_file(main_cfg.c_str(), &slots);
  }
  int32_t attach_n = 0;
  for (const CfgSlot& cs : slots) {
    if (cs.id <= 0) continue;
    InvObject* s = part_slot_ensure(car, cs.id);
    if (!s) continue;
    tree_field_set_float(s, "px", cs.px);
    tree_field_set_float(s, "py", cs.py);
    tree_field_set_float(s, "pz", cs.pz);
    tree_field_set_float(s, "oy", cs.oy);
    tree_field_set_float(s, "op", cs.op);
    tree_field_set_float(s, "or", cs.or_);
    if (!cs.name.empty())
      tree_field_set_obj(s, "name", string_new(cs.name.c_str()));
    if (!cs.attaches.empty()) {
      InvObject* atts = tree_vector_new();
      for (const CfgAttach& a : cs.attaches) {
        InvObject* ao = gameref_new();
        tree_field_set_int(ao, "type_id", a.type_id);
        tree_field_set_int(ao, "mate_slot", a.mate_slot);
        tree_vector_add(atts, ao);
        ++attach_n;
      }
      tree_field_set_obj(s, "attaches", atts);
    }
  }
  tree_field_set_int(car, "cfg_slots_ready", slots.empty() ? 0 : 1);
  tree_field_set_int(car, "cfg_slot_count",
                     static_cast<int32_t>(slots.size()));
  tree_field_set_int(car, "cfg_attach_count", attach_n);
  return !slots.empty();
}

bool part_type_matches_attach(int32_t part_id, int32_t attach_type) {
  if (part_id == 0 || attach_type == 0) return false;
  if (part_id == attach_type) return true;
  // Compare local 16-bit resource ids (pack prefix may differ).
  if ((part_id & 0xFFFF) == (attach_type & 0xFFFF)) return true;
  return false;
}

bool part_try_cfg_path_attaches(InvObject* car, InvObject* part,
                                int32_t* parent_slot, int32_t* child_slot) {
  if (!car || !part || !parent_slot || !child_slot) return false;
  const char* path = nullptr;
  if (InvObject* p = tree_field_get_obj(part, "cfg_path")) path = string_cstr(p);
  if (!path || !path[0]) return false;
  std::vector<CfgSlot> part_slots;
  if (!parse_cfg_slots_file(path, &part_slots)) return false;
  auto slot_free = [&](int32_t id) -> bool {
    if (id <= 0) return false;
    if (part_slot_is_disabled(car, id)) return false;
    return part_on_slot(car, id) == nullptr;
  };
  // Part-side attach: attach <parentType> <parentSlot> (see F_bumper.cfg).
  for (const CfgSlot& ps : part_slots) {
    for (const CfgAttach& a : ps.attaches) {
      if (!slot_free(a.mate_slot)) continue;
      *parent_slot = a.mate_slot;
      *child_slot = ps.id > 0 ? ps.id : a.mate_slot;
      return true;
    }
  }
  return false;
}

bool part_find_cfg_install(InvObject* car, InvObject* part, int32_t* parent_slot,
                           int32_t* child_slot) {
  if (!car || !part || !parent_slot || !child_slot) return false;
  *child_slot = 1;
  part_ensure_chassis_cfg_slots(car);

  auto slot_free = [&](int32_t id) -> bool {
    if (id <= 0) return false;
    if (part_slot_is_disabled(car, id)) return false;
    return part_on_slot(car, id) == nullptr;
  };

  const int32_t pref = tree_field_get_int(part, "install_slot");
  if (pref > 0 && slot_free(pref)) {
    *parent_slot = pref;
    return true;
  }

  const int32_t part_id = java_util_resource_ResourceRef_id(part);
  InvObject* slots = tree_field_get_obj(car, "part_slots");
  const int32_t n = slots ? tree_vector_size(slots) : 0;

  // Phase 2.142 — chassis-side attach match (e.g. slot 401 ↔ block 0x52).
  for (int32_t i = 0; i < n; ++i) {
    InvObject* s = tree_vector_element_at(slots, i);
    if (!s) continue;
    const int32_t sid = tree_field_get_int(s, "slot_id");
    if (!slot_free(sid)) continue;
    InvObject* atts = tree_field_get_obj(s, "attaches");
    if (!atts) continue;
    const int32_t an = tree_vector_size(atts);
    for (int32_t j = 0; j < an; ++j) {
      InvObject* a = tree_vector_element_at(atts, j);
      if (!a) continue;
      const int32_t tid = tree_field_get_int(a, "type_id");
      if (!part_type_matches_attach(part_id, tid)) continue;
      int32_t mate = tree_field_get_int(a, "mate_slot");
      if (mate <= 0) mate = 1;
      *parent_slot = sid;
      *child_slot = mate;
      tree_field_set_int(part, "install_via_attach", 1);
      return true;
    }
  }

  // Part-side attach lines from cfg_path (e.g. F_bumper → slot 3).
  if (part_try_cfg_path_attaches(car, part, parent_slot, child_slot)) {
    tree_field_set_int(part, "install_via_attach", 1);
    return true;
  }

  const char* hc = tree_host_class(part);

  auto prefer_name = [&](const char* needle) -> int32_t {
    if (!needle || !slots) return 0;
    for (int32_t i = 0; i < n; ++i) {
      InvObject* s = tree_vector_element_at(slots, i);
      if (!s) continue;
      const int32_t id = tree_field_get_int(s, "slot_id");
      if (!slot_free(id)) continue;
      const char* nm = nullptr;
      if (InvObject* so = tree_field_get_obj(s, "name")) nm = string_cstr(so);
      if (nm && std::strstr(nm, needle)) return id;
    }
    return 0;
  };

  if (hc && std::strstr(hc, "Bumper")) {
    int32_t id = prefer_name("bumper");
    if (!id && slot_free(3)) id = 3;
    if (!id && slot_free(9)) id = 9;
    if (id) {
      *parent_slot = id;
      *child_slot = id;
      return true;
    }
  }
  if (hc && (std::strstr(hc, "Block") || std::strstr(hc, "Engine") ||
             std::strstr(hc, "enginepart"))) {
    int32_t id = prefer_name("engine");
    if (!id && slot_free(401)) id = 401;
    if (id) {
      *parent_slot = id;
      return true;
    }
  }
  if (hc && std::strstr(hc, "Hood")) {
    int32_t id = prefer_name("hood");
    if (!id && slot_free(601)) id = 601;
    if (id) {
      *parent_slot = id;
      return true;
    }
  }

  for (int32_t i = 0; i < n; ++i) {
    InvObject* s = tree_vector_element_at(slots, i);
    if (!s) continue;
    const int32_t id = tree_field_get_int(s, "slot_id");
    if (id >= 101 && id <= 199) continue;
    if (!slot_free(id)) continue;
    const char* nm = nullptr;
    if (InvObject* so = tree_field_get_obj(s, "name")) nm = string_cstr(so);
    if (!nm || !nm[0]) continue;
    *parent_slot = id;
    return true;
  }
  return false;
}

void part_disable_slot(InvObject* part, int32_t slot_id, int32_t status) {
  if (!part || slot_id == 0) return;
  InvObject* slot = part_slot_ensure(part, slot_id);
  if (!slot) return;
  tree_field_set_int(slot, "disabled", status ? 1 : 0);
}

bool part_slot_is_disabled(InvObject* part, int32_t slot_id) {
  if (!part || slot_id == 0) return false;
  InvObject* slot = part_slot_find(part, slot_id);
  if (!slot) return false;
  return tree_field_get_int(slot, "disabled") != 0;
}

InvObject* part_car_root(InvObject* part) {
  if (!part) return nullptr;
  InvObject* cur = part;
  for (int depth = 0; depth < 64; ++depth) {
    InvObject* parent = tree_field_get_obj(cur, "part_parent");
    if (!parent) return cur;
    cur = parent;
  }
  return cur;
}

int32_t part_wheel_id(InvObject* part) {
  if (!part) return -1;
  InvObject* root = part_car_root(part);
  if (!root) return -1;
  InvObject* cur = part;
  for (int depth = 0; depth < 64; ++depth) {
    InvObject* parent = tree_field_get_obj(cur, "part_parent");
    if (!parent) return -1;
    if (parent == root) {
      const int32_t slot = tree_field_get_int(cur, "part_parent_slot");
      // PE getWheelID @ 0x004691F0: [*(payload+0x4C)+0xD0]; (slot-101)%10.
      if (slot > 100 && slot <= 400) return (slot - 101) % 10;
      return -1;
    }
    cur = parent;
  }
  return -1;
}

int32_t part_slot_count(InvObject* part) {
  InvObject* slots = tree_field_get_obj(part, "part_slots");
  return slots ? tree_vector_size(slots) : 0;
}

int32_t part_slot_id_at(InvObject* part, int32_t index) {
  InvObject* slots = tree_field_get_obj(part, "part_slots");
  if (!slots || index < 0 || index >= tree_vector_size(slots)) return -1;
  InvObject* s = tree_vector_element_at(slots, index);
  return s ? tree_field_get_int(s, "slot_id") : -1;
}

void valocity_sync_car_mesh(InvObject* car) {
  if (!car) return;
  InvObject* mesh = tree_field_get_obj(car, "visual_mesh");
  if (!mesh || !render_d3d9_mesh_ready(mesh)) return;
  const float px = tree_field_get_float(car, "pos_x");
  const float py = tree_field_get_float(car, "pos_y");
  const float pz = tree_field_get_float(car, "pos_z");
  const float oy = tree_field_get_float(car, "ori_y");
  // Chassis SCX is local around origin (same unit space as city meshes).
  // Child body parts inherit this pose via mesh parent hierarchy.
  render_d3d9_mesh_set_transform(mesh, px, py, pz, oy, 0.f, 0.f, 1.f, 1.f, 1.f);
}

void valocity_sync_wheel_visuals(InvObject* car, float dt) {
  if (!car || dt <= 0.f) return;
  constexpr float kMesh = 100.f;  // metres → mesh (same as part slots)
  constexpr float kSteerVis = 0.55f;
  constexpr float kSteerWheelRefR = 0.15f;  // ~SteeringWheel stock r
  constexpr float kTwoPi = 6.2831853f;

  float fwd = 0.f;
  InvObject* body = tree_field_get_obj(car, "chassis");
  if (!body) body = car;
  if (InvObject* vel = java_util_resource_PhysicsRef_getVel(body)) {
    float vx = 0, vy = 0, vz = 0;
    vec3_get(vel, &vx, &vy, &vz);
    const float yaw = tree_field_get_float(car, "ori_y");
    fwd = vx * std::sin(yaw) + vz * std::cos(yaw);
  } else {
    float ssq = tree_field_get_float(car, "speed_sq");
    if (ssq < 0.f) ssq = 0.f;
    fwd = std::sqrt(ssq);
  }

  // Phase 2.71: Chassis.setSteerWheel(r,z) — larger r → less visual yaw.
  float sw_r = tree_field_get_float(car, "steer_wheel_r");
  if (sw_r < 0.02f) sw_r = tree_field_get_float(car, "steer_wheel_radius");
  if (sw_r < 0.02f) sw_r = kSteerWheelRefR;
  float sw_scale = kSteerWheelRefR / sw_r;
  if (sw_scale < 0.4f) sw_scale = 0.4f;
  if (sw_scale > 2.5f) sw_scale = 2.5f;

  for (int32_t i = 0; i < 4; ++i) {
    InvObject* visual = part_slot_visual(car, 101 + i);
    if (!visual || !render_d3d9_mesh_ready(visual)) continue;
    float px = 0, py = 0, pz = 0, oy = 0, op = 0, or_ = 0;
    if (!part_slot_get_pose(car, 101 + i, &px, &py, &pz, &oy, &op, &or_))
      continue;

    InvObject* w = java_game_parts_bodypart_Chassis_getWheel(car, i);
    float radius = 0.32f;
    float steer_ang = 0.f;
    if (w) {
      radius = java_game_parts_WheelRef_getRadius(w);
      if (radius < 0.05f) radius = 0.32f;
      if (i < 2)
        steer_ang =
            -java_game_parts_WheelRef_getSteer(w) * kSteerVis * sw_scale;
    }

    float spin = tree_field_get_float(visual, "wheel_spin");
    spin += (fwd / radius) * dt;
    while (spin > kTwoPi) spin -= kTwoPi;
    while (spin < 0.f) spin += kTwoPi;
    tree_field_set_float(visual, "wheel_spin", spin);

    float sx = 1.f, sy = 1.f, sz = 1.f;
    {
      float ign[6];
      render_d3d9_mesh_get_transform(visual, &ign[0], &ign[1], &ign[2], &ign[3],
                                     &ign[4], &ign[5], &sx, &sy, &sz);
    }
    render_d3d9_mesh_set_transform(visual, px * kMesh, py * kMesh, pz * kMesh,
                                   oy + steer_ang, op + spin, or_, sx, sy, sz);
  }
}

void valocity_ensure_car_physics(InvObject* car) {
  if (!car) return;
  InvObject* body = tree_field_get_obj(car, "chassis");
  if (!body) {
    body = car;
    tree_field_set_obj(car, "chassis", body);
  }
  if (physics_shape(body) == 0) {
    java_util_resource_PhysicsRef_createBox(body, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    physics_set_gear(body, 1);
  }
  const float px = tree_field_get_float(car, "pos_x");
  const float py = tree_field_get_float(car, "pos_y");
  const float pz = tree_field_get_float(car, "pos_z");
  const float oy = tree_field_get_float(car, "ori_y");
  java_util_resource_PhysicsRef_setMatrix(body, vec3_new(px, py, pz),
                                          ypr_new(oy, 0.f, 0.f));
  java_util_resource_GameRef_setActiveCollision(body);
  tree_field_set_float(car, "speed_sq", physics_speed_square(body));
  valocity_ensure_car_mesh(car);
}

// engine_tick_phys_traffic — GroundRef_traffic.cpp

void valocity_simulate(InvObject* city, float dt) {
  if (!city || dt <= 0.f) return;
  if (!tree_field_get_int(city, "entered")) return;
  InvObject* player = tree_field_get_obj(city, "player");
  InvObject* car = tree_field_get_obj(city, "car");
  if (!car && player) car = tree_field_get_obj(player, "car");
  if (!car || tree_field_get_int(car, "stopped")) return;

  InvObject* body = tree_field_get_obj(car, "chassis");
  if (!body) body = car;
  if (physics_shape(body) == 0) valocity_ensure_car_physics(car);
  body = tree_field_get_obj(car, "chassis");
  if (!body) body = car;
  if (physics_shape(body) == 0) return;

  InvObject* ctrl = player ? tree_field_get_obj(player, "controller") : nullptr;
  if (!ctrl) ctrl = input_get_controller(0);

  // Phase 2.66: push turn axis onto front WheelRefs; aggregate drive/radius.
  // Phase 2.67: also friction/sliction/brake/hbrake/roll_res → contact.
  // Phase 2.72: push brake/handbrake axes onto WheelRefs (Brake.java balance).
  {
    const float ctrl_steer =
        ctrl ? input_map_get_logical(ctrl, kAxisTurnLR) : 0.f;
    const float ctrl_brake =
        ctrl ? input_map_get_logical(ctrl, kAxisBrake) : 0.f;
    const float ctrl_hb =
        ctrl ? input_map_get_logical(ctrl, kAxisHandbrake) : 0.f;
    float bal = tree_field_get_float(car, "brake_balance");
    if (bal < 0.01f) bal = 0.5f;  // Chassis.java default
    if (bal > 1.f) bal = 1.f;
    // Torque units match Brake.java stock (~0.18); /0.2 → arcade 0..1 in contact.
    constexpr float kBrkTorque = 0.2f;

    float sum_steer = 0.f;
    int ns = 0;
    float sum_drive = 0.f;
    int nd = 0;
    float sum_r = 0.f;
    int nr = 0;
    float sum_f = 0.f, sum_s = 0.f, sum_b = 0.f, sum_hb = 0.f, sum_rr = 0.f;
    float sum_pk_b = 0.f, sum_pk_c = 0.f, sum_pk_d = 0.f;
    float sum_spring = 0.f, sum_damp = 0.f, sum_rest = 0.f, sum_arm = 0.f;
    int nc = 0, n_arm = 0;
    for (int32_t i = 0; i < 4; ++i) {
      InvObject* w = java_game_parts_bodypart_Chassis_getWheel(car, i);
      if (!w) continue;
      if (i < 2) {
        java_game_parts_WheelRef_setSteer(w, ctrl_steer);
        sum_steer += java_game_parts_WheelRef_getSteer(w);
        ++ns;
      }
      // Front: (1-bal), rear: bal — same split as Brake.updatevariables.
      const float share = (i < 2) ? (1.f - bal) : bal;
      java_game_parts_WheelRef_setBrake(w, kBrkTorque * ctrl_brake * share);
      java_game_parts_WheelRef_setHBrake(
          w, (i >= 2) ? (kBrkTorque * ctrl_hb) : 0.f);
      sum_drive += java_game_parts_WheelRef_getDrive(w);
      ++nd;
      const float rad = java_game_parts_WheelRef_getRadius(w);
      if (rad > 0.05f) {
        sum_r += rad;
        ++nr;
      }
      sum_f += wheelref_get_friction(w);
      sum_s += wheelref_get_sliction(w);
      sum_b += wheelref_get_brake(w);
      sum_hb += wheelref_get_hbrake(w);
      sum_rr += wheelref_get_roll_res(w);
      sum_pk_d += wheelref_get_pacejka(w, 0);
      sum_pk_c += wheelref_get_pacejka(w, 2);
      sum_pk_b += wheelref_get_pacejka(w, 4);
      sum_spring += wheelref_get_force(w);
      sum_damp += wheelref_get_damp_bound(w);
      sum_rest += wheelref_get_rest_len(w);
      const float al = wheelref_get_arm_len(w);
      if (al > 0.05f) {
        sum_arm += al;
        ++n_arm;
      }
      ++nc;
    }
    const float steer = ns ? (sum_steer / static_cast<float>(ns)) : ctrl_steer;
    const float drive = nd ? (sum_drive / static_cast<float>(nd)) : 1.f;
    const float radius = nr ? (sum_r / static_cast<float>(nr)) : 0.32f;
    physics_set_wheel_params(body, steer, drive, radius);
    if (nc > 0) {
      const float inv = 1.f / static_cast<float>(nc);
      // Brake.java torque≈0.18 → near full arcade brake when /0.2.
      float brk = (sum_b * inv) / 0.2f;
      float hb = (sum_hb * inv) / 0.2f;
      if (brk > 1.f) brk = 1.f;
      if (hb > 1.f) hb = 1.f;
      if (brk < 0.f) brk = 0.f;
      if (hb < 0.f) hb = 0.f;
      physics_set_wheel_contact(body, sum_f * inv, sum_s * inv, brk, hb,
                                sum_rr * inv);
      physics_set_wheel_pacejka(body, sum_pk_b * inv, sum_pk_c * inv,
                                sum_pk_d * inv);
      const float arm =
          n_arm ? (sum_arm / static_cast<float>(n_arm)) : 0.244f;
      physics_set_wheel_suspension(body, sum_spring * inv, sum_damp * inv,
                                   sum_rest * inv, arm);
    }
  }

  // Phase 2.81: sample Chassis/DynoData torque at estimated RPM → drive scale.
  {
    InvObject* engine = tree_field_get_obj(car, "engine");
    InvObject* dyno =
        engine ? tree_field_get_obj(engine, "dynodata") : nullptr;
    if (!dyno) dyno = tree_field_get_obj(car, "dynodata");
    if (dyno) {
      float rpm = physics_get_engine_rpm(body);
      if (rpm < 800.f) rpm = 900.f;
      const float nitro_ax =
          ctrl ? input_map_get_logical(ctrl, kAxisNitro) : 0.f;
      const float nm =
          java_game_parts_bodypart_Chassis_getTorque(car, rpm, nitro_ax);
      physics_set_drive_torque(body, nm);
      tree_field_set_float(car, "engine_rpm", rpm);
      tree_field_set_float(car, "engine_torque_nm", nm);
    } else {
      physics_set_drive_torque(body, 0.f);  // legacy accel path
    }
  }

  physics_drive(body, ctrl, dt);

  float x = 0, y = 0, z = 0, oy = 0, op = 0, or_ = 0;
  if (InvObject* p = java_util_resource_PhysicsRef_getPos(body))
    vec3_get(p, &x, &y, &z);
  if (InvObject* o = java_util_resource_PhysicsRef_getOri(body))
    ypr_get(o, &oy, &op, &or_);
  tree_field_set_float(car, "pos_x", x);
  tree_field_set_float(car, "pos_y", y);
  tree_field_set_float(car, "pos_z", z);
  tree_field_set_float(car, "ori_y", oy);
  const float speed_sq = physics_speed_square(body);
  tree_field_set_float(car, "speed_sq", speed_sq);
  // Phase 2.73: odometer — integrate |v|·dt onto Chassis.mileage.
  {
    float spd = speed_sq > 0.f ? std::sqrt(speed_sq) : 0.f;
    const float miles = java_game_parts_bodypart_Chassis_getMileage(car) + spd * dt;
    java_game_parts_bodypart_Chassis_setMileage(car, miles);
  }

  valocity_ensure_car_mesh(car);
  valocity_sync_car_mesh(car);
  valocity_sync_wheel_visuals(car, dt);

  if (InvObject* nav = tree_field_get_obj(city, "nav"))
    java_game_Navigator_updateNavigator(nav, car, 0);

  valocity_update_camera(city);
  // W32D/W33D — PE Engine_tickPhysTraffic @ 0x463AC0 (GII_ANIMATE mode28).
  // Soft: sweep_dead → frame_update(dt) → sort_compact budget=1 after
  // cam update (MainLoop fills Engine_camPosScratch; host lookat).
  // dt ≡ *Engine_physWorld pushed @ 0x463c54.
  if (InvObject* map = tree_field_get_obj(city, "map"))
    engine_tick_phys_traffic(map, dt);
  valocity_update_hud(city);
}

void* valocity_camera_key() {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(0x56C010u));
}

void* valocity_hud_font_key() {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(0x56C020u));
}

void* valocity_hud_speed_key() {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(0x56C021u));
}

void* valocity_hud_gear_key() {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(0x56C022u));
}

float valocity_speed_kph(InvObject* car) {
  if (!car) return 0.f;
  float ssq = tree_field_get_float(car, "speed_sq");
  if (ssq < 0.f) ssq = 0.f;
  // CarInfo.java: sqrt(speedSq) * 2.24 * 1.61 → KPH
  return std::sqrt(ssq) * 2.24f * 1.61f;
}

void valocity_update_hud(InvObject* city) {
  if (!city) return;
  InvObject* car = tree_field_get_obj(city, "car");
  if (!car) {
    if (InvObject* player = tree_field_get_obj(city, "player"))
      car = tree_field_get_obj(player, "car");
  }
  if (!car) return;

  // Stock: Frontend.setFonts → Text.RID_* in frontend.rpk (see Text.java).
  // Loose frontend/meshes/Fonts/*.scx is not shipped; load via RID.
  void* font = frontend_medium_font();
  if (font && !render_d3d9_font_ready(font)) {
    const int32_t rid = java_util_resource_ResourceRef_id(
        static_cast<InvObject*>(font));
    if (rid) render_d3d9_font_load_from_rid(font, rid);
  }
  if (!font || !render_d3d9_font_ready(font)) {
    font = valocity_hud_font_key();
    if (!render_d3d9_font_ready(font)) {
      if (!rpak_find_by_name("frontend.rpk")) rpak_open("frontend.rpk");
      const RpakPack* fe = rpak_find_by_name("frontend.rpk");
      const int32_t rid =
          fe ? rpak_make_id(fe->pack_id, 0x0020) : 0;  // Text.RID_SIMPLE20
      if (!rid || !render_d3d9_font_load_from_rid(font, rid)) return;
    }
  }

  void* speed_key = valocity_hud_speed_key();
  void* gear_key = valocity_hud_gear_key();
  render_d3d9_text_create(speed_key, font, 0.85f, -0.82f);
  render_d3d9_text_set_align(speed_key, 0);  // RIGHT
  render_d3d9_text_set_color(speed_key, 0xFFFFFFFFu);
  render_d3d9_text_create(gear_key, font, -0.85f, -0.82f);
  render_d3d9_text_set_align(gear_key, 2);  // LEFT
  render_d3d9_text_set_color(gear_key, 0xFFCCFFCCu);

  const float kph = valocity_speed_kph(car);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f KPH", kph);
  render_d3d9_text_set_string(speed_key, buf);
  render_d3d9_text_update(speed_key);

  InvObject* body = tree_field_get_obj(car, "chassis");
  if (!body) body = car;
  const int32_t gear = physics_get_gear(body);
  if (gear < 0)
    std::snprintf(buf, sizeof(buf), "R");
  else if (gear == 0)
    std::snprintf(buf, sizeof(buf), "N");
  else
    std::snprintf(buf, sizeof(buf), "G%d", gear);
  render_d3d9_text_set_string(gear_key, buf);
  render_d3d9_text_update(gear_key);
}

void valocity_update_camera(InvObject* city) {
  if (!city) return;
  InvObject* car = tree_field_get_obj(city, "car");
  if (!car) {
    if (InvObject* player = tree_field_get_obj(city, "player"))
      car = tree_field_get_obj(player, "car");
  }
  if (!car) return;
  const float px = tree_field_get_float(car, "pos_x");
  const float py = tree_field_get_float(car, "pos_y");
  const float pz = tree_field_get_float(car, "pos_z");
  const float oy = tree_field_get_float(car, "ori_y");
  void* cam = valocity_camera_key();
  // Ensure camera exists with city-scale far clip.
  if (render_d3d9_camera_half_aov(cam) <= 0.f) {
    void* vp = render_d3d9_viewport_active();
    if (!vp) {
      vp = reinterpret_cast<void*>(static_cast<uintptr_t>(0x56C011u));
      render_d3d9_viewport_create(vp, 0, 0.f, 0.f, 1.f, 1.f);
    }
    render_d3d9_camera_create(cam, nullptr, vp, 0, 45.f, 0.5f, 2000.f, 1.f, 1.f,
                              1, 0);
  }
  render_d3d9_camera_chase(cam, px, py, pz, oy, 8.f, 2.8f, 1.2f);
  // Activate even without a live D3D device so chase lookat is the active cam
  // (headless Valocity.camera smoke; stock always has a device).
  void* vp = render_d3d9_viewport_active();
  if (!vp) vp = reinterpret_cast<void*>(static_cast<uintptr_t>(0x56C011u));
  render_d3d9_camera_activate(cam, vp, 0);
}

InvObject* valocity_return_to_garage(InvObject* city) {
  if (!city) return nullptr;
  if (InvObject* nav = tree_field_get_obj(city, "nav")) {
    if (InvObject* m = tree_field_get_obj(city, "mPlayer"))
      navigator_rem_marker(nav, m);
    navigator_hide(nav);
  }
  InvObject* map = tree_field_get_obj(city, "map");
  if (map) java_util_resource_GroundRef_delTraffic(map);
  tree_field_set_int(city, "traffic_count",
                     map ? tree_field_get_int(map, "traffic_count") : 0);
  tree_field_set_int(city, "entered", 0);
  if (InvObject* player = tree_field_get_obj(city, "player")) {
    InvObject* car = tree_field_get_obj(city, "car");
    if (!car) car = tree_field_get_obj(player, "car");
    if (car) {
      tree_field_set_float(car, "speed_sq", 0.f);
      InvObject* body = tree_field_get_obj(car, "chassis");
      if (!body) body = car;
      if (physics_shape(body) != 0) {
        // PE PhysicsRef.setMatrix(null,null) @ 0x00480920 writes origin.
        // Garage return must stop the chassis in place — not teleport.
        physics_set_velocity(body, 0.f, 0.f, 0.f);
        java_util_resource_PhysicsRef_setStatic(body, 1);
      }
    }
  }
  // Simulate YesNoDialog accept → changeActiveSection(GameLogic.garage)
  InvObject* garage = tree_field_get_obj(city, "parentState");
  if (!garage) garage = game_logic_garage();
  return game_logic_change_active_section(garage);
}


// → GroundRef_traffic.cpp

// java_game_Vehicle_getSpeedSquare / getHorn / hasCrime — bodies in
// runtime/Cars/Cars.cpp (PE @ 0x00480500 / 0x0043DB60 / 0x00440BF0).

}  // namespace inv
