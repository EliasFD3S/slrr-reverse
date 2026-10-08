// Fragment 4/4 of Resources.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


void physics_resolve_collisions() {
  std::vector<InvObject*> keys;
  keys.reserve(g_res.size());
  for (auto& kv : g_res) {
    if (kv.second.shape != 0 && kv.second.collide) keys.push_back(kv.first);
  }
  for (size_t i = 0; i < keys.size(); ++i) {
    for (size_t j = i + 1; j < keys.size(); ++j) {
      resolve_pair(g_res[keys[i]], g_res[keys[j]]);
    }
  }
}

// Soft PE Physics_Step @ 0x4A5190 (ecx=Engine_physWorld; caller
// Engine_SimulateFrame @ 0x42859A / 0x42892B). int_convert offsets:
//   +4 dt accum, +12 last step, +16/+20/+44 counters, +88=0x58, +92=0x5C,
//   +132=0x84 body list (head *[list+0x24], next +0x28),
//   +156=0x9C dllist, +184=0xB8 dllist, +280=0x118 reset wipe,
//   +492=0x1EC solver continue. Gate flt_5F09D4@0x5F09D4=1e-4 (0x38D1B717).
// Soft max substep 0.05 ≡ System kPhysMaxSubstepSoft (*(world+8) OOS).
// Body lists: soft census / gates only — no PE dllist nodes, no invent
// sub_4A3900/sub_4A2D10/sub_4A2540 solver. sub_426F60 animate OOS no-op.
//
// Chassis aero consumer (not Physics_Step — PE has ZERO +0x908 hits):
//   PE Chassis_physWheelTick @ 0x456a83 → Phys_accumForceAtLocalPoint
//   @ 0x4A6520 (F into body force accum; τ += (R*r)×F). Host Chassis
//   writes TREE side-band (aero_impulse_* / aero_applied / aero_force_p*);
//   THIS path is the sole live Δv consumer (Chassis.h). forceUpdate only
//   STOREs hdr+0x8FC..+0x908 → force_update_drag_* (fallback Cd path).
// Constants (get_bytes): flt_5E73CC=0, float_1_0=1, flt_5F09D0=0.5,
// flt_5F0C80=0.25. Torque/inertia tensor / parent-walk (+0x58)&4 OOS.
static int32_t g_phys_step_frame = 0;     // soft dword_6439C0 @ 0x4a520c
static int32_t g_phys_list132 = 0;        // soft +132 walk tally
static int32_t g_phys_list156 = 0;        // soft +156 cull candidates
static int32_t g_phys_list184 = 0;        // soft +184 live candidates
static int32_t g_phys_solver_iters = 0;   // soft *(world+16)
static int32_t g_phys_aero_hits = 0;      // soft Cd / impulse apply count
static float g_phys_world_clock = 0.f;    // soft *(physWorld+0) @ 0x4a548e

// Soft donor for Chassis aero TREE fields. Body key may be Vehicle
// (Native.ptr) while side-band lives on chassis (setMileage / forceUpdate).
static InvObject* physics_aero_sideband_owner(InvObject* key) {
  if (!key) return nullptr;
  if (tree_field_get_int(key, "aero_applied") != 0) return key;
  if (InvObject* ch = tree_field_get_obj(key, "chassis")) {
    if (tree_field_get_int(ch, "aero_applied") != 0) return ch;
  }
  if (tree_field_get_float(key, "force_update_drag_c") > 0.f) return key;
  if (InvObject* ch = tree_field_get_obj(key, "chassis")) {
    if (tree_field_get_float(ch, "force_update_drag_c") > 0.f) return ch;
  }
  return key;
}

static void physics_apply_chassis_aero(InvObject* key, ResState& r, float dt) {
  if (!key || dt <= 0.f) return;
  InvObject* src = physics_aero_sideband_owner(key);
  if (!src) return;

  // Path A — consume Chassis soft side-band (chassis_phys_aero_soft_apply).
  // One-shot: clear aero_applied so kSub≠double-apply (Chassis wrote J for
  // its setMileage dt once; PE accumulates force once per wheel-tick).
  if (tree_field_get_int(src, "aero_applied") != 0) {
    float jx = tree_field_get_float(src, "aero_impulse_fx");
    float jy = tree_field_get_float(src, "aero_impulse_fy");
    float jz = tree_field_get_float(src, "aero_impulse_fz");
    // Soft inv-mass stand-in for PE force-accum → solver. TREE mass /
    // chassis_mass; else 1200 ≡ Chassis_getMass soft default (so Chassis
    // mass-clamped J @ 0x456b2a.. yields |Δv|≲0.5*|v|).
    float mass = tree_field_get_float(src, "mass");
    if (mass <= 0.f) mass = tree_field_get_float(src, "chassis_mass");
    if (mass <= 1e-3f) mass = 1200.f;
    const float inv_m = 1.f / mass;
    float dvx = jx * inv_m;
    float dvy = jy * inv_m;
    float dvz = jz * inv_m;
    // Soft safety vs flt_5F0C80@0x5F0C80=0.25 (PE clamp @ 0x456c97..cab).
    const float v2 = r.vx * r.vx + r.vy * r.vy + r.vz * r.vz;
    constexpr float kQuarter = 0.25f;
    const float max2 = v2 * kQuarter;
    const float d2 = dvx * dvx + dvy * dvy + dvz * dvz;
    if (max2 > 1e-12f && d2 > max2) {
      const float s = std::sqrt(max2 / d2);
      dvx *= s;
      dvy *= s;
      dvz *= s;
    }
    r.vx += dvx;
    r.vy += dvy;
    r.vz += dvz;
    // Soft τ stand-in for Phys_accumForceAtLocalPoint a3=drag_center:
    // ω += r_local × Δv (unit I≈m OOS — no inertia tensor @ body+0x108).
    const float rx = tree_field_get_float(src, "aero_force_px");
    const float ry = tree_field_get_float(src, "aero_force_py");
    const float rz = tree_field_get_float(src, "aero_force_pz");
    r.wx += (ry * dvz - rz * dvy);
    r.wy += (rz * dvx - rx * dvz);
    r.wz += (rx * dvy - ry * dvx);
    tree_field_set_int(src, "aero_applied", 0);
    ++g_phys_aero_hits;
    (void)dt;
    return;
  }

  // Path B — fallback when Chassis tick hasn't written side-band this
  // frame: TREE force_update_drag_* (forceUpdate @ hdr+0x8FC..+0x908).
  // PE gate C_drag<=0 vs flt_5E73CC=0 @ 0x456a83..0x456a94.
  const float cd = tree_field_get_float(src, "force_update_drag_c");
  if (!(cd > 0.f)) return;
  // Soft: drag_center xyz side-band read — torque arm OOS on fallback
  // (Phys_rotateLocalVec @ 0x4A7190). Consumed only on Path A.
  (void)tree_field_get_float(src, "force_update_drag_x");
  (void)tree_field_get_float(src, "force_update_drag_y");
  (void)tree_field_get_float(src, "force_update_drag_z");
  const float v2 = r.vx * r.vx + r.vy * r.vy + r.vz * r.vz;
  // PE |v|^2 <= float_1_0@0x5F08F0 → skip @ 0x456ac8.
  if (v2 <= 1.f) return;
  const float speed = std::sqrt(v2);
  // F = -Cd * |v|^2 * v_hat ≡ -Cd * |v| * v  (@ 0x456b05 fchs).
  float fx = -cd * speed * r.vx;
  float fy = -cd * speed * r.vy;
  float fz = -cd * speed * r.vz;
  // Soft unit-mass impulse = F*dt (PE inv-mass / inertia path OOS).
  float ix = fx * dt;
  float iy = fy * dt;
  float iz = fz * dt;
  // Soft clamp @ 0x456c34..0x456cab: PE mixes F*dt with vel*flt_5F09D0(0.5)
  // then caps vs vel²*flt_5F0C80(0.25). Host: |imp|² ≤ 0.25*|v|² (vel*0.5
  // blend needs inv-mass — OOS).
  constexpr float kQuarter = 0.25f;  // flt_5F0C80 @ 0x5F0C80
  const float max2 = v2 * kQuarter;
  const float i2 = ix * ix + iy * iy + iz * iz;
  if (max2 > 1e-12f && i2 > max2) {
    const float s = std::sqrt(max2 / i2);
    ix *= s;
    iy *= s;
    iz *= s;
  }
  r.vx += ix;
  r.vy += iy;
  r.vz += iz;
  ++g_phys_aero_hits;
}

