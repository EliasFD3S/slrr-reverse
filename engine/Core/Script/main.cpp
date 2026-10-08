#include "natives.hpp"
#include "jvm_bridge.hpp"
#include "jvm.hpp"
#include "runtime.hpp"
#include "host_objects.hpp"
#include "rpak.hpp"
#include "tree_interp.hpp"
#include "game_boot.hpp"
#include "game_script.hpp"
#include "render_d3d9.hpp"
#include "input_win32.hpp"
#include "audio_win32.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// Body split into fragments to stay under the 128 KB editor buffer
// cap (see native/tools/split_source.py). They are concatenated here,
// so this translation unit is identical to the single-file version.
#include "main_part1.inc"
#include "main_part2.inc"
#include "main_part3.inc"
#include "main_part4.inc"
