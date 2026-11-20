# SHARP Brain PW-SH5 – how Windows CE charges the battery

Reverse-engineered from `ref/wince/nk.img` (NK image, header `SHARP ED-SH5 V100`).
Scripts and listings: `ref/wince/charger/` (see "Tooling" at the end).

Confidence tags: **[P]** proven from the disassembly (address given),
**[I]** inferred (reasoning given), **[?]** not determinable from this dump.

## 1. Summary

- The charger is run by SHARP's own state machine (not Freescale `ddi_bc`; there is
  no config struct, every parameter is an immediate in code). The same source is
  compiled twice: in the PMU driver `pmuPdk.dll` (device **ON**) and in the OAL
  `nk.exe` (device **OFF** = `OEMPowerOff` with 5 V present). State machine and
  charge-control function are instruction-identical in both; only the hardware
  helper differs (STOP_ILIMIT).
- **Device ON: slow charge, BATTCHRG_I = 100 mA. Device OFF: fast charge,
  550 mA on the AC adapter / 450 mA on a USB port.** Pre-charge 80 mA below 3.0 V.
- Target voltage is the chip default 4.2 V (ADJ_VOLT, LIION_4P1 never written).
- USB enumeration is not used at all. "AC adapter" = D+/D− shorted, detected with
  the USB PHY pull-ups/pull-downs.

### Charge currents (HW_POWER_CHARGE.BATTCHRG_I) and input limit (HW_POWER_5VCTRL.CHARGE_4P2_ILIMIT)

| Situation | AC adapter (D+/D− shorted) | USB host / anything else | Evidence |
|---|---|---|---|
| ON, default "SLOW" (`PmuSetCharger(0)`) | BATTCHRG_I `0x08` = **100 mA**, ILIMIT left at `0x3f` (780 mA) | BATTCHRG_I `0x08` = **100 mA**, ILIMIT `0x27` (**480 mA**) | [P] pmuPdk c06252f8/c0625348, c0622534 |
| ON, "FAST" (`PmuSetCharger(1)`; in this ROM only DiagApp asks for it) | `0x2c` = **550 mA**, ILIMIT 780 mA | `0x24` = **450 mA**, ILIMIT 480 mA | [P] pmuPdk c06252ec/c062533c |
| OFF (OAL loop, always FAST) | `0x2c` = **550 mA**, ILIMIT 780 mA | `0x24` = **450 mA**, ILIMIT 480 mA | [P] nk.exe 80213250, 80213e08/80213e58 |
| Pre-charge (VBAT < 3000 mV at start, until ≥ 3050 mV) | `0x07` = **80 mA**, ILIMIT 480 mA | same | [P] c062519c, c06252e0 |
| Die temperature > 100 °C | `0x08` = 100 mA, ILIMIT 480 mA | same | [P] c06258a0, c0625970 |
| Not charging, 5 V present | PWD_BATTCHRG=1, BATTCHRG_I=0, STOP_ILIMIT=0, ILIMIT `0x3f` (780 mA) | same | [P] c0623d10 |

No current ramp on BATTCHRG_I (field cleared, written once, then PWD_BATTCHRG cleared).
The only ramp is CHARGE_4P2_ILIMIT 1→63 when the 4P2 rail is started (§5).

### Other parameters

