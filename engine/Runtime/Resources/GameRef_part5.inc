// Fragment 5/6 of GameRef.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


bool mechanic_drag_panel_to(InvObject* mechanic, float x0, float y0, float x1,
                            float y1) {
  // Stock: drag InventoryPanel button → dropGadget → panelSwap.
  if (!mechanic) return false;
  if (!mechanic_hover_at(mechanic, x0, y0)) return false;
  InvObject* start = mechanic_actual_panel(mechanic);
  if (!start) return false;
  InvObject* button = tree_field_get_obj(start, "button");
  if (!button) return false;
  if (!mechanic_hover_at(mechanic, x1, y1)) return false;
  InvObject* dest = mechanic_actual_panel(mechanic);
  if (!dest || dest == start) return false;
  InvObject* osd = tree_field_get_obj(dest, "osd");
  if (!osd) return false;
  tree_field_set_obj(osd, "dropGadget", button);
  const int32_t idx = tree_field_get_int(dest, "index");
  const bool ok = inventory_panel_osd_command(dest, idx);
  tree_field_set_int(mechanic, "drag_swap_count",
                     tree_field_get_int(mechanic, "drag_swap_count") + 1);
  tree_field_set_int(mechanic, "last_drag_from",
                     tree_field_get_int(start, "index"));
  tree_field_set_int(mechanic, "last_drag_to", idx);
  tree_field_set_int(mechanic, "last_drag_ok", ok ? 1 : 0);
  return ok;
}

static bool mechanic_pointer_release(InvObject* mechanic) {
  InvObject* cur = mechanic_actual_panel(mechanic);
  InvObject* drag_obj = tree_field_get_obj(mechanic, "drag_object");
  if (drag_obj && cur) {
    InvObject* osd = tree_field_get_obj(cur, "osd");
    if (osd) tree_field_set_obj(osd, "dropObject", drag_obj);
    tree_field_set_obj(mechanic, "drag_object", nullptr);
    const int32_t idx = tree_field_get_int(cur, "index");
    const bool ok = inventory_panel_osd_command(cur, idx);
    tree_field_set_int(mechanic, "drop_object_count",
                       tree_field_get_int(mechanic, "drop_object_count") + 1);
    tree_field_set_int(mechanic, "last_drop_panel", idx);
    tree_field_set_int(mechanic, "last_drop_ok", ok ? 1 : 0);
    return ok;
  }
  InvObject* start = tree_field_get_obj(mechanic, "drag_start_panel");
  InvObject* button = tree_field_get_obj(mechanic, "drag_button");
  const int32_t moved = tree_field_get_int(mechanic, "drag_moved");
  if (moved && start && cur && start != cur && button) {
    InvObject* osd = tree_field_get_obj(cur, "osd");
    if (osd) tree_field_set_obj(osd, "dropGadget", button);
    const int32_t idx = tree_field_get_int(cur, "index");
    const bool ok = inventory_panel_osd_command(cur, idx);
    tree_field_set_int(mechanic, "drag_swap_count",
                       tree_field_get_int(mechanic, "drag_swap_count") + 1);
    tree_field_set_int(mechanic, "last_drag_from",
                       tree_field_get_int(start, "index"));
    tree_field_set_int(mechanic, "last_drag_to", idx);
    tree_field_set_int(mechanic, "last_drag_ok", ok ? 1 : 0);
    return ok;
  }
  if (!moved && start) {
    mechanic_set_actual_panel(mechanic, tree_field_get_int(start, "index"));
    return mechanic_click_actual(mechanic);
  }
  // Miss panels: stock EC_LCLICK on looked-at car part.
  if (!moved && !start) {
    InvObject* look = tree_field_get_obj(mechanic, "look_part");
    if (look) return mechanic_lclick_part(mechanic, look);
  }
  return false;
}

void mechanic_tick_click(InvObject* mechanic) {
  if (!mechanic) return;
  if (tree_field_get_int(mechanic, "rdrag")) return;
  input_live_poll();
  const float btn = java_io_Input_getAxis(1, kMousePhysBtn1);
  const int32_t down = btn > 0.5f ? 1 : 0;
  const int32_t was = tree_field_get_int(mechanic, "mouse_btn1");
  tree_field_set_int(mechanic, "mouse_btn1", down);
  if (was == 0 && down == 1) {
    InvObject* panel = mechanic_actual_panel(mechanic);
    tree_field_set_obj(mechanic, "drag_start_panel", panel);
    tree_field_set_obj(mechanic, "drag_button",
                       panel ? tree_field_get_obj(panel, "button") : nullptr);
    tree_field_set_int(mechanic, "drag_moved", 0);
  } else if (was == 1 && down == 1) {
    InvObject* start = tree_field_get_obj(mechanic, "drag_start_panel");
    InvObject* cur = mechanic_actual_panel(mechanic);
    if (start && cur && start != cur)
      tree_field_set_int(mechanic, "drag_moved", 1);
  } else if (was == 1 && down == 0) {
    mechanic_pointer_release(mechanic);
    tree_field_set_obj(mechanic, "drag_start_panel", nullptr);
    tree_field_set_obj(mechanic, "drag_button", nullptr);
    tree_field_set_int(mechanic, "drag_moved", 0);
  }
}

int32_t inventory_size(InvObject* inv) {
  if (!inv) return 0;
  InvObject* items = tree_field_get_obj(inv, "items");
  return items ? tree_vector_size(items) : 0;
}

namespace {

void inventory_touch(InvObject* inv) {
  if (!inv) return;
  tree_field_set_int(inv, "size", inventory_size(inv));
  tree_field_set_int(inv, "update_count",
                     tree_field_get_int(inv, "update_count") + 1);
}

InvObject* inventory_item_get_part(InvObject* item) {
  if (!item) return nullptr;
  InvObject* p = tree_field_get_obj(item, "partXXX");
  if (!p) p = tree_field_get_obj(item, "part");
  return p;
}

bool inventory_item_is_part(InvObject* item) {
  const char* hc = item ? tree_host_class(item) : nullptr;
  return hc && std::strstr(hc, "InventoryItem_Part") != nullptr;
}

}  // namespace

void inventory_add_part_item(InvObject* inv, InvObject* part) {
  if (!inv || !part) return;
  InvObject* items = tree_field_get_obj(inv, "items");
  if (!items) {
    items = tree_vector_new();
    tree_field_set_obj(inv, "items", items);
  }
  InvObject* item = tree_host_new("java.game.InventoryItem_Part");
  tree_field_set_obj(item, "inventory", inv);
  tree_field_set_obj(item, "partXXX", part);
  tree_field_set_obj(item, "part", part);
  tree_vector_add(items, item);
  inventory_touch(inv);
}

void inventory_move_to(InvObject* src, int32_t index, InvObject* dst) {
  if (!src || !dst || index < 0) return;
  InvObject* src_items = tree_field_get_obj(src, "items");
  InvObject* dst_items = tree_field_get_obj(dst, "items");
  if (!src_items) return;
  if (!dst_items) {
    dst_items = tree_vector_new();
    tree_field_set_obj(dst, "items", dst_items);
  }
  InvObject* item = tree_vector_element_at(src_items, index);
  if (!item) return;
  tree_vector_remove(src_items, item);
  tree_field_set_obj(item, "inventory", dst);
  tree_vector_add(dst_items, item);
  inventory_touch(src);
  inventory_touch(dst);
}

InvObject* inventory_ensure_player_parts(InvObject* player) {
  if (!player) return nullptr;
  InvObject* parts = tree_field_get_obj(player, "parts");
  if (!parts) {
    parts = inventory_new(player);
    tree_field_set_obj(player, "parts", parts);
  }
  if (!tree_field_get_obj(parts, "items"))
    tree_field_set_obj(parts, "items", tree_vector_new());
  // Seed COMMON parts so Mechanic filters/scroll have work to do.
  // 1 engine + 5 body → 6 items (2 VisualInventory lines @ 5/line).
  // IDs match Baiern chassis attach locals (0x52 block, 0x10F F bumper).
  if (inventory_size(parts) == 0 &&
      !tree_field_get_int(player, "parts_seeded")) {
    InvObject* eng = tree_host_new("java.game.parts.enginepart.Block");
    java_util_resource_ResourceRef_set(eng, 0x00070052);
    tree_field_set_int(eng, "carCategory", 0);  // Part.COMMON
    tree_field_set_int(eng, "part_category", 1);
    tree_field_set_obj(
        eng, "cfg_path",
        string_new(
            "parts/engines/Baiern_Emer/scripts/Baiern_Devils_6SFi_3_6_block.cfg"));
    inventory_add_part_item(parts, eng);
    for (int i = 0; i < 5; ++i) {
      InvObject* spare =
          tree_host_new("java.game.parts.bodypart.Bumper");
      java_util_resource_ResourceRef_set(spare, 0x0000010F);
      tree_field_set_int(spare, "carCategory", 0);
      tree_field_set_int(spare, "part_category", 2);
      tree_field_set_obj(
          spare, "cfg_path",
          string_new("cars/racers/Baiern_data/scripts/F_bumper.cfg"));
      inventory_add_part_item(parts, spare);
    }
    tree_field_set_int(player, "parts_seeded", 1);
  }
  return parts;
}

InvObject* garage_ensure_mechanic(InvObject* garage) {
  if (!garage) return nullptr;
  InvObject* mech = tree_field_get_obj(garage, "mechanic");
  InvObject* player = tree_field_get_obj(garage, "player");
  if (!player) player = game_logic_player();
  inventory_ensure_player_parts(player);
  // Mechanic.PARTS_VP_* — stock VisualInventory strip layout.
  constexpr float kVpLeft = 0.226f;
  constexpr float kVpTop = 0.815f;
  constexpr float kVpWidth = 0.696f;
  constexpr float kVpHeight = 0.178f;
  if (!mech) {
    mech = tree_host_new("java.game.Mechanic");
    tree_field_set_obj(mech, "player", player);
    tree_field_set_obj(mech, "osd", tree_field_get_obj(garage, "osd"));
    InvObject* inv =
        visual_inventory_new(player, kVpLeft, kVpTop, kVpWidth, kVpHeight);
    tree_field_set_obj(mech, "inventory", inv);
    tree_field_set_obj(garage, "mechanic", mech);
  } else if (!tree_field_get_obj(mech, "inventory")) {
    tree_field_set_obj(
        mech, "inventory",
        visual_inventory_new(player, kVpLeft, kVpTop, kVpWidth, kVpHeight));
  } else {
    InvObject* inv = tree_field_get_obj(mech, "inventory");
    if (inv && !tree_field_get_obj(inv, "panels"))
      visual_inventory_init(inv, kVpLeft, kVpTop, kVpWidth, kVpHeight);
  }
  if (!tree_field_get_obj(mech, "player"))
    tree_field_set_obj(mech, "player", player);
  mechanic_ensure_chrome(mech);
  if (InvObject* cam = garage_ensure_camera(garage))
    tree_field_set_obj(mech, "camera", cam);
  return mech;
}

