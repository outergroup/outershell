# Coding agent onboarding

The first card in the HTML and outerframe “Add more…” flow is “Coding agent”, selected by
default. It offers two paths without adding any homepage UI.
“Use your coding agent” shows an inline copyable prompt ending with “[Your request here]”, with the integration guide at
https://outershell.org/api/ and the SSH command supplied by Outer Loop's session
context. Local Outer Loop sessions use local instructions. Ordinary browsers
without session context explain that Outer Loop supplies the SSH connection
automatically; there is no manual SSH entry field.
Example prompts sit directly below the connection prompt and copy without
connection instructions. The “Add a coding agent” card appears alongside this
section, or below it on narrow screens.

The card treats a coding agent as an app like any other. One that runs in a
terminal is used in Terminal; one with a browser UI is registered with
`outerctl app upsert` so it appears with the apps. Either way, it is installed
from Terminal, so the card opens Terminal, or offers to install it from the
catalog. Terminal sessions put Outer Shell's `outerctl` on PATH.

The form is sent in the POST body because install commands can exceed the HTTP
request-target limit. The backend forwards that form to the existing create
route. The daemon decodes full form values without its old PATH_MAX-based
encoded-value truncation. Both backend and daemon must be updated together.

Verify with Scripts/test_web_agent_onboarding.cjs, Scripts/test_create_query.c,
and a real install through the HTML UI. The C test is compiled like the existing
Scripts/test_launchd_failure_status.c.

For local HTML iteration, run OuterShellBackend with --web-only and --web-root
pointing at Resources/OuterShellWeb. Use --api-socket-path to select a development
daemon built from the same checkout. This reads and changes the local registry.

## Coding agents in containers

Each container has one bottom “Manage…” button. It opens container details and lifecycle actions, plus Edit and Share for managed containers. Edit opens directly to the Dockerfile editor, with coding agent instructions in a neighboring tab. Switching tabs preserves unsaved configuration edits.
The Outer Shell app catalog is available at the host level; its apps can work with containers from there.

Inside a container, the card leads with Terminal: any agent installed in the
container is used from a Terminal session there, and agents registered as
terminal commands (Claude Code and Codex) get their own Open buttons. It then
explains that agents belong in the Dockerfile, since anything installed only in
the running container is lost on the next rebuild. In the container editor it
offers ready-made Claude Code and Codex snippets; each installs the agent, keeps
its sign-in in a volume, and registers it with `outerctl image add-command`. For
other agents it lists the Dockerfile steps: `RUN` to install, a `VOLUME` for its
settings, then `outerctl image add-command` or `outerctl image add-app`.

Container instructions for coding agents are published at
https://outershell.org/api/containers/, generated from
`Docs/ContainerAgentInstructions` by `Scripts/sync_container_docs.py`.

Run `Scripts/test_web_agent_onboarding.cjs` and
`Scripts/test_container_initial_dockerfile.py` for card and initial-create
regressions.

## Native frontend parity

The outerframe frontend preserves its New App project generator alongside Coding agent,
Outer Shell app catalog, and Run bash commands. Other recipes and its discovery endpoint
have been removed. Container Edit opens the Dockerfile directly, with an explicit choice
between sandboxed container access and privileged host access on the Coding agent tab.
The editor supports pinch-to-resize text, a zoom menu, wrapping tabs, and a stable rebuild warning.
Remote folder selection uses the in-page breadcrumb picker.

App launches show a spinner in the status-dot position while waiting for readiness. Stopping uses a counterclockwise spinner in the same position. Homepage cards share row heights, command-section baselines, and bottom action alignment. Manage and Manage containers open scrollable panels rather than native context menus. Starting a
stopped container app starts its container if needed, starts the service, and waits for its
published endpoint before opening it. Menus use Pin/Unpin and Move earlier/later, without
User/Root submenus.
