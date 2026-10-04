#pragma once
#include "host_objects.hpp"
#include <cstdint>
namespace inv {

extern float g_measure;
extern float g_measure_div3600;
extern float g_ten_div_measure;
extern int32_t g_load_depth;
extern int32_t g_load_peak;
extern int32_t g_load_opens;
extern int32_t g_ld_priority;
extern float g_ld_work_scale;
extern int32_t g_ld_high;
extern int32_t g_config_apply_count;
extern int32_t g_engine_is_night;
extern InvObject* g_config_host;
extern int32_t g_engine_headlight_rays;
extern int32_t g_engine_flares;
extern int32_t g_engine_shadow_size;
extern int32_t g_engine_shadows;
extern float g_engine_shadow_detail;
extern int32_t g_engine_texture_size;
extern float g_engine_object_detail;
extern float g_engine_object_detail_dup;
extern float g_engine_object_detail_amp;
extern int32_t g_engine_texture_format;
extern float g_engine_video_gamma_inv;
extern float g_engine_particle_density;
extern int32_t g_engine_skidmark_max;
extern int32_t g_engine_texture_save_q;
extern float g_engine_external_damage;
extern float g_engine_internal_damage;
extern float g_engine_deformation;
extern int32_t g_engine_mem_vertex_max;
extern int32_t g_engine_mem_vertex_min;
extern int32_t g_engine_mem_texture_max;
extern int32_t g_engine_mem_texture_min;
extern int32_t g_engine_mem_instance_max;
extern int32_t g_engine_mem_instance_min;
extern int32_t g_engine_mem_sound_max;
extern int32_t g_engine_mem_sound_min;
extern int32_t g_engine_resource_loadrate;
extern int32_t g_engine_force_feedback;
extern float g_engine_ffb_strength;
extern float g_engine_ffb_strength_emu;
extern float g_engine_engine_inertia;
extern float g_engine_wheel_gnd_feedback;
extern float g_engine_wheel_brake_factor;
extern int32_t g_engine_mouse_help;
extern float g_engine_steerhelp_turn;
extern float g_engine_head_move_steer;
extern float g_engine_head_move_vel;
extern float g_engine_head_move_acc;
extern int32_t g_asyncload_frame;

void eng_ctrl_register_gi(int32_t gi_handle, InvObject* script);
void eng_ctrl_unregister_gi(int32_t gi_handle);
void* asyncload_submit_impl(std::uintptr_t type, int32_t async, const char* path,
                           void* a4, void* a5, void* a6);
int asyncload_has_work();
int asyncload_pump_one();

void asyncload_finish_slice();
void engine_mainloop_endframe();
void engine_simulate_frame();
void loading_enter();
void loading_leave();

}  // namespace inv
