// Fragment 1/2 of game_boot.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


namespace inv {
namespace {

// Soft PE ResHandle (16B on stock): +8 remapped key, +0xC owner.
// Host callers (GameRef_core) use a 16B scratch with int32 key @ +8 —
// Engine_LoadGameInit gates *(wt+8)!=0 @ 0x53A640.
using SoftBootRh = uint8_t[16];

static void soft_boot_rh_set_key(SoftBootRh rh, int32_t key) {
  std::memset(rh, 0, 16);
  *reinterpret_cast<int32_t*>(rh + 8) = key;
}

// Soft PE Engine_boot@0x58C700 gate helpers (InitState Config vs MainLoop GameInit).
// PE Engine_InitState@0x427980 (pre-FMV): Remap(system.rpk,4) →
// g_WorldTreeRoot@636460 Bind type=1; Remap(system.rpk,0x47=71) Bind type=8 →
// LoadGameInit(...,"Config")@0x53A5E0 → link EngineState+0xF8..+0x104.
// PE Engine_MainLoop@0x428960 (post-FMV): Config.init → Bind type=8;
// Engine_LoadGameInit(wt, typeRH, unk_63C560, "GameInit")@0x53A5E0 →
// ResHandle_Link into EngineState+0xE8..+0xF4 before while(!quit).
// Host: soft Remap + engine_load_game_init stand-in. Config.init→GameInit
// type RH unknown without Config GameType payload — GameInit type_rh=0
// (node+alias still created; THRD-CREATE attach when type mid present).
struct GameInitPrologue {
  uint32_t wt_key = 0;
  uint32_t config_type_key = 0;
  void* config_gi = nullptr;
  void* gameinit_gi = nullptr;
  int32_t config_rid = 0;
  int32_t gameinit_rid = 0;
};

// Soft ≈ Engine_InitState Config slice @ 0x427980 (before Engine_PlayBootFMV).
GameInitPrologue game_boot_initstate_config_prologue() {
  GameInitPrologue out{};
  out.wt_key = resource_engine_remap_from_path("system.rpk", /*local=*/4);
  out.config_type_key =
      resource_engine_remap_from_path("system.rpk", /*local=*/71);
  if (out.wt_key == 0) {
    std::printf(
        "  Soft InitState@0x427980: wt Remap(system.rpk,4)=0 "
        "(skip LoadGameInit Config)\n");
    return out;
  }

  alignas(4) SoftBootRh wt{};
  soft_boot_rh_set_key(wt, static_cast<int32_t>(out.wt_key));

  alignas(4) SoftBootRh config_type{};
  soft_boot_rh_set_key(config_type, static_cast<int32_t>(out.config_type_key));
  out.config_gi = engine_load_game_init(
      wt,
      out.config_type_key != 0 ? static_cast<void*>(config_type) : nullptr, "",
      "Config");
  if (out.config_gi) {
    out.config_rid =
        *reinterpret_cast<int32_t*>(reinterpret_cast<char*>(out.config_gi) +
                                    0x50);
  }

  std::printf(
      "  Soft InitState@0x427980 Config: wt=0x%X gi=%p rid=0x%X type=0x%X "
      "— PE LoadGameInit@0x53A5E0 (pre-FMV)\n",
      out.wt_key, out.config_gi, out.config_rid, out.config_type_key);
  return out;
}

// Soft ≈ Engine_MainLoop GameInit slice @ 0x428960 (after FMV triad).
void game_boot_mainloop_gameinit_load(GameInitPrologue& out) {
  if (out.wt_key == 0) {
    out.wt_key = resource_engine_remap_from_path("system.rpk", /*local=*/4);
  }
  if (out.wt_key == 0) {
    std::printf(
        "  Soft MainLoop@0x428960 GameInit: wt Remap(system.rpk,4)=0 "
        "(skip LoadGameInit)\n");
    return;
  }

  alignas(4) SoftBootRh wt{};
  soft_boot_rh_set_key(wt, static_cast<int32_t>(out.wt_key));

  // Stock type RH = Config script field "init" (MainLoop@0x42899E..0x4289CD).
  // Soft: nullptr type — InstanceNode alias "GameInit" still allocated.
  out.gameinit_gi = engine_load_game_init(wt, nullptr, "", "GameInit");
  if (out.gameinit_gi) {
    out.gameinit_rid = *reinterpret_cast<int32_t*>(
        reinterpret_cast<char*>(out.gameinit_gi) + 0x50);
  }

  std::printf(
      "  Soft MainLoop@0x428960 GameInit: wt=0x%X gi=%p rid=0x%X "
      "— PE LoadGameInit@0x53A5E0 (post-FMV)\n",
      out.wt_key, out.gameinit_gi, out.gameinit_rid);
}

// --boot (no FMV): Config+GameInit back-to-back stand-in for InitState+MainLoop.
GameInitPrologue game_boot_mainloop_gameinit_prologue() {
  GameInitPrologue out = game_boot_initstate_config_prologue();
  game_boot_mainloop_gameinit_load(out);
  return out;
}

// PE Engine_MainLoop frame tail: ResourceEngine_PumpLoadQueue@0x5378D0 +
// PumpUnloadQueue@0x537B40 (after Sfx; before Present). Soft each host frame.
void game_boot_mainloop_resource_pumps() {
  (void)resource_engine_pump_load_queue();
  (void)resource_engine_pump_unload_queue();
}

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

