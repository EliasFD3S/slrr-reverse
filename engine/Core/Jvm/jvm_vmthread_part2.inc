// Fragment 2/2 of jvm_vmthread.cpp — included, not compiled on its own.
// Split only to stay under the 128 KB editor buffer cap; the
// translation unit is unchanged. See native/tools/split_source.py.


// Static twin of Thread_evalName — accepts 0x1019 classname + companion.
bool vmthread_op24_shape_walkable(const TreeBody& code, size_t idx) {
  if (idx + 1 >= code.nodes.size()) return false;
  const uint16_t hop = vmthread_pe_opcode(code.nodes[idx + 1]);
  if (hop == 0x101A) return true;
  const int32_t segs = static_cast<int32_t>(code.nodes[idx + 1].imm);
  if (segs <= 0 || segs > 64) return false;
  size_t pc = idx + 1;  // on the count node; segments follow
  int32_t remaining = segs;
  bool first = true;
  // Mirror the runtime: after the count node, walk `segs` segment slots,
  // collapsing 0x1019 + companion into one slot.
  while (remaining > 0) {
    if (++pc >= code.nodes.size()) return false;
    uint16_t op = vmthread_pe_opcode(code.nodes[pc]);
    if (first && op == 0x1019) {
      if (remaining <= 1) return false;
      if (++pc >= code.nodes.size()) return false;
      --remaining;
      op = vmthread_pe_opcode(code.nodes[pc]);
    }
    if (op == 0x1019) return false;
    --remaining;
    first = false;
  }
  return true;
}

// PE op 0x101E arrayInit @ 0x4232BB: imm=N; pop N elems into temp PtrVec;
// peek type-var (getType==2) else ScriptError; JVM_getArrayClass +
// Class_newInstance(N); assign elems via Value_assignOp('#') reverse;
// push array Value; N==0 → empty L-typed Value; opDefault_advance.
// Soft: pop N; peek TOS (leave on stack); tree_array_new(_desc); set Obj
// elems reverse (I/F assign OOS → null slot); push array; soft PC+=8.
void vmthread_op101E_array_init(VmThread* thr, int32_t count) {
  if (!thr) return;
  if (count < 0) count = 0;
  if (count > 256) count = 256;
  if (count == 0) {
    // PE @ 0x423465 arrayInit_empty: L-typed null Value → Soft empty array.
    vmthread_value_stack_push(thr, JvmValue::make_obj(tree_array_new(0)));
    thr->pe.unk_24 += 8;
    return;
  }
  std::vector<JvmValue> elems;
  elems.reserve(static_cast<std::size_t>(count));
  for (int32_t i = 0; i < count; ++i) {
    JvmValue v;
    if (!vmthread_pop_operand(thr, &v)) v = JvmValue{};  // PE ValueStack_pop
    elems.push_back(v);  // elems[0] = first pop = former TOS
  }
  // PE peeks type-var at TOS (getType==2); Soft: optional Obj class hint.
  const char* desc = "[Ljava.lang.Object;";
  if (thr->curr_frame && !thr->curr_frame->operand.empty()) {
    const JvmValue& top = thr->curr_frame->operand.back();
    if (top.tag == JvmTag::Obj && top.v.o) {
      const char* cn = tree_host_class(top.v.o);
      if (cn && cn[0] == '[') desc = cn;
    }
  }
  InvObject* arr = tree_array_new_desc(count, desc);  // PE Class_newInstance(N)
  // PE assigns from temp end → index 0..N-1 (source order). Soft reverse.
  for (int32_t i = 0; i < count; ++i) {
    const JvmValue& e = elems[static_cast<std::size_t>(count - 1 - i)];
    if (e.tag == JvmTag::Obj)
      tree_vector_set(arr, i, e.v.o);  // PE Value_assignOp('#')
    // I/F → SoftArray slot stays null (Value_assignOp primitive OOS).
  }
  vmthread_value_stack_push(thr, JvmValue::make_obj(arr));
  thr->pe.unk_24 += 8;  // PE → VMThread_opDefault_advance
}

// PE op 0x1003 @ 0x421823: operand stack underflow → "T_Container: underflow"
// ErrorLogMsgBox; else --count, slot cleared, Value vtbl[1] release(1);
// opDefault_advance. Soft: pop + discard (no Value* refcount).
void vmthread_op1003_pop_release(VmThread* thr) {
  JvmValue discarded;
  (void)vmthread_pop_operand(thr, &discarded);  // PE @ 0x42183B..0x421859
}

// PE op 0x1014 @ 0x423648: PC += TreeInsn_getOperand(insn) * 8 then
// opDefault_afterPc — unconditional relative jump (no extra advance).
void vmthread_op1014_goto(VmThread* thr, int32_t imm) {
  vmthread_pc_skip_by_operand(thr, imm);  // PE @ 0x423650 shl eax,3
}

// PE op 0x1027 @ 0x423677: ValuePool_alloc Value{Value_vtbl, typeDesc_I_box,
// payload 0}; ++JVM_liveObjectCount; payload = TreeInsn imm ([PC+4]); push;
// opDefault_advance. Soft: push make_int(imm) — ValuePool/refcount OOS.
void vmthread_op1027_box_imm(VmThread* thr, int32_t imm) {
  if (!thr) return;
  vmthread_value_stack_push(thr, JvmValue::make_int(imm));  // PE @ 0x4236B0
}

