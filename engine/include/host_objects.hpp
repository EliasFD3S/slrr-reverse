#pragma once

#include "natives.hpp"

#include <cstdint>

namespace inv {

// Host helpers to bind rewrite-side native state onto opaque InvObject*.
InvObject* file_new(const char* path);
InvObject* findfile_new();
InvObject* thread_new(const char* name);
InvObject* vec3_new(float x, float y, float z);
InvObject* ypr_new(float y, float p, float r);
void vec3_get(InvObject* o, float* x, float* y, float* z);
void ypr_get(InvObject* o, float* y, float* p, float* r);
void vec3_set(InvObject* o, float x, float y, float z);
void ypr_set(InvObject* o, float y, float p, float r);
bool vec3_is(InvObject* o);
bool ypr_is(InvObject* o);
InvObject* gameref_new();
InvObject* resref_new();
void gameref_on_res_bound(InvObject* self);
void gameref_on_destroy(InvObject* self);
void resref_ensure(InvObject* self);
InvObject* resref_find_by_id(int32_t id);
void resref_set_parent(InvObject* self, InvObject* parent);

// GameLogic.initVehicleTypes host path (create+init all *_VT under cars:0x1000).
int32_t game_logic_init_vehicle_types();
InvObject* game_logic_vehicle_types();  // java.util.Vector of VehicleType hosts

// Weighted pick + descriptor build (mirrors VehicleType.getVehicleDescriptor).
InvObject* game_logic_get_vehicle_type(int32_t set);
InvObject* vehicle_type_get_vehicle_descriptor(InvObject* vt, int32_t set,
                                               float param);
InvObject* game_logic_get_vehicle_descriptor(int32_t set, float param);

// Input test hooks (no DirectInput yet).
void input_set_axis(int32_t device, int32_t axis, float value);
void input_set_last_key(int32_t key, bool edge_enqueue = true);
// PE Input_cheatRing @ 0x00640924 (16 bytes). lastKey writes ASCII; kismajomCheck
// matches encoded[c]-1 walking backward and zeros the last byte on hit.
void input_cheat_clear();
const char* input_cheat_buffer();
int32_t input_cheat_try_match_encoded(const char* enc);
int32_t input_dik_from_letter(char letter);

// Phase 2.14 — physical→logical axis maps (Input.mapAxis / Controller.user_*).
void input_map_add(InvObject* inst, int32_t vaxis, int32_t device, int32_t paxis,
                   float i_from, float i_to, float l_from, float l_to);
int32_t input_map_del(InvObject* inst, int32_t vaxis, int32_t device,
                      int32_t paxis);
void input_map_reset(InvObject* inst);
float input_map_get_logical(InvObject* inst, int32_t vaxis);
int32_t input_map_count(InvObject* inst);
// Phase 2.32 — override logical axis (Controller.user_SetAxisForce).
void input_map_set_force(InvObject* inst, int32_t vaxis, float value);
void input_map_clear_force(InvObject* inst, int32_t vaxis);
// Phase 2.107 — VirtualAxisSmoothProperties rate filter.
void input_map_set_smooth(InvObject* inst, int32_t vaxis, float center_range,
                          float factor_center, float factor_opposite,
                          float factor_same, float power);
void input_map_set_speed(InvObject* inst, int32_t vaxis, float speed);

// Phase 2.110 — Config mirror for System.getConfigOptions.
InvObject* system_config_host();
InvObject* system_config_host_for_test();
// Phase 2.113 — LoadingScreen / openLib isLoading depth probes.
int32_t system_loading_peak_for_test();
int32_t system_loading_opens_for_test();
int32_t system_ld_priority_for_test();

// Phase 2.18 — host physics body (PhysicsRef + Vehicle speed).
void physics_set_velocity(InvObject* self, float vx, float vy, float vz);
void physics_set_ang_vel(InvObject* self, float wx, float wy, float wz);
void physics_set_asleep(InvObject* self, int32_t asleep);
int32_t physics_is_asleep(InvObject* self);
void physics_integrate(float dt);
float physics_speed_square(InvObject* self);
int32_t physics_shape(InvObject* self);  // 0 none, 1 box, 2 sphere
void physics_extents(InvObject* self, float* a, float* b, float* c);
// Cursor_tick short EVENT_CURSOR: OSD gadgets are PhysicsRef.createBox(z=0.001)
// parented to Group. Map NDC through Osd.convertTextCoordinates (SCALE_3D=1.469)
// and AABB vs half-extents. out_* may be null.
bool physics_pick_osd_gadget(float ndc_x, float ndc_y, InvObject** out_phy,
                             InvObject** out_group, float* out_px, float* out_py,
                             float* out_pz);
// Phase 2.19 — flat ground + arcade drive from Controller axes.
void physics_set_ground_y(float y);
float physics_ground_y();
void physics_drive(InvObject* self, InvObject* controller, float dt);
// Phase 2.66 — WheelRef aggregate: steer (rad-ish axis), drive [0..2], radius m.
void physics_set_wheel_params(InvObject* self, float steer, float drive,
                              float radius);
// Phase 2.67 — friction×sliction → grip; brake/hbrake 0..1; roll_res → drag.
void physics_set_wheel_contact(InvObject* self, float friction, float sliction,
                               float brake, float hbrake, float roll_res);
// Phase 2.68 — Pacejka B/C/D (Wheel.java setPacejka 4/2/0) → lateral grip.
void physics_set_wheel_pacejka(InvObject* self, float b, float c, float d);
// Phase 2.69 — spring N/m, damp N/(m/s), rest_len m, arm_len m.
void physics_set_wheel_suspension(InvObject* self, float spring, float damp,
                                  float rest_len, float arm_len);

// Phase 2.81 — feed Chassis/DynoData Nm into arcade accel; RPM estimate.
void physics_set_drive_torque(InvObject* self, float nm);
float physics_get_engine_rpm(InvObject* self);
int32_t physics_is_airborne(InvObject* self);

// Phase 2.67 — read WheelRef contact fields (host; no Java getters).
float wheelref_get_friction(InvObject* self);
float wheelref_get_sliction(InvObject* self);
float wheelref_get_brake(InvObject* self);
float wheelref_get_hbrake(InvObject* self);
float wheelref_get_roll_res(InvObject* self);
float wheelref_get_pacejka(InvObject* self, int32_t i);
bool wheelref_get_arm(InvObject* self, float out[7]);
bool wheelref_get_hub(InvObject* self, float out[10]);
float wheelref_get_force(InvObject* self);
float wheelref_get_damp_bound(InvObject* self);
float wheelref_get_rest_len(InvObject* self);
float wheelref_get_arm_len(InvObject* self);
// Phase 2.33 — arcade gearbox (-1..5); clutch from controller axes.
int32_t physics_get_gear(InvObject* self);
void physics_set_gear(InvObject* self, int32_t gear);

// Phase 2.25 — body–body collision (AABB / sphere).
void physics_set_collide_active(InvObject* self, int32_t on);
int32_t physics_collide_active(InvObject* self);
int32_t physics_collide_events();  // contacts resolved in last integrate

// Phase 2.22 — host road polyline for GroundRef.alignToRoad.
void physics_road_clear();
void physics_road_add_segment(float x0, float y0, float z0, float x1, float y1,
                              float z1);
int32_t physics_road_count();
// Traffic_trySpawnOnRandomPath @ 0x0057B420: pick a random empty polyline
// (path+196==0), place along it. All occupied / empty graph → false.
bool physics_road_random_spawn(float* out_x, float* out_y, float* out_z,
                               float* out_yaw);
// haltTrafficPath zeros all +196 then marks corridor paths occupied.
void physics_road_clear_occupied();
void physics_road_mark_occupied_at(float x, float y, float z);
int32_t physics_road_occupied_count();
// Project (x,z) onto nearest segment; returns false if no roads.
bool physics_road_project(float x, float z, float* out_x, float* out_y,
                          float* out_z, float* out_dx, float* out_dy,
                          float* out_dz);

// Phase 2.23 — road graph queries (junctions / path).
InvObject* physics_road_nearest_cross(float ax, float ay, float az,
                                      float min_dist);
InvObject* physics_road_start_direction(float fx, float fy, float fz, float tx,
                                        float ty, float tz);
float physics_road_route_length(float x0, float y0, float z0, float x1,
                                float y1, float z1, bool* pe_ok = nullptr);
// Phase 2.88 — last findRoute polyline (for plotRoute / getRoutePos).
int32_t physics_road_last_route_count();
bool physics_road_last_route_point(int32_t i, float* x, float* y, float* z);
float physics_road_last_route_length();
bool physics_road_route_sample(float t, float* x, float* y, float* z);
float physics_road_route_param(float x, float y, float z);
int32_t render_line_point_count(InvObject* self);
bool render_line_point_at(InvObject* self, int32_t i, float* x, float* y,
                          float* z);
int32_t render_line_color(InvObject* self);

// Phase 2.24 — seed Valocity club-garage spine (placeholder until map parse).
int32_t physics_road_seed_valocity();
// Phase 2.29 — add centerline segment(s) from an SCX road mesh (PCA on XZ).
int32_t physics_road_add_from_scx(const char* path);
// Phase 2.30 — scan RPAK `sourcefile` → road*.scx; remap missing city meshes
// to objects/meshes/<stem>_egyedi/<stem>_egyedi.scx when present.
// Returns segments added; out_meshes (optional) = SCX files ingested.
int32_t physics_road_seed_from_rpak(const char* pack_name, int32_t* out_meshes);
// Phase 2.41 — load visual city SCX (identity pose; verts often world-space).
// Phase 2.52 — also seed `mesh 0x` ground-map instance recipes.
// Phase 2.57 — instance recipes draw first (budget); sourcefiles fill remainder.
int32_t city_mesh_seed_from_rpak(const char* pack_name, int32_t max_meshes);
int32_t city_mesh_count();
int32_t city_mesh_vertex_total();
int32_t city_instance_count();
int32_t city_instance_drawn();
void city_mesh_clear();

// Boot: one default Controller so Input.isPlayerActive(0)==1.
InvObject* input_init_controllers();
int32_t input_is_player_active(int32_t n);
InvObject* input_get_controller(int32_t n);

// Controller.reset / activateState (ControlSetState host).
InvObject* controller_reset(InvObject* ctrl);
void controller_activate_state(InvObject* ctrl, int32_t group, int32_t new_state);
int32_t controller_css_get(InvObject* ctrl, int32_t group);

// Boot slice after initVehicleTypes: host Player + Garage (+ controller).
InvObject* game_logic_boot_player_garage();
InvObject* game_logic_player();
InvObject* game_logic_garage();

// Frontend.loadingScreen + GfxEngine (Frontend.render) + changeActiveSection.
InvObject* frontend_loading_screen();
void frontend_loading_screen_show();
void frontend_loading_screen_hide();
int32_t frontend_loading_screen_visible();
// Phase 2.118: Frontend.render singleton; flush Present → Object.notify.
InvObject* frontend_gfx_engine();
void frontend_gfx_engine_frame_notify();
// Phase 2.119: stock LoadingScreen.run pacing loop (host).
void frontend_loading_screen_run(InvObject* self);
// Phase 2.121: userWait / track / display + SoftTimer / LoadingDialog.
void frontend_loading_screen_track(InvObject* self, int32_t wait_for_user);
void frontend_loading_screen_user_wait(InvObject* self, float sec);
void frontend_loading_screen_display(InvObject* self, InvObject* dlg,
                                    float wait_limit);
void frontend_loading_screen_show_dialog(InvObject* self, InvObject* dlg);
void frontend_soft_timer_run(InvObject* self);
void frontend_flash_text_run(InvObject* self);
void frontend_text_change_text(InvObject* self, InvObject* text);
// Phase 2.120: Frontend.init / setFonts / HotkeyWatcher / static getters.
void frontend_init();
void frontend_start_hotkey_watcher();
void frontend_set_fonts();
void frontend_destroy();
void frontend_hotkey_watcher_run(InvObject* self);
InvObject* frontend_large_font();
InvObject* frontend_medium_font();
InvObject* frontend_small_font();
InvObject* frontend_pointers();
InvObject* frontend_def_loading_pic();
InvObject* frontend_input_queue();
int32_t frontend_inited();
InvObject* frontend_hotkey_thread();

// Phase 2.124 — Input.cursor singleton + MouseCursor.enable (DF_LEAVEPOINTER).
InvObject* java_io_Input_cursor();
int32_t java_io_MouseCursor_enable(InvObject* self, int32_t state);
// PE Cursor_tick @ 0x00460140 SysCursor path: copy WndProc NDC → +0xA4/+0xA8,
// clamp [-1,1]. Java Config.SysCursor default 1.
void java_io_MouseCursor_tickSysCursor();
// PE Cursor_tick EVENT_CURSOR 0x10000 → handleEvent(GameRef,int,String) /
// addNotification custmethod (Osd.event_handler).
void java_lang_GameType_dispatchCursor(InvObject* obj_ref, InvObject* param);
// PE Engine_queueEvent(dest=+0xEC watch list): physics auto-sends to parent
// Group (setEventMask EVENT_CURSOR). Direct handleEvent on dest.
void java_lang_GameType_dispatchCursorTo(InvObject* dest, InvObject* obj_ref,
                                         InvObject* param);

InvObject* game_logic_change_active_section(InvObject* state);
InvObject* game_logic_actual_state();
// Java GameLogic.changeActiveSection(GameState) — null or a GameState
// implementor. TREE leftovers (String/Dialog/empty class) are not sections.
bool game_logic_is_section(InvObject* state);
// Full mid-boot: MENUSET + hide loading + SplashScreen(frontend:0x5).
InvObject* game_logic_boot_splash();
// Splash timer / AXIS_CANCEL → MainMenu (dialog shown).
InvObject* game_logic_finish_splash();

// GameLogic.autoSave / autoSaveQuiet (modals skipped; returns 1 = OK).
int32_t game_logic_auto_save();
void game_logic_auto_save_quiet();
int32_t game_logic_auto_save_calls();
int32_t game_logic_auto_save_quiet_calls();
int32_t game_logic_load_defaults_calls();

// MainMenuDialog CMD_NEW core (no modals) + JVM dispatch via osdCommand(50).
void game_logic_load_defaults();
int32_t game_logic_game_mode();
int32_t game_logic_career_in_progress();
void game_logic_set_career_in_progress(int32_t v);
int32_t game_logic_day();
float game_logic_time();
void game_logic_set_time(float t);
void game_logic_spend_time(float dt);
InvObject* garage_enter(InvObject* garage, InvObject* prev_state);
void garage_ensure_map(InvObject* garage);
void garage_exit(InvObject* garage, InvObject* next_state);
bool garage_try_create_osd_objects(InvObject* garage);
bool racesetup_try_create_osd_objects(InvObject* rs);
bool racesetup_try_cmd_race(InvObject* rs);
void game_logic_set_played(int32_t v);
int32_t game_logic_played();
InvObject* main_menu_apply_new_career(const char* player_name);
InvObject* main_menu_cmd_new(const char* player_name);
InvObject* main_menu_cmd_freeride();
InvObject* main_menu_cmd_quickrace();
InvObject* main_menu_cmd_demo();
InvObject* main_menu_cmd_back_to_garage();
bool main_menu_cmd_exit();
bool main_menu_cmd_options();
bool main_menu_cmd_credits();
void main_menu_hub_begin_tree();
void main_menu_hub_end_tree();
bool main_menu_hub_deferring();
void main_menu_hub_note_cas(InvObject* next);
void main_menu_cmd_new_begin_tree();
void main_menu_cmd_new_end_tree();
bool main_menu_cmd_new_cas_pending();
bool main_menu_cmd_freeride_cas_pending();
bool main_menu_cmd_exit_cas_pending();
void main_menu_cmd_new_note_garage_cas();
bool main_menu_cmd_new_deferring_osd();
void game_logic_set_game_mode(int32_t mode);
int32_t game_logic_timeout();
void game_logic_set_timeout(int32_t t);
InvObject* game_logic_racesetup();
void game_logic_set_racesetup(InvObject* rs);
// Dialog.display host (stock show/wait/hide). Smoke primes StringRequester input.
void dialog_set_smoke_string(const char* s);
// TREE often misbinds Dialog.display onto MainMenuDialog — track NEW modals.
void dialog_note_constructed(InvObject* dialog, const char* class_fqn);

// Player vehicle + Garage OSD commands.
InvObject* player_spawn_starter_car();  // STOCK/DEMO descriptor → player.car
void garage_lock_car(InvObject* garage);
const char* vehicle_is_driveable(InvObject* car);  // nullptr = OK
// Phase 2.132 — Mechanic inventory (list-only, no VisualInventory UI).
InvObject* inventory_new(InvObject* player);
int32_t inventory_size(InvObject* inv);
InvObject* inventory_ensure_player_parts(InvObject* player);
void inventory_add_part_item(InvObject* inv, InvObject* part);
void inventory_move_to(InvObject* src, int32_t index, InvObject* dst);
InvObject* garage_ensure_mechanic(InvObject* garage);
InvObject* garage_ensure_painter(InvObject* garage);
void garage_painter_show(InvObject* garage);
void garage_painter_hide(InvObject* garage);
bool garage_painter_select_can(InvObject* garage, int32_t index);
bool garage_paint_car(InvObject* garage, int32_t color);
void mechanic_flush_inventory(InvObject* mechanic);
void mechanic_filter_inventory(InvObject* mechanic, int32_t filter_engine,
                               int32_t filter_body, int32_t filter_rgear);
// Phase 2.135 — VisualInventory panels (Mechanic parts strip).
InvObject* visual_inventory_new(InvObject* player, float left, float top,
                                float width, float height);
void visual_inventory_init(InvObject* inv, float left, float top, float width,
                           float height);
void visual_inventory_update(InvObject* inv);
void visual_inventory_show(InvObject* inv);
void visual_inventory_hide(InvObject* inv);
int32_t visual_inventory_panel_count(InvObject* inv);
int32_t visual_inventory_attached_count(InvObject* inv);
int32_t visual_inventory_preview_count(InvObject* inv);
bool visual_inventory_focus_hook(InvObject* inv, int32_t panel_index);
void mechanic_tick_preview(InvObject* mechanic);
void visual_inventory_scroll_up(InvObject* inv);
void visual_inventory_scroll_down(InvObject* inv);
// Phase 2.137 — inventory → car install (panel click).
const char* inventory_install_to_car(InvObject* inv, int32_t index,
                                    InvObject* car);
bool visual_inventory_panel_left_click(InvObject* inv, int32_t panel_index);
bool inventory_panel_osd_command(InvObject* panel, int32_t cmd);
bool mechanic_click_actual(InvObject* mechanic);
void mechanic_tick_click(InvObject* mechanic);
void mechanic_begin_drag_object(InvObject* mechanic, InvObject* part);
void mechanic_set_look_part(InvObject* mechanic, InvObject* part);
bool mechanic_lclick_part(InvObject* mechanic, InvObject* part);
bool part_is_tuneable(InvObject* part);
bool part_flap_toggle(InvObject* part);
bool mechanic_tune_part(InvObject* mechanic, InvObject* part, int32_t choice);
InvObject* mechanic_open_tune_dialog(InvObject* mechanic, InvObject* part);
bool mechanic_tune_set_slider(InvObject* mechanic, int32_t cmd, float value);
bool mechanic_tune_menu_command(InvObject* mechanic, int32_t cmd);
InvObject* mechanic_pick_part_at(InvObject* mechanic, float nx, float ny);
bool mechanic_hover_car_at(InvObject* mechanic, float nx, float ny);
bool mechanic_drop_object_at(InvObject* mechanic, InvObject* part, float nx,
                             float ny);
bool mechanic_drag_panel_to(InvObject* mechanic, float x0, float y0, float x1,
                            float y1);
// Phase 2.138 — car → inventory (panel drag-drop).
bool part_uninstall(InvObject* part);
bool visual_inventory_panel_drag_drop(InvObject* inv, int32_t panel_index,
                                     InvObject* part);
// Phase 2.143 — panel↔panel swap + Mechanic actualPanel focus.
void inventory_swap(InvObject* inv, int32_t index_a, int32_t index_b);
int32_t visual_inventory_item_index_by_button(InvObject* inv,
                                              InvObject* button);
bool visual_inventory_panel_swap(InvObject* inv, int32_t panel_index_a,
                                 InvObject* dropped_button);
bool visual_inventory_panel_swap_panels(InvObject* inv, int32_t panel_a,
                                        int32_t panel_b);
void mechanic_set_actual_panel(InvObject* mechanic, int32_t panel_index);
void mechanic_clear_actual_panel(InvObject* mechanic);
InvObject* mechanic_actual_panel(InvObject* mechanic);
int32_t visual_inventory_panel_at(InvObject* inv, float nx, float ny);
bool mechanic_hover_at(InvObject* mechanic, float nx, float ny);
void mechanic_tick_hover(InvObject* mechanic);
// Phase 2.149 — Garage EC_RDRAG orbit camera (RMB + look axes).
InvObject* garage_ensure_camera(InvObject* garage);
void garage_rdrag_begin(InvObject* garage);
void garage_rdrag_end(InvObject* garage);
bool garage_rdrag_orbit(InvObject* garage, float dyaw, float dpitch);
void garage_tick_rdrag(InvObject* garage);
// Phase 2.136 — Mechanic OSD chrome (scroll / category filters).
void mechanic_ensure_chrome(InvObject* mechanic);
void mechanic_osd_command(InvObject* mechanic, int32_t cmd);
int32_t mechanic_chrome_button_count(InvObject* mechanic);
// CMD_TIME=116, CMD_TESTTRACK=110, CMD_CARLOT=111, CMD_CATALOG=113,
// CMD_MECHANIC=117, CMD_HITTHESTREET=109 — returns new section or garage.
InvObject* garage_osd_command(InvObject* garage, int32_t cmd);
// Catalog / CarLot / TestTrack → Garage.
InvObject* game_state_return_to_garage(InvObject* state);
// Phase 2.133 — ensure Dialog OSD buttons after TREE show skips createButton.
void dialog_ensure_osd_buttons(InvObject* dialog);
// Stock Dialog.display(): show → pump until osdCommand sets result → hide.
int32_t dialog_display(InvObject* dialog);
// Modal currently inside dialog_display (null if none) — boot skips parent tick.
InvObject* dialog_modal_active();
// Phase 2.158 — MainMenuDialog host show skips video/3D; blit RID_GENERALBG +
// menu labels so --game is not a black window.
void mainmenu_dialog_ensure_chrome(InvObject* dialog);
// Osd leaves (stock Java createBG/createHotkey) — avoid TREE packing SO.
void osd_ensure_defaults(InvObject* osd);
InvObject* osd_create_bg(InvObject* osd, InvObject* texture);
InvObject* osd_create_rectangle(InvObject* osd, float x, float y, float w,
                                float h, int32_t pri, InvObject* texture);
InvObject* osd_create_hotkey(InvObject* osd, int32_t key, int32_t flags,
                             int32_t command, InvObject* handler, int32_t ef);
InvObject* osd_create_text(InvObject* osd, const char* text, InvObject* font,
                           int32_t align, float x, float y);
InvObject* osd_create_header(InvObject* osd, const char* title);
InvObject* osd_create_menu(InvObject* osd, InvObject* style, float x, float y,
                           float spc, int32_t ori);
InvObject* osd_create_button(InvObject* osd, InvObject* style, float x, float y,
                             const char* label, int32_t cmd);
InvObject* menu_add_item(InvObject* menu, const char* text, int32_t cmd);
InvObject* menu_add_item_gfx(InvObject* menu, InvObject* gfx, int32_t cmd,
                             const char* tooltip);

// Stock Garage.createOSDObjects street strip when TREE packing fails.
void garage_create_osd_street_menu(InvObject* garage);
void menu_add_separator(InvObject* menu);
int32_t osd_begin_group(InvObject* osd);
int32_t osd_end_group(InvObject* osd);
void osd_hide_group(InvObject* osd, int32_t gid);
void osd_show_group(InvObject* osd, int32_t gid);
// OptionsDialog.show groups (options/video/…) for CMD_OPTIONS navigation.
void options_dialog_ensure_groups(InvObject* dialog);
void options_dialog_change_mode(InvObject* dialog, int32_t group);
// MainMenuDialog credits scroll (stock animate CREDITS_*).
void mainmenu_build_credits(InvObject* dialog);
void mainmenu_credits_reset(InvObject* dialog);
void mainmenu_credits_tick(InvObject* dialog, float dt);
// Phase 2.162 — OSD pointer nav (stock Group EC_HOVER / Gadget.focus stand-in).
void osd_tick_pointer(InvObject* osd);
// Phase 2.134 — Osd.createButton host call counter (TREE path proof).
void osd_create_button_calls_reset();
int32_t osd_create_button_calls();

// Valocity ctor-shaped prepare (map/nav) + enter host/finalize.
void valocity_prepare(InvObject* city);
InvObject* valocity_enter(InvObject* city, InvObject* prev_state);
void valocity_finalize_enter(InvObject* city);
void valocity_exit(InvObject* city, InvObject* next_state);
// Phase 2.34 — host frame: ensure car PhysicsRef + arcade drive + sync TREE.
void valocity_ensure_car_physics(InvObject* car);
void valocity_simulate(InvObject* city, float dt);
// Phase 2.42 — player chassis SCX visual mesh synced to car pose.
void valocity_ensure_car_mesh(InvObject* car);
void valocity_sync_car_mesh(InvObject* car);
// Phase 2.70 — wheel mesh steer (front yaw) + roll spin from speed/radius.
void valocity_sync_wheel_visuals(InvObject* car, float dt);
// Phase 2.43 — body shell parts parented under chassis (hood/bumpers/…).
void valocity_ensure_car_parts(InvObject* car);
int32_t valocity_car_part_count(InvObject* car);
int32_t valocity_car_part_slotted(InvObject* car);
int32_t valocity_car_wheel_count(InvObject* car);
int32_t valocity_car_tyre_count(InvObject* car);
// Phase 2.50 — Part slot poses (setSlotPos) + visual mesh bind.
void part_bind_slot_visual(InvObject* part, int32_t slot_id, InvObject* visual,
                           float px_m, float py_m, float pz_m, float oy,
                           float op, float or_);
void part_set_slot_pos(InvObject* part, int32_t slot_id, InvObject* pos,
                       InvObject* ypr);
InvObject* part_slot_visual(InvObject* part, int32_t slot_id);
int32_t part_slot_count(InvObject* part);
int32_t part_slot_id_at(InvObject* part, int32_t index);
// Read slot pose (metres + YPR). Returns false if slot missing.
bool part_slot_get_pose(InvObject* part, int32_t slot_id, float* px, float* py,
                        float* pz, float* oy, float* op, float* or_);
// Phase 2.53 — install graph: parent.slot ↔ child.mateSlot.
bool part_install(InvObject* parent, int32_t parent_slot_id, InvObject* child,
                  int32_t child_slot_id);
bool part_ensure_chassis_cfg_slots(InvObject* car);
bool part_find_cfg_install(InvObject* car, InvObject* part, int32_t* parent_slot,
                           int32_t* child_slot);
InvObject* part_on_slot(InvObject* part, int32_t slot_id);
int32_t part_slot_id_on_slot(InvObject* part, int32_t slot_id);
// Phase 2.80 — garage slot lock (Chassis suspension chain / camshaft).
void part_disable_slot(InvObject* part, int32_t slot_id, int32_t status);
bool part_slot_is_disabled(InvObject* part, int32_t slot_id);
// Phase 2.60 — walk part_parent to chassis/vehicle root.
InvObject* part_car_root(InvObject* part);
// Phase 2.62 — chassis wheel corner 0..3 from install slot 101..104.
int32_t part_wheel_id(InvObject* part);
// Phase 2.35 — chase camera behind player car (host D3D look-at).
void* valocity_camera_key();
void valocity_update_camera(InvObject* city);
// Phase 2.36 — OSD speed/gear HUD (simple20, CarInfo KPH factor).
void* valocity_hud_speed_key();
void* valocity_hud_gear_key();
float valocity_speed_kph(InvObject* car);
void valocity_update_hud(InvObject* city);
// Phase 2.37 — Navigator minimap (vp/cam + OSD tile follow).
void navigator_paint(InvObject* nav);
InvObject* navigator_viewport(InvObject* nav);
InvObject* navigator_camera(InvObject* nav);
int32_t navigator_current_tile(InvObject* nav);
InvObject* navigator_add_marker_static(InvObject* nav, int32_t rtype_id, float px,
                                       float pz, int32_t pri);
InvObject* navigator_add_marker_dynamic(InvObject* nav, int32_t rtype_id,
                                        InvObject* obj);
void navigator_rem_marker(InvObject* nav, InvObject* m);
// Phase 2.38 — Scene daytime fog + skydome + envmap (Scene.java).
int32_t scene_time2config(float time_sec, float rnd);
void valocity_apply_scene(InvObject* city);
int32_t valocity_scene_config(InvObject* city);
// Garage trigger ON/OFF (clubGarage 1..3). Returns activeTrigger.
int32_t valocity_fire_garage_trigger(InvObject* city, int32_t clubGarage,
                                    int32_t event_on);
// 1s EVENT_TIME tick — may return Garage after YesNo accept.
InvObject* valocity_tick(InvObject* city);
// Simulate garage trigger YesNoDialog accept → Garage.enter.
InvObject* valocity_return_to_garage(InvObject* city);

// ControlSet host (binary CTRL file).
InvObject* control_set_new();
int32_t control_set_load(InvObject* cs, const char* path);
int32_t control_set_nitems(InvObject* cs);
int32_t control_set_ndevices(InvObject* cs);
int32_t control_set_count_group(InvObject* cs, int32_t group);
int32_t control_set_file_check(const char* path);

}  // namespace inv