| Parameter | Value | Evidence |
|---|---|---|
| STOP_ILIMIT (termination current) | ON: `0x4` = **50 mA**; OFF: `0x8` = **100 mA** | [P] pmuPdk c0622554 (`CHARGE_SET 0x400`); nk.exe 80212974 (`0x800`) |
| "Full" decision | every 60 s: CHRGSTS==0 **and** VBAT ≥ **4130 mV**, 5 consecutive times (CHRGSTS==1 resets the count) → charger off | [P] c0625618–c0625668 |
| Top-off timer | none (the 5 × 60 s debounce above is all there is) | [P] |
| Do not start if | VBAT > **4130 mV** (state = full) | [P] c0625138 |
| Recharge | state full and VBAT < **4000 mV** (checked every 60 s) | [P] c0625710–c0625750 |
| Pre-charge timeout | **30 min** → latched fault | [P] c06254e8 |
| Total charge timeout | **300 min** counted from start incl. pre-charge; then VBAT < 3050 → fault, < 4100 → fault, else "complete" | [P] c062554c–c0625614 |
| Battery temperature window | **−5 … +45 °C** (start + every 60 s); outside → latched fault | [P] c062518c, c06254c4 |
| Battery thermistor | LRADC **channel 0**, current source TEMP_ISRC0 = 7 (140 µA), 5-sample mean, table in §4 | [P] lradc.dll c067359c |
| No battery | thermistor ADC > 3072 → "−100" → no charge, flag NO_BATTERY | [P] c06736b0 |
| Die temperature | LRADC ch 9 − ch 8 (×1.012/4 → K), every 1 s while charging: > **100 °C** → 100 mA; > **110 °C** → stop, latched fault | [P] c0625918–c06259a8 |
| 5 V detect | VBUSVALID comparator (VBUSVALID_5VDETECT=1, VBUSVALID_TRSH=1 → 4.0 V), STS.VBUSVALID0 | [P] c06246e4 |
| 5 V droop | VDD5V_DROOP irq (VBUSDROOP_TRSH=0 → 4.3 V) stops the charger; retry when VDD5V (LRADC ch 15) ≥ 4650 mV | [P] c0624178, c06240d0 |
| Gauge levels | > 3900 / > 3750 / > 3450 / > 3350 mV → 100 / 75 / 50 / 25 (LOW) / 10 % (CRITICAL) | [P] battdrvr c0662028–c06620bc |
| Forced power-off | CRITICAL (≤ 3350 mV averaged) → `PM_FATAL_OFF_EVENT`; < 3250 mV → emergency off | [P] battdrvr c0661798, c0662350 |

## 2. Which modules, where

All addresses are load VAs. File offset in `nk.img` = `VA − text VA + file off` below.

| Module | text VA | file off | Role |
|---|---|---|---|
| `pmuPdk.dll` (`PMI1:`, source `PLATFORM\iMX28-EVK-PDK1_9\SRC\DRIVERS\PMU\PDK\pmupdk.cpp`) | c0621000 | 0x880000 | charger state machine, 5 V/4P2 handling (device ON) |
| `nk.exe` (OAL) | 80201000 | 0x001000 | same state machine, run inside `OEMPowerOff` (device OFF) |
| `battdrvr.dll` (`BAT1:`, pdb `BATTDRVR_EN_CHARGER`) | c0661000 | 0x891000 | policy: asks for SLOW/FAST/STOP, gauge levels, sounds, low-battery off |
| `pmuSdk.dll` | c0611000 | 0x87e000 | thin IOCTL wrappers (`PmuSetCharger` = 0x228004, `PmuStopCharger` = 0x228008, `PmuGetPowerInfo` = 0x228040) |
| `lradc.dll` (`LDC1:`) | c0671000 | 0x896000 | battery thermistor (IOCTL 0x222fa8), die temp (0x222f94), VDD5V (0x222fa4) |
| `usbfn.dll` (`UFN1:`) | c0691000 | 0x89c000 | AC-adapter detection (IOCTL 0x2201a4) |
| `EdAppCtrl.exe`, `DiagApp.exe` | — | 0x198d000 / 0xb07000 | the only users of battdrvr IOCTL 0x290430 (charge mode) |

Functions (pmuPdk / OAL twin):

| Function | pmuPdk | nk.exe |
|---|---|---|
| program charger (`BSPBattdrvrSetCharger`) | c0622534 (wrapper c0622c10) | 80212968 |
| charger off | c0623d10 | 80212a44 |
| charge control (start/stop, picks current) | c06250dc | 80213bf8 |
| state machine (events 1…15) | c0625410 | 80213f2c |
| 1 s / 60 s tick source | `ChargeManagementThread` c0622d0c | off-loop 802131b0 (TIMER2) |
| 5 V / cable / droop thread | `PowerSupplyThread` c0623f50 | off-loop 802131b0 |
| 4P2 start | `PowerStart4p2HW` c06230e8 | 8020e2ec |
| `OEMPowerOff` | — | 8020e9dc |

