import ctypes
import pathlib
import struct
import sys
import tempfile

core = ctypes.CDLL(sys.argv[1])
call = core.OuterShellRegistryRequest
call.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_uint32)]
call.restype = ctypes.c_int
core.OuterShellRegistryFree.argtypes = [ctypes.c_void_p]

def message(kind, size, fields):
    value = bytearray(size)
    struct.pack_into('<H', value, 0, kind)
    for offset, text in fields:
        data = text.encode() if isinstance(text, str) else text
        struct.pack_into('<II', value, offset, len(value), len(data))
        value.extend(data)
    return value

def ref(data, offset):
    start, size = struct.unpack_from('<II', data, offset)
    assert start + size <= len(data)
    return data[start:start + size]

with tempfile.TemporaryDirectory() as temporary:
    path = str(pathlib.Path(temporary) / 'registry.orwa').encode()
    def request(value, success=True):
        output = ctypes.c_void_p()
        size = ctypes.c_uint32()
        assert call(path, bytes(value), len(value), ctypes.byref(output), ctypes.byref(size))
        try:
            response = ctypes.string_at(output, size.value)
        finally:
            core.OuterShellRegistryFree(output)
        assert (struct.unpack_from('<I', response, 2)[0] == 0) == success, response
        return response
    request(message(10, 28, [(4, 'test.backend'), (12, 'Registry test'), (20, '')]))
    pipe = r'\\.\pipe\test.backend.s-1-5-18'
    app = message(13, 82, [(10, 'test.backend'), (18, 'test:main'), (26, 'Test'), (34, '/'), (42, ''), (50, pipe), (58, ''), (74, '')])
    struct.pack_into('<HH', app, 2, 3, 1)
    request(app)
    response = request(message(15, 10, [(2, '')]))
    assert struct.unpack_from('<II', response, 14) == (1, 88)
    assert ref(response, 22 + 40).decode() == pipe
    data = bytes(range(256)) * 100
    request(message(29, 18, [(2, 'test.resource'), (10, data)]))
    assert ref(request(message(28, 10, [(2, 'test.resource')])), 6) == data
    request(message(29, 18, [(2, 'empty'), (10, b'')]))
    assert ref(request(message(28, 10, [(2, 'empty')])), 6) == b''
    malformed = message(10, 28, [(4, 'bad'), (12, 'Bad'), (20, '')])
    struct.pack_into('<I', malformed, 4, 0xffffffff)
    request(malformed, False)
    request(message(11, 10, [(2, 'test.backend')]))
    assert struct.unpack_from('<I', request(message(15, 10, [(2, '')])), 14)[0] == 0
    assert pathlib.Path(path.decode()).read_bytes()[:4] == b'ORWA'
    pathlib.Path(path.decode()).write_bytes(b'corrupt')
    request(message(12, 10, [(2, '')]), False)
    assert pathlib.Path(path.decode()).read_bytes() == b'corrupt'
print('Windows registry persistence, binary resources, malformed requests, cascade, and corruption tests passed.')
