#/bin/bash
set -x
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel --target pacs8-hook
y=../xsd/pacs.008.xsd
ok=../xml-data/pacs8-2.xml
fail=../xml-data/pacs8-2-fail.xml
./pacs8-hook $y $ok $fail $ok