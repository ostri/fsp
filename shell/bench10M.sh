#/bin/bash
set -x
cmake ../.. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel --target pacs8
x=../xml-data/pacs8-1M.xml
y=../xsd/pacs.008.xsd
clear && time ./pacs8 $x $x $x $x $x $x $x $x $x $x $y