`battdrvr.dll`: status/gauge c0661e1c, private IOCTLs c0661b08, poll thread c0662694.
`lradc.dll`: thermistor c067359c (table c06716b8, file 0x8966b8; OAL copy 802074b8), die temp c0672b90, VDD5V c06726b4.
`usbfn.dll`: detection c06a29f4. OAL copy 80212b10.

Bootloader: **not in this dump** [P]. After the NK image (ends 0x1c6ff64) the file only
holds an RGB565 splash bitmap at 0x1e00000. Whatever the SHARP loader programs before
NK starts is unknown [?].

## 3. Charger programming and state machine

### 3.1 Register sequence when charging starts

pmuPdk c0622534 (device ON), argument `code`:

```
CHARGE_CLR  = 0x00000f00            ; STOP_ILIMIT = 0
CHARGE_SET  = 0x00000400            ; STOP_ILIMIT = 50 mA      (OAL: 0x800 = 100 mA)
CHARGE_CLR  = 0x0000003f            ; BATTCHRG_I = 0
5VCTRL_CLR  = 0x30000000            ; VBUSDROOP_TRSH = 0 (4.3 V)
if !((code & 0x28)==0x28 || (code & 0x30)==0x30 || code==0x18):
    5VCTRL_CLR = 0x0003f000 ; 5VCTRL_SET = 0x00027000   ; CHARGE_4P2_ILIMIT = 480 mA
if code == 0x18: code = 8           ; AC + SLOW: 100 mA, input limit untouched
CHARGE_SET  = code & 0x3f           ; BATTCHRG_I
CTRL_CLR    = 0x01400000            ; clear DCDC4P2_BO_IRQ, VDD5V_DROOP_IRQ
CTRL_SET    = 0x00200000            ; ENIRQ_VDD5V_DROOP
CHARGE_CLR  = 0x00010000            ; PWD_BATTCHRG = 0  -> charger on
```

OAL 80212968 differs in three points [P]: STOP_ILIMIT `0x800`; it also does
`5VCTRL_CLR 0x00300000` (PWD_CHARGE_4P2=0); for the "AC" codes it sets PWD_BATTCHRG
before rewriting the current. It has no `0x18→8` rewrite, but the OAL never requests
SLOW, so `0x18` never reaches it.

Charger off (c0623d10): `CHARGE_CLR 0xf00; CHARGE_CLR 0x3f; CHARGE_SET 0x10000;
5VCTRL CHARGE_4P2_ILIMIT = 0x3f`.

Never written anywhere in pmuPdk or the OAL charger code [P]: ADJ_VOLT, LIION_4P1,
CHRG_STS_OFF, ENABLE_FAULT_DETECT, ENABLE_CHARGER_USB0/1, 5VCTRL.HEADROOM_ADJ.
ENABLE_LOAD is set only during 4P2 start-up and cleared again.

### 3.2 Who picks the mode

- `battdrvr` status poll (every 60 s, 5 s when low, and on `Global\USBEVENT`): if 5 V
  present, battery present, charger idle, not "full", not droop-held and charging not
  disabled → `PmuSetCharger(g_fast ? 1 : 0)` (c0662164). `g_fast` is 0 after init. [P]
- battdrvr IOCTL 0x290430 (c0661bc0): input `0x10` → SLOW, `0x20` → FAST, `0x40` → stop
  and disable. In this ROM `EdAppCtrl.exe` sends only `0x10` (on the `USBDetach`
  message, 00014370) and `DiagApp.exe` sends all three (0001ba68…; its UI strings are
  "Charger Enable(Slow)", "Charger Enable(Fast)", "Charging(Slow)", "Charging(Fast)"). [P]
