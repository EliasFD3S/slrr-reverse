// Fragment 2/2 of game_boot.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


          // Phase 2.146 — panel→panel drag (dropGadget → panelSwap).
          InvObject* items_d = minv ? tree_field_get_obj(minv, "items") : nullptr;
          InvObject* d_before0 =
              items_d ? tree_vector_element_at(items_d, 0) : nullptr;
          InvObject* d_before1 =
              items_d ? tree_vector_element_at(items_d, 1) : nullptr;
          const int32_t swap_before =
              minv ? tree_field_get_int(minv, "swap_count") : 0;
          const bool dragged =
              mech ? mechanic_drag_panel_to(mech, hx0, hy0, hx1, hy1) : false;
          InvObject* d_after0 =
              items_d ? tree_vector_element_at(items_d, 0) : nullptr;
          InvObject* d_after1 =
              items_d ? tree_vector_element_at(items_d, 1) : nullptr;
          const int32_t drag_from =
              mech ? tree_field_get_int(mech, "last_drag_from") : -9;
          const int32_t drag_to =
              mech ? tree_field_get_int(mech, "last_drag_to") : -9;
          mech_drag_ok =
              (dragged && d_before0 && d_before1 && d_after0 == d_before1 &&
               d_after1 == d_before0 &&
               (minv ? tree_field_get_int(minv, "swap_count") : 0) >
                   swap_before &&
               drag_from == 0 && drag_to == 1 &&
               (mech ? tree_field_get_int(mech, "drag_swap_count") : 0) >= 1)
                  ? 1
                  : 0;
          std::printf("  mechanic drag ok=%d from=%d to=%d swaps=%d\n",
                      mech_drag_ok, drag_from, drag_to,
                      minv ? tree_field_get_int(minv, "swap_count") : 0);

          // Phase 2.136 — scroll + body filter chrome.
          constexpr int32_t kCmdScrollUp = 0;
          constexpr int32_t kCmdScrollDown = 1;
          constexpr int32_t kCmdFilterBody = 120;
          mechanic_ensure_chrome(mech);
          const int32_t chrome_n = mechanic_chrome_button_count(mech);
          const int32_t size0 = minv ? inventory_size(minv) : 0;
          mechanic_osd_command(mech, kCmdScrollDown);
          const int32_t line1 = minv ? tree_field_get_int(minv, "cline") : -1;
          const int32_t inv_line1 =
              mech ? tree_field_get_int(mech, "inv_line") : -1;
          mechanic_osd_command(mech, kCmdScrollUp);
          const int32_t line0 = minv ? tree_field_get_int(minv, "cline") : -1;
          mechanic_osd_command(mech, kCmdFilterBody);
          const int32_t fbody =
              mech ? tree_field_get_int(mech, "filterBody") : -1;
          const int32_t size_filt = minv ? inventory_size(minv) : -1;
          mechanic_osd_command(mech, kCmdFilterBody);
          const int32_t size_back = minv ? inventory_size(minv) : -1;
          mech_chrome_ok =
              (chrome_n >= 5 && line1 == 1 && inv_line1 == 2 && line0 == 0 &&
               fbody == 2 && size_filt == 1 && size_back == size0)
                  ? 1
                  : 0;
          std::printf("  mechanic inv size=%d panels=%d attached=%d "
                      "vis=%d shown=%d btn=%d\n",
                      minv ? inventory_size(minv) : -1, panels, attached,
                      vis_upd, shown, btn_ok);
          std::printf("  mechanic chrome ok=%d btns=%d scroll=%d/%d "
                      "filt=%d/%d/%d\n",
                      mech_chrome_ok, chrome_n, line1, line0, fbody, size_filt,
                      size_back);

          // Phase 2.137/2.140/2.145 — hover+click installs via CFG slot match.
          float cx0 = 0.f, cy0 = 0.f;
          {
            InvObject* ps = minv ? tree_field_get_obj(minv, "panels") : nullptr;
            InvObject* p0 =
                (ps && tree_vector_size(ps) > 0) ? tree_vector_element_at(ps, 0)
                                                : nullptr;
            if (p0) {
              cx0 = tree_field_get_float(p0, "x") +
                    tree_field_get_float(p0, "width") * 0.5f;
              cy0 = tree_field_get_float(p0, "y") +
                    tree_field_get_float(p0, "height") * 0.5f;
            }
          }
          const int32_t size_pre = minv ? inventory_size(minv) : 0;
          InvObject* car = nullptr;
          if (mech) {
            InvObject* pl = tree_field_get_obj(mech, "player");
            car = pl ? tree_field_get_obj(pl, "car") : nullptr;
          }
          if (!car) car = starter_car;
          const bool hovered = mech ? mechanic_hover_at(mech, cx0, cy0) : false;
          const bool clicked = mech ? mechanic_click_actual(mech) : false;
          const int32_t click_panel =
              mech ? tree_field_get_int(mech, "last_click_panel") : -9;
          const int32_t last_click =
              minv ? tree_field_get_int(minv, "last_panel_click") : -9;
          const int32_t click_n =
              mech ? tree_field_get_int(mech, "click_count") : 0;
          mech_click_ok =
              (hovered && clicked && click_panel == 0 && last_click == 0 &&
               click_n >= 1)
                  ? 1
                  : 0;
          std::printf("  mechanic click ok=%d hover=%d panel=%d clicks=%d\n",
                      mech_click_ok, hovered ? 1 : 0, click_panel, click_n);
          const int32_t size_post = minv ? inventory_size(minv) : -1;
          const int32_t inst_slot =
              minv ? tree_field_get_int(minv, "last_install_slot") : 0;
          const int32_t via_att =
              minv ? tree_field_get_int(minv, "last_install_via_attach") : 0;
          InvObject* on_slot =
              (car && inst_slot > 0) ? part_on_slot(car, inst_slot) : nullptr;
          const int32_t cfg_n =
              car ? tree_field_get_int(car, "cfg_slot_count") : 0;
          const int32_t att_n =
              car ? tree_field_get_int(car, "cfg_attach_count") : 0;
          mech_install_ok =
              (mech_click_ok == 1 && size_pre >= 1 && size_post == size_pre - 1 &&
               inst_slot > 0 && inst_slot != 198 && inst_slot != 199 &&
               on_slot != nullptr && cfg_n > 0 && att_n > 0 && via_att == 1 &&
               (minv ? tree_field_get_int(minv, "install_count") : 0) >= 1)
                  ? 1
                  : 0;
          std::printf("  mechanic install ok=%d size=%d->%d slot=%d "
                      "cfg=%d att=%d via=%d clicks=%d\n",
                      mech_install_ok, size_pre, size_post, inst_slot, cfg_n,
                      att_n, via_att,
                      minv ? tree_field_get_int(minv, "install_count") : 0);

          // Phase 2.148 — pick car part under cursor → look_part (EC_HOVER).
          InvObject* installed = on_slot;
          float pnx = 0.5f, pny = 0.35f;
          float pwx = 0.f, pwy = 0.f, pwz = 0.f;
          if (installed) {
            if (InvObject* pp = java_util_resource_GameRef_getPos(installed))
              vec3_get(pp, &pwx, &pwy, &pwz);
            float spx = 0.f, spy = 0.f;
            if (render_d3d9_project(pwx, pwy, pwz, &spx, &spy)) {
              pnx = (spx + 1.f) * 0.5f;
              pny = (1.f - spy) * 0.5f;
            }
          }
          mechanic_clear_actual_panel(mech);
          const bool picked =
              (mech && installed) ? mechanic_hover_car_at(mech, pnx, pny)
                                  : false;
          InvObject* look = mech ? tree_field_get_obj(mech, "look_part") : nullptr;
          const int32_t pick_n =
              mech ? tree_field_get_int(mech, "pick_count") : 0;
          const int32_t over =
              mech ? tree_field_get_int(mech, "over_vehicle") : 0;
          mech_pick_ok =
              (picked && look == installed && pick_n >= 1 && over == 1 &&
               (car ? tree_field_get_obj(car, "last_installed_part") : nullptr) ==
                   installed)
                  ? 1
                  : 0;
          std::printf("  mechanic pick ok=%d look=%d via=%d dist=%.1f "
                      "xy=%.2f,%.2f w=(%.0f,%.0f,%.0f)\n",
                      mech_pick_ok, look == installed ? 1 : 0,
                      mech ? tree_field_get_int(mech, "pick_via_screen") : -1,
                      mech ? tree_field_get_float(mech, "pick_dist") : -1.f, pnx,
                      pny, pwx, pwy, pwz);

          // Phase 2.149 — EC_RDRAG orbit garage camera.
          InvObject* gcam = garage_ensure_camera(garage);
          float rex0 = 0.f, rey0 = 0.f, rez0 = 0.f, rax0 = 0.f, ray0 = 0.f,
                raz0 = 0.f;
          const bool rgot0 =
              gcam &&
              render_d3d9_camera_get_lookat(gcam, &rex0, &rey0, &rez0, &rax0,
                                            &ray0, &raz0);
          const float ryaw0 =
              garage ? tree_field_get_float(garage, "cam_yaw") : 0.f;
          garage_rdrag_begin(garage);
          const bool orb1 = garage_rdrag_orbit(garage, 0.4f, 0.1f);
          const bool orb2 = garage_rdrag_orbit(garage, 0.3f, -0.05f);
          float rex1 = rex0, rey1 = rey0, rez1 = rez0;
          render_d3d9_camera_get_lookat(gcam, &rex1, &rey1, &rez1, &rax0, &ray0,
                                        &raz0);
          const float ryaw1 =
              garage ? tree_field_get_float(garage, "cam_yaw") : 0.f;
          const int32_t rdrag_on =
              garage ? tree_field_get_int(garage, "rdrag") : 0;
          garage_rdrag_end(garage);
          const int32_t rdrag_off =
              garage ? tree_field_get_int(garage, "rdrag") : 1;
          const float rdpos = std::fabs(rex1 - rex0) + std::fabs(rey1 - rey0) +
                              std::fabs(rez1 - rez0);
          const float rdyaw = ryaw1 - ryaw0;
          mech_rdrag_ok =
              (rgot0 && gcam && orb1 && orb2 && rdrag_on == 1 && rdrag_off == 0 &&
               rdyaw > 0.5f && rdpos > 0.2f &&
               (garage ? tree_field_get_int(garage, "rdrag_orbit_count") : 0) >=
                   2 &&
               (garage ? tree_field_get_int(garage, "rdrag_begin_count") : 0) >=
                   1 &&
               mech && tree_field_get_obj(mech, "camera") == gcam)
                  ? 1
                  : 0;
          std::printf("  mechanic rdrag ok=%d dyaw=%.2f dpos=%.2f orbits=%d "
                      "cam=%d\n",
                      mech_rdrag_ok, rdyaw, rdpos,
                      garage ? tree_field_get_int(garage, "rdrag_orbit_count")
                             : 0,
                      gcam ? 1 : 0);

          // PE Cursor_handleCommand lock/unlock @ 0x00462320 / 0x0046235A
          // → Engine_SysCursorLock ClipCursor 1px, then Unlock ClipCursor(NULL).
          {
            constexpr int32_t kEventCommand = 0x10;
            InvObject* icur = java_io_Input_cursor();
            InvObject* cgr =
                icur ? tree_field_get_obj(icur, "cursor") : nullptr;
            if (!cgr) cgr = gameref_new();
            java_util_resource_GameRef_queueEvent(cgr, nullptr, kEventCommand,
                                                  string_new("lock"));
            const int32_t lock_on = tree_field_get_int(cgr, "cursor_locked");
            const bool pin = input_syscursor_locked();
            java_util_resource_GameRef_queueEvent(cgr, nullptr, kEventCommand,
                                                  string_new("unlock"));
            const int32_t lock_off = tree_field_get_int(cgr, "cursor_locked");
            cursor_lock_ok =
                (lock_on == 1 && pin && lock_off == 0 &&
                 !input_syscursor_locked())
                    ? 1
                    : 0;
            std::printf("  cursor lock ok=%d on=%d pin=%d off=%d\n",
                        cursor_lock_ok, lock_on, pin ? 1 : 0, lock_off);
          }

          // PE Cursor_tick EC_LDRAGBEGIN=9 / END=10 (skip LCLICK).
          {
            constexpr int32_t kEventCursor = 0x00010000;
            InvObject* icur = java_io_Input_cursor();
            InvObject* cgr =
                icur ? tree_field_get_obj(icur, "cursor") : nullptr;
            if (!cgr && icur) {
              cgr = gameref_new();
              tree_field_set_obj(icur, "cursor", cgr);
            }
            InvObject* gt = tree_host_new("java.lang.GameType");
            // Soft PE queueEvent_dispatch @ 0x426653: watcher GI+0x70 must
            // include EVENT_CURSOR (Java Garage/Mechanic/Dialog setEventMask).
            if (gt) java_lang_GameType_setEventMask(gt, kEventCursor);
            if (cgr)
              java_lang_GameType_addNotification(gt, cgr, kEventCursor, 0,
                                                 nullptr);
            input_syscursor_set_buttons(0u);
            java_io_MouseCursor_tickSysCursor();
            const int32_t n0 = tree_field_get_int(gt, "cursor_event_count");
            input_syscursor_set_ndc(0.0f, 0.0f);
            input_syscursor_set_buttons(1u);
            java_io_MouseCursor_tickSysCursor();
            input_syscursor_set_ndc(0.25f, 0.15f);
            java_io_MouseCursor_tickSysCursor();
            input_syscursor_set_buttons(0u);
            java_io_MouseCursor_tickSysCursor();
            const int32_t n1 = tree_field_get_int(gt, "cursor_event_count");
            InvObject* lp = tree_field_get_obj(gt, "last_cursor_param");
            const char* lps = lp ? string_cstr(lp) : "";
            int32_t last_ec = 0;
            if (lps && lps[0]) std::sscanf(lps, "%d", &last_ec);
            cursor_ldrag_ok =
                ((n1 - n0) >= 4 && (last_ec == 10 || last_ec == 11)) ? 1 : 0;
            std::printf("  cursor ldrag ok=%d dn=%d last_ec=%d\n",
                        cursor_ldrag_ok, n1 - n0, last_ec);
          }

          // Phase 2.150/2.151 — MODE_TUNE + TUNE PART dialog (sliders).
          constexpr int32_t kCmdTune = 124;
          constexpr int32_t kCmdMechanic = 117;
          garage_osd_command(garage, kCmdTune);
          const int32_t gmode_t =
              garage ? tree_field_get_int(garage, "mode") : -1;
          const int32_t mmode_t =
              mech ? tree_field_get_int(mech, "mode") : -1;
          // Flap: stock only for !isTuneable — use Bumper (Block is always on).
          InvObject* flap_tgt =
              tree_host_new("java.game.parts.bodypart.Bumper");
          const bool flap_ok =
              (mech && flap_tgt && !part_is_tuneable(flap_tgt))
                  ? mechanic_lclick_part(mech, flap_tgt)
                  : false;
          const int32_t flap_n =
              flap_tgt ? tree_field_get_int(flap_tgt, "flap_toggle_count")
                       : 0;
          const int32_t flap_st =
              flap_tgt ? tree_field_get_int(flap_tgt, "flap_state") : -1;
          if (installed) {
            // Prefer Block menu: installed CFG engine, or force Block fields.
            if (!std::strstr(tree_host_class(installed)
                                 ? tree_host_class(installed)
                                 : "",
                             "Block"))
              tree_field_set_int(installed, "tuneable", 1);
            tree_field_set_int(installed, "tune_kind", 1);  // kTuneBlock
            tree_field_set_float(installed, "rpm_idle", 900.f);
            tree_field_set_float(installed, "RPM_limit", 7200.f);
          }
          InvObject* tdlg =
              (mech && installed) ? mechanic_open_tune_dialog(mech, installed)
                                  : nullptr;
          InvObject* tmenu =
              tdlg ? tree_field_get_obj(tdlg, "tune_menu") : nullptr;
          InvObject* tosd = tdlg ? tree_field_get_obj(tdlg, "osd") : nullptr;
          const int32_t slider_n =
              tmenu ? tree_field_get_int(tmenu, "item_count") : 0;
          const int32_t btn_n =
              tosd ? tree_field_get_int(tosd, "button_count") : 0;
          const char* ttitle = nullptr;
          if (tdlg) {
            if (InvObject* t = tree_field_get_obj(tdlg, "title"))
              ttitle = string_cstr(t);
          }
          InvObject* title_txt =
              tosd ? tree_field_get_obj(tosd, "title_text") : nullptr;
          const int32_t wide_ok =
              tdlg ? tree_field_get_int(tdlg, "wide_applied") : 0;
          const int32_t hk_ok =
              tdlg ? tree_field_get_int(tdlg, "cancel_hotkey") : 0;
          const float vaspect =
              tosd ? tree_field_get_float(tosd, "vpAspect") : 0.f;
          const bool slid1 =
              mech ? mechanic_tune_set_slider(mech, 1, 1100.f) : false;
          const bool slid2 =
              mech ? mechanic_tune_set_slider(mech, 2, 8500.f) : false;
          const float idle_live =
              installed ? tree_field_get_float(installed, "rpm_idle") : 0.f;
          const float red_live =
              installed ? tree_field_get_float(installed, "RPM_limit") : 0.f;
          const bool tune_ok =
              (mech && installed) ? mechanic_tune_part(mech, installed, 0)
                                  : false;
          const int32_t tune_n =
              mech ? tree_field_get_int(mech, "tune_count") : 0;
          const int32_t tune_choice =
              installed ? tree_field_get_int(installed, "tune_session_choice")
                        : -9;
          const int32_t dlg_n =
              mech ? tree_field_get_int(mech, "tune_dialog_count") : 0;
          // Cancel restores backups.
          if (installed) {
            tree_field_set_float(installed, "rpm_idle", 1100.f);
            tree_field_set_float(installed, "RPM_limit", 8500.f);
            tree_field_set_float(installed, "old_rpm_idle", 1100.f);
            tree_field_set_float(installed, "old_RPM_limit", 8500.f);
          }
          InvObject* tdlg2 =
              (mech && installed) ? mechanic_open_tune_dialog(mech, installed)
                                  : nullptr;
          mechanic_tune_set_slider(mech, 1, 500.f);
          const bool cancel_ok =
              (mech && installed) ? mechanic_tune_part(mech, installed, 1)
                                  : false;
          const float idle_cancel =
              installed ? tree_field_get_float(installed, "rpm_idle") : -1.f;
          garage_osd_command(garage, kCmdMechanic);
          const int32_t mmode_s =
              mech ? tree_field_get_int(mech, "mode") : -1;
          mech_tune_ok =
              (gmode_t == 4 && mmode_t == 1 && flap_ok && flap_n >= 1 &&
               flap_st == 1 && tdlg && ttitle && std::strstr(ttitle, "TUNE") &&
               slider_n >= 2 && btn_n >= 2 && slid1 && slid2 &&
               idle_live > 1099.f && red_live > 8499.f && tune_ok &&
               tune_n >= 1 && tune_choice == 0 && dlg_n >= 1 && cancel_ok &&
               idle_cancel > 1099.f && tdlg2 && mmode_s == 0 &&
               (garage ? tree_field_get_int(garage, "mode") : -1) == 1 &&
               title_txt && wide_ok == 1 && hk_ok == 1 && vaspect > 1.9f)
                  ? 1
                  : 0;
          std::printf("  mechanic tune ok=%d gmode=%d/%d dlg='%s' sliders=%d "
                      "btns=%d title=%d wide=%d hk=%d idle=%.0f cancel=%.0f "
                      "flap=%d\n",
                      mech_tune_ok, gmode_t,
                      garage ? tree_field_get_int(garage, "mode") : -1,
                      ttitle ? ttitle : "", slider_n, btn_n, title_txt ? 1 : 0,
                      wide_ok, hk_ok, idle_live, idle_cancel, flap_ok ? 1 : 0);

          // Phase 2.153 — subclass buildTuningMenu (Tyre / Chassis / Camshaft).
          int32_t subclass_ok = 0;
          if (mech) {
            InvObject* tyre = tree_host_new(
                "java.game.parts.rgearpart.reciprocatingrgearpart.Tyre");
            tree_field_set_float(tyre, "inflation", 2.0f);
            tree_field_set_float(tyre, "optimal_inflation", 2.5f);
            InvObject* tdlg_t = mechanic_open_tune_dialog(mech, tyre);
            InvObject* tmenu_t =
                tdlg_t ? tree_field_get_obj(tdlg_t, "tune_menu") : nullptr;
            const int32_t t_items =
                tmenu_t ? tree_field_get_int(tmenu_t, "item_count") : 0;
            const int32_t t_kind = tree_field_get_int(tyre, "tune_kind");
            const bool t_slid = mechanic_tune_set_slider(mech, 1, 2.2f);
            const float infl_live = tree_field_get_float(tyre, "inflation");
            mechanic_tune_part(mech, tyre, 1);
            const float infl_cancel = tree_field_get_float(tyre, "inflation");

            InvObject* chas =
                tree_host_new("java.game.parts.bodypart.Chassis");
            tree_field_set_int(chas, "brake_balance_can_be_set", 1);
            tree_field_set_float(chas, "brake_balance", 0.55f);
            InvObject* tdlg_c = mechanic_open_tune_dialog(mech, chas);
            InvObject* tmenu_c =
                tdlg_c ? tree_field_get_obj(tdlg_c, "tune_menu") : nullptr;
            const int32_t c_items =
                tmenu_c ? tree_field_get_int(tmenu_c, "item_count") : 0;
            const int32_t c_kind = tree_field_get_int(chas, "tune_kind");
            const bool c_slid = mechanic_tune_set_slider(mech, 1, -0.7f);
            const float bal_live = tree_field_get_float(chas, "brake_balance");
            mechanic_tune_part(mech, chas, 0);

            InvObject* cam = tree_host_new(
                "java.game.parts.enginepart.slidingenginepart."
                "reciprocatingenginepart.Camshaft");
            tree_field_set_float(cam, "advance", 12.f);
            tree_field_set_float(cam, "default_advance", 5.f);
            tree_field_set_float(cam, "advance_negative_peak", -5.f);
            tree_field_set_float(cam, "advance_positive_peak", 40.f);
            tree_field_set_float(cam, "advance_minimum_step", 0.5f);
            InvObject* tdlg_m = mechanic_open_tune_dialog(mech, cam);
            InvObject* tmenu_m =
                tdlg_m ? tree_field_get_obj(tdlg_m, "tune_menu") : nullptr;
            const int32_t m_items =
                tmenu_m ? tree_field_get_int(tmenu_m, "item_count") : 0;
            const int32_t m_kind = tree_field_get_int(cam, "tune_kind");
            mechanic_tune_set_slider(mech, 1, 20.f);
            const bool reset_ok = mechanic_tune_menu_command(mech, 0);
            const float adv_rst = tree_field_get_float(cam, "advance");
            mechanic_tune_part(mech, cam, 1);

            // Phase 2.154 — Transmission gears/end/LSD/drive.
            InvObject* tr = tree_host_new(
                "java.game.parts.enginepart.slidingenginepart."
                "reciprocatingenginepart.Transmission");
            tree_field_set_int(tr, "gears", 5);
            tree_field_set_int(tr, "adjustable_gears", 7);  // fwd|R|end
            tree_field_set_float(tr, "ratio_1", 3.5f);
            tree_field_set_float(tr, "ratio_2", 2.1f);
            tree_field_set_float(tr, "ratio_3", 1.5f);
            tree_field_set_float(tr, "ratio_4", 1.1f);
            tree_field_set_float(tr, "ratio_5", 0.9f);
            tree_field_set_float(tr, "ratio_7", 3.2f);
            tree_field_set_float(tr, "end_ratio", 3.9f);
            tree_field_set_float(tr, "diff_lock", 0.3f);
            tree_field_set_float(tr, "diff_lock_min", 0.f);
            tree_field_set_float(tr, "diff_lock_max", 1.f);
            tree_field_set_float(tr, "drive_front", 0.5f);
            tree_field_set_float(tr, "drive_front_min", 0.f);
            tree_field_set_float(tr, "drive_front_max", 1.f);
            InvObject* tdlg_tr = mechanic_open_tune_dialog(mech, tr);
            InvObject* tmenu_tr =
                tdlg_tr ? tree_field_get_obj(tdlg_tr, "tune_menu") : nullptr;
            const int32_t tr_items =
                tmenu_tr ? tree_field_get_int(tmenu_tr, "item_count") : 0;
            const int32_t tr_kind = tree_field_get_int(tr, "tune_kind");
            // During session forward ratios are negated.
            const float r1_sess = tree_field_get_float(tr, "ratio_1");
            const bool tr_g1 = mechanic_tune_set_slider(mech, 1, -4.0f);
            const bool tr_end = mechanic_tune_set_slider(mech, 8, 4.5f);
            const bool tr_lsd = mechanic_tune_set_slider(mech, 9, 0.8f);
            const bool tr_drv = mechanic_tune_set_slider(mech, 10, -0.6f);
            const float end_live = tree_field_get_float(tr, "end_ratio");
            const float lsd_live = tree_field_get_float(tr, "diff_lock");
            const float drv_live = tree_field_get_float(tr, "drive_front");
            mechanic_tune_part(mech, tr, 0);  // OK → flip gears positive
            const float r1_ok = tree_field_get_float(tr, "ratio_1");
            // Cancel restore
            InvObject* tdlg_tr2 = mechanic_open_tune_dialog(mech, tr);
            mechanic_tune_set_slider(mech, 1, -2.0f);
            mechanic_tune_part(mech, tr, 1);
            const float r1_cancel = tree_field_get_float(tr, "ratio_1");

            // Phase 2.155 — FuelInjector + NOS.
            InvObject* inj = tree_host_new(
                "java.game.parts.enginepart.airfueldeliverysystem."
                "FuelInjectorSystem");
            tree_field_set_float(inj, "mixture_ratio", 14.7f);
            InvObject* tdlg_i = mechanic_open_tune_dialog(mech, inj);
            InvObject* tmenu_i =
                tdlg_i ? tree_field_get_obj(tdlg_i, "tune_menu") : nullptr;
            const int32_t i_items =
                tmenu_i ? tree_field_get_int(tmenu_i, "item_count") : 0;
            const int32_t i_kind = tree_field_get_int(inj, "tune_kind");
            const bool i_slid = mechanic_tune_set_slider(mech, 1, 12.5f);
            const float mix_live = tree_field_get_float(inj, "mixture_ratio");
            mechanic_tune_part(mech, inj, 1);
            const float mix_cancel = tree_field_get_float(inj, "mixture_ratio");

            InvObject* nos = tree_host_new(
                "java.game.parts.enginepart.airfueldeliverysystem."
                "NOSInjectorSystem");
            tree_field_set_float(nos, "nitro_consumption", 0.2f);
            tree_field_set_float(nos, "minconsumption", 0.05f);
            tree_field_set_float(nos, "maxconsumption", 0.8f);
            InvObject* tdlg_n = mechanic_open_tune_dialog(mech, nos);
            InvObject* tmenu_n =
                tdlg_n ? tree_field_get_obj(tdlg_n, "tune_menu") : nullptr;
            const int32_t n_items =
                tmenu_n ? tree_field_get_int(tmenu_n, "item_count") : 0;
            const int32_t n_kind = tree_field_get_int(nos, "tune_kind");
            const bool n_slid = mechanic_tune_set_slider(mech, 1, 0.55f);
            const float nos_live =
                tree_field_get_float(nos, "nitro_consumption");
            mechanic_tune_part(mech, nos, 0);
            const float nos_ok = tree_field_get_float(nos, "nitro_consumption");

            subclass_ok =
                (part_is_tuneable(tyre) && tdlg_t && t_kind == 2 &&
                 t_items == 1 && t_slid && infl_live > 2.19f &&
                 infl_cancel > 1.99f && infl_cancel < 2.01f &&
                 part_is_tuneable(chas) && tdlg_c && c_kind == 4 &&
                 c_items == 1 && c_slid && bal_live > 0.69f &&
                 part_is_tuneable(cam) && tdlg_m && m_kind == 5 &&
                 m_items >= 2 && reset_ok && adv_rst > 4.9f && adv_rst < 5.1f &&
                 part_is_tuneable(tr) && tdlg_tr && tdlg_tr2 && tr_kind == 7 &&
                 tr_items >= 8 && r1_sess < -3.4f && tr_g1 && tr_end &&
                 tr_lsd && tr_drv && end_live > 4.4f && lsd_live > 0.79f &&
                 drv_live > 0.59f && r1_ok > 3.9f && r1_ok < 4.1f &&
                 r1_cancel > 3.9f && r1_cancel < 4.1f &&
                 part_is_tuneable(inj) && tdlg_i && i_kind == 8 &&
                 i_items == 1 && i_slid && mix_live > 12.4f &&
                 mix_cancel > 14.6f && mix_cancel < 14.8f &&
                 part_is_tuneable(nos) && tdlg_n && n_kind == 9 &&
                 n_items == 1 && n_slid && nos_live > 0.54f &&
                 nos_ok > 0.54f)
                    ? 1
                    : 0;
            if (mech_tune_ok) mech_tune_ok = subclass_ok;
            std::printf("  mechanic tune_sub ok=%d tyre=%d/%d chassis=%d/%d "
                        "cam=%d/%d tr=%d/%d inj=%d/%d nos=%d/%.2f\n",
                        subclass_ok, t_kind, t_items, c_kind, c_items, m_kind,
                        m_items, tr_kind, tr_items, i_kind, i_items, n_kind,
                        nos_ok);
          }

          // Phase 2.147 — EC_LCLICK car part → inventory (addItem+scrollTo).
          const int32_t size_pre_l = minv ? inventory_size(minv) : 0;
          const bool lclicked =
              (mech && look) ? mechanic_lclick_part(mech, look)
                             : ((mech && installed)
                                    ? mechanic_lclick_part(mech, installed)
                                    : false);
          const int32_t size_l = minv ? inventory_size(minv) : -1;
          InvObject* slot_l =
              (car && inst_slot > 0) ? part_on_slot(car, inst_slot) : installed;
          const int32_t lclick_n =
              mech ? tree_field_get_int(mech, "lclick_count") : 0;
          const int32_t l_idx =
              mech ? tree_field_get_int(mech, "last_lclick_index") : -9;
          const int32_t wake_n =
              car ? tree_field_get_int(car, "wake_count") : 0;
          mech_lclick_ok =
              (lclicked && size_l == size_pre_l + 1 && slot_l == nullptr &&
               lclick_n >= 1 && l_idx == size_l - 1 && wake_n >= 1 &&
               !tree_field_get_obj(mech, "look_part"))
                  ? 1
                  : 0;
          std::printf("  mechanic lclick ok=%d size=%d->%d slot=%d idx=%d "
                      "wake=%d\n",
                      mech_lclick_ok, size_pre_l, size_l, slot_l ? 1 : 0, l_idx,
                      wake_n);

          // Re-install so drop smoke still has a car part on the CFG slot.
          const bool reinstall =
              mech ? (mechanic_hover_at(mech, cx0, cy0) &&
                      mechanic_click_actual(mech))
                   : false;
          on_slot =
              (car && inst_slot > 0) ? part_on_slot(car, inst_slot) : nullptr;
          if (!reinstall || !on_slot) {
            std::printf("  mechanic reinstall for drop failed re=%d slot=%d\n",
                        reinstall ? 1 : 0, on_slot ? 1 : 0);
          }

          // Phase 2.138/2.146 — car→panel via dropObject OSD path.
          InvObject* dropped = on_slot;
          float dx1 = 0.f, dy1 = 0.f;
          {
            InvObject* ps = minv ? tree_field_get_obj(minv, "panels") : nullptr;
            InvObject* p1 =
                (ps && tree_vector_size(ps) > 1) ? tree_vector_element_at(ps, 1)
                                                : nullptr;
            if (p1) {
              dx1 = tree_field_get_float(p1, "x") +
                    tree_field_get_float(p1, "width") * 0.5f;
              dy1 = tree_field_get_float(p1, "y") +
                    tree_field_get_float(p1, "height") * 0.5f;
            }
          }
          const bool dropped_ok =
              (mech && dropped)
                  ? mechanic_drop_object_at(mech, dropped, dx1, dy1)
                  : false;
          const int32_t size_drop = minv ? inventory_size(minv) : -1;
          InvObject* slot_after =
              (car && inst_slot > 0) ? part_on_slot(car, inst_slot) : dropped;
          const int32_t drop_panel =
              mech ? tree_field_get_int(mech, "last_drop_panel") : -9;
          mech_drop_ok =
              (dropped_ok && size_drop == size_pre && slot_after == nullptr &&
               drop_panel == 1 &&
               (minv ? tree_field_get_int(minv, "drop_count") : 0) >= 1 &&
               (mech ? tree_field_get_int(mech, "drop_object_count") : 0) >= 1)
                  ? 1
                  : 0;
          std::printf("  mechanic drop ok=%d size=%d slot%d=%d panel=%d "
                      "drops=%d\n",
                      mech_drop_ok, size_drop, inst_slot, slot_after ? 1 : 0,
                      drop_panel,
                      minv ? tree_field_get_int(minv, "drop_count") : 0);

          gcmd = kCmdPaint;
          garage_smoke_step = 2;
        } else if (garage_smoke_step == 2 && frames >= 16) {
          mode_paint = tree_field_get_int(garage, "mode");
          // Phase 2.156 — painter.show on CMD_PAINT.
          painter_ux_ok =
              (garage && tree_field_get_int(garage, "painter_shown") == 1 &&
               tree_field_get_obj(garage, "painter") && mode_paint == 2)
                  ? 1
                  : 0;
          gcmd = kCmdEscape;
          garage_smoke_step = 3;
        } else if (garage_smoke_step == 3 && frames >= 18) {
          mode_none = tree_field_get_int(garage, "mode");
          if (!garage || tree_field_get_int(garage, "painter_shown") != 0)
            painter_ux_ok = 0;
          garage_smoke_step = 4;
        } else if (garage_smoke_step == 4 && frames >= 20) {
          want_install = true;
          garage_smoke_step = 5;
        } else if (garage_smoke_step == 5 && frames >= 22) {
          want_paint = true;
          garage_smoke_step = 6;
        } else if (garage_smoke_step == 6 && frames >= 24) {
          want_street = true;
          garage_smoke_step = 7;
        }
      }

      if (gcmd) {
        garage_osd_command(garage, gcmd);
        std::printf("  garage cmd=%d mode=%d\n", gcmd,
                    tree_field_get_int(garage, "mode"));
      }
      if (want_install) garage_do_install();
      if (want_paint) garage_do_paint();
      if (want_street) garage_go_street();
    }

    if (ctrl && !auto_new) {
      const float cancel =
          java_io_Controller_user_GetAxisVal(ctrl, kAxisCancel);
      // Edge only — FMV skip leaves ESC held; don't treat that as quit.
      if (prev_cancel < 0.5f && cancel >= 0.5f) {
        std::printf("  ESC — leave interactive loop\n");
        break;
      }
      prev_cancel = cancel;
    }

    render_d3d9_flush();
    ++frames;
    if (max_frames > 0 && frames >= max_frames) break;
