#include "tree_interp.hpp"
#include "tree_interp_internal.hpp"
#include "runtime.hpp"
#include "rpak.hpp"
#include "natives.hpp"
#include "host_objects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Body split into fragments to stay under the 128 KB editor buffer
// cap (see native/tools/split_source.py). They are concatenated here,
// so this translation unit is identical to the single-file version.
#include "tree_eval_part1.inc"
#include "tree_eval_part2.inc"
#include "tree_eval_part3.inc"
