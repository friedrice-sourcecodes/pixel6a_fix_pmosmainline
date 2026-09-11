#!/bin/sh

# Use the render-only Panfrost device while simpledrm remains the display KMS
# device. Do not force KWIN_DRM_DEVICES: card1 has no display connectors.
export DRI_PRIME=1
