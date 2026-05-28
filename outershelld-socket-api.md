# outershelld Socket API

The outershelld API uses length-prefixed Unix socket frames with little-endian
scalars and offset-based references.

```text
Socket frame:
bytes 0..3:    UInt32 little-endian message length, L
bytes 4..<4+L: message bytes

StringRef32:
bytes 0..3: UInt32 little-endian offset to UTF-8 bytes
bytes 4..7: UInt32 little-endian UTF-8 byte length
```

Offsets are relative to the beginning of the message, where byte 0 is the first
byte of `messageType`.

## `outerctlInvoke` request (`messageType = 1`)

This transitional request moves existing `outerctl` registry operations behind
outershelld. Future APIs should add typed messages instead of extending this
argv wrapper.

```text
bytes 2..5:           UInt32 argc
bytes 6..13:          StringRef32 registry database path
bytes 14..<14+8*argc: argv StringRef32 entries
```

## `outerctlInvokeResponse` (`messageType = 2`)

```text
bytes 2..5:    UInt32 process-style exit status
bytes 6..13:   StringRef32 stdout bytes
bytes 14..21:  StringRef32 stderr bytes
```
