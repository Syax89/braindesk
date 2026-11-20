# Kernel patch review: brain-pxp (PW-SH5)

Reviewed read-only; nothing was built or run on the device. The tree kept
changing while I read it (`hw_lock`, the PXP screen overlay,
`brain_lcd.h`, `brain_wait_idle()` and two charger rewrites all landed
mid-review). Everything below refers to the working tree as of
**2026-11-20 00:23 CET**, identical to the patch file at that moment
(patch md5 `eb4c1c80…`, `brain.c` `b719261e…`, `brain_battery.c`
`3d054256…`). Line numbers are for those files.

"Confirmed" = follows from the code plus datasheet / in-tree reference.
"Suspicion" = depends on hardware behaviour I could not check.

## Critical

None found. Nothing in the patch looks able to hang or crash a normal boot.

## High

### H1. PXP: `PXP_IOC_FREE` frees a buffer that is still mmap'ed (confirmed)
`drivers/misc/brain_pxp.c:341-353` (`pxp_ioc_free`), `:261-271`, `:355-376`.
`dma_mmap_wc()` is a `remap_pfn_range()` mapping with no page references.
FREE calls `dma_free_wc()` while the VMA still exists, so the process
keeps read/write access to CMA pages the kernel then hands to someone
else (page cache, anonymous memory, the DRM frame buffer). A client that
frees before `munmap()` and touches the pointer silently corrupts
unrelated memory; any member of group `video` (udev rule) can do it on
purpose. `close()` is safe (the VMA pins the file).
Fix: count mappings per buffer and refuse FREE while mapped.
```c
struct pxp_buf { ...; atomic_t maps; };
static void pxp_vm_open(struct vm_area_struct *v)  { atomic_inc(&((struct pxp_buf *)v->vm_private_data)->maps); }
static void pxp_vm_close(struct vm_area_struct *v) { atomic_dec(&((struct pxp_buf *)v->vm_private_data)->maps); }
static const struct vm_operations_struct pxp_vm_ops = { .open = pxp_vm_open, .close = pxp_vm_close };
/* pxp_mmap(), after dma_mmap_wc() succeeded, still under pf->lock */
vma->vm_private_data = b; vma->vm_ops = &pxp_vm_ops; atomic_inc(&b->maps);
/* pxp_ioc_free() */
if (atomic_read(&pf->bufs[idx].maps)) ret = -EBUSY; else pxp_free_buf(&pf->bufs[idx]);
```

### H2. Charger: the battery thermistor can never be used, and the fallback is to charge without it (confirmed)
`drivers/power/supply/brain_battery.c:740-750`. `devm_of_iomap()` is
`devm_ioremap_resource()`, which *requests* the LRADC region
0x80050000-0x80051fff. `mxs-lradc-ts` already owns it
(`drivers/input/touchscreen/mxs-lradc-ts.c:622`,
`devm_platform_ioremap_resource`), and it always probes first: `input/`
links before `power/`, and the battery probe defers anyway until the IIO
ADC exists. The request fails with -EBUSY ("can't request region" in
dmesg), `:747-750` then sets `bb->ntc = NULL`, and `brain_ntc_ok()`
(`:314-319`) returns true when there is no reading. Result: charging at
up to 450 mA with no -5..45 C window and no "no battery" detection
(`:340-343`). The only visible sign is the missing ", battery thermistor"
in the probe line. (Were the order ever reversed, the touchscreen would
fail to probe instead.) Fix: map without requesting, as
`mxs-lradc-adc.c:714` does, and fail safe:
```c
struct resource res;

if (lradc && !of_address_to_resource(lradc, 0, &res))
	bb->lradc = devm_ioremap(dev, res.start, resource_size(&res));
if (!bb->lradc)		/* thermistor described but unusable: do not charge blind */
	return dev_err_probe(dev, -ENXIO, "cannot map LRADC for the thermistor\n");
...
static bool brain_ntc_ok(struct brain_battery *bb)
{
	if (!bb->ntc)
		return true;	/* board without a thermistor */
	return bb->ntc_adc >= NTC_HOT && bb->ntc_adc <= NTC_COLD;	/* 0 = failed read */
}
```
A failed `iio_read_channel_raw()` (`:281-284`) leaves `ntc_adc = 0`,
which today also counts as "OK"; with the change above it pauses charging.

