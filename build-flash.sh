#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
base=${BLUEJAY_BASE:-$script_dir}
cfg=${BLUEJAY_PMBOOTSTRAP_CONFIG:-$base/work/pmbootstrap-bluejay-7.0.6.cfg}
pmaports=${BLUEJAY_PMAPORTS:-$base/work/pmaports-7.0.6}
out=${BLUEJAY_OUTPUT_DIR:-$base/outputs/bluejay-pmos-7.0.6}
password=${BLUEJAY_PASSWORD:-}
pmb="env GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0=$pmaports pmbootstrap --as-root -c $cfg"

build_images() {
	cd "$base"
	sudo $pmb shutdown 2>/dev/null || true
	sudo $pmb build --arch aarch64 --force bluejay-btloader
	sudo $pmb build --arch aarch64 --force firmware-google-bluejay-bcmdhd
	sudo $pmb build --arch aarch64 --force firmware-google-bluejay-bluetooth
	sudo $pmb build --arch aarch64 --force mesa
	sudo $pmb build --arch aarch64 --force linux-postmarketos-gs101
	sudo $pmb build --arch aarch64 --force device-google-bluejay
	if [ -n "$password" ]; then
		sudo $pmb install --password "$password"
	else
		sudo $pmb install
	fi
	sudo $pmb initfs build

	mkdir -p "$out"
	sudo $pmb export "$out"
	cd "$out"
	if [ -f google-bluejay.img ]; then
		cp -L google-bluejay.img rootfs.img
	elif [ -f google-bluejay.img.zst ]; then
		zstd -d -f google-bluejay.img.zst -o rootfs.img
	else
		echo "ERROR: combined rootfs image was not exported" >&2
		exit 1
	fi
	sha256sum boot.img vendor_boot.img rootfs.img > SHA256SUMS
	ls -lh boot.img vendor_boot.img rootfs.img SHA256SUMS
}

flash_images() {
	cd "$out"
	sha256sum -c SHA256SUMS
	fastboot getvar product 2>&1 | grep -q bluejay
	fastboot flash boot boot.img
	fastboot flash vendor_boot vendor_boot.img
	fastboot flash userdata rootfs.img
	fastboot reboot
}

case "${1:-build}" in
	build) build_images ;;
	flash) flash_images ;;
	all) build_images; flash_images ;;
	*) echo "Usage: $0 [build|flash|all]" >&2; exit 2 ;;
esac
