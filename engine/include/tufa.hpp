#pragma once

#include "jvm.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace inv {

// Parse a SLRR TUFA .class blob into a JvmClass (methods + trees metadata).
// Returns false on format errors.
// One on-disk .class may contain several concatenated TUFA blobs (one per
// public class from the same .java) — use tufa_load_file_all for those.
bool tufa_parse(const uint8_t* data, size_t size, JvmClass* out, std::string* err);

bool tufa_load_file(const char* path, JvmClass* out, std::string* err);
bool tufa_load_file_all(const char* path, std::vector<JvmClass>* out,
                        std::string* err);

}  // namespace inv