// PE op 0x1007 LITERAL @ 0x4218A5. insn.imm is a type tag (rejected when
// > 0x2E) routed through VMThread_literalTypeCaseMap @ 0x423A80 into
// jpt_4218B9 @ 0x4239FC. Tags 0x0C/0x1B/0x1D/0x22 select slot 32, which is
// VMThread_opDefault_advance itself — pure PC += 8. Tag 0x1C selects the
// scalar case 26 @ 0x4218C0: it reads the FOLLOWING node (insn+8) and pushes
// one Value built from that carrier's op and imm:
//   0x000C @ 0x42199B  typeDesc_L,     payload 0            (null)
//   0x100B @ 0x421948  typeDesc_I_box, payload carrier.imm
//   0x100D @ 0x4218F5  typeDesc_I,     payload carrier.imm
//   0x100E @ 0x4219E0  typeDesc_I_box, ConstantPool_resolveRid(cp, imm)
//                      → ResourceId_pack @ 0x4122B0
//                      → ResourceEngine_RemapFromPath @ 0x538440
//   0x200A @ 0x421A70  typeDesc_F,     carrier.imm as float bits
//   0x4009 @ 0x421A50  Value_boxCString(ConstantPool_getUtf8(cp, imm))
// Tag 0x1E case 27 @ 0x422F4B: carrier (stock always 0x1006) imm → getUtf8 →
// JVM_getClass @ 0x410890 → Class_boxObject @ 0x404E20 → push instance.
// Arith (no carrier): 0x16 neg, 0x10 add, 0x0F sub, 0x13 mul, 0x12 div,
// 0x15 lognot, 0x07 eq. Tag 0x1F NEWARRAY needs utf8 carrier (stock 0x1006).
//   other            JVM_ScriptError / Value_* arithmetic (OOS)
// The push @ 0x421AAE targets CallFrame+0x28, then opDefault_advance — the
// carrier node is reached next and falls through to opDefault_advance too.
bool vmthread_op1007_literal(VmThread* thr, const JvmClass* cls, int32_t tag,
                             const TreeNode* carrier, bool* skip_carrier) {
  if (skip_carrier) *skip_carrier = false;
  if (!thr) return false;
  if (tag == 0x0C || tag == 0x1B || tag == 0x1D || tag == 0x22)
    return true;  // jpt_4218B9 slot 32 == VMThread_opDefault_advance

  // PE tag 0x20 case 29 @ 0x422CB7: pop obj; I-box; thr+0x24+=8 onto
  // carrier; Class_isInheritedFrom_desc(obj+0xC, getUtf8(carrier));
  // payload 0/1; release; push; opDefault_advance (past carrier). Soft:
  // pop; isa check via super walk; push I; *skip_carrier=true.
  if (tag == 0x20) {
    if (!cls || !carrier) return false;
    if (carrier->imm >= cls->const_strings.size()) return false;
    const std::string want =
        vmthread_normalize_class_desc(cls->const_strings[carrier->imm].c_str());
    JvmValue v;
    if (!vmthread_pop_operand(thr, &v)) v = JvmValue{};
    int32_t ok = 0;
    if (v.tag == JvmTag::Obj && v.v.o && !want.empty()) {
      const char* have = tree_host_class(v.v.o);
      Jvm* j = jvm_active();
      if (have && have[0] && j) {
        if (want == have) {
          ok = 1;
        } else {
          if (!j->find_class(have)) j->load_class(have);
          const JvmClass* c = j->find_class(have);
          for (int g = 0; c && g < 64; ++g) {
            if (c->name == want) {
              ok = 1;
              break;
            }
            if (c->super_name.empty()) break;
            if (!j->find_class(c->super_name.c_str()))
              j->load_class(c->super_name.c_str());
            c = j->find_class(c->super_name.c_str());
          }
        }
      }
    }
    vmthread_value_stack_push(thr, JvmValue::make_int(ok));
    if (skip_carrier) *skip_carrier = true;
    return true;
  }

  // PE tag 0x21 case 30 @ 0x422D92: pop val; carrier 0x1006 utf8 or 0x1019
  // class CP → JVM_typecastValue; push; opDefault_advance (carrier stays
  // for default). Soft: I↔F; Obj keep if isa target FQN; else leave value.
  if (tag == 0x21) {
    if (!cls || !carrier) return false;
    JvmValue v;
    if (!vmthread_pop_operand(thr, &v)) v = JvmValue{};
    const uint16_t cop = vmthread_pe_opcode(*carrier);
    std::string want;
    if (cop == 0x1006) {
      if (carrier->imm >= cls->const_strings.size()) return false;
      want = vmthread_normalize_class_desc(
          cls->const_strings[carrier->imm].c_str());
    } else if (cop == 0x1019) {
      const JvmClass* tc = vmthread_resolve_class_from_cp(cls, carrier->imm);
      if (!tc) return false;
      want = tc->name;
    } else {
      return false;  // PE "invalid cast structure"
    }
    if (want == "I" || want == "int") {
      if (v.tag == JvmTag::Float)
        v = JvmValue::make_int(static_cast<int32_t>(v.v.f));
      else if (v.tag != JvmTag::Int)
        v = JvmValue::make_int(0);
    } else if (want == "F" || want == "float") {
      if (v.tag == JvmTag::Int)
        v = JvmValue::make_float(static_cast<float>(v.v.i));
      else if (v.tag != JvmTag::Float)
        v = JvmValue::make_float(0.f);
    } else if (!want.empty() && (want[0] == 'L' || want[0] == '[' ||
                                 want.find('.') != std::string::npos)) {
      // Soft object cast: null ok; else require isa (PE Class_isInheritedFrom).
      if (v.tag == JvmTag::Obj && v.v.o) {
        const char* have = tree_host_class(v.v.o);
        const std::string target = vmthread_normalize_class_desc(want.c_str());
        bool isa = false;
        Jvm* j = jvm_active();
        if (have && have[0] && j && !target.empty()) {
          if (target == have) {
            isa = true;
          } else {
            if (!j->find_class(have)) j->load_class(have);
            const JvmClass* c = j->find_class(have);
            for (int g = 0; c && g < 64; ++g) {
              if (c->name == target) {
                isa = true;
                break;
              }
              if (c->super_name.empty()) break;
              if (!j->find_class(c->super_name.c_str()))
                j->load_class(c->super_name.c_str());
              c = j->find_class(c->super_name.c_str());
            }
          }
        }
        if (!isa) v = JvmValue::make_obj(nullptr);
      }
    }
    vmthread_value_stack_push(thr, v);
    return true;
  }

  // PE tag 0x1E @ 0x422F4B: class literal from the next node's utf8 imm.
  if (tag == 0x1E) {
    if (!cls || !carrier) return false;
    const uint32_t imm = carrier->imm;
    if (imm >= cls->const_strings.size()) return false;
    const std::string fqn =
        vmthread_normalize_class_desc(cls->const_strings[imm].c_str());
    if (fqn.empty()) return false;
    // Soft Class_boxObject: ensure class loaded, then new instance.
    Jvm* j = jvm_active();
    if (j) {
      if (!j->find_class(fqn.c_str())) j->load_class(fqn.c_str());
    }
    InvObject* obj = tree_host_new(fqn.c_str());
    vmthread_value_stack_push(thr, JvmValue::make_obj(obj));
    return true;
  }

  // PE tag 0x16 case 21 @ 0x421F57: pop; if getType==2 ValuePool copy;
  // Value_neg @ 0x41B1D0 (F → -f, I → -i); push same Value*; advance.
  // Soft: pop; negate I/F in place on the soft value; push. No carrier.
  if (tag == 0x16) {
    JvmValue v;
    if (!vmthread_pop_operand(thr, &v)) v = JvmValue{};
    if (v.tag == JvmTag::Float)
      v.v.f = -v.v.f;
    else if (v.tag == JvmTag::Int)
      v.v.i = -v.v.i;
    else
      v = JvmValue::make_int(0);  // PE unsupported type → error path; soft 0
    vmthread_value_stack_push(thr, v);
    return true;
  }

  // Binary/unary Value_* arith tags (no carrier). Soft mirrors PE left/right
  // pop order: r=TOS, l=next. F promotion when either side is Float.
  // 0x13 mul@41B860, 0x0F sub@41B760, 0x12 div@41B970, 0x11 mod@41BBF0
  // (case16@422437). Shifts/xor are integral-only in PE (ScriptError else).
  if (tag == 0x13 || tag == 0x0F || tag == 0x12 || tag == 0x11 || tag == 0x0D ||
      tag == 0x0E || tag == 0x04) {
    JvmValue r;
    JvmValue l;
    if (!vmthread_pop_operand(thr, &r)) r = JvmValue{};
    if (!vmthread_pop_operand(thr, &l)) l = JvmValue{};
    const bool shift_xor_mod =
        (tag == 0x0D || tag == 0x0E || tag == 0x04 || tag == 0x11);
    if (!shift_xor_mod &&
        (l.tag == JvmTag::Float || r.tag == JvmTag::Float)) {
      const float lf =
          (l.tag == JvmTag::Float)
              ? l.v.f
              : (l.tag == JvmTag::Int ? static_cast<float>(l.v.i) : 0.f);
      const float rf =
          (r.tag == JvmTag::Float)
              ? r.v.f
              : (r.tag == JvmTag::Int ? static_cast<float>(r.v.i) : 0.f);
      float out = 0.f;
      if (tag == 0x13)
        out = lf * rf;  // Value_mul @ 0x41B860
      else if (tag == 0x0F)
        out = lf - rf;  // Value_sub @ 0x41B760 (neg r + add)
      else
        out = (rf == 0.f) ? 0.f : (lf / rf);  // Value_div @ 0x41B970
      vmthread_value_stack_push(thr, JvmValue::make_float(out));
      return true;
    }
    const int32_t li =
        (l.tag == JvmTag::Int)
            ? l.v.i
            : (l.tag == JvmTag::Float ? static_cast<int32_t>(l.v.f) : 0);
    const int32_t ri =
        (r.tag == JvmTag::Int)
            ? r.v.i
            : (r.tag == JvmTag::Float ? static_cast<int32_t>(r.v.f) : 0);
    int32_t out = 0;
    if (tag == 0x13)
      out = li * ri;
    else if (tag == 0x0F)
      out = li - ri;
    else if (tag == 0x12)
      out = (ri == 0) ? 0 : (li / ri);
    else if (tag == 0x11)
      out = (ri == 0) ? 0 : (li % ri);  // Value_mod @ 0x41BBF0
    else if (tag == 0x0D)
      out = li >> (ri & 31);  // Value_shr @ 0x41AFF0 / case13@421E0D
    else if (tag == 0x0E)
      out = li << (ri & 31);  // Value_shl @ 0x41B0E0 / case14@421EB2
    else
      out = li ^ ri;  // Value_bitxor @ 0x41C380 / case4@422834
    vmthread_value_stack_push(thr, JvmValue::make_int(out));
    return true;
  }

  // PE tag 0x15 case 20 @ 0x4224DC: pop; Value_lognot @ 0x41BDB0
  // (booleanConversion then I: payload = (payload==0)); push.
  if (tag == 0x15) {
    JvmValue v;
    if (!vmthread_pop_operand(thr, &v)) v = JvmValue{};
    vmthread_value_stack_push(
        thr, JvmValue::make_int(vmthread_value_getint_truthy(v) ? 0 : 1));
    return true;
  }

  // PE tag 0x14 case 19 @ 0x42253E: pop; Value_bitnot @ 0x41C0C0 (~I); push.
  if (tag == 0x14) {
    JvmValue v;
    if (!vmthread_pop_operand(thr, &v)) v = JvmValue{};
    const int32_t i =
        (v.tag == JvmTag::Int)
            ? v.v.i
            : (v.tag == JvmTag::Float ? static_cast<int32_t>(v.v.f) : 0);
    vmthread_value_stack_push(thr, JvmValue::make_int(~i));
    return true;
  }

  // PE ++/-- family (no carrier). Prefers ValueField (getType==2) else
  // ScriptError. Soft: pop_ref; mutate via ref_assign when ref is live.
  // 0x19 pre++ case25@421FB9 Value_inc then push copy
  // 0x1A pre-- case26@42202B Value_dec then push copy
  // 0x17 post++ case23@42209D push copy then Value_inc
  // 0x18 post-- case24@422120 push copy then Value_dec
  if (tag == 0x19 || tag == 0x1A || tag == 0x17 || tag == 0x18) {
    JvmValue v;
    VmValueRef ref;
    if (!vmthread_pop_operand_ref(thr, &v, &ref)) v = JvmValue{};
    const bool is_inc = (tag == 0x19 || tag == 0x17);
    const bool is_pre = (tag == 0x19 || tag == 0x1A);
    JvmValue old = v;
    JvmValue neu = v;
    if (v.tag == JvmTag::Float) {
      neu = JvmValue::make_float(v.v.f + (is_inc ? 1.f : -1.f));
    } else {
      const int32_t i =
          (v.tag == JvmTag::Int)
              ? v.v.i
              : (v.tag == JvmTag::Float ? static_cast<int32_t>(v.v.f) : 0);
      neu = JvmValue::make_int(i + (is_inc ? 1 : -1));
    }
    (void)vmthread_ref_assign(thr, ref, neu);  // PE Value_inc/dec in-place
    vmthread_value_stack_push(thr, is_pre ? neu : old);
    return true;
  }

  // PE cmp family (no carrier): pop r,l; Value_* mutates left→I bool; release
  // r; push l. 0x07 eq@0x41C5C0, 0x06 ne@0x41C6C0, 0x0B lt@0x41C9E0,
  // 0x0A gt@0x41C790, 0x09 le@0x41CB60, 0x08 ge@0x41C860.
  if (tag == 0x07 || tag == 0x06 || tag == 0x0B || tag == 0x0A || tag == 0x09 ||
      tag == 0x08) {
    JvmValue r;
    JvmValue l;
    if (!vmthread_pop_operand(thr, &r)) r = JvmValue{};
    if (!vmthread_pop_operand(thr, &l)) l = JvmValue{};
    bool ok = false;
    if (tag == 0x07)
      ok = vmthread_soft_value_eq(l, r);
    else if (tag == 0x06)
      ok = !vmthread_soft_value_eq(l, r);
    else if (tag == 0x0B)
      ok = vmthread_soft_value_cmp(l, r, '<');
    else if (tag == 0x0A)
      ok = vmthread_soft_value_cmp(l, r, '>');
    else if (tag == 0x09)
      ok = vmthread_soft_value_cmp(l, r, 'L');
    else
      ok = vmthread_soft_value_cmp(l, r, 'G');
    vmthread_value_stack_push(thr, JvmValue::make_int(ok ? 1 : 0));
    return true;
  }

  // PE tag 0x10 case 15 @ 0x4221A3: pop right, pop left; Value_add @
  // 0x41B4D0 (numericPromotion then F/I add, or Ljava.lang.String; concat);
  // release right; push left. Soft: pop r,l; I/F/String add; push. No carrier.
  if (tag == 0x10) {
    JvmValue r;
    JvmValue l;
    if (!vmthread_pop_operand(thr, &r)) r = JvmValue{};
    if (!vmthread_pop_operand(thr, &l)) l = JvmValue{};
    if (l.tag == JvmTag::Obj || r.tag == JvmTag::Obj) {
      const char* ls =
          (l.tag == JvmTag::Obj && l.v.o) ? string_cstr(l.v.o) : nullptr;
      const char* rs =
          (r.tag == JvmTag::Obj && r.v.o) ? string_cstr(r.v.o) : nullptr;
      std::string cat = ls ? ls : "";
      if (rs) cat += rs;
      vmthread_value_stack_push(thr, JvmValue::make_obj(string_new(cat.c_str())));
      return true;
    }
    if (l.tag == JvmTag::Float || r.tag == JvmTag::Float) {
      const float lf =
          (l.tag == JvmTag::Float)
              ? l.v.f
              : (l.tag == JvmTag::Int ? static_cast<float>(l.v.i) : 0.f);
      const float rf =
          (r.tag == JvmTag::Float)
              ? r.v.f
              : (r.tag == JvmTag::Int ? static_cast<float>(r.v.i) : 0.f);
      vmthread_value_stack_push(thr, JvmValue::make_float(lf + rf));
      return true;
    }
    const int32_t li = (l.tag == JvmTag::Int) ? l.v.i : 0;
    const int32_t ri = (r.tag == JvmTag::Int) ? r.v.i : 0;
    vmthread_value_stack_push(thr, JvmValue::make_int(li + ri));
    return true;
  }

  // PE logic/bitwise binary tags (no carrier): pop r,l; optional ValueField
  // copy; Value_*(jvm,l,r) mutates l; release r; push l.
  // 0x02 Value_land@0x41BEA0 (&&), 0x01 Value_lor@0x41BFB0 (||),
  // 0x03 Value_bitor@0x41C290 (|), 0x05 Value_bitand@0x41C1A0 (&).
  if (tag == 0x02 || tag == 0x01 || tag == 0x03 || tag == 0x05) {
    JvmValue r;
    JvmValue l;
    if (!vmthread_pop_operand(thr, &r)) r = JvmValue{};
    if (!vmthread_pop_operand(thr, &l)) l = JvmValue{};
    if (tag == 0x02 || tag == 0x01) {
      const bool lt = vmthread_value_getint_truthy(l);
      const bool rt = vmthread_value_getint_truthy(r);
      const bool ok = (tag == 0x02) ? (lt && rt) : (lt || rt);
      vmthread_value_stack_push(thr, JvmValue::make_int(ok ? 1 : 0));
      return true;
    }
    const int32_t li =
        (l.tag == JvmTag::Int)
            ? l.v.i
            : (l.tag == JvmTag::Float ? static_cast<int32_t>(l.v.f) : 0);
    const int32_t ri =
        (r.tag == JvmTag::Int)
            ? r.v.i
            : (r.tag == JvmTag::Float ? static_cast<int32_t>(r.v.f) : 0);
    const int32_t out = (tag == 0x03) ? (li | ri) : (li & ri);
    vmthread_value_stack_push(thr, JvmValue::make_int(out));
    return true;
  }

  // PE tag 0x23 '#' case 31 @ 0x421C71 (literalTypeCaseMap tags 0x23-0x2E
  // share this stub; op char = insn.imm): pop LHS (must getType==2 else
  // ScriptError); pop RHS (ValueField → ValuePool copy); Value_assignOp
  // (thr+0x14, LHS, RHS, op); push RHS; advance. Soft: '#' only via
  // ref_assign; compound $/%/&/... stay OOS. Stack order is LHS-on-TOS
  // (opposite of op45).
  if (tag == 0x23) {
    JvmValue lhs;
    JvmValue rhs;
    VmValueRef lhs_ref;
    if (!vmthread_pop_operand_ref(thr, &lhs, &lhs_ref)) lhs = JvmValue{};
    if (!vmthread_pop_operand(thr, &rhs)) rhs = JvmValue{};
    (void)vmthread_ref_assign(thr, lhs_ref, rhs);  // PE Value_assignOp('#')
    vmthread_value_stack_push(thr, rhs);          // PE @ 0x421CE0 push RHS
    return true;
  }

  // PE tag 0x1F case 28 @ 0x421AD6 NEWARRAY: carrier utf8 desc; count '['
  // dims (0 → null-dim error, >1 → multi error); pop sizes; loop
  // JVM_getArrayClass + Class_newInstance; push. Soft: 1D only via
  // tree_array_new_desc (stock carriers are 0x1006).
  if (tag == 0x1F) {
    if (!cls || !carrier) return false;
    const uint32_t cimm = carrier->imm;
    if (cimm >= cls->const_strings.size()) return false;
    const std::string& desc = cls->const_strings[cimm];
    if (desc.empty() || desc[0] != '[') return false;
    int dims = 0;
    for (char c : desc) {
      if (c != '[') break;
      ++dims;
    }
    if (dims != 1) return false;  // PE ScriptError multi/null-dim
    JvmValue sz;
    if (!vmthread_pop_operand(thr, &sz)) sz = JvmValue{};
    int32_t n = 0;
    if (sz.tag == JvmTag::Int)
      n = sz.v.i;
    else if (sz.tag == JvmTag::Float)
      n = static_cast<int32_t>(sz.v.f);
    if (n < 0) n = 0;
    if (n > 65536) n = 65536;
    InvObject* arr = tree_array_new_desc(n, desc.c_str());
    vmthread_value_stack_push(thr, JvmValue::make_obj(arr));
    return true;
  }

  if (tag != 0x1C || !carrier) return false;
  const uint32_t imm = carrier->imm;
  switch (vmthread_pe_opcode(*carrier)) {
    case 0x000C:
      vmthread_value_stack_push(thr, JvmValue::make_obj(nullptr));
      return true;
    case 0x100B:
    case 0x100D:
      vmthread_value_stack_push(thr,
                                JvmValue::make_int(static_cast<int32_t>(imm)));
      return true;
    case 0x100E: {
      // PE ConstantPool_resolveRid @ 0x406660 → ResourceId_pack @ 0x4122B0.
      // Soft: CONS kind-3 already unpacked into const_rid_pack / const_ints.
      if (!cls || imm >= cls->const_int_valid.size() ||
          !cls->const_int_valid[imm]) {
        vmthread_value_stack_push(thr, JvmValue::make_int(0));
        return true;
      }
      const uint32_t local =
          static_cast<uint32_t>(cls->const_ints[imm]) & 0xFFFFu;
      const char* path = nullptr;
      if (imm < cls->const_rid_pack.size() && !cls->const_rid_pack[imm].empty())
        path = cls->const_rid_pack[imm].c_str();
      const uint32_t rid = resource_engine_remap_from_path(path, local);
      vmthread_value_stack_push(
          thr, JvmValue::make_int(static_cast<int32_t>(rid ? rid : local)));
      return true;
    }
    case 0x200A: {
      float f = 0.f;
      std::memcpy(&f, &imm, sizeof f);  // PE fld dword ptr [carrier+4]
      vmthread_value_stack_push(thr, JvmValue::make_float(f));
      return true;
    }
    case 0x4009:
      if (!cls || imm >= cls->const_strings.size()) return false;
      vmthread_value_stack_push(
          thr, JvmValue::make_obj(string_new(cls->const_strings[imm].c_str())));
      return true;
    default:
      return false;
  }
}

