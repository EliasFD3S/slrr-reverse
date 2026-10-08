// Fragment 2/6 of GameRef.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.

  // These are NOT first-token strcmp (only nested under pickup).
  // race125: nested ammo no-op; +0xB4 via ammo_frac float stand-in.
  if (std::strcmp(tok, "pickup") == 0) {
    s += 6;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    // Nested form: "pickup health N" / "pickup all" / "pickup ammo" (nop)
    if (std::strncmp(s, "health", 6) == 0 &&
        (s[6] == ' ' || s[6] == '\t' || s[6] == '\0')) {
      s += 6;
      while (*s == ' ' || *s == '\t') ++s;
      const long v = std::strtol(s, &end, 10);
      if (end == s) return;
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& r = ref(self);
      // PE +0xB4 float frac: add then cap at 1.0 — host health int + frac.
      r.health += static_cast<int32_t>(v);
      float frac = tree_field_get_float(self, "ammo_frac") +
                   static_cast<float>(v);
      if (frac > 1.f) frac = 1.f;
      if (frac < 0.f) frac = 0.f;
      tree_field_set_int(self, "health", r.health);
      tree_field_set_float(self, "ammo_frac", frac);
      return;
    }
    if (std::strncmp(s, "ammo", 4) == 0 &&
        (s[4] == ' ' || s[4] == '\t' || s[4] == '\0')) {
      // PE @ 0x459ADE: ammo match → jz exit (no store).
      return;
    }
    if (std::strcmp(s, "all") == 0 ||
        (std::strncmp(s, "all", 3) == 0 &&
         (s[3] == ' ' || s[3] == '\t' || s[3] == '\0'))) {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      ref(self).ammo = 1;  // PE +0xB4 = 1.0f full
      tree_field_set_int(self, "ammo", 1);
      tree_field_set_float(self, "ammo_frac", 1.f);
      return;
    }
    const long v = *s ? std::strtol(s, &end, 10) : 1;
    if (*s && end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    ref(self).pickup = static_cast<int32_t>(v);
    tree_field_set_int(self, "pickup", static_cast<int32_t>(v));
    return;
  }
  // PE has no first-token "health"/"ammo" (only nested under pickup).
  // Host stand-in kept for TREE callers that emit bare tokens.
  if (std::strcmp(tok, "health") == 0) {
    s += 6;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s) return;
    std::lock_guard<std::mutex> lock(g_gr_mu);
    ref(self).health = static_cast<int32_t>(v);
    tree_field_set_int(self, "health", static_cast<int32_t>(v));
    return;
  }
  if (std::strcmp(tok, "ammo") == 0) {
    // PE nested ammo is no-op; bare first-token also no-op (no invent parse).
    return;
  }
  // PE @ 0x45A5DA install / 0x45A6BF remove / 0x45A71A cancel / 0x45A76D repair.
  if (std::strcmp(tok, "install") == 0 || std::strcmp(tok, "remove") == 0 ||
      std::strcmp(tok, "cancel") == 0 || std::strcmp(tok, "repair") == 0) {
    char key[32];
    std::snprintf(key, sizeof(key), "cmd_%s", tok);
    tree_field_set_int(self, key, tree_field_get_int(self, key) + 1);
    return;
  }
  // PE @ 0x45AF19 "updatevariables".
  if (std::strcmp(tok, "updatevariables") == 0) {
    tree_field_set_int(self, "updatevariables",
                       tree_field_get_int(self, "updatevariables") + 1);
    return;
  }
  // PE @ 0x45BBA6 autopilot_on / 0x45BBF7 autopilot_off — store 1/0 at
  // gameInst[+0x180] stride (host: GameRefState.autopilot).
  if (std::strcmp(tok, "autopilot_on") == 0) {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    ref(self).autopilot = 1;
    tree_field_set_int(self, "autopilot", 1);
    return;
  }
  if (std::strcmp(tok, "autopilot_off") == 0) {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    ref(self).autopilot = 0;
    tree_field_set_int(self, "autopilot", 0);
    return;
  }
  if (std::strcmp(tok, "ai_cheat") == 0) {
    tree_field_set_int(self, "ai_cheat",
                       tree_field_get_int(self, "ai_cheat") + 1);
    return;
  }
  // race120/126 — physical EVENT_COMMAND cluster (sub_458C00 @ 0x45AE64..).
  // Light/wing/mslot intrusive lists + rollbar table ported on GameRefState
  // (PE blob +0x206C/+0x2088/+0x20A4/+0x1FBC). Chassis slot tables still OOS.
  // IDA: ResHandle_orPayloadFlags @ 0x48D1F0, RenderRef_applyBoneId @ 0x48BD30.
  // PE @ 0x45AE64 "add_part": scanf "%*s %d" → GameRef_cmdAddPart @ 0x45AE95
  // (alias "addpart") + Chassis_attachPartSlot @ 0x45AEA1 (flag |1 @ +0x88).
  // W9B: cmdAddPart @ 0x447B82 inserts *[phys+0xDC] via physDcList_insertTail.
  // W17D: attachPartSlot @ 0x43F2E0 walks SimObjectList(+0x1154).head@+0x115C
  // — Chassis producer/consumer OOS here (no GameRef list insert). phys+0x78
  // table = Part_buildPhysSlotTable @ 0x46EAE0 (Chassis_allocCamBlob).
  if (std::strcmp(tok, "add_part") == 0) {
    s += 8;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long id = std::strtol(s, &end, 10);
    if (end == s) return;
    const int32_t rid = static_cast<int32_t>(id);
    tree_field_set_int(self, "cmd_add_part",
                       tree_field_get_int(self, "cmd_add_part") + 1);
    tree_field_set_int(self, "add_part_id", rid);
    InvObject* type = resref_find_by_id(rid);
    if (!type) return;
    InvObject* part = gameref_new();
    java_util_resource_GameRef_create(part, self, type, string_new(""),
                                      string_new("addpart"));
    int32_t parent_slot = 0, child_slot = 0;
    if (part_find_cfg_install(self, part, &parent_slot, &child_slot))
      part_install(self, parent_slot, part, child_slot);
    else
      java_util_resource_GameRef_setParent(part, self);
    gameref_phys_dc_add_part(self, part);  // PE @ 0x447B82
    tree_field_set_obj(self, "last_add_part", part);
    return;
  }
  // PE @ 0x45AEC6 "rem_part": scanf "%*s %d" → Chassis_detachPartSlot +
  // GameRef_cmdRemPart (alias "rempart").
  // W9B: cmdRemPart @ 0x448047 walks/unlinks *[phys+0xDC] node.
  if (std::strcmp(tok, "rem_part") == 0) {
    s += 8;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long id = std::strtol(s, &end, 10);
    if (end == s) return;
    const int32_t rid = static_cast<int32_t>(id);
    tree_field_set_int(self, "cmd_rem_part",
                       tree_field_get_int(self, "cmd_rem_part") + 1);
    tree_field_set_int(self, "rem_part_id", rid);
    InvObject* part = resref_find_by_id(rid);
    if (!part) part = tree_field_get_obj(self, "last_add_part");
    if (!part) return;
    gameref_phys_dc_rem_part(self, part);  // PE @ 0x448047
    part_uninstall(part);
    java_util_resource_GameRef_setParent(part, nullptr);
    return;
  }
  // PE @ 0x45AF46 "add_light": scanf "%*s %d %d" (type handle, flags);
  // malloc 0x2C; sub_429130; ResHandle_Rebind(node+0xC, type+0xC);
  // node+0x1C=flags; intensity @ +0x20/+0x28 if flags&8|0x30;
  // list insert +0x206C/+0x2074; unless flags&0x40 → orPayloadFlags 0x400000.
  if (std::strcmp(tok, "add_light") == 0) {
    s += 9;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long type_id = std::strtol(s, &end, 10);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t') ++s;
    const long flags_l = *s ? std::strtol(s, &end, 10) : 0;
    const int32_t flags = static_cast<int32_t>(flags_l);
    tree_field_set_int(self, "cmd_add_light",
                       tree_field_get_int(self, "cmd_add_light") + 1);
    tree_field_set_int(self, "add_light_type_id",
                       static_cast<int32_t>(type_id));
    tree_field_set_int(self, "add_light_flags", flags);
    InvObject* type = resref_find_by_id(static_cast<int32_t>(type_id));
    if (!type) return;
    InvObject* light = gameref_new();
    java_util_resource_ResourceRef_set(
        light, java_util_resource_ResourceRef_id(type));
    java_util_resource_GameRef_setParent(light, self);
    tree_field_set_int(light, "light_flags", flags);
    auto node = std::make_unique<GameRefState::PeLightNode>();
    node->type = type;
    node->flags = flags;
    node->light_obj = light;
    // PE @ 0x45AFC9: ResHandle_Rebind(node+0xC, *(type+0xC)); +0x14←*(rh+8).
    node->match_key = void_event_res_handle_rebind_owner(
        node->rh, type, static_cast<int32_t>(type_id));
    if (node->match_key == 0) node->match_key = static_cast<int32_t>(type_id);
    // PE @ 0x45AFD5: flags&8 → rand intensity; flags&0x30 → (obj+8&0xF)*0.01+0.4
    // flt_5F0DBC=1/32768, flt_5F0D50=0.4, flt_5F0CD4=0.2, flt_5F0C20=0.01,
    // flt_5F09D0=0.5 (IDA bytes @ 0x5F0DBC..).
    if (flags & 8) {
      const float r01 =
          static_cast<float>(std::rand() & 0x7FFF) * (1.f / 32768.f);
      node->f20 = r01 * 0.4f + 0.2f;
      node->f24 = 0.f;
      node->f28 = node->f20 * 0.5f;
    } else if (flags & 0x30) {
      int32_t lo4 = 0;
      {
        std::lock_guard<std::mutex> lock(g_gr_mu);
        lo4 = ref(self).flags & 0xF;
      }
      node->f20 = static_cast<float>(lo4) * 0.01f + 0.4f;
      node->f24 = 0.f;
      node->f28 = node->f20 * 0.5f;
    }
    tree_field_set_float(light, "light_f20", node->f20);
    tree_field_set_float(light, "light_f28", node->f28);
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& st = ref(self);
      GameRefState::PeLightNode* raw = node.get();
      pe_light_insert_tail(st, raw);
      st.light_nodes.push_back(std::move(node));
    }
    // PE ResHandle_orPayloadFlags(type, 0x400000) @ 0x48D1F0 / 0x45B05C
    // when !(flags & 0x40).
    if ((flags & 0x40) == 0) res_handle_or_payload_flags(type, 0x400000);
    tree_field_set_obj(self, "last_add_light", light);
    tree_field_set_int(self, "pe_light_count",
                       tree_field_get_int(self, "pe_light_count") + 1);
    return;
  }
  // PE @ 0x45B077 "rem_light": scanf "%*s %d %d"; walk list from +0x2064;
  // match node+0x14==key; node+0x1C &= ~mask; if flags left keep node;
  // else andNot/or 0x400000 (mask&0x40) + vtbl dtor.
  if (std::strcmp(tok, "rem_light") == 0) {
    s += 9;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long key = *s ? std::strtol(s, &end, 10) : 0;
    if (*s && end == s) return;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long mask_l = *s ? std::strtol(s, &end, 10) : 0;
    const int32_t mask = static_cast<int32_t>(mask_l);
    tree_field_set_int(self, "cmd_rem_light",
                       tree_field_get_int(self, "cmd_rem_light") + 1);
    tree_field_set_int(self, "rem_light_key", static_cast<int32_t>(key));
    tree_field_set_int(self, "rem_light_flags", mask);
    InvObject* light_obj = nullptr;
    InvObject* typ = nullptr;
    bool destroy = false;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& st = ref(self);
      auto* sent = st.light_sent.get();
      GameRefState::PeLightNode* cur =
          sent && sent->next && sent->next != sent ? sent->next : nullptr;
      while (cur && cur != sent) {
        GameRefState::PeLightNode* nxt = cur->next;
        if (cur->match_key == static_cast<int32_t>(key)) {
          cur->flags &= ~mask;
          if (cur->flags != 0) break;
          typ = cur->type;
          light_obj = cur->light_obj;
          destroy = true;
          // PE GameRef_LightNode_dtor @ 0x45F8B0: unlink rh from owner list.
          void_event_res_handle_unbind_owner(cur->rh);
          pe_light_unlink(st, cur);
          for (auto it = st.light_nodes.begin(); it != st.light_nodes.end();
               ++it) {
            if (it->get() == cur) {
              st.light_nodes.erase(it);
              break;
            }
          }
          break;
        }
        cur = (nxt && nxt != sent) ? nxt : nullptr;
      }
    }
    if (destroy) {
      if (light_obj) java_util_resource_GameRef_setParent(light_obj, nullptr);
      // PE @ 0x45B100: mask&0x40 → andNot else or 0x400000 on type handle.
      if (typ) {
        if (mask & 0x40)
          res_handle_and_not_payload_flags(typ, 0x400000);
        else
          res_handle_or_payload_flags(typ, 0x400000);
      }
      if (tree_field_get_obj(self, "last_add_light") == light_obj)
        tree_field_set_obj(self, "last_add_light", nullptr);
      const int32_t n = tree_field_get_int(self, "pe_light_count");
      if (n > 0) tree_field_set_int(self, "pe_light_count", n - 1);
    }
    return;
  }
  // PE @ 0x45B15D "add_wing": scanf "%*s %d %d"; malloc 0x1C; +0x0C=2nd,
  // +0x10=1st handle; list insert +0x2088/+0x2090.
  if (std::strcmp(tok, "add_wing") == 0) {
    s += 8;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long a = std::strtol(s, &end, 10);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t') ++s;
    const long b = *s ? std::strtol(s, &end, 10) : 0;
    tree_field_set_int(self, "cmd_add_wing",
                       tree_field_get_int(self, "cmd_add_wing") + 1);
    tree_field_set_int(self, "add_wing_a", static_cast<int32_t>(a));
    tree_field_set_int(self, "add_wing_b", static_cast<int32_t>(b));
    InvObject* wing = resref_find_by_id(static_cast<int32_t>(a));
    if (wing) java_util_resource_GameRef_setParent(wing, self);
    auto node = std::make_unique<GameRefState::PeWingNode>();
    node->key_a = static_cast<int32_t>(a);
    node->key_b = static_cast<int32_t>(b);
    node->handle_a = wing;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& st = ref(self);
      GameRefState::PeWingNode* raw = node.get();
      pe_wing_insert_tail(st, raw);
      st.wing_nodes.push_back(std::move(node));
    }
    if (wing) tree_field_set_obj(self, "last_add_wing", wing);
    tree_field_set_int(self, "pe_wing_count",
                       tree_field_get_int(self, "pe_wing_count") + 1);
    return;
  }
  // PE @ 0x45B223 "rem_wing": scanf "%*s %d %d"; match list @ +0x2080
  // node+0xC==2nd && node+0x10==1st; release +0x10 ref + vtbl dtor.
  if (std::strcmp(tok, "rem_wing") == 0) {
    s += 8;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long a = *s ? std::strtol(s, &end, 10) : 0;
    if (*s && end == s) return;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long b = *s ? std::strtol(s, &end, 10) : 0;
    tree_field_set_int(self, "cmd_rem_wing",
                       tree_field_get_int(self, "cmd_rem_wing") + 1);
    tree_field_set_int(self, "rem_wing_a", static_cast<int32_t>(a));
    tree_field_set_int(self, "rem_wing_b", static_cast<int32_t>(b));
    InvObject* wing = nullptr;
    bool removed = false;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& st = ref(self);
      auto* sent = st.wing_sent.get();
      GameRefState::PeWingNode* cur =
          sent && sent->next && sent->next != sent ? sent->next : nullptr;
      while (cur && cur != sent) {
        GameRefState::PeWingNode* nxt = cur->next;
        if (cur->key_a == static_cast<int32_t>(a) &&
            cur->key_b == static_cast<int32_t>(b)) {
          wing = cur->handle_a;
          pe_wing_unlink(st, cur);
          for (auto it = st.wing_nodes.begin(); it != st.wing_nodes.end();
               ++it) {
            if (it->get() == cur) {
              st.wing_nodes.erase(it);
              break;
            }
          }
          removed = true;
          break;
        }
        cur = (nxt && nxt != sent) ? nxt : nullptr;
      }
    }
    if (wing) java_util_resource_GameRef_setParent(wing, nullptr);
    if (wing && tree_field_get_obj(self, "last_add_wing") == wing)
      tree_field_set_obj(self, "last_add_wing", nullptr);
    if (removed) {
      const int32_t n = tree_field_get_int(self, "pe_wing_count");
      if (n > 0) tree_field_set_int(self, "pe_wing_count", n - 1);
    }
    return;
  }
  // PE @ 0x45B2F9 "add_rollbar": scanf "%*s %d %d %f %f" (defaults
  // 0x461C4000=1e4, 0x43FA0000=500); *[+0x1FBC]+0x44 count max 8;
  // entry at +0x48+16*i = {i0,i1,f0,f1}. Host: GameRefState.rollbars.
  if (std::strcmp(tok, "add_rollbar") == 0) {
    s += 11;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long i0 = *s ? std::strtol(s, &end, 10) : -1;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long i1 = *s ? std::strtol(s, &end, 10) : -1;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    float f0 = 10000.f, f1 = 500.f;
    if (*s) {
      f0 = std::strtof(s, &end);
      if (end != s) {
        s = end;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s) f1 = std::strtof(s, &end);
      }
    }
    tree_field_set_int(self, "cmd_add_rollbar",
                       tree_field_get_int(self, "cmd_add_rollbar") + 1);
    tree_field_set_int(self, "rollbar_a", static_cast<int32_t>(i0));
    tree_field_set_int(self, "rollbar_b", static_cast<int32_t>(i1));
    tree_field_set_float(self, "rollbar_f0", f0);
    tree_field_set_float(self, "rollbar_f1", f1);
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& st = ref(self);
      if (st.rollbars.size() < 8) {
        GameRefState::PeRollbarEntry e;
        e.a = static_cast<int32_t>(i0);
        e.b = static_cast<int32_t>(i1);
        e.f0 = f0;
        e.f1 = f1;
        st.rollbars.push_back(e);
      }
    }
    return;
  }
  // PE @ 0x45B38A "rem_rollbar": match a/b/f0/f1; splice last entry over.
  if (std::strcmp(tok, "rem_rollbar") == 0) {
    s += 11;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long i0 = *s ? std::strtol(s, &end, 10) : -1;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long i1 = *s ? std::strtol(s, &end, 10) : -1;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    float f0 = 10000.f, f1 = 500.f;
    if (*s) {
      f0 = std::strtof(s, &end);
      if (end != s) {
        s = end;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s) f1 = std::strtof(s, &end);
      }
    }
    tree_field_set_int(self, "cmd_rem_rollbar",
                       tree_field_get_int(self, "cmd_rem_rollbar") + 1);
    tree_field_set_int(self, "rollbar_a", static_cast<int32_t>(i0));
    tree_field_set_int(self, "rollbar_b", static_cast<int32_t>(i1));
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& st = ref(self);
      for (size_t i = 0; i < st.rollbars.size(); ++i) {
        auto& e = st.rollbars[i];
        if (e.a == static_cast<int32_t>(i0) &&
            e.b == static_cast<int32_t>(i1) && e.f0 == f0 && e.f1 == f1) {
          st.rollbars[i] = st.rollbars.back();
          st.rollbars.pop_back();
          break;
        }
      }
    }
    return;
  }
  // PE @ 0x45B46E "add_linked": scanf "%*s %d %d %d %d"; Veh_ensureSceneBound
  // ×2; RenderRef_bindBone("bone00"/"bone01") @ 0x48BC40 +
  // RenderRef_applyBoneId @ 0x48BD30. Host: getBoneId (JNI→bindBone).
  if (std::strcmp(tok, "add_linked") == 0) {
    s += 10;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long i0 = *s ? std::strtol(s, &end, 10) : 0;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long i1 = *s ? std::strtol(s, &end, 10) : -1;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long i2 = *s ? std::strtol(s, &end, 10) : 0;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long i3 = *s ? std::strtol(s, &end, 10) : 0;
    tree_field_set_int(self, "cmd_add_linked",
                       tree_field_get_int(self, "cmd_add_linked") + 1);
    tree_field_set_int(self, "linked_i0", static_cast<int32_t>(i0));
    tree_field_set_int(self, "linked_i1", static_cast<int32_t>(i1));
    InvObject* a = resref_find_by_id(static_cast<int32_t>(i2));
    InvObject* b = resref_find_by_id(static_cast<int32_t>(i3));
    if (a) {
      tree_field_set_obj(self, "linked_a", a);
      int32_t b0 =
          java_util_resource_RenderRef_getBoneId(a, string_new("bone00"));
      int32_t b1 =
          java_util_resource_RenderRef_getBoneId(a, string_new("bone01"));
      tree_field_set_int(self, "linked_bone00", b0);
      tree_field_set_int(self, "linked_bone01", b1);
      // PE RenderRef_applyBoneId @ 0x48BD30 after bindBone.
      render_ref_apply_bone_id(a, b0);
      render_ref_apply_bone_id(a, b1);
    }
    if (b) {
      tree_field_set_obj(self, "linked_b", b);
      int32_t b0 =
          java_util_resource_RenderRef_getBoneId(b, string_new("bone00"));
      int32_t b1 =
          java_util_resource_RenderRef_getBoneId(b, string_new("bone01"));
      render_ref_apply_bone_id(b, b0);
      render_ref_apply_bone_id(b, b1);
    }
    return;
  }
  // PE @ 0x45B6D9 "rem_linked": scanf 4 ints; bindBone bone00/01 then dtor.
  if (std::strcmp(tok, "rem_linked") == 0) {
    s += 10;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long i0 = *s ? std::strtol(s, &end, 10) : 0;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long i1 = *s ? std::strtol(s, &end, 10) : -1;
    tree_field_set_int(self, "cmd_rem_linked",
                       tree_field_get_int(self, "cmd_rem_linked") + 1);
    tree_field_set_int(self, "linked_i0", static_cast<int32_t>(i0));
    tree_field_set_int(self, "linked_i1", static_cast<int32_t>(i1));
    InvObject* a = tree_field_get_obj(self, "linked_a");
    if (a) {
      java_util_resource_RenderRef_getBoneId(a, string_new("bone00"));
      java_util_resource_RenderRef_getBoneId(a, string_new("bone01"));
    }
    tree_field_set_obj(self, "linked_a", nullptr);
    tree_field_set_obj(self, "linked_b", nullptr);
    return;
  }
  // PE @ 0x45B7E3 "add_mslot": scanf "%*s %d %d"; ResListNode_ctor;
  // malloc 0x54; +0x0C/+0x10 keys; mesh memcpy from handle+0x2C/+0x38 OOS;
  // list +0x20A4/+0x20AC.
  if (std::strcmp(tok, "add_mslot") == 0) {
    s += 9;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long a = std::strtol(s, &end, 10);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t') ++s;
    const long b = *s ? std::strtol(s, &end, 10) : 0;
    tree_field_set_int(self, "cmd_add_mslot",
                       tree_field_get_int(self, "cmd_add_mslot") + 1);
    tree_field_set_int(self, "mslot_a", static_cast<int32_t>(a));
    tree_field_set_int(self, "mslot_b", static_cast<int32_t>(b));
    auto node = std::make_unique<GameRefState::PeMslotNode>();
    node->key_a = static_cast<int32_t>(a);
    node->key_b = static_cast<int32_t>(b);
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& st = ref(self);
      GameRefState::PeMslotNode* raw = node.get();
      pe_mslot_insert_tail(st, raw);
      st.mslot_nodes.push_back(std::move(node));
    }
    tree_field_set_int(self, "pe_mslot_count",
                       tree_field_get_int(self, "pe_mslot_count") + 1);
    return;
  }
  // PE @ 0x45B89A "rem_mslot": scanf "%*s %d %d"; unlink @ +0x209C
  // match +0x0C/+0x10.
  if (std::strcmp(tok, "rem_mslot") == 0) {
    s += 9;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long a = *s ? std::strtol(s, &end, 10) : 0;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long b = *s ? std::strtol(s, &end, 10) : 0;
    tree_field_set_int(self, "cmd_rem_mslot",
                       tree_field_get_int(self, "cmd_rem_mslot") + 1);
    tree_field_set_int(self, "mslot_a", static_cast<int32_t>(a));
    tree_field_set_int(self, "mslot_b", static_cast<int32_t>(b));
    bool removed = false;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& st = ref(self);
      auto* sent = st.mslot_sent.get();
      GameRefState::PeMslotNode* cur =
          sent && sent->next && sent->next != sent ? sent->next : nullptr;
      while (cur && cur != sent) {
        GameRefState::PeMslotNode* nxt = cur->next;
        if (cur->key_a == static_cast<int32_t>(a) &&
            cur->key_b == static_cast<int32_t>(b)) {
          pe_mslot_unlink(st, cur);
          for (auto it = st.mslot_nodes.begin(); it != st.mslot_nodes.end();
               ++it) {
            if (it->get() == cur) {
              st.mslot_nodes.erase(it);
              break;
            }
          }
          removed = true;
          break;
        }
        cur = (nxt && nxt != sent) ? nxt : nullptr;
      }
    }
    if (removed) {
      const int32_t n = tree_field_get_int(self, "pe_mslot_count");
      if (n > 0) tree_field_set_int(self, "pe_mslot_count", n - 1);
    }
    return;
  }
  // PE @ 0x45C3FD "AI_GoToTrafficSlow" — also fired from EVENT 0x80 case2
  // after LoadGameInit Rebind @ 0x45C3C9 (W9B). sub_45F780 AI cmd +
  // sub_48B750 (0x80000080) traffic-slow path (OOS). Host counter only.
  if (std::strcmp(tok, "AI_GoToTrafficSlow") == 0) {
    tree_field_set_int(self, "cmd_AI_GoToTrafficSlow",
                       tree_field_get_int(self, "cmd_AI_GoToTrafficSlow") + 1);
    return;
  }
  // W13A — PE @ 0x4594B4 "osd": scanf "%*s %d %d" defaults 0,0.
  // Disasm 0x4594CA..0x45952E: first %d→arg_8=osd.id, second→arg_C=ctrl;
  // findCam(arg_C) @ 0x4594F5; if cam+0x5C!=arg_8 Bind(cam+0x54, arg_8,
  // type=1) @ 0x45952E then Chassis_camApplyBoundSlot @ 0x449680.
  // Java Track: "osd "+osd.id()+" "+con.id(); single-arg → findCam(0).
  // osd.id lands @ cam+0x5C — NOT blob+0x128 (that stays ctrl/activate).
  if (std::strcmp(tok, "osd") == 0) {
    s += 3;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long oid = *s ? std::strtol(s, &end, 10) : 0;
    if (*s && end == s) return;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long cid = *s ? std::strtol(s, &end, 10) : 0;
    const int32_t osd_id = static_cast<int32_t>(oid);
    const int32_t ctrl_id = static_cast<int32_t>(cid);
    InvObject* chassis = tree_field_get_obj(self, "chassis");
    if (!chassis) chassis = self;
    chassis_cam_tables_ensure(chassis);
    const int32_t cam_count = cam_table_count_for(self, chassis);
    int32_t cam_idx = -1;
    int32_t bind_ok = 0;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& r = ref(self);
      r.osd_id = osd_id;
      r.osd_ctrl_id = ctrl_id;
      // PE @ 0x4594EB..0x4594F5: findCam(arg_C=ctrl) start=0 step=1.
      cam_idx = find_cam_index_by_match_id_unlocked(r, cam_count, ctrl_id, 0,
                                                    1);
      if (cam_idx >= 0 && cam_idx < GameRefState::kCamTableMax) {
        auto& row = r.cam_rows[static_cast<size_t>(cam_idx)];
        // PE @ 0x459510..0x459520: if cam+0x5C == osd.id → skip Bind.
        if (row.osd_bound_id != osd_id) {
          // PE @ 0x45952E ResHandle_Bind(cam+0x54, osd.id, type=1, 0).
          bind_ok = res_handle_bind_resolve(row.rh_osd, osd_id, 1);
          row.osd_bound_id = osd_id;  // PE key @ cam+0x5C / blob+0x178
        }
        r.osd_cam_idx = cam_idx;
        r.osd_bind_ok = bind_ok;
      } else {
        r.osd_cam_idx = -1;
        r.osd_bind_ok = 0;
      }
      cam_rows_tree_sync(self, r, cam_count);
    }
    tree_field_set_int(self, "osd_id", osd_id);
    tree_field_set_int(self, "osd_ctrl_id", ctrl_id);
    tree_field_set_int(self, "osd_cam_idx", cam_idx);
    tree_field_set_int(self, "osd_bind_ok", bind_ok);
    tree_field_set_int(self, "osd_bound", osd_id != 0 ? 1 : 0);
    tree_field_set_int(self, "cmd_osd",
                       tree_field_get_int(self, "cmd_osd") + 1);
    // PE @ 0x459537: Chassis_camApplyBoundSlot after osd Bind.
    if (cam_idx >= 0)
      chassis_cam_apply_bound_slot(chassis, cam_idx, osd_id != 0 ? 1 : 0);
    return;
  }
  // PE @ 0x458DD5 "camera_int_viewrange" — scanf float then
  // ResHandle_getPayload camera chain (OOS). Host: store float stand-in.
  if (std::strcmp(tok, "camera_int_viewrange") == 0) {
    s += 20;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end != s) tree_field_set_float(self, "camera_int_viewrange", v);
    tree_field_set_int(self, "cmd_camera_int_viewrange",
                       tree_field_get_int(self, "cmd_camera_int_viewrange") + 1);
    return;
  }
  // PE @ 0x45AAEB texture / 0x45ABCC mesh / 0x45ADD1 paint / decals —
  // counters only (RenderInst paths OOS).
  if (std::strcmp(tok, "texture") == 0 || std::strcmp(tok, "mesh") == 0 ||
      std::strcmp(tok, "paint") == 0 || std::strcmp(tok, "decal") == 0 ||
      std::strcmp(tok, "bigdecal") == 0) {
    char key[32];
    std::snprintf(key, sizeof(key), "cmd_%s", tok);
    tree_field_set_int(self, key, tree_field_get_int(self, key) + 1);
    return;
  }
  // race124/126 — IDA string-xref audit of sub_458C00 (size 0x38fe):
  // first-token strcmp covered. Light/wing/mslot intrusive lists + rollbar
  // table on GameRefState (PE +0x206C/+0x2088/+0x20A4/+0x1FBC). W9B/W11A:
  // EVENT 0x80 case2 LoadGameInit Rebind + phys+0xDC list via cmdAddPart
  // (exported gameref_phys_dc_*); render Bind(vp, RESOURCE_VIEWPORT=0x12)
  // @ 0x458d42 → LookupById @ 0x536820 + Rebind when slot map filled.
  // Remaining gap: Chassis forceUpdate → gameref_phys_dc_*; Engine_addTimer
  // 0x80000080; PE gates +0x1FC0/+0x1FCC/+0x1FD0 GII_CONTROL; full
  // LoadGameInit factory. W12A/W13A: findCam +0x128 rows + activate/render;
  // osd Bind(cam+0x54, osd.id) @ 0x45952E (match=+0x128 stays ctrl).
  // navigator_paint map OSD gated when tiles_absolu (PE updateNavigator
  // @ 0x482D30 has no OSD). Soft: Chassis_camApplyBoundSlot @ 0x449680
  // bones/gauges OOS; route/marker OSD host-only.
  // Phase 2.125 — MouseCursor GameRef EVENT_COMMAND (cursor/move/mode/…).
  auto cursor_owner = [self]() -> InvObject* {
    InvObject* parent = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto it = g_refs.find(self);
      if (it != g_refs.end()) parent = it->second.parent;
    }
    if (!parent) parent = tree_field_get_obj(self, "part_parent");
    if (parent) {
      const char* hc = tree_host_class(parent);
      if (hc && std::strstr(hc, "MouseCursor")) return parent;
    }
    if (InvObject* o = tree_field_get_obj(self, "cursor_owner")) return o;
    InvObject* ic = java_io_Input_cursor();
    if (ic && tree_field_get_obj(ic, "cursor") == self) return ic;
    return parent ? parent : ic;
  };
  auto sync_cursor_xy = [](InvObject* gr, InvObject* owner, float x, float y) {
    tree_field_set_float(gr, "cursor_x", x);
    tree_field_set_float(gr, "cursor_y", y);
    tree_field_set_int(gr, "cursor_set", 1);
    if (owner) {
      tree_field_set_float(owner, "cursor_x", x);
      tree_field_set_float(owner, "cursor_y", y);
      tree_field_set_int(owner, "cursor_set", 1);
    }
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      auto& r = ref(gr);
      r.px = x;
      r.py = y;
      r.pz = 0.f;
    }
  };
  if (std::strncmp(s, "cursor", 6) == 0 &&
      (s[6] == ' ' || s[6] == '\t')) {
    s += 6;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long vp_id = std::strtol(s, &end, 10);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t') ++s;
    const long cam_id = std::strtol(s, &end, 10);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t') ++s;
    const long cur_id = std::strtol(s, &end, 10);
    InvObject* owner = cursor_owner();
    InvObject* vp = resref_find_by_id(static_cast<int32_t>(vp_id));
    InvObject* cam = resref_find_by_id(static_cast<int32_t>(cam_id));
    tree_field_set_int(self, "cursor_vp_id", static_cast<int32_t>(vp_id));
    tree_field_set_int(self, "cursor_cam_id", static_cast<int32_t>(cam_id));
    tree_field_set_int(self, "cursor_self_id", static_cast<int32_t>(cur_id));
    if (vp) tree_field_set_obj(self, "vp", vp);
    if (cam) tree_field_set_obj(self, "camera", cam);
    if (owner) {
      if (vp) tree_field_set_obj(owner, "vp", vp);
      if (cam) tree_field_set_obj(owner, "camera", cam);
      tree_field_set_obj(owner, "cursor", self);
    }
    tree_field_set_int(self, "cursor_bound", 1);
    return;
  }
  if (std::strncmp(s, "move", 4) == 0 &&
      (s[4] == ' ' || s[4] == '\t')) {
    s += 4;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    float x = std::strtof(s, &end);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t' || *s == ',') ++s;
    float y = std::strtof(s, &end);
    if (end == s) return;
    // PE Cursor_handleCommand @ 0x00461D20 "move": write instance+0xA4/+0xA8
    // then clamp each axis to [-1,1] (1.0=0x3F800000, -1.0=0xBF800000).
    if (x > 1.f) x = 1.f;
    if (x < -1.f) x = -1.f;
    if (y > 1.f) y = 1.f;
    if (y < -1.f) y = -1.f;
    sync_cursor_xy(self, cursor_owner(), x, y);
    tree_field_set_int(self, "cursor_moved",
                       tree_field_get_int(self, "cursor_moved") + 1);
    return;
  }
  if (std::strcmp(s, "enable") == 0) {
    tree_field_set_int(self, "cursor_collide", 1);
    if (InvObject* o = cursor_owner())
      tree_field_set_int(o, "cursor_collide", 1);
    return;
  }
  if (std::strcmp(s, "disable") == 0) {
    tree_field_set_int(self, "cursor_collide", 0);
    if (InvObject* o = cursor_owner())
      tree_field_set_int(o, "cursor_collide", 0);
    return;
  }
  if (std::strcmp(s, "lock") == 0) {
    // PE Cursor_handleCommand @ 0x00462320: SysCursor≠0 → Engine_SysCursorLock.
    InvObject* cfg = system_config_host();
    const int32_t sys = cfg ? tree_field_get_int(cfg, "SysCursor") : 1;
    if (sys != 0) input_syscursor_lock();
    tree_field_set_int(self, "cursor_locked", 1);
    if (InvObject* o = cursor_owner())
      tree_field_set_int(o, "cursor_locked", 1);
    return;
  }
  if (std::strcmp(s, "unlock") == 0) {
    // PE Cursor_handleCommand @ 0x0046235A: SysCursor≠0 → Engine_SysCursorUnlock.
    InvObject* cfg = system_config_host();
    const int32_t sys = cfg ? tree_field_get_int(cfg, "SysCursor") : 1;
    if (sys != 0) input_syscursor_unlock();
    tree_field_set_int(self, "cursor_locked", 0);
    if (InvObject* o = cursor_owner())
      tree_field_set_int(o, "cursor_locked", 0);
    return;
  }
  if (std::strncmp(s, "activate", 8) == 0 &&
      (s[8] == ' ' || s[8] == '\t' || s[8] == '\0')) {
    s += 8;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long ctrl_id = *s ? std::strtol(s, &end, 10) : 0;
    if (*s && end == s) return;
    s = end ? end : s;
    while (*s == ' ' || *s == '\t') ++s;
    const long cam_id = *s ? std::strtol(s, &end, 10) : 0;
    InvObject* owner = cursor_owner();
    InvObject* ctrl = ctrl_id
                          ? resref_find_by_id(static_cast<int32_t>(ctrl_id))
                          : nullptr;
    InvObject* cam =
        cam_id ? resref_find_by_id(static_cast<int32_t>(cam_id)) : nullptr;
    tree_field_set_int(self, "cursor_ctrl_id", static_cast<int32_t>(ctrl_id));
    tree_field_set_int(self, "cursor_active", 1);
    {
      // PE @ 0x459109 "activate" also marks instance active (host stand-in).
      std::lock_guard<std::mutex> lock(g_gr_mu);
      ref(self).activated = 1;
    }
    tree_field_set_int(self, "activated", 1);
    if (ctrl) tree_field_set_obj(self, "controller", ctrl);
    if (owner) {
      tree_field_set_int(owner, "cursor_active", 1);
      if (ctrl) tree_field_set_obj(owner, "controller", ctrl);
      if (cam) {
        tree_field_set_obj(owner, "controlledcam", cam);
        tree_field_set_int(self, "cursor_cam_id", static_cast<int32_t>(cam_id));
      }
    }
    return;
  }
  if (std::strcmp(s, "sens") == 0) {
    InvObject* cfg = system_config_host_for_test();
    const float sens =
        cfg ? tree_field_get_float(cfg, "mouseSensitivity") : 0.8f;
    tree_field_set_float(self, "mouse_sensitivity", sens);
    if (InvObject* o = cursor_owner())
      tree_field_set_float(o, "mouse_sensitivity", sens);
    tree_field_set_int(self, "sens_count",
                       tree_field_get_int(self, "sens_count") + 1);
    return;
  }
  if (std::strncmp(s, "mode", 4) == 0 &&
      (s[4] == ' ' || s[4] == '\t')) {
    s += 4;
    while (*s == ' ' || *s == '\t') ++s;
    char* end = nullptr;
    const long mode = std::strtol(s, &end, 10);
    if (end == s) return;
    s = end;
    while (*s == ' ' || *s == '\t') ++s;
    char ptr_ch = 0;
    if (*s && *s != ' ' && *s != '\t') {
      ptr_ch = *s;
      ++s;
    }
    while (*s == ' ' || *s == '\t') ++s;
    const long charset_id = *s ? std::strtol(s, &end, 10) : 0;
    InvObject* owner = cursor_owner();
    tree_field_set_int(self, "cursor_mode", static_cast<int32_t>(mode));
    tree_field_set_int(self, "cursor_charset_id",
                       static_cast<int32_t>(charset_id));
    char ptr_buf[2] = {ptr_ch ? ptr_ch : 'J', 0};
    tree_field_set_obj(self, "pointer", string_new(ptr_buf));
    if (owner) {
      tree_field_set_int(owner, "cursor_mode", static_cast<int32_t>(mode));
      tree_field_set_obj(owner, "pointer", string_new(ptr_buf));
      if (InvObject* cs =
              charset_id
                  ? resref_find_by_id(static_cast<int32_t>(charset_id))
                  : nullptr)
        tree_field_set_obj(owner, "pointer_charset", cs);
    }
    // Host Win32 look: digit → stock RT_CURSOR id; else default arrow (2).
    int32_t stock = 2;
    if (ptr_ch >= '2' && ptr_ch <= '9') stock = ptr_ch - '0';
    else if (ptr_ch == '0' || ptr_ch == '1')
      stock = 10 + (ptr_ch - '0');  // '0'→10, '1'→11
    render_d3d9_set_stock_cursor(stock);
    tree_field_set_int(self, "cursor_stock_id", stock);
    return;
  }
  if (std::strncmp(s, "install", 7) != 0) return;
  s += 7;

  float ftok[8] = {};
  int n = 0;
  const char* p = s;
  while (*p && n < 8) {
    while (*p == ' ' || *p == '\t' || *p == ',') ++p;
    if (!*p) break;
    char* end = nullptr;
    const float v = std::strtof(p, &end);
    if (end == p) break;
    ftok[n++] = v;
    p = end;
  }
  if (n < 2) return;

  const int32_t dest_id = static_cast<int32_t>(ftok[1]);
  InvObject* dest = resref_find_by_id(dest_id);
  if (!dest) return;

  int32_t child_slot = 1;
  int32_t parent_slot = 1;
  bool have_pos = false;
  float px = 0, py = 0, pz = 0;
  if (n >= 8) {
    // install 0 dest mySlot dest2 destSlot x y z
    if (static_cast<int32_t>(ftok[2]) > 0)
      child_slot = static_cast<int32_t>(ftok[2]);
    if (static_cast<int32_t>(ftok[4]) > 0)
      parent_slot = static_cast<int32_t>(ftok[4]);
    px = ftok[5];
    py = ftok[6];
    pz = ftok[7];
    have_pos = true;
  } else if (n >= 5) {
    // install 0 dest mySlot dest2 destSlot
    if (static_cast<int32_t>(ftok[2]) > 0)
      child_slot = static_cast<int32_t>(ftok[2]);
    if (static_cast<int32_t>(ftok[4]) > 0)
      parent_slot = static_cast<int32_t>(ftok[4]);
  }

  if (!part_install(dest, parent_slot, self, child_slot)) return;
  java_util_resource_GameRef_setParent(self, dest);
  // Part.addPart commands on `xa`; keep script instance parent in sync.
  InvObject* script = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    auto it = g_refs.find(self);
    if (it != g_refs.end()) script = it->second.script;
  }
  if (script && script != self)
    java_util_resource_GameRef_setParent(script, dest);
  if (have_pos)
    java_util_resource_GameRef_setMatrix(self, vec3_new(px, py, pz), nullptr);
}


