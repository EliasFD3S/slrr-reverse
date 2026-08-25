#include "runtime.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace inv {
namespace {

std::mutex g_mu;
std::list<std::string> g_string_storage;

#ifdef _WIN32
LARGE_INTEGER g_qpc_freq{};
LARGE_INTEGER g_qpc_start{};
bool g_qpc_ok = false;
#else
using Clock = std::chrono::steady_clock;
Clock::time_point g_start;
#endif

float g_sim = 0.f;
float g_warp = 1.f;
float g_last_wall = 0.f;
int32_t g_game_tod = 0;  // seconds-in-day
bool g_exit = false;

float wall_now() {
#ifdef _WIN32
  if (!g_qpc_ok) {
    return 0.f;
  }
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const double dt =
      double(now.QuadPart - g_qpc_start.QuadPart) / double(g_qpc_freq.QuadPart);
  return static_cast<float>(dt);
#else
  using sec = std::chrono::duration<float>;
  return sec(Clock::now() - g_start).count();
#endif
}

}  // namespace

void time_init() {
  std::lock_guard<std::mutex> lock(g_mu);
#ifdef _WIN32
  g_qpc_ok = QueryPerformanceFrequency(&g_qpc_freq) != 0;
  if (g_qpc_ok) {
    QueryPerformanceCounter(&g_qpc_start);
  }
#else
  g_start = Clock::now();
#endif
  g_sim = 0.f;
  g_warp = 1.f;
  g_last_wall = 0.f;
  g_game_tod = 0;
  g_exit = false;
}

float time_current() {
  std::lock_guard<std::mutex> lock(g_mu);
  return wall_now();
}

float time_sim() {
  std::lock_guard<std::mutex> lock(g_mu);
  const float w = wall_now();
  g_sim += (w - g_last_wall) * g_warp;
  g_last_wall = w;
  return g_sim;
}

float time_warp(float m) {
  std::lock_guard<std::mutex> lock(g_mu);
  // PE @ 0x0047BF80 / Engine_setTimeWarp @ 0x004283F0: always return prior
  // Engine_timeWarp (+0x110); apply only if m >= 0; do not write *simTime.
  const float prev = g_warp;
  if (m >= 0.f) g_warp = m;
  return prev;
}

void time_sync_game(float t) {
  std::lock_guard<std::mutex> lock(g_mu);
  // Original: __ftol(t) % 86400 (0x15180)
  int v = static_cast<int>(t);
  g_game_tod = ((v % 86400) + 86400) % 86400;
}

int32_t time_game_day_seconds() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_game_tod;
}

bool exit_requested() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_exit;
}

void request_exit() {
  std::lock_guard<std::mutex> lock(g_mu);
  g_exit = true;
}

InvObject* string_new(const char* utf8) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_string_storage.emplace_back(utf8 ? utf8 : "");
  // Store pointer to stable string; header unused for now.
  auto* s = new InvString();
  s->utf8 = g_string_storage.back().c_str();
  return reinterpret_cast<InvObject*>(s);
}

const char* string_cstr(InvObject* obj) {
  if (!obj) {
    return "<null>";
  }
  return reinterpret_cast<InvString*>(obj)->utf8
             ? reinterpret_cast<InvString*>(obj)->utf8
             : "";
}

}  // namespace inv