// PE op 0x1008 statement dispatcher @ 0x422EDA: insn.imm is the statement tag;
// anything above 0x2E falls straight through to opDefault_advance @ 0x422EE0.
// Otherwise byte_423B04 (kVmStmtCaseMap) picks one of 12 bodies via jpt_422EEE.
// Hosted slots:
//   11 @0x4238ED  tags 0x0C/0x1D/0x22 — literally opDefault_advance
//    6 @0x422F40  tag 0x1C — stores insn+8 into a scratch slot, then advances;
//                 observably a no-op
//    5 @0x422EF5  tag 0x1B — ValueStack_pop + release: discard TOS
//    3 @0x4231CD  tags 0x17/0x19 — pop (getType==2); Value_inc @ 0x41B2D0;
//                 advance. Statement form: mutate in place, no push.
//    4 @0x423203  tags 0x18/0x1A — same with Value_dec @ 0x41B3D0.
//   10 @0x423059  tags 0x23-0x2E — pop dst (ScriptError "stack access: value on
//                 top (instead of variable)" unless getType()==2 @ 0x42306E),
//                 pop src, Value_assignOp(jvm+0x14, dst, src, tag) @ 0x4230C8.
//                 Soft: full PE switch (# $ % & ' ( , - .) via
//                 vmthread_assign_op_result; ) * + → no-op (PE unsupported).
// Returns false when the slot has no host body, so the walker bails before
// touching the stream.
bool vmthread_op1008_statement(VmThread* thr, int32_t tag) {
  if (!thr) return false;
  if (tag < 0 || tag > 0x2E) return true;  // PE @ 0x422EE0 ja → advance
  switch (kVmStmtCaseMap[tag]) {
    case 11:  // == VMThread_opDefault_advance
    case 6:
      return true;
    case 5: {
      JvmValue discarded;
      (void)vmthread_pop_operand(thr, &discarded);
      return true;
    }
    case 3:
    case 4: {
      // PE statement ++/--: pop L-value, Value_inc/dec in place, no push.
      JvmValue v;
      VmValueRef ref;
      if (!vmthread_pop_operand_ref(thr, &v, &ref)) return false;
      const bool is_inc = (tag == 0x17 || tag == 0x19);  // case3 vs case4
      JvmValue neu;
      if (v.tag == JvmTag::Float)
        neu = JvmValue::make_float(v.v.f + (is_inc ? 1.f : -1.f));
      else {
        const int32_t i = (v.tag == JvmTag::Int) ? v.v.i : 0;
        neu = JvmValue::make_int(i + (is_inc ? 1 : -1));
      }
      (void)vmthread_ref_assign(thr, ref, neu);
      return true;
    }
    case 10: {
      JvmValue dst;
      JvmValue src;
      VmValueRef dst_ref;
      // PE pops the destination FIRST (it is TOS), then the source.
      if (!vmthread_pop_operand_ref(thr, &dst, &dst_ref)) return false;
      if (!vmthread_pop_operand(thr, &src)) return false;
      const JvmValue out = vmthread_assign_op_result(dst, src, tag);
      (void)vmthread_ref_assign(thr, dst_ref, out);
      return true;
    }
    default:
      return false;  // cases 0-2, 7-9: comparison / arithmetic statement forms
  }
}

