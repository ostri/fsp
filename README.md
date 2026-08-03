# fsp

Fast SEPA xml file parser

## Build process

The project defines four build environments (`CMAKE_BUILD_TYPE`), each with its own compile/link
flags in `add_fsp_target()` (see `CMakeLists.txt`) and its own `CMakePresets.json` preset:

| Build type | Preset           | Directory        | Purpose                                                                 |
|------------|------------------|------------------|-------------------------------------------------------------------------|
| Debug      | `ninja-debug`    | `build/debug`    | `-O0`, ASan/UBSan/LeakSan enabled, full DWARF debug info                |
| Release    | `ninja-release`  | `build/release`  | `-O3`, LTO, no sanitizers                                               |
| Profile    | `ninja-profile`  | `build/profile`  | Same as Release, plus DWARF debug info for readable `perf` stack traces |
| Coverage   | `ninja-coverage` | `build/coverage` | Debug-like (`-O0`, no sanitizers) + `--coverage` (gcov) instrumentation |

### debug configuraton

```bash
  cmake --preset ninja-debug
  # equivalent to: cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
```

### release configration

```bash
  cmake --preset ninja-release
  # equivalent to: cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
```

### profile configration

```bash
  cmake --preset ninja-profile
  # equivalent to: cmake -S . -B build/profile -DCMAKE_BUILD_TYPE=Profile
```

### coverage configuration

```bash
  cmake --preset ninja-coverage
  # equivalent to: cmake -S . -B build/coverage -DCMAKE_BUILD_TYPE=Coverage
```

### build

```bash
  cmake --build build/<debug|release|profile|coverage>
```

### common libraries setup

```bash
    sudo dnf install -y xerces-c-devel xerces-c-doc.noarch
    sudo dnf install -y libxml2 libxml2-devel
```

### profiling

```bash
  # move to build folder
  ../shell/profile.sh
```

### coverage report

Requires [gcovr](https://gcovr.com/) (`pip install gcovr`). Configure with the `Coverage` build
type, then build the `coverage` target -- it runs `unit_tests` and feeds the resulting `.gcda`
files to `gcovr`, producing an HTML report:

```bash
  cmake --preset ninja-coverage
  cmake --build build/coverage --target coverage
```

Open `build/coverage/coverage/index.html` in a browser for the per-file/per-line/per-branch
breakdown; a lines/functions/branches summary is also printed to the terminal on every run.
Third-party sources (fetched via CPM) and the test files themselves (`src/test/`) are excluded
from the report -- only `src/` production code is measured.

To get just the terminal summary without regenerating the HTML report:

```bash
  gcovr --root . --filter src/ --exclude src/test/ --exclude-unreachable-branches \
        --exclude-throw-branches --print-summary build/coverage
```
