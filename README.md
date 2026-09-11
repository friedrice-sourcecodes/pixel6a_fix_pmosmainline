# postmarketOS on Google Pixel 6a (bluejay)

Experimental postmarketOS device, Linux 7.0.6 and userspace support for the
Google Pixel 6a (`bluejay`, Tensor GS101).

## Current status

| Component | Status |
| --- | --- |
| Display and touch | Working |
| Panfrost Mali-G78 acceleration | Working, experimental |
| Wi-Fi | Working through `bcmdhd` |
| Bluetooth | Working through the Bluejay BCM4389 provisioner |
| Battery reporting and charging | Working, experimental |
| Thermal sensors | Read-only ACPM probes implemented; device validation pending |
| Plasma Mobile | Working |
| Audio | Not working |
| Calls/mobile data/SMS | Not working; CPIF/tinycbd bring-up is incomplete |
| Deep suspend | Not working; retain s2idle |

This is development-quality software. Keep a Google factory image available,
back up important data and expect to recover the phone with fastboot.

## Source layout

- `pmaports/device/testing/device-google-bluejay`: device configuration,
  OpenRC ordering, Plasma defaults and NetworkManager policy.
- `pmaports/device/testing/linux-postmarketos-gs101`: kernel APKBUILD and
  GS101/Bluejay patches.
- `pmaports/temp/bluejay-btloader`: standalone BCM4389 firmware provisioner.
- `pmaports/temp/mesa`: Mesa package with the experimental GS101 Mali-G78 patch.
- `pmaports/device/testing/firmware-google-bluejay-*`: package recipes only;
  proprietary payloads are deliberately excluded.

Copy these paths into a matching postmarketOS `pmaports` checkout, preserving
their relative paths. Configure pmbootstrap to use that checkout before building.

## Build

The helper expects an initialized pmbootstrap configuration and pmaports tree:

```sh
BLUEJAY_BASE="$PWD" \
BLUEJAY_PMAPORTS="$PWD/work/pmaports-7.0.6" \
BLUEJAY_PMBOOTSTRAP_CONFIG="$PWD/work/pmbootstrap-bluejay-7.0.6.cfg" \
./build-flash.sh build
```

Set `BLUEJAY_PASSWORD` only if a non-interactive image build is desired.
Otherwise pmbootstrap asks for the image user's password.

Do not run `flash` until the generated checksums pass and fastboot reports the
product as `bluejay`.

## Bluetooth boot behavior

OpenRC enforces this order:

1. load the `nitrous_min` power driver;
2. power-cycle and provision BCM4389, retrying cold-boot initialization;
3. attach and bring up `hci0`;
4. start `bluetoothd`;
5. start Plasma/BlueDevil with `bluetoothBlocked=false`.

The Bluetooth address is not committed. It can be set in
`/etc/conf.d/bluejay-bluetooth`; otherwise a stable locally administered address
is derived at boot.

## Wi-Fi power policy

The device configuration keeps `wlan1` unmanaged, enables Wi-Fi power saving on
managed Wi-Fi connections, and disables mDNS/LLMNR by default. IPv6 remains
enabled. Users who explicitly prefer the measured lower-power policy can run:

```sh
sudo bluejay-wifi-low-power on
```

Use `off` to restore IPv6 and the default discovery settings. Reconnect the Wi-Fi
profile after changing the policy.

## Proprietary firmware

Broadcom Wi-Fi/Bluetooth firmware, calibration data and the prebuilt `bcmdhd.ko`
are not redistributed here (for now). Modified sources will be provided later.

Relevant upstream work(Great thanks to M8):

- <https://github.com/m8l8th814n-eng/linux-mainline/tree/gs101-7.2>
- <https://github.com/m8l8th814n-eng/mesa-gs101>
- <https://github.com/m8l8th814n-eng/raviole_bcmd_postmarketos>
- <https://github.com/m8l8th814n-eng/tinycbd>

## Known power limitation

Use s2idle. Do not select deep suspend yet. UFS runtime power management remains
a separate unresolved investigation and is intentionally unchanged here.

