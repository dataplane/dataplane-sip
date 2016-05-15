# dataplane-sip

data plane SIP server daemon

dataplane-sip is a minimal SIP server daemon. It listens on TCP/UDP 5060
(IPv4 as well as IPv6 if available).  It logs all incoming SIP requests
including the source/destination addresses and source/destination ports
in CSV format.

## LOG Format

Incoming SIP messages are logged in CSV format in "dataplane-sip.log".
Log format is described below:

`epoch timestamp, protocol, src ip, src port, "message"`

example:

`1445775973,UDP4,127.0.0.1,50751,"INVITE"`

## Dependencies

This program depends on [libpidutil](https://github.com/farrokhi/libpidutil)
