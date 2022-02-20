#!/bin/bash

/etc/qemu-ifup $1 && ip addr a 10.0.2.2/24 dev "$1"
