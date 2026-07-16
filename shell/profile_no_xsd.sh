#/bin/bash
set -x
folder=profile
mkdir $folder
cd $folder
cmake ../.. -DCMAKE_BUILD_TYPE=Profile
cmake --build . --parallel --target pacs8
x=../../xml-data/pacs8-1M.xml
y=../../xsd/pacs.008.xsd
perf record -F 999 -g -- ./pacs8 $x $x $x $x $x $x $x $x $x $x
perf report > report.txt
perf report