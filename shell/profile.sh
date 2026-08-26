#/bin/bash
set -x
folder=profile_v
mkdir -p $folder
cd $folder
cmake ../.. -DCMAKE_BUILD_TYPE=Profile
cmake --build . --parallel --target pacs8
data_dir=~/ach/benchmarks/b10m
y=~/ach/grammars/xsd/local/ct-in_min.xsd
perf record -F 999 -g -o perf_v.data -- ./pacs8 "$data_dir"/*.ct-in $y
perf report -i perf_v.data --stdio --sort=overhead,comm,dso,symbol > report_v.txt
head -60 report_v.txt
