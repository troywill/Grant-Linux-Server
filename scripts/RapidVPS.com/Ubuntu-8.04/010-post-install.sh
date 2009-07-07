#!/bin/sh
# TDW
source ./common-lib.sh
apt-get update && apt-get install aptitude && aptitude safe-upgrade
aptitude install emacs22-nox sudo git-core