- The dictionary application itself lives in NAND (`\Nand\wceprj.exe`), outside this
  dump, so whether it ever asks for FAST while ON cannot be excluded [?]. Nothing in
  ROM does [P].
- pmuPdk (c06240e8): request > 0 → `ChargeControl(7)` (FAST), request == 0 →
  `ChargeControl(5)` (SLOW). OAL off-loop: always `ChargeControl(7)` (80213250, 802138bc). [P]
- On suspend `PMI_PowerDown` stops the charger (event 8, c06228b8); the OAL then starts
  it again in FAST inside `OEMPowerOff` as long as STS.VBUSVALID0 is set. [P]

### 3.3 Charge control (c06250dc), pseudo-code

```
start(mode):                         ; mode 2 = pre, 4 = slow, 6 = fast
  if idle:
     vbat = BATT_VAL*8 ; tbat = thermistor ; tdie = die temp
     if vbat > 4130: status = FULL ; return            ; no charge
     if tbat == -100: status = NO_BATTERY ; return
     if tbat < -5 or tbat > 45: fault(TEMP) ; return   ; latched
     minutes = 0 ; full_count = 0
     if vbat < 3000: mode = 2 (pre-charge)
  elif fault latched: return
  elif pre-charging and mode != 2 and vbat < 3050: return   ; stay in pre-charge
  code = AC  ? {2: 0x07, 4: 0x18, 6: 0x2c}[mode]
       : USB ? {2: 0x07, 4: 0x08, 6: 0x24}[mode]
       : (stop, hold with status |= 0x200)
  program charger(code)
stop: charger off (latched fault bit is cleared only here)
```

### 3.4 Periodic events (c0625410)

Ticks: ON – `ChargeManagementThread`, 1000 ms sleep, 60 000 ms by GetTickCount.
OFF – TIMER2 at 1 kHz, FIXED_COUNT 0x400 (1.024 s), every 60th tick is the "minute".

Every 1 s while charging (event 7):
- thermistor: open (−100) → stop, status NO_BATTERY (restart automatically when it returns);
- die temp: > 383 K → stop + latched temperature fault; > 373 K → reprogram 100 mA
  (never raised again until the next start).

Every 60 s (event 1):
- charging or pre-charging: `minutes++`; thermistor open → stop; outside −5…45 °C → latched fault;
- pre-charging: `minutes > 30` → latched fault (status 0x08); else if VBAT ≥ 3050 mV →
  switch to the requested mode (SLOW or FAST);
- charging: `minutes > 300` → stop; VBAT < 3050 → fault 0x10; < 4100 → fault 0x04; else complete.
  Otherwise the "full" test of §1; after stopping VBAT is read again: ≥ 4000 → complete,
  < 4000 → status 0x80 (battdrvr will simply start a new charge);
- idle and full: VBAT < 4000 mV → start again (recharge);
- idle and NO_BATTERY: thermistor back → start again.

Latched faults (temperature, time-outs, 5 V supply error) are cleared only by a stop:
5 V removal, suspend/resume or `PmuStopCharger`. [P]

While pre-charging, a status request (event 5, c0625854) switches the charger off for
500 ms, reads VBAT, then re-enables 80 mA, so the reported voltage is open-circuit.
In normal charge VBAT is read with the charger running. [P]

Battery voltage everywhere = `HW_POWER_BATTMONITOR.BATT_VAL × 8 mV`, kept fresh by
LRADC ch 7 automatic conversion (delay channel 3, 0x3ff × 0.5 ms, CONVERSION
SCALE_FACTOR = 2, AUTOMATIC = 1; BATTMONITOR.EN_BATADJ = 1). [P] c0624898–c06248f4, lradc c0672588

