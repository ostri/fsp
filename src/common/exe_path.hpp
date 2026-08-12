#pragma once

#include <filesystem>

namespace fsp
{
  /**
   * @brief the directory the currently running executable lives in
   *
   * Asks the OS directly for the running binary's own path -- not argv[0], which POSIX leaves
   * entirely up to the caller (a shell's PATH lookup passes just a bare name, no path at all; a
   * hand-rolled execve() can pass anything) and so cannot be relied on. This lets a program locate
   * files that ship next to its own binary (e.g. log.conf, copied there by add_log_config() in
   * CMakeLists.txt) regardless of the caller's current working directory.
   *
   * Linux: /proc/self/exe. macOS: _NSGetExecutablePath(). Windows: GetModuleFileNameW(). Any other
   * platform, or the above failing on one of the three: falls back to the current working
   * directory -- a best-effort guess, never a reason to fail startup over.
   */
  [[nodiscard]] std::filesystem::path exe_dir();
} // namespace fsp
