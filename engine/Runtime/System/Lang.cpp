#include "natives.hpp"
#include "runtime.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace inv {
namespace {

// PE StringBuffer intern pool (sub_41E610 / sub_41EB70): hash len+(mix);
// bucket count 0xA97; entry +0x4 cstr +0x8 len +0xC refcount +0x10 instance.
struct StringBufferNode {
  std::unique_ptr<char[]> data;
  int32_t length = 0;
  int32_t refcount = 0;
  InvObject* instance = nullptr;
};

std::mutex g_string_buffer_mu;
std::unordered_map<std::string, std::unique_ptr<StringBufferNode>> g_string_buffer;

StringBufferNode* string_buffer_find(const char* cstr) {
  if (!cstr) {
    return nullptr;
  }
  auto it = g_string_buffer.find(cstr);
  if (it == g_string_buffer.end()) {
    return nullptr;
  }
  return it->second.get();
}

// PE StringBuffer_setInstance @ 0x0041ECD0 — store instance @ entry+0x10.
void string_buffer_set_instance(const char* cstr, InvObject* instance) {
  StringBufferNode* node = string_buffer_find(cstr);
  if (!node) {
    std::fprintf(stderr, "!StringBuffer::setInstance: instance not found\n");
    return;
  }
  node->instance = instance;
}

// PE StringBuffer_remove @ 0x0041E830 — dec ref @ entry+0xC; unlink if last.
void string_buffer_remove(const char* cstr) {
  if (!cstr) {
    std::fprintf(stderr, "!StringBuffer: Cannot remove <null> string.\n");
    return;
  }
  auto it = g_string_buffer.find(cstr);
  if (it == g_string_buffer.end()) {
    std::fprintf(stderr, "!StringBuffer: Cannot remove string.\n");
    return;
  }
  if (--it->second->refcount == 0) {
    g_string_buffer.erase(it);
  }
}

// PE JVM_String_from_cstr @ 0x004174A0 miss path — intern cstr, wire shell.
InvObject* lang_string_from_cstr(const char* cstr) {
  if (!cstr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(g_string_buffer_mu);
  auto it = g_string_buffer.find(cstr);
  if (it != g_string_buffer.end()) {
    StringBufferNode* node = it->second.get();
    ++node->refcount;
    if (!node->instance) {
      auto* shell = new InvString{node->data.get()};
      node->instance = reinterpret_cast<InvObject*>(shell);
    }
    return node->instance;
  }
  auto node = std::make_unique<StringBufferNode>();
  node->length = static_cast<int32_t>(std::strlen(cstr));
  node->data = std::make_unique<char[]>(static_cast<size_t>(node->length) + 1u);
  std::memcpy(node->data.get(), cstr, static_cast<size_t>(node->length) + 1u);
  node->refcount = 1;
  auto* shell = new InvString{node->data.get()};
  node->instance = reinterpret_cast<InvObject*>(shell);
  g_string_buffer.emplace(std::string(cstr), std::move(node));
  return reinterpret_cast<InvObject*>(shell);
}

}  // namespace

// --- String ---

void java_lang_String_finalize(InvObject* self) {
  // PE @ 0x00486190 size 0x46. UnboxArg this. cstr =
  // JVM_vm_get_int_field(this, dword_62E008). cstr==0 →
  // ret. Else thiscall StringBuffer_setInstance(*(CallInfo)+0x68, cstr, 0)
  // then StringBuffer_remove(same, cstr) (dec ref @ entry+0xC; free if
  // last). Host: InvString::utf8 ≡ Native.ptr; pool helpers above.
  if (!self) {
    return;
  }
  const char* cstr = reinterpret_cast<InvString*>(self)->utf8;
  if (!cstr) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_string_buffer_mu);
  string_buffer_set_instance(cstr, nullptr);
  string_buffer_remove(cstr);
}

int32_t java_lang_String_length(InvObject* self) {
  // PE @ 0x00481FE0 size 0x34. UnboxArg @ 0x0045D910 this. cstr =
  // JVM_vm_get_int_field @ 0x0042AB50 (this, dword_62E008). Inline
  // strlen (or ecx,-1; xor eax,eax; repne scasb; not ecx; dec ecx → EAX).
  // No null-this / no cstr-null test (field miss → 0 then scasb @0).
  // C byte length, not UTF-16. Host: string_cstr + strlen.
  const char* s = string_cstr(self);
  return static_cast<int32_t>(std::strlen(s));
}

