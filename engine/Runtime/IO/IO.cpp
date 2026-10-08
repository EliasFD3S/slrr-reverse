#include "host_objects.hpp"
#include "runtime.hpp"
#include "rpak.hpp"
#include "tree_interp.hpp"
#include "input_win32.hpp"
#include "game_script.hpp"
#include "jvm.hpp"
#include "render_d3d9.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Body split into fragments to stay under the 128 KB editor buffer
// cap (see native/tools/split_source.py). They are concatenated here,
// so this translation unit is identical to the single-file version.
#include "IO_part1.inc"
#include "IO_part2.inc"