// PE op29 field-get @ 0x4214BC — soft via TREE field bag (no CP/Object_getField).
// Instance path: tree_field_get_*; miss → push null/0 (PE pushes null on miss
// after script-error; soft skips ScriptError).
void vmthread_op29_field_get(VmThread* thr, InvObject* obj, const char* name) {
  if (!thr || !name || !name[0]) {
    vmthread_value_stack_push(thr, JvmValue::make_obj(nullptr));
    return;
  }
  if (!obj) {
    vmthread_value_stack_push(thr, JvmValue::make_obj(nullptr));
    return;
  }
  // Prefer object field; fall back to int then float (PE Value* carries type).
  if (InvObject* fo = tree_field_get_obj(obj, name)) {
    vmthread_value_stack_push(thr, JvmValue::make_obj(fo));
    return;
  }
  const int32_t iv = tree_field_get_int(obj, name);
  if (iv != 0) {
    vmthread_value_stack_push(thr, JvmValue::make_int(iv));
    return;
  }
  const float fv = tree_field_get_float(obj, name);
  if (fv != 0.f) {
    vmthread_value_stack_push(thr, JvmValue::make_float(fv));
    return;
  }
  // Ambiguous zero: PE still pushes the field Value*; soft push int 0.
  vmthread_value_stack_push(thr, JvmValue::make_int(0));
}

// ---------------------------------------------------------------------------
// PE insn stream walk — VMThread_run @ 0x420FF0, opcode loop @ 0x42108B.
//
// JVM_addClass_fromChunks @ 0x411BF8 materialises every TREE chunk node into
// an Engine_malloc(8 * node_count) array of 8-byte records written by
// TREE_readNode @ 0x41A5F0: {op:u16 = TreeOpFlags[raw] @ 0x5F0914, slot:u16,
// imm:u32}. VMThread_invokeMethod @ 0x420222 hands clazz.trees[tree_index] to
// CallFrame+0x2C (code) and VMThread_run walks it through thr+0x24 (PC,
// stride 8). Host TreeBody::nodes IS that array: PC/8 is the node index and
// TreeNode::imm is TreeInsn_getOperand @ 0x423B80 ([insn+4]).
// ---------------------------------------------------------------------------

// PE @ 0x41A5FE: stream opcode = TreeOpFlags[raw]; raw >= 48 is out of table.
uint16_t vmthread_pe_opcode(const TreeNode& n) {
  return (n.op < 48) ? kTreeOpFlags[n.op] : 0;
}

// PE @ 0x4210D4: op <= 0x48 → VMThread_opcodeCaseMap @ 0x4239B0 → jpt_4210E5.
int vmthread_pe_case(uint16_t op) {
  return (op <= 0x48) ? static_cast<int>(kVmOpcodeCaseMap[op]) : -1;
}

// True when the host loop below can dispatch `op` exactly as the PE does.
// PE opcodes with no dedicated case fall through to VMThread_opDefault_advance
// @ 0x4238ED (pure PC += 8), which the loop reproduces 1:1; those nodes are
// operand carriers consumed by their neighbours (0x1009-0x1010, 0x1012-0x1013,
// 0x1018-0x101A, 0x101C-0x101D, 0x1020-0x1026, 0x200A, 0x4009, 0x4012, …).
bool vmthread_pe_op_walkable(uint16_t op) {
  switch (op) {
    // Fatal cases @ 0x421739 ("fatal script error"): PE jumps to afterPc
    // without advancing — host stops the walk instead of spinning.
    case 0:
    case 19:
    case 66:
    case 72:
    case 0x4A:
    case 0x4D:
    // Low dedicated cases with soft hosts.
    case 4:
    case 5:
    case 16:
    case 24:
    case 32:
    case 40:
    case 41:
    case 42:
    case 43:
    case 45:
    // Hi dedicated cases with soft hosts.
    case 0x1001:
    case 0x1002:
    case 0x1003:
    case 0x1014:
    case 0x1015:
    case 0x1016:
    case 0x1017:
    case 0x101B:
    case 0x101E:
    case 0x101F:
    case 0x1027:
    case 0x4025:  // Object_callMethod utf8 @ 0x423725
    case 0x4026:  // Thread_callMethod utf8 on clazz+0x1C8 @ 0x4236EE
    case 0x401C:  // Object_getField utf8 @ 0x4237CE
      return true;
    // Dedicated PE cases still missing a host body (const-pool / evalName
    // bound): 0x1007 literal @ 0x4218A5, 0x1008 @ 0x422EDA, 0x1011 @ 0x422E98,
    // and low 29 (field-get name). 0x21/0x22/0x23 (<init>) and 0x24
    // (evalName invoke) are hosted; 0x1007/0x1008/0x1011/0x24 are also
    // context-sensitive via vmthread_node_walkable.
    case 0x1007:
    case 0x1008:
    case 0x1011:
    case 29:
    case 36:
      return false;
    default:
      return true;  // PE opDefault_advance
  }
}

// Which 0x1008 statement tags have a host body. Kept beside the dispatcher so
// the census and the walker cannot disagree about what is covered.
bool vmthread_op1008_tag_walkable(int32_t tag) {
  if (tag < 0 || tag > 0x2E) return true;  // PE @ 0x422EE0 → advance
  switch (kVmStmtCaseMap[tag]) {
    case 11:
    case 6:
    case 5:
    case 3:  // Value_inc statement @ 0x4231CD
    case 4:  // Value_dec statement @ 0x423203
    case 10:  // '#' + compounds Soft (PE Value_assignOp @ 0x41ACB0)
      return true;
    default:
      return false;
  }
}

// Op 0x1007 and 0x1008 walkability is context-sensitive: it depends on the tag
// in their own imm, and for 0x1007 on the carrier node that follows it
// (PE @ 0x4218A5 / @ 0x422EDA).
bool vmthread_node_walkable(const TreeBody& code, size_t idx) {
  const TreeNode& n = code.nodes[idx];
  const uint16_t op = vmthread_pe_opcode(n);
  if (op == 0x1008)
    return vmthread_op1008_tag_walkable(static_cast<int32_t>(n.imm));
  if (op == 0x1011) {
    // PE op 0x24 @ 0x4212E4 advances past itself then Thread_evalName reads
    // this node as the count header (imm=segCount only; pe-op often 0x1011)
    // and consumes the segments — the walker never dispatches it as fieldPath.
    if (idx > 0 && vmthread_pe_opcode(code.nodes[idx - 1]) == 0x24)
      return true;
    return vmthread_op1011_shape_walkable(code, idx);
  }
  if (op == 0x24) return vmthread_op24_shape_walkable(code, idx);
  if (op != 0x1007) return vmthread_pe_op_walkable(op);
  const uint32_t tag = n.imm;
  if (tag == 0x0C || tag == 0x1B || tag == 0x1D || tag == 0x22) return true;
  // Hosted arith/cmp tags — no carrier.
  if (tag == 0x16 || tag == 0x10 || tag == 0x0F || tag == 0x13 || tag == 0x12 ||
      tag == 0x15 || tag == 0x07 || tag == 0x06 || tag == 0x0B || tag == 0x0A ||
      tag == 0x09 || tag == 0x08 || tag == 0x23 || tag == 0x02 || tag == 0x01 ||
      tag == 0x03 || tag == 0x05 || tag == 0x11 || tag == 0x0D || tag == 0x0E ||
      tag == 0x04 || tag == 0x14 || tag == 0x19 || tag == 0x1A || tag == 0x17 ||
      tag == 0x18)
    return true;
  // Tag 0x1E class lit / 0x1F NEWARRAY / 0x20 instanceof / 0x21 cast —
  // need a following carrier.
  if (tag == 0x1E || tag == 0x1F || tag == 0x20 || tag == 0x21)
    return idx + 1 < code.nodes.size();
  if (tag != 0x1C || idx + 1 >= code.nodes.size()) return false;
  switch (vmthread_pe_opcode(code.nodes[idx + 1])) {
    case 0x000C:
    case 0x100B:
    case 0x100D:
    case 0x100E:  // ConstantPool_resolveRid soft via const_rid_pack
    case 0x200A:
    case 0x4009:
      return true;
    default:
      return false;
  }
}

