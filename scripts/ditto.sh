#!/usr/bin/env bash
# Thin curl wrapper for the Ditto LAN API.
#   export DITTO_TOKEN=... DITTO_HOST=ditto.local
#   scripts/ditto.sh status
#   scripts/ditto.sh type "hello world"
#   scripts/ditto.sh key ctrl alt delete
#   scripts/ditto.sh click left
#   scripts/ditto.sh move 20 -10
#   scripts/ditto.sh scroll -3
set -euo pipefail

HOST="${DITTO_HOST:-ditto.local}"
TOKEN="${DITTO_TOKEN:?set DITTO_TOKEN to the token from the boot log}"
BASE="http://${HOST}"
AUTH=(-H "Authorization: Bearer ${TOKEN}")

json_array() { printf '%s\n' "$@" | sed 's/.*/"&"/' | paste -sd, -; }

cmd="${1:?usage: status|type|key|click|move|scroll|shot|eject|attach|push|pull|ls}"; shift || true
case "$cmd" in
  status) curl -fsS "${AUTH[@]}" "${BASE}/status" ;;
  type)   curl -fsS "${AUTH[@]}" "${BASE}/type" \
            -d "$(printf '{"text":%s}' "$(printf '%s' "$*" | python3 -c 'import json,sys;print(json.dumps(sys.stdin.read()))')")" ;;
  key)    curl -fsS "${AUTH[@]}" "${BASE}/key" -d "{\"keys\":[$(json_array "$@")]}" ;;
  click)  curl -fsS "${AUTH[@]}" "${BASE}/mouse" -d "{\"click\":\"${1:-left}\"}" ;;
  move)   curl -fsS "${AUTH[@]}" "${BASE}/mouse" -d "{\"dx\":${1:-0},\"dy\":${2:-0}}" ;;
  scroll) curl -fsS "${AUTH[@]}" "${BASE}/mouse" -d "{\"scroll\":{\"v\":${1:-0},\"h\":${2:-0}}}" ;;
  shot)   curl -fsS "${AUTH[@]}" "${BASE}/screenshot" -o "${1:-shot.jpg}" && echo "saved ${1:-shot.jpg}" ;;
  eject)  curl -fsS "${AUTH[@]}" "${BASE}/msc" -d '{"action":"eject"}' ;;   # firmware takes disk
  attach) curl -fsS "${AUTH[@]}" "${BASE}/msc" -d '{"action":"attach"}' ;;  # host regains disk
  push)   curl -fsS "${AUTH[@]}" "${BASE}/file?path=$(basename "${1:?file}")" --data-binary "@${1}" ;;
  pull)   curl -fsS "${AUTH[@]}" "${BASE}/file?path=${1:?name}" -o "${2:-${1}}" && echo "saved ${2:-${1}}" ;;
  ls)     curl -fsS "${AUTH[@]}" "${BASE}/files" ;;
  *) echo "unknown command: $cmd" >&2; exit 2 ;;
esac
echo
