#include "natives.hpp"
#include "Resources.h"
#include "Resources_internal.hpp"
#include "host_objects.hpp"
#include "runtime.hpp"
#include "rpak.hpp"
#include "render_d3d9.hpp"
#include "tree_interp.hpp"
#include "audio_win32.hpp"
#include "GameRef.h"
#include <mutex>
#include <cmath>
#include <cstring>
#include <cstdio>
namespace inv {

// ---- GroundRef PATH-TO-WORLD soft (project / nearest cross / spawn) ----
// Stock City / RaceSetup / Valocity: getNearestCross → getStartDirection →
// alignToRoad. Traffic_trySpawnNearCross @ 0x00581E00 also snaps via
// GroundMap_findNearestCross before path spawn.

namespace {

// Soft PE GroundMap_alignToRoad @ 0x00583A00: after RouteSpline_paramAtXZ,
// if param ≥ *(path+96)*0.5 → negate RouteSpline_evalTangent @ 0x0057EB00.
// Host has no path+96; soft recover half via ±50 m probes along tan then
// compare projected arc proxy (dist from "start" probe ≥ half span).
void soft_align_half_path_flip(float qx, float qz, float* dx, float* dy,
                               float* dz) {
  if (!dx || !dz) return;
  const float llen = std::sqrt((*dx) * (*dx) + (*dz) * (*dz));
  if (llen < 1e-6f) return;
  const float ux = (*dx) / llen;
  const float uz = (*dz) / llen;
  float ax = qx, ay = 0.f, az = qz, adx = 0.f, ady = 0.f, adz = 1.f;
  float bx = qx, by = 0.f, bz = qz, bdx = 0.f, bdy = 0.f, bdz = 1.f;
  const bool ha =
      physics_road_project(qx - ux * 50.f, qz - uz * 50.f, &ax, &ay, &az, &adx,
                           &ady, &adz);
  const bool hb =
      physics_road_project(qx + ux * 50.f, qz + uz * 50.f, &bx, &by, &bz, &bdx,
                           &bdy, &bdz);
  if (!ha || !hb) return;
  const float dax = qx - ax, daz = qz - az;
  const float dbx = bx - ax, dbz = bz - az;
  const float span = std::sqrt(dbx * dbx + dbz * dbz);
  if (span < 1e-3f) return;
  const float from_start = std::sqrt(dax * dax + daz * daz);
  if (from_start >= span * 0.5f) {
    *dx = -*dx;
    if (dy) *dy = -*dy;
    *dz = -*dz;
  }
}

// Soft unit XZ tan (City.alignToRoad consumers normalize [1] for lane spacing).
void soft_unitize_xz(float* dx, float* dy, float* dz) {
  (void)dy;
  if (!dx || !dz) return;
  const float llen = std::sqrt((*dx) * (*dx) + (*dz) * (*dz));
  if (llen < 1e-6f) {
    *dx = 0.f;
    *dz = 1.f;
    return;
  }
  *dx /= llen;
  *dz /= llen;
}

}  // namespace

// PE @ 0x00483E60 size 0x1EA (490). GroundRef.alignToRoad(Vector3)→Vector3[2].
// Unbox rp + this (JVM_UnboxArg @ 0x0045D910). rp==null → null. this
// Native.ptr (dword_62E008 @ 0x0042AB50)==0 → null. thiscall
// Engine_queryGameRefChannel(g_EngineState @ 0x00636338, handle, 0x39=57
// GroundMap, 0)==0 → null. Read rp z/y/x; thiscall GroundMap_alignToRoad
// @ 0x00583A00(map, &pos, &tan): GroundMap_lookupPathNearXZ @ 0x00581BF0;
// RouteSpline_paramAtXZ; RouteSpline_evalTangent @ 0x0057EB00 (negated if
// param≥*(path+96)*0.5); RouteSpline_samplePos @ 0x0057E960 snaps pos.
// PE ignores GroundMap_alignToRoad ret — always boxes Vector3[2] via
// Engine_malloc 0x1C×2 + JVM_getClass + JVM_setInstanceArrayField @
// 0x0042B0A0. xref: Natives_RegisterAll data @ 0x004899CA only.
// Host soft: g_res≈Native.ptr; physics_road_project stand-in + soft
// half-path flip; miss keeps input xyz (PE still boxes stack).
InvObject* java_util_resource_GroundRef_alignToRoad(InvObject* self,
                                                    InvObject* rp) {
  // PE @ 0x00483E60 size 0x1EA — GroundMap_alignToRoad soft project.
  if (!rp) return nullptr;
  if (!self) return nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_res.find(self) == g_res.end()) return nullptr;
  }
  float x = 0.f, y = 0.f, z = 0.f;
  vec3_get(rp, &x, &y, &z);
  float ox = x, oy = y, oz = z;
  float dx = 0.f, dy = 0.f, dz = 1.f;
  // PE ignores align fail — keep input; soft default tan (0,0,1).
  if (physics_road_project(x, z, &ox, &oy, &oz, &dx, &dy, &dz)) {
    soft_align_half_path_flip(ox, oz, &dx, &dy, &dz);
    soft_unitize_xz(&dx, &dy, &dz);
  } else {
    ox = x;
    oy = y;
    oz = z;
    dx = 0.f;
    dy = 0.f;
    dz = 1.f;
  }
  InvObject* arr = tree_vector_new();
  tree_vector_add(arr, vec3_new(ox, oy, oz));
  tree_vector_add(arr, vec3_new(dx, dy, dz));
  return arr;
}