// → GameRef_collision.cpp

namespace {
InvObject* g_player = nullptr;
InvObject* g_garage = nullptr;
InvObject* g_loading_screen = nullptr;
InvObject* g_gfx_engine = nullptr;
InvObject* g_large_font = nullptr;
InvObject* g_medium_font = nullptr;
InvObject* g_small_font = nullptr;
InvObject* g_pointers = nullptr;
InvObject* g_def_loading_pic = nullptr;
InvObject* g_input_queue = nullptr;
InvObject* g_hotkey_thread = nullptr;
InvObject* g_hotkey_watcher = nullptr;
int32_t g_frontend_inited = 0;
InvObject* g_actual_state = nullptr;
constexpr int32_t kMenuSet = 2;
constexpr int32_t kGmCareer = 1;
constexpr int32_t kInitialMoney = 20000;
constexpr float kInitialPrestige = 0.3f;

int32_t g_game_mode = 0;
int32_t g_timeout = 0;
int32_t g_career_in_progress = 0;
int32_t g_day = 0;
float g_time_of_day = 0.f;
int32_t g_played = 0;
int32_t g_saved = 0;
int32_t g_auto_save_calls = 0;
int32_t g_auto_save_quiet_calls = 0;
int32_t g_load_defaults_calls = 0;
InvObject* g_car_desc_new = nullptr;
InvObject* g_car_desc_used = nullptr;
float g_dealer_ts_new = 0.f;
float g_dealer_ts_used = 0.f;
InvObject* g_dialog_modal = nullptr;
std::string g_dialog_smoke_string = "Player";
InvObject* g_recent_modal_dialog = nullptr;  // last new *Dialog (not hub)
InvObject* g_racesetup = nullptr;

bool g_hub_tree_defer = false;
enum class HubCasKind { None, Garage, Valocity, Exit };
HubCasKind g_hub_cas_pending = HubCasKind::None;
}  // namespace

