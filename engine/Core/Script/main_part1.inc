// Fragment 1/4 of main.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


namespace {

const char* class_index_path() {
  // Prefer beside the executable (POST_BUILD copy), then source-tree relative.
  static const char* candidates[] = {
      "data/class_index.txt",
      "native/engine/data/class_index.txt",
      "../data/class_index.txt",
  };
  for (const char* p : candidates) {
    if (FILE* f = std::fopen(p, "rb")) {
      std::fclose(f);
      return p;
    }
  }
  return candidates[0];
}

const char* find_game_root() {
  static const char* root_candidates[] = {
      ".",
      "game",
      "../game",
      "Street Legal Racing - Redline",
      "../Street Legal Racing - Redline",
      "../../Street Legal Racing - Redline",
  };
  for (const char* r : root_candidates) {
    std::string probe = std::string(r) + "/system/Scripts/lang/System.class";
    if (FILE* f = std::fopen(probe.c_str(), "rb")) {
      std::fclose(f);
      return r;
    }
  }
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace inv;
#if defined(_WIN32)
  setvbuf(stdout, nullptr, _IONBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
#endif

  const bool want_boot = (argc >= 2 && std::strcmp(argv[1], "--boot") == 0);
  const bool want_game = (argc >= 2 && std::strcmp(argv[1], "--game") == 0);
  bool boot_wait = true;
  bool want_window = false;
  bool game_auto_new = false;
  const char* player_name = "Player";
  for (int i = 1; i < argc; ++i) {
    if (argv[i] && std::strcmp(argv[i], "--window") == 0) want_window = true;
  }
  if (want_boot || want_game) {
    for (int i = 2; i < argc; ++i) {
      if (!argv[i]) continue;
      if (std::strcmp(argv[i], "--no-wait") == 0)
        boot_wait = false;
      else if (std::strcmp(argv[i], "--window") == 0)
        want_window = true;
      else if (std::strcmp(argv[i], "--auto-new") == 0)
        game_auto_new = true;
      else if (argv[i][0] && argv[i][0] != '-')
        player_name = argv[i];
    }
  }
  // Smoke --game --no-wait implies auto_new + capped frames.
  if (want_game && !boot_wait) game_auto_new = true;

  time_init();
  register_all_stubs();
  std::printf("SLRR native engine  table=%zu  build=%d\n", kNativeTableCount,
              java_lang_System_buildNumber());

  if (want_window) {
    if (!render_d3d9_open(800, 600, "SLRR Engine")) {
      std::printf("FAIL render D3D9 open\n");
      return 6;
    }
    for (int i = 0; i < 3; ++i) render_d3d9_flush();
    const int32_t modes = render_d3d9_num_display_modes();
    // Viewport smoke: half-screen TL, activate with clear flags, getters.
    void* vp_key = reinterpret_cast<void*>(static_cast<uintptr_t>(0x560001u));
    render_d3d9_viewport_create(vp_key, 0, 0.f, 0.f, 0.5f, 0.5f);
    render_d3d9_viewport_activate(vp_key,
                                  kViewportClearDepth | kViewportClearTarget);
    // Camera smoke: stock Java would pass aov*0.5 → 45° half for 90° AOV.
    void* cam_key = reinterpret_cast<void*>(static_cast<uintptr_t>(0x560002u));
    render_d3d9_camera_create(cam_key, nullptr, vp_key, 0, 45.f, 0.1f, 100.f,
                              1.f, 1.f, 1, 0);
    render_d3d9_camera_activate(cam_key, vp_key, 0);
    render_d3d9_flush();
    const float vw = render_d3d9_viewport_get_width(vp_key);
    const float vh = render_d3d9_viewport_get_height(vp_key);
    const float asp = render_d3d9_viewport_get_aspect(vp_key);
    const float half_aov = render_d3d9_camera_half_aov(cam_key);
    const bool vp_ok = render_d3d9_viewport_active() == vp_key && vw > 0.49f &&
                       vw < 0.51f && vh > 0.49f && vh < 0.51f && asp > 1.2f &&
                       asp < 1.4f;
    const bool cam_ok = render_d3d9_camera_active() == cam_key &&
                        half_aov > 44.9f && half_aov < 45.1f &&
                        render_d3d9_camera_dmin(cam_key) > 0.09f &&
                        render_d3d9_camera_dmax(cam_key) > 99.f;
    render_d3d9_camera_deactivate(cam_key, vp_key);
    render_d3d9_camera_destroy(cam_key);
    // Fog smoke — Scene/GroundRef.setFog path (e.g. 0x0007121e, 20, 150).
    render_d3d9_set_fog(0x0007121e, 20.f, 150.f);
    render_d3d9_flush();
    const bool fog_ok = render_d3d9_fog_enabled() &&
                        render_d3d9_fog_color() == 0x0007121e &&
                        render_d3d9_fog_near() > 19.f &&
                        render_d3d9_fog_far() > 149.f;
    render_d3d9_clear_fog();
    // Texture smoke — stock makeTexture loads sourcefile DDS (DXT3 Baiern chassis).
    const char* dds =
        "cars/racers/Baiern_data/textures/Baiern_chassis.dds";
    void* tex_key = reinterpret_cast<void*>(static_cast<uintptr_t>(0x560003u));
    std::string tex_path = rpak_resolve_path(dds);
    if (tex_path.empty()) tex_path = dds;
    const bool tex_ok =
        render_d3d9_texture_create_from_file(tex_key, tex_path.c_str()) &&
        render_d3d9_texture_ready(tex_key) &&
        render_d3d9_texture_width(tex_key) == 512 &&
        render_d3d9_texture_height(tex_key) == 512;
    render_d3d9_set_global_envmap(tex_key);
    render_d3d9_osd_clear();
    // OSD blit: fullscreen textured rect (createBG semantics 0,0,2,2 pri=-2).
    render_d3d9_osd_add_rect(0.f, 0.f, 2.f, 2.f, tex_key, -2);
    const bool osd_ok = render_d3d9_osd_count() == 1;
    render_d3d9_flush();
    const bool env_ok = render_d3d9_global_envmap() == tex_key;
    render_d3d9_osd_clear();
    render_d3d9_texture_destroy(tex_key);

    // RPAK→texture: open Baiern pack, load chassis via sourcefile descriptor.
    bool rpak_tex_ok = false;
    const char* gr = find_game_root();
    if (gr) {
      rpak_set_game_root(gr);
      const int32_t baiern_id =
          java_lang_System_openLib(string_new("cars/racers/Baiern.rpk"));
      const RpakPack* baiern = rpak_get(baiern_id);
      if (baiern) {
        for (const auto& e : baiern->entries) {
          if (e.is_dir) continue;
          if (e.name.find("Baiern_chassis") == std::string::npos &&
              e.path.find("Baiern_chassis") == std::string::npos)
            continue;
          const int32_t rid =
              rpak_make_id(baiern_id, static_cast<uint16_t>(e.type_id));
          InvObject* rr = resref_new();
          java_util_resource_ResourceRef_set(rr, rid);
          java_util_resource_ResourceRef_load(rr);
          rpak_tex_ok = render_d3d9_texture_ready(rr) &&
                        render_d3d9_texture_width(rr) == 512 &&
                        render_d3d9_texture_height(rr) == 512;
          std::printf("rpak texture id=0x%X name='%s' path='%s' kind=0x%X "
                      "type=%d sz=%u ready=%d %dx%d\n",
                      rid, e.name.c_str(), e.path.c_str(), e.kind,
                      java_util_resource_ResourceRef_type(rr), e.size,
                      rpak_tex_ok ? 1 : 0, render_d3d9_texture_width(rr),
                      render_d3d9_texture_height(rr));
          render_d3d9_texture_destroy(rr);
          break;
        }
      }
    }

    render_d3d9_viewport_deactivate(vp_key);
    render_d3d9_viewport_destroy(vp_key);
    // Phase 2.123 — stock exe icon + cursors from assets/StreetLegal_Redline_exe.
    const bool assets_ok = render_d3d9_assets_ready();
    const bool icon_ok = render_d3d9_stock_icon_loaded();
    const bool cur2_ok = render_d3d9_set_stock_cursor(2);
    const bool cur11_ok = render_d3d9_set_stock_cursor(11);
    const int32_t cur_id = render_d3d9_stock_cursor();
    render_d3d9_set_stock_cursor(2);
    std::printf("boot stock_assets ok=%d icon=%d cur2=%d cur11=%d id=%d\n",
                assets_ok ? 1 : 0, icon_ok ? 1 : 0, cur2_ok ? 1 : 0,
                cur11_ok ? 1 : 0, cur_id);
    std::printf("render window ok=%d modes=%d size=%dx%d viewport=%d "
                "camera=%d fog=%d texture=%d envmap=%d rpak_tex=%d osd=%d "
                "aspect=%.3f half_aov=%.1f\n",
                render_d3d9_ready() ? 1 : 0, modes, render_d3d9_width(),
                render_d3d9_height(), vp_ok ? 1 : 0, cam_ok ? 1 : 0,
                fog_ok ? 1 : 0, tex_ok ? 1 : 0, env_ok ? 1 : 0,
                rpak_tex_ok ? 1 : 0, osd_ok ? 1 : 0, asp, half_aov);
    if (!render_d3d9_ready() || modes < 1 || !vp_ok || !cam_ok || !fog_ok ||
        !tex_ok || !env_ok || !rpak_tex_ok || !osd_ok || !assets_ok ||
        !icon_ok || !cur2_ok || !cur11_ok || cur_id != 11) {
      std::printf("FAIL render D3D9\n");
      render_d3d9_close();
      return 6;
    }

    // Live input smoke: physical DIK/mouse + mapAxis → virtual SELECT.
    input_live_enable(true);
    InvObject* ctrl_map = gameref_new();
    java_io_Input_mapAxis(ctrl_map, kAxisSelect, /*device*/ 0, kDikReturn, 0.f,
                          1.f, 0.f, 1.f);
#ifdef _WIN32
    keybd_event(VK_RETURN, 0, 0, 0);
#endif
    input_live_poll();
    const float select_phys = java_io_Input_getAxis(0, kDikReturn);
    const float select_virt =
        java_io_Controller_user_GetAxisVal(ctrl_map, kAxisSelect);
    const int32_t live_key = java_io_Input_lastKey() & 0xFF;
#ifdef _WIN32
    keybd_event(VK_RETURN, 0, KEYEVENTF_KEYUP, 0);
#endif
    input_live_poll();
    java_io_Controller_user_Reset(ctrl_map);
    const bool input_ok =
        select_phys > 0.5f && select_virt > 0.5f && live_key == kDikReturn;
    // Mouse DI smoke: inject LMB → device 1 phys btn1 (axis 3).
#ifdef _WIN32
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
#endif
    input_live_poll();
    const float btn1 = java_io_Input_getAxis(1, kMousePhysBtn1);
    const float cx = java_io_Input_getAxis(1, kMousePhysX);
    const float cy = java_io_Input_getAxis(1, kMousePhysY);
#ifdef _WIN32
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
#endif
    input_live_poll();
    const bool mouse_ok = input_di8_mouse_ready() && btn1 > 0.5f;
    input_live_enable(false);
    std::printf("input live phys=%.2f virt=%.2f lastKey=0x%X di8=%d mouse=%d "
                "btn1=%.2f cursor=(%.2f,%.2f) map=%d ok=%d\n",
                select_phys, select_virt, live_key, input_di8_ready() ? 1 : 0,
                input_di8_mouse_ready() ? 1 : 0, btn1, cx, cy,
                input_map_count(ctrl_map) == 0 ? 1 : 0,
                (input_ok && mouse_ok) ? 1 : 0);
    if (!input_ok) {
      std::printf("FAIL live input / mapAxis\n");
      render_d3d9_close();
      return 6;
    }
    if (!input_di8_ready()) {
      std::printf("FAIL DirectInput8 keyboard\n");
      render_d3d9_close();
      return 6;
    }
    if (!mouse_ok) {
      std::printf("FAIL DirectInput8 mouse\n");
      render_d3d9_close();
      return 6;
    }

    // Mesh/SCX smoke — Baiern hood.scx (INVO v4, 2 submeshes).
    void* mesh_key = reinterpret_cast<void*>(static_cast<uintptr_t>(0x560004u));
    const char* scx =
        "cars/racers/Baiern_data/meshes/hood.scx";
    std::string scx_path = rpak_resolve_path(scx);
    if (scx_path.empty()) scx_path = scx;
    const bool mesh_ok =
        render_d3d9_mesh_create_from_file(mesh_key, scx_path.c_str()) &&
        render_d3d9_mesh_ready(mesh_key) &&
        render_d3d9_mesh_submesh_count(mesh_key) == 2 &&
        render_d3d9_mesh_vertex_count(mesh_key) == 284 &&
        render_d3d9_mesh_index_count(mesh_key) == 1074 &&
        render_d3d9_mesh_textured_count(mesh_key) == 2;
    // Pose smoke: yaw 45°, lift via setMatrix; scaleMesh bakes verts (PE).
    const float kPi4 = 0.785398163f;
    const float kPi2 = 1.570796327f;
    float bmin0[3] = {0, 0, 0}, bmax0[3] = {0, 0, 0};
    float bmin1[3] = {0, 0, 0}, bmax1[3] = {0, 0, 0};
    render_d3d9_mesh_local_bounds(mesh_key, bmin0, bmax0);
    InvObject* pose_pos = vec3_new(10.f, 5.f, -20.f);
    InvObject* pose_ori = ypr_new(kPi4, 0.f, 0.f);
    java_util_resource_RenderRef_setMatrix_1(
        reinterpret_cast<InvObject*>(mesh_key), pose_pos, pose_ori);
    java_util_resource_ResourceRef_scaleMesh(
        reinterpret_cast<InvObject*>(mesh_key), 0.5f, 0.5f, 0.5f);
    render_d3d9_mesh_local_bounds(mesh_key, bmin1, bmax1);
    float tpx = 0, tpy = 0, tpz = 0, ty = 0, tp = 0, tr = 0, tsx = 0, tsy = 0,
          tsz = 0;
    render_d3d9_mesh_get_transform(mesh_key, &tpx, &tpy, &tpz, &ty, &tp, &tr,
                                   &tsx, &tsy, &tsz);
    const bool pose_ok =
        tpx > 9.9f && tpx < 10.1f && tpy > 4.9f && tpy < 5.1f && tpz < -19.9f &&
        tpz > -20.1f && ty > 0.78f && ty < 0.79f && tsx > 0.99f && tsx < 1.01f &&
        std::fabs(bmax1[0] - bmax0[0] * 0.5f) < 0.05f &&
        std::fabs(bmax1[1] - bmax0[1] * 0.5f) < 0.05f &&
        std::fabs(bmax1[2] - bmax0[2] * 0.5f) < 0.05f;
    // Parent hierarchy: parent yaw 90°, child local +X10 → world (0,0,-10).
    void* parent_key =
        reinterpret_cast<void*>(static_cast<uintptr_t>(0x560007u));
    void* child_key = reinterpret_cast<void*>(static_cast<uintptr_t>(0x560008u));
    render_d3d9_mesh_set_transform(parent_key, 0.f, 0.f, 0.f, kPi2, 0.f, 0.f,
                                   1.f, 1.f, 1.f);
    render_d3d9_mesh_set_transform(child_key, 10.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f,
                                   1.f, 1.f);
    render_d3d9_mesh_set_parent(child_key, parent_key);
    float wx = 0, wy = 0, wz = 0;
    render_d3d9_mesh_world_origin(child_key, &wx, &wy, &wz);
    const bool hier_ok = render_d3d9_mesh_get_parent(child_key) == parent_key &&
                         wx > -0.05f && wx < 0.05f && wy > -0.05f &&
                         wy < 0.05f && wz < -9.9f && wz > -10.1f;
    // Bone attach: parent bone "wheel" at local +X5; child on that bone → (5,0,0).
    InvObject* parent_obj = reinterpret_cast<InvObject*>(parent_key);
    InvObject* child_obj = reinterpret_cast<InvObject*>(child_key);
    const int32_t bone00 =
        java_util_resource_RenderRef_getBoneId(parent_obj, string_new("bone00"));
    const int32_t bone_w =
        java_util_resource_RenderRef_getBoneId(parent_obj, string_new("wheel"));
    const int32_t bone_w2 =
        java_util_resource_RenderRef_getBoneId(parent_obj, string_new("wheel"));
    render_d3d9_mesh_set_transform(parent_key, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f,
                                   1.f, 1.f);
    java_util_resource_RenderRef_setMatrix(
        parent_obj, bone_w, parent_obj, vec3_new(5.f, 0.f, 0.f), nullptr);
    java_util_resource_RenderRef_setMatrix(child_obj, bone_w, parent_obj,
                                           vec3_new(0.f, 0.f, 0.f), nullptr);
    float bx = 0, by = 0, bz = 0;
    render_d3d9_mesh_world_origin(child_key, &bx, &by, &bz);
    const bool bone_ok = bone00 == 0 && bone_w == 1 && bone_w2 == 1 &&
                         render_d3d9_mesh_get_attach_bone(child_key) == bone_w &&
                         bx > 4.9f && bx < 5.1f && by > -0.05f && by < 0.05f &&
                         bz > -0.05f && bz < 0.05f;
    // Fullscreen viewport + camera with far clip past mesh AABB.
    void* mesh_vp = reinterpret_cast<void*>(static_cast<uintptr_t>(0x560005u));
    void* mesh_cam = reinterpret_cast<void*>(static_cast<uintptr_t>(0x560006u));
    render_d3d9_viewport_create(mesh_vp, 0, 0.f, 0.f, 1.f, 1.f);
    render_d3d9_viewport_activate(mesh_vp,
                                  kViewportClearDepth | kViewportClearTarget);
    render_d3d9_camera_create(mesh_cam, nullptr, mesh_vp, 0, 45.f, 1.f, 500.f,
                              1.f, 1.f, 1, 0);
    render_d3d9_camera_activate(mesh_cam, mesh_vp, 0);
    render_d3d9_mesh_queue_clear();
    render_d3d9_mesh_queue_add(mesh_key);
    const bool mesh_q = render_d3d9_mesh_queue_count() == 1;
    for (int i = 0; i < 2; ++i) render_d3d9_flush();
    std::printf("mesh scx ok=%d queue=%d pose=%d hier=%d bone=%d "
                "world=(%.2f,%.2f,%.2f) bworld=(%.2f,%.2f,%.2f) "
                "subs=%d verts=%d idx=%d tex=%d path='%s'\n",
                mesh_ok ? 1 : 0, mesh_q ? 1 : 0, pose_ok ? 1 : 0,
                hier_ok ? 1 : 0, bone_ok ? 1 : 0, wx, wy, wz, bx, by, bz,
                render_d3d9_mesh_submesh_count(mesh_key),
                render_d3d9_mesh_vertex_count(mesh_key),
                render_d3d9_mesh_index_count(mesh_key),
                render_d3d9_mesh_textured_count(mesh_key), scx_path.c_str());
    render_d3d9_mesh_queue_clear();
    render_d3d9_mesh_destroy(mesh_key);
    render_d3d9_mesh_set_parent(child_key, nullptr);
    render_d3d9_camera_deactivate(mesh_cam, mesh_vp);
    render_d3d9_camera_destroy(mesh_cam);
    render_d3d9_viewport_deactivate(mesh_vp);
    render_d3d9_viewport_destroy(mesh_vp);
    if (!mesh_ok || !mesh_q || !pose_ok || !hier_ok || !bone_ok) {
      std::printf("FAIL mesh/scx\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.54: INVO v3 multi-submesh (Badge wheel.scx = chassis + Material).
    {
      void* v3_key = reinterpret_cast<void*>(static_cast<uintptr_t>(0x5600AAu));
      const char* v3_path =
          "./cars/fake_racers/Badge/meshes/wheel.scx";
      const bool loaded =
          render_d3d9_mesh_create_from_file(v3_key, v3_path) &&
          render_d3d9_mesh_ready(v3_key);
      const int32_t subs = loaded ? render_d3d9_mesh_submesh_count(v3_key) : 0;
      const int32_t verts = loaded ? render_d3d9_mesh_vertex_count(v3_key) : 0;
      const int32_t idx = loaded ? render_d3d9_mesh_index_count(v3_key) : 0;
      // 60+165 verts, 28*3+84*3 indices
      const bool v3_ok =
          loaded && subs >= 2 && verts == 225 && idx == (28 + 84) * 3;
      std::printf("mesh invo_v3 ok=%d subs=%d verts=%d idx=%d path='%s'\n",
                  v3_ok ? 1 : 0, subs, verts, idx, v3_path);
      render_d3d9_mesh_destroy(v3_key);
      if (!v3_ok) {
        std::printf("FAIL mesh/invo_v3\n");
        render_d3d9_close();
        return 6;
      }
    }

    // Font/OSD text smoke — RID→name via frontend.rpk + glyph UVs.
    const char* font_root = find_game_root();
    if (font_root) rpak_set_game_root(font_root);
    const int32_t fe_pack = rpak_open("frontend.rpk");
    InvObject* font_simple = resref_new();
    InvObject* font_slii = resref_new();
    const int32_t rid_simple20 =
        fe_pack > 0 ? rpak_make_id(fe_pack, 0x0020) : 0;  // Text.RID_SIMPLE20
    const int32_t rid_slii24 =
        fe_pack > 0 ? rpak_make_id(fe_pack, 0x0025) : 0;  // Text.RID_SLII24
    if (rid_simple20)
      java_util_resource_ResourceRef_set(font_simple, rid_simple20);
    if (rid_slii24) java_util_resource_ResourceRef_set(font_slii, rid_slii24);
    const bool rid_ok =
        fe_pack > 0 &&
        render_d3d9_font_load_from_rid(font_simple, rid_simple20) &&
        render_d3d9_font_load_from_rid(font_slii, rid_slii24) &&
        std::strcmp(render_d3d9_font_name(font_simple), "simple20") == 0 &&
        std::strcmp(render_d3d9_font_name(font_slii), "slii24") == 0;
    void* text_key = reinterpret_cast<void*>(static_cast<uintptr_t>(0x560011u));
    const bool font_ok =
        rid_ok && render_d3d9_font_ready(font_simple) &&
        render_d3d9_font_glyph_count(font_simple) >= 120;
    const float hello_w = render_d3d9_font_measure_px(font_simple, "SLRR");
    render_d3d9_osd_clear();
    render_d3d9_text_create(text_key, font_simple, 0.f, 0.7f);
    render_d3d9_text_set_align(text_key, 1);  // CENTER
    render_d3d9_text_set_color(text_key, 0xFFFFFFFFu);
    render_d3d9_text_set_string(text_key, "SLRR");
    render_d3d9_text_update(text_key);
    const bool text_ok = font_ok && hello_w > 10.f &&
                         render_d3d9_osd_text_count() == 1;
    for (int i = 0; i < 2; ++i) render_d3d9_flush();
    std::printf("font osd ok=%d rid=%d names='%s','%s' glyphs=%d measure=%.1f "
                "texts=%d\n",
                text_ok ? 1 : 0, rid_ok ? 1 : 0,
                render_d3d9_font_name(font_simple),
                render_d3d9_font_name(font_slii),
                render_d3d9_font_glyph_count(font_simple), hello_w,
                render_d3d9_osd_text_count());
    render_d3d9_text_destroy(text_key);
    render_d3d9_font_destroy(font_simple);
    render_d3d9_font_destroy(font_slii);
    render_d3d9_osd_clear();
    if (!text_ok) {
      std::printf("FAIL font/osd text\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.21+2.26: Sound volumes + DirectSound multi-voice SfxRef.
    java_sound_Sound_setVolume(kAudioChannelEffects, 0.75f);
    java_sound_Sound_setVolume(kAudioChannelMusic, 0.4f);
    java_sound_Sound_changeMusicSet(3);  // MUSIC_SET_MENU
    const float vol_fx = java_sound_Sound_getVolume(kAudioChannelEffects);
    const float vol_mu = java_sound_Sound_getVolume(kAudioChannelMusic);
    const bool ds_ok = audio_ds_ready();
    const int32_t rid_mnu =
        fe_pack > 0 ? rpak_make_id(fe_pack, 0x0079) : 0;  // SFX_MENU_MOVE
    const int32_t rid_sel =
        fe_pack > 0 ? rpak_make_id(fe_pack, 0x007A) : 0;  // SFX_MENU_SELECT
    char wav_path[512] = {};
    const bool wav_ok =
        rid_mnu != 0 && audio_resolve_wav(rid_mnu, wav_path, sizeof(wav_path));
    InvObject* sfx = resref_new();
    InvObject* sfx2 = resref_new();
    if (rid_mnu) java_util_resource_ResourceRef_set(sfx, rid_mnu);
    if (rid_sel) java_util_resource_ResourceRef_set(sfx2, rid_sel);
    const int32_t voice =
        java_util_resource_SfxRef_nplay(sfx, nullptr, 0.f, 1.f, 1.f, 0, 0);
    const int32_t voice_again =
        java_util_resource_SfxRef_nplay(sfx, nullptr, 0.f, 1.f, 1.f, 0, voice);
    const int32_t voice2 =
        java_util_resource_SfxRef_nplay(sfx2, nullptr, 0.f, 1.f, 1.f, 0, 0);
    const int32_t active2 = audio_sfx_active_count();
    java_util_resource_SfxRef_stop(sfx, voice);
    const int32_t active1 = audio_sfx_active_count();
    const bool voice2_alive = audio_sfx_voice_alive(voice2);
    java_util_resource_SfxRef_stop(sfx2, voice2);
    const int32_t active0 = audio_sfx_active_count();
    const bool sfx_ok =
        vol_fx > 0.74f && vol_fx < 0.76f && vol_mu > 0.39f && vol_mu < 0.41f &&
        audio_music_set() == 3 && wav_ok &&
        std::strstr(wav_path, "mnumove") != nullptr && voice > 0 &&
        voice_again == voice && voice2 > 0 && voice2 != voice && active2 >= 2 &&
        active1 == 1 && voice2_alive && active0 == 0 && ds_ok &&
        std::strcmp(audio_backend(), "dsound") == 0;
    std::printf("audio sfx ok=%d ds=%d backend=%s vol=%.2f/%.2f music=%d "
                "wav='%s' v=%d,%d active=%d->%d->%d\n",
                sfx_ok ? 1 : 0, ds_ok ? 1 : 0, audio_backend(), vol_fx, vol_mu,
                audio_music_set(), wav_path, voice, voice2, active2, active1,
                active0);
    if (!sfx_ok) {
      std::printf("FAIL audio/sfx\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.27: music set playlist (Roam_ride) + next/prev + MCI play.
    java_sound_Sound_setVolume(kAudioChannelMusic, 0.35f);
    java_sound_Sound_changeMusicSet(1);  // MUSIC_SET_DRIVING → Roam_ride
    const int32_t m_count = audio_music_track_count();
    const char* m_name0 = audio_music_track_name();
    const int32_t m_idx0 = audio_music_track_index();
    java_sound_Sound_nextTrack();
    const int32_t m_idx1 = audio_music_track_index();
    const char* m_name1 = audio_music_track_name();
    java_sound_Sound_prevTrack();
    const int32_t m_idx2 = audio_music_track_index();
    const bool music_ok =
        audio_music_set() == 1 && m_count >= 2 && m_idx0 == 0 && m_idx1 == 1 &&
        m_idx2 == 0 && m_name0 && m_name0[0] && m_name1 && m_name1[0] &&
        std::strcmp(m_name0, m_name1) != 0;
    std::printf("audio music ok=%d set=%d tracks=%d idx=%d->%d->%d "
                "playing=%d name0='%.40s'\n",
                music_ok ? 1 : 0, audio_music_set(), m_count, m_idx0, m_idx1,
                m_idx2, audio_music_playing() ? 1 : 0, m_name0 ? m_name0 : "");
    java_sound_Sound_changeMusicSet(-1);  // stop
    if (!music_ok) {
      std::printf("FAIL audio/music\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.82: Animation play/loop/pause/seek/speed (City pedestrians).
    InvObject* anim = tree_host_new("java.util.resource.Animation");
    java_util_resource_Animation_setSpeed(anim, 2.f);
    java_util_resource_Animation_setFade(anim, 0.35f);
    java_util_resource_Animation_seek(anim, 0.1f);
    const float p0 = java_util_resource_Animation_getPos(anim);
    java_util_resource_Animation_loopPlay(anim);
    const int32_t looping = tree_field_get_int(anim, "anim_loop");
    java_util_resource_Animation_seek(anim, 0.55f);
    const float p1 = java_util_resource_Animation_getPos(anim);
    java_util_resource_Animation_pause(anim);
    const int32_t paused = tree_field_get_int(anim, "anim_playing");
    java_util_resource_Animation_play(anim);
    const int32_t playing = tree_field_get_int(anim, "anim_loop");
    const float spd = tree_field_get_float(anim, "anim_speed");
    const float fade = tree_field_get_float(anim, "anim_fade");
    const int32_t qn = tree_field_get_int(anim, "anim_q");
    java_util_resource_Animation_finalize(anim);
    const bool anim_ok =
        std::fabs(p0 - 0.1f) < 0.01f && looping == 1 &&
        std::fabs(p1 - 0.55f) < 0.01f && paused == 0 && playing == 0 &&
        std::fabs(spd - 2.f) < 0.01f && std::fabs(fade - 0.35f) < 0.01f &&
        qn >= 7;
    std::printf("anim host ok=%d pos=%.2f->%.2f loop=%d spd=%.1f fade=%.2f q=%d\n",
                anim_ok ? 1 : 0, p0, p1, looping, spd, fade, qn);
    if (!anim_ok) {
      std::printf("FAIL Animation host\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.83: ParticleSystem host (Osd cursor setSource / setDirectSource).
    InvObject* ps = tree_host_new("java.util.resource.ParticleSystem");
    InvObject* ps_parent = resref_new();
    InvObject* ps_type = resref_new();
    java_util_resource_ParticleSystem_init(ps, ps_parent, ps_type,
                                           string_new("cursor_fx"));
    java_util_resource_ParticleSystem_modePermanent(ps, 1);
    java_util_resource_ParticleSystem_setFreq(ps, 30.f);
    InvObject* ppos = vec3_new(0.1f, 0.2f, 0.f);
    InvObject* pvel = vec3_new(0.f, 1.f, 0.f);
    java_util_resource_ParticleSystem_setSource(
        ps, string_new("hooo"), ppos, 0.f, 0.1f, pvel, 0.f, 4.f, 1000.f,
        nullptr);
    java_util_resource_ParticleSystem_setDirectSource(
        ps, string_new("ping"), ppos, 0.f, 0.1f, pvel, 0.f, 4.f, 50.f, nullptr);
    java_util_resource_ParticleSystem_setDirectSource(
        ps, string_new("ping"), ppos, 0.f, 0.1f, pvel, 0.f, 4.f, 25.f, nullptr);
    const int32_t cnt_ping =
        java_util_resource_ParticleSystem_getCounter(ps, string_new("ping"));
    const int32_t nact = tree_field_get_int(ps, "ps_actions");
    const float ps_freq = tree_field_get_float(ps, "ps_freq");
    const int32_t perm = tree_field_get_int(ps, "ps_permanent");
    java_util_resource_ParticleSystem_delAction(ps, string_new("hooo"));
    const int32_t nact2 = tree_field_get_int(ps, "ps_actions");
    java_util_resource_ParticleSystem_stop(ps);
    const int32_t stopped = tree_field_get_int(ps, "ps_stopped");
    const int32_t nact3 = tree_field_get_int(ps, "ps_actions");
    const bool ps_ok =
        cnt_ping == 75 && nact == 2 && nact2 == 1 && nact3 == 0 &&
        stopped == 1 && perm == 1 && std::fabs(ps_freq - 30.f) < 0.01f &&
        tree_field_get_obj(ps, "ps_parent") == ps_parent;
    std::printf("particles host ok=%d ping=%d actions=%d->%d->%d freq=%.0f "
                "perm=%d\n",
                ps_ok ? 1 : 0, cnt_ping, nact, nact2, nact3, ps_freq, perm);
    if (!ps_ok) {
      std::printf("FAIL ParticleSystem host\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.85: GameType notifications + GfxEngine video + Painter strokes.
    InvObject* gt = tree_host_new("java.lang.GameType");
    InvObject* trig = gameref_new();
    java_lang_GameType_addNotification(gt, trig, 0x10, 0, nullptr);
    java_lang_GameType_addNotification_1(gt, trig, 0x20, 0, nullptr,
                                         string_new("event_handlerRaceFinish"));
    const int32_t n0 = tree_field_get_int(gt, "notif_count");
    java_lang_GameType_remNotification(gt, trig, 0x10);
    const int32_t n1 = tree_field_get_int(gt, "notif_count");
    const int32_t et1 = tree_field_get_int(gt, "notif0_etype");
    InvObject* meth1 = tree_field_get_obj(gt, "notif0_method");
    const char* m1 = meth1 ? string_cstr(meth1) : "";
    const int32_t vopen = java_render_GfxEngine_openVideo(
        string_new("data\\fmv\\prime.avi"), 1, 1);
    const int32_t vplay = java_render_GfxEngine_isPlayingVideo();
    java_render_GfxEngine_closeVideo();
    const int32_t vplay2 = java_render_GfxEngine_isPlayingVideo();
    InvObject* painter = tree_host_new("java.game.Painter");
    InvObject* cursor = gameref_new();
    java_game_Painter_doPaint(painter, cursor, 0xFF112233, 7, 0, 0.5f, 1.2f,
                              0);
    java_game_Painter_paintPart(painter, cursor, 0xFFAABBCC);
    java_game_Painter_xPaint(painter, cursor);
    const bool notif_ok =
        n0 == 2 && n1 == 1 && et1 == 0x20 && m1 &&
        std::strcmp(m1, "event_handlerRaceFinish") == 0 && vopen == 0 &&
        vplay == 1 && vplay2 == 0 &&
        tree_field_get_int(painter, "paint_count") == 2 &&
        tree_field_get_int(painter, "paint_part_fills") == 1 &&
        tree_field_get_int(painter, "xpaint_count") == 1 &&
        tree_field_get_int(cursor, "part_texture") ==
            static_cast<int32_t>(0xFFAABBCC);
    std::printf("boot notif/video/paint ok=%d notifs=%d->%d video=%d->%d "
                "paint=%d\n",
                notif_ok ? 1 : 0, n0, n1, vplay, vplay2,
                tree_field_get_int(painter, "paint_count"));
    if (!notif_ok) {
      std::printf("FAIL notif/video/paint\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.159: MainMenu FMV (DirectShow prime.avi) + MUSIC_SET_MENU.
    java_sound_Sound_changeMusicSet(3);
    const int32_t fmv = java_render_GfxEngine_openVideo(
        string_new("data\\fmv\\prime.avi"), 1, 1);
    const int32_t fmv_play = java_render_GfxEngine_isPlayingVideo();
    for (int i = 0; i < 4; ++i) render_d3d9_flush();
    java_render_GfxEngine_closeVideo();
    const bool fmv_ok = fmv == 0 && fmv_play == 1;
    std::printf("boot mainmenu_fmv ok=%d open=%d playing=%d\n", fmv_ok ? 1 : 0,
                fmv, fmv_play);
    if (!fmv_ok) {
      std::printf("FAIL mainmenu_fmv\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.86: Viewport.unproject + GfxEngine.printScreen.
    InvObject* up_vp = tree_host_new("java.render.Viewport");
    InvObject* up_cam = tree_host_new("java.render.Camera");
    render_d3d9_viewport_create(up_vp, 0, 0.f, 0.f, 1.f, 1.f);
    render_d3d9_camera_create(up_cam, nullptr, up_vp, 0, 45.f, 0.1f, 500.f,
                              1.f, 1.f, 1, 0);
    render_d3d9_camera_lookat(up_cam, 0.f, 5.f, -20.f, 0.f, 0.f, 0.f);
    render_d3d9_camera_activate(up_cam, up_vp, 0);
    InvObject* hit = java_render_Viewport_unproject(up_vp, vec3_new(0.f, 0.f, 0.f),
                                                    0);
    float upx = 0, upy = 0, upz = 0;
    if (hit) vec3_get(hit, &upx, &upy, &upz);
    InvObject* hit_r =
        java_render_Viewport_unproject(up_vp, vec3_new(0.5f, 0.f, 0.f), 0);
    float urx = 0, ury = 0, urz = 0;
    if (hit_r) vec3_get(hit_r, &urx, &ury, &urz);
    const char* shot = "native_printscreen_smoke.scrn";
    java_io_File_delete(string_new(shot));
    java_render_GfxEngine_printScreen(string_new(shot));
    const int32_t shot_n = render_d3d9_print_screen_count();
    const char* shot_last = render_d3d9_print_screen_last();
    InvObject* shot_f = file_new(shot);
    const bool shot_exists = java_io_File_exists(shot_f) != 0;
    java_io_File_delete(string_new(shot));
    render_d3d9_camera_deactivate(up_cam, up_vp);
    render_d3d9_viewport_destroy(up_vp);
    const bool unproj_ok =
        std::fabs(upy) < 0.5f && std::fabs(upx) < 2.f && std::fabs(upz) < 2.f &&
        std::fabs(urx) > 1.f && shot_n >= 1 && shot_exists && shot_last &&
        std::strstr(shot_last, "printscreen") != nullptr;
    std::printf("boot unproject/print ok=%d hit=(%.2f,%.2f,%.2f) "
                "right_x=%.2f shot=%d\n",
                unproj_ok ? 1 : 0, upx, upy, upz, urx, shot_n);
    if (!unproj_ok) {
      std::printf("FAIL unproject/printScreen\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.87: kismajomCheck — type decoded cheat via lastKey flush ring.
    input_cheat_clear();
    input_set_last_key(0);
    while (java_io_Input_lastKey()) {
    }
    input_cheat_clear();
    const char* typed = "letmeroc";
    for (const char* p = typed; *p; ++p) {
      input_set_last_key(0);
      input_set_last_key(input_dik_from_letter(*p));
    }
    input_set_last_key(0);
    while (java_io_Input_lastKey()) {
    }
    InvObject* cheats = tree_vector_new();
    tree_vector_add(cheats, string_new("mfunfspd"));     // letmeroc
    tree_vector_add(cheats, string_new("cfhgpsnpofz"));  // begformoney
    const int32_t cheat0 = java_game_GameLogic_kismajomCheck(cheats);
    input_set_last_key(0);
    for (const char* p = "begformoney"; *p; ++p) {
      input_set_last_key(0);
      input_set_last_key(input_dik_from_letter(*p));
    }
    input_set_last_key(0);
    while (java_io_Input_lastKey()) {
    }
    const int32_t cheat1 = java_game_GameLogic_kismajomCheck(cheats);
    input_set_last_key(0);
    for (const char* p = "nope"; *p; ++p) {
      input_set_last_key(0);
      input_set_last_key(input_dik_from_letter(*p));
    }
    input_set_last_key(0);
    while (java_io_Input_lastKey()) {
    }
    const int32_t cheat_miss = java_game_GameLogic_kismajomCheck(cheats);
    const bool cheat_ok = cheat0 == 0 && cheat1 == 1 && cheat_miss < 0;
    std::printf("boot kismajom ok=%d codes=%d,%d,miss=%d\n", cheat_ok ? 1 : 0,
                cheat0, cheat1, cheat_miss);
    if (!cheat_ok) {
      std::printf("FAIL kismajomCheck\n");
      render_d3d9_close();
      return 6;
    }

    // PhysicsRef smoke: box body + kinematic integrate + PE origin pose.
    // PE getPos @ 0x00480B00 / getOri @ 0x00480C10: handle 0 → Mighty
    // ERROR + nullptr. No handle+8 skip (unlike GameRef.getPos). Object
    // exists → always Vector3/Ypr (zeros if unposed). Host: !self → null.
    InvObject* phy_none = java_util_resource_PhysicsRef_getPos(nullptr);
    InvObject* ori_none = java_util_resource_PhysicsRef_getOri(nullptr);
    InvObject* unposed = resref_new();
    InvObject* unposed_p = java_util_resource_PhysicsRef_getPos(unposed);
    InvObject* unposed_o = java_util_resource_PhysicsRef_getOri(unposed);
    float npos_x = 1, npos_y = 1, npos_z = 1, nori_y = 1, nori_p = 1,
          nori_r = 1;
    if (unposed_p) vec3_get(unposed_p, &npos_x, &npos_y, &npos_z);
    if (unposed_o) ypr_get(unposed_o, &nori_y, &nori_p, &nori_r);
    physics_set_ground_y(0.f);
    InvObject* phy = resref_new();
    java_util_resource_PhysicsRef_createBox(phy, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(phy, vec3_new(0.f, 1.f, 0.f),
                                            nullptr);
    physics_set_velocity(phy, 10.f, 0.f, 0.f);
    physics_integrate(0.5f);
    float ppx = 0, ppy = 0, ppz = 0;
    vec3_get(java_util_resource_PhysicsRef_getPos(phy), &ppx, &ppy, &ppz);
    float pvx = 0, pvy = 0, pvz = 0;
    vec3_get(java_util_resource_PhysicsRef_getVel(phy), &pvx, &pvy, &pvz);
    float hx = 0, hy = 0, hz = 0;
    physics_extents(phy, &hx, &hy, &hz);
    const float spd1 = physics_speed_square(phy);
    // PE @ 0x00480920: null,null writes origin pose + stopped vel (no freeze).
    java_util_resource_PhysicsRef_setMatrix(phy, nullptr, nullptr);
    float ppx2 = 0, ppy2 = 0, ppz2 = 0;
    vec3_get(java_util_resource_PhysicsRef_getPos(phy), &ppx2, &ppy2, &ppz2);
    const float spd2 = java_game_Vehicle_getSpeedSquare(phy);
    // Gravity pulls onto ground_y+hy; Java 1×0.5×2 → PE half (0.5,0.25,1).
    const bool phys_ok =
        !phy_none && !ori_none && unposed_p && unposed_o &&
        std::fabs(npos_x) < 0.01f && std::fabs(npos_y) < 0.01f &&
        std::fabs(npos_z) < 0.01f && std::fabs(nori_y) < 0.01f &&
        std::fabs(nori_p) < 0.01f && std::fabs(nori_r) < 0.01f &&
        physics_shape(phy) == 1 && hx > 0.4f && hy > 0.2f && hz > 0.9f &&
        ppx > 4.9f && ppx < 5.1f &
        & ppy > 0.20f && ppy < 0.30f && pvx > 9.9f &&
        spd1 > 99.f && spd1 < 101.f && spd2 < 0.01f && std::fabs(ppx2) < 0.01f &&
        std::fabs(ppy2) < 0.01f && std::fabs(ppz2) < 0.01f;
    std::printf("phys box ok=%d null=%d unposed=%d shape=%d ext=(%.1f,%.1f,%.1f) "
                "pos=(%.2f,%.2f,%.2f) vel=%.1f spd=%.1f->%.1f "
                "origin=(%.2f,%.2f,%.2f) ground=%.2f\n",
                phys_ok ? 1 : 0, (!phy_none && !ori_none) ? 1 : 0,
                (unposed_p && unposed_o) ? 1 : 0, physics_shape(phy), hx, hy,
                hz, ppx, ppy, ppz, pvx, spd1, spd2, ppx2, ppy2, ppz2,
                physics_ground_y());
    if (!phys_ok) {
      std::printf("FAIL physics\n");
      render_d3d9_close();
      return 6;
    }
    // Park off-origin: physics_integrate walks all bodies; leftover origin
    // box would collide with later smokes. Isolation is setStatic, not PE.
    java_util_resource_PhysicsRef_setMatrix(phy, vec3_new(0.f, -10000.f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setStatic(phy, 1);

    // Ground drop + arcade drive from Controller mapAxis (throttle).
    InvObject* car = resref_new();
    InvObject* drv = gameref_new();
    java_util_resource_PhysicsRef_createBox(car, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(car, vec3_new(0.f, 8.f, 0.f),
                                            ypr_new(0.f, 0.f, 0.f));
    for (int i = 0; i < 40; ++i) physics_integrate(0.05f);  // fall ~2s
    float drop_x = 0, drop_y = 0, drop_z = 0;
    vec3_get(java_util_resource_PhysicsRef_getPos(car), &drop_x, &drop_y,
             &drop_z);
    const bool drop_ok = drop_y > 0.20f && drop_y < 0.30f;
    java_io_Input_mapAxis(drv, kAxisThrottle, 0, kDikUp, 0.f, 1.f, 0.f, 1.f);
    input_live_enable(true);
#ifdef _WIN32
    keybd_event(VK_UP, 0, 0, 0);
#endif
    input_live_poll();
    for (int i = 0; i < 20; ++i) physics_drive(car, drv, 0.05f);
#ifdef _WIN32
    keybd_event(VK_UP, 0, KEYEVENTF_KEYUP, 0);
#endif
    input_live_poll();
    input_live_enable(false);
    java_io_Controller_user_Reset(drv);
    float dx = 0, dy = 0, dz = 0;
    vec3_get(java_util_resource_PhysicsRef_getPos(car), &dx, &dy, &dz);
    const float drive_spd = physics_speed_square(car);
    // Forward +Z at yaw0 → moved in +Z, still on ground.
    const bool drive_ok =
        drop_ok && dz > 1.0f && dy > 0.20f && dy < 0.30f && drive_spd > 1.f;
    std::printf("phys drive ok=%d drop_y=%.2f pos=(%.2f,%.2f,%.2f) spd2=%.2f\n",
                drive_ok ? 1 : 0, drop_y, dx, dy, dz, drive_spd);
    if (!drive_ok) {
      std::printf("FAIL physics drive/ground\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.20: alignToRoad → Vector3[2] + visual mesh parented to chassis.
    InvObject* map = resref_new();
    InvObject* aligned =
        java_util_resource_GroundRef_alignToRoad(map, vec3_new(12.f, 99.f, -7.f));
    float ax = 0, ay = 0, az = 0, adx = 0, ady = 0, adz = 0;
    const bool align_arr = aligned && tree_vector_size(aligned) == 2;
    if (align_arr) {
      vec3_get(tree_vector_element_at(aligned, 0), &ax, &ay, &az);
      vec3_get(tree_vector_element_at(aligned, 1), &adx, &ady, &adz);
    }
    const bool align_ok =
        align_arr && ax > 11.9f && ax < 12.1f && ay > -0.05f && ay < 0.05f &&
        az < -6.9f && az > -7.1f && adx > -0.05f && adx < 0.05f &&
        ady > -0.05f && ady < 0.05f && adz > 0.95f && adz < 1.05f;

    void* vis_key = reinterpret_cast<void*>(static_cast<uintptr_t>(0x560012u));
    const bool vis_loaded =
        render_d3d9_mesh_create_from_file(vis_key, scx_path.c_str()) &&
        render_d3d9_mesh_ready(vis_key);
    render_d3d9_mesh_set_transform(vis_key, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f,
                                   1.f, 1.f);
    render_d3d9_mesh_set_parent(vis_key, car);
    float mx = 0, my = 0, mz = 0;
    render_d3d9_mesh_world_origin(vis_key, &mx, &my, &mz);
    const bool bind_ok =
        vis_loaded && render_d3d9_mesh_get_parent(vis_key) == car &&
        mx > dx - 0.05f && mx < dx + 0.05f && my > dy - 0.05f &&
        my < dy + 0.05f && mz > dz - 0.05f && mz < dz + 0.05f;
    std::printf("phys align/bind ok=%d align=%d pos=(%.2f,%.2f,%.2f) "
                "dir=(%.2f,%.2f,%.2f) mesh_w=(%.2f,%.2f,%.2f)\n",
                (align_ok && bind_ok) ? 1 : 0, align_ok ? 1 : 0, ax, ay, az,
                adx, ady, adz, mx, my, mz);
    render_d3d9_mesh_set_parent(vis_key, nullptr);
    render_d3d9_mesh_destroy(vis_key);
    if (!align_ok || !bind_ok) {
      std::printf("FAIL alignToRoad / mesh bind\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.22: road polyline project + lateral grip.
    physics_road_clear();
    physics_road_add_segment(0.f, 2.f, 0.f, 100.f, 2.f, 0.f);   // +X @ y=2
    physics_road_add_segment(0.f, 1.f, 50.f, 0.f, 1.f, 150.f);  // +Z @ y=1
    InvObject* road_hit =
        java_util_resource_GroundRef_alignToRoad(map, vec3_new(50.f, 99.f, 5.f));
    float rx = 0, ry = 0, rz = 0, rdx = 0, rdy = 0, rdz = 0;
    if (road_hit && tree_vector_size(road_hit) == 2) {
      vec3_get(tree_vector_element_at(road_hit, 0), &rx, &ry, &rz);
      vec3_get(tree_vector_element_at(road_hit, 1), &rdx, &rdy, &rdz);
    }
    InvObject* road_hit2 =
        java_util_resource_GroundRef_alignToRoad(map, vec3_new(8.f, 0.f, 80.f));
    float r2x = 0, r2y = 0, r2z = 0, r2dx = 0, r2dy = 0, r2dz = 0;
    if (road_hit2 && tree_vector_size(road_hit2) == 2) {
      vec3_get(tree_vector_element_at(road_hit2, 0), &r2x, &r2y, &r2z);
      vec3_get(tree_vector_element_at(road_hit2, 1), &r2dx, &r2dy, &r2dz);
    }
    // Lateral grip: pure side-slip at yaw0 should decay under drive ticks.
    InvObject* grip = resref_new();
    java_util_resource_PhysicsRef_createBox(grip, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(grip, vec3_new(0.f, 0.5f, 0.f),
                                            ypr_new(0.f, 0.f, 0.f));
    physics_set_velocity(grip, 20.f, 0.f, 0.f);  // lateral (+X), forward is +Z
    for (int i = 0; i < 30; ++i) physics_drive(grip, nullptr, 0.05f);
    float gvx = 0, gvy = 0, gvz = 0;
    vec3_get(java_util_resource_PhysicsRef_getVel(grip), &gvx, &gvy, &gvz);
    const bool road_ok =
        physics_road_count() == 2 && rx > 49.9f && rx < 50.1f && ry > 1.9f &&
        ry < 2.1f && rz > -0.1f && rz < 0.1f && rdx > 0.95f &&
        std::fabs(rdz) < 0.05f && r2x > -0.1f && r2x < 0.1f && r2y > 0.9f &&
        r2y < 1.1f && r2z > 79.9f && r2z < 80.1f && r2dz > 0.95f &&
        std::fabs(gvx) < 2.f;
    std::printf("phys road ok=%d segs=%d hit=(%.2f,%.2f,%.2f) dir=(%.2f,%.2f,%.2f) "
                "hit2=(%.2f,%.2f,%.2f) lat_vx=%.2f\n",
                road_ok ? 1 : 0, physics_road_count(), rx, ry, rz, rdx, rdy, rdz,
                r2x, r2y, r2z, gvx);
    if (!road_ok) {
      physics_road_clear();
      std::printf("FAIL road project / lateral grip\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.23: getNearestCross / getStartDirection / getRouteLength.
    // Graph: (0,0)-(100,0)-(100,80) L-shape.
    physics_road_clear();
    physics_road_add_segment(0.f, 0.f, 0.f, 100.f, 0.f, 0.f);
    physics_road_add_segment(100.f, 0.f, 0.f, 100.f, 0.f, 80.f);
    InvObject* cross0 = java_util_resource_GroundRef_getNearestCross(
        map, vec3_new(3.f, 0.f, 2.f), 0.f);
    InvObject* cross150 = java_util_resource_GroundRef_getNearestCross(
        map, vec3_new(0.f, 0.f, 0.f), 120.f);
    float c0x = 0, c0y = 0, c0z = 0, c1x = 0, c1y = 0, c1z = 0;
    if (cross0) vec3_get(cross0, &c0x, &c0y, &c0z);
    if (cross150) vec3_get(cross150, &c1x, &c1y, &c1z);
    InvObject* sdir = java_util_resource_GroundRef_getStartDirection(
        map, vec3_new(10.f, 0.f, 0.f), vec3_new(90.f, 0.f, 0.f));
    InvObject* sdir_bad = java_util_resource_GroundRef_getStartDirection(
        map, vec3_new(10.f, 0.f, 0.f), vec3_new(10.f, 0.f, 0.f));
    float sdx = 0, sdy = 0, sdz = 0;
    if (sdir) vec3_get(sdir, &sdx, &sdy, &sdz);
    const float rlen = java_util_resource_GroundRef_findRoute(
        map, vec3_new(0.f, 0.f, 0.f), vec3_new(100.f, 0.f, 80.f));
    const bool graph_ok =
        c0x > -0.1f && c0x < 0.1f && c0z > -0.1f && c0z < 0.1f &&
        c1x > 99.9f && c1x < 100.1f && c1z > 79.9f && c1z < 80.1f && sdir &&
        !sdir_bad && sdx > 0.95f && std::fabs(sdz) < 0.05f && rlen > 175.f &&
        rlen < 185.f;
    std::printf("phys graph ok=%d near=(%.1f,%.1f) far=(%.1f,%.1f) dir=(%.2f,%.2f) "
                "len=%.1f\n",
                graph_ok ? 1 : 0, c0x, c0z, c1x, c1z, sdx, sdz, rlen);
    // Phase 2.88: plotRoute samples last getRouteLength path; lineAdd/weld host.
    InvObject* route_root = resref_new();
    InvObject* route_type = resref_new();
    java_util_resource_ResourceRef_set(route_type, 0x17);
    InvObject* route_line = resref_new();
    const int32_t route_n = java_util_resource_RenderRef_plotRoute(
        route_line, route_root, route_type, 0xFFFF0000, 10.f,
        vec3_new(0.01f, 0.f, 0.01f));
    float r0x = 0, r0y = 0, r0z = 0, rNx = 0, rNy = 0, rNz = 0;
    physics_road_last_route_point(0, &r0x, &r0y, &r0z);
    physics_road_last_route_point(physics_road_last_route_count() - 1, &rNx,
                                  &rNy, &rNz);
    InvObject* manual = resref_new();
    const int32_t lc =
        java_util_resource_RenderRef_lineCreate(manual, route_root, route_type);
    java_util_resource_RenderRef_lineAdd(manual, vec3_new(1.f, 0.f, 2.f),
                                         vec3_new(0.f, 1.f, 0.f), 0xFF00FF00,
                                         0.05f);
    java_util_resource_RenderRef_lineAdd(manual, vec3_new(3.f, 0.f, 4.f),
                                         vec3_new(0.f, 1.f, 0.f), 0xFF00FF00,
                                         0.05f);
    InvObject* welder = resref_new();
    InvObject* wpts = tree_vector_new();
    tree_vector_add(wpts, vec3_new(0.f, 0.f, 0.f));
    tree_vector_add(wpts, vec3_new(2.f, 0.f, 0.f));
    java_util_resource_RenderRef_addPoints(welder, wpts, 0.02f,
                                           vec3_new(0.f, 1.f, 0.f));
    const int32_t weld0 =
        java_util_resource_RenderRef_weld(welder, vec3_new(0.5f, 0.f, 0.f),
                                          vec3_new(2.f, 0.f, 0.f), 1.0f);
    const float wprog0 = java_util_resource_RenderRef_progress(welder);
    const int32_t weld1 =
        java_util_resource_RenderRef_weld(welder, vec3_new(1.5f, 0.f, 0.f),
                                          vec3_new(2.f, 0.f, 0.f), 1.0f);
    const float wprog1 = java_util_resource_RenderRef_progress(welder);
    const bool route_ok =
        graph_ok && route_n == 1 &&
        render_line_point_count(route_line) >= 18 &&
        render_line_point_count(route_line) <= 22 &&
        physics_road_last_route_count() >= 3 && std::fabs(r0x) < 0.1f &&
        std::fabs(r0z) < 0.1f && std::fabs(rNx - 100.f) < 0.1f &&
        std::fabs(rNz - 80.f) < 0.1f && lc == 1 &&
        render_line_point_count(manual) == 2 && weld0 == 0 &&
        wprog0 > 0.45f && wprog0 < 0.55f && weld1 == 1 &&
        wprog1 > 0.99f;
    std::printf("phys routeplot ok=%d ret=%d pts=%d last=%d weld=%.2f/%.2f\n",
                route_ok ? 1 : 0, route_n, render_line_point_count(route_line),
                physics_road_last_route_count(), wprog0, wprog1);
    physics_road_clear();
    if (!graph_ok) {
      std::printf("FAIL road graph queries\n");
      render_d3d9_close();
      return 6;
    }
    if (!route_ok) {
      std::printf("FAIL plotRoute / line / weld\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.84: GroundRef water / halt traffic / ped distance.
    java_util_resource_GroundRef_setWater(map, -8.f, 300.f, 50.f);
    java_util_resource_GroundRef_setWater_1(
        map, vec3_new(0.f, -8.f, -1500.f), vec3_new(0.f, 1.f, 0.f), 300.f,
        50.f);
    java_util_resource_GroundRef_addWaterLimit(
        map, vec3_new(0.f, 0.f, -500.f), vec3_new(0.f, 0.f, 1.f));
    InvObject* tcar = gameref_new();
    const int32_t tid = java_util_resource_GroundRef_addTrafficCar(
        map, tcar, nullptr);
    java_util_resource_GroundRef_setTrafficCarBehaviour(map, tid, 2);
    java_util_resource_GroundRef_haltTrafficCross(map, vec3_new(1.f, 0.f, 2.f),
                                                 15.f);
    java_util_resource_GroundRef_haltTrafficPath(
        map, vec3_new(0.f, 0.f, 0.f), vec3_new(10.f, 0.f, 0.f));
    InvObject* ped_t = resref_new();
    java_util_resource_ResourceRef_set(ped_t, 0x42);
    java_util_resource_GroundRef_addPedestrianType(map, ped_t);
    const float ped_near = java_util_resource_GroundRef_pedestrianDistance(
        map, vec3_new(0.f, 0.f, 0.f), 0x42);
    const float ped_far = java_util_resource_GroundRef_pedestrianDistance(
        map, vec3_new(30.f, 0.f, 0.f), 0x42);
    const bool ground_fx_ok =
        tree_field_get_int(map, "water_plane") == 1 &&
        std::fabs(tree_field_get_float(map, "water_level") + 8.f) < 0.01f &&
        std::fabs(tree_field_get_float(map, "water_density") - 300.f) < 0.01f &&
        tree_field_get_int(map, "water_limits") == 1 &&
        tree_field_get_int(map, "halt_crosses") == 1 &&
        tree_field_get_int(map, "halt_paths") == 1 &&
        tree_field_get_int(map, "traffic_behaviour_last") == 2 &&
        tree_field_get_int(tcar, "traffic_behaviour") == 2 &&
        ped_near < 0.01f && ped_far > 29.f && ped_far < 31.f;
    std::printf("phys groundfx ok=%d water_y=%.1f limits=%d halt=%d/%d "
                "ped=%.1f/%.1f\n",
                ground_fx_ok ? 1 : 0, tree_field_get_float(map, "water_level"),
                tree_field_get_int(map, "water_limits"),
                tree_field_get_int(map, "halt_crosses"),
                tree_field_get_int(map, "halt_paths"), ped_near, ped_far);
    if (!ground_fx_ok) {
      std::printf("FAIL GroundRef water/halt/ped\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.24: Valocity club-garage road seed.
    const int32_t seeded = physics_road_seed_valocity();
    InvObject* g0 = java_util_resource_GroundRef_getNearestCross(
        map, vec3_new(-270.f, 9.f, 1030.f), 0.f);
    InvObject* hub = java_util_resource_GroundRef_getNearestCross(
        map, vec3_new(0.f, 0.f, 500.f), 0.f);
    float g0x = 0, g0y = 0, g0z = 0, hubx = 0, huby = 0, hubz = 0;
    if (g0) vec3_get(g0, &g0x, &g0y, &g0z);
    if (hub) vec3_get(hub, &hubx, &huby, &hubz);
    const float city_len = java_util_resource_GroundRef_findRoute(
        map, vec3_new(-278.518f, 9.8f, 1033.002f),
        vec3_new(355.381f, 1.6f, 418.244f));
    InvObject* city_dir = java_util_resource_GroundRef_getStartDirection(
        map, vec3_new(-278.518f, 9.8f, 1033.002f),
        vec3_new(0.f, 2.f, 500.f));
    const bool city_ok =
        seeded >= 6 && g0x < -270.f && g0z > 1000.f && hubx > -1.f &&
        hubx < 1.f && hubz > 499.f && hubz < 501.f && city_len > 700.f &&
        city_dir != nullptr;
    std::printf("phys cityseed ok=%d segs=%d g0=(%.1f,%.1f) hub=(%.1f,%.1f) "
                "len=%.0f\n",
                city_ok ? 1 : 0, seeded, g0x, g0z, hubx, hubz, city_len);
    physics_road_clear();
    if (!city_ok) {
      std::printf("FAIL Valocity road seed\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.25: AABB collision — mover hits static wall.
    InvObject* wall = resref_new();
    InvObject* mover = resref_new();
    java_util_resource_PhysicsRef_createBox(wall, nullptr, 0.5f, 0.5f, 0.5f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(mover, nullptr, 0.5f, 0.5f, 0.5f,
                                            nullptr);
    java_util_resource_PhysicsRef_setStatic(wall, 1);
    java_util_resource_PhysicsRef_setMatrix(wall, vec3_new(5.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(mover, vec3_new(0.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_GameRef_setActiveCollision(wall);
    java_util_resource_GameRef_setActiveCollision(mover);
    physics_set_velocity(mover, 20.f, 0.f, 0.f);
    int hits = 0;
    float max_x = 0.f;
    for (int i = 0; i < 40; ++i) {
      physics_integrate(0.05f);
      hits += physics_collide_events();
      float tx = 0, ty = 0, tz = 0;
      vec3_get(java_util_resource_PhysicsRef_getPos(mover), &tx, &ty, &tz);
      if (tx > max_x) max_x = tx;
      if (hits > 0 && i > 5) break;  // stop soon after first contact
    }
    float mxp = 0, myp = 0, mzp = 0;
    vec3_get(java_util_resource_PhysicsRef_getPos(mover), &mxp, &myp, &mzp);
    float mvx = 0, mvy = 0, mvz = 0;
    vec3_get(java_util_resource_PhysicsRef_getVel(mover), &mvx, &mvy, &mvz);
    // Contact ~x=4; must not tunnel past wall center (5); bounce vx < 0.
    const bool col_ok =
        physics_collide_active(wall) == 1 && physics_collide_active(mover) == 1 &&
        hits >= 1 && max_x < 4.6f && max_x > 3.5f && mvx < 0.f;
    std::printf("phys collide ok=%d hits=%d max_x=%.2f pos=%.2f vel=%.2f\n",
                col_ok ? 1 : 0, hits, max_x, mxp, mvx);
    if (!col_ok) {
      std::printf("FAIL physics collision\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.28: yaw OBB — long box (hz=2) at yaw=90° overlaps a cube at
    // x=2.4; axis-aligned AABB would miss (|dx|=2.4 > 1.0).
    InvObject* core = resref_new();
    InvObject* longb = resref_new();
    java_util_resource_PhysicsRef_createBox(core, nullptr, 0.5f, 0.5f, 0.5f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(longb, nullptr, 0.5f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_setStatic(core, 1);
    java_util_resource_PhysicsRef_setMatrix(core, vec3_new(0.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(longb, vec3_new(2.4f, 0.5f, 0.f),
                                            ypr_new(kPi2, 0.f, 0.f));
    java_util_resource_GameRef_setActiveCollision(core);
    java_util_resource_GameRef_setActiveCollision(longb);
    physics_set_velocity(longb, -10.f, 0.f, 0.f);
    int obb_hits = 0;
    float long_x = 2.4f;
    for (int i = 0; i < 30; ++i) {
      physics_integrate(0.05f);
      obb_hits += physics_collide_events();
      float tx = 0, ty = 0, tz = 0;
      vec3_get(java_util_resource_PhysicsRef_getPos(longb), &tx, &ty, &tz);
      long_x = tx;
      if (obb_hits > 0 && i > 2) break;
    }
    float lvx = 0, lvy = 0, lvz = 0;
    vec3_get(java_util_resource_PhysicsRef_getVel(longb), &lvx, &lvy, &lvz);
    const bool obb_ok = obb_hits >= 1 && long_x > 1.5f && lvx > -10.f;
    std::printf("phys obb ok=%d hits=%d x=%.2f vel=%.2f\n", obb_ok ? 1 : 0,
                obb_hits, long_x, lvx);
    if (!obb_ok) {
      std::printf("FAIL physics OBB/yaw\n");
      render_d3d9_close();
      return 6;
    }
    // Silence prior collide pairs so pitch smoke counts only pad/beam.
    physics_set_collide_active(wall, 0);
    physics_set_collide_active(mover, 0);
    physics_set_collide_active(core, 0);
    physics_set_collide_active(longb, 0);

    // Phase 2.31: pitch OBB — long box (Java hz=2 → PE half 1) at pitch=90°
    // reaches a cube 1.5m above; yaw-only Y-slab (|dy|=1.5 > hy+hy) would miss.
    // Wide pad so side-face pen stays > stack pen after a gravity substep.
    // High above ground so contact_half_height (unpitched hy) cannot steal Y.
    InvObject* pad = resref_new();
    InvObject* beam = resref_new();
    java_util_resource_PhysicsRef_createBox(pad, nullptr, 2.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(beam, nullptr, 0.5f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_setStatic(pad, 1);
    java_util_resource_PhysicsRef_setMatrix(pad, vec3_new(50.f, 10.5f, 50.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(beam, vec3_new(50.f, 12.0f, 50.f),
                                            ypr_new(0.f, kPi2, 0.f));
    java_util_resource_GameRef_setActiveCollision(pad);
    java_util_resource_GameRef_setActiveCollision(beam);
    physics_set_velocity(beam, 0.f, -8.f, 0.f);
    int pitch_hits = 0;
    float beam_y = 12.0f;
    float min_y = 12.0f;
    for (int i = 0; i < 20; ++i) {
      physics_integrate(0.05f);
      pitch_hits += physics_collide_events();
      float tx = 0, ty = 0, tz = 0;
      vec3_get(java_util_resource_PhysicsRef_getPos(beam), &tx, &ty, &tz);
      beam_y = ty;
      if (ty < min_y) min_y = ty;
    }
    float bvx = 0, bvy = 0, bvz = 0;
    vec3_get(java_util_resource_PhysicsRef_getVel(beam), &bvx, &bvy, &bvz);
    // Stays stacked above pad top (y=11): center ~13 with ±2 pitch extent.
    const bool pitch_ok =
        pitch_hits >= 1 && min_y > 11.5f && beam_y > 11.5f && bvy > -8.f;
    std::printf("phys obb3 ok=%d hits=%d y=%.2f min=%.2f vel=%.2f\n",
                pitch_ok ? 1 : 0, pitch_hits, beam_y, min_y, bvy);
    if (!pitch_ok) {
      std::printf("FAIL physics OBB/pitch\n");
      render_d3d9_close();
      return 6;
    }
    physics_set_collide_active(pad, 0);
    physics_set_collide_active(beam, 0);

    // Phase 2.32: handbrake / nitro / road ride-height via physics_drive.
    physics_road_clear();
    physics_road_add_segment(0.f, 3.f, 0.f, 200.f, 3.f, 0.f);
    InvObject* drv2 = gameref_new();
    InvObject* coast = resref_new();
    InvObject* hb = resref_new();
    InvObject* nit = resref_new();
    java_util_resource_PhysicsRef_createBox(coast, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(hb, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(nit, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(coast, vec3_new(10.f, 0.5f, 10.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(hb, vec3_new(20.f, 0.5f, 10.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(nit, vec3_new(30.f, 0.5f, 10.f),
                                            nullptr);
    physics_set_velocity(coast, 0.f, 0.f, 30.f);
    physics_set_velocity(hb, 0.f, 0.f, 30.f);
    physics_set_velocity(nit, 0.f, 0.f, 0.f);
    java_io_Controller_user_SetAxisForce(drv2, kAxisHandbrake, 0.f, 1.f);
    for (int i = 0; i < 20; ++i) {
      physics_drive(coast, nullptr, 0.05f);
      physics_drive(hb, drv2, 0.05f);
    }
    java_io_Controller_user_SetAxisForce(drv2, kAxisHandbrake, 0.f, 0.f);
    float cvx = 0, cvy = 0, cvz = 0, hvx = 0, hvy = 0, hvz = 0;
    vec3_get(java_util_resource_PhysicsRef_getVel(coast), &cvx, &cvy, &cvz);
    vec3_get(java_util_resource_PhysicsRef_getVel(hb), &hvx, &hvy, &hvz);
    float coast_x = 0, coast_y = 0, coast_z = 0;
    vec3_get(java_util_resource_PhysicsRef_getPos(coast), &coast_x, &coast_y,
             &coast_z);
    // Park in place (PE setMatrix(null,null) teleports to origin).
    java_util_resource_PhysicsRef_setStatic(coast, 1);
    java_util_resource_PhysicsRef_setStatic(hb, 1);
    java_util_resource_PhysicsRef_setStatic(nit, 1);
    // Nitro vs throttle: both from rest, same duration; nitro should be faster.
    InvObject* base = resref_new();
    java_util_resource_PhysicsRef_createBox(base, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    InvObject* nit2 = resref_new();
    java_util_resource_PhysicsRef_createBox(nit2, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(base, vec3_new(40.f, 0.5f, 10.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(nit2, vec3_new(50.f, 0.5f, 10.f),
                                            nullptr);
    physics_set_velocity(base, 0.f, 0.f, 0.f);
    physics_set_velocity(nit2, 0.f, 0.f, 0.f);
    java_io_Controller_user_SetAxisForce(drv2, kAxisThrottle, 0.f, 1.f);
    java_io_Controller_user_SetAxisForce(drv2, kAxisNitro, 0.f, 0.f);
    for (int i = 0; i < 25; ++i) physics_drive(base, drv2, 0.05f);
    float base_vx = 0, base_vy = 0, base_vz = 0;
    vec3_get(java_util_resource_PhysicsRef_getVel(base), &base_vx, &base_vy,
             &base_vz);
    java_util_resource_PhysicsRef_setStatic(base, 1);
    java_io_Controller_user_SetAxisForce(drv2, kAxisNitro, 0.f, 1.f);
    for (int i = 0; i < 25; ++i) physics_drive(nit2, drv2, 0.05f);
    java_io_Controller_user_SetAxisForce(drv2, kAxisThrottle, 0.f, 0.f);
    java_io_Controller_user_SetAxisForce(drv2, kAxisNitro, 0.f, 0.f);
    float nit_vx = 0, nit_vy = 0, nit_vz = 0;
    vec3_get(java_util_resource_PhysicsRef_getVel(nit2), &nit_vx, &nit_vy,
             &nit_vz);
    float base_x = 0, base_y = 0, base_z = 0, nit_x = 0, nit_y = 0, nit_z = 0;
    vec3_get(java_util_resource_PhysicsRef_getPos(base), &base_x, &base_y,
             &base_z);
    vec3_get(java_util_resource_PhysicsRef_getPos(nit2), &nit_x, &nit_y, &nit_z);
    const float coast_spd = std::sqrt(cvx * cvx + cvz * cvz);
    const float hb_spd = std::sqrt(hvx * hvx + hvz * hvz);
    const float base_spd = std::sqrt(base_vx * base_vx + base_vz * base_vz);
    const float nit_spd = std::sqrt(nit_vx * nit_vx + nit_vz * nit_vz);
    const bool drive2_ok =
        hb_spd + 5.f < coast_spd && coast_y > 3.2f && coast_y < 3.8f &&
        nit_spd > base_spd + 2.f;
    std::printf(
        "phys drive2 ok=%d hb=%.1f<%.1f road_y=%.2f nitro_spd=%.1f>%.1f "
        "z=%.1f/%.1f\n",
        drive2_ok ? 1 : 0, hb_spd, coast_spd, coast_y, nit_spd, base_spd, nit_z,
        base_z);
    physics_road_clear();
    if (!drive2_ok) {
      std::printf("FAIL physics drive handbrake/nitro/road\n");
      render_d3d9_close();
      return 6;
    }
    java_util_resource_PhysicsRef_setStatic(base, 1);
    java_util_resource_PhysicsRef_setStatic(nit2, 1);

    // Phase 2.33: arcade gear / clutch / reverse.
    InvObject* gear_ctrl = gameref_new();
    InvObject* g1 = resref_new();
    InvObject* g5 = resref_new();
    InvObject* gcl = resref_new();
    InvObject* grev = resref_new();
    java_util_resource_PhysicsRef_createBox(g1, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(g5, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(gcl, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(grev, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(g1, vec3_new(60.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(g5, vec3_new(70.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(gcl, vec3_new(80.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(grev, vec3_new(90.f, 0.5f, 0.f),
                                            nullptr);
    physics_set_gear(g1, 1);
    physics_set_gear(g5, 1);
    // Pulse gear-up 4× → gear 5.
    for (int s = 0; s < 4; ++s) {
      java_io_Controller_user_SetAxisForce(gear_ctrl, kAxisGearUpDown, 0.f, 1.f);
      physics_drive(g5, gear_ctrl, 0.05f);
      java_io_Controller_user_SetAxisForce(gear_ctrl, kAxisGearUpDown, 0.f, 0.f);
      physics_drive(g5, gear_ctrl, 0.05f);
    }
    const int32_t gear5 = physics_get_gear(g5);
    java_io_Controller_user_SetAxisForce(gear_ctrl, kAxisThrottle, 0.f, 1.f);
    for (int i = 0; i < 20; ++i) {
      physics_drive(g1, gear_ctrl, 0.05f);
      physics_drive(g5, gear_ctrl, 0.05f);
    }
    float g1vx = 0, g1vy = 0, g1vz = 0, g5vx = 0, g5vy = 0, g5vz = 0;
    vec3_get(java_util_resource_PhysicsRef_getVel(g1), &g1vx, &g1vy, &g1vz);
    vec3_get(java_util_resource_PhysicsRef_getVel(g5), &g5vx, &g5vy, &g5vz);
    const float g1_spd = std::sqrt(g1vx * g1vx + g1vz * g1vz);
    const float g5_spd = std::sqrt(g5vx * g5vx + g5vz * g5vz);
    java_util_resource_PhysicsRef_setStatic(g1, 1);
    java_util_resource_PhysicsRef_setStatic(g5, 1);
    // Clutch held → no pull despite throttle.
    physics_set_gear(gcl, 1);
    java_io_Controller_user_SetAxisForce(gear_ctrl, kAxisClutch, 0.f, 1.f);
    for (int i = 0; i < 20; ++i) physics_drive(gcl, gear_ctrl, 0.05f);
    float clvx = 0, clvy = 0, clvz = 0;
    vec3_get(java_util_resource_PhysicsRef_getVel(gcl), &clvx, &clvy, &clvz);
    const float cl_spd = std::sqrt(clvx * clvx + clvz * clvz);
    java_io_Controller_user_SetAxisForce(gear_ctrl, kAxisClutch, 0.f, 0.f);
    java_util_resource_PhysicsRef_setStatic(gcl, 1);
    // Reverse gear → -Z.
    physics_set_gear(grev, -1);
    for (int i = 0; i < 20; ++i) physics_drive(grev, gear_ctrl, 0.05f);
    java_io_Controller_user_SetAxisForce(gear_ctrl, kAxisThrottle, 0.f, 0.f);
    float rvx = 0, rvy = 0, rvz = 0, rpx = 0, rpy = 0, rpz = 0;
    vec3_get(java_util_resource_PhysicsRef_getVel(grev), &rvx, &rvy, &rvz);
    vec3_get(java_util_resource_PhysicsRef_getPos(grev), &rpx, &rpy, &rpz);
    java_util_resource_PhysicsRef_setStatic(grev, 1);
    const bool gear_ok = gear5 == 5 && g1_spd > g5_spd + 3.f && cl_spd < 1.f &&
                         rvz < -2.f && rpz < -1.f;
    std::printf("phys gear ok=%d g5=%d spd1=%.1f>spd5=%.1f clutch=%.2f "
                "rev_vz=%.1f z=%.1f\n",
                gear_ok ? 1 : 0, gear5, g1_spd, g5_spd, cl_spd, rvz, rpz);
    if (!gear_ok) {
      std::printf("FAIL physics gear/clutch\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.92: engine brake — coast in gear slows more than clutch coast.
    InvObject* eb_ctrl = gameref_new();
    InvObject* eb_eng = resref_new();
    InvObject* eb_cl = resref_new();
    java_util_resource_PhysicsRef_createBox(eb_eng, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_createBox(eb_cl, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(eb_eng, vec3_new(200.f, 0.5f, 0.f),
                                            nullptr);
    java_util_resource_PhysicsRef_setMatrix(eb_cl, vec3_new(210.f, 0.5f, 0.f),
                                            nullptr);
    physics_set_gear(eb_eng, 2);
    physics_set_gear(eb_cl, 2);
    java_io_Controller_user_SetAxisForce(eb_ctrl, kAxisThrottle, 0.f, 1.f);
    java_io_Controller_user_SetAxisForce(eb_ctrl, kAxisClutch, 0.f, 0.f);
    for (int i = 0; i < 30; ++i) {
      physics_drive(eb_eng, eb_ctrl, 0.05f);
      physics_drive(eb_cl, eb_ctrl, 0.05f);
    }
    float eb0x = 0, eb0y = 0, eb0z = 0, cb0x = 0, cb0y = 0, cb0z = 0;
    vec3_get(java_util_resource_PhysicsRef_getVel(eb_eng), &eb0x, &eb0y, &eb0z);
    vec3_get(java_util_resource_PhysicsRef_getVel(eb_cl), &cb0x, &cb0y, &cb0z);
    const float e0 = std::sqrt(eb0x * eb0x + eb0z * eb0z);
    const float c0 = std::sqrt(cb0x * cb0x + cb0z * cb0z);
    java_io_Controller_user_SetAxisForce(eb_ctrl, kAxisThrottle, 0.f, 0.f);
    for (int i = 0; i < 40; ++i) {
      java_io_Controller_user_SetAxisForce(eb_ctrl, kAxisClutch, 0.f, 0.f);
      physics_drive(eb_eng, eb_ctrl, 0.05f);
      java_io_Controller_user_SetAxisForce(eb_ctrl, kAxisClutch, 0.f, 1.f);
      physics_drive(eb_cl, eb_ctrl, 0.05f);
    }
    float eb1x = 0, eb1y = 0, eb1z = 0, cb1x = 0, cb1y = 0, cb1z = 0;
    vec3_get(java_util_resource_PhysicsRef_getVel(eb_eng), &eb1x, &eb1y, &eb1z);
    vec3_get(java_util_resource_PhysicsRef_getVel(eb_cl), &cb1x, &cb1y, &cb1z);
    const float e1 = std::sqrt(eb1x * eb1x + eb1z * eb1z);
    const float c1 = std::sqrt(cb1x * cb1x + cb1z * cb1z);
    java_util_resource_PhysicsRef_setStatic(eb_eng, 1);
    java_util_resource_PhysicsRef_setStatic(eb_cl, 1);
    java_io_Controller_user_SetAxisForce(eb_ctrl, kAxisClutch, 0.f, 0.f);
    const bool ebrake_ok =
        e0 > 8.f && c0 > 8.f && std::fabs(e0 - c0) < 2.f && e1 + 1.f < c1 &&
        c1 > e1 * 2.f && e1 < e0 * 0.55f;
    std::printf("phys ebrake ok=%d eng=%.1f→%.1f clutch=%.1f→%.1f\n",
                ebrake_ok ? 1 : 0, e0, e1, c0, c1);
    if (!ebrake_ok) {
      std::printf("FAIL physics engine brake\n");
      render_d3d9_close();
      return 6;
    }

    // Phase 2.93: road yaw assist + airborne grip cut.
    physics_road_clear();
    physics_road_add_segment(0.f, 0.f, 0.f, 200.f, 0.f, 0.f);  // +X
    InvObject* ry_ctrl = gameref_new();
    InvObject* ry_body = resref_new();
    java_util_resource_PhysicsRef_createBox(ry_body, nullptr, 1.f, 0.5f, 2.f,
                                            nullptr);
    constexpr float kWantYaw = 1.5707963f;
    const float start_yaw = kWantYaw - 0.5f;
    java_util_resource_PhysicsRef_setMatrix(
        ry_body, vec3_new(50.f, 0.5f, 0.f), ypr_new(start_yaw, 0.f, 0.f));
    physics_set_gear(ry_body, 2);
    physics_set_velocity(ry_body, std::sin(start_yaw) * 14.f, 0.f,
                         std::cos(start_yaw) * 14.f);
    java_io_Controller_user_SetAxisForce(ry_ctrl, kAxisThrottle, 0.f, 0.15f);
    java_io_Controller_user_SetAxisForce(ry_ctrl, kAxisTurnLR, 0.f, 0.f);
    for (int i = 0; i < 45; ++i) physics_drive(ry_body, ry_ctrl, 0.05f);
    float ry_y = 0, ry_p = 0, ry_r = 0;
    ypr_get(java_util_resource_PhysicsRef_getOri(ry_body), &ry_y, &ry_p, &ry_r);
    float dyaw = ry_y - kWantYaw;
    while (dyaw > 3.1416f) dyaw -= 6.2832f;
    while (dyaw < -3.1416f) dyaw += 6.2832f;
    const bool yaw_ok = std::fabs(dyaw) < 0.22f &&
                        std::fabs(dyaw) + 0.15f < std::fabs(start_yaw - kWantYaw);
    java_util_resource_PhysicsRef_setStatic(ry_body, 1);