void vmthread_stream_census(const TreeBody& code, uint32_t* walkable,
                            uint32_t* needs_case) {
  uint32_t ok = 0;
  uint32_t todo = 0;
  for (size_t i = 0; i < code.nodes.size(); ++i) {
    if (vmthread_node_walkable(code, i))
      ++ok;
    else
      ++todo;
  }
  if (walkable) *walkable = ok;
  if (needs_case) *needs_case = todo;
}

// Census helper: tally which opcodes still have no host case. Ranking the boot
// path by this is the only honest measure — static corpus coverage counts trees
// nobody calls, while this counts the ones the boot actually walks into.
void vmthread_stream_blocking_ops(const TreeBody& code,
                                  std::map<uint16_t, uint32_t>* hist) {
  if (!hist) return;
  for (size_t i = 0; i < code.nodes.size(); ++i) {
    if (vmthread_node_walkable(code, i)) continue;
    (*hist)[vmthread_pe_opcode(code.nodes[i])] += 1;
  }
}

bool vmthread_stream_walkable(const TreeBody& code) {
  uint32_t todo = 0;
  vmthread_stream_census(code, nullptr, &todo);
  return todo == 0;
}

// PE opcode loop. Returns kVmStreamDone when op16 / stream exhaustion ends the
// thread, kVmStreamReturned on op42/op43 (frame popped), kVmStreamYield on the
// cooperative cutoff @ 0x4238F1, kVmStreamUnsupported when an opcode has no
// host case yet (caller falls back without having touched the stream).
int vmthread_exec_stream(VmThread* thr, const JvmClass* cls,
                         const TreeBody& code, float budget_ms) {
  if (!thr) return kVmStreamUnsupported;
  // PE @ 0x421050: noBudgetCutoff = 1 when budget_ms == 0.
  const bool no_budget_cutoff = (budget_ms == 0.f);
  const float deadline_ms = vm_now_ms() + budget_ms;  // PE @ 0x421076
  uint32_t insn_tick = 0;

  for (;;) {
    const uint32_t idx = thr->pe.unk_24 / 8u;  // PE thr+0x24, stride 8
    if (idx >= code.nodes.size()) {
      // PE empty / exhausted code: op16 DONE @ 0x423932 ends the thread.
      vmthread_op16_done(thr);
      return kVmStreamDone;
    }
    const TreeNode& insn = code.nodes[idx];
    const uint16_t op = vmthread_pe_opcode(insn);
    const int32_t imm = static_cast<int32_t>(insn.imm);
    const uint32_t pc_before = thr->pe.unk_24;

    // PE @ 0x4210C5: 'J'/'M' and low 0/19/66/72 → JVM_ScriptError then
    // opDefault_afterPc (PC unchanged). Host stops rather than spin.
    if (op == 0 || op == 19 || op == 66 || op == 72 || op == 0x4A ||
        op == 0x4D) {
      vmthread_op16_done(thr);
      return kVmStreamDone;
    }

    bool self_advanced = false;  // case already did its own opDefault work
    if (op <= 0x48) {
      switch (kVmOpcodeCaseMap[op]) {
        case kVmCaseOp4:
          vmthread_op4_cond_skip(thr);  // extra +8 only; default advance below
          break;
        case kVmCaseOp5:
          vmthread_op5_cond_skip(thr);
          break;
        case kVmCaseOp16:
          vmthread_op16_done(thr);  // PE @ 0x423932 → epilogue
          return kVmStreamDone;
        case kVmCaseOp24:
          vmthread_op24_pop_dtor(thr);
          break;
        case kVmCaseOp32:
          vmthread_op32_array_access(thr);  // self-contained PC += 8
          self_advanced = true;
          break;
        case kVmCaseOp40:
          vmthread_op40_pop_push(thr);
          break;
        case kVmCaseOp41:
          vmthread_op41_pool_alloc(thr);
          self_advanced = true;
          break;
        case kVmCaseOp42:
          vmthread_op42_ret_void(thr);  // popFrameRestorePc
          return kVmStreamReturned;
        case kVmCaseOp43:
          vmthread_op43_ret_val(thr);
          return kVmStreamReturned;
        case kVmCaseOp45:
          vmthread_op45_assign_store(thr);
          self_advanced = true;
          break;
        case kVmCaseOp33: {
          // PE op 0x21 @ 0x421254: peek recv via argc depth math;
          // Object_callMethod(recv, thr, "<init>") → class from obj+0xC.
          InvObject* recv = vmthread_op21_peek_recv(thr);
          if (!recv) return kVmStreamUnsupported;
          const char* cn = tree_host_class(recv);
          if (!cn || !cn[0]) cn = cls->name.c_str();
          const StreamCall sc = vmthread_stream_call(thr, recv, cn, "<init>");
          if (sc == StreamCall::Fail) return kVmStreamUnsupported;
          self_advanced = (sc == StreamCall::SelfAdvanced);
          break;
        }
        case kVmCaseOp34: {
          // PE op 0x22 @ 0x4212C7: this=frame+0x30, clazz=frame+0x34;
          // Object_callMethod_init → Thread_callMethod(..., "<init>").
          InvObject* self = nullptr;
          if (thr->curr_frame && !thr->curr_frame->locals.empty() &&
              thr->curr_frame->locals[0].tag == JvmTag::Obj)
            self = thr->curr_frame->locals[0].v.o;
          const StreamCall sc =
              vmthread_stream_call(thr, self, cls->name.c_str(), "<init>");
          if (sc == StreamCall::Fail) return kVmStreamUnsupported;
          self_advanced = (sc == StreamCall::SelfAdvanced);
          break;
        }
        case kVmCaseOp35: {
          // PE op 0x23 @ 0x4212A4: this=frame+0x30,
          // clazz=[frame+0x34]+0x1C8 (superclass). Object_callInitIf: null
          // clazz → ret 1 (continue); else Thread_callMethod "<init>".
          InvObject* self = nullptr;
          if (thr->curr_frame && !thr->curr_frame->locals.empty() &&
              thr->curr_frame->locals[0].tag == JvmTag::Obj)
            self = thr->curr_frame->locals[0].v.o;
          if (cls->super_name.empty() ||
              cls->super_name == "java.lang.Object") {
            // PE Object_callInitIf null clazz → ret 1 → advance.
            break;
          }
          const StreamCall sc = vmthread_stream_call(
              thr, self, cls->super_name.c_str(), "<init>");
          if (sc == StreamCall::Fail) return kVmStreamUnsupported;
          self_advanced = (sc == StreamCall::SelfAdvanced);
          break;
        }
        case kVmCaseOp36: {
          // PE op 0x24 @ 0x4212E4: PC+=8 past the 0x24 node, then
          // Thread_evalName @ 0x4208E0, then Thread_callMethod /
          // VMThread_invokeMethod. ret!=0 → opDefault_advance past the
          // terminator; ret==0 → afterPc (Java queued; op42/43 +8 on return).
          uint32_t walk_pc = thr->pe.unk_24 + 8;
          std::string class_fqn;
          std::string method;
          InvObject* self = nullptr;
          if (!vmthread_op24_eval_name(thr, cls, code, &walk_pc, &class_fqn,
                                       &method, &self))
            return kVmStreamUnsupported;
          thr->pe.unk_24 = walk_pc;  // ON the terminator (0x101A / utf8)
          const StreamCall sc = vmthread_stream_call(
              thr, self, class_fqn.c_str(), method.c_str());
          if (sc == StreamCall::Fail) return kVmStreamUnsupported;
          self_advanced = (sc == StreamCall::SelfAdvanced);
          break;
        }
        case kVmCaseDefault:
          break;  // PE opDefault_advance: no work
        default:
          return kVmStreamUnsupported;  // op29 still needs field operands
      }
    } else {
      switch (op) {
        case 0x1001:
          vmthread_op1001_local_load(thr, static_cast<uint32_t>(imm));
          break;
        case 0x1002: {
          // PE @ 0x421752 append ValueField; name via getUtf8 on the next
          // node (stock: always 0x1006). @ 0x4217F0 if * (PC+8) == 0x2C,
          // set PC on the comma and loc_422D74 pushes that field as lvalue
          // then +8. Stock encodes name before the comma, so PC+8 is 0x1006
          // not 0x2C — soft skips the name node PE already read and applies
          // the same lvalue push when the following node is 0x2C.
          vmthread_op1002_local_declare(thr, static_cast<uint32_t>(imm));
          size_t comma_at = 0;
          if (idx + 1 < code.nodes.size() &&
              vmthread_pe_opcode(code.nodes[idx + 1]) == 0x002C) {
            comma_at = idx + 1;
          } else if (idx + 2 < code.nodes.size() &&
                     vmthread_pe_opcode(code.nodes[idx + 1]) == 0x1006 &&
                     vmthread_pe_opcode(code.nodes[idx + 2]) == 0x002C) {
            comma_at = idx + 2;
          }
          if (comma_at != 0) {
            VmValueRef ref;
            ref.kind = VmRefKind::Local;
            ref.local_index = static_cast<uint32_t>(imm);
            JvmValue cur{};
            if (thr->curr_frame &&
                static_cast<size_t>(imm) < thr->curr_frame->locals.size())
              cur = thr->curr_frame->locals[static_cast<size_t>(imm)];
            vmthread_value_stack_push_ref(thr, cur, ref);
            // Land past the comma (PE loc_422D74 → opDefault_advance).
            thr->pe.unk_24 = static_cast<uint32_t>((comma_at + 1) * 8u);
            self_advanced = true;
          }
          break;
        }
        case 0x1003:
          vmthread_op1003_pop_release(thr);
          break;
        case 0x1007: {
          const TreeNode* carrier =
              (idx + 1 < code.nodes.size()) ? &code.nodes[idx + 1] : nullptr;
          bool skip_carrier = false;
          if (!vmthread_op1007_literal(thr, cls, imm, carrier, &skip_carrier))
            return kVmStreamUnsupported;
          // PE tag 0x20 instanceof advances past the carrier before
          // opDefault_advance (@ 0x422D1F); soft mirrors with +16.
          if (skip_carrier) {
            thr->pe.unk_24 = pc_before + 16;
            self_advanced = true;
          }
          break;
        }
        case 0x1008:
          // PE statement dispatcher; every hosted slot ends in opDefault_advance.
          if (!vmthread_op1008_statement(thr, imm)) return kVmStreamUnsupported;
          break;
        case 0x1011: {
          // evalName walks the segment nodes itself and leaves the PC on the
          // last one; the opDefault_advance below supplies the final +8.
          JvmValue named;
          VmValueRef named_ref;
          uint32_t walk_pc = thr->pe.unk_24;
          if (!vmthread_op1011_eval_name(thr, cls, code, &walk_pc, imm, &named,
                                         &named_ref))
            return kVmStreamUnsupported;
          thr->pe.unk_24 = walk_pc;
          vmthread_value_stack_push_ref(thr, named, named_ref);
          thr->pe.unk_24 += 8;
          self_advanced = true;
          break;
        }
        case 0x101B: {
          // PE @ 0x4234B7 takes the FIELDREF index from [insn+4] (= imm) and
          // runs it through ConstantPool_resolveFieldRef before testing the
          // 0x40000000 bit @ 0x4234FB.
          std::string fname;
          bool is_static = false;
          const JvmClass* owner = nullptr;
          if (!vmthread_resolve_field_ref(cls, static_cast<uint32_t>(insn.imm),
                                          &fname, &is_static, &owner))
            return kVmStreamUnsupported;
          vmthread_op101B_field_ref(thr, fname.c_str(), is_static, owner);
          self_advanced = true;
          break;
        }
        case 0x1014:
          vmthread_op1014_goto(thr, imm);  // PE → afterPc, no extra advance
          self_advanced = true;
          break;
        case 0x1015:
          vmthread_op1015_bool_cond_advance(thr, imm);
          self_advanced = true;
          break;
        case 0x1016:
          vmthread_op1016_bool_cond_skip(thr, imm);
          self_advanced = true;
          break;
        case 0x1017:
          vmthread_op1017_case_eq_skip(thr, imm);
          self_advanced = true;
          break;
        case 0x101E:
          vmthread_op101E_array_init(thr, imm);
          self_advanced = true;
          break;
        case 0x101F:
          vmthread_op101F_push_nulls(thr, imm);
          self_advanced = true;
          break;
        case 0x1027:
          vmthread_op1027_box_imm(thr, imm);
          break;
        case 0x4025: {
          // PE @ 0x423725: pop recv (ValueField→ValuePool copy OOS Soft);
          // Object_callMethod(recv, thr, utf8(insn.imm)) @ 0x408A30;
          // null payload → ScriptError illegal methodcall; Soft → Fail.
          // ret!=0 → opDefault_advance; ret==0 → afterPc (Soft drains).
          JvmValue recv_v;
          if (!vmthread_pop_operand(thr, &recv_v))
            return kVmStreamUnsupported;
          InvObject* recv =
              (recv_v.tag == JvmTag::Obj) ? recv_v.v.o : nullptr;
          if (!recv) return kVmStreamUnsupported;
          if (imm < 0 ||
              static_cast<size_t>(imm) >= cls->const_strings.size())
            return kVmStreamUnsupported;
          const std::string& mname =
              cls->const_strings[static_cast<size_t>(imm)];
          if (mname.empty()) return kVmStreamUnsupported;
          // PE clazz = obj+0xC. Soft: host_class, else Vector bag, else
          // caller class (prior bug: VehicleType.size when preferredColorIndexes
          // lost its host tag).
          const char* cn = tree_host_class(recv);
          if ((!cn || !cn[0]) && tree_vector_is(recv)) cn = "java.util.Vector";
          if (!cn || !cn[0]) cn = cls->name.c_str();
          const StreamCall sc =
              vmthread_stream_call(thr, recv, cn, mname.c_str());
          if (sc == StreamCall::Fail) return kVmStreamUnsupported;
          self_advanced = (sc == StreamCall::SelfAdvanced);
          break;
        }
        case 0x4026: {
          // PE @ 0x4236EE: utf8(insn.imm); Thread_callMethod(thr,
          // [frame+0x34]+0x1C8 super, frame+0x30 this, name). Soft:
          // cls->super_name + locals[0]. Null super → PE ScriptError ret -1
          // → still advance; Soft skips the call.
          if (imm < 0 ||
              static_cast<size_t>(imm) >= cls->const_strings.size())
            return kVmStreamUnsupported;
          const std::string& mname =
              cls->const_strings[static_cast<size_t>(imm)];
          if (mname.empty()) return kVmStreamUnsupported;
          if (cls->super_name.empty()) {
            // PE clazz+0x1C8 null → ScriptError "null clazz", ret -1 → advance
            break;
          }
          InvObject* self = nullptr;
          if (thr->curr_frame && !thr->curr_frame->locals.empty() &&
              thr->curr_frame->locals[0].tag == JvmTag::Obj)
            self = thr->curr_frame->locals[0].v.o;
          const StreamCall sc = vmthread_stream_call(
              thr, self, cls->super_name.c_str(), mname.c_str());
          if (sc == StreamCall::Fail) return kVmStreamUnsupported;
          self_advanced = (sc == StreamCall::SelfAdvanced);
          break;
        }
        case 0x401C: {
          // PE @ 0x4237CE: pop recv; Object_getField(utf8 insn.imm) @ 0x408800;
          // push ValueField*; miss/null → ScriptError then still push.
          // Soft: TREE bag via op29_field_get + Field VmValueRef.
          JvmValue recv_v;
          if (!vmthread_pop_operand(thr, &recv_v))
            return kVmStreamUnsupported;
          InvObject* recv =
              (recv_v.tag == JvmTag::Obj) ? recv_v.v.o : nullptr;
          if (imm < 0 ||
              static_cast<size_t>(imm) >= cls->const_strings.size())
            return kVmStreamUnsupported;
          const std::string& fname =
              cls->const_strings[static_cast<size_t>(imm)];
          if (fname.empty()) return kVmStreamUnsupported;
          vmthread_op29_field_get(thr, recv, fname.c_str());
          if (recv && thr->curr_frame &&
              !thr->curr_frame->operand_ref.empty()) {
            VmValueRef& top = thr->curr_frame->operand_ref.back();
            top.kind = VmRefKind::Field;
            top.obj = recv;
            top.name = fname;
          }
          break;  // PE → opDefault_advance
        }
        default:
          if (!vmthread_pe_op_walkable(op)) return kVmStreamUnsupported;
          break;  // PE opDefault_advance
      }
    }

    // PE VMThread_opDefault_advance @ 0x4238ED: add thr+0x24, 8.
    if (!self_advanced) thr->pe.unk_24 = pc_before + 8;

    // PE @ 0x4238F1 afterPc: skip the clock when budget_ms == 0; otherwise
    // sample Engine_GetTimeMs every 256 advances and cut on the deadline.
    if (!no_budget_cutoff && ((++insn_tick & 0xFF) == 0) &&
        vm_now_ms() >= deadline_ms)
      return kVmStreamYield;
    // PE @ 0x423918 yieldCheck: flags & 0x78 → epilogue; then canContinue.
    if ((thr->pe.flags & kVmThreadFlagYieldMask) != 0) return kVmStreamYield;
    if (!vmthread_can_continue(thr)) return kVmStreamYield;
  }
}

