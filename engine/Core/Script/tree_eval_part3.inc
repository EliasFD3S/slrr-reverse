// Fragment 3/3 of tree_eval.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.

                next = JvmValue::make_obj(nullptr);
                if (!locals.empty() && locals[0].tag == JvmTag::Obj) {
                  InvObject* o = locals[0].v.o;
                  const char* lc = tree_host_class(o);
                  // RaceSetup.osdCommand / Track only — not MainMenuDialog
                  // CMD_EXIT (explicit null → System.exit), not Valocity/City
                  // CAS(racesetup|parent) invent.
                  if (lc && (std::strstr(lc, "RaceSetup") ||
                             (std::strstr(lc, "Track") &&
                              !std::strstr(lc, "City")))) {
                    InvObject* t = tree_field_get_obj(o, "track");
                    if (!t) t = tree_field_get_obj(o, "lastState");
                    if (game_logic_is_section(t)) next = JvmValue::make_obj(t);
                  }
                }
              }
              host->call_by_name("java.game.GameLogic", "changeActiveSection",
                                 {next}, true);
              // Same post-CAS gate as 0x12: only when section left MainMenu
              // (Garage/Valocity enter already ran). No hub-pending freestyle.
              if (!locals.empty() && locals[0].tag == JvmTag::Obj &&
                  locals[0].v.o) {
                const char* shc = tree_host_class(locals[0].v.o);
                if (shc && std::strstr(shc, "MainMenuDialog")) {
                  InvObject* cur = game_logic_actual_state();
                  const char* cn = cur ? tree_host_class(cur) : nullptr;
                  if (!cn || !std::strstr(cn, "MainMenu"))
                    return JvmValue::make_void();
                }
              }
              break;
            }
            // Input.cursor.enable(int): JVM order [MouseCursor, int] with int on
            // top. Generic recv-on-top would steal Dialog `this` and miss the call.
            if (mname == "enable") {
              JvmValue arg = JvmValue::make_int(0);
              if (stack.size() >= 2 && stack.back().tag == JvmTag::Int &&
                  stack[stack.size() - 2].tag == JvmTag::Obj) {
                arg = pop();
                recv = pop();
              } else if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                recv = pop();
                if (!stack.empty()) arg = pop();
              } else {
                recv = JvmValue::make_obj(java_io_Input_cursor());
                if (!stack.empty()) arg = pop();
              }
              args.push_back(recv);
              args.push_back(arg);
            } else if (mname == "createButton" || mname == "createText" ||
                       mname == "createTextBox" || mname == "createRectangle" ||
                       mname == "createHeader" || mname == "createMenu" ||
                       mname == "addItem" || mname == "addSeparator" ||
                       mname == "setSliderStyle") {
              // JVM order: [recv, args…] with last arg on top. For Menu.* the
              // receiver is often under args; fall back to last_menu.
              // Drop OSD leftovers (Rectangle/Button/Text) that createBG/
              // createHeader/addItem left when mis-kept on the stack.
              auto is_osd_junk = [&](const JvmValue& v) -> bool {
                if (v.tag != JvmTag::Obj || !v.v.o) return false;
                const char* hc = tree_host_class(v.v.o);
                if (!hc || !hc[0]) return false;  // bare String — keep
                return std::strstr(hc, "Rectangle") ||
                       std::strstr(hc, "Button") ||
                       std::strstr(hc, "Text") ||
                       std::strstr(hc, "Hotkey") ||
                       std::strstr(hc, "Group");
              };
              while (!stack.empty() && is_osd_junk(stack.back())) pop();

              if (mname == "addItem" && (argc == 2 || argc == 3)) {
                // Prefer real (String,int) or (ResourceRef,int,String): scan.
                JvmValue label = JvmValue::make_obj(nullptr);
                JvmValue tip = JvmValue::make_obj(nullptr);
                JvmValue gfx = JvmValue::make_obj(nullptr);
                JvmValue cmdv = JvmValue::make_int(0);
                bool have_cmd = false;
                int32_t gfx_rid = 0;
                for (int guard = 0; guard < 12 && !stack.empty(); ++guard) {
                  JvmValue v = pop();
                  if (v.tag == JvmTag::Int) {
                    if (v.v.i > 0x10000)
                      gfx_rid = v.v.i;
                    else if (is_icon_osd_cmd(v.v.i) || !have_cmd) {
                      cmdv = v;
                      have_cmd = true;
                    }
                  } else if (v.tag == JvmTag::Obj && v.v.o) {
                    const char* hc = tree_host_class(v.v.o);
                    if (hc && (std::strstr(hc, "ResourceRef") ||
                               std::strstr(hc, "GameRef"))) {
                      if (!gfx.v.o) gfx = v;
                    } else if (!hc || !hc[0] || std::strstr(hc, "String")) {
                      const char* sc = string_cstr(v.v.o);
                      if (sc && is_osd_invoke_name(sc)) {
                        /* skip method-name ldc */
                      } else if (!label.v.o)
                        label = v;
                      else if (!tip.v.o)
                        tip = v;
                    } else if (hc && std::strstr(hc, "Menu")) {
                      recv = v;
                    }
                  }
                  if (have_cmd && (gfx.v.o || label.v.o) && cmdv.v.i != 0)
                    break;
                }
                if (!gfx.v.o && recent_new) {
                  const char* rhc = tree_host_class(recent_new);
                  if (rhc && (std::strstr(rhc, "ResourceRef") ||
                              std::strstr(rhc, "GameRef"))) {
                    gfx = JvmValue::make_obj(recent_new);
                    recent_new = nullptr;
                  }
                }
                if (gfx.v.o && gfx_rid)
                  java_util_resource_ResourceRef_set(gfx.v.o, gfx_rid);
                if (recv.tag != JvmTag::Obj || !recv.v.o) {
                  if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                    const char* thc = tree_host_class(stack.back().v.o);
                    if (thc && std::strstr(thc, "Menu"))
                      recv = pop();
                  }
                  if ((recv.tag != JvmTag::Obj || !recv.v.o) && last_menu)
                    recv = JvmValue::make_obj(last_menu);
                }
                while (!stack.empty() && is_osd_junk(stack.back())) pop();
                while (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                  const char* hc = tree_host_class(stack.back().v.o);
                  if (hc && (std::strstr(hc, "ResourceRef") ||
                             std::strstr(hc, "GameRef"))) {
                    if (!gfx.v.o) gfx = pop();
                    else
                      pop();
                  } else if (hc && (std::strstr(hc, "Style") ||
                                    std::strstr(hc, "Button")))
                    pop();
                  else
                    break;
                }
                args.clear();
                if (gfx.v.o) {
                  args.push_back(gfx);
                  args.push_back(cmdv);
                  args.push_back(tip.v.o ? tip : label);
                } else {
                  args.push_back(label);
                  args.push_back(cmdv);
                  if (tip.v.o) args.push_back(tip);
                }
                args.insert(args.begin(), recv);
              } else {
                for (int i = 0; i < argc; ++i) {
                  while (!stack.empty() && is_osd_junk(stack.back())) pop();
                  if (!stack.empty())
                    args.insert(args.begin(), pop());
                  else
                    args.insert(args.begin(), JvmValue::make_int(0));
                }
                if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                  const char* thc = tree_host_class(stack.back().v.o);
                  if (mname == "addItem" || mname == "addSeparator" ||
                      mname == "setSliderStyle") {
                    if (thc && std::strstr(thc, "Menu"))
                      recv = pop();
                    else if (last_menu)
                      recv = JvmValue::make_obj(last_menu);
                    else
                      recv = last_menu ? JvmValue::make_obj(last_menu)
                                       : pop();
                  } else {
                    recv = pop();
                  }
                } else if (last_menu &&
                           (mname == "addItem" || mname == "addSeparator" ||
                            mname == "setSliderStyle")) {
                  recv = JvmValue::make_obj(last_menu);
                } else if (!locals.empty() && locals[0].tag == JvmTag::Obj) {
                  if (InvObject* osd =
                          tree_field_get_obj(locals[0].v.o, "osd"))
                    recv = JvmValue::make_obj(osd);
                  else
                    recv = locals[0];
                }
                while (!stack.empty() && is_osd_junk(stack.back())) pop();
                // Also drop stray Menu/Style duplicates.
                while (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                  const char* hc = tree_host_class(stack.back().v.o);
                  if (hc && (std::strstr(hc, "Button") ||
                             std::strstr(hc, "Menu") ||
                             std::strstr(hc, "Style")))
                    pop();
                  else
                    break;
                }
                args.insert(args.begin(), recv);
              }
            } else if (mname == "addTraffic" || mname == "addTrafficCar" ||
                       mname == "addTrafficN" || mname == "addTrafficP" ||
                       mname == "notifyTrafficCar" || mname == "setTrafficCarBehaviour" ||
                       mname == "remTrafficCar" ||
                       mname == "setPedestrianDensity" ||
                       mname == "setPedestrianDensityN" ||
                       mname == "addPedestrianType" ||
                       mname == "remPedestrianType" || mname == "setWater" ||
                       mname == "addWaterLimit" ||
                       mname == "getNearestCross" ||
                       mname == "getStartDirection" ||
                       mname == "getRouteLength" || mname == "findRoute" ||
                       mname == "haltTrafficCross" ||
                       mname == "haltTrafficPath" || mname == "alignToRoad" ||
                       mname == "delTraffic") {
              auto resolve_map = [&](InvObject* o) -> InvObject* {
                if (!o) return nullptr;
                if (InvObject* map = tree_field_get_obj(o, "map")) return map;
                if (InvObject* track = tree_field_get_obj(o, "track")) {
                  if (InvObject* map = tree_field_get_obj(track, "map"))
                    return map;
                }
                if (InvObject* last = tree_field_get_obj(o, "lastState")) {
                  if (InvObject* map = tree_field_get_obj(last, "map"))
                    return map;
                }
                return nullptr;
              };
              for (int i = 0; i < argc; ++i) {
                if (!stack.empty())
                  args.insert(args.begin(), pop());
                else
                  args.insert(args.begin(), JvmValue::make_int(0));
              }
              if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                const char* thc = tree_host_class(stack.back().v.o);
                if (thc && std::strstr(thc, "GroundRef"))
                  recv = pop();
              }
              if ((recv.tag != JvmTag::Obj || !recv.v.o) && !locals.empty() &&
                  locals[0].tag == JvmTag::Obj && locals[0].v.o) {
                if (InvObject* map = resolve_map(locals[0].v.o))
                  recv = JvmValue::make_obj(map);
                else
                  recv = locals[0];
              }
              args.insert(args.begin(), recv);
            } else if (mname == "addMarker" || mname == "remMarker" ||
                       mname == "updateNavigator" || mname == "changeMode" ||
                       mname == "changeSize" || mname == "changeZoom") {
              auto resolve_nav = [&](InvObject* o) -> InvObject* {
                if (!o) return nullptr;
                const char* ohc = tree_host_class(o);
                if (ohc && std::strstr(ohc, "Navigator")) return o;
                if (InvObject* nav = tree_field_get_obj(o, "nav")) return nav;
                if (InvObject* track = tree_field_get_obj(o, "track")) {
                  if (InvObject* nav = tree_field_get_obj(track, "nav"))
                    return nav;
                }
                if (InvObject* last = tree_field_get_obj(o, "lastState")) {
                  if (InvObject* nav = tree_field_get_obj(last, "nav"))
                    return nav;
                }
                return nullptr;
              };
              for (int i = 0; i < argc; ++i) {
                if (!stack.empty())
                  args.insert(args.begin(), pop());
                else
                  args.insert(args.begin(), JvmValue::make_int(0));
              }
              if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                const char* thc = tree_host_class(stack.back().v.o);
                if (thc && (std::strstr(thc, "Navigator") ||
                            std::strstr(thc, "Nav")))
                  recv = pop();
              }
              if ((recv.tag != JvmTag::Obj || !recv.v.o) && !locals.empty() &&
                  locals[0].tag == JvmTag::Obj && locals[0].v.o) {
                const char* lhc = tree_host_class(locals[0].v.o);
                const bool garage_or_dlg =
                    lhc && (std::strstr(lhc, "Garage") ||
                            std::strstr(lhc, "Dialog") ||
                            std::strstr(lhc, "MainMenu"));
                if (mname == "changeMode" && garage_or_dlg)
                  recv = locals[0];
                else if (InvObject* nav = resolve_nav(locals[0].v.o))
                  recv = JvmValue::make_obj(nav);
                else
                  recv = locals[0];
              }
              args.insert(args.begin(), recv);
            } else if (mname == "setEventMask" || mname == "clearEventMask" ||
                mname == "addTimer") {
              for (int i = 0; i < argc; ++i) {
                if (!stack.empty())
                  args.insert(args.begin(), pop());
                else
                  args.insert(args.begin(), JvmValue::make_int(0));
              }
              if (mname == "addTimer" && !args.empty() &&
                  args[0].tag == JvmTag::Int) {
                args[0] =
                    JvmValue::make_float(static_cast<float>(args[0].v.i));
              }
              recv = !locals.empty() ? locals[0] : JvmValue::make_obj(nullptr);
              args.insert(args.begin(), recv);
            } else if (mname == "queueEvent") {
              tree_pack_queue_event(stack, locals, args, &recv);
            } else if (mname == "create" && argc == 3) {
              tree_pack_renderref_create(stack, locals, args, &recv);
            } else if (mname == "add" || mname == "mul" || mname == "sub") {
              tree_pack_vector3_binop(stack, locals, args, &recv);
            } else {
              // TREE convention: receiver on top (VT addElement, Osd.createBG…).
              auto is_self_method = [&](const std::string& m) {
                // enter/exit/startRace*: City/Valocity/Garage self — same PE
                // Object_callMethod name gate as 0x12 is_self_method.
                return m == "createOSDObjects" || m == "lockCar" ||
                       m == "releaseCar" ||
                       m == "addSceneElements" || m == "remSceneElements" ||
                       m == "cameraSetup" || m == "giveWarning" ||
                       m == "createQuickRaceBot" || m == "destroyRaceBot" ||
                       m == "enter" || m == "exit" || m == "startRace" ||
                       m == "startRace2";
              };
              if (is_self_method(mname) && !locals.empty() &&
                  locals[0].tag == JvmTag::Obj) {
                if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                    stack.back().v.o != locals[0].v.o)
                  pop();
                recv = locals[0];
              } else if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                recv = pop();
              } else if (!locals.empty()) {
                recv = locals[0];
              }
              args.push_back(recv);
              for (int i = 0; i < argc; ++i) {
                if (!stack.empty())
                  args.insert(args.begin() + 1, pop());
                else
                  args.insert(args.begin() + 1, JvmValue::make_int(0));
              }
            }

            const char* owner = cls_name.c_str();
            const char* hc = tree_host_class(recv.v.o);
            if (hc && hc[0]) owner = hc;
            if ((mname == "add" || mname == "mul" || mname == "sub") &&
                recv.tag == JvmTag::Obj && tree_is_vector3(recv.v.o))
              owner = "java.lang.Vector3";
            if (mname == "enable") owner = "java.io.MouseCursor";
            if (mname == "addElement" || mname == "elementAt" ||
                mname == "size") {
              if (!args.empty() && args[0].tag == JvmTag::Obj &&
                  !tree_vector_is(args[0].v.o) && args.size() >= 2 &&
                  args[1].tag == JvmTag::Obj && tree_vector_is(args[1].v.o)) {
                std::swap(args[0], args[1]);
              }
              owner = "java.util.Vector";
            }
            if ((mname == "createBG" || mname == "createHotkey" ||
                 mname == "createButton" || mname == "createText" ||
                 mname == "createTextBox" || mname == "createRectangle" ||
                 mname == "createHeader" || mname == "createMenu" ||
                 mname == "show" || mname == "hide")) {
              if (hc && std::strstr(hc, "Osd")) {
                owner = "java.render.Osd";
              } else if (!locals.empty() && locals[0].tag == JvmTag::Obj) {
                if (InvObject* osd =
                        tree_field_get_obj(locals[0].v.o, "osd")) {
                  args[0] = JvmValue::make_obj(osd);
                  owner = "java.render.Osd";
                }
              }
            }
            if (mname == "addTraffic" || mname == "addTrafficCar" ||
                mname == "addTrafficN" || mname == "addTrafficP" ||
                mname == "notifyTrafficCar" || mname == "setTrafficCarBehaviour" || mname == "remTrafficCar" ||
                mname == "setPedestrianDensity" ||
                mname == "setPedestrianDensityN" ||
                mname == "addPedestrianType" ||
                mname == "remPedestrianType" || mname == "setWater" ||
                mname == "addWaterLimit" || mname == "getNearestCross" ||
                mname == "getStartDirection" || mname == "getRouteLength" ||
                mname == "findRoute" || mname == "haltTrafficCross" ||
                mname == "haltTrafficPath" || mname == "alignToRoad" ||
                mname == "delTraffic") {
              owner = "java.util.resource.GroundRef";
              if (!args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
                const char* rhc = tree_host_class(args[0].v.o);
                if (!rhc || !std::strstr(rhc, "GroundRef")) {
                  if (!locals.empty() && locals[0].tag == JvmTag::Obj &&
                      locals[0].v.o) {
                    InvObject* o = locals[0].v.o;
                    InvObject* map = tree_field_get_obj(o, "map");
                    if (!map) {
                      if (InvObject* track = tree_field_get_obj(o, "track"))
                        map = tree_field_get_obj(track, "map");
                    }
                    if (!map) {
                      if (InvObject* last = tree_field_get_obj(o, "lastState"))
                        map = tree_field_get_obj(last, "map");
                    }
                    if (map) args[0] = JvmValue::make_obj(map);
                  }
                }
              }
            }
            if (mname == "addMarker" || mname == "remMarker" ||
                mname == "updateNavigator" || mname == "changeMode" ||
                mname == "changeSize" || mname == "changeZoom") {
              if (!args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
                const char* rhc = tree_host_class(args[0].v.o);
                if (rhc && (std::strstr(rhc, "Garage") ||
                            std::strstr(rhc, "Dialog") ||
                            std::strstr(rhc, "MainMenu")) &&
                    mname == "changeMode") {
                  owner = rhc;
                } else if (!rhc || !std::strstr(rhc, "Navigator")) {
                  if (!locals.empty() && locals[0].tag == JvmTag::Obj &&
                      locals[0].v.o) {
                    InvObject* o = locals[0].v.o;
                    InvObject* nav = tree_field_get_obj(o, "nav");
                    if (!nav) {
                      if (InvObject* track = tree_field_get_obj(o, "track"))
                        nav = tree_field_get_obj(track, "nav");
                    }
                    if (!nav) {
                      if (InvObject* last = tree_field_get_obj(o, "lastState"))
                        nav = tree_field_get_obj(last, "nav");
                    }
                    if (nav) {
                      args[0] = JvmValue::make_obj(nav);
                      owner = "java.game.Navigator";
                    }
                  }
                } else {
                  owner = "java.game.Navigator";
                }
              } else {
                owner = "java.game.Navigator";
              }
            }

            if ((mname == "setSliderStyle" || mname == "addItem" ||
                 mname == "addSeparator") &&
                last_menu && !args.empty()) {
              const char* rhc = tree_host_class(args[0].v.o);
              if (!rhc || !std::strstr(rhc, "Menu")) {
                args[0] = JvmValue::make_obj(last_menu);
                owner = "java.render.osd.Menu";
              }
            }
            if (mname == "addCustomGroups" && !locals.empty() &&
                locals[0].tag == JvmTag::Obj && locals[0].v.o) {
              args.clear();
              args.push_back(locals[0]);
              const char* hc = tree_host_class(locals[0].v.o);
              owner = (hc && hc[0]) ? hc : "java.game.MainMenuDialog";
            }

            JvmValue r =
                soft_call_by_name(owner, mname, args, /*is_static=*/false);
            if (mname == "createMenu" && r.tag == JvmTag::Obj && r.v.o)
              last_menu = r.v.o;
            if (mname == "addItem" && r.tag == JvmTag::Obj && r.v.o)
              last_menu_item = r.v.o;
            if (mname == "endGroup" && r.tag == JvmTag::Int)
              capture_endgroup_field(r.v.i, ip);
            // After Soft CAS left MainMenu → Garage/Valocity: stop leftover
            // menu TREE. No hub freeride/new/exit pending abort (≠ PE continue).
            if (mname == "changeActiveSection" && !locals.empty() &&
                locals[0].tag == JvmTag::Obj && locals[0].v.o) {
              const char* shc = tree_host_class(locals[0].v.o);
              if (shc && std::strstr(shc, "MainMenuDialog")) {
                InvObject* cur = game_logic_actual_state();
                const char* cn = cur ? tree_host_class(cur) : nullptr;
                if (!cn || !std::strstr(cn, "MainMenu"))
                  return JvmValue::make_void();
              }
            }
            // display(): push only when used in if(display()==0). Statement
            // calls (PlayerSetupDialog.display()) must not leave 0 for a
            // later IFEQ or CMD_NEW skips loadDefaults/CAS(garage).
            bool keep_display = false;
            if (mname == "display") {
              if (!args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
                InvObject* d = args[0].v.o;
                const char* dhc = tree_host_class(d);
                if (InvObject* hint = tree_field_get_obj(d, "host_class_hint"))
                  if (const char* hs = string_cstr(hint))
                    if (hs && hs[0]) dhc = hs;
                if (dhc && std::strstr(dhc, "StringRequester"))
                  keep_display = true;
              }
            }
            // Soft commit ≡ PE invokeMethod non-V push @ 0x420129 /
            // empty-body Int0 @ 0x42066c. createBG/Header/addItem stay
            // statement-discard on Soft 0x1a (≠ PE always-push).
            r = soft_invoke_commit(mname, r, keep_display);
            if ((mname == "getFirstChild" || mname == "getNextChild") &&
                !locals.empty() && r.tag != JvmTag::Void) {
              locals[0] = r;
            }
          }
          break;
        }
        JvmValue cond;
        const int ni = name_local >= 0 ? name_local : 3;
        if (ni >= 0 && static_cast<size_t>(ni) < locals.size() &&
            locals[static_cast<size_t>(ni)].tag == JvmTag::Obj) {
          cond = locals[static_cast<size_t>(ni)];
        } else {
          cond = pop();
        }
        // Two codegen shapes:
        //  A) target is exit/cleanup → jump when falsy (File.delete)
        //  B) target is continue (has relative GOTO) → jump when tree_truthy (File.copy)
        bool target_continue = false;
        if (n.has_imm && n.imm < tree.nodes.size()) {
          for (size_t j = n.imm; j < tree.nodes.size() && j < n.imm + 5; ++j) {
            if (tree.nodes[j].op == 0x14 && tree.nodes[j].has_imm &&
                (tree.nodes[j].imm & 0x80000000u)) {
              target_continue = true;
              break;
            }
          }
        }
        const bool do_jump = target_continue ? tree_truthy(cond) : !tree_truthy(cond);
        if (do_jump) {
          if (!n.has_imm || n.imm >= tree.nodes.size()) {
            if (err) *err = "JMP target OOB";
            return JvmValue::make_void();
          }
          // Ignore degenerate targets that point at/before this JMP (rpkScan epilogue).
          if (!target_continue && n.imm <= ip) break;
          ip = n.imm - 1;
        } else if (target_continue) {
          // Falsy: skip continue block, find close() sugar (NEW+ARRAY+INVOKESPECIAL).
          for (size_t j = ip + 1; j < tree.nodes.size(); ++j) {
            if (tree.nodes[j].op != 0x27 || j + 2 >= tree.nodes.size()) continue;
            if (tree.nodes[j + 1].op != 0x24 || tree.nodes[j + 2].op != 0x11)
              continue;
            bool rel = false;
            for (size_t k = j + 3; k < tree.nodes.size() && k < j + 12; ++k) {
              if (tree.nodes[k].op == 0x14 && tree.nodes[k].has_imm &&
                  (tree.nodes[k].imm & 0x80000000u)) {
                rel = true;
                break;
              }
              if (tree.nodes[k].op == 0x2b) break;
            }
            if (!rel) {
              ip = j - 1;
              break;
            }
          }
        }
        break;
      }
      case 0x1b: {
        std::string fname = field_name(n.has_imm ? n.imm : 0);
        // FILD getstatic+NEWARRAY: Class.Field size (GameLogic.CLUBS).
        // Pool imm may be a sig/nat slot — fall back to CLUBS when in pattern.
        if (pending_static_newarray) {
          if (fname.empty() || fname[0] == '(' || fname.find('.') != std::string::npos) {
            fname = "CLUBS";
          }
          const int32_t vs = tree_static_vs(fname);
          if (vs >= 0) {
            push(JvmValue::make_int(vs));
            break;
          }
          if (fname == "CLUBS") {
            push(JvmValue::make_int(3));
            break;
          }
        }
        if (ip + 1 < tree.nodes.size()) {
          const TreeNode& next = tree.nodes[ip + 1];
          if (next.op == 0x28) {
            // Soft get (PE Object_getField miss → null): no create.
            InvObject* self = nullptr;
            if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
              self = pop().v.o;
            } else if (last_loaded_local >= 0 &&
                       static_cast<size_t>(last_loaded_local) < locals.size() &&
                       locals[static_cast<size_t>(last_loaded_local)].tag ==
                           JvmTag::Obj) {
              self = locals[static_cast<size_t>(last_loaded_local)].v.o;
            } else if (!locals.empty()) {
              self = locals[0].v.o;
            }
            soft_getfield(self, fname);
            ++ip;
            pending_binop.reset();
            break;
          }
          if (next.op == 0x08 && next.has_imm && next.imm == 35) {
            // After NEW+0x21, 0x1B is a type/field hint before <init> — skip it.
            if (ip > 0 && tree.nodes[ip - 1].op == 0x21) {
              if (!fname_is_garbage(fname)) pending_putfield = fname;
              break;
            }
            // PUTFIELD: prefer JVM [objectref, value] when under is `this`.
            InvObject* self = nullptr;
            JvmValue val = JvmValue::make_int(0);
            if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
                stack[stack.size() - 2].tag == JvmTag::Obj && !locals.empty() &&
                locals[0].tag == JvmTag::Obj &&
                stack[stack.size() - 2].v.o == locals[0].v.o) {
              val = pop();
              self = pop().v.o;
            } else if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                       (stack.size() < 2 ||
                        stack[stack.size() - 2].tag != JvmTag::Obj)) {
              // Single obj on stack = value; target is this.
              val = pop();
              self = locals.empty() ? nullptr : locals[0].v.o;
            } else if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
              // TREE order [value, object] with object on top.
              self = pop().v.o;
              val = pop();
            } else {
              val = pop();
              if (last_loaded_local >= 0 &&
                  static_cast<size_t>(last_loaded_local) < locals.size() &&
                  locals[static_cast<size_t>(last_loaded_local)].tag ==
                      JvmTag::Obj) {
                self = locals[static_cast<size_t>(last_loaded_local)].v.o;
              } else if (!locals.empty()) {
                self = locals[0].v.o;
              }
            }
            soft_putfield(self, fname, val);
            if (!fname_is_garbage(fname)) {
              if (fname == "played" && val.tag == JvmTag::Int)
                game_logic_set_played(val.v.i);
              if (fname == "gameMode" && val.tag == JvmTag::Int)
                game_logic_set_game_mode(val.v.i);
              if (fname == "timeout" && val.tag == JvmTag::Int)
                game_logic_set_timeout(val.v.i);
              if ((fname == "carrerInProgress" || fname == "careerInProgress") &&
                  val.tag == JvmTag::Int)
                game_logic_set_career_in_progress(val.v.i);
              if (fname == "racesetup" && val.tag == JvmTag::Obj)
                game_logic_set_racesetup(val.v.o);
            }
            ++ip;
            if (ip + 1 < tree.nodes.size() && tree.nodes[ip + 1].op == 0x29) ++ip;
            break;
          }
        }
        // Bare 0x1B: getstatic or getfield.
        const int32_t vs = tree_static_vs(fname);
        if (vs >= 0) {
          push(JvmValue::make_int(vs));
          break;
        }
        const int32_t cc = tree_static_rid_carcolor(fname);
        if (cc >= 0) {
          push(JvmValue::make_int(cc));
          break;
        }
        if (fname.rfind("qm_", 0) == 0) {
          push(JvmValue::make_float(tree_static_qm(fname)));
          break;
        }
        if (fname == "RID_GENERALBG" || fname == "RRT_HEADERBG" ||
            fname == "RRT_TEST" || fname == "RRT_NONE" ||
            fname == "RRT_DARKEN" || fname == "RRT_TOOLTIP") {
          // OptionsDialog / Osd ResourceRef statics (frontend locals).
          uint16_t local = 0;
          if (fname == "RID_GENERALBG") local = 0x0016;
          else if (fname == "RRT_HEADERBG") local = 0x0028;
          else if (fname == "RRT_TEST" || fname == "RRT_NONE") local = 0x0018;
          else if (fname == "RRT_DARKEN") local = 0x0019;
          else if (fname == "RRT_TOOLTIP") local = 0x001A;
          int32_t rid = 0;
          if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
            rid = rpak_make_id(fe->pack_id, local);
          else if (const RpakPack* fe = rpak_find_by_name("frontend"))
            rid = rpak_make_id(fe->pack_id, local);
          InvObject* pic = gameref_new();
          if (rid) java_util_resource_ResourceRef_set(pic, rid);
          push(JvmValue::make_obj(pic));
          break;
        }
        if (fname.rfind("RID_", 0) == 0) {
          // Other GameLogic.RID_* ints — unresolved → 0 is acceptable for now.
          push(JvmValue::make_int(0));
          break;
        }
        if (fname == "FILES_ONLY")
          push(JvmValue::make_int(1));
        else if (fname == "DIRS_ONLY")
          push(JvmValue::make_int(2));
        else if (fname == "FILES_DIRS")
          push(JvmValue::make_int(0));
        else if (fname == "EVENT_TIME")
          push(JvmValue::make_int(0x00000080));
        else if (fname == "EVENT_TRIGGER_ON")
          push(JvmValue::make_int(0x00000020));
        else if (fname == "EVENT_TRIGGER_OFF")
          push(JvmValue::make_int(0x00000040));
        else if (fname == "EVENT_COMMAND")
          push(JvmValue::make_int(0x00000010));
        else if (fname == "EVENT_SAME")
          push(JvmValue::make_int(0));
        else if (fname == "EVENT_COLLISION")
          push(JvmValue::make_int(0x00000001));
        else if (fname == "EVENT_ANY")
          push(JvmValue::make_int(0x0FFFFFFF));
        else if (fname == "EVENT_CURSOR")
          push(JvmValue::make_int(0x00010000));
        else if (fname == "EVENT_HOTKEY")
          push(JvmValue::make_int(0x00000002));
        else if (fname == "AXIS_CANCEL")
          push(JvmValue::make_int(35));
        else if (fname == "VIRTUAL")
          push(JvmValue::make_int(1));
        else if (fname == "HK_STATIC")
          push(JvmValue::make_int(0x04));
        else if (fname == "loadingScreen") {
          // Frontend.loadingScreen (static) — no Frontend.class in install.
          push(JvmValue::make_obj(frontend_loading_screen()));
        }
        else if (fname == "render") {
          // Frontend.render.wait() only — never hijack player.render getfield.
          bool next_wait = false;
          if (ip + 1 < tree.nodes.size()) {
            const TreeNode& peek = tree.nodes[ip + 1];
            if (peek.has_imm &&
                (peek.op == 0x10 || peek.op == 0x11 || peek.op == 0x12 ||
                 peek.op == 0x1a) &&
                peek.imm < const_mref_name.size() &&
                const_mref_name[peek.imm] == "wait") {
              next_wait = true;
            }
          }
          if (next_wait) {
            push(JvmValue::make_obj(frontend_gfx_engine()));
            break;
          }
          // Fall through to instance getfield (player.render, …).
          InvObject* self = nullptr;
          if (!stack.empty() && stack.back().tag == JvmTag::Obj)
            self = pop().v.o;
          else if (!locals.empty())
            self = locals[0].v.o;
          soft_getfield(self, fname);
        }
        else if (fname == "player") {
          push(JvmValue::make_obj(game_logic_player()));
        }
        else if (fname == "garage") {
          push(JvmValue::make_obj(game_logic_garage()));
        }
        else if (fname == "racesetup") {
          push(JvmValue::make_obj(game_logic_racesetup()));
        }
        else if (fname == "CLUBS") {
          push(JvmValue::make_int(3));
        }
        else if (fname == "cursor") {
          // Input.cursor static MouseCursor
          push(JvmValue::make_obj(java_io_Input_cursor()));
        }
        else if (fname == "MUSIC_SET_GARAGE") {
          push(JvmValue::make_int(0));
        }
        else if (fname == "MUSIC_SET_MENU") {
          push(JvmValue::make_int(1));
        }
        else if (fname == "IL_NONE") {
          push(JvmValue::make_int(0));
        }
        else if (fname == "OF_MODAL") {
          push(JvmValue::make_int(0x01));
        }
        else if (fname == "ALIGN_CENTER") {
          push(JvmValue::make_int(1));
        }
        else if (fname == "ALIGN_LEFT") {
          // Text.java: ALIGN_LEFT=2, ALIGN_RIGHT=0, ALIGN_CENTER=1
          push(JvmValue::make_int(2));
        }
        else if (fname == "ALIGN_RIGHT") {
          push(JvmValue::make_int(0));
        }
        else if (fname == "DF_MODAL") {
          push(JvmValue::make_int(0x00000001));
        }
        else if (fname == "DF_DEFAULTBG") {
          push(JvmValue::make_int(0x00000004));
        }
        else if (fname == "SIF_NOEMPTY") {
          push(JvmValue::make_int(0x00001000));
        }
        else if (fname == "DF_DARKEN") {
          push(JvmValue::make_int(0x00000100));
        }
        else if (fname == "DF_SILENT") {
          push(JvmValue::make_int(0x00000200));
        }
        else if (fname == "DF_MODAL") {
          push(JvmValue::make_int(0x00000001));
        }
        else if (fname == "DF_FULLSCREEN") {
          push(JvmValue::make_int(0x00000002));
        }
        else if (fname == "DF_DEFAULTBG") {
          push(JvmValue::make_int(0x00000004));
        }
        else if (fname == "DF_FREEZE") {
          push(JvmValue::make_int(0x00000008));
        }
        else if (fname == "DF_LEAVEPOINTER") {
          push(JvmValue::make_int(0x00000010));
        }
        else if (fname == "DF_LOWPRI") {
          push(JvmValue::make_int(0x00000020));
        }
        else if (fname == "DF_HIGHPRI") {
          push(JvmValue::make_int(0x00000040));
        }
        else if (fname == "CHANNEL_EFFECTS") {
          push(JvmValue::make_int(0));
        }
        else if (fname == "CHANNEL_MUSIC") {
          push(JvmValue::make_int(1));
        }
        else if (fname == "CHANNEL_ENGINE") {
          push(JvmValue::make_int(2));
        }
        else if (fname == "RID_SLD_BACK") {
          int32_t rid = 0;
          if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
            rid = rpak_make_id(fe->pack_id, 0x0089);
          else if (const RpakPack* fe = rpak_find_by_name("frontend"))
            rid = rpak_make_id(fe->pack_id, 0x0089);
          push(JvmValue::make_int(rid));
        }
        else if (fname == "RID_SLD_KNOB") {
          int32_t rid = 0;
          if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
            rid = rpak_make_id(fe->pack_id, 0x0091);
          else if (const RpakPack* fe = rpak_find_by_name("frontend"))
            rid = rpak_make_id(fe->pack_id, 0x0091);
          push(JvmValue::make_int(rid));
        }
        else if (fname == "HIGH") {
          push(JvmValue::make_int(2));
        }
        else if (fname == "MID") {
          push(JvmValue::make_int(1));
        }
        else if (fname == "LOW") {
          push(JvmValue::make_int(0));
        }
        else if (fname == "OFF") {
          push(JvmValue::make_int(-1));
        }
        else if (fname == "texture_size_high") {
          push(JvmValue::make_int(0));
        }
        else if (fname == "texture_size_mid") {
          push(JvmValue::make_int(2));
        }
        else if (fname == "texture_size_low") {
          push(JvmValue::make_int(3));
        }
        else if (fname == "shadow_detail_high") {
          push(JvmValue::make_float(0.1f));
        }
        else if (fname == "shadow_detail_mid") {
          push(JvmValue::make_float(0.25f));
        }
        else if (fname == "shadow_detail_low") {
          push(JvmValue::make_float(0.5f));
        }
        else if (fname == "shadow_detail_off") {
          push(JvmValue::make_float(10.f));
        }
        else if (fname == "NCONTROLS") {
          push(JvmValue::make_int(25));
        }
        else if (fname == "largeFont") {
          push(JvmValue::make_obj(frontend_large_font()));
        }
        else if (fname == "mediumFont") {
          push(JvmValue::make_obj(frontend_medium_font()));
        }
        else if (fname == "smallFont") {
          push(JvmValue::make_obj(frontend_small_font()));
        }
        else if (fname == "pointers") {
          push(JvmValue::make_obj(frontend_pointers()));
        }
        else if (fname == "defLoadingPic") {
          push(JvmValue::make_obj(frontend_def_loading_pic()));
        }
        else if (fname == "inputQueue") {
          push(JvmValue::make_obj(frontend_input_queue()));
        }
        else if (fname == "RRT_DARKEN" || fname == "RRT_TEST" ||
                 fname == "RRT_NONE" || fname == "RRT_EMPTY" ||
                 fname == "RRT_HEADERBG" || fname == "RRT_TOOLTIP") {
          push(JvmValue::make_obj(
              tree_host_new("java.util.resource.ResourceRef")));
        }
        else if (fname == "RID_OK" || fname == "RID_CANCEL" ||
                 fname == "RID_BACK" || fname == "RID_EXIT" ||
                 fname == "RID_ARROWLF" || fname == "RID_ARROWRG" ||
                 fname == "RID_ARROWUP" || fname == "RID_ARROWDN" ||
                 fname == "RID_SLD_BACK" || fname == "RID_SLD_KNOB" ||
                 fname == "RID_GHOSTBUTTON") {
          // Osd.java: RID_* are frontend packed ints, not ResourceRef.
          int32_t local = 0;
          if (fname == "RID_OK") local = 0x0010;
          else if (fname == "RID_CANCEL") local = 0x001D;
          else if (fname == "RID_BACK") local = 0x0126;
          else if (fname == "RID_EXIT") local = 0x0135;
          else if (fname == "RID_ARROWUP") local = 0x004C;
          else if (fname == "RID_ARROWDN") local = 0x0084;
          else if (fname == "RID_ARROWLF") local = 0x0085;
          else if (fname == "RID_ARROWRG") local = 0x0088;
          else if (fname == "RID_SLD_BACK") local = 0x0089;
          else if (fname == "RID_SLD_KNOB") local = 0x0091;
          else if (fname == "RID_GHOSTBUTTON") local = 0x0038;
          int32_t rid = local;
          if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
            rid = rpak_make_id(fe->pack_id, local & 0xFFFF);
          else if (const RpakPack* fe = rpak_find_by_name("frontend"))
            rid = rpak_make_id(fe->pack_id, local & 0xFFFF);
          push(JvmValue::make_int(rid));
        }
        else if (fname == "MD_HORIZONTAL") {
          push(JvmValue::make_int(1));
        }
        else if (fname == "MD_VERTICAL") {
          push(JvmValue::make_int(0));
        }
        else if (fname == "AXIS_SELECT" || fname == "AXIS_MENU_LEFT" ||
                 fname == "AXIS_MENU_RIGHT" || fname == "AXIS_MENU_UP" ||
                 fname == "AXIS_MENU_DOWN") {
          push(JvmValue::make_int(1));
        }
        else if (fname == "MAX_PRIORITY") {
          push(JvmValue::make_int(10));
        }
        else if (fname == "RID_DIALOGBG") {
          int32_t rid = 0;
          if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
            rid = rpak_make_id(fe->pack_id, 0x000E);
          else if (const RpakPack* fe = rpak_find_by_name("frontend"))
            rid = rpak_make_id(fe->pack_id, 0x000E);
          push(JvmValue::make_int(rid));
        }
        else if (fname.rfind("CMD_", 0) == 0) {
          // Static final command ids (OptionsDialog / MainMenuDialog). Must not
          // fall through to getfield — that pops the String label for addItem.
          static const std::unordered_map<std::string, int32_t> kCmd = {
              // OptionsDialog
              {"CMD_OPTIONS", 0},
              {"CMD_VIDEO_OPTIONS", 1},
              {"CMD_CONTROL_OPTIONS", 2},
              {"CMD_SOUND_OPTIONS", 3},
              {"CMD_REDEFINE_CONTROLS", 4},
              {"CMD_MAIN", 5},
              {"CMD_REDEFINE_DONE", 6},
              {"CMD_RESET_CONTROLS", 7},
              {"CMD_LOAD_CONTROLS", 8},
              {"CMD_SAVE_CONTROLS", 9},
              {"CMD_RESOLUTION", 10},
              {"CMD_VIDEO_OPTIONS_DONE", 11},
              {"CMD_TEXTURE_DETAIL", 12},
              {"CMD_SHADOW_DETAIL", 13},
              {"CMD_VIEW_RANGE", 14},
              {"CMD_EFFECTS_VOL", 15},
              {"CMD_MUSIC_VOL", 16},
              {"CMD_ENGINE_VOL", 17},
              {"CMD_GAME_OPTIONS", 18},
              {"CMD_DIFFICULTY", 19},
              {"CMD_TRAFFICDENSITY", 20},
              {"CMD_PEDESTRIANDENSITY", 21},
              {"CMD_CLUTCH", 22},
              {"CMD_STEERHELP", 23},
              {"CMD_GAME_OPTIONS_DONE", 24},
              {"CMD_MOUSE_SENS", 25},
              {"CMD_REDEFINE_CONTROLS2", 26},
              {"CMD_REDEFINE_CONTROLS3", 27},
              {"CMD_METRIC", 28},
              {"CMD_GAMMA", 29},
              {"CMD_OBJECT_DETAIL", 30},
              {"CMD_LOD_DETAIL", 31},
              {"CMD_ABS_SLIDER", 32},
              {"CMD_ASR_SLIDER", 33},
              {"CMD_GPSMODE", 34},
              {"CMD_AXISCHECK", 35},
              {"CMD_AXISCHECK_DONE", 36},
              {"CMD_UNUSED", 37},
              {"CMD_SOUND_3D", 38},
              {"CMD_SOUND_HW", 39},
              {"CMD_GAME_OPTIONS2", 40},
              {"CMD_HMF_1", 41},
              {"CMD_HMF_2", 42},
              {"CMD_HMF_3", 43},
              {"CMD_PARTICLE", 44},
              {"CMD_HEADLIGHTS", 45},
              {"CMD_FLARES", 46},
              {"CMD_GETKEY", 100},
              {"CMD_DEAD_ZONE", 200},
              {"CMD_POWER", 300},
              {"CMD_FFB", 400},
              // MainMenuDialog
              {"CMD_NEW", 50},
              {"CMD_EXIT", 51},
              {"CMD_LOAD", 52},
              {"CMD_BACKTOGARAGE", 53},
              {"CMD_DELETE", 54},
              {"CMD_QUICKRACE", 55},
              {"CMD_CREDITS", 56},
              {"CMD_CREDITS_DONE", 57},
              {"CMD_COMPILEFILES", 58},
              {"CMD_DEMO", 59},
              {"CMD_LOADCAR", 60},
              {"CMD_SAVECAR", 61},
              {"CMD_FREERIDE", 62},
              {"CMD_NEXT", 100},
              {"CMD_PREV", 101},
              {"CMD_HNEXT", 102},
              {"CMD_HPREV", 103},
              // Garage
              {"CMD_NONE", 100},
              {"CMD_MAINMENU", 101},
              {"CMD_MENU", 107},
              {"CMD_ROC", 108},
              {"CMD_HITTHESTREET", 109},
              {"CMD_TESTTRACK", 110},
              {"CMD_CARLOT", 111},
              {"CMD_BUYCARS", 112},
              {"CMD_CATALOG", 113},
              {"CMD_CLUBINFO", 114},
              {"CMD_CARINFO", 115},
              {"CMD_TIME", 116},
              {"CMD_MECHANIC", 117},
              {"CMD_PAINT", 118},
              {"CMD_ESCAPE", 119},
              {"CMD_ROCRACE", 120},
              {"CMD_ROCTEST", 121},
              {"CMD_BUYCARSUSED", 122},
              {"CMD_TEST", 123},
              {"CMD_TUNE", 124},
              {"CMD_ROCINFO", 125},
              {"CMD_ROCQUIT", 126},
              {"CMD_CHEATMONEY", 127},
              {"CMD_BEGIN_ROC", 128},
              // RaceSetup.createOSDObjects
              {"CMD_RACE", 0},
              {"CMD_ABANDON", 1},
              {"CMD_ZOOM_IN", 2},
              {"CMD_ZOOM_OUT", 3},
              {"CMD_LESS_MONEY", 4},
              {"CMD_MORE_MONEY", 5},
              {"CMD_PRIZE", 6},
          };
          auto it = kCmd.find(fname);
          push(JvmValue::make_int(it != kCmd.end() ? it->second : 0));
        }
        else if (fname.rfind("SFX_", 0) == 0 || fname == "RID_CAMERA") {
          push(JvmValue::make_int(0));
        }
        else if (fname == "MENUSET") {
          push(JvmValue::make_int(2));
        }
        else if (fname == "played") {
          push(JvmValue::make_int(game_logic_played()));
        }
        else if (fname == "speedymen" || fname == "CARCOLORS") {
          static InvObject* g_empty_vec = nullptr;
          if (!g_empty_vec) g_empty_vec = tree_vector_new();
          push(JvmValue::make_obj(g_empty_vec));
        }
        else if (fname == "GM_CARREER" || fname == "GM_CAREER") {
          push(JvmValue::make_int(1));
        }
        else if (fname == "GM_DEMO") {
          push(JvmValue::make_int(5));
        }
        else if (fname == "GM_FREERIDE") {
          // Stock GameLogic.java: FREERIDE=2, QUICKRACE=3
          push(JvmValue::make_int(2));
        }
        else if (fname == "GM_QUICKRACE") {
          push(JvmValue::make_int(3));
        }
        else if (fname == "GM_SINGLECAR") {
          push(JvmValue::make_int(4));
        }
        else if (fname == "gameMode") {
          push(JvmValue::make_int(game_logic_game_mode()));
        }
        else if (fname == "timeout") {
          push(JvmValue::make_int(game_logic_timeout()));
        }
        else if (fname == "carrerInProgress" || fname == "careerInProgress") {
          push(JvmValue::make_int(game_logic_career_in_progress()));
        }
        else if (fname == "texture_size" || fname == "shadow_size" ||
                 fname == "shadows" || fname == "shadow_detail" ||
                 fname == "video_windowed" || fname == "video_x" ||
                 fname == "video_y" || fname == "video_depth" ||
                 fname == "video_gamma" || fname == "flares" ||
                 fname == "headlight_rays" || fname == "object_detail" ||
                 fname == "object_detail_amp" || fname == "particle_density" ||
                 fname == "camera_ext_viewrange" ||
                 fname == "camera_int_viewrange" ||
                 fname == "trafficDensity" || fname == "pedestrianDensity" ||
                 fname == "mouseSensitivity" || fname == "FFB_strength" ||
                 fname == "FFB_strength_emulated" || fname == "metricSystem" ||
                 fname == "gpsMode" || fname == "player_transmission" ||
                 fname == "player_steeringhelp" || fname == "player_abs" ||
                 fname == "player_asr" || fname == "deformation" ||
                 fname == "internal_damage" ||
                 fname == "player_damage_multiplier" ||
                 fname == "head_move_steer" || fname == "head_move_vel" ||
                 fname == "head_move_acc" || fname == "Sound_Mix_HW" ||
                 fname == "Sound_3D_HW" || fname == "version") {
          // Config.* getstatic → host Config mirror (OptionsDialog.show).
          // Soft get ≡ PE getField miss → 0; never create Config slots here.
          InvObject* cfg = system_config_host();
          if (JvmValue* slot = tree_field_slot(cfg, fname, /*create=*/false))
            push(*slot);
          else
            push(JvmValue::make_int(0));
        }
        else {
          InvObject* self = nullptr;
          const bool field_invoke =
              ip + 1 < tree.nodes.size() && tree.nodes[ip + 1].op == 0x1a &&
              tree.nodes[ip + 1].has_imm &&
              tree.nodes[ip + 1].imm < const_mref_name.size() &&
              (const_mref_name[tree.nodes[ip + 1].imm] == "addElement" ||
               const_mref_name[tree.nodes[ip + 1].imm] == "elementAt" ||
               const_mref_name[tree.nodes[ip + 1].imm] == "size");
          if (field_invoke) {
            // vtdarr.addElement(x): keep x on stack, field is on this.
            if (!locals.empty()) self = locals[0].v.o;
          } else if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
            self = pop().v.o;
          } else if (!locals.empty()) {
            self = locals[0].v.o;
          }
          soft_getfield(self, fname);
        }
        break;
      }
      case 0x1c: {
        // GETFIELD (quick) — or PUTFIELD when followed by 0x08/35
        // (Vector: elementData = new Object[n]). Soft ≡ PE field get/put.
        std::string fname = field_name(n.has_imm ? n.imm : 0);
        if (ip + 1 < tree.nodes.size() && tree.nodes[ip + 1].op == 0x08 &&
            tree.nodes[ip + 1].has_imm && tree.nodes[ip + 1].imm == 35) {
          InvObject* self = nullptr;
          JvmValue val = JvmValue::make_int(0);
          if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj) {
            // Common TREE order [value, object] with object on top.
            self = pop().v.o;
            val = pop();
          } else if (!stack.empty()) {
            val = pop();
            self = (!locals.empty() && locals[0].tag == JvmTag::Obj)
                       ? locals[0].v.o
                       : nullptr;
          }
          soft_putfield(self, fname, val);
          ++ip;
          break;
        }
        InvObject* self = nullptr;
        if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
          self = pop().v.o;
        } else if (!locals.empty()) {
          self = locals[0].v.o;
        }
        soft_getfield(self, fname);
        break;
      }
      case 0x21: {  // store last_new into pending_local
        if (pending_local >= 0 && last_new) {
          ensure_local(static_cast<size_t>(pending_local));
          locals[static_cast<size_t>(pending_local)] =
              JvmValue::make_obj(last_new);
          recent_new = last_new;
          // NEW+store+hint+<init>: leave instance on stack for INVOKESPECIAL.
          if (ip + 2 < tree.nodes.size() && tree.nodes[ip + 1].op == 0x1b &&
              tree.nodes[ip + 2].op == 0x08 && tree.nodes[ip + 2].has_imm &&
              tree.nodes[ip + 2].imm == 35) {
            push(JvmValue::make_obj(last_new));
          }
          // Garage icon ResourceRef: apply packed rid without disturbing tip/CMD.
          else if (!pending_class.empty() &&
                   pending_class.find("ResourceRef") != std::string::npos &&
                   !stack.empty() && stack.back().tag == JvmTag::Int) {
            const int32_t rid = stack.back().v.i;
            if (rid > 0x10000) {
              pop();
              java_util_resource_ResourceRef_set(last_new, rid);
            }
          }
          if (pending_class == "java.io.FindFile" || ff_local < 0)
            ff_local = pending_local;
          pending_local = -1;
          last_new = nullptr;
        }
        break;
      }
      case 0x22:  // this(...) — other <init> overload
        pending_special_init = 1;
        break;
      case 0x23:  // super(...)
        pending_special_init = 2;
        break;
      case 0x24:  // ARRAY_INIT — part of concat/new sugar
        break;
      case 0x25:  // INVOKESPECIAL / instance call by name (SfxRef.precache)
      case 0x26: {  // INVOKESPECIAL super.method (TextDialog → Dialog.show)
        // Phase 2.134: TextDialog.show encodes `super.show()` as 0x26+name;
        // without this, Dialog.show (createButton) never runs.
        std::string mname;
        if (n.has_imm) {
          mname = field_name(n.imm);
          if (mname.empty()) mname = cstr(n.imm);
        }
        if (mname.empty() || mname.find('.') != std::string::npos) break;

        std::string owner = cls_name;
        if (n.op == 0x26 && !super_name.empty()) owner = super_name;

        // Drop NEW packing sugar left for the call (TextDialog: NEW then 0x26).
        // queueEvent(null, EVENT_COMMAND, String): String often has empty
        // host-class — do not drop it (or aconst_null ro) as packing Object.
        if (mname != "queueEvent" && mname != "create" && !stack.empty() &&
            stack.back().tag == JvmTag::Obj && !locals.empty() &&
            locals[0].tag == JvmTag::Obj &&
            stack.back().v.o != locals[0].v.o) {
          const char* phc = tree_host_class(stack.back().v.o);
          // Keep a real Osd/SfxRef recv on top; drop anonymous packing Object.
          if (!phc || !phc[0] || std::strcmp(phc, "java.lang.Object") == 0)
            pop();
        }

        int argc = 0;
        if (mname == "precache" || mname == "play" || mname == "show" ||
            mname == "hide" || mname == "reset" || mname == "nofocus" ||
            mname == "display" || mname == "loopPlay" || mname == "pause")
          argc = 0;
        else if (mname == "enable" || mname == "activateState" ||
                 mname == "enter" || mname == "exit" || mname == "setSpeed" ||
                 mname == "setFade" || mname == "seek")
          argc = 1;
        else if (mname == "setMatrix")
          argc = 2;
        else if (mname == "addTraffic" || mname == "addTrafficN")
          argc = 5;
        else if (mname == "queueEvent")
          argc = 3;
        else if (mname == "create") {
          if ((!stack.empty() && stack.back().tag == JvmTag::Obj &&
               tree_is_renderref(stack.back().v.o)) ||
              (!locals.empty() && locals[0].tag == JvmTag::Obj &&
               tree_is_renderref(locals[0].v.o)) ||
              pending_class.find("RenderRef") != std::string::npos)
            argc = 3;
        }
        else if (mname == "getNearestCross") {
          if (stack.size() >= 2 &&
              (stack.back().tag == JvmTag::Float ||
               stack.back().tag == JvmTag::Int) &&
              stack[stack.size() - 2].tag == JvmTag::Obj)
            argc = 2;
          else
            argc = 1;
        } else if (mname == "alignToRoad" || mname == "time2Config")
          argc = 1;
        else if (mname == "getStartDirection" || mname == "findRoute" ||
                   mname == "haltTrafficCross" || mname == "haltTrafficPath")
          argc = 2;
        else if (mname == "startRace")
          argc = 3;
        else if (mname == "startRace2")
          argc = 0;
        else if (mname == "<init>") {
          // Trigger(map,null,pos,alias) / GameRef(parent,rid,params,alias).
          // this() inside Trigger.<init> has empty pending_class — use cls_name.
          const bool trig =
              pending_class.find("Trigger") != std::string::npos ||
              cls_name.find("Trigger") != std::string::npos;
          const bool rref =
              (pending_class.find("RenderRef") != std::string::npos ||
               cls_name.find("RenderRef") != std::string::npos) &&
              pending_class.find("Camera") == std::string::npos &&
              cls_name.find("Camera") == std::string::npos;
          const bool gref =
              pending_class.find("GameRef") != std::string::npos ||
              cls_name.find("GameRef") != std::string::npos;
          const bool pcar =
              pending_class.find("ParkingCar") != std::string::npos ||
              cls_name.find("ParkingCar") != std::string::npos;
          if (trig) {
            argc = 4;
            for (size_t i = 0; i < stack.size() && i < 6; ++i) {
              if (stack[stack.size() - 1 - i].tag == JvmTag::Float) {
                argc = 5;
                break;
              }
            }
          } else if (pcar) {
            argc = 5;
          } else if (rref) {
            argc = 3;
            if (stack.size() < 3) argc = 1;
          } else if (gref) {
            argc = 4;
            if (stack.size() < 4) argc = 1;
          }
        }
        else if (mname == "mul" || mname == "add" || mname == "sub" ||
                 mname == "setParent")
          argc = 1;
        else if (mname == "createCar") {
          if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
              stack[stack.size() - 2].tag == JvmTag::Obj)
            argc = 2;
          else
            argc = 1;
        }
        else if (mname == "getRouteLength") {
          if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
              stack[stack.size() - 2].tag == JvmTag::Obj)
            argc = 2;
          else
            argc = 0;
        } else if (mname == "addTrafficCar" || mname == "notifyTrafficCar" || mname == "setTrafficCarBehaviour")
          argc = 2;
        else if (mname == "addMarker") {
          if (stack.size() >= 3 && stack.back().tag == JvmTag::Int &&
              stack[stack.size() - 2].tag == JvmTag::Obj &&
              stack[stack.size() - 3].tag == JvmTag::Obj)
            argc = 3;
          else if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
                   stack[stack.size() - 2].tag == JvmTag::Obj)
            argc = 2;
          else
            argc = 1;
        } else if (mname == "remMarker" || mname == "updateNavigator")
          argc = 1;
        else if (mname == "plotRoute")
          argc = 5;
        else if (mname == "timeWarp")
          argc = 1;
        else if (mname == "changeActiveSection")
          argc = 1;

        JvmValue recv = JvmValue::make_obj(nullptr);
        if (n.op == 0x26) {
          recv = !locals.empty() ? locals[0] : JvmValue::make_obj(nullptr);
        } else if (mname == "queueEvent") {
          // Mixed (null, I, String) — tree_pack_queue_event pops recv itself.
        } else if (mname == "create") {
          // Mixed (parent, type, alias) — tree_pack_renderref_create pops recv.
        } else if (mname == "add" || mname == "mul" || mname == "sub") {
          // Vector3.add — tree_pack_vector3_binop pops recv (skip ResourceRef junk).
        } else if (mname == "enter" || mname == "exit" || mname == "startRace" ||
                   mname == "startRace2") {
          // GameState/City self — PE Object_callMethod name-only; do not take
          // prev_state / Vector3 as recv (Valocity.enter → startRace / exit).
          recv = !locals.empty() ? locals[0] : JvmValue::make_obj(nullptr);
          if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
              recv.tag == JvmTag::Obj && stack.back().v.o == recv.v.o)
            pop();
        } else if (mname == "<init>" && n.op == 0x25 &&
                   cls_name.find("Trigger") != std::string::npos &&
                   pending_class.find("GameRef") == std::string::npos &&
                   pending_class.find("RenderRef") == std::string::npos) {
          // this(parent,type,pos[,r],alias) — String alias is on top, not recv.
          recv = !locals.empty() ? locals[0] : JvmValue::make_obj(nullptr);
          if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
              recv.tag == JvmTag::Obj && stack.back().v.o == recv.v.o)
            pop();
        } else if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
          recv = pop();
        } else if (recent_new) {
          recv = JvmValue::make_obj(recent_new);
        } else if (last_new) {
          recv = JvmValue::make_obj(last_new);
        } else if (!locals.empty()) {
          recv = locals[0];
        }

        // RaceSetup.enter: map/nav/line calls often leave RaceSetup as recv.
        if (recv.tag == JvmTag::Obj && recv.v.o) {
          const char* rhc = tree_host_class(recv.v.o);
          if (rhc && std::strstr(rhc, "RaceSetup")) {
            InvObject* track = tree_field_get_obj(recv.v.o, "track");
            if (!track) track = tree_field_get_obj(recv.v.o, "lastState");
            if (mname == "getNearestCross" || mname == "getStartDirection" ||
                mname == "getRouteLength" || mname == "findRoute" ||
                mname == "haltTrafficCross" || mname == "haltTrafficPath" ||
                mname == "alignToRoad") {
              InvObject* map =
                  track ? tree_field_get_obj(track, "map") : nullptr;
              if (map) {
                recv = JvmValue::make_obj(map);
                owner = "java.util.resource.GroundRef";
              }
            } else if (mname == "startRace") {
              if (track) {
                recv = JvmValue::make_obj(track);
                owner = tree_host_class(track) ? tree_host_class(track)
                                               : "java.game.City";
              }
            } else if (mname == "addMarker" || mname == "remMarker" ||
                       mname == "updateNavigator" || mname == "changeMode" ||
                       mname == "changeSize" || mname == "changeZoom") {
              InvObject* nav =
                  track ? tree_field_get_obj(track, "nav") : nullptr;
              if (nav) {
                recv = JvmValue::make_obj(nav);
                owner = "java.game.Navigator";
              }
            } else if (mname == "plotRoute") {
              InvObject* nav =
                  track ? tree_field_get_obj(track, "nav") : nullptr;
              InvObject* line = nav ? tree_field_get_obj(nav, "route") : nullptr;
              if (!line && recent_new) line = recent_new;
              if (!line && last_new) line = last_new;
              if (line) {
                recv = JvmValue::make_obj(line);
                owner = "java.util.resource.RenderRef";
              }
            }
          }
        }

        std::vector<JvmValue> args;
        if (mname == "queueEvent") {
          tree_pack_queue_event(stack, locals, args, &recv);
        } else if (mname == "create" && argc == 3) {
          tree_pack_renderref_create(stack, locals, args, &recv);
        } else if (mname == "add" || mname == "mul" || mname == "sub") {
          tree_pack_vector3_binop(stack, locals, args, &recv);
        } else {
          args.push_back(recv);
          for (int i = 0; i < argc; ++i) {
            if (!stack.empty())
              args.push_back(pop());
            else
              args.push_back(JvmValue::make_int(0));
          }
        }
        const char* hc = tree_host_class(recv.v.o);
        if (n.op != 0x26 && hc && hc[0] &&
            owner.find("RaceSetup") == std::string::npos &&
            owner != "java.util.resource.GroundRef" &&
            owner != "java.game.Navigator" &&
            owner != "java.util.resource.RenderRef")
          owner = hc;
        if ((mname == "add" || mname == "mul" || mname == "sub") &&
            recv.tag == JvmTag::Obj && tree_is_vector3(recv.v.o))
          owner = "java.lang.Vector3";

        if (mname == "changeActiveSection") {
          JvmValue next = JvmValue::make_obj(nullptr);
          for (auto it = args.rbegin(); it != args.rend(); ++it) {
            if (it->tag == JvmTag::Obj && game_logic_is_section(it->v.o)) {
              next = *it;
              break;
            }
          }
          JvmValue r = soft_call_by_name("java.game.GameLogic",
                                         "changeActiveSection", {next},
                                         /*is_static=*/true);
          soft_invoke_commit("changeActiveSection", r, /*keep_extra=*/false,
                             /*always_nonvoid=*/true);
          break;
        }

        // Soft ≡ PE Object_callMethod @ 0x408A30 (0x25 mref invoke).
        JvmValue r =
            soft_call_by_name(owner.c_str(), mname, args, /*is_static=*/false);
        // PlayerSetupDialog.display is 0x25 — discard return (statement).
        soft_invoke_commit(mname, r, /*keep_extra=*/false,
                           /*always_nonvoid=*/mname != "display");
        break;
      }
      case 0x27: {  // NEW
        if (pending_concat) break;  // concat path uses NEW as sugar
        const char* cn =
            pending_class.empty() ? "java.lang.Object" : pending_class.c_str();
        last_new = host->new_instance(cn);
        if (pending_local >= 0) {
          ensure_local(static_cast<size_t>(pending_local));
          locals[static_cast<size_t>(pending_local)] =
              JvmValue::make_obj(last_new);
          if (pending_class == "java.io.FindFile") ff_local = pending_local;
        } else {
          push(JvmValue::make_obj(last_new));
        }
        break;
      }
      case 0x28: {
        if (pending_binop && *pending_binop == 7) {
          JvmValue b = pop();
          JvmValue a = pop();
          int eq = 0;
          if (a.tag == JvmTag::Obj || b.tag == JvmTag::Obj)
            eq = (a.v.o == b.v.o) ? 1 : 0;
          else if (a.tag == JvmTag::Float || b.tag == JvmTag::Float)
            eq = (a.v.f == b.v.f) ? 1 : 0;
          else
            eq = (a.v.i == b.v.i) ? 1 : 0;
          push(JvmValue::make_int(eq));
          pending_binop.reset();
        }
        break;
      }
      case 0x29:
        pop();
        break;
      case 0x20: {
        // AALOAD — array[index]; or AASTORE setup when followed by 0x08/35
        // (Valocity: posGarage[i] = new Vector3(...); Vector: elementData[i]=null).
        if (ip + 1 < tree.nodes.size() && tree.nodes[ip + 1].op == 0x08 &&
            tree.nodes[ip + 1].has_imm && tree.nodes[ip + 1].imm == 35) {
          JvmValue arrv = pop();
          JvmValue idxv = pop();
          pending_aastore_arr =
              (arrv.tag == JvmTag::Obj) ? arrv.v.o : nullptr;
          pending_aastore_idx =
              (idxv.tag == JvmTag::Int)
                  ? idxv.v.i
                  : (idxv.tag == JvmTag::Float ? static_cast<int32_t>(idxv.v.f)
                                               : 0);
          pending_aastore = true;
          break;
        }
        JvmValue idxv = pop();
        JvmValue arrv = pop();
        const int32_t idx = idxv.tag == JvmTag::Int ? idxv.v.i : 0;
        InvObject* elem = nullptr;
        if (arrv.tag == JvmTag::Obj && arrv.v.o)
          elem = tree_vector_element_at(arrv.v.o, idx);
        push(JvmValue::make_obj(elem));
        break;
      }
      case 0x2c:
        // TYPE-declare marker (File.delete / for-loop / checkcast sugar).
        // Value is stored later via 0x2d or NEW+0x21; keep pending_local.
        break;
      case 0x2b: {
        // Soft return ≡ PE VMThread_popFrameRestorePc @ 0x41F7B0 / op DONE
        // sibling: empty operand stack → void (no abort). Soft pending TREE
        // methods often end without an explicit value on void paths.
        if (stack.empty()) return JvmValue::make_void();
        return stack.back();
      }
      case 0x2d:  // DUP2 — also commits stack top into pending_local
        if (pending_local >= 0 && !stack.empty()) {
          ensure_local(static_cast<size_t>(pending_local));
          locals[static_cast<size_t>(pending_local)] = stack.back();
          if (name_local < 0) name_local = pending_local;
          pending_local = -1;
        }
        break;
      case 0x2f: {  // IADD
        JvmValue b = pop();
        JvmValue a = pop();
        const int32_t sum = a.v.i + b.v.i;
        // for (i=0; i<N; i++) often compiles as i = i + 1 without a clean
        // istore — write back when +1 matches the last-loaded local.
        if (last_loaded_local >= 0 &&
            ((b.tag == JvmTag::Int && b.v.i == 1) ||
             (a.tag == JvmTag::Int && a.v.i == 1))) {
          ensure_local(static_cast<size_t>(last_loaded_local));
          JvmValue& loc = locals[static_cast<size_t>(last_loaded_local)];
          const int32_t base = (b.tag == JvmTag::Int && b.v.i == 1) ? a.v.i
                                                                   : b.v.i;
          if (loc.tag == JvmTag::Int && loc.v.i == base) {
            loc.v.i = sum;
          }
        }
        push(JvmValue::make_int(sum));
        break;
      }
      case 0x30: {  // FADD
        JvmValue b = pop();
        JvmValue a = pop();
        float af = a.tag == JvmTag::Float ? a.v.f : static_cast<float>(a.v.i);
        float bf = b.tag == JvmTag::Float ? b.v.f : static_cast<float>(b.v.i);
        const float sum = af + bf;
        // OptionsDialog keyText loop: y += 0.12
        if (last_loaded_local >= 0) {
          ensure_local(static_cast<size_t>(last_loaded_local));
          JvmValue& loc = locals[static_cast<size_t>(last_loaded_local)];
          if (loc.tag == JvmTag::Float &&
              (std::fabs(loc.v.f - af) < 1e-6f ||
               std::fabs(loc.v.f - bf) < 1e-6f)) {
            loc.v.f = sum;
          }
        }
        push(JvmValue::make_float(sum));
        break;
      }
      case 0x36: {  // IOR (guess — vehicleSetMask |= …)
        JvmValue b = pop();
        JvmValue a = pop();
        push(JvmValue::make_int(a.v.i | b.v.i));
        break;
      }
      default:
        break;
    }
  }

  if (!stack.empty()) return stack.back();
  return JvmValue::make_void();
}

}  // namespace inv

