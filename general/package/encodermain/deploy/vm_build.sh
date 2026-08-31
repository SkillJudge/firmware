#!/bin/bash
# ======================================================================
# vm_build.sh — deploy.ps1 的 VM 端构建子脚本 (sh, NOT bash 语法)
# 输入: 工作目录 ~/encdual_build 内已落 encdual_src.tar (纯 tar)
# 动作: 解包 → 覆盖 firmware 对应 src → Makefile.local clean+all
#       → 把产物 + S96/S43/conf 复制到 ~/encdual_build/ 根方便 pscp 回传
# 约束: 绝不调用 build.sh / make BOARD=xxx clean all
# ======================================================================
set -e
cd ~/encdual_build
tar xf encdual_src.tar

FIRM=~/openipc/firmware
for p in encodermain encalertd; do
  L="$(pwd)/firmware/general/package/$p/src"
  R="$FIRM/general/package/$p/src"
  mkdir -p "$R"
  cp -f "$L"/*.c "$L"/*.h "$L"/Makefile "$L"/Makefile.local "$R"/ 2>/dev/null || true
done

cd "$FIRM"
echo "=== Build encodermain (Makefile.local only) ==="
make -C general/package/encodermain/src -f Makefile.local clean >/dev/null
make -C general/package/encodermain/src -f Makefile.local
echo "=== Build encalertd (Makefile.local only) ==="
make -C general/package/encalertd/src  -f Makefile.local clean >/dev/null
make -C general/package/encalertd/src  -f Makefile.local

echo "=== MD5 on VM ==="
md5sum \
  general/package/encodermain/src/encodermain \
  general/package/encalertd/src/encalertd

echo "=== Stage deliverables into ~/encdual_build ==="
cp -f general/package/encodermain/src/encodermain             ~/encdual_build/encodermain
cp -f general/package/encalertd/src/encalertd                  ~/encdual_build/encalertd
cp -f ~/encdual_build/firmware/general/package/encodermain/S96encodermain  ~/encdual_build/S96encodermain
cp -f ~/encdual_build/firmware/general/package/encalertd/S43encalertd      ~/encdual_build/S43encalertd
cp -f ~/encdual_build/firmware/general/package/encalertd/encalertd.conf    ~/encdual_build/encalertd.conf 2>/dev/null || true
ls -l ~/encdual_build
echo BUILD_READY