// PE VMThread_invokeMethod TREE branch @ 0x00420211 — build the CallFrame the
// stream walker runs in. Sequence:
//   0x420227 guard: clazz.trees count > method.tree_index and entry non-null
//   0x420237 callerFrame+0x2C = thr+0x24 — the caller's resume PC
//   0x420247 CallFramePool_alloc + VMThread_CallFrame_ctor @ 0x407B10 with
//            (thr, clazz, code = clazz.trees[tree_index], instance, slots);
//            dllist link @ 0x42027B; frame+0x38 = method name @ 0x420298
//   0x42029F when ACC_STATIC (flags & 8) is clear: frame becomes current and
//            ValueField_ctor("this", instance class) is appended to the locals
//            PtrVec at +0x18 with payload = the instance @ 0x4202FB
//   0x420318 argc = NativeSigDesc_argc; the loop pops the CALLER operand stack
//            (ValueStack_pop on callerFrame+0x28) top-first and appends one
//            ValueField per arg to the SAME locals PtrVec, typed
//            NativeSigDesc_paramAt(argc-1 .. 0). Popping the top first is why
//            locals end up [this,] arg[n-1] .. arg[0] — the reversal jvm.cpp
//            @ 3853 had already inferred from behaviour.
//   0x42056F thr.curr_frame = frame; thr+0x24 = frame+0x2C. The PE PC is a
//            pointer into the node array, so its origin is host PC 0.
VmCallFrame* vmthread_build_tree_frame(VmThread* thr, const JvmClass* cls,
                                       const JvmMethod* m, InvObject* self,
                                       const std::vector<JvmValue>& args) {
  if (!thr || !cls || !m) return nullptr;
  if (m->tree_index < 0 ||
      static_cast<size_t>(m->tree_index) >= cls->trees.size())
    return nullptr;  // PE @ 0x420227 falls through to the empty-body path

  if (thr->curr_frame)  // PE @ 0x420237 save the caller resume PC
    thr->curr_frame->pe.code = thr->pe.unk_24;

  VmCallFrame* fr = callframe_pool_alloc();
  if (!fr) return nullptr;
  const uint32_t inst_u =
      static_cast<uint32_t>(reinterpret_cast<std::uintptr_t>(self));
  callframe_ctor(fr, thr, /*clazz=*/0, /*code=*/0, /*instance=*/inst_u,
                 /*local_slots=*/static_cast<int>(args.size() + 1));
  if (thr->frame_list) frame_list_link(thr->frame_list, fr);

  // PE @ 0x4202AA: "this" occupies locals[0] on instance methods only.
  const bool is_static = (m->flags & 0x8u) != 0;
  fr->locals.clear();
  fr->locals.reserve(args.size() + 1);
  if (!is_static) fr->locals.push_back(JvmValue::make_obj(self));
  // PE @ 0x420333: pops run from the operand-stack top down.
  for (auto it = args.rbegin(); it != args.rend(); ++it) fr->locals.push_back(*it);
  fr->pe.local_count = static_cast<uint32_t>(fr->locals.size());
  fr->pe.local_cap = fr->pe.local_count;
  fr->operand.clear();

  thr->curr_frame = fr;  // PE @ 0x42056F
  thr->pe.curr_frame = 1;
  thr->pe.unk_24 = 0;  // PE thr+0x24 = frame+0x2C, i.e. the code origin
  return fr;
}