### H3. BU26154: capture is digitally muted until something has been played (confirmed from code + datasheet, not tested)
`sound/soc/codecs/bu26154.c:452` sets `DVMUTE` at probe; it is only
cleared inside the playback branch (`:229`). The capture branch
(`:235-238`) never clears it, and the datasheet says DVMUTE mutes the
record digital volume while recording (register 0x68 text and the record
signal-flow table). So `arecord` on a fresh boot records silence; it
works after any playback because nothing sets DVMUTE again (the README
test order is speaker first, then microphone).
Fix: clear it for any running state, just before `RECPLAY = want`:
```c
snd_soc_component_update_bits(c, BU_DVCTL, BU_DVMUTE, 0);
```

## Medium, confirmed

### M1. brain.c: `remove()` drops the driver's reference while the KMS objects live in devm memory
`drivers/gpu/drm/tiny/brain.c:718-722` (`drm_dev_alloc` + `devm_kzalloc`)
and `:811` (`drm_dev_put`, added by the patch). `ili` embeds the
connector, plane, CRTC and encoder. If any fd is open at unbind (X always
has card0 open), `drm_dev_put()` is not the final put: devres then frees
`ili`, and when X closes the fd `drm_mode_config_cleanup()` walks objects
inside freed memory. Before the patch the device was leaked, which hid
this. It matters now that unbind no longer hangs and rebind is tempting
for rotation. Fix: `ili = drmm_kzalloc(drm, sizeof(*ili), GFP_KERNEL);`
(freed with the last DRM reference, after mode-config cleanup), or switch
to `devm_drm_dev_alloc()` with an embedded `struct drm_device`.
Same unbind family: `brain_lcd_scanout_get()` (`:540-555`) reads
`brain_lcd` with no protection against a concurrent `remove()`, and
brain-pxp dereferences `pxp_dev` (NULL after unbind) at
`brain_pxp.c:227, :268, :387` from fds that are still open. Cheapest fix
for brain-pxp: `.driver.suppress_bind_attrs = true`.

### M2. brain.c: command-write failures are ignored and a stuck LCDIF is never recovered
`brain_write_byte()` `:208-234`; callers `:366-422` and `:465-470`.
Answer to "is MASTER / packaging always restored?": yes,
`brain_set_rows()` has no early return. But on a timeout:
- every remaining byte busy-waits at least another 1 s (`sleep_us = 0`)
  with `hw_lock` and the modeset locks held: 6 s or more per update,
  about 2 minutes for the ~120 init bytes in `brain_enable()` at boot;
- `CTRL_MASTER` is set again while RUN may still be set, then CUR_BUF /
  TRANSFER_COUNT are rewritten and pixels are pushed into whatever window
  the panel has;
- if RUN stays set or the FIFO never drains, every later update takes the
  1 s timeout path at `:501-509` forever: a frozen screen again, with no
  reset path.
Fix: return the error, stop on the first failure, force the block idle,
skip the pixel transfer and schedule a re-init.
```c
ret = brain_write_byte(ili, 0x2b, 0);
ret = ret ?: brain_write_byte(ili, y0 >> 8, 1);   /* ...same for the rest */
if (ret) {
	writel(CTRL_RUN, ili->base + LCDC_CTRL + REG_CLR);
	writel(CTRL1_FIFO_CLEAR, ili->base + LCDC_CTRL1 + REG_SET);
	writel(CTRL1_FIFO_CLEAR, ili->base + LCDC_CTRL1 + REG_CLR);
}
/* restore packaging + MASTER as now, then */ return ret;
```
In `brain_enable()` break out of the loop and leave `enabled = false`.

