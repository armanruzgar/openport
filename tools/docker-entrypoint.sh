#!/bin/sh
# The container image's entrypoint. A server start gets the image's settings ahead of
# its own arguments: every interface, the volume's journal and write token, and the
# bundled web terminal. The one-shot modes that refuse server options (--backtest,
# --import-day and --verify-run) run with only the arguments given.
for arg in "$@"; do
  case "$arg" in
    --backtest | --import-day | --verify-run) exec openportd "$@" ;;
  esac
done
exec openportd --address 0.0.0.0 --paper-journal /var/lib/openport/paper-journal.jsonl \
  --write-token-file /var/lib/openport/write-token --web-root /usr/share/openport/web "$@"
