#include "game_boot.hpp"

#include "host_objects.hpp"
#include "input_win32.hpp"
#include "natives.hpp"
#include "render_d3d9.hpp"
#include "rpak.hpp"
#include "runtime.hpp"
#include "tree_interp.hpp"
#include "video_fmv.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace inv {
namespace {

bool game_boot_warmup(Jvm& jvm, const char* game_root) {
  if (!game_root || !game_root[0]) {
    std::fprintf(stderr, "[boot] missing game_root\n");
    return false;
  }

  if (!jvm.find_class("java.lang.System") &&
      !jvm.load_class("java.lang.System")) {
    std::printf("FAIL load System\n");
    return false;
  }

  jvm.load_class("java.game.GameLogic");
  jvm.load_class("java.game.SplashScreen");
  jvm.load_class("java.game.MainMenu");
  jvm.load_class("java.game.Garage");
  jvm.load_class("java.game.Valocity");
  jvm.load_class("java.lang.Object");
  jvm.load_class("java.lang.String");
  jvm.load_class("java.io.FindFile");
  jvm.load_class("java.io.File");

  {
    const char* dir = "tree_rpk_scan_boot";
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    auto write_min = [](const char* path) {
      unsigned char hdr[24] = {'R', 'P', 'A', 'K', 0x00, 0x02, 0, 0, 0, 0, 0,
                               0,   0,   0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0};
      if (FILE* tf = std::fopen(path, "wb")) {
        std::fwrite(hdr, 1, sizeof(hdr), tf);
        std::fclose(tf);
      }
    };
    write_min((std::string(dir) + "/a.rpk").c_str());
    write_min((std::string(dir) + "/b.rpk").c_str());
    jvm.invoke("java.lang.System", "rpkScan", "(Ljava.lang.String;)I",
               {JvmValue::make_obj(string_new((std::string(dir) + "/").c_str()))},
               true);
  }

  const char* dirs[] = {"parts/engines/", "parts/", "cars/racers/",
                        "cars/fake_racers/", "maps/"};
  int scanned = 0;
  for (const char* d : dirs) {
    JvmValue n = jvm.invoke("java.lang.System", "rpkScan",
                            "(Ljava.lang.String;)I",
                            {JvmValue::make_obj(string_new(d))}, true);
    const int got = (n.tag == JvmTag::Int) ? n.v.i : 0;
    scanned += got > 0 ? got : 0;
    std::printf("  rpkScan %s -> %d\n", d, got);
  }
  java_lang_System_openLib(string_new("system.rpk"));
  java_lang_System_openLib(string_new("frontend.rpk"));
  java_lang_System_openLib(string_new("cars.rpk"));
  java_lang_System_openLib(string_new("humans.rpk"));
  std::printf("  packs=%zu scanned_hits=%d\n", rpak_count(), scanned);

  if (!jvm.load_class("java.game.GameLogic")) {
    std::printf("FAIL load GameLogic\n");
    return false;
  }
  jvm.invoke("java.game.GameLogic", "initVehicleTypes", "()V", {}, true);
  InvObject* vts = game_logic_vehicle_types();
  const int nvt = tree_vector_size(vts);
  std::printf("  vehicleTypes=%d\n", nvt);
  if (nvt < 9) {
    std::printf("FAIL initVehicleTypes\n");
    return false;
  }
  frontend_init();
  return true;
}

}  // namespace

int game_boot_run(Jvm& jvm, const char* game_root, const char* player_name,
                  bool wait_enter) {
  const char* name =
      (player_name && player_name[0]) ? player_name : "Player";

  std::printf("=== SLRR rewrite boot (script-driven CAS) ===\n");
  std::printf("root=%s player='%s'\n", game_root, name);
  std::printf(
      "note: SplashScreen.enter TREE-first; other GameState host-shim\n");

  if (!game_boot_warmup(jvm, game_root)) return 3;

  InvObject* ls_boot = frontend_loading_screen();
  if (Jvm* j = jvm_active()) {
    std::vector<JvmValue> a = {JvmValue::make_obj(ls_boot)};
    j->invoke("java.render.LoadingScreen", "show", "()V", a, false);
  } else {
    frontend_loading_screen_show();
  }

  InvObject* splash = game_logic_boot_splash();
  std::printf("  state=%s\n",
              splash && tree_host_class(splash) ? tree_host_class(splash)
                                               : "?");

  InvObject* menu = game_logic_finish_splash();
  std::printf("  state=%s\n",
              menu && tree_host_class(menu) ? tree_host_class(menu) : "?");

  InvObject* garage = main_menu_cmd_new(name);
  InvObject* player = game_logic_player();
  const char* pname =
      player ? string_cstr(tree_field_get_obj(player, "name")) : "?";
  std::printf(
      "  state=%s name='%s' money=%d career=%d map=0x%X loading=%d\n",
      garage && tree_host_class(garage) ? tree_host_class(garage) : "?",
      pname ? pname : "?", player ? tree_field_get_int(player, "money") : -1,
      game_logic_career_in_progress(),
      garage ? tree_field_get_int(garage, "map_id") : 0,
      frontend_loading_screen_visible());

  if (!garage || game_logic_actual_state() != garage) {
    std::printf("FAIL boot did not reach Garage\n");
    return 5;
  }

  InvObject* car = player_spawn_starter_car();
  const char* cname =
      car ? string_cstr(tree_field_get_obj(car, "vehicleName")) : "?";
  std::printf("  car='%s' id=0x%X locked=%d\n", cname ? cname : "?",
              car ? java_util_resource_ResourceRef_id(car) : 0,
              car ? tree_field_get_int(car, "stopped") : 0);
  if (!car || !java_util_resource_ResourceRef_id(car) ||
      vehicle_is_driveable(car)) {
    std::printf("FAIL starter car\n");
    return 5;
  }

  constexpr int32_t CMD_MECHANIC = 117;
  constexpr int32_t CMD_HITTHESTREET = 109;
  garage_osd_command(garage, CMD_MECHANIC);
  std::printf("  CMD_MECHANIC mode=%d\n", tree_field_get_int(garage, "mode"));

  constexpr int32_t CMD_TESTTRACK = 110;
  constexpr int32_t CMD_CARLOT = 111;
  constexpr int32_t CMD_CATALOG = 113;
  InvObject* cat = garage_osd_command(garage, CMD_CATALOG);
  std::printf("  CMD_CATALOG -> %s\n",
              cat && tree_host_class(cat) ? tree_host_class(cat) : "FAIL");
  if (!cat || !std::strstr(tree_host_class(cat), "Catalog") ||
      game_logic_actual_state() != cat) {
    std::printf("FAIL Catalog\n");
    return 5;
  }
  InvObject* back_cat = game_state_return_to_garage(cat);
  if (!back_cat || game_logic_actual_state() != back_cat) {
    std::printf("FAIL Catalog→Garage\n");
    return 5;
  }

  InvObject* lot = garage_osd_command(garage, CMD_CARLOT);
  std::printf("  CMD_CARLOT -> %s map=0x%X\n",
              lot && tree_host_class(lot) ? tree_host_class(lot) : "FAIL",
              lot ? tree_field_get_int(lot, "map_id") : 0);
  if (!lot || !std::strstr(tree_host_class(lot), "CarLot") ||
      tree_field_get_int(lot, "map_id") == 0) {
    std::printf("FAIL CarLot\n");
    return 5;
  }
  if (!game_state_return_to_garage(lot) ||
      game_logic_actual_state() != garage) {
    std::printf("FAIL CarLot→Garage\n");
    return 5;
  }

  InvObject* track = garage_osd_command(garage, CMD_TESTTRACK);
  InvObject* tnav = track ? tree_field_get_obj(track, "nav") : nullptr;
  std::printf("  CMD_TESTTRACK -> %s map=0x%X tiles=%d dyn=%d\n",
              track && tree_host_class(track) ? tree_host_class(track) : "FAIL",
              track ? tree_field_get_int(track, "map_id") : 0,
              tnav ? tree_field_get_int(tnav, "tiles_count") : 0,
              tnav ? tree_field_get_int(tnav, "dynamarker_count") : 0);
  if (!track || !std::strstr(tree_host_class(track), "TestTrack") ||
      tree_field_get_int(track, "map_id") == 0 || !tnav ||
      tree_field_get_int(tnav, "tiles_count") != 48 ||
      tree_field_get_int(tnav, "dynamarker_count") < 1 ||
      tree_field_get_int(tnav, "update_count") < 1) {
    std::printf("FAIL TestTrack/nav\n");
    return 5;
  }
  if (!game_state_return_to_garage(track) ||
      game_logic_actual_state() != garage ||
      tree_field_get_int(car, "stopped") != 1) {
    std::printf("FAIL TestTrack→Garage\n");
    return 5;
  }

  constexpr int32_t CMD_BUYCARS = 112;
  constexpr int32_t CMD_CLUBINFO = 114;
  constexpr int32_t CMD_CARINFO = 115;
  constexpr int32_t CMD_BUYCARSUSED = 122;
  InvObject* market = garage_osd_command(garage, CMD_BUYCARS);
  std::printf("  CMD_BUYCARS -> %s used=%d cars=%d map=0x%X\n",
              market && tree_host_class(market) ? tree_host_class(market) : "FAIL",
              market ? tree_field_get_int(market, "used") : -1,
              market ? tree_field_get_int(market, "cars_for_sale") : 0,
              market ? tree_field_get_int(market, "map_id") : 0);
  if (!market || !std::strstr(tree_host_class(market), "CarMarket") ||
      tree_field_get_int(market, "used") != 0 ||
      tree_field_get_int(market, "cars_for_sale") != 4 ||
      tree_field_get_int(market, "map_id") == 0) {
    std::printf("FAIL CarMarket new\n");
    return 5;
  }
  game_state_return_to_garage(market);

  InvObject* usedm = garage_osd_command(garage, CMD_BUYCARSUSED);
  if (!usedm || tree_field_get_int(usedm, "used") != 1 ||
      tree_field_get_int(usedm, "cars_for_sale") != 4) {
    std::printf("FAIL CarMarket used\n");
    return 5;
  }
  game_state_return_to_garage(usedm);

  InvObject* club = garage_osd_command(garage, CMD_CLUBINFO);
  std::printf("  CMD_CLUBINFO club=%d ranking=%d\n",
              club ? tree_field_get_int(club, "club") : -1,
              club ? tree_field_get_int(club, "ranking") : -1);
  if (!club || !std::strstr(tree_host_class(club), "ClubInfo") ||
      tree_field_get_int(club, "club") != 0) {
    std::printf("FAIL ClubInfo\n");
    return 5;
  }
  game_state_return_to_garage(club);

  InvObject* cinfo = garage_osd_command(garage, CMD_CARINFO);
  const char* ciname =
      cinfo && tree_field_get_obj(cinfo, "car_name")
          ? string_cstr(tree_field_get_obj(cinfo, "car_name"))
          : nullptr;
  std::printf("  CMD_CARINFO name='%s' id=0x%X\n", ciname ? ciname : "?",
              cinfo ? tree_field_get_int(cinfo, "car_id") : 0);
  if (!cinfo || !std::strstr(tree_host_class(cinfo), "CarInfo") ||
      !ciname || !ciname[0] || tree_field_get_int(cinfo, "car_id") == 0) {
    std::printf("FAIL CarInfo\n");
    return 5;
  }
  game_state_return_to_garage(cinfo);

  InvObject* city = garage_osd_command(garage, CMD_HITTHESTREET);
  const int32_t traffic = city ? tree_field_get_int(city, "traffic_count") : 0;
  const int32_t peds = city ? tree_field_get_int(city, "pedestrian_types") : 0;
  const int32_t trigs = city ? tree_field_get_int(city, "trigger_count") : 0;
  InvObject* cnav = city ? tree_field_get_obj(city, "nav") : nullptr;
  const int32_t ntiles = cnav ? tree_field_get_int(cnav, "tiles_count") : 0;
  const int32_t nmark = cnav ? tree_field_get_int(cnav, "marker_count") : 0;
  const int32_t ndyn = cnav ? tree_field_get_int(cnav, "dynamarker_count") : 0;
  const int32_t nupd = cnav ? tree_field_get_int(cnav, "update_count") : 0;
  std::printf(
      "  CMD_HITTHESTREET -> %s map=0x%X daytime=%d traffic=%d peds=%d "
      "trigs=%d nav tiles=%d mark=%d dyn=%d upd=%d\n",
      city && tree_host_class(city) ? tree_host_class(city) : "FAIL",
      city ? tree_field_get_int(city, "map_id") : 0,
      city ? tree_field_get_int(city, "daytime") : -1, traffic, peds, trigs,
      ntiles, nmark, ndyn, nupd);
  if (!city || !std::strstr(tree_host_class(city), "Valocity") ||
      game_logic_actual_state() != city ||
      tree_field_get_int(city, "map_id") == 0 || traffic != 847 || peds != 6 ||
      trigs != 3 || ntiles != 64 || nmark != 3 || ndyn < 1 || nupd < 1) {
    std::printf("FAIL hit the street / Valocity traffic/nav\n");
    return 5;
  }
  std::printf("  addTrafficP ok=%d\n", tree_field_get_int(city, "traffic_p_smoke"));
  if (tree_field_get_int(city, "traffic_p_smoke") != 1) {
    std::printf("FAIL addTrafficP near-cross\n");
    return 5;
  }
  std::printf("  setTrafficCarBehaviour ok=%d\n",
              tree_field_get_int(city, "traffic_bh_ok"));
  if (tree_field_get_int(city, "traffic_bh_ok") != 1) {
    std::printf("FAIL setTrafficCarBehaviour\n");
    return 5;
  }
  std::printf("  haltTrafficCross ok=%d\n",
              tree_field_get_int(city, "halt_smoke"));
  if (tree_field_get_int(city, "halt_smoke") != 1) {
    std::printf("FAIL haltTrafficCross\n");
    return 5;
  }
  std::printf("  haltTrafficPath ok=%d\n",
              tree_field_get_int(city, "halt_path_smoke"));
  if (tree_field_get_int(city, "halt_path_smoke") != 1) {
    std::printf("FAIL haltTrafficPath\n");
    return 5;
  }
  std::printf("  remTrafficCar ok=%d\n",
              tree_field_get_int(city, "rem_car_smoke"));
  if (tree_field_get_int(city, "rem_car_smoke") != 1) {
    std::printf("FAIL remTrafficCar\n");
    return 5;
  }
  std::printf("  delTraffic ok=%d\n",
              tree_field_get_int(city, "del_traffic_smoke"));
  if (tree_field_get_int(city, "del_traffic_smoke") != 1) {
    std::printf("FAIL delTraffic\n");
    return 5;
  }
  std::printf("  setPedestrianDensityN ok=%d\n",
              tree_field_get_int(city, "ped_dens_smoke"));
  if (tree_field_get_int(city, "ped_dens_smoke") != 1) {
    std::printf("FAIL setPedestrianDensityN\n");
    return 5;
  }
  std::printf("  addPedestrianType ok=%d\n",
              tree_field_get_int(city, "ped_type_smoke"));
  if (tree_field_get_int(city, "ped_type_smoke") != 1) {
    std::printf("FAIL addPedestrianType\n");
    return 5;
  }
  std::printf("  remPedestrianType ok=%d\n",
              tree_field_get_int(city, "ped_rem_smoke"));
  if (tree_field_get_int(city, "ped_rem_smoke") != 1) {
    std::printf("FAIL remPedestrianType\n");
    return 5;
  }
  std::printf("  pedestrianDistance ok=%d\n",
              tree_field_get_int(city, "ped_dist_smoke"));
  if (tree_field_get_int(city, "ped_dist_smoke") != 1) {
    std::printf("FAIL pedestrianDistance\n");
    return 5;
  }
  std::printf("  setFog ok=%d\n", tree_field_get_int(city, "fog_smoke"));
  if (tree_field_get_int(city, "fog_smoke") != 1) {
    std::printf("FAIL setFog\n");
    return 5;
  }
  std::printf("  setLight ok=%d\n", tree_field_get_int(city, "light_smoke"));
  if (tree_field_get_int(city, "light_smoke") != 1) {
    std::printf("FAIL setLight\n");
    return 5;
  }
  std::printf("  setFlare ok=%d\n", tree_field_get_int(city, "flare_smoke"));
  if (tree_field_get_int(city, "flare_smoke") != 1) {
    std::printf("FAIL setFlare\n");
    return 5;
  }
  std::printf("  duplicate ok=%d\n", tree_field_get_int(city, "dup_smoke"));
  if (tree_field_get_int(city, "dup_smoke") != 1) {
    std::printf("FAIL duplicate\n");
    return 5;
  }
  std::printf("  create ok=%d\n", tree_field_get_int(city, "create_smoke"));
  if (tree_field_get_int(city, "create_smoke") != 1) {
    std::printf("FAIL create\n");
    return 5;
  }
  std::printf("  changeResource ok=%d\n", tree_field_get_int(city, "chg_smoke"));
  if (tree_field_get_int(city, "chg_smoke") != 1) {
    std::printf("FAIL changeResource\n");
    return 5;
  }
  std::printf("  setColor ok=%d\n", tree_field_get_int(city, "color_smoke"));
  if (tree_field_get_int(city, "color_smoke") != 1) {
    std::printf("FAIL setColor\n");
    return 5;
  }
  std::printf("  getTypeID ok=%d\n", tree_field_get_int(city, "type_smoke"));
  if (tree_field_get_int(city, "type_smoke") != 1) {
    std::printf("FAIL getTypeID\n");
    return 5;
  }
  std::printf("  scaleMesh ok=%d\n", tree_field_get_int(city, "scale_smoke"));
  if (tree_field_get_int(city, "scale_smoke") != 1) {
    std::printf("FAIL scaleMesh\n");
    return 5;
  }
  std::printf("  plotRoute ok=%d\n", tree_field_get_int(city, "plot_smoke"));
  if (tree_field_get_int(city, "plot_smoke") != 1) {
    std::printf("FAIL plotRoute\n");
    return 5;
  }
  std::printf("  getRoutePos ok=%d\n", tree_field_get_int(city, "routepos_smoke"));
  if (tree_field_get_int(city, "routepos_smoke") != 1) {
    std::printf("FAIL getRoutePos\n");
    return 5;
  }
  std::printf("  setMatrix ok=%d\n", tree_field_get_int(city, "setmatrix_smoke"));
  if (tree_field_get_int(city, "setmatrix_smoke") != 1) {
    std::printf("FAIL setMatrix\n");
    return 5;
  }
  std::printf("  setPos ok=%d\n", tree_field_get_int(city, "setpos_smoke"));
  if (tree_field_get_int(city, "setpos_smoke") != 1) {
    std::printf("FAIL setPos\n");
    return 5;
  }
  std::printf("  setState ok=%d\n", tree_field_get_int(city, "setstate_smoke"));
  if (tree_field_get_int(city, "setstate_smoke") != 1) {
    std::printf("FAIL setState\n");
    return 5;
  }
  std::printf("  getPos ok=%d\n", tree_field_get_int(city, "getpos_smoke"));
  if (tree_field_get_int(city, "getpos_smoke") != 1) {
    std::printf("FAIL getPos\n");
    return 5;
  }
  std::printf("  getVel ok=%d\n", tree_field_get_int(city, "getvel_smoke"));
  if (tree_field_get_int(city, "getvel_smoke") != 1) {
    std::printf("FAIL getVel\n");
    return 5;
  }
  std::printf("  setParent ok=%d\n", tree_field_get_int(city, "setparent_smoke"));
  if (tree_field_get_int(city, "setparent_smoke") != 1) {
    std::printf("FAIL setParent\n");
    return 5;
  }
  std::printf("  getSpeedSquare ok=%d\n",
              tree_field_get_int(city, "getspeedsq_smoke"));
  if (tree_field_get_int(city, "getspeedsq_smoke") != 1) {
    std::printf("FAIL getSpeedSquare\n");
    return 5;
  }
  std::printf("  isEmpty ok=%d\n", tree_field_get_int(city, "isempty_smoke"));
  if (tree_field_get_int(city, "isempty_smoke") != 1) {
    std::printf("FAIL isEmpty\n");
    return 5;
  }
  std::printf("  isScripted ok=%d\n",
              tree_field_get_int(city, "isscripted_smoke"));
  if (tree_field_get_int(city, "isscripted_smoke") != 1) {
    std::printf("FAIL isScripted\n");
    return 5;
  }
  std::printf("  getScriptInstance ok=%d\n",
              tree_field_get_int(city, "getscript_smoke"));
  if (tree_field_get_int(city, "getscript_smoke") != 1) {
    std::printf("FAIL getScriptInstance\n");
    return 5;
  }

  valocity_fire_garage_trigger(city, 2, 1);
  InvObject* denied = valocity_tick(city);
  if (denied || tree_field_get_int(city, "garage_denied") != 1 ||
      game_logic_actual_state() != city) {
    std::printf("FAIL higher-club garage deny\n");
    return 5;
  }

  valocity_fire_garage_trigger(city, 1, 1);
  InvObject* back = valocity_tick(city);
  std::printf("  trigger return garage entered=%d car_locked=%d traffic=%d\n",
              back ? tree_field_get_int(back, "entered") : 0,
              car ? tree_field_get_int(car, "stopped") : 0,
              city ? tree_field_get_int(city, "traffic_count") : -1);
  if (!back || game_logic_actual_state() != back ||
      !std::strstr(tree_host_class(back), "Garage") ||
      tree_field_get_int(city, "traffic_count") != 0 ||
      tree_field_get_int(car, "stopped") != 1) {
    std::printf("FAIL Valocity → Garage\n");
    return 5;
  }

  constexpr int32_t CMD_TIME = 116;
  for (int i = 0; i < 11; ++i) garage_osd_command(garage, CMD_TIME);
  std::printf("  CMD_TIME x11 time=%.0fs day=%d\n", game_logic_time(),
              game_logic_day());
  InvObject* night = garage_osd_command(garage, CMD_HITTHESTREET);
  const int32_t ntraffic =
      night ? tree_field_get_int(night, "traffic_count") : 0;
  std::printf("  night Valocity daytime=%d traffic=%d\n",
              night ? tree_field_get_int(night, "daytime") : -1, ntraffic);
  if (!night || tree_field_get_int(night, "daytime") != 0 || ntraffic != 183) {
    std::printf("FAIL night traffic\n");
    return 5;
  }
  valocity_fire_garage_trigger(night, 1, 1);
  InvObject* back2 = valocity_tick(night);
  if (!back2 || game_logic_actual_state() != back2 ||
      tree_field_get_int(night, "traffic_count") != 0) {
    std::printf("FAIL night → Garage\n");
    return 5;
  }

  InvObject* closed = garage_osd_command(garage, CMD_BUYCARS);
  const char* warn =
      tree_field_get_obj(garage, "last_warning")
          ? string_cstr(tree_field_get_obj(garage, "last_warning"))
          : nullptr;
  std::printf("  night dealer closed=%d warn='%s'\n", closed ? 0 : 1,
              warn ? warn : "");
  if (closed || !warn || !std::strstr(warn, "closed")) {
    std::printf("FAIL dealer hours\n");
    return 5;
  }

  std::printf("\n*** RUNNING (host) — market + club/car info ok ***\n");
  std::printf("    car='%s' day=%d time=%.0fs\n", cname ? cname : "?",
              game_logic_day(), game_logic_time());
  if (wait_enter) {
    std::printf("Press Enter to quit.\n");
    std::fflush(stdout);
    (void)std::getchar();
  }
  std::printf("boot exit ok\n");
  return 0;
}

