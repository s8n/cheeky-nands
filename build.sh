#!/bin/sh
# ./build.sh            Switch 2 (default): nand-backup-experiments-switch2.nro via ounce-sdk:latest
# ./build.sh switch     Switch 1: nand-backup-experiments.nro via devkitpro/devkita64 (needs that image)
# Anything after that goes to make, e.g. ./build.sh clean
set -e
cd "$(dirname "$0")"
platform=switch2
image=ounce-sdk:latest
case "${1:-}" in
switch2) shift ;;
switch)
    shift
    platform=switch
    image=devkitpro/devkita64:latest
    ;;
esac
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/build/nand-backup-experiments" -w /build/nand-backup-experiments \
    "$image" make PLATFORM="$platform" "$@"
