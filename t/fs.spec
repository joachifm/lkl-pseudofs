# Basic layout
dir	/run 0755 0 0
dir	/root 0700 0 0
dir	/proc 0755 0 0
dir	/sys 0755 0 0
dir	/dev 0755 0 0
dir	/dev/pts 0755 0 0
slink	/dev/random /dev/urandom 0666 0 0
nod	/dev/urandom 0666 0 0 c 1 9
nod	/dev/vda 0666 0 0 b 8 0

pipe	/run/dbus-control-pipe 0666 0 0
sock	/run/dbus.socket 0666 0 0
file	/large.file large.file 0777 0 0