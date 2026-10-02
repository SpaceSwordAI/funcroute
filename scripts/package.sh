#!/bin/sh
# package.sh - build a release tarball for one platform.
#
#   scripts/package.sh linux-x86_64
#   scripts/package.sh darwin-arm64
#
# Expects ./funcroute and ./funcroute-client to have been built already (the
# Makefile puts FUNCROUTE_VERSION into both). Produces
# dist/funcroute-<version>-<platform>.tar.gz containing a single top-level
# directory, so it unpacks without scattering files.
#
# On macOS the binaries are linked against Homebrew libraries. Rewriting every
# library reference to @executable_path/lib/<name> and shipping the dylibs is
# what makes the tarball runnable on a machine that has never seen brew. The
# script then refuses to package anything that still points at Homebrew.
set -eu

PLATFORM="${1:-}"
if [ -z "$PLATFORM" ]; then
    echo "usage: $0 <platform-tag>   e.g. linux-x86_64, darwin-arm64" >&2
    exit 2
fi

cd "$(dirname "$0")/.."
ROOT=$(pwd)

if [ ! -x ./funcroute ] || [ ! -x ./funcroute-client ]; then
    echo "package.sh: run make first (funcroute / funcroute-client missing)" >&2
    exit 1
fi

# Version: an explicit VERSION wins, then the git tag, then the Makefile default.
if [ -z "${VERSION:-}" ]; then
    if VERSION=$(git describe --tags --always --dirty 2>/dev/null); then
        :
    else
        VERSION=$(sed -n 's/^VERSION  *?= *//p' Makefile | head -1)
        VERSION=${VERSION:-0.0.0}
    fi
fi
VERSION=${VERSION#v}

NAME="funcroute-${VERSION}-${PLATFORM}"
STAGE="$ROOT/dist/$NAME"
rm -rf "$STAGE"
mkdir -p "$STAGE"

echo "packaging $NAME"

# ---- macOS: bundle the Homebrew dylibs and repoint every reference ----
if [ "$(uname -s)" = "Darwin" ]; then
    LIBDIR="$STAGE/lib"
    mkdir -p "$LIBDIR"

    resolve_dep() {
        # $1 is an install name from otool -L. Prints a real path, or nothing.
        case "$1" in
            /*) [ -f "$1" ] && printf '%s' "$1" ;;
            @*) base=$(basename "$1")
                for dir in "$(brew --prefix 2>/dev/null)/lib" /opt/homebrew/lib \
                           /usr/local/lib /opt/homebrew/opt/*/lib /usr/local/opt/*/lib; do
                    [ -f "$dir/$base" ] && { printf '%s' "$dir/$base"; return 0; }
                done
                printf '%s' "$1" ;;
        esac
    }

    queue=$(mktemp)
    seen=$(mktemp)
    for bin in funcroute funcroute-client; do
        otool -L "$bin" | tail -n +2 | awk '{print $1}' | grep -v '^/usr/lib/' \
            | grep -v '^/System/' >> "$queue"
    done

    while [ -s "$queue" ]; do
        dep=$(head -n 1 "$queue")
        tail -n +2 "$queue" > "$queue.tmp" && mv "$queue.tmp" "$queue"
        base=$(basename "$dep")
        grep -qx "$base" "$seen" && continue
        echo "$base" >> "$seen"
        real=$(resolve_dep "$dep")
        if [ ! -f "$real" ]; then
            echo "package.sh: cannot resolve dependency $dep" >&2
            exit 1
        fi
        cp "$real" "$LIBDIR/$base"
        chmod u+w "$LIBDIR/$base"
        # Follow this dylib's own dependencies too.
        otool -L "$LIBDIR/$base" | tail -n +2 | awk '{print $1}' \
            | grep -v '^/usr/lib/' | grep -v '^/System/' >> "$queue"
    done

    # Every bundled dylib gets a relocatable identity...
    for lib in "$LIBDIR"/*.dylib; do
        [ -f "$lib" ] || continue
        install_name_tool -id "@executable_path/lib/$(basename "$lib")" "$lib"
    done
    # ...and every reference in the binaries and the dylibs points at it.
    for file in funcroute funcroute-client "$LIBDIR"/*.dylib; do
        [ -f "$file" ] || continue
        otool -L "$file" | tail -n +2 | awk '{print $1}' | while read -r dep; do
            case "$dep" in
                /usr/lib/*|/System/*) continue ;;
            esac
            base=$(basename "$dep")
            [ -f "$LIBDIR/$base" ] || continue
            install_name_tool -change "$dep" "@executable_path/lib/$base" "$file"
        done
    done

    # Refuse to ship a binary that still reaches into Homebrew.
    prefix=$(brew --prefix 2>/dev/null || echo /nonexistent)
    if otool -L funcroute funcroute-client "$LIBDIR"/*.dylib 2>/dev/null | grep -q "$prefix"; then
        echo "package.sh: binaries still link to $prefix, refusing to package" >&2
        otool -L funcroute | grep "$prefix" >&2
        exit 1
    fi
    echo "bundled $(ls "$LIBDIR" | wc -l | tr -d ' ') libraries into lib/"
fi

# ---- payload ----
install -m 0755 funcroute funcroute-client "$STAGE/"
for file in README.md run.sh config.json .env.example; do
    [ -f "$file" ] && cp "$file" "$STAGE/"
done
for file in LICENSE COPYING LICENSE.md; do
    [ -f "$file" ] && cp "$file" "$STAGE/"
done
[ -f "$STAGE/run.sh" ] && chmod +x "$STAGE/run.sh"

# ---- smoke test: this binary must at least load and print usage ----
if ! (cd "$STAGE" && ./funcroute --help >/dev/null 2>&1); then
    echo "package.sh: funcroute --help failed inside the staging directory" >&2
    (cd "$STAGE" && ./funcroute --help) || true
    exit 1
fi
if [ "$(uname -s)" = "Linux" ] && ldd "$STAGE/funcroute" 2>/dev/null | grep -q 'not found'; then
    echo "package.sh: missing shared libraries:" >&2
    ldd "$STAGE/funcroute" | grep 'not found' >&2
    exit 1
fi

mkdir -p "$ROOT/dist"
( cd "$ROOT/dist" && tar czf "$NAME.tar.gz" "$NAME" )
if command -v sha256sum >/dev/null 2>&1; then
    ( cd "$ROOT/dist" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256" )
elif command -v shasum >/dev/null 2>&1; then
    ( cd "$ROOT/dist" && shasum -a 256 "$NAME.tar.gz" > "$NAME.tar.gz.sha256" )
fi

rm -rf "$STAGE"
echo "wrote dist/$NAME.tar.gz"
