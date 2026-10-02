#!/bin/sh
# run.sh - start funcroute with the provider API keys from .env.
#
# Provider API keys are loaded from a `.env` file next to this script. Copy
# .env.example to .env and fill in the real values, then keep .env out of
# version control and restrict its permissions (`chmod 600 .env`).
#
# The file is sourced as POSIX sh (`. ./.env`), so use plain KEY=VALUE lines.
# Because it is sourced, any value in it becomes part of this script's
# environment and is inherited by ./funcroute. To use different keys without
# editing the file, point ENV_FILE at another file: `ENV_FILE=prod.env ./run.sh`.
#
# Request logging is optional. Pick whichever form you like:
#
#   ./run.sh                    # log to the path in config.json (funcroute.db)
#   ./run.sh --no-log           # disable SQLite logging (stdout summaries only)
#   ./run.sh --log other.db     # log to a different database file
#   LOGDB=other.db ./run.sh     # same, via the LOGDB environment variable
#   LOGDB=off ./run.sh          # same as --no-log
#   ./run.sh config.json --no-log   # config path and log flag together
#
# Any arguments are forwarded to ./funcroute unchanged.

# Load provider API keys. A missing .env is only a warning: funcroute still
# starts and reports per-provider failures when a key is actually needed.
ENV_FILE="${ENV_FILE:-.env}"
# POSIX `.` searches $PATH for a name without a slash, so force a cwd lookup
# for a bare filename (".env") while still accepting "prod.env" or "/tmp/x.env".
case "$ENV_FILE" in
    /*|*/*) ENV_SRC="$ENV_FILE"   ;;  # absolute or path-qualified
    *)      ENV_SRC="./$ENV_FILE" ;;  # bare filename -> current directory
esac
if [ -f "$ENV_SRC" ]; then
    # `set -a` exports every assignment made while sourcing, so plain
    # KEY=VALUE lines in .env (no `export` needed) reach ./funcroute.
    set -a
    # shellcheck disable=SC1090  # path is dynamic by design
    . "$ENV_SRC"
    set +a
else
    echo "run.sh: warning: $ENV_FILE not found (copy .env.example to .env)" >&2
fi

# Optional log storage override via the LOGDB env var (CLI flags still win).
case "${LOGDB:-}" in
    "")           ;;                        # use config.json database.path
    off|no|0|false) set -- --no-log "$@" ;; # disable persistence
    *)            set -- --log "$LOGDB" "$@" ;;
esac

exec ./funcroute "$@"
