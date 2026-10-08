// Fragment 4/6 of GameRef.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


  // Player blip — host map shim only (PE uses 3D tiles, no OSD blip).
  if (!pe_like_3d) {
    InvObject* tex = tree_field_get_obj(nav, "osd_tex");
    render_d3d9_osd_set_rect(blip_key, map_x, map_y, map_w * 0.08f,
                             map_h * 0.08f, tex, 42);
  }

  InvObject* cam = tree_field_get_obj(nav, "cam");
  if (cam) {
    // Host lookat stand-in for PE cam bone write (SetLocalMatrix @
    // 0x483096) — PE never calls lookat. offsetX/Z are world-cm *deltas*
    // (PE @ 0x482DF2–0x482E39: world = car + prior), not absolute eye.
    // Absolute ox/oz→eye broke once Cars.cpp stored PE deltas.
    // Mode0 Java xz = world * flt_5F0C20 (0.01) @ loc_483067 (PE matrix
    // *0.1 / setMatrix *10). Mode1 @ 0x482FD2: eye = world*0.01 +
    // Ypr_forward*DEF_ZOOM → at = world*0.01.
    // Dynamarker yaw: PE @ 0x48314B Engine_queryGameRefChannel(GII_DIR=4)
    // → int° * flt_5F13BC. Host getInfo(GII_DIR) now returns int° (W8A);
    // Cars seed prefers that path when non-zero.
    constexpr float kScale01 = 0.01f;  // flt_5F0C20 @ 0x005F0C20
    constexpr float kDefZoom = 4.5f;   // flt_5F13C0 @ 0x005F13C0
    const float ox = tree_field_get_float(nav, "offsetX");
    const float oz = tree_field_get_float(nav, "offsetZ");
    const float at_x = (fx + ox) * kScale01;
    const float at_z = (fz + oz) * kScale01;

    float ex = at_x, ey = 0.f, ez = at_z;
    if (InvObject* bp = java_util_resource_RenderRef_getPos(cam)) {
      float bx = 0.f, by = 0.f, bz = 0.f;
      vec3_get(bp, &bx, &by, &bz);
      // Prefer bone from updateNavigator setMatrix_1 (mode0≈at, mode1
      // orbit). show() before first update: getPos may be 0 → keep at+zoom.
      if (bx != 0.f || by != 0.f || bz != 0.f) {
        ex = bx;
        ey = by;
        ez = bz;
      }
    }
    if (ey <= 0.f) {
      ey = tree_field_get_float(nav, "zoom");
      if (ey <= 0.f) ey = kDefZoom;
    }
    render_d3d9_camera_lookat(cam, ex, ey, ez, at_x, 0.f, at_z);
  }
}

InvObject* inventory_new(InvObject* player) {
  InvObject* inv = tree_host_new("java.game.Inventory");
  tree_field_set_obj(inv, "player", player);
  tree_field_set_obj(inv, "items", tree_vector_new());
  tree_field_set_int(inv, "size", 0);
  tree_field_set_int(inv, "update_count", 0);
  return inv;
}

namespace {

InvObject* inventory_panel_item_part(InvObject* item) {
  if (!item) return nullptr;
  InvObject* p = tree_field_get_obj(item, "partXXX");
  if (!p) p = tree_field_get_obj(item, "part");
  return p;
}

InvObject* inventory_item_ensure_localroot(InvObject* item) {
  if (!item) return nullptr;
  InvObject* lr = tree_field_get_obj(item, "localroot");
  if (lr) return lr;
  // Stock InventoryItem: new Dummy(inventory, WORLDTREEROOT).
  lr = tree_host_new("java.util.resource.Dummy");
  tree_field_set_obj(item, "localroot", lr);
  InvObject* inv = tree_field_get_obj(item, "inventory");
  if (inv) java_util_resource_GameRef_setParent(lr, inv);
  return lr;
}

void inventory_panel_cleanup(InvObject* panel) {
  if (!panel) return;
  InvObject* light = tree_field_get_obj(panel, "light");
  if (light) {
    tree_field_set_int(light, "destroyed", 1);
    tree_field_set_obj(panel, "light", nullptr);
  }
  InvObject* cam = tree_field_get_obj(panel, "cam");
  if (cam) {
    InvObject* osd = tree_field_get_obj(panel, "osd");
    InvObject* vp = osd ? tree_field_get_obj(osd, "vp") : nullptr;
    if (vp) render_d3d9_camera_deactivate(cam, vp);
    java_render_Camera_destroy(cam);
    tree_field_set_obj(panel, "cam", nullptr);
  }
  tree_field_set_int(panel, "preview_ready", 0);
}

void inventory_panel_create_def_camera(InvObject* panel, float size,
                                      int32_t flags) {
  if (!panel) return;
  if (size <= 0.f) size = 1.f;
  tree_field_set_float(panel, "size", size);
  tree_field_set_int(panel, "flags", flags);

  InvObject* item = tree_field_get_obj(panel, "invItem");
  InvObject* localroot =
      item ? inventory_item_ensure_localroot(item) : nullptr;
  InvObject* osd = tree_field_get_obj(panel, "osd");
  InvObject* vp = osd ? tree_field_get_obj(osd, "vp") : nullptr;
  if (!localroot || !vp) return;

  if (InvObject* old = tree_field_get_obj(panel, "cam")) {
    render_d3d9_camera_deactivate(old, vp);
    java_render_Camera_destroy(old);
    tree_field_set_obj(panel, "cam", nullptr);
  }

  // Stock Camera(..., aov=110) → native half-AOV 55°.
  InvObject* cam = tree_host_new("java.render.Camera");
  java_render_Camera_create(cam, localroot, vp, 1, 55.f, 0.001f, 10.f, 1.f,
                            1.f, 1, 0);

  float yaw = -2.5f, pitch = -0.7f, roll = 0.f;
  InvObject* ypr_obj = tree_field_get_obj(panel, "ypr");
  if (InvObject* part = inventory_panel_item_part(item)) {
    if (InvObject* cy = tree_field_get_obj(part, "catalog_view_ypr")) {
      ypr_obj = cy;
      tree_field_set_obj(panel, "ypr", cy);
    }
  }
  if (!ypr_obj) {
    ypr_obj = ypr_new(yaw, pitch, roll);
    tree_field_set_obj(panel, "ypr", ypr_obj);
  }
  ypr_get(ypr_obj, &yaw, &pitch, &roll);

  // Stock: camPos=(0,size,size).rotate(Ypr(ypr.y, 0, 0)) — orbit by yaw.
  const float c = std::cos(yaw);
  const float s = std::sin(yaw);
  const float ex = size * s;
  const float ey = size;
  const float ez = size * c;

  java_util_resource_GameRef_setMatrix(cam, vec3_new(ex, ey, ez), ypr_obj);
  // Host view path needs lookat (identity view when lookat=false).
  render_d3d9_camera_lookat(cam, ex, ey, ez, 0.f, 0.f, 0.f);
  tree_field_set_obj(panel, "cam", cam);
  tree_field_set_int(panel, "preview_ready", 1);
}

void inventory_panel_apply_cam_pose(InvObject* panel) {
  if (!panel) return;
  InvObject* cam = tree_field_get_obj(panel, "cam");
  InvObject* ypr_obj = tree_field_get_obj(panel, "ypr");
  if (!cam || !ypr_obj) return;
  float size = tree_field_get_float(panel, "size");
  if (size <= 0.f) size = 1.f;
  float yaw = 0.f, pitch = 0.f, roll = 0.f;
  ypr_get(ypr_obj, &yaw, &pitch, &roll);
  const float c = std::cos(yaw);
  const float s = std::sin(yaw);
  const float ex = size * s;
  const float ey = size;
  const float ez = size * c;
  java_util_resource_GameRef_setMatrix(cam, vec3_new(ex, ey, ez), ypr_obj);
  render_d3d9_camera_lookat(cam, ex, ey, ez, 0.f, 0.f, 0.f);
}

bool inventory_panel_focus_hook(InvObject* panel) {
  if (!panel) return false;
  if (!tree_field_get_int(panel, "flags")) return false;
  if (!tree_field_get_obj(panel, "cam")) return false;
  InvObject* ypr_obj = tree_field_get_obj(panel, "ypr");
  if (!ypr_obj) {
    ypr_obj = ypr_new(-2.5f, -0.7f, 0.f);
    tree_field_set_obj(panel, "ypr", ypr_obj);
  }
  float yaw = 0.f, pitch = 0.f, roll = 0.f;
  ypr_get(ypr_obj, &yaw, &pitch, &roll);
  // Stock InventoryPanel.focusHook: ypr.y += 0.03.
  yaw += 0.03f;
  ypr_set(ypr_obj, yaw, pitch, roll);
  inventory_panel_apply_cam_pose(panel);
  tree_field_set_int(panel, "focus_ticks",
                     tree_field_get_int(panel, "focus_ticks") + 1);
  return true;
}

void inventory_panel_create_def_light(InvObject* panel) {
  if (!panel) return;
  InvObject* item = tree_field_get_obj(panel, "invItem");
  InvObject* localroot =
      item ? inventory_item_ensure_localroot(item) : nullptr;
  if (!localroot) return;

  if (InvObject* old = tree_field_get_obj(panel, "light")) {
    tree_field_set_int(old, "destroyed", 1);
    tree_field_set_obj(panel, "light", nullptr);
  }

  // InventoryPanel.RID_INVENTORY_LIGHT = misc.garage:0x00000024.
  if (!rpak_find_by_name("garage.rpk"))
    java_lang_System_openLib(string_new("garage.rpk"));
  int32_t rid = 0x24;
  if (const RpakPack* gp = rpak_find_by_name("garage.rpk"))
    rid = rpak_make_id(gp->pack_id, 0x0024);
  InvObject* type = resref_new();
  java_util_resource_ResourceRef_set(type, rid);
  InvObject* light = tree_host_new("java.util.resource.RenderRef");
  java_util_resource_RenderRef_create(light, localroot, type, nullptr);
  tree_field_set_obj(panel, "light", light);
  if (tree_field_get_obj(panel, "cam"))
    tree_field_set_int(panel, "preview_ready", 1);
}

void inventory_panel_show_item(InvObject* panel, InvObject* item) {
  if (!panel || !item) return;
  InvObject* part = inventory_panel_item_part(item);
  if (part) java_util_resource_GameRef_setMatrix(part, nullptr, nullptr);
  inventory_item_ensure_localroot(item);
  float size_m = 1.f;
  if (part) {
    const int32_t cm =
        java_util_resource_GameRef_getInfo(part, /*GII_SIZE*/ 11, 0);
    if (cm > 0) size_m = static_cast<float>(cm) / 100.f;
  }
  inventory_panel_create_def_camera(panel, size_m, /*enableRotate*/ 1);
  inventory_panel_create_def_light(panel);
}

InvObject* inventory_panel_new(InvObject* inv, int32_t index, float x, float y,
                               float w, float h) {
  InvObject* panel = tree_host_new("java.game.InventoryPanel");
  tree_field_set_obj(panel, "inv", inv);
  tree_field_set_int(panel, "index", index);
  tree_field_set_float(panel, "x", x);
  tree_field_set_float(panel, "y", y);
  tree_field_set_float(panel, "width", w);
  tree_field_set_float(panel, "height", h);
  tree_field_set_int(panel, "visible", 0);
  tree_field_set_obj(panel, "ypr", ypr_new(-2.5f, -0.7f, 0.f));

  InvObject* osd = tree_host_new("java.render.Osd");
  tree_field_set_obj(osd, "groups", tree_vector_new());
  tree_field_set_obj(osd, "hotkey", tree_vector_new());
  tree_field_set_obj(osd, "rectangles", tree_vector_new());
  tree_field_set_obj(osd, "buttons", tree_vector_new());
  tree_field_set_int(osd, "init", 1);
  tree_field_set_int(osd, "iLevel", 3);  // IL_TIPS
  tree_field_set_obj(osd, "globalHandler", panel);
  tree_field_set_float(osd, "vpWidth", w > 0.f ? w : 1.f);
  tree_field_set_float(osd, "vpHeight", h > 0.f ? h : 1.f);
  tree_field_set_float(osd, "vpAspect", 1.f);
  tree_field_set_float(osd, "vpLeft", x);
  tree_field_set_float(osd, "vpTop", y);

  // Stock: new Osd(new Viewport(15, cwidth, cheight, itemW, itemH)).
  InvObject* vp = tree_host_new("java.render.Viewport");
  render_d3d9_viewport_create(vp, 15, x, y, w > 0.f ? w : 0.1f,
                              h > 0.f ? h : 0.1f);
  tree_field_set_obj(osd, "vp", vp);

  InvObject* style = tree_host_new("java.render.osd.Style");
  tree_field_set_float(style, "rWidth", w * 2.f);
  tree_field_set_float(style, "rHeight", h * 2.f);
  tree_field_set_float(style, "width", w * 2.f);
  tree_field_set_float(style, "height", h * 2.f);

  InvObject* btn = tree_host_new("java.render.osd.Button");
  tree_field_set_obj(btn, "style", style);
  tree_field_set_float(btn, "x", 0.f);
  tree_field_set_float(btn, "y", 0.f);
  tree_field_set_int(btn, "command", index);
  tree_field_set_int(btn, "drop_enabled", 1);
  tree_field_set_obj(btn, "osd", osd);
  tree_vector_add(tree_field_get_obj(osd, "buttons"), btn);
  tree_field_set_int(osd, "button_count", 1);

  tree_field_set_obj(panel, "osd", osd);
  tree_field_set_obj(panel, "button", btn);
  return panel;
}

void inventory_panel_attach(InvObject* panel, InvObject* item) {
  if (!panel) return;
  InvObject* prev = tree_field_get_obj(panel, "invItem");
  if (prev == item) return;
  if (prev) inventory_panel_cleanup(panel);
  tree_field_set_obj(panel, "invItem", item);
  InvObject* btn = tree_field_get_obj(panel, "button");
  if (btn) {
    if (item) {
      InvObject* tip = tree_field_get_obj(item, "info");
      if (!tip) tip = string_new("part");
      tree_field_set_obj(btn, "tooltip", tip);
    } else {
      tree_field_set_obj(btn, "tooltip", nullptr);
    }
  }
  if (item) inventory_panel_show_item(panel, item);
  tree_field_set_int(panel, "attach_count",
                     tree_field_get_int(panel, "attach_count") + 1);
}

}  // namespace

