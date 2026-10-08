#include "host_objects.hpp"
#include "natives.hpp"
#include "runtime.hpp"
#include "rpak.hpp"
#include "render_d3d9.hpp"
#include "input_win32.hpp"
#include "tree_interp.hpp"
#include "audio_win32.hpp"
#include "System.h"
#include "Resources.h"
#include "Resources_internal.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

// Body split into fragments to stay under the 128 KB editor buffer
// cap (see native/tools/split_source.py). They are concatenated here,
// so this translation unit is identical to the single-file version.
#include "Resources_part1.inc"
#include "Resources_part2.inc"
#include "Resources_part3.inc"
#include "Resources_part4.inc"
