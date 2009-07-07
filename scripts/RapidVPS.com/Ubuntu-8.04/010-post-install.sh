#!/bin/sh
set -e 'errexit'
# TDW
source ./common-lib.sh
aptitude update
aptitude safe-upgrade
aptitude install emacs22-nox git-core