int32_t java_lang_String_intValue(InvObject* self) {
  // PE @ 0x00481ED0 size 0x3C. Twin floatValue @ 0x00481F10 ("%f").
  // UnboxArg this. cstr = JVM_vm_get_int_field(this, dword_62E008).
  // Util_Sscanf @ 0x00551240 cstr,"%d",&local (aD_17). Ignore count;
  // fail → garbage stack (push ecx seed). NO null-cstr check.
  // Register Natives_RegisterAll @ 0x0048828A.
  int32_t v;
  const char* cstr = string_cstr(self);
  std::sscanf(cstr, "%d", &v);
  return v;
}

float java_lang_String_floatValue(InvObject* self) {
  // PE @ 0x00481F10 size 0x3C. Twin intValue @ 0x00481ED0 ("%d").
  // UnboxArg this. cstr = JVM_vm_get_int_field(this, dword_62E008).
  // Util_Sscanf @ 0x00551240 cstr,"%f",&local (asc_6133A8). Ignore count;
  // fail → garbage stack (push ecx seed). NO null-cstr check.
  // fld [var_4]; ret. Register Natives_RegisterAll @ 0x004882A9.
  float v;
  const char* cstr = string_cstr(self);
  std::sscanf(cstr, "%f", &v);
  return v;
}

InvObject* java_lang_String_append(InvObject* self, int32_t ascii) {
  // PE @ 0x00481F50 size 0x88. Unbox this+I; strncpy cstr→buf[256];
  // ascii==8 → truncate; ascii>=0x20 → append byte; else no-op;
  // JVM_String_from_cstr (new String). Not decimal int append.
  std::string s = string_cstr(self);
  if (ascii == 8) {  // backspace
    if (!s.empty()) {
      s.pop_back();
    }
  } else if (ascii >= 0x20) {
    s.push_back(static_cast<char>(ascii));
  }
  return lang_string_from_cstr(s.c_str());
}

InvObject* java_lang_String_token(InvObject* self, int32_t n, InvObject* delimiters) {
  // PE @ 0x00481CF0 size 0xDD. UnboxArg this,n,delim. this cstr =
  // JVM_vm_get_int_field(this, dword_62E008); copy strlen+1 (stack 0x100
  // or malloc 0x54F560; strncpy 0x551120). CRT strtok @ 0x00554C70 (skip
  // leading/consecutive delims, punch NUL). Loop: if (n-- == 0) break else
  // next. sub_4174A0(cstr): nullptr in → nullptr out (no empty String).
  const char* src = string_cstr(self);
  const char* delim = string_cstr(delimiters);
  std::vector<char> buf(src, src + std::strlen(src) + 1);
  char* ctx = nullptr;
#if defined(_MSC_VER)
  char* tok = strtok_s(buf.data(), delim, &ctx);
#else
  char* tok = strtok_r(buf.data(), delim, &ctx);
#endif
  while (tok) {
    if (n-- == 0) {
      break;
    }
#if defined(_MSC_VER)
    tok = strtok_s(nullptr, delim, &ctx);
#else
    tok = strtok_r(nullptr, delim, &ctx);
#endif
  }
  if (!tok) {
    return nullptr;
  }
  return lang_string_from_cstr(tok);
}

// --- Integer / Float ---

InvObject* java_lang_Integer_toString(int32_t i) {
  // PE @ 0x00481E30 size 0x49. Static UnboxArg dest0=nullptr (skip this),
  // dest1=int. sub esp,0x404 → DWORD + buf[0x400]. sprintf sub_551220
  // (210 xrefs, not renamed; callee Util_Vsprintf) buf,"%d",i then
  // JVM_String_from_cstr @ 0x004174A0. Contrast toHexString @ 0x00481E80
  // same layout, format "0x%08x" (host already; not patched).
  char buf[1024];
  std::snprintf(buf, sizeof(buf), "%d", i);
  return lang_string_from_cstr(buf);
}

InvObject* java_lang_Integer_toHexString(int32_t i) {
  // PE @ 0x00481E80 size 0x49. Same skeleton as toString @ 0x00481E30:
  // sub esp,0x404 → DWORD + buf[0x400]. Format a0x08x @ 0x0061339C =
  // "0x%08x" (prefix, zero-pad 8, lowercase; not JDK / not %X).
  char buf[1024];
  std::snprintf(buf, sizeof(buf), "0x%08x", static_cast<unsigned>(i));
  return lang_string_from_cstr(buf);
}

