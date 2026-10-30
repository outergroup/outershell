#!/usr/bin/env python3
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'Frontend/BackendsContent.swift').read_text()
a = source.index('private enum ContainerResourceText {')
b = source.index('\n}\n', a) + 3
helpers = source[a:b].replace('private enum', 'enum')
program = '''import Foundation
''' + helpers + '''
let parse = ContainerResourceText.parseMemory
for (text, expected) in [("8", 8192), ("1.5", 1536), ("1.5 GB", 1536), ("512 MB", 512), ("512m", 512),
                         ("2g", 2048), (" 3919 MiB ", 3919), (".5", 512), ("1.5GB", 1536)] {
    precondition(parse(text) == expected, text)
}
for text in ["", "abc", "1.5 TB", "-1", "1e3", "1.2.3", "GB"] {
    precondition(parse(text) == nil, text)
}
let format = ContainerResourceText.formatMemory
for (value, expected) in [(8192, "8 GB"), (1536, "1.5 GB"), (2304, "2.25 GB"), (3919, "3919 MB"), (512, "512 MB"), (128, "128 MB")] {
    precondition(format(value) == expected, expected)
}
let step = ContainerResourceText.steppedMemory
precondition(step(3919, -1, 3919) == 3584)
precondition(step(3584, 1, 3919) == 3919)
precondition(step(1536, 1, 8192) == 2048)
precondition(step(1536, -1, 8192) == 1024)
precondition(step(512, -1, 8192) == 128)
precondition(step(128, 1, 8192) == 512)
precondition(step(16384, -1, 3919) == 15872)
print("PASS: outerframe memory text parses MB/GB, formats readably, and steps in 512 MB increments within limits")
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'test.swift'
    path.write_text(program.replace('steppedMemory(_ memoryInMB: Int, by delta: Int, maximum: Int)',
                                    'steppedMemory(_ memoryInMB: Int, _ delta: Int, _ maximum: Int)'))
    subprocess.run(['xcrun', 'swiftc', '-module-cache-path', str(Path(directory) / 'modules'), str(path), '-o', str(Path(directory) / 'test')], check=True)
    subprocess.run([str(Path(directory) / 'test')], check=True)