// PE @ 0x00483D00 size 0x158 (344). GroundRef.getNearestCross(V,F)V3.
// Unbox this+approx+distance. approx==null → null. Native.ptr via
// dword_62E008 on this; 0 OR Engine_queryGameRefChannel(handle, 0x39=57,
// 0)==0 → null. Read approx xyz (z/y/x order in PE);
// GroundMap_findNearestCross @ 0x00581A00 (a6=0): minimize
// |len(node-approx)-distance| over two cross grids. Box Vector3 (malloc
// 0x1C + Class vtbl off_5E7354) from result+0x24/28/2C — PE boxes even
// when i==0 (UB). PATH-TO-WORLD: RaceSetup/City/Valocity spawn +
// Traffic_trySpawnNearCross @ 0x00581E00 (dist arg a5, filterY=0).
// Host soft: g_res≈ch57; physics_road_nearest_cross; miss → approx@ground_y.
InvObject* java_util_resource_GroundRef_getNearestCross(InvObject* self,
                                                        InvObject* approx,
                                                        float distance) {
  // PE @ 0x00483D00 — GroundRef.getNearestCross(V,F)V3 soft spawn snap.
  if (!self || !approx) return nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_res.find(self) == g_res.end()) return nullptr;
  }
  float x = 0.f, y = 0.f, z = 0.f;
  vec3_get(approx, &x, &y, &z);
  InvObject* cross = physics_road_nearest_cross(x, y, z, distance);
  // Soft PE always boxes — never return null after gate (physics may still
  // hand back approx@ground when graph empty).
  if (!cross) return vec3_new(x, physics_ground_y(), z);
  return cross;
}