int game_interactive_run(Jvm& jvm, const char* game_root,
                         const char* player_name, bool auto_new,
                         int32_t max_frames) {
  const char* name =
      (player_name && player_name[0]) ? player_name : "Player";

  std::printf("=== SLRR rewrite --game (FMV→Splash→MainMenu) ===\n");
  std::printf("root=%s player='%s' auto_new=%d max_frames=%d\n", game_root,
              name, auto_new ? 1 : 0, max_frames);

  if (!game_boot_warmup(jvm, game_root)) return 3;

  if (!render_d3d9_ready()) {
    if (!render_d3d9_open(1024, 768, "SLRR — MainMenu")) {
      std::printf("FAIL --game render open\n");
      return 6;
    }
  }
  render_d3d9_clear_quit();

  // IDA Engine_boot @ 0x0058C700 size 0x2D2 (722). Xref: WinMain@0x5514A8.
  // ResourceEngine_Init@0x535E70 → LoadPack("system.rpk") fail 0xFFFF →
  // Engine_InitState@0x427980 (JVM_bootstrap + Natives_Register*, BEFORE FMV)
  // → Gfx/Sound → FMV DirectShow@0x55C470 (NOT GfxEngine.openVideo@0x47C330)
  // Activision → Invictus → StreetLegal.avi (SL1 leftover; File_PathExists skip,
  // no error) → Engine_MainLoop@0x428960 (GameInit + JVM; splash/menu HERE)
  // → TextureLog_FlushWrite@0x53C940 AFTER the loop (texture.log, not JVM).
  // Host: warmup/FMV then harness splash/menu (stand-in for MainLoop).
  // FAIL fmv_n<2 is smoke for shipped Activision+Invictus, not a PE error.
  const int32_t fmv_cap = (auto_new || max_frames > 0) ? 8 : 0;
  const int32_t fmv_n = video_fmv_play_boot_intros(fmv_cap);
  std::printf("  boot FMV clips=%d (Activision+Invictus)\n", fmv_n);
  if (fmv_n < 2) {
    std::printf("FAIL --game boot FMV intros (need Activision+Invictus)\n");
    return 5;
  }

  // Stock GameLogic ctor end: Frontend.loadingScreen.hide() — do not show() a
  // SimpleLoadingDialog (SoftTimer/FlashText path crashes without full Dialog).
  frontend_loading_screen_hide();
  if (Jvm* j = jvm_active()) {
    InvObject* ls = frontend_loading_screen();
    std::vector<JvmValue> a = {JvmValue::make_obj(ls)};
    j->invoke("java.render.LoadingScreen", "hide", "()V", a, false);
  }

  InvObject* splash = game_logic_boot_splash();
  const char* splash_cn =
      splash && tree_host_class(splash) ? tree_host_class(splash) : "?";
  std::printf("  splash state='%s' entered=%d\n", splash_cn,
              splash ? tree_field_get_int(splash, "entered") : -1);
  if (!splash || !std::strstr(splash_cn, "SplashScreen")) {
    std::printf("FAIL --game SplashScreen\n");
    return 5;
  }

  InvObject* menu = game_logic_finish_splash();
  const int32_t splash_mask = tree_field_get_int(splash, "event_mask");
  constexpr int32_t kEventTime = 0x00000080;
  const int32_t splash_time_off = (splash_mask & kEventTime) == 0 ? 1 : 0;
  std::printf("  splash exit mask=0x%x time_off=%d\n", splash_mask,
              splash_time_off);
  InvObject* mmd = menu ? tree_field_get_obj(menu, "mmd") : nullptr;
  const int32_t mmd_shown = mmd ? tree_field_get_int(mmd, "shown") : 0;
  // Prefer TREE chrome (createHeader/createMenu/addItem leaves). Fallback host
  // chrome only if TREE did not materialize enough labels/buttons.
  if (mmd && !tree_field_get_int(mmd, "menu_chrome")) {
    InvObject* osd = tree_field_get_obj(mmd, "osd");
    const int32_t btn =
        osd ? tree_field_get_int(osd, "button_count") : 0;
    const int32_t txt = render_d3d9_osd_text_count();
    const int32_t osdn = render_d3d9_osd_count();
    std::printf("[script] MainMenu TREE chrome probe btn=%d txt=%d osd=%d\n",
                btn, txt, osdn);
    if (btn >= 12 && txt >= 12 && osdn >= 1) {
      tree_field_set_int(mmd, "menu_chrome", 1);
      std::printf("[script] MainMenuDialog chrome via TREE btn=%d txt=%d osd=%d\n",
                  btn, txt, osdn);
    } else {
      mainmenu_dialog_ensure_chrome(mmd);
    }
  }
  const int32_t menu_chrome = mmd ? tree_field_get_int(mmd, "menu_chrome") : 0;
  const int32_t menu_osd_n = render_d3d9_osd_count();
  const int32_t menu_txt_n = render_d3d9_osd_text_count();
  const int32_t acg_host =
      mmd ? tree_field_get_int(mmd, "addCustomGroups_via_host") : 0;
  const int32_t btn_n =
      mmd && tree_field_get_obj(mmd, "osd")
          ? tree_field_get_int(tree_field_get_obj(mmd, "osd"), "button_count")
          : 0;
  std::printf(
      "  splash→menu state='%s' mmd_shown=%d chrome=%d osd=%d txt=%d "
      "acg_host=%d btn=%d\n",
      menu && tree_host_class(menu) ? tree_host_class(menu) : "?", mmd_shown,
      menu_chrome, menu_osd_n, menu_txt_n, acg_host, btn_n);
  if (!menu || !std::strstr(tree_host_class(menu), "MainMenu") ||
      game_logic_actual_state() != menu || mmd_shown != 1) {
    std::printf("FAIL --game MainMenu\n");
    return 5;
  }
  // Phase 2.161: GENERALBG/header rects + ≥12 labels (or buttons).
  if (menu_chrome != 1 || menu_txt_n < 12 || menu_osd_n < 1) {
    std::printf("FAIL --game MainMenu chrome (chrome=%d osd=%d txt=%d)\n",
                menu_chrome, menu_osd_n, menu_txt_n);
    return 5;
  }
  if (!splash_time_off) {
    std::printf("FAIL --game SplashScreen.exit EVENT_TIME still armed\n");
    return 5;
  }

  // Stock Frontend.init starts HotkeyWatcher; we deferred past Splash TREE.
  frontend_start_hotkey_watcher();

  InvObject* player = game_logic_player();
  InvObject* ctrl = player ? tree_field_get_obj(player, "controller") : nullptr;
  InvObject* cursor = java_io_Input_cursor();
  if (ctrl) {
    tree_field_set_obj(cursor, "controller", ctrl);
    java_io_Input_mapAxis(ctrl, kAxisSelect, /*device*/ 0, kDikReturn, 0.f, 1.f,
                          0.f, 1.f);
    java_io_Input_mapAxis(ctrl, kAxisThrottle, /*device*/ 0, kDikUp, 0.f, 1.f,
                          0.f, 1.f);
    java_io_Input_mapAxis(ctrl, kAxisCancel, /*device*/ 0, kDikEscape, 0.f, 1.f,
                          0.f, 1.f);
  }
  java_io_MouseCursor_enable(cursor, 1);
  input_live_enable(true);

  InvObject* osd = mmd ? tree_field_get_obj(mmd, "osd") : nullptr;
  if (osd) tree_field_set_int(osd, "visible", 1);

  std::printf(
      "  live: ENTER=New Career  close window=quit  (ESC skips FMV only)\n");
  if (auto_new)
    std::printf("  smoke: auto_new + garage + Valocity drive/return\n");

  // Drain residual ESC from FMV skip before AXIS_CANCEL is live.
#ifdef _WIN32
  while ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
    render_d3d9_pump(0);
    Sleep(1);
    if (render_d3d9_quit_requested()) break;
  }
