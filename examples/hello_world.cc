// Copyright 2026 Pride Leong.
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

// TIANSHU hello world example.
// Phase 0: minimal smoke test for both CMake and Bazel build systems.

#include <iostream>

#include "tianshu/version.h"

int main() {
  std::cout << "TIANSHU v" << tianshu_version_string() << "\n";
  std::cout << "  major:    " << tianshu_version_major() << "\n";
  std::cout << "  minor:    " << tianshu_version_minor() << "\n";
  std::cout << "  patch:    " << tianshu_version_patch() << "\n";
  std::cout << "  profile:  " << tianshu_build_profile() << "\n";
  return 0;
}
