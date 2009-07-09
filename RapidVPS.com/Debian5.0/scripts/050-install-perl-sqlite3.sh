#!/bin/sh
# troydwill@gmail 2009-07-02
# Grant Linux on RapidVPS.com Debian 5 server
# Install Self-contained RDBMS in a DBI Driver
# See http://search.cpan.org/dist/DBD-SQLite/
sudo perl -MCPAN -e "install DBD::SQLite"
# Can also sudo aptitude install libdbd-sqlite3-perl
