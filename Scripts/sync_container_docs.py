#!/usr/bin/env python3
"""Generate bundled and public container instructions from the canonical docs."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--site", type=Path, help="outershell.org Jekyll source directory")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
source = root / "Docs/ContainerAgentInstructions"
web = root / "Resources/OuterShellWeb"
agent = (source / "AGENTS.md").read_text()
reference = (source / "OUTERCTL.md").read_text()
(web / "container-agent.md").write_text(agent)
(web / "container-outerctl.md").write_text(reference)
if args.site:
    intro = """# Container apps

Use this guide to install and register apps inside an existing Outer Shell
container, whether you edit its Dockerfile directly or use a coding agent.

## Connect to the container

In Outer Shell, choose **Add more… → Coding agent** under the container and
copy the prompt. Outer Loop includes the SSH command and the command to enter
that specific container. You can also use the container terminal's **Copy SSH
command** action. Run that complete command from your computer; a host-only SSH
connection does not enter the container.

If your coding agent already runs inside the container, work there directly.
Check the environment before installing anything. The instructions below assume
an Outer Shell-enabled image with `outerctl` installed. Host apps use the
[general integration API](https://outershell.org/api/) instead.

"""
    body = intro + agent.replace("# Outer Shell container", "## Configure the container", 1)
    body = body.replace("Use the outer-shell skill for app integration.", "Use the publishing reference below for app integration.")
    body = body.replace("Read `/usr/local/share/outershell/OUTERCTL.md` before adding or changing a\npublished tool.", "Read the publishing reference below before adding or changing a published tool.")
    body += "\n---\n\n" + reference + "\n## Local instructions\n\nOpenCode container installations bundle\n`/usr/local/share/outershell/container-agent.md` and\n`/usr/local/share/outershell/OUTERCTL.md` for automatic context and offline use. This page is generated from the same source files.\n"
    (args.site / "container-apps.md").write_text("---\nlayout: default\ntitle: Container apps\nparent: API\nnav_order: 1\npermalink: /api/containers/\n---\n\n[Download these instructions as Markdown](/api/containers/instructions.md)\n\n" + body)
    output = args.site / "api/containers"
    output.mkdir(parents=True, exist_ok=True)
    (output / "instructions.md").write_text(body)