// Soft sync twin of PE CallNamedMethod_va: invokeMethod ret0 → VMThread_run(0).
// Ephemeral VmThread so we never re-enter vmthread_run from Jvm::invoke.
// Nested stream_call→invoke must NOT re-enter exec_stream (C++ stack blow-up;
// PE queues on the same thr). TLS gate → tree_eval for callees.
bool vmthread_try_stream_eval(const JvmClass* cls, const JvmMethod* m,
                              InvObject* self,
                              const std::vector<JvmValue>& args,
                              JvmValue* out) {
  if (!cls || !m || !out) return false;
  if (m->is_native) return false;
  if (m->tree_index < 0 ||
      static_cast<size_t>(m->tree_index) >= cls->trees.size())
    return false;
  // Soft <init> / VehicleType.init: stream 4025 still sees VehicleType as
  // recv for size/elementAt when getfield packing ≠ Soft tree_eval.
  if (m->name == "<init>" || m->name == "init") return false;

  thread_local int tls_pe_stream_depth = 0;
  if (tls_pe_stream_depth > 0) return false;  // nested: Soft tree_eval

  static const bool pe_stream_enabled = [] {
    const char* v = std::getenv("SLRR_PE_STREAM");
    if (!v || !v[0]) return true;  // default on (same gate as vmthread_run)
    return !(v[0] == '0' && v[1] == '\0');
  }();
  if (!pe_stream_enabled) return false;

  const TreeBody& code = cls->trees[static_cast<size_t>(m->tree_index)];
  uint32_t ok = 0;
  uint32_t todo = 0;
  vmthread_stream_census(code, &ok, &todo);
  if (todo != 0) return false;
  // Default leaf-only (race211 EXIT=0). Call-op trees need Soft host leaves
  // (command/addTraffic/enter…) before default ON — opt-in SLRR_PE_STREAM_INVOKE=1
  // exercises name-only lookup + Vector 4025 packing from this ticket.
  static const bool pe_stream_invoke_full = [] {
    const char* v = std::getenv("SLRR_PE_STREAM_INVOKE");
    return v && v[0] == '1' && v[1] == '\0';
  }();
  if (!pe_stream_invoke_full) {
    for (const TreeNode& n : code.nodes) {
      const uint16_t op = vmthread_pe_opcode(n);
      if (op == 0x21 || op == 0x22 || op == 0x23 || op == 0x24 || op == 0x4025 ||
          op == 0x4026)
        return false;
    }
  }

  // `args` mirror pending.args: this at [0] for instance (Jvm::invoke /
  // stream_call). build_tree_frame wants self separate + remaining params.
  std::vector<JvmValue> call_args = args;
  InvObject* recv = self;
  const bool is_static = (m->flags & 0x8u) != 0;
  if (!is_static) {
    if (!call_args.empty() && call_args.front().tag == JvmTag::Obj) {
      if (!recv) recv = call_args.front().v.o;
      call_args.erase(call_args.begin());
    }
  }

  static const bool pe_stream_trace = [] {
    const char* v = std::getenv("SLRR_PE_STREAM_TRACE");
    return v && v[0] == '1';
  }();
  if (pe_stream_trace)
    std::fprintf(stderr, "[pe-stream] enter %s.%s%s nodes=%zu\n",
                 cls->name.c_str(), m->name.c_str(), m->signature.c_str(),
                 code.nodes.size());

  VmThread thr{};
  VmFrameList frames;
  thr.frame_list = &frames;
  if (!vmthread_build_tree_frame(&thr, cls, m, recv, call_args)) return false;

  struct StreamDepthGuard {
    int* d;
    explicit StreamDepthGuard(int* p) : d(p) { ++(*d); }
    ~StreamDepthGuard() { --(*d); }
  } depth_guard(&tls_pe_stream_depth);

  const int st = vmthread_exec_stream(&thr, cls, code, /*budget_ms=*/0.f);
  if (pe_stream_trace)
    std::fprintf(stderr, "[pe-stream] leave %s.%s st=%d\n", cls->name.c_str(),
                 m->name.c_str(), st);
  if (st == kVmStreamUnsupported) {
    while (thr.curr_frame) vmthread_pop_frame_restore_pc(&thr);
    return false;
  }

  // op43 @ 0x421662 pushes the return onto thr.operand after popFrame.
  if (jni_sig_returns_void(m->signature)) {
    *out = JvmValue::make_void();
  } else if (!thr.operand.empty()) {
    *out = thr.operand.back();
  } else if (thr.curr_frame && !thr.curr_frame->operand.empty()) {
    *out = thr.curr_frame->operand.back();
  } else {
    *out = JvmValue{};
  }
  while (thr.curr_frame) vmthread_pop_frame_restore_pc(&thr);
  return true;
}

// Cumulative PE-stream coverage of everything vmthread_run was asked to run.
static VmStreamStats g_vm_stream_stats;

const VmStreamStats& vmthread_stream_stats() { return g_vm_stream_stats; }