Debug log codes (`[LOG] BE[C=n]`): 0 stop, 1 start, 2 complete, 10 no battery, 11 battery
temperature, 12 pre-charge timeout, 13/14 charge timeout (VBAT < 3050 / < 4100),
15 terminated but < 4000 mV, 16/17 droop released/held, 18 die-temp stop, 19 die > 110 °C,
20 die > 100 °C, 21 recharge, 22 battery returned, 23 5 V supply error.
`BE[U=0/1/2]` = 5 V gone / valid / sagging, `BE[AC=0/1]` = USB / AC adapter.

## 4. Temperature

Battery thermistor (lradc.dll c067359c, OAL 802159fc) [P]:
1. `HW_LRADC_CTRL2`: TEMP_SENSOR_IENABLE0 = 1, TEMP_ISRC0 = 7; wait 10 ms.
2. 5 conversions of channel 0, mean of the 12-bit values; then ISRC0 = 0, IENABLE0 = 0,
   TEMPSENSE_PWD = 1.
3. Add a per-device calibration offset if byte `[0xA00EA8D0]` ≠ 0: int32 at
   `0xA00EA8D1`, clamped to ±200 counts (factory "BATT TEMP AD calibration data").
4. `> 3072` → −100 (no battery). Else interpolate in this table (5 °C per step):

| °C | −25 | −20 | −15 | −10 | −5 | 0 | 5 | 10 | 15 | 20 | 25 | 30 | 35 | 40 | 45 | 50 | 55 | 60 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ADC | 2883 | 2811 | 2729 | 2637 | 2538 | 2433 | 2324 | 2213 | 2103 | 1997 | 1895 | 1801 | 1713 | 1633 | 1561 | 1497 | 1439 | 1388 |

   Above 2883 → −26, at or below 1388 → 61. With integer truncation the window is
   effectively **1561 ≤ ADC ≤ 2557**.
- TEMP_ISRC code 7 = 140 µA is from the i.MX28 manual [I]. DIVIDE_BY_TWO for channel 0
  is not touched by this code, so it is the reset value 0 [I] (the EVK LRADC keypad
  driver that could change it is not registered: registry key is `KeyPadS` without `Dll`).
- The calibration offset comes from SHARP's shared boot area (phys 0x400EA8D0), filled by
  their loader; not available from this dump [?]. 200 counts is roughly 10 °C.

Die temperature (lradc.dll c0672b90, OAL 80212d84) [P]: TEMPSENSE_PWD = 0, LRADC1SELECT = 9,
LRADC6SELECT = 8, 5 samples each, `K = (ch9 − ch8) × 1012 / 4000`; selects restored to 1 and 6.

## 5. 5 V present: detection, AC vs USB, 4P2, droop

Init (`PowerInit` c06246e4) [P]: `5VCTRL_SET 0x10` (VBUSVALID_5VDETECT), VBUSVALID_TRSH = 1
(4.0 V), `BATTMONITOR |= 0x400`, LOOPCTRL RCSCALE_THRESH = 1 / EN_RCSCALE = 3,
`CTRL_CLR 1` (ENIRQ_VDD5V_GT_VDDIO off), `CTRL_SET 8` (ENIRQ_VBUS_VALID),
`CHARGE_SET 0x10000`. VDD5V_GT_VDDIO is not used as a detector. IRQ 6 (VDD5V) and
IRQ 4 (VDD5V_DROOP) are used.

Cable check (`PowerSupplyThread` c06241ec) [P]:
1. VBUSVALID_TRSH = 1, wait 10 ms; if STS.VBUSVALID0:
2. ask `UFN1:` IOCTL 0x2201a4; result 3 → AC adapter, else USB. `g_4p2_ilimit = 0x3f`.
3. `CTRL_CLR 0x00a00000`; if DCDC4P2.ENABLE_4P2 == 0 → start 4P2; clear irqs,
   POLARITY_VBUSVALID = 0, DCDC_XFER only if the 4P2 flag is 0, wait 50 ms,
   `CTRL_SET 0x200000` (droop irq on).
