#include "callinfo.hpp"

#include <cstdio>
#include <cstring>

namespace inv {
namespace {

JvmTag read_type(const char*& p) {
  if (!p || !*p) return JvmTag::Void;
  switch (*p++) {
    case 'V':
      return JvmTag::Void;
    case 'I':
    case 'Z':
    case 'B':
    case 'C':
    case 'S':
      return JvmTag::Int;
    case 'F':
      return JvmTag::Float;
    case 'J':
      return JvmTag::Int;
    case 'D':
      return JvmTag::Float;
    case 'L': {
      while (*p && *p != ';') ++p;
      if (*p == ';') ++p;
      return JvmTag::Obj;
    }
    case '[': {
      // Array → Obj
      while (*p == '[') ++p;
      if (*p == 'L') {
        while (*p && *p != ';') ++p;
        if (*p == ';') ++p;
      } else if (*p) {
        ++p;
      }
      return JvmTag::Obj;
    }
    default:
      return JvmTag::Void;
  }
}

std::string trim_copy(std::string s) {
  size_t a = 0;
  while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
  size_t b = s.size();
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
  return s.substr(a, b - a);
}

std::string java_type_to_jni_token(std::string t) {
  t = trim_copy(std::move(t));
  while (t.size() >= 2 && t[t.size() - 2] == '[' && t.back() == ']') {
    t.resize(t.size() - 2);
    t = trim_copy(std::move(t));
    return "Ljava.lang.Object;";
  }
  if (t == "void") return "V";
  if (t == "int" || t == "boolean" || t == "bool" || t == "byte" ||
      t == "char" || t == "short" || t == "long")
    return "I";
  if (t == "float" || t == "double") return "F";
  return "Ljava.lang.Object;";
}

std::string arg_type_from_decl(const std::string& decl) {
  std::string s = trim_copy(decl);
  if (s.empty()) return {};
  size_t i = 0;
  while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
  std::string ty = s.substr(0, i);
  std::string rest = trim_copy(s.substr(i));
  if (rest.size() >= 2 && rest[0] == '[' && rest[1] == ']')
    return "Ljava.lang.Object;";
  return java_type_to_jni_token(ty);
}

}  // namespace

std::string java_sig_to_jni(const char* java_sig) {
  if (!java_sig || !*java_sig) return {};
  if (java_sig[0] == '(') return java_sig;
  const char* paren = std::strchr(java_sig, '(');
  if (!paren) return {};
  const char* close = std::strchr(paren, ')');
  if (!close) return {};
  const char* p = java_sig;
  while (*p == ' ' || *p == '\t') ++p;
  const char* ret_end = p;
  while (*ret_end && *ret_end != ' ' && *ret_end != '\t' && *ret_end != '(')
    ++ret_end;
  std::string ret = java_type_to_jni_token(std::string(p, ret_end));
  std::string out = "(";
  const char* a = paren + 1;
  while (*a == ' ' || *a == '\t') ++a;
  if (a < close && *a != ')') {
    std::string cur;
    for (const char* q = a; q < close; ++q) {
      if (*q == ',') {
        out += arg_type_from_decl(cur);
        cur.clear();
      } else {
        cur.push_back(*q);
      }
    }
    out += arg_type_from_decl(cur);
  }
  out += ')';
  out += ret;
  return out;
}

bool jni_parse(const char* signature, JvmTag* ret, std::vector<JvmTag>* args,
               std::string* err) {
  if (!signature || !ret || !args) {
    if (err) *err = "null";
    return false;
  }
  args->clear();
  const char* p = signature;
  if (*p != '(') {
    if (err) *err = "expected '('";
    return false;
  }
  ++p;
  while (*p && *p != ')') {
    const char* before = p;
    JvmTag t = read_type(p);
    if (p == before) {
      if (err) *err = "bad arg type";
      return false;
    }
    args->push_back(t);
  }
  if (*p != ')') {
    if (err) *err = "expected ')'";
    return false;
  }
  ++p;
  *ret = read_type(p);
  return true;
}

bool call_native(const NativeEntry* entry, CallFrame* frame, std::string* err) {
  if (!entry || !entry->fn || !frame) {
    if (err) *err = "null entry/frame";
    return false;
  }

  JvmTag ret = JvmTag::Void;
  std::vector<JvmTag> atypes;
  // JVM_UnboxArg @ 0x0045D910: descriptor comes from the native method, not
  // the boxed TREE tags (getAxis TREE packs (FF)F, registry is (II)F).
  std::string jni_owned;
  const char* jni = frame->jni_signature;
  if (entry->java_signature && entry->java_signature[0]) {
    jni_owned = java_sig_to_jni(entry->java_signature);
    if (!jni_owned.empty()) jni = jni_owned.c_str();
  }
  if (!jni_parse(jni, &ret, &atypes, err)) return false;

  // Align instance 'this' with parsed args: JNI descriptor excludes this.
  std::vector<JvmValue> argv = frame->args;
  if (!frame->is_static) {
    if (argv.empty()) {
      if (err) *err = "missing this";
      return false;
    }
  } else {
    // Static: argv should match atypes size
  }

  const size_t need = atypes.size() + (frame->is_static ? 0 : 1);
  if (argv.size() < need) {
    // Pad with zeros / null for missing trailing args
    while (argv.size() < need) argv.push_back(JvmValue::make_int(0));
  }

  auto arg_i = [&](size_t i) -> int32_t {
    return argv[i].tag == JvmTag::Float ? static_cast<int32_t>(argv[i].v.f)
                                        : argv[i].v.i;
  };
  auto arg_f = [&](size_t i) -> float {
    return argv[i].tag == JvmTag::Float ? argv[i].v.f
                                        : static_cast<float>(argv[i].v.i);
  };
  auto arg_o = [&](size_t i) -> InvObject* {
    return argv[i].tag == JvmTag::Obj ? argv[i].v.o : nullptr;
  };

  // Encode a small shape key from static/instance + up to 4 arg tags + ret.
  // Cover the signatures used by the rewrite host smoke + common System/Math/IO.
  const bool st = frame->is_static;
  void* fn = entry->fn;

  auto finish_i = [&](int32_t v) {
    frame->result = JvmValue::make_int(v);
    return true;
  };
  auto finish_f = [&](float v) {
    frame->result = JvmValue::make_float(v);
    return true;
  };
  auto finish_o = [&](InvObject* v) {
    frame->result = JvmValue::make_obj(v);
    return true;
  };
  auto finish_v = [&]() {
    frame->result = JvmValue::make_void();
    return true;
  };

  if (st && atypes.empty()) {
    if (ret == JvmTag::Int) {
      using Fn = int32_t (*)();
      return finish_i(reinterpret_cast<Fn>(fn)());
    }
    if (ret == JvmTag::Float) {
      using Fn = float (*)();
      return finish_f(reinterpret_cast<Fn>(fn)());
    }
    if (ret == JvmTag::Obj) {
      using Fn = InvObject* (*)();
      return finish_o(reinterpret_cast<Fn>(fn)());
    }
    if (ret == JvmTag::Void) {
      using Fn = void (*)();
      reinterpret_cast<Fn>(fn)();
      return finish_v();
    }
  }

  if (st && atypes.size() == 1) {
    if (atypes[0] == JvmTag::Obj && ret == JvmTag::Void) {
      using Fn = void (*)(InvObject*);
      reinterpret_cast<Fn>(fn)(arg_o(0));
      return finish_v();
    }
    if (atypes[0] == JvmTag::Obj && ret == JvmTag::Int) {
      using Fn = int32_t (*)(InvObject*);
      return finish_i(reinterpret_cast<Fn>(fn)(arg_o(0)));
    }
    if (atypes[0] == JvmTag::Float && ret == JvmTag::Float) {
      using Fn = float (*)(float);
      return finish_f(reinterpret_cast<Fn>(fn)(arg_f(0)));
    }
    if (atypes[0] == JvmTag::Float && ret == JvmTag::Void) {
      using Fn = void (*)(float);
      reinterpret_cast<Fn>(fn)(arg_f(0));
      return finish_v();
    }
    if (atypes[0] == JvmTag::Int && ret == JvmTag::Void) {
      using Fn = void (*)(int32_t);
      reinterpret_cast<Fn>(fn)(arg_i(0));
      return finish_v();
    }
    if (atypes[0] == JvmTag::Int && ret == JvmTag::Int) {
      using Fn = int32_t (*)(int32_t);
      return finish_i(reinterpret_cast<Fn>(fn)(arg_i(0)));
    }
    if (atypes[0] == JvmTag::Int && ret == JvmTag::Float) {
      using Fn = float (*)(int32_t);
      return finish_f(reinterpret_cast<Fn>(fn)(arg_i(0)));
    }
  }

  if (st && atypes.size() == 2) {
    if (atypes[0] == JvmTag::Int && atypes[1] == JvmTag::Int &&
        ret == JvmTag::Float) {
      using Fn = float (*)(int32_t, int32_t);
      return finish_f(reinterpret_cast<Fn>(fn)(arg_i(0), arg_i(1)));
    }
    if (atypes[0] == JvmTag::Obj && atypes[1] == JvmTag::Obj &&
        ret == JvmTag::Int) {
      using Fn = int32_t (*)(InvObject*, InvObject*);
      return finish_i(reinterpret_cast<Fn>(fn)(arg_o(0), arg_o(1)));
    }
    if (atypes[0] == JvmTag::Obj && atypes[1] == JvmTag::Int &&
        ret == JvmTag::Int) {
      using Fn = int32_t (*)(InvObject*, int32_t);
      return finish_i(reinterpret_cast<Fn>(fn)(arg_o(0), arg_i(1)));
    }
  }

  // Instance: argv[0]=this
  if (!st && atypes.empty()) {
    if (ret == JvmTag::Int) {
      using Fn = int32_t (*)(InvObject*);
      return finish_i(reinterpret_cast<Fn>(fn)(arg_o(0)));
    }
    if (ret == JvmTag::Float) {
      using Fn = float (*)(InvObject*);
      return finish_f(reinterpret_cast<Fn>(fn)(arg_o(0)));
    }
    if (ret == JvmTag::Obj) {
      using Fn = InvObject* (*)(InvObject*);
      return finish_o(reinterpret_cast<Fn>(fn)(arg_o(0)));
    }
    if (ret == JvmTag::Void) {
      using Fn = void (*)(InvObject*);
      reinterpret_cast<Fn>(fn)(arg_o(0));
      return finish_v();
    }
  }

  if (!st && atypes.size() == 1) {
    if (atypes[0] == JvmTag::Int && ret == JvmTag::Int) {
      using Fn = int32_t (*)(InvObject*, int32_t);
      return finish_i(reinterpret_cast<Fn>(fn)(arg_o(0), arg_i(1)));
    }
    if (atypes[0] == JvmTag::Float && ret == JvmTag::Void) {
      using Fn = void (*)(InvObject*, float);
      reinterpret_cast<Fn>(fn)(arg_o(0), arg_f(1));
      return finish_v();
    }
    if (atypes[0] == JvmTag::Obj && ret == JvmTag::Void) {
      using Fn = void (*)(InvObject*, InvObject*);
      reinterpret_cast<Fn>(fn)(arg_o(0), arg_o(1));
      return finish_v();
    }
    if (atypes[0] == JvmTag::Obj && ret == JvmTag::Int) {
      using Fn = int32_t (*)(InvObject*, InvObject*);
      return finish_i(reinterpret_cast<Fn>(fn)(arg_o(0), arg_o(1)));
    }
    if (atypes[0] == JvmTag::Obj && ret == JvmTag::Obj) {
      using Fn = InvObject* (*)(InvObject*, InvObject*);
      return finish_o(reinterpret_cast<Fn>(fn)(arg_o(0), arg_o(1)));
    }
    if (atypes[0] == JvmTag::Int && ret == JvmTag::Void) {
      using Fn = void (*)(InvObject*, int32_t);
      reinterpret_cast<Fn>(fn)(arg_o(0), arg_i(1));
      return finish_v();
    }
  }

  if (!st && atypes.size() == 2) {
    if (atypes[0] == JvmTag::Obj && atypes[1] == JvmTag::Obj &&
        ret == JvmTag::Void) {
      using Fn = void (*)(InvObject*, InvObject*, InvObject*);
      reinterpret_cast<Fn>(fn)(arg_o(0), arg_o(1), arg_o(2));
      return finish_v();
    }
    if (atypes[0] == JvmTag::Obj && atypes[1] == JvmTag::Int &&
        ret == JvmTag::Obj) {
      using Fn = InvObject* (*)(InvObject*, InvObject*, int32_t);
      return finish_o(reinterpret_cast<Fn>(fn)(arg_o(0), arg_o(1), arg_i(2)));
    }
    if (atypes[0] == JvmTag::Float && atypes[1] == JvmTag::Int &&
        ret == JvmTag::Void) {
      using Fn = void (*)(InvObject*, float, int32_t);
      reinterpret_cast<Fn>(fn)(arg_o(0), arg_f(1), arg_i(2));
      return finish_v();
    }
    if (atypes[0] == JvmTag::Int && atypes[1] == JvmTag::Int &&
        ret == JvmTag::Void) {
      using Fn = void (*)(InvObject*, float, int32_t);
      reinterpret_cast<Fn>(fn)(arg_o(0), arg_f(1), arg_i(2));
      return finish_v();
    }
  }

  if (!st && atypes.size() == 3) {
    if (atypes[0] == JvmTag::Obj && atypes[1] == JvmTag::Int &&
        atypes[2] == JvmTag::Obj && ret == JvmTag::Void) {
      // GameRef.queueEvent(ResourceRef,I,String)V @ 0x0047DA30
      using Fn = void (*)(InvObject*, InvObject*, int32_t, InvObject*);
      reinterpret_cast<Fn>(fn)(arg_o(0), arg_o(1), arg_i(2), arg_o(3));
      return finish_v();
    }
    if (atypes[0] == JvmTag::Obj && atypes[1] == JvmTag::Obj &&
        atypes[2] == JvmTag::Obj && ret == JvmTag::Void) {
      // RenderRef.create(ResourceRef,RenderRef,String)V @ 0x00480EE0
      using Fn = void (*)(InvObject*, InvObject*, InvObject*, InvObject*);
      reinterpret_cast<Fn>(fn)(arg_o(0), arg_o(1), arg_o(2), arg_o(3));
      return finish_v();
    }
  }
  if (!st && atypes.size() == 4) {
    if (atypes[0] == JvmTag::Obj && atypes[1] == JvmTag::Int &&
        atypes[2] == JvmTag::Int && atypes[3] == JvmTag::Obj &&
        ret == JvmTag::Void) {
      using Fn = void (*)(InvObject*, InvObject*, int32_t, int32_t, InvObject*);
      reinterpret_cast<Fn>(fn)(arg_o(0), arg_o(1), arg_i(2), arg_i(3),
                               arg_o(4));
      return finish_v();
    }
  }
  if (!st && atypes.size() == 5) {
    if (atypes[0] == JvmTag::Obj && atypes[1] == JvmTag::Int &&
        atypes[2] == JvmTag::Float && atypes[3] == JvmTag::Float &&
        atypes[4] == JvmTag::Float && ret == JvmTag::Int) {
      // GroundRef.addTrafficN(GameRef,IFFF)I @ 0x00484050
      using Fn = int32_t (*)(InvObject*, InvObject*, int32_t, float, float,
                             float);
      return finish_i(reinterpret_cast<Fn>(fn)(arg_o(0), arg_o(1), arg_i(2),
                                               arg_f(3), arg_f(4), arg_f(5)));
    }
    if (atypes[0] == JvmTag::Obj && atypes[1] == JvmTag::Int &&
        atypes[2] == JvmTag::Int && atypes[3] == JvmTag::Obj &&
        atypes[4] == JvmTag::Obj && ret == JvmTag::Void) {
      using Fn = void (*)(InvObject*, InvObject*, int32_t, int32_t, InvObject*,
                          InvObject*);
      reinterpret_cast<Fn>(fn)(arg_o(0), arg_o(1), arg_i(2), arg_i(3), arg_o(4),
                               arg_o(5));
      return finish_v();
    }
    if (atypes[0] == JvmTag::Obj && atypes[1] == JvmTag::Float &&
        atypes[2] == JvmTag::Float && atypes[3] == JvmTag::Float &&
        atypes[4] == JvmTag::Obj && ret == JvmTag::Void) {
      using Fn = void (*)(InvObject*, InvObject*, float, float, float,
                          InvObject*);
      reinterpret_cast<Fn>(fn)(arg_o(0), arg_o(1), arg_f(2), arg_f(3), arg_f(4),
                               arg_o(5));
      return finish_v();
    }
  }

  if (err) {
    *err = std::string("unsupported shape ") + (frame->class_fqn ? frame->class_fqn : "?") +
           "." + (frame->method_name ? frame->method_name : "?") +
           (jni ? jni : "");
  }
  return false;
}

}  // namespace inv