// (hub helpers use anonymous-namespace state in this TU.)

void main_menu_hub_begin_tree() {
  g_hub_tree_defer = true;
  g_hub_cas_pending = HubCasKind::None;
}
void main_menu_hub_end_tree() { g_hub_tree_defer = false; }
bool main_menu_hub_deferring() { return g_hub_tree_defer; }
bool main_menu_cmd_new_cas_pending() {
  return g_hub_cas_pending == HubCasKind::Garage;
}
bool main_menu_cmd_freeride_cas_pending() {
  return g_hub_cas_pending == HubCasKind::Valocity;
}
bool main_menu_cmd_exit_cas_pending() {
  return g_hub_cas_pending == HubCasKind::Exit;
}
void main_menu_hub_note_cas(InvObject* next) {
  if (!next) {
    g_hub_cas_pending = HubCasKind::Exit;
    return;
  }
  const char* nn = tree_host_class(next);
  if (!nn) return;
  if (std::strstr(nn, "Garage"))
    g_hub_cas_pending = HubCasKind::Garage;
  else if (std::strstr(nn, "Valocity") || std::strstr(nn, "RaceSetup"))
    g_hub_cas_pending = HubCasKind::Valocity;
}
// Compat aliases used by older call sites / tree_interp.
void main_menu_cmd_new_begin_tree() { main_menu_hub_begin_tree(); }
void main_menu_cmd_new_end_tree() { main_menu_hub_end_tree(); }
void main_menu_cmd_new_note_garage_cas() {
  g_hub_cas_pending = HubCasKind::Garage;
}
bool main_menu_cmd_new_deferring_osd() { return g_hub_tree_defer; }

