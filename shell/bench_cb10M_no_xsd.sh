#/bin/bash
set -x
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel --target pacs8-cb
data_dir=~/ach/benchmarks/b10m
clear && time ./pacs8-cb "$data_dir"/*.ct-in
