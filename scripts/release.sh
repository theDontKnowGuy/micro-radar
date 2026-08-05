#!/usr/bin/env bash
#
# Cuts a firmware release the radar can install over the air.
#
#   scripts/release.sh
#
# Builds every release environment, publishes the binaries as a GitHub release
# tagged from FIRMWARE_VERSION, and attaches the manifest.json that devices poll.
#
# Version, release date and notes all come from include/FirmwareVersion.h and
# nowhere else -- edit them there, commit, then run this with no arguments. That
# is what keeps the values compiled into the binary identical to the ones the
# manifest advertises. It matters for more than tidiness: the configuration page
# describes the running firmware from these same constants, so a release whose
# notes were typed on the command line would leave every updated radar unable to
# say what it is running.
#
# Devices read https://github.com/<repo>/releases/latest/download/manifest.json.
# GitHub resolves "latest" to the most recent non-draft, non-prerelease release,
# so marking a release as a prerelease is how you stage a build without shipping
# it to every radar in the field.
set -euo pipefail

REPO="${MICRO_RADAR_REPO:-thedontknowguy/micro-radar}"

# Every environment whose binary goes into the release. The names double as the
# manifest's build keys and must match the -DFIRMWARE_BUILD flags in
# platformio.ini, or devices will not find an image meant for them.
RELEASE_ENVS="${MICRO_RADAR_RELEASE_ENVS:-esp32-s3-gc9b72 esp32-s3-devkitm-1}"

# PlatformIO's own interpreter -- the project .venv has an esptool that breaks
# the build.
PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGING="$ROOT/build/release"

die() { echo "error: $*" >&2; exit 1; }

md5_of() {
    if command -v md5 >/dev/null 2>&1; then
        md5 -q "$1"
    else
        md5sum "$1" | cut -d' ' -f1
    fi
}

size_of() {
    if stat -f%z "$1" >/dev/null 2>&1; then
        stat -f%z "$1"
    else
        stat -c%s "$1"
    fi
}

command -v gh >/dev/null 2>&1 || die "gh CLI not found"
[ -x "$PIO" ] || die "pio not found at $PIO (set PIO=...)"

HEADER="$ROOT/include/FirmwareVersion.h"

# Notes must not contain a double quote -- the value is a C string literal in
# the header and is extracted here by matching to the closing quote.
read_define() {
    sed -n "s/^#define $1 \"\(.*\)\"\$/\1/p" "$HEADER"
}

VERSION="$(read_define FIRMWARE_VERSION)"
RELEASED="$(read_define FIRMWARE_RELEASED)"
NOTES="$(read_define FIRMWARE_NOTES)"

[ -n "$VERSION" ] || die "could not read FIRMWARE_VERSION from $HEADER"
[ -n "$RELEASED" ] || die "could not read FIRMWARE_RELEASED from $HEADER"
[ -n "$NOTES" ] || die "could not read FIRMWARE_NOTES from $HEADER"

echo "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' \
    || die "FIRMWARE_VERSION '$VERSION' is not MAJOR.MINOR.PATCH"
echo "$RELEASED" | grep -Eq '^[0-9]{4}-[0-9]{2}-[0-9]{2}$' \
    || die "FIRMWARE_RELEASED '$RELEASED' is not YYYY-MM-DD"

# The date is baked into the binary and shown on the configuration page for the
# life of the build, so a forgotten bump is not a cosmetic problem -- every
# radar would report the wrong release date until the next update.
TODAY="$(date -u +%Y-%m-%d)"
if [ "$RELEASED" != "$TODAY" ] && [ -z "${MICRO_RADAR_ALLOW_STALE_DATE:-}" ]; then
    die "FIRMWARE_RELEASED is $RELEASED but today is $TODAY (UTC).
  Update it in $HEADER, or set MICRO_RADAR_ALLOW_STALE_DATE=1 to publish as-is."
fi

TAG="v$VERSION"

# A dirty tree means the tag would not describe what was actually built.
[ -z "$(git -C "$ROOT" status --porcelain)" ] \
    || die "working tree is dirty -- commit before releasing"

if git -C "$ROOT" rev-parse "$TAG" >/dev/null 2>&1; then
    die "tag $TAG already exists -- bump FIRMWARE_VERSION in include/FirmwareVersion.h"
fi

if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
    die "release $TAG already published to $REPO"
fi

echo "==> Releasing $TAG to $REPO"
echo "    released $RELEASED"
echo "    $NOTES"

rm -rf "$STAGING"
mkdir -p "$STAGING"

for env in $RELEASE_ENVS; do
    echo "==> Building $env"
    # Force a full rebuild so the binary cannot contain a stale FIRMWARE_VERSION
    # from before the bump.
    "$PIO" run --project-dir "$ROOT" -e "$env" -t clean >/dev/null
    "$PIO" run --project-dir "$ROOT" -e "$env"

    src="$ROOT/.pio/build/$env/firmware.bin"
    [ -f "$src" ] || die "no firmware.bin for $env"
    cp "$src" "$STAGING/micro-radar-$env-$VERSION.bin"
done

echo "==> Writing manifest"
MANIFEST="$STAGING/manifest.json"
{
    echo "{"
    echo "  \"version\": \"$VERSION\","
    printf '  "notes": '
    python3 -c 'import json,sys; print(json.dumps(sys.argv[1]) + ",")' "$NOTES"
    echo "  \"released\": \"$RELEASED\","
    echo "  \"builds\": {"

    first=1
    for env in $RELEASE_ENVS; do
        bin="$STAGING/micro-radar-$env-$VERSION.bin"
        [ $first -eq 1 ] || echo ","
        first=0
        printf '    "%s": {\n' "$env"
        printf '      "url": "https://github.com/%s/releases/download/%s/micro-radar-%s-%s.bin",\n' \
            "$REPO" "$TAG" "$env" "$VERSION"
        printf '      "md5": "%s",\n' "$(md5_of "$bin")"
        printf '      "size": %s\n' "$(size_of "$bin")"
        printf '    }'
    done
    echo
    echo "  }"
    echo "}"
} > "$MANIFEST"

python3 -m json.tool "$MANIFEST" >/dev/null || die "generated manifest is not valid JSON"
cat "$MANIFEST"

echo "==> Tagging"
git -C "$ROOT" tag -a "$TAG" -m "$NOTES"
git -C "$ROOT" push "$(git -C "$ROOT" remote | grep -qx fork && echo fork || echo origin)" "$TAG"

echo "==> Publishing release"
gh release create "$TAG" \
    --repo "$REPO" \
    --title "$TAG" \
    --notes "$NOTES" \
    "$STAGING"/*.bin \
    "$MANIFEST"

echo
echo "Published $TAG. Radars will pick it up within an hour."