int32_t game_logic_auto_save() {
  // Stock GameLogic.autoSave — skip WarningDialog/Sfx; always allow continue.
  ++g_auto_save_calls;
  if (g_played && !g_saved && g_game_mode == kGmCareer && g_career_in_progress) {
    game_logic_auto_save_quiet();
    g_saved = 1;
  }
  return 1;  // 1-OK; 0-cancelled (never yet)
}

void game_logic_auto_save_quiet() {
  ++g_auto_save_quiet_calls;
  // Stock writes career save dir; host marks saved without disk I/O yet.
  if (g_game_mode == kGmCareer && g_career_in_progress) g_saved = 1;
}

int32_t game_logic_auto_save_calls() { return g_auto_save_calls; }
int32_t game_logic_auto_save_quiet_calls() { return g_auto_save_quiet_calls; }
int32_t game_logic_load_defaults_calls() { return g_load_defaults_calls; }

InvObject* game_logic_boot_player_garage() {
  InvObject* ctrl = input_init_controllers();
  InvObject* player = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    if (!g_player) {
      g_player = tree_host_new("java.game.Player");
      tree_field_set_obj(g_player, "controller", ctrl);
      tree_field_set_obj(g_player, "decals", tree_vector_new());
      tree_field_set_int(g_player, "hints", 0);
      tree_field_set_int(g_player, "flags", 0);
      InvObject* carlot = tree_host_new("java.game.CarLot");
      tree_field_set_obj(carlot, "player", g_player);
      tree_field_set_int(carlot, "parks", 10);
      tree_field_set_int(carlot, "floors", 10);
      tree_field_set_obj(g_player, "carlot", carlot);
    } else {
      tree_field_set_obj(g_player, "controller", ctrl);
      if (!tree_field_get_obj(g_player, "carlot")) {
        InvObject* carlot = tree_host_new("java.game.CarLot");
        tree_field_set_obj(carlot, "player", g_player);
        tree_field_set_obj(g_player, "carlot", carlot);
      }
    }
    if (!g_garage) g_garage = tree_host_new("java.game.Garage");
    player = g_player;
  }
  // Outside g_gr_mu: ResourceRef_set → gameref_on_res_bound also takes g_gr_mu.
  inventory_ensure_player_parts(player);
  return player;
}