// PE @ 0x00483400 size 0x1d5 (469). GroundRef.getStartDirection(V,V)V3.
// Unbox this+from+to. Native.ptr + Engine_queryGameRefChannel(handle,
// 0x39=57,0) fail → null. Read from/to xyz; GroundMap_getStartDirection
// @ 0x00583840: findNearestCross(from,0,filterY=1) + findNearestCross(to,
// 0,filterY=1) → GroundMap_findRoute @ 0x005826F0; RouteSpline_paramAtXZ +
// RouteSpline_evalTangent at start; free temp spline (cache untouched).
// Miss → zero dir. JNI: |dir|==0 → null; else box NEGATED (-dx,-dy,-dz).
// PATH-TO-WORLD spawn: City.startRace / RaceSetup.enter loop until
// getStartDirection(pStart,pFinish) non-null.
// Host soft: g_res≈ch57; snap both ends via nearest_cross(0) then
// physics_road_start_direction; negate; |dir|~0 → null.
InvObject* java_util_resource_GroundRef_getStartDirection(InvObject* self,
                                                          InvObject* from,
                                                          InvObject* to) {
  // PE @ 0x00483400 size 0x1d5 — soft spawn start dir (snap+route tan).
  if (!self) return nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_res.find(self) == g_res.end()) return nullptr;
  }
  float fx = 0, fy = 0, fz = 0, tx = 0, ty = 0, tz = 0;
  if (from) vec3_get(from, &fx, &fy, &fz);
  if (to) vec3_get(to, &tx, &ty, &tz);
  // Soft PE GroundMap_getStartDirection: snap both ends to nearest cross
  // (dist=0; host lacks filterY=a6=1 |dy|<5 grid filter — 3D minimize).
  if (physics_road_count() > 0) {
    if (InvObject* c0 = physics_road_nearest_cross(fx, fy, fz, 0.f))
      vec3_get(c0, &fx, &fy, &fz);
    if (InvObject* c1 = physics_road_nearest_cross(tx, ty, tz, 0.f))
      vec3_get(c1, &tx, &ty, &tz);
  }
  InvObject* dir =
      physics_road_start_direction(fx, fy, fz, tx, ty, tz);
  if (!dir) return nullptr;
  float dx = 0, dy = 0, dz = 0;
  vec3_get(dir, &dx, &dy, &dz);
  const float len2 = dx * dx + dy * dy + dz * dz;
  if (len2 < 1e-12f) return nullptr;  // PE zero dir → null
  // PE always stores -dir after GroundMap_getStartDirection.
  vec3_set(dir, -dx, -dy, -dz);
  return dir;
}

// PE @ 0x00483750 size 0x20e (526). GroundRef.findRoute /
// getRouteLength(V,V)F — shared native body (registry findRoute sig @
// 0x00616AD8, getRouteLength(V,V) @ 0x00616A24; host stubs both → here).
// Default -1.0f. JVM_UnboxArg @ 0x0045D910 (this+p1); this/p2 null → -1.
// Native.ptr dword_62E008 + Engine_queryGameRefChannel(handle,0x39=57,0)
// GroundMap gate → -1. Read p1/p2 xyz. GroundMap_findRoute @ 0x005826F0
// (a8=0,a9=0); null → -1, cache untouched. RouteSpline_paramAtXZ @
// 0x0057EF20 (thiscall spline, xz → u,dist) at each endpoint →
// GroundRef_routeParamStart/End (6408D8/6408DC); return fabs(u1-u0) →
// GroundRef_cachedRouteLength (6408D4). Prior GroundRef_cachedRoute
// (6408D0): RouteSpline_freeSegBuf @ 0x0057DA40 + Engine_free. Cache
// spline ptr + endpoint coords 6408E0..6408F4. xrefs: Natives_RegisterAll
// 0x4899AB/0x4899E9. Host: g_res ≡ Native.ptr; physics_road_route_length
// builds polyline + restores cache on miss; route_arc_param_xz_locked ≡
// RouteSpline_paramAtXZ on g_last_route.
float java_util_resource_GroundRef_findRoute(InvObject* self, InvObject* p1,
                                             InvObject* p2) {
  // PE @ 0x00483750 — shared findRoute / getRouteLength(V,V)F body.
  if (!self || !p1 || !p2) return -1.f;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_res.find(self) == g_res.end()) return -1.f;  // Native.ptr==0
  }
  float x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
  vec3_get(p1, &x0, &y0, &z0);
  vec3_get(p2, &x1, &y1, &z1);
  bool ok = false;
  const float len =
      physics_road_route_length(x0, y0, z0, x1, y1, z1, &ok);
  return ok ? len : -1.f;
}

// PE @ 0x00483C00 size 0x17 (23). GroundRef.getRouteLength()F.
// No this / no JVM_UnboxArg (static-shaped body; Java still instance).
// mov eax, GroundRef_cachedRoute @ 0x6408D0; test; jz → fld 0.0; else
// fld GroundRef_cachedRouteLength @ 0x6408D4. Writer: findRoute /
// getRouteLength(V,V)F @ 0x00483750. Host: g_last_route size<2 ≡ null
// spline → 0; else g_last_route_len ≡ 6408D4. Ignores self (PE fld-only).
float java_util_resource_GroundRef_getRouteLength(InvObject* self) {
  // PE @ 0x00483C00 size 0x17 — cachedRouteLength @ 0x6408D4.
  (void)self;
  return physics_road_last_route_length();
}

