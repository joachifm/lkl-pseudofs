#! /bin/sh
cd "$(dirname "$0")"
truncate -s 128M fs.img
yes | mkfs.ext4 -q fs.img
truncate -s 100M large.file
$(nix-build ../ --no-out-link) < ./fs.spec
