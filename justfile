# Task runner for this overlay. Run "just" to list the recipes.

[private]
default:
    @just --list

# Put the packages into a pmaports checkout (build/pmaports) at the pinned commit.
setup:
    scripts/setup-pmaports.sh

# Run every pmaports CI check, or only the named ones: just ci shellcheck grep
ci *checks:
    scripts/ci.sh {{checks}}

# Scan the history and the working tree for secrets.
secrets:
    scripts/ci.sh gitleaks
