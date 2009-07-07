#!/bin/sh
I='sudo gem install --verbose --no-ri --no-rdoc'
${I} rails mongrel capistrano sqlite3-ruby mysql

