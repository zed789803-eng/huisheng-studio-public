#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
OUT="${OUT:-build/tests}"
mkdir -p "$OUT"

CXX="${CXX:-clang++}"
INC="-I shared"

# 产品密钥不进仓库：CI 把它放在环境变量 HS_PRODUCT_KEY（64 位十六进制）里，
# 这里生成 License.cpp 约定的头文件。没有这个变量就跳过注册码用例，其余照跑
# （本地开发与别人 fork 出去的仓库都能过）。
KEY_INJECTED=0
if [ -n "${HS_PRODUCT_KEY:-}" ]; then
    BYTES="$(printf '%s' "$HS_PRODUCT_KEY" | sed 's/../0x&, /g')"
    printf '#define HS_LICENSE_KEY_BYTES %s\n' "$BYTES" > shared/LicenseKey.generated.h
    KEY_INJECTED=1
fi

echo "== driver loopback"
"$CXX" -std=c++17 -O2 $INC -o "$OUT/driver_loopback" tests/driver_loopback_test.cpp \
    -framework CoreAudio -framework CoreFoundation
"$OUT/driver_loopback"

echo "== driver property sizes"
"$CXX" -std=c++17 -O2 $INC -o "$OUT/driver_property" tests/driver_property_test.cpp \
    -framework CoreAudio -framework CoreFoundation
"$OUT/driver_property"

echo "== driver timestamps"
"$CXX" -std=c++17 -O2 $INC -o "$OUT/driver_timestamp" tests/driver_timestamp_test.cpp \
    -framework CoreAudio -framework CoreFoundation
"$OUT/driver_timestamp"

echo "== capture slots: one ring per App, per instance"
"$CXX" -std=c++17 -O2 $INC -o "$OUT/slot_rings" tests/slot_rings_test.cpp shared/RingBuffer.cpp
"$OUT/slot_rings"

echo "== drift bridge: long run"
"$CXX" -std=c++17 -O2 $INC -o "$OUT/drift_longrun" \
    tests/drift_longrun_test.cpp shared/DriftBridge.cpp shared/Resampler.cpp
"$OUT/drift_longrun"

echo "== drift bridge: block sizes and rate ratios"
"$CXX" -std=c++17 -O2 $INC -o "$OUT/drift_blocksize" \
    tests/drift_blocksize_test.cpp shared/DriftBridge.cpp shared/Resampler.cpp
"$OUT/drift_blocksize"

echo "== licence codes"
if [ "$KEY_INJECTED" = "1" ]; then
    "$CXX" -std=c++17 -O2 $INC -o "$OUT/license" tests/license_test.cpp shared/License.cpp
    "$OUT/license"
else
    echo "   skipped: HS_PRODUCT_KEY is not set (the reference codes need the product key)"
fi

echo "all tests passed"
