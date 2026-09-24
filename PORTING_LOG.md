# Pixel 6a postmarketOS porting log: from scratch to the castle

This document records how the Google Pixel 6a (`bluejay`, Tensor GS101) tree
diverged from the original postmarketOS and GS101 sources. It covers both the
working hardware enablement and the unfinished experiments preserved in this
branch.

The tree is an experimental snapshot, not an upstream-ready patch series. The
known-good release subset lives on the repository's `main` branch. This full
snapshot is kept on `bluejay-experimental-full` so development can continue on
another machine without losing failed experiments, firmware packaging, or
diagnostic utilities.

## Starting point

- postmarketOS `pmaports` edge snapshot based around upstream commit `1ea42816`.
- `pmbootstrap` 3.11.1 with a separate Bluejay configuration.
- Linux 7.0.6 from the GS101 mainline development tree.
- Initial device support could boot a framebuffer/touch-oriented system, but
  lacked usable accelerated graphics, Wi-Fi, Bluetooth, battery reporting,
  charging, thermal control, audio, and cellular service.
- OpenRC was retained after Plasma Mobile's systemd-channel dependency path
  proved unsuitable for this working tree.

## 1. Boot, display, touch, and image production

- Established the `device-google-bluejay` package and Bluejay boot metadata.
- Built and flashed all three required image classes together: root filesystem,
  `boot.img`, and `vendor_boot.img`. Mixing an old boot/vendor boot with a new
  rootfs repeatedly caused debug-shell or splash-screen failures.
- Added `0007-drm-sysfb-clip-damage-to-scanout.patch` to stop invalid framebuffer damage
  rectangles from reaching scanout.
- Standardized build/export/flash helpers and checksum checks. The stable branch
  contains `build-flash.sh` and `build-all-bluejay.sh`; this snapshot preserves
  the complete package tree consumed by those scripts.

Result: Plasma Mobile boots with working display and touchscreen.

## 2. Mali-G78 Panfrost acceleration

Kernel changes:

- `0001-drm-panfrost-add-mali-g78.patch` adds Mali-G78 recognition.
- `0002-drm-panfrost-log-gpu-capability-registers.patch` exposed capability data
  used while validating the G78 (`id 0x9202`, `shader_present=0x779fff`).
- `0003-arm64-dts-gs101-mark-gpu-dma-coherent.patch` fixes the GS101 coherency
  selection responsible for corrupted or shattered text.
- `0019-drm-panfrost-gs101-enable-acpm-devfreq.patch` connects Panfrost to GS101
  ACPM-backed GPU frequencies (151–572 MHz) instead of leaving it at the boot
  clock.

Mesa changes:

- `temp/mesa/panfrost-g78-gs101.patch` adds the matching userspace support.
- The Bluejay package pins the validated Mesa build (`26.2.2-r2`) because an
  unpatched or later repository Mesa silently falls back to LLVMpipe.
- `device-google-bluejay/90-bluejay-gpu.sh` supplies the session environment.

Important diagnosis: a running DRM device does not prove acceleration. The
actual failure was confirmed by Qt RHI and `eglinfo` reporting LLVMpipe. With
the patched/pinned Mesa, Plasma uses Panfrost again.

Result: hardware acceleration works. AFBC remains disabled because enabling the
wrong coherency/compression path caused visual corruption or GPU faults.

## 3. Wi-Fi

- Added GS101 HSI1 clocks, PCIe PHY, and PCIe root-complex support:
  `0004`, `0005`, and `0006` in `linux-postmarketos-gs101`.
- Packaged the external `bcmdhd.ko` plus Bluejay firmware/calibration files in
  `firmware-google-bluejay-bcmdhd`.
- Installed `fw_bcmdhd.bin`, `bcmdhd.cal`, and `bcmdhd_clm.blob` at the paths
  expected by the driver.
- Added NetworkManager policy in `NetworkManager-bluejay.conf`: the auxiliary
  interface is unmanaged and Wi-Fi power saving is disabled to prevent the
  observed 5–10 minute traffic stalls/reconnect cycle.
- Added `bluejay-wifi-low-power` to toggle the more aggressive low-power policy
  explicitly rather than making it the unreliable default.

