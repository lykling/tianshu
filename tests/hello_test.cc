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

// TIANSHU smoke test using GoogleTest (includes GoogleMock).
// Validates that the version info C ABI works as expected.

#include <cstring>

#include <gtest/gtest.h>

#include "tianshu/version.h"

TEST(VersionTest, MajorMatchesMacro) { EXPECT_EQ(tianshu_version_major(), TIANSHU_VERSION_MAJOR); }

TEST(VersionTest, MinorMatchesMacro) { EXPECT_EQ(tianshu_version_minor(), TIANSHU_VERSION_MINOR); }

TEST(VersionTest, PatchMatchesMacro) { EXPECT_EQ(tianshu_version_patch(), TIANSHU_VERSION_PATCH); }

TEST(VersionTest, StringMatchesMacro) {
  EXPECT_STREQ(tianshu_version_string(), TIANSHU_VERSION_STRING);
}

TEST(VersionTest, StringContainsVersionNumbers) {
  const char* version = tianshu_version_string();
  EXPECT_NE(version, nullptr);
  EXPECT_GT(std::strlen(version), 0u);
}

TEST(VersionTest, BuildProfileIsValid) {
  const char* profile = tianshu_build_profile();
  EXPECT_NE(profile, nullptr);
  EXPECT_GT(std::strlen(profile), 0u);

  const char* known_profiles[] = {"desktop", "server", "vehicle", "embedded", "mcu"};
  bool found = false;
  for (const char* p : known_profiles) {
    if (std::strcmp(profile, p) == 0) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "Unknown profile: " << profile;
}
