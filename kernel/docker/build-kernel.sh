#!/bin/sh
# Compila il kernel del Brain (stesso commit di Brainux) con le patch
# brain-pxp, dentro un volume Linux.
set -e
SHA=a9f534c7f53e7ee6bb2cb3581fd471cc964f1601
cd /kernel
if [ ! -d .git ]; then
  git init -q . && git remote add origin https://github.com/brain-hackers/linux-brain.git
fi
if [ "$(git rev-parse HEAD 2>/dev/null)" != "$SHA" ]; then
  git fetch -q --depth 1 origin $SHA
fi
# sorgenti puliti + patch
git checkout -q -f $SHA 2>/dev/null || git checkout -q -f FETCH_HEAD
git clean -q -fd -e .config
for p in /src/patches/*.patch; do
  [ -e "$p" ] && git apply --whitespace=nowarn "$p" && echo "applicata $(basename $p)"
done
cp /src/brain.config .config
[ "${STOCK:-0}" = 1 ] || cat /src/docker/brain.fragment >> .config
make olddefconfig >/dev/null
for o in CMA DMA_CMA POWER_SUPPLY BATTERY_BRAIN SND_SOC_BU26154 SND_SOC_MXS_BRAIN BRAIN_PXP ZRAM; do
  printf '%s=%s ' $o "$(./scripts/config -s $o)"
done; echo
# il nome di versione deve restare quello di Brainux (niente "-dirty")
echo "kernelrelease: $(make -s kernelrelease LOCALVERSION= )"
make -j"$(nproc)" W=1 zImage dtbs modules 2>&1 | grep -E "warning:|error:|Error" | grep -E "brain|bu26154|stmp3xxx|pwsh5" || true
make -j"$(nproc)" zImage dtbs modules 2>&1 | tail -2
mkdir -p /src/out
cp arch/arm/boot/zImage /src/out/zImage
cp arch/arm/boot/dts/imx28-pwsh5.dtb /src/out/
# varianti di orientamento per brain-rotate
LCD=/apb@80000000/apbh@80000000/lcdif@80030000
for r in 1 2; do
  cp /src/out/imx28-pwsh5.dtb /src/out/imx28-pwsh5-rot$r.dtb
  fdtput -t u /src/out/imx28-pwsh5-rot$r.dtb "$LCD" sharp,rotation $r
done
echo "nodo pannello: $LCD"; fdtget /src/out/imx28-pwsh5-rot1.dtb "$LCD" sharp,rotation
ls -l /src/out
