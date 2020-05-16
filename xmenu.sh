#!/bin/sh

cat <<EOF | ./xmenu -w | xargs sh -c
Applications
	Web Browser	firefox
	Image editor	gimp
Terminal (xterm)	xterm
Terminal (urxvt)	urxvt
Terminal (st)		st

Shutdown		poweroff
Reboot			reboot
EOF

