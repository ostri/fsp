#include "exe_path.hpp"

#ifdef __APPLE__
#  include <mach-o/dyld.h>
#  include <climits>
#  include <cstdint>
#  include <vector>
#elifdef _WIN32
// NOLINTNEXTLINE(llvm-include-order) - windows.h has to precede nothing else here; there is no ordering conflict to guard against
#  include <windows.h>
#  include <vector>
#endif

namespace fsp
{
#ifdef __linux__
  std::filesystem::path exe_dir()
  {
    std::error_code             ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec || exe.empty()) return std::filesystem::current_path(); // best effort fallback - never throws startup over this
    return exe.parent_path();
  }
#elifdef __APPLE__
  std::filesystem::path exe_dir()
  {
    // _NSGetExecutablePath fills buf and returns 0 on success, or the required size (including the
    // terminating NUL) in size when buf was too small - the size hint is a bare guess, so the
    // buffer is grown and retried once.
    std::uint32_t     size = PATH_MAX;
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0)
    {
      buf.resize(size);
      if (_NSGetExecutablePath(buf.data(), &size) != 0) return std::filesystem::current_path(); // best effort fallback
    }

    std::error_code             ec;
    const std::filesystem::path exe = std::filesystem::canonical(std::filesystem::path(buf.data()), ec); // resolves any symlink
    if (ec || exe.empty()) return std::filesystem::current_path();
    return exe.parent_path();
  }
#elifdef _WIN32
  std::filesystem::path exe_dir()
  {
    // GetModuleFileNameW(nullptr, ...) asks for the calling process' own module (the .exe itself,
    // not any DLL) - grown and retried until the buffer is large enough (ERROR_INSUFFICIENT_BUFFER,
    // no truncation).
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;)
    {
      const DWORD written = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
      if (written == 0) return std::filesystem::current_path(); // best effort fallback
      if (written < buf.size()) return std::filesystem::path(buf.data(), buf.data() + written).parent_path();
      buf.resize(buf.size() * 2);
    }
  }
#else
  // Unknown platform: no reliable "path to my own binary" API to call - the current working
  // directory is the best guess available, same fallback every branch above also uses when its own
  // OS call fails.
  std::filesystem::path exe_dir() { return std::filesystem::current_path(); }
#endif
} // namespace fsp