InvObject* garage_ensure_painter(InvObject* garage) {
  // Phase 2.156/2.157 — stock Garage.painter + PaintCans palette.
  if (!garage) return nullptr;
  InvObject* painter = tree_field_get_obj(garage, "painter");
  InvObject* player = tree_field_get_obj(garage, "player");
  if (!player) player = game_logic_player();
  if (!painter) {
    painter = tree_host_new("java.game.Painter");
    tree_field_set_obj(garage, "painter", painter);
  }
  if (player) tree_field_set_obj(painter, "player", player);
  if (InvObject* osd = tree_field_get_obj(garage, "osd"))
    tree_field_set_obj(painter, "osd", osd);
  if (tree_field_get_int(painter, "mode") == 0)
    tree_field_set_int(painter, "mode", 2);  // MODE_PAINTPART
  if (tree_field_get_int(painter, "lastPaintMode") == 0)
    tree_field_set_int(painter, "lastPaintMode", 2);

  InvObject* cans = tree_field_get_obj(painter, "paintCans");
  if (!cans) {
    cans = tree_host_new("java.game.PaintCans");
    tree_field_set_obj(painter, "paintCans", cans);
  }
  InvObject* items = tree_field_get_obj(cans, "items");
  if (!items) {
    items = tree_vector_new();
    tree_field_set_obj(cans, "items", items);
  }
  if (tree_vector_size(items) < 1) {
    // Stock first rows (greys + reds + a few accents) — enough for UX/smoke.
    static const int32_t kPalette[] = {
        0xF5F5F5, 0xB9B9B9, 0x7F7F7F, 0x505050, 0x1D1D1D, 0xDC191A,
        0xAE1515, 0x800F0F, 0x530A0A, 0x250505, 0xF58E08, 0x0F3EED};
    for (int32_t c : kPalette) {
      InvObject* can = tree_host_new("java.game.PaintCan");
      tree_field_set_int(can, "color", c);
      tree_field_set_float(can, "capacity", 1.f);
      tree_vector_add(items, can);
    }
    tree_field_set_int(cans, "item_count", tree_vector_size(items));
    tree_field_set_int(cans, "lastCanId", 0);
    tree_field_set_int(cans, "paintColor", kPalette[0]);
  }
  // Sync painter.paintColor from cans (stock |0xFF000000 on apply).
  if (tree_field_get_int(painter, "paintColor") == 0) {
    int32_t pc = tree_field_get_int(cans, "paintColor");
    if (pc == 0) pc = 0xF5F5F5;
    tree_field_set_int(painter, "paintColor", pc | static_cast<int32_t>(0xFF000000));
  }
  return painter;
}

void garage_painter_hide(InvObject* garage) {
  if (!garage) return;
  if (InvObject* p = tree_field_get_obj(garage, "painter")) {
    tree_field_set_int(p, "shown", 0);
    tree_field_set_int(p, "visible", 0);
  }
  tree_field_set_int(garage, "painter_shown", 0);
}

void garage_painter_show(InvObject* garage) {
  // Stock changeMode(MODE_PAINT): painter.show(); hide mechanic if leaving
  // szerel/tune.
  if (!garage) return;
  InvObject* painter = garage_ensure_painter(garage);
  if (!painter) return;
  if (InvObject* mech = tree_field_get_obj(garage, "mechanic")) {
    if (InvObject* inv = tree_field_get_obj(mech, "inventory"))
      visual_inventory_hide(inv);
    tree_field_set_int(mech, "shown", 0);
  }
  tree_field_set_int(painter, "shown", 1);
  tree_field_set_int(painter, "visible", 1);
  tree_field_set_int(painter, "show_count",
                     tree_field_get_int(painter, "show_count") + 1);
  tree_field_set_int(garage, "painter_shown", 1);
}

bool garage_painter_select_can(InvObject* garage, int32_t index) {
  // Stock PaintCans: lastCanId=index; paintColor=getCanbyIndex(index).color
  if (!garage || index < 0) return false;
  InvObject* painter = garage_ensure_painter(garage);
  if (!painter) return false;
  InvObject* cans = tree_field_get_obj(painter, "paintCans");
  InvObject* items = cans ? tree_field_get_obj(cans, "items") : nullptr;
  const int32_t n = items ? tree_vector_size(items) : 0;
  if (index >= n) return false;
  InvObject* can = tree_vector_element_at(items, index);
  if (!can) return false;
  const int32_t rgb = tree_field_get_int(can, "color");
  tree_field_set_int(cans, "lastCanId", index);
  tree_field_set_int(cans, "paintColor", rgb);
  tree_field_set_int(painter, "paintColor",
                     rgb | static_cast<int32_t>(0xFF000000));
  tree_field_set_int(garage, "last_can_id", index);
  tree_field_set_int(garage, "can_select_count",
                     tree_field_get_int(garage, "can_select_count") + 1);
  return true;
}

bool garage_paint_car(InvObject* garage, int32_t color) {
  // Host mirror of Painter MODE_PAINTPART click → paintPart(car, color).
  if (!garage) return false;
  InvObject* painter = garage_ensure_painter(garage);
  InvObject* player = tree_field_get_obj(garage, "player");
  if (!player) player = game_logic_player();
  InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
  if (!painter || !car) return false;
  int32_t c = color;
  if (c == 0) {
    InvObject* cans = tree_field_get_obj(painter, "paintCans");
    int32_t rgb = cans ? tree_field_get_int(cans, "paintColor") : 0;
    if (rgb == 0) rgb = tree_field_get_int(painter, "paintColor");
    c = rgb | static_cast<int32_t>(0xFF000000);
  }
  java_game_Painter_paintPart(painter, car, c);
  tree_field_set_int(painter, "paintColor", c);
  tree_field_set_int(garage, "last_paint_color", c);
  tree_field_set_int(garage, "paint_car_count",
                     tree_field_get_int(garage, "paint_car_count") + 1);
  return tree_field_get_int(car, "part_texture") == c;
}

namespace {

InvObject* mechanic_add_chrome_button(InvObject* osd, float x, float y,
                                      int32_t cmd, const char* tip) {
  if (!osd) return nullptr;
  InvObject* btns = tree_field_get_obj(osd, "buttons");
  if (!btns) {
    btns = tree_vector_new();
    tree_field_set_obj(osd, "buttons", btns);
  }
  InvObject* style = tree_host_new("java.render.osd.Style");
  tree_field_set_float(style, "rWidth", 0.10f);
  tree_field_set_float(style, "rHeight", 0.10f);
  tree_field_set_float(style, "width", 0.10f);
  tree_field_set_float(style, "height", 0.10f);
  InvObject* btn = tree_host_new("java.render.osd.Button");
  tree_field_set_obj(btn, "style", style);
  tree_field_set_float(btn, "x", x);
  tree_field_set_float(btn, "y", y);
  tree_field_set_int(btn, "command", cmd);
  tree_field_set_obj(btn, "osd", osd);
  if (tip) tree_field_set_obj(btn, "tooltip", string_new(tip));
  tree_vector_add(btns, btn);
  tree_field_set_int(osd, "button_count",
                     tree_field_get_int(osd, "button_count") + 1);
  return btn;
}

void mechanic_sync_line_text(InvObject* mechanic) {
  if (!mechanic) return;
  InvObject* inv = tree_field_get_obj(mechanic, "inventory");
  const int32_t line =
      inv ? tree_field_get_int(inv, "cline") + 1 : 1;
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d", line);
  InvObject* txt = tree_field_get_obj(mechanic, "invLineTxt");
  if (!txt) {
    txt = tree_host_new("java.render.Text");
    tree_field_set_obj(mechanic, "invLineTxt", txt);
  }
  tree_field_set_obj(txt, "text", string_new(buf));
  tree_field_set_int(mechanic, "inv_line", line);
}

}  // namespace

void mechanic_ensure_chrome(InvObject* mechanic) {
  if (!mechanic) return;
  if (tree_field_get_int(mechanic, "chrome_ready") == 1) return;
  InvObject* osd = tree_field_get_obj(mechanic, "osd");
  if (!osd) {
    osd = tree_host_new("java.render.Osd");
    tree_field_set_obj(osd, "groups", tree_vector_new());
    tree_field_set_obj(osd, "hotkey", tree_vector_new());
    tree_field_set_obj(osd, "rectangles", tree_vector_new());
    tree_field_set_obj(osd, "buttons", tree_vector_new());
    tree_field_set_int(osd, "init", 1);
    tree_field_set_obj(mechanic, "osd", osd);
  }
  tree_field_set_obj(osd, "globalHandler", mechanic);

  // Stock: scroll arrows + filter menu icons.
  constexpr int32_t CMD_SCROLL_UP = 0;
  constexpr int32_t CMD_SCROLL_DOWN = 1;
  constexpr int32_t CMD_ENGINE = 119;
  constexpr int32_t CMD_BODY = 120;
  constexpr int32_t CMD_RUNNING_GEAR = 121;
  mechanic_add_chrome_button(osd, 0.94f, 0.68f, CMD_SCROLL_UP, "Scroll up");
  mechanic_add_chrome_button(osd, 0.94f, 0.92f, CMD_SCROLL_DOWN,
                             "Scroll down");
  mechanic_add_chrome_button(osd, -0.98f, 0.88f, CMD_ENGINE, "Filter ENGINE");
  mechanic_add_chrome_button(osd, -0.86f, 0.88f, CMD_BODY, "Filter BODY");
  mechanic_add_chrome_button(osd, -0.74f, 0.88f, CMD_RUNNING_GEAR,
                             "Filter RGEAR");

  InvObject* line = tree_host_new("java.render.Text");
  tree_field_set_obj(line, "text", string_new("1"));
  tree_field_set_obj(mechanic, "invLineTxt", line);
  tree_field_set_int(mechanic, "inv_line", 1);
  tree_field_set_int(mechanic, "filterEngine", 0);
  tree_field_set_int(mechanic, "filterBody", 0);
  tree_field_set_int(mechanic, "filterRGear", 0);
  tree_field_set_int(mechanic, "chrome_ready", 1);
  tree_field_set_int(mechanic, "chrome_buttons", 5);
}

int32_t mechanic_chrome_button_count(InvObject* mechanic) {
  if (!mechanic) return 0;
  return tree_field_get_int(mechanic, "chrome_buttons");
}

void mechanic_osd_command(InvObject* mechanic, int32_t cmd) {
  if (!mechanic) return;
  constexpr int32_t CMD_SCROLL_UP = 0;
  constexpr int32_t CMD_SCROLL_DOWN = 1;
  constexpr int32_t CMD_ENGINE = 119;
  constexpr int32_t CMD_BODY = 120;
  constexpr int32_t CMD_RUNNING_GEAR = 121;

  InvObject* inv = tree_field_get_obj(mechanic, "inventory");
  tree_field_set_int(mechanic, "last_osd_cmd", cmd);

  if (cmd == CMD_SCROLL_UP) {
    visual_inventory_scroll_up(inv);
    mechanic_sync_line_text(mechanic);
    return;
  }
  if (cmd == CMD_SCROLL_DOWN) {
    visual_inventory_scroll_down(inv);
    mechanic_sync_line_text(mechanic);
    return;
  }

  auto toggle = [](int32_t v) -> int32_t { return 2 - v; };
  if (cmd == CMD_ENGINE) {
    const int32_t v = toggle(tree_field_get_int(mechanic, "filterEngine"));
    tree_field_set_int(mechanic, "filterEngine", v);
  } else if (cmd == CMD_BODY) {
    const int32_t v = toggle(tree_field_get_int(mechanic, "filterBody"));
    tree_field_set_int(mechanic, "filterBody", v);
  } else if (cmd == CMD_RUNNING_GEAR) {
    const int32_t v = toggle(tree_field_get_int(mechanic, "filterRGear"));
    tree_field_set_int(mechanic, "filterRGear", v);
  } else {
    return;
  }

  mechanic_filter_inventory(mechanic,
                            tree_field_get_int(mechanic, "filterEngine"),
                            tree_field_get_int(mechanic, "filterBody"),
                            tree_field_get_int(mechanic, "filterRGear"));
  // Reset scroll to first page after filter reshuffle.
  if (inv) {
    tree_field_set_int(inv, "cline", 0);
    visual_inventory_update(inv);
  }
  mechanic_sync_line_text(mechanic);
}