InvObject* game_logic_player() {
  std::lock_guard<std::mutex> lock(g_gr_mu);
  return g_player;
}

InvObject* game_logic_garage() {
  std::lock_guard<std::mutex> lock(g_gr_mu);
  return g_garage;
}

InvObject* game_logic_racesetup() {
  std::lock_guard<std::mutex> lock(g_gr_mu);
  return g_racesetup;
}

void game_logic_set_racesetup(InvObject* rs) {
  // Stock GameLogic.racesetup is RaceSetup only — TREE putstatic packing
  // sometimes writes MainMenu/prev_state onto the static.
  if (rs) {
    const char* hc = tree_host_class(rs);
    if (!hc || !std::strstr(hc, "RaceSetup")) return;
  }
  std::lock_guard<std::mutex> lock(g_gr_mu);
  g_racesetup = rs;
}

InvObject* frontend_loading_screen() {
  InvObject* ls = nullptr;
  bool need_start = false;
  {
    std::lock_guard<std::mutex> lock(g_gr_mu);
    if (!g_loading_screen) {
      g_loading_screen = tree_host_new("java.render.LoadingScreen");
      tree_field_set_int(g_loading_screen, "visible", 0);
      tree_field_set_int(g_loading_screen, "init", 0);
      // Stock: Object termSig = new Object() — woken by hide().
      tree_field_set_obj(g_loading_screen, "termSig",
                         tree_host_new("java.lang.Object"));
      need_start = true;
    }
    ls = g_loading_screen;
  }
  // Stock LoadingScreen() { start(); }. Defer OS thread — concurrent
  // frontend_loading_screen_run + SplashScreen.enter TREE raced to stack overflow.
  // Arm via frontend_loading_screen_show() when a real loading gate is needed.
  if (need_start) tree_field_set_int(ls, "engine_ls_started", 0);
  return ls;
}

void frontend_loading_screen_show() {
  InvObject* ls = frontend_loading_screen();
  if (!tree_field_get_int(ls, "engine_ls_started")) {
    java_lang_Thread_start(ls);
    tree_field_set_int(ls, "engine_ls_started", 1);
  }
  tree_field_set_int(ls, "visible", 1);
  tree_field_set_int(ls, "init", 1);
  // Stock LoadingScreen.show / track → notify() wakes run() after hide()/wait().
  java_lang_Object_notify(ls);
}

void frontend_loading_screen_hide() {
  InvObject* ls = frontend_loading_screen();
  tree_field_set_int(ls, "visible", 0);
  tree_field_set_int(ls, "hide_gen", tree_field_get_int(ls, "hide_gen") + 1);
  InvObject* term = tree_field_get_obj(ls, "termSig");
  if (term) java_lang_Object_notifyAll(term);
}

void frontend_loading_screen_show_dialog(InvObject* self, InvObject* dlg) {
  if (!self) self = frontend_loading_screen();
  tree_field_set_int(self, "init", 1);
  tree_field_set_int(self, "visible", 1);
  if (dlg && !tree_field_get_obj(self, "loadingDialog")) {
    tree_field_set_obj(self, "loadingDialog", dlg);
    tree_field_set_int(dlg, "shown", 1);
    // Minimal LoadingDialog.show: Text stub for FlashText path.
    if (!tree_field_get_obj(dlg, "loadingText"))
      tree_field_set_obj(dlg, "loadingText",
                         tree_host_new("java.render.osd.Text"));
    java_lang_System_setLdPriority(1);
  }
  java_lang_Object_notify(self);
}

void frontend_loading_screen_track(InvObject* self, int32_t wait_for_user) {
  if (!self) self = frontend_loading_screen();
  tree_field_set_int(self, "waitForUser", wait_for_user);
  java_lang_System_isLoadingReset();
  frontend_loading_screen_show();
  java_lang_Object_notify(self);
}

static void loading_screen_block_until_hide(InvObject* self) {
  // Poll hide_gen + flush on THIS thread (Garage.display blocks here).
  // Avoid Object.wait + foreign flush threads — D3D9 is not thread-safe.
  const int32_t gen0 = tree_field_get_int(self, "hide_gen");
  tree_field_set_int(self, "user_wait_blocked", 1);
  for (int i = 0; i < 20000; ++i) {
    if (tree_field_get_int(self, "hide_gen") != gen0) break;
    render_d3d9_flush();
    java_lang_Thread_sleep(5.f);
  }
  tree_field_set_int(self, "user_wait_blocked", 0);
}

void frontend_loading_screen_user_wait(InvObject* self, float sec) {
  if (!self) self = frontend_loading_screen();
  // Fresh termSig — avoid sticky notifyAll tokens from earlier hide().
  InvObject* term = tree_host_new("java.lang.Object");
  tree_field_set_obj(self, "termSig", term);
  tree_field_set_float(self, "waitForUserTimeLimit", sec);
  // Stock: track((int)sec) — nonzero arms waitForUser in run kill path.
  frontend_loading_screen_track(self, static_cast<int32_t>(sec));
  loading_screen_block_until_hide(self);
  (void)term;
}

void frontend_loading_screen_display(InvObject* self, InvObject* dlg,
                                    float wait_limit) {
  if (!self) self = frontend_loading_screen();
  InvObject* term = tree_host_new("java.lang.Object");
  tree_field_set_obj(self, "termSig", term);
  if (dlg)
    frontend_loading_screen_show_dialog(self, dlg);
  else
    frontend_loading_screen_show();
  tree_field_set_float(self, "waitForUserTimeLimit", wait_limit);
  // Stock: if (waitForUserTimeLimit) track(1); else track(0);
  frontend_loading_screen_track(self, wait_limit != 0.f ? 1 : 0);
  loading_screen_block_until_hide(self);
  (void)term;
}

void frontend_soft_timer_run(InvObject* self) {
  if (!self) return;
  InvObject* listener = tree_field_get_obj(self, "listener");
  float wait_time = tree_field_get_float(self, "waitTime");
  if (wait_time < 0.f) wait_time = 0.f;
  java_lang_Thread_sleep(wait_time * 1000.f);
  if (listener) java_lang_Object_notify(listener);
  tree_field_set_int(self, "engine_soft_fired", 1);
}

void frontend_text_change_text(InvObject* self, InvObject* text) {
  if (!self) return;
  tree_field_set_obj(self, "text", text);
  if (text)
    render_d3d9_text_set_string(self, string_cstr(text));
  else
    render_d3d9_text_set_string(self, "");
  render_d3d9_text_update(self);
}

void frontend_flash_text_run(InvObject* self) {
  // Stock FlashText.run — blink "PRESS ENTER TO CONTINUE..."
  if (!self) return;
  InvObject* th = tree_field_get_obj(self, "engine_thread");
  InvObject* flash = tree_field_get_obj(self, "flash");
  int32_t ticks = 0;
  while (th && java_lang_Thread_isAlive(th)) {
    frontend_text_change_text(flash, string_new("PRESS ENTER TO CONTINUE..."));
    tree_field_set_int(self, "flash_on", 1);
    java_lang_Thread_sleep(600.f);
    if (!java_lang_Thread_isAlive(th)) break;
    frontend_text_change_text(flash, nullptr);
    tree_field_set_int(self, "flash_on", 0);
    ++ticks;
    tree_field_set_int(self, "flash_ticks", ticks);
    java_lang_Thread_sleep(600.f);
  }
}