InvObject* java_lang_Float_toString(float f, InvObject* fmt) {
  // PE @ 0x00481DD0 size 0x51 (int_convert). Static UnboxArg
  // dest0=nullptr (skip this), dest1=float, dest2=fmt cstr (L box+8).
  // sub esp,0x408: f_as_double + fmt* + float + dst[0x400].
  // fld dword; fstp qword; push fmt; push dst; Util_Sprintf @ 0x00551220
  // (va→Util_Vsprintf; 210+ xrefs) then add esp,10h (dst+fmt+double).
  // JVM_String_from_cstr @ 0x004174A0. No null/empty fmt guard.
  // Contrast Integer.toString @ 0x00481E30: fixed "%d", sub esp,0x404.
  // Host: already-unboxed; snprintf into 0x400; (double)f ≡ fstp.
  char buf[1024];
  std::snprintf(buf, sizeof(buf), string_cstr(fmt), static_cast<double>(f));
  return lang_string_from_cstr(buf);
}

// --- Math ---

float java_lang_Math_random() {
  // PE @ 0x0047C820 size 0x2a. Static ()F — no UnboxArg, no this/args
  // (push ecx scratch only). CRT_rand @ 0x005D7408 → and 0x7FFF
  // (int_convert: 32767); cmp/jz → store 0x7FFE (32766) if max; else
  // keep. fild dword; fmul flt_Math_random_1div32767 @ 0x005F1388
  // (LE 00 01 00 38 = IEEE 0x38000100 ≡ 1/32767.0f); pop ecx; retn ST0
  // → [0, 1). Contrast setrandseed @ 0x0047C7F0: UnboxArg int →
  // sub_5D73FB (CRT srand), no rand consume. Contrast randomize @
  // 0x0047C7D0: wall-ms → same srand. Host: std::rand + same clamp/scale.
  int v = std::rand() & 0x7fff;
  if (v == 0x7fff) {
    v = 0x7ffe;
  }
  return static_cast<float>(v) * (1.0f / 32767.0f);
}

float java_lang_Math_sqrt(float a) {
  // PE @ 0x0047C850 size 0x1b. Static UnboxArg dest0=nullptr (skip this),
  // dest1=float into [esp+arg_0]. fld dword [esp+10h]; x87 fsqrt (not CRT
  // sqrt); add esp,0xC; retn ST0. No NaN/neg check. Natives_RegisterAll
  // @ 0x0048862C. Host: already-unboxed std::sqrt(float) ≡ fsqrt.
  return std::sqrt(a);
}

void java_lang_Math_randomize() {
  // PE @ 0x0047C7D0 size 0x12. Static ()V; no UnboxArg; no this/args.
  // call sub_5516C0 → QPC (now-epoch)/freq * flt_5F0910 @ 0x005F0910
  // (0x447A0000 = 1000.0) → wall ms on ST0; sub_5D6750 fistp chop
  // (RC=11) → EAX seed; push EAX; sub_5D73FB (= CRT srand) → TLS+0x14;
  // pop ecx; retn.
  // Contrast setrandseed @ 0x0047C7F0: UnboxArg int → same srand (explicit
  // seed). Contrast random @ 0x0047C820: CRT_rand consume (no reseed).
  // Host: time_current() = wall seconds ≡ stock ms*0.001; seed =
  // trunc(ms) via (int32_t)(sec*1000.0) then std::srand.
  const double ms = static_cast<double>(time_current()) * 1000.0;
  std::srand(static_cast<unsigned>(static_cast<int32_t>(ms)));
}

void java_lang_Math_setrandseed(int32_t seed) {
  // PE @ 0x0047C7F0 size 0x28. Static (I)V.
  // UnboxArg: dest0=nullptr (skip this), dest1=&stack int; zero slot then
  // JVM_UnboxArg @ 0x0045D910. Hex-Rays misreads push 0 as srand arg —
  // real seed is [esp+var_4] after unbox → push edx; sub_5D73FB (= CRT
  // srand → TLS+0x14); add esp,14h; retn. No rename of sub_5D73FB.
  // Contrast randomize @ 0x0047C7D0: no UnboxArg; wall-ms → same srand.
  // Contrast random @ 0x0047C820: CRT_rand consume (no reseed).
  // Host: already-unboxed int → std::srand((unsigned)seed).
  std::srand(static_cast<unsigned>(seed));
}

}  // namespace inv