Result: scanning, association, and sustained network traffic work. Wi-Fi and
modem PCIe experiments must still be tested together because both touch shared
GS101 PCIe resources.

## 4. Battery reporting and charging

- Added the Bluejay MAX77759 fuel-gauge device-tree node with
  `0010-arm64-dts-gs101-add-max77759-fuel-gauge.patch`.
- Backported/adapted the MAX17042/MAX77759 fuel-gauge implementation and headers
  into the kernel package. It exposes `max170xx_battery` with capacity, voltage,
  current, temperature, health, and charge counters.
- Added the MAX77759 charger implementation and module-load configuration.
- The charger uses a 3 A fallback input limit when TCPM reports no negotiated
  limit, matching the tested hardware path. It exposes charging status to the
  desktop environment.
- Earlier attempts to bind the generic MAX17042 driver directly returned
  `ENODEV`; the Pixel-specific MAX77759-compatible handling was required.

Result: the battery icon, battery statistics, and charging status work. Charging
is experimental and should be monitored for temperature and negotiated-source
limits.

## 5. Bluetooth: BCM4389

This was the longest completed subsystem investigation.

Kernel/device-tree work:

- `0011` adds the BCM4389 Bluetooth UART description.
- `0012` exposes `/dev/ttySAC1` for userspace provisioning.
- `0013` adds Samsung UART RTS trigger handling.
- `0014` added UART-state diagnostics.
- `0015` implements the minimal Google Nitrous power/rfkill driver.
- `0016` keeps the required USI UART clock requested.
- `0017` tolerates an optional controller MWS command rejected by this firmware.

Userspace work:

- Initial `hci_bcm` and `hciattach bcm43xx` attempts timed out during reset,
  baud-rate changes, or HCD download even though GPIO and UART TX counters were
  correct.
- GPIO power toggling alone was insufficient; the controller required Google's
  actual provisioning sequence.
- `temp/bluejay-btloader` is a small standalone BCM4389 loader derived from that
  sequence. It enters MiniDrv, streams all HCD records, performs the final reset,
  changes UART speed, and attaches `hci0`.
- Firmware is packaged by `firmware-google-bluejay-bluetooth` with the expected
  BCM4389 aliases.
- `bluejay-bluetooth` performs power/provision/attach with cold-boot retries.
- `bluejay-bluetooth-finalize` waits for the controller, restarts Bluetooth,
  raises `hci0`, and applies the same recovery sequence that proved reliable
  manually.
- A stable locally administered Bluetooth address is derived when no factory
  address is available; the original `AA:AA:AA:AA:AA:AA` address prevented
  practical discovery/pairing.
- BlueDevil defaults are installed with Bluetooth unblocked.

Result: controller initialization, discovery, pairing, and normal Bluetooth use
work. OpenRC install hooks enable both Bluejay Bluetooth services at installation.

## 6. Thermal sensors and performance policy

- `0018-thermal-gs101-add-read-only-acpm-sensors.patch` exposes ACPM-backed
  thermal zones for the little/mid/big CPU clusters, GPU, ISP, and TPU.
- `bluejay-thermal-control` applies progressive CPU/GPU limits at 68, 76, and
  84 degrees Celsius.
- `bluejay-power.start` applies the tested CPU governor and frequency ceilings.
- The policy was tested with sustained 1080p60 playback; the first thermal stage
  held the device near 68 degrees Celsius.

Result: standard thermal-monitoring applications can read the zones, and the
device has a conservative userspace thermal policy. Hardware video decoding is
not implemented, so high-resolution software-decoded video remains expensive.

## 7. Plasma Mobile and permissions

- Plasma Mobile was made functional on OpenRC/tinydm.
- Added NetworkManager Polkit permission for administrative Wi-Fi actions; this
  removed the earlier requirement to connect only through `sudo nmcli`.
- Added the tinydm GPU environment and BlueDevil defaults.
- Duplicate tinydm/KWin sessions were identified as another source of severe
  lag; only one compositor/session should run.

Result: Plasma Mobile, Wi-Fi settings, battery indication, charging indication,
GPU composition, and Bluetooth integration work in the validated image.

## 8. AOC and built-in audio — preserved, unsafe, unfinished

Preserved sources include:

