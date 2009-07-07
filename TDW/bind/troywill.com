$TTL 14400
;@               SOA     ns9879.rapidvps.net. troywill.com. ( ; example.com is the primary server for this zone
@               SOA     ns9879.rapidvps.net. ( ; example.com is the primary server for this zone
                        webmaster               ; contact email is webmaster@example.com
                        2007112800              ; Serial ID in reverse date format
                        21600                   ; Refresh interval for slave servers
                        1800                    ; Retry interval for slave servers
                        604800                  ; Expire limit for cached info on slave servers
                        900 )                   ; Minimum Cache TTL in zone records


@               NS      ns9879.rapidvps.net   ; ns1.example.com is a nameserver for example.com
troywill.com.           14400   IN      A       208.77.98.79
www                     14400   IN      A       208.77.98.79
esl                     14400   IN      A       208.77.98.79
getfit                  14400   IN      A       208.77.98.79
i4y                     14400   IN      A       208.77.98.79
ftp                     14400   IN      A       208.77.98.79
rails                   14400   IN      A       208.77.98.79
