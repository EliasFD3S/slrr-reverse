// Fragment 2/2 of jvm.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


    // java.render.Rectangle(group, pos, tmpl) — TREE names osd.Rectangle (no .class).
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      const char* hc = tree_host_class(self);
      const bool is_rect =
          (class_fqn && std::strstr(class_fqn, "Rectangle")) ||
          (hc && std::strstr(hc, "Rectangle"));
      if (is_rect) {
        InvObject* group = nullptr;
        InvObject* pos = nullptr;
        InvObject* tmpl = nullptr;
        InvObject* tex = nullptr;
        float x = 0.f, y = 0.f, w = 2.f, h = 2.f;
        int32_t pri = -1;
        int nf = 0;
        for (size_t i = 1; i < pargs->size(); ++i) {
          const JvmValue& a = (*pargs)[i];
          if (a.tag == JvmTag::Obj && a.v.o) {
            const char* ac = tree_host_class(a.v.o);
            if (ac && std::strstr(ac, "Vector3"))
              pos = a.v.o;
            else if (ac && std::strstr(ac, "Template"))
              tmpl = a.v.o;
            else if (ac && (std::strstr(ac, "ResourceRef") ||
                            std::strstr(ac, "Group") ||
                            std::strstr(ac, "Osd"))) {
              if (!group) group = a.v.o;
              if (ac && std::strstr(ac, "ResourceRef")) tex = a.v.o;
            }
          } else if (a.tag == JvmTag::Float || a.tag == JvmTag::Int) {
            const float f = a.tag == JvmTag::Float ? a.v.f
                                                   : static_cast<float>(a.v.i);
            if (nf == 0) x = f;
            else if (nf == 1) y = f;
            else if (nf == 2) w = f;
            else if (nf == 3) h = f;
            else if (a.tag == JvmTag::Int) pri = a.v.i;
            ++nf;
          }
        }
        if (pos) {
          x = tree_field_get_float(pos, "x");
          y = tree_field_get_float(pos, "y");
        }
        if (tmpl) {
          const float tw = tree_field_get_float(tmpl, "width");
          const float th = tree_field_get_float(tmpl, "height");
          if (tw != 0.f) w = tw;
          if (th != 0.f) h = th;
          if (!tex) tex = tree_field_get_obj(tmpl, "texture");
        }
        tree_field_set_obj(self, "root", group);
        tree_field_set_obj(self, "pos", pos);
        tree_field_set_obj(self, "rt", tmpl);
        tree_field_set_obj(self, "texture", tex);
        tree_field_set_float(self, "x", x);
        tree_field_set_float(self, "y", y);
        tree_field_set_float(self, "w", w);
        tree_field_set_float(self, "h", h);
        tree_field_set_int(self, "pri", pri);
        InvObject* osd = group;
        if (osd) {
          const char* gc = tree_host_class(osd);
          if (gc && std::strstr(gc, "Group"))
            osd = tree_field_get_obj(group, "osd");
          else if (gc && !std::strstr(gc, "Osd"))
            osd = nullptr;
        }
        if (osd) {
          osd_ensure_defaults(osd);
          InvObject* rects = tree_field_get_obj(osd, "rectangles");
          if (rects) tree_vector_add(rects, self);
        }
        if (tex) java_util_resource_ResourceRef_load(tex);
        render_d3d9_osd_add_rect(x, y, w, h, tex, pri);
        return JvmValue::make_void();
      }
    }

    // java.render.osd.Menu(Osd, Style, x, y, spc[, ori]) — TREE argc=0.
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      const char* hc = tree_host_class(self);
      const bool is_menu =
          ((class_fqn && std::strstr(class_fqn, "Menu") &&
            !std::strstr(class_fqn, "MainMenu")) ||
           (hc && std::strstr(hc, "Menu") && !std::strstr(hc, "MainMenu")));
      if (is_menu) {
        InvObject* osd = nullptr;
        InvObject* style = nullptr;
        float x = 0.f, y = 0.f, spc = 0.f;
        int32_t ori = 0;
        std::vector<float> floats;
        std::vector<int32_t> ints;
        for (size_t i = 1; i < pargs->size(); ++i) {
          const JvmValue& a = (*pargs)[i];
          if (a.tag == JvmTag::Obj && a.v.o) {
            const char* ac = tree_host_class(a.v.o);
            if (ac && std::strstr(ac, "Osd") && !std::strstr(ac, "dialog") &&
                !std::strstr(ac, "Rectangle") && !osd)
              osd = a.v.o;
            else if (ac && std::strstr(ac, "Style") && !style)
              style = a.v.o;
          } else if (a.tag == JvmTag::Float)
            floats.push_back(a.v.f);
          else if (a.tag == JvmTag::Int)
            ints.push_back(a.v.i);
        }
        if (floats.size() >= 1) x = floats[0];
        if (floats.size() >= 2) y = floats[1];
        if (floats.size() >= 3) spc = floats[2];
        if (!ints.empty()) ori = ints.back();
        if (!osd) osd = tree_field_get_obj(self, "osd");
        InvObject* made = osd_create_menu(osd, style, x, y, spc, ori);
        if (made && made != self) {
          tree_field_set_obj(self, "osd", tree_field_get_obj(made, "osd"));
          tree_field_set_obj(self, "sty", tree_field_get_obj(made, "sty"));
          tree_field_set_obj(self, "items", tree_field_get_obj(made, "items"));
          tree_field_set_int(self, "item_count",
                             tree_field_get_int(made, "item_count"));
          tree_field_set_float(self, "x", tree_field_get_float(made, "x"));
          tree_field_set_float(self, "y", tree_field_get_float(made, "y"));
          tree_field_set_float(self, "spacing",
                               tree_field_get_float(made, "spacing"));
          tree_field_set_int(self, "orientation",
                             tree_field_get_int(made, "orientation"));
        }
        if (osd) tree_field_set_obj(osd, "last_menu", self);
        return JvmValue::make_void();
      }
    }

    // ResourceRef(int id) — RaceSetup/Garage `new ResourceRef(Osd.RID_*)`.
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      const char* hc = tree_host_class(self);
      const bool is_rr =
          (hc && std::strstr(hc, "ResourceRef")) ||
          (class_fqn && std::strstr(class_fqn, "ResourceRef"));
      if (is_rr && pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Int) {
        java_util_resource_ResourceRef_set(self, (*pargs)[1].v.i);
        return JvmValue::make_void();
      }
    }

    // Style(float,float[,aspect],charset,align,bg) — TREE mis-routes to
    // Animation.<init> via cross-class native argc match.
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      const char* hc = tree_host_class(self);
      const bool is_style =
          (hc && std::strstr(hc, "Style")) ||
          (class_fqn && std::strstr(class_fqn, "Style"));
      if (is_style) {
        float w = 0.45f, h = 0.12f, aspect = 4.f / 3.f;
        int32_t align = 1;
        InvObject* charset = nullptr;
        InvObject* bg = nullptr;
        std::vector<float> floats;
        std::vector<int32_t> ints;
        for (size_t i = 1; i < pargs->size(); ++i) {
          const JvmValue& a = (*pargs)[i];
          if (a.tag == JvmTag::Float)
            floats.push_back(a.v.f);
          else if (a.tag == JvmTag::Int)
            ints.push_back(a.v.i);
          else if (a.tag == JvmTag::Obj && a.v.o) {
            if (!charset)
              charset = a.v.o;
            else
              bg = a.v.o;
          }
        }
        if (floats.size() >= 1) w = floats[0];
        if (floats.size() >= 2) h = floats[1];
        if (floats.size() >= 3) aspect = floats[2];
        if (!ints.empty()) align = ints.back();
        tree_field_set_float(self, "width", w);
        tree_field_set_float(self, "height", h);
        tree_field_set_float(self, "aspect", aspect);
        tree_field_set_int(self, "align", align);
        if (charset) tree_field_set_obj(self, "charset", charset);
        if (bg) tree_field_set_obj(self, "background", bg);
        tree_field_set_float(self, "rWidth", w * 1.47f * aspect);
        tree_field_set_float(self, "rHeight", h * 1.47f);
        return JvmValue::make_void();
      }
    }

    // Trigger(parent, type, pos[, r], alias) — TREE this() often arrives argc=0.
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      const char* hc = tree_host_class(self);
      const bool is_trig =
          (hc && std::strstr(hc, "Trigger")) ||
          (class_fqn && std::strstr(class_fqn, "Trigger"));
      if (is_trig && trigger_apply_ctor(self, *pargs))
        return JvmValue::make_void();
      const bool is_pcar =
          (hc && std::strstr(hc, "ParkingCar")) ||
          (class_fqn && std::strstr(class_fqn, "ParkingCar"));
      if (is_pcar && parking_car_apply_ctor(self, *pargs))
        return JvmValue::make_void();
    }

    // GameRef(parent, type|rid, params, alias) → create native @ 0x0047D7B0.
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        class_fqn && std::strstr(class_fqn, "GameRef") && !pargs->empty() &&
        (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      if (pargs->size() == 2 && (*pargs)[1].tag == JvmTag::Int) {
        java_util_resource_ResourceRef_set(self, (*pargs)[1].v.i);
        return JvmValue::make_void();
      }
      if (pargs->size() >= 5) {
        InvObject* parent =
            (*pargs)[1].tag == JvmTag::Obj ? (*pargs)[1].v.o : nullptr;
        InvObject* type = nullptr;
        if ((*pargs)[2].tag == JvmTag::Int) {
          type = gameref_new();
          java_util_resource_ResourceRef_set(type, (*pargs)[2].v.i);
        } else if ((*pargs)[2].tag == JvmTag::Obj) {
          type = (*pargs)[2].v.o;
        }
        InvObject* params =
            (*pargs)[3].tag == JvmTag::Obj ? (*pargs)[3].v.o : nullptr;
        InvObject* alias =
            (*pargs)[4].tag == JvmTag::Obj ? (*pargs)[4].v.o : nullptr;
        java_util_resource_GameRef_create(self, parent, type, params, alias);
        return JvmValue::make_void();
      }
    }

    // RenderRef(parent, type|rid, alias) → create @ 0x00480EE0 (not GameRef 4-arg).
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o &&
        is_renderref_not_camera((*pargs)[0].v.o, class_fqn)) {
      InvObject* self = (*pargs)[0].v.o;
      if (pargs->size() == 2 && (*pargs)[1].tag == JvmTag::Int) {
        java_util_resource_ResourceRef_set(self, (*pargs)[1].v.i);
        return JvmValue::make_void();
      }
      if (pargs->size() >= 3) {
        renderref_apply_create(*pargs);
        return JvmValue::make_void();
      }
    }

    // VehicleModel(int id, int mask) — TREE <init> never writes id (stays 0).
    if (!prefer_static && method && class_fqn &&
        std::strcmp(class_fqn, "java.game.VehicleModel") == 0 &&
        std::strcmp(method, "<init>") == 0 && !pargs->empty() &&
        (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      int32_t id = 0;
      int32_t mask = 0x3f;
      if (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Int)
        id = (*pargs)[1].v.i;
      if (pargs->size() >= 3 && (*pargs)[2].tag == JvmTag::Int)
        mask = (*pargs)[2].v.i;
      tree_field_set_int(self, "id", id);
      tree_field_set_int(self, "vehicleSetMask", mask);
      return JvmValue::make_void();
    }

    if (!jvm_->find_class(class_fqn)) jvm_->load_class(class_fqn);
    const JvmClass* cls = jvm_->find_class(class_fqn);
    const int argc =
        prefer_static ? static_cast<int>(pargs->size())
                      : (pargs->empty() ? 0 : static_cast<int>(pargs->size()) - 1);

    // Math.random() — TREE binds onto Valocity/City (createQuickRaceBot).
    if (method && std::strcmp(method, "random") == 0 && argc == 0)
      return JvmValue::make_float(java_lang_Math_random());

    // System.exit(String) stolen as Bot.exit when botVd is null.
    if (method && std::strcmp(method, "exit") == 0) {
      InvObject* msg = nullptr;
      for (size_t i = prefer_static ? 0 : 1; i < pargs->size(); ++i) {
        if ((*pargs)[i].tag == JvmTag::Obj && (*pargs)[i].v.o &&
            string_cstr((*pargs)[i].v.o)) {
          msg = (*pargs)[i].v.o;
          break;
        }
      }
      const char* hc0 =
          (!prefer_static && !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj)
              ? tree_host_class((*pargs)[0].v.o)
              : nullptr;
      const bool gamestate =
          hc0 && (std::strstr(hc0, "Splash") || std::strstr(hc0, "MainMenu") ||
                  std::strstr(hc0, "Garage") || std::strstr(hc0, "Valocity") ||
                  std::strstr(hc0, "City") || std::strstr(hc0, "Track") ||
                  std::strstr(hc0, "RaceSetup") || std::strstr(hc0, "Dialog"));
      if (msg && !gamestate) {
        std::fprintf(stderr, "[script] System.exit stolen: %s\n",
                     string_cstr(msg));
        return JvmValue::make_void();
      }
      if ((hc0 && std::strstr(hc0, "Bot")) ||
          (class_fqn && std::strstr(class_fqn, "Bot"))) {
        std::fprintf(stderr, "[script] System.exit stolen as Bot.exit\n");
        return JvmValue::make_void();
      }
    }

    // GameType.addNotification @ 0x0047E050 / 0x0047E100 → Watch @ 0x0048B500.
    // TREE permutes (ref,etype,ealias,msg[,method]) into unsupported JNI shapes.
    if (!prefer_static && method && std::strcmp(method, "addNotification") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      InvObject* ref = nullptr;
      InvObject* msg = nullptr;
      InvObject* meth = nullptr;
      int32_t etype = 0, ealias = 0;
      int nint = 0;
      for (size_t i = 1; i < pargs->size(); ++i) {
        const JvmValue& a = (*pargs)[i];
        if (a.tag == JvmTag::Int || a.tag == JvmTag::Float) {
          const int32_t v = a.tag == JvmTag::Int
                                ? a.v.i
                                : static_cast<int32_t>(a.v.f);
          if (nint == 0)
            etype = v;
          else if (nint == 1)
            ealias = v;
          ++nint;
          continue;
        }
        if (a.tag != JvmTag::Obj) continue;
        if (!a.v.o) continue;
        if (string_cstr(a.v.o) &&
            (!tree_host_class(a.v.o)[0] ||
             std::strstr(tree_host_class(a.v.o), "String"))) {
          if (!meth)
            meth = a.v.o;
          else
            msg = a.v.o;
          continue;
        }
        if (!ref) ref = a.v.o;
      }
      if (meth && string_cstr(meth) &&
          std::strncmp(string_cstr(meth), "event_", 6) == 0) {
        /* method name in meth, msg stays null */
      } else if (meth && !msg) {
        msg = meth;
        meth = nullptr;
      }
      if (meth)
        java_lang_GameType_addNotification_1(self, ref, etype, ealias, msg,
                                             meth);
      else
        java_lang_GameType_addNotification(self, ref, etype, ealias, msg);
      return JvmValue::make_void();
    }

    // Vector3.add/mul/sub — Java TREE; recv often ResourceRef leftover.
    if (method &&
        (std::strcmp(method, "add") == 0 || std::strcmp(method, "mul") == 0 ||
         std::strcmp(method, "sub") == 0)) {
      auto is_v3 = [](InvObject* o) -> bool {
        if (!o) return false;
        const char* c = tree_host_class(o);
        if (c && std::strstr(c, "Vector3")) return true;
        if (c && c[0]) return false;
        if (string_cstr(o)) return false;
        return vec3_is(o);
      };
      InvObject* recv = nullptr;
      for (size_t i = 0; i < pargs->size(); ++i) {
        if ((*pargs)[i].tag == JvmTag::Obj && is_v3((*pargs)[i].v.o)) {
          recv = (*pargs)[i].v.o;
          break;
        }
      }
      if (is_v3(recv)) {
        float x = 0, y = 0, z = 0;
        vec3_get(recv, &x, &y, &z);
        if (x == 0.f && y == 0.f && z == 0.f) {
          x = tree_field_get_float(recv, "x");
          y = tree_field_get_float(recv, "y");
          z = tree_field_get_float(recv, "z");
        }
        float scale = 1.f;
        bool have_scale = false;
        InvObject* oth = nullptr;
        for (size_t i = 0; i < pargs->size(); ++i) {
          const JvmValue& a = (*pargs)[i];
          if (a.tag == JvmTag::Float || a.tag == JvmTag::Int) {
            scale = a.tag == JvmTag::Float ? a.v.f : static_cast<float>(a.v.i);
            have_scale = true;
          } else if (a.tag == JvmTag::Obj && a.v.o && a.v.o != recv && !oth)
            oth = a.v.o;
        }
        if (std::strcmp(method, "mul") == 0 && have_scale) {
          x *= scale;
          y *= scale;
          z *= scale;
        } else if (oth) {
            float ox = 0, oy = 0, oz = 0;
            vec3_get(oth, &ox, &oy, &oz);
            if (ox == 0.f && oy == 0.f && oz == 0.f) {
              ox = tree_field_get_float(oth, "x");
              oy = tree_field_get_float(oth, "y");
              oz = tree_field_get_float(oth, "z");
            }
            if (std::strcmp(method, "sub") == 0) {
              x -= ox;
              y -= oy;
              z -= oz;
            } else if (std::strcmp(method, "mul") == 0) {
              x *= ox;
              y *= oy;
              z *= oz;
            } else {
              x += ox;
              y += oy;
              z += oz;
            }
        }
        vec3_set(recv, x, y, z);
        tree_field_set_float(recv, "x", x);
        tree_field_set_float(recv, "y", y);
        tree_field_set_float(recv, "z", z);
        return JvmValue::make_obj(recv);
      }
      // Stolen Vector3.add/mul/sub onto City/Valocity/ResourceRef.
      return JvmValue::make_void();
    }

    // VehicleDescriptor has no 2-arg Java ctor — TREE leftover (id, colorIndex).
    if (!prefer_static && method && std::strcmp(method, "<init>") == 0 &&
        class_fqn && std::strstr(class_fqn, "VehicleDescriptor") &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      if (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Int)
        tree_field_set_int(self, "id", (*pargs)[1].v.i);
      if (pargs->size() >= 3 && (*pargs)[2].tag == JvmTag::Int)
        tree_field_set_int(self, "colorIndex", (*pargs)[2].v.i);
      if (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Obj &&
          (*pargs)[1].v.o) {
        InvObject* src = (*pargs)[1].v.o;
        tree_field_set_int(self, "id", tree_field_get_int(src, "id"));
        tree_field_set_int(self, "colorIndex",
                           tree_field_get_int(src, "colorIndex"));
        tree_field_set_float(self, "power", tree_field_get_float(src, "power"));
        tree_field_set_float(self, "optical",
                             tree_field_get_float(src, "optical"));
        tree_field_set_float(self, "wear", tree_field_get_float(src, "wear"));
        tree_field_set_float(self, "tear", tree_field_get_float(src, "tear"));
      }
      return JvmValue::make_void();
    }

    // PhysicsRef.createBox(parent,x,y,z,alias) — TREE shape noise.
    if (!prefer_static && method && std::strcmp(method, "createBox") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      InvObject* parent = nullptr;
      InvObject* alias = nullptr;
      float xyz[3] = {1.f, 0.5f, 2.f};
      int nf = 0;
      for (size_t i = 1; i < pargs->size(); ++i) {
        const JvmValue& a = (*pargs)[i];
        if (a.tag == JvmTag::Float || a.tag == JvmTag::Int) {
          if (nf < 3)
            xyz[nf++] = a.tag == JvmTag::Float ? a.v.f
                                               : static_cast<float>(a.v.i);
        } else if (a.tag == JvmTag::Obj && a.v.o) {
          if (string_cstr(a.v.o))
            alias = a.v.o;
          else if (!parent)
            parent = a.v.o;
        }
      }
      java_util_resource_PhysicsRef_createBox(self, parent, xyz[0], xyz[1],
                                              xyz[2], alias);
      return JvmValue::make_void();
    }

    // GameRef.queueEvent(ResourceRef,I,String)V @ 0x0047DA30.
    // TREE 0x12 defaults static argc=0 (MouseCursor/Player/Vehicle/Bot miss).
    if (method && std::strcmp(method, "queueEvent") == 0) {
      if (argc == 0) return JvmValue::make_void();
      queue_event_apply(*pargs);
      return JvmValue::make_void();
    }

    // RenderRef.create(parent, type, alias)V @ 0x00480EE0.
    // TREE 0x12 defaults static argc=0; Java wrapper re-invokes native create.
    // Camera.create is a different native — skip Camera recv / FQN.
    if (method && std::strcmp(method, "create") == 0 && !pargs->empty() &&
        (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      if (is_renderref_not_camera(self, class_fqn)) {
        if (argc == 0) return JvmValue::make_void();
        renderref_apply_create(*pargs);
        return JvmValue::make_void();
      }
    }

    if (!prefer_static && method && std::strcmp(method, "createCar") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      const char* hc = tree_host_class(self);
      const bool is_bot = hc && std::strstr(hc, "Bot");
      auto ok_bot = [&](InvObject* o) -> bool {
        if (!o || o == self) return false;
        const char* c = tree_host_class(o);
        if (!c || !c[0]) return true;
        return std::strstr(c, "Bot") != nullptr;
      };
      // Bot.createCar(map, Vehicle c) stolen onto Vehicle — host leaf, no
      // retarget (1-arg TREE ↔ 2-arg Vehicle ping-pongs).
      if (hc && std::strstr(hc, "Vehicle")) {
        InvObject* bot = nullptr;
        InvObject* map = nullptr;
        InvObject* car = self;
        for (size_t i = 1; i < pargs->size(); ++i) {
          if ((*pargs)[i].tag != JvmTag::Obj || !(*pargs)[i].v.o) continue;
          InvObject* o = (*pargs)[i].v.o;
          const char* ac = tree_host_class(o);
          if (ac && std::strstr(ac, "Bot"))
            bot = o;
          else if (ac && std::strstr(ac, "GroundRef"))
            map = o;
          else if (ac && std::strstr(ac, "Vehicle") && o != self)
            car = o;
          else if (!map)
            map = o;
        }
        if (!ok_bot(bot)) {
          InvObject* st = game_logic_actual_state();
          bot = st ? tree_field_get_obj(st, "raceBot") : nullptr;
        }
        if (ok_bot(bot)) {
          if (map) tree_field_set_obj(bot, "world", map);
          tree_field_set_obj(bot, "car", car);
          std::vector<JvmValue> ec = {JvmValue::make_obj(bot),
                                      JvmValue::make_obj(car)};
          call_by_name("java.game.Bot", "enterCar", ec, false);
        }
        return JvmValue::make_void();
      }
      const bool stolen =
          hc && (std::strstr(hc, "Valocity") || std::strstr(hc, "City") ||
                 std::strstr(hc, "Track") || std::strstr(hc, "RaceSetup"));
      if (is_bot) {
        if (!tree_field_get_obj(self, "botVd"))
          tree_field_set_obj(self, "botVd",
                             game_logic_get_vehicle_descriptor(1, -1.f));
        // createCar(map) TREE often binds the filename overload → System.exit.
        // Java: new Vehicle(this, botVd.*) then createCar(map, vhc).
        if (argc <= 1) {
          InvObject* vd = tree_field_get_obj(self, "botVd");
          InvObject* map = nullptr;
          if (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Obj)
            map = (*pargs)[1].v.o;
          if (!map) map = tree_field_get_obj(self, "world");
          InvObject* car = tree_field_get_obj(self, "car");
          if (!car) {
            car = tree_host_new("java.game.Vehicle");
            int32_t rid = vd ? tree_field_get_int(vd, "id") : 0;
            if (!rid && vd) rid = java_util_resource_ResourceRef_id(vd);
            if (rid) {
              java_util_resource_ResourceRef_set(car, rid);
              tree_field_set_int(car, "id", rid);
            }
            if (vd) {
              tree_field_set_float(car, "power",
                                   tree_field_get_float(vd, "power"));
              tree_field_set_float(car, "optical",
                                   tree_field_get_float(vd, "optical"));
              tree_field_set_float(car, "tear",
                                   tree_field_get_float(vd, "tear"));
              tree_field_set_float(car, "wear",
                                   tree_field_get_float(vd, "wear"));
              tree_field_set_int(car, "colorIndex",
                                 tree_field_get_int(vd, "colorIndex"));
            }
            tree_field_set_int(car, "driveable", 1);
            tree_field_set_obj(car, "owner", self);
          }
          if (map) tree_field_set_obj(self, "world", map);
          tree_field_set_obj(self, "car", car);
          std::vector<JvmValue> ec = {JvmValue::make_obj(self),
                                      JvmValue::make_obj(car)};
          call_by_name("java.game.Bot", "enterCar", ec, false);
          return JvmValue::make_void();
        }
      } else if (stolen) {
        InvObject* bot = tree_field_get_obj(self, "raceBot");
        if (!ok_bot(bot)) {
          InvObject* st = game_logic_actual_state();
          bot = st ? tree_field_get_obj(st, "raceBot") : nullptr;
        }
        if (ok_bot(bot)) {
          if (!tree_field_get_obj(bot, "botVd"))
            tree_field_set_obj(bot, "botVd",
                               game_logic_get_vehicle_descriptor(1, -1.f));
          std::vector<JvmValue> a = {JvmValue::make_obj(bot)};
          for (size_t i = 1; i < pargs->size(); ++i) a.push_back((*pargs)[i]);
          return call_by_name("java.game.Bot", "createCar", a, false);
        }
      }
    }

    // GameType.java wrappers → registerCallback natives (PE @ 0x00481C40).
    if (!prefer_static && method && !pargs->empty() &&
        (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* self = (*pargs)[0].v.o;
      if (std::strcmp(method, "enableAnimateHook") == 0 && argc == 0) {
        java_lang_GameType_registerCallback(self, 28);  // GII_ANIMATE
        return JvmValue::make_void();
      }
      if (std::strcmp(method, "disableAnimateHook") == 0 && argc == 0) {
        java_lang_GameType_unregisterCallback(self, 28);
        return JvmValue::make_void();
      }
      if (std::strcmp(method, "enableControlHook") == 0 && argc == 0) {
        java_lang_GameType_registerCallback(self, 8);  // GII_CONTROL
        return JvmValue::make_void();
      }
      if (std::strcmp(method, "disableControlHook") == 0 && argc == 0) {
        java_lang_GameType_unregisterCallback(self, 8);
        return JvmValue::make_void();
      }
      // Track/City/Nav/map methods mis-bound onto RaceSetup during CAS.
      const char* hc = tree_host_class(self);
      if (hc && (std::strstr(hc, "RaceSetup") || std::strstr(hc, "Valocity") ||
                 std::strstr(hc, "City") || std::strstr(hc, "Track"))) {
        InvObject* track = tree_field_get_obj(self, "track");
        if (!track) track = tree_field_get_obj(self, "lastState");
        // Host handoff / TREE putfield may lag — prev_state is Valocity.
        if (!track && pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Obj)
          track = (*pargs)[1].v.o;
        if (!track) {
          InvObject* st = game_logic_actual_state();
          const char* sc = st ? tree_host_class(st) : nullptr;
          if (sc && (std::strstr(sc, "Valocity") || std::strstr(sc, "City")))
            track = st;
        }
        InvObject* map = track ? tree_field_get_obj(track, "map") : nullptr;
        if (!map) map = tree_field_get_obj(self, "map");
        InvObject* nav = track ? tree_field_get_obj(track, "nav") : nullptr;
        if (!nav && self) nav = tree_field_get_obj(self, "nav");
        InvObject* osd = tree_field_get_obj(self, "osd");
        if (!osd && track) osd = tree_field_get_obj(track, "osd");
        InvObject* raceBot = tree_field_get_obj(self, "raceBot");
        if (!raceBot && track) raceBot = tree_field_get_obj(track, "raceBot");
        auto host_has = [](InvObject* o, const char* n) -> bool {
          const char* c = o ? tree_host_class(o) : nullptr;
          return c && std::strstr(c, n);
        };
        auto host_has_or_empty = [](InvObject* o, const char* n) -> bool {
          const char* c = o ? tree_host_class(o) : nullptr;
          if (!c) return false;
          if (!c[0]) return true;
          return std::strstr(c, n) != nullptr;
        };
        auto retarget = [&](InvObject* recv, const char* owner) -> JvmValue {
          if (!recv || recv == self) return JvmValue::make_void();
          std::vector<JvmValue> a = {JvmValue::make_obj(recv)};
          for (size_t i = 1; i < pargs->size(); ++i)
            a.push_back((*pargs)[i]);
          return call_by_name(owner, method, a, false);
        };
        if (hc && std::strstr(hc, "RaceSetup") && track && track != self &&
            (std::strcmp(method, "createQuickRaceBot") == 0 ||
             std::strcmp(method, "destroyRaceBot") == 0 ||
             std::strcmp(method, "startRace2") == 0 ||
             std::strcmp(method, "startRace") == 0 ||
             std::strcmp(method, "abandonRace") == 0 ||
             std::strcmp(method, "changeCamTarget2") == 0)) {
          const char* tc = tree_host_class(track);
          return retarget(track, tc && tc[0] ? tc : "java.game.City");
        }
        if (osd && osd != self && host_has_or_empty(osd, "Osd") &&
            !host_has(osd, "dialog") &&
            (std::strcmp(method, "createHotkey") == 0 ||
             std::strcmp(method, "createMenu") == 0 ||
             std::strcmp(method, "createBG") == 0 ||
             std::strcmp(method, "createText") == 0 ||
             std::strcmp(method, "createButton") == 0 ||
             std::strcmp(method, "endGroup") == 0 ||
             std::strcmp(method, "beginGroup") == 0 ||
             std::strcmp(method, "hideGroup") == 0 ||
             std::strcmp(method, "showGroup") == 0 ||
             std::strcmp(method, "show") == 0 ||
             std::strcmp(method, "hide") == 0 ||
             std::strcmp(method, "removeAllElements") == 0)) {
          return retarget(osd, "java.render.Osd");
        }
        if (std::strcmp(method, "createCar") == 0 && raceBot &&
            raceBot != self && host_has_or_empty(raceBot, "Bot") &&
            !(hc && std::strstr(hc, "Bot"))) {
          if (!tree_field_get_obj(raceBot, "botVd"))
            tree_field_set_obj(raceBot, "botVd",
                               game_logic_get_vehicle_descriptor(1, -1.f));
          return retarget(raceBot, "java.game.Bot");
        }
        if (map && map != self && host_has_or_empty(map, "GroundRef") &&
            std::strcmp(method, "alignToRoad") == 0) {
          return retarget(map, "java.util.resource.GroundRef");
        }
        if ((std::strcmp(method, "cleanupNightRace") == 0 ||
             std::strcmp(method, "changeCamNone") == 0 ||
             std::strcmp(method, "enableOsd") == 0 ||
             std::strcmp(method, "leaveAsyncMode_Script") == 0 ||
             std::strcmp(method, "leaveAsyncMode") == 0) &&
            argc <= 1 && track && track != self) {
          const char* tc = tree_host_class(track);
          return retarget(track, tc && tc[0] ? tc : "java.game.City");
        }
        if (map && map != self && host_has_or_empty(map, "GroundRef") &&
            (std::strcmp(method, "getNearestCross") == 0 ||
             std::strcmp(method, "getStartDirection") == 0 ||
             std::strcmp(method, "getRouteLength") == 0 ||
             std::strcmp(method, "findRoute") == 0 ||
             std::strcmp(method, "addTraffic") == 0 ||
             std::strcmp(method, "setPedestrianDensity") == 0)) {
          return retarget(map, "java.util.resource.GroundRef");
        }
        if (std::strcmp(method, "addMarker") == 0 ||
            std::strcmp(method, "remMarker") == 0 ||
            std::strcmp(method, "updateNavigator") == 0 ||
            std::strcmp(method, "changeMode") == 0 ||
            std::strcmp(method, "changeSize") == 0 ||
            std::strcmp(method, "changeZoom") == 0) {
          if (nav && nav != self &&
              (host_has(nav, "Navigator") || host_has(nav, "Nav"))) {
            const char* nc = tree_host_class(nav);
            return retarget(nav, nc && nc[0] ? nc : "java.game.Navigator");
          }
          // Soft no-op — avoid miss spam mid-enter before nav is wired.
          if (std::strcmp(method, "addMarker") == 0)
            return JvmValue::make_obj(nullptr);
          return JvmValue::make_void();
        }
        if (std::strcmp(method, "plotRoute") == 0) {
          // Stock RenderRef.plotRoute(parent, type, color, step, scale).
          // TREE often packs (IFFIF) onto RaceSetup — recover nav.route + defaults.
          InvObject* line = nav ? tree_field_get_obj(nav, "route") : nullptr;
          if (!line) line = tree_field_get_obj(self, "route");
          InvObject* parent = nav ? tree_field_get_obj(nav, "localroot") : nullptr;
          InvObject* type = nullptr;
          int32_t color = static_cast<int32_t>(0xFFFF0000);
          float step = 10.f;
          InvObject* scale = vec3_new(0.01f, 0.f, 0.01f);
          for (size_t i = 1; i < pargs->size(); ++i) {
            const JvmValue& a = (*pargs)[i];
            if (a.tag == JvmTag::Obj && a.v.o) {
              const char* ac = tree_host_class(a.v.o);
              if (ac && std::strstr(ac, "Vector3"))
                scale = a.v.o;
              else if (ac && (std::strstr(ac, "ResourceRef") ||
                              std::strstr(ac, "GameRef") ||
                              std::strstr(ac, "Dummy"))) {
                if (!parent)
                  parent = a.v.o;
                else if (!type)
                  type = a.v.o;
              }
            } else if (a.tag == JvmTag::Int) {
              if (a.v.i > 256 || (static_cast<uint32_t>(a.v.i) & 0xFF000000u))
                color = a.v.i;
            } else if (a.tag == JvmTag::Float) {
              if (a.v.f > 1.f) step = a.v.f;
            }
          }
          if (!line) line = resref_new();
          if (!type) type = resref_new();
          if (nav) tree_field_set_obj(nav, "route", line);
          return JvmValue::make_int(java_util_resource_RenderRef_plotRoute(
              line, parent, type, color, step, scale));
        }
        if (std::strcmp(method, "timeWarp") == 0) {
          std::vector<JvmValue> a;
          for (size_t i = 1; i < pargs->size(); ++i) a.push_back((*pargs)[i]);
          if (a.empty()) a.push_back(JvmValue::make_float(0.f));
          return call_by_name("java.lang.System", method, a, true);
        }
      }
    }

    // GroundRef.java wrappers (not PE natives). Prefer leaf over broken TREE
    // body that re-invokes addTrafficN with argc=0. Also fix Valocity-as-recv.
    if (!prefer_static && method && !pargs->empty() &&
        (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o &&
        (std::strcmp(method, "addTraffic") == 0 ||
         std::strcmp(method, "addTrafficN") == 0 ||
         std::strcmp(method, "setPedestrianDensity") == 0 ||
         std::strcmp(method, "getNearestCross") == 0 ||
         std::strcmp(method, "haltTrafficCross") == 0 ||
         std::strcmp(method, "alignToRoad") == 0)) {
      auto resolve_map = [&](InvObject* self) -> InvObject* {
        if (!self) return nullptr;
        const char* hc = tree_host_class(self);
        if (hc && std::strstr(hc, "GroundRef")) return self;
        if (InvObject* map = tree_field_get_obj(self, "map")) return map;
        if (InvObject* track = tree_field_get_obj(self, "track")) {
          if (InvObject* map = tree_field_get_obj(track, "map")) return map;
        }
        if (InvObject* last = tree_field_get_obj(self, "lastState")) {
          if (InvObject* map = tree_field_get_obj(last, "map")) return map;
        }
        for (size_t i = 1; i < pargs->size(); ++i) {
          if ((*pargs)[i].tag != JvmTag::Obj || !(*pargs)[i].v.o) continue;
          const char* ac = tree_host_class((*pargs)[i].v.o);
          if (ac && std::strstr(ac, "GroundRef")) return (*pargs)[i].v.o;
        }
        return nullptr;
      };
      // Stock Java: getNearestCross(approx) → getNearestCross(approx, 0.0).
      // TREE 1-arg body re-invokes with argc=1 forever — call native here.
      if (std::strcmp(method, "getNearestCross") == 0 && argc == 1) {
        InvObject* map = resolve_map((*pargs)[0].v.o);
        InvObject* approx =
            (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Obj)
                ? (*pargs)[1].v.o
                : nullptr;
        if (map)
          return JvmValue::make_obj(
              java_util_resource_GroundRef_getNearestCross(map, approx, 0.f));
      }
      // GroundRef.alignToRoad(Vector3) @ 0x00483E60. TREE often argc=0 onto City.
      if (std::strcmp(method, "alignToRoad") == 0) {
        InvObject* map = resolve_map((*pargs)[0].v.o);
        InvObject* rp = nullptr;
        for (size_t i = 1; i < pargs->size(); ++i) {
          if ((*pargs)[i].tag != JvmTag::Obj || !(*pargs)[i].v.o) continue;
          const char* ac = tree_host_class((*pargs)[i].v.o);
          if (ac && std::strstr(ac, "Vector3")) {
            rp = (*pargs)[i].v.o;
            break;
          }
          if (!rp) rp = (*pargs)[i].v.o;
        }
        if (!rp) {
          InvObject* city = (*pargs)[0].v.o;
          rp = tree_field_get_obj(city, "pS");
          if (!rp) rp = tree_field_get_obj(city, "posStart");
          if (!rp) {
            InvObject* player = tree_field_get_obj(city, "player");
            if (!player) player = game_logic_player();
            if (player) {
              if (InvObject* car = tree_field_get_obj(player, "car"))
                rp = tree_field_get_obj(car, "pos");
            }
          }
        }
        if (!map) map = (*pargs)[0].v.o;
        if (!rp) rp = vec3_new(0.f, 0.f, 0.f);
        return JvmValue::make_obj(
            java_util_resource_GroundRef_alignToRoad(map, rp));
      }
      // Stock: map.haltTrafficCross(raceStart, 15.0). TREE often swaps to (F,Obj).
      if (std::strcmp(method, "haltTrafficCross") == 0 && argc >= 1) {
        InvObject* map = resolve_map((*pargs)[0].v.o);
        InvObject* pos = nullptr;
        float time = 15.f;
        for (size_t i = 1; i < pargs->size(); ++i) {
          const JvmValue& a = (*pargs)[i];
          if (a.tag == JvmTag::Obj && a.v.o) {
            const char* ac = tree_host_class(a.v.o);
            if (ac && std::strstr(ac, "Vector3"))
              pos = a.v.o;
            else if (!pos)
              pos = a.v.o;
          } else if (a.tag == JvmTag::Float) {
            time = a.v.f;
          } else if (a.tag == JvmTag::Int) {
            time = static_cast<float>(a.v.i);
          }
        }
        if (map) {
          java_util_resource_GroundRef_haltTrafficCross(map, pos, time);
          return JvmValue::make_void();
        }
      }
      if (std::strcmp(method, "addTraffic") == 0 ||
          std::strcmp(method, "addTrafficN") == 0 ||
          std::strcmp(method, "setPedestrianDensity") == 0) {
      auto arg_int = [&](size_t i) -> int32_t {
        if (i >= pargs->size()) return 0;
        if ((*pargs)[i].tag == JvmTag::Int) return (*pargs)[i].v.i;
        if ((*pargs)[i].tag == JvmTag::Float)
          return static_cast<int32_t>((*pargs)[i].v.f);
        return 0;
      };
      auto arg_float = [&](size_t i) -> float {
        if (i >= pargs->size()) return 0.f;
        if ((*pargs)[i].tag == JvmTag::Float) return (*pargs)[i].v.f;
        if ((*pargs)[i].tag == JvmTag::Int)
          return static_cast<float>((*pargs)[i].v.i);
        return 0.f;
      };
      auto config_f = [](const char* key, float def) -> float {
        InvObject* cfg = system_config_host();
        if (!cfg) return def;
        return tree_field_get_float(cfg, key);
      };
      if ((std::strcmp(method, "addTraffic") == 0 ||
           std::strcmp(method, "addTrafficN") == 0) &&
          (argc == 5 || argc == 2 || argc >= 1)) {
        InvObject* map = resolve_map((*pargs)[0].v.o);
        InvObject* type =
            (pargs->size() >= 2 && (*pargs)[1].tag == JvmTag::Obj)
                ? (*pargs)[1].v.o
                : nullptr;
        if (map && type) {
          const float dens = config_f("trafficDensity", 1.f);
          int32_t n = arg_int(2);
          // Java addTraffic multiplies n*Config.trafficDensity; addTrafficN
          // @ 0x00484050 does not (it reads trafficDensity itself).
          if (std::strcmp(method, "addTraffic") == 0)
            n = static_cast<int32_t>(static_cast<float>(n) * dens);
          const float lb = argc >= 5 ? arg_float(3) : 1.f;
          const float le = argc >= 5 ? arg_float(4) : 4.f;
          const float wb = argc >= 5 ? arg_float(5) : 2.f;
          return JvmValue::make_int(java_util_resource_GroundRef_addTrafficN(
              map, type, n, lb, le, wb));
        }
      }
      if (std::strcmp(method, "setPedestrianDensity") == 0 && argc == 1) {
        InvObject* map = resolve_map((*pargs)[0].v.o);
        if (map) {
          const float dens = config_f("pedestrianDensity", 0.f);
          java_util_resource_GroundRef_setPedestrianDensityN(
              map, arg_float(1) * dens);
          return JvmValue::make_void();
        }
      }
      }  // addTraffic / setPedestrianDensity
    }

    // Stock GameRef/RenderRef.setMatrix(Vector3, Ypr). finishObject uses null
    // Ypr (TREE packs as Int 0) → JNI shape (Ljava.lang.Object;I)V misses.
    if (!prefer_static && method && std::strcmp(method, "setMatrix") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o &&
        argc >= 1 && argc <= 2) {
      InvObject* self = (*pargs)[0].v.o;
      const char* hc = tree_host_class(self);
      InvObject* pos = nullptr;
      InvObject* ori = nullptr;
      for (size_t i = 1; i < pargs->size(); ++i) {
        const JvmValue& a = (*pargs)[i];
        if (a.tag != JvmTag::Obj || !a.v.o) continue;
        const char* ac = tree_host_class(a.v.o);
        if (ac && std::strstr(ac, "Ypr"))
          ori = a.v.o;
        else if (ac && std::strstr(ac, "Vector3") && !pos)
          pos = a.v.o;
        else if (!pos)
          pos = a.v.o;
      }
      if (hc && std::strstr(hc, "RenderRef"))
        java_util_resource_RenderRef_setMatrix_1(self, pos, ori);
      else
        java_util_resource_GameRef_setMatrix(self, pos, ori);
      return JvmValue::make_void();
    }

    // Animation.setSpeed(F)V @ 0x0047EC00. TREE 0x25 packs argc=0 onto
    // ResourceRef (RenderRef/Animation host-class often collapsed).
    if (!prefer_static && method && std::strcmp(method, "setSpeed") == 0 &&
        !pargs->empty() && (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      float spd = 1.f;
      for (size_t i = 1; i < pargs->size(); ++i) {
        if ((*pargs)[i].tag == JvmTag::Float) {
          spd = (*pargs)[i].v.f;
          break;
        }
        if ((*pargs)[i].tag == JvmTag::Int) {
          spd = static_cast<float>((*pargs)[i].v.i);
          break;
        }
      }
      java_util_resource_Animation_setSpeed((*pargs)[0].v.o, spd);
      return JvmValue::make_void();
    }

    // Prefer native table overload matching argc on the named class.
    auto argc_of = [](const NativeEntry& e) -> int {
      const char* js = e.java_signature;
      if (!js) return 0;
      const char* paren = std::strchr(js, '(');
      if (!paren || paren[1] == ')') return 0;
      int n = 1;
      for (const char* p = paren; *p && *p != ')'; ++p)
        if (*p == ',') ++n;
      return n;
    };
    const NativeEntry* best = nullptr;
    for (size_t i = 0; i < kNativeTableCount; ++i) {
      if (std::strcmp(kNativeTable[i].class_fqn, class_fqn) != 0) continue;
      if (std::strcmp(kNativeTable[i].method_name, method) != 0) continue;
      if (kNativeTable[i].is_static != prefer_static) continue;
      if (argc_of(kNativeTable[i]) == argc) {
        best = &kNativeTable[i];
        break;
      }
    }
    // Cross-class fallback: GameLogic TREE invokes ResourceRef.getFirstChild/cache
    // while owner is still java.game.GameLogic. Never steal Menu.addItem → SfxTable.
    // Never steal <init> across classes (Style → Animation noise).
    if (!best || argc_of(*best) != argc) {
      best = nullptr;
      const bool skip_cross_additem =
          method && std::strcmp(method, "addItem") == 0;
      const bool skip_cross_init =
          method && std::strcmp(method, "<init>") == 0;
      for (size_t i = 0; i < kNativeTableCount; ++i) {
        if (std::strcmp(kNativeTable[i].method_name, method) != 0) continue;
        if (kNativeTable[i].is_static != prefer_static) continue;
        if (skip_cross_init) continue;
        if (skip_cross_additem &&
            std::strstr(kNativeTable[i].class_fqn, "SfxTable"))
          continue;
        // Exact argc only — wrong-arity GameRef.queueEvent blew the stack when
        // TREE named MouseCursor.queueEvent with argc=0.
        if (argc_of(kNativeTable[i]) == argc) {
          best = &kNativeTable[i];
          break;
        }
      }
    }
    // RenderRef.plotRoute(parent, type, color, step, scale) — TREE often
    // packs primitives only (IFFIF). Recover from nav.route / defaults.
    if (method && std::strcmp(method, "plotRoute") == 0 && !pargs->empty() &&
        (*pargs)[0].tag == JvmTag::Obj && (*pargs)[0].v.o) {
      InvObject* line = (*pargs)[0].v.o;
      const char* lc = tree_host_class(line);
      InvObject* nav = nullptr;
      if (lc && std::strstr(lc, "RaceSetup")) {
        InvObject* track = tree_field_get_obj(line, "track");
        if (!track) track = tree_field_get_obj(line, "lastState");
        nav = track ? tree_field_get_obj(track, "nav") : nullptr;
        InvObject* route = nav ? tree_field_get_obj(nav, "route") : nullptr;
        if (route) line = route;
      } else if (lc && std::strstr(lc, "Navigator")) {
        nav = line;
        InvObject* route = tree_field_get_obj(nav, "route");
        if (route) line = route;
      }
      InvObject* parent = nav ? tree_field_get_obj(nav, "localroot") : nullptr;
      InvObject* type = nullptr;
      int32_t color = static_cast<int32_t>(0xFFFF0000);
      float step = 10.f;
      InvObject* scale = vec3_new(0.01f, 0.f, 0.01f);
      for (size_t i = 1; i < pargs->size(); ++i) {
        const JvmValue& a = (*pargs)[i];
        if (a.tag == JvmTag::Obj && a.v.o) {
          const char* ac = tree_host_class(a.v.o);
          if (ac && std::strstr(ac, "Vector3"))
            scale = a.v.o;
          else if (ac && (std::strstr(ac, "ResourceRef") ||
                          std::strstr(ac, "GameRef") ||
                          std::strstr(ac, "Dummy"))) {
            if (!parent)
              parent = a.v.o;
            else if (!type)
              type = a.v.o;
          }
        } else if (a.tag == JvmTag::Int) {
          if (a.v.i > 256 || (static_cast<uint32_t>(a.v.i) & 0xFF000000u))
            color = a.v.i;
        } else if (a.tag == JvmTag::Float) {
          if (a.v.f > 1.f) step = a.v.f;
        }
      }
      if (!type) type = resref_new();
      if (nav) tree_field_set_obj(nav, "route", line);
      return JvmValue::make_int(java_util_resource_RenderRef_plotRoute(
          line, parent, type, color, step, scale));
    }
    if (best && best->fn && argc_of(*best) == argc) {
      // Soft UnboxArg @ 0x0045D910: registered descriptor only (not TREE tags).
      // Fallback TREE-shaped JNI only if java_sig_to_jni cannot parse inventory.
      std::string sig = java_sig_to_jni(best->java_signature);
      if (sig.empty()) {
        size_t start = prefer_static ? 0 : 1;
        char ret = 'V';
        if (best->java_signature) {
          if (std::strncmp(best->java_signature, "int ", 4) == 0) ret = 'I';
          else if (std::strncmp(best->java_signature, "float ", 6) == 0)
            ret = 'F';
          else if (std::strncmp(best->java_signature, "void ", 5) == 0)
            ret = 'V';
          else
            ret = 'L';
        }
        sig = "(";
        for (size_t i = start; i < pargs->size(); ++i) {
          if ((*pargs)[i].tag == JvmTag::Float) sig += 'F';
          else if ((*pargs)[i].tag == JvmTag::Obj) sig += "Ljava.lang.Object;";
          else sig += 'I';
        }
        sig += ')';
        if (ret == 'L') sig += "Ljava.lang.Object;";
        else sig += ret;
      }

      CallFrame frame;
      frame.args = *pargs;
      soft_unbox_bind_registered_sig(&frame, best);
      // Prefer converted registered JNI; keep TREE fallback string alive.
      if (!sig.empty()) frame.jni_signature = sig.c_str();
      std::string e;
      if (!call_native(best, &frame, &e)) {
        std::fprintf(stderr, "[jvm] call_by_name native failed %s.%s: %s\n",
                     best->class_fqn, method, e.c_str());
        return JvmValue::make_void();
      }
      return frame.result;
    }

    // Fall back to class method table + JNI sig match by argc.
    if (cls) {
      const JvmMethod* tree_init = nullptr;
      const JvmMethod* any_match = nullptr;
      for (auto& m : cls->methods) {
        if (m.name != method) continue;
        int n = count_jni_args(m.signature.c_str());
        if (n != argc) continue;
        if (!any_match) any_match = &m;
        const bool has_nodes =
            m.tree_index >= 0 &&
            static_cast<size_t>(m.tree_index) < cls->trees.size() &&
            !cls->trees[static_cast<size_t>(m.tree_index)].nodes.empty();
        if (has_nodes) {
          tree_init = &m;
          break;
        }
      }
      // City ships stub/native <init> MTHD rows plus FILD posGarage — prefer a
      // real TREE body, else fall through to the super-walk below.
      const JvmMethod* pick = tree_init ? tree_init : any_match;
      if (pick && (tree_init || method == nullptr ||
                   std::strcmp(method, "<init>") != 0 ||
                   cls->field_inits.empty())) {
        return jvm_->invoke(class_fqn, method, pick->signature.c_str(), args,
                            prefer_static);
      }
    }

    // City has FILD but no TREE <init> — walk supers via invoke (Scene/Track).
    if (method && std::strcmp(method, "<init>") == 0 && cls &&
        !cls->field_inits.empty()) {
      return jvm_->invoke(class_fqn, "<init>", "()V", args, prefer_static);
    }

    // Inherited TREE on *_VT: call_by_name only scanned the leaf MTHD table.
    // Narrow fallback — a blanket invoke() steals common names (show/hide).
    if (cls && method && !cls->super_name.empty() &&
        std::strcmp(method, "addColorIndex") == 0) {
      return jvm_->invoke(class_fqn, method, "(I)V", *pargs, prefer_static);
    }

    // Inherited TREE: Valocity.startRace / createQuickRaceBot live on City.
    if (cls && method) {
      std::string super = cls->super_name;
      for (int depth = 0; !super.empty() && depth < 16; ++depth) {
        if (!jvm_->find_class(super.c_str())) jvm_->load_class(super.c_str());
        const JvmClass* sc = jvm_->find_class(super.c_str());
        if (!sc) break;
        for (auto& sm : sc->methods) {
          if (sm.name != method) continue;
          int n = count_jni_args(sm.signature.c_str());
          if (n != argc) continue;
          const bool has_nodes =
              sm.tree_index >= 0 &&
              static_cast<size_t>(sm.tree_index) < sc->trees.size() &&
              !sc->trees[static_cast<size_t>(sm.tree_index)].nodes.empty();
          if (has_nodes) {
            return jvm_->invoke(super.c_str(), method, sm.signature.c_str(),
                                args, prefer_static);
          }
        }
        super = sc->super_name;
      }
    }

    std::fprintf(stderr, "[jvm] call_by_name miss %s.%s argc=%d\n", class_fqn,
                 method, argc);
    return JvmValue::make_void();
  }

 private:
  Jvm* jvm_;
};

}  // namespace

JvmValue Jvm::invoke(const char* class_fqn, const char* name, const char* signature,
                     const std::vector<JvmValue>& args, bool is_static) {
  thread_local int tls_depth = 0;
  thread_local char tls_trail[16][96];
  struct DepthGuard {
    int* d;
    DepthGuard(int* p, const char* c, const char* m) : d(p) {
      if (*d >= 0 && *d < 16)
        std::snprintf(tls_trail[*d], sizeof(tls_trail[*d]), "%.60s.%.30s",
                      c ? c : "?", m ? m : "?");
      ++(*d);
    }
    ~DepthGuard() { --(*d); }
  } depth_guard(&tls_depth, class_fqn, name);
  if (tls_depth > 48) {
    std::fprintf(stderr, "[jvm] invoke depth=%d abort %s.%s\n", tls_depth,
                 class_fqn ? class_fqn : "?", name ? name : "?");
    for (int i = 0; i < tls_depth && i < 16; ++i)
      std::fprintf(stderr, "  #%d %s\n", i, tls_trail[i]);
    return JvmValue::make_void();
  }

  // Empty PE ctors — same early-out as call_by_name. Stream op35/op34 nest
  // through invoke (not call_by_name), so keep these here too.
  if (name && std::strcmp(name, "<init>") == 0 && class_fqn &&
      (std::strcmp(class_fqn, "java.lang.Object") == 0 ||
       std::strcmp(class_fqn, "java.lang.Native") == 0))
    return JvmValue::make_void();

  // java.util.Vector bag — same Soft leaves as call_by_name. PE stream nests
  // Vector.size/elementAt/addElement through invoke (owner often VehicleType).
  // Do not gate on is_static: stream_call passes self==nullptr → is_static true
  // even when args[0] is the Vector receiver.
  if (name && !args.empty()) {
    const bool is_add = std::strcmp(name, "addElement") == 0;
    const bool is_at = std::strcmp(name, "elementAt") == 0;
    const bool is_sz = std::strcmp(name, "size") == 0;
    const bool is_rm = std::strcmp(name, "removeElement") == 0;
    const bool is_rmall = std::strcmp(name, "removeAllElements") == 0;
    const bool is_last = std::strcmp(name, "lastElement") == 0;
    const bool is_empty = std::strcmp(name, "isEmpty") == 0;
    if (is_add || is_at || is_sz || is_rm || is_rmall || is_last || is_empty) {
      InvObject* a0 = (args[0].tag == JvmTag::Obj) ? args[0].v.o : nullptr;
      InvObject* a1 = (args.size() >= 2 && args[1].tag == JvmTag::Obj)
                          ? args[1].v.o
                          : nullptr;
      InvObject* vec = nullptr;
      InvObject* elem = nullptr;
      int32_t idx = 0;
      if (a0 && tree_vector_is(a0)) {
        vec = a0;
        if (args.size() >= 2) {
          if (args[1].tag == JvmTag::Obj) elem = args[1].v.o;
          else if (args[1].tag == JvmTag::Int) idx = args[1].v.i;
        }
      } else if (a1 && tree_vector_is(a1)) {
        vec = a1;
        elem = a0;
      }
      if (vec) {
        if (is_add) {
          tree_vector_add(vec, elem);
          return JvmValue::make_void();
        }
        if (is_sz) return JvmValue::make_int(tree_vector_size(vec));
        if (is_empty)
          return JvmValue::make_int(tree_vector_size(vec) == 0 ? 1 : 0);
        if (is_last) {
          const int32_t n = tree_vector_size(vec);
          return JvmValue::make_obj(
              n > 0 ? tree_vector_element_at(vec, n - 1) : nullptr);
        }
        if (is_at) {
          if (args.size() >= 2 && args[1].tag == JvmTag::Int) idx = args[1].v.i;
          return JvmValue::make_obj(tree_vector_element_at(vec, idx));
        }
        if (is_rm) {
          tree_vector_remove(vec, elem);
          return JvmValue::make_void();
        }
        if (is_rmall) {
          tree_vector_resize(vec, 0);
          return JvmValue::make_void();
        }
      }
    }
  }

  if (name && std::strcmp(name, "queueEvent") == 0) {
    if (args.size() <= 1) return JvmValue::make_void();
    queue_event_apply(args);
    return JvmValue::make_void();
  }
  if (name && std::strcmp(name, "create") == 0 && !args.empty() &&
      args[0].tag == JvmTag::Obj && args[0].v.o &&
      is_renderref_not_camera(args[0].v.o, class_fqn)) {
    if (args.size() <= 1) return JvmValue::make_void();
    renderref_apply_create(args);
    return JvmValue::make_void();
  }
  if (name && (std::strcmp(name, "addTraffic") == 0 ||
               std::strcmp(name, "addTrafficN") == 0) &&
      !args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
    InvObject* map = args[0].v.o;
    const char* hc = tree_host_class(map);
    if (!hc || !std::strstr(hc, "GroundRef")) {
      if (InvObject* m = tree_field_get_obj(map, "map")) map = m;
    }
    InvObject* type = nullptr;
    int32_t n = 0;
    float lb = 2.f, le = 5.f, wb = 2.f;
    for (size_t i = 1; i < args.size(); ++i) {
      const JvmValue& a = args[i];
      if (a.tag == JvmTag::Obj && a.v.o && !type)
        type = a.v.o;
      else if (a.tag == JvmTag::Int || a.tag == JvmTag::Float) {
        const float f = a.tag == JvmTag::Float ? a.v.f
                                               : static_cast<float>(a.v.i);
        if (n == 0 && a.tag == JvmTag::Int)
          n = a.v.i;
        else if (n == 0)
          n = static_cast<int32_t>(f);
        else if (lb == 2.f)
          lb = f;
        else if (le == 5.f)
          le = f;
        else
          wb = f;
      }
    }
    if (map && type) {
      if (std::strcmp(name, "addTraffic") == 0) {
        InvObject* cfg = system_config_host();
        const float dens =
            cfg ? tree_field_get_float(cfg, "trafficDensity") : 1.f;
        n = static_cast<int32_t>(static_cast<float>(n) * dens);
      }
      return JvmValue::make_int(java_util_resource_GroundRef_addTrafficN(
          map, type, n, lb, le, wb));
    }
  }
  if (name && std::strcmp(name, "addSceneElements") == 0) {
    InvObject* self = nullptr;
    if (!args.empty() && args[0].tag == JvmTag::Obj) self = args[0].v.o;
    if (!self) self = game_logic_actual_state();
    if (self) valocity_apply_scene(self);
    return JvmValue::make_void();
  }
  if (name && std::strcmp(name, "time2Config") == 0) {
    float t = game_logic_time();
    InvObject* self = nullptr;
    if (!is_static && !args.empty() && args[0].tag == JvmTag::Obj)
      self = args[0].v.o;
    const size_t i0 = (!is_static && !args.empty()) ? 1 : 0;
    for (size_t i = i0; i < args.size(); ++i) {
      if (args[i].tag == JvmTag::Float) {
        t = args[i].v.f;
        break;
      }
      if (args[i].tag == JvmTag::Int) {
        t = static_cast<float>(args[i].v.i);
        break;
      }
    }
    float rnd = self ? tree_field_get_float(self, "lastSelectionSeed") : 0.f;
    return JvmValue::make_int(scene_time2config(t, rnd));
  }

  // Navigator.changeMode / changeSize / changeZoom — pure Java; leaf avoids
  // RaceSetup/RenderRef misbinds and cam.setMatrix TREE noise.
  if (!is_static && name && !args.empty() && args[0].tag == JvmTag::Obj &&
      args[0].v.o) {
    InvObject* self = args[0].v.o;
    const char* hc = tree_host_class(self);
    const bool is_nav =
        (class_fqn && std::strstr(class_fqn, "Navigator")) ||
        (hc && std::strstr(hc, "Navigator"));
    if (is_nav && std::strcmp(name, "changeMode") == 0) {
      int32_t mode = 0;
      if (args.size() >= 2) {
        if (args[1].tag == JvmTag::Int) mode = args[1].v.i;
        else if (args[1].tag == JvmTag::Float)
          mode = static_cast<int32_t>(args[1].v.f);
      }
      tree_field_set_int(self, "mode", mode);
      return JvmValue::make_void();
    }
    if (is_nav && std::strcmp(name, "changeZoom") == 0) {
      float z = 4.5f;
      if (args.size() >= 2) {
        if (args[1].tag == JvmTag::Float) z = args[1].v.f;
        else if (args[1].tag == JvmTag::Int)
          z = static_cast<float>(args[1].v.i);
      }
      tree_field_set_float(self, "zoom", z);
      return JvmValue::make_void();
    }
    if (is_nav && std::strcmp(name, "changeSize") == 0 && args.size() >= 5) {
      auto af = [&](size_t i) -> float {
        if (args[i].tag == JvmTag::Float) return args[i].v.f;
        if (args[i].tag == JvmTag::Int)
          return static_cast<float>(args[i].v.i);
        return 0.f;
      };
      tree_field_set_float(self, "vp_l", af(1));
      tree_field_set_float(self, "vp_t", af(2));
      tree_field_set_float(self, "vp_w", af(3));
      tree_field_set_float(self, "vp_h", af(4));
      return JvmValue::make_void();
    }
  }

  // Stock Valocity.enter(RaceSetup): TREE instanceof often misses, then the
  // QUICKRACE else-branch CAS(racesetup) undoes startRace. Mirror the Java gate.
  if (!is_static && name && std::strcmp(name, "enter") == 0 && class_fqn &&
      std::strstr(class_fqn, "Valocity") && args.size() >= 2 &&
      args[0].tag == JvmTag::Obj && args[0].v.o && args[1].tag == JvmTag::Obj &&
      args[1].v.o) {
    const char* pc = tree_host_class(args[1].v.o);
    if (pc && std::strstr(pc, "RaceSetup")) {
      InvObject* self = args[0].v.o;
      if (tree_field_get_int(self, "raceState") == 0) {
        InvObject* parent = tree_field_get_obj(self, "parentState");
        if (parent) game_logic_change_active_section(parent);
        tree_field_set_int(self, "enter_via_tree", 1);
        std::printf("[script] java.game.Valocity.enter via TREE (RaceSetup abandon)\n");
        return JvmValue::make_void();
      }
      frontend_loading_screen_show();
      if (InvObject* osd = tree_field_get_obj(self, "osd"))
        tree_field_set_int(osd, "visible", 1);
      tree_field_set_int(self, "entered", 1);
      tree_field_set_int(self, "enter_via_tree", 1);
      frontend_loading_screen_hide();
      std::printf("[script] java.game.Valocity.enter via TREE (RaceSetup return)\n");
      return JvmValue::make_void();
    }
  }

  // Trigger.<init> — Java body is GameRef.create; TREE this() packing is lossy.
  if (!is_static && name && std::strcmp(name, "<init>") == 0 && class_fqn &&
      std::strstr(class_fqn, "Trigger") && !args.empty() &&
      args[0].tag == JvmTag::Obj && args[0].v.o) {
    if (trigger_apply_ctor(args[0].v.o, args))
      return JvmValue::make_void();
  }

  // Thread ctors TREE: Thread(){this(this);} recurses when packing is wrong.
  // RaceSetup: new Thread(this, "…") → init(name)+target.
  if (!is_static && name && std::strcmp(name, "<init>") == 0 && class_fqn &&
      std::strstr(class_fqn, "Thread") && !args.empty() &&
      args[0].tag == JvmTag::Obj && args[0].v.o) {
    InvObject* self = args[0].v.o;
    InvObject* target = nullptr;
    InvObject* tname = nullptr;
    if (args.size() >= 2 && args[1].tag == JvmTag::Obj) {
      const char* ac = tree_host_class(args[1].v.o);
      if (ac && std::strstr(ac, "String"))
        tname = args[1].v.o;
      else
        target = args[1].v.o;
    }
    if (args.size() >= 3 && args[2].tag == JvmTag::Obj) {
      const char* ac = tree_host_class(args[2].v.o);
      if (ac && std::strstr(ac, "String"))
        tname = args[2].v.o;
      else if (!target)
        target = args[2].v.o;
    }
    if (target) tree_field_set_obj(self, "target", target);
    java_lang_Thread_init(self, tname);
    return JvmValue::make_void();
  }

  // Navigator.updateNavigator(car) → updateNavigator(car, mode) Java wrapper.
  if (!is_static && name && std::strcmp(name, "updateNavigator") == 0 &&
      !args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
    InvObject* self = args[0].v.o;
    const char* hc = tree_host_class(self);
    const bool is_nav =
        (class_fqn && std::strstr(class_fqn, "Navigator")) ||
        (hc && std::strstr(hc, "Navigator"));
    if (is_nav) {
      const int argc = static_cast<int>(args.size()) - 1;
      InvObject* car =
          (argc >= 1 && args[1].tag == JvmTag::Obj) ? args[1].v.o : nullptr;
      int32_t mode = tree_field_get_int(self, "mode");
      if (argc >= 2) {
        if (args[2].tag == JvmTag::Int) mode = args[2].v.i;
        else if (args[2].tag == JvmTag::Float)
          mode = static_cast<int32_t>(args[2].v.f);
      }
      java_game_Navigator_updateNavigator(self, car, mode);
      return JvmValue::make_void();
    }
  }

  // Navigator.addMarker overloads — TREE wrappers recurse when argc packing
  // collapses (RaceSetup static markers are the 3-arg form).
  if (!is_static && name && std::strcmp(name, "addMarker") == 0 &&
      !args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
    InvObject* self = args[0].v.o;
    const char* hc = tree_host_class(self);
    const bool is_nav =
        (class_fqn && std::strstr(class_fqn, "Navigator")) ||
        (hc && std::strstr(hc, "Navigator"));
    if (is_nav) {
      const int argc = static_cast<int>(args.size()) - 1;
      if (argc >= 3 && args[1].tag == JvmTag::Obj &&
          args[2].tag == JvmTag::Obj) {
        int32_t rtype = 0;
        if (args[1].v.o) rtype = tree_field_get_int(args[1].v.o, "id");
        float px = 0.f, pz = 0.f;
        if (args[2].v.o) {
          px = tree_field_get_float(args[2].v.o, "x");
          pz = tree_field_get_float(args[2].v.o, "z");
        }
        int32_t pri = 0;
        if (args[3].tag == JvmTag::Int) pri = args[3].v.i;
        else if (args[3].tag == JvmTag::Float)
          pri = static_cast<int32_t>(args[3].v.f);
        return JvmValue::make_obj(
            navigator_add_marker_static(self, rtype, px, pz, pri));
      }
      if (argc == 2 && args[1].tag == JvmTag::Obj &&
          args[2].tag == JvmTag::Obj) {
        int32_t rtype = 0;
        if (args[1].v.o) rtype = tree_field_get_int(args[1].v.o, "id");
        return JvmValue::make_obj(
            navigator_add_marker_dynamic(self, rtype, args[2].v.o));
      }
      if (argc == 1 && args[1].tag == JvmTag::Obj && args[1].v.o) {
        InvObject* rc = args[1].v.o;
        InvObject* marker = tree_field_get_obj(rc, "marker");
        InvObject* car = tree_field_get_obj(rc, "car");
        int32_t rtype = marker ? tree_field_get_int(marker, "id") : 0;
        return JvmValue::make_obj(
            navigator_add_marker_dynamic(self, rtype, car));
      }
      return JvmValue::make_obj(nullptr);
    }
  }

  // GroundRef.getNearestCross(approx) Java wrapper TREE re-invokes itself when
  // packing drops the 0.0f — hit the PE native (distance=0) directly.
  if (!is_static && name && std::strcmp(name, "getNearestCross") == 0 &&
      !args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
    InvObject* self = args[0].v.o;
    const char* hc = tree_host_class(self);
    const bool named_gr =
        class_fqn && std::strstr(class_fqn, "GroundRef") != nullptr;
    const bool self_gr = hc && std::strstr(hc, "GroundRef") != nullptr;
    InvObject* map = self_gr ? self : nullptr;
    if (!map && self) {
      map = tree_field_get_obj(self, "map");
      if (!map) {
        if (InvObject* track = tree_field_get_obj(self, "track"))
          map = tree_field_get_obj(track, "map");
      }
      if (!map) {
        if (InvObject* last = tree_field_get_obj(self, "lastState"))
          map = tree_field_get_obj(last, "map");
      }
    }
    if ((named_gr || self_gr || map)) {
      if (!map) map = self;
      const int argc = static_cast<int>(args.size()) - 1;
      if (argc <= 1) {
        InvObject* approx =
            (argc >= 1 && args[1].tag == JvmTag::Obj) ? args[1].v.o : nullptr;
        return JvmValue::make_obj(java_util_resource_GroundRef_getNearestCross(
            map, approx, 0.f));
      }
      if (argc >= 2) {
        InvObject* approx =
            (args[1].tag == JvmTag::Obj) ? args[1].v.o : nullptr;
        float dist = 0.f;
        if (args[2].tag == JvmTag::Float) dist = args[2].v.f;
        else if (args[2].tag == JvmTag::Int)
          dist = static_cast<float>(args[2].v.i);
        return JvmValue::make_obj(java_util_resource_GroundRef_getNearestCross(
            map, approx, dist));
      }
    }
  }
  // Host fast-path: GameLogic.initVehicleTypes (arrays + static field).
  if (is_static && name && std::strcmp(name, "initVehicleTypes") == 0 &&
      class_fqn && std::strcmp(class_fqn, "java.game.GameLogic") == 0) {
    const int32_t n = game_logic_init_vehicle_types();
    return JvmValue::make_int(n);
  }
  // LoadingScreen / HotkeyWatcher run loops: C++ mirrors (TREE spins / races).
  if (!is_static && name && std::strcmp(name, "run") == 0 && class_fqn &&
      !args.empty() && args[0].tag == JvmTag::Obj) {
    if (std::strstr(class_fqn, "LoadingScreen")) {
      frontend_loading_screen_run(args[0].v.o);
      return JvmValue::make_void();
    }
    if (std::strstr(class_fqn, "HotkeyWatcher")) {
      frontend_hotkey_watcher_run(args[0].v.o);
      return JvmValue::make_void();
    }
  }
  // Dialog.display — stock Java wait()/notify(); host auto-accepts after show.
  if (!is_static && name && std::strcmp(name, "display") == 0 && class_fqn &&
      std::strstr(class_fqn, "Dialog") && !args.empty() &&
      args[0].tag == JvmTag::Obj && args[0].v.o) {
    return JvmValue::make_int(dialog_display(args[0].v.o));
  }
  // MainMenuDialog / OptionsDialog.show — huge TREE (video modes + menus) hits
  // invoke-depth abort and leaves incomplete chrome. Leaf: music/FMV +
  // addCustomGroups TREE + Dialog shown.
  if (!is_static && name && std::strcmp(name, "show") == 0 && class_fqn &&
      (std::strstr(class_fqn, "MainMenuDialog") ||
       std::strstr(class_fqn, "OptionsDialog")) &&
      !args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
    InvObject* self = args[0].v.o;
    InvObject* osd = tree_field_get_obj(self, "osd");
    if (!osd) {
      osd = tree_host_new("java.render.Osd");
      tree_field_set_obj(self, "osd", osd);
    }
    osd_ensure_defaults(osd);
    if (std::strstr(class_fqn, "MainMenuDialog")) {
      java_sound_Sound_changeMusicSet(3);
      const int32_t fmv_ok =
          video_fmv_open("data\\fmv\\prime.avi", 1, 1);
      tree_field_set_int(self, "bgVideoActive", fmv_ok == 0 ? 1 : 0);
    }
    // Prefer stock addCustomGroups TREE (createBG/header/menus via leaves).
    std::vector<JvmValue> acg = {JvmValue::make_obj(self)};
    // Avoid re-entering this show leaf: call_by_name path for addCustomGroups.
    // Use invoke with a name that has TREE; temporarily no show recursion.
    {
      // Direct tree eval via invoke on addCustomGroups only.
      const char* owner = class_fqn;
      if (std::strstr(class_fqn, "MainMenuDialog"))
        owner = "java.game.MainMenuDialog";
      invoke(owner, "addCustomGroups", "()V", acg, false);
    }
    tree_field_set_int(osd, "visible", 1);
    tree_field_set_int(self, "shown", 1);
    tree_field_set_int(self, "show_via_tree", 1);
    const int32_t btn = tree_field_get_int(osd, "button_count");
    const int32_t txt = render_d3d9_osd_text_count();
    if (btn >= 12 && txt >= 12)
      tree_field_set_int(self, "menu_chrome", 1);
    std::printf("[script] %s.show via TREE addCustomGroups btn=%d txt=%d chrome=%d\n",
                class_fqn, btn, txt, tree_field_get_int(self, "menu_chrome"));
    return JvmValue::make_void();
  }
  // SplashScreen.enter — run TREE (createBG/createHotkey are engine leaves).
  // SplashScreen.exit / MainMenu.exit — TREE often mis-packs argc; host mirrors
  // the Java body. SplashScreen.exit MUST clearEventMask(EVENT_ANY) or the
  // addTimer(3,1) oneshot still has EVENT_TIME and CAS(MainMenu) mid-Valocity.
  if (!is_static && name && std::strcmp(name, "exit") == 0 && class_fqn &&
      (std::strstr(class_fqn, "SplashScreen") ||
       std::strcmp(class_fqn, "java.game.MainMenu") == 0) &&
      !args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
    InvObject* self = args[0].v.o;
    tree_field_set_int(self, "entered", 0);
    tree_field_set_int(self, "exit_via_tree", 1);
    if (std::strstr(class_fqn, "SplashScreen")) {
      // Java: clearEventMask(EVENT_ANY); osd.hide();
      constexpr int32_t kEventAny = 0x0FFFFFFF;
      java_lang_GameType_clearEventMask(self, kEventAny);
      if (InvObject* osd = tree_field_get_obj(self, "osd")) {
        tree_field_set_int(osd, "visible", 0);
        java_lang_GameType_clearEventMask(osd, kEventAny);
      }
    }
    if (std::strcmp(class_fqn, "java.game.MainMenu") == 0) {
      if (InvObject* mmd = tree_field_get_obj(self, "mmd")) {
        tree_field_set_int(mmd, "shown", 0);
        video_fmv_close();
        tree_field_set_obj(self, "mmd", nullptr);
      }
    }
    std::printf("[script] %s.exit via engine mirror\n", class_fqn);
    return JvmValue::make_void();
  }
  // Garage.enter / exit: TREE-first (createOSDObjects + Scene leaves).
  // On TREE success, garage_ensure_map / lockCar run below; exit calls garage_exit.
  // Valocity.enter: TREE-first (City/Track super + GroundRef leaves).
  // On success, valocity_finalize_enter normalizes traffic/triggers below.
  if (name && std::strcmp(name, "getVehicleDescriptor") == 0) {
    if (is_static && class_fqn &&
        std::strcmp(class_fqn, "java.game.GameLogic") == 0) {
      int32_t set = 0;
      float param = -1.f;
      if (!args.empty() && args[0].tag == JvmTag::Int) set = args[0].v.i;
      if (args.size() >= 2) {
        if (args[1].tag == JvmTag::Float) param = args[1].v.f;
        else if (args[1].tag == JvmTag::Int)
          param = static_cast<float>(args[1].v.i);
      }
      return JvmValue::make_obj(game_logic_get_vehicle_descriptor(set, param));
    }
    if (!is_static && !args.empty() && args[0].tag == JvmTag::Obj) {
      int32_t set = 0;
      float param = -1.f;
      if (args.size() >= 2 && args[1].tag == JvmTag::Int) set = args[1].v.i;
      if (args.size() >= 3) {
        if (args[2].tag == JvmTag::Float) param = args[2].v.f;
        else if (args[2].tag == JvmTag::Int)
          param = static_cast<float>(args[2].v.i);
      }
      return JvmValue::make_obj(
          vehicle_type_get_vehicle_descriptor(args[0].v.o, set, param));
    }
  }

  if (!find_class(class_fqn)) {
    if (!load_class(class_fqn)) {
      std::fprintf(stderr, "[jvm] no class %s\n", class_fqn);
      return JvmValue::make_void();
    }
  }
  const JvmClass* cls = find_class(class_fqn);
  if (!cls) return JvmValue::make_void();

  // Walk supers for inherited TREE methods (e.g. VehicleType.init on *_VT).
  const JvmClass* owner = cls;
  const JvmMethod* m = find_method(*cls, name, signature);
  auto method_has_tree = [](const JvmClass& c, const JvmMethod* meth) -> bool {
    if (!meth || meth->tree_index < 0) return false;
    if (static_cast<size_t>(meth->tree_index) >= c.trees.size()) return false;
    return !c.trees[static_cast<size_t>(meth->tree_index)].nodes.empty();
  };
  // City: stub/native <init> MTHD + FILD arrays — skip empty body, use Scene.
  if (m && name && std::strcmp(name, "<init>") == 0 && !method_has_tree(*cls, m) &&
      !cls->field_inits.empty()) {
    m = nullptr;
  }
  if (!m || !method_has_tree(*owner, m)) {
    const JvmMethod* leaf = m;
    const JvmClass* leaf_owner = owner;
    if (m && !method_has_tree(*owner, m)) m = nullptr;
    std::string super = cls->super_name;
    for (int depth = 0; !m && !super.empty() && depth < 16; ++depth) {
      if (!find_class(super.c_str())) load_class(super.c_str());
      owner = find_class(super.c_str());
      if (!owner) break;
      m = find_method(*owner, name, signature);
      if (m && !method_has_tree(*owner, m)) m = nullptr;
      if (m) break;
      super = owner->super_name;
    }
    if (!m) {
      m = leaf;
      owner = leaf_owner;
    }
  }
  if (!m) {
    // Native-only path (no MTHD entry match) — still try table.
    const NativeEntry* ne = resolve_native(class_fqn, name, signature);
    if (ne && ne->fn) {
      CallFrame frame;
      frame.args = args;
      soft_unbox_bind_registered_sig(&frame, ne);
      std::string err;
      if (!call_native(ne, &frame, &err)) {
        std::fprintf(stderr, "[jvm] call_native failed: %s\n", err.c_str());
        return JvmValue::make_void();
      }
      return frame.result;
    }
    std::fprintf(stderr, "[jvm] no method %s.%s%s\n", class_fqn, name, signature);
    return JvmValue::make_void();
  }

  const bool has_tree =
      m->tree_index >= 0 &&
      static_cast<size_t>(m->tree_index) < owner->trees.size() &&
      !owner->trees[static_cast<size_t>(m->tree_index)].nodes.empty();

  if (has_tree) {
    JvmTreeHost host(this);
    auto apply_field_inits = [&](const JvmClass& c, InvObject* self) {
      if (!self || c.field_inits.empty()) return;
      for (const JvmFieldInit& fi : c.field_inits) {
        if (fi.tree_index < 0 ||
            static_cast<size_t>(fi.tree_index) >= c.trees.size())
          continue;
        // Skip heavy FILD trees (SfxRef / bots). City.posGarage NEWARRAY packs
        // to ~40+ nodes; allow up to 96 for array/scalar field inits.
        if (c.trees[static_cast<size_t>(fi.tree_index)].nodes.size() > 96)
          continue;
        JvmMethod fim;
        fim.name = "<fieldinit>";
        fim.signature = "()V";
        fim.tree_index = fi.tree_index;
        std::string ferr;
        JvmValue fv =
            tree_eval(&host, c, fim, {JvmValue::make_obj(self)}, &ferr);
        if (!ferr.empty()) continue;
        if (fv.tag == JvmTag::Obj)
          tree_field_set_obj(self, fi.name.c_str(), fv.v.o);
        else if (fv.tag == JvmTag::Int)
          tree_field_set_int(self, fi.name.c_str(), fv.v.i);
        else if (fv.tag == JvmTag::Float)
          tree_field_set_float(self, fi.name.c_str(), fv.v.f);
      }
    };
    std::vector<JvmValue> locals = args;
    // Invictus static TREE locals are argN..arg0 (reversed). Instance keeps this
    // at [0]; <init> args after this are also reversed (Vector elementData size).
    if (is_static && locals.size() > 1) {
      std::reverse(locals.begin(), locals.end());
    } else if (!is_static && name && std::strcmp(name, "<init>") == 0 &&
               locals.size() > 2) {
      std::reverse(locals.begin() + 1, locals.end());
    }
    // Phase 2.115: FILD before <init> body. Java runs superclass field
    // initializers via super(); City ships stub/native <init> MTHD rows so
    // Valocity.super() often never allocates posGarage — apply the chain.
    if (!is_static && name && std::strcmp(name, "<init>") == 0 &&
        !args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
      std::vector<const JvmClass*> chain;
      for (const JvmClass* c = cls; c;) {
        chain.push_back(c);
        if (c->super_name.empty()) break;
        if (!find_class(c->super_name.c_str()))
          load_class(c->super_name.c_str());
        c = find_class(c->super_name.c_str());
      }
      for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        apply_field_inits(**it, args[0].v.o);
    }
    // Census: how much of the real boot workload the PE stream walker
    // (VMThread_run @ 0x420FF0) could already execute 1:1. Read-only.
    if (const char* c = std::getenv("SLRR_PE_STREAM_CENSUS")) {
      if (c[0] == '1' && m->tree_index >= 0 &&
          static_cast<size_t>(m->tree_index) < owner->trees.size()) {
        static uint32_t n_calls = 0, n_walkable = 0;
        static std::map<uint16_t, uint32_t> blocking;
        uint32_t ok = 0, todo = 0;
        vmthread_stream_census(owner->trees[m->tree_index], &ok, &todo);
        vmthread_stream_blocking_ops(owner->trees[m->tree_index], &blocking);
        ++n_calls;
        if (todo == 0) ++n_walkable;
        if ((n_calls % 500) == 0) {
          std::fprintf(stderr, "[pe-census] tree calls=%u walkable=%u (%.1f%%)\n",
                       n_calls, n_walkable, 100.0 * n_walkable / n_calls);
          // Rank by hit count: the top entry is the next ticket worth taking.
          std::vector<std::pair<uint32_t, uint16_t>> ranked;
          ranked.reserve(blocking.size());
          for (const auto& kv : blocking)
            ranked.emplace_back(kv.second, kv.first);
          std::sort(ranked.rbegin(), ranked.rend());
          std::fprintf(stderr, "[pe-census] blocking opcodes on called trees:\n");
          for (const auto& p : ranked)
            std::fprintf(stderr, "[pe-census]   0x%04X  %u\n", p.second, p.first);
        }
      }
    }
    // PE invokeMethod @ 0x420222 Java path → CallFrame → VMThread_run(0).
    // Walkable trees: same opcode loop via ephemeral thr (no run re-entry).
    // Fallback tree_eval when todo!=0 / SLRR_PE_STREAM=0 / unsupported mid-body.
    std::string err;
    JvmValue r;
    InvObject* stream_self = nullptr;
    if (!is_static && !args.empty() && args[0].tag == JvmTag::Obj)
      stream_self = args[0].v.o;
    if (!vmthread_try_stream_eval(owner, m, stream_self, args, &r))
      r = tree_eval(&host, *owner, *m, std::move(locals), &err);
    if (err.empty()) {
      // Synthetic subclass <init> (City has FILD but no MTHD <init>): after
      // inherited Track.<init>, run the requested class field inits.
      if (!is_static && name && std::strcmp(name, "<init>") == 0 &&
          owner != cls && !args.empty() && args[0].tag == JvmTag::Obj &&
          args[0].v.o) {
        apply_field_inits(*cls, args[0].v.o);
      }
      if (!args.empty() && args[0].tag == JvmTag::Obj && args[0].v.o) {
        InvObject* self = args[0].v.o;
        if (std::strcmp(name, "enter") == 0) {
          tree_field_set_int(self, "enter_via_tree", 1);
          tree_field_set_int(self, "entered", 1);
          if (std::strstr(class_fqn, "Garage")) {
            garage_ensure_map(self);
            garage_ensure_camera(self);
            garage_lock_car(self);
            game_logic_set_played(1);
            InvObject* osd = tree_field_get_obj(self, "osd");
            const int32_t btn =
                osd ? tree_field_get_int(osd, "button_count") : 0;
            // Mechanic/painter inflate button_count; require street strip
            // (CMD_HITTHESTREET=109) before trusting TREE OSD.
            bool street = false;
            if (InvObject* btns =
                    osd ? tree_field_get_obj(osd, "buttons") : nullptr) {
              const int32_t n = tree_vector_size(btns);
              for (int32_t i = 0; i < n; ++i) {
                InvObject* b = tree_vector_element_at(btns, i);
                if (b && tree_field_get_int(b, "command") == 109) {
                  street = true;
                  break;
                }
              }
            }
            if (btn >= 8 && street) {
              tree_field_set_int(self, "osd_via_tree", 1);
              tree_field_set_int(self, "osd_via_host", 0);
              if (osd) tree_field_set_int(osd, "visible", 1);
            } else {
              garage_try_create_osd_objects(self);
            }
          }
          if (std::strstr(class_fqn, "RaceSetup")) {
            racesetup_try_create_osd_objects(self);
          }
          if (std::strstr(class_fqn, "Valocity")) {
            InvObject* prev =
                (args.size() >= 2 && args[1].tag == JvmTag::Obj) ? args[1].v.o
                                                                 : nullptr;
            const char* pc = prev ? tree_host_class(prev) : nullptr;
            const bool from_rs = pc && std::strstr(pc, "RaceSetup");
            // Stock Valocity.enter(RaceSetup): unpause only — do not reseat
            // traffic / raceState=0 (would bounce QUICKRACE back to parent).
            if (!from_rs) valocity_finalize_enter(self);
            // Stock Valocity.enter QUICKRACE: always CAS(GameLogic.racesetup)
            // when prev is not RaceSetup. TREE often packs that CAS as MainMenu
            // (polluted racesetup / wrong stack) — recover here.
            if (game_logic_game_mode() == 3) {
              InvObject* st = game_logic_actual_state();
              const char* sc = st ? tree_host_class(st) : nullptr;
              const bool on_rs = sc && std::strstr(sc, "RaceSetup");
              if (!from_rs && !on_rs) {
                InvObject* player = game_logic_player();
                tree_field_set_obj(self, "challenger", player);
                tree_field_set_int(self, "raceState", 1);
                InvObject* rs = game_logic_racesetup();
                if (!rs) {
                  rs = tree_host_new("java.game.RaceSetup");
                  game_logic_set_racesetup(rs);
                }
                tree_field_set_obj(rs, "lastState", self);
                tree_field_set_obj(rs, "track", self);
                game_logic_change_active_section(rs);
                std::printf(
                    "[script] Valocity QUICKRACE → RaceSetup (stock handoff)\n");
              }
            }
          }
          frontend_loading_screen_hide();
          std::printf("[script] %s.enter via TREE\n", class_fqn);
        } else if (std::strcmp(name, "exit") == 0) {
          tree_field_set_int(self, "exit_via_tree", 1);
          tree_field_set_int(self, "entered", 0);
          if (std::strstr(class_fqn, "Valocity")) {
            valocity_exit(self, nullptr);
          }
          if (std::strstr(class_fqn, "Garage")) {
            InvObject* next =
                (args.size() >= 2 && args[1].tag == JvmTag::Obj) ? args[1].v.o
                                                                 : nullptr;
            garage_exit(self, next);
          }
          std::printf("[script] %s.exit via TREE\n", class_fqn);
        } else if (std::strcmp(name, "osdCommand") == 0) {
          tree_field_set_int(self, "osd_cmd_via_tree", 1);
          if (args.size() >= 2 && args[1].tag == JvmTag::Int)
            tree_field_set_int(self, "last_osd_cmd", args[1].v.i);
          std::printf("[script] %s.osdCommand via TREE\n", class_fqn);
        } else if (std::strcmp(name, "startRace") == 0) {
          tree_field_set_int(self, "start_race_via_tree", 1);
          std::printf("[script] %s.startRace via TREE\n", class_fqn);
        } else if (std::strcmp(name, "handleEvent") == 0) {
          tree_field_set_int(self, "handle_event_via_tree", 1);
          std::printf("[script] %s.handleEvent via TREE\n", class_fqn);
        } else if (std::strcmp(name, "show") == 0) {
          tree_field_set_int(self, "show_via_tree", 1);
          tree_field_set_int(self, "shown", 1);
          if (InvObject* osd = tree_field_get_obj(self, "osd"))
            tree_field_set_int(osd, "visible", 1);
          std::printf("[script] %s.show via TREE\n", class_fqn);
        } else if (std::strcmp(name, "hide") == 0) {
          tree_field_set_int(self, "hide_via_tree", 1);
          tree_field_set_int(self, "shown", 0);
          if (InvObject* osd = tree_field_get_obj(self, "osd"))
            tree_field_set_int(osd, "visible", 0);
          tree_field_set_obj(self, "backShieldOsd", nullptr);
          std::printf("[script] %s.hide via TREE\n", class_fqn);
        }
      }
      return r;
    }
    std::fprintf(stderr, "[jvm] TREE eval %s.%s: %s\n", owner->name.c_str(),
                 name, err.c_str());
    return r;
  }

  const NativeEntry* ne = resolve_native(class_fqn, name, signature);
  if (ne && ne->fn) {
    // Soft PE UnboxArg: descriptor from registered NativeRec, not caller tags.
    CallFrame frame;
    frame.args = args;
    soft_unbox_bind_registered_sig(&frame, ne);
    std::string err;
    if (!call_native(ne, &frame, &err)) {
      std::fprintf(stderr, "[jvm] call_native failed: %s\n", err.c_str());
      return JvmValue::make_void();
    }
    return frame.result;
  }

  std::fprintf(stderr, "[jvm] no native binding %s.%s\n", class_fqn, name);
  return JvmValue::make_void();
}

}  // namespace inv
