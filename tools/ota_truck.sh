#!/bin/bash
# Queues a signed firmware update for the truck node. The next ack tells the truck,
# which joins the hotspot, checks the signature, installs and proves itself by
# completing a LoRa sync. The key stays on this machine, so the server alone can't
# make the truck install anything.
#
# Usage: ota_truck.sh <firmware.bin>    queue it and watch for the truck to report it
#        ota_truck.sh --clear           remove the queued update
set -e
CT100=ray@192.168.1.104
DIR=truck-log/fw
KEY="$HOME/.secrets/truck-ota.key"

if [ "$1" = "--clear" ]; then
  ssh "$CT100" "rm -f $DIR/order.txt $DIR/truck_node.bin"
  echo "cleared"
  exit 0
fi

BIN="$1"
[ -f "$BIN" ] && [ -f "$KEY" ] || { echo "need a firmware file and $KEY"; exit 1; }
SIZE=$(stat -c %s "$BIN")
SHA=$(sha256sum "$BIN" | cut -d' ' -f1)
MAC=$(SHA="$SHA" python3 -c 'import hashlib,hmac,os,sys
print(hmac.new(open(sys.argv[1]).read().strip().encode(), os.environ["SHA"].encode(), hashlib.sha256).hexdigest())' "$KEY")
ID=$((16#${SHA:0:8}))

ssh "$CT100" "mkdir -p $DIR"
scp -q "$BIN" "$CT100:$DIR/truck_node.tmp"
ssh "$CT100" "mv $DIR/truck_node.tmp $DIR/truck_node.bin && echo '$SIZE $SHA $MAC' > $DIR/order.txt.tmp && mv $DIR/order.txt.tmp $DIR/order.txt"
echo "queued ${SHA:0:8}, $SIZE bytes. Waiting for the truck to report it."
[ -n "$OTA_WATCH_SECONDS" ] || OTA_WATCH_SECONDS=2700
END=$((SECONDS + OTA_WATCH_SECONDS))
while [ $SECONDS -lt $END ]; do
  FW=$(ssh "$CT100" "curl -s localhost:8101/truck/api/latest" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("fw"))')
  if [ "$FW" = "$ID" ]; then
    echo "truck is running ${SHA:0:8}"
    exit 0
  fi
  sleep 15
done
echo "truck has not reported ${SHA:0:8} yet"
exit 1
