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

void rot_ypr(float& x, float& y, float& z, float yaw, float pitch, float roll) {
  // PE Ypr_toMatrix @ 0x0054ECD0: R = Ry(yaw)*Rx(pitch)*Rz(roll), out = R*v.
  const float cy = std::cos(yaw), sy = std::sin(yaw);
  const float cp = std::cos(pitch), sp = std::sin(pitch);
  const float cr = std::cos(roll), sr = std::sin(roll);
  float x1 = cr * x - sr * y;
  float y1 = sr * x + cr * y;
  float z1 = z;
  float x2 = x1;
  float y2 = cp * y1 - sp * z1;
  float z2 = sp * y1 + cp * z1;
  x = cy * x2 + sy * z2;
  y = y2;
  z = -sy * x2 + cy * z2;
}

// PE Ypr_toMatrix @ 0x0054ECD0: 3x4 row-major stride 16, cols=right/up/fwd,
// R=Ry(yaw)*Rx(pitch)*Rz(roll). Cells [3]/[7]/[11] unused by fromMatrix.
void ypr_to_mat34(float m[12], float yaw, float pitch, float roll) {
  const float sy = std::sin(yaw), cy = std::cos(yaw);
  const float sp = std::sin(pitch), cp = std::cos(pitch);
  const float sr = std::sin(roll), cr = std::cos(roll);
  const float sr_sp = sr * sp;
  const float cr_sp = cr * sp;
  m[0] = sr_sp * sy + cr * cy;
  m[1] = cr_sp * sy - sr * cy;
  m[2] = cp * sy;
  m[4] = sr * cp;
  m[5] = cr * cp;
  m[6] = -sp;
  m[8] = sr_sp * cy - cr * sy;
  m[9] = cr_sp * cy + sr * sy;
  m[10] = cp * cy;
}

// PE Mat3x4_fromAxisAngle @ 0x0054EE80 (callee of rotate axis @ 0x00482280):
// thiscall ecx=&M00, (ax,ay,az,angle). 3x4 stride 0x10 Rodrigues. Angle as-is
// (radians → fcos/fsin). ||axis||^2 < 1e-6 → early leave M uninit (caller still
// multiplies). Normalizes axis then builds R; cells [3]/[7]/[11] unused.
void mat34_from_axis_angle(float m[12], float ax, float ay, float az, float angle) {
  const float len2 = ax * ax + ay * ay + az * az;
  if (len2 < 1e-6f) return;
  const float inv = 1.f / std::sqrt(len2);
  ax *= inv;
  ay *= inv;
  az *= inv;
  const float c = std::cos(angle), s = std::sin(angle);
  const float t = 1.f - c;
  const float xx = ax * ax, yy = ay * ay, zz = az * az;
  const float xy = ax * ay, xz = ax * az, yz = ay * az;
  m[0] = c + xx * t;
  m[1] = xy * t - az * s;
  m[2] = xz * t + ay * s;
  m[4] = xy * t + az * s;
  m[5] = c + yy * t;
  m[6] = yz * t - ax * s;
  m[8] = xz * t - ay * s;
  m[9] = yz * t + ax * s;
  m[10] = c + zz * t;
}

// PE Mat3x4_mulLeft @ 0x0054EAB0: this := A * this (3x3 in 3x4 stride 16).
void mat34_mul_left(float th[12], const float a[12]) {
  float o[12];
  o[0] = a[0] * th[0] + a[1] * th[4] + a[2] * th[8];
  o[1] = a[0] * th[1] + a[1] * th[5] + a[2] * th[9];
  o[2] = a[0] * th[2] + a[1] * th[6] + a[2] * th[10];
  o[4] = a[4] * th[0] + a[5] * th[4] + a[6] * th[8];
  o[5] = a[4] * th[1] + a[5] * th[5] + a[6] * th[9];
  o[6] = a[4] * th[2] + a[5] * th[6] + a[6] * th[10];
  o[8] = a[8] * th[0] + a[9] * th[4] + a[10] * th[8];
  o[9] = a[8] * th[1] + a[9] * th[5] + a[10] * th[9];
  o[10] = a[8] * th[2] + a[9] * th[6] + a[10] * th[10];
  th[0] = o[0];
  th[1] = o[1];
  th[2] = o[2];
  th[4] = o[4];
  th[5] = o[5];
  th[6] = o[6];
  th[8] = o[8];
  th[9] = o[9];
  th[10] = o[10];
}

