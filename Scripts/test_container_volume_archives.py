import pathlib
import runpy
import subprocess
import tempfile
import uuid
from unittest.mock import patch

provider = runpy.run_path(str(pathlib.Path(__file__).resolve().parents[1] / "Resources/outershell-container-provider"))
prepare = provider["prepare_persistent_volume"]
transfer = provider["transfer_persistent_volume"]
container_id, item_id = str(uuid.uuid4()), str(uuid.uuid4())

with tempfile.TemporaryDirectory() as temporary:
    root = pathlib.Path(temporary)
    calls = []
    exists = False

    def docker(arguments, **kwargs):
        calls.append(arguments)
        return subprocess.CompletedProcess(arguments, 0 if arguments[:2] != ["volume", "inspect"] or exists else 1, "", "")

    with patch.dict(prepare.__globals__, container_directory=lambda _: root, docker=docker):
        archive = provider["volume_archive_path"](container_id, item_id)
        archive.parent.mkdir()
        archive.write_bytes(b"test snapshot")
        prepare(container_id, item_id)
        restore = next(c for c in calls if c[0] == "run")
        assert "-xpf" in restore and ",target=/transfer,readonly" in restore[restore.index("--mount", 8) + 1]
        assert not archive.exists()
        assert all("type=bind,source=" + str(root / "Persistent Data") not in arg for arg in restore)
        print("PASS: pending snapshot is restored inside the runtime and consumed only after success")

        archive.write_bytes(b"test snapshot")
        exists = True
        calls.clear()
        try:
            prepare(container_id, item_id)
            raise AssertionError("Existing volume was overwritten")
        except RuntimeError:
            pass
        assert archive.exists() and not any(c[0] == "run" for c in calls)
        print("PASS: existing volume cannot be overwritten by an import retry")

        exists = False
        def fail_transfer(*args, **kwargs):
            raise RuntimeError("restore failed")
        calls.clear()
        with patch.dict(prepare.__globals__, transfer_persistent_volume=fail_transfer):
            try:
                prepare(container_id, item_id)
                raise AssertionError("Restore failure was ignored")
            except RuntimeError:
                pass
        assert archive.exists() and any(c[:2] == ["volume", "rm"] for c in calls)
        print("PASS: failed restoration retains its snapshot and removes the incomplete volume")

        legacy = provider["persistent_data_directory"](container_id, item_id)
        legacy.mkdir(parents=True)
        calls.clear()
        prepare(container_id, item_id)
        assert calls == []
        print("PASS: existing bind-mounted user data is preserved")

import io
import tarfile

finish = provider["operation_finish_import"]
with tempfile.TemporaryDirectory() as temporary:
    root = pathlib.Path(temporary)
    transfer_id = str(uuid.uuid4())
    transfer_root = root / transfer_id
    transfer_root.mkdir()
    with tarfile.open(transfer_root / "upload.outershell-container", "w:gz") as archive:
        value = provider["offset_archive_encode"]({"version": 1})
        member = tarfile.TarInfo("manifest.orwv")
        member.size = len(value)
        archive.addfile(member, io.BytesIO(value))
    with patch.dict(finish.__globals__, transfer_directory=lambda: root):
        try:
            finish({"transferID": transfer_id})
            raise AssertionError("Version 1 was accepted")
        except ValueError as error:
            assert "unsupported format" in str(error)
    print("PASS: old archive versions are rejected before creating a container")
