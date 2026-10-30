# Coding agent onboarding

The first card in the HTML and outerframe “Add more…” flow is “Coding agent”, selected by
default. It offers two paths without adding any homepage UI.
“Use your coding agent” shows an inline copyable prompt ending with “[Your request here]”, with the integration guide at
https://outershell.org/api/ and the SSH command supplied by Outer Loop's session
context. Local Outer Loop sessions use local instructions. Ordinary browsers
without session context explain that Outer Loop supplies the SSH connection
automatically; there is no manual SSH entry field.
Example prompts sit directly below the connection prompt and copy without
connection instructions. OpenCode setup appears alongside this section, or below
it on narrow screens.

OpenCode also appears in the HTML app catalog for the host user on macOS and
Linux, and Windows. The catalog uses the same icon size and installed state as the other apps. The coding-agent guide offers “Open OpenCode” when it is installed. New installations are pinned to the user’s home group as soon
as they are registered, preserving existing pins and layout changes from other devices.

“Set up OpenCode” uses the existing command-port service recipe. It downloads
the latest compatible stable OpenCode release from anomalyco/opencode on GitHub, checks the
release asset’s SHA-256 digest, and runs its web UI on loopback (default port 4096).
macOS and Linux ARM64/x86-64 are supported, including musl Linux. Windows x64 and ARM64 use a checksummed standalone
executable launched through the existing scheduled-task command recipe. The native outerframe frontend uses the same installer templates and verified release assets.

The installer and skill are served from Resources/OuterShellWeb. The binary is
kept at ~/.local/share/outershell-opencode/<initial-version>/opencode on Unix and %LOCALAPPDATA%/outershell-opencode/<initial-version>/opencode.exe on Windows.
The launcher creates ~/server-admin (USERPROFILE/server-admin on Windows), writes
workspace instructions if absent, and installs the outer-shell skill before starting
OpenCode. When git is available it initializes an empty repository. It creates and
persists a Server administration session and registers its explicit session URL;
a directory-only URL can otherwise reopen a browser’s previously selected project. The installer does not replace a user's standalone
OpenCode binary. It preserves existing provider configuration and skill content,
and adds an idempotent Outer Shell section to the global OpenCode AGENTS.md.
Model provider authentication is completed by the user in OpenCode.

The form is sent in the POST body because install commands exceed the HTTP
request-target limit. The backend forwards that form to the existing create
route. The daemon decodes full form values without its old PATH_MAX-based
encoded-value truncation. Both backend and daemon must be updated together.

Verify with Scripts/test_outerframe_opencode.py, Scripts/test_web_agent_onboarding.cjs, Scripts/test_create_query.c,
and a real install through the HTML UI. The C test is compiled like the existing
Scripts/test_launchd_failure_status.c. Changing the supported OpenCode major version also
requires updating every platform asset digest in opencode-start.sh and opencode-start.ps1. Windows commands embed UTF-8
PowerShell as base64 chunks to stay below cmd.exe’s per-line limit.

For local HTML iteration, run OuterShellBackend with --web-only and --web-root
pointing at Resources/OuterShellWeb. Use --api-socket-path to select a development
daemon built from the same checkout. This reads and changes the local registry.

## OpenCode in containers

Each container has one bottom “Manage…” button. It opens container details and lifecycle actions, plus Edit and Share for managed containers. Edit opens directly to the Dockerfile editor, with coding agent instructions in a neighboring tab. Switching tabs preserves unsaved configuration edits.
The Outer Shell app catalog is available at the host level; its apps can work with containers from there.
OpenCode is the only bundled app setup offered inside containers.

The HTML coding-agent card offers “Set up in a container”, opening the existing
container form with “Install OpenCode” selected. The same checkbox is available
when creating a container normally. Existing containers have an “Add OpenCode”
action in their Dockerfile editor; Save & rebuild applies the addition.
Use the default Outer Shell Debian image, or a Debian/Ubuntu image that already
includes Outer Shell support.

The shared `opencode-container.Dockerfile` recipe installs the checksummed
standalone binary and registers it with `outerctl image add-app`. Its launcher
uses the container's socket forwarding and opens a saved “Container workspace”
session in `/work`. New container groups pin OpenCode by default. `/work` and
`/var/lib/opencode` are declared volumes: the former holds projects and experiment
outputs; the latter holds OpenCode sessions, model-provider configuration, and
credentials. Each provider preserves these across rebuilds.

`container-agent.md`, `container-outerctl.md`, and `outer-shell-skill.md` are
embedded in the image. The launcher explicitly supplies the container instructions
to OpenCode, installs the skill, and writes `/work/AGENTS.md` only if absent.
These explain the mounted Dockerfile, durable app registration, live changes,
persistent data, and the container boundary. Instructions are gzip/base64 encoded
in the generated recipe to keep its size small enough for Apple container's builder.
The documentation copies under `Docs/ContainerAgentInstructions` link to these
shared resources.

Run `Scripts/test_web_opencode_container.cjs` and
`Scripts/test_container_initial_dockerfile.py` for recipe/context and initial-create
regressions. Runtime verification should include creating an OpenCode container,
checking its skill and explicit session URL, then rebuilding with a marker in
`/work` and confirming both the marker and saved session survive.

### OpenCode releases and updates

New host installs and generated container Dockerfiles resolve the newest stable
OpenCode 1 release through GitHub's release API. OpenCode 2 requires a separate
integration review. Release lookup errors and missing SHA-256 asset digests stop
setup; there is no fallback to an old bundled version. The generated launcher or
Dockerfile saves the selected version and checksums, so restarts and rebuilds do
not silently upgrade.

For an existing host installation, **Update OpenCode…** resolves a release and
provides checksum-verified update commands. Stop OpenCode from its app menu, run
the commands on the host as the same user (PowerShell on Windows), then start it
again. The updater replaces the binary at the launcher's existing path, preserving
sessions and settings. It refuses ambiguous installations with multiple binaries.
Container updates belong in the container's Dockerfile; rebuilding an unchanged
Dockerfile deliberately retains its recorded release.

OpenCode host update checks run when a running installation first appears, on
launch/focus/return to the page, and while the page remains visible. Health is
rechecked at most every five minutes; release results are reused for 24 hours.
The Unix HTML backend shares release responses between clients and coalesces
concurrent requests. Browser storage also retains the selected release across
reloads. Network failures are silent and retried after an hour. Stopped apps
are never started by a check. A newer compatible release adds a tile badge and
an “Update to VERSION…” app-menu action; onboarding has no update button.

## Native frontend parity

The outerframe frontend preserves its New App project generator alongside Coding agent,
Outer Shell app catalog, and Run bash commands. Other recipes and its discovery endpoint
have been removed. Container Edit opens the Dockerfile directly, with an explicit choice
between sandboxed container access and privileged host access on the Coding agent tab.
Hosted OpenCode links prefill the appropriate prompt without submitting it. The editor
supports pinch-to-resize text, a zoom menu, wrapping tabs, and a stable rebuild warning.
Remote folder selection uses the in-page breadcrumb picker.

App launches show a spinner in the status-dot position while waiting for readiness. Stopping uses a counterclockwise spinner in the same position. Homepage cards share row heights, command-section baselines, and bottom action alignment. Manage and Manage containers open scrollable panels rather than native context menus. Starting a
stopped container app starts its container if needed, starts the service, and waits for its
published endpoint before opening it. Menus use Pin/Unpin and Move earlier/later, without
User/Root submenus. OpenCode checks its running version periodically and only offers update
commands when a newer compatible stable version is known.
