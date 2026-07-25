#/bin/bash
set -x
folder=profile_v
mkdir -p $folder
cd $folder
cmake ../.. -DCMAKE_BUILD_TYPE=Profile
cmake --build . --parallel --target pacs8
x=../../xml-data/pacs8-1M.xml
y=../../xsd/pacs.008.xsd
perf record -F 999 -g -o perf_v.data -- ./pacs8 $x $x $x $x $x $x $x $x $x $x $y
perf report -i perf_v.data --stdio --sort=overhead,comm,dso,symbol > report_v.txt
head -60 report_v.txt