void frontend_loading_screen_run(InvObject* self) {
  // Mirrors LoadingScreen.run (+ waitForUser / SoftTimer / FlashText).
  if (!self) self = frontend_loading_screen();
  for (;;) {
    if (!java_lang_Thread_isAlive(self)) break;
    if (tree_field_get_int(self, "engine_ls_stop") != 0) break;

    java_lang_Object_wait(frontend_gfx_engine());

    int32_t kill = 0;
    const int32_t init = tree_field_get_int(self, "init");
    if (!init) {
      if (!java_lang_System_isLoading()) kill = 1;
    } else {
      tree_field_set_int(self, "init", init - 1);
    }

    if (kill) {
      const int32_t wait_user = tree_field_get_int(self, "waitForUser");
      InvObject* dlg = tree_field_get_obj(self, "loadingDialog");
      if (wait_user && dlg) {
        InvObject* loading_text = tree_field_get_obj(dlg, "loadingText");
        if (loading_text) {
          java_render_Text_create(loading_text, nullptr, frontend_large_font(),
                                  0.9f, 0.8f);
          java_render_Text_changeAlign(loading_text, 0);  // ALIGN_RIGHT
          InvObject* flasher = tree_host_new("java.render.FlashText");
          tree_field_set_obj(flasher, "flash", loading_text);
          InvObject* fth = tree_host_new("java.lang.Thread");
          tree_field_set_obj(fth, "target", flasher);
          tree_field_set_obj(flasher, "engine_thread", fth);
          java_lang_Thread_init(fth, string_new("Dialog text flasher"));
          java_lang_Thread_start(fth);
          tree_field_set_obj(self, "flashTextThread", fth);
          tree_field_set_obj(self, "flashTextRunner", flasher);
        }
        const float lim = tree_field_get_float(self, "waitForUserTimeLimit");
        if (lim >= 0.f) {
          const float sleep_sec = lim;
          InvObject* listener = dlg;
          std::thread([listener, sleep_sec]() {
            java_lang_Thread_sleep(sleep_sec * 1000.f);
            java_lang_Object_notify(listener);
          }).detach();
        }
        tree_field_set_int(self, "dialog_wait", 1);
        java_lang_Object_wait(dlg);
        tree_field_set_int(self, "dialog_wait", 0);
        if (InvObject* fth = tree_field_get_obj(self, "flashTextThread")) {
          java_lang_Thread_stop(fth);
          tree_field_set_obj(self, "flashTextThread", nullptr);
        }
      }
      tree_field_set_int(self, "waitForUser", 0);
      tree_field_set_obj(self, "loadingDialog", nullptr);
      frontend_loading_screen_hide();
      tree_field_set_int(self, "run_parked", 1);
      java_lang_Object_wait(self);
      tree_field_set_int(self, "run_parked", 0);
    } else {
      java_lang_Thread_sleep(300.f);
    }
  }
}

InvObject* frontend_gfx_engine() {
  std::lock_guard<std::mutex> lock(g_gr_mu);
  if (!g_gfx_engine) {
    // Frontend.render = new GfxEngine(); LoadingScreen waits on this instance.
    g_gfx_engine = tree_host_new("java.render.GfxEngine");
  }
  return g_gfx_engine;
}

void frontend_gfx_engine_frame_notify() {
  // Present path: wake Frontend.render.wait() once per frame.
  java_lang_Object_notify(frontend_gfx_engine());
}

namespace {

int32_t frontend_rid(uint16_t local) {
  if (!rpak_find_by_name("frontend.rpk"))
    java_lang_System_openLib(string_new("frontend.rpk"));
  const RpakPack* p = rpak_find_by_name("frontend.rpk");
  if (!p) return static_cast<int32_t>(local);
  return rpak_make_id(p->pack_id, local);
}

InvObject* frontend_make_ref(uint16_t local) {
  InvObject* r = resref_new();
  java_util_resource_ResourceRef_set(r, frontend_rid(local));
  return r;
}

}  // namespace

void frontend_set_fonts() {
  // Text.RID_* locals (frontend.rpk).
  constexpr uint16_t kSlII24 = 0x0025;
  constexpr uint16_t kSlII17 = 0x0139;
  constexpr uint16_t kSlII11 = 0x0138;
  constexpr uint16_t kConsole10 = 0x0023;
  constexpr uint16_t kConsole5 = 0x00B2;
  constexpr uint16_t kPointers = 0x003C;

  InvObject* cfg = system_config_host_for_test();
  int32_t vx = cfg ? tree_field_get_int(cfg, "video_x") : 800;
  int32_t vy = cfg ? tree_field_get_int(cfg, "video_y") : 600;
  if (render_d3d9_ready()) {
    vx = render_d3d9_width();
    vy = render_d3d9_height();
  }

  g_pointers = frontend_make_ref(kPointers);

  uint16_t large = kConsole5, medium = kConsole5, small = kConsole5;
  if (vy >= 1024 || vy >= 768) {
    large = kSlII24;
    medium = kSlII24;
    small = kSlII17;
  } else if (vx >= 800 || vy >= 600) {
    large = kSlII24;
    medium = kSlII17;
    small = kSlII11;
  } else if (vx >= 640 || vy >= 480) {
    large = kSlII17;
    medium = kSlII11;
    small = kConsole5;
  } else if (vy >= 320 || vy >= 240) {
    large = kConsole10;
    medium = kConsole10;
    small = kConsole5;
  }

  g_large_font = frontend_make_ref(large);
  g_medium_font = frontend_make_ref(medium);
  g_small_font = frontend_make_ref(small);
}

void frontend_init() {
  frontend_gfx_engine();
  if (!g_def_loading_pic)
    g_def_loading_pic = frontend_make_ref(0x00A8);  // frontend:0xA8
  if (!g_input_queue) g_input_queue = tree_vector_new();
  frontend_set_fonts();
  frontend_loading_screen();

  // Defer HotkeyWatcher thread — concurrent invoke() during SplashScreen.enter
  // TREE raced the main boot thread (stack overflow). Started from --game after
  // Splash→MainMenu, or frontend_start_hotkey_watcher().
  g_frontend_inited = 1;
}

void frontend_start_hotkey_watcher() {
  if (g_hotkey_thread) return;
  g_hotkey_watcher = tree_host_new("java.render.HotkeyWatcher");
  g_hotkey_thread = tree_host_new("java.lang.Thread");
  tree_field_set_obj(g_hotkey_thread, "target", g_hotkey_watcher);
  tree_field_set_obj(g_hotkey_watcher, "engine_thread", g_hotkey_thread);
  java_lang_Thread_init(g_hotkey_thread, string_new("Hotkey watcher"));
  java_lang_Thread_start(g_hotkey_thread);
}

void osd_ensure_defaults(InvObject* osd) {
  // Stock Osd(Viewport): Vectors + default Group(this).
  if (!osd) return;
  if (!tree_field_get_obj(osd, "groups"))
    tree_field_set_obj(osd, "groups", tree_vector_new());
  if (!tree_field_get_obj(osd, "hotkey"))
    tree_field_set_obj(osd, "hotkey", tree_vector_new());
  if (!tree_field_get_obj(osd, "rectangles"))
    tree_field_set_obj(osd, "rectangles", tree_vector_new());
  if (!tree_field_get_obj(osd, "text"))
    tree_field_set_obj(osd, "text", tree_vector_new());
  if (!tree_field_get_obj(osd, "object"))
    tree_field_set_obj(osd, "object", tree_vector_new());
  if (tree_field_get_float(osd, "vpWidth") <= 0.f) {
    tree_field_set_float(osd, "vpWidth", 1.f);
    tree_field_set_float(osd, "vpHeight", 1.f);
    tree_field_set_float(osd, "vpAspect",
                         render_d3d9_ready() && render_d3d9_height() > 0
                             ? static_cast<float>(render_d3d9_width()) /
                                   static_cast<float>(render_d3d9_height())
                             : 4.f / 3.f);
  }
  if (!tree_field_get_int(osd, "iLevel"))
    tree_field_set_int(osd, "iLevel", 3);  // IL_MENU
  InvObject* groups = tree_field_get_obj(osd, "groups");
  if (groups && tree_vector_size(groups) == 0) {
    InvObject* g = tree_host_new("java.render.osd.Group");
    tree_field_set_obj(g, "osd", osd);
    tree_field_set_obj(g, "hotkey", tree_vector_new());
    tree_field_set_int(g, "active", 1);
    tree_vector_add(groups, g);
  }
}

InvObject* osd_create_rectangle(InvObject* osd, float x, float y, float w,
                                float h, int32_t pri, InvObject* texture) {
  // Stock Osd.createRectangle → new java.render.Rectangle(group, pos, tmpl).
  // TREE often names java.render.osd.Rectangle (no such .class).
  if (!osd) return nullptr;
  osd_ensure_defaults(osd);
  InvObject* rect = tree_host_new("java.render.Rectangle");
  tree_field_set_obj(rect, "texture", texture);
  tree_field_set_float(rect, "x", x);
  tree_field_set_float(rect, "y", y);
  tree_field_set_float(rect, "w", w);
  tree_field_set_float(rect, "h", h);
  tree_field_set_int(rect, "pri", pri);
  InvObject* rects = tree_field_get_obj(osd, "rectangles");
  if (rects) tree_vector_add(rects, rect);
  if (texture) java_util_resource_ResourceRef_load(texture);
  render_d3d9_osd_add_rect(x, y, w, h, texture, pri);
  return rect;
}

InvObject* osd_create_bg(InvObject* osd, InvObject* texture) {
  // Stock Osd.createBG → createRectangle(0,0,2,2,-2,texture).
  InvObject* rect = osd_create_rectangle(osd, 0.f, 0.f, 2.f, 2.f, -2, texture);
  if (!osd) return rect;
  tree_field_set_int(osd, "bg_created", 1);
  if (texture) tree_field_set_obj(osd, "bg", texture);
  return rect;
}

InvObject* osd_create_hotkey(InvObject* osd, int32_t key, int32_t flags,
                             int32_t command, InvObject* handler, int32_t ef) {
  // Stock Osd.createHotkey — HK_STATIC → osd.hotkey + Input.createHotkey.
  if (!osd) return nullptr;
  osd_ensure_defaults(osd);
  constexpr int32_t kHkStatic = 0x04;
  if (ef == 0) ef = 1;  // Event.F_KEY_PRESS
  int32_t f = flags;
  const bool is_static = (f & kHkStatic) != 0;
  if (is_static) f &= ~kHkStatic;

  InvObject* hk = tree_host_new("java.render.osd.Hotkey");
  tree_field_set_int(hk, "key", key);
  tree_field_set_int(hk, "flags", f);
  tree_field_set_int(hk, "command", command);
  tree_field_set_int(hk, "eventFilter", ef);
  tree_field_set_obj(hk, "handler", handler);
  tree_field_set_obj(hk, "osd", osd);

  if (is_static) {
    tree_vector_add(tree_field_get_obj(osd, "hotkey"), hk);
  } else {
    InvObject* groups = tree_field_get_obj(osd, "groups");
    InvObject* g = groups && tree_vector_size(groups) > 0
                       ? tree_vector_element_at(groups, tree_vector_size(groups) - 1)
                       : nullptr;
    if (g) {
      if (!tree_field_get_obj(g, "hotkey"))
        tree_field_set_obj(g, "hotkey", tree_vector_new());
      tree_vector_add(tree_field_get_obj(g, "hotkey"), hk);
    } else {
      tree_vector_add(tree_field_get_obj(osd, "hotkey"), hk);
    }
  }
  java_io_Input_createHotkey(key, f, hk, handler, osd, ef);
  tree_field_set_int(osd, "hotkey_count",
                     tree_vector_size(tree_field_get_obj(osd, "hotkey")));
  return hk;
}

