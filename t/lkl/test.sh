#! /bin/sh
cd "$(dirname "$0")"

echo "Creating empty image ..." >&2
truncate -s 128M fs.img
yes | mkfs.ext4 -q fs.img
truncate -s 100M large.file

echo "Building ..." >&2
prog=$(nix-build ../../lkl --no-out-link)

echo "Running test ..." >&2
$prog < ./fs.spec
