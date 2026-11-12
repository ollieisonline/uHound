// The one place uHound's version and home are written down.
//
// kVersion is what this build calls itself: the About text, the Hound
// client string and the startup update check (update_check.h) all read it, so a
// release only has to change this line.
#pragma once

namespace ufin {

constexpr const char* kVersion = "1.3.0";

// Where releases are published (the startup update check asks GitHub for the
// latest one) and where people can read about / download them.
constexpr const char* kGithubOwner = "ollieisonline";
constexpr const char* kGithubRepo = "uHound";

// The package name uHound has in the Homebrew App Store's repo.json
// (https://wiiu.cdn.fortheusers.org/repo.json). Compared case-insensitively.
constexpr const char* kStorePackageName = "uhound";

} // namespace ufin