void mechanic_flush_inventory(InvObject* mechanic) {
  if (!mechanic) return;
  InvObject* inv = tree_field_get_obj(mechanic, "inventory");
  InvObject* player = tree_field_get_obj(mechanic, "player");
  if (!player) player = game_logic_player();
  InvObject* parts = inventory_ensure_player_parts(player);
  if (!inv || !parts) return;
  while (inventory_size(inv) > 0) inventory_move_to(inv, 0, parts);
}

void mechanic_filter_inventory(InvObject* mechanic, int32_t filter_engine,
                               int32_t filter_body, int32_t filter_rgear) {
  if (!mechanic) return;
  InvObject* player = tree_field_get_obj(mechanic, "player");
  if (!player) player = game_logic_player();
  InvObject* parts = inventory_ensure_player_parts(player);
  InvObject* inv = tree_field_get_obj(mechanic, "inventory");
  if (!inv) {
    inv = inventory_new(player);
    tree_field_set_obj(mechanic, "inventory", inv);
  }
  constexpr int32_t kGiiType = 6;
  constexpr int32_t kGiiPartCategory = 55;
  InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
  const int32_t car_id =
      car ? (java_util_resource_GameRef_getInfo(car, kGiiType, 0) >> 16) : 0;

  auto part_ok_for_car = [car_id](InvObject* part) -> bool {
    if (!part) return false;
    if (tree_field_get_int(part, "carCategory") == 0) return true;  // COMMON
    const int32_t pid =
        java_util_resource_GameRef_getInfo(part, /*GII_TYPE*/ 6, 0) >> 16;
    return pid == car_id;
  };

  // parts → inventory (accepted by current filters).
  for (int32_t i = 0; i < inventory_size(parts);) {
    InvObject* items = tree_field_get_obj(parts, "items");
    InvObject* t = tree_vector_element_at(items, i);
    bool move = false;
    if (!inventory_item_is_part(t)) {
      move = true;
    } else {
      InvObject* part = inventory_item_get_part(t);
      if (part_ok_for_car(part)) {
        const int32_t cat =
            java_util_resource_GameRef_getInfo(part, kGiiPartCategory, 0);
        if (cat == 1) {
          if (!filter_engine) move = true;
        } else if (cat == 2) {
          if (!filter_body) move = true;
        } else if (cat == 3) {
          if (!filter_rgear) move = true;
        } else if (cat == 0) {
          move = true;
        }
      }
    }
    if (move)
      inventory_move_to(parts, i, inv);
    else
      ++i;
  }

  // inventory → parts (rejected by filters).
  for (int32_t i = 0; i < inventory_size(inv);) {
    InvObject* items = tree_field_get_obj(inv, "items");
    InvObject* t = tree_vector_element_at(items, i);
    bool back = false;
    if (inventory_item_is_part(t)) {
      InvObject* part = inventory_item_get_part(t);
      if (!part_ok_for_car(part)) {
        back = true;
      } else {
        const int32_t cat =
            java_util_resource_GameRef_getInfo(part, kGiiPartCategory, 0);
        if (cat == 1) {
          if (filter_engine) back = true;
        } else if (cat == 2) {
          if (filter_body) back = true;
        } else if (cat == 3) {
          if (filter_rgear) back = true;
        }
      }
    }
    if (back)
      inventory_move_to(inv, i, parts);
    else
      ++i;
  }
  inventory_touch(inv);
  visual_inventory_update(inv);
}

InvObject* garage_osd_command(InvObject* garage, int32_t cmd) {
  if (!garage) return nullptr;
  constexpr int32_t CMD_HITTHESTREET = 109;
  constexpr int32_t CMD_TESTTRACK = 110;
  constexpr int32_t CMD_CARLOT = 111;
  constexpr int32_t CMD_BUYCARS = 112;
  constexpr int32_t CMD_CATALOG = 113;
  constexpr int32_t CMD_CLUBINFO = 114;
  constexpr int32_t CMD_CARINFO = 115;
  constexpr int32_t CMD_TIME = 116;
  constexpr int32_t CMD_MECHANIC = 117;
  constexpr int32_t CMD_PAINT = 118;
  constexpr int32_t CMD_ESCAPE = 119;
  constexpr int32_t CMD_BUYCARSUSED = 122;
  constexpr int32_t CMD_TEST = 123;
  constexpr int32_t CMD_TUNE = 124;
  constexpr int32_t CMD_NONE = 100;
  constexpr int32_t MODE_NONE = 0;
  constexpr int32_t MODE_SZEREL = 1;
  constexpr int32_t MODE_PAINT = 2;
  constexpr int32_t MODE_TEST = 3;
  constexpr int32_t MODE_TUNE = 4;

  tree_field_set_int(garage, "last_cmd", cmd);
  tree_field_set_obj(garage, "last_warning", nullptr);

  auto set_mode = [garage](int32_t mode) {
    tree_field_set_int(garage, "prevMode", tree_field_get_int(garage, "mode"));
    tree_field_set_int(garage, "mode", mode);
    tree_field_set_int(garage, "mode_changes",
                       tree_field_get_int(garage, "mode_changes") + 1);
  };

  if (cmd == CMD_TIME) {
    game_logic_spend_time(3600.f);
    InvObject* player = tree_field_get_obj(garage, "player");
    if (!player) player = game_logic_player();
    if (player) {
      const float p = tree_field_get_float(player, "prestige");
      tree_field_set_float(player, "prestige", p - (0.00333f / 24.f));
    }
    tree_field_set_float(garage, "time_after_cmd", game_logic_time());
    return garage;
  }

  if (cmd == CMD_MECHANIC) {
    garage_painter_hide(garage);
    set_mode(MODE_SZEREL);
    InvObject* mech = garage_ensure_mechanic(garage);
    if (mech) tree_field_set_int(mech, "mode", 0);  // szereles
    mechanic_filter_inventory(mech, 0, 0, 0);
    if (mech) {
      InvObject* inv = tree_field_get_obj(mech, "inventory");
      visual_inventory_show(inv);
      tree_field_set_int(mech, "shown", 1);
      mechanic_ensure_chrome(mech);
    }
    return garage;
  }
  if (cmd == CMD_PAINT) {
    set_mode(MODE_PAINT);
    garage_painter_show(garage);
    return garage;
  }
  if (cmd == CMD_TEST) {
    garage_painter_hide(garage);
    set_mode(MODE_TEST);
    return garage;
  }
  if (cmd == CMD_TUNE) {
    // Stock changeMode(MODE_TUNE): mechanic.show(); mechanic.mode=1.
    garage_painter_hide(garage);
    set_mode(MODE_TUNE);
    InvObject* mech = garage_ensure_mechanic(garage);
    if (mech) {
      tree_field_set_int(mech, "mode", 1);
      InvObject* inv = tree_field_get_obj(mech, "inventory");
      visual_inventory_show(inv);
      tree_field_set_int(mech, "shown", 1);
      mechanic_ensure_chrome(mech);
    }
    return garage;
  }
  if (cmd == CMD_ESCAPE || cmd == CMD_NONE) {
    garage_painter_hide(garage);
    set_mode(MODE_NONE);
    return garage;
  }

  if (cmd == CMD_CATALOG) {
    return catalog_enter(garage);
  }

  if (cmd == CMD_CARLOT) {
    InvObject* player = tree_field_get_obj(garage, "player");
    if (!player) player = game_logic_player();
    InvObject* carlot = player ? tree_field_get_obj(player, "carlot") : nullptr;
    if (!carlot) {
      tree_field_set_obj(garage, "last_warning", string_new("No car lot."));
      return nullptr;
    }
    return carlot_enter(carlot, garage);
  }

  if (cmd == CMD_CLUBINFO) {
    return clubinfo_enter(garage);
  }

  if (cmd == CMD_CARINFO) {
    InvObject* player = tree_field_get_obj(garage, "player");
    if (!player) player = game_logic_player();
    InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
    if (!car) {
      tree_field_set_obj(
          garage, "last_warning",
          string_new(
              "You need a car to do this! \n Buy a car or get one from the car lot."));
      return nullptr;
    }
    return carinfo_enter(garage, car);
  }

  if (cmd == CMD_BUYCARS || cmd == CMD_BUYCARSUSED) {
    const float hour = game_logic_time() / 3600.f;
    if (!(hour > 7.f && hour < 17.f)) {
      tree_field_set_obj(
          garage, "last_warning",
          string_new(
              "The car dealer is closed now! \n Opening hours: 7am to 5pm"));
      return nullptr;
    }
    game_logic_spend_time(1800.f);  // travel to dealer
    return carmarket_enter(garage, cmd == CMD_BUYCARSUSED ? 1 : 0);
  }

  if (cmd == CMD_TESTTRACK) {
    InvObject* player = tree_field_get_obj(garage, "player");
    if (!player) player = game_logic_player();
    InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
    const char* problem = vehicle_is_driveable(car);
    if (problem) {
      tree_field_set_obj(garage, "last_warning", string_new(problem));
      return nullptr;
    }
    game_logic_spend_time(1800.f);  // travel to track
    return testtrack_enter(garage);
  }

  if (cmd == CMD_HITTHESTREET) {
    InvObject* player = tree_field_get_obj(garage, "player");
    if (!player) player = game_logic_player();
    InvObject* car = player ? tree_field_get_obj(player, "car") : nullptr;
    const char* problem = vehicle_is_driveable(car);
    if (problem) {
      tree_field_set_obj(garage, "last_warning", string_new(problem));
      return nullptr;
    }
    InvObject* city = tree_host_new("java.game.Valocity");
    tree_field_set_obj(city, "parentState", garage);
    tree_field_set_obj(city, "player", player);
    tree_field_set_obj(city, "car", car);
    valocity_prepare(city);
    // Stock Garage: changeActiveSection(new Valocity()) — enter does the rest.
    return game_logic_change_active_section(city);
  }

  return nullptr;
}

InvObject* game_state_return_to_garage(InvObject* state) {
  return section_return_to_garage(state);
}

// java_game_Navigator_updateNavigator — body in Cars.cpp (PE @ 0x00482D30).

