#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
cc -std=c11 -Wall -Wextra -O2 -o /tmp/test_sha256 test_sha256.c ../src/sha256.c && /tmp/test_sha256