namespace {
int32_t osd_next_text_key(InvObject* osd) {
  const int32_t n = tree_field_get_int(osd, "text_blit_count");
  tree_field_set_int(osd, "text_blit_count", n + 1);
  return n;
}

void osd_ensure_font_ready(void** font_io) {
  if (!font_io) return;
  void* f = *font_io;
  if (!f) f = frontend_medium_font();
  auto try_load = [](void* f) -> bool {
    if (!f) return false;
    if (render_d3d9_font_ready(f)) return true;
    const int32_t rid =
        java_util_resource_ResourceRef_id(reinterpret_cast<InvObject*>(f));
    if (rid && render_d3d9_font_load_from_rid(f, rid)) return true;
    return render_d3d9_font_load(f, "simple20") != 0;
  };
  if (!try_load(f)) {
    f = frontend_medium_font();
    try_load(f);
  }
  if (!f || !render_d3d9_font_ready(f)) {
    f = frontend_large_font();
    try_load(f);
  }
  *font_io = (f && render_d3d9_font_ready(f)) ? f : nullptr;
}

void osd_blit_label(InvObject* osd, const char* text, InvObject* font, float x,
                    float y, int32_t align) {
  if (!osd || !text) return;
  void* f = font;
  osd_ensure_font_ready(&f);
  if (!f) return;
  void* key = reinterpret_cast<void*>(static_cast<uintptr_t>(
      0x6D500000u + static_cast<uint32_t>(osd_next_text_key(osd))));
  render_d3d9_text_create(key, f, x, y);
  render_d3d9_text_set_string(key, text);
  render_d3d9_text_set_align(key, align);
  render_d3d9_text_update(key);
}
}  // namespace

InvObject* osd_create_text(InvObject* osd, const char* text, InvObject* font,
                           int32_t align, float x, float y) {
  if (!osd) return nullptr;
  osd_ensure_defaults(osd);
  InvObject* txt = tree_host_new("java.render.Text");
  tree_field_set_obj(txt, "string", string_new(text ? text : ""));
  tree_field_set_obj(txt, "font", font);
  tree_field_set_int(txt, "align", align);
  tree_field_set_float(txt, "x", x);
  tree_field_set_float(txt, "y", y);
  if (!tree_field_get_obj(osd, "text"))
    tree_field_set_obj(osd, "text", tree_vector_new());
  tree_vector_add(tree_field_get_obj(osd, "text"), txt);
  tree_field_set_int(osd, "text_count",
                     tree_vector_size(tree_field_get_obj(osd, "text")));
  osd_blit_label(osd, text, font, x, y, align);
  return txt;
}

InvObject* osd_create_header(InvObject* osd, const char* title) {
  if (!osd) return nullptr;
  osd_ensure_defaults(osd);
  // Stock: createRectangle header bg + createText title.
  int32_t rid = 0;
  if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
    rid = rpak_make_id(fe->pack_id, 0x0028);  // RRT_HEADERBG
  if (rid) {
    InvObject* pic = gameref_new();
    java_util_resource_ResourceRef_set(pic, rid);
    java_util_resource_ResourceRef_load(pic);
    render_d3d9_osd_add_rect(0.f, -0.95f, 2.f, 0.1f, pic, -1);
  }
  return osd_create_text(osd, title && title[0] ? title : "MENU",
                         frontend_large_font(), /*ALIGN_LEFT*/ 2, -0.95f,
                         -0.98f);
}

InvObject* osd_create_menu(InvObject* osd, InvObject* style, float x, float y,
                           float spc, int32_t ori) {
  if (!osd) return nullptr;
  osd_ensure_defaults(osd);
  InvObject* menu = tree_host_new("java.render.osd.Menu");
  tree_field_set_obj(menu, "osd", osd);
  tree_field_set_obj(menu, "sty", style);
  tree_field_set_obj(menu, "items", tree_vector_new());
  tree_field_set_int(menu, "item_count", 0);
  tree_field_set_float(menu, "x", x);
  tree_field_set_float(menu, "y", y);
  float spacing = spc;
  if (spacing == 0.f && style) {
    spacing = (ori == 1) ? tree_field_get_float(style, "width")
                         : tree_field_get_float(style, "height");
    if (spacing <= 0.f) spacing = 0.12f;
  }
  tree_field_set_float(menu, "spacing", spacing);
  tree_field_set_int(menu, "orientation", ori >= 0 ? ori : 0);
  return menu;
}

InvObject* osd_create_button(InvObject* osd, InvObject* style, float x, float y,
                             const char* label, int32_t cmd) {
  if (!osd) return nullptr;
  osd_ensure_defaults(osd);
  InvObject* btn = tree_host_new("java.render.osd.Button");
  tree_field_set_obj(btn, "osd", osd);
  tree_field_set_int(btn, "command", cmd);
  tree_field_set_float(btn, "x", x);
  tree_field_set_float(btn, "y", y);
  if (label) tree_field_set_obj(btn, "label_string", string_new(label));
  if (!tree_field_get_obj(osd, "buttons"))
    tree_field_set_obj(osd, "buttons", tree_vector_new());
  tree_vector_add(tree_field_get_obj(osd, "buttons"), btn);
  tree_field_set_int(osd, "button_count",
                     tree_vector_size(tree_field_get_obj(osd, "buttons")));

  int32_t align = 1;
  InvObject* font = frontend_medium_font();
  float w = 0.12f, h = 0.12f;
  if (style) {
    align = tree_field_get_int(style, "align");
    if (InvObject* cs = tree_field_get_obj(style, "charset")) font = cs;
    const float sw = tree_field_get_float(style, "width");
    const float sh = tree_field_get_float(style, "height");
    if (sw > 0.f) w = sw;
    if (sh > 0.f) h = sh;
    // Icon buttons: Style.background / .rt is the pictogram ResourceRef.
    InvObject* gfx = tree_field_get_obj(style, "background");
    if (!gfx) gfx = tree_field_get_obj(style, "rt");
    if (gfx) {
      tree_field_set_obj(btn, "gfx", gfx);
      if (java_util_resource_ResourceRef_id(gfx) != 0)
        java_util_resource_ResourceRef_load(gfx);
      render_d3d9_osd_add_rect(x, y, w, h, gfx, 0);
    }
  }
  // Text.ALIGN_LEFT=2, CENTER=1, RIGHT=0 (stock Text.java).
  if (align != 0 && align != 1 && align != 2) align = 1;
  if (label && label[0]) osd_blit_label(osd, label, font, x, y, align);
  return btn;
}

void menu_add_separator(InvObject* menu) {
  if (!menu) return;
  InvObject* osd = tree_field_get_obj(menu, "osd");
  float spacing = tree_field_get_float(menu, "spacing");
  if (spacing <= 0.f) spacing = 0.12f;
  const float vpH =
      osd && tree_field_get_float(osd, "vpHeight") > 0.f
          ? tree_field_get_float(osd, "vpHeight")
          : 1.f;
  const int32_t ori = tree_field_get_int(menu, "orientation");
  if (ori == 0) {
    tree_field_set_float(menu, "y",
                         tree_field_get_float(menu, "y") + spacing * 0.5f / vpH);
  } else {
    const int32_t align =
        tree_field_get_obj(menu, "sty")
            ? tree_field_get_int(tree_field_get_obj(menu, "sty"), "align")
            : 1;
    float x = tree_field_get_float(menu, "x");
    const float dx = spacing * 0.5f / vpH;
    tree_field_set_float(menu, "x", align == 0 ? x - dx : x + dx);
  }
}

InvObject* menu_add_item(InvObject* menu, const char* text, int32_t cmd) {
  if (!menu) return nullptr;
  InvObject* osd = tree_field_get_obj(menu, "osd");
  InvObject* sty = tree_field_get_obj(menu, "sty");
  const float x = tree_field_get_float(menu, "x");
  const float y = tree_field_get_float(menu, "y");
  InvObject* btn = osd_create_button(osd, sty, x, y, text, cmd);
  if (!tree_field_get_obj(menu, "items"))
    tree_field_set_obj(menu, "items", tree_vector_new());
  tree_vector_add(tree_field_get_obj(menu, "items"), btn);
  tree_field_set_int(menu, "item_count",
                     tree_vector_size(tree_field_get_obj(menu, "items")));
  // Stock Menu.nextLine = two separators.
  menu_add_separator(menu);
  menu_add_separator(menu);
  return btn;
}

InvObject* menu_add_item_gfx(InvObject* menu, InvObject* gfx, int32_t cmd,
                             const char* tooltip) {
  if (!menu) return nullptr;
  InvObject* osd = tree_field_get_obj(menu, "osd");
  InvObject* base = tree_field_get_obj(menu, "sty");
  InvObject* style = tree_host_new("java.render.osd.Style");
  if (base) {
    tree_field_set_float(style, "width", tree_field_get_float(base, "width"));
    tree_field_set_float(style, "height", tree_field_get_float(base, "height"));
    tree_field_set_float(style, "aspect", tree_field_get_float(base, "aspect"));
    tree_field_set_int(style, "align", tree_field_get_int(base, "align"));
    if (InvObject* cs = tree_field_get_obj(base, "charset"))
      tree_field_set_obj(style, "charset", cs);
  } else {
    tree_field_set_float(style, "width", 0.12f);
    tree_field_set_float(style, "height", 0.12f);
    tree_field_set_int(style, "align", 2);  // ALIGN_LEFT
    tree_field_set_obj(style, "charset", frontend_medium_font());
  }
  if (gfx) {
    tree_field_set_obj(style, "background", gfx);
    tree_field_set_obj(style, "rt", gfx);
  }
  const float x = tree_field_get_float(menu, "x");
  const float y = tree_field_get_float(menu, "y");
  InvObject* btn =
      osd_create_button(osd, style, x, y, tooltip ? tooltip : "", cmd);
  if (!tree_field_get_obj(menu, "items"))
    tree_field_set_obj(menu, "items", tree_vector_new());
  tree_vector_add(tree_field_get_obj(menu, "items"), btn);
  tree_field_set_int(menu, "item_count",
                     tree_vector_size(tree_field_get_obj(menu, "items")));
  menu_add_separator(menu);
  menu_add_separator(menu);
  return btn;
}

