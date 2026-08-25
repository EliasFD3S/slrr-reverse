// Shared host state for Runtime/Parts (+ Cars Painter, Resources anim/particles).
#pragma once

#include "natives.hpp"
#include "runtime.hpp"

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace inv {
namespace world_state {

struct WheelRefState {
  float px = 0, py = 0, pz = 0;
  float oy = 0, op = 0, or_ = 0;
  bool has_pos = false;
  bool has_ypr = false;
  float drive = 1.f, steer = 0;
  float radius = 0.32f;
  float width = 0.225f;
  float cpatch_hw = 0, cpatch_ang = 0, cpatch_off = 0;
  float friction = 1.f, frictn_x = 1.f, sliction = 1.f, stiffness = 1.f;
  float roll_res = 0, bearing = 0, max_load = 0, load_smooth = 0;
  // PE setPacejka @ 0x00441210: 17 slots [handle+0x1E8+4*i], i=0..16.
  float pacejka[17] = {1.4f, 0.f, 1.49f, 0.f, 15.20f, 0.f, 0.f, 0.f,
                       -1.f, 0.f, 0.f, 8000.f, 1.f, 0.015f, 0.4f, 0.f, 0.f};
  float force = 0, damping = 0, damp_bound = 0, damp_rebound = 0;
  float rest_len = 0, min_len = 0, max_len = 0;
  float ic[6] = {};
  float brake = 0, hbrake = 0;
  int32_t opp_wheel = -1;
  float arm[7] = {};
  float hub[10] = {};
  bool has_arm = false;
  bool has_hub = false;
};

struct SfxItem {
  InvObject* sfx = nullptr;
  float pitch = 0, pmin = 0, pmax = 0, vmin = 0, vmax = 0;
};

struct DynoState {
  std::vector<float> nm;
  float max_rpm = 7000.f;
  int32_t steps = 0;
};

struct BuckEntry {
  int32_t part_id = 0, buck_id = 0;
  float freq = 0, prob = 0, rpmdep = 0, amp = 0;
};

// PE Animation native @ 0x0047ED80..0x0047F050: 8-byte queue {op, arg}.
// 0=play 1=loopPlay 3=pause 4=seek 5=setSpeed 6=setFade (2 unused in Java).
struct AnimOp {
  int32_t op = 0;
  float arg = 0.f;
};

struct AnimState {
  float speed = 1.f;
  float fade = 0.f;
  float pos = 0.f;
  float duration = 1.f;
  bool playing = false;
  bool loop = false;
  float last_t = -1.f;
  std::vector<AnimOp> queue;
};

struct ParticleAction {
  enum Kind : int32_t { None = 0, Source = 1, Direct = 2, Counter = 3 };
  Kind kind = None;
  float px = 0, py = 0, pz = 0;
  float rmin = 0, rmax = 0;
  float vx = 0, vy = 0, vz = 0;
  float vmin = 0, vmax = 0;
  float rate = 0;
  std::string bone;
  int32_t counter = 0;
};

struct ParticleState {
  InvObject* parent = nullptr;
  InvObject* type = nullptr;
  std::string sys_alias;
  float freq = 0.f;
  bool permanent = false;
  bool stopped = false;
  std::unordered_map<std::string, ParticleAction> actions;
};

struct PaintStroke {
  InvObject* cursor = nullptr;
  int32_t color = 0;
  int32_t brush = 0;
  int32_t temp = 0;
  float rot = 0, size = 1.f;
  int32_t flip = 0;
  bool part_fill = false;
};

extern std::unordered_map<InvObject*, WheelRefState> g_wheelrefs;
extern std::unordered_map<InvObject*, std::array<std::string, 4>> g_wheel_dmg;
extern std::unordered_map<InvObject*, std::vector<SfxItem>> g_sfxtables;
extern std::unordered_map<InvObject*, DynoState> g_dyno;
extern std::unordered_map<InvObject*, std::vector<BuckEntry>> g_bucks;
extern std::unordered_map<InvObject*, std::unordered_map<int32_t, std::string>>
    g_slot_dmg;
extern std::unordered_map<InvObject*, AnimState> g_anims;
extern std::unordered_map<InvObject*, ParticleState> g_particles;
extern std::unordered_map<InvObject*, std::vector<PaintStroke>> g_painter;

WheelRefState& WR(InvObject* self);
AnimState& AN(InvObject* self);
ParticleState& PS(InvObject* self);
void anim_advance(AnimState& a);
std::string alias_key(InvObject* alias);

}  // namespace world_state

// Bring symbols into inv:: for native bodies (drop world_state:: prefix).
using world_state::WheelRefState;
using world_state::SfxItem;
using world_state::DynoState;
using world_state::BuckEntry;
using world_state::AnimOp;
using world_state::AnimState;
using world_state::ParticleAction;
using world_state::ParticleState;
using world_state::PaintStroke;
using world_state::g_wheelrefs;
using world_state::g_wheel_dmg;
using world_state::g_sfxtables;
using world_state::g_dyno;
using world_state::g_bucks;
using world_state::g_slot_dmg;
using world_state::g_anims;
using world_state::g_particles;
using world_state::g_painter;
using world_state::WR;
using world_state::AN;
using world_state::PS;
using world_state::anim_advance;
using world_state::alias_key;

}  // namespace inv