### M3. Charger: a uevent every 2 s whenever plugged in and not charging
`brain_battery.c:680-681`: `power_supply_changed(bb->bat)` fires on every
poll when the charger is not active, i.e. every 2 s for as long as the
device is plugged in and full, paused or inhibited (every 30 s on
battery). Each one makes udev/upower/brainbar re-read all properties,
each a fresh LRADC conversion. Notify only on a state change or when the
reported capacity changes.

### M4. BU26154: two documented sequencing rules are not followed (datasheet)
- `bu26154.c:187, 192`: PLLOE stays 1 across sample-rate changes. The
  datasheet ("When PLL is not used"): "When changing MCLKI input
  frequency, PLLOE should be set to 0, then PLLOE should be set to 1
  back". MCLK changes with every rate change (512 fs from SAIF0). Fix:
  write `CLKEN = 0` in the stop path; in the start path write
  `MCLKEN` first, then `MCLKEN | PLLOE`.
- `:208-211` and `:181, 231`: SPMDSEL (class D/AB) and SPEN change in the
  same write. Datasheet: "At the time of the change, set SPEN=0 before
  setting SPMDSEL". Fix: select class D once at probe
  (`BU_SPK_FIXED | BU_SPMDSEL_D`) and keep that bit in every later write.

## Medium, suspicions (need a look at the device)

### S1. brain.c: portrait rotations are mirror images, not rotations
`brain.c:391-395`. MV, MX, MY and GS are each a reflection; a proper
rotation toggles an even number of them. `rotation=1` toggles one (MV),
`rotation=2` toggles three (MV, MX, MY), so both should show the landscape
picture transposed (mirrored text). Working it through with the PW-SH5
flags (MV|GS): rot 1 maps fb (u,v) to glass (s=u, g=853-v), while the
touch matrix in `os/brain-rotate` ("0 -1 1 1 0 0") assumes
(s=479-u, g=853-v). So display and touch would also disagree by one flip.
A screenshot cannot show this (it reads the frame buffer); look at text
on the glass. If it is mirrored:
```c
if (rotation == 1)
	mac ^= ILI9805_MADCTL_MV | ILI9805_MADCTL_MX;
else if (rotation == 2)
	mac ^= ILI9805_MADCTL_MV | ILI9805_MADCTL_MY;
```
These two match the existing touch matrices exactly.

### S2. brain.c: disable gates the pixel clock mid-transfer; enable starts PIO without a guaranteed idle, empty LCDIF
`brain_disable()` `:437-445` does not wait for RUN or the FIFOs. A
DPMS-off / blank within ~17 ms of an update leaves a master transfer
frozen. On re-enable `brain_reset_block()` (`:236-251`) only clears
SFTRST/CLKGATE (it is not a soft reset); `FIFO_CLEAR` is set at `:343`
and dropped again by the CTRL1 write in `brain_set_pixel_fmt()` while the
block is still running; RUN is only cleared by the CTRL write after it.
Words fetched in between stay in the FIFO. Since `brain_write_byte()` now
ends with `brain_wait_idle()`, that would show up as a timeout on every
init byte (the M2 stall) and a panel that was never initialised. Same
family as the MASTER bug, but only after a blank/unblank. Fix:
```c
/* brain_disable(), before clk_disable_unprepare() (needs a u32 reg) */
readl_poll_timeout(ili->base + LCDC_CTRL, reg, !(reg & CTRL_RUN), 200, 50000);
brain_wait_idle(ili);
/* brain_enable(), after the "Unset DOTCLK" write at :350 (block is stopped here) */
writel(CTRL1_FIFO_CLEAR, ili->base + LCDC_CTRL1 + REG_SET);
writel(CTRL1_FIFO_CLEAR, ili->base + LCDC_CTRL1 + REG_CLR);
```
(mainline `mxsfb_reset_block()` pulses FIFO_CLEAR the same way.)
Related: DPMS off sends no Display Off / Sleep In, so the panel keeps
showing the last frame.

