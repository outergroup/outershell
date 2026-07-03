# Deployment And Testing

This document covers release deployment and direct deployment to test machines.
Private pre-release channels are intentionally documented outside this public
repository.

## Public Release Deploys

The deployment scripts live in:

```text
/Users/mrcslws/dev/outergroup-deploy
```

Production is the default:

```bash
cd /Users/mrcslws/dev/outergroup-deploy
./scripts/deploy-app.sh Files
./scripts/deploy-outer-shell.sh
./scripts/deploy-all.sh
```

`deploy-all.sh` deploys configured bundled app archives first, then deploys
Outer Shell with a catalog that points at those app archives.

## Direct Server Deploys

Direct deploys bypass `outershell.org` entirely. Use them for tight iteration on
lab machines, not for release validation.

Deploy only the Outer Shell frontend bundles to a host that already has Outer
Shell installed:

```bash
cd /Users/mrcslws/dev/outergroup-deploy
CONFIGURATION=Release ./scripts/deploy-outer-shell-bundles-to-host.sh Pircus
```

Deploy a host-specific app directly when the host is configured in
`config/hosts.json`:

```bash
cd /Users/mrcslws/dev/outergroup-deploy
./scripts/deploy-app-to-host.sh Pircus Files
```

## Recommended Test Flow

1. Use direct host deploys for tight iteration on a known lab machine.
2. Use the private release-channel documentation when validating installer,
   catalog, update, or cross-platform packaging behavior before a public release.
3. Use the public deploy scripts only for intentional releases.

## Production Guardrail

Use production deploys only for intentional releases:

```bash
./scripts/deploy-all.sh
```

Before a production deploy, verify the intended version, catalog contents, and
platform archives.
