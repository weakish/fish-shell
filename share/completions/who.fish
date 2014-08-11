complete --signature '
	Usage: who [options]
	Options:
		-a, --all       Same as -b -d --login -p -r -t -T -u
		-b, --boot      Print time of last boot
		-d, --dead      Print dead processes
		-H, --heading   Print line of headings
		-i, --idle      Print idle time
		-l, --login     Print login process
		--lookup        Canonicalize hostnames via DNS
		-m              Print hostname and user for stdin
		-p, --process   Print active processes spawned by init
		-q, --count     Print all login names and number of users logged on
		-r, --runlevel  Print current runlevel
		-s, --short     Print name, line, and time
		-t, --time      Print last system clock change
		-T, --mesg      Print users message status as +, - or ?
		-w, --writable  Print users message status as +, - or ?
		--message       Print users message status as +, - or ?
		-u, --users     List users logged in
		--help          Display help and exit
		--version       Display version and exit
'