### S3. PXP: source crop origin under scaling may read past the S0 buffer
`brain_pxp.c:437-439, 512-513`. `src_x/src_y` are validated in source
pixels, but S0CROP width/height are programmed in output pixels, and the
PXP crops after scaling. If the origin is in scaled coordinates too, a
downscale with non-zero `src_x/src_y` fetches up to 4x further than
checked: physical memory after the buffer ends up in an image the caller
can read. No client uses this combination (pxpwin/pxpvideo/pxptest all
pass 0), so it is untested. Until verified, reject it:
```c
if ((xscale != SCALE_ONE || yscale != SCALE_ONE) && (j.src_x || j.src_y))
	return -EINVAL;
```

### S4. PXP: output sizes that are not multiples of 8
`brain_pxp.c:432-433, 474-476, 479-486`. Only S0/dst coordinates are
forced to 8-pixel blocks; `out_width`, `out_height`, `out_y` are not, and
the screen path requires `out_width == 854`. The bounds checks assume the
PXP clips output at pixel granularity. If it rounds up to whole 8x8
blocks, it writes up to 7 rows (and 2 pixels per row at 854) past the
checked area: past the frame buffer when the overlay touches the bottom
rows, or past a user buffer with less than a block of slack. One test
settles it: pattern-fill a buffer, run a job with `out_height = 12` and
`out_y` leaving room below, and check that rows below `out_y + 12` are
untouched. If they are not, require multiples of 8 for the height and
bound with `ALIGN(..., 8)`.

### S5. RTC: enabling the node lets the stock probe rewrite PERSISTENT0 clock bits shared with Windows CE
`drivers/rtc/rtc-stmp3xxx.c:339-379` (existing code, newly active through
`imx28-pwsh5.dts:171-175`). If HW_RTC_STAT reports no 32 kHz crystal
(fuses not blown, which the driver comment says happens) the probe
silently selects the 24 MHz source, clears `XTAL32KHZ_PWRUP` and sets
`XTAL24MHZ_PWRUP`: the 24 MHz oscillator then stays on while the unit is
"off" (battery drain) and the RTC runs from a different source than under
Windows CE. Check once with the rescue kernel and once with the new one:
`devmem 0x80056010` (bits 27/28) and `devmem 0x80056060` (bits 0, 4, 5,
6). If the stock value has bits 0 and 5 set while STAT bits 27/28 are 0,
add `stmp,crystal-freq = <32768>;` (or 32000) to the node. The probe also
clears ALARM_EN / ALARM_WAKE_EN / ALARM_WAKE (`:376-379`), so a wake
alarm set by Windows CE is lost after one Linux boot.

### S6. Charger: depends on the 4P2 state the boot chain left, and has no 5 V droop handling
`brain_battery.c:218-246` only touches PWD_BATTCHRG, BATTCHRG_I,
STOP_ILIMIT and CHARGE_4P2_ILIMIT. Per `ref/wince/charger-notes.md` the
firmware also starts the 4P2 rail on plug-in (ENABLE_4P2, ILIMIT ramp,
`PWD_CHARGE_4P2 = 0`), powers it down on unplug (`PWD_CHARGE_4P2 = 3`)
and stops the charger on a VDD5V_DROOP interrupt. Under Linux none of
that happens. If the unit was booted on battery and plugged in later,
the charger may deliver nothing: the state shows "Charging" until the
10 h timeout, and the system may keep running from the battery. A weak
5 V source has no back-off either. Check: boot on battery, plug in, read
`devmem 0x80044010` (bits 21:20), `devmem 0x80044080` (bits 23:22),
`devmem 0x800440c0` (bit 11) and watch `voltage_now` rise. If
PWD_CHARGE_4P2 is set, clear it when starting; at minimum poll
`STS.VDD5V_DROOP` (bit 4) and stop on it.

