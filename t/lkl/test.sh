#! /bin/sh
set -o errexit -o nounset

cd "$(dirname "$0")"

echo "Building ..." >&2
prog=$(nix-build ../../lkl --no-out-link)

echo "Preparing root fs image ..." >&2
touch large.file root.img
chattr +dC large.file root.img
truncate -s 100M large.file
truncate -s 128M root.img
yes | mkfs.ext4 -q root.img

echo "Preparing boot fs image ..." >&2
touch boot.img
chattr +dC boot.img
truncate -s 64M boot.img
yes | mkfs.vfat -F32 boot.img >/dev/null

echo "Testing root fs creation ..." >&2
$prog -t ext4 -P 0 -i root.img < ./root.spec

echo "Testing boot fs creation ..." >&2
$prog -t vfat -P 0 -i boot.img < ./boot.spec
