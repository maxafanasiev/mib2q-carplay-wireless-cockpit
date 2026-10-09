# MHI2Q CarPlay Wireless Cockpit

CarPlay on the Audi Virtual Cockpit for MHI2Q head units, **wired and through a wireless CarPlay adapter**.

This is a fork of [lindayi/mib2-carplay-rgi-altscreen](https://github.com/lindayi/mib2-carplay-rgi-altscreen),
which combines [MHI2Q-CarPlay-AltScreen](https://github.com/yuedizhibo/MHI2Q-CarPlay-AltScreen) (CarPlay
cluster video) with [mib2q-carplay-rgi](https://github.com/luka-dev/mib2q-carplay-rgi) (route guidance on the
cluster). Upstream works with a cable only: a wireless adapter never requests the CarPlay instrument-cluster
stream, and with AltScreen loaded the adapter session does not come up at all. This fork adds a wireless mode.

**Tested on:** Audi Q7 (4M) · `MHI2Q_ER_AUG22_P5092` · MU `1329` · Audi Virtual Cockpit ·
wireless adapter "smartBox" (Carlinkit-class, firmware `20250630126`) · iPhone.

**Disclaimer:** use at your own risk. This modifies binaries and system configuration of the head unit.
Back up the unit first. The authors are not responsible for damage, bricked units or warranty issues.

## What you get

| | Wired iPhone (`wired` mode) | Wireless adapter (`wireless` mode, default) |
| --- | --- | --- |
| CarPlay on the main screen | yes | yes |
| Cluster picture | separate CarPlay cluster map (AltScreen) | copy of the whole main CarPlay screen, dock included, with a blurred fill on the sides |
| Picture stays on the cluster while the MMI shows its own UI (camera, menus, retracted screen) | as upstream | yes |
| Steering-wheel roller | zooms the cluster map | turns the CarPlay knob (moves focus, scrolls lists) |
| Steering-wheel roller press / back key | as upstream | CarPlay knob select / back |
| Route-guidance arrows (RGI) | yes | depends on whether the adapter forwards route guidance |

Switch modes in GEM (see below). The choice survives reboots and applies on the next phone connection.

## How the wireless mode works

- `carplay_startup.sh` starts `dio_manager` without the AltScreen preload unless `hooks/mode.wired` exists.
- The hook interposes `screen_post_window` / `screen_destroy_window` (libairplay draws the CarPlay video
  through its PLT), reads the main CarPlay window back with `screen_read_window` and publishes RGBA frames
  to the `/carplay_mainmirror` shared memory.
- `mainmirror_display` draws those frames into cluster displayable 3; the Java patch switches the cluster
  to context 81 while `/tmp/carplay_mainmirror.ready` exists.
- `AirPlayReceiverSessionChangeModes` is interposed so the phone is not told that the car took the
  screen; otherwise iOS stops sending video whenever the MMI shows its own UI.
- The roller and wheel keys are turned into reports on the virtual HID knob that `dio_manager` registers
  with the phone (`HIDDevicePostReport`, same bytes the real MMI knob produces).
- The AltScreen library is patched to add the alt screen to the SETUP response only when the source
  asks for it (`tools/patch_altscreen_requested.py`), which keeps wired sessions unchanged.

## Build

Requirements are the same as upstream: Docker, the
[qnx65-armv7-toolchain](https://github.com/luka-dev/qnx65-armv7-toolchain) image, and the stock HMI jar of
your own unit converted with jxe2jar (`../../Tools/jxe2jar/out/<STOCK_JAR>`). No Audi firmware or jar is
included in this repository.

```sh
git clone https://github.com/luka-dev/qnx65-armv7-toolchain
(cd qnx65-armv7-toolchain && ./host-scripts/qnx-run.sh build)

STOCK_JAR=MU1329-base.jar ./scripts/build_sd.sh                    # -> build/sd/
SD=/Volumes/CARD STOCK_JAR=MU1329-base.jar ./scripts/build_sd.sh   # or straight onto a card
```

`build_sd.sh` builds the Java patch, the hook, the maneuver renderer and `mainmirror_display`, applies the
30 fps and the "alt screen on request" patches to AltScreen, and verifies `SHA256SUMS-SD.txt`.

Tests: `./scripts/run_tests.sh`, `STOCK_JAR=MU1329-base.jar ./scripts/test_route_info.sh`, and the
`scripts/test_install_*.sh` / `scripts/test_supervisor_lifecycle.sh` suites.

## Install

Prerequisites: CarPlay already working on the unit (FEC / CarPlay activation, e.g. with M.I.B.) and GEM
enabled.

1. Build the card, insert it into SD1.
2. GEM → **Toolbox → Update Toolbox**.
3. GEM → **MMI-Cockpit-Carplay → INSTALL**, reboot.
4. GEM → **MMI-Cockpit-Carplay → START**, reboot.

To remove everything: **MMI-Cockpit-Carplay → RESTORE ORIGINAL** with the card that ran INSTALL (it keeps the
stock backups in `MMI-Cockpit-Carplay/` on that card), then reboot.

## GEM menu (MMI-Cockpit-Carplay)

| Entry | Effect |
| --- | --- |
| Cockpit mod: ON / OFF | OFF starts stock CarPlay (nothing preloaded, no renderers); ON brings the mod back |
| Cockpit mode: wireless / wired | wireless = main-screen mirror (default); wired = AltScreen cluster map for a cable iPhone |
| INSTALL, START, STATUS, RESTORE ORIGINAL, Cluster map layout, … | as upstream |

## Settings

Optional `/mnt/app/root/hooks/mainmirror.conf`, one `key=value` per line, read on the next phone connection:

| Key | Default | Meaning |
| --- | --- | --- |
| `fit=x,y,w,h` | `316,0,806,455` | cluster area (of 1440x455) the whole CarPlay screen is fitted into |
| `crop=x,y,w,h` | off | fill the cluster with this part of the CarPlay frame instead |
| `bg_blur` / `bg_dim` | `1` / `45` | blurred side fill and its brightness in percent |
| `fps` | `15` | capture rate |
| `wheel_invert` | `0` | flip the roller direction |
| `keep_screen` | `1` | keep video coming while the MMI shows its own UI |
| `swap_rb` | `1` | colour channel order |

`/mnt/app/root/hooks/mainmirror.off` disables the mirror without uninstalling.

## Known limitations

- An adapter cannot carry the separate CarPlay cluster map; the cluster shows what the main screen shows.
- The cluster's own lower info bar (altitude, street, scale) stays on top of the mirror; the mod only
  blanks its values.
- Turning the MMI console knob while the screen is retracted extends the screen. That decision is made
  by the IOC, not by the head unit software, so use the steering-wheel roller instead.
- Tested on one car, one adapter and one firmware. Other adapters may behave differently.

## Logs

As upstream: `/tmp/carplay_hook.log`, `/tmp/carplay_java.log`, `/tmp/carplay_wrapper.log`,
`/tmp/mainmirror_display.log`. Use `logging_MoreIncredibleBash/` to save them to the card.

## Credits and licenses

- [yuedizhibo](https://github.com/yuedizhibo) and [Lanye-z](https://github.com/Lanye-z):
  MHI2Q-CarPlay-AltScreen, [PolyForm Noncommercial 1.0.0](altscreen/LICENSE). Noncommercial use only.
- [luka-dev](https://github.com/luka-dev): mib2q-carplay-rgi, route guidance and the hook framework.
- [lindayi](https://github.com/lindayi): the combined AltScreen + RGI build this fork is based on.
- [jilleb](https://github.com/jilleb/mib2-toolbox): MIB2 Toolbox, [MIT](altscreen/LICENSE.TOOLBOX-MIT).
- Mirror sidecar: [Unlicense](altscreen/Toolbox/carplay_alt_screen/mirror_display/release/LICENSE.MMI-MIRROR).

The additions in this fork are released under the same noncommercial terms. Do not resell.
