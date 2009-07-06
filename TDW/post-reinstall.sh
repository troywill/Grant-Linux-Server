#!/bin/sh
cp --verbose ~/.bash_aliases ./bash_aliases
git add ./bash_aliases
git commit -m 'updated bash_aliases'
git push
