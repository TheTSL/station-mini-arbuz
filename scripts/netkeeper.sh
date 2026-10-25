#!/bin/sh
# Сторож сети для maind-free режима.
# UPLINK_GW — адрес шлюза вашей сети (замените на свой).
UPLINK_GW="${UPLINK_GW:-192.168.1.1}"
LOG=/bkupgrade/netkeeper.log

say() { echo "$(date) $*" >> "$LOG"; }
count=0
say "keeper start (gw=$UPLINK_GW)"

while true; do
  ip link set wlan0 up 2>/dev/null
  link=$(iw dev wlan0 link 2>/dev/null | grep -c "Connected to")
  ip4=$(ip -4 addr show wlan0 2>/dev/null | grep -c "inet ")
  if [ "$link" = "0" ] || [ "$ip4" = "0" ]; then
    say "link=$link ip=$ip4 -> restart wpa/dhcp"
    killall wpa_supplicant 2>/dev/null
    sleep 1
    wpa_supplicant -B -c /vendor/wpa_supplicant.conf -iwlan0 -Dnl80211 >>"$LOG" 2>&1
    sleep 6
    dhcpcd -k wlan0 >>"$LOG" 2>&1
    sleep 1
    dhcpcd wlan0 >>"$LOG" 2>&1
    sleep 4
  fi
  if ping -c1 -W2 "$UPLINK_GW" >/dev/null 2>&1; then
    count=0
  else
    count=$((count+1))
    say "ping fail count=$count"
  fi
  if [ "$count" -ge 6 ]; then
    say "-> restart launcher/maind"
    setsid /system/vendor/quasar/quasar_launcher2 /system/vendor/quasar/launch.json >/dev/null 2>&1 < /dev/null &
    count=0
  fi
  sleep 5
done