void physics_integrate(float dt) {
  // PE @ 0x4a5197..0x4a51ae: v3=a2+*(world+4); if v3 < 1e-4 return 0.
  // Soft: world+4 accum always cleared after a taken step → gate ≡ dt
  // (flt_5F09D4@0x5F09D4 bytes 17 b7 d1 38 ≈ 1e-4).
  if (dt < 1e-4f) return;
  g_collide_events = 0;
  ++g_phys_step_frame;       // PE ++dword_6439C0 @ 0x4a520c
  g_phys_solver_iters = 0;   // PE *(world+16)=0 @ 0x4a5214
  g_phys_aero_hits = 0;

  // Soft body-list census (PE walks @ 0x4a5230 / 0x4a525f / 0x4a52f2).
  // +132: dynamic shaped bodies (prep sub_4A3F90 + integrate sub_4A3BC0).
  // +156: asleep/static with shape — cull candidates (vtbl+12 < 0 OOS).
  // +184: collide-enabled dynamics — live-check candidates (vtbl+24 OOS).
  // sub_426F60 @ 0x4a52d0 (g_EngineState animate) — OOS no-op.
  // Physics_stepResetPending wipe @ world+280 OOS (no host dllist).
  int n132 = 0, n156 = 0, n184 = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& kv : g_res) {
      const auto& r = kv.second;
      if (r.shape == 0) continue;
      if (r.is_static || r.asleep) {
        ++n156;
        continue;
      }
      ++n132;
      if (r.collide) ++n184;
    }
  }
  g_phys_list132 = n132;
  g_phys_list156 = n156;
  g_phys_list184 = n184;

  // Soft substep: PE one integrate pass per Physics_Step call; outer
  // SimulateFrame already slices by *(world+8)≈0.05. Internal kSub=4 was
  // over-subdividing — use 1 when dt already ≤ soft max, else ceil.
  constexpr float kPeMaxSub = 0.05f;
  int kSub = 1;
  if (dt > kPeMaxSub) {
    kSub = static_cast<int>(dt / kPeMaxSub + 0.999f);
    if (kSub < 2) kSub = 2;
    if (kSub > 4) kSub = 4;  // soft cap (was fixed 4)
  }
  const float sdt = dt / static_cast<float>(kSub);
  // Soft *(world+16) iter tally — PE solver loop max 20 @ 0x4a5455 OOS
  // (sub_4A3900 / sub_4A2D10 / sub_4A2540). Host: collide+Euler only.
  struct Step {
    InvObject* key;
    float px, py, pz, oy, op, or_, sx, sy, sz;
  };
  std::vector<Step> steps;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (int sub = 0; sub < kSub; ++sub) {
      // Soft +132 prep (sub_4A3F90 @ 0x4a5235) OOS — resolve-before-move.
      physics_resolve_collisions();
      ++g_phys_solver_iters;
      for (auto& kv : g_res) {
        auto& r = kv.second;
        if (r.shape == 0) continue;
        if (r.is_static || r.asleep) continue;
        // Soft aero before Euler ≡ PE wheel-tick force accum → Step
        // integrate. Path A consumes Chassis aero_impulse_* once (clears
        // aero_applied); Path B Cd fallback uses sdt each substep.
        physics_apply_chassis_aero(kv.first, r, sdt);
        r.vy -= kGravity * sdt;
        r.px += r.vx * sdt;
        r.py += r.vy * sdt;
        r.pz += r.vz * sdt;
        r.oy += r.wy * sdt;
        r.op += r.wx * sdt;
        r.or_ += r.wz * sdt;
        const float half = contact_half_height(r);
        const float min_y = g_ground_y + half;
        if (r.py < min_y) {
          r.py = min_y;
          if (r.vy < 0.f) r.vy = 0.f;
        }
      }
      // Soft +132 integrate re-walk (sub_4A3BC0 @ 0x4a52e6 / 0x4a543d) —
      // post-move collide stands in; no PE constraint solver.
      physics_resolve_collisions();
      ++g_phys_solver_iters;
    }
    for (auto& kv : g_res) {
      auto& r = kv.second;
      if (r.shape == 0) continue;
      if (r.is_static || r.asleep) continue;
      steps.push_back(Step{kv.first, r.px, r.py, r.pz, r.oy, r.op, r.or_, r.sx,
                           r.sy, r.sz});
    }
  }
  // Soft +184 live-check / +132 finalize (sub_4A4ED0) OOS — pose sync only.
  for (const Step& s : steps)
    render_d3d9_mesh_set_transform(s.key, s.px, s.py, s.pz, s.oy, s.op, s.or_,
                                   s.sx, s.sy, s.sz);
  // PE *(physWorld+0) += v22 @ 0x4a548e; flt_6439C4 EMA OOS.
  g_phys_world_clock += dt;
  (void)g_phys_step_frame;
  (void)g_phys_list132;
  (void)g_phys_list156;
  (void)g_phys_list184;
  (void)g_phys_solver_iters;
  (void)g_phys_aero_hits;
  (void)g_phys_world_clock;
}

void physics_set_wheel_params(InvObject* self, float steer, float drive,
                              float radius) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.has_wheel_params = true;
  r.wheel_steer = steer;
  r.wheel_drive = drive;
  r.wheel_radius = (radius > 0.05f) ? radius : 0.32f;
}

void physics_set_wheel_contact(InvObject* self, float friction, float sliction,
                               float brake, float hbrake, float roll_res) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.has_wheel_params = true;
  r.wheel_friction = friction;
  r.wheel_sliction = sliction;
  r.wheel_brake = brake;
  r.wheel_hbrake = hbrake;
  r.wheel_roll_res = roll_res;
}

void physics_set_wheel_pacejka(InvObject* self, float b, float c, float d) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.has_wheel_params = true;
  r.wheel_pk_b = b;
  r.wheel_pk_c = c;
  r.wheel_pk_d = d;
}

void physics_set_wheel_suspension(InvObject* self, float spring, float damp,
                                  float rest_len, float arm_len) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.has_wheel_params = true;
  r.wheel_spring = spring;
  r.wheel_damp = damp;
  r.wheel_rest_len = (rest_len > 0.01f) ? rest_len : 0.39f;
  r.wheel_arm_len = (arm_len > 0.05f) ? arm_len : 0.244f;
}

void physics_set_drive_torque(InvObject* self, float nm) {
  if (!self) return;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  if (nm < 0.f) nm = 0.f;
  r.drive_torque_nm = nm;
}

float physics_get_engine_rpm(InvObject* self) {
  if (!self) return 0.f;
  std::lock_guard<std::mutex> lock(g_mu);
  return R(self).engine_rpm;
}

