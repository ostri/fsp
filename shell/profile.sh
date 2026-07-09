#/bin/bash
set -x
folder=profile_$$
mkdir $folder
cd $folder
cmake ../.. -DCMAKE_BUILD_TYPE=Profile
cmake --build . --parallel --target fsp
perf record -F 999 -g -- ./fsp ../../xml-data/pacs8-1M.xml ../../xsd/pacs.008.xsd
perf report > report.txt
perf report