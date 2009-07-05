#!/bin/sh
I='sudo gem install --verbose --no-ri --no-rdoc'
${I} rails
${I} mongrel
${I} capistrano
${I} sqlite3-ruby
${I} mysql
