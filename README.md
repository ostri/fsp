# fsp

Fast SEPA xml file parser

## Build process

### debug configuraton

```bash
  cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
```

### release configration

```bash
  cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
```

### profile configration

```bash
  cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Profile
```

### build

```bash
  cmake --build
```

### common libraries setup

```bash
    sudo dnf install -y xerces-c-devel xerces-c-doc.noarch
    sudo dnf install -y libxml2 libxml2-devel
```

### profiling

```bash
    cmake -B build-profile -DCMAKE_BUILD_TYPE=Profile
    cmake --build build-profile --target fsp
    cd build-profile
    perf record -F 999 -g -- ../fsp ../../xml-data/pacs8-1M.xml
    perf report > report.txt
    perf report
```
