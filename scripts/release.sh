#!/bin/sh
# release.sh - cut a release: tag it, push it, watch GitHub publish it.
#
#   scripts/release.sh 0.6.0            explicit version
#   scripts/release.sh patch            bump the newest tag (minor, major too)
#   scripts/release.sh patch --dry-run  say what would happen, change nothing
#   scripts/release.sh 0.6.0 --skip-tests
#
# Pushing the tag is the whole trigger: .github/workflows/release.yml builds all
# four platforms, runs the offline routing test, packages the tarballs, writes
# SHA256SUMS and publishes the release. This script publishes nothing itself and
# is safe to run twice (a dry run has no effect at all).
set -eu

usage() {
    cat <<'EOF'
usage: scripts/release.sh <version|patch|minor|major> [options]

  version   an explicit X.Y.Z (a leading "v" is accepted and ignored)
  patch     bump the third number of the newest tag
  minor     bump the second number, zero the third
  major     bump the first number, zero the rest

options:
  --dry-run      print the plan, run nothing that changes state
  --skip-tests   don't run `make test` before tagging
  --skip-watch   don't wait for the workflow after pushing
  -h, --help     this text
EOF
}

DRY_RUN=0
SKIP_TESTS=0
SKIP_WATCH=0
POSITIONAL=""

for arg in "$@"; do
    case "$arg" in
        --dry-run)    DRY_RUN=1 ;;
        --skip-tests) SKIP_TESTS=1 ;;
        --skip-watch) SKIP_WATCH=1 ;;
        -h|--help)    usage; exit 0 ;;
        -*)           echo "release.sh: unknown option '$arg'" >&2; usage >&2; exit 2 ;;
        *)
            if [ -n "$POSITIONAL" ]; then
                echo "release.sh: expected one version, got '$POSITIONAL' and '$arg'" >&2
                exit 2
            fi
            POSITIONAL="$arg"
            ;;
    esac
done

if [ -z "$POSITIONAL" ]; then
    usage >&2
    exit 2
fi

say() { printf '%s\n' "$*"; }
die() { printf 'release.sh: %s\n' "$*" >&2; exit 1; }
run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        say "  would run: $*"
    else
        say "  $*"
        "$@"
    fi
}

cd "$(dirname "$0")/.."

# ---- sanity checks ----
git rev-parse --is-inside-work-tree >/dev/null 2>&1 \
    || die "not a git repository"

if ! git diff --quiet || ! git diff --cached --quiet; then
    if [ "$DRY_RUN" -eq 1 ]; then
        say "note: working tree is dirty (fine for a dry run, refused for real)"
    else
        die "working tree is dirty; commit or stash first (nothing was tagged)"
    fi
fi

branch=$(git rev-parse --abbrev-ref HEAD)
if [ "$branch" != "main" ] && [ "$branch" != "master" ] && [ "$DRY_RUN" -eq 0 ]; then
    die "on branch '$branch', not main (use --dry-run if you are just looking)"
fi

run git fetch --tags --quiet

# ---- work out the version ----
latest=$(git describe --tags --abbrev=0 2>/dev/null || true)

bump() {
    printf '%s' "$1" | awk -F. -v kind="$2" '
        { major = $1 + 0; minor = $2 + 0; patch = $3 + 0
          if (kind == "major")      { major++; minor = 0; patch = 0 }
          else if (kind == "minor") { minor++; patch = 0 }
          else                      { patch++ }
          printf "%d.%d.%d", major, minor, patch }'
}

case "$POSITIONAL" in
    patch|minor|major)
        [ -n "$latest" ] || die "no existing tag to bump; pass an explicit version"
        VERSION=$(bump "${latest#v}" "$POSITIONAL")
        say "bumping ${latest} -> ${VERSION} ($POSITIONAL)"
        ;;
    *)
        VERSION="${POSITIONAL#v}"
        ;;
esac

printf '%s' "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' \
    || die "'$VERSION' is not X.Y.Z"

TAG="v$VERSION"

if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
    die "tag $TAG already exists (delete it with: git tag -d $TAG)"
fi

say ""
say "plan:"
say "  tag            $TAG"
say "  branch         $branch"
say "  previous tag   ${latest:-none}"
say "  version in bin $VERSION   (stamped into /api/version and the banner)"
say "  workflow       .github/workflows/release.yml"
say ""

# ---- prove it builds and routes before tagging anything ----
if [ "$SKIP_TESTS" -eq 1 ]; then
    say "skipping tests (--skip-tests)"
else
    run make VERSION="$VERSION"
    run make test
fi

if [ "$DRY_RUN" -eq 1 ]; then
    say ""
    say "dry run: nothing was tagged, pushed or published."
    exit 0
fi

# ---- tag, push, watch ----
run git tag -a "$TAG" -m "funcroute $VERSION"
run git push origin "$TAG"

if [ "$SKIP_WATCH" -eq 1 ]; then
    exit 0
fi

if ! command -v gh >/dev/null 2>&1; then
    say ""
    say "tag $TAG pushed. gh is not installed, so watch the run here:"
    say "  https://github.com/SpaceSwordAI/funcroute/actions/workflows/release.yml"
    exit 0
fi

say ""
say "waiting for the release workflow (Ctrl-C is safe: the tag is already pushed)"
sleep 5
run_id=$(gh run list --workflow=release.yml --limit 1 \
          --json databaseId,headBranch --jq '.[0].databaseId' 2>/dev/null || true)
if [ -z "$run_id" ]; then
    say "could not find the run yet; check the Actions tab"
    exit 0
fi
if gh run watch "$run_id" --exit-status; then
    say ""
    say "released: $(gh release view "$TAG" --json url --jq .url 2>/dev/null || echo "$TAG")"
else
    die "the release workflow failed; the tag is pushed, so fix and re-run the
workflow (it replaces assets) or delete the tag and start again"
fi
