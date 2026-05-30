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

### build

```bash
  cmake --build
```

### common libraries setup

```bash
    sudo dnf install -y xerces-c-devel xerces-c-doc.noarch
    sudo dnf install -y pugixml-devel pugixml-doc
```

### profiling

```bash
    cmake -B build-profile -DCMAKE_BUILD_TYPE=Profile ..
    cmake --build build-profile --target fsp
    cd build-profile/
    ./fsp ../../xml-data/pacs8-1M.xml ../../xsd/pacs.008.xsd
    gprof fsp gmon.out > profile_report.txt
    gprof fsp gmon.out |head -50
```
