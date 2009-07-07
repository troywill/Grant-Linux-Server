#!/bin/bash
set -o 'errexit'
. ./common-lib.sh
# 2009-07-07 TDW
echo "${INSTALL_COMMAND} wget"
${INSTALL_COMMAND} wget
wget --no-clobber ftp://ftp.ruby-lang.org/pub/ruby/ruby-1.8.7-p174.tar.bz2
${INSTALL_COMMAND} ncurses-dev libssl-dev zlib1g-dev libreadline5-dev

exit
DIR='ruby-1.8.7-p174'
sudo make DESTDIR=/stow/${DIR} install
sudo stow -v ${DIR}