void garage_create_osd_street_menu(InvObject* garage) {
  // Stock Garage.createOSDObjects (!roc) icon strip — VA-backed layout from
  // Java; used when TREE mis-packs ResourceRef addItem as addSeparator.
  if (!garage) return;
  InvObject* osd = tree_field_get_obj(garage, "osd");
  if (!osd) {
    osd = tree_host_new("java.render.Osd");
    osd_ensure_defaults(osd);
    tree_field_set_obj(garage, "osd", osd);
  }
  osd_ensure_defaults(osd);
  tree_field_set_obj(osd, "globalHandler", garage);
  tree_field_set_int(osd, "visible", 1);

  InvObject* style = tree_host_new("java.render.osd.Style");
  tree_field_set_float(style, "width", 0.12f);
  tree_field_set_float(style, "height", 0.12f);
  tree_field_set_int(style, "align", 2);
  tree_field_set_obj(style, "charset", frontend_medium_font());

  InvObject* menu = osd_create_menu(osd, style, -0.98f, -0.84f, 0.f,
                                    /*MD_HORIZONTAL*/ 1);
  tree_field_set_obj(osd, "last_menu", menu);

  auto fe_icon = [](uint16_t local) -> InvObject* {
    InvObject* pic = gameref_new();
    int32_t rid = 0;
    if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
      rid = rpak_make_id(fe->pack_id, local);
    else if (const RpakPack* fe = rpak_find_by_name("frontend"))
      rid = rpak_make_id(fe->pack_id, local);
    if (rid) {
      java_util_resource_ResourceRef_set(pic, rid);
      java_util_resource_ResourceRef_load(pic);
    }
    return pic;
  };

  struct Item {
    uint16_t local;
    int32_t cmd;
    const char* tip;
  };
  static const Item kItems[] = {
      {0x011D, 109, "Go driving in the city"},
      {0x011E, 110, "Test Track"},
      {0x011F, 108, "Go to the Race Of Champions"},
      {0x0120, 111, "Go to the Car Lot"},
      {0x0121, 112, "Buy new cars"},
      {0x0122, 122, "Buy used cars or sell your car"},
      {0x0125, 113, "Browse the Catalog"},
      {0x0123, 114, "Check your ranking here"},
      {0x0124, 115, "Details of this car"},
      {0x0129, 117, "Install/Remove parts"},
      {0x0128, 124, "Fine tune specific parts"},
      {0x012C, 118, "Body paint"},
      {0x0127, 123, "Test engine and steering"},
      {0x012A, 116, "Advance time (1 hour)"},
  };
  for (const Item& it : kItems)
    menu_add_item_gfx(menu, fe_icon(it.local), it.cmd, it.tip);

  // RID_BACK → main menu (frontend local often 0x00B3 / Osd.RID_BACK).
  InvObject* back = fe_icon(0x00B3);
  if (java_util_resource_ResourceRef_id(back) == 0) {
    // Fallback: still count as a button for chrome probes.
    back = gameref_new();
  }
  menu_add_item_gfx(menu, back, /*CMD_MAINMENU*/ 101, "Go back to Main Menu");

  osd_create_text(osd, "Welcome!", frontend_medium_font(), /*ALIGN_RIGHT*/ 0,
                  0.97f, 0.54f);
  tree_field_set_int(garage, "osd_via_host", 1);
  tree_field_set_int(osd, "visible", 1);
}

int32_t osd_begin_group(InvObject* osd) {
  if (!osd) return -1;
  osd_ensure_defaults(osd);
  InvObject* g = tree_host_new("java.render.osd.Group");
  tree_field_set_obj(g, "osd", osd);
  tree_field_set_obj(g, "hotkey", tree_vector_new());
  tree_field_set_obj(g, "gadget", tree_vector_new());
  tree_field_set_int(g, "active", 1);
  InvObject* groups = tree_field_get_obj(osd, "groups");
  tree_vector_add(groups, g);
  return tree_vector_size(groups) - 1;
}

int32_t osd_end_group(InvObject* osd) {
  if (!osd) return -1;
  InvObject* groups = tree_field_get_obj(osd, "groups");
  const int32_t n = groups ? tree_vector_size(groups) : 0;
  return n > 0 ? n - 1 : -1;
}

void osd_hide_group(InvObject* osd, int32_t gid) {
  if (!osd || gid < 0) return;
  InvObject* groups = tree_field_get_obj(osd, "groups");
  if (!groups || gid >= tree_vector_size(groups)) return;
  if (InvObject* g = tree_vector_element_at(groups, gid))
    tree_field_set_int(g, "active", 0);
}

void osd_show_group(InvObject* osd, int32_t gid) {
  if (!osd || gid < 0) return;
  InvObject* groups = tree_field_get_obj(osd, "groups");
  if (!groups || gid >= tree_vector_size(groups)) return;
  if (InvObject* g = tree_vector_element_at(groups, gid))
    tree_field_set_int(g, "active", 1);
}

void options_dialog_ensure_groups(InvObject* dialog) {
  if (!dialog) return;
  // Groups are allocated during addCustomGroups / show TREE (begin/endGroup).
  if (!tree_field_get_obj(dialog, "osd")) {
    InvObject* osd = tree_host_new("java.render.Osd");
    osd_ensure_defaults(osd);
    tree_field_set_obj(dialog, "osd", osd);
  }
}

void options_dialog_change_mode(InvObject* dialog, int32_t group) {
  // Stock OptionsDialog.changeMode: hide actGroup, show group, actGroup=group.
  if (!dialog) return;
  options_dialog_ensure_groups(dialog);
  InvObject* osd = tree_field_get_obj(dialog, "osd");
  const int32_t act = tree_field_get_int(dialog, "actGroup");
  if (act != group) {
    if (act >= 0) osd_hide_group(osd, act);
    tree_field_set_int(dialog, "actGroup", group);
    if (group >= 0) osd_show_group(osd, group);
  }
  tree_field_set_int(dialog, "change_mode_via_host", 1);
}

void mainmenu_credits_reset(InvObject* dialog) {
  if (!dialog) return;
  tree_field_set_int(dialog, "lastInLine", 0);
  tree_field_set_int(dialog, "lastOutLine", 0);
  tree_field_set_float(dialog, "creditsSpacing", 0.f);
  tree_field_set_float(dialog, "creditsTime",
                       static_cast<float>(java_lang_System_currentTime()));
  tree_field_set_int(dialog, "credits_reset_via_host", 1);
}

void mainmenu_build_credits(InvObject* dialog) {
  if (!dialog) return;
  // Stock buildCredits fills creditsTxt[]; TREE addCustomGroups often did it.
  if (!tree_field_get_obj(dialog, "creditsTxt"))
    tree_field_set_obj(dialog, "creditsTxt", tree_vector_new());
  tree_field_set_int(dialog, "credits_built", 1);
}

void mainmenu_credits_tick(InvObject* dialog, float dt) {
  (void)dialog;
  (void)dt;
  // Full CREDITS_* scroll lives in TREE animate; tick is a no-op host hook.
}

void mainmenu_dialog_ensure_chrome(InvObject* dialog) {
  // Stock MainMenuDialog.show: MUSIC_SET_MENU + openVideo(prime.avi,1,1) +
  // addCustomGroups (GENERALBG / MAIN MENU / columns). TREE show overflows;
  // this is the boot mirror until Osd.create* packing is solid.
  if (!dialog) return;
  if (tree_field_get_int(dialog, "menu_chrome") == 1) return;

  java_sound_Sound_changeMusicSet(3);  // Sound.MUSIC_SET_MENU
  const int32_t fmv_ok =
      video_fmv_open("data\\fmv\\prime.avi", /*non_exclusive=*/1, /*loop=*/1);
  tree_field_set_int(dialog, "bgVideoActive", fmv_ok == 0 ? 1 : 0);

  InvObject* osd = tree_field_get_obj(dialog, "osd");
  if (!osd) {
    osd = tree_host_new("java.render.Osd");
    tree_field_set_obj(dialog, "osd", osd);
  }
  osd_ensure_defaults(osd);
  tree_field_set_int(osd, "visible", 1);

  if (fmv_ok != 0) {
    int32_t rid = 0;
    if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
      rid = rpak_make_id(fe->pack_id, 0x0016);  // RID_GENERALBG
    else if (const RpakPack* fe = rpak_find_by_name("frontend"))
      rid = rpak_make_id(fe->pack_id, 0x0016);
    if (rid) {
      InvObject* pic = gameref_new();
      java_util_resource_ResourceRef_set(pic, rid);
      java_util_resource_ResourceRef_load(pic);
      tree_field_set_obj(osd, "bg", pic);
      render_d3d9_osd_add_rect(0.f, 0.f, 2.f, 2.f, pic, -2);
    }
  }

  void* font = frontend_medium_font();
  if (font && !render_d3d9_font_ready(font)) {
    const int32_t rid = java_util_resource_ResourceRef_id(
        reinterpret_cast<InvObject*>(font));
    if (!render_d3d9_font_load_from_rid(font, rid))
      render_d3d9_font_load(font, "simple20");
  }
  auto add_label = [&](const char* text, float x, float y, int32_t idx) {
    void* key = reinterpret_cast<void*>(
        static_cast<uintptr_t>(0x6D4E0000u + static_cast<uint32_t>(idx)));
    render_d3d9_text_create(key, font, x, y);
    render_d3d9_text_set_string(key, text);
    render_d3d9_text_set_align(key, 1);
    render_d3d9_text_update(key);
  };
  // Header + two columns from MainMenu.addCustomGroups (stock x=±0.05, y=-0.6).
  add_label("MAIN MENU", 0.f, -0.95f, 0);
  static const char* kLeft[] = {"NEW CAREER",     "LOAD CAREER",
                                "DELETE CAREER",  "BACK TO GARAGE",
                                "LOAD CAR",       "SAVE CAR"};
  static const char* kRight[] = {"QUICKRACE", "FREE RIDE", "DEMO",
                                 "OPTIONS",   "CREDITS",   "EXIT GAME"};
  for (int i = 0; i < 6; ++i) {
    const float y = -0.6f + static_cast<float>(i) * 0.12f;
    add_label(kLeft[i], -0.05f, y, 1 + i);
    add_label(kRight[i], 0.05f, y, 7 + i);
  }

  // FMV path has no GENERALBG rect — keep ≥1 OSD entry for smoke/boot checks.
  if (render_d3d9_osd_count() < 1) {
    render_d3d9_osd_add_rect(0.f, 0.f, 2.f, 2.f, nullptr, -2);
  }

  tree_field_set_int(dialog, "menu_chrome", 1);
  tree_field_set_int(dialog, "shown", 1);
  tree_field_set_int(osd, "button_count", 12);
  std::printf("[script] MainMenuDialog chrome fmv=%d osd=%d txt=%d\n",
              fmv_ok == 0 ? 1 : 0, render_d3d9_osd_count(),
              render_d3d9_osd_text_count());
}

void frontend_destroy() {
  if (g_hotkey_thread) {
    java_lang_Thread_stop(g_hotkey_thread);
    g_hotkey_thread = nullptr;
    g_hotkey_watcher = nullptr;
  }
  g_frontend_inited = 0;
}

void frontend_hotkey_watcher_run(InvObject* self) {
  InvObject* th =
      self ? tree_field_get_obj(self, "engine_thread") : g_hotkey_thread;
  if (!th) th = g_hotkey_thread;
  while (th && java_lang_Thread_isAlive(th)) {
    java_lang_Thread_sleep(50.f);
    InvObject* q = frontend_input_queue();
    const int32_t n = tree_vector_size(q);
    if (n <= 0) continue;
    InvObject* focused = tree_vector_element_at(q, n - 1);
    if (!focused) continue;
    static InvObject* g_cursor = nullptr;
    if (!g_cursor) g_cursor = java_io_Input_cursor();
    InvObject* ctrl = tree_field_get_obj(g_cursor, "controller");
    if (!ctrl) continue;
    java_io_Input_checkHotkeys(ctrl, focused);
    java_lang_GameType_pollTimers();
    tree_field_set_int(self ? self : g_hotkey_watcher, "ticks",
                       tree_field_get_int(self ? self : g_hotkey_watcher,
                                          "ticks") +
                           1);
  }
}