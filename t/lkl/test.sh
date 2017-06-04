#! /bin/sh
set -o errexit -o nounset

cd "$(dirname "$0")"

echo "Building ..." >&2
prog=$(nix-build ../../lkl --no-out-link)

echo "Running test ..." >&2
truncate -s 100M large.file
truncate -s 128M root.img
yes | mkfs.ext4 -q root.img
$prog -t ext4 -P 0 -i root.img < ./root.spec

truncate -s 64M boot.img
yes | mkfs.vfat -F32 boot.img
$prog -t vfat -P 0 -i boot.img < ./boot.spec
