#!/usr/bin/env bash
set -euo pipefail

args=()
for arg in "$@"; do
    case "$arg" in
        -ftrivial-auto-var-init=zero)
            ;;
        *)
            args+=("$arg")
            ;;
    esac
done

exec gcc "${args[@]}"
