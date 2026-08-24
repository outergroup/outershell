# outer-socket-bridge

`outer-socket-bridge` carries multiplexed byte streams between its standard
input/output and Unix-domain sockets. Outer Shell uses it to publish sockets
from container runtimes without requiring SSH inside a container.

This source is part of Outer Shell. Building Outer Shell does not require an
Outer Loop checkout or an Outer Loop build product.

## Protocol compatibility

The standard-stream framing protocol begins with a 16-byte little-endian
header:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Magic, `0x3142524f` (`ORB1`) |
| 4 | 2 | Protocol version |
| 6 | 2 | Frame type |
| 8 | 4 | Stream identifier |
| 12 | 4 | Payload length |

The current protocol version is `1`. Frame types are `HELLO`, `OPEN`,
`OPEN_OK`, `OPEN_ERROR`, `DATA`, `EOF`, `CLOSE`, `PING`, `PONG`, and
`SOCKET_STATE`.

Clients must negotiate and validate the protocol rather than assume that the
helper came from a particular application. Alternative browsers may use this
protocol, native SSH stream-local forwarding, or another transport to reach
the Unix sockets published by Outer Shell.

Run `outer-socket-bridge --version` to inspect the executable version.
