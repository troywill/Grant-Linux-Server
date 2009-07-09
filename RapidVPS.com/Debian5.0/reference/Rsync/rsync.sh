# rsync --archive --verbose --delete troywill.com::etc etc
# rsync --archive --verbose --delete troywill.com::var var
rsync --archive --verbose --delete troywill.com::tdw stow
rsync --archive --verbose --delete troywill.com::home home
