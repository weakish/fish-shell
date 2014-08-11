
function __fish_complete_ssh -d "common completions for ssh commands" --argument command

    set -l SSH_OPTIONS "
        AddressFamily
        BatchMode
        BindAddress
        ChallengeResponseAuthentication
        CheckHostIP
        Cipher
        Ciphers
        Compression
        CompressionLevel
        ConnectionAttempts
        ConnectTimeout
        ControlMaster
        ControlPath
        GlobalKnownHostsFile
        GSSAPIAuthentication
        GSSAPIDelegateCredentials
        Host
        HostbasedAuthentication
        HostKeyAlgorithms
        HostKeyAlias
        HostName
        IdentityFile
        IdentitiesOnly
        LogLevel
        MACs
        NoHostAuthenticationForLocalhost
        NumberOfPasswordPrompts
        PasswordAuthentication
        Port
        PreferredAuthentications
        Protocol
        ProxyCommand
        PubkeyAuthentication
        RhostsRSAAuthentication
        RSAAuthentication
        SendEnv
        ServerAliveInterval
        ServerAliveCountMax
        SmartcardDevice
        StrictHostKeyChecking
        TCPKeepAlive
        UsePrivilegedPort
        User
        UserKnownHostsFile
        VerifyHostKeyDNS
    "

    complete -c $command --signature "
        Usage: ssh [options]

        Options:
            -1  Protocol version 1 only
            -2  Protocol version 2 only
            -4  IPv4 addresses only
            -6  IPv6 addresses only
            -C  Compress all data
            -v  Verbose mode
            -c <cipher_spec>    Encryption algorithm
            -F <config_file>    Configuration file
            -i <identity_file>  Identity file
            -o <option>         Options

        Conditions:
            <cipher_spec>  blowfish 3des des
            <option>       $SSH_OPTIONS
    "
end

