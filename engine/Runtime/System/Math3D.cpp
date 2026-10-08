#include "host_objects.hpp"
#include "natives.hpp"
#include "runtime.hpp"
#include "tree_interp.hpp"

#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace inv {
namespace {

std::mutex g_mu;

struct Vec3 {
  float x = 0, y = 0, z = 0;
};
struct Ypr {
  float y = 0, p = 0, r = 0;
};

std::unordered_map<InvObject*, Vec3> g_vec;
std::unordered_map<InvObject*, Ypr> g_ypr;

Vec3* vget(InvObject* o) {
  auto it = g_vec.find(o);
  return it == g_vec.end() ? nullptr : &it->second;
}
Ypr* yget(InvObject* o) {
  auto it = g_ypr.find(o);
  return it == g_ypr.end() ? nullptr : &it->second;
}

float bits_f32(unsigned bits) {
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

// Soft PE deepen — host dual-store: prefer native map (vec3_set/ypr_set),
// else TREE fields (PE path = JVM_vm_get_float_field @ 0x0042A560 only).
bool load_vec3(InvObject* o, float& x, float& y, float& z) {
  if (!o) {
    x = y = z = 0.f;
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (Vec3* v = vget(o)) {
      x = v->x;
      y = v->y;
      z = v->z;
      return true;
    }
  }
  x = tree_field_get_float(o, "x");
  y = tree_field_get_float(o, "y");
  z = tree_field_get_float(o, "z");
  return true;
}

bool load_ypr(InvObject* o, float& y, float& p, float& r) {
  if (!o) {
    y = p = r = 0.f;
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (Ypr* t = yget(o)) {
      y = t->y;
      p = t->p;
      r = t->r;
      return true;
    }
  }
  y = tree_field_get_float(o, "y");
  p = tree_field_get_float(o, "p");
  r = tree_field_get_float(o, "r");
  return true;
}

void store_vec3(InvObject* o, float x, float y, float z) {
  vec3_set(o, x, y, z);
  tree_field_set_float(o, "x", x);
  tree_field_set_float(o, "y", y);
  tree_field_set_float(o, "z", z);
}

void store_ypr(InvObject* o, float y, float p, float r) {
  ypr_set(o, y, p, r);
  tree_field_set_float(o, "y", y);
  tree_field_set_float(o, "p", p);
  tree_field_set_float(o, "r", r);
}

// Soft PE Mat3x4 cluster — float stand-in of PE 3x4 stride 0x10 (cols
// right/up/fwd at +0/+4/+8 per row; row pitch 0x10). Used by Vector3/Ypr
// natives only (no java.lang.Matrix JNI). PE Ypr_toMatrix / fromAxisAngle
// / setIdentity leave cells [3]/[7]/[11] unwritten; Soft zeros them on write.

// Soft PE Vec3_store @ 0x00551C70 size 0x17: *this={a,b,c}. Used by
// Vector3.rotate(Ypr) / Vector3.<init>(Ypr) to pack y/p/r before toMatrix.
void soft_vec3_store(float out[3], float a, float b, float c) {
  out[0] = a;
  out[1] = b;
  out[2] = c;
}

// Soft PE Mat3x4_setIdentity @ 0x0054E1D0 size 0x22 (int_convert 34).
// thiscall ecx=&M00. xor eax; edx=0x3F800000 (1.0f). Writes only rot
// cells (+0/+4/+8/+10h/+14h/+18h/+20h/+24h/+28h); leaves pad +0C/+1C/+2C
// unwritten. Soft: zero pad (deterministic stand-in, same as mulLeft Soft).
void mat34_set_identity(float m[12]) {
  const float one = bits_f32(0x3F800000u);
  m[0] = one;
  m[1] = 0.f;
  m[2] = 0.f;
  m[3] = 0.f;
  m[4] = 0.f;
  m[5] = one;
  m[6] = 0.f;
  m[7] = 0.f;
  m[8] = 0.f;
  m[9] = 0.f;
  m[10] = one;
  m[11] = 0.f;
}

// Soft PE Ypr_toMatrix @ 0x0054ECD0 size 0xc1: R=Ry(yaw)*Rx(pitch)*Rz(roll).
// PE writes only rot cells (ecx+0/+4/+8/+10h/+14h/+18h/+20h/+24h/+28h).
void ypr_to_mat34(float m[12], float yaw, float pitch, float roll) {
  const float sy = std::sin(yaw), cy = std::cos(yaw);
  const float sp = std::sin(pitch), cp = std::cos(pitch);
  const float sr = std::sin(roll), cr = std::cos(roll);
  const float sr_sp = sr * sp;
  const float cr_sp = cr * sp;
  m[0] = sr_sp * sy + cr * cy;
  m[1] = cr_sp * sy - sr * cy;
  m[2] = cp * sy;
  m[3] = 0.f;
  m[4] = sr * cp;
  m[5] = cr * cp;
  m[6] = -sp;
  m[7] = 0.f;
  m[8] = sr_sp * cy - cr * sy;
  m[9] = cr_sp * cy + sr * sy;
  m[10] = cp * cy;
  m[11] = 0.f;
}

// Soft PE Mat3x4_fromAxisAngle @ 0x0054EE80 size 0x131 (rotate axis callee).
// thiscall ecx=&M00, (ax,ay,az,angle). Rodrigues; angle radians as-is.
// PE: len2 fcomp flt_5F09C0 (0x358637BD) AH&1 → leave M uninit; else if
// |len2-1| fcomp AH&41h skip normalize when ≤eps. Soft: same thresholds;
// degenerate leaves M untouched (rotate_1 Soft early-out before M*v —
// PE would mul uninit).
void mat34_from_axis_angle(float m[12], float ax, float ay, float az,
                           float angle) {
  const float kEps = bits_f32(0x358637BDu);  // flt_5F09C0
  const float len2 = ax * ax + ay * ay + az * az;
  if (len2 < kEps) return;
  if (std::fabs(len2 - 1.f) > kEps) {
    const float inv = 1.f / std::sqrt(len2);
    ax *= inv;
    ay *= inv;
    az *= inv;
  }
  const float c = std::cos(angle), s = std::sin(angle);
  const float t = 1.f - c;
  const float xx = ax * ax, yy = ay * ay, zz = az * az;
  const float xy = ax * ay, xz = ax * az, yz = ay * az;
  m[0] = c + xx * t;
  m[1] = xy * t - az * s;
  m[2] = xz * t + ay * s;
  m[3] = 0.f;
  m[4] = xy * t + az * s;
  m[5] = c + yy * t;
  m[6] = yz * t - ax * s;
  m[7] = 0.f;
  m[8] = xz * t - ay * s;
  m[9] = yz * t + ax * s;
  m[10] = c + zz * t;
  m[11] = 0.f;
}

// Soft PE Mat3x4_mulLeft @ 0x0054EAB0 size 0x102: this := A * this (3x3 in
// 3x4). PE qmemcpy 48B copies stack (pad [3]/[7]/[11] = garbage). Soft:
// rewrite rotation cells + zero pad (deterministic stand-in).
void mat34_mul_left(float th[12], const float a[12]) {
  float o[12];
  o[0] = a[0] * th[0] + a[1] * th[4] + a[2] * th[8];
  o[1] = a[0] * th[1] + a[1] * th[5] + a[2] * th[9];
  o[2] = a[0] * th[2] + a[1] * th[6] + a[2] * th[10];
  o[3] = 0.f;
  o[4] = a[4] * th[0] + a[5] * th[4] + a[6] * th[8];
  o[5] = a[4] * th[1] + a[5] * th[5] + a[6] * th[9];
  o[6] = a[4] * th[2] + a[5] * th[6] + a[6] * th[10];
  o[7] = 0.f;
  o[8] = a[8] * th[0] + a[9] * th[4] + a[10] * th[8];
  o[9] = a[8] * th[1] + a[9] * th[5] + a[10] * th[9];
  o[10] = a[8] * th[2] + a[9] * th[6] + a[10] * th[10];
  o[11] = 0.f;
  std::memcpy(th, o, sizeof(o));
}

// Soft PE Mat3x4_mulVec @ 0x00436880 size 0x6f — out = M * v (rows 0/1/2
// stride 4; no translation). Same formula as Vector3.rotate(Ypr) @
// 0x004821E7 and rotate(V,F) @ 0x0048234D (inlined).
void mat34_mul_vec(const float m[12], float x, float y, float z, float& ox,
                   float& oy, float& oz) {
  ox = m[0] * x + m[1] * y + m[2] * z;
  oy = m[4] * x + m[5] * y + m[6] * z;
  oz = m[8] * x + m[9] * y + m[10] * z;
}

// Soft PE Ypr_fromMatrix @ 0x00551C90 size 0x81. Gimbal: pitch bits
// 0x40490FDB (π), roll=0, yaw=atan2(m00,-m01). Else yaw/roll atan2 then
// pitch via cos(roll)*m12 with fcom≤0 (AH C0|C3) branch.
void ypr_from_mat34(const float m[12], float& yaw, float& pitch, float& roll) {
  if (m[10] == 0.f && m[2] == 0.f) {
    pitch = bits_f32(0x40490FDBu);
    roll = 0.f;
    yaw = std::atan2(m[0], -m[1]);
    return;
  }
  yaw = std::atan2(m[2], m[10]);
  roll = std::atan2(m[4], m[5]);
  const float cr = std::cos(roll);
  const float v6 = cr * m[6];
  // Soft PE deepen: fcom cos(roll) vs 0.0; test AH C0|C3 (≤0 or unordered).
  if (!(cr > 0.f))
    pitch = std::atan2(v6, -m[5]);
  else
    pitch = std::atan2(-v6, m[5]);
}

// Soft PE Ypr_fromDirection @ 0x00551D20 size 0x99.
// z/x fcomp dbl_5F0C18 (==0 AH&40) both → singularity; else atan2 path.
// Singularity: y fcomp flt_5E73CC AH&41 (≤0) → flt_5F3AB0 (-π/2) else
// flt_5F0CC4 (+π/2); roll=0. Soft: ==0 / <=0 same on normals.
void ypr_from_direction(float x, float y, float z, float& yaw, float& pitch,
                        float& roll) {
  roll = 0.f;
  if (x == 0.f && z == 0.f) {
    yaw = 0.f;
    pitch = (y <= 0.f) ? bits_f32(0xBFC90FDBu) : bits_f32(0x3FC90FDBu);
  } else {
    yaw = std::atan2(-x, -z);
    pitch = std::atan2(y, std::sqrt(x * x + z * z));
  }
}

}  // namespace

InvObject* vec3_new(float x, float y, float z) {
  InvObject* o = tree_host_new("java.lang.Vector3");
  vec3_set(o, x, y, z);
  tree_field_set_float(o, "x", x);
  tree_field_set_float(o, "y", y);
  tree_field_set_float(o, "z", z);
  return o;
}

InvObject* ypr_new(float y, float p, float r) {
  InvObject* o = tree_host_new("java.lang.Ypr");
  ypr_set(o, y, p, r);
  tree_field_set_float(o, "y", y);
  tree_field_set_float(o, "p", p);
  tree_field_set_float(o, "r", r);
  return o;
}

bool vec3_is(InvObject* o) {
  if (!o) return false;
  std::lock_guard<std::mutex> lock(g_mu);
  return vget(o) != nullptr;
}

bool ypr_is(InvObject* o) {
  if (!o) return false;
  std::lock_guard<std::mutex> lock(g_mu);
  return yget(o) != nullptr;
}

void vec3_get(InvObject* o, float* x, float* y, float* z) {
  std::lock_guard<std::mutex> lock(g_mu);
  Vec3* v = vget(o);
  if (!v) {
    *x = *y = *z = 0;
    return;
  }
  *x = v->x;
  *y = v->y;
  *z = v->z;
}

void ypr_get(InvObject* o, float* y, float* p, float* r) {
  std::lock_guard<std::mutex> lock(g_mu);
  Ypr* t = yget(o);
  if (!t) {
    *y = *p = *r = 0;
    return;
  }
  *y = t->y;
  *p = t->p;
  *r = t->r;
}

void vec3_set(InvObject* o, float x, float y, float z) {
  if (!o) return;
  std::lock_guard<std::mutex> lock(g_mu);
  g_vec[o] = Vec3{x, y, z};
}

void ypr_set(InvObject* o, float y, float p, float r) {
  if (!o) return;
  std::lock_guard<std::mutex> lock(g_mu);
  g_ypr[o] = Ypr{y, p, r};
}

// Soft PE @ 0x004823E0 — java.lang.Vector3.length()F size 0x65
// UnboxArg this @ 0x0045D910. get_float x/y/z @ 0x0061340C/10/14
// (JVM_vm_get_float_field @ 0x0042A560). x87: z²+y²+x² then fsqrt.
// No null-this branch (crash via get_float_field/Object path [esi+0Ch]).
// Missing/wrong field → 0.0 from get_float_field. Soft: null self → 0.f.
float java_lang_Vector3_length(InvObject* self) {
  // PE @ 0x004823E0 size 0x65 — java.lang.Vector3.length()F
  // UnboxArg this @ 0x0045D910; get_float x/y/z; x87 z²+y²+x² → fsqrt.
  if (!self) return 0.f;
  float x = 0.f, y = 0.f, z = 0.f;
  load_vec3(self, x, y, z);
  return std::sqrt(x * x + y * y + z * z);
}

// Soft PE @ 0x00482550 — java.lang.Vector3.distance(Ljava.lang.Vector3;)F
// size 0xB5. UnboxArg dest0=this (var_1C), dest1=v (arg_0 rewrite).
// Soft PE deepen: PE get order v x/y/z @ 0x613430/34/38 THEN this
// x/y/z @ 0x61343C/40/44; x87 fsub (this-v); fsqrt ||this-v||.
// No null-this/null-v branch (crash [esi+0Ch]). Soft: null self/v → 0.f.
float java_lang_Vector3_distance(InvObject* self, InvObject* other) {
  // PE @ 0x00482550 size 0xB5 — java.lang.Vector3.distance(Ljava.lang.Vector3;)F
  // UnboxArg this+v; get v x/y/z then this x/y/z; fsub (this-v); fsqrt.
  if (!self) return 0.f;
  // Soft PE deepen: match PE read order — other first, then this.
  float vx = 0.f, vy = 0.f, vz = 0.f;
  if (other) load_vec3(other, vx, vy, vz);
  float tx = 0.f, ty = 0.f, tz = 0.f;
  load_vec3(self, tx, ty, tz);
  const float dx = tx - vx, dy = ty - vy, dz = tz - vz;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Soft PE @ 0x00482450 — java.lang.Vector3.normalize()V size 0xF3
// UnboxArg this. Soft PE deepen: PE get order z/y/x @ 0x613418/1C/20;
// fsqrt; fcom flt_5E73CC AH&40 → skip scale; else fdivr 1.0.
// ALWAYS set_float x/y/z @ 0x613424/28/2C (even len==0 rewrite).
// Soft: null→ret; !=0.f stand-in for fcom AH&40.
void java_lang_Vector3_normalize(InvObject* self) {
  // PE @ 0x00482450 size 0xF3 — java.lang.Vector3.normalize()V
  // UnboxArg this; get z/y/x; fsqrt; fcom0 skip scale; ALWAYS set x/y/z.
  if (!self) return;
  float x = 0.f, y = 0.f, z = 0.f;
  load_vec3(self, x, y, z);
  const float len = std::sqrt(x * x + y * y + z * z);
  if (len != 0.f) {
    const float inv = 1.f / len;
    x *= inv;
    y *= inv;
    z *= inv;
  }
  store_vec3(self, x, y, z);
}

// Soft PE @ 0x00482110 — Vector3.rotate(Ypr) size 0x165.
// Unbox this+ypr. Soft PE deepen: PE get ypr y/p/r @ 0x6133C4/C8/CC then
// this z/y/x @ 0x6133D0/D4/D8; Vec3_store @ 0x00551C70 + Ypr_toMatrix
// → M*this; ALWAYS set x/y/z @ 0x6133DC/E0/E4. Returns this (JNI; host
// TREE void). Soft: null self→ret; null ypr → Mat3x4_setIdentity
// @ 0x0054E1D0 (PE null-ypr crashes get_float_field).
void java_lang_Vector3_rotate(InvObject* self, InvObject* ypr) {
  // PE @ 0x00482110 size 0x165 — Vector3.rotate(Ypr)Ljava.lang.Vector3;
  // Soft helpers: Vec3_store @ 0x00551C70, Ypr_toMatrix @ 0x0054ECD0 /
  // Mat3x4_setIdentity @ 0x0054E1D0; M*this via Mat3x4_mulVec @ 0x00436880.
  if (!self) return;
  float x = 0.f, y = 0.f, z = 0.f;
  load_vec3(self, x, y, z);
  float m[12];
  if (ypr) {
    float yaw = 0.f, pitch = 0.f, roll = 0.f;
    load_ypr(ypr, yaw, pitch, roll);
    float ypr_pack[3];
    soft_vec3_store(ypr_pack, yaw, pitch, roll);
    ypr_to_mat34(m, ypr_pack[0], ypr_pack[1], ypr_pack[2]);
  } else {
    mat34_set_identity(m);
  }
  float ox = 0.f, oy = 0.f, oz = 0.f;
  mat34_mul_vec(m, x, y, z, ox, oy, oz);
  store_vec3(self, ox, oy, oz);
}

// Soft PE @ 0x00482280 — Vector3.rotate(Vector3,F) size 0x15b.
// Soft PE deepen: PE get this z/y/x @ 0x6133E8/EC/F0 then axis z/y/x @
// 0x6133F4/F8/FC; Mat3x4_fromAxisAngle @ 0x0054EE80; out=M*this;
// ALWAYS set x/y/z @ 0x613400/04/08. Soft GAP: ||axis||^2 < flt_5F09C0
// → leave this unchanged (PE mul uninit M).
void java_lang_Vector3_rotate_1(InvObject* self, InvObject* axis,
                                float angle) {
  // PE @ 0x00482280 size 0x15b — Vector3.rotate(Vector3,F)Ljava.lang.Vector3;
  // Soft helper: Mat3x4_fromAxisAngle @ 0x0054EE80; out=M*this.
  if (!self) return;
  float x = 0.f, y = 0.f, z = 0.f;
  load_vec3(self, x, y, z);
  float ax = 0.f, ay = 0.f, az = 0.f;
  if (axis) load_vec3(axis, ax, ay, az);
  // Soft: same flt_5F09C0 gate as Mat3x4_fromAxisAngle (PE leaves M uninit).
  if (ax * ax + ay * ay + az * az < bits_f32(0x358637BDu)) return;
  float m[12] = {};
  mat34_from_axis_angle(m, ax, ay, az, angle);
  float ox = 0.f, oy = 0.f, oz = 0.f;
  mat34_mul_vec(m, x, y, z, ox, oy, oz);
  store_vec3(self, ox, oy, oz);
}

// Soft PE @ 0x004826C0 — java.lang.Ypr.mul(Ypr)V size 0x10d
// Soft PE deepen: Unbox dest0=this, dest1=v; get this y/p/r then v y/p/r;
// Soft Ypr_toMatrix both → Soft Mat3x4_mulLeft(M_this,M_v) → Soft
// Ypr_fromMatrix into ypr pack; ALWAYS set y/p/r. Soft: null self→ret;
// null v → Mat3x4_setIdentity @ 0x0054E1D0 (PE null-v crashes).
void java_lang_Ypr_mul(InvObject* self, InvObject* other) {
  // PE @ 0x004826C0 size 0x10d — java.lang.Ypr.mul(Ypr)V
  // Soft helpers: Ypr_toMatrix @ 0x0054ECD0 / Mat3x4_setIdentity @
  // 0x0054E1D0, Mat3x4_mulLeft @ 0x0054EAB0, Ypr_fromMatrix @ 0x00551C90.
  if (!self) return;
  float y = 0.f, p = 0.f, r = 0.f;
  load_ypr(self, y, p, r);
  float m_this[12] = {};
  float m_v[12] = {};
  ypr_to_mat34(m_this, y, p, r);
  if (other) {
    float oy = 0.f, op = 0.f, ov = 0.f;
    load_ypr(other, oy, op, ov);
    ypr_to_mat34(m_v, oy, op, ov);
  } else {
    mat34_set_identity(m_v);
  }
  mat34_mul_left(m_this, m_v);
  ypr_from_mat34(m_this, y, p, r);
  store_ypr(self, y, p, r);
}

// Soft PE @ 0x00482020 — Vector3.<init>(Ypr) size 0xe3.
// Soft PE deepen: Unbox dest0=this, dest1=ypr; get ypr y/p/r; Soft
// Vec3_store+Ypr_toMatrix; result=-fwd (*flt_5F0C70=0xBF800000): -m[2/6/10];
// ALWAYS set x/y/z. Soft: null self→ret; null ypr → Mat3x4_setIdentity
// @ 0x0054E1D0 then -fwd → (0,0,-1).
void java_lang_Vector3_init_Ypr(InvObject* self, InvObject* ypr) {
  // PE @ 0x00482020 size 0xe3 — Vector3.<init>(Ypr)
  // Soft helpers: Vec3_store @ 0x00551C70, Ypr_toMatrix @ 0x0054ECD0 /
  // Mat3x4_setIdentity @ 0x0054E1D0; result=-fwd (*flt_5F0C70): -m[2/6/10].
  if (!self) return;
  float m[12];
  if (ypr) {
    float yaw = 0.f, pitch = 0.f, roll = 0.f;
    load_ypr(ypr, yaw, pitch, roll);
    float ypr_pack[3];
    soft_vec3_store(ypr_pack, yaw, pitch, roll);
    ypr_to_mat34(m, ypr_pack[0], ypr_pack[1], ypr_pack[2]);
  } else {
    mat34_set_identity(m);
  }
  const float kNeg1 = bits_f32(0xBF800000u);  // flt_5F0C70
  store_vec3(self, m[2] * kNeg1, m[6] * kNeg1, m[10] * kNeg1);
}

// Soft PE @ 0x00482610 — Ypr.<init>(Vector3) size 0xa1.
// Soft PE deepen: Unbox dest0=this, dest1=v; get v x/y/z @ 0x613448/4C/50;
// Soft Ypr_fromDirection @ 0x00551D20; ALWAYS set y/p/r.
// Soft: null self→ret; null/missing v → (0,0,0) → pitch -π/2.
void java_lang_Ypr_init_Vector3(InvObject* self, InvObject* v) {
  // PE @ 0x00482610 size 0xa1 — Ypr.<init>(Vector3)
  // Soft helper: Ypr_fromDirection @ 0x00551D20; ALWAYS set y/p/r.
  if (!self) return;
  float x = 0.f, y = 0.f, z = 0.f;
  if (v) load_vec3(v, x, y, z);
  float yaw = 0.f, pitch = 0.f, roll = 0.f;
  ypr_from_direction(x, y, z, yaw, pitch, roll);
  store_ypr(self, yaw, pitch, roll);
}

}  // namespace inv
