#include "jvm.hpp"
#include "jvm_internal.hpp"
#include "callinfo.hpp"
#include "tufa.hpp"
#include "jvm_bridge.hpp"
#include "tree_interp.hpp"
#include "host_objects.hpp"
#include "runtime.hpp"
#include "natives.hpp"
#include "rpak.hpp"
#include "video_fmv.hpp"
#include "render_d3d9.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace inv {

// PE JVM load / compile gate (ticket jvm_load_host; ticket VA 0x00416000 is
// mid-body of JVM_compileHook, not a function entry):
//   java.lang.System.compileAll @ 0x0047C080
//     → JVM_compileAllDir @ 0x00418EB0 (recurse *; *.java → compileSource)
//     → JVM_compileSource @ 0x004165A0 (TUFA hdr magic/ver/build check;
//        may parse .java via sub_408F60 then JVM_compileHook @ 0x00412B10)
//   JVM_getClass @ 0x00410890
//     → JVM_findLoadedClass @ 0x00411610 (hash)
//     → JVM_loadClassByFqn @ 0x00410630
//         → Classpath_resolvePackageDir @ 0x0041D290
//         → JavaMachine_loadClass @ 0x00410430 (compileSource + read .class
//            → JVM_addClass_fromChunks @ 0x00411B20)
// Soft: load existing TUFA .class (no .java→TREE compiler / compileHook emit).

std::vector<std::string> jvm_split_ws(const std::string& s) {
  std::istringstream iss(s);
  std::vector<std::string> out;
  std::string w;
  while (iss >> w) out.push_back(w);
  return out;
}

bool jvm_file_exists(const char* path) {
  if (FILE* f = std::fopen(path, "rb")) {
    std::fclose(f);
    return true;
  }
  return false;
}

// Soft stand-in for Classpath_resolvePackageDir @ 0x0041D290 + path join used
// by JVM_loadClassByFqn @ 0x00410630 → JavaMachine_loadClass @ 0x00410430
// (`{dir}\\{simple}.class`). Package→filesystem map is kClasspathMap.
bool resolve_classpath_file(const char* game_root, const char* fqn,
                            std::string* out_path) {
  if (!game_root || !fqn || !out_path) return false;
  const ClasspathMapEntry* best = nullptr;
  size_t best_len = 0;
  for (size_t i = 0; i < kClasspathMapCount; ++i) {
    const char* pkg = kClasspathMap[i].java_package;
    const size_t plen = std::strlen(pkg);
    if (std::strncmp(fqn, pkg, plen) != 0) continue;
    if (fqn[plen] != '.' && fqn[plen] != '\0') continue;
    if (plen >= best_len) {
      best_len = plen;
      best = &kClasspathMap[i];
    }
  }

  auto file_ok = [](const std::string& p) -> bool {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
  };

  if (best) {
    const char* rest = fqn + best_len;
    if (*rest == '.') ++rest;
    *out_path = std::string(game_root);
    if (!out_path->empty() && out_path->back() != '/' &&
        out_path->back() != '\\') {
      out_path->push_back('/');
    }
    *out_path += best->filesystem_prefix;
    out_path->push_back('/');
    for (const char* p = rest; *p; ++p) {
      out_path->push_back(*p == '.' ? '/' : *p);
    }
    *out_path += ".class";
    if (file_ok(*out_path)) return true;
  }

  // java.game.cars.Baiern_VT → cars/racers/*/scripts/Baiern_VT.class
  if (std::strncmp(fqn, "java.game.cars.", 15) == 0) {
    const char* simple = fqn + 15;
    for (const char* p = simple; *p; ++p)
      if (*p == '.') simple = p + 1;
#ifdef _WIN32
    std::string pattern = std::string(game_root) + "\\cars\\racers\\*_data\\scripts\\";
    pattern += simple;
    pattern += ".class";
    // Expand *_data via FindFirstFile on racers\*
    std::string search = std::string(game_root) + "\\cars\\racers\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        std::string cand = std::string(game_root) + "/cars/racers/" +
                           fd.cFileName + "/scripts/" + simple + ".class";
        if (file_ok(cand)) {
          *out_path = cand;
          FindClose(h);
          return true;
        }
      } while (FindNextFileA(h, &fd));
      FindClose(h);
    }
#else
    (void)simple;
#endif
  }
  return best != nullptr && !out_path->empty();
}

void Jvm::set_game_root(const char* root) {
  game_root_ = root ? root : "";
  classes_.reserve(256);  // avoid realloc invalidating tree_eval snapshots
}

void Jvm::upsert_class(JvmClass cls) {
  for (auto& c : classes_) {
    if (c.name == cls.name) {
      c = std::move(cls);
      return;
    }
  }
  classes_.push_back(std::move(cls));
}

bool Jvm::load_index(const char* path) {
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "[jvm] cannot open %s\n", path);
    return false;
  }
  classes_.clear();
  JvmClass* cur = nullptr;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    auto tok = jvm_split_ws(line);
    if (tok.empty()) continue;
    if (tok[0] == "CLASS" && tok.size() >= 2) {
      classes_.push_back({});
      cur = &classes_.back();
      cur->name = tok[1];
    } else if (!cur) {
      continue;
    } else if (tok[0] == "SUPER" && tok.size() >= 2) {
      cur->super_name = tok[1];
    } else if (tok[0] == "FILE" && tok.size() >= 2) {
      cur->file = tok[1];
    } else if (tok[0] == "METHOD" && tok.size() >= 3) {
      JvmMethod m;
      m.name = tok[1];
      m.signature = tok[2];
      m.is_native = true;
      for (size_t i = 3; i < tok.size(); ++i) {
        if (tok[i] == "native=0") m.is_native = false;
        if (tok[i] == "native=1") m.is_native = true;
      }
      cur->methods.push_back(std::move(m));
    }
  }
  return !classes_.empty();
}

