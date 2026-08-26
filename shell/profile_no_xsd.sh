#/bin/bash
set -x
folder=profile_no_v
mkdir -p $folder
cd $folder
cmake ../.. -DCMAKE_BUILD_TYPE=Profile
cmake --build . --parallel --target pacs8
data_dir=~/ach/benchmarks/b10m
perf record -F 999 -g -o perf_no_v.data -- ./pacs8 "$data_dir"/*.ct-in
perf report -i perf_no_v.data --stdio --sort=overhead,comm,dso,symbol > report_no_v.txt
head -60 report_no_v.txt