int vmthread_run(VmThread* thr, float budget_ms) {
  if (!thr) return -1;
  // PE @ 0x421018: flags&4 → "already running!"
  if ((thr->pe.flags & kVmThreadFlagRunning) != 0) return -1;
  if (!thr->pending.live) {
    // PE empty thread (no code @ +0x24) → error path; host: idle ok.
    return 0;
  }
  Jvm* j = jvm_active();
  if (!j) return -1;
  if (!vmthread_can_continue(thr)) return 0;

  thr->pe.flags |= kVmThreadFlagRunning;  // PE @ 0x42107D |= 4
  g_vm_sched_cursor = thr;                // PE stash JVM+0x78 cursor

  // PE VMThread_invokeMethod @ 0x420222: code = clazz.trees[tree_index] into
  // CallFrame+0x2C, PC (thr+0x24) = 0. Walk that stream when every opcode in
  // it already has a host case; otherwise keep the soft invoke below.
  //
  // Default ON since boot-path walkability hit 1000/1000 (race209). Opt out
  // with SLRR_PE_STREAM=0. Trees with any unhosted opcode still fall through
  // to soft invoke (todo != 0 gate below).
  static const bool pe_stream_enabled = [] {
    const char* v = std::getenv("SLRR_PE_STREAM");
    if (!v || !v[0]) return true;  // default on
    return !(v[0] == '0' && v[1] == '\0');
  }();
  if (const JvmClass* cls = j->find_class(thr->pending.class_fqn.c_str())) {
    if (const JvmMethod* m =
            j->find_method(*cls, thr->pending.method.c_str(),
                           thr->pending.signature.c_str())) {
      if (!m->is_native && m->tree_index >= 0 &&
          static_cast<size_t>(m->tree_index) < cls->trees.size()) {
        const TreeBody& code = cls->trees[m->tree_index];
        uint32_t ok = 0;
        uint32_t todo = 0;
        vmthread_stream_census(code, &ok, &todo);
        ++g_vm_stream_stats.trees;
        g_vm_stream_stats.insns += ok + todo;
        g_vm_stream_stats.insns_walkable += ok;
        if (todo == 0) {
          ++g_vm_stream_stats.trees_walkable;
        }
        if (todo == 0 && pe_stream_enabled) {
          // PE @ 0x420211: frame first (locals seeded from the args), then the
          // opcode loop. pending.args carry `this` at [0] for instance calls,
          // matching how vmthread_invoke_method packs them.
          std::vector<JvmValue> call_args = thr->pending.args;
          InvObject* self = nullptr;
          const bool is_static = (m->flags & 0x8u) != 0;
          if (!is_static && !call_args.empty() &&
              call_args.front().tag == JvmTag::Obj) {
            self = call_args.front().v.o;
            call_args.erase(call_args.begin());
          }
          VmCallFrame* prev_frame = thr->curr_frame;
          if (vmthread_build_tree_frame(thr, cls, m, self, call_args)) {
            const int st = vmthread_exec_stream(thr, cls, code, budget_ms);
            if (st != kVmStreamUnsupported) {
              thr->pending.live = false;
              thr->pending.args.clear();
              if ((thr->pe.flags & kVmThreadFlagDone) == 0 &&
                  thr->pe.unk_24 == 0)
                vmthread_op16_done(thr);
              thr->pe.flags &= ~kVmThreadFlagRunning;  // PE epilogue @ 0x423936
              return 0;
            }
            // Bailed mid-body: drop the frame and let the soft invoke run.
            if (thr->curr_frame && thr->curr_frame != prev_frame)
              vmthread_pop_frame_restore_pc(thr);
            thr->curr_frame = prev_frame;
            thr->pe.curr_frame = prev_frame ? 1u : 0u;
          }
        }
      }
    }
  }

  // Soft body: TREE/jvm invoke ≡ PE bytecode loop (residual cases OOS).
  JvmValue result =
      j->invoke(thr->pending.class_fqn.c_str(), thr->pending.method.c_str(),
                thr->pending.signature.c_str(), thr->pending.args, false);
  const bool ret_void = jni_sig_returns_void(thr->pending.signature);
  thr->pending.live = false;
  thr->pending.args.clear();

  // Method produced return value on operand (PE leaves TOS for op43).
  if (!ret_void) {
    vmthread_value_stack_push(thr, result);
    // PE @ 0x421662 op43 ret-val → popFrame → empty PC → op16 DONE.
    vmthread_op43_ret_val(thr);
  } else {
    // PE @ 0x421697 op42 ret-void → popFrame → empty PC → op16 DONE.
    vmthread_op42_ret_void(thr);
  }
  // Ensure DONE if popFrame left a soft non-zero PC without parent (leaf).
  if ((thr->pe.flags & kVmThreadFlagDone) == 0 && thr->pe.unk_24 == 0)
    vmthread_op16_done(thr);

  // PE epilogue @ 0x423936: flags &= ~4; restore JVM+0x78 (cursor unchanged).
  thr->pe.flags &= ~kVmThreadFlagRunning;
  return 0;
}

// PE Thread_setSleepDeadline @ 0x41F630 (Thread.sleep → JNI @ 0x47C650).
void vmthread_set_sleep_deadline(VmThread* thr, float ms) {
  if (!thr) return;
  thr->pe.sleep_deadline = vm_now_ms() + ms;
  thr->pe.flags |= kVmThreadFlagSleep;
}

// PE Thread_requestStop @ 0x41F780: if flags&0x10 → MonitorDequeue; |= 0x80.
void vmthread_request_stop(VmThread* thr) {
  if (!thr) return;
  thr->pe.flags |= kVmThreadFlagStop;
}

void vmthread_destroy(VmThread* thr) {
  if (!thr) return;
  vm_sched_unlink(thr);
  if (thr->frame_list) {
    for (VmCallFrame* f : thr->frame_list->frames) callframe_pool_release(f);
    thr->frame_list->frames.clear();
    thr->frame_list->cursor = nullptr;
    delete thr->frame_list;
    thr->frame_list = nullptr;
  }
  thr->curr_frame = nullptr;
  thr->pending.live = false;
  delete thr;
}

// PE Jvm_RunThreadsBudgeted @ 0x416940 (W34-17 deepen).
// while time < now+budget (budget==0 → until idle wrap):
//   cursor = JVM+0x78; wrap from *(JVM+0x18)+8 dllist head;
//   skip flags&0x82; require !(flags&0x44) && prio>=min;
//   clear sleep&8 when GetTimeMs > *(float*)(+0x30); prio-scale slice;
//   VMThread_run (PE always; host: pending.live → TREE + op42/43/16);
//   inside !&0x82: 0x40&!1 → |=STOP; THEN advance cursor via thr+4;
//   STOP&!RUNNING → vtbl dtor(1). saw_run (PE bl) only on VMThread_run.
int jvm_run_threads_budgeted(float budget_ms, int32_t min_prio) {
  const bool unlimited = (budget_ms == 0.f);
  float deadline = vm_now_ms() + budget_ms;
  float slice = 10.f;  // PE v17 / flt_5E7334 = 10.0f
  if (!unlimited && budget_ms < 10.f) slice = budget_ms;

  // PE bl @ 0x416950: run resets; second wrap without run → return 1.
  bool saw_run = true;
  while (true) {
    // PE @ 0x4169A7..0x4169B5: budget cut (skipped when a2==0).
    if (!unlimited && vm_now_ms() >= deadline) return 1;

    // PE @ 0x4169BB..0x4169DC: cursor null → reload list head; !saw_run→1.
    if (!g_vm_sched_cursor) {
      g_vm_sched_cursor = g_vm_sched_head;  // PE *(JVM+0x18)+8 gated
      if (!saw_run) return 1;
      saw_run = false;
    }
    VmThread* cur = g_vm_sched_cursor;
    if (!cur) return 0;  // PE @ 0x4169E3 empty → ret 0

    // PE body uses JVM+0x78 throughout; advance only @ 0x416AA4.
    if ((cur->pe.flags & kVmThreadFlagSkipMask) == 0) {
      // PE @ 0x4169F4: !(flags&0x44) && priority >= min.
      if ((cur->pe.flags & kVmThreadFlagBusyMask) == 0 &&
          cur->pe.priority >= min_prio) {
        // PE @ 0x416A05..0x416A1B: sleep&8 && now > +0x30 → clear &8.
        if ((cur->pe.flags & kVmThreadFlagSleep) != 0 &&
            vm_now_ms() > cur->pe.sleep_deadline) {
          cur->pe.flags &= ~kVmThreadFlagSleep;
        }
        if ((cur->pe.flags & kVmThreadFlagSleep) == 0) {
          // PE @ 0x416A28..0x416A6C: flt_5F08F8=0.1, F4=99, F0=1,
          // EC=0.9, E8=0.1 — priority-scaled slice.
          const int32_t prio = cur->pe.priority;
          float scale = 1.f;
          if (prio < 0)
            scale = static_cast<float>(prio + 10) * 0.1f * 0.9f + 0.1f;
          else
            scale = static_cast<float>(prio) * 0.1f * 99.f + 1.f;
          const float scaled = scale * slice;
          // PE @ 0x416A70..0x416A81: deadline = deadline - slice + scaled.
          if (!unlimited) deadline = deadline - slice + scaled;
          // PE always VMThread_run; host pending → TREE + op42/43/16 slice.
          if (cur->pending.live) {
            vmthread_run(cur, unlimited ? 0.f : scaled);
            saw_run = true;  // PE bl=1 @ 0x416A8C only here
          }
        }
      }
      // PE @ 0x416A8E..0x416AA1: DONE&!(flags&1 sync) → |= STOP.
      // Bit0 = CallNamedMethod sync; sync==1 keeps thread for caller.
      if ((cur->pe.flags & kVmThreadFlagDone) != 0 &&
          (cur->pe.flags & 1) == 0) {
        cur->pe.flags |= kVmThreadFlagStop;
      }
    }

    // PE @ 0x416AA4..0x416AB7: advance before dtor (thr+4 next; sentinel).
    g_vm_sched_cursor = cur->sched_next;

    // PE @ 0x416ABA..0x416AD9: !RUNNING && STOP → vtbl dtor(1).
    // Do NOT set saw_run — PE bl stays clear across destroy-only wraps.
    if ((cur->pe.flags & kVmThreadFlagRunning) == 0 &&
        (cur->pe.flags & kVmThreadFlagStop) != 0) {
      vmthread_destroy(cur);
    }
  }
}

// PE Jvm_PumpFrame @ 0x418D10 size 0x191 — MainLoop @ 0x428ABC when
// EngineState+0xE4 (== g_JVM @ 0x63641C). PE order: ++Jvm+0x40 →
// Jvm_GcSlice @ 0x418C20 → (time start) mark-sweep if +0x34>+0x38 →
// EMA Jvm+0x24 → Jvm_RunThreadsBudgeted(budget, 0).
// Soft: EMA + RunThreads deepened; GcSlice + mark-sweep SKIP (dllists /
// FinalizeFree→opcode@4210D4) — residual OOS. Gameloop hosted.
int32_t jvm_gc_time_ema() { return g_jvm_gc_ema; }

int jvm_pump_frame(float budget_ms) {
  ++g_jvm_pump_gen;  // PE ++*(Jvm+0x40)
  jvm_gc_slice();    // PE Jvm_GcSlice — SKIP body (see stub)
  // PE @ 0x418D2D: TimeMs AFTER GcSlice — EMA covers mark-sweep only.
  const float t0 = vm_now_ms();
  jvm_mark_sweep_shell();  // PE @ 0x418D37..0x418E5F — SKIP (OOS)
  const float t1 = vm_now_ms();
  // PE @ 0x418E62..0x418E90: Engine_ftol(delta_ms*1000*0.8 + old*0.2).
  g_jvm_gc_ema = static_cast<int32_t>((t1 - t0) * 1000.0 * 0.8 +
                                      static_cast<double>(g_jvm_gc_ema) * 0.2);
  return jvm_run_threads_budgeted(budget_ms, /*min_prio=*/0);
}

}  // namespace inv

