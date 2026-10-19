#!/bin/sh
# Fetch the SDR++ source matching the installed SDR++ build into third_party/SDRPlusPlus.
# The module compiles against these headers, so they must come from the same commit.
set -e
cd "$(dirname "$0")/.."
commit=$(cat SDRPP_COMMIT)
installed=$(dpkg-query -W -f='${Version}' sdrpp 2>/dev/null || true)
echo "Installed sdrpp package: ${installed:-unknown}; building against commit $commit"
echo "If SDR++ was upgraded, update SDRPP_COMMIT to the commit that build came from."
dir=third_party/SDRPlusPlus
if [ ! -d "$dir/.git" ]; then
    git init -q "$dir"
    git -C "$dir" remote add origin https://github.com/AlexandreRouma/SDRPlusPlus.git
fi
git -C "$dir" fetch -q --depth 1 origin "$commit"
git -C "$dir" checkout -q FETCH_HEAD
echo "SDR++ source ready in $dir"
