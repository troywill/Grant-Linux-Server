# 2009-07-04
INSTALL='sudo aptitude install'
wget --no-clobber ftp://ftp.ruby-lang.org/pub/ruby/ruby-1.8.7-p174.tar.bz2
${INSTALL} ncurses-dev
${INSTALL} libssl-dev
${INSTALL} zlib1g-dev
${INSTALL} libreadline5-dev
