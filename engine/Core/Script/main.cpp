#include "natives.hpp"
#include "jvm_bridge.hpp"
#include "jvm.hpp"
#include "runtime.hpp"
#include "host_objects.hpp"
#include "rpak.hpp"
#include "tree_interp.hpp"
#include "game_boot.hpp"
#include "game_script.hpp"
#include "render_d3d9.hpp"
#include "input_win32.hpp"
#include "audio_win32.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

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
        ppx > 4.9f && ppx < 5.1f && ppy > 0.20f && ppy < 0.30f && pvx > 9.9f &&
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
