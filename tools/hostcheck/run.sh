#!/bin/sh
# Host syntax check of the port's own sources against the runtime's real
# headers, with a fake switch.h (tools/hostcheck/fake). Needs clang only.
# It proves the files parse and agree with the runtime's declarations; it
# does not build, link or run anything.  Usage: tools/hostcheck/run.sh [files]
cd "$(dirname "$0")/../.." || exit 1
FILES=$(ls source/cw_*.c); [ -n "$*" ] && FILES="$*"  # carried-over files need real libnx headers; build them with the toolchain
rc=0
for f in $FILES; do
  clang -fsyntax-only -std=gnu11 -Werror=implicit-function-declaration -DPORT_PAYLOAD_NAME=\"cardwars_nx\" \
    -Itools/hostcheck/fake -Isource -Iruntime/source "$f" 2>&1 | grep -E "error" \
    | grep -v "errors generated" | grep -v "static assertion\|unknown register" | head -15 | sed "s|^|$f: |" | grep . && rc=1
    -DPORT_PAYLOAD_NAME=\"cardwars_nx\" -Itools/hostcheck/fake -Isource -Iruntime/source "$f" 2>&1 | head -15 | sed "s|^|$f: |" | grep -E "error" | grep -v "errors generated" | grep -v "static assertion\|^.*:    [0-9]* |" && rc=1
done
[ $rc = 0 ] && echo "hostcheck: all files parse" || echo "hostcheck: errors above"
exit $rc
