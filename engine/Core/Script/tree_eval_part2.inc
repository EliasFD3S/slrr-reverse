// Fragment 2/3 of tree_eval.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.

            if (stack.size() >= 7 && stack.back().tag == JvmTag::Int &&
                stack[stack.size() - 2].tag == JvmTag::Obj &&
                stack[stack.size() - 3].tag == JvmTag::Int)
              return 6;
            return 5;
          }
          if (mname == "createHeader") return 1;
          if (mname == "createRectangle") return 6;
          if (mname == "neon") return 0;
          if (mname == "userWait") {
            if (stack.size() >= 2 &&
                (stack[stack.size() - 2].tag == JvmTag::Float ||
                 stack[stack.size() - 2].tag == JvmTag::Int))
              return 1;
            return 0;
          }
          if (mname == "checkHint") return 1;
          if (mname == "activateState") {
            if (stack.size() >= 3 &&
                stack[stack.size() - 2].tag == JvmTag::Int &&
                stack[stack.size() - 3].tag == JvmTag::Int)
              return 2;
            return 1;
          }
          if (mname == "activate") {
            if (stack.size() >= 2 &&
                (stack[stack.size() - 2].tag == JvmTag::Int ||
                 stack[stack.size() - 2].tag == JvmTag::Obj))
              return 1;
            return 0;
          }
          if (mname == "remNotification") return 2;
          if (mname == "cleanup" || mname == "disableCameraControl") return 0;
          if (mname == "flushInventory" || mname == "currentLine") return 0;
          if (mname == "osdCommand") return 1;
          if (mname == "giveWarning") return 2;
          if (mname == "changePointer") return 0;
          if (mname == "log") {
            if (stack.size() >= 2 && stack[stack.size() - 2].tag == JvmTag::Obj)
              return 1;
            return 0;
          }
          if (mname == "putMessage") return 1;
          if (mname == "setPriority") return 1;
          if (mname == "setPos") {
            if (stack.size() >= 3 &&
                (stack[stack.size() - 2].tag == JvmTag::Float ||
                 stack[stack.size() - 2].tag == JvmTag::Int) &&
                (stack[stack.size() - 3].tag == JvmTag::Float ||
                 stack[stack.size() - 3].tag == JvmTag::Int))
              return 2;
            if (stack.size() >= 2 && stack[stack.size() - 2].tag == JvmTag::Obj)
              return 1;
            return 0;
          }
          if (mname == "changeCamTarget" || mname == "changeCamTarget2") {
            if (stack.size() >= 2 && stack[stack.size() - 2].tag == JvmTag::Obj)
              return 1;
            return 0;
          }
          if (mname == "setTransmission" || mname == "leaveCar" ||
              mname == "constructName") {
            if (stack.size() >= 2 &&
                (stack[stack.size() - 2].tag == JvmTag::Int ||
                 stack[stack.size() - 2].tag == JvmTag::Float))
              return 1;
            return 0;
          }
          if (mname == "setDefaultTransmission" ||
              mname == "setDefaultSteeringHelp" || mname == "setDefaultASR" ||
              mname == "setDefaultABS")
            return 0;
          if (mname == "set") {
            if (stack.size() >= 2 &&
                (stack[stack.size() - 2].tag == JvmTag::Int ||
                 stack[stack.size() - 2].tag == JvmTag::Obj))
              return 1;
            return 0;
          }
          if (mname == "removeAllElements") return 0;
          if (mname == "createMenu") {
            // (style,x,y,spc[,ori]) or +stySld/+knob. Do NOT use raw
            // stack.size() — leftover ResourceRef/look packs inflate it and
            // Garage.createOSDObjects then mis-packs (TREE btn=1).
            // ResourceRef is addItem, never a createMenu arg — stop before it.
            int base = 0;
            if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
              const char* hc = tree_host_class(stack.back().v.o);
              if (hc && std::strstr(hc, "Osd")) base = 1;
            }
            auto at = [&](int i) -> const JvmValue* {
              if (i < 0 || static_cast<size_t>(i) >= stack.size())
                return nullptr;
              return &stack[stack.size() - 1 - static_cast<size_t>(i)];
            };
            int nobj = 0, nfloat = 0, nint = 0;
            for (int i = base; i < base + 8; ++i) {
              const JvmValue* v = at(i);
              if (!v) break;
              if (v->tag == JvmTag::Float) ++nfloat;
              else if (v->tag == JvmTag::Int) ++nint;
              else if (v->tag == JvmTag::Obj && v->v.o) {
                const char* hc = tree_host_class(v->v.o);
                if (hc && std::strstr(hc, "Style"))
                  ++nobj;
                else if (hc && std::strstr(hc, "ResourceRef"))
                  break;
                else if (hc && std::strstr(hc, "Osd"))
                  break;
                else if (hc && (std::strstr(hc, "Menu") ||
                                std::strstr(hc, "Button") ||
                                std::strstr(hc, "Garage")))
                  break;
                else
                  ++nobj;
              } else
                break;
            }
            const int nargs = nobj + nfloat + nint;
            if (nargs >= 6) return 6;
            if (nargs >= 5) return 5;
            if (nargs >= 4) return 4;
            return 5;
          }
          if (mname == "addItem") {
            // Button / slider / multi overloads — sniff stack (Menu recv on top
            // or JVM [menu, args…] with last arg on top).
            int base = 0;
            if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
              const char* hc = tree_host_class(stack.back().v.o);
              if (hc && std::strstr(hc, "Menu")) base = 1;
            }
            auto at = [&](int i) -> const JvmValue* {
              if (i < 0 || static_cast<size_t>(i) >= stack.size())
                return nullptr;
              return &stack[stack.size() - 1 - static_cast<size_t>(i)];
            };
            for (int i = base; i < base + 8; ++i) {
              const JvmValue* v = at(i);
              if (!v || v->tag != JvmTag::Obj || !v->v.o) continue;
              const char* hc = tree_host_class(v->v.o);
              if (hc && (std::strstr(hc, "Vector") || hc[0] == '['))
                return 5;  // multi: text,cmd,value,choices,tooltip
            }
            int floats = 0;
            for (int i = base; i < base + 8; ++i) {
              const JvmValue* v = at(i);
              if (v && v->tag == JvmTag::Float) ++floats;
            }
            // Garage icon: RR lives in recent_new (NEW+0x21). Leftover createMenu
            // floats must not force slider argc=4 — that skips icon packing and
            // yields text addItem for CMD_HITTHESTREET/TESTTRACK.
            bool icon_cmd = false;
            for (int i = base; i < base + 6; ++i) {
              const JvmValue* v = at(i);
              if (v && v->tag == JvmTag::Int && is_icon_osd_cmd(v->v.i)) {
                icon_cmd = true;
                break;
              }
            }
            if (icon_cmd && recent_new) {
              const char* rhc = tree_host_class(recent_new);
              if (rhc && (std::strstr(rhc, "ResourceRef") ||
                          std::strstr(rhc, "GameRef")))
                return 3;
            }
            if (floats >= 3) return 7;  // ranged slider
            if (floats >= 1 && !icon_cmd) return 4;  // simple slider
            // (ResourceRef,int,String) icon buttons — Garage.createOSDObjects.
            for (int i = base; i < base + 4; ++i) {
              const JvmValue* v = at(i);
              if (!v || v->tag != JvmTag::Obj || !v->v.o) continue;
              const char* hc = tree_host_class(v->v.o);
              if (hc && (std::strstr(hc, "ResourceRef") ||
                         std::strstr(hc, "GameRef")))
                return 3;
            }
            // (String,int,String tooltip)
            {
              int strs = 0, ints = 0;
              for (int i = base; i < base + 4; ++i) {
                const JvmValue* v = at(i);
                if (!v) break;
                if (v->tag == JvmTag::Int) ++ints;
                if (v->tag == JvmTag::Obj && v->v.o) {
                  const char* hc = tree_host_class(v->v.o);
                  if (!hc || !hc[0] || std::strstr(hc, "String")) ++strs;
                }
              }
              if (strs >= 2 && ints >= 1) return 3;
            }
            // (String,int) — never return 3 just because a String Obj is at base
            // (that ate the label and invented a phantom 3rd arg).
            return 2;
          }
          if (mname == "disable" || mname == "lockPlayerCar" ||
              mname == "moveCamera" || mname == "loadVisibleCars" ||
              mname == "deleteOSDObjects" || mname == "loop" ||
              mname == "enterAsyncMode")
            return 0;
          if (mname == "startdir" || mname == "changeColor") return 1;
          if (mname == "point") {
            if (stack.size() >= 6) return 5;
            return 4;
          }
          if (mname == "duplicate") return 1;
          if (mname == "createText") return 5;
          if (mname == "createBox") return 5;
          // PE @ 0x004806F0 createSphere(ResourceRef,F,String)V — argc 3, not box 5.
          if (mname == "createSphere") return 3;
          if (mname == "timeWarp") return 1;
          if (mname == "getTime" || mname == "changeMusicSet" ||
              mname == "setGlobalEnvmap" || mname == "setTimerText") {
            // resolved as static below
            return -1;
          }
          if (mname == "addElement" || mname == "elementAt" ||
              mname == "addColorIndex")
            return 1;
          if (mname == "size" || mname == "intValue" ||
              mname == "getFirstChild" || mname == "getNextChild" ||
              mname == "cache" || mname == "currentLine" ||
              mname == "toString" ||
              mname == "getPos" || mname == "getOri" || mname == "destroy" ||
              mname == "unload" || mname == "id" || mname == "reset" ||
              mname == "disableCameraControl")
            return 0;
          return -1;  // unknown
        };

        int argc = 0;
        bool is_static_call = (n.op == 0x12);
        std::string owner = cls_name;
        if (mname == "first") {
          is_static_call = false;
          owner = "java.io.FindFile";
          // recv on top; FLAGS int just under recv → first(path, flags).
          argc = 1;
          if (stack.size() >= 2 && stack[stack.size() - 2].tag == JvmTag::Int)
            argc = 2;
        } else if (mname == "next" || mname == "close") {
          argc = 0;
          is_static_call = false;
          owner = "java.io.FindFile";
        } else if (mname == "delete" || mname == "exists") {
          argc = 1;
          is_static_call = true;
          owner = "java.io.File";
        } else if (mname == "copy" || mname == "move") {
          argc = 2;
          is_static_call = true;
          owner = "java.io.File";
        } else if (mname == "openLib") {
          argc = 1;
          is_static_call = true;
          owner = "java.lang.System";
        } else if (mname == "addColorIndex") {
          argc = 1;
          is_static_call = false;
        } else if (mname == "createHotkey") {
          // SplashScreen encodes osd.createHotkey as 0x12 + packed args;
          // still an instance call on Osd (recv on top after getfield).
          argc = 4;
          if (stack.size() >= 6 && stack[stack.size() - 2].tag == JvmTag::Int &&
              stack[stack.size() - 3].tag == JvmTag::Obj)
            argc = 5;
          is_static_call = false;
          owner = "java.render.Osd";
        } else if (mname == "createMenu" || mname == "createText" ||
                   mname == "createTextBox" || mname == "createButton" ||
                   mname == "createRectangle" || mname == "createHeader" ||
                   mname == "createBG") {
          const int a = argc_for_method(mname);
          argc = a >= 0 ? a : 4;
          is_static_call = false;
          owner = "java.render.Osd";
        } else if (mname == "addItem" || mname == "addSeparator" ||
                   mname == "setSliderStyle") {
          const int a = argc_for_method(mname);
          argc = a >= 0 ? a : (mname == "addSeparator" ? 0 : 2);
          is_static_call = false;
          owner = "java.render.osd.Menu";
        } else if (mname == "disable") {
          argc = 0;
          is_static_call = false;
        } else if (mname == "changeActiveSection") {
          argc = 1;
          is_static_call = true;
          owner = "java.game.GameLogic";
        } else if (mname == "autoSave") {
          argc = 0;
          is_static_call = true;
          owner = "java.game.GameLogic";
        } else if (mname == "autoSaveQuiet") {
          argc = 0;
          is_static_call = true;
          owner = "java.game.GameLogic";
        } else if (mname == "loadDefaults") {
          argc = 0;
          is_static_call = true;
          owner = "java.game.GameLogic";
        } else if (mname == "setTime") {
          argc = 1;
          is_static_call = true;
          owner = "java.game.GameLogic";
        } else if (mname == "getTime") {
          argc = 0;
          is_static_call = true;
          owner = "java.game.GameLogic";
        } else if (mname == "setTimerText") {
          argc = 2;
          is_static_call = true;
          owner = "java.game.GameLogic";
        } else if (mname == "setGlobalEnvmap") {
          argc = 1;
          is_static_call = true;
          owner = "java.render.GfxEngine";
        } else if (mname == "changeMusicSet") {
          argc = 1;
          is_static_call = true;
          owner = "java.sound.Sound";
        } else if (mname == "getVolume" || mname == "setVolume") {
          argc = (mname == "setVolume") ? 2 : 1;
          is_static_call = true;
          owner = "java.sound.Sound";
        } else if (mname == "numDisplayModes" || mname == "currDisplayMode") {
          argc = 0;
          is_static_call = true;
          owner = "java.render.GfxEngine";
        } else if (mname == "displayModeName" || mname == "changeVideoMode") {
          argc = (mname == "changeVideoMode") ? 3 : 1;
          is_static_call = true;
          owner = "java.render.GfxEngine";
        } else if (mname == "getConfigOptions") {
          argc = 0;
          is_static_call = true;
          owner = "java.lang.System";
        } else if (mname == "getAxis") {
          argc = 2;
          is_static_call = true;
          owner = "java.io.Input";
        } else if (mname == "timeWarp") {
          argc = 1;
          is_static_call = true;
          owner = "java.lang.System";
        } else if (mname == "show" || mname == "hide") {
          // May be Osd (argc 0) or LoadingScreen — prefer recv class.
          if (stack.size() >= 1 && stack.back().tag == JvmTag::Obj) {
            const char* hc = tree_host_class(stack.back().v.o);
            if (hc && std::strstr(hc, "LoadingScreen")) {
              argc = 0;
              is_static_call = false;
              owner = "java.render.LoadingScreen";
            } else {
              const int a = argc_for_method(mname);
              argc = a >= 0 ? a : 0;
              is_static_call = false;
            }
          } else {
            const int a = argc_for_method(mname);
            argc = a >= 0 ? a : 0;
            is_static_call = false;
          }
        } else if (mname == "userWait") {
          argc = 0;
          if (stack.size() >= 2 &&
              (stack[stack.size() - 2].tag == JvmTag::Float ||
               stack[stack.size() - 2].tag == JvmTag::Int))
            argc = 1;
          is_static_call = false;
          owner = "java.render.LoadingScreen";
        } else if (mname == "osdCommand") {
          // SplashScreen.handleEvent encodes osdCommand as 0x12 instance call.
          argc = 1;
          is_static_call = false;
        } else if (mname == "random" || mname == "randomize") {
          argc = 0;
          is_static_call = true;
          owner = "java.lang.Math";
        } else if (mname == "<init>") {
          const int a = argc_for_method(mname);
          argc = a >= 0 ? a : 0;
          if (argc <= 0 &&
              pending_class.find("Trigger") != std::string::npos) {
            argc = 4;
            for (size_t i = 0; i < stack.size() && i < 6; ++i) {
              if (stack[stack.size() - 1 - i].tag == JvmTag::Float) {
                argc = 5;
                break;
              }
            }
          } else if (argc <= 0 &&
                     pending_class.find("ParkingCar") != std::string::npos) {
            argc = 5;
          } else if (argc <= 0 &&
                     pending_class.find("RenderRef") != std::string::npos &&
                     pending_class.find("Camera") == std::string::npos) {
            argc = (stack.size() >= 4) ? 3 : 1;
          } else if (argc <= 0 &&
                     pending_class.find("GameRef") != std::string::npos) {
            argc = (stack.size() >= 5) ? 4 : 1;
          } else if (argc <= 0 &&
                     pending_class.find("Vector3") != std::string::npos) {
            const int a = argc_for_method(mname);
            argc = a >= 0 ? a : 0;
          } else if (argc <= 0 &&
                     pending_class.find("Rectangle") != std::string::npos) {
            argc = 3;
          } else if (argc <= 0 &&
                     pending_class.find("Menu") != std::string::npos &&
                     pending_class.find("MainMenu") == std::string::npos) {
            argc = 5;
            if (stack.size() >= 7 &&
                stack[stack.size() - 2].tag == JvmTag::Int)
              argc = 6;
          }
          is_static_call = false;
        } else {
          const int a = argc_for_method(mname);
          if (a >= 0) {
            argc = a;
            is_static_call = false;
          } else if (!mname.empty()) {
            argc = 0;
          } else {
            break;
          }
        }

        std::vector<JvmValue> args;
        if (!is_static_call) {
          auto is_self_method = [&](const std::string& m) {
            // changeMode is Garage/Dialog self OR Navigator (track.nav) —
            // never force RaceSetup/City this as recv.
            // enter/exit/startRace*: GameState/City self (Java this) — PE
            // Object_callMethod @ 0x408A30 name-only; Vector3/prev on top must
            // not steal recv (Valocity.enter → startRace / exit INSTANCEOF path).
            return m == "createOSDObjects" || m == "lockCar" ||
                   m == "releaseCar" ||
                   m == "addSceneElements" || m == "remSceneElements" ||
                   m == "cameraSetup" || m == "giveWarning" ||
                   m == "setEventMask" || m == "clearEventMask" ||
                   m == "addTimer" || m == "addCustomGroups" ||
                   m == "enableAnimateHook" || m == "disableAnimateHook" ||
                   m == "createQuickRaceBot" || m == "destroyRaceBot" ||
                   m == "enter" || m == "exit" || m == "startRace" ||
                   m == "startRace2";
          };
          auto is_osd_builder = [&](const std::string& m) {
            return m == "createButton" || m == "createText" ||
                   m == "createTextBox" || m == "createRectangle" ||
                   m == "createHeader" || m == "createMenu" ||
                   m == "createBG" || m == "createHotkey" ||
                   m == "addItem" || m == "addSeparator" ||
                   m == "setSliderStyle";
          };
          JvmValue recv = JvmValue::make_obj(nullptr);
          auto is_osd_junk = [&](const JvmValue& v) -> bool {
            if (v.tag != JvmTag::Obj || !v.v.o) return false;
            const char* hc = tree_host_class(v.v.o);
            if (!hc || !hc[0]) return false;
            return std::strstr(hc, "Rectangle") || std::strstr(hc, "Button") ||
                   std::strstr(hc, "Text") || std::strstr(hc, "Hotkey") ||
                   std::strstr(hc, "Group");
          };
          if (mname == "addItem" && (argc == 2 || argc == 3)) {
            while (!stack.empty() && is_osd_junk(stack.back())) pop();
            // Garage: buttonStyle often left on stack after createMenu packing.
            while (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                   stack.back().v.o) {
              const char* shc = tree_host_class(stack.back().v.o);
              if (shc && std::strstr(shc, "Style"))
                pop();
              else
                break;
            }
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
                  // Last icon CMD wins — leftover 0 (createMenu ori) is popped
                  // first; RaceSetup CMD_ABANDON=1 must replace it.
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
                  // 0x09 method-name ldc (addItem/createMenu) is not a tip.
                  if (sc && is_osd_invoke_name(sc)) {
                    /* skip */
                  } else if (!label.v.o)
                    label = v;
                  else if (!tip.v.o)
                    tip = v;
                } else if (hc && std::strstr(hc, "Menu"))
                  recv = v;
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
                if (thc && std::strstr(thc, "Menu")) recv = pop();
              }
              if ((recv.tag != JvmTag::Obj || !recv.v.o) && last_menu)
                recv = JvmValue::make_obj(last_menu);
            }
            while (!stack.empty() && is_osd_junk(stack.back())) pop();
            // Recover RR under tip/cmd — do not discard it as sugar.
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
            args.push_back(recv);
            if (gfx.v.o) {
              args.push_back(gfx);
              args.push_back(cmdv);
              args.push_back(tip.v.o ? tip : label);
            } else {
              args.push_back(label);
              args.push_back(cmdv);
              if (tip.v.o) args.push_back(tip);
            }
          } else if (is_osd_builder(mname)) {
            // Dialog.show: LOCAL_LOAD osd then 0x12 → recv on top.
            // Splash createBG often pushes osd first (JVM order).
            bool recv_on_top = false;
            if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
              const char* thc = tree_host_class(stack.back().v.o);
              if (thc && std::strstr(thc, "Osd")) recv_on_top = true;
            }
            if (mname == "createButton") {
              if (recv_on_top) {
                if (stack.size() >= 8 &&
                    stack[stack.size() - 2].tag == JvmTag::Int &&
                    stack[stack.size() - 3].tag == JvmTag::Obj &&
                    stack[stack.size() - 4].tag == JvmTag::Int)
                  argc = 6;
                else
                  argc = 5;
              } else if (stack.size() >= 7 && stack.back().tag == JvmTag::Int &&
                         stack[stack.size() - 2].tag == JvmTag::Obj &&
                         stack[stack.size() - 3].tag == JvmTag::Int) {
                argc = 6;
              } else {
                argc = 5;
              }
            }
            if (recv_on_top) {
              recv = pop();
              for (int i = 0; i < argc; ++i) {
                if (!stack.empty())
                  args.insert(args.begin(), pop());
                else
                  args.insert(args.begin(), JvmValue::make_int(0));
              }
              args.insert(args.begin(), recv);
            } else {
              // JVM: [osdRecv, args…] last arg on top.
              for (int i = 0; i < argc; ++i) {
                if (!stack.empty())
                  args.insert(args.begin(), pop());
                else
                  args.insert(args.begin(), JvmValue::make_int(0));
              }
              if (!stack.empty() && stack.back().tag == JvmTag::Obj)
                recv = pop();
              else if (!locals.empty() && locals[0].tag == JvmTag::Obj) {
                if (InvObject* osd =
                        tree_field_get_obj(locals[0].v.o, "osd"))
                  recv = JvmValue::make_obj(osd);
                else
                  recv = locals[0];
              }
              args.insert(args.begin(), recv);
            }
            // Garage: buttonStyle lives in a local; inject if packing missed it.
            if (mname == "createMenu") {
              bool has_sty = false;
              for (size_t i = 1; i < args.size(); ++i) {
                if (args[i].tag != JvmTag::Obj || !args[i].v.o) continue;
                const char* hc = tree_host_class(args[i].v.o);
                if (hc && std::strstr(hc, "Style")) {
                  has_sty = true;
                  break;
                }
              }
              if (!has_sty) {
                for (const JvmValue& loc : locals) {
                  if (loc.tag != JvmTag::Obj || !loc.v.o) continue;
                  const char* hc = tree_host_class(loc.v.o);
                  if (hc && std::strstr(hc, "Style")) {
                    args.insert(args.begin() + 1, loc);
                    break;
                  }
                }
              }
            }
          } else if (mname == "addTraffic" || mname == "addTrafficCar" ||
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
            // JVM: [map, args…] last arg on top. Do not fall back to Valocity
            // `this` when floats/ints sit on the stack. RaceSetup uses track.map.
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
            // Garage/Dialog.changeMode(group) stays on self.
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
          } else if (is_self_method(mname) && !locals.empty() &&
              locals[0].tag == JvmTag::Obj) {
            // Garage/Scene methods bind to `this`; ignore stray Osd/Vector3 on stack.
            if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                stack.back().v.o != locals[0].v.o)
              pop();
            recv = locals[0];
            for (int i = 0; i < argc; ++i) {
              if (!stack.empty())
                args.insert(args.begin(), pop());
              else
                args.insert(args.begin(), JvmValue::make_int(0));
            }
            args.insert(args.begin(), recv);
          } else if (mname == "queueEvent") {
            tree_pack_queue_event(stack, locals, args, &recv);
          } else if (mname == "create") {
            tree_pack_renderref_create(stack, locals, args, &recv);
          } else if (mname == "add" || mname == "mul" || mname == "sub") {
            tree_pack_vector3_binop(stack, locals, args, &recv);
          } else {
            if (!stack.empty() && stack.back().tag == JvmTag::Obj)
              recv = pop();
            else if (!locals.empty())
              recv = locals[0];
            for (int i = 0; i < argc; ++i) {
              if (!stack.empty())
                args.insert(args.begin(), pop());
              else
                args.insert(args.begin(), JvmValue::make_int(0));
            }
            args.insert(args.begin(), recv);
          }
          const char* hc = tree_host_class(recv.v.o);
          if (hc && hc[0]) owner = hc;
          if ((mname == "add" || mname == "mul" || mname == "sub") &&
              recv.tag == JvmTag::Obj && tree_is_vector3(recv.v.o))
            owner = "java.lang.Vector3";
          if (mname == "<init>" &&
              pending_class.find("Trigger") != std::string::npos) {
            auto is_trig = [](InvObject* o) -> bool {
              const char* c = tree_host_class(o);
              return c && std::strstr(c, "Trigger");
            };
            InvObject* t = is_trig(recv.v.o) ? recv.v.o : nullptr;
            if (!t && recent_new && is_trig(recent_new)) t = recent_new;
            if (!t && last_new && is_trig(last_new)) t = last_new;
            if (t && (!args.empty() && args[0].v.o != t)) {
              if (recv.tag == JvmTag::Obj && recv.v.o && recv.v.o != t)
                args.push_back(recv);
              args[0] = JvmValue::make_obj(t);
              recv = args[0];
            }
            owner = "java.game.Trigger";
          }
          if (mname == "addElement" || mname == "elementAt" ||
              mname == "size") {
            // Prefer the real Vector handle when TREE left the element on top.
            if (!args.empty() && args[0].tag == JvmTag::Obj &&
                !tree_vector_is(args[0].v.o) && args.size() >= 2 &&
                args[1].tag == JvmTag::Obj && tree_vector_is(args[1].v.o)) {
              std::swap(args[0], args[1]);
            }
            owner = "java.util.Vector";
          }
          if (mname == "createHotkey" || mname == "createBG" ||
              mname == "createButton" || mname == "createText" ||
              mname == "createTextBox" || mname == "createRectangle" ||
              mname == "createHeader" || mname == "createMenu" ||
              mname == "show" || mname == "hide") {
            if (hc && std::strstr(hc, "Osd")) {
              owner = "java.render.Osd";
            } else if (!locals.empty() && locals[0].tag == JvmTag::Obj) {
              if (InvObject* osd = tree_field_get_obj(locals[0].v.o, "osd")) {
                args[0] = JvmValue::make_obj(osd);
                owner = "java.render.Osd";
              }
            } else {
              owner = "java.render.Osd";
            }
          }
          if (mname == "addTraffic" || mname == "addTrafficCar" ||
              mname == "addTrafficN" || mname == "addTrafficP" ||
              mname == "notifyTrafficCar" || mname == "setTrafficCarBehaviour" || mname == "remTrafficCar" ||
              mname == "setPedestrianDensity" ||
              mname == "setPedestrianDensityN" ||
              mname == "addPedestrianType" || mname == "remPedestrianType" ||
              mname == "setWater" || mname == "addWaterLimit" ||
              mname == "getNearestCross" || mname == "getStartDirection" ||
              mname == "getRouteLength" || mname == "findRoute" ||
              mname == "haltTrafficCross" || mname == "haltTrafficPath" ||
              mname == "alignToRoad" || mname == "delTraffic") {
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
          if (mname == "startRace") {
            if (!args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
              const char* rhc = tree_host_class(args[0].v.o);
              if (rhc && std::strstr(rhc, "RaceSetup")) {
                InvObject* track = tree_field_get_obj(args[0].v.o, "track");
                if (!track)
                  track = tree_field_get_obj(args[0].v.o, "lastState");
                if (track) {
                  args[0] = JvmValue::make_obj(track);
                  const char* tc = tree_host_class(track);
                  if (tc && tc[0]) owner = tc;
                }
              }
            }
          }
          if (mname == "addMarker" || mname == "remMarker" ||
              mname == "updateNavigator" || mname == "changeMode" ||
              mname == "changeSize" || mname == "changeZoom") {
            // Garage/Dialog keep self; RaceSetup/City → track.nav.
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
        } else {
          for (int i = 0; i < argc; ++i) args.insert(args.begin(), pop());
        }

        // OptionsDialog Menu chain: createBG leaves Rectangle on stack; if
        // createMenu result was discarded, setSliderStyle/addItem steal it.
        if ((mname == "setSliderStyle" || mname == "addItem" ||
             mname == "addSeparator") &&
            last_menu && !args.empty()) {
          const char* rhc = tree_host_class(args[0].v.o);
          if (!rhc || !std::strstr(rhc, "Menu")) {
            args[0] = JvmValue::make_obj(last_menu);
            owner = "java.render.osd.Menu";
          }
        }
        if ((mname == "printValue" || mname == "setValue" ||
             mname == "setRange" || mname == "setTicks" ||
             mname == "setOffset" || mname == "setToolTip") &&
            last_menu_item && !args.empty()) {
          const char* rhc = tree_host_class(args[0].v.o);
          if (!rhc || (!std::strstr(rhc, "Slider") &&
                       !std::strstr(rhc, "MultiChoice") &&
                       !std::strstr(rhc, "Button") &&
                       !std::strstr(rhc, "Gadget"))) {
            args[0] = JvmValue::make_obj(last_menu_item);
            owner = rhc && rhc[0] ? rhc : "java.render.osd.Slider";
            const char* ihc = tree_host_class(last_menu_item);
            if (ihc && ihc[0]) owner = ihc;
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
            soft_call_by_name(owner.c_str(), mname, args, is_static_call);
        if (mname == "createMenu" && r.tag == JvmTag::Obj && r.v.o)
          last_menu = r.v.o;
        if (mname == "addItem" && r.tag == JvmTag::Obj && r.v.o)
          last_menu_item = r.v.o;
        if (mname == "endGroup" && r.tag == JvmTag::Int)
          capture_endgroup_field(r.v.i, ip);
        // MainMenuDialog.osdCommand: after CAS left MainMenu (Garage/Valocity),
        // stop before leftover super.osdCommand. PE Object_callMethod continues
        // the frame — Soft only gates on actual section change, not hub
        // freeride/new/exit pending flags (those skipped Soft world enter).
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
        // Soft commit ≡ PE invokeMethod native push @ 0x420129 /
        // empty-body Int0 @ 0x42066c (fn @ 0x41FBC0).
        soft_invoke_commit(mname, r,
                           soft_invoke_is_osd_builder_result(mname));
        break;
      }
      case 0x14: {  // FIELD_REF_STATIC or relative GOTO when imm is negative
        if (n.has_imm && (n.imm & 0x80000000u)) {
          const int32_t rel = static_cast<int32_t>(n.imm);
          const int64_t next = static_cast<int64_t>(ip) + rel;
          if (next < 0 || next >= static_cast<int64_t>(tree.nodes.size())) {
            if (err) *err = "GOTO OOB";
            return JvmValue::make_void();
          }
          ip = static_cast<size_t>(next) - 1;  // compensated by loop ++
        }
        break;
      }
      case 0x15: {
        // IFEQ / relative forward jump when condition falsy.
        // With pending ICMP_EQ (0x07/7): compare top two stack values first
        // (SplashScreen.osdCommand: cmd == Input.AXIS_CANCEL).
        // With pending INSTANCEOF (0x07/32)+TYPE: Valocity.enter RaceSetup gate.
        JvmValue cond;
        bool from_stack = false;
        if (pending_binop && *pending_binop == 7 && stack.size() >= 2) {
          JvmValue b = pop();
          JvmValue a = pop();
          int eq = 0;
          if (a.tag == JvmTag::Obj || b.tag == JvmTag::Obj)
            eq = (a.v.o == b.v.o) ? 1 : 0;
          else if (a.tag == JvmTag::Float || b.tag == JvmTag::Float)
            eq = (a.v.f == b.v.f) ? 1 : 0;
          else
            eq = (a.v.i == b.v.i) ? 1 : 0;
          cond = JvmValue::make_int(eq);
          pending_binop.reset();
          from_stack = true;
        } else if (pending_binop && *pending_binop == 32 &&
                   !pending_class.empty()) {
          cond = JvmValue::make_int(eval_instanceof());
          pending_binop.reset();
          from_stack = true;
        } else if (!stack.empty()) {
          cond = pop();
          from_stack = true;
        } else {
          cond = !locals.empty() ? locals[0] : JvmValue::make_obj(nullptr);
        }
        // Jump when condition matches IFEQ polarity. Special case: after
        // Dialog.display(), stock Java is `if (display() == 0) { body }` but
        // TREE emits IFEQ that must fall through on 0 (OK) and skip on cancel.
        bool invert_display_eq0 = false;
        if (!pending_binop) {
          for (int k = 1; k <= 8 && ip >= static_cast<size_t>(k); ++k) {
            const TreeNode& pn = tree.nodes[ip - static_cast<size_t>(k)];
            if (pn.op == 0x07 || pn.op == 0x0b || pn.op == 0x06 ||
                pn.op == 0x01 || pn.op == 0x02 || pn.op == 0x03)
              continue;
            if ((pn.op == 0x1a || pn.op == 0x12 || pn.op == 0x25) &&
                pn.has_imm) {
              std::string mn = field_name(pn.imm);
              if (mn.empty()) mn = cstr(pn.imm);
              if (mn == "display") invert_display_eq0 = true;
            }
            // First meaningful prior op decides polarity; stop scanning.
            break;
          }
        }
        const bool take_jump =
            invert_display_eq0 ? tree_truthy(cond) : !tree_truthy(cond);
        if (take_jump && n.has_imm) {
          const int32_t rel = static_cast<int32_t>(n.imm);
          if (rel > 0) {
            const int64_t next = static_cast<int64_t>(ip) + rel;
            if (next >= 0 && next < static_cast<int64_t>(tree.nodes.size())) {
              ip = static_cast<size_t>(next) - 1;
            }
          } else if (n.imm < tree.nodes.size()) {
            ip = n.imm - 1;
          }
        } else if (from_stack && !take_jump) {
          // fall through; cond already consumed
        }
        break;
      }
      case 0x19: {  // JMP_EQ — peek stack (keep value for later use, e.g. first path)
        // Class hint in getstatic+NEWARRAY FILD pattern (skip invoke).
        if (pending_static_newarray) break;
        // getstatic sugar: … 0x11/0x19/0x1b Field (Frontend.mediumFont,
        // Osd.MD_HORIZONTAL). Must not treat spc=0 / other zeros as IFEQ —
        // Garage.createOSDObjects was jumping over createMenu.
        if (ip + 1 < tree.nodes.size() && tree.nodes[ip + 1].op == 0x1b)
          break;
        // Oversized imm = method-ref (Garage.exit SfxRef.play / Vehicle.command…).
        if (n.has_imm && n.imm >= tree.nodes.size()) {
          std::string mname = field_name(n.imm);
          if (mname.empty()) mname = cstr(n.imm);
          if (!mname.empty()) {
            int argc = 0;
            if (mname == "command" || mname == "play" || mname == "enable" ||
                mname == "activateState" || mname == "setSpeed" ||
                mname == "setFade" || mname == "seek")
              argc = 1;
            else if (mname == "giveWarning" || mname == "setMatrix")
              argc = 2;
            else if (mname == "queueEvent")
              argc = 3;
            else if (mname == "add" || mname == "mul" || mname == "sub")
              argc = 1;
            else if (mname == "create") {
              if ((!stack.empty() && stack.back().tag == JvmTag::Obj &&
                   tree_is_renderref(stack.back().v.o)) ||
                  (!locals.empty() && locals[0].tag == JvmTag::Obj &&
                   tree_is_renderref(locals[0].v.o)))
                argc = 3;
            }
            JvmValue recv = JvmValue::make_obj(nullptr);
            std::vector<JvmValue> args;
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
            } else if (mname == "queueEvent") {
              tree_pack_queue_event(stack, locals, args, &recv);
            } else if (mname == "create" && argc == 3) {
              tree_pack_renderref_create(stack, locals, args, &recv);
            } else if (mname == "add" || mname == "mul" || mname == "sub") {
              tree_pack_vector3_binop(stack, locals, args, &recv);
            } else {
              if (!stack.empty() && stack.back().tag == JvmTag::Obj)
                recv = pop();
              else if (!locals.empty())
                recv = locals[0];
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
            // Soft ≡ PE Object_callMethod @ 0x408A30 via op33 / oversized 0x19.
            JvmValue r = soft_call_by_name(owner, mname, args, /*is_static=*/false);
            soft_invoke_commit(mname, r, /*keep_extra=*/false,
                               /*always_nonvoid=*/true);
          }
          break;
        }
        // Empty stack = statement epilogue after void calls (SplashScreen
        // createBG → 0x08/27 → 0x11 → 0x19), not a failed FindFile.first.
        if (stack.empty()) break;
        JvmValue cond = stack.back();
        if (!tree_truthy(cond)) {
          pop();
          if (!n.has_imm || n.imm >= tree.nodes.size()) {
            if (err) *err = "JMP target OOB";
            return JvmValue::make_void();
          }
          if (n.imm > ip) {
            ip = n.imm - 1;
          }
        }
        break;
      }
      case 0x1a: {  // JMP_EQ2 — while end  OR  method-ref invoke (imm = const idx)
        // Method-ref when imm is outside the node table (preCacheGametypes), OR
        // when imm names a VT-ctor method that collides with a valid jump index
        // (Baiern_VT addElement imm=20 in a 2238-node tree). Never treat File.*
        // control-flow names (delete/copy/…) as methods here — those are JMPs.
        auto is_inline_mref = [](const std::string& m) {
          // GameRef.command strings ("filter 2") are the method name.
          if (m.find(' ') != std::string::npos) return true;
          return m == "addElement" || m == "elementAt" || m == "size" ||
                 m == "addColorIndex" || m == "intValue" || m == "createBG" ||
                 m == "createHotkey" || m == "show" || m == "hide" ||
                 m == "setEventMask" || m == "clearEventMask" ||
                 m == "addTimer" || m == "removeAllTimers" ||
                 m == "createOSDObjects" || m == "lockCar" ||
                 m == "releaseCar" || m == "changeMode" ||
                 m == "addSceneElements" || m == "remSceneElements" ||
                 m == "cameraSetup" || m == "giveWarning" || m == "endGroup" ||
                 m == "beginGroup" || m == "hideGroup" || m == "showGroup" ||
                 m == "display" || m == "filterInventory" ||
                 m == "flushInventory" || m == "play" || m == "enable" ||
                 m == "addHandler" || m == "remHandler" ||
                 m == "enableCameraControl" || m == "disableCameraControl" ||
                 m == "command" || m == "destroy" || m == "unload" ||
                 m == "getPos" || m == "getOri" || m == "reset" ||
                 m == "activateState" || m == "checkHint" || m == "neon" ||
                 m == "tesztmen" || m == "/" || m == "deleteOSDObjects" ||
                 m == "userWait" || m == "activate" || m == "cleanup" ||
                 m == "remNotification" || m == "flushInventory" ||
                 m == "enableOsd" || m == "removeAllElements" ||
                 m == "osdCommand" || m == "changePointer" || m == "flush" ||
                 m == "log" || m == "putMessage" || m == "setPriority" ||
                 m == "setPos" || m == "changeCamTarget" ||
                 m == "changeCamTarget2" || m == "setTransmission" ||
                 m == "leaveCar" || m == "constructName" ||
                 m == "setDefaultTransmission" ||
                 m == "setDefaultSteeringHelp" || m == "setDefaultASR" ||
                 m == "setDefaultABS" || m == "set" ||
                 m == "setDamageMultiplier" || m == "setCruiseControl" ||
                 m == "wakeUp" || m == "duplicate" || m == "createBox" ||
                 m == "createSphere" ||
                 m == "setMatrix" || m == "setSpeed" || m == "toString" || m == "id" ||
                 m == "scrollToLine" || m == "currentLine" ||
                 m == "createButton" || m == "createText" ||
                 m == "createTextBox" || m == "createHeader" ||
                 m == "createMenu" || m == "countTokens" ||
                 m == "nextToken" || m == "precache" || m == "timeWarp" ||
                 m == "addCustomGroups" || m == "addItem" ||
                 m == "addSeparator" || m == "setSliderStyle" ||
                 m == "enableAnimateHook" || m == "disableAnimateHook" ||
                 m == "printValue" || m == "setValue" || m == "token" ||
                 // OptionsDialog.show video-mode loop (else 0x1a → false JMP).
                 m == "numDisplayModes" || m == "currDisplayMode" ||
                 m == "displayModeName" || m == "changeVideoMode" ||
                 // PATH-TO-WORLD: Valocity/City enter·exit·startRace* must not
                 // fall through as JMP when imm collides with a node index —
                 // PE op33 Object_callMethod @ 0x408A30 is name-only invoke.
                 m == "enter" || m == "exit" ||
                 m == "startRace" || m == "startRace2" ||
                 m == "haltTrafficCross" ||
                 m == "haltTrafficPath" || m == "setParent" ||
                 m == "getVel" || m == "createCar" ||
                 m == "createQuickRaceBot" || m == "addNotification" ||
                 m == "queueEvent" ||
                 m == "add" || m == "mul" || m == "sub" ||
                 m == "changeActiveSection";
        };
        const bool named_mref =
            n.has_imm && n.imm < const_mref_name.size() &&
            is_inline_mref(const_mref_name[n.imm]);
        if (named_mref || (n.has_imm && n.imm >= tree.nodes.size())) {
          std::string mname = field_name(n.imm);
          if (mname.empty()) mname = cstr(n.imm);
          // Soft invoke ≡ PE op33 Object_callMethod: name-only. Curated
          // is_inline_mref (incl. "filter 2" GameRef cmds) stays trusted;
          // out-of-table pool imm still goes through garbage gate.
          if (!named_mref) {
            mname = resolve_invoke_name(mname);
            if (!soft_invoke_name_ok(mname)) break;
          }
          if (!mname.empty()) {
            // NEW+ARRAY packing immediately before setEventMask/addTimer.
            if ((mname == "setEventMask" || mname == "clearEventMask" ||
                 mname == "addTimer" || mname == "show" || mname == "hide") &&
                ip > 0 && tree.nodes[ip - 1].op == 0x24 && !stack.empty() &&
                stack.back().tag == JvmTag::Obj) {
              pop();
            }

            // Instance call: TREE often leaves recv on top; JVM leaves args on
            // top. Prefer recv-on-top when top is Obj of a plausible owner.
            std::vector<JvmValue> args;
            int argc = 0;
            if (mname == "addElement" || mname == "elementAt" ||
                mname == "addColorIndex")
              argc = 1;
            else if (mname == "size" || mname == "intValue" ||
                     mname == "getFirstChild" || mname == "getNextChild" ||
                     mname == "cache" || mname == "show" || mname == "hide" ||
                     mname == "createOSDObjects" || mname == "lockCar" ||
                     mname == "releaseCar" || mname == "remSceneElements" ||
                     mname == "endGroup" || mname == "beginGroup" ||
                     mname == "display" || mname == "flushInventory" ||
                     mname == "wakeUp" || mname == "destroy" ||
                     mname == "unload" || mname == "getPos" ||
                     mname == "getOri" || mname == "reset" ||
                     mname == "disableCameraControl" || mname == "neon" ||
                     mname == "cleanup" || mname == "activate" ||
                     mname == "removeAllElements" ||
                     mname == "toString" || mname == "id" ||
                     mname == "currentLine" || mname == "countTokens" ||
                     mname == "precache" || mname == "addCustomGroups" ||
                     mname == "enableAnimateHook" ||
                     mname == "disableAnimateHook" || mname == "addSeparator" ||
                     mname == "flush" || mname == "changePointer" ||
                     mname == "tesztmen" || mname == "deleteOSDObjects")
              argc = 0;
            else if (mname == "setDefaultTransmission" ||
                     mname == "setDefaultSteeringHelp" ||
                     mname == "setDefaultASR" || mname == "setDefaultABS")
              argc = 0;
            else if (mname.find(' ') != std::string::npos)
              argc = 0;
            else if (mname == "createBG" || mname == "setEventMask" ||
                     mname == "clearEventMask" || mname == "changeMode" ||
                     mname == "addSceneElements" || mname == "cameraSetup" ||
                     mname == "enable" || mname == "addHandler" ||
                     mname == "remHandler" || mname == "enableCameraControl" ||
                     mname == "filterInventory" || mname == "scrollToLine" ||
                     mname == "command" || mname == "setDamageMultiplier" ||
                     mname == "setCruiseControl" || mname == "duplicate" ||
                     mname == "play" || mname == "activateState" ||
                     mname == "nextToken" || mname == "hideGroup" ||
                     mname == "showGroup" || mname == "printValue" ||
                     mname == "setValue" || mname == "token" ||
                     mname == "createHeader" || mname == "checkHint" ||
                     mname == "enableOsd" || mname == "activateState" ||
                     mname == "osdCommand" || mname == "setPriority" ||
                     mname == "putMessage" || mname == "log" ||
                     mname == "changeCamTarget" || mname == "changeCamTarget2")
              argc = 1;
            else if (mname == "setTransmission" || mname == "leaveCar" ||
                     mname == "constructName" || mname == "set") {
              if (stack.size() >= 2 &&
                  (stack[stack.size() - 2].tag == JvmTag::Int ||
                   stack[stack.size() - 2].tag == JvmTag::Float ||
                   stack[stack.size() - 2].tag == JvmTag::Obj))
                argc = 1;
              else
                argc = 0;
            }
            else if (mname == "setPos") {
              if (stack.size() >= 3 &&
                  (stack[stack.size() - 2].tag == JvmTag::Float ||
                   stack[stack.size() - 2].tag == JvmTag::Int) &&
                  (stack[stack.size() - 3].tag == JvmTag::Float ||
                   stack[stack.size() - 3].tag == JvmTag::Int))
                argc = 2;
              else if (stack.size() >= 2 &&
                       stack[stack.size() - 2].tag == JvmTag::Obj)
                argc = 1;
              else
                argc = 0;
            }
            else if (mname == "userWait") {
              argc = 0;
              if (stack.size() >= 2 &&
                  (stack[stack.size() - 2].tag == JvmTag::Float ||
                   stack[stack.size() - 2].tag == JvmTag::Int))
                argc = 1;
            }
            else if (mname == "setSliderStyle" || mname == "setRange" ||
                     mname == "remNotification")
              argc = 2;
            else if (mname == "addItem") {
              // Stock Menu.addItem(String,int)=2. Size heuristics stole the
              // prior addItem Button and dropped the label (cmd→0).
              int base = 0;
              if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                const char* hc = tree_host_class(stack.back().v.o);
                if (hc && std::strstr(hc, "Menu")) base = 1;
              }
              auto at = [&](int i) -> const JvmValue* {
                if (i < 0 || static_cast<size_t>(i) >= stack.size())
                  return nullptr;
                return &stack[stack.size() - 1 - static_cast<size_t>(i)];
              };
              int floats = 0;
              bool multi = false;
              bool icon_cmd = false;
              for (int i = base; i < base + 8; ++i) {
                const JvmValue* v = at(i);
                if (!v) break;
                if (v->tag == JvmTag::Float) ++floats;
                if (v->tag == JvmTag::Int && is_icon_osd_cmd(v->v.i))
                  icon_cmd = true;
                if (v->tag == JvmTag::Obj && v->v.o) {
                  const char* hc = tree_host_class(v->v.o);
                  if (hc && (std::strstr(hc, "Vector") || hc[0] == '['))
                    multi = true;
                }
              }
              if (multi)
                argc = 5;
              else if (icon_cmd && recent_new) {
                const char* rhc = tree_host_class(recent_new);
                if (rhc && (std::strstr(rhc, "ResourceRef") ||
                            std::strstr(rhc, "GameRef")))
                  argc = 3;
                else if (floats >= 3)
                  argc = 7;
                else if (floats >= 1)
                  argc = 4;
                else
                  argc = 3;
              } else if (floats >= 3)
                argc = 7;
              else if (floats >= 1)
                argc = 4;
              else {
                bool icon = false;
                int strs = 0, ints = 0;
                for (int i = base; i < base + 4; ++i) {
                  const JvmValue* v = at(i);
                  if (!v) break;
                  if (v->tag == JvmTag::Int) ++ints;
                  if (v->tag == JvmTag::Obj && v->v.o) {
                    const char* hc = tree_host_class(v->v.o);
                    if (hc && (std::strstr(hc, "ResourceRef") ||
                               std::strstr(hc, "GameRef")))
                      icon = true;
                    if (!hc || !hc[0] || std::strstr(hc, "String")) ++strs;
                  }
                }
                if (icon || (strs >= 2 && ints >= 1))
                  argc = 3;
                else
                  argc = 2;
              }
            }
            else if (mname == "createMenu") {
              // Same as argc_for_method: ignore leftover ResourceRefs on stack.
              int base = 0;
              if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
                const char* hc = tree_host_class(stack.back().v.o);
                if (hc && std::strstr(hc, "Osd")) base = 1;
              }
              int n = 0;
              for (int i = base; i < base + 8 &&
                   static_cast<size_t>(i) < stack.size();
                   ++i) {
                const JvmValue& v = stack[stack.size() - 1 - static_cast<size_t>(i)];
                if (v.tag == JvmTag::Float || v.tag == JvmTag::Int) {
                  ++n;
                  continue;
                }
                if (v.tag == JvmTag::Obj && v.v.o) {
                  const char* hc = tree_host_class(v.v.o);
                  if (hc && (std::strstr(hc, "Style") ||
                             std::strstr(hc, "ResourceRef"))) {
                    ++n;
                    continue;
                  }
                }
                break;
              }
              argc = n >= 6 ? 6 : (n >= 4 ? n : 5);
            }
            else if (mname == "addTimer" || mname == "giveWarning" ||
                     mname == "setMatrix")
              argc = 2;
            else if (mname == "setSpeed" || mname == "setFade" ||
                     mname == "seek")
              argc = 1;
            else if (mname == "createText")
              argc = 5;
            else if (mname == "numDisplayModes" || mname == "currDisplayMode")
              argc = 0;
            else if (mname == "displayModeName")
              argc = 1;
            else if (mname == "changeVideoMode")
              argc = 3;
            else if (mname == "createButton") {
              argc = 5;
              // Shield: (style,x,y,pri,label,cmd) — pri+label+cmd on top.
              if (stack.size() >= 7 && stack.back().tag == JvmTag::Int &&
                  stack[stack.size() - 2].tag == JvmTag::Obj &&
                  stack[stack.size() - 3].tag == JvmTag::Int)
                argc = 6;
            }
            else if (mname == "createTextBox")
              argc = 6;
            else if (mname == "createBox")
              argc = 5;
            else if (mname == "createSphere")
              argc = 3;
            else if (mname == "createHotkey") {
              argc = 4;
              if (stack.size() >= 6 &&
                  stack[stack.size() - 2].tag == JvmTag::Int &&
                  stack[stack.size() - 3].tag == JvmTag::Obj)
                argc = 5;
            }
            else if (mname == "addTraffic" || mname == "addTrafficN")
              argc = 5;
            else if (mname == "queueEvent")
              argc = 3;
            else if (mname == "create") {
              if ((!stack.empty() && stack.back().tag == JvmTag::Obj &&
                   tree_is_renderref(stack.back().v.o)) ||
                  (!locals.empty() && locals[0].tag == JvmTag::Obj &&
                   tree_is_renderref(locals[0].v.o)))
                argc = 3;
            }
            else if (mname == "enter" || mname == "exit")
              argc = 1;
            else if (mname == "setPedestrianDensity" ||
                     mname == "setPedestrianDensityN" ||
                     mname == "addPedestrianType" ||
                     mname == "remPedestrianType" ||
                     mname == "remTrafficCar" || mname == "delTraffic")
              argc = (mname == "delTraffic") ? 0 : 1;
            else if (mname == "addTrafficCar" || mname == "notifyTrafficCar" || mname == "setTrafficCarBehaviour" ||
                     mname == "addWaterLimit")
              argc = 2;
            else if (mname == "getNearestCross") {
              if (stack.size() >= 2 &&
                  (stack.back().tag == JvmTag::Float ||
                   stack.back().tag == JvmTag::Int) &&
                  stack[stack.size() - 2].tag == JvmTag::Obj)
                argc = 2;
              else
                argc = 1;
            }
            else if (mname == "alignToRoad" || mname == "time2Config")
              argc = 1;
            else if (mname == "getStartDirection" ||
                     mname == "haltTrafficCross" || mname == "haltTrafficPath")
              argc = 2;
            else if (mname == "startRace")
              argc = 3;
            else if (mname == "startRace2")
              argc = 0;
            else if (mname == "mul" || mname == "add" || mname == "sub" ||
                     mname == "setParent")
              argc = 1;
            else if (mname == "diff")
              argc = 2;
            else if (mname == "getVel" || mname == "stop" ||
                     mname == "createQuickRaceBot")
              argc = 0;
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
            }
            else if (mname == "findRoute")
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
            }
            else if (mname == "remMarker" || mname == "updateNavigator")
              argc = 1;
            else if (mname == "changeZoom")
              argc = 1;
            else if (mname == "changeSize")
              argc = 4;
            else if (mname == "setWater")
              argc = 4;
            else if (mname == "addTrigger")
              argc = 6;
            else if (mname == "addNotification") {
              argc = 4;
              if (stack.size() >= 5 && stack.back().tag == JvmTag::Obj) {
                const JvmValue& a2 = stack[stack.size() - 3];
                const JvmValue& a3 = stack[stack.size() - 4];
                if (a2.tag == JvmTag::Int && a3.tag == JvmTag::Int) argc = 5;
              }
            }

            JvmValue recv = JvmValue::make_obj(nullptr);
            // GfxEngine statics — OptionsDialog.show bounds videoModes loop.
            if (mname == "numDisplayModes" || mname == "currDisplayMode" ||
                mname == "displayModeName" || mname == "changeVideoMode") {
              std::vector<JvmValue> sargs;
              for (int i = 0; i < argc; ++i) {
                if (!stack.empty())
                  sargs.insert(sargs.begin(), pop());
                else
                  sargs.insert(sargs.begin(), JvmValue::make_int(0));
              }
              JvmValue r = host->call_by_name("java.render.GfxEngine",
                                              mname.c_str(), sargs, true);
              if (r.tag != JvmTag::Void) push(r);
              if (pending_local >= 0 && r.tag != JvmTag::Void) {
                ensure_local(static_cast<size_t>(pending_local));
                locals[static_cast<size_t>(pending_local)] = r;
                pending_local = -1;
              }
              break;
            }
            if (mname == "changeActiveSection") {
              // Stock GameLogic.changeActiveSection(state) — static. RaceSetup
              // osdCommand 0x1a otherwise binds Valocity argc=0 and skips CAS.
              // PE Object_callMethod: pass stack operand only — do not invent
              // Valocity/City lastState (that freestyle skipped RaceSetup /
              // new-section enter). RaceSetup/Track → track|lastState matches
              // stock RaceSetup.osdCommand CAS(track) when TREE left recv junk.
              JvmValue next = JvmValue::make_obj(nullptr);
              if (!stack.empty()) next = pop();
              if (next.tag != JvmTag::Obj || !game_logic_is_section(next.v.o)) {