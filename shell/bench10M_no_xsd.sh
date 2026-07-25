#/bin/bash
set -x
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel --target pacs8
x=../xml-data/pacs8-1M.xml
y=../xsd/pacs.008.xsd
clear && time -v ./pacs8 $y $x $x $x $x $x $x $x $x $x $x 

