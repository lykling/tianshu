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

// TIANSHU version info implementation.
// Phase 0: minimal stub to validate build system.

#include "tianshu/version.h"

#include <cstdint>

int32_t tianshu_version_major(void) { return TIANSHU_VERSION_MAJOR; }

int32_t tianshu_version_minor(void) { return TIANSHU_VERSION_MINOR; }

int32_t tianshu_version_patch(void) { return TIANSHU_VERSION_PATCH; }

const char* tianshu_version_string(void) { return TIANSHU_VERSION_STRING; }

const char* tianshu_build_profile(void) {
#ifdef TIANSHU_PROFILE_DESKTOP
  return "desktop";
#elif defined(TIANSHU_PROFILE_SERVER)
  return "server";
#elif defined(TIANSHU_PROFILE_VEHICLE)
  return "vehicle";
#elif defined(TIANSHU_PROFILE_EMBEDDED)
  return "embedded";
#elif defined(TIANSHU_PROFILE_MCU)
  return "mcu";
#else
  return "unknown";
#endif
}
