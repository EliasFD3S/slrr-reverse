// Fragment 1/3 of tree_eval.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


namespace inv {

JvmValue tree_eval(TreeHost* host, const JvmClass& cls, const JvmMethod& method,
                   std::vector<JvmValue> locals, std::string* err) {
  if (!host) {
    if (err) *err = "null host";
    return JvmValue::make_void();
  }
  if (method.tree_index < 0 ||
      static_cast<size_t>(method.tree_index) >= cls.trees.size()) {
    if (err) *err = "bad tree_index";
    return JvmValue::make_void();
  }
  // Snapshot: nested load_class during CALL can reallocate Jvm::classes_ and
  // invalidate references into `cls` (SplashScreen.enter → createBG → Osd).
  const TreeBody tree = cls.trees[static_cast<size_t>(method.tree_index)];
  const std::string cls_name = cls.name;
  const std::string super_name = cls.super_name;
  // Garage icons use CMD 100..199; RaceSetup.createOSDObjects uses 0..6
  // (CMD_RACE..CMD_PRIZE). Same ResourceRef+tip addItem packing.
  auto is_icon_osd_cmd = [&](int32_t c) -> bool {
    if (c >= 100 && c < 200) return true;
    if (cls_name.find("RaceSetup") != std::string::npos && c >= 0 && c <= 6)
      return true;
    return false;
  };
  const std::vector<std::string> const_strings = cls.const_strings;
  const std::vector<std::string> const_mref_name = cls.const_mref_name;
  const std::vector<int32_t> const_ints = cls.const_ints;
  const std::vector<uint8_t> const_int_valid = cls.const_int_valid;
  const std::vector<std::string> const_rid_pack = cls.const_rid_pack;

  // Grow locals on demand (TREE may declare slots beyond args).
  auto ensure_local = [&](size_t i) {
    if (locals.size() <= i) locals.resize(i + 1, JvmValue::make_int(0));
  };

  std::vector<JvmValue> stack;
  stack.reserve(32);
  std::optional<uint32_t> pending_binop;
  std::string pending_class;
  int pending_local = -1;
  bool pending_concat = false;
  int pending_concat_args = 0;  // how many string operands waiting (2 or 4)
  InvObject* last_new = nullptr;
  InvObject* recent_new = nullptr;  // survives 0x21 clear for AASTORE+<init>
  InvObject* last_menu = nullptr;   // OptionsDialog: createMenu → setSliderStyle/addItem
  InvObject* last_menu_item = nullptr;  // addItem → printValue/setValue chain
  std::string pending_invoke_name;  // Garage: 0x09 method name before 0x12
  int ff_local = -1;
  int name_local = -1;
  int last_loaded_local = -1;
  int vm_local = -1;  // VehicleModel local slot (reused across NEW in VT ctors)
  std::string pending_putfield;  // field name after NEW+0x21+0x1B hint
  // Phase 2.114: arr[i] = new T(...) / arr[i] = null — 0x20 then 0x08/35.
  InvObject* pending_aastore_arr = nullptr;
  int32_t pending_aastore_idx = 0;
  bool pending_aastore = false;
  // Phase 2.115: 0x22 this()/0x23 super() completed by 0x08/34.
  int pending_special_init = 0;  // 1=this, 2=super
  // getstatic Class.Field + NEWARRAY: 0x11/2 0x19/cls 0x1b/fld 0x07/31 0x06 0x10
  bool pending_static_newarray = false;
  size_t steps = 0;

  auto pop = [&]() -> JvmValue {
    if (stack.empty()) return JvmValue::make_int(0);
    JvmValue v = stack.back();
    stack.pop_back();
    return v;
  };
  auto push = [&](JvmValue v) { stack.push_back(v); };

  auto is_osd_invoke_name = [](const std::string& s) -> bool {
    return s == "createMenu" || s == "addItem" || s == "addSeparator" ||
           s == "createText" || s == "createTextBox" || s == "createButton" ||
           s == "createRectangle" || s == "createHeader" || s == "createBG" ||
           s == "createHotkey" || s == "disable" || s == "enable" ||
           s == "setToolTip" || s == "endGroup" || s == "beginGroup" ||
           s == "hideGroup" || s == "showGroup" || s == "createOSDObjects";
  };
  auto mname_is_garbage = [&](const std::string& s) -> bool {
    if (s.empty()) return true;
    if (is_osd_invoke_name(s)) return false;
    // Soft method fidelity: PE Object_callMethod @ 0x408A30 is name-only;
    // descriptors / FQNs / coords leaked via empty mref are not methods.
    if (s[0] == ' ' || s[0] == 'L' || s[0] == '(' || s[0] == '[')
      return true;
    if (s.find(',') != std::string::npos) return true;
    if (s.find(' ') != std::string::npos) return true;
    if (s.find('.') != std::string::npos && s.find('(') == std::string::npos)
      return true;  // java.game.Scene etc.
    return false;
  };
  auto resolve_invoke_name = [&](std::string mname) -> std::string {
    auto stack_has_icon_rr = [&]() -> bool {
      for (size_t i = 0; i < 6 && i < stack.size(); ++i) {
        const JvmValue& v = stack[stack.size() - 1 - i];
        if (v.tag != JvmTag::Obj || !v.v.o) continue;
        const char* hc = tree_host_class(v.v.o);
        if (hc && (std::strstr(hc, "ResourceRef") ||
                   std::strstr(hc, "GameRef")))
          return true;
      }
      return false;
    };
    // Named createMenu with icon-addItem args (Garage TREE) → addItem.
    if (!mname_is_garbage(mname)) {
      if (mname == "createMenu" && last_menu) {
        bool rr = stack_has_icon_rr();
        if (!rr && recent_new) {
          const char* rhc = tree_host_class(recent_new);
          if (rhc && (std::strstr(rhc, "ResourceRef") ||
                      std::strstr(rhc, "GameRef")))
            rr = true;
        }
        bool cmd = false;
        for (size_t i = 0; i < 6 && i < stack.size(); ++i) {
          const JvmValue& v = stack[stack.size() - 1 - i];
          if (v.tag == JvmTag::Int && is_icon_osd_cmd(v.v.i)) cmd = true;
        }
        if (rr || cmd) return "addItem";
      }
      return mname;
    }
    // Osd on top + broken imm → createMenu when floats under Osd.
    // Style often lives only in a local (Garage buttonStyle).
    // Do NOT treat trailing ori int as addItem — that skipped createMenu.
    if (!stack.empty() && stack.back().tag == JvmTag::Obj && stack.back().v.o) {
      const char* thc = tree_host_class(stack.back().v.o);
      if (thc && std::strstr(thc, "Osd")) {
        bool looks_menu_args = false;
        bool looks_additem = false;
        bool has_float = false;
        bool looks_text = false;  // createText: String/Font + floats
        for (size_t i = 1; i < 6 && i < stack.size(); ++i) {
          const JvmValue& v = stack[stack.size() - 1 - i];
          if (v.tag == JvmTag::Obj && v.v.o) {
            const char* hc = tree_host_class(v.v.o);
            if (hc && std::strstr(hc, "Style")) looks_menu_args = true;
            if (hc && (std::strstr(hc, "ResourceRef") ||
                       std::strstr(hc, "GameRef")))
              looks_additem = true;
            if (hc && (std::strstr(hc, "Font") || std::strstr(hc, "Text")))
              looks_text = true;
            if ((!hc || !hc[0] || std::strstr(hc, "String")) &&
                string_cstr(v.v.o))
              looks_text = true;
          } else if (v.tag == JvmTag::Float)
            has_float = true;
        }
        // Garage.createOSDObjects keeps buttonStyle in a local — stack is
        // often just floats[+ori] under Osd (sty=0). Floats + !RR ⇒ createMenu.
        // Do not steal createText (String/Font + floats).
        if (!looks_menu_args) {
          for (const JvmValue& loc : locals) {
            if (loc.tag != JvmTag::Obj || !loc.v.o) continue;
            const char* hc = tree_host_class(loc.v.o);
            if (hc && std::strstr(hc, "Style")) {
              looks_menu_args = true;
              break;
            }
          }
        }
        if ((looks_menu_args || has_float) && has_float && !looks_additem &&
            !looks_text) {
          pending_invoke_name.clear();
          return "createMenu";
        }
        if (looks_additem && last_menu) {
          pending_invoke_name.clear();
          return "addItem";
        }
      }
    }
    // Icon addItem only after createMenu. Skip leftover Style (buttonStyle)
    // left on the stack after createMenu / packing sugar.
    auto stack_looks_additem = [&]() -> bool {
      if (!last_menu || stack.empty()) return false;
      size_t top_i = 0;
      while (top_i < 3 && top_i < stack.size()) {
        const JvmValue& v =
            stack[stack.size() - 1 - top_i];
        if (v.tag == JvmTag::Obj && v.v.o) {
          const char* hc = tree_host_class(v.v.o);
          if (hc && std::strstr(hc, "Style")) {
            ++top_i;
            continue;
          }
        }
        break;
      }
      if (top_i >= stack.size()) return false;
      const JvmValue& top = stack[stack.size() - 1 - top_i];
      if (top.tag == JvmTag::Obj && top.v.o) {
        const char* hc = tree_host_class(top.v.o);
        if (hc && std::strstr(hc, "Osd")) return false;
        if (hc && std::strstr(hc, "Menu")) return false;
        if (hc && (std::strstr(hc, "ResourceRef") || std::strstr(hc, "GameRef")))
          return true;
        if (!hc || !hc[0] || std::strstr(hc, "String")) {
          bool rr = false, cmd = false;
          for (size_t i = top_i + 1;
               i < top_i + 5 && i < stack.size(); ++i) {
            const JvmValue& v = stack[stack.size() - 1 - i];
            if (v.tag == JvmTag::Int) cmd = true;
            if (v.tag == JvmTag::Obj && v.v.o) {
              const char* vh = tree_host_class(v.v.o);
              if (vh && (std::strstr(vh, "ResourceRef") ||
                         std::strstr(vh, "GameRef")))
                rr = true;
            }
          }
          return rr && cmd;
        }
      }
      // RR buried under tip/cmd with Style above.
      for (size_t i = top_i; i < top_i + 5 && i < stack.size(); ++i) {
        const JvmValue& v = stack[stack.size() - 1 - i];
        if (v.tag != JvmTag::Obj || !v.v.o) continue;
        const char* vh = tree_host_class(v.v.o);
        if (vh && (std::strstr(vh, "ResourceRef") ||
                   std::strstr(vh, "GameRef")))
          return true;
      }
      return false;
    };
    if (stack_looks_additem()) {
      pending_invoke_name.clear();
      return "addItem";
    }
    // Garage icon addItem: ResourceRef stays in recent_new after NEW+0x21
    // (not pushed). Tip+CMD are on the stack — without this, mname stays the
    // broken 0x12 pool string (" 3.5") and the invoke is dropped (no 109/110…).
    if (last_menu && recent_new) {
      const char* rhc = tree_host_class(recent_new);
      if (rhc && (std::strstr(rhc, "ResourceRef") ||
                  std::strstr(rhc, "GameRef"))) {
        bool cmd = false;
        for (size_t i = 0; i < 6 && i < stack.size(); ++i) {
          const JvmValue& v = stack[stack.size() - 1 - i];
          if (v.tag == JvmTag::Int && is_icon_osd_cmd(v.v.i)) {
            cmd = true;
            break;
          }
        }
        if (cmd) {
          pending_invoke_name.clear();
          return "addItem";
        }
      }
    }
    if (!pending_invoke_name.empty()) {
      // Stale "createMenu" string before icon addItem (Garage TREE pool).
      if (pending_invoke_name == "createMenu" && last_menu) {
        bool rr = false, cmd = false;
        for (size_t i = 0; i < 6 && i < stack.size(); ++i) {
          const JvmValue& v = stack[stack.size() - 1 - i];
          if (v.tag == JvmTag::Int && is_icon_osd_cmd(v.v.i)) cmd = true;
          if (v.tag != JvmTag::Obj || !v.v.o) continue;
          const char* hc = tree_host_class(v.v.o);
          if (hc && (std::strstr(hc, "ResourceRef") ||
                     std::strstr(hc, "GameRef")))
            rr = true;
        }
        if (rr || cmd) {
          pending_invoke_name.clear();
          return "addItem";
        }
      }
      mname = pending_invoke_name;
      pending_invoke_name.clear();
      return mname;
    }
    // Method name may still sit on the stack (0x09 pushed before packing).
    for (int i = 0; i < 4 && !stack.empty(); ++i) {
      const JvmValue& top = stack.back();
      if (top.tag != JvmTag::Obj || !top.v.o) break;
      const char* hc = tree_host_class(top.v.o);
      if (hc && hc[0]) break;  // not a bare String
      const char* s = string_cstr(top.v.o);
      if (s && is_osd_invoke_name(s)) {
        mname = s;
        pop();
        return mname;
      }
      break;
    }
    return mname;
  };

  // INSTANCEOF after 0x07/32 + TYPE(pending_class). Object from stack or last local.
  auto eval_instanceof = [&]() -> int32_t {
    InvObject* obj = nullptr;
    if (!stack.empty() && stack.back().tag == JvmTag::Obj)
      obj = pop().v.o;
    else if (last_loaded_local >= 0 &&
             static_cast<size_t>(last_loaded_local) < locals.size() &&
             locals[static_cast<size_t>(last_loaded_local)].tag == JvmTag::Obj)
      obj = locals[static_cast<size_t>(last_loaded_local)].v.o;
    if (!obj || pending_class.empty()) return 0;
    const char* hc = tree_host_class(obj);
    if (!hc || !hc[0]) return 0;
    if (std::strstr(hc, pending_class.c_str()) != nullptr) return 1;
    const char* slash = std::strrchr(pending_class.c_str(), '.');
    const char* simple = slash ? slash + 1 : pending_class.c_str();
    return (simple && std::strstr(hc, simple)) ? 1 : 0;
  };

  auto cstr = [&](uint32_t imm) -> std::string {
    if (imm < const_strings.size()) return const_strings[imm];
    return {};
  };
  auto field_name = [&](uint32_t imm) -> std::string {
    if (imm < const_mref_name.size() && !const_mref_name[imm].empty())
      return const_mref_name[imm];
    return cstr(imm);
  };
  // Soft TREE field/method fidelity (PE VMThread_run @ 0x420FF0):
  //   op29 @ 0x4214BC → Object_getField @ 0x408800 / Class_getFieldByName
  //   @ 0x404820 — miss logs "illegal fieldaccess" (null.%s / unknown) and
  //   still pushes null/0; never allocates a field slot (sub_403840 lookup).
  //   Soft put may create a host bag slot (write path); soft get must not.
  //   op33 @ 0x421254 → Object_callMethod @ 0x408A30 — name-only invoke;
  //   descriptors / FQNs leaked via empty mref are not methods.
  auto fname_is_garbage = [](const std::string& s) -> bool {
    if (s.empty()) return true;
    if (s[0] == '(' || s[0] == '[' || s[0] == 'L') return true;
    if (s.find(' ') != std::string::npos || s.find(',') != std::string::npos)
      return true;
    // Simple field/method id only — FQN leaked from empty mref is not a name.
    if (s.find('.') != std::string::npos) return true;
    return false;
  };
  auto soft_getfield = [&](InvObject* self, const std::string& fname) {
    // PE Object_getField miss → push null/0. Never create on get.
    // Null receiver / garbage name → same (PE null.%s / unknown fieldname).
    // op29 @ 0x4214BC: miss still ValueStack_push (null) then continue.
    if (fname_is_garbage(fname) || !self) {
      push(JvmValue::make_int(0));
      return;
    }
    // Vector.length via tree_fields vector bag (call, don't rewrite fields).
    if (fname == "length" && tree_vector_is(self)) {
      push(JvmValue::make_int(tree_vector_size(self)));
      return;
    }
    if (JvmValue* slot = tree_field_slot(self, fname, /*create=*/false))
      push(*slot);
    else
      push(JvmValue::make_int(0));
  };
  auto soft_putfield = [&](InvObject* self, const std::string& fname,
                           const JvmValue& val) {
    // Soft put: invent host bag slot on write only (get never does).
    if (fname_is_garbage(fname) || !self) return;
    if (JvmValue* slot = tree_field_slot(self, fname, /*create=*/true)) *slot = val;
  };
  // Soft invoke name gate ≡ PE Object_callMethod name-only resolve.
  auto soft_invoke_name_ok = [&](const std::string& mname) -> bool {
    return !mname_is_garbage(mname);
  };
  // Soft expression results that Soft/pending VMThread consumers need on the
  // stack (RunThreadsBudgeted → vmthread_run → TREE). PE VMThread_invokeMethod
  // @ 0x41FBC0 (ticket VA 0x420000 inside): native ret push @ 0x420129 and
  // empty-body default Int0 @ 0x42066c always ValueStack_push when
  // NativeSigDesc_retType ≠ 0. Soft allowlist approximates non-V keep on 0x12.
  // OSD create* that poison 0x1a statement chains stay on the 0x12 keep path.
  auto soft_invoke_is_expr = [](const std::string& m) -> bool {
    return m == "getFirstChild" || m == "getNextChild" || m == "first" ||
           m == "next" || m == "elementAt" || m == "size" || m == "intValue" ||
           m == "exists" || m == "getPos" || m == "getOri" || m == "getVel" ||
           m == "toString" || m == "id" || m == "enable" || m == "createMenu" ||
           m == "beginGroup" || m == "endGroup" || m == "nextToken" ||
           m == "countTokens" || m == "token" || m == "autoSave" ||
           m == "displayModeName" || m == "numDisplayModes" ||
           m == "currDisplayMode" || m == "getVolume" || m == "getRouteLength" ||
           m == "getRoutePos" || m == "getRouteDist" || m == "getNearestCross" ||
           m == "getStartDirection" || m == "getTime" || m == "getAxis" ||
           m == "currentLine" || m == "add" || m == "mul" || m == "sub" ||
           m == "diff" || m == "createCar" || m == "createQuickRaceBot" ||
           // Hot non-V getters (Java call sites; Soft 0x12 keep ≡ PE ret push).
           m == "alignToRoad" || m == "getChildNodes" || m == "getParent" ||
           m == "getMin" || m == "getMax" || m == "getCM" || m == "clone" ||
           m == "getMatrix" || m == "getAngVel" || m == "getParentID";
  };
  // 0x12 OSD builders often omit POP — keep return; 0x1a statement form must not.
  auto soft_invoke_is_osd_builder_result = [](const std::string& m) -> bool {
    return m == "createButton" || m == "createText" || m == "createTextBox" ||
           m == "createHotkey" || m == "display";
  };
  // PE @ 0x41FBC0: instance method + null a4 → "no instance" log, ret -1;
  // op33/op36 test eax / jnz continues — Soft must not abort the TREE frame.
  auto soft_invoke_null_recv = [](bool is_static,
                                  const std::vector<JvmValue>& args) -> bool {
    if (is_static) return false;
    if (args.empty()) return true;
    return args[0].tag != JvmTag::Obj || args[0].v.o == nullptr;
  };
  // Set when soft_call_by_name skips for null recv (PE ret -1, no stack push).
  bool soft_invoke_null_recv_skip = false;
  // Commit Soft invoke result: pending_local store and/or stack keep.
  // PE empty-body @ 0x42066c: retType≠0 → ValuePool Int0 + ValueStack_push.
  // Soft: Void + (expr | pending_local | always_nonvoid) → Int0, except
  // null-recv skip (PE never reaches empty-body / native push).
  // always_nonvoid: Soft 0x19/0x25 leave non-V on stack (not Soft 0x1a).
  auto soft_invoke_commit = [&](const std::string& mname, JvmValue r,
                                bool keep_extra,
                                bool always_nonvoid = false) -> JvmValue {
    const bool expr = soft_invoke_is_expr(mname);
    if (r.tag == JvmTag::Void && !soft_invoke_null_recv_skip &&
        (expr || pending_local >= 0 || always_nonvoid))
      r = JvmValue::make_int(0);
    if (pending_local >= 0 && r.tag != JvmTag::Void) {
      ensure_local(static_cast<size_t>(pending_local));
      locals[static_cast<size_t>(pending_local)] = r;
      if (name_local < 0) name_local = pending_local;
      pending_local = -1;
    } else if (r.tag != JvmTag::Void &&
               (keep_extra || expr || always_nonvoid)) {
      push(r);
    }
    soft_invoke_null_recv_skip = false;
    return r;
  };
  // Soft call_by_name with PE null-recv / name gates. Does not invent bytecode.
  auto soft_call_by_name = [&](const char* owner, const std::string& mname,
                               const std::vector<JvmValue>& args,
                               bool is_static) -> JvmValue {
    soft_invoke_null_recv_skip = false;
    if (!soft_invoke_name_ok(mname)) return JvmValue::make_void();
    // PE Object_callInitIf @ 0x408A90: null clazz → ret 1, no Thread_callMethod.
    // Soft <init> with empty owner ≡ that gate (continue, no abort).
    if (mname == "<init>" && (!owner || !owner[0]))
      return JvmValue::make_void();
    if (soft_invoke_null_recv(is_static, args)) {
      // PE Object_callMethod → Thread_callMethod → invokeMethod null a4 @
      // 0x41FBC0: continue without nested frame (ret -1). Soft: no call,
      // no empty-body Int0 push @ 0x42066c.
      soft_invoke_null_recv_skip = true;
      return soft_invoke_is_expr(mname) ? JvmValue::make_int(0)
                                        : JvmValue::make_void();
    }
    return host->call_by_name(owner, mname.c_str(), args, is_static);
  };
  // Soft ≡ PE Object_callInitIf @ 0x408A90 via op35 @ 0x4212A4:
  //   clazz!=0 → Thread_callMethod "<init>"; else ret 1 (continue).
  // Soft TREE keeps the gate local (no thr / vmthread_op35_* invent).
  auto soft_init_clazz = [&](InvObject* self) -> const char* {
    const char* cn = tree_host_class(self);
    if (cn && cn[0]) return cn;
    if (!pending_class.empty()) return pending_class.c_str();
    return nullptr;  // ≡ PE a3==0
  };
  auto soft_call_init_if = [&](const char* clazz,
                               const std::vector<JvmValue>& args) {
    if (!clazz || !clazz[0]) return;  // ≡ PE ret 1 — no nested frame
    host->call_by_name(clazz, "<init>", args, false);
  };

  auto resolve_rid = [&](uint32_t imm) -> int32_t {
    if (imm >= const_int_valid.size() || !const_int_valid[imm]) return 0;
    const int32_t local = const_ints[imm];
    auto try_path = [&](std::string path) -> int32_t {
      if (path.empty() || path.find(".rpk") == std::string::npos) return 0;
      for (char& c : path)
        if (c == '/') c = '\\';
      std::string base = path;
      const auto slash = base.find_last_of('\\');
      if (slash != std::string::npos) base = base.substr(slash + 1);
      const RpakPack* pack = rpak_find_by_name(base.c_str());
      if (!pack) {
        java_lang_System_openLib(string_new(path.c_str()));
        pack = rpak_find_by_name(base.c_str());
      }
      if (!pack) return 0;
      return rpak_make_id(pack->pack_id, static_cast<uint16_t>(local & 0xFFFF));
    };
    if (imm < const_rid_pack.size() && !const_rid_pack[imm].empty()) {
      if (int32_t id = try_path(const_rid_pack[imm])) return id;
      // Garage icons: pack_idx → non-.rpk; frontend.rpk is nearby in CONS.
      for (int j = static_cast<int>(imm) - 1;
           j >= 0 && j + 8 >= static_cast<int>(imm); --j) {
        if (static_cast<size_t>(j) < const_strings.size()) {
          if (int32_t id = try_path(const_strings[static_cast<size_t>(j)]))
            return id;
        }
      }
    }
    return local;
  };


  // hideGroup(optionsGroup = endGroup()): putfield name often sits in a nearby
  // 0x1b hint — write the closed group id onto this.*Group when present.
  auto capture_endgroup_field = [&](int32_t gid, size_t at_ip) {
    if (gid < 0 || locals.empty() || locals[0].tag != JvmTag::Obj ||
        !locals[0].v.o)
      return;
    for (size_t j = at_ip + 1; j < tree.nodes.size() && j < at_ip + 10; ++j) {
      const TreeNode& tn = tree.nodes[j];
      if (tn.op != 0x1b || !tn.has_imm) continue;
      std::string fname = field_name(tn.imm);
      if (fname.size() < 5 || fname.find("Group") == std::string::npos)
        continue;
      tree_field_set_int(locals[0].v.o, fname.c_str(), gid);
      return;
    }
  };

  for (size_t ip = 0; ip < tree.nodes.size(); ++ip) {
    if (++steps > 2000000) {
      if (err) {
        *err = "TREE step limit at ip=" + std::to_string(ip) + "/" +
               std::to_string(tree.nodes.size());
      }
      return JvmValue::make_void();
    }
    const TreeNode& n = tree.nodes[ip];
    switch (n.op) {
      case 0x01: {  // LOCAL_LOAD
        if (!n.has_imm) break;
        ensure_local(n.imm);
        last_loaded_local = static_cast<int>(n.imm);
        push(locals[n.imm]);
        break;
      }
      case 0x02:  // declare / bind local index for upcoming NEW
        if (n.has_imm) {
          pending_local = static_cast<int>(n.imm);
          vm_local = pending_local;
        }
        break;
      case 0x03:
        break;
      case 0x04: {
        // AND short-circuit: cmp; 0x04; 0x14 +N — if falsy, keep 0 and goto.
        // OptionsDialog.show video filter: w>=0 && w<=2048 && …
        // Garage: pending ICMP_EQ (gameMode==SINGLECAR) must not survive into
        // the !majomParade IFEQ — that compared gm==1 and skipped the rest of
        // the street menu for FREERIDE/QUICKRACE/DEMO (ai=2).
        if (ip + 1 < tree.nodes.size() && tree.nodes[ip + 1].op == 0x14 &&
            tree.nodes[ip + 1].has_imm &&
            (tree.nodes[ip + 1].imm & 0x80000000u) == 0 && !stack.empty()) {
          const int32_t rel = static_cast<int32_t>(tree.nodes[ip + 1].imm);
          if (!tree_truthy(stack.back())) {
            if (pending_binop && *pending_binop == 7) pending_binop.reset();
            ip = ip + 1 + static_cast<size_t>(rel) - 1;
          } else {
            if (pending_binop && *pending_binop == 7) pending_binop.reset();
            pop();
            ++ip;  // skip forward GOTO; evaluate next conjunct
          }
          break;
        }
        break;
      }
      case 0x05: {
        // OR short-circuit: cmp; 0x05; 0x14 +N — if tree_truthy, keep 1 and goto.
        // OptionsDialog keyText: if (i < 10 || i > 14). Not INSTANCEOF.
        if ((!pending_binop || *pending_binop != 32) &&
            ip + 1 < tree.nodes.size() && tree.nodes[ip + 1].op == 0x14 &&
            tree.nodes[ip + 1].has_imm &&
            (tree.nodes[ip + 1].imm & 0x80000000u) == 0 && !stack.empty() &&
            stack.back().tag == JvmTag::Int) {
          const int32_t rel = static_cast<int32_t>(tree.nodes[ip + 1].imm);
          if (tree_truthy(stack.back())) {
            ip = ip + 1 + static_cast<size_t>(rel) - 1;
          } else {
            pop();
            ++ip;
          }
          break;
        }
        // INSTANCEOF: after LOCAL_LOAD + 0x07/32 + TYPE(pending_class).
        int32_t ok = eval_instanceof();
        // OR-chain accumulator (instanceof A || B || ...).
        if (!stack.empty() && stack.back().tag == JvmTag::Int) {
          if (pop().v.i) ok = 1;
        }
        push(JvmValue::make_int(ok));
        if (pending_binop) pending_binop.reset();
        break;
      }
      case 0x06: {  // type / class const
        if (!n.has_imm) break;
        std::string t = cstr(n.imm);
        if (t == "I" || t == "Z" || t == "B" || t == "C" || t == "S") {
          if (pending_local >= 0) {
            ensure_local(static_cast<size_t>(pending_local));
            locals[static_cast<size_t>(pending_local)] = JvmValue::make_int(0);
          }
        } else if (t == "F" || t == "D") {
          if (pending_local >= 0) {
            ensure_local(static_cast<size_t>(pending_local));
            locals[static_cast<size_t>(pending_local)] = JvmValue::make_float(0.f);
          }
        } else if (!t.empty() && t[0] == '[') {
          // ANEWARRAY / NEWARRAY: size on stack, then 0x07/31 + type.
          pending_class = t;
          if (pending_binop && *pending_binop == 31) {
            int32_t len = 0;
            if (!stack.empty()) {
              JvmValue top = pop();
              if (top.tag == JvmTag::Int) len = top.v.i;
              else if (top.tag == JvmTag::Float)
                len = static_cast<int32_t>(top.v.f);
            }
            if (len < 0) len = 0;
            if (len > 1 << 20) len = 1 << 20;
            push(JvmValue::make_obj(tree_array_new_desc(len, t.c_str())));
            pending_binop.reset();
          }
        } else if (!t.empty() && (t[0] == 'L' || t.find('.') != std::string::npos)) {
          pending_class = tree_strip_class_desc(t);
          if (pending_class == "java.render.osd.Rectangle")
            pending_class = "java.render.Rectangle";
          if (pending_local >= 0 && pending_class == "java.lang.String" &&
              ff_local >= 0 && name_local < 0) {
            name_local = pending_local;
          }
        }
        break;
      }
      case 0x07: {
        if (!n.has_imm) break;
        auto apply2 = [&](uint32_t op) -> bool {
          if (stack.size() < 2) return false;
          JvmValue b = pop();
          JvmValue a = pop();
          switch (op) {
            case 15: {  // ISUB
              if (a.tag == JvmTag::Float || b.tag == JvmTag::Float) {
                float af =
                    a.tag == JvmTag::Float ? a.v.f : static_cast<float>(a.v.i);
                float bf =
                    b.tag == JvmTag::Float ? b.v.f : static_cast<float>(b.v.i);
                push(JvmValue::make_float(af - bf));
              } else {
                push(JvmValue::make_int(a.v.i - b.v.i));
              }
              return true;
            }
            case 8: {  // ICMP_GE (i >= k) — for-loop i>=0
              push(JvmValue::make_int(a.v.i >= b.v.i ? 1 : 0));
              return true;
            }
            case 9: {  // ICMP_LE — OptionsDialog: for (i=10; i<=14; i++)
              push(JvmValue::make_int(a.v.i <= b.v.i ? 1 : 0));
              return true;
            }
            case 3: {  // IOR (VS_RRACE | VS_NRACE before VehicleModel NEW)
              push(JvmValue::make_int(a.v.i | b.v.i));
              return true;
            }
            case 5: {  // IAND
              push(JvmValue::make_int(a.v.i & b.v.i));
              return true;
            }
            case 10: {  // ICMP_GT
              push(JvmValue::make_int(a.v.i > b.v.i ? 1 : 0));
              return true;
            }
            case 11: {  // ICMP_LT
              push(JvmValue::make_int(a.v.i < b.v.i ? 1 : 0));
              return true;
            }
            default:
              push(a);
              push(b);
              return false;
          }
        };
        if (n.imm == 7) {
          pending_binop = 7;
        } else if (n.imm == 32) {
          pending_binop = 32;  // INSTANCEOF (resolved by OP_05 / IFEQ / NOT)
        } else if (n.imm == 21) {
          // NOT — Valocity.exit: !(next instanceof RaceSetup)
          int32_t v = 0;
          if (pending_binop && *pending_binop == 32 && !pending_class.empty()) {
            v = eval_instanceof();
            pending_binop.reset();
          } else if (!stack.empty()) {
            v = tree_truthy(pop()) ? 1 : 0;
          }
          push(JvmValue::make_int(v ? 0 : 1));
        } else if (n.imm == 1) {
          // OR — `instanceof A || instanceof B` after TYPE without OP_05.
          if (pending_binop && *pending_binop == 32 && !pending_class.empty()) {
            int32_t ok = eval_instanceof();
            pending_binop.reset();
            if (!stack.empty() && stack.back().tag == JvmTag::Int && pop().v.i)
              ok = 1;
            push(JvmValue::make_int(ok));
          } else {
            pending_binop = 1;
          }
        } else if (n.imm == 16) {
          pending_concat = true;
          pending_concat_args += 2;
        } else if (n.imm == 27 || n.imm == 28 || n.imm == 30) {
          // structural markers (thunk / cleanup / new-object hint)
          if (n.imm == 30 && pending_local < 0 && vm_local >= 0) {
            // Re-NEW into the same local (Baiern_VT: many VehicleModel in one slot).
            pending_local = vm_local;
          }
        } else if (n.imm == 22) {
          // FNEG — Valocity: posGarage coords (prefix before const or postfix).
          if (!stack.empty() && (stack.back().tag == JvmTag::Float ||
                                 stack.back().tag == JvmTag::Int)) {
            JvmValue v = pop();
            float f =
                v.tag == JvmTag::Float ? v.v.f : static_cast<float>(v.v.i);
            push(JvmValue::make_float(-f));
          } else {
            pending_binop = 22;
          }
        } else if (n.imm == 31) {
          // NEWARRAY / ANEWARRAY — size already on stack; type follows via 0x06.
          pending_binop = 31;
        } else if (n.imm == 2) {
          // Logical-AND marker between 0x04 short-circuit conjuncts
          // (OptionsDialog video mode filter). Combine is branchy, not stacky.
        } else if (apply2(n.imm)) {
          // binary op applied
        } else {
          pending_binop = n.imm;
        }
        break;
      }
      case 0x08: {
        if (n.has_imm && n.imm == 34) {
          // Completes this()/super() started by 0x22 / 0x23.
          // Soft ≡ PE op34 @ 0x4212C7 → Object_callMethod_init @ 0x408A70
          // (always Thread_callMethod "<init>"; no callInitIf null gate).
          // Clear pending always — never abort TREE.
          if (pending_special_init && !locals.empty() &&
              locals[0].tag == JvmTag::Obj && locals[0].v.o) {
            InvObject* self = locals[0].v.o;
            // Drop stray NEW result left for sugar.
            if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                stack.back().v.o != self) {
              const char* shc = tree_host_class(stack.back().v.o);
              if (!shc || !shc[0] || std::strcmp(shc, "java.lang.Object") == 0)
                pop();
            }
            const bool mixed_init =
                cls_name.find("Trigger") != std::string::npos ||
                pending_class.find("Trigger") != std::string::npos ||
                cls_name.find("GameRef") != std::string::npos ||
                cls_name.find("RenderRef") != std::string::npos;
            // this() leaves `this` on top; do not stop mixed packing at self.
            if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                stack.back().v.o == self)
              pop();
            std::vector<JvmValue> params;
            while (params.size() < 8 && !stack.empty()) {
              if (stack.back().tag == JvmTag::Obj) {
                if (!mixed_init) break;
                InvObject* o = stack.back().v.o;
                if (!o) {
                  params.push_back(pop());
                  continue;
                }
                const char* hc = tree_host_class(o);
                if (!hc || !hc[0] || std::strstr(hc, "GameRef") ||
                    std::strstr(hc, "ResourceRef") ||
                    std::strstr(hc, "Vector3") || std::strstr(hc, "String") ||
                    std::strstr(hc, "Ypr") || std::strstr(hc, "GroundRef")) {
                  params.push_back(pop());
                  continue;
                }
                break;
              }
              params.push_back(pop());
            }
            std::vector<JvmValue> args;
            args.push_back(JvmValue::make_obj(self));
            for (auto it = params.rbegin(); it != params.rend(); ++it)
              args.push_back(*it);
            // op34 pushes frame class (+0x34) — Soft: this=cls_name / super.
            const char* target = cls_name.c_str();
            if (pending_special_init == 2 && !super_name.empty())
              target = super_name.c_str();
            if (target && target[0])
              host->call_by_name(target, "<init>", args, false);
          }
          pending_special_init = 0;
          break;
        }
        if (n.has_imm && n.imm == 35) {
          // Soft imm 35 multiplexes local-store sugar + PE op35 callInitIf.
          // Local store sugar: `x = -0.4` → push val; load x; 0x08/35.
          // Must run before <init>/AASTORE — both also use imm 35.
          // Object assigns (`m = createMenu()`) are NOT handled here: both
          // stack slots are Obj and would collide with INVOKESPECIAL <init>.
          if (!pending_aastore && last_loaded_local >= 0 && stack.size() >= 2 &&
              stack.back().tag != JvmTag::Obj &&
              stack[stack.size() - 2].tag != JvmTag::Obj) {
            pop();  // discard current local value
            JvmValue val = pop();
            ensure_local(static_cast<size_t>(last_loaded_local));
            locals[static_cast<size_t>(last_loaded_local)] = val;
            break;
          }
          // Soft ≡ PE op35 @ 0x4212A4 → Object_callInitIf @ 0x408A90.
          // PUTFIELD is consumed via 0x1b lookahead. Leftover 0x08/35 is
          // INVOKESPECIAL <init>: stack = [..., arg0, arg1, recv].
          // Or AASTORE commit after 0x20 (value = recent_new / stack).
          // Null clazz → continue (no nested frame); never abort TREE.
          if (pending_aastore) {
            InvObject* val = recent_new;
            // `arr[i] = null` leaves aconst_null on the stack; prefer it over a
            // stale recent_new from an earlier NEW in the same TREE.
            if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                stack.back().v.o != pending_aastore_arr) {
              if (!val || stack.back().v.o == nullptr) {
                val = pop().v.o;
              }
            }
            if (val && val != pending_aastore_arr) {
              // Finish <init> when this is `arr[i] = new T(args)`.
              // ParkingCar(map, type, pos, ori, seed) is mixed Obj+prim —
              // primitive-only pops left argc=3 (leftover floats).
              const char* vhc = tree_host_class(val);
              const bool pcar =
                  (vhc && std::strstr(vhc, "ParkingCar")) ||
                  (!pending_class.empty() &&
                   pending_class.find("ParkingCar") != std::string::npos);
              const bool aastore_v3 =
                  (vhc && std::strstr(vhc, "Vector3")) ||
                  (!pending_class.empty() &&
                   pending_class.find("Vector3") != std::string::npos);
              std::vector<JvmValue> params;
              if (pcar) {
                while (params.size() < 5 && !stack.empty()) {
                  if (stack.back().tag == JvmTag::Obj) {
                    InvObject* o = stack.back().v.o;
                    if (!o) {
                      params.push_back(pop());
                      continue;
                    }
                    const char* hc = tree_host_class(o);
                    if (!hc || !hc[0] || std::strstr(hc, "ResourceRef") ||
                        std::strstr(hc, "GameRef") ||
                        std::strstr(hc, "Vector3") ||
                        std::strstr(hc, "String") || std::strstr(hc, "Ypr") ||
                        std::strstr(hc, "GroundRef")) {
                      params.push_back(pop());
                      continue;
                    }
                    break;
                  }
                  params.push_back(pop());
                }
              } else {
                const size_t prim_cap = aastore_v3 ? 3u : 8u;
                while (params.size() < prim_cap && !stack.empty() &&
                       stack.back().tag != JvmTag::Obj) {
                  params.push_back(pop());
                }
              }
              if (!params.empty() || pcar) {
                std::vector<JvmValue> args;
                args.push_back(JvmValue::make_obj(val));
                for (auto it = params.rbegin(); it != params.rend(); ++it)
                  args.push_back(*it);
                // Soft ≡ PE op35 Object_callInitIf — null clazz → skip, continue.
                soft_call_init_if(soft_init_clazz(val), args);
              }
            }
            if (pending_aastore_arr)
              tree_vector_set(pending_aastore_arr, pending_aastore_idx, val);
            pending_aastore = false;
            pending_aastore_arr = nullptr;
            recent_new = nullptr;
            break;
          }
          if (!stack.empty() && stack.back().tag == JvmTag::Obj) {
            JvmValue recv = pop();
            // Cap at 2 for general <init> (VT leftover prevalence+= on stack).
            // Vector3/Ypr (FFF) need 3 — detect via pending_class / host class.
            // Style(FF,charset,align,bg): charset/bg are Obj — must not stop at
            // first object or align stays default CENTER and Menu columns overlap.
            const char* peek_cn = tree_host_class(recv.v.o);
            if (!peek_cn || !peek_cn[0]) {
              peek_cn = pending_class.empty() ? "" : pending_class.c_str();
            }
            const bool style_init =
                (peek_cn && std::strstr(peek_cn, "Style") != nullptr) ||
                (!pending_class.empty() &&
                 pending_class.find("Style") != std::string::npos);
            const bool vehicle_model_init =
                (peek_cn && std::strstr(peek_cn, "VehicleModel") != nullptr) ||
                (!pending_class.empty() &&
                 pending_class.find("VehicleModel") != std::string::npos);
            const bool trigger_init =
                (peek_cn && std::strstr(peek_cn, "Trigger") != nullptr) ||
                (!pending_class.empty() &&
                 pending_class.find("Trigger") != std::string::npos);
            const bool parking_car_init =
                (peek_cn && std::strstr(peek_cn, "ParkingCar") != nullptr) ||
                (!pending_class.empty() &&
                 pending_class.find("ParkingCar") != std::string::npos);
            const bool renderref_init =
                ((peek_cn && std::strstr(peek_cn, "RenderRef") &&
                  !std::strstr(peek_cn, "Camera")) ||
                 (!pending_class.empty() &&
                  pending_class.find("RenderRef") != std::string::npos &&
                  pending_class.find("Camera") == std::string::npos));
            const bool gameref_init =
                (peek_cn && std::strstr(peek_cn, "GameRef")) ||
                (!pending_class.empty() &&
                 pending_class.find("GameRef") != std::string::npos);
            const bool vector3_init =
                (peek_cn && std::strstr(peek_cn, "Vector3")) ||
                (!pending_class.empty() &&
                 pending_class.find("Vector3") != std::string::npos);
            const bool ypr_init =
                !vector3_init &&
                ((peek_cn && std::strstr(peek_cn, "Ypr")) ||
                 (!pending_class.empty() &&
                  pending_class.find("Ypr") != std::string::npos));
            const bool rectangle_init =
                (peek_cn && std::strstr(peek_cn, "Rectangle")) ||
                (!pending_class.empty() &&
                 pending_class.find("Rectangle") != std::string::npos);
            const bool menu_init =
                ((peek_cn && std::strstr(peek_cn, "Menu") &&
                  !std::strstr(peek_cn, "MainMenu")) ||
                 (!pending_class.empty() &&
                  pending_class.find("Menu") != std::string::npos &&
                  pending_class.find("MainMenu") == std::string::npos));
            const size_t max_args =
                style_init ? 6u
                : menu_init ? 6u
                : trigger_init ? 5u
                : parking_car_init ? 5u
                : renderref_init ? 3u
                : gameref_init ? 4u
                : vehicle_model_init ? 2u
                : rectangle_init ? 3u
                : (vector3_init || ypr_init) ? 3u
                      : 2u;
            std::vector<JvmValue> params;
            if (vector3_init || ypr_init) {
              // Java Vector3(FFF) / copy / native Vector3(Ypr) @ 0x00482020.
              if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                  stack.back().v.o &&
                  (tree_is_vector3(stack.back().v.o) ||
                   tree_is_ypr(stack.back().v.o))) {
                params.push_back(pop());
              } else {
                while (params.size() < 3 && !stack.empty() &&
                       (stack.back().tag == JvmTag::Float ||
                        stack.back().tag == JvmTag::Int))
                  params.push_back(pop());
              }
            } else {
            while (params.size() < max_args && !stack.empty()) {
              if (!style_init && !trigger_init && !gameref_init &&
                  !renderref_init && !parking_car_init && !rectangle_init &&
                  !menu_init) {
                if (stack.back().tag == JvmTag::Obj) break;
                params.push_back(pop());
                continue;
              }
              // Style / Trigger / ParkingCar / GameRef / Rectangle / Menu:
              // mixed Obj+prim.
              if (stack.back().tag == JvmTag::Obj) {
                InvObject* o = stack.back().v.o;
                if (!o) {
                  params.push_back(pop());
                  continue;
                }
                const char* hc = tree_host_class(o);
                // vec3_new / gameref_new leave empty host-class.
                if (!hc || !hc[0] || std::strstr(hc, "ResourceRef") ||
                    std::strstr(hc, "GameRef") || std::strstr(hc, "Vector3") ||
                    std::strstr(hc, "String") || std::strstr(hc, "Ypr") ||
                    std::strstr(hc, "GroundRef") ||
                    (rectangle_init && (std::strstr(hc, "Template") ||
                                        std::strstr(hc, "Group") ||
                                        std::strstr(hc, "Osd"))) ||
                    (menu_init && (std::strstr(hc, "Osd") ||
                                   std::strstr(hc, "Style")))) {
                  params.push_back(pop());
                  continue;
                }
                break;
              }
              params.push_back(pop());
            }
            }
            std::vector<JvmValue> args;
            args.push_back(recv);
            for (auto it = params.rbegin(); it != params.rend(); ++it)
              args.push_back(*it);
            // Soft ≡ PE Object_callInitIf: host_class / pending_class only.
            // Do NOT fall back to enclosing cls_name (freestyle ≠ PE clazz).
            soft_call_init_if(soft_init_clazz(recv.v.o), args);
            // Soft put after NEW+0x21+0x1B hint — same as PE putfield name gate.
            if (!pending_putfield.empty() && recv.tag == JvmTag::Obj &&
                !locals.empty() && locals[0].tag == JvmTag::Obj) {
              soft_putfield(locals[0].v.o, pending_putfield, recv);
              pending_putfield.clear();
            }
          }
        } else if (n.has_imm && n.imm == 39) {
          // FADD statement: y += 0.12 (OptionsDialog keyText) OR prevalence +=.
          JvmValue b = pop();
          JvmValue a = pop();
          float af =
              a.tag == JvmTag::Float ? a.v.f : static_cast<float>(a.v.i);
          float bf =
              b.tag == JvmTag::Float ? b.v.f : static_cast<float>(b.v.i);
          JvmValue sum = JvmValue::make_float(af + bf);
          if (last_loaded_local >= 0) {
            ensure_local(static_cast<size_t>(last_loaded_local));
            JvmValue& loc = locals[static_cast<size_t>(last_loaded_local)];
            if (loc.tag == JvmTag::Float || loc.tag == JvmTag::Int) {
              loc = sum;
              break;
            }
          }
          if (!locals.empty() && locals[0].tag == JvmTag::Obj) {
            if (JvmValue* slot =
                    tree_field_slot(locals[0].v.o, "prevalence", true)) {
              *slot = sum;
            }
          }
        } else if (n.has_imm && n.imm == 46) {
          // Statement: vehicleSetMask |= … on this (VehicleType.init).
          JvmValue b = pop();
          JvmValue a = pop();
          JvmValue r = JvmValue::make_int(a.v.i | b.v.i);
          if (!locals.empty() && locals[0].tag == JvmTag::Obj) {
            if (JvmValue* slot =
                    tree_field_slot(locals[0].v.o, "vehicleSetMask", true)) {
              *slot = r;
            }
          }
        } else if (n.has_imm && n.imm == 23) {
          // IINC last_loaded (for-loop i++ — OptionsDialog video/keyText).
          const int li = last_loaded_local >= 0 ? last_loaded_local : 1;
          ensure_local(static_cast<size_t>(li));
          JvmValue& loc = locals[static_cast<size_t>(li)];
          if (!stack.empty() && stack.back().tag == JvmTag::Int) {
            JvmValue v = pop();
            v.v.i += 1;
            loc = v;
          } else if (loc.tag == JvmTag::Int) {
            loc.v.i += 1;
          } else if (loc.tag == JvmTag::Float) {
            loc.v.f += 1.f;
          }
        } else if (n.has_imm && n.imm == 24) {
          // IDEC last_loaded / local 1 (for-loop i--).
          const int li = last_loaded_local >= 0 ? last_loaded_local : 1;
          ensure_local(static_cast<size_t>(li));
          if (locals[static_cast<size_t>(li)].tag == JvmTag::Int)
            locals[static_cast<size_t>(li)].v.i -= 1;
          else if (locals[static_cast<size_t>(li)].tag == JvmTag::Float)
            locals[static_cast<size_t>(li)].v.f -= 1.f;
        } else if (n.has_imm && n.imm == 27) {
          // expression terminator — ignore
        } else if (last_loaded_local >= 0 && !stack.empty() &&
                   stack.back().tag == JvmTag::Int) {
          // Empirically: CAST local; STRING <n> → IINC local (rpksfound++).
          JvmValue v = pop();
          v.v.i += 1;
          ensure_local(static_cast<size_t>(last_loaded_local));
          locals[static_cast<size_t>(last_loaded_local)] = v;
        }
        break;
      }
      case 0x09: {  // STRING literal (prefer Utf8; ints use 0x0B / RID use 0x0E)
        if (!n.has_imm) break;
        // Garage.createOSDObjects: TREE imms for icon tip/RID are biased -6 vs
        // CONS (stock pairs start at frontend.rpk+RID; early imms hit camera
        // strings / createMenu). Shift only that band.
        uint32_t cimm = n.imm;
        if (cls_name.find("Garage") != std::string::npos && cimm >= 950 &&
            cimm < 1020)
          cimm += 6;
        // Garage icon TREE: tip slot sometimes holds a RID const (kind 3).
        if (cimm < const_rid_pack.size() && !const_rid_pack[cimm].empty()) {
          push(JvmValue::make_int(resolve_rid(cimm)));
          break;
        }
        std::string s = cstr(cimm);
        if (!s.empty()) {
          // Garage createOSDObjects: method name is a STRING before NEW+ARRAY+0x12.
          if (is_osd_invoke_name(s)) {
            bool invoke_soon = false;
            for (size_t j = ip + 1; j < tree.nodes.size() && j < ip + 10; ++j) {
              const uint8_t op = tree.nodes[j].op;
              if (op == 0x12 || op == 0x1a || op == 0x10) {
                invoke_soon = true;
                break;
              }
              if (op == 0x27 || op == 0x24 || op == 0x11 || op == 0x01 ||
                  op == 0x07 || op == 0x08)
                continue;
              break;
            }
            if (invoke_soon) {
              pending_invoke_name = s;
              break;
            }
          }
          push(JvmValue::make_obj(string_new(s.c_str())));
        } else if (cimm < const_int_valid.size() &&
                   const_int_valid[cimm]) {
          push(JvmValue::make_int(const_ints[cimm]));
        } else {
          push(JvmValue::make_obj(string_new("")));
        }
        break;
      }
      case 0x0a: {  // FLOAT const (IEEE bits in imm)
        if (!n.has_imm) break;
        float f = 0.f;
        std::memcpy(&f, &n.imm, sizeof(f));
        if (pending_binop && *pending_binop == 22) {
          f = -f;
          pending_binop.reset();
        }
        push(JvmValue::make_float(f));
        break;
      }
      case 0x0b: {  // INT immediate (imm is the value; locals use 0x01)
        if (!n.has_imm) break;
        push(JvmValue::make_int(static_cast<int32_t>(n.imm)));
        break;
      }
      case 0x0c:  // ACONST_NULL (MainMenu.exit: mmd = null)
        push(JvmValue::make_obj(nullptr));
        break;
      case 0x0e: {  // RID (kind 3) — or Utf8 tip (Garage icon TREE)
        if (!n.has_imm) break;
        uint32_t cimm = n.imm;
        if (cls_name.find("Garage") != std::string::npos && cimm >= 950 &&
            cimm < 1020)
          cimm += 6;
        if (cimm < const_rid_pack.size() && !const_rid_pack[cimm].empty()) {
          push(JvmValue::make_int(resolve_rid(cimm)));
        } else {
          std::string s = cstr(cimm);
          // Real tooltips are prose; coord fragments (" 0,0,0") and pack
          // names must not pollute the stack (they broke createMenu).
          const bool tip_like =
              !s.empty() && s[0] != ' ' && s.find(',') == std::string::npos &&
              s.find(".rpk") == std::string::npos && s.size() > 3;
          if (tip_like)
            push(JvmValue::make_obj(string_new(s.c_str())));
          else
            push(JvmValue::make_int(
                (cimm < const_int_valid.size() && const_int_valid[cimm])
                    ? resolve_rid(cimm)
                    : 0));
        }
        break;
      }
      case 0x10:
      case 0x11:
      case 0x12: {  // INVOKE / INVOKESPECIAL / INVOKESTATIC
        auto do_concat_n = [&](int pairs) {
          // pairs of (left,right) on stack; evaluate top pair first then deeper.
          std::vector<JvmValue> out;
          out.reserve(static_cast<size_t>(pairs));
          for (int i = 0; i < pairs; ++i) {
            JvmValue right = pop();
            JvmValue left = pop();
            out.push_back(JvmValue::make_obj(
                tree_concat_str(tree_value_as_string(left), tree_value_as_string(right))));
          }
          // out is top-first; push deeper first so top matches Java arg order.
          for (auto it = out.rbegin(); it != out.rend(); ++it) push(*it);
          pending_concat = false;
          pending_concat_args = 0;
          last_new = nullptr;
        };

        if (n.op == 0x11) {
          if (pending_concat) {
            const int pairs = std::max(1, pending_concat_args / 2);
            do_concat_n(pairs);
            break;
          }
          // City FILD: getstatic GameLogic.CLUBS + anewarray Vector3/Ypr.
          if (ip + 5 < tree.nodes.size() && n.has_imm &&
              tree.nodes[ip + 1].op == 0x19 && tree.nodes[ip + 2].op == 0x1b &&
              tree.nodes[ip + 3].op == 0x07 && tree.nodes[ip + 3].has_imm &&
              tree.nodes[ip + 3].imm == 31 && tree.nodes[ip + 4].op == 0x06 &&
              tree.nodes[ip + 5].op == 0x10) {
            pending_static_newarray = true;
            break;
          }
          const bool after_array =
              ip > 0 && tree.nodes[ip - 1].op == 0x24;  // ARRAY_INIT
          if (after_array) {
            last_new = nullptr;
            // ResourceRef call sugar: NEW+ARRAY+0x11 … 0x1A method-ref.
            bool method_ref = false;
            for (size_t j = ip + 1; j < tree.nodes.size() && j < ip + 8; ++j) {
              const TreeNode& peek = tree.nodes[j];
              if (peek.op == 0x1a && peek.has_imm) {
                const bool named =
                    peek.imm < const_mref_name.size() &&
                    (const_mref_name[peek.imm] == "addElement" ||
                     const_mref_name[peek.imm] == "getFirstChild" ||
                     const_mref_name[peek.imm] == "getNextChild" ||
                     const_mref_name[peek.imm] == "cache" ||
                     const_mref_name[peek.imm] == "size" ||
                     const_mref_name[peek.imm] == "elementAt" ||
                     const_mref_name[peek.imm] == "addColorIndex");
                if (named || peek.imm >= tree.nodes.size()) {
                  method_ref = true;
                  break;
                }
              }
              if (peek.op == 0x10 || peek.op == 0x12) break;
              if (peek.op == 0x19 || peek.op == 0x2b) break;
            }
            if (method_ref) {
              // Drop NEW packing object only; keep invoke arg under it (vmd).
              if (ip >= 2 && tree.nodes[ip - 2].op == 0x27 && !stack.empty() &&
                  stack.back().tag == JvmTag::Obj) {
                pop();
              }
              break;
            }
            // Also packing before 0x1A/0x12 Splash/Osd/GameType/GameLogic calls.
            bool call_mref = false;
            for (size_t j = ip + 1; j < tree.nodes.size() && j < ip + 8; ++j) {
              const TreeNode& peek = tree.nodes[j];
              if ((peek.op == 0x1a || peek.op == 0x12) && peek.has_imm &&
                  peek.imm < const_mref_name.size()) {
                const std::string& mn = const_mref_name[peek.imm];
                if (mn == "createBG" || mn == "createHotkey" ||
                    mn == "createButton" || mn == "createText" ||
                    mn == "createTextBox" || mn == "createHeader" ||
                    mn == "createMenu" || mn == "addItem" ||
                    mn == "addSeparator" || mn == "show" || mn == "hide" ||
                    mn == "setEventMask" || mn == "clearEventMask" ||
                    mn == "addTimer" || mn == "changeActiveSection" ||
                    mn == "osdCommand") {
                  call_mref = true;
                  break;
                }
              }
              // Bare string pool may hold the name when mref slot is empty.
              if ((peek.op == 0x1a || peek.op == 0x12) && peek.has_imm) {
                std::string mn = field_name(peek.imm);
                if (mn.empty()) mn = cstr(peek.imm);
                if (mn == "createBG" || mn == "createHotkey" ||
                    mn == "createButton" || mn == "createText" ||
                    mn == "createTextBox" || mn == "createHeader" ||
                    mn == "createMenu" || mn == "addItem" ||
                    mn == "addSeparator" || mn == "show" || mn == "hide" ||
                    mn == "setEventMask" || mn == "clearEventMask" ||
                    mn == "addTimer" || mn == "changeActiveSection" ||
                    mn == "osdCommand") {
                  call_mref = true;
                  break;
                }
              }
              if (peek.op == 0x10 || peek.op == 0x2b) break;
              // 0x19 often sits between packing and invoke (getstatic sugar).
            }
            if (call_mref) {
              if (ip >= 2 && tree.nodes[ip - 2].op == 0x27 && !stack.empty() &&
                  stack.back().tag == JvmTag::Obj) {
                pop();
              }
              break;
            }
            if (!stack.empty() && stack.back().tag == JvmTag::Obj) pop();

            // Packing sugar before INVOKESTATIC (e.g. first(path, FLAGS)) — not next.
            bool invoke_soon = false;
            for (size_t j = ip + 1; j < tree.nodes.size() && j < ip + 8; ++j) {
              const uint8_t op = tree.nodes[j].op;
              if (op == 0x10 || op == 0x12) {
                invoke_soon = true;
                break;
              }
              if (op == 0x19) continue;  // peek/getstatic sugar before invoke
              if (op == 0x1a || op == 0x2b) break;
            }
            if (invoke_soon) break;

            InvObject* ff = nullptr;
            const int idx = ff_local >= 0 ? ff_local : 2;
            if (static_cast<size_t>(idx) < locals.size() &&
                locals[static_cast<size_t>(idx)].tag == JvmTag::Obj) {
              ff = locals[static_cast<size_t>(idx)].v.o;
            }
            bool has_rel_goto = false;
            for (size_t j = ip + 1; j < tree.nodes.size() && j < ip + 15; ++j) {
              const TreeNode& peek = tree.nodes[j];
              if (peek.op == 0x14 && peek.has_imm && (peek.imm & 0x80000000u)) {
                has_rel_goto = true;
                break;
              }
              if (peek.op == 0x2b) break;
            }
            if (ff) {
              if (has_rel_goto) {
                JvmValue r = host->call_by_name("java.io.FindFile", "next",
                                                {JvmValue::make_obj(ff)}, false);
                push(r);
                const int ni = name_local >= 0 ? name_local : 3;
                ensure_local(static_cast<size_t>(ni));
                locals[static_cast<size_t>(ni)] = r;
              } else {
                host->call_by_name("java.io.FindFile", "close",
                                   {JvmValue::make_obj(ff)}, false);
              }
            }
            break;
          }
          if (last_new) {
            push(JvmValue::make_obj(last_new));
            last_new = nullptr;
            break;
          }
          break;
        }

        if (pending_concat) {
          do_concat_n(std::max(1, pending_concat_args / 2));
          break;
        }

        // End of getstatic+NEWARRAY FILD expression — leave array on stack.
        if (pending_static_newarray && n.op == 0x10) {
          pending_static_newarray = false;
          break;
        }

        std::string mname;
        if (n.has_imm) {
          mname = field_name(n.imm);
          if (mname.empty()) mname = cstr(n.imm);
        }
        mname = resolve_invoke_name(mname);
        // createMenu: getfield osd then 0x12 with broken imm (Garage TREE).
        // Only when stack below Osd looks like Style+floats — not addItem.
        if (mname_is_garbage(mname) && ip > 0) {
          for (int k = 1; k <= 3 && ip >= static_cast<size_t>(k); ++k) {
            const TreeNode& pn = tree.nodes[ip - static_cast<size_t>(k)];
            if (pn.op != 0x1b || !pn.has_imm) continue;
            std::string fn = field_name(pn.imm);
            if (fn.empty()) fn = cstr(pn.imm);
            if (fn == "osd") {
              // Style may be local-only; floats under Osd identify createMenu.
              // RR is addItem; String/Font is createText — ori int alone must
              // not veto.
              bool ok = false, bad = false, has_float = false, text = false;
              for (size_t i = 0; i < 5 && i < stack.size(); ++i) {
                const JvmValue& v = stack[stack.size() - 1 - i];
                if (v.tag == JvmTag::Obj && v.v.o) {
                  const char* hc = tree_host_class(v.v.o);
                  if (hc && std::strstr(hc, "Style")) ok = true;
                  if (hc && (std::strstr(hc, "ResourceRef") ||
                             std::strstr(hc, "GameRef")))
                    bad = true;
                  if (hc && (std::strstr(hc, "Font") || std::strstr(hc, "Text")))
                    text = true;
                  if ((!hc || !hc[0] || std::strstr(hc, "String")) &&
                      string_cstr(v.v.o))
                    text = true;
                } else if (v.tag == JvmTag::Float)
                  has_float = true;
              }
              if (!ok) {
                for (const JvmValue& loc : locals) {
                  if (loc.tag != JvmTag::Obj || !loc.v.o) continue;
                  const char* hc = tree_host_class(loc.v.o);
                  if (hc && std::strstr(hc, "Style")) {
                    ok = true;
                    break;
                  }
                }
              }
              if ((ok || has_float) && has_float && !bad && !text)
                mname = "createMenu";
              break;
            }
          }
        }
        if (mname.find('.') != std::string::npos && mname.find('(') == std::string::npos) {
          break;
        }
        // Soft invoke: PE Object_callMethod @ 0x408A30 — name-only; garbage → skip.
        if (!soft_invoke_name_ok(mname)) break;

        auto argc_for_method = [&](const std::string& mname) -> int {
          // 0x12 <init>: recv on top after NEW+0x11. argc=0 was dropping
          // Trigger(map,null,pos,alias) / GameRef(parent,rid,params,alias).
          if (mname.find(' ') != std::string::npos) return 0;
          if (mname == "<init>") {
            if (stack.empty() || stack.back().tag != JvmTag::Obj ||
                !stack.back().v.o)
              return -1;
            const char* hc = tree_host_class(stack.back().v.o);
            if (hc && std::strstr(hc, "Trigger")) {
              for (size_t i = 1; i < 6 && i < stack.size(); ++i) {
                if (stack[stack.size() - 1 - i].tag == JvmTag::Float)
                  return 5;
              }
              return 4;
            }
            if (hc && std::strstr(hc, "ParkingCar")) return 5;
            if (hc && std::strstr(hc, "Camera")) return -1;
            if (hc && std::strstr(hc, "RenderRef")) {
              if (stack.size() >= 4) return 3;
              return 1;
            }
            if (hc && std::strstr(hc, "GameRef")) {
              if (stack.size() >= 5) return 4;
              return 1;
            }
            if (hc && std::strstr(hc, "Rectangle")) return 3;
            if (hc && std::strstr(hc, "Menu") && !std::strstr(hc, "MainMenu")) {
              if (stack.size() >= 7 &&
                  stack[stack.size() - 2].tag == JvmTag::Int)
                return 6;
              return 5;
            }
            if (tree_is_vector3(stack.back().v.o) ||
                ((!hc || !hc[0]) &&
                 pending_class.find("Vector3") != std::string::npos)) {
              if (stack.size() >= 2 &&
                  stack[stack.size() - 2].tag == JvmTag::Obj &&
                  stack[stack.size() - 2].v.o &&
                  (tree_is_vector3(stack[stack.size() - 2].v.o) ||
                   tree_is_ypr(stack[stack.size() - 2].v.o)))
                return 1;
              int nf = 0;
              for (size_t i = 1; i < 4 && i < stack.size(); ++i) {
                const JvmValue& v = stack[stack.size() - 1 - i];
                if (v.tag == JvmTag::Float || v.tag == JvmTag::Int)
                  ++nf;
                else
                  break;
              }
              return nf > 0 ? nf : 0;
            }
            return -1;
          }
          if (mname == "create") {
            auto is_rr = [](InvObject* o) -> bool {
              if (!o) return false;
              const char* c = tree_host_class(o);
              if (!c || !c[0]) return false;
              if (std::strstr(c, "Camera")) return false;
              return std::strstr(c, "RenderRef") != nullptr;
            };
            if (!stack.empty() && stack.back().tag == JvmTag::Obj &&
                stack.back().v.o) {
              const char* tc = tree_host_class(stack.back().v.o);
              if (tc && std::strstr(tc, "Camera")) return -1;
              if (is_rr(stack.back().v.o)) return 3;
              const char* sc = string_cstr(stack.back().v.o);
              if (sc && (!tc || !tc[0] || std::strstr(tc, "String")) &&
                  stack.size() >= 4) {
                const JvmValue& maybe = stack[stack.size() - 4];
                if (maybe.tag == JvmTag::Obj && is_rr(maybe.v.o)) return 3;
              }
            }
            if (!pending_class.empty() &&
                pending_class.find("RenderRef") != std::string::npos &&
                pending_class.find("Camera") == std::string::npos)
              return 3;
            if (!locals.empty() && locals[0].tag == JvmTag::Obj &&
                is_rr(locals[0].v.o))
              return 3;
            return -1;
          }
          if (mname == "createBG") return 1;
          if (mname == "createHotkey") {
            // (key,flags,cmd,handler[+ef]); recv on top.
            if (stack.size() >= 6 && stack[stack.size() - 2].tag == JvmTag::Int &&
                stack[stack.size() - 3].tag == JvmTag::Obj)
              return 5;
            return 4;
          }
          if (mname == "show" || mname == "hide") {
            if (stack.size() >= 2 && stack[stack.size() - 2].tag == JvmTag::Int)
              return 1;
            return 0;
          }
          if (mname == "setEventMask" || mname == "clearEventMask") return 1;
          if (mname == "addTimer") return 2;
          if (mname == "addTraffic" || mname == "addTrafficN") return 5;
          if (mname == "queueEvent") return 3;
          if (mname == "enter" || mname == "exit") return 1;
          if (mname == "setPedestrianDensity" || mname == "setPedestrianDensityN")
            return 1;
          if (mname == "addPedestrianType" || mname == "remPedestrianType" ||
              mname == "removePedestrianType" || mname == "remTrafficCar" ||
              mname == "enableOsd")
            return 1;
          if (mname == "findRoute") return 2;
          if (mname == "getRoutePos" || mname == "getRouteDist") return 1;
          // Stock: getRouteLength(V3,V3)F caches route; ()F reads cache.
          if (mname == "getRouteLength") {
            if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
                stack[stack.size() - 2].tag == JvmTag::Obj)
              return 2;
            return 0;
          }
          if (mname == "getStartDirection") return 2;
          if (mname == "haltTrafficCross" || mname == "haltTrafficPath")
            return 2;
          if (mname == "startRace") return 3;
          if (mname == "startRace2") return 0;
          if (mname == "mul" || mname == "add" || mname == "sub" ||
              mname == "setParent")
            return 1;
          if (mname == "diff") return 2;
          if (mname == "getVel" || mname == "stop" ||
              mname == "createQuickRaceBot")
            return 0;
          if (mname == "createCar") {
            if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
                stack[stack.size() - 2].tag == JvmTag::Obj)
              return 2;
            return 1;
          }
          if (mname == "getNearestCross") {
            if (stack.size() >= 2 &&
                (stack.back().tag == JvmTag::Float ||
                 stack.back().tag == JvmTag::Int) &&
                stack[stack.size() - 2].tag == JvmTag::Obj)
              return 2;
            return 1;
          }
          if (mname == "alignToRoad" || mname == "time2Config") return 1;
          if (mname == "plotRoute") return 5;
          if (mname == "addMarker") {
            if (stack.size() >= 3 && stack.back().tag == JvmTag::Int &&
                stack[stack.size() - 2].tag == JvmTag::Obj &&
                stack[stack.size() - 3].tag == JvmTag::Obj)
              return 3;
            if (stack.size() >= 2 && stack.back().tag == JvmTag::Obj &&
                stack[stack.size() - 2].tag == JvmTag::Obj)
              return 2;
            return 1;
          }
          if (mname == "remMarker" || mname == "updateNavigator") return 1;
          if (mname == "timeWarp") return 1;
          if (mname == "changeZoom") return 1;
          if (mname == "changeSize") return 4;
          if (mname == "setWater") return 4;
          if (mname == "addWaterLimit" || mname == "addTrafficCar" ||
              mname == "notifyTrafficCar" || mname == "setTrafficCarBehaviour")
            return 2;
          if (mname == "addTrigger") return 6;
          if (mname == "addNotification") {
            // (ref,etype,ealias,msg) or + custmethod (City.startRace).
            if (stack.size() >= 5 && stack.back().tag == JvmTag::Obj) {
              const JvmValue& a2 = stack[stack.size() - 3];
              const JvmValue& a3 = stack[stack.size() - 4];
              if (a2.tag == JvmTag::Int && a3.tag == JvmTag::Int) return 5;
            }
            return 4;
          }
          if (mname == "prepareNightRace" || mname == "removeAllTimers" ||
              mname == "flush" || mname == "changeCamChase")
            return 0;
          if (mname == "createOSDObjects" || mname == "lockCar" ||
              mname == "releaseCar" || mname == "remSceneElements" ||
              mname == "endGroup" || mname == "beginGroup" ||
              mname == "display" || mname == "flushInventory" ||
              mname == "wakeUp" || mname == "precache" ||
              mname == "countTokens" || mname == "addCustomGroups" ||
              mname == "enableAnimateHook" || mname == "disableAnimateHook" ||
              mname == "addSeparator")
            return 0;
          if (mname == "changeMode" || mname == "addSceneElements" ||
              mname == "cameraSetup" || mname == "enable" ||
              mname == "addHandler" || mname == "remHandler" ||
              mname == "enableCameraControl" || mname == "filterInventory" ||
              mname == "scrollToLine" || mname == "command" ||
              mname == "setDamageMultiplier" || mname == "setCruiseControl" ||
              mname == "duplicate" || mname == "play" || mname == "nextToken" ||
              mname == "printValue" || mname == "setValue" ||
              mname == "setTicks" || mname == "setOffset" ||
              mname == "setToolTip" || mname == "token" ||
              mname == "hideGroup" || mname == "showGroup")
            return 1;
          if (mname == "setSliderStyle" || mname == "setRange") return 2;
          if (mname == "giveWarning" || mname == "getAxis" ||
              mname == "setMatrix")
            return 2;
          if (mname == "setSpeed" || mname == "setFade" || mname == "seek")
            return 1;
          if (mname == "createTextBox") return 6;
          if (mname == "createButton") {
            // (style,x,y,label,cmd) or (style,x,y,pri,label,cmd)