#include "game_boot.hpp"

#include "Resources.h"
#include "host_objects.hpp"
#include "input_win32.hpp"
#include "natives.hpp"
#include "render_d3d9.hpp"
#include "rpak.hpp"
#include "runtime.hpp"
#include "tree_interp.hpp"
#include "video_fmv.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

// Body split into fragments to stay under the 128 KB editor buffer
// cap (see native/tools/split_source.py). They are concatenated here,
// so this translation unit is identical to the single-file version.
#include "game_boot_part1.inc"
#include "game_boot_part2.inc"
