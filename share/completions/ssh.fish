#
# Load common ssh options
#

__fish_complete_ssh ssh


complete --signature "
    Usage: ssh [<user> | <hostname>] [<command_to_run>] [options]
    Options: -a  Disables forwarding of the authentication agent
             -A  Enables forwarding of the authentication agent
             -b <bind_address>  Interface to transmit from
             -e <escape_char>  Sets the escape character for sessions with a pty
             -f  Go to background
             -g  Allow remote host to connect to local forwarded ports
             -I  Smartcard device
             -k  Disable forwarding of Kerberos tickets
             -l <user>  User
             -m  MAC algorithm
             -n  Prevent reading from stdin
             -N  Do not execute remote command
             -p  Port
             -q  Quiet mode
             -s  Subsystem
             -t  Force pseudo-tty allocation
             -T  Disable pseudo-tty allocation
             -x  Disable X11 forwarding
             -X  Enable X11 forwarding
             -L  Locally forwarded ports
             -R  Remotely forwarded ports
             -D  Dynamic port forwarding
    Conditions:
      <hostname>
            (__fish_print_hostnames)
            (
            #Prepend any username specified in the completion to the hostname
            echo (commandline -ct)|sed -ne 's/\(.*@\).*/\1/p'
            )(__fish_print_hostnames)
      
      <user>            (__fish_print_users | sgrep -v '^_')@
      <command_to_run>  (__fish_complete_subcommand --fcs-skip=2)
      <bind_address>    (cat /proc/net/arp ^/dev/null| sgrep -v '^IP'|cut -d ' ' -f 1 ^/dev/null)
      <escape_char>     \^ none
"

# Since ssh runs subcommands, it can accept any switches
complete -c ssh -u