#endif

  float prev_select = 0.f;
  float prev_cancel = 1.f;  // require ESC edge (up→down), not sticky hold
  int32_t frames = 0;
  int32_t garage_ok = 0;
  bool fired_new = false;
  bool fired_freeride = false;
  int32_t hub_freeride_ok = 0;
  int32_t hub_quickrace_ok = 0;
  int32_t hub_demo_ok = 0;
  int32_t hub_back_ok = 0;
  int32_t hub_exit_ok = 0;
  int32_t hub_options_ok = 0;
  int32_t hub_credits_ok = 0;
  bool garage_phase = false;
  bool city_phase = false;
  InvObject* garage = nullptr;
  InvObject* city = nullptr;
  InvObject* starter_car = nullptr;
  InvObject* install_probe = nullptr;
  InvObject* garage_painter = nullptr;
  int32_t garage_smoke_step = 0;
  int32_t mode_mech = -1;
  int32_t mode_paint = -1;
  int32_t mode_none = -1;
  int32_t car_ok = 0;
  int32_t install_ok = 0;
  int32_t paint_ok = 0;
  int32_t painter_ux_ok = 0;
  int32_t mech_inv_ok = 0;
  int32_t mech_vis_ok = 0;
  int32_t mech_chrome_ok = 0;
  int32_t mech_install_ok = 0;
  int32_t mech_drop_ok = 0;
  int32_t mech_preview_ok = 0;
  int32_t mech_rotate_ok = 0;
  int32_t mech_swap_ok = 0;
  int32_t mech_hover_ok = 0;
  int32_t mech_click_ok = 0;
  int32_t mech_drag_ok = 0;
  int32_t mech_lclick_ok = 0;
  int32_t mech_pick_ok = 0;
  int32_t mech_rdrag_ok = 0;
  int32_t cursor_lock_ok = 0;
  int32_t cursor_ldrag_ok = 0;
  int32_t mech_tune_ok = 0;
  int32_t valo_entered = 0;
  float valo_dist = 0.f;
  float valo_spd = 0.f;
  int32_t valo_shape = 0;
  int32_t valo_denied = 0;
  int32_t valo_back = 0;
  int32_t valo_cam = 0;
  int32_t valo_nav_upd = 0;
  int32_t valo_hud = 0;
  float valo_cam_behind = 0.f;
  int32_t traffic_p_ok = 0;
  int32_t traffic_bh_ok = 0;
  int32_t traffic_halt_ok = 0;
  int32_t traffic_halt_path_ok = 0;
  int32_t traffic_rem_ok = 0;
  int32_t traffic_del_ok = 0;
  int32_t traffic_ped_ok = 0;
  int32_t traffic_ped_type_ok = 0;
  int32_t traffic_ped_rem_ok = 0;
  int32_t traffic_ped_dist_ok = 0;
  int32_t traffic_fog_ok = 0;
  int32_t traffic_light_ok = 0;
  int32_t traffic_flare_ok = 0;
  int32_t traffic_dup_ok = 0;
  int32_t traffic_create_ok = 0;
  int32_t traffic_chg_ok = 0;
  int32_t traffic_color_ok = 0;
  int32_t traffic_type_ok = 0;
  int32_t traffic_scale_ok = 0;
  int32_t traffic_plot_ok = 0;
  int32_t traffic_routepos_ok = 0;
  int32_t traffic_setmatrix_ok = 0;
  int32_t traffic_setpos_ok = 0;
  int32_t traffic_setstate_ok = 0;
  int32_t traffic_getpos_ok = 0;
  int32_t traffic_getvel_ok = 0;
  int32_t traffic_setparent_ok = 0;
  int32_t traffic_getspeedsq_ok = 0;
  int32_t traffic_isempty_ok = 0;
  int32_t traffic_isscripted_ok = 0;
  int32_t traffic_getscript_ok = 0;

  if (ctrl && osd) {
    java_io_Input_flushHotkeys();
    java_io_Input_checkHotkeys(ctrl, osd);
  }

  constexpr int32_t kCmdMechanic = 117;
  constexpr int32_t kCmdPaint = 118;
  constexpr int32_t kCmdEscape = 119;
  constexpr int32_t kCmdHitTheStreet = 109;
  constexpr int32_t kProbeSlot = 199;
  constexpr int32_t kPaintColor = static_cast<int32_t>(0xFFAABBCC);
  constexpr int32_t kDikM = 0x32;
  constexpr int32_t kDikP = 0x19;
  constexpr int32_t kDikT = 0x14;
  constexpr int32_t kDikU = 0x16;
  constexpr int32_t kDikN = 0x31;
  constexpr int32_t kDikI = 0x17;  // install probe
  constexpr int32_t kDikK = 0x25;  // paintPart
  constexpr int32_t kDikH = 0x23;  // Hit the Street
  constexpr int32_t kDikG = 0x22;  // return Garage trigger

  auto garage_do_install = [&]() {
    if (!starter_car) return;
    if (!install_probe) {
      install_probe = gameref_new();
      java_util_resource_ResourceRef_set(install_probe, 0x7E0300AA);
    }
    part_disable_slot(starter_car, kProbeSlot, 0);
    const bool ok = part_install(starter_car, kProbeSlot, install_probe, 1);
    install_ok =
        (ok && part_on_slot(starter_car, kProbeSlot) == install_probe) ? 1 : 0;
    std::printf("  garage install slot=%d ok=%d\n", kProbeSlot, install_ok);
  };

  auto garage_do_paint = [&]() {
    if (!starter_car || !garage) return;
    // Phase 2.157 — select stock red can then paintPart from cans.paintColor.
    constexpr int32_t kCanIdx = 5;
    constexpr int32_t kExpect =
        static_cast<int32_t>(0xFF000000u | 0xDC191Au);
    const bool sel = garage_painter_select_can(garage, kCanIdx);
    const bool painted = garage_paint_car(garage, 0);
    garage_painter = tree_field_get_obj(garage, "painter");
    InvObject* cans =
        garage_painter ? tree_field_get_obj(garage_painter, "paintCans")
                       : nullptr;
    InvObject* items = cans ? tree_field_get_obj(cans, "items") : nullptr;
    const int32_t n_cans = items ? tree_vector_size(items) : 0;
    paint_ok =
        (sel && painted && garage_painter && n_cans >= 12 &&
         tree_field_get_int(garage_painter, "paint_part_fills") >= 1 &&
         tree_field_get_int(starter_car, "part_texture") == kExpect &&
         tree_field_get_int(garage, "paint_car_count") >= 1 &&
         tree_field_get_int(garage, "last_can_id") == kCanIdx)
            ? 1
            : 0;
    std::printf("  garage paint ok=%d fills=%d cans=%d can=%d ux=%d\n",
                paint_ok,
                garage_painter
                    ? tree_field_get_int(garage_painter, "paint_part_fills")
                    : 0,
                n_cans, kCanIdx, painter_ux_ok);
  };

  auto garage_go_street = [&]() -> bool {
    if (!garage) return false;
    InvObject* next = garage_osd_command(garage, kCmdHitTheStreet);
    if (!next || !std::strstr(tree_host_class(next), "Valocity")) {
      const char* warn =
          tree_field_get_obj(garage, "last_warning")
              ? string_cstr(tree_field_get_obj(garage, "last_warning"))
              : "?";
      std::printf("  Hit the Street FAIL warn='%s'\n", warn ? warn : "?");
      return false;
    }
    city = next;
    city_phase = true;
    garage_phase = false;
    valo_entered = tree_field_get_int(city, "entered") ? 1 : 0;
    if (!valo_entered) valo_entered = (game_logic_actual_state() == city) ? 1 : 0;
    traffic_p_ok = tree_field_get_int(city, "traffic_p_smoke");
    traffic_bh_ok = tree_field_get_int(city, "traffic_bh_ok");
    traffic_halt_ok = tree_field_get_int(city, "halt_smoke");
    traffic_halt_path_ok = tree_field_get_int(city, "halt_path_smoke");
    traffic_rem_ok = tree_field_get_int(city, "rem_car_smoke");
    traffic_del_ok = tree_field_get_int(city, "del_traffic_smoke");
    traffic_ped_ok = tree_field_get_int(city, "ped_dens_smoke");
    traffic_ped_type_ok = tree_field_get_int(city, "ped_type_smoke");
    traffic_ped_rem_ok = tree_field_get_int(city, "ped_rem_smoke");
    traffic_ped_dist_ok = tree_field_get_int(city, "ped_dist_smoke");
    traffic_fog_ok = tree_field_get_int(city, "fog_smoke");
    traffic_light_ok = tree_field_get_int(city, "light_smoke");
    traffic_flare_ok = tree_field_get_int(city, "flare_smoke");
    traffic_dup_ok = tree_field_get_int(city, "dup_smoke");
    traffic_create_ok = tree_field_get_int(city, "create_smoke");
    traffic_chg_ok = tree_field_get_int(city, "chg_smoke");
    traffic_color_ok = tree_field_get_int(city, "color_smoke");
    traffic_type_ok = tree_field_get_int(city, "type_smoke");
    traffic_scale_ok = tree_field_get_int(city, "scale_smoke");
    traffic_plot_ok = tree_field_get_int(city, "plot_smoke");
    traffic_routepos_ok = tree_field_get_int(city, "routepos_smoke");
    traffic_setmatrix_ok = tree_field_get_int(city, "setmatrix_smoke");
    traffic_setpos_ok = tree_field_get_int(city, "setpos_smoke");
    traffic_setstate_ok = tree_field_get_int(city, "setstate_smoke");
    traffic_getpos_ok = tree_field_get_int(city, "getpos_smoke");
    traffic_getvel_ok = tree_field_get_int(city, "getvel_smoke");
    traffic_setparent_ok = tree_field_get_int(city, "setparent_smoke");
    traffic_getspeedsq_ok = tree_field_get_int(city, "getspeedsq_smoke");
    traffic_isempty_ok = tree_field_get_int(city, "isempty_smoke");
    traffic_isscripted_ok = tree_field_get_int(city, "isscripted_smoke");
    traffic_getscript_ok = tree_field_get_int(city, "getscript_smoke");
    std::printf(
        "  Valocity live: UP=throttle G=garage ESC=quit  entered=%d "
        "via_tree=%d map=0x%X\n",
        valo_entered, tree_field_get_int(city, "enter_via_tree"),
        tree_field_get_int(city, "map_id"));
    return true;
  };

  auto city_stop_car = [&]() {
    if (!starter_car) return;
    if (ctrl) {
      java_io_Controller_user_SetAxisForce(ctrl, kAxisThrottle, 0.f, 0.f);
      java_io_Controller_user_SetAxisForce(ctrl, kAxisBrake, 0.f, 0.f);
    }
    InvObject* body = tree_field_get_obj(starter_car, "chassis");
    if (!body) body = starter_car;
    if (physics_shape(body) != 0) physics_set_velocity(body, 0.f, 0.f, 0.f);
    tree_field_set_float(starter_car, "speed_sq", 0.f);
  };

  auto city_try_garage = [&](int32_t club) -> InvObject* {
    if (!city) return nullptr;
    city_stop_car();
    valocity_fire_garage_trigger(city, club, 1);
    return valocity_tick(city);
  };

  auto city_return_garage = [&]() -> bool {
    InvObject* back = city_try_garage(1);
    if (!back || !std::strstr(tree_host_class(back), "Garage")) {
      std::printf("  Valocity→Garage FAIL denied=%d state='%s'\n",
                  city ? tree_field_get_int(city, "garage_denied") : -1,
                  back && tree_host_class(back) ? tree_host_class(back) : "?");
      return false;
    }
    garage = back;
    garage_phase = true;
    city_phase = false;
    valo_back = 1;
    osd = tree_field_get_obj(garage, "osd");
    std::printf("  Valocity→Garage ok traffic=%d car_locked=%d\n",
                city ? tree_field_get_int(city, "traffic_count") : -1,
                starter_car ? tree_field_get_int(starter_car, "stopped") : -1);
    if (ctrl && osd) {
      java_io_Input_flushHotkeys();
      java_io_Input_checkHotkeys(ctrl, osd);
    }
    return true;
  };

  auto city_drive_smoke = [&]() {
    if (!city || !starter_car || !ctrl) return;
    const float x0 = tree_field_get_float(starter_car, "pos_x");
    const float z0 = tree_field_get_float(starter_car, "pos_z");
    java_io_Controller_user_SetAxisForce(ctrl, kAxisThrottle, 0.f, 1.f);
    for (int i = 0; i < 40; ++i) valocity_simulate(city, 0.05f);
    java_io_Controller_user_SetAxisForce(ctrl, kAxisThrottle, 0.f, 0.f);
    const float x1 = tree_field_get_float(starter_car, "pos_x");
    const float z1 = tree_field_get_float(starter_car, "pos_z");
    valo_dist = std::sqrt((x1 - x0) * (x1 - x0) + (z1 - z0) * (z1 - z0));
    valo_spd = tree_field_get_float(starter_car, "speed_sq");
    const float live_spd = java_game_Vehicle_getSpeedSquare(starter_car);
    if (live_spd > valo_spd) valo_spd = live_spd;
    valo_shape = physics_shape(starter_car);
    std::printf("  Valocity drive dist=%.1f spd=%.1f shape=%d\n", valo_dist,
                valo_spd, valo_shape);
  };

  auto city_cam_smoke = [&]() {
    if (!city || !starter_car) return;
    // Chase/HUD/nav already advanced by simulate; refresh cam once like boot.
    valocity_update_camera(city);
    void* vcam = valocity_camera_key();
    float ex = 0.f, ey = 0.f, ez = 0.f, ax = 0.f, ay = 0.f, az = 0.f;
    const bool got =
        render_d3d9_camera_get_lookat(vcam, &ex, &ey, &ez, &ax, &ay, &az);
    const float cpx = tree_field_get_float(starter_car, "pos_x");
    const float cpy = tree_field_get_float(starter_car, "pos_y");
    const float cpz = tree_field_get_float(starter_car, "pos_z");
    const float coy = tree_field_get_float(starter_car, "ori_y");
    const float cfx = std::sin(coy);
    const float cfz = std::cos(coy);
    valo_cam_behind = (ex - cpx) * cfx + (ez - cpz) * cfz;
    const float look_ahead = (ax - cpx) * cfx + (az - cpz) * cfz;
    InvObject* nav = tree_field_get_obj(city, "nav");
    valo_nav_upd = nav ? tree_field_get_int(nav, "update_count") : 0;
    const int32_t nvis = nav ? tree_field_get_int(nav, "visible") : 0;
    const char* speed_txt =
        render_d3d9_text_get_string(valocity_hud_speed_key());
    const float kph = valocity_speed_kph(starter_car);
    valo_hud =
        (speed_txt && std::strstr(speed_txt, "KPH") && kph > 5.f) ? 1 : 0;
    valo_cam =
        (got && render_d3d9_camera_active() == vcam && valo_cam_behind < -2.f &&
         ey > cpy + 1.f && look_ahead > 0.5f && valo_nav_upd >= 40 &&
         nvis == 1 && valo_hud == 1)
            ? 1
            : 0;
    std::printf(
        "  Valocity cam behind=%.1f eye_y=%.1f active=%d nav_upd=%d vis=%d "
        "hud=%d kph=%.0f\n",
        valo_cam_behind, ey, render_d3d9_camera_active() == vcam ? 1 : 0,
        valo_nav_upd, nvis, valo_hud, kph);
  };

  auto city_return_smoke = [&]() {
    // Higher-club garage must deny, then club-1 accepts.
    InvObject* denied = city_try_garage(2);
    valo_denied =
        (!denied && city && tree_field_get_int(city, "garage_denied") == 1) ? 1
                                                                           : 0;
    std::printf("  garage deny club2 ok=%d\n", valo_denied);
    city_return_garage();
  };

  while (!render_d3d9_quit_requested()) {
    render_d3d9_pump(0);
    input_live_poll();
    // Nested Dialog.display pumps its own OSD — skip parent tick.
    if (!dialog_modal_active()) {
      if (ctrl && osd) java_io_Input_checkHotkeys(ctrl, osd);
      // Phase 2.162: pointer hover/click for OSD buttons (MainMenu, dialogs…).
      if (osd) osd_tick_pointer(osd);
      if (mmd) mainmenu_credits_tick(mmd, 0.05f);
    }
    // PE Engine_tickTimers @ 0x00427160 — EVENT_TIME oneshot queue.
    java_lang_GameType_pollTimers();
    // Stock EXIT → changeActiveSection(null).
    if (!game_logic_actual_state()) break;

    float select = 0.f;
    if (ctrl) select = java_io_Controller_user_GetAxisVal(ctrl, kAxisSelect);
    const bool select_edge = (prev_select < 0.5f && select >= 0.5f);
    prev_select = select;

    if (city_phase && city) {
      valocity_simulate(city, 0.05f);

      const int32_t lk = java_io_Input_lastKey() & 0xFF;
      if (!auto_new && lk == kDikG) city_return_garage();

      if (auto_new && garage_smoke_step == 7) {
        city_drive_smoke();
        city_cam_smoke();
        garage_smoke_step = 8;
      } else if (auto_new && garage_smoke_step == 8) {
        city_return_smoke();
        garage_smoke_step = 9;
        break;
      }
    } else if (!garage_phase) {
      if (auto_new && !fired_new && frames == 8) {
#ifdef _WIN32
        keybd_event(VK_RETURN, 0, 0, 0);
#endif
        input_live_poll();
      }
      if (auto_new && !fired_new && frames == 9) {
#ifdef _WIN32
        keybd_event(VK_RETURN, 0, KEYEVENTF_KEYUP, 0);
#endif
        input_live_poll();
        if (ctrl) select = java_io_Controller_user_GetAxisVal(ctrl, kAxisSelect);
      }

      // Hub FREERIDE is exercised after career smoke (see post-garage), not
      // before CMD_NEW — loadDefaults mid-hub poisons Garage OSD TREE.

      const bool smoke_force = auto_new && !fired_new && frames == 10;
      if (!fired_new && (select_edge || smoke_force ||
                         (auto_new && frames == 9 && select >= 0.5f))) {
        InvObject* st = game_logic_actual_state();
        if (st && std::strstr(tree_host_class(st), "MainMenu")) {
          main_menu_cmd_new(name);
          fired_new = true;
        }
      }

      InvObject* st = game_logic_actual_state();
      if (st && std::strstr(tree_host_class(st), "Garage")) {
        garage_ok = 1;
        garage = st;
        garage_phase = true;
        starter_car = player_spawn_starter_car();
        car_ok =
            (starter_car && java_util_resource_ResourceRef_id(starter_car)) ? 1
                                                                            : 0;
        osd = tree_field_get_obj(garage, "osd");
        if (!osd && mmd) osd = tree_field_get_obj(mmd, "osd");
        std::printf(
            "  Garage live: M=mechanic P=paint T=test U=tune N=none "
            "I=install K=paintPart H=street ESC=quit  car=%d\n",
            car_ok);
        if (ctrl && osd) {
          java_io_Input_flushHotkeys();
          java_io_Input_checkHotkeys(ctrl, osd);
        }
      }
    } else if (garage) {
      // Phase 2.149: RMB EC_RDRAG orbit camera (locks panel hover/click).
      garage_tick_rdrag(garage);
      // Phase 2.144/2.145/2.150: Mechanic szereles (1) or tune (4).
      const int32_t gmode = tree_field_get_int(garage, "mode");
      if ((gmode == 1 || gmode == 4) && !tree_field_get_int(garage, "rdrag")) {
        InvObject* mech = tree_field_get_obj(garage, "mechanic");
        if (mech) {
          mechanic_tick_hover(mech);
          mechanic_tick_click(mech);
          mechanic_tick_preview(mech);
        }
      }
      const int32_t lk = java_io_Input_lastKey() & 0xFF;
      int32_t gcmd = 0;
      bool want_install = false;
      bool want_paint = false;
      bool want_street = false;
      // auto_new owns garage_smoke_step; live DIK must not steal mode_mech.
      if (!auto_new) {
        if (lk == kDikM)
          gcmd = kCmdMechanic;
        else if (lk == kDikP)
          gcmd = kCmdPaint;
        else if (lk == kDikT)
          gcmd = 123;  // CMD_TEST
        else if (lk == kDikU)
          gcmd = 124;  // CMD_TUNE
        else if (lk == kDikN)
          gcmd = kCmdEscape;
        else if (lk == kDikI)
          want_install = true;
        else if (lk == kDikK)
          want_paint = true;
        else if (lk == kDikH)
          want_street = true;
      }

      // Smoke: modes → install → paint → Hit the Street → drive.
      if (auto_new) {
        if (garage_smoke_step == 0 && frames >= 12) {
          gcmd = kCmdMechanic;
          garage_smoke_step = 1;
        } else if (garage_smoke_step == 1 && frames >= 14) {
          mode_mech = tree_field_get_int(garage, "mode");
          InvObject* mech = tree_field_get_obj(garage, "mechanic");
          if (!mech) mech = garage_ensure_mechanic(garage);
          InvObject* minv = mech ? tree_field_get_obj(mech, "inventory") : nullptr;
          mech_inv_ok = (minv && inventory_size(minv) >= 6) ? 1 : 0;
          const int32_t panels = visual_inventory_panel_count(minv);
          const int32_t attached = visual_inventory_attached_count(minv);
          const int32_t vis_upd =
              minv ? tree_field_get_int(minv, "visualsUpdated") : 0;
          const int32_t shown = minv ? tree_field_get_int(minv, "shown") : 0;
          InvObject* p0 =
              (minv && tree_field_get_obj(minv, "panels"))
                  ? tree_vector_element_at(tree_field_get_obj(minv, "panels"), 0)
                  : nullptr;
          const int32_t btn_ok =
              p0 && tree_field_get_obj(p0, "button") &&
              tree_field_get_obj(p0, "osd")
                  ? 1
                  : 0;
          mech_vis_ok = (panels >= 5 && attached >= 1 && vis_upd == 1 &&
                         shown == 1 && btn_ok == 1)
                            ? 1
                            : 0;

          // Phase 2.139 — InventoryPanel Camera + light preview.
          const int32_t preview = visual_inventory_preview_count(minv);
          InvObject* p0cam = p0 ? tree_field_get_obj(p0, "cam") : nullptr;
          const float p0_aov =
              p0cam ? render_d3d9_camera_half_aov(p0cam) : 0.f;
          mech_preview_ok =
              (preview >= 1 && attached >= 1 && preview == attached &&
               p0cam && p0_aov > 50.f &&
               p0 && tree_field_get_obj(p0, "light") &&
               tree_field_get_obj(p0, "osd") &&
               tree_field_get_obj(tree_field_get_obj(p0, "osd"), "vp"))
                  ? 1
                  : 0;
          std::printf("  mechanic preview ok=%d n=%d/%d aov=%.1f cam=%d "
                      "light=%d\n",
                      mech_preview_ok, preview, attached, p0_aov,
                      p0cam ? 1 : 0,
                      p0 && tree_field_get_obj(p0, "light") ? 1 : 0);

          // Phase 2.141 — focusHook rotates catalog yaw + cam lookat.
          float yaw0 = 0.f, pitch0 = 0.f, roll0 = 0.f;
          float ex0 = 0.f, ey0 = 0.f, ez0 = 0.f, ax0 = 0.f, ay0 = 0.f, az0 = 0.f;
          InvObject* ypr0 = p0 ? tree_field_get_obj(p0, "ypr") : nullptr;
          if (ypr0) ypr_get(ypr0, &yaw0, &pitch0, &roll0);
          if (p0cam)
            render_d3d9_camera_get_lookat(p0cam, &ex0, &ey0, &ez0, &ax0, &ay0,
                                          &az0);
          int32_t rot_n = 0;
          for (int i = 0; i < 10; ++i) {
            if (visual_inventory_focus_hook(minv, 0)) ++rot_n;
          }
          float yaw1 = yaw0, pitch1 = pitch0, roll1 = roll0;
          float ex1 = ex0, ey1 = ey0, ez1 = ez0;
          if (ypr0) ypr_get(ypr0, &yaw1, &pitch1, &roll1);
          if (p0cam)
            render_d3d9_camera_get_lookat(p0cam, &ex1, &ey1, &ez1, &ax0, &ay0,
                                          &az0);
          const int32_t ticks =
              p0 ? tree_field_get_int(p0, "focus_ticks") : 0;
          const float dyaw = yaw1 - yaw0;
          const float dpos =
              std::fabs(ex1 - ex0) + std::fabs(ez1 - ez0);
          mech_rotate_ok =
              (rot_n == 10 && ticks >= 10 && dyaw > 0.25f && dpos > 0.01f)
                  ? 1
                  : 0;
          std::printf("  mechanic rotate ok=%d ticks=%d dyaw=%.3f dpos=%.3f\n",
                      mech_rotate_ok, ticks, dyaw, dpos);

          // Phase 2.143 — panelSwap + actualPanel focus.
          InvObject* items0 = minv ? tree_field_get_obj(minv, "items") : nullptr;
          InvObject* before0 =
              items0 ? tree_vector_element_at(items0, 0) : nullptr;
          InvObject* before1 =
              items0 ? tree_vector_element_at(items0, 1) : nullptr;
          const bool swapped =
              minv ? visual_inventory_panel_swap_panels(minv, 0, 1) : false;
          InvObject* after0 =
              items0 ? tree_vector_element_at(items0, 0) : nullptr;
          InvObject* after1 =
              items0 ? tree_vector_element_at(items0, 1) : nullptr;
          mechanic_set_actual_panel(mech, 1);
          InvObject* ap_obj = mechanic_actual_panel(mech);
          const int32_t ap_idx =
              mech ? tree_field_get_int(mech, "actualPanel") : -1;
          const int32_t ticks1 =
              ap_obj ? tree_field_get_int(ap_obj, "focus_ticks") : 0;
          mechanic_tick_preview(mech);
          mechanic_tick_preview(mech);
          const int32_t ticks2 =
              ap_obj ? tree_field_get_int(ap_obj, "focus_ticks") : 0;
          InvObject* info = mech ? tree_field_get_obj(mech, "infoline") : nullptr;
          const char* info_txt = nullptr;
          if (info) {
            if (InvObject* t = tree_field_get_obj(info, "text"))
              info_txt = string_cstr(t);
          }
          mech_swap_ok =
              (swapped && before0 && before1 && after0 == before1 &&
               after1 == before0 &&
               (minv ? tree_field_get_int(minv, "swap_count") : 0) >= 1 &&
               ap_idx == 1 && ap_obj && ticks2 > ticks1 && info_txt &&
               info_txt[0])
                  ? 1
                  : 0;
          std::printf("  mechanic swap ok=%d swaps=%d ap=%d ticks=%d->%d "
                      "info='%s'\n",
                      mech_swap_ok,
                      minv ? tree_field_get_int(minv, "swap_count") : 0, ap_idx,
                      ticks1, ticks2, info_txt ? info_txt : "");

          // Phase 2.144 — EC_HOVER hit-test → actualPanel (norm [0,1]).
          auto panel_center = [&](int32_t pi, float* ox, float* oy) {
            *ox = *oy = -1.f;
            if (!minv) return;
            InvObject* ps = tree_field_get_obj(minv, "panels");
            InvObject* p =
                (ps && pi < tree_vector_size(ps))
                    ? tree_vector_element_at(ps, pi)
                    : nullptr;
            if (!p) return;
            *ox = tree_field_get_float(p, "x") +
                  tree_field_get_float(p, "width") * 0.5f;
            *oy = tree_field_get_float(p, "y") +
                  tree_field_get_float(p, "height") * 0.5f;
          };
          float hx0 = 0.f, hy0 = 0.f, hx1 = 0.f, hy1 = 0.f;
          panel_center(0, &hx0, &hy0);
          panel_center(1, &hx1, &hy1);
          mechanic_clear_actual_panel(mech);
          const bool hit0 = mechanic_hover_at(mech, hx0, hy0);
          const int32_t hover0 =
              mech ? tree_field_get_int(mech, "actualPanel") : -9;
          InvObject* hover0_obj = mechanic_actual_panel(mech);
          const bool miss = !mechanic_hover_at(mech, 0.99f, 0.99f);
          const int32_t hover_miss =
              mech ? tree_field_get_int(mech, "actualPanel") : -9;
          const bool hit1 = mechanic_hover_at(mech, hx1, hy1);
          const int32_t hover1 =
              mech ? tree_field_get_int(mech, "actualPanel") : -9;
          const int32_t at0 = visual_inventory_panel_at(minv, hx0, hy0);
          const int32_t at1 = visual_inventory_panel_at(minv, hx1, hy1);
          mech_hover_ok =
              (hit0 && hover0 == 0 && hover0_obj && miss && hover_miss < 0 &&
               hit1 && hover1 == 1 && at0 == 0 && at1 == 1 &&
               hx0 > 0.f && hy0 > 0.f)
                  ? 1
                  : 0;
          std::printf("  mechanic hover ok=%d at=%d/%d ap=%d->miss->%d "
                      "xy0=%.2f,%.2f\n",
                      mech_hover_ok, at0, at1, hover0, hover1, hx0, hy0);

          // Phase 2.146 — panel→panel drag (dropGadget → panelSwap).
          InvObject* items_d = minv ? tree_field_get_obj(minv, "items") : nullptr;
          InvObject* d_before0 =
              items_d ? tree_vector_element_at(items_d, 0) : nullptr;
          InvObject* d_before1 =
              items_d ? tree_vector_element_at(items_d, 1) : nullptr;
          const int32_t swap_before =
              minv ? tree_field_get_int(minv, "swap_count") : 0;
          const bool dragged =
              mech ? mechanic_drag_panel_to(mech, hx0, hy0, hx1, hy1) : false;
          InvObject* d_after0 =
              items_d ? tree_vector_element_at(items_d, 0) : nullptr;
          InvObject* d_after1 =
              items_d ? tree_vector_element_at(items_d, 1) : nullptr;
          const int32_t drag_from =
              mech ? tree_field_get_int(mech, "last_drag_from") : -9;
          const int32_t drag_to =
              mech ? tree_field_get_int(mech, "last_drag_to") : -9;
          mech_drag_ok =
              (dragged && d_before0 && d_before1 && d_after0 == d_before1 &&
               d_after1 == d_before0 &&
               (minv ? tree_field_get_int(minv, "swap_count") : 0) >
                   swap_before &&
               drag_from == 0 && drag_to == 1 &&
               (mech ? tree_field_get_int(mech, "drag_swap_count") : 0) >= 1)
                  ? 1
                  : 0;
          std::printf("  mechanic drag ok=%d from=%d to=%d swaps=%d\n",
                      mech_drag_ok, drag_from, drag_to,
                      minv ? tree_field_get_int(minv, "swap_count") : 0);

          // Phase 2.136 — scroll + body filter chrome.
          constexpr int32_t kCmdScrollUp = 0;
          constexpr int32_t kCmdScrollDown = 1;
          constexpr int32_t kCmdFilterBody = 120;
          mechanic_ensure_chrome(mech);
          const int32_t chrome_n = mechanic_chrome_button_count(mech);
          const int32_t size0 = minv ? inventory_size(minv) : 0;
          mechanic_osd_command(mech, kCmdScrollDown);
          const int32_t line1 = minv ? tree_field_get_int(minv, "cline") : -1;
          const int32_t inv_line1 =
              mech ? tree_field_get_int(mech, "inv_line") : -1;
          mechanic_osd_command(mech, kCmdScrollUp);
          const int32_t line0 = minv ? tree_field_get_int(minv, "cline") : -1;
          mechanic_osd_command(mech, kCmdFilterBody);
          const int32_t fbody =
              mech ? tree_field_get_int(mech, "filterBody") : -1;
          const int32_t size_filt = minv ? inventory_size(minv) : -1;
          mechanic_osd_command(mech, kCmdFilterBody);
          const int32_t size_back = minv ? inventory_size(minv) : -1;
          mech_chrome_ok =
              (chrome_n >= 5 && line1 == 1 && inv_line1 == 2 && line0 == 0 &&
               fbody == 2 && size_filt == 1 && size_back == size0)
                  ? 1
                  : 0;
          std::printf("  mechanic inv size=%d panels=%d attached=%d "
                      "vis=%d shown=%d btn=%d\n",
                      minv ? inventory_size(minv) : -1, panels, attached,
                      vis_upd, shown, btn_ok);
          std::printf("  mechanic chrome ok=%d btns=%d scroll=%d/%d "
                      "filt=%d/%d/%d\n",
                      mech_chrome_ok, chrome_n, line1, line0, fbody, size_filt,
                      size_back);

          // Phase 2.137/2.140/2.145 — hover+click installs via CFG slot match.
          float cx0 = 0.f, cy0 = 0.f;
          {
            InvObject* ps = minv ? tree_field_get_obj(minv, "panels") : nullptr;
            InvObject* p0 =
                (ps && tree_vector_size(ps) > 0) ? tree_vector_element_at(ps, 0)
                                                : nullptr;
            if (p0) {
              cx0 = tree_field_get_float(p0, "x") +
                    tree_field_get_float(p0, "width") * 0.5f;
              cy0 = tree_field_get_float(p0, "y") +
                    tree_field_get_float(p0, "height") * 0.5f;
            }
          }
          const int32_t size_pre = minv ? inventory_size(minv) : 0;
          InvObject* car = nullptr;
          if (mech) {
            InvObject* pl = tree_field_get_obj(mech, "player");
            car = pl ? tree_field_get_obj(pl, "car") : nullptr;
          }
          if (!car) car = starter_car;
          const bool hovered = mech ? mechanic_hover_at(mech, cx0, cy0) : false;
          const bool clicked = mech ? mechanic_click_actual(mech) : false;
          const int32_t click_panel =
              mech ? tree_field_get_int(mech, "last_click_panel") : -9;
          const int32_t last_click =
              minv ? tree_field_get_int(minv, "last_panel_click") : -9;
          const int32_t click_n =
              mech ? tree_field_get_int(mech, "click_count") : 0;
          mech_click_ok =
              (hovered && clicked && click_panel == 0 && last_click == 0 &&
               click_n >= 1)
                  ? 1
                  : 0;
          std::printf("  mechanic click ok=%d hover=%d panel=%d clicks=%d\n",
                      mech_click_ok, hovered ? 1 : 0, click_panel, click_n);
          const int32_t size_post = minv ? inventory_size(minv) : -1;
          const int32_t inst_slot =
              minv ? tree_field_get_int(minv, "last_install_slot") : 0;
          const int32_t via_att =
              minv ? tree_field_get_int(minv, "last_install_via_attach") : 0;
          InvObject* on_slot =
              (car && inst_slot > 0) ? part_on_slot(car, inst_slot) : nullptr;
          const int32_t cfg_n =
              car ? tree_field_get_int(car, "cfg_slot_count") : 0;
          const int32_t att_n =
              car ? tree_field_get_int(car, "cfg_attach_count") : 0;
          mech_install_ok =
              (mech_click_ok == 1 && size_pre >= 1 && size_post == size_pre - 1 &&
               inst_slot > 0 && inst_slot != 198 && inst_slot != 199 &&
               on_slot != nullptr && cfg_n > 0 && att_n > 0 && via_att == 1 &&
               (minv ? tree_field_get_int(minv, "install_count") : 0) >= 1)
                  ? 1
                  : 0;
          std::printf("  mechanic install ok=%d size=%d->%d slot=%d "
                      "cfg=%d att=%d via=%d clicks=%d\n",
                      mech_install_ok, size_pre, size_post, inst_slot, cfg_n,
                      att_n, via_att,
                      minv ? tree_field_get_int(minv, "install_count") : 0);

          // Phase 2.148 — pick car part under cursor → look_part (EC_HOVER).
          InvObject* installed = on_slot;
          float pnx = 0.5f, pny = 0.35f;
          float pwx = 0.f, pwy = 0.f, pwz = 0.f;
          if (installed) {
            if (InvObject* pp = java_util_resource_GameRef_getPos(installed))
              vec3_get(pp, &pwx, &pwy, &pwz);
            float spx = 0.f, spy = 0.f;
            if (render_d3d9_project(pwx, pwy, pwz, &spx, &spy)) {
              pnx = (spx + 1.f) * 0.5f;
              pny = (1.f - spy) * 0.5f;
            }
          }
          mechanic_clear_actual_panel(mech);
          const bool picked =
              (mech && installed) ? mechanic_hover_car_at(mech, pnx, pny)
                                  : false;
          InvObject* look = mech ? tree_field_get_obj(mech, "look_part") : nullptr;
          const int32_t pick_n =
              mech ? tree_field_get_int(mech, "pick_count") : 0;
          const int32_t over =
              mech ? tree_field_get_int(mech, "over_vehicle") : 0;
          mech_pick_ok =
              (picked && look == installed && pick_n >= 1 && over == 1 &&
               (car ? tree_field_get_obj(car, "last_installed_part") : nullptr) ==
                   installed)
                  ? 1
                  : 0;
          std::printf("  mechanic pick ok=%d look=%d via=%d dist=%.1f "
                      "xy=%.2f,%.2f w=(%.0f,%.0f,%.0f)\n",
                      mech_pick_ok, look == installed ? 1 : 0,
                      mech ? tree_field_get_int(mech, "pick_via_screen") : -1,
                      mech ? tree_field_get_float(mech, "pick_dist") : -1.f, pnx,
                      pny, pwx, pwy, pwz);

          // Phase 2.149 — EC_RDRAG orbit garage camera.
          InvObject* gcam = garage_ensure_camera(garage);
          float rex0 = 0.f, rey0 = 0.f, rez0 = 0.f, rax0 = 0.f, ray0 = 0.f,
                raz0 = 0.f;
          const bool rgot0 =
              gcam &&
              render_d3d9_camera_get_lookat(gcam, &rex0, &rey0, &rez0, &rax0,
                                            &ray0, &raz0);
          const float ryaw0 =
              garage ? tree_field_get_float(garage, "cam_yaw") : 0.f;
          garage_rdrag_begin(garage);
          const bool orb1 = garage_rdrag_orbit(garage, 0.4f, 0.1f);
          const bool orb2 = garage_rdrag_orbit(garage, 0.3f, -0.05f);
          float rex1 = rex0, rey1 = rey0, rez1 = rez0;
          render_d3d9_camera_get_lookat(gcam, &rex1, &rey1, &rez1, &rax0, &ray0,
                                        &raz0);
          const float ryaw1 =
              garage ? tree_field_get_float(garage, "cam_yaw") : 0.f;
          const int32_t rdrag_on =
              garage ? tree_field_get_int(garage, "rdrag") : 0;
          garage_rdrag_end(garage);
          const int32_t rdrag_off =
              garage ? tree_field_get_int(garage, "rdrag") : 1;
          const float rdpos = std::fabs(rex1 - rex0) + std::fabs(rey1 - rey0) +
                              std::fabs(rez1 - rez0);
          const float rdyaw = ryaw1 - ryaw0;
          mech_rdrag_ok =
              (rgot0 && gcam && orb1 && orb2 && rdrag_on == 1 && rdrag_off == 0 &&
               rdyaw > 0.5f && rdpos > 0.2f &&
               (garage ? tree_field_get_int(garage, "rdrag_orbit_count") : 0) >=
                   2 &&
               (garage ? tree_field_get_int(garage, "rdrag_begin_count") : 0) >=
                   1 &&
               mech && tree_field_get_obj(mech, "camera") == gcam)
                  ? 1
                  : 0;
          std::printf("  mechanic rdrag ok=%d dyaw=%.2f dpos=%.2f orbits=%d "
                      "cam=%d\n",
                      mech_rdrag_ok, rdyaw, rdpos,
                      garage ? tree_field_get_int(garage, "rdrag_orbit_count")
                             : 0,
                      gcam ? 1 : 0);

          // PE Cursor_handleCommand lock/unlock @ 0x00462320 / 0x0046235A
          // → Engine_SysCursorLock ClipCursor 1px, then Unlock ClipCursor(NULL).
          {
            constexpr int32_t kEventCommand = 0x10;
            InvObject* icur = java_io_Input_cursor();
            InvObject* cgr =
                icur ? tree_field_get_obj(icur, "cursor") : nullptr;
            if (!cgr) cgr = gameref_new();
            java_util_resource_GameRef_queueEvent(cgr, nullptr, kEventCommand,
                                                  string_new("lock"));
            const int32_t lock_on = tree_field_get_int(cgr, "cursor_locked");
            const bool pin = input_syscursor_locked();
            java_util_resource_GameRef_queueEvent(cgr, nullptr, kEventCommand,
                                                  string_new("unlock"));
            const int32_t lock_off = tree_field_get_int(cgr, "cursor_locked");
            cursor_lock_ok =
                (lock_on == 1 && pin && lock_off == 0 &&
                 !input_syscursor_locked())
                    ? 1
                    : 0;
            std::printf("  cursor lock ok=%d on=%d pin=%d off=%d\n",
                        cursor_lock_ok, lock_on, pin ? 1 : 0, lock_off);
          }

          // PE Cursor_tick EC_LDRAGBEGIN=9 / END=10 (skip LCLICK).
          {
            constexpr int32_t kEventCursor = 0x00010000;
            InvObject* icur = java_io_Input_cursor();
            InvObject* cgr =
                icur ? tree_field_get_obj(icur, "cursor") : nullptr;
            if (!cgr && icur) {
              cgr = gameref_new();
              tree_field_set_obj(icur, "cursor", cgr);
            }
            InvObject* gt = tree_host_new("java.lang.GameType");
            if (cgr)
              java_lang_GameType_addNotification(gt, cgr, kEventCursor, 0,
                                                 nullptr);
            input_syscursor_set_buttons(0u);
            java_io_MouseCursor_tickSysCursor();
            const int32_t n0 = tree_field_get_int(gt, "cursor_event_count");
            input_syscursor_set_ndc(0.0f, 0.0f);
            input_syscursor_set_buttons(1u);
            java_io_MouseCursor_tickSysCursor();
            input_syscursor_set_ndc(0.25f, 0.15f);
            java_io_MouseCursor_tickSysCursor();
            input_syscursor_set_buttons(0u);
            java_io_MouseCursor_tickSysCursor();
            const int32_t n1 = tree_field_get_int(gt, "cursor_event_count");
            InvObject* lp = tree_field_get_obj(gt, "last_cursor_param");
            const char* lps = lp ? string_cstr(lp) : "";
            int32_t last_ec = 0;
            if (lps && lps[0]) std::sscanf(lps, "%d", &last_ec);
            cursor_ldrag_ok =
                ((n1 - n0) >= 4 && (last_ec == 10 || last_ec == 11)) ? 1 : 0;
            std::printf("  cursor ldrag ok=%d dn=%d last_ec=%d\n",
                        cursor_ldrag_ok, n1 - n0, last_ec);
          }

          // Phase 2.150/2.151 — MODE_TUNE + TUNE PART dialog (sliders).
          constexpr int32_t kCmdTune = 124;
          constexpr int32_t kCmdMechanic = 117;
          garage_osd_command(garage, kCmdTune);
          const int32_t gmode_t =
              garage ? tree_field_get_int(garage, "mode") : -1;
          const int32_t mmode_t =
              mech ? tree_field_get_int(mech, "mode") : -1;
          // Flap: stock only for !isTuneable — use Bumper (Block is always on).
          InvObject* flap_tgt =
              tree_host_new("java.game.parts.bodypart.Bumper");
          const bool flap_ok =
              (mech && flap_tgt && !part_is_tuneable(flap_tgt))
                  ? mechanic_lclick_part(mech, flap_tgt)
                  : false;
          const int32_t flap_n =
              flap_tgt ? tree_field_get_int(flap_tgt, "flap_toggle_count")
                       : 0;
          const int32_t flap_st =
              flap_tgt ? tree_field_get_int(flap_tgt, "flap_state") : -1;
          if (installed) {
            // Prefer Block menu: installed CFG engine, or force Block fields.
            if (!std::strstr(tree_host_class(installed)
                                 ? tree_host_class(installed)
                                 : "",
                             "Block"))
              tree_field_set_int(installed, "tuneable", 1);
            tree_field_set_int(installed, "tune_kind", 1);  // kTuneBlock
            tree_field_set_float(installed, "rpm_idle", 900.f);
            tree_field_set_float(installed, "RPM_limit", 7200.f);
          }
          InvObject* tdlg =
              (mech && installed) ? mechanic_open_tune_dialog(mech, installed)
                                  : nullptr;
          InvObject* tmenu =
              tdlg ? tree_field_get_obj(tdlg, "tune_menu") : nullptr;
          InvObject* tosd = tdlg ? tree_field_get_obj(tdlg, "osd") : nullptr;
          const int32_t slider_n =
              tmenu ? tree_field_get_int(tmenu, "item_count") : 0;
          const int32_t btn_n =
              tosd ? tree_field_get_int(tosd, "button_count") : 0;
          const char* ttitle = nullptr;
          if (tdlg) {
            if (InvObject* t = tree_field_get_obj(tdlg, "title"))
              ttitle = string_cstr(t);
          }
          InvObject* title_txt =
              tosd ? tree_field_get_obj(tosd, "title_text") : nullptr;
          const int32_t wide_ok =
              tdlg ? tree_field_get_int(tdlg, "wide_applied") : 0;
          const int32_t hk_ok =
              tdlg ? tree_field_get_int(tdlg, "cancel_hotkey") : 0;
          const float vaspect =
              tosd ? tree_field_get_float(tosd, "vpAspect") : 0.f;
          const bool slid1 =
              mech ? mechanic_tune_set_slider(mech, 1, 1100.f) : false;
          const bool slid2 =
              mech ? mechanic_tune_set_slider(mech, 2, 8500.f) : false;
          const float idle_live =
              installed ? tree_field_get_float(installed, "rpm_idle") : 0.f;
          const float red_live =
              installed ? tree_field_get_float(installed, "RPM_limit") : 0.f;
          const bool tune_ok =
              (mech && installed) ? mechanic_tune_part(mech, installed, 0)
                                  : false;
          const int32_t tune_n =
              mech ? tree_field_get_int(mech, "tune_count") : 0;
          const int32_t tune_choice =
              installed ? tree_field_get_int(installed, "tune_session_choice")
                        : -9;
          const int32_t dlg_n =
              mech ? tree_field_get_int(mech, "tune_dialog_count") : 0;
          // Cancel restores backups.
          if (installed) {
            tree_field_set_float(installed, "rpm_idle", 1100.f);
            tree_field_set_float(installed, "RPM_limit", 8500.f);
            tree_field_set_float(installed, "old_rpm_idle", 1100.f);
            tree_field_set_float(installed, "old_RPM_limit", 8500.f);
          }
          InvObject* tdlg2 =
              (mech && installed) ? mechanic_open_tune_dialog(mech, installed)
                                  : nullptr;
          mechanic_tune_set_slider(mech, 1, 500.f);
          const bool cancel_ok =
              (mech && installed) ? mechanic_tune_part(mech, installed, 1)
                                  : false;
          const float idle_cancel =
              installed ? tree_field_get_float(installed, "rpm_idle") : -1.f;
          garage_osd_command(garage, kCmdMechanic);
          const int32_t mmode_s =
              mech ? tree_field_get_int(mech, "mode") : -1;
          mech_tune_ok =
              (gmode_t == 4 && mmode_t == 1 && flap_ok && flap_n >= 1 &&
               flap_st == 1 && tdlg && ttitle && std::strstr(ttitle, "TUNE") &&
               slider_n >= 2 && btn_n >= 2 && slid1 && slid2 &&
               idle_live > 1099.f && red_live > 8499.f && tune_ok &&
               tune_n >= 1 && tune_choice == 0 && dlg_n >= 1 && cancel_ok &&
               idle_cancel > 1099.f && tdlg2 && mmode_s == 0 &&
               (garage ? tree_field_get_int(garage, "mode") : -1) == 1 &&
               title_txt && wide_ok == 1 && hk_ok == 1 && vaspect > 1.9f)
                  ? 1
                  : 0;
          std::printf("  mechanic tune ok=%d gmode=%d/%d dlg='%s' sliders=%d "
                      "btns=%d title=%d wide=%d hk=%d idle=%.0f cancel=%.0f "
                      "flap=%d\n",
                      mech_tune_ok, gmode_t,
                      garage ? tree_field_get_int(garage, "mode") : -1,
                      ttitle ? ttitle : "", slider_n, btn_n, title_txt ? 1 : 0,
                      wide_ok, hk_ok, idle_live, idle_cancel, flap_ok ? 1 : 0);

          // Phase 2.153 — subclass buildTuningMenu (Tyre / Chassis / Camshaft).
          int32_t subclass_ok = 0;
          if (mech) {
            InvObject* tyre = tree_host_new(
                "java.game.parts.rgearpart.reciprocatingrgearpart.Tyre");
            tree_field_set_float(tyre, "inflation", 2.0f);
            tree_field_set_float(tyre, "optimal_inflation", 2.5f);
            InvObject* tdlg_t = mechanic_open_tune_dialog(mech, tyre);
            InvObject* tmenu_t =
                tdlg_t ? tree_field_get_obj(tdlg_t, "tune_menu") : nullptr;
            const int32_t t_items =
                tmenu_t ? tree_field_get_int(tmenu_t, "item_count") : 0;
            const int32_t t_kind = tree_field_get_int(tyre, "tune_kind");
            const bool t_slid = mechanic_tune_set_slider(mech, 1, 2.2f);
            const float infl_live = tree_field_get_float(tyre, "inflation");
            mechanic_tune_part(mech, tyre, 1);
            const float infl_cancel = tree_field_get_float(tyre, "inflation");

            InvObject* chas =
                tree_host_new("java.game.parts.bodypart.Chassis");
            tree_field_set_int(chas, "brake_balance_can_be_set", 1);
            tree_field_set_float(chas, "brake_balance", 0.55f);
            InvObject* tdlg_c = mechanic_open_tune_dialog(mech, chas);
            InvObject* tmenu_c =
                tdlg_c ? tree_field_get_obj(tdlg_c, "tune_menu") : nullptr;
            const int32_t c_items =
                tmenu_c ? tree_field_get_int(tmenu_c, "item_count") : 0;
            const int32_t c_kind = tree_field_get_int(chas, "tune_kind");
            const bool c_slid = mechanic_tune_set_slider(mech, 1, -0.7f);
            const float bal_live = tree_field_get_float(chas, "brake_balance");
            mechanic_tune_part(mech, chas, 0);

            InvObject* cam = tree_host_new(
                "java.game.parts.enginepart.slidingenginepart."
                "reciprocatingenginepart.Camshaft");
            tree_field_set_float(cam, "advance", 12.f);
            tree_field_set_float(cam, "default_advance", 5.f);
            tree_field_set_float(cam, "advance_negative_peak", -5.f);
            tree_field_set_float(cam, "advance_positive_peak", 40.f);
            tree_field_set_float(cam, "advance_minimum_step", 0.5f);
            InvObject* tdlg_m = mechanic_open_tune_dialog(mech, cam);
            InvObject* tmenu_m =
                tdlg_m ? tree_field_get_obj(tdlg_m, "tune_menu") : nullptr;
            const int32_t m_items =
                tmenu_m ? tree_field_get_int(tmenu_m, "item_count") : 0;
            const int32_t m_kind = tree_field_get_int(cam, "tune_kind");
            mechanic_tune_set_slider(mech, 1, 20.f);
            const bool reset_ok = mechanic_tune_menu_command(mech, 0);
            const float adv_rst = tree_field_get_float(cam, "advance");
            mechanic_tune_part(mech, cam, 1);

            // Phase 2.154 — Transmission gears/end/LSD/drive.
            InvObject* tr = tree_host_new(
                "java.game.parts.enginepart.slidingenginepart."
                "reciprocatingenginepart.Transmission");
            tree_field_set_int(tr, "gears", 5);
            tree_field_set_int(tr, "adjustable_gears", 7);  // fwd|R|end
            tree_field_set_float(tr, "ratio_1", 3.5f);
            tree_field_set_float(tr, "ratio_2", 2.1f);
            tree_field_set_float(tr, "ratio_3", 1.5f);
            tree_field_set_float(tr, "ratio_4", 1.1f);
            tree_field_set_float(tr, "ratio_5", 0.9f);
            tree_field_set_float(tr, "ratio_7", 3.2f);
            tree_field_set_float(tr, "end_ratio", 3.9f);
            tree_field_set_float(tr, "diff_lock", 0.3f);
            tree_field_set_float(tr, "diff_lock_min", 0.f);
            tree_field_set_float(tr, "diff_lock_max", 1.f);
            tree_field_set_float(tr, "drive_front", 0.5f);
            tree_field_set_float(tr, "drive_front_min", 0.f);
            tree_field_set_float(tr, "drive_front_max", 1.f);
            InvObject* tdlg_tr = mechanic_open_tune_dialog(mech, tr);
            InvObject* tmenu_tr =
                tdlg_tr ? tree_field_get_obj(tdlg_tr, "tune_menu") : nullptr;
            const int32_t tr_items =
                tmenu_tr ? tree_field_get_int(tmenu_tr, "item_count") : 0;
            const int32_t tr_kind = tree_field_get_int(tr, "tune_kind");
            // During session forward ratios are negated.
            const float r1_sess = tree_field_get_float(tr, "ratio_1");
            const bool tr_g1 = mechanic_tune_set_slider(mech, 1, -4.0f);
            const bool tr_end = mechanic_tune_set_slider(mech, 8, 4.5f);
            const bool tr_lsd = mechanic_tune_set_slider(mech, 9, 0.8f);
            const bool tr_drv = mechanic_tune_set_slider(mech, 10, -0.6f);
            const float end_live = tree_field_get_float(tr, "end_ratio");
            const float lsd_live = tree_field_get_float(tr, "diff_lock");
            const float drv_live = tree_field_get_float(tr, "drive_front");
            mechanic_tune_part(mech, tr, 0);  // OK → flip gears positive
            const float r1_ok = tree_field_get_float(tr, "ratio_1");
            // Cancel restore
            InvObject* tdlg_tr2 = mechanic_open_tune_dialog(mech, tr);
            mechanic_tune_set_slider(mech, 1, -2.0f);
            mechanic_tune_part(mech, tr, 1);
            const float r1_cancel = tree_field_get_float(tr, "ratio_1");

            // Phase 2.155 — FuelInjector + NOS.
            InvObject* inj = tree_host_new(
                "java.game.parts.enginepart.airfueldeliverysystem."
                "FuelInjectorSystem");
            tree_field_set_float(inj, "mixture_ratio", 14.7f);
            InvObject* tdlg_i = mechanic_open_tune_dialog(mech, inj);
            InvObject* tmenu_i =
                tdlg_i ? tree_field_get_obj(tdlg_i, "tune_menu") : nullptr;
            const int32_t i_items =
                tmenu_i ? tree_field_get_int(tmenu_i, "item_count") : 0;
            const int32_t i_kind = tree_field_get_int(inj, "tune_kind");
            const bool i_slid = mechanic_tune_set_slider(mech, 1, 12.5f);
            const float mix_live = tree_field_get_float(inj, "mixture_ratio");
            mechanic_tune_part(mech, inj, 1);
            const float mix_cancel = tree_field_get_float(inj, "mixture_ratio");

            InvObject* nos = tree_host_new(
                "java.game.parts.enginepart.airfueldeliverysystem."
                "NOSInjectorSystem");
            tree_field_set_float(nos, "nitro_consumption", 0.2f);
            tree_field_set_float(nos, "minconsumption", 0.05f);
            tree_field_set_float(nos, "maxconsumption", 0.8f);
            InvObject* tdlg_n = mechanic_open_tune_dialog(mech, nos);
            InvObject* tmenu_n =
                tdlg_n ? tree_field_get_obj(tdlg_n, "tune_menu") : nullptr;
            const int32_t n_items =
                tmenu_n ? tree_field_get_int(tmenu_n, "item_count") : 0;
            const int32_t n_kind = tree_field_get_int(nos, "tune_kind");
            const bool n_slid = mechanic_tune_set_slider(mech, 1, 0.55f);
            const float nos_live =
                tree_field_get_float(nos, "nitro_consumption");
            mechanic_tune_part(mech, nos, 0);
            const float nos_ok = tree_field_get_float(nos, "nitro_consumption");

            subclass_ok =
                (part_is_tuneable(tyre) && tdlg_t && t_kind == 2 &&
                 t_items == 1 && t_slid && infl_live > 2.19f &&
                 infl_cancel > 1.99f && infl_cancel < 2.01f &&
                 part_is_tuneable(chas) && tdlg_c && c_kind == 4 &&
                 c_items == 1 && c_slid && bal_live > 0.69f &&
                 part_is_tuneable(cam) && tdlg_m && m_kind == 5 &&
                 m_items >= 2 && reset_ok && adv_rst > 4.9f && adv_rst < 5.1f &&
                 part_is_tuneable(tr) && tdlg_tr && tdlg_tr2 && tr_kind == 7 &&
                 tr_items >= 8 && r1_sess < -3.4f && tr_g1 && tr_end &&
                 tr_lsd && tr_drv && end_live > 4.4f && lsd_live > 0.79f &&
                 drv_live > 0.59f && r1_ok > 3.9f && r1_ok < 4.1f &&
                 r1_cancel > 3.9f && r1_cancel < 4.1f &&
                 part_is_tuneable(inj) && tdlg_i && i_kind == 8 &&
                 i_items == 1 && i_slid && mix_live > 12.4f &&
                 mix_cancel > 14.6f && mix_cancel < 14.8f &&
                 part_is_tuneable(nos) && tdlg_n && n_kind == 9 &&
                 n_items == 1 && n_slid && nos_live > 0.54f &&
                 nos_ok > 0.54f)
                    ? 1
                    : 0;
            if (mech_tune_ok) mech_tune_ok = subclass_ok;
            std::printf("  mechanic tune_sub ok=%d tyre=%d/%d chassis=%d/%d "
                        "cam=%d/%d tr=%d/%d inj=%d/%d nos=%d/%.2f\n",
                        subclass_ok, t_kind, t_items, c_kind, c_items, m_kind,
                        m_items, tr_kind, tr_items, i_kind, i_items, n_kind,
                        nos_ok);
          }

          // Phase 2.147 — EC_LCLICK car part → inventory (addItem+scrollTo).
          const int32_t size_pre_l = minv ? inventory_size(minv) : 0;
          const bool lclicked =
              (mech && look) ? mechanic_lclick_part(mech, look)
                             : ((mech && installed)
                                    ? mechanic_lclick_part(mech, installed)
                                    : false);
          const int32_t size_l = minv ? inventory_size(minv) : -1;
          InvObject* slot_l =
              (car && inst_slot > 0) ? part_on_slot(car, inst_slot) : installed;
          const int32_t lclick_n =
              mech ? tree_field_get_int(mech, "lclick_count") : 0;
          const int32_t l_idx =
              mech ? tree_field_get_int(mech, "last_lclick_index") : -9;
          const int32_t wake_n =
              car ? tree_field_get_int(car, "wake_count") : 0;
          mech_lclick_ok =
              (lclicked && size_l == size_pre_l + 1 && slot_l == nullptr &&
               lclick_n >= 1 && l_idx == size_l - 1 && wake_n >= 1 &&
               !tree_field_get_obj(mech, "look_part"))
                  ? 1
                  : 0;
          std::printf("  mechanic lclick ok=%d size=%d->%d slot=%d idx=%d "
                      "wake=%d\n",
                      mech_lclick_ok, size_pre_l, size_l, slot_l ? 1 : 0, l_idx,
                      wake_n);

          // Re-install so drop smoke still has a car part on the CFG slot.
          const bool reinstall =
              mech ? (mechanic_hover_at(mech, cx0, cy0) &&
                      mechanic_click_actual(mech))
                   : false;
          on_slot =
              (car && inst_slot > 0) ? part_on_slot(car, inst_slot) : nullptr;
          if (!reinstall || !on_slot) {
            std::printf("  mechanic reinstall for drop failed re=%d slot=%d\n",
                        reinstall ? 1 : 0, on_slot ? 1 : 0);
          }

          // Phase 2.138/2.146 — car→panel via dropObject OSD path.
          InvObject* dropped = on_slot;
          float dx1 = 0.f, dy1 = 0.f;
          {
            InvObject* ps = minv ? tree_field_get_obj(minv, "panels") : nullptr;
            InvObject* p1 =
                (ps && tree_vector_size(ps) > 1) ? tree_vector_element_at(ps, 1)
                                                : nullptr;
            if (p1) {
              dx1 = tree_field_get_float(p1, "x") +
                    tree_field_get_float(p1, "width") * 0.5f;
              dy1 = tree_field_get_float(p1, "y") +
                    tree_field_get_float(p1, "height") * 0.5f;
            }
          }
          const bool dropped_ok =
              (mech && dropped)
                  ? mechanic_drop_object_at(mech, dropped, dx1, dy1)
                  : false;
          const int32_t size_drop = minv ? inventory_size(minv) : -1;
          InvObject* slot_after =
              (car && inst_slot > 0) ? part_on_slot(car, inst_slot) : dropped;
          const int32_t drop_panel =
              mech ? tree_field_get_int(mech, "last_drop_panel") : -9;
          mech_drop_ok =
              (dropped_ok && size_drop == size_pre && slot_after == nullptr &&
               drop_panel == 1 &&
               (minv ? tree_field_get_int(minv, "drop_count") : 0) >= 1 &&
               (mech ? tree_field_get_int(mech, "drop_object_count") : 0) >= 1)
                  ? 1
                  : 0;
          std::printf("  mechanic drop ok=%d size=%d slot%d=%d panel=%d "
                      "drops=%d\n",
                      mech_drop_ok, size_drop, inst_slot, slot_after ? 1 : 0,
                      drop_panel,
                      minv ? tree_field_get_int(minv, "drop_count") : 0);

          gcmd = kCmdPaint;
          garage_smoke_step = 2;
        } else if (garage_smoke_step == 2 && frames >= 16) {
          mode_paint = tree_field_get_int(garage, "mode");
          // Phase 2.156 — painter.show on CMD_PAINT.
          painter_ux_ok =
              (garage && tree_field_get_int(garage, "painter_shown") == 1 &&
               tree_field_get_obj(garage, "painter") && mode_paint == 2)
                  ? 1
                  : 0;
          gcmd = kCmdEscape;
          garage_smoke_step = 3;
        } else if (garage_smoke_step == 3 && frames >= 18) {
          mode_none = tree_field_get_int(garage, "mode");
          if (!garage || tree_field_get_int(garage, "painter_shown") != 0)
            painter_ux_ok = 0;
          garage_smoke_step = 4;
        } else if (garage_smoke_step == 4 && frames >= 20) {
          want_install = true;
          garage_smoke_step = 5;
        } else if (garage_smoke_step == 5 && frames >= 22) {
          want_paint = true;
          garage_smoke_step = 6;
        } else if (garage_smoke_step == 6 && frames >= 24) {
          want_street = true;
          garage_smoke_step = 7;
        }
      }

      if (gcmd) {
        garage_osd_command(garage, gcmd);
        std::printf("  garage cmd=%d mode=%d\n", gcmd,
                    tree_field_get_int(garage, "mode"));
      }
      if (want_install) garage_do_install();
      if (want_paint) garage_do_paint();
      if (want_street) garage_go_street();
    }

    if (ctrl && !auto_new) {
      const float cancel =
          java_io_Controller_user_GetAxisVal(ctrl, kAxisCancel);
      // Edge only — FMV skip leaves ESC held; don't treat that as quit.
      if (prev_cancel < 0.5f && cancel >= 0.5f) {
        std::printf("  ESC — leave interactive loop\n");
        break;
      }
      prev_cancel = cancel;
    }

    render_d3d9_flush();
    ++frames;
    if (max_frames > 0 && frames >= max_frames) break;
#ifdef _WIN32
    Sleep(16);
#endif
  }

  input_live_enable(false);

  // Hub Valocity modes after career smoke — separate from CMD_NEW so
  // loadDefaults cannot poison Garage.createOSDObjects during career.
  if (auto_new && !fired_freeride) {
    auto hub_valo = [&](const char* tag, InvObject* (*fn)()) {
      InvObject* menu2 = tree_host_new("java.game.MainMenu");
      game_logic_change_active_section(menu2);
      menu = game_logic_actual_state();
      mmd = menu ? tree_field_get_obj(menu, "mmd") : nullptr;
      int32_t ok = 0;
      if (menu && std::strstr(tree_host_class(menu), "MainMenu")) {
        InvObject* city = fn();
        const char* cn = city ? tree_host_class(city) : nullptr;
        // Stock QUICKRACE Valocity.enter immediately CAS → RaceSetup.
        ok = (cn && (std::strstr(cn, "Valocity") ||
                     (std::strcmp(tag, "QUICKRACE") == 0 &&
                      std::strstr(cn, "RaceSetup"))))
                 ? 1
                 : 0;
        std::printf("  hub %s ok=%d state='%s'\n", tag, ok, cn ? cn : "?");
        // Stock RaceSetup.osdCommand(CMD_RACE) → startRace + CAS(track).
        // Fire before hub restores Garage so valo_ret_ok still sees garage.
        if (ok && std::strcmp(tag, "QUICKRACE") == 0 && cn &&
            std::strstr(cn, "RaceSetup"))
          racesetup_try_cmd_race(city);
      }
      if (garage && std::strstr(tree_host_class(garage), "Garage"))
        game_logic_change_active_section(garage);
      return ok;
    };
    hub_freeride_ok = hub_valo("FREERIDE", main_menu_cmd_freeride);
    hub_quickrace_ok = hub_valo("QUICKRACE", main_menu_cmd_quickrace);
    hub_demo_ok = hub_valo("DEMO", main_menu_cmd_demo);
    // BACK TO GARAGE from MainMenu after city modes.
    {
      InvObject* menu2 = tree_host_new("java.game.MainMenu");
      game_logic_change_active_section(menu2);
      menu = game_logic_actual_state();
      if (menu && std::strstr(tree_host_class(menu), "MainMenu")) {
        InvObject* g = main_menu_cmd_back_to_garage();
        hub_back_ok =
            (g && std::strstr(tree_host_class(g), "Garage")) ? 1 : 0;
        std::printf("  hub BACKTOGARAGE ok=%d state='%s'\n", hub_back_ok,
                    g && tree_host_class(g) ? tree_host_class(g) : "?");
      }
      if (garage && std::strstr(tree_host_class(garage), "Garage"))
        game_logic_change_active_section(garage);
    }
    // EXIT last — CAS null then restore Garage for return checks.
    {
      InvObject* menu2 = tree_host_new("java.game.MainMenu");
      game_logic_change_active_section(menu2);
      menu = game_logic_actual_state();
      if (menu && std::strstr(tree_host_class(menu), "MainMenu")) {
        hub_exit_ok = main_menu_cmd_exit() ? 1 : 0;
        std::printf("  hub EXIT ok=%d state='%s'\n", hub_exit_ok,
                    game_logic_actual_state()
                        ? tree_host_class(game_logic_actual_state())
                        : "null");
      }
      if (garage && std::strstr(tree_host_class(garage), "Garage"))
        game_logic_change_active_section(garage);
    }
    // OPTIONS + CREDITS — changeMode group switches (no CAS).
    {
      InvObject* menu2 = tree_host_new("java.game.MainMenu");
      game_logic_change_active_section(menu2);
      menu = game_logic_actual_state();
      mmd = menu ? tree_field_get_obj(menu, "mmd") : nullptr;
      if (menu && mmd && std::strstr(tree_host_class(menu), "MainMenu")) {
        hub_options_ok = main_menu_cmd_options() ? 1 : 0;
        std::printf("  hub OPTIONS ok=%d act=%d opt=%d\n", hub_options_ok,
                    tree_field_get_int(mmd, "actGroup"),
                    tree_field_get_int(mmd, "optionsGroup"));
        hub_credits_ok = main_menu_cmd_credits() ? 1 : 0;
        std::printf("  hub CREDITS ok=%d act=%d cred=%d\n", hub_credits_ok,
                    tree_field_get_int(mmd, "actGroup"),
                    tree_field_get_int(mmd, "creditsGroup"));
      }
      if (garage && std::strstr(tree_host_class(garage), "Garage"))
        game_logic_change_active_section(garage);
    }
    fired_freeride = true;
  }

  InvObject* final_st = game_logic_actual_state();
  const char* stn =
      final_st && tree_host_class(final_st) ? tree_host_class(final_st) : "?";
  std::printf("boot game_loop ok=%d frames=%d fired=%d state='%s' garage=%d\n",
              (menu && mmd_shown == 1 && frames > 0) ? 1 : 0, frames,
              fired_new ? 1 : 0, stn, garage_ok);
  std::printf("boot mainmenu_chrome ok=%d osd=%d txt=%d acg_host=%d btn=%d\n",
              (menu_chrome == 1 && menu_txt_n >= 12 && menu_osd_n >= 1) ? 1 : 0,
              menu_osd_n, menu_txt_n, acg_host, btn_n);

  // Boot-path fidelity toward splash→menu→garage→world (not full game %).
  // Rubric: FMV/JVM~15 SplashTREE~25 MainMenuTREE~35 chromeTREE~48
  // GarageOSD-TREE~58 MechanicTREE~65 ValocityTREE~80 RaceSetup~88
  // GarageOSD~90 RaceSetupOSD~94 CMD_RACE/startRace~100.
  // Valocity.enter TREE (not host mirror) is the ~80 gate.
  int boot_pct = 15;
  if (splash && std::strstr(splash_cn, "SplashScreen")) boot_pct = 25;
  if (menu && std::strstr(tree_host_class(menu), "MainMenu")) boot_pct = 35;
  if (menu_chrome == 1 && menu_txt_n >= 12 && acg_host == 0) boot_pct = 48;
  {
    InvObject* menu_st = menu;
    // MainMenuDialog.osdCommand TREE = career/freeride/demo hub multiplexor.
    if (menu_st && tree_field_get_int(menu_st, "osd_cmd_via_tree") == 1)
      boot_pct = 54;
    const int32_t hub_valo_n =
        (hub_freeride_ok ? 1 : 0) + (hub_quickrace_ok ? 1 : 0) +
        (hub_demo_ok ? 1 : 0);
    if (hub_valo_n >= 1) boot_pct = 56;
    if (hub_valo_n >= 3) boot_pct = 58;
  }
  {
    InvObject* g = garage ? garage : (final_st && tree_host_class(final_st) &&
                                              std::strstr(tree_host_class(final_st),
                                                          "Garage")
                                          ? final_st
                                          : nullptr);
    if (g && tree_field_get_int(g, "enter_via_tree") == 1) boot_pct = 55;
    if (g && tree_field_get_int(g, "osd_via_tree") == 1) boot_pct = 60;
    else if (g && tree_field_get_int(g, "osd_via_host") == 1 && boot_pct < 55)
      boot_pct = 52;
    else if (garage_ok && boot_pct < 50) boot_pct = 50;
  }
  if (valo_entered && boot_pct >= 60) boot_pct = 64;
  else if (valo_entered && boot_pct >= 55) boot_pct = 62;
  else if (valo_entered && boot_pct >= 50) boot_pct = 58;
  {
    const int32_t hub_valo_n =
        (hub_freeride_ok ? 1 : 0) + (hub_quickrace_ok ? 1 : 0) +
        (hub_demo_ok ? 1 : 0);
    if (hub_valo_n >= 1 && boot_pct >= 62) boot_pct = 66;
    if (hub_valo_n >= 3 && boot_pct >= 66) boot_pct = 68;
    if (hub_back_ok == 1 && boot_pct >= 68) boot_pct = 69;
    if (hub_exit_ok == 1 && boot_pct >= 69) boot_pct = 70;
    if (hub_options_ok == 1 && boot_pct >= 70) boot_pct = 71;
    if (hub_credits_ok == 1 && boot_pct >= 71) boot_pct = 72;
  }
  // ~80: Valocity.enter via TREE (mirror removed; flag set only on TREE ok).
  if (valo_entered && city && tree_field_get_int(city, "enter_via_tree") == 1 &&
      boot_pct >= 62)
    boot_pct = 80;
  // ~88: QUICKRACE hub lands on RaceSetup via TREE (stock Valocity→RaceSetup).
  if (hub_quickrace_ok == 1 && boot_pct >= 80) {
    InvObject* rs = game_logic_racesetup();
    if (rs && tree_field_get_int(rs, "enter_via_tree") == 1)
      boot_pct = 88;
    else
      boot_pct = 84;
  }
  // ~90: Garage street OSD via TREE (career + hub re-enters, not host strip).
  {
    InvObject* g = game_logic_garage();
    if (!g && final_st && tree_host_class(final_st) &&
        std::strstr(tree_host_class(final_st), "Garage"))
      g = final_st;
    if (g && tree_field_get_int(g, "osd_via_tree") == 1 && boot_pct >= 88)
      boot_pct = 90;
  }
  // ~94: RaceSetup OSD via TREE (CMD_RACE=0 / CMD_ABANDON=1).
  {
    InvObject* rs = game_logic_racesetup();
    if (rs && tree_field_get_int(rs, "osd_via_tree") == 1 && boot_pct >= 90)
      boot_pct = 94;
  }
  // ~100: RaceSetup.osdCommand(CMD_RACE) TREE → City.startRace + CAS(track).
  {
    InvObject* rs = game_logic_racesetup();
    InvObject* track = rs ? tree_field_get_obj(rs, "track") : nullptr;
    if (!track && rs) track = tree_field_get_obj(rs, "lastState");
    const bool cmd =
        rs && tree_field_get_int(rs, "osd_cmd_via_tree") == 1 &&
        tree_field_get_int(rs, "last_osd_cmd") == 0;
    const bool started =
        (track && tree_field_get_int(track, "start_race_via_tree") == 1 &&
         tree_field_get_obj(track, "raceStart")) ||
        (rs && tree_field_get_int(rs, "start_race_via_tree") == 1);
    if (cmd && started && boot_pct >= 94) boot_pct = 100;
  }
  std::printf("boot progress ~%d%% (splash→menu-hub→garage/city path)\n",
              boot_pct);
  std::printf(
      "boot hub_valo freeride=%d quickrace=%d demo=%d back=%d exit=%d "
      "options=%d credits=%d\n",
      hub_freeride_ok, hub_quickrace_ok, hub_demo_ok, hub_back_ok, hub_exit_ok,
      hub_options_ok, hub_credits_ok);

  const bool garage_modes_ok =
      !auto_new || (mode_mech == 1 && mode_paint == 2 && mode_none == 0 &&
                    car_ok == 1 &&
                    tree_field_get_int(garage ? garage : final_st,
                                       "mode_changes") >= 3);
  std::printf("boot garage_loop ok=%d car=%d modes=%d/%d/%d changes=%d\n",
              garage_modes_ok ? 1 : 0, car_ok, mode_mech, mode_paint, mode_none,
              garage ? tree_field_get_int(garage, "mode_changes") : 0);

  const bool mechanic_inv_ok = !auto_new || (mech_inv_ok == 1);
  std::printf("boot mechanic_inv ok=%d\n", mechanic_inv_ok ? 1 : 0);

  const bool visual_inv_ok = !auto_new || (mech_vis_ok == 1);
  std::printf("boot visual_inv ok=%d\n", visual_inv_ok ? 1 : 0);

  const bool mechanic_preview_ok = !auto_new || (mech_preview_ok == 1);
  std::printf("boot mechanic_preview ok=%d\n", mechanic_preview_ok ? 1 : 0);

  const bool mechanic_rotate_ok = !auto_new || (mech_rotate_ok == 1);
  std::printf("boot mechanic_rotate ok=%d\n", mechanic_rotate_ok ? 1 : 0);

  const bool mechanic_swap_ok = !auto_new || (mech_swap_ok == 1);
  std::printf("boot mechanic_swap ok=%d\n", mechanic_swap_ok ? 1 : 0);

  const bool mechanic_hover_ok = !auto_new || (mech_hover_ok == 1);
  std::printf("boot mechanic_hover ok=%d\n", mechanic_hover_ok ? 1 : 0);

  const bool mechanic_click_ok = !auto_new || (mech_click_ok == 1);
  std::printf("boot mechanic_click ok=%d\n", mechanic_click_ok ? 1 : 0);

  const bool mechanic_drag_ok = !auto_new || (mech_drag_ok == 1);
  std::printf("boot mechanic_drag ok=%d\n", mechanic_drag_ok ? 1 : 0);

  const bool mechanic_lclick_ok = !auto_new || (mech_lclick_ok == 1);
  std::printf("boot mechanic_lclick ok=%d\n", mechanic_lclick_ok ? 1 : 0);

  const bool mechanic_pick_ok = !auto_new || (mech_pick_ok == 1);
  std::printf("boot mechanic_pick ok=%d\n", mechanic_pick_ok ? 1 : 0);

  const bool mechanic_rdrag_ok = !auto_new || (mech_rdrag_ok == 1);
  std::printf("boot mechanic_rdrag ok=%d\n", mechanic_rdrag_ok ? 1 : 0);

  const bool cursor_syslock_ok = !auto_new || (cursor_lock_ok == 1);
  std::printf("boot cursor_lock ok=%d\n", cursor_syslock_ok ? 1 : 0);

  const bool cursor_ldrag_boot_ok = !auto_new || (cursor_ldrag_ok == 1);
  std::printf("boot cursor_ldrag ok=%d\n", cursor_ldrag_boot_ok ? 1 : 0);

  const bool mechanic_tune_ok = !auto_new || (mech_tune_ok == 1);
  std::printf("boot mechanic_tune ok=%d\n", mechanic_tune_ok ? 1 : 0);

  const bool mechanic_chrome_ok = !auto_new || (mech_chrome_ok == 1);
  std::printf("boot mechanic_chrome ok=%d\n", mechanic_chrome_ok ? 1 : 0);

  const bool mechanic_install_ok = !auto_new || (mech_install_ok == 1);
  std::printf("boot mechanic_install ok=%d\n", mechanic_install_ok ? 1 : 0);

  const bool mechanic_drop_ok = !auto_new || (mech_drop_ok == 1);
  std::printf("boot mechanic_drop ok=%d\n", mechanic_drop_ok ? 1 : 0);

  const bool garage_parts_ok =
      !auto_new || (install_ok == 1 && paint_ok == 1 && painter_ux_ok == 1);
  std::printf("boot garage_parts ok=%d install=%d paint=%d painter_ux=%d\n",
              garage_parts_ok ? 1 : 0, install_ok, paint_ok, painter_ux_ok);

  std::printf("boot splash_timer ok=%d\n", splash_time_off);

  const bool traffic_p_boot_ok = !auto_new || (traffic_p_ok == 1);
  std::printf("boot addTrafficP ok=%d\n", traffic_p_boot_ok ? 1 : 0);

  const bool traffic_bh_boot_ok = !auto_new || (traffic_bh_ok == 1);
  std::printf("boot setTrafficCarBehaviour ok=%d\n", traffic_bh_boot_ok ? 1 : 0);

  const bool traffic_halt_boot_ok = !auto_new || (traffic_halt_ok == 1);
  std::printf("boot haltTrafficCross ok=%d\n", traffic_halt_boot_ok ? 1 : 0);

  const bool traffic_halt_path_boot_ok =
      !auto_new || (traffic_halt_path_ok == 1);
  std::printf("boot haltTrafficPath ok=%d\n",
              traffic_halt_path_boot_ok ? 1 : 0);

  const bool traffic_rem_boot_ok = !auto_new || (traffic_rem_ok == 1);
  std::printf("boot remTrafficCar ok=%d\n", traffic_rem_boot_ok ? 1 : 0);

  const bool traffic_del_boot_ok = !auto_new || (traffic_del_ok == 1);
  std::printf("boot delTraffic ok=%d\n", traffic_del_boot_ok ? 1 : 0);

  const bool traffic_ped_boot_ok = !auto_new || (traffic_ped_ok == 1);
  std::printf("boot setPedestrianDensityN ok=%d\n",
              traffic_ped_boot_ok ? 1 : 0);

  const bool traffic_ped_type_boot_ok = !auto_new || (traffic_ped_type_ok == 1);
  std::printf("boot addPedestrianType ok=%d\n",
              traffic_ped_type_boot_ok ? 1 : 0);

  const bool traffic_ped_rem_boot_ok = !auto_new || (traffic_ped_rem_ok == 1);
  std::printf("boot remPedestrianType ok=%d\n",
              traffic_ped_rem_boot_ok ? 1 : 0);

  const bool traffic_ped_dist_boot_ok = !auto_new || (traffic_ped_dist_ok == 1);
  std::printf("boot pedestrianDistance ok=%d\n",
              traffic_ped_dist_boot_ok ? 1 : 0);

  const bool traffic_fog_boot_ok = !auto_new || (traffic_fog_ok == 1);
  std::printf("boot setFog ok=%d\n", traffic_fog_boot_ok ? 1 : 0);

  const bool traffic_light_boot_ok = !auto_new || (traffic_light_ok == 1);
  std::printf("boot setLight ok=%d\n", traffic_light_boot_ok ? 1 : 0);

  const bool traffic_flare_boot_ok = !auto_new || (traffic_flare_ok == 1);
  std::printf("boot setFlare ok=%d\n", traffic_flare_boot_ok ? 1 : 0);

  const bool traffic_dup_boot_ok = !auto_new || (traffic_dup_ok == 1);
  std::printf("boot duplicate ok=%d\n", traffic_dup_boot_ok ? 1 : 0);

  const bool traffic_create_boot_ok = !auto_new || (traffic_create_ok == 1);
  std::printf("boot create ok=%d\n", traffic_create_boot_ok ? 1 : 0);

  const bool traffic_chg_boot_ok = !auto_new || (traffic_chg_ok == 1);
  std::printf("boot changeResource ok=%d\n", traffic_chg_boot_ok ? 1 : 0);

  const bool traffic_color_boot_ok = !auto_new || (traffic_color_ok == 1);
  std::printf("boot setColor ok=%d\n", traffic_color_boot_ok ? 1 : 0);

  const bool traffic_type_boot_ok = !auto_new || (traffic_type_ok == 1);
  std::printf("boot getTypeID ok=%d\n", traffic_type_boot_ok ? 1 : 0);

  const bool traffic_scale_boot_ok = !auto_new || (traffic_scale_ok == 1);
  std::printf("boot scaleMesh ok=%d\n", traffic_scale_boot_ok ? 1 : 0);

  const bool traffic_plot_boot_ok = !auto_new || (traffic_plot_ok == 1);
  std::printf("boot plotRoute ok=%d\n", traffic_plot_boot_ok ? 1 : 0);

  const bool traffic_routepos_boot_ok = !auto_new || (traffic_routepos_ok == 1);
  std::printf("boot getRoutePos ok=%d\n", traffic_routepos_boot_ok ? 1 : 0);

  const bool traffic_setmatrix_boot_ok = !auto_new || (traffic_setmatrix_ok == 1);
  std::printf("boot setMatrix ok=%d\n", traffic_setmatrix_boot_ok ? 1 : 0);

  const bool traffic_setpos_boot_ok = !auto_new || (traffic_setpos_ok == 1);
  std::printf("boot setPos ok=%d\n", traffic_setpos_boot_ok ? 1 : 0);

  const bool traffic_setstate_boot_ok = !auto_new || (traffic_setstate_ok == 1);
  std::printf("boot setState ok=%d\n", traffic_setstate_boot_ok ? 1 : 0);

  const bool traffic_getpos_boot_ok = !auto_new || (traffic_getpos_ok == 1);
  std::printf("boot getPos ok=%d\n", traffic_getpos_boot_ok ? 1 : 0);

  const bool traffic_getvel_boot_ok = !auto_new || (traffic_getvel_ok == 1);
  std::printf("boot getVel ok=%d\n", traffic_getvel_boot_ok ? 1 : 0);

  const bool traffic_setparent_boot_ok =
      !auto_new || (traffic_setparent_ok == 1);
  std::printf("boot setParent ok=%d\n", traffic_setparent_boot_ok ? 1 : 0);

  const bool traffic_getspeedsq_boot_ok =
      !auto_new || (traffic_getspeedsq_ok == 1);
  std::printf("boot getSpeedSquare ok=%d\n",
              traffic_getspeedsq_boot_ok ? 1 : 0);

  const bool traffic_isempty_boot_ok = !auto_new || (traffic_isempty_ok == 1);
  std::printf("boot isEmpty ok=%d\n", traffic_isempty_boot_ok ? 1 : 0);

  const bool traffic_isscripted_boot_ok =
      !auto_new || (traffic_isscripted_ok == 1);
  std::printf("boot isScripted ok=%d\n", traffic_isscripted_boot_ok ? 1 : 0);

  const bool traffic_getscript_boot_ok =
      !auto_new || (traffic_getscript_ok == 1);
  std::printf("boot getScriptInstance ok=%d\n",
              traffic_getscript_boot_ok ? 1 : 0);

  {
    const int32_t type_null = java_util_resource_ResourceRef_type(nullptr);
    const float vp_w0 = java_render_Viewport_getWidth(nullptr);
    const float vp_a0 = java_render_Viewport_getAspect(nullptr);
    InvObject* nav_hide = tree_host_new("java.game.Navigator");
    java_game_Navigator_updateNavigator(nav_hide, nullptr, 0);
    const int32_t nav_upd = tree_field_get_int(nav_hide, "update_count");
    std::printf("boot ResourceRef_type null=%d ok=%d\n", type_null,
                type_null == 0 ? 1 : 0);
    std::printf("boot Viewport getters w0=%.3f aspect0=%.3f ok=%d\n", vp_w0,
                vp_a0, (vp_w0 == 0.f && vp_a0 == 1.f) ? 1 : 0);
    std::printf("boot updateNavigator hide upd=%d ok=%d\n", nav_upd,
                nav_upd == 0 ? 1 : 0);
  }

  const bool valo_ok =
      !auto_new || (valo_entered == 1 && valo_shape == 1 && valo_dist > 2.f &&
                    valo_spd > 1.f);
  std::printf("boot valocity_live ok=%d entered=%d dist=%.1f spd=%.1f shape=%d\n",
              valo_ok ? 1 : 0, valo_entered, valo_dist, valo_spd, valo_shape);

  const bool valo_cam_ok = !auto_new || (valo_cam == 1);
  std::printf("boot valocity_cam ok=%d behind=%.1f nav_upd=%d hud=%d\n",
              valo_cam_ok ? 1 : 0, valo_cam_behind, valo_nav_upd, valo_hud);

  const bool valo_ret_ok =
      !auto_new ||
      (valo_denied == 1 && valo_back == 1 && garage &&
       std::strstr(tree_host_class(garage), "Garage") &&
       game_logic_actual_state() == garage &&
       city && tree_field_get_int(city, "traffic_count") == 0);
  std::printf("boot valocity_return ok=%d denied=%d back=%d traffic=%d\n",
              valo_ret_ok ? 1 : 0, valo_denied, valo_back,
              city ? tree_field_get_int(city, "traffic_count") : -1);

  if (auto_new && !garage_ok) {
    std::printf("FAIL --game auto_new did not reach Garage\n");
    return 5;
  }
  if (auto_new && !garage_modes_ok) {
    std::printf("FAIL --game garage modes\n");
    return 5;
  }
  if (auto_new && !mechanic_inv_ok) {
    std::printf("FAIL --game mechanic inventory\n");
    return 5;
  }
  if (auto_new && !visual_inv_ok) {
    std::printf("FAIL --game VisualInventory panels\n");
    return 5;
  }
  if (auto_new && !mechanic_preview_ok) {
    std::printf("FAIL --game InventoryPanel preview\n");
    return 5;
  }
  if (auto_new && !mechanic_rotate_ok) {
    std::printf("FAIL --game InventoryPanel focusHook rotate\n");
    return 5;
  }
  if (auto_new && !mechanic_swap_ok) {
    std::printf("FAIL --game VisualInventory panelSwap\n");
    return 5;
  }
  if (auto_new && !mechanic_hover_ok) {
    std::printf("FAIL --game Mechanic hover actualPanel\n");
    return 5;
  }
  if (auto_new && !mechanic_click_ok) {
    std::printf("FAIL --game Mechanic panel click\n");
    return 5;
  }
  if (auto_new && !mechanic_drag_ok) {
    std::printf("FAIL --game Mechanic panel drag swap\n");
    return 5;
  }
  if (auto_new && !mechanic_lclick_ok) {
    std::printf("FAIL --game Mechanic EC_LCLICK uninstall\n");
    return 5;
  }
  if (auto_new && !mechanic_pick_ok) {
    std::printf("FAIL --game Mechanic car part pick\n");
    return 5;
  }
  if (auto_new && !mechanic_rdrag_ok) {
    std::printf("FAIL --game Garage EC_RDRAG camera\n");
    return 5;
  }
  if (auto_new && !cursor_syslock_ok) {
    std::printf("FAIL --game Cursor lock/unlock ClipCursor\n");
    return 5;
  }
  if (auto_new && !cursor_ldrag_boot_ok) {
    std::printf("FAIL --game Cursor EC_LDRAG BEGIN/END\n");
    return 5;
  }
  if (auto_new && !mechanic_tune_ok) {
    std::printf("FAIL --game Mechanic tune mode\n");
    return 5;
  }
  if (auto_new && !mechanic_chrome_ok) {
    std::printf("FAIL --game Mechanic chrome\n");
    return 5;
  }
  if (auto_new && !mechanic_install_ok) {
    std::printf("FAIL --game Mechanic panel install\n");
    return 5;
  }
  if (auto_new && !mechanic_drop_ok) {
    std::printf("FAIL --game Mechanic panel drop\n");
    return 5;
  }
  if (auto_new && !garage_parts_ok) {
    std::printf("FAIL --game garage install/paint\n");
    return 5;
  }
  if (auto_new && !traffic_p_boot_ok) {
    std::printf("FAIL --game addTrafficP near-cross\n");
    return 5;
  }
  if (auto_new && !traffic_bh_boot_ok) {
    std::printf("FAIL --game setTrafficCarBehaviour\n");
    return 5;
  }
  if (auto_new && !traffic_halt_boot_ok) {
    std::printf("FAIL --game haltTrafficCross\n");
    return 5;
  }
  if (auto_new && !traffic_halt_path_boot_ok) {
    std::printf("FAIL --game haltTrafficPath\n");
    return 5;
  }
  if (auto_new && !traffic_rem_boot_ok) {
    std::printf("FAIL --game remTrafficCar\n");
    return 5;
  }
  if (auto_new && !traffic_del_boot_ok) {
    std::printf("FAIL --game delTraffic\n");
    return 5;
  }
  if (auto_new && !traffic_ped_boot_ok) {
    std::printf("FAIL --game setPedestrianDensityN\n");
    return 5;
  }
  if (auto_new && !traffic_ped_type_boot_ok) {
    std::printf("FAIL --game addPedestrianType\n");
    return 5;
  }
  if (auto_new && !traffic_ped_rem_boot_ok) {
    std::printf("FAIL --game remPedestrianType\n");
    return 5;
  }
  if (auto_new && !traffic_ped_dist_boot_ok) {
    std::printf("FAIL --game pedestrianDistance\n");
    return 5;
  }
  if (auto_new && !traffic_fog_boot_ok) {
    std::printf("FAIL --game setFog\n");
    return 5;
  }
  if (auto_new && !traffic_light_boot_ok) {
    std::printf("FAIL --game setLight\n");
    return 5;
  }
  if (auto_new && !traffic_flare_boot_ok) {
    std::printf("FAIL --game setFlare\n");
    return 5;
  }
  if (auto_new && !traffic_dup_boot_ok) {
    std::printf("FAIL --game duplicate\n");
    return 5;
  }
  if (auto_new && !traffic_create_boot_ok) {
    std::printf("FAIL --game create\n");
    return 5;
  }
  if (auto_new && !traffic_chg_boot_ok) {
    std::printf("FAIL --game changeResource\n");
    return 5;
  }
  if (auto_new && !traffic_color_boot_ok) {
    std::printf("FAIL --game setColor\n");
    return 5;
  }
  if (auto_new && !traffic_type_boot_ok) {
    std::printf("FAIL --game getTypeID\n");
    return 5;
  }
  if (auto_new && !traffic_scale_boot_ok) {
    std::printf("FAIL --game scaleMesh\n");
    return 5;
  }
  if (auto_new && !traffic_plot_boot_ok) {
    std::printf("FAIL --game plotRoute\n");
    return 5;
  }
  if (auto_new && !traffic_routepos_boot_ok) {
    std::printf("FAIL --game getRoutePos\n");
    return 5;
  }
  if (auto_new && !traffic_setmatrix_boot_ok) {
    std::printf("FAIL --game setMatrix\n");
    return 5;
  }
  if (auto_new && !traffic_setpos_boot_ok) {
    std::printf("FAIL --game setPos\n");
    return 5;
  }
  if (auto_new && !traffic_setstate_boot_ok) {
    std::printf("FAIL --game setState\n");
    return 5;
  }
  if (auto_new && !traffic_getpos_boot_ok) {
    std::printf("FAIL --game getPos\n");
    return 5;
  }
  if (auto_new && !traffic_getvel_boot_ok) {
    std::printf("FAIL --game getVel\n");
    return 5;
  }
  if (auto_new && !traffic_setparent_boot_ok) {
    std::printf("FAIL --game setParent\n");
    return 5;
  }
  if (auto_new && !traffic_getspeedsq_boot_ok) {
    std::printf("FAIL --game getSpeedSquare\n");
    return 5;
  }
  if (auto_new && !traffic_isempty_boot_ok) {
    std::printf("FAIL --game isEmpty\n");
    return 5;
  }
  if (auto_new && !traffic_isscripted_boot_ok) {
    std::printf("FAIL --game isScripted\n");
    return 5;
  }
  if (auto_new && !traffic_getscript_boot_ok) {
    std::printf("FAIL --game getScriptInstance\n");
    return 5;
  }
  if (auto_new && !valo_ok) {
    std::printf("FAIL --game Valocity live\n");
    return 5;
  }
  if (auto_new && !valo_cam_ok) {
    std::printf("FAIL --game Valocity cam/hud/nav\n");
    return 5;
  }
  if (auto_new && !valo_ret_ok) {
    std::printf("FAIL --game Valocity return\n");
    return 5;
  }
  if (frames < 1) {
    std::printf("FAIL --game no frames\n");
    return 5;
  }
  return 0;
}

}  // namespace inv