4. On the falling irq: 4P2 off (ENABLE_DCDC/ENABLE_4P2 = 0, PWD_CHARGE_4P2 = 3,
   DROPOUT_CTRL = 0, PWRUP_VBUS_CMPS = 0, ILIMIT = 0x3f, ENABLE_LINREG_ILIMIT = 1), charger
   stop, then VBUSVALID_TRSH = 0 (2.9 V): still valid → "sagging" (treated as on battery,
   VDD5V polled every 5 s, re-check when ≥ 4600 mV); not valid → removed, TRSH back to 1,
   `5VCTRL_SET 1`.

AC-adapter detection (usbfn c06a29f4, OAL 80212b10) [P], USBPHY0 at 0x8007C000,
PORTSC1 at 0x80080184:
`PHY_CTRL_CLR 0x10` → `PHY_DEBUG_SET 0x30`, `PHY_DEBUG_SET 0x0c` → 10 ms →
`PHY_DEBUG_CLR 0x0c`, `PHY_CTRL_SET 0x10` (ENDEVPLUGINDETECT) → wait 500 ms (abort on bus
activity) → if PORTSC1 line state (bits 11:10) == 11: `PHY_DEBUG_SET 0x08`, wait 100 ms,
line state == 00 → **3 = dedicated charger (D+/D− shorted)**; anything else → 2.
Restore: `PHY_CTRL_CLR 0x10`, `PHY_DEBUG_CLR 0x0c`, 1 ms, `PHY_DEBUG_CLR 0x30`.

USB enumeration [P]: no 100 mA / 500 mA handling exists. `BSPUsbSetCurrentLimitation`
(usbfn c0693c8c) only writes ILIMIT = 0x3f when called with 0. A PC port is charged with
the 480 mA input limit whether or not the gadget is configured (and the device does this
while OFF too).

4P2 start, driver version (c06230e8) [P]: PWRUP_VBUS_CMPS = 1, VBUSVALID_5VDETECT = 1,
TRSH = 0 during the sequence; VDDD/VDDA/VDDIO LINREG_OFFSET = 2 and FETs enabled;
`DCDC4P2`: CMPTRIP = 0, TRG = 0 (4.2 V), ENABLE_4P2 = 1; `CHARGE_SET 0x400000` (ENABLE_LOAD);
PWD_CHARGE_4P2 = 0; CHARGE_4P2_ILIMIT stepped 1…63 with ≥ 100 µs per step, then set to
`g_4p2_ilimit` (0x3f); `DCDC4P2.BO = 0x10` (4.0 V); wait STS.DCDC_4P2_BO == 0 (≤ 1 s);
`DCDC4P2.ENABLE_DCDC = 1`; `5VCTRL_SET 1` (ENABLE_DCDC); restore TRSH/5VDETECT/PWD_BO;
ENABLE_LOAD = 0. DROPOUT_CTRL is not written here.
OAL version (8020e2ec) [P]: same idea, DROPOUT_CTRL = 0x1, ramp 50 µs per step ending at
0x3f, `5VCTRL_CLR 0x80`, BO = 0x16 briefly then 0.

Droop / weak supply [P]: a VDD5V_DROOP irq while BATTCHRG_I ≠ 0 stops the charger
(status |= 0x200). VDD5V is then read on LRADC ch 15 every 1 s (`mV = raw × 7400 / 4096`);
≥ 4650 mV → charge request again. Over the next 5 one-second ticks the charger must
survive at least 3, otherwise "5 V supply error" (C=23): stop, latched until unplug,
`Battery_Event_Voltage Error` → `CHARGE_ERR.WAV`.

## 6. Gauge, low battery, shutdown (battdrvr.dll c0661e1c) [P]

- Input: BATT_VAL × 8 mV, mean of up to 6 samples spaced ≥ 5 s (history cleared on
  AC plug/unplug; after unplug it waits 400 ms and re-reads).
- Levels (strictly greater than):

