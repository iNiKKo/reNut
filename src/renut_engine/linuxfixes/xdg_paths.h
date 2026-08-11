// XDG base-directory paths, so renut never writes next to its own executable.
//
// Why
// ---
// By default the SDK puts everything beside the binary:
//
//   rex_app.cpp:145   config_path = exe_dir / "<app>.toml"
//   rex_app.cpp:170   log_dir     = exe_dir / "logs"
//   path_config_store.h  <exe_dir>/<app>.cfg
//
// That is fine when running out of a build tree, but it breaks the moment renut
// ships as anything users don't own the directory of -- an AppImage (read-only
// squashfs mount), a Flatpak, or a system-wide install under /opt or /usr. The
// app would fail to save its config and, worse, fail to open a log file just
// when you most want one.
//
// This header resolves the three writable locations to the XDG base directory
// spec instead:
//
//   config   $XDG_CONFIG_HOME/renut   (default ~/.config/renut)
//   state    $XDG_STATE_HOME/renut    (default ~/.local/state/renut)
//   logs     <state>/logs
//
// Game/user data already lands in ~/.local/share/renut via the path wizard, so
// it needs no change here.
//
// Linux only, by design: on Windows the exe-relative layout is conventional and
// writable, so that path is left exactly as it was. Every include of this header
// is guarded by #ifndef _WIN32.
//
// Existing installs are migrated rather than abandoned: the first time a file is
// wanted at its new location and is not there, an old copy sitting beside the
// executable is copied across. That keeps a developer's working renut.cfg (with
// their asset paths already filled in) after the switch.

#pragma once

#ifndef _WIN32

#include <climits>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

namespace renut::linuxfixes {

// Directory containing the running executable. Used only to find legacy files
// to migrate, never as a write target.
inline std::filesystem::path ExecutableDir() {
  char buffer[PATH_MAX] = {};
  const ssize_t len = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (len > 0) {
    return std::filesystem::path(std::string(buffer, static_cast<size_t>(len))).parent_path();
  }
  std::error_code ec;
  auto cwd = std::filesystem::current_path(ec);
  return ec ? std::filesystem::path(".") : cwd;
}

// Reads an XDG environment variable, ignoring the relative paths the spec says
// to treat as unset. Falls back to $HOME/<fallback_suffix>.
inline std::filesystem::path XdgDir(const char* env_name, const char* fallback_suffix) {
  if (const char* value = std::getenv(env_name)) {
    // The spec requires absolute paths; anything else must be ignored.
    if (value[0] == '/') {
      return std::filesystem::path(value);
    }
  }
  const char* home = std::getenv("HOME");
  if (!home || home[0] == '\0') {
    // No HOME at all (unusual). Fall back to the CWD so we still land somewhere
    // writable rather than trying to write into a read-only mount.
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    return (ec ? std::filesystem::path(".") : cwd) / ".renut";
  }
  return std::filesystem::path(home) / fallback_suffix;
}

inline std::filesystem::path ConfigDir() {
  return XdgDir("XDG_CONFIG_HOME", ".config") / "renut";
}

inline std::filesystem::path StateDir() {
  return XdgDir("XDG_STATE_HOME", ".local/state") / "renut";
}

inline std::filesystem::path LogDir() {
  return StateDir() / "logs";
}

// Creates dir if needed. Returns false if it could not be created, letting
// callers fall back rather than handing the SDK an unusable path.
inline bool EnsureDir(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return !ec && std::filesystem::is_directory(dir, ec);
}

// Resolves a writable path for file_name inside dir, migrating a pre-existing
// copy from beside the executable on first use.
inline std::filesystem::path ResolveWithMigration(const std::filesystem::path& dir,
                                                  const std::string& file_name) {
  const auto target = dir / file_name;
  std::error_code ec;

  if (std::filesystem::exists(target, ec)) {
    return target;
  }
  if (!EnsureDir(dir)) {
    // Nowhere to put it; let the caller keep the old behaviour.
    return ExecutableDir() / file_name;
  }

  const auto legacy = ExecutableDir() / file_name;
  if (std::filesystem::exists(legacy, ec)) {
    std::filesystem::copy_file(legacy, target, std::filesystem::copy_options::skip_existing, ec);
    // If the copy failed the target simply will not exist yet, which is the same
    // as a clean first run -- nothing is lost, so ec is deliberately ignored.
  }
  return target;
}

// Path for the next sequential log file, mirroring the SDK's NextSequentialLogPath
// ("<app>_NNN.log"). Reimplemented here because setting the log_file cvar bypasses
// the SDK's own sequencing, and losing per-run logs would be a regression.
inline std::filesystem::path NextSequentialLog(const std::filesystem::path& dir,
                                               const std::string& app_name) {
  if (!EnsureDir(dir)) {
    return {};
  }

  const std::string prefix = app_name + "_";
  unsigned max_seq = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.size() < prefix.size() + 5 || name.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }
    if (entry.path().extension() != ".log") {
      continue;
    }
    // Digits between the prefix and ".log"; skip rotated names like renut_001.1.log.
    const std::string stem = entry.path().stem().string().substr(prefix.size());
    if (stem.empty() || stem.find_first_not_of("0123456789") != std::string::npos) {
      continue;
    }
    max_seq = std::max(max_seq, static_cast<unsigned>(std::strtoul(stem.c_str(), nullptr, 10)));
  }

  char name[64];
  std::snprintf(name, sizeof(name), "%s_%03u.log", app_name.c_str(), max_seq + 1);
  return dir / name;
}

}  // namespace renut::linuxfixes

#endif  // !_WIN32
