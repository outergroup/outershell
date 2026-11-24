#!/usr/bin/env python3
"""Generate the public container instructions from the canonical docs."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--site", type=Path, help="outershell.org Jekyll source directory")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = root / "Docs/ContainerAgentInstructions"
agent = (source / "AGENTS.md").read_text()
reference = (source / "OUTERCTL.md").read_text()

support = root / "Container/Support/share"
(support / "skills/outer-shell").mkdir(parents=True, exist_ok=True)
(support / "OUTERCTL.md").write_text(reference)
skill_header = """---
name: outer-shell
description: Make a web page, dashboard, or small web app that the user can open from Outer Loop on any of their devices, or add a terminal command they can launch. Also explains this Outer Shell container and how to make changes survive a rebuild.
metadata: { "openclaw": { "requires": { "bins": ["outerctl"] } } }
---

# Outer Shell container

You are running inside a container managed by Outer Shell. The user reaches
this container from Outer Loop, a browser on their Mac, iPhone, and other
devices. Web apps you publish here appear in Outer Shell next to the user's
other apps, and they open in Outer Loop from any of those devices.

Published apps are private to the user. Outer Loop reaches them through Unix
sockets over SSH, so they need no password, token, or open network port. Don't
publish anything on a network port, and don't add your own sign-in.

To make something the user can open:

1. Write a small web server. Keep its code with the container's build context
   (see below), not in `/tmp`, so it survives a rebuild.
2. Prefer a server that listens on a Unix socket; otherwise use a loopback TCP
   port.
3. Declare it with `outerctl image add-app` in the Dockerfile, then publish it
   in the running container too, as described in
   `/usr/local/share/outershell/OUTERCTL.md`.
4. Tell the user the app's name, so they can open it in Outer Shell.

"""
skill_body = agent[agent.index("## Preserve the result"):]
(support / "skills/outer-shell/SKILL.md").write_text(skill_header + skill_body)
if args.site is None:
    raise SystemExit(0)
intro = """# Container apps

Use this guide to install and register apps inside an existing Outer Shell
container, whether you edit its Dockerfile directly or use a coding agent.

## Connect to the container

In Outer Shell, choose **Add more… → Coding agent** under the container, pick
**Inside container**, and copy the prompt. Outer Loop includes the SSH command
and the command to enter that specific container. You can also use the container terminal's **Copy SSH
command** action. Run that complete command from your computer; a host-only SSH
connection does not enter the container.

If your coding agent already runs inside the container, work there directly.
Check the environment before installing anything. The instructions below assume
an Outer Shell-enabled image with `outerctl` installed. Host apps use the
[general integration API](https://outershell.org/api/) instead.

"""
body = intro + agent.replace("# Outer Shell container", "## Configure the container", 1)
body = body.replace("Read `/usr/local/share/outershell/OUTERCTL.md` before adding or changing a\npublished tool.", "Read the publishing reference below before adding or changing a published tool.")
body += "\n---\n\n" + reference
(args.site / "container-apps.md").write_text("---\nlayout: page\ntitle: Container apps\nparent: API\nnav_order: 1\npermalink: /api/containers/\n---\n\n[Download these instructions as Markdown](/api/containers/instructions.md)\n\n" + body)
output = args.site / "api/containers"
output.mkdir(parents=True, exist_ok=True)
(output / "instructions.md").write_text(body)