| Averaged VBAT | BatteryLifePercent | BatteryFlag | DiagApp name |
|---|---|---|---|
| > 3900 mV | 100 | HIGH | (full) |
| > 3750 mV | 75 | HIGH | "Level1" |
| > 3450 mV | 50 | HIGH | "Level2" |
| > 3350 mV | 25 | LOW | "Caution" → `EdPowerCaution` broadcast |
| ≤ 3350 mV | 10 | CRITICAL | "Fatal" → backlight off, `Global\PM_FATAL_OFF_EVENT` |
| < 3250 mV | — | — | emergency off (`PWR1:` IOCTL 0x220018 + fatal event) |

  DiagApp's "Discharge until Level1/Level2/Caution/Fatal" targets are exactly
  3900/3750/3450/3350 (0001f168…0001f2b0) [P]. That 100/75/50 are the 3/2/1-bar icon is
  [I]: the icon is drawn by the NAND application.
- On battery the level only goes down, one step at a time (HIGH never jumps to CRITICAL).
- Charging: flag CHARGING while the charger runs; full is reported as
  `BatteryLifeTime = 540000`; faults are encoded in `BatteryLifeTime`: 30 = time-out,
  9 = too cold, 10 = too hot, 11 = die temperature.
- AC vs USB is exported in `BackupBatteryLifeTime` (1 = AC adapter).
- OFF loop: with 5 V sagging and VBAT ≤ 3350 mV three times, the OAL gives up and powers off.
- Registry `Drivers\BuiltIn\Battery` (BatteryVoltageLowLevel 3200, HighLevel 3700,
  MaxBatteryVoltage 4200) is Freescale leftover; battdrvr does not read it [P].

## 7. What a Linux driver should replicate

1. Keep PWD_BATTCHRG = 1 and BATTCHRG_I = 0 whenever not charging; stop on 5 V loss.
2. No start above 4130 mV, recharge below 4000 mV, pre-charge 80 mA below 3000 mV until
   3050 mV (30 min limit), termination = CHRGSTS low and ≥ 4130 mV for 5 minutes,
   300 min limit.
3. Currents: WinCE only ever fast-charges (450 mA USB / 550 mA AC, STOP_ILIMIT 100 mA)
   with the system suspended; with the system running it uses 100 mA and STOP_ILIMIT
   50 mA. Linux has no "off" loop, so this is a design choice. At 100 mA a deeply
   discharged pack will not finish inside the 300 min limit [I, capacity unknown];
   WinCE then stops with a time-out fault (C=14, `BatteryLifeTime` = 30) [P].
4. Input limit: CHARGE_4P2_ILIMIT 480 mA while charging from anything that is not a
   D+/D−-shorted adapter, 780 mA otherwise and whenever the charger is off.
5. Battery thermistor on LRADC0 with the 140 µA source, table of §4, window −5…45 °C,
   open circuit = no battery = no charging. The factory offset is not available, so
   keep margin.
6. Die temperature each second: > 100 °C → 100 mA, > 110 °C → stop.
7. Faults latch until unplug. Droop irq → stop and retry only when VDD5V ≥ 4.65 V; give
   up after repeated collapses.
8. Leave ADJ_VOLT / LIION_4P1 / ENABLE_FAULT_DETECT at reset values.
9. There is no charge LED in this code path [I] (GPIO3_4/3_5 written by battdrvr are the
   LCD backlight); feedback is `CHARGE.WAV` / `CHARGE_ERR.WAV` and codec beeps in the OFF loop.

## 8. Gaps

- SHARP bootloader not in the dump → initial 4P2/charger state before NK unknown.
- NAND application not in the dump → cannot exclude FAST requests while ON; bar icon mapping.
- Thermistor calibration offset and the charge-enable bits read by battdrvr at init
  (`[0xA00EA404] & 0x08000080`, else "Disable Charging") come from SHARP's boot area.
- Battery capacity is not encoded anywhere in these modules.

## 9. Evidence excerpts

Current selection, pmuPdk `ChargeControl` (AC branch then USB branch):

