# Container storage cleanup

Outer Shell removes an owned Apple container's recipe image when deleting the
container, unless another container or saved recipe still references that image.
Image deletion errors are reported before the registration is removed, so deletion
can be retried. Externally managed containers keep their images.

After image builds and container creation/deletion, Outer Shell schedules a
coalesced cache cleanup after 30 seconds. Cleanup is serialized with its image
builds and container creation, and deferred while creation, rebuilding, or startup
is in progress. It runs `container image prune`, without `--all`, to collect
orphaned snapshots and blobs. If the image inventory includes untagged or digest
references, automatic pruning is skipped rather than risking those images.
Failures are logged and do not turn successful builds into failures.

Use **Container storage…** in the web overview or the native runtime menu for
explicit cleanup. The native menu opens the same web screen in a new tab.
No tagged images are selected by default. Cleanup with no selections only prunes
orphaned cache data. The screen offers numeric superseded Outer Shell base images,
older workspace images for this user, and unreferenced recipe images. It preserves:

- The current base image and the newest installed workspace fallback.
- Images used by running or stopped Apple containers, including digest aliases.
- Current Apple recipe images and base/Dockerfile references from saved recipes,
  including their last realized definitions.
- Unrelated images, custom base tags, builder storage, volumes, and mounted folders.

Dockerfiles with variable image references conservatively prevent named-image
cleanup because those dependencies cannot be resolved without evaluating the build.
Selections are revalidated before removal, deletion does not use `--force`, and
partial failures are shown in the screen. Cached third-party toolchains and BuildKit
storage remain available; this action is not a global image or builder reset.

Sizes estimate allocated snapshot bytes in Apple's default local store. Orphan
estimates include the hidden Apple system-image references from `state.json`.
No snapshot files are removed directly. APFS sharing means estimates are not a
promise of additional free disk space. Unavailable estimates are omitted.

Validation:

```sh
sh Scripts/test_container_storage.sh
node Scripts/test_web_container_storage.cjs
sh Scripts/test_container_runtime.sh
node Scripts/test_web_container_delete.cjs
```

## Docker and Windows containers

The same storage screen supports Docker on macOS, Linux, and Windows, and WSLC
on Windows. Select a runtime to inspect its unused Outer Shell images. Deleting
an owned container also attempts to remove its unused recipe image, without
forcing deletion. Failures retain the registration so deletion can be retried.
Externally managed containers keep their images.

Cleanup protects running and stopped containers, registered recipes, saved
Dockerfile dependencies, shared image aliases, and the current base image.
Only recognized Outer Shell recipe names and older numeric base versions are
offered. macOS also offers superseded workspace images. Selections are validated
before cleanup and checked again immediately before each removal. Partial failures
are reported. Windows conservatively skips named-image cleanup when saved
Dockerfiles contain variables or digest-pinned dependencies.

New Docker builds on macOS and Linux carry `org.outershell.managed=true`.
The screen can offer unused dangling images with that label. Historical unlabelled
dangling images are preserved. Windows currently offers named images only.
Docker image sizes are estimates and may include shared layers; Windows omits sizes.

Docker has a separate **build-cache cleanup** action. It runs
`docker builder prune --force --filter until=168h --keep-storage 10GB`.
This explicitly requested action affects the selected Docker engine's shared build
cache, including other projects, and can make subsequent builds slower. It targets
eligible cache older than seven days with a 10 GB retention setting. It does not
run automatically or as part of selected-image cleanup. WSLC does not expose this
action. No platform runs `system prune`, global `image prune --all`, or volume
pruning as part of storage cleanup. Docker cleanup with no image selections is a
no-op; Apple's orphan-cache collection described above is Apple-specific.

Additional validation:

```sh
python3 Scripts/test_docker_storage.py
python3 Scripts/test_windows_container_storage.py
```

The Windows test compiles the actual C inventory and cleanup command paths with
mock runtimes and filesystem enumeration. Native Windows compilation and live
Docker Desktop/WSLC validation remain separate platform checks.

### Linux Docker cache details

The Linux provider reports build-cache usage and reclaimable bytes from Docker's
storage summary, with an explicit unavailable state when inspection fails. These
are rounded estimates; shared layers mean they are not a free-space guarantee.
The dialog identifies the selected engine socket and explains that other users'
engines and the host-root daemon are outside its scope.

In addition to the existing old-cache policy, **Remove all unused build cache**
runs `docker builder prune --force --all`, without age or retention limits. Both
cache actions are separate from image deletion, and the response reports Docker's
reclaimed-space result plus refreshed usage. Missing reclaimed-space output is
shown as unavailable, never as zero. No cleanup runs merely by opening the dialog.
The additional action is capability-gated for providers that implement it.