## Low

- `brain.c:135`: `rotation` is `0644`. Writing it at runtime changes
  `brain_rotated()` under a live mode: the next `get_modes` returns a size
  outside `mode_config` limits and the next enable programs the other
  MADCTL with the old frame buffer. Make it `0444`.
- `brain.c:413-414, 419-420`: enable programs end column = `hdisplay`
  and end page = `vdisplay` (one past the end). The panel evidently
  clamps or ignores it, since full-frame streaming without RAMWR worked
  before; partial updates use the correct `y1 - 1`. Send `hdisplay - 1` /
  `vdisplay - 1` so it stops depending on that.
- `brain.c:511-523`: when `pitch != width * cpp` the code falls back to a
  full frame, but the LCDIF still reads `width * cpp` contiguous bytes
  (skewed image, pre-existing). `fb->offsets[0]` is ignored at `:522`
  while `brain_lcd_scanout_get()` adds it at `:563`. Reject such frame
  buffers in a `.check` hook instead.
- `brain.c:572-585`: the overlay pushes rows from the pinned frame buffer
  even if the plane switched to another one during the PXP job (stale
  stripe until the next update).
- `brain_pxp.c:497, 559-560`: `timeout_ms` is unbounded and other callers
  wait in an uninterruptible `mutex_lock(&pxp->hw_lock)`; the frame
  buffer stays pinned meanwhile. Clamp to a few seconds and use
  `mutex_lock_interruptible()`.
- `brain_pxp.c:232-242`: 8 x 4 MiB per fd, unlimited fds, from the same
  8 MiB CMA area as DRM dumb buffers; a leaky client makes X fail to
  allocate. Consider a global cap.
- `mxs-brain.c:73-79, 107-112, 173-180`: static jack state is never
  reset. After a failed `snd_soc_jack_add_gpios()` `hp_jack_gpio.desc`
  is an ERR_PTR that `remove()` passes to `snd_soc_jack_free_gpios()`;
  after a normal unbind it is a dangling descriptor that the next bind
  reuses, and the notifier block is registered twice. Set `desc = NULL`
  after free/failure and unregister the notifier.
- `bu26154.c:350-364`: every `prepare` (including xrun recovery) stops
  and restarts RECPLAY for both directions, which glitches the other
  stream. Skip when the wanted state and configuration are unchanged.
- `bu26154.c:341`: the rate-conflict test uses `bu->state`, which is only
  set at prepare. Two streams with different rates both pass `hw_params`
  if neither has prepared; the last one wins the clock.
- `bu26154.c:307-321`: jack-driven output changes do not call
  `snd_ctl_notify()`, so mixer UIs show stale switch values.
- Codec reset line (GPIO0_28 per the README) is not described or driven;
  audio depends on the boot firmware leaving it released. No PM ops in
  codec or card.
- `brain_battery.c:649-673`: the low-battery power-off only runs when
  `sts_ext_power()` is false, but charging needs `VBUSVALID0` (`:323`).
  With a sagging 5 V source (VDD5V_GT_VDDIO set, VBUSVALID0 clear) the
  unit neither charges nor protects the cell. Run the check whenever the
  charger is not active.
- `brain_battery.c:336-337`: the die-temperature read result is ignored
  (failure = 0 = cool), and the 100/110 thresholds rely on mxs-lradc
  returning whole degrees instead of the IIO ABI's milli-degrees
  (`mxs-lradc-adc.c:231-253`). It is correct today; add a comment or use
  `iio_read_channel_processed_scale(..., 1000)` and compare in mC.