namespace {

InvObject* traffic_type_ref(const char* label, int32_t local_id) {
  InvObject* type = gameref_new();
  int32_t id = local_id;
  if (const RpakPack* tp = rpak_find_by_name("traffic.rpk"))
    id = rpak_make_id(tp->pack_id, local_id & 0xFFFF);
  java_util_resource_ResourceRef_set(type, id);
  if (label) tree_field_set_obj(type, "vehicleName", string_new(label));
  return type;
}

InvObject* police_type_ref() {
  InvObject* type = gameref_new();
  int32_t id = 0x6;
  if (!rpak_find_by_name("Police.rpk") && !rpak_find_by_name("police.rpk"))
    java_lang_System_openLib(string_new("cars/misc/Police.rpk"));
  if (const RpakPack* pp = rpak_find_by_name("Police.rpk"))
    id = rpak_make_id(pp->pack_id, 0x6);
  else if (const RpakPack* pp = rpak_find_by_name("police.rpk"))
    id = rpak_make_id(pp->pack_id, 0x6);
  java_util_resource_ResourceRef_set(type, id);
  tree_field_set_obj(type, "vehicleName", string_new("Police"));
  return type;
}

void valocity_spawn_traffic(InvObject* city, InvObject* map) {
  if (!city || !map) return;
  // Config.trafficDensity / pedestrianDensity defaults (1.0 / 0.0).
  constexpr float kTrafficDensity = 1.f;
  constexpr float kPedConfig = 0.f;

  const int daytime = tree_field_get_int(city, "daytime");
  const int32_t mode = game_logic_game_mode();
  constexpr int32_t kGmDemo = 5;

  auto add_n = [&](const char* name, int32_t n, float lb, float le, float wb) {
    const int32_t scaled = static_cast<int32_t>(n * kTrafficDensity);
    java_util_resource_GroundRef_addTrafficN(map, traffic_type_ref(name, 0x6),
                                             scaled, lb, le, wb);
  };

  if (daytime) {
    add_n("Taxi", 80, 2, 5, 2);
    add_n("Ambulance", 20, 2, 5, 2);
    add_n("FireEngine", 12, 2, 5, 2);
    add_n("Coach", 30, 2, 10, 2);
    add_n("Schoolbus", 55, 2, 10, 2);
    add_n("ArmoredVan", 20, 2, 5, 2);
    add_n("Wagon", 200, 2, 5, 2);
    add_n("Erbilac", 200, 2, 5, 2);
    add_n("CivilVan", 200, 2, 5, 2);
    if (mode == kGmCareer)
      java_util_resource_GroundRef_addTrafficN(
          map, police_type_ref(),
          static_cast<int32_t>(30 * kTrafficDensity), 2, 5, 2);
    else if (mode == kGmDemo)
      java_util_resource_GroundRef_addTrafficN(
          map, police_type_ref(),
          static_cast<int32_t>(50 * kTrafficDensity), 2, 5, 2);
    java_util_resource_GroundRef_setPedestrianDensityN(map, 0.003f * kPedConfig);
    tree_field_set_float(city, "pedestrianDensity", 0.003f);
  } else {
    add_n("Erbilac", 20, 2, 5, 2);
    add_n("CivilVan", 10, 2, 5, 2);
    add_n("Taxi", 150, 2, 5, 2);
    if (mode == kGmCareer)
      java_util_resource_GroundRef_addTrafficN(
          map, police_type_ref(),
          static_cast<int32_t>(3 * kTrafficDensity), 2, 5, 2);
    else if (mode == kGmDemo)
      java_util_resource_GroundRef_addTrafficN(
          map, police_type_ref(),
          static_cast<int32_t>(20 * kTrafficDensity), 2, 5, 2);
    java_util_resource_GroundRef_setPedestrianDensityN(map, 0.0005f * kPedConfig);
    tree_field_set_float(city, "pedestrianDensity", 0.0005f);
  }

  // Valocity pedestrian type list (humans local ids).
  const int32_t ped_locals[] = {0x57, 0x58, 0x59, 0x5A, 0x5B, 0x22};
  if (!rpak_find_by_name("humans.rpk"))
    java_lang_System_openLib(string_new("humans/humans.rpk"));
  for (int32_t loc : ped_locals) {
    InvObject* g = gameref_new();
    int32_t id = loc;
    if (const RpakPack* hp = rpak_find_by_name("humans.rpk"))
      id = rpak_make_id(hp->pack_id, loc);
    java_util_resource_ResourceRef_set(g, id);
    java_util_resource_GroundRef_addPedestrianType(map, g);
  }

  tree_field_set_int(city, "traffic_count",
                     tree_field_get_int(map, "traffic_count"));
  tree_field_set_int(city, "pedestrian_types",
                     tree_field_get_int(map, "pedestrian_types"));
  std::printf("  traffic path_spawn=%d roads=%d count=%d\n",
              tree_field_get_int(map, "path_spawns"), physics_road_count(),
              tree_field_get_int(city, "traffic_count"));
  // PE addTrafficP @ 0x00484420: City.sleepPoliceScoutQuick GRT_POLICECAR.
  // Far Vector3 — spawn must snap to a road, not setPos(want).
  {
    const float want_x = 12345.f, want_y = 99.f, want_z = 67890.f;
    const int32_t np = java_util_resource_GroundRef_addTrafficP(
        map, police_type_ref(), vec3_new(want_x, want_y, want_z), 1, 2.f, 5.f,
        2.f);
    const float px = tree_field_get_float(map, "traffic_p_x");
    const float pz = tree_field_get_float(map, "traffic_p_z");
    const float pdx = std::fabs(px - want_x) + std::fabs(pz - want_z);
    const int32_t p_ok =
        (np == 1 && tree_field_get_int(map, "traffic_p_ok") == 1 && pdx > 1.f)
            ? 1
            : 0;
    tree_field_set_int(map, "traffic_p_smoke", p_ok);
    tree_field_set_int(city, "traffic_p_smoke", p_ok);
    std::printf("  traffic addTrafficP ok=%d n=%d dx=%.1f\n", p_ok, np, pdx);
  }
  // PE setTrafficCarBehaviour @ 0x00487EC0: write mode on the traffic instance
  // (+0x160) only if live (+0x138). Bot.setTrafficBehaviour(TC_PASSIVE=2).
  {
    InvObject* tinst = gameref_new();
    const int32_t tid =
        java_util_resource_GroundRef_addTrafficCar(map, tinst, nullptr);
    java_util_resource_GroundRef_setTrafficCarBehaviour(map, tid, 2);
    java_util_resource_GroundRef_setTrafficCarBehaviour(map, 0, 1);
    const int32_t bh = tree_field_get_int(tinst, "traffic_behaviour");
    const int32_t last = tree_field_get_int(map, "traffic_behaviour_last");
    const int32_t bh_ok = (tid != 0 && bh == 2 && last == 2) ? 1 : 0;
    tree_field_set_int(map, "traffic_bh_ok", bh_ok);
    tree_field_set_int(city, "traffic_bh_ok", bh_ok);
    std::printf("  traffic setBehaviour ok=%d id=%d bh=%d last=%d\n", bh_ok, tid,
                bh, last);
  }
  // PE haltTrafficCross @ 0x00484B90: City.startRace(raceStart, 15.0) duration.
  // Evict cars whose nearest junction is the halted cross (~100 m).
  {
    InvObject* tinst = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      GroundTrafficState& g = ground(map);
      if (!g.traffic_cars.empty()) tinst = g.traffic_cars.back();
    }
    InvObject* hp = tinst ? java_util_resource_GameRef_getPos(tinst) : nullptr;
    java_util_resource_GroundRef_haltTrafficCross(map, hp, 15.f);
    const int32_t hc = tree_field_get_int(map, "halt_cleared");
    const int32_t hn = tree_field_get_int(map, "halt_crosses");
    const int32_t halt_ok = (hn >= 1 && hc >= 1) ? 1 : 0;
    tree_field_set_int(map, "halt_smoke", halt_ok);
    tree_field_set_int(city, "halt_smoke", halt_ok);
    std::printf("  traffic haltCross ok=%d crosses=%d cleared=%d\n", halt_ok, hn,
                hc);
  }
  // PE haltTrafficPath @ 0x004835E0: City.prepareNightRace pS..pF.
  // GroundMap_findRoute then haltCrossTraffic(0.001) per waypoint.
  {
    InvObject* tinst = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      GroundTrafficState& g = ground(map);
      if (!g.traffic_cars.empty()) tinst = g.traffic_cars.front();
    }
    InvObject* hp = tinst ? java_util_resource_GameRef_getPos(tinst) : nullptr;
    float px = 0.f, py = 0.f, pz = 0.f;
    if (hp) vec3_get(hp, &px, &py, &pz);
    InvObject* farp = physics_road_nearest_cross(px, py, pz, 100.f);
    java_util_resource_GroundRef_haltTrafficPath(map, hp, farp);
    const int32_t pn = tree_field_get_int(map, "halt_paths");
    const int32_t pc = tree_field_get_int(map, "halt_path_cleared");
    const int32_t pxn = tree_field_get_int(map, "halt_path_crosses");
    const int32_t occ = physics_road_occupied_count();
    const int32_t empty = physics_road_count() - occ;
    int32_t spawned = 0;
    for (int32_t i = 0; i < 16; ++i) {
      float sx = 0.f, sy = 0.f, sz = 0.f, yaw = 0.f;
      if (physics_road_random_spawn(&sx, &sy, &sz, &yaw)) ++spawned;
    }
    const int32_t path_ok =
        (pn >= 1 && pc >= 1 && pxn >= 1 && occ >= 1 && empty >= 1 &&
         spawned == 16)
            ? 1
            : 0;
    tree_field_set_int(map, "halt_path_smoke", path_ok);
    tree_field_set_int(city, "halt_path_smoke", path_ok);
    std::printf(
        "  traffic haltPath ok=%d paths=%d crosses=%d cleared=%d occ=%d "
        "empty=%d spawn=%d\n",
        path_ok, pn, pxn, pc, occ, empty, spawned);
  }
  // PE remTrafficCar @ 0x00484A90: Bot.leaveTraffic. Traffic_destroy pool,
  // not GameRef.destroy. id==0 / unknown id = no-op (no count bump).
  // W30D: rem → deactivate → Traffic_pool_sort_compact drops dead slot.
  // W31D: sort_compact also soft-scores/ticks (rem budget=0 → keep live).
  // W32D: Engine_tickPhysTraffic @ 0x463AC0 wires a2=1 via
  // valocity_simulate (not rem).
  {
    const int32_t before = tree_field_get_int(map, "traffic_count");
    const int32_t pool_before = tree_field_get_int(map, "traffic_pool_n");
    InvObject* tinst = gameref_new();
    const int32_t tid =
        java_util_resource_GroundRef_addTrafficCar(map, tinst, nullptr);
    // PE notifyTrafficCar @ 0x00484AF0: Valocity.enter notify(id,1) after
    // addTrafficCar dummycar — write +0xDD, ret MOVSX (must stay live=1).
    const int32_t nret =
        java_util_resource_GroundRef_notifyTrafficCar(map, tid, 1);
    const int32_t nflag = tree_field_get_int(tinst, "traffic_flag_221");
    const int32_t nact = tree_field_get_int(tinst, "traffic_active");
    const int32_t notify_ok =
        (tid != 0 && nret == 1 && nflag == 1 && nact == 1) ? 1 : 0;
    tree_field_set_int(map, "notify_car_smoke", notify_ok);
    tree_field_set_int(city, "notify_car_smoke", notify_ok);
    std::printf("  traffic notifyTrafficCar ok=%d id=%d ret=%d flag=%d act=%d\n",
                notify_ok, tid, nret, nflag, nact);
    const int32_t pool_mid = tree_field_get_int(map, "traffic_pool_n");
    java_util_resource_GroundRef_setTrafficCarBehaviour(map, tid, 2);
    java_util_resource_GroundRef_remTrafficCar(map, 0);
    const int32_t c0 = tree_field_get_int(map, "traffic_count");
    java_util_resource_GroundRef_remTrafficCar(map, tid);
    const int32_t c1 = tree_field_get_int(map, "traffic_count");
    const int32_t pool_after = tree_field_get_int(map, "traffic_pool_n");
    java_util_resource_GroundRef_remTrafficCar(map, tid);
    const int32_t c2 = tree_field_get_int(map, "traffic_count");
    java_util_resource_GroundRef_setTrafficCarBehaviour(map, tid, 1);
    const int32_t bh = tree_field_get_int(tinst, "traffic_behaviour");
    const int32_t flag = tree_field_get_int(tinst, "traffic_flag_221");
    const int32_t rem_ok =
        (tid != 0 && c0 == before + 1 && c1 == before && c2 == before &&
         bh == 2 && flag == 1 && pool_mid == pool_before + 1 &&
         pool_after == pool_before)
            ? 1
            : 0;
    tree_field_set_int(map, "rem_car_smoke", rem_ok);
    tree_field_set_int(city, "rem_car_smoke", rem_ok);
    std::printf(
        "  traffic remTrafficCar ok=%d id=%d c=%d/%d/%d bh=%d live=%d "
        "pool=%d/%d/%d\n",
        rem_ok, tid, c0, c1, c2, bh, flag, pool_before, pool_mid, pool_after);
  }
  // PE delTraffic @ 0x00484B50: Track.exit → GroundMap_delTraffic. Count 0,
  // addTrafficCar GameRef stays (not ResourceRef.destroy). Own map so the
  // Valocity 847 spawn is left intact.
  {
    InvObject* pmap = gameref_new();
    InvObject* dummy = gameref_new();
    const int32_t tid =
        java_util_resource_GroundRef_addTrafficCar(pmap, dummy, nullptr);
    const int32_t c0 = tree_field_get_int(pmap, "traffic_count");
    java_util_resource_GroundRef_delTraffic(pmap);
    const int32_t c1 = tree_field_get_int(pmap, "traffic_count");
    const int32_t live = tree_field_get_int(dummy, "traffic_flag_221");
    const int32_t del_ok =
        (tid != 0 && c0 == 1 && c1 == 0 && live == 1) ? 1 : 0;
    tree_field_set_int(map, "del_traffic_smoke", del_ok);
    tree_field_set_int(city, "del_traffic_smoke", del_ok);
    std::printf("  traffic delTraffic ok=%d id=%d c=%d/%d live=%d\n", del_ok, tid,
                c0, c1, live);
  }
  // PE setPedestrianDensityN @ 0x00484D30: Pedestrian_setDensity(d, d*1.1).
  // Config.pedestrianDensity stock is 0 so Valocity N gets 0; probe raw 0.003.
  {
    InvObject* pmap = gameref_new();
    java_util_resource_GroundRef_setPedestrianDensityN(pmap, 0.003f);
    const float d0 = tree_field_get_float(pmap, "pedestrian_density");
    const float d1 = tree_field_get_float(pmap, "pedestrian_density_hi");
    java_util_resource_GroundRef_setPedestrianDensityN(pmap, 0.f);
    const float z0 = tree_field_get_float(pmap, "pedestrian_density");
    const float z1 = tree_field_get_float(pmap, "pedestrian_density_hi");
    const int32_t dens_ok =
        (std::fabs(d0 - 0.003f) < 1e-6f && std::fabs(d1 - 0.003f * 1.1f) < 1e-6f &&
         z0 == 0.f && z1 == 0.f)
            ? 1
            : 0;
    tree_field_set_int(map, "ped_dens_smoke", dens_ok);
    tree_field_set_int(city, "ped_dens_smoke", dens_ok);
    std::printf("  traffic setPedestrianDensityN ok=%d d=%.4f hi=%.4f z=%.1f\n",
                dens_ok, d0, d1, z0);
  }
  // PE addPedestrianType @ 0x00484D90: Pedestrian_addType 32-slot, skip dup.
  {
    InvObject* pmap = gameref_new();
    InvObject* g0 = resref_new();
    java_util_resource_ResourceRef_set(g0, 0x42);
    java_util_resource_GroundRef_addPedestrianType(pmap, g0);
    java_util_resource_GroundRef_addPedestrianType(pmap, g0);
    const int32_t n1 = tree_field_get_int(pmap, "pedestrian_types");
    InvObject* g1 = resref_new();
    java_util_resource_ResourceRef_set(g1, 0x43);
    java_util_resource_GroundRef_addPedestrianType(pmap, g1);
    const int32_t n2 = tree_field_get_int(pmap, "pedestrian_types");
    const float near = java_util_resource_GroundRef_pedestrianDistance(
        pmap, vec3_new(0.f, 0.f, 0.f), 0x42);
    const int32_t type_ok = (n1 == 1 && n2 == 2 && near < 0.01f) ? 1 : 0;
    tree_field_set_int(map, "ped_type_smoke", type_ok);
    tree_field_set_int(city, "ped_type_smoke", type_ok);
    std::printf("  traffic addPedestrianType ok=%d n=%d/%d near=%.3f\n", type_ok,
                n1, n2, near);
  }
  // PE remPedestrianType @ 0x00484E20: Pedestrian_remType miss=no-op.
  // Own map so Valocity's 6 types stay. Erase 0x42, keep 0x43.
  {
    InvObject* pmap = gameref_new();
    InvObject* g0 = resref_new();
    java_util_resource_ResourceRef_set(g0, 0x42);
    InvObject* g1 = resref_new();
    java_util_resource_ResourceRef_set(g1, 0x43);
    InvObject* gmiss = resref_new();
    java_util_resource_ResourceRef_set(gmiss, 0x44);
    java_util_resource_GroundRef_addPedestrianType(pmap, g0);
    java_util_resource_GroundRef_addPedestrianType(pmap, g1);
    java_util_resource_GroundRef_remPedestrianType(pmap, g0);
    const int32_t n1 = tree_field_get_int(pmap, "pedestrian_types");
    java_util_resource_GroundRef_remPedestrianType(pmap, g0);
    java_util_resource_GroundRef_remPedestrianType(pmap, gmiss);
    const int32_t n2 = tree_field_get_int(pmap, "pedestrian_types");
    const float gone = java_util_resource_GroundRef_pedestrianDistance(
        pmap, vec3_new(0.f, 0.f, 0.f), 0x42);
    const float keep = java_util_resource_GroundRef_pedestrianDistance(
        pmap, vec3_new(0.f, 0.f, 0.f), 0x43);
    const int32_t rem_ok =
        (n1 == 1 && n2 == 1 && gone < 0.f && keep < 0.01f) ? 1 : 0;
    tree_field_set_int(map, "ped_rem_smoke", rem_ok);
    tree_field_set_int(city, "ped_rem_smoke", rem_ok);
    std::printf("  traffic remPedestrianType ok=%d n=%d/%d gone=%.1e keep=%.3f\n",
                rem_ok, n1, n2, gone, keep);
  }
  // PE pedestrianDistance @ 0x00484C60: Pedestrian_distance empty/miss → -1.0.
  {
    InvObject* pmap = gameref_new();
    const float empty = java_util_resource_GroundRef_pedestrianDistance(
        pmap, vec3_new(0.f, 0.f, 0.f), 0x42);
    InvObject* g0 = resref_new();
    java_util_resource_ResourceRef_set(g0, 0x42);
    java_util_resource_GroundRef_addPedestrianType(pmap, g0);
    const float near = java_util_resource_GroundRef_pedestrianDistance(
        pmap, vec3_new(0.f, 0.f, 0.f), 0x42);
    const float far = java_util_resource_GroundRef_pedestrianDistance(
        pmap, vec3_new(30.f, 0.f, 0.f), 0x42);
    const float any = java_util_resource_GroundRef_pedestrianDistance(
        pmap, vec3_new(0.f, 0.f, 0.f), 0);
    const float miss = java_util_resource_GroundRef_pedestrianDistance(
        pmap, vec3_new(0.f, 0.f, 0.f), 0x99);
    const int32_t dist_ok =
        (empty == -1.f && near < 0.01f && far > 29.f && far < 31.f &&
         any < 0.01f && miss == -1.f)
            ? 1
            : 0;
    tree_field_set_int(map, "ped_dist_smoke", dist_ok);
    tree_field_set_int(city, "ped_dist_smoke", dist_ok);
    std::printf(
        "  traffic pedestrianDistance ok=%d empty=%.1f near=%.3f far=%.1f "
        "any=%.3f miss=%.1f\n",
        dist_ok, empty, near, far, any, miss);
  }
  // PE setFog @ 0x00486A20: near/far * 10.0 (flt_5E7334). Scene night 20/150.
  {
    const int32_t save_c = render_d3d9_fog_color();
    const float save_n = render_d3d9_fog_near();
    const float save_f = render_d3d9_fog_far();
    const int32_t save_on = render_d3d9_fog_enabled() ? 1 : 0;
    InvObject* pmap = gameref_new();
    java_util_resource_GroundRef_setFog(pmap, 0x0007121e, 20.f, 150.f);
    const int32_t rgb = tree_field_get_int(pmap, "fog_color");
    const float n = tree_field_get_float(pmap, "fog_near");
    const float f = tree_field_get_float(pmap, "fog_far");
    const int32_t fog_ok =
        (tree_field_get_int(pmap, "fog_on") == 1 && rgb == 0x0007121e &&
         n == 200.f && f == 1500.f && render_d3d9_fog_enabled() &&
         render_d3d9_fog_color() == 0x0007121e &&
         render_d3d9_fog_near() == 200.f && render_d3d9_fog_far() == 1500.f)
            ? 1
            : 0;
    if (save_on)
      render_d3d9_set_fog(save_c, save_n, save_f);
    else
      render_d3d9_clear_fog();
    tree_field_set_int(map, "fog_smoke", fog_ok);
    tree_field_set_int(city, "fog_smoke", fog_ok);
    std::printf("  traffic setFog ok=%d rgb=%06X n=%.0f f=%.0f\n", fog_ok, rgb,
                n, f);
  }
  // PE setLight @ 0x00486AB0: RenderRef_applyLight RGB * 1/256. Scene night
  // suntype.setLight(0x00466285, 0x0007121e, 0x00466285). Restore D3D.
  {
    const int32_t save_d = render_d3d9_light_diffuse();
    const int32_t save_a = render_d3d9_light_ambient();
    const int32_t save_s = render_d3d9_light_specular();
    const int32_t save_on = render_d3d9_light_enabled() ? 1 : 0;
    InvObject* sun = resref_new();
    java_util_resource_RenderRef_setLight(sun, 0x00466285, 0x0007121e,
                                          0x00466285);
    const int32_t ld = tree_field_get_int(sun, "light_diffuse");
    const int32_t la = tree_field_get_int(sun, "light_ambient");
    const int32_t ls = tree_field_get_int(sun, "light_specular");
    const int32_t light_ok =
        (ld == 0x00466285 && la == 0x0007121e && ls == 0x00466285 &&
         render_d3d9_light_enabled() &&
         render_d3d9_light_diffuse() == 0x00466285 &&
         render_d3d9_light_ambient() == 0x0007121e &&
         render_d3d9_light_specular() == 0x00466285)
            ? 1
            : 0;
    if (save_on)
      render_d3d9_set_light(save_d, save_a, save_s);
    else
      render_d3d9_clear_light();
    tree_field_set_int(map, "light_smoke", light_ok);
    tree_field_set_int(city, "light_smoke", light_ok);
    std::printf("  traffic setLight ok=%d d=%06X a=%06X s=%06X\n", light_ok, ld,
                la, ls);
  }
  // PE setFlare @ 0x00486B20: RenderRef_applyFlare stores as-is. Scene noon
  // suntype.setFlare(skydome:0x0100, 0xe4e4e4FF, 1, 10, 15, 8). Keyed object.
  {
    InvObject* sun = resref_new();
    InvObject* glow = resref_new();
    java_util_resource_ResourceRef_set(glow, 0x7E105001);
    render_d3d9_texture_create_solid(glow, 0xFFE4E4E4u, 16);
    java_util_resource_RenderRef_setFlare(sun, glow,
                                          static_cast<int32_t>(0xe4e4e4FFu),
                                          1.f, 10.f, 15, 8);
    const int32_t fc = tree_field_get_int(sun, "flare_count");
    const int32_t fr = tree_field_get_int(sun, "flare_rays");
    const int32_t col = tree_field_get_int(sun, "flare_color");
    const float fmax = tree_field_get_float(sun, "flare_max");
    const int32_t flare_ok =
        (fc == 15 && fr == 8 && col == static_cast<int32_t>(0xe4e4e4FFu) &&
         std::fabs(fmax - 10.f) < 0.01f &&
         tree_field_get_int(sun, "flare_tex_id") == 0x7E105001)
            ? 1
            : 0;
    render_d3d9_clear_flare(sun);
    tree_field_set_int(map, "flare_smoke", flare_ok);
    tree_field_set_int(city, "flare_smoke", flare_ok);
    std::printf("  traffic setFlare ok=%d n=%d rays=%d max=%.0f\n", flare_ok, fc,
                fr, fmax);
  }
  // PE duplicate @ 0x004802C0: clone mesh/tex/light/obj (types 5/7/13/14);
  // else share. Painter: unique changeResource after duplicate.
  {
    InvObject* base = resref_new();
    InvObject* dst = resref_new();
    InvObject* tex_old = resref_new();
    InvObject* tex_new = resref_new();
    java_util_resource_ResourceRef_set(tex_old, 0x7E105010);
    java_util_resource_ResourceRef_set(tex_new, 0x7E105011);
    render_d3d9_mesh_create_skydome(base, 10.f);
    render_d3d9_texture_create_solid(tex_old, 0xFFFF0000u, 8);
    render_d3d9_texture_create_solid(tex_new, 0xFF00FF00u, 8);
    render_d3d9_mesh_set_texture(base, tex_old);
    java_util_resource_ResourceRef_duplicate(dst, base);
    java_util_resource_RenderRef_changeResource(dst, tex_old, tex_new);
    InvObject* miss = resref_new();
    java_util_resource_ResourceRef_duplicate(miss, nullptr);
    const int32_t ncloned = tree_field_get_int(dst, "dup_cloned");
    const int32_t src_keep =
        render_d3d9_mesh_get_texture(base, 0) == tex_old ? 1 : 0;
    const int32_t dst_swap =
        render_d3d9_mesh_get_texture(dst, 0) == tex_new ? 1 : 0;
    const int32_t dup_ok =
        (render_d3d9_mesh_ready(dst) &&
         render_d3d9_mesh_vertex_count(dst) ==
             render_d3d9_mesh_vertex_count(base) &&
         src_keep && dst_swap && ncloned == 1 && !render_d3d9_mesh_ready(miss))
            ? 1
            : 0;
    render_d3d9_mesh_destroy(dst);
    tree_field_set_int(map, "dup_smoke", dup_ok);
    tree_field_set_int(city, "dup_smoke", dup_ok);
    std::printf("  traffic duplicate ok=%d cloned=%d src_tex=%d dst_tex=%d\n",
                dup_ok, ncloned, src_keep, dst_swap);
  }
  // PE RenderRef.create @ 0x00480EE0: factory type=3 INSTANCE_RENDER, alias
  // default _renderinst, bindBone "bone00". Scene: sun = new RenderRef(map,
  // suntype, "sunny"). Disposable — do not clobber city.sun.
  {
    InvObject* parent = resref_new();
    InvObject* typ = resref_new();
    InvObject* inst = resref_new();
    java_util_resource_ResourceRef_set(parent, 0x7E105200);
    java_util_resource_ResourceRef_set(typ, 0x7E105124);
    render_d3d9_mesh_create_skydome(parent, 8.f);
    render_d3d9_mesh_create_skydome(typ, 5.f);
    java_util_resource_RenderRef_create(inst, parent, typ, string_new("sunny"));
    InvObject* ao = tree_field_get_obj(inst, "alias");
    const char* al = ao ? string_cstr(ao) : "";
    const int32_t create_ok =
        (java_util_resource_ResourceRef_id(inst) == 0x7E105124 &&
         java_util_resource_ResourceRef_getParentID(inst) == 0x7E105200 &&
         java_util_resource_ResourceRef_type(inst) == 3 && al &&
         std::strcmp(al, "sunny") == 0 &&
         render_d3d9_mesh_get_parent(inst) == parent &&
         render_d3d9_mesh_get_attach_bone(inst) == 0 &&
         render_d3d9_mesh_ready(inst))
            ? 1
            : 0;
    const std::string alias_copy = al ? al : "";
    render_d3d9_mesh_destroy(inst);
    render_d3d9_mesh_destroy(typ);
    render_d3d9_mesh_destroy(parent);
    tree_field_set_int(map, "create_smoke", create_ok);
    tree_field_set_int(city, "create_smoke", create_ok);
    std::printf("  traffic create ok=%d parent=1 alias=%s type=3 bone00=1\n",
                create_ok, alias_copy.empty() ? "?" : alias_copy.c_str());
  }
  // PE changeResource @ 0x00480220: slot.vtable+8(old,new) by native identity
  // (RID), not Java pointer. Painter: new ResourceRef(same RID). Navigator:
  // mesh slot after duplicate. Disposable — do not clobber painter/nav.
  {
    InvObject* base = resref_new();
    InvObject* dst = resref_new();
    InvObject* tex_a = resref_new();
    InvObject* tex_b = resref_new();
    InvObject* old_rid = resref_new();
    java_util_resource_ResourceRef_set(tex_a, 0x7E105010);
    java_util_resource_ResourceRef_set(tex_b, 0x7E105011);
    java_util_resource_ResourceRef_set(old_rid, 0x7E105010);
    render_d3d9_mesh_create_skydome(base, 10.f);
    render_d3d9_texture_create_solid(tex_a, 0xFFFF0000u, 8);
    render_d3d9_texture_create_solid(tex_b, 0xFF00FF00u, 8);
    render_d3d9_mesh_set_texture(base, tex_a);
    java_util_resource_ResourceRef_duplicate(dst, base);
    java_util_resource_RenderRef_changeResource(dst, old_rid, tex_b);
    const int32_t rid_ok =
        render_d3d9_mesh_get_texture(dst, 0) == tex_b ? 1 : 0;

    InvObject* msh_src = resref_new();
    InvObject* msh_new = resref_new();
    InvObject* inst = resref_new();
    render_d3d9_mesh_create_skydome(msh_src, 10.f);
    render_d3d9_mesh_create_skydome(msh_new, 5.f);
    java_util_resource_ResourceRef_duplicate(inst, msh_src);
    java_util_resource_RenderRef_changeResource(inst, old_rid, tex_b);
    java_util_resource_RenderRef_changeResource(inst, msh_src, msh_new);
    const int32_t mesh_ok =
        (render_d3d9_mesh_ready(inst) &&
         render_d3d9_mesh_get_texture(inst, 0) == tex_b)
            ? 1
            : 0;
    const int32_t chg_ok = (rid_ok && mesh_ok) ? 1 : 0;
    render_d3d9_mesh_destroy(dst);
    render_d3d9_mesh_destroy(inst);
    render_d3d9_mesh_destroy(base);
    render_d3d9_mesh_destroy(msh_src);
    render_d3d9_mesh_destroy(msh_new);
    tree_field_set_int(map, "chg_smoke", chg_ok);
    tree_field_set_int(city, "chg_smoke", chg_ok);
    std::printf("  traffic changeResource ok=%d rid=%d mesh=%d\n", chg_ok,
                rid_ok, mesh_ok);
  }
  // PE setColor @ 0x00480310: slot+0xCC = DWORD as-is (no 1/256).
  // InventoryItem_Folder 0xFFFF0000 / InventoryItem_Paint paintCan.color.
  {
    InvObject* can = resref_new();
    render_d3d9_mesh_create_skydome(can, 2.f);
    java_util_resource_RenderRef_setColor(
        can, static_cast<int32_t>(0xFFFF0000u));
    const int32_t col = tree_field_get_int(can, "color");
    const int32_t color_ok =
        (col == static_cast<int32_t>(0xFFFF0000u) &&
         render_d3d9_mesh_get_color(can) == static_cast<int32_t>(0xFFFF0000u))
            ? 1
            : 0;
    render_d3d9_mesh_destroy(can);
    tree_field_set_int(map, "color_smoke", color_ok);
    tree_field_set_int(city, "color_smoke", color_ok);
    std::printf("  traffic setColor ok=%d c=%08X\n", color_ok,
                static_cast<uint32_t>(col));
  }
  // PE setType @ 0x00486BA0 / getTypeID @ 0x00486BE0: instance tag 3 then
  // type id at slot+0x80. Non-instance → 0 (MouseCursor spark filter).
  {
    InvObject* parent = resref_new();
    InvObject* typ = resref_new();
    InvObject* typ2 = resref_new();
    InvObject* inst = resref_new();
    InvObject* bare = resref_new();
    java_util_resource_ResourceRef_set(parent, 0x7E105200);
    java_util_resource_ResourceRef_set(typ, 0x7E105124);
    java_util_resource_ResourceRef_set(typ2, 0x7E105125);
    java_util_resource_ResourceRef_set(bare, 0x7E105010);
    java_util_resource_RenderRef_create(inst, parent, typ, string_new("t"));
    const int32_t id0 = java_util_resource_RenderRef_getTypeID(inst);
    java_util_resource_RenderRef_setType(inst, typ2);
    const int32_t id1 = java_util_resource_RenderRef_getTypeID(inst);
    const int32_t id_bare = java_util_resource_RenderRef_getTypeID(bare);
    const int32_t type_ok =
        (id0 == 0x7E105124 && id1 == 0x7E105125 && id_bare == 0 &&
         java_util_resource_RenderRef_getTypeID(nullptr) == 0)
            ? 1
            : 0;
    tree_field_set_int(map, "type_smoke", type_ok);
    tree_field_set_int(city, "type_smoke", type_ok);
    std::printf("  traffic getTypeID ok=%d create=%X set=%X bare=%d\n", type_ok,
                static_cast<uint32_t>(id0), static_cast<uint32_t>(id1),
                id_bare);
  }
  // PE scaleMesh @ 0x00480390 → ResourceRef_applyScaleMesh @ 0x0048E7F0:
  // bake verts (not MeshXform). RectangleTemplate: duplicate, scale, then
  // changeResource so the clone carries AABB.
  {
    InvObject* src = resref_new();
    InvObject* scaled = resref_new();
    InvObject* inst = resref_new();
    render_d3d9_mesh_create_skydome(src, 10.f);
    java_util_resource_ResourceRef_duplicate(scaled, src);
    java_util_resource_ResourceRef_scaleMesh(scaled, 2.f, 3.f, 4.f);
    java_util_resource_RenderRef_changeResource(inst, src, scaled);
    float omin[3] = {0, 0, 0}, omax[3] = {0, 0, 0};
    float smin[3] = {0, 0, 0}, smax[3] = {0, 0, 0};
    float imin[3] = {0, 0, 0}, imax[3] = {0, 0, 0};
    render_d3d9_mesh_local_bounds(src, omin, omax);
    render_d3d9_mesh_local_bounds(scaled, smin, smax);
    render_d3d9_mesh_local_bounds(inst, imin, imax);
    float tsx = 0, tsy = 0, tsz = 0;
    float dummy = 0;
    render_d3d9_mesh_get_transform(scaled, &dummy, &dummy, &dummy, &dummy,
                                   &dummy, &dummy, &tsx, &tsy, &tsz);
    java_util_resource_ResourceRef_scaleMesh(nullptr, 9.f, 9.f, 9.f);
    const int32_t scale_ok =
        (std::fabs(omax[0] - 10.f) < 0.05f &&
         std::fabs(smax[0] - 20.f) < 0.05f &&
         std::fabs(smax[1] - 30.f) < 0.05f &&
         std::fabs(smax[2] - 40.f) < 0.05f &&
         std::fabs(imax[0] - 20.f) < 0.05f &&
         std::fabs(imax[1] - 30.f) < 0.05f &&
         std::fabs(imax[2] - 40.f) < 0.05f && tsx > 0.99f && tsx < 1.01f)
            ? 1
            : 0;
    render_d3d9_mesh_destroy(inst);
    render_d3d9_mesh_destroy(scaled);
    render_d3d9_mesh_destroy(src);
    tree_field_set_int(map, "scale_smoke", scale_ok);
    tree_field_set_int(city, "scale_smoke", scale_ok);
    std::printf("  traffic scaleMesh ok=%d aabb=(%.0f,%.0f,%.0f) xform=%.2f\n",
                scale_ok, smax[0], smax[1], smax[2], tsx);
  }
  // PE plotRoute @ 0x00483960: return 0|1. No spline / no parent / no type → 0.
  // lineCreate @ 0x0047FE70 same 0|1. lineAdd without create = no-op.
  // RaceSetup: plotRoute(nav.localroot, particles:0x17, 0xFFFF0000, 10, 0.01).
  {
    InvObject* parent = resref_new();
    InvObject* typ = resref_new();
    InvObject* line = resref_new();
    InvObject* bare = resref_new();
    java_util_resource_ResourceRef_set(typ, 0x17);
    const float rlen = java_util_resource_GroundRef_findRoute(
        map, vec3_new(0.f, 0.f, 0.f), vec3_new(100.f, 0.f, 80.f));
    const int32_t miss_pt =
        java_util_resource_RenderRef_plotRoute(
            line, nullptr, typ, static_cast<int32_t>(0xFFFF0000u), 10.f,
            vec3_new(0.01f, 0.f, 0.01f));
    const int32_t ok = java_util_resource_RenderRef_plotRoute(
        line, parent, typ, static_cast<int32_t>(0xFFFF0000u), 10.f,
        vec3_new(0.01f, 0.f, 0.01f));
    const int32_t npts = render_line_point_count(line);
    java_util_resource_RenderRef_lineAdd(
        bare, vec3_new(1.f, 0.f, 2.f), vec3_new(0.f, 1.f, 0.f),
        static_cast<int32_t>(0xFF00FF00u), 0.05f);
    const int32_t lc_miss =
        java_util_resource_RenderRef_lineCreate(bare, nullptr, typ);
    const int32_t lc_ok =
        java_util_resource_RenderRef_lineCreate(bare, parent, typ);
    const int32_t plot_ok =
        (rlen > 1.f && miss_pt == 0 && ok == 1 && npts >= 2 &&
         render_line_point_count(bare) == 0 && lc_miss == 0 && lc_ok == 1)
            ? 1
            : 0;
    tree_field_set_int(map, "plot_smoke", plot_ok);
    tree_field_set_int(city, "plot_smoke", plot_ok);
    std::printf("  traffic plotRoute ok=%d ret=%d n=%d miss=%d lc=%d\n", plot_ok,
                ok, npts, miss_pt, lc_ok);
  }
  // PE getRoutePos @ 0x00483B30 / getRouteDist @ 0x00483C20 /
  // getRouteLength()F @ 0x00483C00: cached spline from findRoute. No spline
  // → pos=null / dist=0 / len=0. pos null → dist=-1. t unclamped.
  {
    const float rlen = java_util_resource_GroundRef_findRoute(
        map, vec3_new(0.f, 0.f, 0.f), vec3_new(100.f, 0.f, 80.f));
    InvObject* p0 = java_util_resource_GroundRef_getRoutePos(map, 0.f);
    InvObject* p1 = java_util_resource_GroundRef_getRoutePos(map, 1.f);
    float x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
    if (p0) vec3_get(p0, &x0, &y0, &z0);
    if (p1) vec3_get(p1, &x1, &y1, &z1);
    const float d0 = java_util_resource_GroundRef_getRouteDist(map, p0);
    const float d1 = java_util_resource_GroundRef_getRouteDist(map, p1);
    const float dnull = java_util_resource_GroundRef_getRouteDist(map, nullptr);
    const float lenf = java_util_resource_GroundRef_getRouteLength(map);
    const int32_t route_ok =
        (rlen > 1.f && p0 && p1 && std::fabs(x0) < 5.f && std::fabs(z0) < 5.f &&
         std::fabs(x1 - 100.f) < 15.f && std::fabs(z1 - 80.f) < 15.f &&
         d0 > -0.05f && d0 < 0.15f && d1 > 0.85f && d1 < 1.15f &&
         dnull == -1.f && std::fabs(lenf - rlen) < 0.5f)
            ? 1
            : 0;
    tree_field_set_int(map, "routepos_smoke", route_ok);
    tree_field_set_int(city, "routepos_smoke", route_ok);
    std::printf("  traffic getRoutePos ok=%d d0=%.2f d1=%.2f len=%.0f\n",
                route_ok, d0, d1, lenf);
  }
  // PE setMatrix @ 0x0047E490: null Vector3/Ypr → pose 0,0,0 + ori 0,0,0
  // then GameRef_applyWorldXform. Vehicle.create setMatrix(null,null).
  {
    InvObject* car = gameref_new();
    java_util_resource_GameRef_setMatrix(
        car, vec3_new(10.f, 20.f, 30.f), ypr_new(0.5f, -0.25f, 1.f));
    float px = 0, py = 0, pz = 0, oy = 0, op = 0, or_ = 0;
    vec3_get(java_util_resource_GameRef_getPos(car), &px, &py, &pz);
    ypr_get(java_util_resource_GameRef_getOri(car), &oy, &op, &or_);
    java_util_resource_GameRef_setMatrix(car, nullptr, nullptr);
    float nx = 0, ny = 0, nz = 0, nyaw = 0, np = 0, nr = 0;
    vec3_get(java_util_resource_GameRef_getPos(car), &nx, &ny, &nz);
    ypr_get(java_util_resource_GameRef_getOri(car), &nyaw, &np, &nr);
    java_util_resource_GameRef_setMatrix(car, vec3_new(1.f, 2.f, 3.f), nullptr);
    float zx = 0, zy = 0, zz = 0, zoy = 0, zop = 0, zor = 0;
    vec3_get(java_util_resource_GameRef_getPos(car), &zx, &zy, &zz);
    ypr_get(java_util_resource_GameRef_getOri(car), &zoy, &zop, &zor);
    const int32_t mtx_ok =
        (std::fabs(px - 10.f) < 0.01f && std::fabs(py - 20.f) < 0.01f &&
         std::fabs(pz - 30.f) < 0.01f && std::fabs(oy - 0.5f) < 0.01f &&
         std::fabs(op + 0.25f) < 0.01f && std::fabs(or_ - 1.f) < 0.01f &&
         std::fabs(nx) < 0.01f && std::fabs(ny) < 0.01f &&
         std::fabs(nz) < 0.01f && std::fabs(nyaw) < 0.01f &&
         std::fabs(np) < 0.01f && std::fabs(nr) < 0.01f &&
         std::fabs(zx - 1.f) < 0.01f && std::fabs(zy - 2.f) < 0.01f &&
         std::fabs(zz - 3.f) < 0.01f && std::fabs(zoy) < 0.01f &&
         std::fabs(zop) < 0.01f && std::fabs(zor) < 0.01f)
            ? 1
            : 0;
    tree_field_set_int(map, "setmatrix_smoke", mtx_ok);
    tree_field_set_int(city, "setmatrix_smoke", mtx_ok);
    std::printf("  traffic setMatrix ok=%d null=(%.0f,%.0f,%.0f)\n", mtx_ok, nx,
                ny, nz);
  }
  // PE setPos @ 0x0047E350: pose + identity YPR via applyWorldXform.
  // Garage.lockCar: player.car.setPos(defCarPos).
  {
    InvObject* car = gameref_new();
    java_util_resource_GameRef_setMatrix(
        car, vec3_new(10.f, 20.f, 30.f), ypr_new(0.5f, -0.25f, 1.f));
    java_util_resource_GameRef_setPos(car, vec3_new(4.f, 5.f, 6.f));
    float px = 0, py = 0, pz = 0, oy = 0, op = 0, or_ = 0;
    vec3_get(java_util_resource_GameRef_getPos(car), &px, &py, &pz);
    ypr_get(java_util_resource_GameRef_getOri(car), &oy, &op, &or_);
    const int32_t pos_ok =
        (std::fabs(px - 4.f) < 0.01f && std::fabs(py - 5.f) < 0.01f &&
         std::fabs(pz - 6.f) < 0.01f && std::fabs(oy) < 0.01f &&
         std::fabs(op) < 0.01f && std::fabs(or_) < 0.01f)
            ? 1
            : 0;
    tree_field_set_int(map, "setpos_smoke", pos_ok);
    tree_field_set_int(city, "setpos_smoke", pos_ok);
    std::printf("  traffic setPos ok=%d ori=(%.2f,%.2f,%.2f)\n", pos_ok, oy, op,
                or_);
  }
  // PE setState @ 0x0047E630: pose + linvel + angvel via applyWorldXform.
  // Null linvel/angvel → 0,0,0. setMatrix zeros vx — setState must restore.
  {
    InvObject* car = gameref_new();
    java_util_resource_GameRef_setMatrix(
        car, vec3_new(10.f, 20.f, 30.f), ypr_new(0.5f, -0.25f, 1.f));
    java_util_resource_GameRef_setState(
        car, vec3_new(4.f, 5.f, 6.f), ypr_new(0.1f, 0.2f, 0.3f),
        vec3_new(1.f, 2.f, 3.f), vec3_new(7.f, 8.f, 9.f));
    float px = 0, py = 0, pz = 0, oy = 0, op = 0, or_ = 0;
    float vx = 0, vy = 0, vz = 0, wx = 0, wy = 0, wz = 0;
    vec3_get(java_util_resource_GameRef_getPos(car), &px, &py, &pz);
    ypr_get(java_util_resource_GameRef_getOri(car), &oy, &op, &or_);
    vec3_get(java_util_resource_GameRef_getVel(car), &vx, &vy, &vz);
    vec3_get(java_util_resource_PhysicsRef_getAngVel(car), &wx, &wy, &wz);
    java_util_resource_GameRef_setState(car, vec3_new(1.f, 2.f, 3.f), nullptr,
                                        nullptr, nullptr);
    float nvx = 0, nvy = 0, nvz = 0, nwx = 0, nwy = 0, nwz = 0;
    vec3_get(java_util_resource_GameRef_getVel(car), &nvx, &nvy, &nvz);
    vec3_get(java_util_resource_PhysicsRef_getAngVel(car), &nwx, &nwy, &nwz);
    const int32_t state_ok =
        (std::fabs(px - 4.f) < 0.01f && std::fabs(py - 5.f) < 0.01f &&
         std::fabs(pz - 6.f) < 0.01f && std::fabs(oy - 0.1f) < 0.01f &&
         std::fabs(op - 0.2f) < 0.01f && std::fabs(or_ - 0.3f) < 0.01f &&
         std::fabs(vx - 1.f) < 0.01f && std::fabs(vy - 2.f) < 0.01f &&
         std::fabs(vz - 3.f) < 0.01f && std::fabs(wx - 7.f) < 0.01f &&
         std::fabs(wy - 8.f) < 0.01f && std::fabs(wz - 9.f) < 0.01f &&
         std::fabs(nvx) < 0.01f && std::fabs(nvy) < 0.01f &&
         std::fabs(nvz) < 0.01f && std::fabs(nwx) < 0.01f &&
         std::fabs(nwy) < 0.01f && std::fabs(nwz) < 0.01f)
            ? 1
            : 0;
    tree_field_set_int(map, "setstate_smoke", state_ok);
    tree_field_set_int(city, "setstate_smoke", state_ok);
    std::printf("  traffic setState ok=%d vel=(%.1f,%.1f,%.1f)\n", state_ok, vx,
                vy, vz);
  }
  // PE getPos @ 0x0047DAD0: handle 0 → Mighty ERROR + nullptr.
  // handle+8==0 → nullptr (not Vector3 0,0,0). Else sub_48B280 + new Vector3.
  // Host: empty==true / !self → nullptr; posed origin still Vector3.
  {
    InvObject* fresh = gameref_new();
    InvObject* none = java_util_resource_GameRef_getPos(fresh);
    java_util_resource_GameRef_setPos(fresh, vec3_new(4.f, 5.f, 6.f));
    InvObject* after_pos = java_util_resource_GameRef_getPos(fresh);
    float px = 0, py = 0, pz = 0;
    if (after_pos) vec3_get(after_pos, &px, &py, &pz);
    InvObject* car = gameref_new();
    java_util_resource_GameRef_setMatrix(car, vec3_new(7.f, 8.f, 9.f), nullptr);
    InvObject* after_mtx = java_util_resource_GameRef_getPos(car);
    float mx = 0, my = 0, mz = 0;
    if (after_mtx) vec3_get(after_mtx, &mx, &my, &mz);
    java_util_resource_GameRef_setPos(car, vec3_new(0.f, 0.f, 0.f));
    InvObject* origin = java_util_resource_GameRef_getPos(car);
    float ox = 1.f, oy = 1.f, oz = 1.f;
    if (origin) vec3_get(origin, &ox, &oy, &oz);
    const int32_t getpos_ok =
        (!none && after_pos && std::fabs(px - 4.f) < 0.01f &&
         std::fabs(py - 5.f) < 0.01f && std::fabs(pz - 6.f) < 0.01f &&
         after_mtx && std::fabs(mx - 7.f) < 0.01f &&
         std::fabs(my - 8.f) < 0.01f && std::fabs(mz - 9.f) < 0.01f && origin &&
         std::fabs(ox) < 0.01f && std::fabs(oy) < 0.01f &&
         std::fabs(oz) < 0.01f)
            ? 1
            : 0;
    tree_field_set_int(map, "getpos_smoke", getpos_ok);
    tree_field_set_int(city, "getpos_smoke", getpos_ok);
    std::printf("  traffic getPos ok=%d\n", getpos_ok);
  }
  // PE isEmpty @ 0x00486D10: handle 0 → 1 (no Mighty ERROR). inner+0xC /
  // RESTYPE_GAME=8 + payload → 0. Unlike getPos, no handle+8 test.
  // Host: empty=inner miss; type==8 payload slots; type!=8 RID/script/pose
  // stand-in. fresh=1; RID/create/pose=0; destroy=1. getPos(fresh)
  // still nullptr (shared empty); getVel(fresh) still Vector3(0,0,0).
  {
    InvObject* fresh = gameref_new();
    const int32_t fresh_e = java_util_resource_GameRef_isEmpty(fresh);
    const int32_t null_e = java_util_resource_GameRef_isEmpty(nullptr);
    InvObject* fresh_pos = java_util_resource_GameRef_getPos(fresh);
    InvObject* fresh_vel = java_util_resource_GameRef_getVel(fresh);
    float fvx = 1.f, fvy = 1.f, fvz = 1.f;
    if (fresh_vel) vec3_get(fresh_vel, &fvx, &fvy, &fvz);
    InvObject* rid = gameref_new();
    java_util_resource_ResourceRef_set(rid, 1);
    const int32_t rid_e = java_util_resource_GameRef_isEmpty(rid);
    InvObject* posed = gameref_new();
    java_util_resource_GameRef_setPos(posed, vec3_new(4.f, 5.f, 6.f));
    const int32_t posed_e = java_util_resource_GameRef_isEmpty(posed);
    InvObject* posed_pos = java_util_resource_GameRef_getPos(posed);
    float px = 0, py = 0, pz = 0;
    if (posed_pos) vec3_get(posed_pos, &px, &py, &pz);
    InvObject* xa = gameref_new();
    InvObject* type = resref_new();
    java_util_resource_ResourceRef_set(type, 1);
    java_util_resource_GameRef_create(xa, nullptr, type, string_new(""),
                                      string_new("isempty_smoke"));
    const int32_t created_e = java_util_resource_GameRef_isEmpty(xa);
    java_util_resource_ResourceRef_destroy(xa);
    const int32_t dest_e = java_util_resource_GameRef_isEmpty(xa);
    const int32_t isempty_ok =
        (fresh_e == 1 && null_e == 1 && !fresh_pos && fresh_vel &&
         std::fabs(fvx) < 0.01f && std::fabs(fvy) < 0.01f &&
         std::fabs(fvz) < 0.01f && rid_e == 0 && posed_e == 0 && posed_pos &&
         std::fabs(px - 4.f) < 0.01f && std::fabs(py - 5.f) < 0.01f &&
         std::fabs(pz - 6.f) < 0.01f && created_e == 0 && dest_e == 1)
            ? 1
            : 0;
    tree_field_set_int(map, "isempty_smoke", isempty_ok);
    tree_field_set_int(city, "isempty_smoke", isempty_ok);
    std::printf("  traffic isEmpty ok=%d\n", isempty_ok);
  }
  // PE isScripted @ 0x00486DA0: handle 0 → ebx=0 @ loc_486F1E (no Mighty,
  // unlike getPos loc_47DB90). Contrast isEmpty @ 0x00486D10: handle 0 →
  // edi=1; RESTYPE_GAME=8 only. No script → 0. clazzname null → 1 iff
  // scripted. Empty C str → 0 (JNI `L;`). FQN is-a (Class_isInheritedFrom),
  // not alias. VehicleType is not a Part.
  {
    const int32_t null_h =
        java_util_resource_GameRef_isScripted(nullptr, nullptr);
    const int32_t null_empty = java_util_resource_GameRef_isEmpty(nullptr);
    InvObject* fresh = gameref_new();
    const int32_t fresh_any =
        java_util_resource_GameRef_isScripted(fresh, nullptr);
    const int32_t fresh_part = java_util_resource_GameRef_isScripted(
        fresh, string_new("java.game.parts.Part"));
    InvObject* part = gameref_new();
    InvObject* vt = gameref_new();
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      bind_gameref(part, nullptr, "java.game.parts.Part", "part");
      bind_gameref(vt, nullptr, "java.game.VehicleType", "VehicleType");
    }
    const int32_t part_any =
        java_util_resource_GameRef_isScripted(part, nullptr);
    const int32_t part_part = java_util_resource_GameRef_isScripted(
        part, string_new("java.game.parts.Part"));
    const int32_t part_set = java_util_resource_GameRef_isScripted(
        part, string_new("java.game.parts.Set"));
    const int32_t part_vt = java_util_resource_GameRef_isScripted(
        part, string_new("java.game.VehicleType"));
    const int32_t part_empty =
        java_util_resource_GameRef_isScripted(part, string_new(""));
    const int32_t vt_any = java_util_resource_GameRef_isScripted(vt, nullptr);
    const int32_t vt_vt = java_util_resource_GameRef_isScripted(
        vt, string_new("java.game.VehicleType"));
    const int32_t vt_part = java_util_resource_GameRef_isScripted(
        vt, string_new("java.game.parts.Part"));
    int32_t inherit_ok = 1;
    if (Jvm* j = jvm_active()) {
      const char* wheel_fqn =
          "java.game.parts.rgearpart.reciprocatingrgearpart.Wheel";
      if (!j->find_class(wheel_fqn)) j->load_class(wheel_fqn);
      if (j->find_class(wheel_fqn)) {
        InvObject* wheel = gameref_new();
        {
          std::lock_guard<std::mutex> lock(g_gr_mu);
          bind_gameref(wheel, nullptr, wheel_fqn, "w");
        }
        inherit_ok =
            (java_util_resource_GameRef_isScripted(
                 wheel, string_new("java.game.parts.Part")) == 1 &&
             java_util_resource_GameRef_isScripted(
                 wheel, string_new("java.game.parts.Set")) == 0)
                ? 1
                : 0;
      }
    }
    const int32_t isscripted_ok =
        (null_h == 0 && null_empty == 1 && fresh_any == 0 && fresh_part == 0 &&
         part_any == 1 && part_part == 1 && part_set == 0 && part_vt == 0 &&
         part_empty == 0 && vt_any == 1 && vt_vt == 1 && vt_part == 0 &&
         inherit_ok)
            ? 1
            : 0;
    tree_field_set_int(map, "isscripted_smoke", isscripted_ok);
    tree_field_set_int(city, "isscripted_smoke", isscripted_ok);
    std::printf("  traffic isScripted ok=%d\n", isscripted_ok);
  }
  // PE getScriptInstance @ 0x00486F30: handle 0 → null (no Mighty).
  // type1 → *(payload+0x50); type8 → Class_boxObject stand-in
  // (tree_host_new(script_class)); type0 → script.
  // Host: type 0 after bind_gameref → .script (part_si==part smoke).
  {
    InvObject* null_si =
        java_util_resource_GameRef_getScriptInstance(nullptr);
    InvObject* fresh = gameref_new();
    InvObject* fresh_si = java_util_resource_GameRef_getScriptInstance(fresh);
    InvObject* part = gameref_new();
    {
      std::lock_guard<std::mutex> lock(g_gr_mu);
      bind_gameref(part, nullptr, "java.game.parts.Part", "part");
    }
    InvObject* part_si = java_util_resource_GameRef_getScriptInstance(part);
    const int32_t getscript_ok =
        (!null_si && !fresh_si && part_si == part) ? 1 : 0;
    tree_field_set_int(map, "getscript_smoke", getscript_ok);
    tree_field_set_int(city, "getscript_smoke", getscript_ok);
    std::printf("  traffic getScriptInstance ok=%d\n", getscript_ok);
  }
  // race112 PE getVel @ 0x0047DCE0 / getOri @ 0x0047DBE0: handle 0 → Mighty
  // ERROR + nullptr. No handle+8 skip (unlike getPos). Always alloc if
  // handle≠0. Empty still Vector3(0,0,0) / Ypr(0,0,0). Only !self → nullptr.
  {
    InvObject* posed = gameref_new();
    java_util_resource_GameRef_setPos(posed, vec3_new(4.f, 5.f, 6.f));
    InvObject* posed_vel = java_util_resource_GameRef_getVel(posed);
    InvObject* posed_ori = java_util_resource_GameRef_getOri(posed);
    InvObject* empty = gameref_new();
    InvObject* empty_vel = java_util_resource_GameRef_getVel(empty);
    InvObject* empty_ori = java_util_resource_GameRef_getOri(empty);
    float evx = 1.f, evy = 1.f, evz = 1.f;
    float eoy = 1.f, eop = 1.f, eor = 1.f;
    if (empty_vel) vec3_get(empty_vel, &evx, &evy, &evz);
    if (empty_ori) ypr_get(empty_ori, &eoy, &eop, &eor);
    InvObject* null_vel = java_util_resource_GameRef_getVel(nullptr);
    InvObject* null_ori = java_util_resource_GameRef_getOri(nullptr);
    const int32_t getvel_ok =
        (posed_vel && posed_ori && empty_vel && empty_ori &&
         std::fabs(evx) < 0.01f && std::fabs(evy) < 0.01f &&
         std::fabs(evz) < 0.01f && std::fabs(eoy) < 0.01f &&
         std::fabs(eop) < 0.01f && std::fabs(eor) < 0.01f && !null_vel &&
         !null_ori)
            ? 1
            : 0;
    tree_field_set_int(map, "getvel_smoke", getvel_ok);
    tree_field_set_int(city, "getvel_smoke", getvel_ok);
    std::printf("  traffic getVel ok=%d\n", getvel_ok);
  }
  // Soft pose/vel/phys sample: GetWorldPos ch=2 / GII_VEL ch=3 prefer
  // chassis PhysicsRef (Vehicle.set Native.ptr). Write-through
  // GameRefState. traffic_car_sample_pos under g_gr_mu Soft-samples phys
  // without nesting GameRef_getPos (Resources mutex ≠ GameRef g_gr_mu).
  {
    InvObject* car = gameref_new();
    InvObject* chassis = gameref_new();
    tree_field_set_obj(car, "chassis", chassis);