void visual_inventory_init(InvObject* inv, float left, float top, float width,
                           float height) {
  if (!inv) return;
  constexpr int32_t kLines = 1;
  constexpr int32_t kPerLine = 5;
  tree_field_set_int(inv, "linesPerPage", kLines);
  tree_field_set_int(inv, "partsPerLine", kPerLine);
  tree_field_set_int(inv, "cline", 0);
  tree_field_set_int(inv, "interactive", 1);
  tree_field_set_int(inv, "start", 0);
  tree_field_set_int(inv, "stop", kLines * kPerLine);
  tree_field_set_int(inv, "visualsUpdated", 0);

  const float hSpacing = 0.013f;
  const float vSpacing = 0.01f;
  const float itemW =
      (width - (kPerLine - 1) * hSpacing) / static_cast<float>(kPerLine);
  const float itemH =
      (height - (kLines - 1) * vSpacing) / static_cast<float>(kLines);

  const int32_t n = kPerLine * kLines;
  InvObject* panels = tree_array_new(n);
  float cheight = top;
  for (int32_t i = 0; i < kLines; ++i) {
    float cwidth = left;
    for (int32_t j = 0; j < kPerLine; ++j) {
      const int32_t index = i * kPerLine + j;
      InvObject* panel =
          inventory_panel_new(inv, index, cwidth, cheight, itemW, itemH);
      tree_vector_set(panels, index, panel);
      cwidth += itemW + hSpacing;
    }
    cheight += itemH + vSpacing;
  }
  tree_field_set_obj(inv, "panels", panels);
}

InvObject* visual_inventory_new(InvObject* player, float left, float top,
                                float width, float height) {
  InvObject* inv = tree_host_new("java.game.VisualInventory");
  tree_field_set_obj(inv, "player", player);
  tree_field_set_obj(inv, "items", tree_vector_new());
  tree_field_set_int(inv, "size", 0);
  tree_field_set_int(inv, "update_count", 0);
  visual_inventory_init(inv, left, top, width, height);
  return inv;
}

int32_t visual_inventory_panel_count(InvObject* inv) {
  if (!inv) return 0;
  InvObject* panels = tree_field_get_obj(inv, "panels");
  return panels ? tree_vector_size(panels) : 0;
}

int32_t visual_inventory_attached_count(InvObject* inv) {
  if (!inv) return 0;
  InvObject* panels = tree_field_get_obj(inv, "panels");
  if (!panels) return 0;
  int32_t n = 0;
  const int32_t pc = tree_vector_size(panels);
  for (int32_t i = 0; i < pc; ++i) {
    InvObject* p = tree_vector_element_at(panels, i);
    if (p && tree_field_get_obj(p, "invItem")) ++n;
  }
  return n;
}

int32_t visual_inventory_preview_count(InvObject* inv) {
  if (!inv) return 0;
  InvObject* panels = tree_field_get_obj(inv, "panels");
  if (!panels) return 0;
  int32_t n = 0;
  const int32_t pc = tree_vector_size(panels);
  for (int32_t i = 0; i < pc; ++i) {
    InvObject* p = tree_vector_element_at(panels, i);
    if (!p || !tree_field_get_obj(p, "invItem")) continue;
    InvObject* cam = tree_field_get_obj(p, "cam");
    InvObject* light = tree_field_get_obj(p, "light");
    InvObject* osd = tree_field_get_obj(p, "osd");
    InvObject* vp = osd ? tree_field_get_obj(osd, "vp") : nullptr;
    const float aov = cam ? render_d3d9_camera_half_aov(cam) : 0.f;
    if (cam && light && vp && aov > 50.f &&
        tree_field_get_int(p, "preview_ready") == 1)
      ++n;
  }
  return n;
}

bool visual_inventory_focus_hook(InvObject* inv, int32_t panel_index) {
  if (!inv || panel_index < 0) return false;
  InvObject* panels = tree_field_get_obj(inv, "panels");
  if (!panels) return false;
  if (panel_index >= tree_vector_size(panels)) return false;
  return inventory_panel_focus_hook(tree_vector_element_at(panels, panel_index));
}

void mechanic_tick_preview(InvObject* mechanic) {
  if (!mechanic) return;
  InvObject* inv = tree_field_get_obj(mechanic, "inventory");
  if (!inv || !tree_field_get_int(inv, "shown")) return;
  InvObject* panel = mechanic_actual_panel(mechanic);
  if (!panel) return;
  visual_inventory_focus_hook(inv, tree_field_get_int(panel, "index"));
}

void visual_inventory_update(InvObject* inv) {
  if (!inv) return;
  InvObject* panels = tree_field_get_obj(inv, "panels");
  if (!panels) return;
  const int32_t parts_per_line = tree_field_get_int(inv, "partsPerLine");
  const int32_t lines = tree_field_get_int(inv, "linesPerPage");
  int32_t ppl = parts_per_line > 0 ? parts_per_line : 5;
  int32_t lpp = lines > 0 ? lines : 1;
  int32_t cline = tree_field_get_int(inv, "cline");
  const int32_t sz = inventory_size(inv);
  if (cline && cline * ppl >= sz) cline = cline > 0 ? cline - 1 : 0;
  tree_field_set_int(inv, "cline", cline);

  const int32_t begin = tree_field_get_int(inv, "start");
  const int32_t end = tree_field_get_int(inv, "stop");
  const int32_t start = cline * ppl;
  const int32_t stop = start + lpp * ppl;
  tree_field_set_int(inv, "start", start);
  tree_field_set_int(inv, "stop", stop);

  InvObject* items = tree_field_get_obj(inv, "items");
  const int32_t pc = tree_vector_size(panels);
  auto item_at = [&](int32_t i) -> InvObject* {
    if (!items || i < 0 || i >= inventory_size(inv)) return nullptr;
    return tree_vector_element_at(items, i);
  };

  if (begin == start && end == stop) {
    for (int32_t vis = 0; vis < pc; ++vis) {
      InvObject* panel = tree_vector_element_at(panels, vis);
      InvObject* item = item_at(start + vis);
      if (panel && tree_field_get_obj(panel, "invItem") != item)
        inventory_panel_attach(panel, nullptr);
    }
    for (int32_t vis = 0; vis < pc; ++vis) {
      InvObject* panel = tree_vector_element_at(panels, vis);
      InvObject* item = item_at(start + vis);
      if (panel && tree_field_get_obj(panel, "invItem") != item)
        inventory_panel_attach(panel, item);
    }
  } else {
    for (int32_t vis = 0; vis < pc; ++vis)
      inventory_panel_attach(tree_vector_element_at(panels, vis), nullptr);
    for (int32_t vis = 0; vis < pc; ++vis)
      inventory_panel_attach(tree_vector_element_at(panels, vis),
                             item_at(start + vis));
  }

  tree_field_set_int(inv, "visualsUpdated", 1);
  tree_field_set_int(inv, "size", sz);
  tree_field_set_int(inv, "update_count",
                     tree_field_get_int(inv, "update_count") + 1);
}

void visual_inventory_show(InvObject* inv) {
  if (!inv) return;
  InvObject* panels = tree_field_get_obj(inv, "panels");
  if (!panels) return;
  const int32_t pc = tree_vector_size(panels);
  for (int32_t i = 0; i < pc; ++i) {
    InvObject* panel = tree_vector_element_at(panels, i);
    if (!panel) continue;
    tree_field_set_int(panel, "visible", 1);
    if (InvObject* osd = tree_field_get_obj(panel, "osd"))
      tree_field_set_int(osd, "visible", 1);
  }
  tree_field_set_int(inv, "shown", 1);
}

void visual_inventory_hide(InvObject* inv) {
  if (!inv) return;
  InvObject* panels = tree_field_get_obj(inv, "panels");
  if (!panels) return;
  const int32_t pc = tree_vector_size(panels);
  for (int32_t i = 0; i < pc; ++i) {
    InvObject* panel = tree_vector_element_at(panels, i);
    if (!panel) continue;
    tree_field_set_int(panel, "visible", 0);
    if (InvObject* osd = tree_field_get_obj(panel, "osd"))
      tree_field_set_int(osd, "visible", 0);
  }
  tree_field_set_int(inv, "shown", 0);
}

void visual_inventory_scroll_up(InvObject* inv) {
  if (!inv) return;
  const int32_t cline = tree_field_get_int(inv, "cline");
  if (cline <= 0) return;
  tree_field_set_int(inv, "cline", cline - 1);
  visual_inventory_update(inv);
}

