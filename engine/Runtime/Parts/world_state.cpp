#include "world_state.hpp"

namespace inv {
namespace world_state {

std::unordered_map<InvObject*, WheelRefState> g_wheelrefs;
std::unordered_map<InvObject*, std::array<std::string, 4>> g_wheel_dmg;
std::unordered_map<InvObject*, std::vector<SfxItem>> g_sfxtables;
std::unordered_map<InvObject*, DynoState> g_dyno;
std::unordered_map<InvObject*, std::vector<BuckEntry>> g_bucks;
std::unordered_map<InvObject*, std::unordered_map<int32_t, std::string>> g_slot_dmg;
std::unordered_map<InvObject*, AnimState> g_anims;
std::unordered_map<InvObject*, ParticleState> g_particles;
std::unordered_map<InvObject*, std::vector<PaintStroke>> g_painter;

WheelRefState& WR(InvObject* self) { return g_wheelrefs[self]; }
AnimState& AN(InvObject* self) { return g_anims[self]; }
ParticleState& PS(InvObject* self) { return g_particles[self]; }

void anim_advance(AnimState& a) {
  const float now = time_current();
  if (a.last_t < 0.f) a.last_t = now;
  const float dt = now - a.last_t;
  a.last_t = now;
  if (!a.playing || dt <= 0.f) return;
  a.pos += dt * a.speed;
  if (a.duration < 0.01f) a.duration = 1.f;
  if (a.loop) {
    while (a.pos >= a.duration) a.pos -= a.duration;
    while (a.pos < 0.f) a.pos += a.duration;
  } else if (a.pos >= a.duration) {
    a.pos = a.duration;
    a.playing = false;
  } else if (a.pos < 0.f) {
    a.pos = 0.f;
    a.playing = false;
  }
}

std::string alias_key(InvObject* alias) {
  const char* s = alias ? string_cstr(alias) : nullptr;
  return s ? std::string(s) : std::string();
}

}  // namespace world_state
}  // namespace inv