// PE Ypr_fromMatrix @ 0x00551C90. Gimbal pitch bits 0x40490FDB (float π).
void ypr_from_mat34(const float m[12], float& yaw, float& pitch, float& roll) {
  if (m[10] == 0.f && m[2] == 0.f) {
    unsigned bits = 0x40490FDB;
    std::memcpy(&pitch, &bits, sizeof(pitch));
    roll = 0.f;
    yaw = std::atan2(m[0], -m[1]);
    return;
  }
  yaw = std::atan2(m[2], m[10]);
  roll = std::atan2(m[4], m[5]);
  const float cr = std::cos(roll);
  const float v6 = cr * m[6];
  // fcom cos(roll) vs 0.0; test AH C0|C3 (≤0 or unordered).
  if (!(cr > 0.f))
    pitch = std::atan2(v6, -m[5]);
  else
    pitch = std::atan2(-v6, m[5]);
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

// PE @ 0x004823E0 — java.lang.Vector3.length()F size 0x65
// UnboxArg this @ 0x0045D910. get_float_field x/y/z @ 0x0061340C/10/14
// (JVM_vm_get_float_field @ 0x0042A560). x87: z²+y²+x² then fsqrt.
// No null-this branch (crash via get_float_field/Object path [esi+0Ch]).
// Missing/wrong field → 0.0 from get_float_field. Host: null self → 0.f.
float java_lang_Vector3_length(InvObject* self) {
  if (!self) return 0.f;
  float x = 0.f, y = 0.f, z = 0.f;
  bool have_native = false;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (Vec3* v = vget(self)) {
      x = v->x;
      y = v->y;
      z = v->z;
      have_native = true;
    }
  }
  if (!have_native) {
    x = tree_field_get_float(self, "x");
    y = tree_field_get_float(self, "y");
    z = tree_field_get_float(self, "z");
  }
  return std::sqrt(x * x + y * y + z * z);
}

// PE @ 0x00482550 — java.lang.Vector3.distance(Ljava.lang.Vector3;)F size 0xB5
// UnboxArg @ 0x0045D910: dest0=this (local), dest1=v (overwrites CallInfo arg0).
// get_float_field @ 0x0042A560: v x/y/z @ 0x00613430/34/38 then this @
// 0x0061343C/40/44. x87: fsub (this-v) per axis; fsqrt sum sq (= ||this-v||).
// Callees: UnboxArg / get_float_field only. No null-this/null-v branch
// (crash [esi+0Ch] like length). Host: null self → 0.f; null/missing v → 0.f.
float java_lang_Vector3_distance(InvObject* self, InvObject* other) {
  if (!self) return 0.f;
  float tx = 0.f, ty = 0.f, tz = 0.f;
  bool have_self = false;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (Vec3* a = vget(self)) {
      tx = a->x;
      ty = a->y;
      tz = a->z;
      have_self = true;
    }
  }
  if (!have_self) {
    tx = tree_field_get_float(self, "x");
    ty = tree_field_get_float(self, "y");
    tz = tree_field_get_float(self, "z");
  }
  float vx = 0.f, vy = 0.f, vz = 0.f;
  if (other) {
    bool have_other = false;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      if (Vec3* b = vget(other)) {
        vx = b->x;
        vy = b->y;
        vz = b->z;
        have_other = true;
      }
    }
    if (!have_other) {
      vx = tree_field_get_float(other, "x");
      vy = tree_field_get_float(other, "y");
      vz = tree_field_get_float(other, "z");
    }
  }
  const float dx = tx - vx, dy = ty - vy, dz = tz - vz;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// PE @ 0x00482450 — java.lang.Vector3.normalize()V size 0xF3