// PE @ 0x00483B30 size 0xD0 (208). GroundRef.getRoutePos(F)Vector3.
// Callees: JVM_UnboxArg @ 0x0045D910 (this + t). this unused after unbox.
// Gate dword_6408D0 (cached route spline from findRoute @ 0x00483750 /
// getRouteLength(V,V)F); span dword_6408D4. dword_6408D0==0 → null.
// t unclamped: u=(1-t)*dword_6408D8+t*dword_6408DC (RouteSpline_paramAtXZ
// arc params at route endpoints); Traffic_path_sample_pos_rev @ 0x0057EA00
// (spline,u,0) → xyz. Y kept (plotRoute → 0xBA03126F). Engine_malloc 0x1C
// + JVM_getClass("java.lang.Vector3") + set float x/y/z.
// Host: g_last_route + u0/u1; physics_road_route_sample; miss → nullptr.
InvObject* java_util_resource_GroundRef_getRoutePos(InvObject* self, float t) {
  // PE @ 0x00483B30 size 0xD0 — sample cached route spline.
  (void)self;
  float x = 0.f, y = 0.f, z = 0.f;
  if (!physics_road_route_sample(t, &x, &y, &z)) return nullptr;
  return vec3_new(x, y, z);
}

// PE @ 0x00483C20 size 0xd9 (217). GroundRef.getRouteDist(V)F.
// Unbox this+pos (this unused after). Gate GroundRef_cachedRoute
// (6408D0)==0 OR GroundRef_cachedRouteLength (6408D4)<=0 → 0.0.
// pos==null → -1.0. Else read x/z (y discarded); RouteSpline_paramAtXZ
// @ 0x0057EF20 → u; return (u - routeParamStart) / (End - Start).
// Writer: findRoute @ 0x00483750. Host: g_last_route_len≈span;
// physics_road_route_param.
float java_util_resource_GroundRef_getRouteDist(InvObject* self, InvObject* pos) {
  // PE @ 0x00483C20 size 0xd9 — RouteSpline_paramAtXZ on cached route.
  (void)self;
  if (physics_road_last_route_length() <= 0.f) return 0.f;
  if (!pos) return -1.f;
  float x = 0, y = 0, z = 0;
  vec3_get(pos, &x, &y, &z);
  return physics_road_route_param(x, y, z);
}

// PE @ 0x00484E20 size 0x49 (73). GroundRef.removePedestrianType(GameRef)V
// — PE name string (Java: remPedestrianType). UnboxArg (L)V: dest0=this
// dest1=g — swapped vs addPedestrianType @ 0x00484D90. Gate: Native.ptr
// dword_62E008 on THIS (not g), then Engine_queryGameRefChannel @
// 0x00426470 (handle, channel 0x3D=61, out=0); eax==0 → early out.
// Success: Pedestrian_remType @ 0x00589A20 (ecx=channel, push g). rem: no
// mesh prep (contrast add → Pedestrian_addType @ 0x00589940 after LOD).
// remType: scan 16-byte slots @ eng+13256 keyed [g+8]; miss=no-op; hit
// unlink +0x48, count-- @+13244, compact. Host: g_res miss ≡ Native.ptr 0
// → early out; else remPedestrianType erases ped_samples by
// ResourceRef_id (miss no-op). Channel-61 object not mirrored.
void java_util_resource_GroundRef_removePedestrianType(InvObject* self,
                                                       InvObject* g) {
  // PE @ 0x00484E20 size 0x49 — remPedestrianType / Pedestrian_remType.
  if (!self) return;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_res.find(self) == g_res.end()) return;  // Native.ptr==0
  }
  java_util_resource_GroundRef_remPedestrianType(self, g);
}

}  // namespace inv