void visual_inventory_scroll_down(InvObject* inv) {
  if (!inv) return;
  const int32_t sz = inventory_size(inv);
  if (sz <= 1) return;
  int32_t ppl = tree_field_get_int(inv, "partsPerLine");
  if (ppl <= 0) ppl = 5;
  const int32_t cline = tree_field_get_int(inv, "cline");
  const int32_t max_line = (sz - 1) / ppl;
  if (cline >= max_line) return;
  tree_field_set_int(inv, "cline", cline + 1);
  visual_inventory_update(inv);
}

const char* inventory_install_to_car(InvObject* inv, int32_t index,
                                    InvObject* car) {
  if (!inv) return "no inventory";
  if (!car) return "no car";
  if (index < 0 || index >= inventory_size(inv)) return "bad index";
  InvObject* items = tree_field_get_obj(inv, "items");
  if (!items) return "empty";
  InvObject* item = tree_vector_element_at(items, index);
  if (!item) return "no item";
  InvObject* part = tree_field_get_obj(item, "partXXX");
  if (!part) part = tree_field_get_obj(item, "part");
  if (!part) return "no part";

  int32_t slot = 0;
  int32_t mate = 1;
  const int32_t pref = tree_field_get_int(part, "install_slot");
  if (pref > 0) {
    slot = pref;
  } else if (!part_find_cfg_install(car, part, &slot, &mate)) {
    return "no compatible slot";
  }
  if (part_on_slot(car, slot)) return "slot busy";
  part_disable_slot(car, slot, 0);
  if (!part_install(car, slot, part, mate)) return "install failed";
  if (part_on_slot(car, slot) != part) return "install verify failed";

  tree_field_set_obj(car, "last_installed_part", part);
  {
    // Sync GameRef pos from CFG slot (mesh cm) for pick/project.
    float sx = 0.f, sy = 0.f, sz = 0.f, oy = 0.f, op = 0.f, or_ = 0.f;
    if (part_slot_get_pose(car, slot, &sx, &sy, &sz, &oy, &op, &or_)) {
      constexpr float kM = 100.f;
      float cx = 0.f, cy = 0.f, cz = 0.f;
      if (InvObject* cp = java_util_resource_GameRef_getPos(car))
        vec3_get(cp, &cx, &cy, &cz);
      java_util_resource_GameRef_setMatrix(
          part, vec3_new(cx + sx * kM, cy + sy * kM, cz + sz * kM),
          ypr_new(oy, op, or_));
    }
  }

  tree_vector_remove(items, item);
  tree_field_set_obj(item, "partXXX", nullptr);
  tree_field_set_obj(item, "part", nullptr);
  tree_field_set_obj(part, "inventory_item", nullptr);
  tree_field_set_int(inv, "size", inventory_size(inv));
  tree_field_set_int(inv, "update_count",
                     tree_field_get_int(inv, "update_count") + 1);
  if (tree_field_get_obj(inv, "panels")) visual_inventory_update(inv);
  tree_field_set_obj(inv, "last_install_error", nullptr);
  tree_field_set_int(inv, "last_install_slot", slot);
  tree_field_set_int(inv, "last_install_via_attach",
                     tree_field_get_int(part, "install_via_attach"));
  tree_field_set_int(inv, "install_count",
                     tree_field_get_int(inv, "install_count") + 1);
  return nullptr;
}

bool visual_inventory_panel_left_click(InvObject* inv, int32_t panel_index) {
  if (!inv || panel_index < 0) return false;
  int32_t ppl = tree_field_get_int(inv, "partsPerLine");
  if (ppl <= 0) ppl = 5;
  const int32_t index =
      panel_index + tree_field_get_int(inv, "cline") * ppl;
  InvObject* player = tree_field_get_obj(inv, "player");
  InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
  if (!car) car = game_logic_player()
                      ? tree_field_get_obj(game_logic_player(), "car")
                      : nullptr;
  const char* err = inventory_install_to_car(inv, index, car);
  if (err && err[0]) {
    tree_field_set_obj(inv, "last_install_error", string_new(err));
    return false;
  }
  return true;
}

bool part_uninstall(InvObject* part) {
  if (!part) return false;
  const char* hc = tree_host_class(part);
  if (hc && std::strstr(hc, "Chassis")) return false;
  // Mirror GII_REMOVE_OK: refuse if dependents still attached.
  const int32_t n = part_slot_count(part);
  for (int32_t i = 0; i < n; ++i) {
    const int32_t sid = part_slot_id_at(part, i);
    if (sid > 0 && part_on_slot(part, sid)) return false;
  }
  InvObject* parent = tree_field_get_obj(part, "part_parent");
  const int32_t slot_id = tree_field_get_int(part, "part_parent_slot");
  if (parent && slot_id) {
    InvObject* slots = tree_field_get_obj(parent, "part_slots");
    const int32_t sn = slots ? tree_vector_size(slots) : 0;
    for (int32_t i = 0; i < sn; ++i) {
      InvObject* s = tree_vector_element_at(slots, i);
      if (!s || tree_field_get_int(s, "slot_id") != slot_id) continue;
      if (tree_field_get_obj(s, "child") == part) {
        tree_field_set_obj(s, "child", nullptr);
        tree_field_set_int(s, "child_slot_id", 0);
        if (tree_field_get_obj(s, "visual") == part)
          tree_field_set_obj(s, "visual", nullptr);
      }
      break;
    }
  }
  tree_field_set_obj(part, "part_parent", nullptr);
  tree_field_set_int(part, "part_parent_slot", 0);
  return true;
}

bool visual_inventory_panel_drag_drop(InvObject* inv, int32_t panel_index,
                                     InvObject* part) {
  if (!inv || !part || panel_index < 0) return false;
  const int32_t pc = visual_inventory_panel_count(inv);
  if (pc > 0 && panel_index >= pc) return false;
  if (!part_uninstall(part)) return false;

  inventory_add_part_item(inv, part);
  // Stock: swap newly added item into the drop panel slot when possible.
  int32_t ppl = tree_field_get_int(inv, "partsPerLine");
  if (ppl <= 0) ppl = 5;
  const int32_t start = tree_field_get_int(inv, "cline") * ppl;
  const int32_t target = start + panel_index;
  InvObject* items = tree_field_get_obj(inv, "items");
  const int32_t sz = inventory_size(inv);
  if (items && target >= 0 && target < sz - 1) {
    InvObject* a = tree_vector_element_at(items, target);
    InvObject* b = tree_vector_element_at(items, sz - 1);
    tree_vector_set(items, target, b);
    tree_vector_set(items, sz - 1, a);
  }
  if (tree_field_get_obj(inv, "panels")) visual_inventory_update(inv);
  tree_field_set_int(inv, "drop_count",
                     tree_field_get_int(inv, "drop_count") + 1);
  tree_field_set_int(inv, "last_panel_drop", panel_index);
  return true;
}

void inventory_swap(InvObject* inv, int32_t index_a, int32_t index_b) {
  if (!inv || index_a < 0 || index_b < 0) return;
  InvObject* items = tree_field_get_obj(inv, "items");
  if (!items) return;
  const int32_t sz = tree_vector_size(items);
  if (index_a >= sz || index_b >= sz || index_a == index_b) return;
  InvObject* a = tree_vector_element_at(items, index_a);
  InvObject* b = tree_vector_element_at(items, index_b);
  tree_vector_set(items, index_a, b);
  tree_vector_set(items, index_b, a);
  tree_field_set_int(inv, "size", inventory_size(inv));
  tree_field_set_int(inv, "update_count",
                     tree_field_get_int(inv, "update_count") + 1);
}

int32_t visual_inventory_item_index_by_button(InvObject* inv,
                                              InvObject* button) {
  if (!inv || !button) return inventory_size(inv);
  InvObject* panels = tree_field_get_obj(inv, "panels");
  if (!panels) return inventory_size(inv);
  int32_t ppl = tree_field_get_int(inv, "partsPerLine");
  if (ppl <= 0) ppl = 5;
  const int32_t start = tree_field_get_int(inv, "cline") * ppl;
  const int32_t pc = tree_vector_size(panels);
  for (int32_t i = 0; i < pc; ++i) {
    InvObject* p = tree_vector_element_at(panels, i);
    if (p && tree_field_get_obj(p, "button") == button) return start + i;
  }
  return inventory_size(inv);
}

bool visual_inventory_panel_swap(InvObject* inv, int32_t panel_index_a,
                                 InvObject* dropped_button) {
  if (!inv || panel_index_a < 0 || !dropped_button) return false;
  const int32_t pc = visual_inventory_panel_count(inv);
  if (pc > 0 && panel_index_a >= pc) return false;
  int32_t ppl = tree_field_get_int(inv, "partsPerLine");
  if (ppl <= 0) ppl = 5;
  const int32_t start = tree_field_get_int(inv, "cline") * ppl;
  const int32_t index_a = start + panel_index_a;
  const int32_t index_b =
      visual_inventory_item_index_by_button(inv, dropped_button);
  if (index_b >= inventory_size(inv)) return false;
  inventory_swap(inv, index_a, index_b);
  if (tree_field_get_obj(inv, "panels")) visual_inventory_update(inv);
  tree_field_set_int(inv, "swap_count",
                     tree_field_get_int(inv, "swap_count") + 1);
  tree_field_set_int(inv, "last_panel_swap", panel_index_a);
  tree_field_set_int(inv, "last_swap_b", index_b);
  return true;
}

bool visual_inventory_panel_swap_panels(InvObject* inv, int32_t panel_a,
                                        int32_t panel_b) {
  if (!inv || panel_a < 0 || panel_b < 0) return false;
  const int32_t pc = visual_inventory_panel_count(inv);
  if (pc > 0 && (panel_a >= pc || panel_b >= pc)) return false;
  int32_t ppl = tree_field_get_int(inv, "partsPerLine");
  if (ppl <= 0) ppl = 5;
  const int32_t start = tree_field_get_int(inv, "cline") * ppl;
  const int32_t index_a = start + panel_a;
  const int32_t index_b = start + panel_b;
  if (index_a >= inventory_size(inv) || index_b >= inventory_size(inv))
    return false;
  InvObject* items = tree_field_get_obj(inv, "items");
  InvObject* item_a = items ? tree_vector_element_at(items, index_a) : nullptr;
  InvObject* item_b = items ? tree_vector_element_at(items, index_b) : nullptr;
  inventory_swap(inv, index_a, index_b);
  if (tree_field_get_obj(inv, "panels")) visual_inventory_update(inv);
  tree_field_set_int(inv, "swap_count",
                     tree_field_get_int(inv, "swap_count") + 1);
  tree_field_set_int(inv, "last_panel_swap", panel_a);
  tree_field_set_int(inv, "last_swap_b", index_b);
  // Prove swap: panel attach should flip.
  InvObject* panels = tree_field_get_obj(inv, "panels");
  InvObject* pa = panels ? tree_vector_element_at(panels, panel_a) : nullptr;
  InvObject* pb = panels ? tree_vector_element_at(panels, panel_b) : nullptr;
  const bool ok =
      pa && pb && tree_field_get_obj(pa, "invItem") == item_b &&
      tree_field_get_obj(pb, "invItem") == item_a;
  tree_field_set_int(inv, "last_swap_ok", ok ? 1 : 0);
  return ok;
}

