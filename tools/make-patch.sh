#!/bin/sh
# Rigenera patches/0001-brain-pw-sh5-hardware.patch dall'albero linux-brain/
cd "$(dirname "$0")/../linux-brain" || exit 1
git add -N include/linux/brain_lcd.h drivers/misc/brain_pxp.c drivers/power/supply/brain_battery.c \
	include/uapi/misc/brain_pxp.h sound/soc/codecs/bu26154.c sound/soc/codecs/bu26154.h sound/soc/mxs/mxs-brain.c 2>/dev/null
git diff -- arch/arm/boot/dts/imx28-pwsh5.dts drivers/gpu/drm/tiny/brain.c \
	drivers/gpu/drm/tiny/brain-lq050j1ug01.c drivers/misc drivers/power/supply/Kconfig \
	drivers/power/supply/Makefile drivers/power/supply/brain_battery.c drivers/rtc/rtc-stmp3xxx.c \
	include/linux/brain_lcd.h include/uapi/misc/brain_pxp.h sound/soc/codecs/Kconfig \
	sound/soc/codecs/Makefile sound/soc/codecs/bu26154.c sound/soc/codecs/bu26154.h sound/soc/mxs \
	> ../patches/0001-brain-pw-sh5-hardware.patch
wc -l ../patches/0001-brain-pw-sh5-hardware.patch
