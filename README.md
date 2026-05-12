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