void mechanic_set_actual_panel(InvObject* mechanic, int32_t panel_index) {
  if (!mechanic) return;
  InvObject* inv = tree_field_get_obj(mechanic, "inventory");
  InvObject* panel = nullptr;
  if (inv && panel_index >= 0) {
    InvObject* panels = tree_field_get_obj(inv, "panels");
    if (panels && panel_index < tree_vector_size(panels))
      panel = tree_vector_element_at(panels, panel_index);
  }
  tree_field_set_int(mechanic, "actualPanel", panel_index);
  tree_field_set_obj(mechanic, "actualPanelObj", panel);
  tree_field_set_int(mechanic, "actualPanelChanged", 1);
  // Stock spinner updates infoline from invItem tip.
  InvObject* tip = nullptr;
  if (panel) {
    InvObject* item = tree_field_get_obj(panel, "invItem");
    if (item) {
      tip = tree_field_get_obj(item, "info");
      if (!tip) tip = string_new("part");
    } else {
      tip = string_new("empty slot");
    }
  }
  InvObject* line = tree_field_get_obj(mechanic, "infoline");
  if (!line) {
    line = tree_host_new("java.render.Text");
    tree_field_set_obj(mechanic, "infoline", line);
  }
  tree_field_set_obj(line, "text", tip);
}

void mechanic_clear_actual_panel(InvObject* mechanic) {
  if (!mechanic) return;
  if (!tree_field_get_obj(mechanic, "actualPanelObj") &&
      tree_field_get_int(mechanic, "actualPanel") < 0)
    return;
  tree_field_set_int(mechanic, "actualPanel", -1);
  tree_field_set_obj(mechanic, "actualPanelObj", nullptr);
  tree_field_set_int(mechanic, "actualPanelChanged", 1);
  InvObject* line = tree_field_get_obj(mechanic, "infoline");
  if (line) tree_field_set_obj(line, "text", nullptr);
}

InvObject* mechanic_actual_panel(InvObject* mechanic) {
  if (!mechanic) return nullptr;
  return tree_field_get_obj(mechanic, "actualPanelObj");
}

int32_t visual_inventory_panel_at(InvObject* inv, float nx, float ny) {
  // nx,ny in [0,1] top-left origin (same as panel x/y from VisualInventory).
  if (!inv) return -1;
  InvObject* panels = tree_field_get_obj(inv, "panels");
  if (!panels) return -1;
  const int32_t pc = tree_vector_size(panels);
  for (int32_t i = 0; i < pc; ++i) {
    InvObject* p = tree_vector_element_at(panels, i);
    if (!p || !tree_field_get_int(p, "visible")) continue;
    const float x = tree_field_get_float(p, "x");
    const float y = tree_field_get_float(p, "y");
    const float w = tree_field_get_float(p, "width");
    const float h = tree_field_get_float(p, "height");
    if (w <= 0.f || h <= 0.f) continue;
    if (nx >= x && nx <= x + w && ny >= y && ny <= y + h) return i;
  }
  return -1;
}

bool mechanic_hover_at(InvObject* mechanic, float nx, float ny) {
  if (!mechanic) return false;
  InvObject* inv = tree_field_get_obj(mechanic, "inventory");
  if (!inv || !tree_field_get_int(inv, "shown")) {
    mechanic_clear_actual_panel(mechanic);
    return false;
  }
  const int32_t idx = visual_inventory_panel_at(inv, nx, ny);
  if (idx >= 0) {
    mechanic_set_actual_panel(mechanic, idx);
    tree_field_set_int(mechanic, "last_hover_panel", idx);
    return true;
  }
  mechanic_clear_actual_panel(mechanic);
  tree_field_set_int(mechanic, "last_hover_panel", -1);
  return false;
}

void mechanic_tick_hover(InvObject* mechanic) {
  if (!mechanic) return;
  if (tree_field_get_int(mechanic, "rdrag")) return;
  // Mouse phys axes are [-1,1] (Y up). Panels use [0,1] top-left.
  input_live_poll();
  const float cx = java_io_Input_getAxis(1, /*kMousePhysX*/ 0);
  const float cy = java_io_Input_getAxis(1, /*kMousePhysY*/ 1);
  const float nx = (cx + 1.f) * 0.5f;
  const float ny = (1.f - cy) * 0.5f;
  mechanic_hover_at(mechanic, nx, ny);
  // Stock: inventory phyId hover OR car-part EC_HOVER (dest GameRef).
  if (!mechanic_actual_panel(mechanic)) mechanic_hover_car_at(mechanic, nx, ny);
}

bool inventory_panel_osd_command(InvObject* panel, int32_t cmd) {
  // Stock InventoryPanel.osdCommand: dropObject → drag, dropGadget → swap,
  // else panelLeftClick (install).
  if (!panel) return false;
  InvObject* inv = tree_field_get_obj(panel, "inv");
  if (!inv || tree_field_get_int(inv, "interactive") == 0) return false;
  InvObject* osd = tree_field_get_obj(panel, "osd");
  if (osd && tree_field_get_obj(osd, "dropObject")) {
    InvObject* dropped = tree_field_get_obj(osd, "dropObject");
    const bool ok = visual_inventory_panel_drag_drop(inv, cmd, dropped);
    tree_field_set_obj(osd, "dropObject", nullptr);
    return ok;
  }
  if (osd && tree_field_get_obj(osd, "dropGadget")) {
    InvObject* dropped = tree_field_get_obj(osd, "dropGadget");
    const bool ok = visual_inventory_panel_swap(inv, cmd, dropped);
    tree_field_set_obj(osd, "dropGadget", nullptr);
    return ok;
  }
  const bool ok = visual_inventory_panel_left_click(inv, cmd);
  tree_field_set_int(inv, "last_panel_click", cmd);
  return ok;
}

bool mechanic_click_actual(InvObject* mechanic) {
  if (!mechanic) return false;
  InvObject* panel = mechanic_actual_panel(mechanic);
  if (!panel) return false;
  const int32_t idx = tree_field_get_int(panel, "index");
  const bool ok = inventory_panel_osd_command(panel, idx);
  tree_field_set_int(mechanic, "click_count",
                     tree_field_get_int(mechanic, "click_count") + 1);
  tree_field_set_int(mechanic, "last_click_panel", idx);
  tree_field_set_int(mechanic, "last_click_ok", ok ? 1 : 0);
  return ok;
}

void mechanic_begin_drag_object(InvObject* mechanic, InvObject* part) {
  if (!mechanic) return;
  tree_field_set_obj(mechanic, "drag_object", part);
  if (part) tree_field_set_obj(mechanic, "look_part", part);
}

void mechanic_set_look_part(InvObject* mechanic, InvObject* part) {
  if (!mechanic) return;
  tree_field_set_obj(mechanic, "look_part", part);
}

static void part_collect_mounted(InvObject* root, std::vector<InvObject*>* out,
                                 int depth) {
  if (!root || !out || depth > 24) return;
  const int32_t n = part_slot_count(root);
  for (int32_t i = 0; i < n; ++i) {
    const int32_t sid = part_slot_id_at(root, i);
    if (sid <= 0) continue;
    InvObject* child = part_on_slot(root, sid);
    if (!child) continue;
    const char* hc = tree_host_class(child);
    if (hc && std::strstr(hc, "Chassis")) {
      part_collect_mounted(child, out, depth + 1);
      continue;
    }
    out->push_back(child);
    part_collect_mounted(child, out, depth + 1);
  }
}

static bool part_approx_world_pos(InvObject* part, float* ox, float* oy,
                                  float* oz) {
  if (!part || !ox || !oy || !oz) return false;
  InvObject* parent = tree_field_get_obj(part, "part_parent");
  const int32_t sid = tree_field_get_int(part, "part_parent_slot");
  float sx = 0.f, sy = 0.f, sz = 0.f, oy0 = 0.f, op0 = 0.f, or0 = 0.f;
  if (parent && sid > 0 &&
      part_slot_get_pose(parent, sid, &sx, &sy, &sz, &oy0, &op0, &or0)) {
    constexpr float kM = 100.f;
    float cx = 0.f, cy = 0.f, cz = 0.f;
    if (InvObject* cp = java_util_resource_GameRef_getPos(parent))
      vec3_get(cp, &cx, &cy, &cz);
    *ox = cx + sx * kM;
    *oy = cy + sy * kM;
    *oz = cz + sz * kM;
    return true;
  }
  if (InvObject* p = java_util_resource_GameRef_getPos(part)) {
    vec3_get(p, ox, oy, oz);
    return true;
  }
  return false;
}

InvObject* mechanic_pick_part_at(InvObject* mechanic, float nx, float ny) {
  // nx,ny in [0,1] top-left (same as inventory panels).
  if (!mechanic) return nullptr;
  InvObject* player = tree_field_get_obj(mechanic, "player");
  InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
  if (!car && game_logic_player())
    car = tree_field_get_obj(game_logic_player(), "car");
  if (!car) return nullptr;

  const float ndc_x = nx * 2.f - 1.f;
  const float ndc_y = 1.f - ny * 2.f;
  float hit_x = 0.f, hit_y = 0.f, hit_z = 0.f;
  void* vp = render_d3d9_viewport_active();
  void* cam = render_d3d9_camera_active();
  if (!render_d3d9_viewport_unproject(vp, cam, ndc_x, ndc_y, &hit_x, &hit_y,
                                      &hit_z)) {
    hit_x = ndc_x * 2.f;
    hit_y = 0.f;
    hit_z = ndc_y * 2.f;
  }

  std::vector<InvObject*> parts;
  part_collect_mounted(car, &parts, 0);
  InvObject* prefer = tree_field_get_obj(car, "last_installed_part");

  InvObject* best = nullptr;
  float best_d2 = 0.15f * 0.15f;  // NDC screen radius
  bool screen_hit = false;
  for (InvObject* p : parts) {
    float wx = 0.f, wy = 0.f, wz = 0.f;
    if (!part_approx_world_pos(p, &wx, &wy, &wz)) continue;
    float px = 0.f, py = 0.f;
    if (!render_d3d9_project(wx, wy, wz, &px, &py)) continue;
    float d2 = (px - ndc_x) * (px - ndc_x) + (py - ndc_y) * (py - ndc_y);
    if (prefer && p == prefer) d2 *= 0.25f;
    if (d2 < best_d2) {
      best_d2 = d2;
      best = p;
      screen_hit = true;
    }
  }
  if (!screen_hit) {
    // Fallback: XZ distance to unprojected ground hit (mesh cm).
    best_d2 = 120.f * 120.f;
    for (InvObject* p : parts) {
      float wx = 0.f, wy = 0.f, wz = 0.f;
      if (!part_approx_world_pos(p, &wx, &wy, &wz)) continue;
      const float dx = wx - hit_x;
      const float dz = wz - hit_z;
      float d2 = dx * dx + dz * dz;
      if (prefer && p == prefer) d2 *= 0.25f;
      if (d2 < best_d2) {
        best_d2 = d2;
        best = p;
      }
    }
  }
  tree_field_set_float(mechanic, "pick_hit_x", hit_x);
  tree_field_set_float(mechanic, "pick_hit_y", hit_y);
  tree_field_set_float(mechanic, "pick_hit_z", hit_z);
  tree_field_set_float(mechanic, "pick_dist",
                       best ? std::sqrt(best_d2) : -1.f);
  tree_field_set_int(mechanic, "pick_via_screen", screen_hit ? 1 : 0);
  return best;
}

