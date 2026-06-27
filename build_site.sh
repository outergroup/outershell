#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "Outer Shell is backend-backed; building the runnable package instead of a static site."
exec "${SCRIPT_DIR}/build_run.sh"