void physics_drive(InvObject* self, InvObject* controller, float dt) {
  if (!self || dt <= 0.f) return;
  float throttle =
      controller ? input_map_get_logical(controller, kAxisThrottle) : 0.f;
  float brake =
      controller ? input_map_get_logical(controller, kAxisBrake) : 0.f;
  float steer =
      controller ? input_map_get_logical(controller, kAxisTurnLR) : 0.f;
  float handbrake =
      controller ? input_map_get_logical(controller, kAxisHandbrake) : 0.f;
  const float nitro =
      controller ? input_map_get_logical(controller, kAxisNitro) : 0.f;
  const float clutch =
      controller ? input_map_get_logical(controller, kAxisClutch) : 0.f;
  const float gear_axis =
      controller ? input_map_get_logical(controller, kAxisGearUpDown) : 0.f;

  // Phase 2.93: sample support before lock (road_project also locks).
  float sample_px = 0, sample_py = 0, sample_pz = 0, sample_half = 0.5f;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& r = R(self);
    if (r.shape == 0 || r.is_static || r.asleep) return;
    sample_px = r.px;
    sample_py = r.py;
    sample_pz = r.pz;
    sample_half = contact_half_height(r);
  }
  float sox = 0, soy = 0, soz = 0, srdx = 0, srdy = 0, srdz = 0;
  const bool sample_on_road = physics_road_project(
      sample_px, sample_pz, &sox, &soy, &soz, &srdx, &srdy, &srdz);
  const float support_y =
      sample_on_road ? (soy + sample_half) : (g_ground_y + sample_half);
  const bool airborne_now = sample_py > support_y + kAirborneClearance;

  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& r = R(self);
    if (r.shape == 0 || r.is_static || r.asleep) return;
    r.airborne = airborne_now ? 1 : 0;

    float drive_mul = 1.f;
    float radius_mul = 1.f;
    float grip_mul = 1.f;
    float roll_extra = 0.f;
    float pk_b = 15.2f, pk_c = 1.49f, pk_d = 1.4f;
    bool use_pacejka = false;
    float arm_steer_mul = 1.f;
    if (r.has_wheel_params) {
      steer = r.wheel_steer;
      drive_mul = r.wheel_drive;
      if (drive_mul < 0.f) drive_mul = 0.f;
      if (drive_mul > 2.f) drive_mul = 2.f;
      radius_mul = r.wheel_radius / 0.32f;
      if (radius_mul < 0.25f) radius_mul = 0.25f;
      if (radius_mul > 2.5f) radius_mul = 2.5f;
      grip_mul = r.wheel_friction * r.wheel_sliction;
      if (grip_mul < 0.05f) grip_mul = 0.05f;
      if (grip_mul > 3.f) grip_mul = 3.f;
      if (r.wheel_brake > brake) brake = r.wheel_brake;
      if (brake > 1.f) brake = 1.f;
      if (r.wheel_hbrake > handbrake) handbrake = r.wheel_hbrake;
      if (handbrake > 1.f) handbrake = 1.f;
      roll_extra = r.wheel_roll_res * 800.f;
      if (roll_extra < 0.f) roll_extra = 0.f;
      if (roll_extra > 8.f) roll_extra = 8.f;
      pk_b = r.wheel_pk_b;
      pk_c = r.wheel_pk_c;
      pk_d = r.wheel_pk_d;
      use_pacejka = true;
      if (r.wheel_arm_len > 0.05f) {
        arm_steer_mul = 0.244f / r.wheel_arm_len;
        if (arm_steer_mul < 0.5f) arm_steer_mul = 0.5f;
        if (arm_steer_mul > 2.f) arm_steer_mul = 2.f;
      }
    }
    if (airborne_now) {
      grip_mul *= 0.08f;
      steer *= 0.08f;
      arm_steer_mul *= 0.2f;
    }
    // Phase 2.103: ASR cuts excess throttle; ABS boosts grip under brake.
    {
      float asr_v = tree_field_get_float(self, "asr");
      float abs_v = tree_field_get_float(self, "abs");
      if (controller) {
        const float a1 = tree_field_get_float(controller, "asr");
        const float a2 = tree_field_get_float(controller, "abs");
        if (a1 > asr_v) asr_v = a1;
        if (a2 > abs_v) abs_v = a2;
      }
      if (asr_v > 0.f && throttle > 0.35f) {
        float cut = asr_v * 0.4f;
        if (cut > 0.65f) cut = 0.65f;
        throttle *= 1.f - cut * (throttle - 0.35f);
        if (throttle < 0.f) throttle = 0.f;
      }
      if (abs_v > 0.f && brake > 0.2f) {
        grip_mul *= 1.f + abs_v * 0.45f;
        if (grip_mul > 4.f) grip_mul = 4.f;
      }
    }

    // Gear shift on edge of AXIS_GEAR_UPDOWN.
    if (gear_axis > 0.5f && r.gear_axis_prev <= 0.5f) {
      if (r.gear < 5) ++r.gear;
    } else if (gear_axis < -0.5f && r.gear_axis_prev >= -0.5f) {
      if (r.gear > -1) --r.gear;
    }
    r.gear_axis_prev = gear_axis;

    const float yaw = r.oy;
    const float fx = std::sin(yaw);
    const float fz = std::cos(yaw);
    const float dx = (r.gear < 0) ? -fx : fx;
    const float dz = (r.gear < 0) ? -fz : fz;
    float spd = std::sqrt(r.vx * r.vx + r.vz * r.vz);

    float engage = 1.f - clutch;
    if (engage < 0.f) engage = 0.f;
    if (engage > 1.f) engage = 1.f;
    if (r.gear == 0) engage = 0.f;

    auto gear_accel = [](int32_t g) -> float {
      switch (g) {
        case -1:
          return 0.55f;
        case 1:
          return 1.00f;
        case 2:
          return 0.90f;
        case 3:
          return 0.75f;
        case 4:
          return 0.60f;
        case 5:
          return 0.48f;
        default:
          return 0.f;
      }
    };
    auto gear_vmax = [](int32_t g) -> float {
      switch (g) {
        case -1:
          return 0.40f;
        case 1:
          return 0.55f;
        case 2:
          return 0.70f;
        case 3:
          return 0.85f;
        case 4:
          return 1.00f;
        case 5:
          return 1.18f;
        default:
          return 0.f;
      }
    };

    const bool have_tq = r.drive_torque_nm > 1.f;
    float torque_scale = 1.f;
    if (have_tq) {
      torque_scale = r.drive_torque_nm / kRefDriveTorqueNm;
      if (torque_scale < 0.2f) torque_scale = 0.2f;
      if (torque_scale > 2.5f) torque_scale = 2.5f;
    }

    const float accel =
        kDriveAccel * gear_accel(r.gear) *
        (1.f + ((!have_tq && nitro > 0.01f) ? kNitroBoost * nitro : 0.f)) *
        engage * drive_mul * radius_mul * torque_scale;
    if (throttle > 0.01f && accel > 0.01f) {
      r.vx += dx * accel * throttle * dt;
      r.vz += dz * accel * throttle * dt;
    }
    // Phase 2.92: engine braking — engaged gear, throttle released (needs
    // a controller so free-coast test bodies without maps keep sliding).
    if (controller && throttle < 0.05f && brake < 0.01f && handbrake < 0.01f &&
        engage > 0.05f && r.gear != 0 && spd > 0.8f) {
      const float nx = r.vx / spd;
      const float nz = r.vz / spd;
      float eb = kEngineBrake * gear_accel(r.gear) * engage;
      if (have_tq) eb *= (0.65f + 0.35f * torque_scale);
      if (eb < 0.f) eb = 0.f;
      r.vx -= nx * eb * dt;
      r.vz -= nz * eb * dt;
      spd = std::sqrt(r.vx * r.vx + r.vz * r.vz);
    }
    if (brake > 0.01f) {
      if (spd > 0.5f) {
        const float dec = kBrakeDecel * brake * dt;
        const float nx = r.vx / spd, nz = r.vz / spd;
        r.vx -= nx * dec;
        r.vz -= nz * dec;
      } else if (r.gear >= 0) {
        r.vx -= fx * kDriveAccel * 0.35f * brake * dt;
        r.vz -= fz * kDriveAccel * 0.35f * brake * dt;
      }
    }
    if (handbrake > 0.01f && spd > 0.2f) {
      const float dec = kHandbrakeDecel * handbrake * dt;
      const float nx = r.vx / spd, nz = r.vz / spd;
      r.vx -= nx * dec;
      r.vz -= nz * dec;
    }
    const float drag = kDrag + roll_extra;
    r.vx *= (1.f - drag * dt);
    r.vz *= (1.f - drag * dt);
    {
      const float fwd_spd = r.vx * fx + r.vz * fz;
      float lat_x = r.vx - fx * fwd_spd;
      float lat_z = r.vz - fz * fwd_spd;
      float pace_mul = 1.f;
      if (use_pacejka) {
        // Simplified Magic Formula: Fy ~ D*sin(C*atan(B*alpha)).
        const float lat_spd = std::sqrt(lat_x * lat_x + lat_z * lat_z);
        const float alpha =
            std::atan2(lat_spd, std::fabs(fwd_spd) + 0.5f);
        const float mf =
            pk_d * std::sin(pk_c * std::atan(pk_b * alpha));
        constexpr float kStock = 1.4f * 1.49f * 15.2f;
        float stiff = (pk_d * pk_c * pk_b) / kStock;
        if (stiff < 0.05f) stiff = 0.05f;
        if (stiff > 4.f) stiff = 4.f;
        float shape = 1.f;
        if (std::fabs(pk_d) > 0.01f) {
          shape = std::fabs(mf) / std::fabs(pk_d);
          if (shape < 0.15f) shape = 0.15f;
          if (shape > 1.f) shape = 1.f;
        }
        pace_mul = stiff * (0.5f + 0.5f * shape);
      }
      const float grip =
          ((handbrake > 0.01f) ? kHandbrakeGrip : kLateralGrip) * grip_mul *
          pace_mul;
      const float damp = 1.f / (1.f + grip * dt);
      lat_x *= damp;
      lat_z *= damp;
      r.vx = fx * fwd_spd + lat_x;
      r.vz = fz * fwd_spd + lat_z;
    }
    spd = std::sqrt(r.vx * r.vx + r.vz * r.vz);
    const float max_spd =
        kMaxSpeed * gear_vmax(r.gear) *
        (1.f + (nitro > 0.01f ? 0.25f * nitro : 0.f)) * radius_mul;
    if (max_spd > 0.5f && spd > max_spd) {
      r.vx *= max_spd / spd;
      r.vz *= max_spd / spd;
      spd = max_spd;
    }
    const float steer_scale = spd / (spd + 5.f);
    r.wy = -steer * kSteerRate * steer_scale * arm_steer_mul;
    if (airborne_now) r.wy *= 0.15f;

    // Phase 2.81: rough engine RPM from road speed × gear (for getTorque).
    float gear_factor = 1.f;
    switch (r.gear) {
      case -1:
        gear_factor = 1.4f;
        break;
      case 0:
        gear_factor = 0.f;
        break;
      case 1:
        gear_factor = 1.6f;
        break;
      case 2:
        gear_factor = 1.25f;
        break;
      case 3:
        gear_factor = 1.0f;
        break;
      case 4:
        gear_factor = 0.82f;
        break;
      case 5:
        gear_factor = 0.68f;
        break;
      default:
        break;
    }
    float rpm = 900.f;
    if (r.gear != 0) rpm = 800.f + spd * 90.f * gear_factor;
    if (rpm < 800.f) rpm = 800.f;
    if (rpm > 9000.f) rpm = 9000.f;
    r.engine_rpm = rpm;
  }

  physics_integrate(dt);
  {
    float px = 0, pz = 0, half = 0.5f;
    float ride_bias = 0.f;
    float spring = 0.f, damp = 0.f;
    bool use_susp = false;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      auto& r = R(self);
      px = r.px;
      pz = r.pz;
      half = contact_half_height(r);
      if (r.has_wheel_params) {
        ride_bias = (r.wheel_rest_len - 0.39f) * 0.5f;
        if (ride_bias < -0.15f) ride_bias = -0.15f;
        if (ride_bias > 0.25f) ride_bias = 0.25f;
        spring = r.wheel_spring;
        damp = r.wheel_damp;
        use_susp = spring > 1.f || damp > 1.f || std::fabs(ride_bias) > 0.001f;
      }
    }
    float ox = 0, oy = 0, oz = 0, rdx = 0, rdy = 0, rdz = 0;
    const bool on_road =
        physics_road_project(px, pz, &ox, &oy, &oz, &rdx, &rdy, &rdz);
    {
      std::lock_guard<std::mutex> lock(g_mu);
      auto& r = R(self);
      if (on_road) {
        r.py = oy + half + ride_bias;
        if (r.vy < 0.f) r.vy = 0.f;
        // Phase 2.93: gentle yaw toward road tangent when grounded + low steer.
        if (controller && !r.airborne) {
          float th =
              input_map_get_logical(controller, kAxisThrottle);
          float st =
              r.has_wheel_params
                  ? r.wheel_steer
                  : input_map_get_logical(controller, kAxisTurnLR);
          const float spd_xz = std::sqrt(r.vx * r.vx + r.vz * r.vz);
          if (th < 0.35f && std::fabs(st) < 0.25f && spd_xz > 1.5f) {
            float tlen = std::sqrt(rdx * rdx + rdz * rdz);
            if (tlen > 1e-4f) {
              rdx /= tlen;
              rdz /= tlen;
              float want = std::atan2(rdx, rdz);
              // Prefer orientation matching velocity when reversing.
              const float fwd = r.vx * std::sin(r.oy) + r.vz * std::cos(r.oy);
              if (fwd < -0.5f) want += 3.14159265f;
              float dyaw = want - r.oy;
              while (dyaw > 3.14159265f) dyaw -= 6.2831853f;
              while (dyaw < -3.14159265f) dyaw += 6.2831853f;
              if (std::fabs(dyaw) < 0.85f) {
                // Phase 2.102: Vehicle.steerhelp scales road yaw assist.
                float help = tree_field_get_float(self, "steerhelp");
                if (controller) {
                  const float ch = tree_field_get_float(controller, "steerhelp");
                  if (ch > help) help = ch;
                }
                if (help < 0.f) help = 0.f;
                if (help > 2.f) help = 2.f;
                float w = kRoadYawAssist * (1.f + help) * dt *
                          (1.f - std::fabs(st));
                if (w > 1.f) w = 1.f;
                r.oy += dyaw * w;
              }
            }
          }
        }
        // Phase 2.95: blend body pitch to road slope (rdy = ΔY / ΔXZ).
        if (!r.airborne) {
          float want_p = std::atan(rdy);
          const float along = r.vx * rdx + r.vz * rdz;
          if (along < -0.5f) want_p = -want_p;
          float dp = want_p - r.op;
          float wp = kRoadPitchAssist * dt;
          if (wp > 1.f) wp = 1.f;
          r.op += dp * wp;
        }
      } else if (use_susp) {
        const float min_y = g_ground_y + half;
        float supported_y = min_y + ride_bias;
        if (supported_y < min_y) supported_y = min_y;
        // Near ground: sit at spring rest ride height (arcade, no fight vs g).
        if (r.py <= supported_y + 0.08f) {
          r.py = supported_y;
          if (r.vy < 0.f) r.vy = 0.f;
        } else {
          const float err = r.py - supported_y;
          float k = (spring / 20000.f) * 50.f;
          if (k < 0.f) k = 0.f;
          if (k > 120.f) k = 120.f;
          r.vy -= k * err * dt;
        }
        float dn = (damp / 2000.f) * 10.f;
        if (dn > 40.f) dn = 40.f;
        r.vy *= 1.f / (1.f + dn * dt);
        if (r.py < min_y) {
          r.py = min_y;
          if (r.vy < 0.f) r.vy = 0.f;
        }
      }
    }
  }
}

int32_t physics_get_gear(InvObject* self) {
  if (!self) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_res.find(self);
  return it == g_res.end() ? 0 : it->second.gear;
}

int32_t physics_is_airborne(InvObject* self) {
  if (!self) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_res.find(self);
  return it == g_res.end() ? 0 : it->second.airborne;
}

void physics_set_gear(InvObject* self, int32_t gear) {
  if (!self) return;
  if (gear < -1) gear = -1;
  if (gear > 5) gear = 5;
  std::lock_guard<std::mutex> lock(g_mu);
  auto& r = R(self);
  r.gear = gear;
  r.gear_axis_prev = 0.f;
}

float physics_speed_square(InvObject* self) {
  if (!self) return 0.f;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_res.find(self);
  if (it == g_res.end()) return 0.f;
  const auto& r = it->second;
  return r.vx * r.vx + r.vy * r.vy + r.vz * r.vz;
}

int32_t physics_shape(InvObject* self) {
  if (!self) return 0;
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_res.find(self);
  return it == g_res.end() ? 0 : it->second.shape;
}

void physics_extents(InvObject* self, float* a, float* b, float* c) {
  float hx = 0, hy = 0, hz = 0;
  if (self) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_res.find(self);
    if (it != g_res.end()) {
      hx = it->second.hx;
      hy = it->second.hy;
      hz = it->second.hz;
    }
  }
  if (a) *a = hx;
  if (b) *b = hy;
  if (c) *c = hz;
}