// Soft: FilePool_ReadEntireFile + JVM_addClass_fromChunks @ 0x00411B20 side of
// JavaMachine_loadClass @ 0x00410430 (skips PE compileSource refresh).
bool Jvm::load_class_file(const char* path) {
  std::vector<JvmClass> all;
  std::string err;
  if (!tufa_load_file_all(path, &all, &err)) {
    std::fprintf(stderr, "[jvm] tufa load failed %s: %s\n", path, err.c_str());
    return false;
  }
  for (auto& cls : all) upsert_class(std::move(cls));
  return !all.empty();
}

// Soft gate for JVM_getClass @ 0x00410890 miss path:
// find_class ≈ JVM_findLoadedClass @ 0x00411610; else resolve + load_class_file
// ≈ JVM_loadClassByFqn @ 0x00410630 → JavaMachine_loadClass @ 0x00410430.
bool Jvm::load_class(const char* fqn) {
  if (fqn && std::strcmp(fqn, "java.render.osd.Rectangle") == 0)
    fqn = "java.render.Rectangle";
  if (find_class(fqn)) return true;
  if (game_root_.empty()) {
    std::fprintf(stderr, "[jvm] load_class: no game_root\n");
    return false;
  }
  std::string path;
  if (!resolve_classpath_file(game_root_.c_str(), fqn, &path)) {
    std::fprintf(stderr, "[jvm] no classpath entry for %s\n", fqn);
    return false;
  }
  if (jvm_file_exists(path.c_str())) {
    if (!load_class_file(path.c_str())) return false;
    return find_class(fqn) != nullptr;
  }

  // Sibling class: MainMenuDialog lives inside MainMenu.class (multi-TUFA).
  // Scan the package directory for other .class files that might contain it.
  const size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) {
    std::fprintf(stderr, "[jvm] class file missing %s\n", path.c_str());
    return false;
  }
  const std::string dir = path.substr(0, slash);
#if defined(_WIN32)
  WIN32_FIND_DATAA fd;
  const std::string pattern = dir + "\\*.class";
  HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    std::fprintf(stderr, "[jvm] class file missing %s\n", path.c_str());
    return false;
  }
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::string cand = dir + "\\" + fd.cFileName;
    if (!load_class_file(cand.c_str())) continue;
    if (find_class(fqn)) {
      FindClose(h);
      return true;
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  (void)dir;
#endif
  std::fprintf(stderr, "[jvm] class file missing %s\n", path.c_str());
  return false;
}

namespace {

int compile_scan_dir(Jvm* jvm, const std::string& dir, int depth) {
  if (!jvm || depth > 10) return 0;
  int n = 0;
#ifdef _WIN32
  WIN32_FIND_DATAA fd;
  const HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;
  do {
    if (fd.cFileName[0] == '.') continue;
    const std::string full = dir + "\\" + fd.cFileName;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      n += compile_scan_dir(jvm, full, depth + 1);
      continue;
    }
    const size_t len = std::strlen(fd.cFileName);
    if (len < 7) continue;
    if (_stricmp(fd.cFileName + (len - 6), ".class") != 0) continue;
    if (jvm->load_class_file(full.c_str())) ++n;
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  (void)dir;
#endif
  return n;
}

std::string join_root(const std::string& root, const char* rel) {
  std::string out = root;
  if (!out.empty() && out.back() != '/' && out.back() != '\\') out.push_back('/');
  if (rel) {
    while (*rel == '/' || *rel == '\\') ++rel;
    out += rel;
  }
  while (!out.empty() && (out.back() == '/' || out.back() == '\\')) out.pop_back();
  return out;
}

}  // namespace

// Soft ≈ JVM_compileAllDir @ 0x00418EB0 (from System.compileAll @ 0x0047C080).
// PE walks *.java → JVM_compileSource @ 0x004165A0 → JVM_compileHook @ 0x00412B10
// (TUFA emit @ ~0x00416000). Soft has no .java compiler: recurse and load
// existing TUFA *.class via load_class_file; return successful load count.
int32_t Jvm::compile_all(const char* rel_path) {
  if (game_root_.empty()) return 0;
  const char* rel = (rel_path && rel_path[0]) ? rel_path : ".";
  int32_t total = 0;
  if (std::strcmp(rel, ".") == 0 || std::strcmp(rel, "./") == 0) {
    for (size_t i = 0; i < kClasspathMapCount; ++i) {
      total += compile_scan_dir(
          this, join_root(game_root_, kClasspathMap[i].filesystem_prefix), 0);
    }
  } else {
    total = compile_scan_dir(this, join_root(game_root_, rel), 0);
  }
  return total;
}

const JvmClass* Jvm::find_class(const char* fqn) const {
  for (auto& c : classes_) {
    if (c.name == fqn) return &c;
  }
  return nullptr;
}

const JvmMethod* Jvm::find_method(const JvmClass& cls, const char* name,
                                  const char* signature) const {
  const JvmMethod* by_name = nullptr;
  for (auto& m : cls.methods) {
    if (m.name != name) continue;
    if (signature && m.signature == signature) return &m;
    if (!by_name) by_name = &m;
  }
  // TUFA MTHD rows occasionally mis-pair name/sig (e.g. addCustomGroups
  // tagged "(I)V"). Fall back to first name match when exact sig misses.
  return by_name;
}


}  // namespace inv