bool mechanic_hover_car_at(InvObject* mechanic, float nx, float ny) {
  if (!mechanic) return false;
  InvObject* part = mechanic_pick_part_at(mechanic, nx, ny);
  if (part) {
    mechanic_set_look_part(mechanic, part);
    tree_field_set_int(mechanic, "over_vehicle", 1);
    tree_field_set_int(mechanic, "pick_count",
                       tree_field_get_int(mechanic, "pick_count") + 1);
    // Stock EC_HOVER: infoline ← part name + condition.
    InvObject* tip = tree_field_get_obj(part, "name");
    if (!tip) tip = string_new("part");
    InvObject* line = tree_field_get_obj(mechanic, "infoline");
    if (!line) {
      line = tree_host_new("java.render.Text");
      tree_field_set_obj(mechanic, "infoline", line);
    }
    tree_field_set_obj(line, "text", tip);
    return true;
  }
  if (tree_field_get_int(mechanic, "over_vehicle")) {
    tree_field_set_int(mechanic, "over_vehicle", 0);
    tree_field_set_obj(mechanic, "look_part", nullptr);
    InvObject* line = tree_field_get_obj(mechanic, "infoline");
    if (line) tree_field_set_obj(line, "text", nullptr);
  }
  return false;
}

bool part_is_tuneable(InvObject* part) {
  if (!part) return false;
  if (tree_field_get_int(part, "tuneable") != 0) return true;
  const char* hc = tree_host_class(part);
  if (!hc || !hc[0]) return false;
  if (std::strstr(hc, "Tyre") || std::strstr(hc, "Block")) return true;
  if (std::strstr(hc, "NOS") || std::strstr(hc, "FuelInjector")) return true;
  if (std::strstr(hc, "Transmission")) {
    const int32_t adj_gears = tree_field_get_int(part, "adjustable_gears");
    const float dmin = tree_field_get_float(part, "diff_lock_min");
    const float dmax = tree_field_get_float(part, "diff_lock_max");
    const float fmin = tree_field_get_float(part, "drive_front_min");
    const float fmax = tree_field_get_float(part, "drive_front_max");
    return adj_gears != 0 || dmin != dmax || fmin != fmax;
  }
  if (std::strstr(hc, "Turbo")) {
    return tree_field_get_float(part, "max_waste") > 0.f &&
           tree_field_get_float(part, "min_waste") > 0.f;
  }
  if (std::strstr(hc, "Chassis"))
    return tree_field_get_int(part, "brake_balance_can_be_set") != 0;
  if (std::strstr(hc, "Camshaft")) {
    return tree_field_get_float(part, "advance_positive_peak") >
           tree_field_get_float(part, "advance_negative_peak");
  }
  if (std::strstr(hc, "Shock")) {
    return tree_field_get_int(part, "adjustable_damping") != 0 &&
           tree_field_get_int(part, "adjustable_rebound_factor") != 0;
  }
  return false;
}

bool part_flap_toggle(InvObject* part) {
  // Stock: part.command("flap_toggle") when not tuneable in Mechanic tune mode.
  if (!part) return false;
  const int32_t next = tree_field_get_int(part, "flap_state") ^ 1;
  tree_field_set_int(part, "flap_state", next);
  tree_field_set_int(part, "flap_toggle_count",
                     tree_field_get_int(part, "flap_toggle_count") + 1);
  tree_field_set_obj(part, "last_command", string_new("flap_toggle"));
  return true;
}

// Phase 2.153/2.154 — stock Part.buildTuningMenu dispatch.
enum : int32_t {
  kTuneBlock = 1,
  kTuneTyre = 2,
  kTuneTurbo = 3,
  kTuneChassis = 4,
  kTuneCamshaft = 5,
  kTuneShock = 6,
  kTuneTransmission = 7,
  kTuneInjector = 8,
  kTuneNos = 9,
};

static float part_ratio_at(InvObject* part, int32_t i) {
  char key[16];
  std::snprintf(key, sizeof(key), "ratio_%d", i);
  return tree_field_get_float(part, key);
}

static void part_ratio_set(InvObject* part, int32_t i, float v) {
  char key[16];
  std::snprintf(key, sizeof(key), "ratio_%d", i);
  tree_field_set_float(part, key, v);
}

static float part_old_ratio_at(InvObject* part, int32_t i) {
  char key[20];
  std::snprintf(key, sizeof(key), "old_ratio_%d", i);
  return tree_field_get_float(part, key);
}

static void part_old_ratio_set(InvObject* part, int32_t i, float v) {
  char key[20];
  std::snprintf(key, sizeof(key), "old_ratio_%d", i);
  tree_field_set_float(part, key, v);
}

static int32_t part_detect_tune_kind(InvObject* part) {
  if (!part) return kTuneBlock;
  const int32_t stored = tree_field_get_int(part, "tune_kind");
  if (stored > 0) return stored;
  const char* hc = tree_host_class(part);
  if (hc && hc[0]) {
    if (std::strstr(hc, "Tyre")) return kTuneTyre;
    if (std::strstr(hc, "Turbo")) return kTuneTurbo;
    if (std::strstr(hc, "NOS")) return kTuneNos;
    if (std::strstr(hc, "FuelInjector") || std::strstr(hc, "InjectorSystem"))
      return kTuneInjector;
    if (std::strstr(hc, "Transmission")) return kTuneTransmission;
    if (std::strstr(hc, "Chassis")) return kTuneChassis;
    if (std::strstr(hc, "Camshaft")) return kTuneCamshaft;
    if (std::strstr(hc, "Shock")) return kTuneShock;
    if (std::strstr(hc, "Block")) return kTuneBlock;
  }
  if (tree_field_get_float(part, "optimal_inflation") > 0.f ||
      tree_field_get_float(part, "inflation") > 0.f)
    return kTuneTyre;
  if (tree_field_get_float(part, "max_waste") > 0.f) return kTuneTurbo;
  if (tree_field_get_float(part, "nitro_consumption") > 0.f ||
      tree_field_get_float(part, "maxconsumption") > 0.f)
    return kTuneNos;
  if (tree_field_get_float(part, "mixture_ratio") > 0.f) return kTuneInjector;
  if (tree_field_get_int(part, "adjustable_gears") != 0 ||
      tree_field_get_int(part, "gears") > 0)
    return kTuneTransmission;
  if (tree_field_get_int(part, "brake_balance_can_be_set") != 0)
    return kTuneChassis;
  if (tree_field_get_float(part, "advance_positive_peak") >
      tree_field_get_float(part, "advance_negative_peak"))
    return kTuneCamshaft;
  if (tree_field_get_int(part, "adjustable_damping") != 0) return kTuneShock;
  return kTuneBlock;
}

static InvObject* menu_add_slider(InvObject* menu, const char* text, int32_t cmd,
                                  float value, float a, float b, int32_t steps) {
  if (!menu) return nullptr;
  InvObject* s = tree_host_new("java.render.osd.Slider");
  tree_field_set_obj(s, "label", string_new(text ? text : "Tune"));
  tree_field_set_int(s, "command", cmd);
  tree_field_set_float(s, "value", value);
  tree_field_set_float(s, "min", a);
  tree_field_set_float(s, "max", b);
  tree_field_set_int(s, "steps", steps);
  float y = tree_field_get_float(menu, "y");
  float sp = tree_field_get_float(menu, "spacing");
  if (sp <= 0.f) sp = 0.08f;
  tree_field_set_float(s, "x", tree_field_get_float(menu, "x"));
  tree_field_set_float(s, "y", y);
  tree_field_set_float(menu, "y", y + sp);
  InvObject* items = tree_field_get_obj(menu, "items");
  if (!items) {
    items = tree_vector_new();
    tree_field_set_obj(menu, "items", items);
  }
  tree_vector_add(items, s);
  tree_field_set_int(menu, "item_count",
                     tree_field_get_int(menu, "item_count") + 1);
  return s;
}

static InvObject* menu_add_button(InvObject* menu, const char* text,
                                  int32_t cmd) {
  if (!menu) return nullptr;
  InvObject* g = tree_host_new("java.render.osd.Gadget");
  tree_field_set_obj(g, "label", string_new(text ? text : "Reset"));
  tree_field_set_int(g, "command", cmd);
  float y = tree_field_get_float(menu, "y");
  float sp = tree_field_get_float(menu, "spacing");
  if (sp <= 0.f) sp = 0.08f;
  tree_field_set_float(g, "x", tree_field_get_float(menu, "x"));
  tree_field_set_float(g, "y", y);
  tree_field_set_float(menu, "y", y + sp);
  InvObject* items = tree_field_get_obj(menu, "items");
  if (!items) {
    items = tree_vector_new();
    tree_field_set_obj(menu, "items", items);
  }
  tree_vector_add(items, g);
  tree_field_set_int(menu, "item_count",
                     tree_field_get_int(menu, "item_count") + 1);
  return g;
}

static void menu_set_slider_value(InvObject* menu, int32_t cmd, float v) {
  if (!menu) return;
  InvObject* items = tree_field_get_obj(menu, "items");
  const int32_t n = items ? tree_vector_size(items) : 0;
  for (int32_t i = 0; i < n; ++i) {
    InvObject* s = tree_vector_element_at(items, i);
    if (!s || tree_field_get_int(s, "command") != cmd) continue;
    const char* hc = tree_host_class(s);
    if (hc && !std::strstr(hc, "Slider")) continue;
    float mn = tree_field_get_float(s, "min");
    float mx = tree_field_get_float(s, "max");
    if (v < mn) v = mn;
    if (v > mx) v = mx;
    tree_field_set_float(s, "value", v);
    return;
  }
}