// UnboxArg this @ 0x0045D910. get_float_field z/y/x @ 0x00613418/1C/20
// (length @ 0x004823E0 reads x/y/z — same fields, different order).
// x87: fsqrt(y²+z²+x²); fcom flt_5E73CC (0.0); test AH,40h (C3) → skip;
// else fdivr flt_5F08F0 (1.0) then *x/*y/*z. ALWAYS set_float_field x/y/z
// @ 0x00613424/28/2C (even if len==0: rewrite originals). No null-this
// branch (crash [esi+0Ch] like length). Callees: UnboxArg / get / set only.
// Host: null self → return. Missing field → 0.f.
void java_lang_Vector3_normalize(InvObject* self) {
  if (!self) return;
  float x = 0.f, y = 0.f, z = 0.f;
  bool have_native = false;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (Vec3* v = vget(self)) {
      x = v->x;
      y = v->y;
      z = v->z;
      have_native = true;
    }
  }
  if (!have_native) {
    x = tree_field_get_float(self, "x");
    y = tree_field_get_float(self, "y");
    z = tree_field_get_float(self, "z");
  }
  const float len = std::sqrt(x * x + y * y + z * z);
  if (len != 0.f) {
    const float inv = 1.f / len;
    x *= inv;
    y *= inv;
    z *= inv;
  }
  vec3_set(self, x, y, z);
  tree_field_set_float(self, "x", x);
  tree_field_set_float(self, "y", y);
  tree_field_set_float(self, "z", z);
}

// PE @ 0x00482110 — java.lang.Vector3.rotate(Ljava.lang.Ypr;)Ljava.lang.Vector3;
// size 0x165 (retn @ 0x00482274). IDA name
// java_lang_Vector3_rotate__Ljava_lang_Ypr_Ljava_lang_Vector3.
// UnboxArg dest0=this dest1=ypr. No null-this/null-ypr test.
// get_float_field Ypr y/p/r @ 0x006133C4/C8/CC then Vector3 z/y/x @
// 0x006133D0/D4/D8. Callees: Vec3_store @ 0x00551C70 (y,p,r) then
// Ypr_toMatrix @ 0x0054ECD0 (3x4 stride 0x10, R=Ry*Rx*Rz; host
// ypr_to_mat34). out=M*v (asm order; same as row*vec):
//   ox=m[0]*x+m[1]*y+m[2]*z;
//   oy=m[5]*y+m[6]*z+m[4]*x;
//   oz=m[9]*y+m[10]*z+m[8]*x.
// ALWAYS set_float_field x/y/z @ 0x006133DC/E0/E4. Returns this (JNI
// Vector3; host/TREE void). Contrast rotate(Vector3,F) @ 0x00482280 —
// Mat3x4_fromAxisAngle only (no Vec3_store/Ypr_toMatrix).
// Host: null self → return. Null/missing ypr → yaw/pitch/roll 0 (identity).
void java_lang_Vector3_rotate(InvObject* self, InvObject* ypr) {
  if (!self) return;
  float x = 0.f, y = 0.f, z = 0.f;
  bool have_native = false;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (Vec3* v = vget(self)) {
      x = v->x;
      y = v->y;
      z = v->z;
      have_native = true;
    }
  }
  if (!have_native) {
    // PE field-get order is z, y, x — same values.
    z = tree_field_get_float(self, "z");
    y = tree_field_get_float(self, "y");
    x = tree_field_get_float(self, "x");
  }
  float yaw = 0.f, pitch = 0.f, roll = 0.f;
  if (ypr) {
    bool have_ypr = false;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      if (Ypr* t = yget(ypr)) {
        yaw = t->y;
        pitch = t->p;
        roll = t->r;
        have_ypr = true;
      }
    }
    if (!have_ypr) {
      yaw = tree_field_get_float(ypr, "y");
      pitch = tree_field_get_float(ypr, "p");
      roll = tree_field_get_float(ypr, "r");
    }
  }
  // PE: Vec3_store(y,p,r) + Ypr_toMatrix → M; then M*this.
  float m[12];
  ypr_to_mat34(m, yaw, pitch, roll);
  const float ox = m[0] * x + m[1] * y + m[2] * z;
  const float oy = m[4] * x + m[5] * y + m[6] * z;
  const float oz = m[8] * x + m[9] * y + m[10] * z;
  vec3_set(self, ox, oy, oz);
  tree_field_set_float(self, "x", ox);
  tree_field_set_float(self, "y", oy);
  tree_field_set_float(self, "z", oz);
}