bool physics_pick_osd_gadget(float ndc_x, float ndc_y, InvObject** out_phy,
                             InvObject** out_group, float* out_px, float* out_py,
                             float* out_pz) {
  // Java Osd.createButton: phy.createBox(g, rWidth, rHeight, 0.001) then
  // setMatrix(convertTextCoordinates). Group.activate setEventMask(EVENT_CURSOR)
  // — physics instances send to parent (Group.java). Thin Z = OSD vs car boxes.
  struct Cand {
    InvObject* phy = nullptr;
    InvObject* parent = nullptr;
    float px = 0, py = 0, pz = 0, hx = 0, hy = 0, hz = 0;
  };
  std::vector<Cand> cands;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto& kv : g_res) {
      const ResState& r = kv.second;
      if (r.shape != 1) continue;
      // half of 0.001 = 0.0005; car boxes are orders of magnitude larger.
      if (r.hz <= 0.f || r.hz >= 0.01f) continue;
      if (r.hx <= 0.f || r.hy <= 0.f) continue;
      // PE collides posed primitives; TREE createBox without setMatrix stays
      // at origin and would false-hit every NDC near 0.
      if (!r.pose_set) continue;
      if (!kv.first || !r.parent) continue;
      Cand c;
      c.phy = kv.first;
      c.parent = r.parent;
      c.px = r.px;
      c.py = r.py;
      c.pz = r.pz;
      c.hx = r.hx;
      c.hy = r.hy;
      c.hz = r.hz;
      cands.push_back(c);
    }
  }
  constexpr int32_t kEventCursor = 0x00010000;
  constexpr float kScale3d = 1.469f;  // Osd.SCALE_3D
  InvObject* state = game_logic_actual_state();
  InvObject* focus_osd = state ? tree_field_get_obj(state, "osd") : nullptr;
  InvObject* best_phy = nullptr;
  InvObject* best_group = nullptr;
  float best_px = 0, best_py = 0, best_pz = 0;
  float best_area = 0.f;
  bool have = false;
  for (const Cand& c : cands) {
    if ((tree_field_get_int(c.parent, "event_mask") & kEventCursor) == 0)
      continue;
    InvObject* osd = tree_field_get_obj(c.parent, "osd");
    if (!osd) continue;
    // No current-state OSD → do not guess (Garage chrome would steal Valocity).
    if (!focus_osd || osd != focus_osd) continue;
    InvObject* gh = tree_field_get_obj(osd, "globalHandler");
    if (!gh || gh != state) continue;
    float vw = tree_field_get_float(osd, "vpWidth");
    float vh = tree_field_get_float(osd, "vpHeight");
    float va = tree_field_get_float(osd, "vpAspect");
    if (vw <= 0.f) vw = 1.f;
    if (vh <= 0.f) vh = 1.f;
    if (va <= 0.f) va = 1.f;
    const float cx3 = ndc_x * kScale3d * va * vw;
    const float cy3 = -ndc_y * kScale3d * vh;
    if (std::fabs(cx3 - c.px) > c.hx) continue;
    if (std::fabs(cy3 - c.py) > c.hy) continue;
    const float area = c.hx * c.hy;
    if (!have || c.pz > best_pz + 1e-6f ||
        (std::fabs(c.pz - best_pz) <= 1e-6f && area < best_area)) {
      have = true;
      best_phy = c.phy;
      best_group = c.parent;
      best_px = c.px;
      best_py = c.py;
      best_pz = c.pz;
      best_area = area;
    }
  }
  if (out_phy) *out_phy = best_phy;
  if (out_group) *out_group = best_group;
  if (out_px) *out_px = best_px;
  if (out_py) *out_py = best_py;
  if (out_pz) *out_pz = best_pz;
  return have;
}


// GroundRef route natives → GroundRef_route.cpp

// ---- SfxRef ----
// nplay @ 0x00480D40 — body in Audio/Sound.cpp (Sfx_3DListenerCull).

// PE @ 0x00480E60 size 0x7b (int_convert 123). UnboxArg (I)V: dest0=this
// (arg_0), dest1=instance (var_4). Native.ptr dword_62E008; 0 → "!Mighty
// ERROR" then ret (void). Else thiscall SfxRef_stopInstance @ 0x0048D180
// (ecx=handle, push instance): inner=[handle+0xC]; type!=1 → vtable+0x14
// (1.0f); sub_5447D0(inner, 0x80000000, 0, 0); sign → skip; else
// vtable+0xC(1.0f) → payload; voice=*(payload+0xC); voice &&
// *(voice+0x48)>=0 → sub_550870(instance, *[handle+8] rid). Gate is
// Native.ptr — NOT id ([handle+8]). Host: !self / no g_res = handle 0
// (silent); else audio_sfx_stop(instance) stand-in (no load walk /
// voice+0x48 mixer field — race124 still OOS, no invent).
// race125: IDA renamed Sfx_stopVoiceInstance @ 0x550870; host still
// audio_sfx_stop stand-in (no voice+0x48 gate).
void java_util_resource_SfxRef_stop(InvObject* self, int32_t instance) {
  // PE @ 0x00480E60 size 0x7b — Unbox this+I; Native.ptr → SfxRef_stopInstance
  // @ 0x0048D180 → Sfx_stopVoiceInstance @ 0x00550870.
  if (!self) return;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_res.find(self) == g_res.end()) return;  // Native.ptr==0
  }
  audio_sfx_stop(instance);
}

// ---------------------------------------------------------------------------
// W9A / W11B / W12B — PE Engine_LoadGameInit @ 0x0053A5E0 size 0x51f.
// cdecl (a1=wtroot_rh, a2=type_rh, a3=params, a4=alias) → GI*|0.
// Callers: voidEvent case2 @ 0x45C3BD, GameRef_create @ 0x47D824,
// Engine_MainLoop @ 0x4289E5, GroundRef_addTrafficN @ 0x4841C4, …
//
// PE factory chain (IDA W11B/W12B):
//   ResourceEngine_AllocLocalRid(g_ResourceEngine@618D48) @ 0x536ED0
//   create desc {type=1, rid, 33, 0, 0, alias*} @ 0x53A68E
//   wtroot+0xC!=0 → CreateNodeUnder @ 0x536900 (a6=parent)
//   wtroot+0xC==0 → CreateNode @ 0x536890 → CreateNodeByDesc a6=0
//     → ResourceEngine_ResolveParent(wt_key, type=1, 0) @ 0x537000
//     → InstanceNode_PoolAlloc @ 0x538720 + ctor @ 0x53E6E0
//     → TREE splice @ 0x536DFF (parent+0x38 / sentinel parent+0x30)
//   parent LOD @ 0x53A706; GI+0x54 |= 0x200 @ 0x53A76D
//
// Host: PoolAlloc + TREE + ResolveParent RID slices (cache / FindByRid).
// W13B: eng+0x11C SimObject root + Class*→FQN → attach_gametype.
// W15C: parent LOD @ 0x53A706 — PrepareLod gate + setLodReadyBit stand-in.
// Gaps: GameType ctor vtbl+0xC @ 0x53A8B2; sub_537240 non-early / PE vtbl
//   dispatch (host GI has no ResNode_vtbl*).
// ---------------------------------------------------------------------------