void part_build_tuning_menu(InvObject* part, InvObject* menu) {
  // Host mirrors of Part subclass buildTuningMenu (Block/Tyre/Turbo/…).
  if (!part || !menu) return;
  const int32_t kind = part_detect_tune_kind(part);
  tree_field_set_int(part, "tune_kind", kind);

  if (kind == kTuneTyre) {
    float infl = tree_field_get_float(part, "inflation");
    if (infl <= 0.f) infl = 2.f;
    float opt = tree_field_get_float(part, "optimal_inflation");
    if (opt <= 0.5f) opt = 2.5f;
    tree_field_set_float(part, "inflation", infl);
    tree_field_set_float(part, "new_inflation", infl);
    tree_field_set_float(part, "old_inflation", infl);
    tree_field_set_float(part, "optimal_inflation", opt);
    menu_add_slider(menu, "Inflation", 1, infl, 0.5f, opt,
                    static_cast<int32_t>((opt - 0.5f) / 0.1f) + 1);
  } else if (kind == kTuneTurbo) {
    float waste = tree_field_get_float(part, "P_turbo_waste");
    float mn = tree_field_get_float(part, "min_waste");
    float mx = tree_field_get_float(part, "max_waste");
    if (mn <= 0.f) mn = 0.5f;
    if (mx <= mn) mx = mn + 1.f;
    if (waste < mn) waste = mn;
    tree_field_set_float(part, "P_turbo_waste", waste);
    tree_field_set_float(part, "old_waste", waste);
    tree_field_set_float(part, "min_waste", mn);
    tree_field_set_float(part, "max_waste", mx);
    menu_add_slider(menu, "Wastegate pressure", 1, waste, mn, mx,
                    static_cast<int32_t>((mx - mn) / 0.05f) + 1);
  } else if (kind == kTuneChassis) {
    float bal = tree_field_get_float(part, "brake_balance");
    if (bal <= 0.f) bal = 0.5f;
    tree_field_set_float(part, "brake_balance", bal);
    tree_field_set_float(part, "old_brake_balance", bal);
    menu_add_slider(menu, "F-R brake balance", 1, -bal, -1.f, 0.f, 51);
  } else if (kind == kTuneCamshaft) {
    float adv = tree_field_get_float(part, "advance");
    float neg = tree_field_get_float(part, "advance_negative_peak");
    float pos = tree_field_get_float(part, "advance_positive_peak");
    float step = tree_field_get_float(part, "advance_minimum_step");
    if (step <= 0.f) step = 0.5f;
    if (pos <= neg) {
      neg = -10.f;
      pos = 40.f;
    }
    float def = tree_field_get_float(part, "default_advance");
    tree_field_set_float(part, "advance", adv);
    tree_field_set_float(part, "old_advance", adv);
    tree_field_set_float(part, "default_advance", def);
    tree_field_set_float(part, "advance_negative_peak", neg);
    tree_field_set_float(part, "advance_positive_peak", pos);
    menu_add_slider(menu, "Advance angle", 1, adv, neg, pos,
                    static_cast<int32_t>((pos - neg) / step) + 1);
    menu_add_button(menu, "Reset to factory defaults", 0);
  } else if (kind == kTuneShock) {
    float damp = tree_field_get_float(part, "damping");
    float reb = tree_field_get_float(part, "rebound_factor");
    float dmin = tree_field_get_float(part, "min_damping");
    float dmax = tree_field_get_float(part, "max_damping");
    float rmin = tree_field_get_float(part, "min_rebound_factor");
    float rmax = tree_field_get_float(part, "max_rebound_factor");
    if (dmax <= dmin) {
      dmin = 1000.f;
      dmax = 8000.f;
    }
    if (rmax <= rmin) {
      rmin = 0.2f;
      rmax = 1.5f;
    }
    tree_field_set_float(part, "damping", damp);
    tree_field_set_float(part, "rebound_factor", reb);
    tree_field_set_float(part, "old_damping", damp);
    tree_field_set_float(part, "old_rebound_factor", reb);
    if (tree_field_get_float(part, "default_damping") <= 0.f)
      tree_field_set_float(part, "default_damping", damp);
    if (tree_field_get_float(part, "default_rebound_factor") <= 0.f)
      tree_field_set_float(part, "default_rebound_factor", reb);
    if (tree_field_get_int(part, "adjustable_damping"))
      menu_add_slider(menu, "Bound damping", 1, damp, dmin, dmax,
                      static_cast<int32_t>((dmax - dmin) / 100.f) + 1);
    if (tree_field_get_int(part, "adjustable_rebound_factor"))
      menu_add_slider(menu, "Rebound factor", 2, reb, rmin, rmax,
                      static_cast<int32_t>(rmax - rmin) - 1);
    menu_add_button(menu, "Reset to factory defaults", 0);
  } else if (kind == kTuneTransmission) {
    // Stock Transmission.buildTuningMenu — gears / R / end / LSD / drive.
    int32_t gears = tree_field_get_int(part, "gears");
    if (gears < 1) gears = 5;
    if (gears > 6) gears = 6;
    tree_field_set_int(part, "gears", gears);
    int32_t adj = tree_field_get_int(part, "adjustable_gears");
    if (adj == 0) adj = 7;  // forward|reverse|end
    tree_field_set_int(part, "adjustable_gears", adj);
    // Default positive ratios if unset.
    static const float kDefRatio[8] = {0.f, 3.5f, 2.2f, 1.5f, 1.1f, 0.9f,
                                       0.75f, 3.2f};
    for (int32_t i = 0; i < 8; ++i) {
      float r = part_ratio_at(part, i);
      if (i > 0 && i < 7 && r == 0.f) r = kDefRatio[i];
      if (i == 7 && r == 0.f) r = kDefRatio[7];
      part_ratio_set(part, i, r);
      part_old_ratio_set(part, i, r);
    }
    float end_r = tree_field_get_float(part, "end_ratio");
    if (end_r <= 0.f) end_r = 3.9f;
    tree_field_set_float(part, "end_ratio", end_r);
    tree_field_set_float(part, "old_end_ratio", end_r);
    // Stock flips forward gears to negative for slider UI (−5..−0.5).
    for (int32_t i = 1; i <= 6; ++i)
      part_ratio_set(part, i, -part_ratio_at(part, i));
    float dlock = tree_field_get_float(part, "diff_lock");
    float dmin = tree_field_get_float(part, "diff_lock_min");
    float dmax = tree_field_get_float(part, "diff_lock_max");
    if (dmax <= dmin) {
      dmin = 0.f;
      dmax = 1.f;
    }
    tree_field_set_float(part, "diff_lock", dlock);
    tree_field_set_float(part, "old_diff_lock", dlock);
    tree_field_set_float(part, "diff_lock_min", dmin);
    tree_field_set_float(part, "diff_lock_max", dmax);
    float drive = tree_field_get_float(part, "drive_front");
    float fmin = tree_field_get_float(part, "drive_front_min");
    float fmax = tree_field_get_float(part, "drive_front_max");
    if (fmax <= fmin) {
      fmin = 0.f;
      fmax = 1.f;
    }
    tree_field_set_float(part, "drive_front", drive);
    tree_field_set_float(part, "old_drive_front", drive);
    tree_field_set_float(part, "drive_front_min", fmin);
    tree_field_set_float(part, "drive_front_max", fmax);
    const int32_t adj_diff = (dmin != dmax) ? 1 : 0;
    const int32_t adj_drive = (fmin != fmax) ? 1 : 0;
    tree_field_set_int(part, "adjustable_diff_lock", adj_diff);
    tree_field_set_int(part, "adjustable_drive", adj_drive);
    static const char* kGearNames[] = {"",    "1st", "2nd", "3rd",
                                       "4th", "5th", "6th"};
    if (adj & 1) {
      for (int32_t i = 1; i <= gears; ++i)
        menu_add_slider(menu, kGearNames[i], i, part_ratio_at(part, i), -5.f,
                        -0.5f, 0);
    }
    if (adj & 2)
      menu_add_slider(menu, "R", 7, part_ratio_at(part, 7), -5.f, -0.5f, 0);
    if (adj & 4)
      menu_add_slider(menu, "End ratio", 8, end_r, 1.f, 8.f, 0);
    if (adj_diff)
      menu_add_slider(menu, "Limited slip differential lock rate", 9, dlock,
                      dmin, dmax,
                      static_cast<int32_t>((dmax - dmin) * 100.f) + 1);
    if (adj_drive)
      menu_add_slider(menu, "Drive distribution", 10, -drive, -fmax, -fmin,
                      static_cast<int32_t>((fmax - fmin) * 100.f) + 1);
  } else if (kind == kTuneInjector) {
    // FuelInjectorSystem — Mixture ratio (fuel type MultiChoice skipped).
    float mix = tree_field_get_float(part, "mixture_ratio");
    if (mix <= 0.f) mix = 14.7f;
    tree_field_set_float(part, "mixture_ratio", mix);
    tree_field_set_float(part, "old_mixture_ratio", mix);
    menu_add_slider(menu, "Mixture ratio", 1, mix, 8.f, 20.f, 49);
  } else if (kind == kTuneNos) {
    float cons = tree_field_get_float(part, "nitro_consumption");
    float mn = tree_field_get_float(part, "minconsumption");
    float mx = tree_field_get_float(part, "maxconsumption");
    if (mn <= 0.f) mn = 0.01f;
    if (mx <= mn) mx = mn + 0.5f;
    if (cons < mn) cons = mn;
    tree_field_set_float(part, "nitro_consumption", cons);
    tree_field_set_float(part, "old_nitro_consumption", cons);
    tree_field_set_float(part, "minconsumption", mn);
    tree_field_set_float(part, "maxconsumption", mx);
    menu_add_slider(menu, "Amount injected", 1, cons, mn, mx, 0);
  } else {
    // Block.buildTuningMenu — Idle + Redline.
    float idle = tree_field_get_float(part, "rpm_idle");
    if (idle <= 0.f) idle = 800.f;
    float redline = tree_field_get_float(part, "RPM_limit");
    if (redline <= 0.f) redline = 7000.f;
    tree_field_set_float(part, "rpm_idle", idle);
    tree_field_set_float(part, "RPM_limit", redline);
    tree_field_set_float(part, "old_rpm_idle", idle);
    tree_field_set_float(part, "old_RPM_limit", redline);
    menu_add_slider(menu, "Idle", 1, idle, 150.f, 2500.f,
                    (2500 - 150) / 50 + 1);
    menu_add_slider(menu, "Redline", 2, redline, 3000.f, 12000.f,
                    (12000 - 3000) / 250 + 1);
  }
  tree_field_set_int(part, "tuning_menu_built", 1);
}

InvObject* dialog_modal_active() { return g_dialog_modal; }

void dialog_set_smoke_string(const char* s) {
  g_dialog_smoke_string = (s && s[0]) ? s : "Player";
}

void dialog_note_constructed(InvObject* dialog, const char* class_fqn) {
  // Track modal dialogs for TREE that misbinds display() onto MainMenuDialog.
  if (!dialog || !class_fqn) return;
  if (!std::strstr(class_fqn, "Dialog")) return;
  if (std::strstr(class_fqn, "MainMenuDialog") ||
      std::strstr(class_fqn, "OptionsDialog"))
    return;
  g_recent_modal_dialog = dialog;
  tree_field_set_obj(dialog, "host_class_hint", string_new(class_fqn));
}

InvObject* dialog_resolve_display_target(InvObject* maybe_hub) {
  if (!maybe_hub) return g_recent_modal_dialog;
  const char* hc = tree_host_class(maybe_hub);
  if (hc && (std::strstr(hc, "MainMenuDialog") ||
             std::strstr(hc, "OptionsDialog"))) {
    if (g_recent_modal_dialog) return g_recent_modal_dialog;
  }
  return maybe_hub;
}