// PE @ 0x00482280 — java.lang.Vector3.rotate(Ljava.lang.Vector3;F)Ljava.lang.Vector3;
// size 0x15b. NOT the Ypr overload (@ 0x00482110).
// UnboxArg dest0=this dest1=axis dest2=angle. No null-this/null-axis test.
// get_float_field this z/y/x @ 0x006133E8/EC/F0 then axis z/y/x @
// 0x006133F4/F8/FC. Mat3x4_fromAxisAngle @ 0x0054EE80 (ecx=&M00, ax,ay,az,
// angle; 3x4 stride 0x10 Rodrigues; angle radians as-is). Axis through origin
// (no pivot translate). Degenerate ||axis||^2 < 1e-6 → M uninit in PE.
// out=M*this (same layout as Ypr path):
//   ox=M00*x+M01*y+M02*z; oy=M10*x+M11*y+M12*z; oz=M20*x+M21*y+M22*z.
// ALWAYS set_float_field x/y/z @ 0x00613400/04/08. Returns this (JNI Vector3;
// Java void / host TREE void). Contrast: Ypr uses Vec3_store+Ypr_toMatrix;
// this uses Mat3x4_fromAxisAngle only — no Ypr.
// Host: null self → return. Null/missing axis → 0,0,0 then ||^2<1e-6 leave
// unchanged (PE would multiply uninit M).
void java_lang_Vector3_rotate_1(InvObject* self, InvObject* axis, float angle) {
  if (!self) return;
  float x = 0.f, y = 0.f, z = 0.f;
  bool have_native = false;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (Vec3* v = vget(self)) {
      x = v->x;
      y = v->y;
      z = v->z;
      have_native = true;
    }
  }
  if (!have_native) {
    x = tree_field_get_float(self, "x");
    y = tree_field_get_float(self, "y");
    z = tree_field_get_float(self, "z");
  }
  float ax = 0.f, ay = 0.f, az = 0.f;
  if (axis) {
    bool have_axis = false;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      if (Vec3* a = vget(axis)) {
        ax = a->x;
        ay = a->y;
        az = a->z;
        have_axis = true;
      }
    }
    if (!have_axis) {
      ax = tree_field_get_float(axis, "x");
      ay = tree_field_get_float(axis, "y");
      az = tree_field_get_float(axis, "z");
    }
  }
  if (ax * ax + ay * ay + az * az < 1e-6f) return;
  float m[12];
  mat34_from_axis_angle(m, ax, ay, az, angle);
  const float ox = m[0] * x + m[1] * y + m[2] * z;
  const float oy = m[4] * x + m[5] * y + m[6] * z;
  const float oz = m[8] * x + m[9] * y + m[10] * z;
  vec3_set(self, ox, oy, oz);
  tree_field_set_float(self, "x", ox);
  tree_field_set_float(self, "y", oy);
  tree_field_set_float(self, "z", oz);
}

// PE @ 0x004826C0 — java.lang.Ypr.mul(Ypr)V size 0x10d
// UnboxArg dest0=this dest1=v. get_float_field this y/p/r @ 0x00613460/64/68
// then v y/p/r @ 0x0061346C/70/74. Ypr_toMatrix M_this then M_v.
// Mat3x4_mulLeft(M_this, M_v) @ 0x0054EAB0: M_this := M_v * M_this.
// Ypr_fromMatrix @ 0x00551C90. ALWAYS set_float_field y/p/r @ 0x00613478/7C/80.
// No null-this/null-v branch (crash [esi+0Ch] like length).
// Host: null self → return. Null/missing v → 0,0,0 (identity).
void java_lang_Ypr_mul(InvObject* self, InvObject* other) {
  if (!self) return;
  float y = 0.f, p = 0.f, r = 0.f;
  bool have_self = false;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (Ypr* t = yget(self)) {
      y = t->y;
      p = t->p;
      r = t->r;
      have_self = true;
    }
  }
  if (!have_self) {
    y = tree_field_get_float(self, "y");
    p = tree_field_get_float(self, "p");
    r = tree_field_get_float(self, "r");
  }
  float oy = 0.f, op = 0.f, ov = 0.f;
  if (other) {
    bool have_other = false;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      if (Ypr* t = yget(other)) {
        oy = t->y;
        op = t->p;
        ov = t->r;
        have_other = true;
      }
    }
    if (!have_other) {
      oy = tree_field_get_float(other, "y");
      op = tree_field_get_float(other, "p");
      ov = tree_field_get_float(other, "r");
    }
  }
  float m_this[12] = {};
  float m_v[12] = {};
  ypr_to_mat34(m_this, y, p, r);
  ypr_to_mat34(m_v, oy, op, ov);
  mat34_mul_left(m_this, m_v);
  ypr_from_mat34(m_this, y, p, r);
  ypr_set(self, y, p, r);
  tree_field_set_float(self, "y", y);
  tree_field_set_float(self, "p", p);
  tree_field_set_float(self, "r", r);
}