namespace {

// PE InstanceNode slot 348 (0x15C) — PoolAlloc @ 0x538720.
// ResHandle_Rebind owner (@ 0x00429060 a2):
//   +0x4/+0x8 dllist; +0x14 parent; +0x20→&+0x30 (FindByRid walk);
//   +0x30 sentinel / +0x38 head (TREE); +0x48 RH HEAD; +0x4C type;
//   +0x50 AllocLocalRid key; +0x54 flags; +0x60 alias*; +0x68 lod.
struct HostLoadGameInitGi {
  void* vtbl = nullptr;             // +0x0
  void* list_prev = nullptr;        // +0x4  → parent+0x30 sentinel
  void* list_next = nullptr;        // +0x8  old child head
  uint8_t pad_0c[0x8]{};            // +0xC .. +0x13
  void* parent = nullptr;           // +0x14 PE v9[5]
  uint8_t pad_18[0x8]{};            // +0x18 .. +0x1F
  void* find_by_rid_head = nullptr; // +0x20 PE [this+0x20] → &+0x30
  uint8_t pad_24[0xC]{};            // +0x24 .. +0x2F
  void* child_sentinel = nullptr;   // +0x30 PE list sentinel (addr taken)
  void* pad_34 = nullptr;           // +0x34
  void* child_head = nullptr;       // +0x38 PE v10[14] / init → &+0x1C
  uint8_t pad_3c[0xC]{};            // +0x3C .. +0x47
  void* rh_list_head = nullptr;     // +0x48
  int32_t type = 1;                 // +0x4C INSTANCE
  int32_t rebind_key = 0;           // +0x50 → rh+8 after Rebind
  int32_t flags = 0;                // +0x54
  int32_t desc_pad_c = 0;           // +0x58
  int32_t desc_pad_d = 0;           // +0x5C
  char* alias = nullptr;            // +0x60
  int32_t lod_state = 0;            // +0x64 PE byte&1 PrepareLod gate
  float lod_scale = 0.f;            // +0x68 PE v13[26]
  float lod_copy_6c = 0.f;          // +0x6C PE parent+108
  float lod_copy_70 = 0.f;          // +0x70 PE parent+112
  uint8_t pad_74_80[0xC]{};         // +0x74 .. +0x7F
  // W21D PE ResNode_unionChildAabb @ 0x4988A0: state/center/half.
  int32_t aabb_state = 0;           // +0x80; 1=ready; 2=skip contrib
  float aabb_cx = 0.f;              // +0x84
  float aabb_cy = 0.f;              // +0x88
  float aabb_cz = 0.f;              // +0x8C
  float aabb_hx = 0.f;              // +0x90 half-extent
  float aabb_hy = 0.f;              // +0x94
  float aabb_hz = 0.f;              // +0x98
  // +0x9C .. +0xD7: initExt +0xB8 bias / +0xBC scale (before embed mid).
  uint8_t pad_9c_d8[0xD8 - 0x9C]{};
  // PE ResNode_getEmbeddedMid @ 0x53EFA0 → this+0xD8 (slot mid size 0x84).
  // W19C: mid+0x38 ResHandle_Rebind type owner.
  // W20A: mid+0x50 script (a3) @ 0x53A515.
  uint8_t mid_embed[0x84]{};
};
static_assert(offsetof(HostLoadGameInitGi, list_prev) == 0x4, "GI+0x4");
static_assert(offsetof(HostLoadGameInitGi, parent) == 0x14, "GI+0x14");
static_assert(offsetof(HostLoadGameInitGi, find_by_rid_head) == 0x20,
              "GI+0x20 FindByRid");
static_assert(offsetof(HostLoadGameInitGi, child_sentinel) == 0x30, "GI+0x30");
static_assert(offsetof(HostLoadGameInitGi, child_head) == 0x38, "GI+0x38");
static_assert(offsetof(HostLoadGameInitGi, rh_list_head) == 0x48, "GI+0x48");
static_assert(offsetof(HostLoadGameInitGi, type) == 0x4C, "GI+0x4C");
static_assert(offsetof(HostLoadGameInitGi, rebind_key) == 0x50, "GI+0x50");
static_assert(offsetof(HostLoadGameInitGi, flags) == 0x54, "GI+0x54");
static_assert(offsetof(HostLoadGameInitGi, alias) == 0x60, "GI+0x60");
static_assert(offsetof(HostLoadGameInitGi, lod_scale) == 0x68, "GI+0x68");
static_assert(offsetof(HostLoadGameInitGi, aabb_state) == 0x80, "GI+0x80 AABB");
static_assert(offsetof(HostLoadGameInitGi, aabb_cx) == 0x84, "GI+0x84");
static_assert(offsetof(HostLoadGameInitGi, aabb_hx) == 0x90, "GI+0x90");
static_assert(offsetof(HostLoadGameInitGi, mid_embed) == 0xD8, "GI+0xD8 mid");
static_assert(sizeof(HostLoadGameInitGi) == 348, "InstanceNode slot 348");

std::vector<HostLoadGameInitGi*> g_loadgameinit_gis;
// PE ResourceEngine_AllocLocalRid @ 0x536ED0: u16 freelist @ eng+0xDA0,
// refill 128, return rid|0xFFFF0000 (empty → 0xFFFF0000). Host seq stand-in.
uint16_t g_lgi_rid_seq = 1;
// g_lgi_create_count — PE @ 0x765F60; defined with GCSweep soft lives (W32C).

// PE ResolveParent RID cache @ eng+0x106E9C (active) / +0x106E94 (MRU).
// Entry: +0x14 rid, +0x18 node*. Host: flat rid→node map (hold g_mu).
std::unordered_map<int32_t, void*> g_resolve_parent_rid;
// PE pending miss RIDs @ eng+0x106ECC count / +0x106ED0 ptr (a4&1==0).
std::vector<int32_t> g_resolve_parent_pending;

// PE InstanceNode_PoolAlloc @ 0x538720 (g_InstanceNodePool @ 0x765F70):
// freelist @ pool+4; else slab index, stride 348, batch 1024 (malloc 0x57000).
struct HostInstanceNodePool {
  void* freelist = nullptr;
  std::vector<std::vector<uint8_t>> slabs;
  uint32_t used = 0;
  uint32_t capacity = 0;
};
HostInstanceNodePool g_inode_pool;

uint32_t host_alloc_local_rid() {
  uint16_t rid = g_lgi_rid_seq++;
  if (rid == 0) rid = g_lgi_rid_seq++;
  return 0xFFFF0000u | static_cast<uint32_t>(rid);
}

HostLoadGameInitGi* host_instance_node_pool_alloc() {
  constexpr uint32_t kBatch = 1024;
  constexpr size_t kSlot = 348;
  HostLoadGameInitGi* gi = nullptr;
  if (g_inode_pool.freelist) {
    auto* slot = reinterpret_cast<HostLoadGameInitGi*>(g_inode_pool.freelist);
    g_inode_pool.freelist = *reinterpret_cast<void**>(slot);
    gi = new (slot) HostLoadGameInitGi{};
  } else {
    if (g_inode_pool.used == g_inode_pool.capacity) {
      g_inode_pool.slabs.emplace_back(kBatch * kSlot);
      g_inode_pool.capacity += kBatch;
    }
    const uint32_t idx = g_inode_pool.used++;
    const uint32_t slab_i = idx / kBatch;
    const uint32_t slot_i = idx % kBatch;
    uint8_t* raw = g_inode_pool.slabs[slab_i].data() + slot_i * kSlot;
    gi = new (raw) HostLoadGameInitGi{};
  }
  if (gi) {
    // PE InstanceNode_initExt @ 0x544D40: +0xB8 bias=0, +0xBC scale=1.0
    // (ResNode_setLodCopy6C/70 formula). pad_74_d8 / mid_embed otherwise 0.
    auto* p = reinterpret_cast<uint8_t*>(gi);
    *reinterpret_cast<float*>(p + 0xB8) = 0.f;
    *reinterpret_cast<float*>(p + 0xBC) = 1.0f;
  }
  return gi;
}

// PE sub_551750 Node base: [+0x20]=&+0x30; [+0x38]=&+0x1C (FindByRid/TREE).
void host_node_ensure_child_list_hdr(uint8_t* p) {
  if (!p) return;
  void** p38 = reinterpret_cast<void**>(p + 0x38);
  if (*p38) return;
  *reinterpret_cast<void**>(p + 0x20) = p + 0x30;
  *reinterpret_cast<void**>(p + 0x30) = nullptr;
  *reinterpret_cast<void**>(p + 0x34) = nullptr;
  *p38 = p + 0x1C;
}

// PE ResourceEngine_CreateNodeByDesc TREE register @ 0x536DFF:
//   node[+0x14]=parent; old=parent[+0x38]; old[+4]=node;
//   node[+4]=&parent[+0x30]; node[+8]=old; parent[+0x38]=node.
void host_create_node_by_desc_tree_register(HostLoadGameInitGi* node,
                                           void* parent) {
  if (!node || !parent) return;
  auto* n = reinterpret_cast<uint8_t*>(node);
  auto* p = reinterpret_cast<uint8_t*>(parent);
  host_node_ensure_child_list_hdr(p);
  host_node_ensure_child_list_hdr(n);
  *reinterpret_cast<void**>(n + 0x14) = parent;
  void* old = *reinterpret_cast<void**>(p + 0x38);
  if (old) {
    *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(old) + 4) = node;
  }
  *reinterpret_cast<void**>(n + 4) = p + 0x30;
  *reinterpret_cast<void**>(n + 8) = old;
  *reinterpret_cast<void**>(p + 0x38) = node;
  node->parent = parent;
}

// W21D PE ResNode_unionChildAabb @ 0x4988A0 size 0x267 — no callees.
// Walk FindByRid chain: start=*(node+0x20) if *(start+4)!=0; next via +4
// until next==0 or *(next+4)==0. Contrib when type(+0x4C)==2 &&
// aabb_state(+0x80)!=2: center±half @ +0x84/+0x90 → union min/max.
// If any contrib: state=1; write center=(min+max)/2, half=(max-min)/2.
void host_res_node_union_child_aabb(void* node) {
  if (!node) return;
  auto* a1 = reinterpret_cast<uint8_t*>(node);
  void* head = *reinterpret_cast<void**>(a1 + 0x20);
  if (!head) return;
  void* cur =
      (*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(head) + 4) !=
       nullptr)
          ? head
          : nullptr;
  bool have = false;
  float mn_x = 0.f, mn_y = 0.f, mn_z = 0.f;
  float mx_x = 0.f, mx_y = 0.f, mx_z = 0.f;
  while (cur) {
    auto* c = reinterpret_cast<uint8_t*>(cur);
    if (*reinterpret_cast<int32_t*>(c + 0x4C) == 2 &&
        *reinterpret_cast<int32_t*>(c + 0x80) != 2) {
      const float cx = *reinterpret_cast<float*>(c + 0x84);
      const float cy = *reinterpret_cast<float*>(c + 0x88);
      const float cz = *reinterpret_cast<float*>(c + 0x8C);
      const float hx = *reinterpret_cast<float*>(c + 0x90);
      const float hy = *reinterpret_cast<float*>(c + 0x94);
      const float hz = *reinterpret_cast<float*>(c + 0x98);
      const float lo_x = cx - hx;
      const float lo_y = cy - hy;
      const float lo_z = cz - hz;
      const float hi_x = cx + hx;
      const float hi_y = cy + hy;
      const float hi_z = cz + hz;
      if (!have) {
        mn_x = lo_x;
        mn_y = lo_y;
        mn_z = lo_z;
        mx_x = hi_x;
        mx_y = hi_y;
        mx_z = hi_z;
        have = true;
      } else {
        if (lo_x < mn_x) mn_x = lo_x;
        if (hi_x > mx_x) mx_x = hi_x;
        if (lo_y < mn_y) mn_y = lo_y;
        if (hi_y > mx_y) mx_y = hi_y;
        if (lo_z < mn_z) mn_z = lo_z;
        if (hi_z > mx_z) mx_z = hi_z;
      }
    }
    cur = *reinterpret_cast<void**>(c + 4);
    if (!cur ||
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cur) + 4) ==
            nullptr) {
      cur = nullptr;
    }
  }
  if (!have) return;
  *reinterpret_cast<int32_t*>(a1 + 0x80) = 1;
  const float hx = (mx_x - mn_x) * 0.5f;
  const float hy = (mx_y - mn_y) * 0.5f;
  const float hz = (mx_z - mn_z) * 0.5f;
  *reinterpret_cast<float*>(a1 + 0x84) = mn_x + hx;
  *reinterpret_cast<float*>(a1 + 0x88) = mn_y + hy;
  *reinterpret_cast<float*>(a1 + 0x8C) = mn_z + hz;
  *reinterpret_cast<float*>(a1 + 0x90) = hx;
  *reinterpret_cast<float*>(a1 + 0x94) = hy;
  *reinterpret_cast<float*>(a1 + 0x98) = hz;
}

// W22D PE ResNode_drainSpatial @ 0x498B10 size 0x260 — cdecl(node) → 0|1.
// CreateGI while @ 0x53A54C / LoadGameInit @ 0x53AA40 until 0.
// Slice: aabb_state(+0x80)==2 → 0; parent climb +0x14 stop on
//   !grandparent | flags(+0x54)&0x30 | either aabb==2 (v5=14);
//   AABB sub_54B850/54BA20 OOS → no further climb. RID(+0x50) same → 0.
// Gaps: GameRef_dllist_unlink@4A5D00 + sub_4A5D20 reparent;
//   Type53_ensureHandleSlot@4B3EE0; sibling SimObjectList@parent+0x18;
//   AllocLocalRid@536ED0 + sub_5447B0.
int32_t host_res_node_drain_spatial(void* node) {
  if (!node) return 0;
  auto* a1 = reinterpret_cast<uint8_t*>(node);
  // PE @ 0x498b27: aabb_state==2 → return 0.
  if (*reinterpret_cast<int32_t*>(a1 + 0x80) == 2) return 0;

  void* v2 = *reinterpret_cast<void**>(a1 + 0x14);  // parent
  if (!v2) return 0;  // stock assumes parent; host safe-exit

  // PE climb @ 0x498b32..0x498ba1.
  for (;;) {
    auto* p = reinterpret_cast<uint8_t*>(v2);
    void* grand = *reinterpret_cast<void**>(p + 0x14);
    if (!grand || (*reinterpret_cast<int32_t*>(p + 0x54) & 0x30) != 0)
      break;
    // PE: parent|self aabb_state==2 → v5=14 (bit8) → break.
    const int32_t p_aabb = *reinterpret_cast<int32_t*>(p + 0x80);
    const int32_t s_aabb = *reinterpret_cast<int32_t*>(a1 + 0x80);
    if (p_aabb == 2 || s_aabb == 2) break;
    // AABB helpers OOS — stop without climbing (as if contained / bit8).
    break;
  }

  void* cur_parent = *reinterpret_cast<void**>(a1 + 0x14);
  if (!cur_parent) return 0;
  // PE @ 0x498bae: walked RID != current parent RID → reparent path.
  if (*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(v2) + 0x50) !=
      *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(cur_parent) +
                                  0x50)) {
    // reparent + Type53_ensureHandleSlot OOS
    return 0;
  }
  // Sibling SimObjectList / AllocLocalRid path OOS → return 0 (v16).
  return 0;
}