- `firmware-google-bluejay-aoc` and proprietary `aoc.bin` packaging.
- AOC, mailbox, IOMMU, ALSA, and speaker-amplifier patches/modules.
- `device-google-bluejay/bluejay-audio-test` and AOC blacklist/load controls.

What was learned:

- Loading the AOC stack during normal boot can freeze or reboot the phone.
- Safe staged tests reached AOC IOMMU/mailbox setup and ALSA probe, but the
  sound card waited for the AOC output-control channel.
- Manual platform binding failed with `-EINVAL` in `pinctrl_bind_pins`, traced
  before the actual AOC probe ran. This means at least one active/default AOC
  pinctrl state is invalid for the current mainline DT.
- Later staged loading still stopped the device, so speaker-amplifier nodes were
  deliberately kept disabled and AOC autoload is not part of the stable image.

Result: no safe built-in audio. Do not enable the AOC service at boot until its
pinctrl, firmware handshake, and output-control sequencing are fixed.

## 9. Cellular/RIL — preserved, incomplete

Preserved work includes:

- `0008-pci-gs101-add-cpif-compatibility.patch` and
  `0009-arm64-dts-gs101-add-cpif.patch`.
- CPIF/shmem/boot-device modules and S5123 plumbing.
- `device/testing/tinycbd`, its firmware/NV preparation helper, init script,
  and status utility.
- `bluejay-sit-monitor`, a read-only Samsung SIT traffic monitor.

What was achieved:

- CP power-on and bootloader transfer worked.
- BOOT, MAIN, NV_NORM, NV_PROT, and REPLAY images could be uploaded.
- The security ioctl absent from the PCIe path was made nonfatal.

Current blocker:

- `IOCTL_COMPLETE_NORMAL_BOOTUP` times out while waiting for the CP
  initialization handshake. Repeated SPI image requests were observed.
- There is no completed S5123 SIT daemon exposed through oFono/ModemManager.
- SIM state/PIN, registration, operator selection, mobile data, SMS, and voice
  calls are therefore unfinished.
- Enabling CPIF also interfered with Wi-Fi in some builds, reinforcing the need
  to resolve PCIe/resource interactions before autostarting `tinycbd`.

Result: modem firmware upload is partially working; this is not usable RIL and
must not autostart in the stable image.

## 10. Features still missing or deliberately deferred

- Built-in speakers and microphones (AOC audio).
- Cellular data, calls, SMS, and SIM management.
- Accelerometer/IIO-backed automatic rotation.
- Hardware video decoding.
- Deep suspend; use s2idle.
- Camera enablement.
- A fully upstreamable, minimal patch series.

## Current branch roles

- `main`: validated release subset—display/touch, Panfrost, Wi-Fi, Bluetooth,
  battery, charging, thermal control, and Plasma Mobile. It intentionally omits
  audio and RIL.
- `bluejay-experimental-full`: complete self-contained pmaports snapshot,
  including proprietary payloads, build artifacts, audio experiments, CPIF/RIL,
  and diagnostics. Continue risky development here.

## Repository milestones

Stable-source history:

- `cf19203` — first combined BT, Mesa/Panfrost, Wi-Fi, battery, and charging set.
- `8b233f6` — publish stable hardware support without audio/RIL.
- `352d375` — validated thermal governor.
- `f56bd3e` — clean release build fixes and full build documentation.
- `9ab2c9b` — package the tested CPU policy.
- `2f940a4` — dedicated complete build/flash script.
- `57b0493` — enable Bluejay services during package installation.

Full-tree export:

- `0895427` — self-contained experimental pmaports snapshot.

## Resume checklist on another computer

1. Clone `bluejay-experimental-full` as a single branch.
2. Install and initialize a compatible `pmbootstrap` configuration for
   `google-bluejay`, edge, OpenRC, and Plasma Mobile.
3. Keep the Mesa package pinned until the GS101 Panfrost patch is rebased.
4. Build kernel, firmware packages, BT loader, device package, and rootfs.
5. Flash matching `boot`, `vendor_boot`, and rootfs images together.
6. Verify Panfrost renderer, Wi-Fi stability, Bluetooth services, battery,
   charger, and thermal zones before attempting AOC or CPIF work.
7. Keep AOC audio and `tinycbd` disabled at boot while debugging them.
