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

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// Body split into fragments to stay under the 128 KB editor buffer
// cap (see native/tools/split_source.py). They are concatenated here,
// so this translation unit is identical to the single-file version.
#include "jvm_part1.inc"
#include "jvm_part2.inc"