// host_lgi_set_lod_ready_bit — PE @ 0x544B90; defined near PumpLoad (W22A).

// PE ResHandle_PrepareLod @ 0x5447D0 — LoadGameInit call site a2=0,a3=0,a4=0.
// W16A/W17A: RelinkLodSlot (tryRelinkLod + freelist/OR) for maybeRelinkLod;
//   PrepareLod→RelinkLodSlot still OOS here (a2 bit29 / +0x54&0x100 gate).
//   W18B: PumpUnloadQueue mark+recycle freelist hosted;
//   W28C: SimObjectList ctor/empty + GCSweep(0) soft counters;
//   W29C: type-bucket touch@+0x150 / age→aged@+0x16C / typeGate scan shell;
//   W30C: wantUnloadFrame@53EFB0 (+0x78 stamp) / vtbl+0x30 null drain.
//   W31C: mid_gameTypeUnloadUpdate@53E620 → this PrepareLod soft.
//   W32C: GCSweep force eng+0x106ED8..EE4 + pre-scan budget Hi.
//   W33C: GCSweep touch setLodCopy6C(0) + evict move/Lo/caps soft.
//   W34: ResNode_tryUnload@53EC20 soft gate/children/lod-ready.
//   W35-03: TouchResNode@537D10 soft on success (stamp≠frame, !(flags&0x40M)).
//   W19A: PumpLoadQueue empty+LoadRing/isLoading; W20B LABEL_28 recycle;
//   W21A/W22A/W23A vtbl+0x28@53E860 LABEL_68 + parseLodBuf body slice.
// Full body (scale table dword_619DC0) OOS. Stand-in:
//   sub_537240 early @ 0x53725a when this+0x5C==0 → setLodReadyBit(1)+lod=1.0;
//   return 0 if (this+0x64)&1 else 0x80000001; Touch on success.
int32_t host_lgi_prepare_lod(void* node, int32_t flags, float a3, float a4) {
  (void)a3;
  (void)a4;
  if (!node) return static_cast<int32_t>(0x80000001u);
  auto* p = reinterpret_cast<uint8_t*>(node);
  int32_t* lod_st = reinterpret_cast<int32_t*>(p + 0x64);
  if ((flags & 0x40000000) == 0) {
    const bool need =
        (*lod_st & 1) == 0;  // PE also re-enters on scale mismatch — OOS
    if (need) {
      // PE sub_537240 early path when dword[23] (+0x5C) == 0.
      if (*reinterpret_cast<int32_t*>(p + 0x5C) == 0) {
        host_lgi_set_lod_ready_bit(node, 1);
        *reinterpret_cast<float*>(p + 0x68) = 1.0f;
      }
      // else: full LOD apply OOS — leave bit0 clear → fail below
    }
  }
  // @ 0x5449C8: !(+0x64&1) → 0x80000001.
  if ((*lod_st & 1) == 0) return static_cast<int32_t>(0x80000001u);
  // @ 0x5449DD..0x5449F7: !(flags&0x40000000) && stamp≠frame → Touch.
  if ((flags & 0x40000000) == 0 &&
      node_gc_stamp(static_cast<HostResNode*>(node)) != g_res_gc_frame)
    resource_engine_touch_res_node(static_cast<HostResNode*>(node));
  return 0;
}

// PE @ 0x53A706..0x53A764: gi vtbl+0x18/+0x14 parent floats; +0x68 copy;
// if !(parent+0x64&1): type!=1 → parent vtbl+0x14(1.0); PrepareLod(0,0,0);
// ≥0 → parent vtbl+0xC(1.0).
// ResNode_vtbl: +0x18=setLodCopy70@53F040, +0x14=setLodCopy6C@53EFF0,
//   +0xC=getEmbeddedMid@53EFA0. Formula a2*(+0xBC)+(+0xB8); initExt seeds
//   +0xB8=0,+0xBC=1.0 → direct float write ≡ vtbl hop.
void host_lgi_parent_lod_copies(HostLoadGameInitGi* gi, void* parent) {
  if (!gi || !parent) return;
  auto* pb = reinterpret_cast<uint8_t*>(parent);
  // PE gi vtbl+0x18(parent+0x70) / vtbl+0x14(parent+0x6C); +0x68 direct.
  gi->lod_copy_70 = *reinterpret_cast<float*>(pb + 0x70);
  gi->lod_copy_6c = *reinterpret_cast<float*>(pb + 0x6C);
  gi->lod_scale = *reinterpret_cast<float*>(pb + 0x68);

  int32_t* pst = reinterpret_cast<int32_t*>(pb + 0x64);
  if ((*pst & 1) != 0) return;  // PE @ 0x53A72F already ready → skip

  // PE @ 0x53A735: parent+0x4C!=1 → parent vtbl+0x14(1.0f).
  if (*reinterpret_cast<int32_t*>(pb + 0x4C) != 1) {
    const float bias = *reinterpret_cast<float*>(pb + 0xB8);
    const float scale = *reinterpret_cast<float*>(pb + 0xBC);
    *reinterpret_cast<float*>(pb + 0x6C) = 1.0f * scale + bias;
  }

  // PE @ 0x53A74F PrepareLod(parent,0,0,0); ≥0 → vtbl+0xC(1.0f).
  if (host_lgi_prepare_lod(parent, 0, 0.f, 0.f) >= 0) {
    // INSTANCE type1: getEmbeddedMid no-op side effect. Non-INSTANCE PE
    // vtbl+0xC payload hop OOS (host parent may lack ResNode_vtbl).
    (void)pb;
  }
}

static int32_t host_node_type_at(void* node) {
  if (!node) return 0;
  return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(node) + 0x4C);
}

static int32_t host_node_rid_at(void* node) {
  if (!node) return 0;
  return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(node) + 0x50);
}

// PE InstanceNode_FindByRid @ 0x544AC0 / FindByRidType @ 0x544B10:
// match [node+0x50]==rid; type filter [node+0x4C]; children via [+0x20]/+4.
// Host: recursive walk when +0x20 wired; else flat scan of known nodes.
void* host_find_by_rid_walk(void* node, int32_t rid, int32_t type, int depth) {
  if (!node || rid == 0 || depth > 4096) return nullptr;
  if (host_node_rid_at(node) == rid) {
    if (type != 0 && host_node_type_at(node) != type) return nullptr;
    return node;
  }
  auto* p = reinterpret_cast<uint8_t*>(node);
  void* cur = *reinterpret_cast<void**>(p + 0x20);
  if (!cur) return nullptr;
  // PE: require [cur+4]!=0 before walking (sentinel empty gate).
  if (*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cur) + 4) == nullptr)
    return nullptr;
  for (int guard = 0; cur && guard < 4096; ++guard) {
    void* next = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cur) + 4);
    if (!next) break;
    if (void* hit = host_find_by_rid_walk(cur, rid, type, depth + 1))
      return hit;
    cur = next;
    if (*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(cur) + 4) ==
        nullptr)
      break;
  }
  return nullptr;
}

// Hold g_mu. Flat fallback when eng+0x11C root walk misses.
void* host_find_by_rid_scan(int32_t rid, int32_t type) {
  if (!rid) return nullptr;
  for (HostLoadGameInitGi* gi : g_loadgameinit_gis) {
    if (!gi || gi->rebind_key != rid) continue;
    if (type != 0 && gi->type != type) continue;
    return gi;
  }
  for (auto& kv : g_native_ptr) {
    HostNativeHandle* h = kv.second;
    if (!h || !h->node) continue;
    if (host_node_rid_at(h->node) != rid) continue;
    if (type != 0 && host_node_type_at(h->node) != type) continue;
    return h->node;
  }
  return nullptr;
}

// PE g_ResourceEngine+0x11C SimObject root (ResolveParent miss @ 0x53708d:
// !SimObjectListEmpty(eng+0x114) && *(eng+0x11C) → FindByRid @ 0x544AC0).
// Host: single InstanceNode-shaped root; orphans TREE-spliced under it.
HostLoadGameInitGi* g_eng_sim_object_root = nullptr;

HostLoadGameInitGi* host_eng_sim_object_root() {
  if (g_eng_sim_object_root) return g_eng_sim_object_root;
  g_eng_sim_object_root = host_instance_node_pool_alloc();
  if (!g_eng_sim_object_root) return nullptr;
  host_node_ensure_child_list_hdr(
      reinterpret_cast<uint8_t*>(g_eng_sim_object_root));
  g_eng_sim_object_root->type = 0;  // root sentinel (not INSTANCE)
  g_eng_sim_object_root->rebind_key = 0;
  return g_eng_sim_object_root;
}

// Attach node under eng+0x11C when it has no parent (top-level forest).
void host_sim_object_root_adopt(HostLoadGameInitGi* node) {
  if (!node || node->parent) return;
  HostLoadGameInitGi* root = host_eng_sim_object_root();
  if (!root || node == root) return;
  host_create_node_by_desc_tree_register(node, root);
}

void host_rid_register(int32_t rid, void* node) {
  if (!rid || !node) return;
  g_resolve_parent_rid[rid] = node;
}