  // Soft Engine_boot@0x58C700 without FMV: InitState Config + MainLoop GameInit
  // back-to-back (PE separates them around Engine_PlayBootFMV@0x55C470).
  (void)game_boot_mainloop_gameinit_prologue();

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

  // Soft PE Engine_boot @ 0x0058C700 size 0x2D2 (722). Xref: WinMain@0x5514A8.
  // PE order (IDA):
  //   DiagLog → CheckDiskCached@0x401000 → ResourceEngine_Init@0x535E70 →
  //   LoadPack("system.rpk")@0x538380 (fail AX==0xFFFF → Shutdown+Fatal) →
  //   Engine_BootPostLoadPackStub@0x497FD0 (return 1) →
  //   Engine_InitState@0x427980 (JVM + Natives_Register* + LoadGameInit
  //     "Config" + ApplyConfigGfx; BEFORE FMV) →
  //   Config acceptBuild/version/FILD gates →
  //   GfxEngine_InitDisplay@0x4FC1B0 + AllocPools@0x4FBCE0 →
  //   Input_ResetGlobals@0x54DD50 → Sound_InitDirectSound@0x5598C0 + HW caps →
  //   FMV triad File_PathExists@0x54C170 → Engine_PlayBootFMV@0x55C470
  //     (NOT GfxEngine.openVideo@0x47C330): Activision → Invictus →
  //     StreetLegal.avi (SL1 leftover; PathExists skip, no error) →
  //   Engine_MainLoop@0x428960 (LoadGameInit "GameInit"@0x53A5E0 + while) →
  //   TextureLog_FlushWrite@0x53C940 AFTER loop → teardown
  //     (shutdownMainHandles → DestroyOwnedAt11C → clearSimCallbacks →
  //      Sound/Input/Gfx shutdown → Engine_BootPreShutdownStub@0x497FE0 →
  //      ResourceEngine_Shutdown).
  // Soft map: warmup≈RE_Init+LoadPack+JVM/natives; render_d3d9_open≈
  // InitDisplay; initstate_config≈InitState Config; video_fmv_play_boot_intros
  // ≈PlayBootFMV triad; mainloop_gameinit_load≈MainLoop GameInit; harness≈
  // MainLoop while (splash/menu/world). FAIL fmv_n<2 is host smoke for shipped
  // Activision+Invictus, not a PE PathExists error.
  std::printf(
      "  Soft Engine_boot@0x58C700 gate: warmup packs+JVM → Gfx open → "
      "InitState Config → FMV triad → MainLoop GameInit+harness\n");

  // Soft ≈ InitState LoadGameInit("Config") — PE places this before FMV.
  GameInitPrologue gi_boot = game_boot_initstate_config_prologue();

  const int32_t fmv_cap = (auto_new || max_frames > 0) ? 8 : 0;
  const int32_t fmv_n = video_fmv_play_boot_intros(fmv_cap);
  std::printf(
      "  Soft Engine_PlayBootFMV@0x55C470 clips=%d (Activision+Invictus; "
      "StreetLegal PathExists-skip)\n",
      fmv_n);
  if (fmv_n < 2) {
    std::printf("FAIL --game boot FMV intros (need Activision+Invictus)\n");
    return 5;
  }