int32_t dialog_display(InvObject* dialog) {
  // Stock Dialog.display(): if (!shown) show(); wait(); hide(); return result;
  // Host: no Object.wait — smoke/live auto-accept OK (result=0). Do NOT invoke
  // subclass show/hide TREE (MainMenuDialog.hide would tear down the hub).
  dialog = dialog_resolve_display_target(dialog);
  if (!dialog) return -1;
  g_dialog_modal = dialog;
  const char* hc = tree_host_class(dialog);
  // Prefer FQN from NEW when host_class was left as Object/hub.
  if (InvObject* hint = tree_field_get_obj(dialog, "host_class_hint")) {
    if (const char* hs = string_cstr(hint))
      if (hs[0]) hc = hs;
  }

  if (!tree_field_get_obj(dialog, "osd")) {
    InvObject* osd = tree_host_new("java.render.Osd");
    osd_ensure_defaults(osd);
    tree_field_set_obj(dialog, "osd", osd);
  }
  dialog_ensure_osd_buttons(dialog);
  tree_field_set_int(dialog, "shown", 1);

  const bool is_str_req = hc && std::strstr(hc, "StringRequester");
  const bool is_setup = hc && std::strstr(hc, "PlayerSetup");

  if (is_str_req) {
    tree_field_set_obj(dialog, "input",
                       string_new(g_dialog_smoke_string.c_str()));
    if (InvObject* player = game_logic_player())
      tree_field_set_obj(player, "name",
                         string_new(g_dialog_smoke_string.c_str()));
  }
  if (is_setup) {
    InvObject* plr = tree_field_get_obj(dialog, "plr");
    if (!plr) plr = game_logic_player();
    if (plr && !tree_field_get_obj(plr, "character")) {
      int32_t rid = 0;
      if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
        rid = rpak_make_id(fe->pack_id, 0x00da);
      else if (const RpakPack* fe = rpak_find_by_name("frontend"))
        rid = rpak_make_id(fe->pack_id, 0x00da);
      InvObject* ch = tree_host_new("java.util.resource.ResourceRef");
      if (rid) java_util_resource_ResourceRef_set(ch, rid);
      tree_field_set_obj(plr, "character", ch);
    }
  }

  tree_field_set_int(dialog, "result", 0);
  tree_field_set_int(dialog, "shown", 0);
  if (InvObject* osd = tree_field_get_obj(dialog, "osd"))
    tree_field_set_int(osd, "visible", 0);
  tree_field_set_int(dialog, "display_via_host", 1);
  g_dialog_modal = nullptr;
  // Consume so a second display() picks the next NEW dialog.
  if (dialog == g_recent_modal_dialog) g_recent_modal_dialog = nullptr;
  std::printf("[script] Dialog.display accept %s\n",
              hc && hc[0] ? hc : "?");
  return 0;
}

void dialog_ensure_osd_buttons(InvObject* dialog) {
  // Stock Dialog ctor/show: title Text + OK;Cancel buttons + AXIS_CANCEL hotkey.
  // TREE show often skips createButton — mirror chrome for tune/warning dialogs.
  if (!dialog) return;
  InvObject* osd = tree_field_get_obj(dialog, "osd");
  if (!osd) return;

  const int32_t flags = tree_field_get_int(dialog, "flags");
  constexpr int32_t kDfWide = 0x00000080;
  if (flags & kDfWide) {
    tree_field_set_float(osd, "vpAspect", 2.f);  // 6/3
    tree_field_set_float(osd, "vpWidth", 0.9f);
    tree_field_set_int(dialog, "wide_applied", 1);
  }

  if (!tree_field_get_obj(osd, "buttons"))
    tree_field_set_obj(osd, "buttons", tree_vector_new());
  InvObject* btns = tree_field_get_obj(osd, "buttons");
  if (tree_vector_size(btns) < 1) {
    const char* texts = "OK;Cancel";
    if (InvObject* bt = tree_field_get_obj(dialog, "buttonTexts")) {
      if (const char* s = string_cstr(bt))
        if (s[0]) texts = s;
    }
    // Parse "OK;Cancel" → one Button per token (command = index).
    int32_t cmd = 0;
    const char* p = texts;
    while (p && *p) {
      const char* semi = std::strchr(p, ';');
      InvObject* btn = tree_host_new("java.render.osd.Button");
      tree_field_set_int(btn, "command", cmd);
      tree_field_set_obj(btn, "osd", osd);
      tree_vector_add(btns, btn);
      ++cmd;
      if (!semi) break;
      p = semi + 1;
    }
  }
  tree_field_set_int(osd, "button_count", tree_vector_size(btns));

  if (!tree_field_get_obj(osd, "title_text")) {
    InvObject* title = tree_field_get_obj(dialog, "title");
    if (title && string_cstr(title) && string_cstr(title)[0]) {
      InvObject* txt = tree_host_new("java.render.Text");
      tree_field_set_obj(txt, "string", title);
      tree_field_set_obj(osd, "title_text", txt);
      tree_field_set_int(osd, "text_count",
                         tree_field_get_int(osd, "text_count") + 1);
    }
  }

  const int32_t esc = tree_field_get_int(dialog, "escapeCmd");
  if (esc >= 0 && !tree_field_get_int(dialog, "cancel_hotkey")) {
    if (!tree_field_get_obj(osd, "hotkey"))
      tree_field_set_obj(osd, "hotkey", tree_vector_new());
    InvObject* hk = tree_host_new("java.render.osd.Hotkey");
    tree_field_set_int(hk, "command", esc);
    tree_vector_add(tree_field_get_obj(osd, "hotkey"), hk);
    tree_field_set_int(osd, "hotkey_count",
                       tree_vector_size(tree_field_get_obj(osd, "hotkey")));
    tree_field_set_int(dialog, "cancel_hotkey", 1);
  }
}

InvObject* mechanic_open_tune_dialog(InvObject* mechanic, InvObject* part) {
  // Stock Mechanic: new Dialog(..., "TUNE PART", "OK;Cancel") + createMenu +
  // buildTuningMenu.
  if (!mechanic || !part) return nullptr;
  constexpr int32_t kFlags =
      0x00000001 | 0x00000004 | 0x00000008 | 0x00000080;  // MODAL|BG|FREEZE|WIDE
  InvObject* dlg = tree_host_new("java.render.osd.dialog.Dialog");
  InvObject* osd = tree_host_new("java.render.Osd");
  tree_field_set_obj(osd, "groups", tree_vector_new());
  tree_field_set_obj(osd, "hotkey", tree_vector_new());
  tree_field_set_obj(osd, "rectangles", tree_vector_new());
  tree_field_set_obj(osd, "buttons", tree_vector_new());
  // Stock DF_WIDE: Osd(0.9, 6/3).
  tree_field_set_float(osd, "vpAspect", 2.f);
  tree_field_set_float(osd, "vpWidth", 0.9f);
  tree_field_set_float(osd, "vpHeight", 1.f);
  tree_field_set_int(osd, "iLevel", 3);
  tree_field_set_obj(osd, "globalHandler", part);
  tree_field_set_obj(dlg, "osd", osd);
  InvObject* player = tree_field_get_obj(mechanic, "player");
  InvObject* ctrl = player ? tree_field_get_obj(player, "controller") : nullptr;
  if (ctrl) tree_field_set_obj(dlg, "controller", ctrl);
  tree_field_set_int(dlg, "flags", kFlags);
  tree_field_set_obj(dlg, "title", string_new("TUNE PART"));
  tree_field_set_obj(dlg, "buttonTexts", string_new("OK;Cancel"));
  tree_field_set_float(dlg, "bodyRatio", 0.9f);
  tree_field_set_int(dlg, "priority", 22);
  tree_field_set_int(dlg, "escapeCmd", 1);
  // DF_DEFAULTBG
  {
    int32_t rid = 0;
    if (const RpakPack* fe = rpak_find_by_name("frontend.rpk"))
      rid = rpak_make_id(fe->pack_id, 0x000E);
    else if (const RpakPack* fe = rpak_find_by_name("frontend"))
      rid = rpak_make_id(fe->pack_id, 0x000E);
    if (rid) {
      InvObject* pic = gameref_new();
      java_util_resource_ResourceRef_set(pic, rid);
      tree_field_set_obj(osd, "bg", pic);
      tree_field_set_int(osd, "bg_created", 1);
      java_util_resource_ResourceRef_load(pic);
      render_d3d9_osd_add_rect(0.f, 0.f, 2.f, 2.f, pic, -2);
    }
  }
  InvObject* menu = tree_host_new("java.render.osd.Menu");
  tree_field_set_obj(menu, "osd", osd);
  tree_field_set_obj(menu, "items", tree_vector_new());
  tree_field_set_int(menu, "item_count", 0);
  tree_field_set_float(menu, "x", 0.f);
  tree_field_set_float(menu, "y", -0.5f);
  tree_field_set_float(menu, "spacing", 0.08f);
  tree_field_set_obj(dlg, "tune_menu", menu);
  tree_field_set_obj(osd, "tune_menu", menu);
  part_build_tuning_menu(part, menu);
  tree_field_set_obj(osd, "globalHandler", dlg);
  dialog_ensure_osd_buttons(dlg);
  tree_field_set_int(dlg, "shown", 1);
  tree_field_set_int(osd, "visible", 1);
  tree_field_set_obj(mechanic, "tune_dialog", dlg);
  tree_field_set_obj(mechanic, "last_tune_part", part);
  tree_field_set_int(mechanic, "tune_dialog_count",
                     tree_field_get_int(mechanic, "tune_dialog_count") + 1);
  return dlg;
}

bool mechanic_tune_set_slider(InvObject* mechanic, int32_t cmd, float value) {
  // Stock Part.handleMessage for slider cmds (kind-dependent fields).
  if (!mechanic) return false;
  InvObject* part = tree_field_get_obj(mechanic, "last_tune_part");
  InvObject* dlg = tree_field_get_obj(mechanic, "tune_dialog");
  InvObject* menu = dlg ? tree_field_get_obj(dlg, "tune_menu") : nullptr;
  if (!part || !menu) return false;
  InvObject* items = tree_field_get_obj(menu, "items");
  const int32_t n = items ? tree_vector_size(items) : 0;
  for (int32_t i = 0; i < n; ++i) {
    InvObject* s = tree_vector_element_at(items, i);
    if (!s || tree_field_get_int(s, "command") != cmd) continue;
    const char* hc = tree_host_class(s);
    if (hc && !std::strstr(hc, "Slider")) continue;
    float v = value;
    const float mn = tree_field_get_float(s, "min");
    const float mx = tree_field_get_float(s, "max");
    if (v < mn) v = mn;
    if (v > mx) v = mx;
    tree_field_set_float(s, "value", v);
    const int32_t kind = tree_field_get_int(part, "tune_kind");
    if (kind == kTuneTyre && cmd == 1) {
      tree_field_set_float(part, "new_inflation", v);
      tree_field_set_float(part, "inflation", v);
    } else if (kind == kTuneTurbo && cmd == 1) {
      tree_field_set_float(part, "P_turbo_waste", v);
    } else if (kind == kTuneChassis && cmd == 1) {
      tree_field_set_float(part, "brake_balance", -v);
    } else if (kind == kTuneCamshaft && cmd == 1) {
      tree_field_set_float(part, "advance", v);
    } else if (kind == kTuneShock) {
      if (cmd == 1) tree_field_set_float(part, "damping", v);
      if (cmd == 2) tree_field_set_float(part, "rebound_factor", v);
    } else if (kind == kTuneTransmission) {
      if (cmd >= 1 && cmd <= 7)
        part_ratio_set(part, cmd, v);
      else if (cmd == 8)
        tree_field_set_float(part, "end_ratio", v);
      else if (cmd == 9)
        tree_field_set_float(part, "diff_lock", v);
      else if (cmd == 10)
        tree_field_set_float(part, "drive_front", -v);
    } else if (kind == kTuneInjector && cmd == 1) {
      tree_field_set_float(part, "mixture_ratio", v);
    } else if (kind == kTuneNos && cmd == 1) {
      tree_field_set_float(part, "nitro_consumption", v);
    } else {
      if (cmd == 1) tree_field_set_float(part, "rpm_idle", v);
      if (cmd == 2) tree_field_set_float(part, "RPM_limit", v);
    }
    tree_field_set_int(mechanic, "tune_slider_count",
                       tree_field_get_int(mechanic, "tune_slider_count") + 1);
    return true;
  }
  return false;
}