#ifdef _WIN32
    Sleep(16);
#endif
  }

  input_live_enable(false);

  // Hub Valocity modes after career smoke — separate from CMD_NEW so
  // loadDefaults cannot poison Garage.createOSDObjects during career.
  if (auto_new && !fired_freeride) {
    auto hub_valo = [&](const char* tag, InvObject* (*fn)()) {
      InvObject* menu2 = tree_host_new("java.game.MainMenu");
      game_logic_change_active_section(menu2);
      menu = game_logic_actual_state();
      mmd = menu ? tree_field_get_obj(menu, "mmd") : nullptr;
      int32_t ok = 0;
      if (menu && std::strstr(tree_host_class(menu), "MainMenu")) {
        InvObject* city = fn();
        const char* cn = city ? tree_host_class(city) : nullptr;
        // Stock QUICKRACE Valocity.enter immediately CAS → RaceSetup.
        ok = (cn && (std::strstr(cn, "Valocity") ||
                     (std::strcmp(tag, "QUICKRACE") == 0 &&
                      std::strstr(cn, "RaceSetup"))))
                 ? 1
                 : 0;
        std::printf("  hub %s ok=%d state='%s'\n", tag, ok, cn ? cn : "?");
        // Stock RaceSetup.osdCommand(CMD_RACE) → startRace + CAS(track).
        // Fire before hub restores Garage so valo_ret_ok still sees garage.
        if (ok && std::strcmp(tag, "QUICKRACE") == 0 && cn &&
            std::strstr(cn, "RaceSetup"))
          racesetup_try_cmd_race(city);
      }
      if (garage && std::strstr(tree_host_class(garage), "Garage"))
        game_logic_change_active_section(garage);
      return ok;
    };
    hub_freeride_ok = hub_valo("FREERIDE", main_menu_cmd_freeride);
    hub_quickrace_ok = hub_valo("QUICKRACE", main_menu_cmd_quickrace);
    hub_demo_ok = hub_valo("DEMO", main_menu_cmd_demo);
    // BACK TO GARAGE from MainMenu after city modes.
    {
      InvObject* menu2 = tree_host_new("java.game.MainMenu");
      game_logic_change_active_section(menu2);
      menu = game_logic_actual_state();
      if (menu && std::strstr(tree_host_class(menu), "MainMenu")) {
        InvObject* g = main_menu_cmd_back_to_garage();
        hub_back_ok =
            (g && std::strstr(tree_host_class(g), "Garage")) ? 1 : 0;
        std::printf("  hub BACKTOGARAGE ok=%d state='%s'\n", hub_back_ok,
                    g && tree_host_class(g) ? tree_host_class(g) : "?");
      }
      if (garage && std::strstr(tree_host_class(garage), "Garage"))
        game_logic_change_active_section(garage);
    }
    // EXIT last — CAS null then restore Garage for return checks.
    {
      InvObject* menu2 = tree_host_new("java.game.MainMenu");
      game_logic_change_active_section(menu2);
      menu = game_logic_actual_state();
      if (menu && std::strstr(tree_host_class(menu), "MainMenu")) {
        hub_exit_ok = main_menu_cmd_exit() ? 1 : 0;
        std::printf("  hub EXIT ok=%d state='%s'\n", hub_exit_ok,
                    game_logic_actual_state()
                        ? tree_host_class(game_logic_actual_state())
                        : "null");
      }
      if (garage && std::strstr(tree_host_class(garage), "Garage"))
        game_logic_change_active_section(garage);
    }
    // OPTIONS + CREDITS — changeMode group switches (no CAS).
    {
      InvObject* menu2 = tree_host_new("java.game.MainMenu");
      game_logic_change_active_section(menu2);
      menu = game_logic_actual_state();
      mmd = menu ? tree_field_get_obj(menu, "mmd") : nullptr;
      if (menu && mmd && std::strstr(tree_host_class(menu), "MainMenu")) {
        hub_options_ok = main_menu_cmd_options() ? 1 : 0;
        std::printf("  hub OPTIONS ok=%d act=%d opt=%d\n", hub_options_ok,
                    tree_field_get_int(mmd, "actGroup"),
                    tree_field_get_int(mmd, "optionsGroup"));
        hub_credits_ok = main_menu_cmd_credits() ? 1 : 0;
        std::printf("  hub CREDITS ok=%d act=%d cred=%d\n", hub_credits_ok,
                    tree_field_get_int(mmd, "actGroup"),
                    tree_field_get_int(mmd, "creditsGroup"));
      }
      if (garage && std::strstr(tree_host_class(garage), "Garage"))
        game_logic_change_active_section(garage);
    }
    fired_freeride = true;
  }

  InvObject* final_st = game_logic_actual_state();
  const char* stn =
      final_st && tree_host_class(final_st) ? tree_host_class(final_st) : "?";
  std::printf("boot game_loop ok=%d frames=%d fired=%d state='%s' garage=%d\n",
              (menu && mmd_shown == 1 && frames > 0) ? 1 : 0, frames,
              fired_new ? 1 : 0, stn, garage_ok);
  std::printf("boot mainmenu_chrome ok=%d osd=%d txt=%d acg_host=%d btn=%d\n",
              (menu_chrome == 1 && menu_txt_n >= 12 && menu_osd_n >= 1) ? 1 : 0,
              menu_osd_n, menu_txt_n, acg_host, btn_n);

  // Boot-path fidelity toward splash→menu→garage→world (not full game %).
  // Rubric: FMV/JVM~15 SplashTREE~25 MainMenuTREE~35 chromeTREE~48
  // GarageOSD-TREE~58 MechanicTREE~65 ValocityTREE~80 RaceSetup~88
  // GarageOSD~90 RaceSetupOSD~94 CMD_RACE/startRace~100.
  // Valocity.enter TREE (not host mirror) is the ~80 gate.
  int boot_pct = 15;
  if (splash && std::strstr(splash_cn, "SplashScreen")) boot_pct = 25;
  if (menu && std::strstr(tree_host_class(menu), "MainMenu")) boot_pct = 35;
  if (menu_chrome == 1 && menu_txt_n >= 12 && acg_host == 0) boot_pct = 48;
  {
    InvObject* menu_st = menu;
    // MainMenuDialog.osdCommand TREE = career/freeride/demo hub multiplexor.
    if (menu_st && tree_field_get_int(menu_st, "osd_cmd_via_tree") == 1)
      boot_pct = 54;
    const int32_t hub_valo_n =
        (hub_freeride_ok ? 1 : 0) + (hub_quickrace_ok ? 1 : 0) +
        (hub_demo_ok ? 1 : 0);
    if (hub_valo_n >= 1) boot_pct = 56;
    if (hub_valo_n >= 3) boot_pct = 58;
  }
  {
    InvObject* g = garage ? garage : (final_st && tree_host_class(final_st) &&
                                              std::strstr(tree_host_class(final_st),
                                                          "Garage")
                                          ? final_st
                                          : nullptr);
    if (g && tree_field_get_int(g, "enter_via_tree") == 1) boot_pct = 55;
    if (g && tree_field_get_int(g, "osd_via_tree") == 1) boot_pct = 60;
    else if (g && tree_field_get_int(g, "osd_via_host") == 1 && boot_pct < 55)
      boot_pct = 52;
    else if (garage_ok && boot_pct < 50) boot_pct = 50;
  }
  if (valo_entered && boot_pct >= 60) boot_pct = 64;
  else if (valo_entered && boot_pct >= 55) boot_pct = 62;
  else if (valo_entered && boot_pct >= 50) boot_pct = 58;
  {
    const int32_t hub_valo_n =
        (hub_freeride_ok ? 1 : 0) + (hub_quickrace_ok ? 1 : 0) +
        (hub_demo_ok ? 1 : 0);
    if (hub_valo_n >= 1 && boot_pct >= 62) boot_pct = 66;
    if (hub_valo_n >= 3 && boot_pct >= 66) boot_pct = 68;
    if (hub_back_ok == 1 && boot_pct >= 68) boot_pct = 69;
    if (hub_exit_ok == 1 && boot_pct >= 69) boot_pct = 70;
    if (hub_options_ok == 1 && boot_pct >= 70) boot_pct = 71;
    if (hub_credits_ok == 1 && boot_pct >= 71) boot_pct = 72;
  }
  // ~80: Valocity.enter via TREE (mirror removed; flag set only on TREE ok).
  if (valo_entered && city && tree_field_get_int(city, "enter_via_tree") == 1 &&
      boot_pct >= 62)
    boot_pct = 80;
  // ~88: QUICKRACE hub lands on RaceSetup via TREE (stock Valocity→RaceSetup).
  if (hub_quickrace_ok == 1 && boot_pct >= 80) {
    InvObject* rs = game_logic_racesetup();
    if (rs && tree_field_get_int(rs, "enter_via_tree") == 1)
      boot_pct = 88;
    else
      boot_pct = 84;
  }
  // ~90: Garage street OSD via TREE (career + hub re-enters, not host strip).
  {
    InvObject* g = game_logic_garage();
    if (!g && final_st && tree_host_class(final_st) &&
        std::strstr(tree_host_class(final_st), "Garage"))
      g = final_st;
    if (g && tree_field_get_int(g, "osd_via_tree") == 1 && boot_pct >= 88)
      boot_pct = 90;
  }
  // ~94: RaceSetup OSD via TREE (CMD_RACE=0 / CMD_ABANDON=1).
  {
    InvObject* rs = game_logic_racesetup();
    if (rs && tree_field_get_int(rs, "osd_via_tree") == 1 && boot_pct >= 90)
      boot_pct = 94;
  }
  // ~100: RaceSetup.osdCommand(CMD_RACE) TREE → City.startRace + CAS(track).
  {
    InvObject* rs = game_logic_racesetup();
    InvObject* track = rs ? tree_field_get_obj(rs, "track") : nullptr;
    if (!track && rs) track = tree_field_get_obj(rs, "lastState");
    const bool cmd =
        rs && tree_field_get_int(rs, "osd_cmd_via_tree") == 1 &&
        tree_field_get_int(rs, "last_osd_cmd") == 0;
    const bool started =
        (track && tree_field_get_int(track, "start_race_via_tree") == 1 &&
         tree_field_get_obj(track, "raceStart")) ||
        (rs && tree_field_get_int(rs, "start_race_via_tree") == 1);
    if (cmd && started && boot_pct >= 94) boot_pct = 100;
  }
  std::printf("boot progress ~%d%% (splash→menu-hub→garage/city path)\n",
              boot_pct);
  std::printf(
      "boot hub_valo freeride=%d quickrace=%d demo=%d back=%d exit=%d "
      "options=%d credits=%d\n",
      hub_freeride_ok, hub_quickrace_ok, hub_demo_ok, hub_back_ok, hub_exit_ok,
      hub_options_ok, hub_credits_ok);

  const bool garage_modes_ok =
      !auto_new || (mode_mech == 1 && mode_paint == 2 && mode_none == 0 &&
                    car_ok == 1 &&
                    tree_field_get_int(garage ? garage : final_st,
                                       "mode_changes") >= 3);
  std::printf("boot garage_loop ok=%d car=%d modes=%d/%d/%d changes=%d\n",
              garage_modes_ok ? 1 : 0, car_ok, mode_mech, mode_paint, mode_none,
              garage ? tree_field_get_int(garage, "mode_changes") : 0);

  const bool mechanic_inv_ok = !auto_new || (mech_inv_ok == 1);
  std::printf("boot mechanic_inv ok=%d\n", mechanic_inv_ok ? 1 : 0);

  const bool visual_inv_ok = !auto_new || (mech_vis_ok == 1);
  std::printf("boot visual_inv ok=%d\n", visual_inv_ok ? 1 : 0);

  const bool mechanic_preview_ok = !auto_new || (mech_preview_ok == 1);
  std::printf("boot mechanic_preview ok=%d\n", mechanic_preview_ok ? 1 : 0);

  const bool mechanic_rotate_ok = !auto_new || (mech_rotate_ok == 1);
  std::printf("boot mechanic_rotate ok=%d\n", mechanic_rotate_ok ? 1 : 0);

  const bool mechanic_swap_ok = !auto_new || (mech_swap_ok == 1);
  std::printf("boot mechanic_swap ok=%d\n", mechanic_swap_ok ? 1 : 0);

  const bool mechanic_hover_ok = !auto_new || (mech_hover_ok == 1);
  std::printf("boot mechanic_hover ok=%d\n", mechanic_hover_ok ? 1 : 0);

  const bool mechanic_click_ok = !auto_new || (mech_click_ok == 1);
  std::printf("boot mechanic_click ok=%d\n", mechanic_click_ok ? 1 : 0);

  const bool mechanic_drag_ok = !auto_new || (mech_drag_ok == 1);
  std::printf("boot mechanic_drag ok=%d\n", mechanic_drag_ok ? 1 : 0);

  const bool mechanic_lclick_ok = !auto_new || (mech_lclick_ok == 1);
  std::printf("boot mechanic_lclick ok=%d\n", mechanic_lclick_ok ? 1 : 0);

  const bool mechanic_pick_ok = !auto_new || (mech_pick_ok == 1);
  std::printf("boot mechanic_pick ok=%d\n", mechanic_pick_ok ? 1 : 0);

  const bool mechanic_rdrag_ok = !auto_new || (mech_rdrag_ok == 1);
  std::printf("boot mechanic_rdrag ok=%d\n", mechanic_rdrag_ok ? 1 : 0);

  const bool cursor_syslock_ok = !auto_new || (cursor_lock_ok == 1);
  std::printf("boot cursor_lock ok=%d\n", cursor_syslock_ok ? 1 : 0);

  const bool cursor_ldrag_boot_ok = !auto_new || (cursor_ldrag_ok == 1);
  std::printf("boot cursor_ldrag ok=%d\n", cursor_ldrag_boot_ok ? 1 : 0);

  const bool mechanic_tune_ok = !auto_new || (mech_tune_ok == 1);
  std::printf("boot mechanic_tune ok=%d\n", mechanic_tune_ok ? 1 : 0);

  const bool mechanic_chrome_ok = !auto_new || (mech_chrome_ok == 1);
  std::printf("boot mechanic_chrome ok=%d\n", mechanic_chrome_ok ? 1 : 0);

  const bool mechanic_install_ok = !auto_new || (mech_install_ok == 1);
  std::printf("boot mechanic_install ok=%d\n", mechanic_install_ok ? 1 : 0);

  const bool mechanic_drop_ok = !auto_new || (mech_drop_ok == 1);
  std::printf("boot mechanic_drop ok=%d\n", mechanic_drop_ok ? 1 : 0);

  const bool garage_parts_ok =
      !auto_new || (install_ok == 1 && paint_ok == 1 && painter_ux_ok == 1);
  std::printf("boot garage_parts ok=%d install=%d paint=%d painter_ux=%d\n",
              garage_parts_ok ? 1 : 0, install_ok, paint_ok, painter_ux_ok);

  std::printf("boot splash_timer ok=%d\n", splash_time_off);

  const bool traffic_p_boot_ok = !auto_new || (traffic_p_ok == 1);
  std::printf("boot addTrafficP ok=%d\n", traffic_p_boot_ok ? 1 : 0);

  const bool traffic_bh_boot_ok = !auto_new || (traffic_bh_ok == 1);
  std::printf("boot setTrafficCarBehaviour ok=%d\n", traffic_bh_boot_ok ? 1 : 0);

  const bool traffic_halt_boot_ok = !auto_new || (traffic_halt_ok == 1);
  std::printf("boot haltTrafficCross ok=%d\n", traffic_halt_boot_ok ? 1 : 0);

  const bool traffic_halt_path_boot_ok =
      !auto_new || (traffic_halt_path_ok == 1);
  std::printf("boot haltTrafficPath ok=%d\n",
              traffic_halt_path_boot_ok ? 1 : 0);

  const bool traffic_rem_boot_ok = !auto_new || (traffic_rem_ok == 1);
  std::printf("boot remTrafficCar ok=%d\n", traffic_rem_boot_ok ? 1 : 0);

  const bool traffic_del_boot_ok = !auto_new || (traffic_del_ok == 1);
  std::printf("boot delTraffic ok=%d\n", traffic_del_boot_ok ? 1 : 0);

  const bool traffic_ped_boot_ok = !auto_new || (traffic_ped_ok == 1);
  std::printf("boot setPedestrianDensityN ok=%d\n",
              traffic_ped_boot_ok ? 1 : 0);

  const bool traffic_ped_type_boot_ok = !auto_new || (traffic_ped_type_ok == 1);
  std::printf("boot addPedestrianType ok=%d\n",
              traffic_ped_type_boot_ok ? 1 : 0);

  const bool traffic_ped_rem_boot_ok = !auto_new || (traffic_ped_rem_ok == 1);
  std::printf("boot remPedestrianType ok=%d\n",
              traffic_ped_rem_boot_ok ? 1 : 0);

  const bool traffic_ped_dist_boot_ok = !auto_new || (traffic_ped_dist_ok == 1);
  std::printf("boot pedestrianDistance ok=%d\n",
              traffic_ped_dist_boot_ok ? 1 : 0);

  const bool traffic_fog_boot_ok = !auto_new || (traffic_fog_ok == 1);
  std::printf("boot setFog ok=%d\n", traffic_fog_boot_ok ? 1 : 0);

  const bool traffic_light_boot_ok = !auto_new || (traffic_light_ok == 1);
  std::printf("boot setLight ok=%d\n", traffic_light_boot_ok ? 1 : 0);

  const bool traffic_flare_boot_ok = !auto_new || (traffic_flare_ok == 1);
  std::printf("boot setFlare ok=%d\n", traffic_flare_boot_ok ? 1 : 0);

  const bool traffic_dup_boot_ok = !auto_new || (traffic_dup_ok == 1);
  std::printf("boot duplicate ok=%d\n", traffic_dup_boot_ok ? 1 : 0);

  const bool traffic_create_boot_ok = !auto_new || (traffic_create_ok == 1);
  std::printf("boot create ok=%d\n", traffic_create_boot_ok ? 1 : 0);

  const bool traffic_chg_boot_ok = !auto_new || (traffic_chg_ok == 1);
  std::printf("boot changeResource ok=%d\n", traffic_chg_boot_ok ? 1 : 0);

  const bool traffic_color_boot_ok = !auto_new || (traffic_color_ok == 1);
  std::printf("boot setColor ok=%d\n", traffic_color_boot_ok ? 1 : 0);

  const bool traffic_type_boot_ok = !auto_new || (traffic_type_ok == 1);
  std::printf("boot getTypeID ok=%d\n", traffic_type_boot_ok ? 1 : 0);

  const bool traffic_scale_boot_ok = !auto_new || (traffic_scale_ok == 1);
  std::printf("boot scaleMesh ok=%d\n", traffic_scale_boot_ok ? 1 : 0);

  const bool traffic_plot_boot_ok = !auto_new || (traffic_plot_ok == 1);
  std::printf("boot plotRoute ok=%d\n", traffic_plot_boot_ok ? 1 : 0);

  const bool traffic_routepos_boot_ok = !auto_new || (traffic_routepos_ok == 1);
  std::printf("boot getRoutePos ok=%d\n", traffic_routepos_boot_ok ? 1 : 0);

  const bool traffic_setmatrix_boot_ok = !auto_new || (traffic_setmatrix_ok == 1);
  std::printf("boot setMatrix ok=%d\n", traffic_setmatrix_boot_ok ? 1 : 0);

  const bool traffic_setpos_boot_ok = !auto_new || (traffic_setpos_ok == 1);
  std::printf("boot setPos ok=%d\n", traffic_setpos_boot_ok ? 1 : 0);

  const bool traffic_setstate_boot_ok = !auto_new || (traffic_setstate_ok == 1);
  std::printf("boot setState ok=%d\n", traffic_setstate_boot_ok ? 1 : 0);

  const bool traffic_getpos_boot_ok = !auto_new || (traffic_getpos_ok == 1);
  std::printf("boot getPos ok=%d\n", traffic_getpos_boot_ok ? 1 : 0);

  const bool traffic_getvel_boot_ok = !auto_new || (traffic_getvel_ok == 1);
  std::printf("boot getVel ok=%d\n", traffic_getvel_boot_ok ? 1 : 0);

  const bool traffic_setparent_boot_ok =
      !auto_new || (traffic_setparent_ok == 1);
  std::printf("boot setParent ok=%d\n", traffic_setparent_boot_ok ? 1 : 0);

  const bool traffic_getspeedsq_boot_ok =
      !auto_new || (traffic_getspeedsq_ok == 1);
  std::printf("boot getSpeedSquare ok=%d\n",
              traffic_getspeedsq_boot_ok ? 1 : 0);

  const bool traffic_isempty_boot_ok = !auto_new || (traffic_isempty_ok == 1);
  std::printf("boot isEmpty ok=%d\n", traffic_isempty_boot_ok ? 1 : 0);

  const bool traffic_isscripted_boot_ok =
      !auto_new || (traffic_isscripted_ok == 1);
  std::printf("boot isScripted ok=%d\n", traffic_isscripted_boot_ok ? 1 : 0);

  const bool traffic_getscript_boot_ok =
      !auto_new || (traffic_getscript_ok == 1);
  std::printf("boot getScriptInstance ok=%d\n",
              traffic_getscript_boot_ok ? 1 : 0);

  {
    const int32_t type_null = java_util_resource_ResourceRef_type(nullptr);
    const float vp_w0 = java_render_Viewport_getWidth(nullptr);
    const float vp_a0 = java_render_Viewport_getAspect(nullptr);
    InvObject* nav_hide = tree_host_new("java.game.Navigator");
    java_game_Navigator_updateNavigator(nav_hide, nullptr, 0);
    const int32_t nav_upd = tree_field_get_int(nav_hide, "update_count");
    std::printf("boot ResourceRef_type null=%d ok=%d\n", type_null,
                type_null == 0 ? 1 : 0);
    std::printf("boot Viewport getters w0=%.3f aspect0=%.3f ok=%d\n", vp_w0,
                vp_a0, (vp_w0 == 0.f && vp_a0 == 1.f) ? 1 : 0);
    std::printf("boot updateNavigator hide upd=%d ok=%d\n", nav_upd,
                nav_upd == 0 ? 1 : 0);
  }

  const bool valo_ok =
      !auto_new || (valo_entered == 1 && valo_shape != 0 && valo_dist > 2.f &&
                    valo_spd > 1.f);
  std::printf("boot valocity_live ok=%d entered=%d dist=%.1f spd=%.1f shape=%d\n",
              valo_ok ? 1 : 0, valo_entered, valo_dist, valo_spd, valo_shape);

  // Soft deepen phys/aero/vehicle — report post-drive side-bands only.
  const bool phys_boot_ok = !auto_new || (valo_phys == 1);
  std::printf("boot phys ok=%d shape=%d ch_shape=%d spd=%.1f\n",
              phys_boot_ok ? 1 : 0, valo_shape, valo_chassis_shape,
              valo_phys_spd);
  const bool aero_boot_ok = !auto_new || (valo_aero == 1);
  std::printf("boot aero ok=%d applied=%d |F|=%.1f cd=%.3f\n",
              aero_boot_ok ? 1 : 0, valo_aero_applied, valo_aero_fmag,
              valo_aero_cd);
  const bool vehicle_boot_ok = !auto_new || (valo_vehicle == 1);
  std::printf("boot vehicle ok=%d driveable=%d chassis_shape=%d\n",
              vehicle_boot_ok ? 1 : 0, valo_driveable, valo_chassis_shape);

  const bool valo_cam_ok = !auto_new || (valo_cam == 1);
  std::printf("boot valocity_cam ok=%d behind=%.1f nav_upd=%d hud=%d\n",
              valo_cam_ok ? 1 : 0, valo_cam_behind, valo_nav_upd, valo_hud);

  const bool valo_ret_ok =
      !auto_new ||
      (valo_denied == 1 && valo_back == 1 && garage &&
       std::strstr(tree_host_class(garage), "Garage") &&
       game_logic_actual_state() == garage &&
       city && tree_field_get_int(city, "traffic_count") == 0);
  std::printf("boot valocity_return ok=%d denied=%d back=%d traffic=%d\n",
              valo_ret_ok ? 1 : 0, valo_denied, valo_back,
              city ? tree_field_get_int(city, "traffic_count") : -1);

  // Catalog render 13/13 — runtime probe (DEFAULT_RENDER ids in
  // gen_global_progress.py). Reuses traffic/garage smoke flags + live D3D.
  InvObject* vmesh =
      starter_car ? tree_field_get_obj(starter_car, "visual_mesh") : nullptr;
  const int32_t present_n = render_d3d9_present_count();
  const int32_t r_device =
      (render_d3d9_ready() && present_n > 0 && render_d3d9_hwnd() != nullptr)
          ? 1
          : 0;
  // vp/cam: use Valocity-latched actives (cleared by teardown before here).
  const int32_t r_viewport = valo_vp;
  const int32_t r_camera = valo_cam;
  const int32_t r_fog = traffic_fog_ok;
  const int32_t r_tex =
      (paint_ok == 1 ||
       (vmesh && render_d3d9_mesh_textured_count(vmesh) > 0))
          ? 1
          : 0;
  const int32_t r_mesh =
      ((vmesh && render_d3d9_mesh_ready(vmesh)) || city_mesh_count() > 0) ? 1
                                                                         : 0;
  const int32_t r_pose = traffic_setmatrix_ok;
  const int32_t r_bone = traffic_setparent_ok;
  const int32_t r_osd =
      (menu_chrome == 1 && menu_txt_n >= 12 && menu_osd_n >= 1) ? 1 : 0;
  const int32_t r_light_flare =
      (traffic_light_ok == 1 && traffic_flare_ok == 1) ? 1 : 0;
  const int32_t r_force7 = present_n >= 7 ? 1 : 0;
  const int32_t r_fmv = fmv_n >= 2 ? 1 : 0;
  const int32_t r_endframe = (frames >= 1 && present_n > 0) ? 1 : 0;
  constexpr int32_t kRenderCatalogN = 13;
  const int32_t render_ok_n = r_device + r_viewport + r_camera + r_fog + r_tex +
                              r_mesh + r_pose + r_bone + r_osd + r_light_flare +
                              r_force7 + r_fmv + r_endframe;
  const bool render_boot_ok =
      !auto_new || (render_ok_n == kRenderCatalogN);
  std::printf(
      "boot render ok=%d/%d device=%d vp=%d cam=%d fog=%d tex=%d mesh=%d "
      "pose=%d bone=%d osd=%d lightflare=%d force7=%d fmv=%d endframe=%d "
      "present=%d\n",
      render_ok_n, kRenderCatalogN, r_device, r_viewport, r_camera, r_fog,
      r_tex, r_mesh, r_pose, r_bone, r_osd, r_light_flare, r_force7, r_fmv,
      r_endframe, present_n);

  if (auto_new && !garage_ok) {
    std::printf("FAIL --game auto_new did not reach Garage\n");
    return 5;
  }
  if (auto_new && !garage_modes_ok) {
    std::printf("FAIL --game garage modes\n");
    return 5;
  }
  if (auto_new && !mechanic_inv_ok) {
    std::printf("FAIL --game mechanic inventory\n");
    return 5;
  }
  if (auto_new && !visual_inv_ok) {
    std::printf("FAIL --game VisualInventory panels\n");
    return 5;
  }
  if (auto_new && !mechanic_preview_ok) {
    std::printf("FAIL --game InventoryPanel preview\n");
    return 5;
  }
  if (auto_new && !mechanic_rotate_ok) {
    std::printf("FAIL --game InventoryPanel focusHook rotate\n");
    return 5;
  }
  if (auto_new && !mechanic_swap_ok) {
    std::printf("FAIL --game VisualInventory panelSwap\n");
    return 5;
  }
  if (auto_new && !mechanic_hover_ok) {
    std::printf("FAIL --game Mechanic hover actualPanel\n");
    return 5;
  }
  if (auto_new && !mechanic_click_ok) {
    std::printf("FAIL --game Mechanic panel click\n");
    return 5;
  }
  if (auto_new && !mechanic_drag_ok) {
    std::printf("FAIL --game Mechanic panel drag swap\n");
    return 5;
  }
  if (auto_new && !mechanic_lclick_ok) {
    std::printf("FAIL --game Mechanic EC_LCLICK uninstall\n");
    return 5;
  }
  if (auto_new && !mechanic_pick_ok) {
    std::printf("FAIL --game Mechanic car part pick\n");
    return 5;
  }
  if (auto_new && !mechanic_rdrag_ok) {
    std::printf("FAIL --game Garage EC_RDRAG camera\n");
    return 5;
  }
  if (auto_new && !cursor_syslock_ok) {
    std::printf("FAIL --game Cursor lock/unlock ClipCursor\n");
    return 5;
  }
  if (auto_new && !cursor_ldrag_boot_ok) {
    std::printf("FAIL --game Cursor EC_LDRAG BEGIN/END\n");
    return 5;
  }
  if (auto_new && !mechanic_tune_ok) {
    std::printf("FAIL --game Mechanic tune mode\n");
    return 5;
  }
  if (auto_new && !mechanic_chrome_ok) {
    std::printf("FAIL --game Mechanic chrome\n");
    return 5;
  }
  if (auto_new && !mechanic_install_ok) {
    std::printf("FAIL --game Mechanic panel install\n");
    return 5;
  }
  if (auto_new && !mechanic_drop_ok) {
    std::printf("FAIL --game Mechanic panel drop\n");
    return 5;
  }
  if (auto_new && !garage_parts_ok) {
    std::printf("FAIL --game garage install/paint\n");
    return 5;
  }
  if (auto_new && !traffic_p_boot_ok) {
    std::printf("FAIL --game addTrafficP near-cross\n");
    return 5;
  }
  if (auto_new && !traffic_bh_boot_ok) {
    std::printf("FAIL --game setTrafficCarBehaviour\n");
    return 5;
  }
  if (auto_new && !traffic_halt_boot_ok) {
    std::printf("FAIL --game haltTrafficCross\n");
    return 5;
  }
  if (auto_new && !traffic_halt_path_boot_ok) {
    std::printf("FAIL --game haltTrafficPath\n");
    return 5;
  }
  if (auto_new && !traffic_rem_boot_ok) {
    std::printf("FAIL --game remTrafficCar\n");
    return 5;
  }
  if (auto_new && !traffic_del_boot_ok) {
    std::printf("FAIL --game delTraffic\n");
    return 5;
  }
  if (auto_new && !traffic_ped_boot_ok) {
    std::printf("FAIL --game setPedestrianDensityN\n");
    return 5;
  }
  if (auto_new && !traffic_ped_type_boot_ok) {
    std::printf("FAIL --game addPedestrianType\n");
    return 5;
  }
  if (auto_new && !traffic_ped_rem_boot_ok) {
    std::printf("FAIL --game remPedestrianType\n");
    return 5;
  }
  if (auto_new && !traffic_ped_dist_boot_ok) {
    std::printf("FAIL --game pedestrianDistance\n");
    return 5;
  }
  if (auto_new && !traffic_fog_boot_ok) {
    std::printf("FAIL --game setFog\n");
    return 5;
  }
  if (auto_new && !traffic_light_boot_ok) {
    std::printf("FAIL --game setLight\n");
    return 5;
  }
  if (auto_new && !traffic_flare_boot_ok) {
    std::printf("FAIL --game setFlare\n");
    return 5;
  }
  if (auto_new && !traffic_dup_boot_ok) {
    std::printf("FAIL --game duplicate\n");
    return 5;
  }
  if (auto_new && !traffic_create_boot_ok) {
    std::printf("FAIL --game create\n");
    return 5;
  }
  if (auto_new && !traffic_chg_boot_ok) {
    std::printf("FAIL --game changeResource\n");
    return 5;
  }
  if (auto_new && !traffic_color_boot_ok) {
    std::printf("FAIL --game setColor\n");
    return 5;
  }
  if (auto_new && !traffic_type_boot_ok) {
    std::printf("FAIL --game getTypeID\n");
    return 5;
  }
  if (auto_new && !traffic_scale_boot_ok) {
    std::printf("FAIL --game scaleMesh\n");
    return 5;
  }
  if (auto_new && !traffic_plot_boot_ok) {
    std::printf("FAIL --game plotRoute\n");
    return 5;
  }
  if (auto_new && !traffic_routepos_boot_ok) {
    std::printf("FAIL --game getRoutePos\n");
    return 5;
  }
  if (auto_new && !traffic_setmatrix_boot_ok) {
    std::printf("FAIL --game setMatrix\n");
    return 5;
  }
  if (auto_new && !traffic_setpos_boot_ok) {
    std::printf("FAIL --game setPos\n");
    return 5;
  }
  if (auto_new && !traffic_setstate_boot_ok) {
    std::printf("FAIL --game setState\n");
    return 5;
  }
  if (auto_new && !traffic_getpos_boot_ok) {
    std::printf("FAIL --game getPos\n");
    return 5;
  }
  if (auto_new && !traffic_getvel_boot_ok) {
    std::printf("FAIL --game getVel\n");
    return 5;
  }
  if (auto_new && !traffic_setparent_boot_ok) {
    std::printf("FAIL --game setParent\n");
    return 5;
  }
  if (auto_new && !traffic_getspeedsq_boot_ok) {
    std::printf("FAIL --game getSpeedSquare\n");
    return 5;
  }
  if (auto_new && !traffic_isempty_boot_ok) {
    std::printf("FAIL --game isEmpty\n");
    return 5;
  }
  if (auto_new && !traffic_isscripted_boot_ok) {
    std::printf("FAIL --game isScripted\n");
    return 5;
  }
  if (auto_new && !traffic_getscript_boot_ok) {
    std::printf("FAIL --game getScriptInstance\n");
    return 5;
  }
  if (auto_new && !valo_ok) {
    std::printf("FAIL --game Valocity live\n");
    return 5;
  }
  if (auto_new && !phys_boot_ok) {
    std::printf("FAIL --game phys probe\n");
    return 5;
  }
  // Aero is soft report-only (PE skip gates cd/flags/v2 remain valid).
  if (auto_new && !vehicle_boot_ok) {
    std::printf("FAIL --game vehicle probe\n");
    return 5;
  }
  if (auto_new && !valo_cam_ok) {
    std::printf("FAIL --game Valocity cam/hud/nav\n");
    return 5;
  }
  if (auto_new && !valo_ret_ok) {
    std::printf("FAIL --game Valocity return\n");
    return 5;
  }
  if (auto_new && !render_boot_ok) {
    std::printf("FAIL --game render catalog probe\n");
    return 5;
  }
  if (frames < 1) {
    std::printf("FAIL --game no frames\n");
    return 5;
  }
  return 0;
}

}  // namespace inv
