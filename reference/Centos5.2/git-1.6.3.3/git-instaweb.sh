#!/bin/sh
#
# Copyright (c) 2006 Eric Wong
#

PERL='@@PERL@@'
OPTIONS_KEEPDASHDASH=
OPTIONS_SPEC="\
git instaweb [options] (--start | --stop | --restart)
--
l,local        only bind on 127.0.0.1
p,port=        the port to bind to
d,httpd=       the command to launch
b,browser=     the browser to launch
m,module-path= the module path (only needed for apache2)
 Action
stop           stop the web server
start          start the web server
restart        restart the web server
"

. git-sh-setup

fqgitdir="$GIT_DIR"
local="$(git config --bool --get instaweb.local)"
httpd="$(git config --get instaweb.httpd)"
port=$(git config --get instaweb.port)
module_path="$(git config --get instaweb.modulepath)"

conf="$GIT_DIR/gitweb/httpd.conf"

# Defaults:

# if installed, it doesn't need further configuration (module_path)
test -z "$httpd" && httpd='lighttpd -f'

# any untaken local port will do...
test -z "$port" && port=1234

resolve_full_httpd () {
	case "$httpd" in
	*apache2*|*lighttpd*)
		# ensure that the apache2/lighttpd command ends with "-f"
		if ! echo "$httpd" | grep -- '-f *$' >/dev/null 2>&1
		then
			httpd="$httpd -f"
		fi
		;;
	esac

	httpd_only="$(echo $httpd | cut -f1 -d' ')"
	if case "$httpd_only" in /*) : ;; *) which $httpd_only >/dev/null 2>&1;; esac
	then
		full_httpd=$httpd
	else
		# many httpds are installed in /usr/sbin or /usr/local/sbin
		# these days and those are not in most users $PATHs
		# in addition, we may have generated a server script
		# in $fqgitdir/gitweb.
		for i in /usr/local/sbin /usr/sbin "$fqgitdir/gitweb"
		do
			if test -x "$i/$httpd_only"
			then
				full_httpd=$i/$httpd
				return
			fi
		done

		echo >&2 "$httpd_only not found. Install $httpd_only or use" \
		     "--httpd to specify another httpd daemon."
		exit 1
	fi
}

start_httpd () {
	# here $httpd should have a meaningful value
	resolve_full_httpd

	# don't quote $full_httpd, there can be arguments to it (-f)
	$full_httpd "$fqgitdir/gitweb/httpd.conf"
	if test $? != 0; then
		echo "Could not execute http daemon $httpd."
		exit 1
	fi
}

stop_httpd () {
	test -f "$fqgitdir/pid" && kill $(cat "$fqgitdir/pid")
}

while test $# != 0
do
	case "$1" in
	--stop|stop)
		stop_httpd
		exit 0
		;;
	--start|start)
		start_httpd
		exit 0
		;;
	--restart|restart)
		stop_httpd
		start_httpd
		exit 0
		;;
	-l|--local)
		local=true
		;;
	-d|--httpd)
		shift
		httpd="$1"
		;;
	-b|--browser)
		shift
		browser="$1"
		;;
	-p|--port)
		shift
		port="$1"
		;;
	-m|--module-path)
		shift
		module_path="$1"
		;;
	--)
		;;
	*)
		usage
		;;
	esac
	shift
done

mkdir -p "$GIT_DIR/gitweb/tmp"
GIT_EXEC_PATH="$(git --exec-path)"
GIT_DIR="$fqgitdir"
export GIT_EXEC_PATH GIT_DIR


webrick_conf () {
	# generate a standalone server script in $fqgitdir/gitweb.
	cat >"$fqgitdir/gitweb/$httpd.rb" <<EOF
require 'webrick'
require 'yaml'
options = YAML::load_file(ARGV[0])
options[:StartCallback] = proc do
  File.open(options[:PidFile],"w") do |f|
    f.puts Process.pid
  end
end
options[:ServerType] = WEBrick::Daemon
server = WEBrick::HTTPServer.new(options)
['INT', 'TERM'].each do |signal|
  trap(signal) {server.shutdown}
end
server.start
EOF
	# generate a shell script to invoke the above ruby script,
	# which assumes _ruby_ is in the user's $PATH. that's _one_
	# portable way to run ruby, which could be installed anywhere,
	# really.
	cat >"$fqgitdir/gitweb/$httpd" <<EOF
#!/bin/sh
exec ruby "$fqgitdir/gitweb/$httpd.rb" \$*
EOF
	chmod +x "$fqgitdir/gitweb/$httpd"

	cat >"$conf" <<EOF
:Port: $port
:DocumentRoot: "$fqgitdir/gitweb"
:DirectoryIndex: ["gitweb.cgi"]
:PidFile: "$fqgitdir/pid"
EOF
	test "$local" = true && echo ':BindAddress: "127.0.0.1"' >> "$conf"
}

lighttpd_conf () {
	cat > "$conf" <<EOF
server.document-root = "$fqgitdir/gitweb"
server.port = $port
server.modules = ( "mod_setenv", "mod_cgi" )
server.indexfiles = ( "gitweb.cgi" )
server.pid-file = "$fqgitdir/pid"
server.errorlog = "$fqgitdir/gitweb/error.log"

# to enable, add "mod_access", "mod_accesslog" to server.modules
# variable above and uncomment this
#accesslog.filename = "$fqgitdir/gitweb/access.log"

setenv.add-environment = ( "PATH" => "/usr/local/bin:/usr/bin:/bin" )

cgi.assign = ( ".cgi" => "" )

# mimetype mapping
mimetype.assign             = (
  ".pdf"          =>      "application/pdf",
  ".sig"          =>      "application/pgp-signature",
  ".spl"          =>      "application/futuresplash",
  ".class"        =>      "application/octet-stream",
  ".ps"           =>      "application/postscript",
  ".torrent"      =>      "application/x-bittorrent",
  ".dvi"          =>      "application/x-dvi",
  ".gz"           =>      "application/x-gzip",
  ".pac"          =>      "application/x-ns-proxy-autoconfig",
  ".swf"          =>      "application/x-shockwave-flash",
  ".tar.gz"       =>      "application/x-tgz",
  ".tgz"          =>      "application/x-tgz",
  ".tar"          =>      "application/x-tar",
  ".zip"          =>      "application/zip",
  ".mp3"          =>      "audio/mpeg",
  ".m3u"          =>      "audio/x-mpegurl",
  ".wma"          =>      "audio/x-ms-wma",
  ".wax"          =>      "audio/x-ms-wax",
  ".ogg"          =>      "application/ogg",
  ".wav"          =>      "audio/x-wav",
  ".gif"          =>      "image/gif",
  ".jpg"          =>      "image/jpeg",
  ".jpeg"         =>      "image/jpeg",
  ".png"          =>      "image/png",
  ".xbm"          =>      "image/x-xbitmap",
  ".xpm"          =>      "image/x-xpixmap",
  ".xwd"          =>      "image/x-xwindowdump",
  ".css"          =>      "text/css",
  ".html"         =>      "text/html",
  ".htm"          =>      "text/html",
  ".js"           =>      "text/javascript",
  ".asc"          =>      "text/plain",
  ".c"            =>      "text/plain",
  ".cpp"          =>      "text/plain",
  ".log"          =>      "text/plain",
  ".conf"         =>      "text/plain",
  ".text"         =>      "text/plain",
  ".txt"          =>      "text/plain",
  ".dtd"          =>      "text/xml",
  ".xml"          =>      "text/xml",
  ".mpeg"         =>      "video/mpeg",
  ".mpg"          =>      "video/mpeg",
  ".mov"          =>      "video/quicktime",
  ".qt"           =>      "video/quicktime",
  ".avi"          =>      "video/x-msvideo",
  ".asf"          =>      "video/x-ms-asf",
  ".asx"          =>      "video/x-ms-asf",
  ".wmv"          =>      "video/x-ms-wmv",
  ".bz2"          =>      "application/x-bzip",
  ".tbz"          =>      "application/x-bzip-compressed-tar",
  ".tar.bz2"      =>      "application/x-bzip-compressed-tar",
  ""              =>      "text/plain"
 )
EOF
	test x"$local" = xtrue && echo 'server.bind = "127.0.0.1"' >> "$conf"
}

apache2_conf () {
	test -z "$module_path" && module_path=/usr/lib/apache2/modules
	mkdir -p "$GIT_DIR/gitweb/logs"
	bind=
	test x"$local" = xtrue && bind='127.0.0.1:'
	echo 'text/css css' > $fqgitdir/mime.types
	cat > "$conf" <<EOF
ServerName "git-instaweb"
ServerRoot "$fqgitdir/gitweb"
DocumentRoot "$fqgitdir/gitweb"
PidFile "$fqgitdir/pid"
Listen $bind$port
EOF

	for mod in mime dir; do
		if test -e $module_path/mod_${mod}.so; then
			echo "LoadModule ${mod}_module " \
			     "$module_path/mod_${mod}.so" >> "$conf"
		fi
	done
	cat >> "$conf" <<EOF
TypesConfig $fqgitdir/mime.types
DirectoryIndex gitweb.cgi
EOF

	# check to see if Dennis Stosberg's mod_perl compatibility patch
	# (<20060621130708.Gcbc6e5c@leonov.stosberg.net>) has been applied
	if test -f "$module_path/mod_perl.so" && grep '^our $gitbin' \
				"$GIT_DIR/gitweb/gitweb.cgi" >/dev/null
	then
		# favor mod_perl if available
		cat >> "$conf" <<EOF
LoadModule perl_module $module_path/mod_perl.so
PerlPassEnv GIT_DIR
PerlPassEnv GIT_EXEC_DIR
<Location /gitweb.cgi>
	SetHandler perl-script
	PerlResponseHandler ModPerl::Registry
	PerlOptions +ParseHeaders
	Options +ExecCGI
</Location>
EOF
	else
		# plain-old CGI
		resolve_full_httpd
		list_mods=$(echo "$full_httpd" | sed "s/-f$/-l/")
		$list_mods | grep 'mod_cgi\.c' >/dev/null 2>&1 || \
		echo "LoadModule cgi_module $module_path/mod_cgi.so" >> "$conf"
		cat >> "$conf" <<EOF
AddHandler cgi-script .cgi
<Location /gitweb.cgi>
	Options +ExecCGI
</Location>
EOF
	fi
}

script='
s#^(my|our) \$projectroot =.*#$1 \$projectroot = "'$(dirname "$fqgitdir")'";#;
s#(my|our) \$gitbin =.*#$1 \$gitbin = "'$GIT_EXEC_PATH'";#;
s#(my|our) \$projects_list =.*#$1 \$projects_list = \$projectroot;#;
s#(my|our) \$git_temp =.*#$1 \$git_temp = "'$fqgitdir/gitweb/tmp'";#;'

gitweb_cgi () {
	cat > "$1.tmp" <<\EOFGITWEB
@@GITWEB_CGI@@
EOFGITWEB
	# Use the configured full path to perl to match the generated
	# scripts' 'hashpling' line
	"$PERL" -p -e "$script" "$1.tmp"  > "$1"
	chmod +x "$1"
	rm -f "$1.tmp"
}

gitweb_css () {
	cat > "$1" <<\EOFGITWEB
@@GITWEB_CSS@@
EOFGITWEB
}

gitweb_cgi "$GIT_DIR/gitweb/gitweb.cgi"
gitweb_css "$GIT_DIR/gitweb/gitweb.css"

case "$httpd" in
*lighttpd*)
	lighttpd_conf
	;;
*apache2*)
	apache2_conf
	;;
webrick)
	webrick_conf
	;;
*)
	echo "Unknown httpd specified: $httpd"
	exit 1
	;;
esac

start_httpd
url=http://127.0.0.1:$port

if test -n "$browser"; then
	git web--browse -b "$browser" $url || echo $url
else
	git web--browse -c "instaweb.browser" $url || echo $url
fi