bool mechanic_tune_menu_command(InvObject* mechanic, int32_t cmd) {
  // Stock handleMessage for non-slider items (cmd 0 = factory reset).
  if (!mechanic || cmd != 0) return false;
  InvObject* part = tree_field_get_obj(mechanic, "last_tune_part");
  InvObject* dlg = tree_field_get_obj(mechanic, "tune_dialog");
  InvObject* menu = dlg ? tree_field_get_obj(dlg, "tune_menu") : nullptr;
  if (!part || !menu) return false;
  const int32_t kind = tree_field_get_int(part, "tune_kind");
  if (kind == kTuneCamshaft) {
    float def = tree_field_get_float(part, "default_advance");
    tree_field_set_float(part, "advance", def);
    menu_set_slider_value(menu, 1, def);
  } else if (kind == kTuneShock) {
    float dd = tree_field_get_float(part, "default_damping");
    float dr = tree_field_get_float(part, "default_rebound_factor");
    tree_field_set_float(part, "damping", dd);
    tree_field_set_float(part, "rebound_factor", dr);
    menu_set_slider_value(menu, 1, dd);
    menu_set_slider_value(menu, 2, dr);
  } else {
    return false;
  }
  tree_field_set_int(mechanic, "tune_reset_count",
                     tree_field_get_int(mechanic, "tune_reset_count") + 1);
  return true;
}

bool mechanic_tune_part(InvObject* mechanic, InvObject* part, int32_t choice) {
  // Stock: Dialog.display() → endTuningSession(choice) (0=OK, 1=Cancel).
  if (!mechanic || !part) return false;
  InvObject* dlg = tree_field_get_obj(mechanic, "tune_dialog");
  if (!dlg || tree_field_get_obj(mechanic, "last_tune_part") != part)
    dlg = mechanic_open_tune_dialog(mechanic, part);
  const int32_t kind = tree_field_get_int(part, "tune_kind");
  if (choice != 0) {
    if (kind == kTuneTyre) {
      tree_field_set_float(part, "inflation",
                           tree_field_get_float(part, "old_inflation"));
      tree_field_set_float(part, "new_inflation",
                           tree_field_get_float(part, "old_inflation"));
    } else if (kind == kTuneTurbo) {
      tree_field_set_float(part, "P_turbo_waste",
                           tree_field_get_float(part, "old_waste"));
    } else if (kind == kTuneChassis) {
      tree_field_set_float(part, "brake_balance",
                           tree_field_get_float(part, "old_brake_balance"));
    } else if (kind == kTuneCamshaft) {
      tree_field_set_float(part, "advance",
                           tree_field_get_float(part, "old_advance"));
    } else if (kind == kTuneShock) {
      tree_field_set_float(part, "damping",
                           tree_field_get_float(part, "old_damping"));
      tree_field_set_float(part, "rebound_factor",
                           tree_field_get_float(part, "old_rebound_factor"));
    } else if (kind == kTuneTransmission) {
      for (int32_t i = 0; i < 8; ++i)
        part_ratio_set(part, i, part_old_ratio_at(part, i));
      tree_field_set_float(part, "end_ratio",
                           tree_field_get_float(part, "old_end_ratio"));
      tree_field_set_float(part, "diff_lock",
                           tree_field_get_float(part, "old_diff_lock"));
      tree_field_set_float(part, "drive_front",
                           tree_field_get_float(part, "old_drive_front"));
    } else if (kind == kTuneInjector) {
      tree_field_set_float(part, "mixture_ratio",
                           tree_field_get_float(part, "old_mixture_ratio"));
    } else if (kind == kTuneNos) {
      tree_field_set_float(part, "nitro_consumption",
                           tree_field_get_float(part, "old_nitro_consumption"));
    } else {
      tree_field_set_float(part, "rpm_idle",
                           tree_field_get_float(part, "old_rpm_idle"));
      tree_field_set_float(part, "RPM_limit",
                           tree_field_get_float(part, "old_RPM_limit"));
    }
  } else {
    if (kind == kTuneTyre) {
      tree_field_set_float(part, "inflation",
                           tree_field_get_float(part, "new_inflation"));
      game_logic_spend_time(1.f * 60.f);
    } else if (kind == kTuneTurbo) {
      if (tree_field_get_float(part, "P_turbo_waste") !=
          tree_field_get_float(part, "old_waste"))
        game_logic_spend_time(4.f * 60.f);
    } else if (kind == kTuneChassis) {
      game_logic_spend_time(3.f * 60.f);
    } else if (kind == kTuneCamshaft) {
      if (tree_field_get_float(part, "advance") !=
          tree_field_get_float(part, "old_advance"))
        game_logic_spend_time(7.f * 60.f);
    } else if (kind == kTuneShock) {
      if (tree_field_get_float(part, "damping") !=
          tree_field_get_float(part, "old_damping"))
        game_logic_spend_time(5.f * 60.f);
      if (tree_field_get_float(part, "rebound_factor") !=
          tree_field_get_float(part, "old_rebound_factor"))
        game_logic_spend_time(5.f * 60.f);
    } else if (kind == kTuneTransmission) {
      // Flip forward gears back to positive (stock endTuningSession).
      for (int32_t i = 1; i <= 6; ++i)
        part_ratio_set(part, i, -part_ratio_at(part, i));
      bool gear_chg = false;
      for (int32_t i = 0; i < 8; ++i) {
        if (part_ratio_at(part, i) != part_old_ratio_at(part, i)) {
          gear_chg = true;
          break;
        }
      }
      if (tree_field_get_float(part, "end_ratio") !=
          tree_field_get_float(part, "old_end_ratio"))
        gear_chg = true;
      if (gear_chg) {
        const int32_t gears = tree_field_get_int(part, "gears");
        game_logic_spend_time(10.f * 60.f +
                              static_cast<float>(gears) * 2.f * 60.f);
      }
      if (tree_field_get_float(part, "diff_lock") !=
          tree_field_get_float(part, "old_diff_lock"))
        game_logic_spend_time(30.f * 60.f);
      if (tree_field_get_float(part, "drive_front") !=
          tree_field_get_float(part, "old_drive_front"))
        game_logic_spend_time(30.f * 60.f);
    } else if (kind == kTuneInjector) {
      if (tree_field_get_float(part, "mixture_ratio") !=
          tree_field_get_float(part, "old_mixture_ratio"))
        game_logic_spend_time(5.f * 60.f);
    } else if (kind == kTuneNos) {
      if (tree_field_get_float(part, "nitro_consumption") !=
          tree_field_get_float(part, "old_nitro_consumption"))
        game_logic_spend_time(3.f * 60.f);
    } else {
      if (tree_field_get_float(part, "rpm_idle") !=
          tree_field_get_float(part, "old_rpm_idle"))
        game_logic_spend_time(4.f * 60.f);
      if (tree_field_get_float(part, "RPM_limit") !=
          tree_field_get_float(part, "old_RPM_limit"))
        game_logic_spend_time(8.f * 60.f);
    }
  }
  if (dlg) {
    tree_field_set_int(dlg, "shown", 0);
    if (InvObject* osd = tree_field_get_obj(dlg, "osd"))
      tree_field_set_int(osd, "visible", 0);
  }
  tree_field_set_obj(mechanic, "tune_dialog", nullptr);
  tree_field_set_int(part, "tune_session_choice", choice);
  tree_field_set_int(part, "tune_session_count",
                     tree_field_get_int(part, "tune_session_count") + 1);
  tree_field_set_int(mechanic, "tune_count",
                     tree_field_get_int(mechanic, "tune_count") + 1);
  tree_field_set_obj(mechanic, "last_tune_part", part);
  tree_field_set_int(mechanic, "last_tune_choice", choice);
  return true;
}

bool mechanic_lclick_part(InvObject* mechanic, InvObject* part) {
  // Stock Mechanic EVENT_CURSOR EC_LCLICK:
  //   mode==0 szereles → GII_GETOUT_OK → inventory.addItem
  //   mode!=0 tuning → isTuneable ? Dialog/endTuningSession : flap_toggle
  if (!mechanic || !part) return false;
  InvObject* player = tree_field_get_obj(mechanic, "player");
  InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;

  if (tree_field_get_int(mechanic, "mode") != 0) {
    bool ok = false;
    if (part_is_tuneable(part)) {
      ok = mechanic_tune_part(mechanic, part, /*OK*/ 0);
    } else {
      ok = part_flap_toggle(part);
      tree_field_set_int(mechanic, "flap_count",
                         tree_field_get_int(mechanic, "flap_count") + 1);
    }
    if (ok && car)
      tree_field_set_int(car, "wake_count",
                         tree_field_get_int(car, "wake_count") + 1);
    tree_field_set_int(mechanic, "lclick_count",
                       tree_field_get_int(mechanic, "lclick_count") + 1);
    tree_field_set_obj(mechanic, "last_lclick_part", part);
    tree_field_set_int(mechanic, "last_lclick_ok", ok ? 1 : 0);
    tree_field_set_int(mechanic, "last_lclick_tune",
                       part_is_tuneable(part) ? 1 : 0);
    return ok;
  }

  constexpr int32_t kGiiGetoutOk = 41;
  if (java_util_resource_GameRef_getInfo(part, kGiiGetoutOk, 0) == -1) {
    tree_field_set_int(mechanic, "last_lclick_ok", 0);
    return false;
  }
  InvObject* inv = tree_field_get_obj(mechanic, "inventory");
  if (!inv) return false;
  if (!part_uninstall(part)) {
    tree_field_set_int(mechanic, "last_lclick_ok", 0);
    return false;
  }
  inventory_add_part_item(inv, part);
  const int32_t idx = inventory_size(inv) - 1;
  int32_t ppl = tree_field_get_int(inv, "partsPerLine");
  if (ppl <= 0) ppl = 5;
  if (idx >= 0) {
    tree_field_set_int(inv, "cline", idx / ppl);
    if (tree_field_get_obj(inv, "panels")) visual_inventory_update(inv);
  }
  if (car)
    tree_field_set_int(car, "wake_count",
                       tree_field_get_int(car, "wake_count") + 1);
  tree_field_set_int(mechanic, "lclick_count",
                     tree_field_get_int(mechanic, "lclick_count") + 1);
  tree_field_set_obj(mechanic, "last_lclick_part", part);
  tree_field_set_int(mechanic, "last_lclick_index", idx);
  tree_field_set_int(mechanic, "last_lclick_ok", 1);
  tree_field_set_int(mechanic, "last_lclick_tune", 0);
  tree_field_set_obj(mechanic, "look_part", nullptr);
  return true;
}

bool mechanic_drop_object_at(InvObject* mechanic, InvObject* part, float nx,
                             float ny) {
  // Stock: OSD dropObject on InventoryPanel button → panelDragNDrop.
  if (!mechanic || !part) return false;
  if (!mechanic_hover_at(mechanic, nx, ny)) return false;
  InvObject* panel = mechanic_actual_panel(mechanic);
  if (!panel) return false;
  InvObject* osd = tree_field_get_obj(panel, "osd");
  if (!osd) return false;
  tree_field_set_obj(osd, "dropObject", part);
  const int32_t idx = tree_field_get_int(panel, "index");
  const bool ok = inventory_panel_osd_command(panel, idx);
  tree_field_set_obj(mechanic, "drag_object", nullptr);
  tree_field_set_int(mechanic, "drop_object_count",
                     tree_field_get_int(mechanic, "drop_object_count") + 1);
  tree_field_set_int(mechanic, "last_drop_panel", idx);
  tree_field_set_int(mechanic, "last_drop_ok", ok ? 1 : 0);
  return ok;
}