// PE Class_getNameCstr @ 0x404EA0:
//   mov eax,[ecx+8] / mov eax,[eax] / ret  — FQN C-string via Class+0x8.
// Host Class shell (class_box_object): tree fields "name" / "clazzname".
const char* host_class_get_name_cstr(void* clazz) {
  if (!clazz) return nullptr;
  auto* obj = reinterpret_cast<InvObject*>(clazz);
  // Host java.lang.Class shell (side-map) before PE +0x8 deref.
  if (const char* hc = tree_host_class(obj)) {
    if (hc[0] && std::strcmp(hc, "java.lang.Class") == 0) {
      if (InvObject* name = tree_field_get_obj(obj, "name")) {
        if (const char* s = string_cstr(name)) {
          if (s[0]) return s;
        }
      }
      if (InvObject* cn = tree_field_get_obj(obj, "clazzname")) {
        if (const char* s = string_cstr(cn)) {
          if (s[0]) return s;
        }
      }
      return nullptr;
    }
  }
  // PE Class*: [Class+0x8] → char*.
  void* name_slot =
      *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(clazz) + 8);
  if (!name_slot) return nullptr;
  const char* pe = *reinterpret_cast<const char**>(name_slot);
  return (pe && pe[0]) ? pe : nullptr;
}

// PE LoadGameInit @ 0x53A7BD..0x53A908:
//   type_rh+8!=0 → Rebind embed to *(type_rh+0xC) → getPayload(tag 0x80000000)
//   → Class* at payload+0x10 → Class_boxObject; host needs FQN for attach.
// Host: HostResNode.mid.script_class (+0x10) = Class* or FQN C-string.
const char* host_lgi_fqn_from_type_rh(void* type_rh) {
  if (!type_rh) return nullptr;
  auto* rh = reinterpret_cast<uint8_t*>(type_rh);
  if (*reinterpret_cast<int32_t*>(rh + 8) == 0) return nullptr;
  void* inner = *reinterpret_cast<void**>(rh + 0xC);
  if (!inner) return nullptr;

  auto* hn = reinterpret_cast<HostResNode*>(inner);
  void* at10 = hn->mid.script_class;  // PE GameType payload+0x10
  if (!at10) return nullptr;

  if (const char* fqn = host_class_get_name_cstr(at10)) return fqn;
  // Host may store FQN C-string directly at mid+0x10.
  auto* direct = reinterpret_cast<const char*>(at10);
  if (direct[0] >= 0x20 && direct[0] < 0x7F) return direct;
  return nullptr;
}

// PE LoadGameInit @ 0x53A8B2: ecx = *(type_payload+0xC) = mid.leaf (ctor*).
void* host_lgi_mid_from_type_rh(void* type_rh) {
  if (!type_rh) return nullptr;
  auto* rh = reinterpret_cast<uint8_t*>(type_rh);
  if (*reinterpret_cast<int32_t*>(rh + 8) == 0) return nullptr;
  void* inner = *reinterpret_cast<void**>(rh + 0xC);
  if (!inner) return nullptr;
  return &reinterpret_cast<HostResNode*>(inner)->mid;
}

// PE ResourceEngine_ResolveParent @ 0x537000 thiscall(eng, rid, type, flags).
// 1) Walk eng+0x106E9C cache: entry[+0x14]==rid; type→[node+0x4C]; return
//    entry[+0x18]. 2) Miss: FindByRid(eng+0x11C) @ 0x544AC0 / Type @ 0x544B10.
// 3) Miss+(flags&1)==0: push rid to eng+0x106ECC pending. Hold g_mu.
void* host_resolve_parent(int32_t rid, int32_t type, char flags) {
  if (!rid) return nullptr;

  auto it = g_resolve_parent_rid.find(rid);
  if (it != g_resolve_parent_rid.end() && it->second) {
    void* node = it->second;
    // PE @ 0x5370cd: a3!=0 → require [inner+0x4C]==a3.
    if (type != 0 && host_node_type_at(node) != type) return nullptr;
    return node;
  }

  // PE miss @ 0x53708d: !SimObjectListEmpty(eng+0x114) && *(eng+0x11C).
  void* found = nullptr;
  if (HostLoadGameInitGi* root = host_eng_sim_object_root()) {
    // PE FindByRid(this=*(eng+0x11C), rid) — walk forest from root.
    found = host_find_by_rid_walk(root, rid, type, 0);
  }
  if (!found) {
    for (HostLoadGameInitGi* gi : g_loadgameinit_gis) {
      if (!gi) continue;
      found = host_find_by_rid_walk(gi, rid, type, 0);
      if (found) break;
    }
  }
  if (!found) {
    for (auto& kv : g_native_ptr) {
      if (!kv.second || !kv.second->node) continue;
      found = host_find_by_rid_walk(kv.second->node, rid, type, 0);
      if (found) break;
    }
  }
  if (!found) found = host_find_by_rid_scan(rid, type);

  if (found) {
    g_resolve_parent_rid[rid] = found;
    return found;
  }

  // PE @ 0x5371f7: (a4 & 1)==0 → append rid to pending array.
  if ((flags & 1) == 0) g_resolve_parent_pending.push_back(rid);
  return nullptr;
}

// PE CreateNodeByDesc @ 0x536A10 — W26C host slice: case1 INSTANCE only
// when parent (a6) non-null. Cases 2..21 / ResolveParent(a6==0) OOS.
// CreateNodeUnder passes (a3=packId unused here, desc, xform, a7, parent).
void* host_create_node_by_desc_under_parent(const int32_t desc[5],
                                            const uint32_t* xform7_or_null,
                                            int /*a7*/, void* parent) {
  if (!desc || !parent) return nullptr;
  if (desc[0] != 1) return nullptr;  // non-INSTANCE ctors OOS
  HostLoadGameInitGi* gi = host_instance_node_pool_alloc();
  if (!gi) return nullptr;
  host_node_ensure_child_list_hdr(reinterpret_cast<uint8_t*>(gi));
  g_loadgameinit_gis.push_back(gi);
  // Node_initFromCreateDesc @ 0x54464D — memcpy 0x14 → +0x4C..+0x5C.
  gi->type = desc[0];
  gi->rebind_key = desc[1];
  gi->flags = desc[2];
  gi->desc_pad_c = desc[3];
  gi->desc_pad_d = desc[4];
  // InstanceNode_initExt @ 0x544DD7 — optional 0x1C xform → +0x80.
  if (xform7_or_null) {
    std::memcpy(reinterpret_cast<uint8_t*>(gi) + 0x80, xform7_or_null, 0x1C);
  }
  host_create_node_by_desc_tree_register(gi, parent);  // @ 0x536DFF
  if (desc[1] != 0) host_rid_register(desc[1], gi);
  return gi;
}

// PE ResourceEngine_CreateNodeUnder @ 0x536900 size 0x66 —
// __thiscall(eng, a2=remapped, a3, a4=desc*, a5=parent, a6=xform*|0, a7).
// HIWORD(a2)!=FFFF && <count → EnsureIndex(eng+0xFFE94+0x70*hi);
// CreateNodeByDesc(eng, a3, desc, xform, a7, parent); setLodReadyBit(node,0).
void* host_resource_engine_create_node_under(uint32_t remapped_id, uint32_t a3,
                                            const int32_t desc[5], void* parent,
                                            const uint32_t* xform7_or_null,
                                            int a7) {
  (void)a3;  // PE forwards to CreateNodeByDesc a2 (ResolveParent when !parent)
  host_pack_eng_sync_from_rpak();
  const uint32_t hi = remapped_id >> 16;  // @ 0x536904
  if (hi != 0xFFFFu && hi < g_pack_eng.pack_count && hi < kMaxPackSlots) {
    // @ 0x536919..0x53692C — eng+0xFFE94 + 0x70*hi
    (void)host_resource_pack_ensure_index(g_pack_eng.slots[hi].raw);
  }
  void* node = host_create_node_by_desc_under_parent(desc, xform7_or_null, a7,
                                                     parent);  // @ 0x53694C
  if (node) host_lgi_set_lod_ready_bit(node, 0);               // @ 0x53695B
  return node;
}

}  // namespace

// PE ResourceEngine_RemapFromPath @ 0x538440 — public Soft entry.
uint32_t resource_engine_remap_from_path(const char* path, uint32_t local) {
  return host_resource_engine_remap_from_path(path, local);
}

// PE Chassis_forceUpdate_cloneHdr lazy Remap+Bind @ 0x43EDD7..0x43EE56.
void resource_engine_clone_hdr_lazy_binds() {
  host_clone_hdr_lazy_remap_binds();
}

void* resource_engine_void_event_type_rh() {
  return &g_voidEvent_LoadGameInit_typeRH_soft;
}

uint32_t resource_engine_void_event_type_rh_id() {
  return static_cast<uint32_t>(
      reinterpret_cast<uintptr_t>(g_voidEvent_LoadGameInit_typeRH_soft.id));
}

uint32_t resource_engine_particles_rh_a_id() {
  return static_cast<uint32_t>(
      reinterpret_cast<uintptr_t>(g_RH_particles_rpk_A_soft.id));
}

uint32_t resource_engine_particles_rh_b_id() {
  return static_cast<uint32_t>(
      reinterpret_cast<uintptr_t>(g_RH_particles_rpk_B_soft.id));
}