```
c06252c0  bl   c0622b78            ; cable == 4 (AC)?
c06252cc  and  r3, r6, #6          ; mode
c06252e0  movne r0, #7             ; pre-charge
c06252ec  mov  r0, #0x2c           ; mode 6, AC   -> 550 mA
c06252fc  mov  r0, #0x18           ; mode 4, AC   -> rewritten to 8 in c06225c4
c0625310  bl   c0622b94            ; cable == 3 or 4 ?
c0625330  movne r0, #7
c062533c  mov  r0, #0x24           ; mode 6, USB  -> 450 mA
c062534c  mov  r0, #8              ; mode 4, USB  -> 100 mA
c0625360  bl   c0622c10            ; BSPBattdrvrSetCharger (rejects > 0x3c)
```

Programming, pmuPdk c0622534 (r2 = HW_POWER base):

```
c0622550  mov r3,#0xf00 ; c0622554 mov r1,#0x400
c0622558  str r3,[r2,#0x38] ; c062255c str r1,[r2,#0x34]     ; STOP_ILIMIT = 50 mA
c0622560  mov r1,#0x3f ; c0622568 str r1,[r2,#0x38]
c0622564  and r3,r4,#0x28 ; c0622570 cmp r3,#0x28 ; beq c06225b0
c0622578  and r3,r4,#0x30 ; cmp r3,#0x30 ; beq c06225b0 ; c0622584 cmp r4,#0x18 ; beq c06225b0
c062259c  mov r3,#0x3f000 ; str r3,[r2,#0x18] ; mov r3,#0x27000 ; str r3,[r2,#0x14]
c06225bc  cmp r4,#0x18 ; c06225c4 moveq r4,#8
c06225c8  and r3,r4,#0x3f ; str r3,[r2,#0x34]
c06225ec  mov r2,#0x10000 ; c06225f4 str r2,[r3,#0x38]        ; PWD_BATTCHRG = 0
```

OAL twin: `80212970 mov lr,#0xf00 ; 80212974 mov r4,#0x800 ; 80212984 str lr,[r3,#0x38] ;
80212994 str r4,[r3,#0x34]` (STOP_ILIMIT = 100 mA).

Thresholds in the state machine: `c06254c4 cmn r0,#5 / c06254cc cmp r0,#0x2d` (−5/45 °C),
`c06254e8 cmp r3,#0x1e` (30 min), `c062554c cmp r3,#0x12c` (300 min),
`c0625538 0xb00|0xea` (3050), `c06255b8 0x1000|4` (4100), `c0625638 0x1000|0x22` (4130),
`c0625654 cmp r3,#5`, `c0625728 cmp r3,#0xfa0` (4000),
`c0625934 0x100|0x7f` (383 K), `c0625970 0x100|0x75` (373 K).

OFF loop: `80213250 mov r1,#7 ; bl 80213bf8` (FAST), tick `8021393c ldr r3,=0x88888889 …
8021395c moveq r2,#1 ; movne r2,#7` (every 60th tick = minute event).

Diff check: after masking branch targets and literal offsets, pmuPdk c0625410…c0625ab0
vs nk.exe 80213f2c… (425 instructions) and c06250dc… vs 80213bf8… (202 instructions)
are identical.

## 10. Tooling (`ref/wince/charger/`)

- `romx.py` – ROMHDR/TOC/e32/o32 parser (`python3 romx.py v` lists modules and sections).
- `extract.py <module>…` – dumps sections and strings to `mods/`.
- `cedis.py <module>…` – annotated ARM listing in `dis/` (needs `pip install capstone`):
  literal pools, UTF-16 strings, import thunks resolved through coredll's export table.
- `findconst.py <hex>…` – finds code that builds a constant (IOCTL codes, base addresses).
- Kept listings: pmuPdk, pmuSdk, battdrvr, lradc, nk.exe, usbfn, EdAppCtrl, DiagApp,
  edna2_powermgr. Others can be regenerated.