- `brain_battery.c:793-810`: `INIT_DELAYED_WORK` runs after the supplies
  are registered, while `set_property` (`:576`) calls `mod_delayed_work`.
  Initialise the work first. In `remove()` (`:820-829`) a sysfs write can
  re-arm the work after the cancel, because devm unregisters the supply
  later. No suspend handling (charger keeps running unsupervised while
  the jiffies-based timers stop). Comment at `:762` is stale.
- Charger policy differs from the firmware it cites: 450 mA with the
  system running (firmware: 100 mA when on, 450 mA only when off) and a
  600 min limit (firmware: 300). The die guard covers the heat, but it
  is the only one.
- DT: the charger starts by itself 1 s after boot on external power
  (no `sharp,no-charger`); `dr_mode = "peripheral"` is now fixed.
- Watchdog (now active through the RTC node): when started it sets
  PERSISTENT1 bit 31, a register shared with the boot chain.

## Checked, no issue found

- Concurrent `brain_fb_dirty_full()` callers: atomic commits on the one
  CRTC are serialised by the atomic helpers; the PXP overlay path and
  enable/disable are serialised by `hw_lock`. Lock order (`plane->mutex`
  only in `scanout_get`, `hw_lock` only in the hardware paths) is
  consistent. `drm_dev_enter/exit` are balanced on every path. After
  disable, updates are skipped (`enabled` is cleared under the lock).
- Row window: every update, full or partial, re-sends RASET + RAMWR, so a
  partial window cannot leak into a later full-frame update. `ye = y1 - 1`
  is right in all three rotations (the row address is logical, so GS/MY
  do not change it). CASET is never touched after enable.
- XRGB8888: not in `formats[]`, so that branch is dead. Do not enable it
  as is: the 8-bit command trick (packaging 0x3 with 16-bit word length)
  does not hold for 24-bit words.
- `brain_wait_idle()` STAT bits (28 LFIFO_EMPTY, 26 TXFIFO_EMPTY,
  25 BUSY) match the i.MX28 LCDIF_STAT layout.
- PXP: register offsets and fields match `ref/regs-pxp.h`; S0PARAM,
  S0CROP, S0SCALE and OUTSIZE-under-rotation match `ref/mxs_pxp.c`; index
  and size checks use 64-bit math, including `out_y` and the screen path;
  FREE cannot race a running job (`pf->lock`); the frame buffer is pinned
  for the whole overlay job; dma-buf get/attach/map/unmap are balanced;
  the reset, STAT clear and `reinit_completion` order prevents stale
  completions.
- ASoC: SAI format 0x80 (16 bit, 32 fs BCLK, I2S delay) matches SAIF
  16-bit mode; `CBC_CFC` is flipped to `BP_FP` for the SAIF by the core;
  MCLK 512 fs stays within 4.096-49.152 MHz for 8-48 kHz; VMID (5 ms +
  5 ms) and HPVDD (1 ms) waits match the datasheet; register indices and
  bit positions match the register map; jack polarity (active low +
  pull-up) is consistent.
- RTC: 946652400 is 2000-01-01T00:00 JST; get/set/alarm apply the offset
  symmetrically; `range_min/max` are right; the RTC core adds no offset
  of its own (no `start-year`).
- Charger: HW_POWER offsets and fields match `ref/regs-power.h`;
  BATTCHRG_I / STOP_ILIMIT / CHARGE_4P2_ILIMIT bit weights are right;
  termination now needs CHRGSTS low and 4.13 V for 5 minutes, so a
  charger that delivers nothing cannot report "full"; the reboot
  notifier and `remove()` switch the charger off.
- DT: GPIO0_24 has no other user; LRADC channel 0 is free with a 4-wire
  touchscreen (it uses 2-5); the new pinctrl node name is unique (the mxs
  same-name grouping quirk is not triggered); only `imx28-pwsh5.dts`
  changed, so other models are untouched at DT level. Shared code that
  changes for them: partial updates and `hw_lock` in `brain.c` (all
  ILI9805 models, PW-SH1 to SH7), untested there.