  // Soft ≈ MainLoop prologue (stock post-FMV, pre-while): GameInit under wtroot.
  game_boot_mainloop_gameinit_load(gi_boot);
  if (!gi_boot.gameinit_gi) {
    std::printf(
        "WARN PATH-TO-WORLD: GameInit LoadGameInit returned null "
        "(wt=0x%X) — continuing Splash harness\n",
        gi_boot.wt_key);
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
    std::printf(
        "  smoke PATH-TO-WORLD: auto_new → Garage → Hit the Street → "
        "Valocity\n");

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
  int32_t valo_vp = 0;
  int32_t valo_nav_upd = 0;
  int32_t valo_hud = 0;
  float valo_cam_behind = 0.f;
  // Soft phys/aero/vehicle probes (post city_drive_smoke side-bands only).
  int32_t valo_phys = 0;
  int32_t valo_aero = 0;
  int32_t valo_vehicle = 0;
  float valo_phys_spd = 0.f;
  float valo_aero_fmag = 0.f;
  float valo_aero_cd = 0.f;
  int32_t valo_aero_applied = 0;
  int32_t valo_chassis_shape = 0;
  int32_t valo_driveable = 0;
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
        "  PATH-TO-WORLD: Valocity entered=%d map=0x%X (Garage CMD_HITTHESTREET "
        "→ load into world)\n",
        valo_entered, tree_field_get_int(city, "map_id"));
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
    // Soft deepen: observe phys/aero/vehicle side-bands after simulate.
    // Body key mirrors Vehicle.getSpeedSquare @ 0x480500 + city_stop_car
    // (chassis Native.ptr after Vehicle.set; no invent solve/drive).
    InvObject* chassis = tree_field_get_obj(starter_car, "chassis");
    InvObject* body = chassis ? chassis : starter_car;
    if (physics_shape(body) == 0 && physics_shape(starter_car) != 0)
      body = starter_car;
    valo_chassis_shape = chassis ? physics_shape(chassis) : 0;
    valo_shape = physics_shape(body);
    if (valo_shape == 0) valo_shape = physics_shape(starter_car);
    valo_phys_spd = (physics_shape(body) != 0) ? physics_speed_square(body) : 0.f;
    if (valo_phys_spd <= 0.f && physics_shape(starter_car) != 0)
      valo_phys_spd = physics_speed_square(starter_car);
    const float live_spd = java_game_Vehicle_getSpeedSquare(starter_car);
    if (live_spd > valo_spd) valo_spd = live_spd;
    if (valo_phys_spd > valo_spd) valo_spd = valo_phys_spd;
    // Chassis_physWheelTick aero soft @ 0x456a6b..0x456d2d TREE band.
    valo_aero_applied =
        chassis ? tree_field_get_int(chassis, "aero_applied") : 0;
    valo_aero_fmag =
        chassis ? tree_field_get_float(chassis, "aero_phys_20f4") : 0.f;
    valo_aero_cd =
        chassis ? tree_field_get_float(chassis, "force_update_drag_c") : 0.f;
    if (valo_aero_cd == 0.f && chassis)
      valo_aero_cd = tree_field_get_float(chassis, "C_drag");
    const int32_t flags70 =
        chassis ? tree_field_get_int(chassis, "force_update_flags70") : 0;
    valo_driveable = vehicle_is_driveable(starter_car) ? 0 : 1;
    valo_phys =
        (valo_shape != 0 && valo_phys_spd > 1.f && valo_dist > 2.f) ? 1 : 0;
    // Soft aero ok: applied⇒|F|>0; else PE skip gates (flags70&1 / cd<=0 /
    // |v|^2<=1 @ 0x456a61/83/ac8) still coherent — no invent force.
    if (!chassis) {
      valo_aero = 0;
    } else if (valo_aero_applied == 1) {
      valo_aero = (valo_aero_fmag > 0.f) ? 1 : 0;
    } else if ((flags70 & 1) != 0 || valo_aero_cd <= 0.f ||
               valo_phys_spd <= 1.f) {
      valo_aero = 1;
    } else {
      valo_aero = 0;
    }
    valo_vehicle =
        (chassis && valo_driveable == 1 && live_spd > 1.f) ? 1 : 0;
    std::printf(
        "  Valocity drive dist=%.1f spd=%.1f shape=%d ch_shape=%d "
        "phys_spd=%.1f aero_app=%d |F|=%.1f cd=%.3f driveable=%d\n",
        valo_dist, valo_spd, valo_shape, valo_chassis_shape, valo_phys_spd,
        valo_aero_applied, valo_aero_fmag, valo_aero_cd, valo_driveable);
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
    // Latch while chase cam is live — teardown clears g_active_vp/cam before
    // the end-of-smoke render catalog probe.
    valo_vp = render_d3d9_viewport_active() ? 1 : 0;
    std::printf(
        "  Valocity cam behind=%.1f eye_y=%.1f active=%d vp=%d nav_upd=%d "
        "vis=%d hud=%d kph=%.0f\n",
        valo_cam_behind, ey, render_d3d9_camera_active() == vcam ? 1 : 0,
        valo_vp, valo_nav_upd, nvis, valo_hud, kph);
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
    // PE MainLoop RE pumps @ 0x428CC9 / 0x428CD4 (soft stand-in).
    game_boot_mainloop_resource_pumps();
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
          std::printf(
              "  PATH-TO-WORLD: CMD_NEW (MainMenuDialog.osdCommand=50) → "
              "loadDefaults + career + Garage\n");
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
            "  PATH-TO-WORLD: Garage career=%d money=%d car=%d — next Hit "
            "the Street → Valocity\n",
            game_logic_career_in_progress(),
            game_logic_player() ? tree_field_get_int(game_logic_player(), "money")
                                : -1,
            car_ok);
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