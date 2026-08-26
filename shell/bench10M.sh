#/bin/bash
set -x
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel --target pacs8
data_dir=~/ach/benchmarks/b10m
y=~/ach/grammars/xsd/local/ct-in_min.xsd
clear && time ./pacs8 "$data_dir"/*.ct-in $y
