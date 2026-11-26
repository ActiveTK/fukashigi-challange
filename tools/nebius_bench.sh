#!/bin/bash
set -u
cd "$(dirname "$0")/.."
export PATH=$HOME/.nebius/bin:$PATH
P=${P:?project id}
SUB=${SUB:?subnet id}
PP=${PP:?pricing policy id}
LIMIT=${LIMIT:-780}
IMAGE=${IMAGE:-ubuntu24.04-kernel-7.0-nvidia580-x86_64}
PROBE=${PROBE:-tests/gpu_probe8.sh}
HARVEST=${HARVEST:-1}
KEY=$HOME/.ssh/nebius_fk
OUT=build/nebius
NAME=fk-bench-$(date +%s)
mkdir -p "$OUT"
log() { echo "$(date +%T) $*"; }
VM=""
WD=""
find_vm() {
  timeout 30 nebius compute instance list --parent-id "$P" --format json 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin)
for it in d.get('items',[]):
    if it['metadata'].get('name')=='$1': print(it['metadata']['id'])
" 2>/dev/null | head -n 1
}
cleanup() {
  [ -n "$WD" ] && kill "$WD" 2>/dev/null
  [ -z "$VM" ] && [ -n "${NAME:-}" ] && VM=$(find_vm "$NAME")
  if [ -n "$VM" ]; then
    log "deleting $VM"
    timeout 300 nebius compute instance delete --id "$VM" > "$OUT/delete.log" 2>&1
    log "delete rc=$?"
  fi
  left=$(timeout 30 nebius compute instance list --parent-id "$P" 2>/dev/null | grep -c ' id: ')
  disks=$(timeout 30 nebius compute disk list --parent-id "$P" 2>/dev/null | grep -c ' id: ')
  log "cleanup instances_left=$left disks_left=$disks"
}
trap cleanup EXIT
UD=$(printf '#cloud-config\nusers:\n  - name: fk\n    sudo: ALL=(ALL) NOPASSWD:ALL\n    shell: /bin/bash\n    ssh_authorized_keys:\n      - %s\n' "$(cat "$KEY.pub")")
T0=$(date +%s)
START_WAIT=${START_WAIT:-360}
log "create $NAME"
timeout 900 nebius compute instance create --parent-id "$P" --name "$NAME" \
  --resources-platform gpu-h200-sxm --resources-preset 8gpu-128vcpu-1600gb \
  --boot-disk-managed-disk-name "$NAME-boot" --boot-disk-managed-disk-type network_ssd \
  --boot-disk-managed-disk-size-gibibytes 50 \
  --boot-disk-managed-disk-source-image-family-image-family "$IMAGE" \
  --boot-disk-attach-mode READ_WRITE --cloud-init-user-data "$UD" \
  --network-interfaces "[{\"name\": \"eth0\", \"ip_address\": {}, \"public_ip_address\": {}, \"subnet_id\": \"$SUB\"}]" \
  --recovery-policy fail --preemptible-on-preemption stop --spot-pricing-policy-id "$PP" \
  --format json > "$OUT/vm.json" 2> "$OUT/vm.err" &
IP=""
ST=""
for i in $(seq 1 200); do
  sleep 5
  if [ -z "$VM" ]; then
    VM=$(find_vm "$NAME")
    if [ -n "$VM" ]; then
      log "vm $VM"
      ( sleep $(( LIMIT - ( $(date +%s) - T0 ) )); echo "$(date +%T) WATCHDOG deleting $VM"; timeout 300 nebius compute instance delete --id "$VM" > /dev/null 2>&1; echo "$(date +%T) WATCHDOG done" ) &
      WD=$!
    fi
    continue
  fi
  timeout 30 nebius compute instance get --id "$VM" --format json > "$OUT/vmget.json" 2>/dev/null
  read -r ST IP < <(python3 -c "
import json
d=json.load(open('$OUT/vmget.json'))
s=d['status']
ip=''
for n in s.get('network_interfaces',[]):
    a=n.get('public_ip_address',{}).get('address','')
    if a: ip=a.split('/')[0]
print(s.get('state'), ip or '-')
" 2>/dev/null)
  [ "$ST" = "RUNNING" ] && [ -n "$IP" ] && [ "$IP" != "-" ] && break
  if [ $(( $(date +%s) - T0 )) -gt "$START_WAIT" ]; then
    log "not running after ${START_WAIT}s state=$ST"
    tail -n 3 "$OUT/vm.err"
    exit 2
  fi
done
log "running ip=$IP after $(( $(date +%s) - T0 ))s"
SSHO=(-i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o ServerAliveInterval=20 -o LogLevel=ERROR)
ok=0
for i in $(seq 1 40); do
  if timeout 15 ssh "${SSHO[@]}" "fk@$IP" 'sudo shutdown -h +14 >/dev/null 2>&1; echo ready' 2>/dev/null | grep -q ready; then ok=1; break; fi
  [ $(( $(date +%s) - T0 )) -gt 480 ] && break
  sleep 5
done
[ "$ok" = 1 ] || { log "ssh not ready"; exit 1; }
log "ssh ready after $(( $(date +%s) - T0 ))s"
timeout 60 scp "${SSHO[@]}" build/kit.tgz "fk@$IP:kit.tgz" || { log "upload failed"; exit 1; }
REM=$(( LIMIT - ( $(date +%s) - T0 ) - 60 ))
log "probe budget ${REM}s"
timeout "$REM" ssh "${SSHO[@]}" "fk@$IP" "mkdir -p fk && tar -C fk -xzf kit.tgz && cd fk && bash $PROBE" 2>&1 | tee "$OUT/probe.log"
log "probe finished after $(( $(date +%s) - T0 ))s"
REM=$(( LIMIT - ( $(date +%s) - T0 ) - 60 ))
if [ "$HARVEST" = 1 ] && [ "$REM" -gt 120 ]; then
  rm -f "$OUT/h.res"
  RUSER=fk RDIR=/home/fk/fk KEY="$KEY" timeout 40 tools/remote_run.sh "$IP" 22 "$OUT/h.res" -n 24 -k 3 -g 8
  RUSER=fk KEY="$KEY" EVERY=10 STALL=90 MAXFAIL=3 timeout $(( REM - 20 )) tools/harvest.sh "$IP" 22 /home/fk/fk/res.txt /home/fk/fk/run.log "$OUT/h.res"
  log "harvest rc=$?"
  cat "$OUT/h.res"
fi
log "elapsed $(( $(date +%s) - T0 ))s"