void* engine_load_game_init(void* wtroot_rh, void* type_rh,
                            const char* params, const char* alias) {
  (void)params;
  // PE @ 0x53A640: if *(wtroot+8)==0 → return null (no create).
  if (!wtroot_rh) return nullptr;
  const auto* wt =
      reinterpret_cast<const uint8_t*>(wtroot_rh);
  const int32_t wt_key = *reinterpret_cast<const int32_t*>(wt + 8);
  if (wt_key == 0) return nullptr;

  // PE @ 0x53A5F8: type_rh+8!=0 → payload+0x4C RESTYPE==8 (GameType) or
  // log "Wrong GameType!" — warn-only, still creates. type_rh may be
  // g_voidEvent_LoadGameInit_typeRH @ 0x63C680 (caller-owned ResHandle).

  // PE create desc @ 0x53A68E: type=1, AllocLocalRid, +8=33, alias* @ +0x14.
  const uint32_t local_rid = host_alloc_local_rid();
  if (local_rid == 0) return nullptr;  // PE treats 0 as alloc fail

  // PE CreateNodeUnder when *(wtroot+0xC)!=0 (a6=parent); else CreateNode →
  // CreateNodeByDesc a6=0 → ResolveParent(wt_key, *desc=1, 0) @ 0x537000.
  void* parent = *reinterpret_cast<void* const*>(wt + 0xC);

  // PE case1 @ 0x536A38: InstanceNode_PoolAlloc + InstanceNode_ctor.
  HostLoadGameInitGi* gi = nullptr;
  const char* script_fqn = nullptr;
  void* type_mid = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    (void)host_eng_sim_object_root();  // seed eng+0x11C stand-in
    if (!parent) {
      // PE CreateNodeByDesc @ 0x536ad1: ResolveParent(a2=wt_key, a3=1, a4=0).
      parent = host_resolve_parent(wt_key, /*type=*/1, /*flags=*/0);
    }
    gi = host_instance_node_pool_alloc();
    if (!gi) return nullptr;
    host_node_ensure_child_list_hdr(reinterpret_cast<uint8_t*>(gi));
    g_loadgameinit_gis.push_back(gi);

    gi->type = 1;
    gi->rebind_key = static_cast<int32_t>(local_rid);
    gi->flags = 33;  // create desc[2] → Node_initFromCreateDesc @ +0x54
    gi->rh_list_head = nullptr;

    // Alias: PE Engine_malloc(32)+Util_strncpy_n(31) → node+0x60; else
    // "_gameinst".
    {
      const char* src = (alias && alias[0]) ? alias : "_gameinst";
      char* buf = new char[32]{};
      std::strncpy(buf, src, 31);
      buf[31] = '\0';
      gi->alias = buf;
    }

    // TREE register (CreateNodeByDesc @ 0x536DFF) when parent known.
    if (parent) {
      host_create_node_by_desc_tree_register(gi, parent);
      host_lgi_parent_lod_copies(gi, parent);
    } else {
      // No ResolveParent hit — adopt under eng+0x11C so FindByRid can see RID.
      host_sim_object_root_adopt(gi);
    }

    host_rid_register(static_cast<int32_t>(local_rid), gi);

    // PE @ 0x53A8CE gate: *(GameType payload+0x10) Class* != 0.
    script_fqn = host_lgi_fqn_from_type_rh(type_rh);
    type_mid = host_lgi_mid_from_type_rh(type_rh);
  }

  ++g_lgi_create_count;           // PE ++g_InstanceNodeCreateCount @ 0x53A767
  gi->flags |= 0x200;             // PE @ 0x53A76D
  // PE @ 0x53A778: ResNode_setLodReadyBit(gi,1) then +0x68=1.0 @ 0x53A77D.
  host_lgi_set_lod_ready_bit(gi, 1);
  gi->lod_scale = 1.0f;
  // PE @ 0x53A78D: gi vtbl+0xC(1.0f) → ResNode_getEmbeddedMid → mid;
  //   Rebind mid+0x38 (same as CreateGameInstanceNative W19C @ 0x53A435).
  //   LoadGameInit Rebind still OOS here — void_event_loadgameinit_rebind
  //   owns the caller RH wire.

  // PE @ 0x53A8B2 ctor hop then @ 0x53A8FD THRD-CREATE.
  // W37: PE mov [GI_mid+0x4C],eax @ 0x53A8C9 — distinct from node+0x4C RESTYPE.
  if (void* nat =
          engine_load_game_init_run_gametype_ctor(gi, type_mid, params)) {
    *reinterpret_cast<void**>(gi->mid_embed + 0x4C) = nat;
  }
  (void)engine_load_game_init_attach_gametype(gi, script_fqn, gi->rebind_key);

  // W21D PE @ 0x53AA3A: ResNode_unionChildAabb(gi).
  // W22D PE @ 0x53AA40: while(ResNode_drainSpatial(gi)).
  host_res_node_union_child_aabb(gi);
  while (host_res_node_drain_spatial(gi) != 0) {
  }

  // Gaps: sub_537240 non-early; PE vtbl* dispatch; mid.leaf until seed;
  //   drain reparent/sibling; Type53_ensureHandleSlot@4B3EE0.
  return gi;
}

// ---------------------------------------------------------------------------
// W18A/W19C/W20A/W21D/W22D — PE Engine_CreateGameInstanceNative @ 0x0053A2A0
// size 0x332. cdecl (a1=parent_rh, a2=type_rh, a3=script, a4=params,
// a5=alias) → GI*|0.
//
// Call sites (IDA):
//   GameRef.create_native @ 0x47D975 — script=0, then ResHandle_Link(GI+0x44).
//   GameType.createNativeInstance @ 0x481B52 — script=this (mid+0x50).
//
// vs Engine_LoadGameInit @ 0x53A5E0:
//   Gate type_rh+8!=0 (LoadGameInit gates parent+8). Same create desc
//   {type=1, AllocLocalRid, flags=33} + CreateNodeUnder/CreateNode + parent
//   LOD + |0x200 + setLodReadyBit. Then Rebind mid+0x38←type owner;
//   PrepareLod(type,0x80000000)+GameTypeCtor(params); store a3 at mid+0x50.
//   NO Class_boxObject / THRD-CREATE / attach_gametype.
//
// Host: PoolAlloc/TREE/LOD + run_gametype_ctor; skip attach_gametype.
// W19C: ResHandle_Rebind(GI mid+0x38, *(type_rh+0xC)) @ 0x53A435 —
//   getEmbeddedMid ≡ GI+0xD8 (PE @ 0x53EFA0).
// W20A: mid+0x50=script(a3) @ 0x53A515 (create_native a3=0).
// W21D: ResNode_unionChildAabb @ 0x4988A0 / call @ 0x53A546.
// W22D: ResNode_drainSpatial @ 0x498B10 / while @ 0x53A54C (slice).
// Gaps: drain reparent/sibling; Type53_ensureHandleSlot@4B3EE0.
// ---------------------------------------------------------------------------
void* engine_create_game_instance_native(void* parent_rh, void* type_rh,
                                         void* script, const char* params,
                                         const char* alias) {
  // PE @ 0x53A2FA: *(type_rh+8)==0 → return null (before AllocLocalRid).
  if (!type_rh) return nullptr;
  const auto* trh = reinterpret_cast<const uint8_t*>(type_rh);
  if (*reinterpret_cast<const int32_t*>(trh + 8) == 0) return nullptr;

  // PE @ 0x53A2B9: type_rh+8!=0 → payload+0x4C==8 or log Wrong GameType!
  // (warn-only). Host: no Fatal.

  if (!parent_rh) return nullptr;
  const auto* wt = reinterpret_cast<const uint8_t*>(parent_rh);
  const int32_t wt_key = *reinterpret_cast<const int32_t*>(wt + 8);

  const uint32_t local_rid = host_alloc_local_rid();
  if (local_rid == 0) return nullptr;  // PE @ 0x53A30B

  void* parent = *reinterpret_cast<void* const*>(wt + 0xC);
  HostLoadGameInitGi* gi = nullptr;
  void* type_mid = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    (void)host_eng_sim_object_root();
    if (!parent && wt_key != 0) {
      // PE CreateNode when parent+0xC==0 → ResolveParent(wt_key, type=1, 0).
      parent = host_resolve_parent(wt_key, /*type=*/1, /*flags=*/0);
    }
    gi = host_instance_node_pool_alloc();
    if (!gi) return nullptr;
    host_node_ensure_child_list_hdr(reinterpret_cast<uint8_t*>(gi));
    g_loadgameinit_gis.push_back(gi);

    gi->type = 1;
    gi->rebind_key = static_cast<int32_t>(local_rid);
    gi->flags = 33;
    gi->rh_list_head = nullptr;

    {
      const char* src = (alias && alias[0]) ? alias : "_gameinst";
      char* buf = new char[32]{};
      std::strncpy(buf, src, 31);
      buf[31] = '\0';
      gi->alias = buf;
    }

    if (parent) {
      host_create_node_by_desc_tree_register(gi, parent);
      host_lgi_parent_lod_copies(gi, parent);
    } else {
      host_sim_object_root_adopt(gi);
    }

    host_rid_register(static_cast<int32_t>(local_rid), gi);
    type_mid = host_lgi_mid_from_type_rh(type_rh);
  }

  ++g_lgi_create_count;  // PE @ 0x53A3FA
  gi->flags |= 0x200;    // PE @ 0x53A40C
  host_lgi_set_lod_ready_bit(gi, 1);
  gi->lod_scale = 1.0f;

  // W19C PE @ 0x53A427..0x53A435: vtbl+0xC → getEmbeddedMid (GI+0xD8);
  // ResHandle_Rebind(mid+0x38, *(type_rh+0xC)). Owner = GameType node
  // (+0x48 list / +0x50 key) — same as voidEvent Rebind @ 0x429060.
  void* type_owner = *reinterpret_cast<void* const*>(trh + 0xC);
  res_handle_rebind(gi->mid_embed + 0x38, type_owner);

  // PE @ 0x53A4F8..0x53A50C: GameTypeCtor vtbl+0xC(ctor, tmpRH, params).
  // No THRD-CREATE (contrast LoadGameInit @ 0x53A8FD).
  // W37: store create result at mid+0x4C (PE @ 0x53A8C9 twin).
  if (void* nat =
          engine_load_game_init_run_gametype_ctor(gi, type_mid, params)) {
    *reinterpret_cast<void**>(gi->mid_embed + 0x4C) = nat;
  }

  // W20A PE @ 0x53A515: mov [ebx+50h], ecx — mid+0x50 = a3 (script).
  // create_native passes a3=0; createNativeInstance passes this.
  *reinterpret_cast<void**>(gi->mid_embed + 0x50) = script;

  // W21D PE @ 0x53A546: ResNode_unionChildAabb(gi) — AABB from type==2 kids.
  // W22D PE @ 0x53A54C: while(ResNode_drainSpatial(gi)); Type53 OOS.
  host_res_node_union_child_aabb(gi);
  while (host_res_node_drain_spatial(gi) != 0) {
  }

  // Gaps: drain reparent/sibling; Type53_ensureHandleSlot@4B3EE0.
  return gi;
}

// PE ResourceEngine_type_gametype @ 0x53A1A0 size 0xf8.
// cdecl (parent_rh, Class*, name_cstr) -> node*|0.
// createNativeInstance @ 0x481AD6: type_gametype(&dword_62F260, class,
//   Class_getNameCstr) -> store [node+0x50] at class+0x1D4.
// Create desc: type=8 RESTYPE_GAME, flags=2, alias name|"_gametype".
// @ 0x53A27D: *(vtbl+0xC(node,1.0f)+0x10) = Class* — host mid.script_class.
void* resource_engine_type_gametype(void* parent_rh, void* clazz,
                                    const char* name) {
  (void)parent_rh;  // CreateNodeUnder / CreateNode parent splice OOS
  // Class shell alloc before g_mu (tree_host_new must not nest under lock).
  void* seed = clazz;
  if (!seed && name && name[0]) seed = class_box_object(name);

  std::lock_guard<std::mutex> lock(g_mu);
  const uint32_t local_rid = host_alloc_local_rid();
  if (local_rid == 0) return nullptr;

  HostResNode* node = host_node_new(/*owner=*/nullptr, 0);
  if (!node) return nullptr;
  node->type = 8;  // RESTYPE_GAME
  if (int32_t* fl = host_node_flags(node)) *fl = 2;  // create desc[2]
  *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(node) + 0x50) =
      static_cast<int32_t>(local_rid);

  // PE @ 0x53A27D: payload+0x10 = a2 Class*. Prefer Class*; else FQN cstr.
  host_mid_seed_script_class(node, seed, name);

  host_rid_register(static_cast<int32_t>(local_rid), node);
  return node;
}

}  // namespace inv
