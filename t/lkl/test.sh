#! /bin/sh

set -o errexit -o nounset

uuid=eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee

cd "$(dirname "$0")"

mkdir -p build
chattr +dC build

echo "Preparing root fs image ..." >&2
touch build/large.file build/root.img
truncate -s 100M build/large.file
truncate -s 128M build/root.img
yes | mkfs.ext4 -q -U "$uuid" -E lazy_itable_init=1,lazy_journal_init=1 -O uninit_bg build/root.img

echo "Preparing boot fs image ..." >&2
truncate -s 64M build/boot.img
mkfs.vfat --invariant -F32 build/boot.img >/dev/null

echo "Building ..." >&2
prog=$(nix-build ../../lkl --no-out-link)

echo "Testing boot fs creation ..." >&2
$prog -t vfat -P 0 -i build/boot.img < ./boot.spec

echo "Testing root fs creation ..." >&2
$prog -t ext4 -P 0 -i build/root.img < ./root.spec
