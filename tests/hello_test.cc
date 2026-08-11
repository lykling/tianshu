// Copyright 2026 The TIANSHU Team. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// TIANSHU smoke test (Phase 0).
// Validates that the version info API works as expected.
// Returns 0 on success, non-zero on failure.

#include <cstdio>
#include <cstring>

#include "tianshu/version.h"

namespace {

int check_version_major() {
  if (tianshu_version_major() != TIANSHU_VERSION_MAJOR) {
    std::printf("FAIL: version major mismatch: got %d, expected %d\n", tianshu_version_major(),
                TIANSHU_VERSION_MAJOR);
    return 1;
  }
  return 0;
}

int check_version_string() {
  const char* v = tianshu_version_string();
  if (std::strcmp(v, TIANSHU_VERSION_STRING) != 0) {
    std::printf("FAIL: version string mismatch: got %s, expected %s\n", v, TIANSHU_VERSION_STRING);
    return 1;
  }
  return 0;
}

int check_build_profile() {
  const char* p = tianshu_build_profile();
  if (p == nullptr || std::strlen(p) == 0) {
    std::printf("FAIL: build profile is empty\n");
    return 1;
  }
  return 0;
}

}  // namespace

int main() {
  int failures = 0;
  failures += check_version_major();
  failures += check_version_string();
  failures += check_build_profile();

  if (failures == 0) {
    std::printf("PASS: all checks succeeded (version=%s, profile=%s)\n", tianshu_version_string(),
                tianshu_build_profile());
    return 0;
  }
  std::printf("FAIL: %d check(s) failed\n", failures);
  return 1;
}