// PE @ 0x00482020 — java.lang.Vector3.<init>(Ljava.lang.Ypr;)
// size 0xe3. UnboxArg dest0=this dest1=ypr. No null-this/null-ypr test.
// get_float_field Ypr y/p/r @ 0x006133AC/B0/B4. Vec3_store @ 0x00551C70 (y,p,r);
// Ypr_toMatrix @ 0x0054ECD0 (ecx=&M, push &ypr; 3x4 stride 0x10, cols=right/up/fwd).
// Result = -fwd (* flt_5F0C70=-1.0f): ox=-m[2], oy=-m[6], oz=-m[10].
// ALWAYS set_float_field x/y/z @ 0x006133B8/BC/C0. JNI ctor returns void.
// Host: null self → return. Null/missing ypr → yaw/pitch/roll 0 → (0,0,-1).
void java_lang_Vector3_init_Ypr(InvObject* self, InvObject* ypr) {
  if (!self) return;
  float yaw = 0.f, pitch = 0.f, roll = 0.f;
  if (ypr) {
    bool have_ypr = false;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      if (Ypr* t = yget(ypr)) {
        yaw = t->y;
        pitch = t->p;
        roll = t->r;
        have_ypr = true;
      }
    }
    if (!have_ypr) {
      yaw = tree_field_get_float(ypr, "y");
      pitch = tree_field_get_float(ypr, "p");
      roll = tree_field_get_float(ypr, "r");
    }
  }
  float m[12];
  ypr_to_mat34(m, yaw, pitch, roll);
  const float ox = -m[2];
  const float oy = -m[6];
  const float oz = -m[10];
  vec3_set(self, ox, oy, oz);
  tree_field_set_float(self, "x", ox);
  tree_field_set_float(self, "y", oy);
  tree_field_set_float(self, "z", oz);
}

// PE @ 0x00482610 — java.lang.Ypr.<init>(Ljava.lang.Vector3;) size 0xa1
// Contrast Vector3.<init>(Ypr) @ 0x00482020 (Ypr→dir via Ypr_toMatrix -fwd):
// this is the inverse dir→Ypr. UnboxArg dest0=this dest1=v. No null test.
// get_float_field v x/y/z @ 0x00613448/4C/50. thiscall sub_551D20
// @ 0x00551D20 (ecx=&ypr out, push &xyz) size 0x99: if x==0 && z==0 →
// y=0,r=0, p=+π/2 (flt_5F0CC4) if y>0 else -π/2 (flt_5F3AB0, y<=0 via
// fcomp flt_5E73CC + AH&41h); else y=atan2(-x,-z), p=atan2(y,sqrt(x²+z²)),
// r=0. ALWAYS set_float_field this y/p/r @ 0x00613454/58/5C.
// Host: null self → return. Null/missing v → fields 0 → pitch -π/2.
void java_lang_Ypr_init_Vector3(InvObject* self, InvObject* v) {
  if (!self) return;
  float x = 0.f, y = 0.f, z = 0.f;
  if (v) {
    bool have_v = false;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      if (Vec3* t = vget(v)) {
        x = t->x;
        y = t->y;
        z = t->z;
        have_v = true;
      }
    }
    if (!have_v) {
      x = tree_field_get_float(v, "x");
      y = tree_field_get_float(v, "y");
      z = tree_field_get_float(v, "z");
    }
  }
  float yaw = 0.f, pitch = 0.f;
  const float roll = 0.f;
  if (x == 0.f && z == 0.f) {
    // sub_551D20 singularity: flt_5F0CC4 / flt_5F3AB0 (±π/2).
    pitch = (y <= 0.f) ? -1.57079637f : 1.57079637f;
  } else {
    yaw = std::atan2(-x, -z);
    pitch = std::atan2(y, std::sqrt(x * x + z * z));
  }
  ypr_set(self, yaw, pitch, roll);
  tree_field_set_float(self, "y", yaw);
  tree_field_set_float(self, "p", pitch);
  tree_field_set_float(self, "r", roll);
}

}  // namespace inv
